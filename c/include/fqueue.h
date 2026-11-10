#ifndef HOOD_MELVILLE_FQUEUE_H
#define HOOD_MELVILLE_FQUEUE_H

// Functional (persistent) real-time queue of Hood & Melville on six stacks:
// push, pop and front in worst-case O(1); every operation returns a new
// version and leaves its argument unchanged.

#include <stdbool.h>

#include "option.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fqueue fqueue;

// Representation. Exposed only so that versions can be stored by value
// (see the value API below); everything named fqueue_impl_* / FQUEUE_IMPL_*
// and all fields are private — do not access them.
typedef struct fqueue_impl_node fqueue_impl_node;

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
  int               ok;
} fqueue_impl_reversing;

typedef struct {
  fqueue_impl_node *acc;
  fqueue_impl_node *rfacc;
  int               ok;
} fqueue_impl_appending;

typedef struct {
  fqueue_impl_phase tag;
  union {
    fqueue_impl_reversing reversing;
    fqueue_impl_appending appending;
  };
} fqueue_impl_rotation;

struct fqueue {
  int                  size;
  int                  front_len;
  int                  rear_len;
  fqueue_impl_node    *front;
  fqueue_impl_node    *rear;
  fqueue_impl_rotation rot;
};

// Create an empty queue.
fqueue* fqueue_create(void);

// Return true iff the queue contains no elements.
bool fqueue_is_empty(const fqueue* q);

// Number of elements in the queue.
int fqueue_size(const fqueue* q);

// Complexity: fqueue_push, fqueue_pop, fqueue_front, fqueue_size and
// fqueue_is_empty are worst-case O(1) (at most 3 new nodes per push and 2 per
// pop). fqueue_free is not: it frees every node the version owned
// exclusively, which can be Theta(n); in total freeing costs no more than
// node creation, i.e. amortized O(1) per operation. So
// `q2 = fqueue_push(q, x); fqueue_free(q);` is O(1) for the push but not
// necessarily for the free. Proof: docs/proof.md.

// Enqueue: return a new queue with `value` appended to the rear of `q`.
// `q` itself is not modified.
fqueue* fqueue_push(const fqueue* q, int value);

// Dequeue: return a new queue without the front element of `q`.
// `q` itself is not modified. Return NULL on an empty queue.
fqueue* fqueue_pop(const fqueue* q);

// Return the value at the front of `q`, or none() on an empty queue.
option fqueue_front(const fqueue* q);

// Free the queue and release all nodes it owns. Shared nodes are freed only
// when their reference count drops to zero (i.e., when no other queue version
// holds a reference to them).
void fqueue_free(fqueue* q);

// ---------------------------------------------------------------------------
// Value API: the same persistent operations, but versions live in storage
// owned by the caller (a local variable, an array element, ...), so creating a
// version allocates only queue nodes, never the version itself. The pointer
// API above is a thin wrapper over these functions.
//
// Ownership: an initialized fqueue value owns references to its nodes. It may
// be moved (copied bitwise, after which the source must not be used or
// released), but not duplicated — releasing two bitwise copies frees shared
// nodes twice. `out` must not alias `q`. Same complexity as the pointer API.
// ---------------------------------------------------------------------------

// Initialize *q as an empty queue.
void fqueue_init(fqueue* q);

// Write into *out a new version: `q` with `value` appended. `q` is unchanged.
void fqueue_push_into(fqueue* out, const fqueue* q, int value);

// Write into *out a new version: `q` without its front element; `q` is
// unchanged. Return false (and leave *out untouched) if `q` is empty.
bool fqueue_pop_into(fqueue* out, const fqueue* q);

// Release the nodes owned by *q; the storage of *q itself is not freed.
void fqueue_release(fqueue* q);

#ifdef __cplusplus
}
#endif

#endif // HOOD_MELVILLE_FQUEUE_H
