#ifndef HOOD_MELVILLE_FQUEUE_COMMON_H
#define HOOD_MELVILLE_FQUEUE_COMMON_H

// Parts of the fqueue library that do not depend on the element type; see
// fqueue_template.h for the queue itself.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define FQUEUE_IMPL_RESTRICT __restrict
#else
#define FQUEUE_IMPL_RESTRICT restrict
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FQUEUE_IMPL_NODISCARD __attribute__((warn_unused_result))
#else
#define FQUEUE_IMPL_NODISCARD
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fqueue_status {
  FQUEUE_OK = 0,
  FQUEUE_EMPTY,    // pop on an empty queue, or an exhausted iterator
  FQUEUE_NOMEM,    // a node could not be allocated
  FQUEUE_OVERFLOW, // the queue would exceed UINT32_MAX elements
} fqueue_status;

// Human-readable description of a status, e.g. for error messages.
static inline const char *fqueue_strerror(fqueue_status status) {
  switch (status) {
  case FQUEUE_OK:
    return "success";
  case FQUEUE_EMPTY:
    return "queue is empty";
  case FQUEUE_NOMEM:
    return "out of memory";
  case FQUEUE_OVERFLOW:
    return "too many elements";
  }
  return "unknown fqueue status";
}

// Statistics of a node pool (FQUEUE_NODE_POOL instantiations, NAME_pool_stats;
// docs/pool.md). Approximate while other threads use the pool.
typedef struct fqueue_pool_stats {
  size_t slabs;         // slabs held by the pool
  size_t slab_bytes;    // their total size
  size_t capacity;      // nodes in them
  size_t free;          // of these, free: in thread caches, uncarved, global
  size_t live;          // of these, in use: capacity - free
  uint64_t slab_allocs; // slabs ever allocated (calls to the system allocator)
  uint64_t slab_frees;  // slabs ever returned (by NAME_pool_trim)
  uint64_t spills;      // batches given by thread caches to the global stack
  uint64_t refills;     // batches taken from it
} fqueue_pool_stats;

// Private: lengths and the rotation counter `ok` (the same type as `Len` in
// Rust), and the phase of a rotation.
typedef uint32_t fqueue_impl_len;

typedef enum {
  FQUEUE_IMPL_IDLE,
  FQUEUE_IMPL_REVERSING,
  FQUEUE_IMPL_APPENDING
} fqueue_impl_phase;

#ifdef __cplusplus
}
#endif

#endif // HOOD_MELVILLE_FQUEUE_COMMON_H
