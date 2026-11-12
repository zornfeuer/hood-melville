// fqueue_sync (FQUEUE_ATOMIC): the same semantics as fqueue — checked by
// running the exhaustive model test for every instantiation — plus use from
// several threads, also with the node pool (fqueue_pooled_sync). Run under ThreadSanitizer with `just c-tsan`.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include "fqueue_api.hpp"

// Every push/pop history up to `depth` operations, against std::deque; in
// every state also in-place operations on a clone (which shares every node).
template <class Q>
static void explore(const Q *q, std::deque<int> &model, int depth, long &states) {
  using A = Api<Q>;
  ++states;
  REQUIRE(contents_of(q) == as_vector(model));

  Q c;
  A::clone(&c, q);
  REQUIRE(A::push_in_place(&c, -1) == FQUEUE_OK);
  int v = -2;
  REQUIRE(A::pop_in_place(&c, &v) == FQUEUE_OK);
  REQUIRE(v == (model.empty() ? -1 : model.front()));
  A::destroy(&c);
  if (depth == 0) return;

  Q child;
  REQUIRE(A::push(&child, q, depth) == FQUEUE_OK);
  model.push_back(depth);
  explore(&child, model, depth - 1, states);
  model.pop_back();
  A::destroy(&child);

  fqueue_status s = A::pop(&child, q);
  if (model.empty()) {
    REQUIRE(s == FQUEUE_EMPTY);
    return;
  }
  REQUIRE(s == FQUEUE_OK);
  int x = model.front();
  model.pop_front();
  explore(&child, model, depth - 1, states);
  model.push_front(x);
  A::destroy(&child);
}

TEMPLATE_TEST_CASE("all instantiations match a model on all short histories",
                   "[sync]", fqueue, fqueue_sync, fqueue_pooled, fqueue_pooled_sync) {
  using A = Api<TestType>;
  TestType q;
  A::init(&q);
  std::deque<int> model;
  long states = 0;
  explore(&q, model, 13, states);
  REQUIRE(states > 2000);
  A::destroy(&q);
}

static uint64_t xorshift(uint64_t &x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

TEMPLATE_TEST_CASE("threads derive versions from shared versions", "[sync]",
                   fqueue_sync, fqueue_pooled_sync) {
  using A = Api<TestType>;
  // Shared versions in different rotation states, with their models.
  std::vector<TestType> shared(1);
  A::init(&shared[0]);
  std::vector<std::deque<int>> models{{}};
  for (int i = 0; i < 40; ++i) {
    TestType next;
    std::deque<int> m = models.back();
    if (i % 3 == 2) {
      REQUIRE(A::pop(&next, &shared.back()) == FQUEUE_OK);
      m.pop_front();
    } else {
      REQUIRE(A::push(&next, &shared.back(), i) == FQUEUE_OK);
      m.push_back(i);
    }
    shared.push_back(next);
    models.push_back(m);
  }

  // Catch2 assertions are not thread-safe: each thread records its first
  // mismatch, the main thread checks.
  enum { THREADS = 8, ROUNDS = 2000 };
  std::vector<int> failures(THREADS, 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; ++t) {
    threads.emplace_back([&, t] {
      uint64_t x = 0x9E3779B97F4A7C15u ^ (uint64_t)(t + 1);
      for (int k = 0; k < ROUNDS && !failures[t]; ++k) {
        size_t idx = xorshift(x) % shared.size();
        TestType q;
        A::clone(&q, &shared[idx]);
        std::deque<int> m = models[idx];
        for (int j = 0; j < 4; ++j) {
          int value = 1000000 * (t + 1) + k * 4 + j;
          if (xorshift(x) % 3 == 0) {
            int v = -1;
            fqueue_status s = A::pop_in_place(&q, &v);
            if (m.empty() ? s != FQUEUE_EMPTY : (s != FQUEUE_OK || v != m.front())) failures[t] = 2;
            if (!m.empty()) m.pop_front();
          } else {
            TestType next; // persistent push from q, then in place
            if (A::push(&next, &q, value) != FQUEUE_OK) failures[t] = 3;
            A::destroy(&q);
            q = next;
            if (A::push_in_place(&q, value + 1) != FQUEUE_OK) failures[t] = 4;
            m.push_back(value);
            m.push_back(value + 1);
          }
        }
        // Contents through the iterator (a clone popped in place).
        typename A::iter it;
        A::iter_init(&it, &q);
        int v;
        for (int expected : m)
          if (A::iter_next(&it, &v) != FQUEUE_OK || v != expected) failures[t] = 6;
        if (A::iter_next(&it, &v) != FQUEUE_EMPTY) failures[t] = 7;
        A::iter_destroy(&it);
        A::destroy(&q);
      }
    });
  }
  for (std::thread &th : threads)
    th.join();
  for (int t = 0; t < THREADS; ++t) {
    INFO("thread " << t);
    REQUIRE(failures[t] == 0);
  }
  for (size_t i = 0; i < shared.size(); ++i) {
    REQUIRE(contents_of(&shared[i]) == as_vector(models[i]));
    A::destroy(&shared[i]);
  }
}

TEMPLATE_TEST_CASE("versions are destroyed in other threads", "[sync]",
                   fqueue_sync, fqueue_pooled_sync) {
  using A = Api<TestType>;
  enum { N = 20000, VERSIONS = 8 };
  TestType base;
  A::init(&base);
  for (int i = 0; i < N; ++i)
    REQUIRE(A::push_in_place(&base, i) == FQUEUE_OK);
  std::vector<TestType> versions(VERSIONS);
  for (int i = 0; i < VERSIONS; ++i) {
    A::clone(&versions[i], &base);
    for (int k = 0; k < i * N / VERSIONS; ++k)
      REQUIRE(A::pop_in_place(&versions[i], nullptr) == FQUEUE_OK);
  }
  A::destroy(&base);
  std::vector<std::thread> threads;
  for (TestType &v : versions)
    threads.emplace_back([&v] { Api<TestType>::destroy(&v); });
  for (std::thread &th : threads)
    th.join();
}

TEMPLATE_TEST_CASE("threads read shared nodes, the last owner frees them",
                   "[sync]", fqueue_sync, fqueue_pooled_sync) {
  // Every thread walks its own clone of one version (reading value and next of
  // shared nodes, advancing past them) and destroys it; the nodes are freed by
  // whichever thread drops the last reference, after the others have read
  // them. Without release/acquire on the counts, ThreadSanitizer reports the
  // free racing with those reads.
  using A = Api<TestType>;
  enum { N = 2000, THREADS = 8, ROUNDS = 20 };
  for (int round = 0; round < ROUNDS; ++round) {
    TestType base;
    A::init(&base);
    for (int i = 0; i < N; ++i)
      REQUIRE(A::push_in_place(&base, i) == FQUEUE_OK);
    std::vector<TestType> clones(THREADS);
    for (TestType &c : clones)
      A::clone(&c, &base);
    A::destroy(&base);
    std::vector<long> sums(THREADS, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
      threads.emplace_back([&, t] {
        int v;
        while (A::pop_in_place(&clones[t], &v) == FQUEUE_OK)
          sums[t] += v;
        A::destroy(&clones[t]);
      });
    }
    for (std::thread &th : threads)
      th.join();
    for (long sum : sums)
      REQUIRE(sum == (long)N * (N - 1) / 2);
  }
}
