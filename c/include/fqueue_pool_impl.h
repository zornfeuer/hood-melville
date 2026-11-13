// Private: the node pool of the FQUEUE_NODE_POOL instantiations of
// fqueue_template.h (docs/pool.md is the specification). Included by the
// template implementation; not part of the API.
//
// A pool hands out fixed-size nodes. Memory comes in slabs of
// FQUEUE_IMPL_SLAB_BYTES, aligned to their size, from FQUEUE_ALIGNED_MALLOC; a
// slab is never returned while the pool has a live node
// (fqueue_impl_npool_trim returns all of them once none is live). Every thread
// has a cache per pool: a LIFO list of free nodes and the slab it is carving.
// The cache owns the slabs it carved: the slab header names it, and the slab
// of a node is its address rounded down to the slab size. A node freed by the
// owner goes to its list; a node freed by another thread goes to the owner's
// remote stack (lock-free: pushed by any thread, taken whole by the owner when
// its list is empty). Nodes never leave their owner, so threads do not trade
// cache lines of nodes through the pool. A thread freeing other caches' nodes
// gathers up to FQUEUE_IMPL_REMOTE_BATCH of one owner and pushes them at once:
// one atomic operation on the owner's stack per batch, not per node.
//
// The cache lives on the heap behind a thread-local pointer, so that the
// thread-exit destructor (a pthread key) does not depend on the order in which
// thread-local storage is torn down. An exiting thread leaves its cache to the
// pool as an orphan, with its slabs, list and remote stack; the next thread
// that needs a cache adopts it. Caches are freed only with the pool: slab
// headers point to them.
//
// A free node holds the pool's link in its first word (the next node of a
// list, batch or remote stack). The slab header — the first node of the slab —
// holds the next slab of the pool in word 0 and the owner cache in word 1.
#ifndef HOOD_MELVILLE_FQUEUE_POOL_IMPL_H
#define HOOD_MELVILLE_FQUEUE_POOL_IMPL_H

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fqueue_common.h"

// A page of Apple M: the header of a node's slab is on the node's page, so the
// owner check on free costs no extra TLB entry (docs/pool.md).
#define FQUEUE_IMPL_SLAB_BYTES   ((size_t)16384)
#define FQUEUE_IMPL_REMOTE_BATCH 64u

#if defined(__GNUC__)
#define FQUEUE_IMPL_LIKELY(x)   __builtin_expect(!!(x), 1)
#define FQUEUE_IMPL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define FQUEUE_IMPL_NOINLINE    __attribute__((noinline))
#else
#define FQUEUE_IMPL_LIKELY(x)   (x)
#define FQUEUE_IMPL_UNLIKELY(x) (x)
#define FQUEUE_IMPL_NOINLINE
#endif

typedef struct fqueue_impl_npool fqueue_impl_npool;

// A cache line of its own (128 bytes on Apple M, 64 elsewhere): the caches of
// different threads are written on every operation, and two of them in one
// line would make it bounce between cores (false sharing). The remote stack,
// written by other threads, has a line of its own too.
#define FQUEUE_IMPL_CACHE_LINE 128

typedef struct fqueue_impl_npool_cache {
  _Alignas(FQUEUE_IMPL_CACHE_LINE) void *free; // LIFO list of free nodes
  _Atomic uint32_t nfree;                      // its length (read by stats)
  _Atomic(char *) cur;                         // carving: next node of the slab
  _Atomic(char *) end;                         // end of the slab
  // Nodes of another cache freed here, pushed to it as one batch.
  struct fqueue_impl_npool_cache *out_owner;
  void *out_head, *out_tail;
  _Atomic uint32_t nout;
  fqueue_impl_npool *pool;
  struct fqueue_impl_npool_cache *next;   // registry of the pool's caches
  struct fqueue_impl_npool_cache *orphan; // next orphan, while one
  void *raw;                              // FQUEUE_MALLOC block holding it
  _Atomic uint64_t collects;              // remote stacks taken (stats)
  // Nodes of this cache's slabs freed by other threads.
  _Alignas(FQUEUE_IMPL_CACHE_LINE) _Atomic(void *) remote;
} fqueue_impl_npool_cache;

struct fqueue_impl_npool {
  size_t node_size; // >= 2 pointers, a multiple of alignment
  _Atomic int lock;
  void *slabs; // all slabs, linked through word 0
  size_t nslabs;
  size_t abandoned; // uncarved nodes of slabs left by fqueue_impl_npool_reserve
  fqueue_impl_npool_cache *registry; // all caches, orphans included
  fqueue_impl_npool_cache *orphans;  // caches of exited threads
  uint64_t slab_allocs, slab_frees, adoptions;
  bool key_ready;
  pthread_key_t key; // thread-exit destructor
};

#define FQUEUE_IMPL_NPOOL_INIT(node_type) {.node_size = sizeof(node_type)}

static inline void **fqueue_impl_npool_word(void *node, int i) {
  return (void **)node + i;
}

// The cache that owns the node's slab (fixed while the slab lives).
static inline fqueue_impl_npool_cache *fqueue_impl_npool_owner(void *node) {
  char *slab = (char *)node - ((uintptr_t)node & (FQUEUE_IMPL_SLAB_BYTES - 1));
  return (fqueue_impl_npool_cache *)*fqueue_impl_npool_word(slab, 1);
}

static void fqueue_impl_npool_lock(fqueue_impl_npool *p) {
  for (;;) {
    if (!atomic_exchange_explicit(&p->lock, 1, memory_order_acquire)) return;
    while (atomic_load_explicit(&p->lock, memory_order_relaxed)) sched_yield();
  }
}

static void fqueue_impl_npool_unlock(fqueue_impl_npool *p) {
  atomic_store_explicit(&p->lock, 0, memory_order_release);
}

static size_t fqueue_impl_npool_per_slab(const fqueue_impl_npool *p) {
  return FQUEUE_IMPL_SLAB_BYTES / p->node_size - 1; // the first is the header
}

// Push the chain head..last (linked through word 0) onto the owner's remote
// stack.
static void fqueue_impl_npool_push_remote(fqueue_impl_npool_cache *owner,
                                          void *head, void *last) {
  void *top = atomic_load_explicit(&owner->remote, memory_order_relaxed);
  do {
    *fqueue_impl_npool_word(last, 0) = top;
  } while (!atomic_compare_exchange_weak_explicit(
      &owner->remote, &top, head, memory_order_release, memory_order_relaxed));
}

// Push the batch of other caches' nodes gathered here, if any.
static void fqueue_impl_npool_flush_out(fqueue_impl_npool_cache *c) {
  if (!c->out_head) return;
  fqueue_impl_npool_push_remote(c->out_owner, c->out_head, c->out_tail);
  c->out_head = c->out_tail = NULL;
  atomic_store_explicit(&c->nout, 0, memory_order_relaxed);
}

// Thread exit: the batch goes to its owner; the cache, with everything else it
// holds, becomes an orphan for the next thread that needs one.
static void fqueue_impl_npool_thread_exit(void *arg) {
  fqueue_impl_npool_cache *c = (fqueue_impl_npool_cache *)arg;
  fqueue_impl_npool *p = c->pool;
  fqueue_impl_npool_flush_out(c);
  fqueue_impl_npool_lock(p);
  c->orphan = p->orphans;
  p->orphans = c;
  fqueue_impl_npool_unlock(p);
}

// The cache of this thread: an orphan if there is one, otherwise a new one;
// NULL if it cannot be allocated.
static fqueue_impl_npool_cache *
fqueue_impl_npool_register(fqueue_impl_npool *p, fqueue_impl_npool_cache **tl) {
  fqueue_impl_npool_lock(p);
  if (!p->key_ready) { // pthread_once takes no argument, so under the lock
    if (pthread_key_create(&p->key, fqueue_impl_npool_thread_exit) != 0) {
      fqueue_impl_npool_unlock(p);
      return NULL;
    }
    p->key_ready = true;
  }
  fqueue_impl_npool_cache *c = p->orphans;
  if (c) {
    p->orphans = c->orphan;
    p->adoptions++;
  }
  fqueue_impl_npool_unlock(p);
  if (!c) {
    // FQUEUE_MALLOC promises no more than max_align_t: align by hand.
    char *raw = (char *)FQUEUE_MALLOC(sizeof(fqueue_impl_npool_cache) +
                                      FQUEUE_IMPL_CACHE_LINE);
    if (!raw) return NULL;
    c = (fqueue_impl_npool_cache *)(raw + FQUEUE_IMPL_CACHE_LINE -
                                    (uintptr_t)raw % FQUEUE_IMPL_CACHE_LINE);
    *c = (fqueue_impl_npool_cache){0};
    c->pool = p;
    c->raw = raw;
    fqueue_impl_npool_lock(p);
    c->next = p->registry;
    p->registry = c;
    fqueue_impl_npool_unlock(p);
  }
  pthread_setspecific(p->key, c);
  *tl = c;
  return c;
}

// Move the cache's remote stack to the front of its list; false if it is
// empty.
static bool fqueue_impl_npool_collect(fqueue_impl_npool_cache *c) {
  if (!atomic_load_explicit(&c->remote, memory_order_relaxed)) return false;
  void *head = atomic_exchange_explicit(&c->remote, NULL, memory_order_acquire);
  void *last = head;
  uint32_t n = 1;
  while (*fqueue_impl_npool_word(last, 0)) {
    last = *fqueue_impl_npool_word(last, 0);
    n++;
  }
  *fqueue_impl_npool_word(last, 0) = c->free;
  c->free = head;
  atomic_store_explicit(
      &c->nfree, atomic_load_explicit(&c->nfree, memory_order_relaxed) + n,
      memory_order_relaxed);
  atomic_store_explicit(
      &c->collects,
      atomic_load_explicit(&c->collects, memory_order_relaxed) + 1,
      memory_order_relaxed);
  return true;
}

// A new slab owned by `c`, carved from its first node; `left` uncarved nodes
// of the previous slab are given up until trim. False if it cannot be
// allocated.
static bool fqueue_impl_npool_new_slab(fqueue_impl_npool *p,
                                       fqueue_impl_npool_cache *c,
                                       size_t left) {
  char *slab = (char *)FQUEUE_ALIGNED_MALLOC(FQUEUE_IMPL_SLAB_BYTES,
                                             FQUEUE_IMPL_SLAB_BYTES);
  if (!slab) return false;
  *fqueue_impl_npool_word(slab, 1) = c;
  fqueue_impl_npool_lock(p);
  *fqueue_impl_npool_word(slab, 0) = p->slabs;
  p->slabs = slab;
  p->nslabs++;
  p->slab_allocs++;
  p->abandoned += left;
  fqueue_impl_npool_unlock(p);
  char *cur = slab + p->node_size;
  atomic_store_explicit(&c->end,
                        cur + fqueue_impl_npool_per_slab(p) * p->node_size,
                        memory_order_relaxed);
  atomic_store_explicit(&c->cur, cur, memory_order_relaxed);
  return true;
}

static inline void *fqueue_impl_npool_pop(fqueue_impl_npool_cache *c) {
  void *n = c->free;
  c->free = *fqueue_impl_npool_word(n, 0);
  atomic_store_explicit(
      &c->nfree, atomic_load_explicit(&c->nfree, memory_order_relaxed) - 1,
      memory_order_relaxed);
  return n;
}

// Allocation when the cache's list is empty (or there is no cache yet): the
// remote stack, then the cache's slab, then a new slab.
static void *fqueue_impl_npool_alloc_slow(fqueue_impl_npool *p,
                                          fqueue_impl_npool_cache **tl) {
  fqueue_impl_npool_cache *c = *tl;
  if (!c && !(c = fqueue_impl_npool_register(p, tl))) return NULL;
  if (c->free || fqueue_impl_npool_collect(c)) // an adopted orphan has a list
    return fqueue_impl_npool_pop(c);
  char *cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
  if (!cur || cur == atomic_load_explicit(&c->end, memory_order_relaxed)) {
    if (!fqueue_impl_npool_new_slab(p, c, 0)) return NULL;
    cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
  }
  atomic_store_explicit(&c->cur, cur + p->node_size, memory_order_relaxed);
  return cur;
}

// Make sure this thread's cache can hand out n more nodes without failing:
// its list and the uncarved rest of its slab hold at least n, taking the
// remote stack or a new slab now if not. An in-place operation calls it before
// changing the queue — it is the only step that can fail — and then allocates
// in the rotation step itself, after the step's frees, as the persistent
// operations and Rust do: allocating the nodes ahead would change their order,
// and with LIFO reuse the order decides the layout of the lists
// (docs/pool.md). Frees meanwhile only add nodes.
static bool fqueue_impl_npool_reserve_slow(fqueue_impl_npool *p,
                                           fqueue_impl_npool_cache **tl,
                                           unsigned n) {
  fqueue_impl_npool_cache *c = *tl;
  if (!c && !(c = fqueue_impl_npool_register(p, tl))) return false;
  char *cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
  char *end = atomic_load_explicit(&c->end, memory_order_relaxed);
  size_t left = cur ? (size_t)(end - cur) / p->node_size : 0;
  if (atomic_load_explicit(&c->nfree, memory_order_relaxed) + left >= n)
    return true;
  if (fqueue_impl_npool_collect(c) &&
      atomic_load_explicit(&c->nfree, memory_order_relaxed) + left >= n)
    return true;
  return fqueue_impl_npool_new_slab(p, c, left);
}

// Usually the list alone holds n nodes.
static inline bool fqueue_impl_npool_reserve(fqueue_impl_npool *p,
                                             fqueue_impl_npool_cache **tl,
                                             unsigned n) {
  fqueue_impl_npool_cache *c = *tl;
  if (FQUEUE_IMPL_LIKELY(
          c && atomic_load_explicit(&c->nfree, memory_order_relaxed) >= n))
    return true;
  return fqueue_impl_npool_reserve_slow(p, tl, n);
}

// A node of another cache's slab: into this thread's batch for its owner (a
// batch for another owner is pushed first). Without memory for a cache the
// node goes to its owner alone.
static FQUEUE_IMPL_NOINLINE void
fqueue_impl_npool_free_remote(fqueue_impl_npool *p,
                              fqueue_impl_npool_cache **tl,
                              fqueue_impl_npool_cache *owner, void *n) {
  fqueue_impl_npool_cache *c = *tl;
  if (!c && !(c = fqueue_impl_npool_register(p, tl))) {
    fqueue_impl_npool_push_remote(owner, n, n);
    return;
  }
  if (c->out_owner != owner) {
    fqueue_impl_npool_flush_out(c);
    c->out_owner = owner;
  }
  *fqueue_impl_npool_word(n, 0) = c->out_head;
  if (!c->out_head) c->out_tail = n;
  c->out_head = n;
  uint32_t k = atomic_load_explicit(&c->nout, memory_order_relaxed) + 1;
  atomic_store_explicit(&c->nout, k, memory_order_relaxed);
  if (k == FQUEUE_IMPL_REMOTE_BATCH) fqueue_impl_npool_flush_out(c);
}

static inline void *fqueue_impl_npool_alloc(fqueue_impl_npool *p,
                                            fqueue_impl_npool_cache **tl) {
  fqueue_impl_npool_cache *c = *tl;
  if (FQUEUE_IMPL_LIKELY(c && c->free)) return fqueue_impl_npool_pop(c);
  return fqueue_impl_npool_alloc_slow(p, tl);
}

static inline void fqueue_impl_npool_free(fqueue_impl_npool *p,
                                          fqueue_impl_npool_cache **tl,
                                          void *n) {
  fqueue_impl_npool_cache *c = *tl, *owner = fqueue_impl_npool_owner(n);
  if (FQUEUE_IMPL_LIKELY(owner == c)) {
    uint32_t k = atomic_load_explicit(&c->nfree, memory_order_relaxed) + 1;
    *fqueue_impl_npool_word(n, 0) = c->free;
    c->free = n;
    atomic_store_explicit(&c->nfree, k, memory_order_relaxed);
    return;
  }
  fqueue_impl_npool_free_remote(p, tl, owner, n);
}

// The same out of line, for the atomic instantiations: inlined there, it makes
// their in-place operations slower (the release paths grow and inline
// differently: +54M instructions on mixed_mut 10^6, docs/pool.md).
static FQUEUE_IMPL_NOINLINE void
fqueue_impl_npool_free_outlined(fqueue_impl_npool *p,
                                fqueue_impl_npool_cache **tl, void *n) {
  fqueue_impl_npool_free(p, tl, n);
}

// The length of a cache's remote stack: taken whole, counted, put back.
static size_t fqueue_impl_npool_remote_len(fqueue_impl_npool_cache *c) {
  void *head = atomic_exchange_explicit(&c->remote, NULL, memory_order_acquire);
  if (!head) return 0;
  void *last = head;
  size_t n = 1;
  while (*fqueue_impl_npool_word(last, 0)) {
    last = *fqueue_impl_npool_word(last, 0);
    n++;
  }
  fqueue_impl_npool_push_remote(c, head, last);
  return n;
}

// Free nodes: caches (list, remote stack, batch, uncarved slab), abandoned;
// the caller holds the lock. Other threads' counts are read racily.
static size_t fqueue_impl_npool_free_nodes(const fqueue_impl_npool *p) {
  size_t n = p->abandoned;
  for (fqueue_impl_npool_cache *c = p->registry; c; c = c->next) {
    n += atomic_load_explicit(&c->nfree, memory_order_relaxed);
    n += atomic_load_explicit(&c->nout, memory_order_relaxed);
    n += fqueue_impl_npool_remote_len(c);
    char *cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
    char *end = atomic_load_explicit(&c->end, memory_order_relaxed);
    if (cur) n += (size_t)(end - cur) / p->node_size;
  }
  return n;
}

static void fqueue_impl_npool_stats(fqueue_impl_npool *p,
                                    fqueue_pool_stats *s) {
  fqueue_impl_npool_lock(p);
  s->slabs = p->nslabs;
  s->slab_bytes = p->nslabs * FQUEUE_IMPL_SLAB_BYTES;
  s->capacity = p->nslabs * fqueue_impl_npool_per_slab(p);
  s->free = fqueue_impl_npool_free_nodes(p);
  s->live = s->capacity - s->free;
  s->slab_allocs = p->slab_allocs;
  s->slab_frees = p->slab_frees;
  s->collects = 0;
  for (fqueue_impl_npool_cache *c = p->registry; c; c = c->next)
    s->collects += atomic_load_explicit(&c->collects, memory_order_relaxed);
  s->adoptions = p->adoptions;
  fqueue_impl_npool_unlock(p);
}

// Return every slab if no node is live; the other threads must not use the
// pool meanwhile (their caches are reset from here).
static size_t fqueue_impl_npool_trim(fqueue_impl_npool *p) {
  fqueue_impl_npool_lock(p);
  if (fqueue_impl_npool_free_nodes(p) !=
      p->nslabs * fqueue_impl_npool_per_slab(p)) {
    fqueue_impl_npool_unlock(p);
    return 0;
  }
  size_t bytes = p->nslabs * FQUEUE_IMPL_SLAB_BYTES;
  for (void *s = p->slabs; s;) {
    void *next = *fqueue_impl_npool_word(s, 0);
    FQUEUE_FREE(s);
    p->slab_frees++;
    s = next;
  }
  p->slabs = NULL;
  p->nslabs = p->abandoned = 0;
  for (fqueue_impl_npool_cache *c = p->registry; c; c = c->next) {
    c->free = NULL;
    atomic_store_explicit(&c->nfree, 0, memory_order_relaxed);
    atomic_store_explicit(&c->cur, NULL, memory_order_relaxed);
    atomic_store_explicit(&c->end, NULL, memory_order_relaxed);
    c->out_owner = NULL;
    c->out_head = c->out_tail = NULL;
    atomic_store_explicit(&c->nout, 0, memory_order_relaxed);
    atomic_store_explicit(&c->remote, NULL, memory_order_relaxed);
  }
  fqueue_impl_npool_unlock(p);
  return bytes;
}

#endif // HOOD_MELVILLE_FQUEUE_POOL_IMPL_H
