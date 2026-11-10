// Shared helpers for the fqueue tests.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fqueue.h"

// An unexpected fqueue_status, reported by Catch with its description.
struct fqueue_error : std::runtime_error {
  fqueue_status status;
  explicit fqueue_error(fqueue_status s)
      : std::runtime_error(fqueue_strerror(s)), status(s) {}
};

// RAII owner of one version, so that tests read like the Rust ones and cannot
// leak: copying is fqueue_clone, destruction is fqueue_destroy, moving is a
// bitwise copy followed by re-initializing the source. Any failure throws
// fqueue_error.
class Q {
public:
  Q() { fqueue_init(&q_); }
  Q(const Q &other) {
    fqueue_status s = fqueue_clone(&q_, &other.q_);
    if (s != FQUEUE_OK) throw fqueue_error(s); // q_ is not initialized
  }
  Q(Q &&other) noexcept : q_(other.q_) { fqueue_init(&other.q_); }
  Q &operator=(Q other) noexcept {
    std::swap(q_, other.q_);
    return *this;
  }
  ~Q() { fqueue_destroy(&q_); }

  Q push(int value) const {
    Q r{uninit};
    r.check(fqueue_push(&r.q_, &q_, value));
    return r;
  }

  // nullopt on an empty queue.
  std::optional<Q> pop() const {
    Q r{uninit};
    fqueue_status s = fqueue_pop(&r.q_, &q_);
    if (s == FQUEUE_EMPTY) {
      fqueue_init(&r.q_);
      return std::nullopt;
    }
    r.check(s);
    return r;
  }

  std::optional<int> front() const {
    int v;
    if (!fqueue_front(&q_, &v)) return std::nullopt;
    return v;
  }

  size_t size() const { return fqueue_size(&q_); }
  bool   empty() const { return fqueue_is_empty(&q_); }

  const fqueue *get() const { return &q_; }

private:
  struct Uninit {};
  static constexpr Uninit uninit{};
  explicit Q(Uninit) {} // storage for an fqueue_* output parameter

  // A failed operation leaves q_ uninitialized: make it a valid empty queue
  // for the destructor, then report.
  void check(fqueue_status s) {
    if (s == FQUEUE_OK) return;
    fqueue_init(&q_);
    throw fqueue_error(s);
  }

  fqueue q_;
};

// Value at the front of a queue known to be non-empty.
inline int front_of(const fqueue *q) {
  int v;
  REQUIRE(fqueue_front(q, &v));
  return v;
}

// Contents of q front to back; q itself is left untouched. Walks a clone,
// moving each new version into `cur` bitwise.
inline std::vector<int> contents(const fqueue *q) {
  std::vector<int> out;
  fqueue cur, next;
  REQUIRE(fqueue_clone(&cur, q) == FQUEUE_OK);
  while (!fqueue_is_empty(&cur)) {
    out.push_back(front_of(&cur));
    REQUIRE(fqueue_pop(&next, &cur) == FQUEUE_OK);
    fqueue_destroy(&cur);
    cur = next;
  }
  fqueue_destroy(&cur);
  return out;
}

inline std::vector<int> contents(const Q &q) { return contents(q.get()); }
