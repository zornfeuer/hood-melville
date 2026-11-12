// Private: the node pool of the FQUEUE_NODE_POOL instantiations of
// fqueue_template.h (docs/pool.md is the specification; the Rust and Zig
// pools follow the same algorithm and constants). Included by the template
// implementation; not part of the API.
//
// A pool hands out fixed-size nodes. Memory comes in slabs of
// FQUEUE_IMPL_SLAB_BYTES from FQUEUE_MALLOC; a slab is never returned while
// the pool has a live node (fqueue_impl_npool_trim returns all of them once
// none is live). Every thread has a cache per pool: a LIFO list of free nodes
// and the slab it is carving. The cache lives on the heap behind a
// thread-local pointer, so that the thread-exit destructor (a pthread key)
// does not depend on the order in which thread-local storage is torn down.
// A cache longer than FQUEUE_IMPL_CACHE_MAX gives FQUEUE_IMPL_BATCH nodes to
// the pool's global stack of batches, under a spin lock; an empty cache takes
// a batch back before carving.
//
// A free node holds the pool's links in its first two words: word 0 — the next
// node of a list or batch; in the head of a batch, word 1 — the next batch; in
// the second node of a batch, word 1 — the number of nodes in the batch (a
// batch of one node has no second node).
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

#define FQUEUE_IMPL_SLAB_BYTES ((size_t)65536)
#define FQUEUE_IMPL_CACHE_MAX  4096u
#define FQUEUE_IMPL_BATCH      2048u

#if defined(__GNUC__)
#define FQUEUE_IMPL_LIKELY(x)   __builtin_expect(!!(x), 1)
#define FQUEUE_IMPL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FQUEUE_IMPL_LIKELY(x)   (x)
#define FQUEUE_IMPL_UNLIKELY(x) (x)
#endif

typedef struct fqueue_impl_npool fqueue_impl_npool;

// A cache line of its own (128 bytes on Apple M, 64 elsewhere): the caches of
// different threads are written on every operation, and two of them in one
// line would make it bounce between cores (false sharing).
#define FQUEUE_IMPL_CACHE_LINE 128

typedef struct fqueue_impl_npool_cache {
  _Alignas(FQUEUE_IMPL_CACHE_LINE)
  void                          *free;  // LIFO list of free nodes
  _Atomic uint32_t               nfree; // its length (read by stats)
  _Atomic(char *)                cur;   // carving: next node of the slab
  _Atomic(char *)                end;   // end of the slab
  fqueue_impl_npool              *pool;
  struct fqueue_impl_npool_cache *next;  // registry of the pool's caches
  void                          *raw;   // FQUEUE_MALLOC block holding it
} fqueue_impl_npool_cache;

struct fqueue_impl_npool {
  size_t                  node_size; // >= 2 pointers, a multiple of alignment
  _Atomic int             lock;
  _Atomic(void *)         batches;   // stack of batches (peeked without the lock)
  size_t                  global;    // nodes in batches
  void                   *slabs;     // all slabs, linked through word 0
  size_t                  nslabs;
  size_t                  abandoned; // uncarved nodes of exited threads' slabs
  fqueue_impl_npool_cache *registry;
  uint64_t                slab_allocs, slab_frees, spills, refills;
  bool                    key_ready;
  pthread_key_t           key;       // thread-exit destructor
};

#define FQUEUE_IMPL_NPOOL_INIT(node_type) {.node_size = sizeof(node_type)}

static inline void **fqueue_impl_npool_word(void *node, int i) {
  return (void **)node + i;
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

// Push the batch of `n` nodes headed by `head` (linked through word 0, the
// last one NULL); the caller holds the lock.
static void fqueue_impl_npool_push_batch(fqueue_impl_npool *p, void *head,
                                        uint32_t n) {
  void *second = *fqueue_impl_npool_word(head, 0);
  if (second) *fqueue_impl_npool_word(second, 1) = (void *)(uintptr_t)n;
  *fqueue_impl_npool_word(head, 1) = atomic_load_explicit(&p->batches, memory_order_relaxed);
  atomic_store_explicit(&p->batches, head, memory_order_relaxed);
  p->global += n;
}

// Give the cache's whole list to the pool in batches; the caller holds the
// lock.
static void fqueue_impl_npool_flush_locked(fqueue_impl_npool *p,
                                          fqueue_impl_npool_cache *c) {
  void *node = c->free;
  while (node) {
    void    *head = node, *last = node;
    uint32_t n    = 1;
    while (n < FQUEUE_IMPL_BATCH && *fqueue_impl_npool_word(last, 0)) {
      last = *fqueue_impl_npool_word(last, 0);
      n++;
    }
    node = *fqueue_impl_npool_word(last, 0);
    *fqueue_impl_npool_word(last, 0) = NULL;
    fqueue_impl_npool_push_batch(p, head, n);
  }
  c->free = NULL;
  atomic_store_explicit(&c->nfree, 0, memory_order_relaxed);
}

// Thread exit: free nodes go to the global stack, the rest of the slab stays
// uncarved until trim; the cache leaves the registry.
static void fqueue_impl_npool_thread_exit(void *arg) {
  fqueue_impl_npool_cache *c = (fqueue_impl_npool_cache *)arg;
  fqueue_impl_npool       *p = c->pool;
  fqueue_impl_npool_lock(p);
  fqueue_impl_npool_flush_locked(p, c);
  char *cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
  char *end = atomic_load_explicit(&c->end, memory_order_relaxed);
  if (cur) p->abandoned += (size_t)(end - cur) / p->node_size;
  for (fqueue_impl_npool_cache **r = &p->registry; *r; r = &(*r)->next)
    if (*r == c) {
      *r = c->next;
      break;
    }
  fqueue_impl_npool_unlock(p);
  FQUEUE_FREE(c->raw);
}

// The cache of this thread, created and registered on first use; NULL if it
// cannot be allocated.
static fqueue_impl_npool_cache *
fqueue_impl_npool_register(fqueue_impl_npool *p, fqueue_impl_npool_cache **tl) {
  // FQUEUE_MALLOC promises no more than max_align_t: align by hand.
  char *raw = (char *)FQUEUE_MALLOC(sizeof(fqueue_impl_npool_cache) + FQUEUE_IMPL_CACHE_LINE);
  if (!raw) return NULL;
  fqueue_impl_npool_cache *c =
      (fqueue_impl_npool_cache *)(raw + FQUEUE_IMPL_CACHE_LINE -
                                  (uintptr_t)raw % FQUEUE_IMPL_CACHE_LINE);
  *c      = (fqueue_impl_npool_cache){0};
  c->pool = p;
  c->raw  = raw;
  fqueue_impl_npool_lock(p);
  if (!p->key_ready) { // pthread_once takes no argument, so under the lock
    if (pthread_key_create(&p->key, fqueue_impl_npool_thread_exit) != 0) {
      fqueue_impl_npool_unlock(p);
      FQUEUE_FREE(raw);
      return NULL;
    }
    p->key_ready = true;
  }
  c->next     = p->registry;
  p->registry = c;
  fqueue_impl_npool_unlock(p);
  pthread_setspecific(p->key, c);
  *tl = c;
  return c;
}

// Allocation when the cache's list is empty (or there is no cache yet): a
// batch from the global stack, then the cache's slab, then a new slab.
static void *fqueue_impl_npool_alloc_slow(fqueue_impl_npool        *p,
                                         fqueue_impl_npool_cache **tl) {
  fqueue_impl_npool_cache *c = *tl;
  if (!c && !(c = fqueue_impl_npool_register(p, tl))) return NULL;
  if (c->free) { // registered just now by a free
    void *n = c->free;
    c->free = *fqueue_impl_npool_word(n, 0);
    atomic_store_explicit(&c->nfree,
                          atomic_load_explicit(&c->nfree, memory_order_relaxed) - 1,
                          memory_order_relaxed);
    return n;
  }
  if (atomic_load_explicit(&p->batches, memory_order_relaxed)) { // again under the lock
    fqueue_impl_npool_lock(p);
    void *head = atomic_load_explicit(&p->batches, memory_order_relaxed);
    if (head) {
      atomic_store_explicit(&p->batches, *fqueue_impl_npool_word(head, 1),
                            memory_order_relaxed);
      void    *sec = *fqueue_impl_npool_word(head, 0);
      uint32_t n   = sec ? (uint32_t)(uintptr_t)*fqueue_impl_npool_word(sec, 1) : 1;
      p->global -= n;
      p->refills++;
      fqueue_impl_npool_unlock(p);
      c->free = *fqueue_impl_npool_word(head, 0);
      atomic_store_explicit(&c->nfree, n - 1, memory_order_relaxed);
      return head;
    }
    fqueue_impl_npool_unlock(p);
  }
  char *cur = atomic_load_explicit(&c->cur, memory_order_relaxed);
  if (!cur || cur == atomic_load_explicit(&c->end, memory_order_relaxed)) {
    char *slab = (char *)FQUEUE_MALLOC(FQUEUE_IMPL_SLAB_BYTES);
    if (!slab) return NULL;
    fqueue_impl_npool_lock(p);
    *fqueue_impl_npool_word(slab, 0) = p->slabs;
    p->slabs = slab;
    p->nslabs++;
    p->slab_allocs++;
    fqueue_impl_npool_unlock(p);
    cur = slab + p->node_size;
    atomic_store_explicit(&c->end, cur + fqueue_impl_npool_per_slab(p) * p->node_size,
                          memory_order_relaxed);
  }
  atomic_store_explicit(&c->cur, cur + p->node_size, memory_order_relaxed);
  return cur;
}

// A free with no cache yet, or with a cache over FQUEUE_IMPL_CACHE_MAX after
// the node was added: register, or give FQUEUE_IMPL_BATCH nodes from the top.
// Without memory for a cache the node goes to the global stack alone.
static void fqueue_impl_npool_free_slow(fqueue_impl_npool        *p,
                                       fqueue_impl_npool_cache **tl, void *n) {
  fqueue_impl_npool_cache *c = *tl;
  if (!c) {
    *fqueue_impl_npool_word(n, 0) = NULL;
    if (!(c = fqueue_impl_npool_register(p, tl))) {
      fqueue_impl_npool_lock(p);
      fqueue_impl_npool_push_batch(p, n, 1);
      fqueue_impl_npool_unlock(p);
      return;
    }
    c->free = n;
    atomic_store_explicit(&c->nfree, 1, memory_order_relaxed);
    return;
  }
  void *head = c->free, *last = head;
  for (uint32_t i = 1; i < FQUEUE_IMPL_BATCH; i++)
    last = *fqueue_impl_npool_word(last, 0);
  c->free = *fqueue_impl_npool_word(last, 0);
  *fqueue_impl_npool_word(last, 0) = NULL;
  atomic_store_explicit(&c->nfree,
                        atomic_load_explicit(&c->nfree, memory_order_relaxed) -
                            FQUEUE_IMPL_BATCH,
                        memory_order_relaxed);
  fqueue_impl_npool_lock(p);
  fqueue_impl_npool_push_batch(p, head, FQUEUE_IMPL_BATCH);
  p->spills++;
  fqueue_impl_npool_unlock(p);
}

static inline void *fqueue_impl_npool_alloc(fqueue_impl_npool        *p,
                                           fqueue_impl_npool_cache **tl) {
  fqueue_impl_npool_cache *c = *tl;
  void                   *n;
  if (FQUEUE_IMPL_LIKELY(c && (n = c->free))) {
    c->free = *fqueue_impl_npool_word(n, 0);
    atomic_store_explicit(&c->nfree,
                          atomic_load_explicit(&c->nfree, memory_order_relaxed) - 1,
                          memory_order_relaxed);
    return n;
  }
  return fqueue_impl_npool_alloc_slow(p, tl);
}

static inline void fqueue_impl_npool_free(fqueue_impl_npool        *p,
                                         fqueue_impl_npool_cache **tl, void *n) {
  fqueue_impl_npool_cache *c = *tl;
  if (FQUEUE_IMPL_LIKELY(c)) {
    uint32_t k = atomic_load_explicit(&c->nfree, memory_order_relaxed) + 1;
    *fqueue_impl_npool_word(n, 0) = c->free;
    c->free = n;
    atomic_store_explicit(&c->nfree, k, memory_order_relaxed);
    if (FQUEUE_IMPL_LIKELY(k <= FQUEUE_IMPL_CACHE_MAX)) return;
  }
  fqueue_impl_npool_free_slow(p, tl, n);
}

// Free nodes: caches (list and uncarved slab), global stack, abandoned; the
// caller holds the lock. Other threads' counts are read racily.
static size_t fqueue_impl_npool_free_nodes(const fqueue_impl_npool *p) {
  size_t n = p->global + p->abandoned;
  for (const fqueue_impl_npool_cache *c = p->registry; c; c = c->next) {
    n += atomic_load_explicit(&c->nfree, memory_order_relaxed);
    fqueue_impl_npool_cache *m   = (fqueue_impl_npool_cache *)c;
    char                   *cur = atomic_load_explicit(&m->cur, memory_order_relaxed);
    char                   *end = atomic_load_explicit(&m->end, memory_order_relaxed);
    if (cur) n += (size_t)(end - cur) / p->node_size;
  }
  return n;
}

static void fqueue_impl_npool_stats(fqueue_impl_npool *p, fqueue_pool_stats *s) {
  fqueue_impl_npool_lock(p);
  s->slabs       = p->nslabs;
  s->slab_bytes  = p->nslabs * FQUEUE_IMPL_SLAB_BYTES;
  s->capacity    = p->nslabs * fqueue_impl_npool_per_slab(p);
  s->free        = fqueue_impl_npool_free_nodes(p);
  s->live        = s->capacity - s->free;
  s->slab_allocs = p->slab_allocs;
  s->slab_frees  = p->slab_frees;
  s->spills      = p->spills;
  s->refills     = p->refills;
  fqueue_impl_npool_unlock(p);
}

// Return every slab if no node is live; the other threads must not use the
// pool meanwhile (their caches are reset from here).
static size_t fqueue_impl_npool_trim(fqueue_impl_npool *p) {
  fqueue_impl_npool_lock(p);
  if (fqueue_impl_npool_free_nodes(p) != p->nslabs * fqueue_impl_npool_per_slab(p)) {
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
  atomic_store_explicit(&p->batches, NULL, memory_order_relaxed);
  p->nslabs = p->global = p->abandoned = 0;
  for (fqueue_impl_npool_cache *c = p->registry; c; c = c->next) {
    c->free = NULL;
    atomic_store_explicit(&c->nfree, 0, memory_order_relaxed);
    atomic_store_explicit(&c->cur, NULL, memory_order_relaxed);
    atomic_store_explicit(&c->end, NULL, memory_order_relaxed);
  }
  fqueue_impl_npool_unlock(p);
  return bytes;
}

#endif // HOOD_MELVILLE_FQUEUE_POOL_IMPL_H
