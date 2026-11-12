#ifndef HOOD_MELVILLE_FQUEUE_H
#define HOOD_MELVILLE_FQUEUE_H

// The queue of fqueue_template.h instantiated for int, twice: `fqueue`
// (fqueue_init, fqueue_push, fqueue_pop, ...) for use from one thread at a
// time, and `fqueue_sync` (fqueue_sync_init, fqueue_sync_push, ...) with
// atomic reference counts, whose versions threads may share (FQUEUE_ATOMIC;
// see Threads in fqueue_template.h). The API is the same; the single-threaded
// one is faster. The definitions are in src/fqueue.c.

// The library is built with hidden visibility, so that a shared libfqueue
// exports exactly these functions.
#if defined(__GNUC__) || defined(__clang__)
#define HOOD_MELVILLE_FQUEUE_EXPORT __attribute__((visibility("default")))
#else
#define HOOD_MELVILLE_FQUEUE_EXPORT
#endif

#define FQUEUE_API  HOOD_MELVILLE_FQUEUE_EXPORT
#define FQUEUE_NAME fqueue
#define FQUEUE_T    int
#include "fqueue_template.h"

#define FQUEUE_API  HOOD_MELVILLE_FQUEUE_EXPORT
#define FQUEUE_NAME fqueue_sync
#define FQUEUE_T    int
#include "fqueue_template.h"

// The same two with nodes from a node pool (docs/pool.md).
#define FQUEUE_API  HOOD_MELVILLE_FQUEUE_EXPORT
#define FQUEUE_NAME fqueue_pooled
#define FQUEUE_T    int
#define FQUEUE_NODE_POOL
#include "fqueue_template.h"

#define FQUEUE_API  HOOD_MELVILLE_FQUEUE_EXPORT
#define FQUEUE_NAME fqueue_pooled_sync
#define FQUEUE_T    int
#define FQUEUE_NODE_POOL
#include "fqueue_template.h"

#endif // HOOD_MELVILLE_FQUEUE_H
