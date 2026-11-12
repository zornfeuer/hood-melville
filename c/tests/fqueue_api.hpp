// The instantiations of fqueue.h behind one interface, so that a test can run
// for all of them: Api<fqueue>::push is fqueue_push, Api<fqueue_sync>::push is
// fqueue_sync_push, and so on (fqueue_pooled, fqueue_pooled_sync: node pool).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <vector>

#include "fqueue.h"

template <class Q> struct Api;

#define FQUEUE_TEST_API(Q)                                                     \
  template <> struct Api<Q> {                                                  \
    using iter = Q##_iter;                                                     \
    static constexpr auto init = Q##_init;                                     \
    static constexpr auto clone = Q##_clone;                                   \
    static constexpr auto destroy = Q##_destroy;                               \
    static constexpr auto size = Q##_size;                                     \
    static constexpr auto front = Q##_front;                                   \
    static constexpr auto push = Q##_push;                                     \
    static constexpr auto pop = Q##_pop;                                       \
    static constexpr auto push_in_place = Q##_push_in_place;                   \
    static constexpr auto pop_in_place = Q##_pop_in_place;                     \
    static constexpr auto iter_init = Q##_iter_init;                           \
    static constexpr auto iter_next = Q##_iter_next;                           \
    static constexpr auto iter_destroy = Q##_iter_destroy;                     \
  };

FQUEUE_TEST_API(fqueue)
FQUEUE_TEST_API(fqueue_sync)
FQUEUE_TEST_API(fqueue_pooled)
FQUEUE_TEST_API(fqueue_pooled_sync)

#undef FQUEUE_TEST_API

// Contents of q front to back, through the iterator; q itself is untouched.
template <class Q> std::vector<int> contents_of(const Q *q) {
  using A = Api<Q>;
  std::vector<int> out;
  typename A::iter it;
  A::iter_init(&it, q);
  int v;
  fqueue_status s;
  while ((s = A::iter_next(&it, &v)) == FQUEUE_OK) out.push_back(v);
  A::iter_destroy(&it);
  REQUIRE(s == FQUEUE_EMPTY);
  REQUIRE(out.size() == A::size(q));
  return out;
}

inline std::vector<int> as_vector(const std::deque<int> &model) {
  return {model.begin(), model.end()};
}
