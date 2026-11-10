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
// Errors. Operations that create a version return an fqueue_status. On any
// status other than FQUEUE_OK, `q` is unchanged, *out is left uninitialized
// (nothing to destroy) and no memory is leaked. The library never aborts.
//
// Optional parameter FQUEUE_API: a prefix for the declarations of the public
// functions, e.g. a visibility or dllexport attribute (see fqueue.h).
//
// Implementation options (define before the FQUEUE_IMPLEMENTATION include):
// FQUEUE_MALLOC and FQUEUE_FREE together replace malloc and free; they must
// have the same signatures.
//
// Threads. Versions that share nodes must not be used concurrently, even for
// reading: reference counts are not atomic. Versions with no common history
// are independent; the library has no global state.
//
// Complexity: everything except NAME_destroy is worst-case O(1) (at most 3 new
// nodes per push and 2 per pop). NAME_destroy is not: it frees every node the
// version owned exclusively, which can be Theta(n); in total freeing costs no
// more than node creation, i.e. amortized O(1) per operation. So
// `NAME_push(&b, &a, x); NAME_destroy(&a);` is O(1) for the push but not
// necessarily for the destroy. Proof: docs/proof.md.

#include "fqueue_common.h"

#ifndef FQUEUE_NAME
#error "define FQUEUE_NAME (the name of the queue type) before including fqueue_template.h"
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
  fqueue_impl_len   ok;
} FQUEUE_IMPL_P(reversing);

typedef struct {
  FQUEUE_IMPL_NODE *acc;
  FQUEUE_IMPL_NODE *rfacc;
  fqueue_impl_len   ok;
} FQUEUE_IMPL_P(appending);

typedef struct {
  fqueue_impl_phase tag;
  union {
    FQUEUE_IMPL_P(reversing) reversing;
    FQUEUE_IMPL_P(appending) appending;
  };
} FQUEUE_IMPL_P(rotation);

typedef struct FQUEUE_NAME {
  fqueue_impl_len          size;
  fqueue_impl_len          front_len;
  fqueue_impl_len          rear_len;
  FQUEUE_IMPL_NODE        *front;
  FQUEUE_IMPL_NODE        *rear;
  FQUEUE_IMPL_P(rotation)  rot;
} FQUEUE_NAME;

// Initialize *q as an empty queue. Allocates nothing.
FQUEUE_API void FQUEUE_IMPL_N(init)(FQUEUE_NAME* q);

// Initialize *out as another owner of the version `q`: the same contents,
// all nodes shared, nothing allocated. Fails only with FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD FQUEUE_API
fqueue_status FQUEUE_IMPL_N(clone)(FQUEUE_NAME* FQUEUE_IMPL_RESTRICT out,
                                   const FQUEUE_NAME* FQUEUE_IMPL_RESTRICT q);

// Release the nodes owned by *q (shared nodes are freed only when no other
// version references them). Afterwards *q is uninitialized storage, as after
// a move: it may be passed as `out` or to NAME_init, nothing else. The
// storage of *q itself is not freed. Unlike free(), destroying twice is an
// error — like Rust's drop, and resetting *q measurably slows linear
// histories (docs/benchmarks.md).
FQUEUE_API void FQUEUE_IMPL_N(destroy)(FQUEUE_NAME* q);

// Return true iff the queue contains no elements.
FQUEUE_API bool FQUEUE_IMPL_N(is_empty)(const FQUEUE_NAME* q);

// Number of elements in the queue.
FQUEUE_API size_t FQUEUE_IMPL_N(size)(const FQUEUE_NAME* q);

// Front element: write it to *value and return true; if `q` is empty, return
// false and leave *value untouched.
FQUEUE_API bool FQUEUE_IMPL_N(front)(const FQUEUE_NAME* q, FQUEUE_T* value);

// Enqueue: initialize *out as `q` with `value` appended to the rear.
// Fails with FQUEUE_NOMEM or FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD FQUEUE_API
fqueue_status FQUEUE_IMPL_N(push)(FQUEUE_NAME* FQUEUE_IMPL_RESTRICT out,
                                  const FQUEUE_NAME* FQUEUE_IMPL_RESTRICT q,
                                  FQUEUE_T value);

// Dequeue: initialize *out as `q` without its front element. Fails with
// FQUEUE_EMPTY (*out is then not even written), FQUEUE_NOMEM or
// FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD FQUEUE_API
fqueue_status FQUEUE_IMPL_N(pop)(FQUEUE_NAME* FQUEUE_IMPL_RESTRICT out,
                                 const FQUEUE_NAME* FQUEUE_IMPL_RESTRICT q);

// Iterator over the elements of a version, front to back. It owns a clone of
// the version (which may be destroyed meanwhile) and pops it step by step, so
// a step is worst-case O(1) like pop — and, like pop, may need to allocate:
// in-order traversal of the stacks without allocation is not O(1) per step.
// The same design as Iter in Rust.
typedef struct {
  FQUEUE_NAME rest; // private
} FQUEUE_IMPL_N(iter);

// Initialize *it at the front of `q`. Fails only with FQUEUE_OVERFLOW, as
// NAME_clone; *it is then uninitialized.
FQUEUE_IMPL_NODISCARD FQUEUE_API
fqueue_status FQUEUE_IMPL_N(iter_init)(FQUEUE_IMPL_N(iter)* it,
                                       const FQUEUE_NAME* q);

// Next element: write it to *value and return FQUEUE_OK; FQUEUE_EMPTY after
// the last one. FQUEUE_NOMEM or FQUEUE_OVERFLOW leave *it and *value
// unchanged, so the step can be retried.
FQUEUE_IMPL_NODISCARD FQUEUE_API
fqueue_status FQUEUE_IMPL_N(iter_next)(FQUEUE_IMPL_N(iter)* it,
                                       FQUEUE_T* value);

// Release the iterator, at any point of the iteration.
FQUEUE_API void FQUEUE_IMPL_N(iter_destroy)(FQUEUE_IMPL_N(iter)* it);

#ifdef __cplusplus
}
#endif

#else // FQUEUE_IMPLEMENTATION

#include <assert.h>
#include <stdlib.h>

// Allocator and limits, shared by all instantiations in this translation unit.
#ifndef FQUEUE_IMPL_CONFIGURED
#define FQUEUE_IMPL_CONFIGURED
#if defined(FQUEUE_MALLOC) != defined(FQUEUE_FREE)
#error "define both FQUEUE_MALLOC and FQUEUE_FREE, or neither"
#endif
#ifdef FQUEUE_MALLOC
void *FQUEUE_MALLOC(size_t size);
void  FQUEUE_FREE(void *ptr);
#else
#define FQUEUE_MALLOC malloc
#define FQUEUE_FREE   free
#endif

// Limits; the tests lower them to reach the error paths. A queue holds at most
// LEN_LIMIT elements. Between operations every node has at most REFS_LIMIT
// references: every reference an operation adds goes through retain, which
// reports a count above the limit, and such an operation is undone. Within
// one operation a count grows by at most 10 (6 in copy_into, 1 in
// start_rotation, 3 in advance), so the headroom above REFS_LIMIT keeps it
// from wrapping before the operation is undone.
#ifndef FQUEUE_IMPL_LEN_LIMIT
#define FQUEUE_IMPL_LEN_LIMIT UINT32_MAX
#endif
#ifndef FQUEUE_IMPL_REFS_LIMIT
#define FQUEUE_IMPL_REFS_LIMIT (UINT32_MAX - 16)
#endif
#endif // FQUEUE_IMPL_CONFIGURED

struct FQUEUE_IMPL_NODE {
  FQUEUE_T                 value;
  uint32_t                 refcount;
  struct FQUEUE_IMPL_NODE *next;
};

// Increment refcount; NULL-safe. Returns true iff the count is now above
// REFS_LIMIT; callers accumulate that with `|=` and undo the operation (the
// assert verifies the headroom in tests).
static bool FQUEUE_IMPL_P(retain)(FQUEUE_IMPL_NODE *n) {
  if (!n) return false;
  assert(n->refcount < FQUEUE_IMPL_REFS_LIMIT + 16);
  n->refcount++;
  return n->refcount > FQUEUE_IMPL_REFS_LIMIT;
}

// Decrement refcount iteratively; free chain when count reaches 0.
// Before free(n) the next node's refcount is loaded (and discarded) so that
// its cache miss overlaps with free() instead of stalling the next iteration.
// Measured across 4 code layouts: -3..5% cycles, and no slow allocator mode in
// mixed/fill 10^7; __builtin_prefetch in the same place gave no effect.
static void FQUEUE_IMPL_P(release)(FQUEUE_IMPL_NODE *n) {
  while (n && --n->refcount == 0) {
    FQUEUE_IMPL_NODE *next = n->next;
    if (next) (void)*(volatile uint32_t *)&next->refcount;
    FQUEUE_FREE(n);
    n = next;
  }
}

// Allocate a new node (refcount=1) on top of `next`. Takes over the caller's
// reference to `next` instead of adding one, like Stack::cons in Rust.
// Returns NULL, leaving the reference with the caller, if allocation fails.
static FQUEUE_IMPL_NODE *FQUEUE_IMPL_P(cons)(FQUEUE_T value,
                                             FQUEUE_IMPL_NODE *next) {
  FQUEUE_IMPL_NODE *n = (FQUEUE_IMPL_NODE *)FQUEUE_MALLOC(sizeof *n);
  if (!n) return NULL;
  n->value    = value;
  n->refcount = 1;
  n->next     = next;
  return n;
}

// Retain all node pointers held by a rotation, based on its current tag.
// Returns true iff a count went above REFS_LIMIT.
static bool FQUEUE_IMPL_P(rotation_retain)(const FQUEUE_IMPL_P(rotation) *r) {
  bool overflow = false; // accumulated without branches: all are retained
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE: break;
  case FQUEUE_IMPL_REVERSING:
    overflow |= FQUEUE_IMPL_P(retain)(r->reversing.acc);
    overflow |= FQUEUE_IMPL_P(retain)(r->reversing.rfacc);
    overflow |= FQUEUE_IMPL_P(retain)(r->reversing.old_front);
    overflow |= FQUEUE_IMPL_P(retain)(r->reversing.old_rear);
    break;
  case FQUEUE_IMPL_APPENDING:
    overflow |= FQUEUE_IMPL_P(retain)(r->appending.acc);
    overflow |= FQUEUE_IMPL_P(retain)(r->appending.rfacc);
    break;
  }
  return overflow;
}

// Release all node pointers held by a rotation and reset it to IDLE.
static void FQUEUE_IMPL_P(rotation_release)(FQUEUE_IMPL_P(rotation) *r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE: break;
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
// Returns true iff a count went above REFS_LIMIT.
static bool FQUEUE_IMPL_P(copy_into)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT dst,
                                     const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT src) {
  *dst = *src;
  bool overflow = FQUEUE_IMPL_P(retain)(dst->front);
  overflow |= FQUEUE_IMPL_P(retain)(dst->rear);
  overflow |= FQUEUE_IMPL_P(rotation_retain)(&dst->rot);
  return overflow;
}

// Replace the reference `n` with a reference to its successor, like
// Stack::advance in Rust. A node owned only by us is freed and hands its
// reference to the successor over without touching the count (the successor's
// count is loaded early, as in release); a shared node stays with the other
// versions, and we take a new reference to its successor (setting *overflow if
// its count went above REFS_LIMIT).
static FQUEUE_IMPL_NODE *FQUEUE_IMPL_P(advance)(FQUEUE_IMPL_NODE *n,
                                                bool *overflow) {
  if (!n) return NULL;
  FQUEUE_IMPL_NODE *next = n->next;
  if (n->refcount == 1) {
    if (next) (void)*(volatile uint32_t *)&next->refcount;
    FQUEUE_FREE(n);
    return next;
  }
  n->refcount--;
  *overflow |= FQUEUE_IMPL_P(retain)(next);
  return next;
}

// *dst = cons(value, *dst): the reference *dst held moves into the new node.
// Returns false, leaving *dst as it was, if allocation fails.
static bool FQUEUE_IMPL_P(cons_prepend)(FQUEUE_IMPL_NODE **dst,
                                        FQUEUE_T value) {
  FQUEUE_IMPL_NODE *n = FQUEUE_IMPL_P(cons)(value, *dst);
  if (!n) return false;
  *dst = n;
  return true;
}

// Returns true iff a count went above REFS_LIMIT.
static bool FQUEUE_IMPL_P(start_rotation)(FQUEUE_NAME *q) {
  q->rot.tag                 = FQUEUE_IMPL_REVERSING;
  // old_front shares front's node list — give old_front its own reference.
  q->rot.reversing.old_front = q->front;
  // old_rear takes ownership of rear; rear pointer is cleared.
  q->rot.reversing.old_rear  = q->rear; // transfer: rear's ref moves to old_rear
  q->rear                    = NULL;
  q->rear_len                = 0;
  q->rot.reversing.acc       = NULL;
  q->rot.reversing.rfacc     = NULL;
  q->rot.reversing.ok        = q->front_len;
  return FQUEUE_IMPL_P(retain)(q->front);
}

// Install acc as the new front and reset rotation state.
static void FQUEUE_IMPL_P(finish_rotation)(FQUEUE_NAME *q) {
  FQUEUE_IMPL_P(release)(q->front);
  q->front     = q->rot.appending.acc;            // acc ownership moves to front
  FQUEUE_IMPL_P(release)(q->rot.appending.rfacc); // leftover rfacc (ok == 0)
  q->front_len = q->size - q->rear_len;
  q->rot.tag   = FQUEUE_IMPL_IDLE;
}

// One step of the ongoing rotation. Modifies q in place (q is already a fresh
// copy). Returns false if allocation fails; q may then be half-stepped, but
// every field still owns its reference, so NAME_destroy undoes it. Sets
// *overflow if a count went above REFS_LIMIT.
static bool FQUEUE_IMPL_P(rotate_step)(FQUEUE_NAME *q, bool *overflow) {
  if (q->rot.tag == FQUEUE_IMPL_IDLE) return true;

  if (q->rot.tag == FQUEUE_IMPL_REVERSING) {
    if (!FQUEUE_IMPL_P(cons_prepend)(&q->rot.reversing.acc,
                                     q->rot.reversing.old_rear->value))
      return false;
    q->rot.reversing.old_rear =
        FQUEUE_IMPL_P(advance)(q->rot.reversing.old_rear, overflow);

    if (q->rot.reversing.old_front != NULL) {
      if (!FQUEUE_IMPL_P(cons_prepend)(&q->rot.reversing.rfacc,
                                       q->rot.reversing.old_front->value))
        return false;
      q->rot.reversing.old_front =
          FQUEUE_IMPL_P(advance)(q->rot.reversing.old_front, overflow);
    }

    if (q->rot.reversing.old_rear != NULL) return true;

    // Transition to APPENDING: extract fields before overwriting the union.
    FQUEUE_IMPL_NODE *acc   = q->rot.reversing.acc;
    FQUEUE_IMPL_NODE *rfacc = q->rot.reversing.rfacc;
    fqueue_impl_len   ok    = q->rot.reversing.ok;
    q->rot.tag             = FQUEUE_IMPL_APPENDING;
    q->rot.appending.acc   = acc;
    q->rot.appending.rfacc = rfacc;
    q->rot.appending.ok    = ok;
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
                                   q->rot.appending.rfacc->value))
    return false;
  q->rot.appending.ok--;
  q->rot.appending.rfacc =
      FQUEUE_IMPL_P(advance)(q->rot.appending.rfacc, overflow);
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0)
    FQUEUE_IMPL_P(finish_rotation)(q);
  return true;
}

// Error exit of an operation: undo the partially built *out.
static fqueue_status FQUEUE_IMPL_P(fail)(FQUEUE_NAME *out,
                                         fqueue_status status) {
  FQUEUE_IMPL_N(destroy)(out);
  return status;
}

void FQUEUE_IMPL_N(init)(FQUEUE_NAME *q) {
  q->size      = 0;
  q->front_len = 0;
  q->rear_len  = 0;
  q->front     = NULL;
  q->rear      = NULL;
  q->rot       = (FQUEUE_IMPL_P(rotation)){ .tag = FQUEUE_IMPL_IDLE };
}

fqueue_status FQUEUE_IMPL_N(clone)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                                   const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q) {
  assert(out != q);
  if (FQUEUE_IMPL_P(copy_into)(out, q))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_OVERFLOW);
  return FQUEUE_OK;
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
  bool overflow = FQUEUE_IMPL_P(copy_into)(out, q);
  // The copy's reference to rear moves into the new node.
  if (!FQUEUE_IMPL_P(cons_prepend)(&out->rear, value))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  out->rear_len++;
  out->size++;
  if (out->rot.tag == FQUEUE_IMPL_IDLE && out->rear_len > out->front_len)
    overflow |= FQUEUE_IMPL_P(start_rotation)(out);
  // One step suffices: docs/proof.md.
  if (!FQUEUE_IMPL_P(rotate_step)(out, &overflow))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  if (overflow) return FQUEUE_IMPL_P(fail)(out, FQUEUE_OVERFLOW);
  return FQUEUE_OK;
}

fqueue_status FQUEUE_IMPL_N(pop)(FQUEUE_NAME *FQUEUE_IMPL_RESTRICT out,
                                 const FQUEUE_NAME *FQUEUE_IMPL_RESTRICT q) {
  assert(out != q);
  if (q->size == 0) return FQUEUE_EMPTY;
  bool overflow = FQUEUE_IMPL_P(copy_into)(out, q);
  // The head is shared with q.
  out->front = FQUEUE_IMPL_P(advance)(out->front, &overflow);
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
    overflow |= FQUEUE_IMPL_P(start_rotation)(out);
  }
  // One step suffices: docs/proof.md.
  if (!FQUEUE_IMPL_P(rotate_step)(out, &overflow))
    return FQUEUE_IMPL_P(fail)(out, FQUEUE_NOMEM);
  if (overflow) return FQUEUE_IMPL_P(fail)(out, FQUEUE_OVERFLOW);
  return FQUEUE_OK;
}

bool FQUEUE_IMPL_N(front)(const FQUEUE_NAME *q, FQUEUE_T *value) {
  if (!q->front) return false;
  *value = q->front->value;
  return true;
}

fqueue_status FQUEUE_IMPL_N(iter_init)(FQUEUE_IMPL_N(iter) *it,
                                       const FQUEUE_NAME *q) {
  return FQUEUE_IMPL_N(clone)(&it->rest, q);
}

fqueue_status FQUEUE_IMPL_N(iter_next)(FQUEUE_IMPL_N(iter) *it,
                                       FQUEUE_T *value) {
  FQUEUE_T    front;
  FQUEUE_NAME next;
  if (!FQUEUE_IMPL_N(front)(&it->rest, &front)) return FQUEUE_EMPTY;
  fqueue_status s = FQUEUE_IMPL_N(pop)(&next, &it->rest);
  if (s != FQUEUE_OK) return s;
  FQUEUE_IMPL_N(destroy)(&it->rest);
  it->rest = next;
  *value   = front;
  return FQUEUE_OK;
}

void FQUEUE_IMPL_N(iter_destroy)(FQUEUE_IMPL_N(iter) *it) {
  FQUEUE_IMPL_N(destroy)(&it->rest);
}

#endif // FQUEUE_IMPLEMENTATION

#undef FQUEUE_IMPL_CAT2
#undef FQUEUE_IMPL_CAT
#undef FQUEUE_IMPL_N
#undef FQUEUE_IMPL_P
#undef FQUEUE_IMPL_NODE
#undef FQUEUE_NAME
#undef FQUEUE_T
#undef FQUEUE_IMPLEMENTATION
#undef FQUEUE_API
