#include "fqueue.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

struct fqueue_node {
  int                 value;
  int                 refcount;
  struct fqueue_node *next;
};

typedef struct fqueue_node node;
typedef fqueue_rotation    rotation;

// Increment refcount; NULL-safe. Returns n for chaining.
static node *node_retain(node *n) {
  if (n)
    n->refcount++;
  return n;
}

// Decrement refcount iteratively; free chain when count reaches 0.
// Before free(n) the next node's refcount is loaded (and discarded) so that
// its cache miss overlaps with free() instead of stalling the next iteration.
// Measured across 4 code layouts: -3..5% cycles, and no slow allocator mode in
// mixed/fill 10^7; __builtin_prefetch in the same place gave no effect.
static void node_release(node *n) {
  while (n && --n->refcount == 0) {
    node *next = n->next;
    if (next) (void)*(volatile int *)&next->refcount;
    free(n);
    n = next;
  }
}

// Allocate a new node (refcount=1) pointing to an existing shared node.
static node *cons(int value, node *next) {
  node *n = malloc(sizeof(node));
  if (!n) abort();
  n->value    = value;
  n->refcount = 1;
  n->next     = node_retain(next);
  return n;
}

// Retain all node pointers held by a rotation, based on its current tag.
static void rotation_retain(const rotation *r) {
  switch (r->tag) {
  case FQ_IDLE: break;
  case FQ_REVERSING:
    node_retain(r->reversing.acc);
    node_retain(r->reversing.rfacc);
    node_retain(r->reversing.old_front);
    node_retain(r->reversing.old_rear);
    break;
  case FQ_APPENDING:
    node_retain(r->appending.acc);
    node_retain(r->appending.rfacc);
    break;
  }
}

// Release all node pointers held by a rotation and reset it to IDLE.
static void rotation_release(rotation *r) {
  switch (r->tag) {
  case FQ_IDLE: break;
  case FQ_REVERSING:
    node_release(r->reversing.acc);
    node_release(r->reversing.rfacc);
    node_release(r->reversing.old_front);
    node_release(r->reversing.old_rear);
    break;
  case FQ_APPENDING:
    node_release(r->appending.acc);
    node_release(r->appending.rfacc);
    break;
  }
  r->tag = FQ_IDLE;
}

// Pool of freed fqueue structs, so that a linear history
// (q2 = fqueue_push(q, x); fqueue_free(q); ...) never reaches malloc: each
// operation reuses the struct released by the previous fqueue_free.
// The pool is capped so that freeing many versions at once (all versions of
// a persistent history) returns memory to the allocator instead of hoarding it.
// Single-threaded, like the rest of the implementation.
typedef union fqueue_slot {
  fqueue             q;
  union fqueue_slot *next; // valid only while the slot is in the pool
} fqueue_slot;

enum { FQUEUE_POOL_CAP = 64 };

static fqueue_slot *fqueue_pool     = NULL;
static int          fqueue_pool_len = 0;

static fqueue *fqueue_alloc(void) {
  fqueue_slot *s = fqueue_pool;
  if (s) {
    fqueue_pool = s->next;
    fqueue_pool_len--;
  } else {
    s = malloc(sizeof(fqueue_slot));
    if (!s) abort();
  }
  return &s->q;
}

// A pointer to a union member converts back to a pointer to the union.
static void fqueue_dealloc(fqueue *q) {
  fqueue_slot *s = (fqueue_slot *)q;
  if (fqueue_pool_len == FQUEUE_POOL_CAP) {
    free(s);
    return;
  }
  s->next     = fqueue_pool;
  fqueue_pool = s;
  fqueue_pool_len++;
}

// Make *dst a copy of *src that holds its own references to all shared nodes.
static void fqueue_copy_into(fqueue *dst, const fqueue *src) {
  *dst = *src;
  node_retain(dst->front);
  node_retain(dst->rear);
  rotation_retain(&dst->rot);
}

// Advance a node pointer to its successor with correct ownership transfer.
static node *node_advance(node *n) {
  node *next = node_retain(n ? n->next : NULL);
  node_release(n);
  return next;
}

// Set *dst to a new cons node built from (value, *dst), releasing the old *dst.
// Pattern: acc = cons(value, acc) with ref-counting.
static void cons_prepend(node **dst, int value) {
  node *old = *dst;
  *dst = cons(value, old); // cons retains old internally
  node_release(old);       // release the ref that *dst used to hold
}

static void start_rotation(fqueue *q) {
  q->rot.tag                 = FQ_REVERSING;
  // old_front shares front's node list — give old_front its own reference.
  q->rot.reversing.old_front = node_retain(q->front);
  // old_rear takes ownership of rear; rear pointer is cleared.
  q->rot.reversing.old_rear  = q->rear; // transfer: rear's ref moves to old_rear
  q->rear                    = NULL;
  q->rear_len                = 0;
  q->rot.reversing.acc       = NULL;
  q->rot.reversing.rfacc     = NULL;
  q->rot.reversing.ok        = q->front_len;
}

// Install acc as the new front and reset rotation state.
static void finish_rotation(fqueue *q) {
  node_release(q->front);
  q->front     = q->rot.appending.acc;   // acc ownership transferred to front
  node_release(q->rot.appending.rfacc);  // abandon leftover rfacc (ok==0 case)
  q->front_len = q->size - q->rear_len;
  q->rot.tag   = FQ_IDLE;
}

// One step of the ongoing rotation. Modifies q in place (q is already a fresh copy).
static void rotate_step(fqueue *q) {
  if (q->rot.tag == FQ_IDLE) return;

  if (q->rot.tag == FQ_REVERSING) {
    cons_prepend(&q->rot.reversing.acc,   q->rot.reversing.old_rear->value);
    q->rot.reversing.old_rear = node_advance(q->rot.reversing.old_rear);

    if (q->rot.reversing.old_front != NULL) {
      cons_prepend(&q->rot.reversing.rfacc, q->rot.reversing.old_front->value);
      q->rot.reversing.old_front = node_advance(q->rot.reversing.old_front);
    }

    if (q->rot.reversing.old_rear != NULL) return;

    // Transition to APPENDING: extract fields before overwriting the union.
    node *acc   = q->rot.reversing.acc;
    node *rfacc = q->rot.reversing.rfacc;
    int   ok    = q->rot.reversing.ok;
    q->rot.tag             = FQ_APPENDING;
    q->rot.appending.acc   = acc;
    q->rot.appending.rfacc = rfacc;
    q->rot.appending.ok    = ok;
  }

  // FQ_APPENDING
  // Complete immediately:
  //   rfacc == NULL → naturally exhausted all old_front elements
  //   ok == 0       → all remaining steps are no-ops (elements already popped)
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0) {
    finish_rotation(q);
    return;
  }
  cons_prepend(&q->rot.appending.acc, q->rot.appending.rfacc->value);
  q->rot.appending.ok--;
  q->rot.appending.rfacc = node_advance(q->rot.appending.rfacc);
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0)
    finish_rotation(q);
}

void fqueue_init(fqueue *q) {
  q->size      = 0;
  q->front_len = 0;
  q->rear_len  = 0;
  q->front     = NULL;
  q->rear      = NULL;
  q->rot       = (rotation){ .tag = FQ_IDLE };
}

bool fqueue_is_empty(const fqueue *q) { return q->size == 0; }

int fqueue_size(const fqueue *q) { return q->size; }

void fqueue_push_into(fqueue *out, const fqueue *q, int value) {
  assert(out != q);
  fqueue_copy_into(out, q);
  // Replace out->rear (retained by the copy) with cons(value, old_rear).
  node *old_rear = out->rear;
  out->rear = cons(value, old_rear); // cons retains old_rear
  node_release(old_rear);            // release the copy's retain
  out->rear_len++;
  out->size++;
  if (out->rot.tag == FQ_IDLE && out->rear_len > out->front_len)
    start_rotation(out);
  rotate_step(out); // one step suffices: docs/proof.md
}

bool fqueue_pop_into(fqueue *out, const fqueue *q) {
  assert(out != q);
  if (q->size == 0) return false;
  fqueue_copy_into(out, q);
  // Advance out->front (retained by the copy) to its successor.
  node *old_head = out->front;
  out->front = node_retain(old_head->next); // take ownership of next
  node_release(old_head);                   // release the copy's retain of head
  out->front_len--;
  out->size--;
  // During a rotation ok >= 1 here (lemma 2 in docs/proof.md).
  if (out->rot.tag == FQ_REVERSING) {
    assert(out->rot.reversing.ok > 0);
    out->rot.reversing.ok--;
  } else if (out->rot.tag == FQ_APPENDING) {
    assert(out->rot.appending.ok > 0);
    out->rot.appending.ok--;
  } else if (out->rear_len > out->front_len) {
    start_rotation(out);
  }
  rotate_step(out); // one step suffices: docs/proof.md
  return true;
}

void fqueue_release(fqueue *q) {
  node_release(q->front);
  node_release(q->rear);
  rotation_release(&q->rot);
}

option fqueue_front(const fqueue *q) {
  if (!q->front) return none();
  return some(q->front->value);
}

// Pointer API: each version in its own pooled heap slot.

fqueue *fqueue_create() {
  fqueue *q = fqueue_alloc();
  fqueue_init(q);
  return q;
}

fqueue *fqueue_push(fqueue *q, int value) {
  fqueue *q2 = fqueue_alloc();
  fqueue_push_into(q2, q, value);
  return q2;
}

fqueue *fqueue_pop(fqueue *q) {
  if (q->size == 0) return NULL;
  fqueue *q2 = fqueue_alloc();
  fqueue_pop_into(q2, q);
  return q2;
}

void fqueue_free(fqueue *q) {
  if (!q) return;
  fqueue_release(q);
  fqueue_dealloc(q);
}
