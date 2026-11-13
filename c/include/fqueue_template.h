// Functional (persistent) real-time queue of Hood & Melville on six stacks,
// generated for an element type: push, pop and front in worst-case O(1); every
// operation writes a new version and leaves its argument unchanged.
//
// Instantiation. Define the parameters and include this file — without
// FQUEUE_IMPLEMENTATION in a header (declarations), and once more with it in
// one .c file (definitions; the declarations must already be visible). The
// file has no include guard and undefines its parameters, so it can be
// instantiated for several types, even in one translation unit:
//
//   // strq.h                         // strq.c
//   #define FQUEUE_NAME strq          #include "strq.h"
//   #define FQUEUE_T    const char *  #define FQUEUE_NAME strq
//   #include "fqueue_template.h"      #define FQUEUE_T    const char *
//                                     #define FQUEUE_IMPLEMENTATION
//                                     #include "fqueue_template.h"
//
// This generates the type `strq` and functions strq_init, strq_push, ... as
// documented below with NAME = strq, T = const char *. fqueue.h is such an
// instantiation for int.
//
// Elements. T is copied with `=` and never destroyed, like elements of klib
// containers: a value is copied into several nodes while the queue rotates and
// shared between versions. For values that own resources store a pointer or a
// handle and manage their lifetime separately.
//
// Versions are values: a NAME lives in storage owned by the caller (a local
// variable, an array element, a field, ...), and creating a version allocates
// only queue nodes, never the version itself.
//
// Ownership. An initialized NAME owns references to its nodes and must be
// destroyed exactly once with NAME_destroy. It may be moved by a bitwise copy,
// after which the source must not be used until it is initialized again; a
// bitwise copy is not a second owner — use NAME_clone for that. For the
// operations below `out` is uninitialized storage (or a destroyed version) and
// must not alias `q`.
//
// Errors. Operations that may allocate return an fqueue_status. On any status
// other than FQUEUE_OK, `q` is unchanged, *out is left uninitialized (nothing
// to destroy) and no memory is leaked. The library never aborts.
//
// Reference counts saturate, like refcount_t in the Linux kernel: a node with
// UINT32_MAX - 1 references (UINT32_MAX / 2 with FQUEUE_ATOMIC) keeps that
// count for good and is never freed. Reaching it takes billions of references
// to one node, i.e. leaked versions; the operations stay correct and memory
// safe, the saturated nodes leak. So no operation fails on a reference count:
// NAME_clone cannot fail at all, FQUEUE_OVERFLOW means only the length limit.
//
// Optional parameter FQUEUE_API: a prefix for the declarations of the public
// functions, e.g. a visibility or dllexport attribute (see fqueue.h).
//
// Implementation options (define before the FQUEUE_IMPLEMENTATION include):
// FQUEUE_MALLOC and FQUEUE_FREE together replace malloc and free; they must
// have the same signatures. With them, a node pool also needs
// FQUEUE_ALIGNED_MALLOC, which replaces aligned_alloc (its blocks are freed
// by FQUEUE_FREE). FQUEUE_ATOMIC makes this instantiation thread-safe (see
// Threads); it affects only the definitions, not the declarations.
//
// Node pool. With FQUEUE_NODE_POOL defined for both includes (it changes the
// declarations too), nodes come from a pool of this instantiation instead of
// one FQUEUE_MALLOC per node: slabs of 16 KiB from FQUEUE_ALIGNED_MALLOC, a
// cache of free nodes per thread (POSIX threads); a node freed by another
// thread goes back to the thread that carved it. The queue API and its
// guarantees are the same; a pool slab that cannot be allocated is
// FQUEUE_NOMEM. Slabs are not returned while a node is live — the pool keeps
// the peak of live nodes — until NAME_pool_trim; it is the library's only
// global state. Specification and trade-offs: docs/pool.md. fqueue.h provides
// fqueue_pooled and fqueue_pooled_sync.
//
// Threads. Without FQUEUE_ATOMIC, versions that share nodes must not be used
// concurrently, even for reading: reference counts are not atomic. Versions
// with no common history are independent; the library has no global state
// (except the node pool of FQUEUE_NODE_POOL instantiations, which is
// thread-safe for any of them).
// With FQUEUE_ATOMIC (C11 atomics, memory orders as Arc in Rust), any number
// of threads may concurrently read a version and derive new versions from it
// (NAME_clone, NAME_push, NAME_pop with it as `q`, NAME_front, ...), even if
// versions share nodes. A version object itself is like any C object: writing
// it (as `out`, with NAME_destroy, NAME_push_in_place, NAME_pop_in_place, the
// iterator) must not race with other accesses to that same object. A count
// that reaches UINT32_MAX / 2 is set to 3/4 of the range and stays there under
// any race, so counts never wrap.
// fqueue.h provides both: fqueue (single-threaded) and fqueue_sync (and the
// same with a node pool: fqueue_pooled, fqueue_pooled_sync).
//
// Complexity: everything except NAME_destroy is worst-case O(1) (at most 3 new
// nodes per push and 2 per pop). NAME_destroy is not: it frees every node the
// version owned exclusively, which can be Theta(n); in total freeing costs no
// more than node creation, i.e. amortized O(1) per operation. So
// `NAME_push(&b, &a, x); NAME_destroy(&a);` is O(1) for the push but not
// necessarily for the destroy. Proof: docs/proof.md.

#include "fqueue_common.h"

#ifndef FQUEUE_NAME
#error                                                                         \
    "define FQUEUE_NAME (the name of the queue type) before including fqueue_template.h"
#endif
#ifndef FQUEUE_T
#error "define FQUEUE_T (the element type) before including fqueue_template.h"
#endif

// NAME_x (public) and NAME_impl_x (private) for this instantiation.
#define FQUEUE_IMPL_CAT2(a, b) a##_##b
#define FQUEUE_IMPL_CAT(a, b)  FQUEUE_IMPL_CAT2(a, b)
#define FQUEUE_IMPL_N(x)       FQUEUE_IMPL_CAT(FQUEUE_NAME, x)
#define FQUEUE_IMPL_P(x)       FQUEUE_IMPL_CAT(FQUEUE_NAME, impl_##x)
#define FQUEUE_IMPL_NODE       FQUEUE_IMPL_P(node)

#ifndef FQUEUE_API
#define FQUEUE_API
#endif

#ifndef FQUEUE_IMPLEMENTATION

#ifdef __cplusplus
extern "C" {
#endif

// Representation. Exposed only so that versions can be stored by value;
// everything named *_impl_* / FQUEUE_IMPL_* and all fields are private — do
// not access them.
typedef struct FQUEUE_IMPL_NODE FQUEUE_IMPL_NODE;

typedef struct {
  FQUEUE_IMPL_NODE *acc;
  FQUEUE_IMPL_NODE *rfacc;
  FQUEUE_IMPL_NODE *old_front;
  FQUEUE_IMPL_NODE *old_rear;
  fqueue_impl_len ok;
} FQUEUE_IMPL_P(reversing);

typedef struct {
  FQUEUE_IMPL_NODE *acc;
  FQUEUE_IMPL_NODE *rfacc;
  fqueue_impl_len ok;
} FQUEUE_IMPL_P(appending);

typedef struct {
  fqueue_impl_phase tag;
  union {
    FQUEUE_IMPL_P(reversing) reversing;
    FQUEUE_IMPL_P(appending) appending;
  };
} FQUEUE_IMPL_P(rotation);

typedef struct FQUEUE_NAME {
  fqueue_impl_len size;
  fqueue_impl_len front_len;
  fqueue_impl_len rear_len;
  FQUEUE_IMPL_NODE *front;
  FQUEUE_IMPL_NODE *rear;
  FQUEUE_IMPL_P(rotation) rot;
} FQUEUE_NAME;

// Initialize *q as an empty queue. Allocates nothing.
FQUEUE_API void FQUEUE_IMPL_N(init)(FQUEUE_NAME *q);

// Initialize *out as another owner of the version `q`: the same contents,
// all nodes shared, nothing allocated. Cannot fail.
FQUEUE_API void FQUEUE_IMPL_N(clone)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                                     const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q);

// Release the nodes owned by *q (shared nodes are freed only when no other
// version references them). Afterwards *q is uninitialized storage, as after
// a move: it may be passed as `out` or to NAME_init, nothing else. The
// storage of *q itself is not freed. Unlike free(), destroying twice is an
// error — like Rust's drop, and resetting *q measurably slows linear
// histories (docs/benchmarks.md).
FQUEUE_API void FQUEUE_IMPL_N(destroy)(FQUEUE_NAME *q);

// Return true iff the queue contains no elements.
FQUEUE_API bool FQUEUE_IMPL_N(is_empty)(const FQUEUE_NAME *q);

// Number of elements in the queue.
FQUEUE_API size_t FQUEUE_IMPL_N(size)(const FQUEUE_NAME *q);

// Front element: write it to *value and return true; if `q` is empty, return
// false and leave *value untouched.
FQUEUE_API bool FQUEUE_IMPL_N(front)(const FQUEUE_NAME *q, FQUEUE_T *value);

// Enqueue: initialize *out as `q` with `value` appended to the rear.
// Fails with FQUEUE_NOMEM, or FQUEUE_OVERFLOW at the length limit.
FQUEUE_IMPL_NODISCARD FQUEUE_API fqueue_status FQUEUE_IMPL_N(push)(
    FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
    const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q, FQUEUE_T value);

// Dequeue: initialize *out as `q` without its front element. Fails with
// FQUEUE_EMPTY (*out is then not even written) or FQUEUE_NOMEM.
FQUEUE_IMPL_NODISCARD FQUEUE_API fqueue_status
    FQUEUE_IMPL_N(pop)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                       const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q);

// In-place enqueue: append `value` to *q itself — the same as NAME_push into
// a new version followed by NAME_destroy of *q, but without copying the
// version and releasing the old one. Other versions sharing nodes with *q are
// not affected. The operation itself is O(1); like NAME_destroy of the old
// version, it may also free nodes that *q owned alone. Fails with
// FQUEUE_NOMEM, or FQUEUE_OVERFLOW at the length limit, leaving *q unchanged.
FQUEUE_IMPL_NODISCARD FQUEUE_API fqueue_status
    FQUEUE_IMPL_N(push_in_place)(FQUEUE_NAME *q, FQUEUE_T value);

// In-place dequeue: remove the front element of *q itself and write it to
// *value unless `value` is NULL; see NAME_push_in_place. Fails with
// FQUEUE_EMPTY or FQUEUE_NOMEM, leaving *q and *value unchanged.
FQUEUE_IMPL_NODISCARD FQUEUE_API fqueue_status
    FQUEUE_IMPL_N(pop_in_place)(FQUEUE_NAME *q, FQUEUE_T *value);

// Iterator over the elements of a version, front to back. It owns a clone of
// the version (which may be destroyed meanwhile) and pops it step by step, so
// a step is worst-case O(1) like pop — and, like pop, may need to allocate:
// in-order traversal of the stacks without allocation is not O(1) per step.
// The same design as Iter in Rust.
typedef struct {
  FQUEUE_NAME rest; // private
} FQUEUE_IMPL_N(iter);

// Initialize *it at the front of `q`. Cannot fail, as NAME_clone.
FQUEUE_API void FQUEUE_IMPL_N(iter_init)(FQUEUE_IMPL_N(iter) * it,
                                         const FQUEUE_NAME *q);

// Next element: write it to *value and return FQUEUE_OK; FQUEUE_EMPTY after
// the last one. FQUEUE_NOMEM leaves *it and *value unchanged, so the step can
// be retried.
FQUEUE_IMPL_NODISCARD FQUEUE_API fqueue_status
    FQUEUE_IMPL_N(iter_next)(FQUEUE_IMPL_N(iter) * it, FQUEUE_T *value);

// Release the iterator, at any point of the iteration.
FQUEUE_API void FQUEUE_IMPL_N(iter_destroy)(FQUEUE_IMPL_N(iter) * it);

#ifdef FQUEUE_NODE_POOL
// Statistics of this instantiation's node pool.
FQUEUE_API void FQUEUE_IMPL_N(pool_stats)(fqueue_pool_stats *stats);

// Return every slab of the pool to FQUEUE_FREE if no node is live (every
// version of this type destroyed); otherwise do nothing. Returns the bytes
// returned. No other thread may use the pool meanwhile.
FQUEUE_API size_t FQUEUE_IMPL_N(pool_trim)(void);
#endif

#ifdef __cplusplus
}
#endif

#else // FQUEUE_IMPLEMENTATION

#include <assert.h>
#include <stdlib.h>
#ifdef FQUEUE_ATOMIC
#include <stdatomic.h>
#endif

// Allocator and limits, shared by all instantiations in this translation unit.
#ifndef FQUEUE_IMPL_CONFIGURED
#define FQUEUE_IMPL_CONFIGURED
#if defined(FQUEUE_MALLOC) != defined(FQUEUE_FREE)
#error "define both FQUEUE_MALLOC and FQUEUE_FREE, or neither"
#endif
#ifdef FQUEUE_MALLOC
void *FQUEUE_MALLOC(size_t size);
void FQUEUE_FREE(void *ptr);
#ifdef FQUEUE_ALIGNED_MALLOC
void *FQUEUE_ALIGNED_MALLOC(size_t alignment, size_t size);
#endif
#else
#if defined(FQUEUE_ALIGNED_MALLOC)
#error "FQUEUE_ALIGNED_MALLOC goes with FQUEUE_MALLOC and FQUEUE_FREE"
#endif
#define FQUEUE_MALLOC         malloc
#define FQUEUE_FREE           free
#define FQUEUE_ALIGNED_MALLOC aligned_alloc
#endif

// Limits; the tests lower them to reach them. A queue holds at most LEN_LIMIT
// elements. A reference count saturates at REFS_LIMIT (see Errors).
#ifndef FQUEUE_IMPL_LEN_LIMIT
#define FQUEUE_IMPL_LEN_LIMIT UINT32_MAX
#endif
#ifndef FQUEUE_IMPL_REFS_LIMIT
#define FQUEUE_IMPL_REFS_LIMIT (UINT32_MAX - 1)
#endif
// FQUEUE_ATOMIC: a count found at or above the limit is set to the middle of
// the saturated range, 2^30 away from both the limit and the wrap — further
// than any number of racing threads can move it.
#ifndef FQUEUE_IMPL_ATOMIC_REFS_LIMIT
#define FQUEUE_IMPL_ATOMIC_REFS_LIMIT (UINT32_MAX / 2)
#endif

// The persistent and the in-place operations share the code of a rotation
// step; forced inlining with a constant pool argument gives the persistent
// operations the same code as without the pool.
#if defined(__GNUC__)
#define FQUEUE_IMPL_ALWAYS_INLINE inline __attribute__((always_inline))
#define FQUEUE_IMPL_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#define FQUEUE_IMPL_ALWAYS_INLINE __forceinline
#define FQUEUE_IMPL_UNLIKELY(x)   (x)
#else
#define FQUEUE_IMPL_ALWAYS_INLINE inline
#define FQUEUE_IMPL_UNLIKELY(x)   (x)
#endif
#if defined(__SANITIZE_THREAD__)
#define FQUEUE_IMPL_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FQUEUE_IMPL_TSAN 1
#endif
#endif
#ifndef FQUEUE_IMPL_TSAN
#define FQUEUE_IMPL_TSAN 0
#endif
#endif // FQUEUE_IMPL_CONFIGURED

struct FQUEUE_IMPL_NODE {
  FQUEUE_T value;
#ifdef FQUEUE_ATOMIC
  _Atomic uint32_t refcount;
#else
  uint32_t refcount;
#endif
  struct FQUEUE_IMPL_NODE *next;
};

// Where nodes come from: FQUEUE_MALLOC/FQUEUE_FREE per node, or the pool of
// this instantiation (fqueue_pool_impl.h, docs/pool.md).
#ifdef FQUEUE_NODE_POOL
#ifndef FQUEUE_ALIGNED_MALLOC
#error "a node pool with FQUEUE_MALLOC needs FQUEUE_ALIGNED_MALLOC"
#endif
#include "fqueue_pool_impl.h"
_Static_assert(sizeof(FQUEUE_IMPL_NODE) >= 2 * sizeof(void *),
               "a free pool node holds two pointers");
static fqueue_impl_npool
    FQUEUE_IMPL_P(node_pool) = FQUEUE_IMPL_NPOOL_INIT(FQUEUE_IMPL_NODE);
static _Thread_local fqueue_impl_npool_cache *FQUEUE_IMPL_P(node_pool_cache);
#define FQUEUE_IMPL_NODE_NEW()                                                 \
  ((FQUEUE_IMPL_NODE *)fqueue_impl_npool_alloc(                                \
      &FQUEUE_IMPL_P(node_pool), &FQUEUE_IMPL_P(node_pool_cache)))
#ifdef FQUEUE_ATOMIC
#define FQUEUE_IMPL_NODE_DELETE(n)                                             \
  fqueue_impl_npool_free_outlined(&FQUEUE_IMPL_P(node_pool),                   \
                                  &FQUEUE_IMPL_P(node_pool_cache), (n))
#else
#define FQUEUE_IMPL_NODE_DELETE(n)                                             \
  fqueue_impl_npool_free(&FQUEUE_IMPL_P(node_pool),                            \
                         &FQUEUE_IMPL_P(node_pool_cache), (n))
#endif

void FQUEUE_IMPL_N(pool_stats)(fqueue_pool_stats *stats) {
  fqueue_impl_npool_stats(&FQUEUE_IMPL_P(node_pool), stats);
}

size_t FQUEUE_IMPL_N(pool_trim)(void) {
  return fqueue_impl_npool_trim(&FQUEUE_IMPL_P(node_pool));
}
#else
#define FQUEUE_IMPL_NODE_NEW()                                                 \
  ((FQUEUE_IMPL_NODE *)FQUEUE_MALLOC(sizeof(FQUEUE_IMPL_NODE)))
#define FQUEUE_IMPL_NODE_DELETE(n) FQUEUE_FREE(n)
#endif

// Reference count primitives. Atomic memory orders are those of Arc in Rust:
// a new reference is made from an existing one, so the increment is relaxed;
// a decrement is release, and the last owner issues an acquire fence before
// freeing, so all accesses of the other owners (reads of value and next)
// happen before the free; the uniqueness check is acquire for the same reason
// (the caller then takes the node apart and frees it). Counts saturate at
// FQUEUE_IMPL_LIMIT (see Errors); a saturated node is never freed.
#ifdef FQUEUE_ATOMIC
#define FQUEUE_IMPL_LIMIT     FQUEUE_IMPL_ATOMIC_REFS_LIMIT
#define FQUEUE_IMPL_SATURATED (FQUEUE_IMPL_LIMIT + FQUEUE_IMPL_LIMIT / 2)

static inline void FQUEUE_IMPL_P(refs_init)(FQUEUE_IMPL_NODE *n) {
  atomic_init(&n->refcount, 1);
}
static inline uint32_t FQUEUE_IMPL_P(refs)(FQUEUE_IMPL_NODE *n) {
  return atomic_load_explicit(&n->refcount, memory_order_relaxed);
}
static inline bool FQUEUE_IMPL_P(refs_unique)(FQUEUE_IMPL_NODE *n) {
  return atomic_load_explicit(&n->refcount, memory_order_acquire) == 1;
}
static inline void FQUEUE_IMPL_P(refs_inc)(FQUEUE_IMPL_NODE *n) {
  uint32_t old =
      atomic_fetch_add_explicit(&n->refcount, 1, memory_order_relaxed);
  if (FQUEUE_IMPL_UNLIKELY(old >= FQUEUE_IMPL_LIMIT))
    atomic_store_explicit(&n->refcount, FQUEUE_IMPL_SATURATED,
                          memory_order_relaxed);
}
// True if the reference was the last one.
static inline bool FQUEUE_IMPL_P(refs_dec)(FQUEUE_IMPL_NODE *n) {
  uint32_t old =
      atomic_fetch_sub_explicit(&n->refcount, 1, memory_order_release);
  if (FQUEUE_IMPL_UNLIKELY(old > FQUEUE_IMPL_LIMIT)) {
    atomic_store_explicit(&n->refcount, FQUEUE_IMPL_SATURATED,
                          memory_order_relaxed);
    return false;
  }
  if (old != 1) return false;
#if FQUEUE_IMPL_TSAN
  // ThreadSanitizer does not model fences; an acquire load of the count
  // synchronizes the same way (as Arc in Rust does under TSan).
  (void)atomic_load_explicit(&n->refcount, memory_order_acquire);
#else
  atomic_thread_fence(memory_order_acquire);
#endif
  return true;
}
// Load the count (and discard it) so that its cache line arrives early.
static inline void FQUEUE_IMPL_P(touch)(FQUEUE_IMPL_NODE *n) {
  (void)atomic_load_explicit(&n->refcount, memory_order_relaxed);
}
#else
#define FQUEUE_IMPL_LIMIT FQUEUE_IMPL_REFS_LIMIT

static inline void FQUEUE_IMPL_P(refs_init)(FQUEUE_IMPL_NODE *n) {
  n->refcount = 1;
}
static inline uint32_t FQUEUE_IMPL_P(refs)(FQUEUE_IMPL_NODE *n) {
  return n->refcount;
}
static inline bool FQUEUE_IMPL_P(refs_unique)(FQUEUE_IMPL_NODE *n) {
  return n->refcount == 1;
}
static inline void FQUEUE_IMPL_P(refs_inc)(FQUEUE_IMPL_NODE *n) {
  n->refcount += n->refcount != FQUEUE_IMPL_LIMIT;
}
static inline bool FQUEUE_IMPL_P(refs_dec)(FQUEUE_IMPL_NODE *n) {
  if (FQUEUE_IMPL_UNLIKELY(n->refcount == FQUEUE_IMPL_LIMIT)) return false;
  return --n->refcount == 0;
}
static inline void FQUEUE_IMPL_P(touch)(FQUEUE_IMPL_NODE *n) {
  (void)*(volatile uint32_t *)&n->refcount;
}
#endif

// Increment refcount; NULL-safe; saturating.
static inline void FQUEUE_IMPL_P(retain)(FQUEUE_IMPL_NODE *n) {
  if (n) FQUEUE_IMPL_P(refs_inc)(n);
}

// Decrement refcount iteratively; free chain when count reaches 0.
// Before free(n) the next node's refcount is loaded (and discarded) so that
// its cache miss overlaps with free() instead of stalling the next iteration.
// Measured across 4 code layouts: -3..5% cycles, and no slow allocator mode in
// mixed/fill 10^7; __builtin_prefetch in the same place gave no effect.
static void FQUEUE_IMPL_P(release)(FQUEUE_IMPL_NODE *n) {
  while (n && FQUEUE_IMPL_P(refs_dec)(n)) {
    FQUEUE_IMPL_NODE *next = n->next;
    if (next) FQUEUE_IMPL_P(touch)(next);
    FQUEUE_IMPL_NODE_DELETE(n);
    n = next;
  }
}

// Nodes allocated ahead of an in-place operation, so that the operation
// itself cannot fail. A rotation step allocates at most 2 (REVERSING).
typedef struct {
  FQUEUE_IMPL_NODE *nodes[2];
  unsigned n;
} FQUEUE_IMPL_P(pool);

// Allocate a new node (refcount=1) on top of `next`: from `pool` if it is not
// NULL, otherwise with FQUEUE_MALLOC. Takes over the caller's reference to
// `next` instead of adding one, like Stack::cons in Rust. Returns NULL,
// leaving the reference with the caller, if allocation fails.
static FQUEUE_IMPL_ALWAYS_INLINE FQUEUE_IMPL_NODE *
FQUEUE_IMPL_P(cons)(FQUEUE_T value, FQUEUE_IMPL_NODE *next,
                    FQUEUE_IMPL_P(pool) * pool) {
  FQUEUE_IMPL_NODE *n;
  if (pool) {
    assert(pool->n > 0);
    n = pool->nodes[--pool->n];
  } else {
    n = FQUEUE_IMPL_NODE_NEW();
    if (!n) return NULL;
  }
  n->value = value;
  FQUEUE_IMPL_P(refs_init)(n);
  n->next = next;
  return n;
}

// Retain all node pointers held by a rotation, based on its current tag.
static void FQUEUE_IMPL_P(rotation_retain)(const FQUEUE_IMPL_P(rotation) * r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE:
    break;
  case FQUEUE_IMPL_REVERSING:
    FQUEUE_IMPL_P(retain)(r->reversing.acc);
    FQUEUE_IMPL_P(retain)(r->reversing.rfacc);
    FQUEUE_IMPL_P(retain)(r->reversing.old_front);
    FQUEUE_IMPL_P(retain)(r->reversing.old_rear);
    break;
  case FQUEUE_IMPL_APPENDING:
    FQUEUE_IMPL_P(retain)(r->appending.acc);
    FQUEUE_IMPL_P(retain)(r->appending.rfacc);
    break;
  }
}

// Release all node pointers held by a rotation and reset it to IDLE.
static void FQUEUE_IMPL_P(rotation_release)(FQUEUE_IMPL_P(rotation) * r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE:
    break;
  case FQUEUE_IMPL_REVERSING:
    FQUEUE_IMPL_P(release)(r->reversing.acc);
    FQUEUE_IMPL_P(release)(r->reversing.rfacc);
    FQUEUE_IMPL_P(release)(r->reversing.old_front);
    FQUEUE_IMPL_P(release)(r->reversing.old_rear);
    break;
  case FQUEUE_IMPL_APPENDING:
    FQUEUE_IMPL_P(release)(r->appending.acc);
    FQUEUE_IMPL_P(release)(r->appending.rfacc);
    break;
  }
  r->tag = FQUEUE_IMPL_IDLE;
}

// Make *dst a copy of *src that holds its own references to all shared nodes.
static void
FQUEUE_IMPL_P(copy_into)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT dst,
                         const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT src) {
  *dst = *src;
  FQUEUE_IMPL_P(retain)(dst->front);
  FQUEUE_IMPL_P(retain)(dst->rear);
  FQUEUE_IMPL_P(rotation_retain)(&dst->rot);
}

// Replace the reference `n` with a reference to its successor, like
// Stack::advance in Rust. A node owned only by us is freed and hands its
// reference to the successor over without touching the count (the successor's
// count is loaded early, as in release); a shared node stays with the other
// versions, and we take a new reference to its successor.
static FQUEUE_IMPL_NODE *FQUEUE_IMPL_P(advance)(FQUEUE_IMPL_NODE *n) {
  if (!n) return NULL;
  FQUEUE_IMPL_NODE *next = n->next;
  if (FQUEUE_IMPL_P(refs_unique)(n)) {
    if (next) FQUEUE_IMPL_P(touch)(next);
    FQUEUE_IMPL_NODE_DELETE(n);
    return next;
  }
#ifdef FQUEUE_ATOMIC
  // The other owners may drop theirs meanwhile, making ours the last
  // reference: take the successor first, then release n in full.
  FQUEUE_IMPL_P(retain)(next);
  FQUEUE_IMPL_P(release)(n);
#else
  (void)FQUEUE_IMPL_P(refs_dec)(n); // shared: not the last reference
  FQUEUE_IMPL_P(retain)(next);
#endif
  return next;
}

// *dst = cons(value, *dst): the reference *dst held moves into the new node.
// Returns false, leaving *dst as it was, if allocation fails.
static FQUEUE_IMPL_ALWAYS_INLINE bool
FQUEUE_IMPL_P(cons_prepend)(FQUEUE_IMPL_NODE **dst, FQUEUE_T value,
                            FQUEUE_IMPL_P(pool) * pool) {
  FQUEUE_IMPL_NODE *n = FQUEUE_IMPL_P(cons)(value, *dst, pool);
  if (!n) return false;
  *dst = n;
  return true;
}

static void FQUEUE_IMPL_P(start_rotation)(FQUEUE_NAME *q) {
  q->rot.tag = FQUEUE_IMPL_REVERSING;
  // old_front shares front's node list — give old_front its own reference.
  q->rot.reversing.old_front = q->front;
  // old_rear takes ownership of rear; rear pointer is cleared.
  q->rot.reversing.old_rear = q->rear; // transfer: rear's ref moves to old_rear
  q->rear = NULL;
  q->rear_len = 0;
  q->rot.reversing.acc = NULL;
  q->rot.reversing.rfacc = NULL;
  q->rot.reversing.ok = q->front_len;
  FQUEUE_IMPL_P(retain)(q->front);
}

// Install acc as the new front and reset rotation state.
static void FQUEUE_IMPL_P(finish_rotation)(FQUEUE_NAME *q) {
  FQUEUE_IMPL_P(release)(q->front);
  q->front = q->rot.appending.acc; // acc ownership moves to front
  FQUEUE_IMPL_P(release)(q->rot.appending.rfacc); // leftover rfacc (ok == 0)
  q->front_len = q->size - q->rear_len;
  q->rot.tag = FQUEUE_IMPL_IDLE;
}

// One step of the ongoing rotation, taking new nodes from `pool` unless it is
// NULL. Modifies q in place. Returns false if allocation fails; q may then be
// half-stepped, but every field still owns its reference, so NAME_destroy
// undoes it.
static FQUEUE_IMPL_ALWAYS_INLINE bool
FQUEUE_IMPL_P(rotate_step_from)(FQUEUE_NAME *q, FQUEUE_IMPL_P(pool) * pool) {
  if (q->rot.tag == FQUEUE_IMPL_IDLE) return true;

  if (q->rot.tag == FQUEUE_IMPL_REVERSING) {
    if (!FQUEUE_IMPL_P(cons_prepend)(&q->rot.reversing.acc,
                                     q->rot.reversing.old_rear->value, pool))
      return false;
    q->rot.reversing.old_rear =
        FQUEUE_IMPL_P(advance)(q->rot.reversing.old_rear);

    if (q->rot.reversing.old_front != NULL) {
      if (!FQUEUE_IMPL_P(cons_prepend)(&q->rot.reversing.rfacc,
                                       q->rot.reversing.old_front->value, pool))
        return false;
      q->rot.reversing.old_front =
          FQUEUE_IMPL_P(advance)(q->rot.reversing.old_front);
    }

    if (q->rot.reversing.old_rear != NULL) return true;

    // Transition to APPENDING: extract fields before overwriting the union.
    // The step ends here, like σ in docs/proof.md and step_mut in Rust: the
    // first APPENDING step is the next operation's.
    FQUEUE_IMPL_NODE *acc = q->rot.reversing.acc;
    FQUEUE_IMPL_NODE *rfacc = q->rot.reversing.rfacc;
    fqueue_impl_len ok = q->rot.reversing.ok;
    q->rot.tag = FQUEUE_IMPL_APPENDING;
    q->rot.appending.acc = acc;
    q->rot.appending.rfacc = rfacc;
    q->rot.appending.ok = ok;
    // ok == 0: every element left in rfacc has been popped — done.
    if (ok == 0) FQUEUE_IMPL_P(finish_rotation)(q);
    return true;
  }

  // FQUEUE_IMPL_APPENDING
  // Complete immediately:
  //   rfacc == NULL → naturally exhausted all old_front elements
  //   ok == 0       → all remaining steps are no-ops (elements already popped)
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0) {
    FQUEUE_IMPL_P(finish_rotation)(q);
    return true;
  }
  if (!FQUEUE_IMPL_P(cons_prepend)(&q->rot.appending.acc,
                                   q->rot.appending.rfacc->value, pool))
    return false;
  q->rot.appending.ok--;
  q->rot.appending.rfacc = FQUEUE_IMPL_P(advance)(q->rot.appending.rfacc);
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0)
    FQUEUE_IMPL_P(finish_rotation)(q);
  return true;
}

// A step of the persistent operations: q is a fresh copy, nodes come from
// FQUEUE_MALLOC.
static bool FQUEUE_IMPL_P(rotate_step)(FQUEUE_NAME *q) {
  return FQUEUE_IMPL_P(rotate_step_from)(q, NULL);
}

// Error exit of an operation: undo the partially built *out.
static fqueue_status FQUEUE_IMPL_P(fail)(FQUEUE_NAME *out,
                                         fqueue_status status) {
  FQUEUE_IMPL_N(destroy)(out);
  return status;
}

void FQUEUE_IMPL_N(init)(FQUEUE_NAME *q) {
  q->size = 0;
  q->front_len = 0;
  q->rear_len = 0;
  q->front = NULL;
  q->rear = NULL;
  q->rot = (FQUEUE_IMPL_P(rotation)){.tag = FQUEUE_IMPL_IDLE};
}

void FQUEUE_IMPL_N(clone)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                          const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q) {
  assert(out != q);
  FQUEUE_IMPL_P(copy_into)(out, q);
}

void FQUEUE_IMPL_N(destroy)(FQUEUE_NAME *q) {
  FQUEUE_IMPL_P(release)(q->front);
  FQUEUE_IMPL_P(release)(q->rear);
  FQUEUE_IMPL_P(rotation_release)(&q->rot);
}

bool FQUEUE_IMPL_N(is_empty)(const FQUEUE_NAME *q) { return q->size == 0; }

size_t FQUEUE_IMPL_N(size)(const FQUEUE_NAME *q) { return q->size; }

fqueue_status FQUEUE_IMPL_N(push)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                                  const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q,
                                  FQUEUE_T value) {
  assert(out != q);
  if (q->size == FQUEUE_IMPL_LEN_LIMIT) return FQUEUE_OVERFLOW;
  FQUEUE_IMPL_P(copy_into)(out, q);
  // The copy's reference to rear moves into the new node.
  if (!FQUEUE_IMPL_P(cons_prepend)(&out->rear, value, NULL))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  out->rear_len++;
  out->size++;
  if (out->rot.tag == FQUEUE_IMPL_IDLE && out->rear_len > out->front_len)
    FQUEUE_IMPL_P(start_rotation)(out);
  // One step suffices: docs/proof.md.
  if (!FQUEUE_IMPL_P(rotate_step)(out))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  return FQUEUE_OK;
}

fqueue_status FQUEUE_IMPL_N(pop)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                                 const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q) {
  assert(out != q);
  if (q->size == 0) return FQUEUE_EMPTY;
  FQUEUE_IMPL_P(copy_into)(out, q);
  // The head is shared with q.
  out->front = FQUEUE_IMPL_P(advance)(out->front);
  out->front_len--;
  out->size--;
  // During a rotation ok >= 1 here (lemma 2 in docs/proof.md).
  if (out->rot.tag == FQUEUE_IMPL_REVERSING) {
    assert(out->rot.reversing.ok > 0);
    out->rot.reversing.ok--;
  } else if (out->rot.tag == FQUEUE_IMPL_APPENDING) {
    assert(out->rot.appending.ok > 0);
    out->rot.appending.ok--;
  } else if (out->rear_len > out->front_len) {
    FQUEUE_IMPL_P(start_rotation)(out);
  }
  // One step suffices: docs/proof.md.
  if (!FQUEUE_IMPL_P(rotate_step)(out))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  return FQUEUE_OK;
}

// In-place operations change *q where it is: no copy_into (a retain per
// field) and no destroy of the old version (a release per field). To leave *q
// unchanged on failure, everything that can fail — the allocations — happens
// before the first change: the nodes the rotation step will allocate are
// counted from the state (step_allocs only reads it) and allocated up front
// into a pool (with the node pool, reserved; fqueue_impl_npool_reserve).

// Nodes allocated by one rotate_step_from on rotation `r`; mirrors its
// branches without changing anything.
static unsigned FQUEUE_IMPL_P(step_allocs)(const FQUEUE_IMPL_P(rotation) * r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE:
    return 0;
  case FQUEUE_IMPL_REVERSING:
    return 1 + (r->reversing.old_front != NULL);
  case FQUEUE_IMPL_APPENDING:
    return r->appending.rfacc != NULL && r->appending.ok != 0;
  }
  return 0;
}

static bool FQUEUE_IMPL_P(pool_fill)(FQUEUE_IMPL_P(pool) * p, unsigned n) {
#ifdef FQUEUE_NODE_POOL
  // The node pool reserves without allocating: the step allocates the nodes
  // itself, after its frees (fqueue_impl_npool_reserve).
  p->n = 0;
  return fqueue_impl_npool_reserve(&FQUEUE_IMPL_P(node_pool),
                                   &FQUEUE_IMPL_P(node_pool_cache), n);
#else
  for (p->n = 0; p->n < n; p->n++) {
    p->nodes[p->n] = FQUEUE_IMPL_NODE_NEW();
    if (!p->nodes[p->n]) {
      while (p->n > 0) FQUEUE_IMPL_NODE_DELETE(p->nodes[--p->n]);
      return false;
    }
  }
  return true;
#endif
}

fqueue_status FQUEUE_IMPL_N(push_in_place)(FQUEUE_NAME *q, FQUEUE_T value) {
  if (q->size == FQUEUE_IMPL_LEN_LIMIT) return FQUEUE_OVERFLOW;
  FQUEUE_IMPL_NODE *rear = FQUEUE_IMPL_P(cons)(value, q->rear, NULL);
  if (!rear) return FQUEUE_NOMEM;
  // The rotation as the step will see it: as in NAME_push.
  bool start = q->rot.tag == FQUEUE_IMPL_IDLE && q->rear_len + 1 > q->front_len;
  FQUEUE_IMPL_P(rotation) r = q->rot;
  if (start)
    r = (FQUEUE_IMPL_P(rotation)){.tag = FQUEUE_IMPL_REVERSING,
                                  .reversing = {.old_front = q->front,
                                                .old_rear = rear,
                                                .ok = q->front_len}};
  FQUEUE_IMPL_P(pool) pool;
  if (!FQUEUE_IMPL_P(pool_fill)(&pool, FQUEUE_IMPL_P(step_allocs)(&r))) {
    FQUEUE_IMPL_NODE_DELETE(rear); // the reference to q->rear stays with q
    return FQUEUE_NOMEM;
  }

  // From here on nothing fails.
  q->rear = rear;
  q->rear_len++;
  q->size++;
  if (start) FQUEUE_IMPL_P(start_rotation)(q);
#ifdef FQUEUE_NODE_POOL
  (void)FQUEUE_IMPL_P(rotate_step_from)(q, NULL); // reserved
#else
  (void)FQUEUE_IMPL_P(rotate_step_from)(q, &pool);
#endif
  assert(pool.n == 0);
  return FQUEUE_OK;
}

fqueue_status FQUEUE_IMPL_N(pop_in_place)(FQUEUE_NAME *q, FQUEUE_T *value) {
  if (q->size == 0) return FQUEUE_EMPTY;
  // The rotation as the step will see it: as in NAME_pop.
  FQUEUE_IMPL_NODE *front = q->front; // not NULL in a non-empty queue
  bool start = false;
  FQUEUE_IMPL_P(rotation) r = q->rot;
  if (r.tag == FQUEUE_IMPL_REVERSING) {
    r.reversing.ok--;
  } else if (r.tag == FQUEUE_IMPL_APPENDING) {
    r.appending.ok--;
  } else if (q->rear_len > q->front_len - 1) {
    start = true;
    r = (FQUEUE_IMPL_P(rotation)){.tag = FQUEUE_IMPL_REVERSING,
                                  .reversing = {.old_front = front->next,
                                                .old_rear = q->rear,
                                                .ok = q->front_len - 1}};
  }
  FQUEUE_IMPL_P(pool) pool;
  if (!FQUEUE_IMPL_P(pool_fill)(&pool, FQUEUE_IMPL_P(step_allocs)(&r)))
    return FQUEUE_NOMEM;

  // From here on nothing fails.
  if (value) *value = front->value;
  q->front = FQUEUE_IMPL_P(advance)(front);
  q->front_len--;
  q->size--;
  if (q->rot.tag == FQUEUE_IMPL_REVERSING) {
    assert(q->rot.reversing.ok > 0);
    q->rot.reversing.ok--;
  } else if (q->rot.tag == FQUEUE_IMPL_APPENDING) {
    assert(q->rot.appending.ok > 0);
    q->rot.appending.ok--;
  } else if (start) {
    FQUEUE_IMPL_P(start_rotation)(q);
  }
#ifdef FQUEUE_NODE_POOL
  (void)FQUEUE_IMPL_P(rotate_step_from)(q, NULL); // reserved
#else
  (void)FQUEUE_IMPL_P(rotate_step_from)(q, &pool);
#endif
  assert(pool.n == 0);
  return FQUEUE_OK;
}

bool FQUEUE_IMPL_N(front)(const FQUEUE_NAME *q, FQUEUE_T *value) {
  if (!q->front) return false;
  *value = q->front->value;
  return true;
}

void FQUEUE_IMPL_N(iter_init)(FQUEUE_IMPL_N(iter) * it, const FQUEUE_NAME *q) {
  FQUEUE_IMPL_N(clone)(&it->rest, q);
}

fqueue_status FQUEUE_IMPL_N(iter_next)(FQUEUE_IMPL_N(iter) * it,
                                       FQUEUE_T *value) {
  return FQUEUE_IMPL_N(pop_in_place)(&it->rest, value);
}

void FQUEUE_IMPL_N(iter_destroy)(FQUEUE_IMPL_N(iter) * it) {
  FQUEUE_IMPL_N(destroy)(&it->rest);
}

#endif // FQUEUE_IMPLEMENTATION

#undef FQUEUE_IMPL_CAT2
#undef FQUEUE_IMPL_CAT
#undef FQUEUE_IMPL_N
#undef FQUEUE_IMPL_P
#undef FQUEUE_IMPL_NODE
#undef FQUEUE_IMPL_LIMIT
#undef FQUEUE_IMPL_SATURATED
#undef FQUEUE_IMPL_NODE_NEW
#undef FQUEUE_IMPL_NODE_DELETE
#undef FQUEUE_ATOMIC
#undef FQUEUE_NODE_POOL
#undef FQUEUE_NAME
#undef FQUEUE_T
#undef FQUEUE_IMPLEMENTATION
#undef FQUEUE_API
