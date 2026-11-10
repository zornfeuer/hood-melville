#ifndef HOOD_MELVILLE_FQUEUE_H
#define HOOD_MELVILLE_FQUEUE_H

// Functional (persistent) real-time queue of Hood & Melville on six stacks:
// push, pop and front in worst-case O(1); every operation writes a new
// version and leaves its argument unchanged.
//
// Versions are values: an `fqueue` lives in storage owned by the caller (a
// local variable, an array element, a field, ...), and creating a version
// allocates only queue nodes, never the version itself.
//
// Ownership. An initialized `fqueue` owns references to its nodes and must be
// destroyed exactly once with fqueue_destroy. It may be moved by a bitwise
// copy, after which the source must not be used until it is initialized
// again; a bitwise copy is not a second owner — use fqueue_clone for that.
// For the operations below `out` is uninitialized storage (or a destroyed
// version) and must not alias `q`.
//
// Errors. Operations that create a version return an fqueue_status. On any
// status other than FQUEUE_OK, `q` is unchanged, *out is left uninitialized
// (nothing to destroy) and no memory is leaked. The library never aborts.
//
// Build configuration (when compiling fqueue.c): define FQUEUE_MALLOC and
// FQUEUE_FREE together to replace malloc and free; they must have the same
// signatures.
//
// Threads. Versions that share nodes must not be used concurrently, even for
// reading: reference counts are not atomic. Versions with no common history
// are independent; the library has no global state.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define FQUEUE_IMPL_RESTRICT __restrict
extern "C" {
#else
#define FQUEUE_IMPL_RESTRICT restrict
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FQUEUE_IMPL_NODISCARD __attribute__((warn_unused_result))
#else
#define FQUEUE_IMPL_NODISCARD
#endif

typedef enum fqueue_status {
  FQUEUE_OK = 0,
  FQUEUE_EMPTY,    // fqueue_pop on an empty queue
  FQUEUE_NOMEM,    // a node could not be allocated
  FQUEUE_OVERFLOW, // the queue would exceed UINT32_MAX elements, or a node
                   // would get more references than its counter holds
                   // (reachable only by leaking versions)
} fqueue_status;

// Human-readable description of a status, e.g. for error messages.
const char* fqueue_strerror(fqueue_status status);

typedef struct fqueue fqueue;

// Representation. Exposed only so that versions can be stored by value;
// everything named fqueue_impl_* / FQUEUE_IMPL_* and all fields are private —
// do not access them.
typedef struct fqueue_impl_node fqueue_impl_node;

// Lengths and the rotation counter `ok`; the same type as `Len` in Rust.
typedef uint32_t fqueue_impl_len;

typedef enum {
  FQUEUE_IMPL_IDLE,
  FQUEUE_IMPL_REVERSING,
  FQUEUE_IMPL_APPENDING
} fqueue_impl_phase;

typedef struct {
  fqueue_impl_node *acc;
  fqueue_impl_node *rfacc;
  fqueue_impl_node *old_front;
  fqueue_impl_node *old_rear;
  fqueue_impl_len   ok;
} fqueue_impl_reversing;

typedef struct {
  fqueue_impl_node *acc;
  fqueue_impl_node *rfacc;
  fqueue_impl_len   ok;
} fqueue_impl_appending;

typedef struct {
  fqueue_impl_phase tag;
  union {
    fqueue_impl_reversing reversing;
    fqueue_impl_appending appending;
  };
} fqueue_impl_rotation;

struct fqueue {
  fqueue_impl_len      size;
  fqueue_impl_len      front_len;
  fqueue_impl_len      rear_len;
  fqueue_impl_node    *front;
  fqueue_impl_node    *rear;
  fqueue_impl_rotation rot;
};

// Complexity: everything except fqueue_destroy is worst-case O(1) (at most 3
// new nodes per push and 2 per pop). fqueue_destroy is not: it frees every
// node the version owned exclusively, which can be Theta(n); in total freeing
// costs no more than node creation, i.e. amortized O(1) per operation. So
// `fqueue_push(&b, &a, x); fqueue_destroy(&a);` is O(1) for the push but not
// necessarily for the destroy. Proof: docs/proof.md.

// Initialize *q as an empty queue. Allocates nothing.
void fqueue_init(fqueue* q);

// Initialize *out as another owner of the version `q`: the same contents,
// all nodes shared, nothing allocated. Fails only with FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD
fqueue_status fqueue_clone(fqueue* FQUEUE_IMPL_RESTRICT out,
                           const fqueue* FQUEUE_IMPL_RESTRICT q);

// Release the nodes owned by *q (shared nodes are freed only when no other
// version references them). Afterwards *q is uninitialized storage, as after
// a move: it may be passed as `out` or to fqueue_init, nothing else. The
// storage of *q itself is not freed. Unlike free(), destroying twice is an
// error — like Rust's drop, and resetting *q measurably slows linear
// histories (docs/benchmarks.md).
void fqueue_destroy(fqueue* q);

// Return true iff the queue contains no elements.
bool fqueue_is_empty(const fqueue* q);

// Number of elements in the queue.
size_t fqueue_size(const fqueue* q);

// Front element: write it to *value and return true; if `q` is empty, return
// false and leave *value untouched.
bool fqueue_front(const fqueue* q, int* value);

// Enqueue: initialize *out as `q` with `value` appended to the rear.
// Fails with FQUEUE_NOMEM or FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD
fqueue_status fqueue_push(fqueue* FQUEUE_IMPL_RESTRICT out,
                          const fqueue* FQUEUE_IMPL_RESTRICT q, int value);

// Dequeue: initialize *out as `q` without its front element. Fails with
// FQUEUE_EMPTY (*out is then not even written), FQUEUE_NOMEM or
// FQUEUE_OVERFLOW.
FQUEUE_IMPL_NODISCARD
fqueue_status fqueue_pop(fqueue* FQUEUE_IMPL_RESTRICT out,
                         const fqueue* FQUEUE_IMPL_RESTRICT q);

#ifdef __cplusplus
}
#endif

#endif // HOOD_MELVILLE_FQUEUE_H
