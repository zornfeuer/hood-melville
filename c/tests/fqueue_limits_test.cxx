// Error paths: fqueue.c built with a failing test allocator and small limits
// (TEST_LEN_LIMIT, TEST_REFS_LIMIT; see CMakeLists.txt). Every failure must
// leave `q` unchanged and leak nothing.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <vector>

#include "fqueue.h"
#include "fqueue_test_util.hpp"

// Test allocator: counts live blocks and can fail one chosen allocation.
static long live = 0;     // allocated and not yet freed
static long fail_in = -1; // fail the allocation after this many; -1: never
static bool injected = false;

extern "C" void *fqueue_test_malloc(size_t size) {
  if (fail_in >= 0 && fail_in-- == 0) {
    injected = true;
    return nullptr;
  }
  void *p = std::malloc(size);
  if (p) ++live;
  return p;
}

extern "C" void fqueue_test_free(void *ptr) {
  if (ptr) --live;
  std::free(ptr);
}

// Fail the k-th allocation (from 0) of the next operation.
static void arm(long k) {
  fail_in = k;
  injected = false;
}

static void disarm() { fail_in = -1; }

// Perform `op` (writing *out from q), failing its k-th allocation for
// k = 0, 1, ... until it succeeds. Every injected failure must yield
// FQUEUE_NOMEM, leave q's contents as `expected` and leak nothing. Returns the
// number of failures; *out holds the successful result.
template <class Op>
static long op_under_oom(Op op, fqueue *out, const fqueue *q,
                         const std::vector<int> &expected) {
  for (long k = 0;; ++k) {
    long before = live;
    arm(k);
    fqueue_status s = op(out, q);
    disarm();
    if (!injected) {
      REQUIRE(s == FQUEUE_OK);
      return k;
    }
    REQUIRE(s == FQUEUE_NOMEM);
    REQUIRE(live == before);
    REQUIRE(contents(q) == expected);
  }
}

static fqueue_status do_pop(fqueue *out, const fqueue *q) {
  return fqueue_pop(out, q);
}

// Every push/pop history up to `depth` operations, with every allocation of
// every operation failed once before it is allowed to succeed.
static void explore_oom(const fqueue *q, std::deque<int> &model, int depth,
                        long &failures) {
  std::vector<int> expected(model.begin(), model.end());
  REQUIRE(contents(q) == expected);
  if (depth == 0) return;

  fqueue child;
  failures += op_under_oom(
      [&](fqueue *out, const fqueue *p) { return fqueue_push(out, p, depth); },
      &child, q, expected);
  model.push_back(depth);
  explore_oom(&child, model, depth - 1, failures);
  model.pop_back();
  fqueue_destroy(&child);

  if (!model.empty()) {
    failures += op_under_oom(do_pop, &child, q, expected);
    int x = model.front();
    model.pop_front();
    explore_oom(&child, model, depth - 1, failures);
    model.push_front(x);
    fqueue_destroy(&child);
  }
}

TEST_CASE("out of memory in any allocation of short histories", "[limits]") {
  long start = live;
  fqueue q;
  fqueue_init(&q);
  std::deque<int> model;
  long failures = 0;
  explore_oom(&q, model, 10, failures);
  fqueue_destroy(&q);
  REQUIRE(failures > 500);
  REQUIRE(live == start);
}

TEST_CASE("out of memory in any allocation along a long history", "[limits]") {
  // Push two, pop one, then drain: rotations over queues of many lengths,
  // up to the length limit. Old versions stay alive, so pops see shared nodes.
  long start = live;
  {
    std::vector<Q> versions(1);
    std::deque<int> model;
    long failures = 0;
    int k = 0;
    auto push = [&] {
      std::vector<int> expected(model.begin(), model.end());
      fqueue out;
      failures += op_under_oom(
          [&](fqueue *o, const fqueue *p) { return fqueue_push(o, p, k); },
          &out, versions.back().get(), expected);
      fqueue_destroy(&out); // checked; now build it through the wrapper
      versions.push_back(versions.back().push(k));
      model.push_back(k++);
    };
    auto pop = [&] {
      std::vector<int> expected(model.begin(), model.end());
      fqueue out;
      failures += op_under_oom(do_pop, &out, versions.back().get(), expected);
      fqueue_destroy(&out);
      versions.push_back(*versions.back().pop());
      model.pop_front();
    };
    while (model.size() + 2 <= TEST_LEN_LIMIT) {
      push();
      push();
      pop();
    }
    while (!model.empty())
      pop();
    REQUIRE(failures > 100);
  }
  REQUIRE(live == start);
}

TEST_CASE("pushing past the length limit reports FQUEUE_OVERFLOW",
          "[limits]") {
  long start = live;
  {
    Q q;
    for (int i = 0; i < TEST_LEN_LIMIT; ++i)
      q = q.push(i);
    std::vector<int> expected = contents(q);

    fqueue out;
    REQUIRE(fqueue_push(&out, q.get(), -1) == FQUEUE_OVERFLOW);
    REQUIRE(contents(q) == expected);

    Q popped = *q.pop(); // there is room again
    REQUIRE(popped.push(-1).size() == TEST_LEN_LIMIT);
  }
  REQUIRE(live == start);
}

TEST_CASE("too many references to a node report FQUEUE_OVERFLOW", "[limits]") {
  long start = live;
  {
    // A version in the middle of a rotation, so that its rotation's nodes are
    // shared too.
    Q q;
    for (int i = 0; i < 9; ++i)
      q = q.push(i);
    q = *q.pop();
    std::vector<int> expected = contents(q);

    // Clone until some node of q is at the limit; the failing clone is undone.
    std::vector<fqueue> clones;
    fqueue_status s;
    for (;;) {
      fqueue c;
      s = fqueue_clone(&c, q.get());
      if (s != FQUEUE_OK) break;
      clones.push_back(c);
      REQUIRE(clones.size() <= TEST_REFS_LIMIT);
    }
    REQUIRE(s == FQUEUE_OVERFLOW);
    REQUIRE(clones.size() > TEST_REFS_LIMIT / 2);
    long saturated = live;

    // Every operation on q would add a reference to a saturated node.
    fqueue out;
    REQUIRE(fqueue_push(&out, q.get(), -1) == FQUEUE_OVERFLOW);
    REQUIRE(fqueue_pop(&out, q.get()) == FQUEUE_OVERFLOW);
    REQUIRE(live == saturated);

    // Destroying some clones makes room again; all versions are intact.
    for (int i = 0; i < 8; ++i) {
      fqueue_destroy(&clones.back());
      clones.pop_back();
    }
    REQUIRE(contents(q) == expected);
    REQUIRE(contents(&clones.front()) == expected);
    expected.push_back(-1);
    REQUIRE(contents(q.push(-1)) == expected);

    for (fqueue &c : clones)
      fqueue_destroy(&c);
  }
  REQUIRE(live == start);
}

TEST_CASE("random operations on many shared versions never overflow",
          "[limits]") {
  // Versions are cloned, pushed, popped and destroyed at random while the
  // reference limit is hit again and again. A reference the overflow check
  // missed would let counts climb past the limit plus headroom, which the
  // assert in node_retain catches; every successful result is checked against
  // a model.
  long start = live;
  {
    std::vector<fqueue> versions(1);
    fqueue_init(&versions[0]);
    std::vector<std::deque<int>> models{{}};
    uint64_t x = 0x2545F4914F6CDD1Du;
    long overflows = 0;
    for (int k = 0; k < 20000; ++k) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      // Half of the operations hit the first few versions, so that the same
      // nodes are shared over and over.
      size_t n = (x & 1) ? std::min<size_t>(versions.size(), 4) : versions.size();
      size_t i = (x >> 8) % n;
      std::deque<int> model = models[i];
      fqueue out;
      fqueue_status s;
      switch ((x >> 40) % 8) {
      case 0: // destroy (keep at least one version)
        if (versions.size() > 1) {
          fqueue_destroy(&versions[i]);
          versions.erase(versions.begin() + (long)i);
          models.erase(models.begin() + (long)i);
        }
        continue;
      case 1:
      case 2:
        s = fqueue_clone(&out, &versions[i]);
        break;
      case 3:
      case 4:
        s = fqueue_pop(&out, &versions[i]);
        if (s == FQUEUE_OK) model.pop_front();
        break;
      default:
        s = fqueue_push(&out, &versions[i], k);
        if (s == FQUEUE_OK) model.push_back(k);
        break;
      }
      if (s == FQUEUE_OVERFLOW) {
        ++overflows;
        continue;
      }
      if (s == FQUEUE_EMPTY) {
        REQUIRE(models[i].empty());
        continue;
      }
      REQUIRE(s == FQUEUE_OK);
      REQUIRE(fqueue_size(&out) == model.size());
      if (!model.empty()) REQUIRE(front_of(&out) == model.front());
      versions.push_back(out);
      models.push_back(std::move(model));
    }
    REQUIRE(overflows > 100);
    for (fqueue &v : versions)
      fqueue_destroy(&v);
  }
  REQUIRE(live == start);
}
