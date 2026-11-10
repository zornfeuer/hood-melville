#include "fqueue.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct fqueue_impl_node {
  int                      value;
  uint32_t                 refcount;
  struct fqueue_impl_node *next;
};

typedef struct fqueue_impl_node node;
typedef fqueue_impl_rotation rotation;

// Increment refcount; NULL-safe. Returns n for chaining. Aborts on overflow
// (more than UINT32_MAX references to one node), like NodePtr::clone in Rust:
// overwriting a version without destroying it leaks its references, so the
// count can overflow without UB in the caller.
static node *node_retain(node *n) {
  if (n) {
    if (n->refcount == UINT32_MAX) abort();
    n->refcount++;
  }
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
    if (next) (void)*(volatile uint32_t *)&next->refcount;
    free(n);
    n = next;
  }
}

// Allocate a new node (refcount=1) on top of `next`. Takes over the caller's
// reference to `next` instead of adding one, like Stack::cons in Rust.
static node *cons(int value, node *next) {
  node *n = malloc(sizeof(node));
  if (!n) abort();
  n->value    = value;
  n->refcount = 1;
  n->next     = next;
  return n;
}

// Retain all node pointers held by a rotation, based on its current tag.
static void rotation_retain(const rotation *r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE: break;
  case FQUEUE_IMPL_REVERSING:
    node_retain(r->reversing.acc);
    node_retain(r->reversing.rfacc);
    node_retain(r->reversing.old_front);
    node_retain(r->reversing.old_rear);
    break;
  case FQUEUE_IMPL_APPENDING:
    node_retain(r->appending.acc);
    node_retain(r->appending.rfacc);
    break;
  }
}

// Release all node pointers held by a rotation and reset it to IDLE.
static void rotation_release(rotation *r) {
  switch (r->tag) {
  case FQUEUE_IMPL_IDLE: break;
  case FQUEUE_IMPL_REVERSING:
    node_release(r->reversing.acc);
    node_release(r->reversing.rfacc);
    node_release(r->reversing.old_front);
    node_release(r->reversing.old_rear);
    break;
  case FQUEUE_IMPL_APPENDING:
    node_release(r->appending.acc);
    node_release(r->appending.rfacc);
    break;
  }
  r->tag = FQUEUE_IMPL_IDLE;
}

// Make *dst a copy of *src that holds its own references to all shared nodes.
static void copy_into(fqueue *restrict dst, const fqueue *restrict src) {
  *dst = *src;
  node_retain(dst->front);
  node_retain(dst->rear);
  rotation_retain(&dst->rot);
}

// Replace the reference `n` with a reference to its successor, like
// Stack::advance in Rust. A node owned only by us is freed and hands its
// reference to the successor over without touching the count (the successor's
// count is loaded early, as in node_release); a shared node stays with the
// other versions, and we take a new reference to its successor.
static node *node_advance(node *n) {
  if (!n) return NULL;
  node *next = n->next;
  if (n->refcount == 1) {
    if (next) (void)*(volatile uint32_t *)&next->refcount;
    free(n);
    return next;
  }
  n->refcount--;
  return node_retain(next);
}

// *dst = cons(value, *dst): the reference *dst held moves into the new node.
static void cons_prepend(node **dst, int value) {
  *dst = cons(value, *dst);
}

static void start_rotation(fqueue *q) {
  q->rot.tag                 = FQUEUE_IMPL_REVERSING;
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
  q->rot.tag   = FQUEUE_IMPL_IDLE;
}

// One step of the ongoing rotation. Modifies q in place (q is already a fresh copy).
static void rotate_step(fqueue *q) {
  if (q->rot.tag == FQUEUE_IMPL_IDLE) return;

  if (q->rot.tag == FQUEUE_IMPL_REVERSING) {
    cons_prepend(&q->rot.reversing.acc,   q->rot.reversing.old_rear->value);
    q->rot.reversing.old_rear = node_advance(q->rot.reversing.old_rear);

    if (q->rot.reversing.old_front != NULL) {
      cons_prepend(&q->rot.reversing.rfacc, q->rot.reversing.old_front->value);
      q->rot.reversing.old_front = node_advance(q->rot.reversing.old_front);
    }

    if (q->rot.reversing.old_rear != NULL) return;

    // Transition to APPENDING: extract fields before overwriting the union.
    node           *acc   = q->rot.reversing.acc;
    node           *rfacc = q->rot.reversing.rfacc;
    fqueue_impl_len ok    = q->rot.reversing.ok;
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
  q->rot       = (rotation){ .tag = FQUEUE_IMPL_IDLE };
}

void fqueue_clone(fqueue *restrict out, const fqueue *restrict q) {
  assert(out != q);
  copy_into(out, q);
}

void fqueue_destroy(fqueue *q) {
  node_release(q->front);
  node_release(q->rear);
  rotation_release(&q->rot);
}

bool fqueue_is_empty(const fqueue *q) { return q->size == 0; }

size_t fqueue_size(const fqueue *q) { return q->size; }

void fqueue_push(fqueue *restrict out, const fqueue *restrict q, int value) {
  assert(out != q);
  if (q->size == FQUEUE_IMPL_LEN_MAX) abort();
  copy_into(out, q);
  cons_prepend(&out->rear, value); // the copy's reference to rear moves in
  out->rear_len++;
  out->size++;
  if (out->rot.tag == FQUEUE_IMPL_IDLE && out->rear_len > out->front_len)
    start_rotation(out);
  rotate_step(out); // one step suffices: docs/proof.md
}

bool fqueue_pop(fqueue *restrict out, const fqueue *restrict q) {
  assert(out != q);
  if (q->size == 0) return false;
  copy_into(out, q);
  out->front = node_advance(out->front); // head is shared with q
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
    start_rotation(out);
  }
  rotate_step(out); // one step suffices: docs/proof.md
  return true;
}

bool fqueue_front(const fqueue *q, int *value) {
  if (!q->front) return false;
  *value = q->front->value;
  return true;
}
