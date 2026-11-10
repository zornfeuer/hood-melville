#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <cstdint>
#include <deque>
#include <queue>
#include <vector>

#include "fqueue.h"

// Drain the queue into a vector by advancing through pop. The original pointer
// is not modified (functional semantics), so the caller still holds a valid
// queue after this call.
static std::vector<int> drain(fqueue *q) {
  std::vector<int> result;
  while (!fqueue_is_empty(q)) {
    option o = fqueue_front(q);
    result.push_back(unwrap(&o));
    q = fqueue_pop(q);
  }
  return result;
}

// Unwrap fqueue_front — use when the queue is known to be non-empty.
static int front_value(fqueue *q) {
  option o = fqueue_front(q);
  return unwrap(&o);
}

// Check that fqueue_front returns none (queue is empty).
static bool front_is_none(fqueue *q) {
  option o = fqueue_front(q);
  return is_none(&o);
}

TEST_CASE("fqueue_create produces an empty queue", "[fqueue]") {
  fqueue *q = fqueue_create();

  REQUIRE(fqueue_is_empty(q));
  REQUIRE(fqueue_size(q) == 0);
  REQUIRE(front_is_none(q));
  REQUIRE(fqueue_pop(q) == NULL);
}

TEST_CASE("fqueue_push returns a new queue without modifying the original",
          "[fqueue]") {
  fqueue *q0 = fqueue_create();
  fqueue *q1 = fqueue_push(q0, 42);

  // q0 must be completely unaffected.
  REQUIRE(fqueue_is_empty(q0));
  REQUIRE(fqueue_size(q0) == 0);
  REQUIRE(front_is_none(q0));

  // q1 holds exactly the one pushed element.
  REQUIRE(!fqueue_is_empty(q1));
  REQUIRE(fqueue_size(q1) == 1);
  REQUIRE(front_value(q1) == 42);
}

TEST_CASE("fqueue_pop returns a new queue without modifying the original",
          "[fqueue]") {
  fqueue *q0 = fqueue_create();
  fqueue *q1 = fqueue_push(q0, 10);
  fqueue *q2 = fqueue_pop(q1);

  // q1 must be completely unaffected.
  REQUIRE(fqueue_size(q1) == 1);
  REQUIRE(front_value(q1) == 10);

  // q2 is the queue after removing the only element.
  REQUIRE(fqueue_is_empty(q2));
  REQUIRE(fqueue_size(q2) == 0);
  REQUIRE(front_is_none(q2));
}

TEST_CASE("fqueue_pop on empty queue returns NULL", "[fqueue]") {
  fqueue *q = fqueue_create();
  REQUIRE(fqueue_pop(q) == NULL);
}

TEST_CASE("fqueue preserves FIFO order for sequential push then pop",
          "[fqueue]") {
  fqueue *q = fqueue_create();
  q = fqueue_push(q, 1);
  q = fqueue_push(q, 2);
  q = fqueue_push(q, 3);
  q = fqueue_push(q, 4);
  q = fqueue_push(q, 5);

  REQUIRE(fqueue_size(q) == 5);

  std::vector expected = {1, 2, 3, 4, 5};
  REQUIRE_THAT(drain(q), Catch::Matchers::Equals(expected));
}

TEST_CASE("fqueue_size and fqueue_is_empty are consistent", "[fqueue]") {
  fqueue *q = fqueue_create();

  for (int i = 0; i < 8; ++i) {
    REQUIRE(fqueue_size(q) == i);
    REQUIRE(fqueue_is_empty(q) == (i == 0));
    q = fqueue_push(q, i * 10);
  }
  REQUIRE(fqueue_size(q) == 8);

  for (int remaining = 8; remaining > 0; --remaining) {
    REQUIRE(fqueue_size(q) == remaining);
    REQUIRE(!fqueue_is_empty(q));
    q = fqueue_pop(q);
  }
  REQUIRE(fqueue_size(q) == 0);
  REQUIRE(fqueue_is_empty(q));
}

TEST_CASE("fqueue interleaved push and pop", "[fqueue]") {
  fqueue *q = fqueue_create();

  // push 10, 20 — pop 10 — push 30 — pop 20 — pop 30
  q = fqueue_push(q, 10);
  q = fqueue_push(q, 20);
  REQUIRE(front_value(q) == 10);
  q = fqueue_pop(q);

  q = fqueue_push(q, 30);
  REQUIRE(front_value(q) == 20);
  q = fqueue_pop(q);

  REQUIRE(front_value(q) == 30);
  q = fqueue_pop(q);

  REQUIRE(fqueue_is_empty(q));
}

TEST_CASE(
    "fqueue branching: two queues derived from the same base are independent",
    "[fqueue]") {
  // Functional semantics means two push results from the same base are
  // fully independent — operating on one must not affect the other.
  fqueue *base = fqueue_create();
  base = fqueue_push(base, 1);
  base = fqueue_push(base, 2);

  fqueue *branch_a = fqueue_push(base, 3);  // [1, 2, 3]
  fqueue *branch_b = fqueue_push(base, 99); // [1, 2, 99]

  // base is unchanged.
  REQUIRE(fqueue_size(base) == 2);
  REQUIRE(front_value(base) == 1);

  std::vector expected_a = {1, 2, 3};
  std::vector expected_b = {1, 2, 99};
  REQUIRE_THAT(drain(branch_a), Catch::Matchers::Equals(expected_a));
  REQUIRE_THAT(drain(branch_b), Catch::Matchers::Equals(expected_b));

  // base is still intact after both branches were drained.
  REQUIRE(fqueue_size(base) == 2);
  REQUIRE(front_value(base) == 1);
}

TEST_CASE("fqueue pop-derived queues are independent of each other",
          "[fqueue]") {
  // Two pop results from the same queue must not share mutable state.
  fqueue *q = fqueue_create();
  q = fqueue_push(q, 1);
  q = fqueue_push(q, 2);
  q = fqueue_push(q, 3);

  fqueue *after_pop = fqueue_pop(q);          // [2, 3]
  fqueue *after_pop2 = fqueue_pop(after_pop); // [3]

  // Original and intermediate queues must still be valid.
  REQUIRE(front_value(q) == 1);
  REQUIRE(front_value(after_pop) == 2);
  REQUIRE(front_value(after_pop2) == 3);
}

TEST_CASE("fqueue rotation: push-heavy sequence forces a full rotation cycle",
          "[fqueue]") {
  // Pushing more elements than the current front length triggers the
  // Hood-Melville rotation. Each subsequent push/pop advances it by one
  // step. This sequence is long enough to complete at least one full cycle
  // and start a second one; FIFO order must hold throughout.
  fqueue *q = fqueue_create();

  // Three pushes: rear overtakes front on the third push.
  q = fqueue_push(q, 1);
  q = fqueue_push(q, 2);
  q = fqueue_push(q, 3);

  // Interleaved operations advance the rotation step by step.
  REQUIRE(front_value(q) == 1);
  q = fqueue_pop(q); // dequeues 1, one rotation step
  q = fqueue_push(q, 4);
  q = fqueue_push(q, 5);
  REQUIRE(front_value(q) == 2);
  q = fqueue_pop(q); // dequeues 2 — may hit INVALIDATE if in APPENDING
  q = fqueue_push(q, 6);
  REQUIRE(front_value(q) == 3);
  q = fqueue_pop(q);
  REQUIRE(front_value(q) == 4);
  q = fqueue_pop(q);
  REQUIRE(front_value(q) == 5);
  q = fqueue_pop(q);
  REQUIRE(front_value(q) == 6);
  q = fqueue_pop(q);

  REQUIRE(fqueue_is_empty(q));
}

TEST_CASE("fqueue rotation: front_len+1 pops during rotation don't crash",
          "[fqueue]") {
  // Rotation starts when rear_len = front_len + 1, here front_len = 1.
  // Doing front_len+1 = 2 pops exhausts the old front mid-rotation.
  // finish_rotation must install the new front before front becomes NULL,
  // which requires 2 rotate_step calls per operation (not 1).
  fqueue *q = fqueue_create();
  q = fqueue_push(q, 1); // front=[1], rear=[]
  q = fqueue_push(q, 2); // front=[1], rear=[2], rear_len=1 <= front_len=1
  q = fqueue_push(q, 3); // rear_len=2 > front_len=1 → rotation starts

  // front still = [1]; pop exhausts it while rotation is in flight
  REQUIRE(front_value(q) == 1);
  q = fqueue_pop(q);

  // new front from rotation must be ready — must not crash or return wrong value
  REQUIRE(front_value(q) == 2);
  q = fqueue_pop(q);

  REQUIRE(front_value(q) == 3);
  q = fqueue_pop(q);

  REQUIRE(fqueue_is_empty(q));
}

TEST_CASE("fqueue stress: 200 operations match a reference std::queue",
          "[fqueue]") {
  // Drives multiple full Hood-Melville rotation cycles. The push-two-pop-one
  // pattern keeps the queue growing, which re-triggers new rotations and
  // exercises the INVALIDATE transition (pop during the APPENDING phase).
  std::queue<int> ref;
  fqueue *q = fqueue_create();

  // Phase 1: seed with 6 elements so the first rotation starts immediately.
  for (int i = 0; i < 6; ++i) {
    q = fqueue_push(q, i);
    ref.push(i);
  }

  // Phase 2: push two, pop one — net +1 per iteration, causes repeated
  // rotations across ~200 total push/pop calls.
  for (int i = 6; i < 106; ++i) {
    q = fqueue_push(q, i);
    ref.push(i);

    q = fqueue_push(q, i * 100);
    ref.push(i * 100);

    REQUIRE(front_value(q) == ref.front());
    REQUIRE(fqueue_size(q) == (int)ref.size());
    q = fqueue_pop(q);
    ref.pop();
  }

  // Phase 3: drain both queues in lockstep.
  while (!ref.empty()) {
    REQUIRE(!fqueue_is_empty(q));
    REQUIRE(fqueue_size(q) == (int)ref.size());
    REQUIRE(front_value(q) == ref.front());
    q = fqueue_pop(q);
    ref.pop();
  }
  REQUIRE(fqueue_is_empty(q));
}

// Contents of q front to back; frees every intermediate version (q itself is
// left untouched).
static std::vector<int> contents(const fqueue *q) {
  std::vector<int> out;
  fqueue *cur = fqueue_pop(const_cast<fqueue *>(q)); // NULL when empty
  if (!fqueue_is_empty(q)) out.push_back(front_value(const_cast<fqueue *>(q)));
  while (cur && !fqueue_is_empty(cur)) {
    out.push_back(front_value(cur));
    fqueue *next = fqueue_pop(cur);
    fqueue_free(cur);
    cur = next;
  }
  fqueue_free(cur);
  return out;
}

TEST_CASE("fqueue branching history matches a reference model", "[fqueue]") {
  // Same deterministic parent selection as the `branching` scenario in
  // bench/fqueue_bench.c: 8 active branches, every 64th step a branch forks
  // from a random version of the whole history.
  enum { BRANCHES = 8, OPS = 5000 };
  uint64_t x = 0x9E3779B97F4A7C15u;
  size_t heads[BRANCHES] = {0};
  std::vector<fqueue *> versions{fqueue_create()};
  std::vector<std::deque<int>> models{{}};
  size_t max_len = 0;

  for (int k = 0; k < OPS; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t slot = (x >> 3) % BRANCHES;
    size_t idx = (x & 63) == 0 ? (x >> 12) % versions.size() : heads[slot];
    fqueue *parent = versions[idx];
    std::vector<int> before = contents(parent);
    std::deque<int> model = models[idx];

    fqueue *child;
    if ((x >> 40) % 3 == 0 && !fqueue_is_empty(parent)) {
      child = fqueue_pop(parent);
      model.pop_front();
    } else {
      child = fqueue_push(parent, k);
      model.push_back(k);
    }

    REQUIRE(contents(parent) == before); // the parent is unchanged
    REQUIRE(fqueue_size(child) == (int)model.size());
    if (!model.empty()) REQUIRE(front_value(child) == model.front());
    max_len = std::max(max_len, model.size());
    heads[slot] = versions.size();
    versions.push_back(child);
    models.push_back(std::move(model));
  }

  REQUIRE(max_len > 200); // queues grow long enough for real rotations
  for (size_t i = 0; i < versions.size(); ++i) {
    std::vector<int> expected(models[i].begin(), models[i].end());
    REQUIRE(contents(versions[i]) == expected);
  }
  for (fqueue *q : versions)
    fqueue_free(q);
}

// Every push/pop history up to DEPTH operations, checked against std::deque
// in every reachable state. Each version is derived from a live parent and
// freed after its subtree is explored.
static void explore(fqueue *q, std::deque<int> &model, int depth, long &states) {
  ++states;
  REQUIRE(fqueue_size(q) == (int)model.size());
  REQUIRE(fqueue_is_empty(q) == model.empty());
  if (model.empty()) REQUIRE(front_is_none(q));
  else REQUIRE(front_value(q) == model.front());
  if (depth == 0) return;

  fqueue *pushed = fqueue_push(q, depth);
  model.push_back(depth);
  explore(pushed, model, depth - 1, states);
  model.pop_back();
  fqueue_free(pushed);

  if (!model.empty()) {
    fqueue *popped = fqueue_pop(q);
    int x = model.front();
    model.pop_front();
    explore(popped, model, depth - 1, states);
    model.push_front(x);
    fqueue_free(popped);
  }
}

TEST_CASE("fqueue exhaustive short histories match a reference model",
          "[fqueue]") {
  enum { DEPTH = 16 };
  fqueue *q = fqueue_create();
  std::deque<int> model;
  long states = 0;
  explore(q, model, DEPTH, states);
  REQUIRE(states > 20000);
  fqueue_free(q);
}

// ---------------------------------------------------------------------------
// Value API (fqueue_init / fqueue_push_into / fqueue_pop_into / fqueue_release)
// ---------------------------------------------------------------------------

static void explore_value(const fqueue *q, std::deque<int> &model, int depth,
                          long &states) {
  ++states;
  REQUIRE(fqueue_size(q) == (int)model.size());
  REQUIRE(fqueue_is_empty(q) == model.empty());
  option o = fqueue_front(q);
  if (model.empty()) REQUIRE(is_none(&o));
  else REQUIRE(unwrap(&o) == model.front());
  if (depth == 0) return;

  fqueue child;
  fqueue_push_into(&child, q, depth);
  model.push_back(depth);
  explore_value(&child, model, depth - 1, states);
  model.pop_back();
  fqueue_release(&child);

  if (fqueue_pop_into(&child, q)) {
    int x = model.front();
    model.pop_front();
    explore_value(&child, model, depth - 1, states);
    model.push_front(x);
    fqueue_release(&child);
  } else {
    REQUIRE(model.empty());
  }
}

TEST_CASE("fqueue value API: exhaustive short histories", "[fqueue]") {
  fqueue q;
  fqueue_init(&q);
  std::deque<int> model;
  long states = 0;
  explore_value(&q, model, 16, states);
  REQUIRE(states > 20000);
  fqueue_release(&q);
}

TEST_CASE("fqueue value API: branching history stored by value", "[fqueue]") {
  // Versions live in a std::vector<fqueue>: growth moves them bitwise, which
  // the value API allows. Same parent selection as the `branching` scenario.
  enum { BRANCHES = 8, OPS = 5000 };
  uint64_t x = 0x9E3779B97F4A7C15u;
  size_t heads[BRANCHES] = {0};
  std::vector<fqueue> versions(1);
  fqueue_init(&versions[0]);
  std::vector<std::deque<int>> models{{}};

  for (int k = 0; k < OPS; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t slot = (x >> 3) % BRANCHES;
    size_t idx = (x & 63) == 0 ? (x >> 12) % versions.size() : heads[slot];
    std::deque<int> model = models[idx];
    fqueue child;
    if ((x >> 40) % 3 == 0 && fqueue_pop_into(&child, &versions[idx])) {
      model.pop_front();
    } else {
      fqueue_push_into(&child, &versions[idx], k);
      model.push_back(k);
    }
    heads[slot] = versions.size();
    versions.push_back(child); // moves the new version into the vector
    models.push_back(std::move(model));
  }
  for (size_t i = 0; i < versions.size(); ++i) {
    std::vector<int> expected(models[i].begin(), models[i].end());
    REQUIRE(contents(&versions[i]) == expected);
  }
  for (fqueue &q : versions)
    fqueue_release(&q);
}

// ============================================================================
// Benchmarks
// Run: ./persistent_test "[benchmark]" --benchmark-samples 200
//
// Goal: show that push and pop are O(1) worst-case — time per single operation
// stays flat as queue size N grows from 100 to 100 000.
//
// A naive two-stack queue would be O(n) on the push that triggers reversal;
// Hood-Melville spreads that work across 2 steps/op, so every push/pop
// costs the same constant time regardless of N.
// ============================================================================

// Builds a queue with n elements. Not measured — just setup.
static fqueue *bench_make_queue(int n) {
  fqueue *q = fqueue_create();
  for (int i = 0; i < n; ++i)
    q = fqueue_push(q, i);
  return q;
}

TEST_CASE("bench: fqueue_push is O(1) worst-case", "[fqueue][benchmark]") {
  // Each sub-benchmark measures a SINGLE push onto a pre-built queue of size N.
  // If push were O(n), these numbers would scale linearly — they should be flat.
  BENCHMARK_ADVANCED("push at N=100")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(100);
    meter.measure([&] { return fqueue_push(q, 42); });
  };
  BENCHMARK_ADVANCED("push at N=1 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(1'000);
    meter.measure([&] { return fqueue_push(q, 42); });
  };
  BENCHMARK_ADVANCED("push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(10'000);
    meter.measure([&] { return fqueue_push(q, 42); });
  };
  BENCHMARK_ADVANCED("push at N=100 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(100'000);
    meter.measure([&] { return fqueue_push(q, 42); });
  };
}

TEST_CASE("bench: fqueue_pop is O(1) worst-case", "[fqueue][benchmark]") {
  // Same idea for pop. The worst case is a pop that steps through a rotation;
  // it should still take O(1) time.
  BENCHMARK_ADVANCED("pop at N=100")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(100);
    meter.measure([&] { return fqueue_pop(q); });
  };
  BENCHMARK_ADVANCED("pop at N=1 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(1'000);
    meter.measure([&] { return fqueue_pop(q); });
  };
  BENCHMARK_ADVANCED("pop at N=10 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(10'000);
    meter.measure([&] { return fqueue_pop(q); });
  };
  BENCHMARK_ADVANCED("pop at N=100 000")(Catch::Benchmark::Chronometer meter) {
    fqueue *q = bench_make_queue(100'000);
    meter.measure([&] { return fqueue_pop(q); });
  };
}

TEST_CASE("bench: rotation-trigger push vs normal push", "[fqueue][benchmark]") {
  // The push that triggers rotation (rear_len just exceeded front_len) is
  // the theoretical worst case for a naive queue. Here it should cost the same
  // as a push that does nothing special.
  BENCHMARK_ADVANCED("rotation-trigger push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    // Build N-1 elements so the very next push fires start_rotation.
    fqueue *q = bench_make_queue(10'000 - 1);
    meter.measure([&] { return fqueue_push(q, 42); });
  };
  BENCHMARK_ADVANCED("normal push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    // Pop once after building so rear_len < front_len — next push stays idle.
    fqueue *q = fqueue_pop(bench_make_queue(10'000));
    meter.measure([&] { return fqueue_push(q, 42); });
  };
}
