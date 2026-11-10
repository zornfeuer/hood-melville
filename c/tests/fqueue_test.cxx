#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "fqueue.h"

// RAII owner of one version, so that tests read like the Rust ones and cannot
// leak: copying is fqueue_clone, destruction is fqueue_destroy, moving is a
// bitwise copy followed by re-initializing the source.
class Q {
public:
  Q() { fqueue_init(&q_); }
  Q(const Q &other) { fqueue_clone(&q_, &other.q_); }
  Q(Q &&other) noexcept : q_(other.q_) { fqueue_init(&other.q_); }
  Q &operator=(Q other) noexcept {
    std::swap(q_, other.q_);
    return *this;
  }
  ~Q() { fqueue_destroy(&q_); }

  Q push(int value) const {
    Q r{uninit};
    fqueue_push(&r.q_, &q_, value);
    return r;
  }

  // nullopt on an empty queue.
  std::optional<Q> pop() const {
    Q r{uninit};
    if (fqueue_pop(&r.q_, &q_)) return r;
    fqueue_init(&r.q_); // fqueue_pop left it untouched
    return std::nullopt;
  }

  std::optional<int> front() const {
    int v;
    if (!fqueue_front(&q_, &v)) return std::nullopt;
    return v;
  }

  int  size() const { return fqueue_size(&q_); }
  bool empty() const { return fqueue_is_empty(&q_); }

  const fqueue *get() const { return &q_; }

private:
  struct Uninit {};
  static constexpr Uninit uninit{};
  explicit Q(Uninit) {} // storage for an fqueue_* output parameter

  fqueue q_;
};

// Value at the front of a queue known to be non-empty.
static int front_of(const fqueue *q) {
  int v;
  REQUIRE(fqueue_front(q, &v));
  return v;
}

// Contents of q front to back; q itself is left untouched. Walks a clone,
// moving each new version into `cur` bitwise.
static std::vector<int> contents(const fqueue *q) {
  std::vector<int> out;
  fqueue cur, next;
  fqueue_clone(&cur, q);
  while (!fqueue_is_empty(&cur)) {
    out.push_back(front_of(&cur));
    fqueue_pop(&next, &cur);
    fqueue_destroy(&cur);
    cur = next;
  }
  fqueue_destroy(&cur);
  return out;
}

static std::vector<int> contents(const Q &q) { return contents(q.get()); }

TEST_CASE("fqueue_init produces an empty queue", "[fqueue]") {
  fqueue q;
  fqueue_init(&q);

  REQUIRE(fqueue_is_empty(&q));
  REQUIRE(fqueue_size(&q) == 0);
  int v = 7;
  REQUIRE(!fqueue_front(&q, &v));
  REQUIRE(v == 7); // left untouched
  fqueue_destroy(&q);
}

TEST_CASE("fqueue_pop on an empty queue fails and leaves out untouched",
          "[fqueue]") {
  fqueue q, out;
  fqueue_init(&q);
  std::memset(&out, 0xA5, sizeof out);
  unsigned char before[sizeof out];
  std::memcpy(before, &out, sizeof out);

  REQUIRE(!fqueue_pop(&out, &q));
  REQUIRE(std::memcmp(before, &out, sizeof out) == 0);
  fqueue_destroy(&q);
}

TEST_CASE("fqueue_push writes a new version without modifying the original",
          "[fqueue]") {
  Q q0;
  Q q1 = q0.push(42);

  // q0 must be completely unaffected.
  REQUIRE(q0.empty());
  REQUIRE(q0.size() == 0);
  REQUIRE(q0.front() == std::nullopt);

  // q1 holds exactly the one pushed element.
  REQUIRE(!q1.empty());
  REQUIRE(q1.size() == 1);
  REQUIRE(q1.front() == 42);
}

TEST_CASE("fqueue_pop writes a new version without modifying the original",
          "[fqueue]") {
  Q q1 = Q().push(10);
  Q q2 = *q1.pop();

  // q1 must be completely unaffected.
  REQUIRE(q1.size() == 1);
  REQUIRE(q1.front() == 10);

  // q2 is the queue after removing the only element.
  REQUIRE(q2.empty());
  REQUIRE(q2.size() == 0);
  REQUIRE(q2.front() == std::nullopt);
  REQUIRE(!q2.pop());
}

TEST_CASE("fqueue preserves FIFO order for sequential push then pop",
          "[fqueue]") {
  Q q = Q().push(1).push(2).push(3).push(4).push(5);

  REQUIRE(q.size() == 5);

  std::vector expected = {1, 2, 3, 4, 5};
  REQUIRE_THAT(contents(q), Catch::Matchers::Equals(expected));
}

TEST_CASE("fqueue_size and fqueue_is_empty are consistent", "[fqueue]") {
  Q q;

  for (int i = 0; i < 8; ++i) {
    REQUIRE(q.size() == i);
    REQUIRE(q.empty() == (i == 0));
    q = q.push(i * 10);
  }
  REQUIRE(q.size() == 8);

  for (int remaining = 8; remaining > 0; --remaining) {
    REQUIRE(q.size() == remaining);
    REQUIRE(!q.empty());
    q = *q.pop();
  }
  REQUIRE(q.size() == 0);
  REQUIRE(q.empty());
}

TEST_CASE("fqueue interleaved push and pop", "[fqueue]") {
  Q q;

  // push 10, 20 — pop 10 — push 30 — pop 20 — pop 30
  q = q.push(10).push(20);
  REQUIRE(q.front() == 10);
  q = *q.pop();

  q = q.push(30);
  REQUIRE(q.front() == 20);
  q = *q.pop();

  REQUIRE(q.front() == 30);
  q = *q.pop();

  REQUIRE(q.empty());
}

TEST_CASE(
    "fqueue branching: two queues derived from the same base are independent",
    "[fqueue]") {
  // Functional semantics means two push results from the same base are
  // fully independent — operating on one must not affect the other.
  Q base = Q().push(1).push(2);

  Q branch_a = base.push(3);  // [1, 2, 3]
  Q branch_b = base.push(99); // [1, 2, 99]

  // base is unchanged.
  REQUIRE(base.size() == 2);
  REQUIRE(base.front() == 1);

  std::vector expected_a = {1, 2, 3};
  std::vector expected_b = {1, 2, 99};
  REQUIRE_THAT(contents(branch_a), Catch::Matchers::Equals(expected_a));
  REQUIRE_THAT(contents(branch_b), Catch::Matchers::Equals(expected_b));

  // base is still intact after both branches were walked.
  REQUIRE(base.size() == 2);
  REQUIRE(base.front() == 1);
}

TEST_CASE("fqueue pop-derived queues are independent of each other",
          "[fqueue]") {
  // Two pop results from the same queue must not share mutable state.
  Q q = Q().push(1).push(2).push(3);

  Q after_pop = *q.pop();          // [2, 3]
  Q after_pop2 = *after_pop.pop(); // [3]

  // Original and intermediate queues must still be valid.
  REQUIRE(q.front() == 1);
  REQUIRE(after_pop.front() == 2);
  REQUIRE(after_pop2.front() == 3);
}

TEST_CASE("fqueue rotation: push-heavy sequence forces a full rotation cycle",
          "[fqueue]") {
  // Pushing more elements than the current front length triggers the
  // Hood-Melville rotation. Each subsequent push/pop advances it by one
  // step. This sequence is long enough to complete at least one full cycle
  // and start a second one; FIFO order must hold throughout.

  // Three pushes: rear overtakes front on the third push.
  Q q = Q().push(1).push(2).push(3);

  // Interleaved operations advance the rotation step by step.
  REQUIRE(q.front() == 1);
  q = *q.pop(); // dequeues 1, one rotation step
  q = q.push(4).push(5);
  REQUIRE(q.front() == 2);
  q = *q.pop(); // dequeues 2 — may hit INVALIDATE if in APPENDING
  q = q.push(6);
  REQUIRE(q.front() == 3);
  q = *q.pop();
  REQUIRE(q.front() == 4);
  q = *q.pop();
  REQUIRE(q.front() == 5);
  q = *q.pop();
  REQUIRE(q.front() == 6);
  q = *q.pop();

  REQUIRE(q.empty());
}

TEST_CASE("fqueue rotation: front_len+1 pops during rotation don't crash",
          "[fqueue]") {
  // Rotation starts when rear_len = front_len + 1, here front_len = 1.
  // Doing front_len+1 = 2 pops exhausts the old front mid-rotation.
  // finish_rotation must install the new front before front becomes NULL.
  Q q = Q().push(1)  // front=[1], rear=[]
             .push(2)  // front=[1], rear=[2], rear_len=1 <= front_len=1
             .push(3); // rear_len=2 > front_len=1 → rotation starts

  // front still = [1]; pop exhausts it while rotation is in flight
  REQUIRE(q.front() == 1);
  q = *q.pop();

  // new front from rotation must be ready — must not crash or return wrong value
  REQUIRE(q.front() == 2);
  q = *q.pop();

  REQUIRE(q.front() == 3);
  q = *q.pop();

  REQUIRE(q.empty());
}

TEST_CASE("fqueue stress: 200 operations match a reference std::queue",
          "[fqueue]") {
  // Drives multiple full Hood-Melville rotation cycles. The push-two-pop-one
  // pattern keeps the queue growing, which re-triggers new rotations and
  // exercises the INVALIDATE transition (pop during the APPENDING phase).
  std::queue<int> ref;
  Q q;

  // Phase 1: seed with 6 elements so the first rotation starts immediately.
  for (int i = 0; i < 6; ++i) {
    q = q.push(i);
    ref.push(i);
  }

  // Phase 2: push two, pop one — net +1 per iteration, causes repeated
  // rotations across ~200 total push/pop calls.
  for (int i = 6; i < 106; ++i) {
    q = q.push(i);
    ref.push(i);

    q = q.push(i * 100);
    ref.push(i * 100);

    REQUIRE(q.front() == ref.front());
    REQUIRE(q.size() == (int)ref.size());
    q = *q.pop();
    ref.pop();
  }

  // Phase 3: drain both queues in lockstep.
  while (!ref.empty()) {
    REQUIRE(!q.empty());
    REQUIRE(q.size() == (int)ref.size());
    REQUIRE(q.front() == ref.front());
    q = *q.pop();
    ref.pop();
  }
  REQUIRE(q.empty());
}

TEST_CASE("fqueue_clone shares contents and outlives the original",
          "[fqueue]") {
  // Build a version in the middle of a rotation, so that the clone also has
  // to retain the rotation's node lists.
  Q q;
  for (int i = 0; i < 9; ++i)
    q = q.push(i);
  q = *q.pop();

  fqueue c;
  fqueue_clone(&c, q.get());
  std::vector<int> expected = contents(q);
  REQUIRE(contents(&c) == expected);

  // Diverge: the original and the clone are independent owners.
  Q pushed = q.push(100);
  q = Q(); // destroys the original
  REQUIRE(contents(&c) == expected);
  expected.push_back(100);
  REQUIRE(contents(pushed) == expected);
  fqueue_destroy(&c);
}

TEST_CASE("fqueue_destroy leaves storage reusable as out or for init",
          "[fqueue]") {
  fqueue q, q1;
  fqueue_init(&q);
  fqueue_push(&q1, &q, 1);
  fqueue_destroy(&q);
  fqueue_push(&q, &q1, 2); // destroyed storage as out
  fqueue_destroy(&q1);
  REQUIRE(contents(&q) == std::vector<int>{1, 2});

  fqueue_destroy(&q);
  fqueue_init(&q); // destroyed storage re-initialized
  REQUIRE(fqueue_is_empty(&q));
  fqueue_destroy(&q);
}

TEST_CASE("fqueue branching history stored by value matches a reference model",
          "[fqueue]") {
  // Versions live in a std::vector<fqueue>: growth moves them bitwise, which
  // the API allows. Same deterministic parent selection as the `branching`
  // scenario in bench/fqueue_bench.c: 8 active branches, every 64th step a
  // branch forks from a random version of the whole history.
  enum { BRANCHES = 8, OPS = 5000 };
  uint64_t x = 0x9E3779B97F4A7C15u;
  size_t heads[BRANCHES] = {0};
  std::vector<fqueue> versions(1);
  fqueue_init(&versions[0]);
  std::vector<std::deque<int>> models{{}};
  size_t max_len = 0;

  for (int k = 0; k < OPS; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t slot = (x >> 3) % BRANCHES;
    size_t idx = (x & 63) == 0 ? (x >> 12) % versions.size() : heads[slot];
    const fqueue *parent = &versions[idx];
    std::vector<int> before = contents(parent);
    std::deque<int> model = models[idx];

    fqueue child;
    if ((x >> 40) % 3 == 0 && fqueue_pop(&child, parent)) {
      model.pop_front();
    } else {
      fqueue_push(&child, parent, k);
      model.push_back(k);
    }

    REQUIRE(contents(parent) == before); // the parent is unchanged
    REQUIRE(fqueue_size(&child) == (int)model.size());
    if (!model.empty()) REQUIRE(front_of(&child) == model.front());
    max_len = std::max(max_len, model.size());
    heads[slot] = versions.size();
    versions.push_back(child); // moves the new version into the vector
    models.push_back(std::move(model));
  }

  REQUIRE(max_len > 200); // queues grow long enough for real rotations
  for (size_t i = 0; i < versions.size(); ++i) {
    std::vector<int> expected(models[i].begin(), models[i].end());
    REQUIRE(contents(&versions[i]) == expected);
  }
  for (fqueue &q : versions)
    fqueue_destroy(&q);
}

// Every push/pop history up to DEPTH operations, checked against std::deque
// in every reachable state; every state is also cloned and the clone checked.
// Each version is derived from a live parent and destroyed after its subtree
// is explored.
static void explore(const fqueue *q, std::deque<int> &model, int depth,
                    long &states) {
  ++states;
  REQUIRE(fqueue_size(q) == (int)model.size());
  REQUIRE(fqueue_is_empty(q) == model.empty());
  int v;
  REQUIRE(fqueue_front(q, &v) == !model.empty());
  if (!model.empty()) REQUIRE(v == model.front());

  fqueue c;
  fqueue_clone(&c, q);
  REQUIRE(contents(&c) == std::vector<int>(model.begin(), model.end()));
  fqueue_destroy(&c);
  if (depth == 0) return;

  fqueue child;
  fqueue_push(&child, q, depth);
  model.push_back(depth);
  explore(&child, model, depth - 1, states);
  model.pop_back();
  fqueue_destroy(&child);

  if (fqueue_pop(&child, q)) {
    int x = model.front();
    model.pop_front();
    explore(&child, model, depth - 1, states);
    model.push_front(x);
    fqueue_destroy(&child);
  } else {
    REQUIRE(model.empty());
  }
}

TEST_CASE("fqueue exhaustive short histories match a reference model",
          "[fqueue]") {
  fqueue q;
  fqueue_init(&q);
  std::deque<int> model;
  long states = 0;
  explore(&q, model, 16, states);
  REQUIRE(states > 20000);
  fqueue_destroy(&q);
}

// ============================================================================
// Benchmarks
// Run: ./fqueue_test "[benchmark]" --benchmark-samples 200
//
// Goal: show that push and pop are O(1) worst-case — time per single operation
// stays flat as queue size N grows from 100 to 100 000.
//
// A naive two-stack queue would be O(n) on the push that triggers reversal;
// Hood-Melville spreads that work across operations, so every push/pop
// costs the same constant time regardless of N. Each measurement includes
// destroying the new version, which frees at most the nodes it created.
// ============================================================================

// Builds a queue with n elements. Not measured — just setup.
static Q bench_make_queue(int n) {
  Q q;
  for (int i = 0; i < n; ++i)
    q = q.push(i);
  return q;
}

TEST_CASE("bench: fqueue_push is O(1) worst-case", "[fqueue][benchmark]") {
  // Each sub-benchmark measures a SINGLE push onto a pre-built queue of size N.
  // If push were O(n), these numbers would scale linearly — they should be flat.
  BENCHMARK_ADVANCED("push at N=100")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(100);
    meter.measure([&] { return q.push(42); });
  };
  BENCHMARK_ADVANCED("push at N=1 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(1'000);
    meter.measure([&] { return q.push(42); });
  };
  BENCHMARK_ADVANCED("push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(10'000);
    meter.measure([&] { return q.push(42); });
  };
  BENCHMARK_ADVANCED("push at N=100 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(100'000);
    meter.measure([&] { return q.push(42); });
  };
}

TEST_CASE("bench: fqueue_pop is O(1) worst-case", "[fqueue][benchmark]") {
  // Same idea for pop. The worst case is a pop that steps through a rotation;
  // it should still take O(1) time.
  BENCHMARK_ADVANCED("pop at N=100")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(100);
    meter.measure([&] { return q.pop(); });
  };
  BENCHMARK_ADVANCED("pop at N=1 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(1'000);
    meter.measure([&] { return q.pop(); });
  };
  BENCHMARK_ADVANCED("pop at N=10 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(10'000);
    meter.measure([&] { return q.pop(); });
  };
  BENCHMARK_ADVANCED("pop at N=100 000")(Catch::Benchmark::Chronometer meter) {
    Q q = bench_make_queue(100'000);
    meter.measure([&] { return q.pop(); });
  };
}

TEST_CASE("bench: rotation-trigger push vs normal push", "[fqueue][benchmark]") {
  // The push that triggers rotation (rear_len just exceeded front_len) is
  // the theoretical worst case for a naive queue. Here it should cost the same
  // as a push that does nothing special.
  BENCHMARK_ADVANCED("rotation-trigger push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    // Build N-1 elements so the very next push fires start_rotation.
    Q q = bench_make_queue(10'000 - 1);
    meter.measure([&] { return q.push(42); });
  };
  BENCHMARK_ADVANCED("normal push at N=10 000")(Catch::Benchmark::Chronometer meter) {
    // Pop once after building so rear_len < front_len — next push stays idle.
    Q q = *bench_make_queue(10'000).pop();
    meter.measure([&] { return q.push(42); });
  };
}
