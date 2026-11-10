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
  FQUEUE_OVERFLOW, // the queue would exceed UINT32_MAX elements, or a node
                   // would get more references than its counter holds
                   // (reachable only by leaking versions)
} fqueue_status;

// Human-readable description of a status, e.g. for error messages.
static inline const char* fqueue_strerror(fqueue_status status) {
  switch (status) {
  case FQUEUE_OK:       return "success";
  case FQUEUE_EMPTY:    return "queue is empty";
  case FQUEUE_NOMEM:    return "out of memory";
  case FQUEUE_OVERFLOW: return "too many elements or references";
  }
  return "unknown fqueue status";
}

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
