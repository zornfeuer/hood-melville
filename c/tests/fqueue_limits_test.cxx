// Error paths: fqueue.c built with a failing test allocator and small limits
// (TEST_LEN_LIMIT, TEST_REFS_LIMIT; see CMakeLists.txt). Every failure must
// leave `q` unchanged and leak nothing; only saturated reference counts leak
// their nodes, by design.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <set>
#include <type_traits>
#include <vector>

#include "fqueue.h"
#include "fqueue_api.hpp"
#include "fqueue_test_util.hpp"

// Test allocator: counts live blocks and can fail one chosen allocation.
static long live = 0;     // allocated and not yet freed
static long allocs = 0;   // successful allocations, ever
static long fail_in = -1; // fail the allocation after this many; -1: never
static bool injected = false;
// Blocks allocated while tracking and not yet freed: the nodes that saturated
// counts leak, freed by the test that made them (free_tracked).
static std::set<void *> *tracked = nullptr;

// The block from `get` (nullptr if the armed allocation fails), counted.
template <class Get> static void *test_alloc(Get get) {
  if (fail_in == 0) {
    fail_in = -1;
    injected = true;
    return nullptr;
  }
  if (fail_in > 0) --fail_in;
  void *p = get();
  if (p) ++live, ++allocs;
  if (p && tracked) tracked->insert(p);
  return p;
}

extern "C" void *fqueue_test_malloc(size_t size) {
  return test_alloc([size] { return std::malloc(size); });
}

extern "C" void *fqueue_test_aligned_malloc(size_t alignment, size_t size) {
  return test_alloc([=] { return std::aligned_alloc(alignment, size); });
}

extern "C" void fqueue_test_free(void *ptr) {
  if (ptr) --live;
  if (tracked) tracked->erase(ptr);
  std::free(ptr);
}

// Track the blocks allocated from here on.
static void track(std::set<void *> &blocks) { tracked = &blocks; }

// Free the tracked blocks still alive (leaked by saturated counts); returns
// their number.
static long free_tracked() {
  std::set<void *> &blocks = *tracked;
  tracked = nullptr;
  for (void *p : blocks) fqueue_test_free(p);
  long n = (long)blocks.size();
  blocks.clear();
  return n;
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
template <class Op, class Q>
static long op_under_oom(Op op, Q *out, const Q *q,
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
    REQUIRE(contents_of(q) == expected);
  }
}

template <class Q> static fqueue_status do_pop(Q *out, const Q *q) {
  return Api<Q>::pop(out, q);
}

// The same for an in-place operation on *q: every injected failure must leave
// *q as `expected`. Returns the number of failures; *q is then changed.
template <class Op, class Q>
static long in_place_under_oom(Op op, Q *q, const std::vector<int> &expected) {
  for (long k = 0;; ++k) {
    long before = live;
    arm(k);
    fqueue_status s = op(q);
    disarm();
    if (!injected) {
      REQUIRE(s == FQUEUE_OK);
      return k;
    }
    REQUIRE(s == FQUEUE_NOMEM);
    REQUIRE(live == before);
    REQUIRE(contents_of(q) == expected);
  }
}

template <class Q> static fqueue_status do_pop_in_place(Q *q) {
  int v = -1;
  fqueue_status s = Api<Q>::pop_in_place(q, &v);
  if (s != FQUEUE_OK) REQUIRE(v == -1);
  return s;
}

// In-place push and pop under OOM on a clone of q, which shares its nodes.
template <class Q>
static long in_place_oom_on_clone(const Q *q, std::deque<int> model,
                                  int value) {
  std::vector<int> expected(model.begin(), model.end());
  Q c;
  Api<Q>::clone(&c, q);
  long failures = in_place_under_oom(
      [&](Q *p) { return Api<Q>::push_in_place(p, value); }, &c, expected);
  expected.push_back(value);
  REQUIRE(contents_of(&c) == expected);
  failures += in_place_under_oom(do_pop_in_place<Q>, &c, expected);
  expected.erase(expected.begin());
  REQUIRE(contents_of(&c) == expected);
  Api<Q>::destroy(&c);
  REQUIRE(contents_of(q) == std::vector<int>(model.begin(), model.end()));
  return failures;
}

// Every push/pop history up to `depth` operations, with every allocation of
// every operation failed once before it is allowed to succeed.
template <class Q>
static void explore_oom(const Q *q, std::deque<int> &model, int depth,
                        long &failures) {
  std::vector<int> expected(model.begin(), model.end());
  REQUIRE(contents_of(q) == expected);
  if (model.size() < TEST_LEN_LIMIT)
    failures += in_place_oom_on_clone(q, model, -depth);
  if (depth == 0) return;

  Q child;
  failures += op_under_oom(
      [&](Q *out, const Q *p) { return Api<Q>::push(out, p, depth); }, &child,
      q, expected);
  model.push_back(depth);
  explore_oom(&child, model, depth - 1, failures);
  model.pop_back();
  Api<Q>::destroy(&child);

  if (!model.empty()) {
    failures += op_under_oom(do_pop<Q>, &child, q, expected);
    int x = model.front();
    model.pop_front();
    explore_oom(&child, model, depth - 1, failures);
    model.push_front(x);
    Api<Q>::destroy(&child);
  }
}

TEMPLATE_TEST_CASE("out of memory in any allocation of short histories",
                   "[limits]", fqueue, fqueue_sync) {
  long start = live;
  TestType q;
  Api<TestType>::init(&q);
  std::deque<int> model;
  long failures = 0;
  explore_oom(&q, model, 10, failures);
  Api<TestType>::destroy(&q);
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
      failures +=
          op_under_oom(do_pop<fqueue>, &out, versions.back().get(), expected);
      fqueue_destroy(&out);
      versions.push_back(*versions.back().pop());
      model.pop_front();
    };
    while (model.size() + 2 <= TEST_LEN_LIMIT) {
      push();
      push();
      pop();
    }
    while (!model.empty()) pop();
    REQUIRE(failures > 100);
  }
  REQUIRE(live == start);
}

TEST_CASE("out of memory in any allocation of in-place operations on a version "
          "owned alone",
          "[limits]") {
  // Push two, pop one up to the length limit, then drain, all in place: no
  // other version shares the nodes.
  long start = live;
  {
    fqueue q;
    fqueue_init(&q);
    std::deque<int> model;
    long failures = 0;
    int k = 0;
    auto push = [&] {
      std::vector<int> expected(model.begin(), model.end());
      failures += in_place_under_oom(
          [&](fqueue *p) { return fqueue_push_in_place(p, k); }, &q, expected);
      model.push_back(k++);
    };
    auto pop = [&] {
      std::vector<int> expected(model.begin(), model.end());
      failures += in_place_under_oom(do_pop_in_place<fqueue>, &q, expected);
      model.pop_front();
    };
    while (model.size() + 2 <= TEST_LEN_LIMIT) {
      push();
      push();
      pop();
    }
    while (!model.empty()) pop();
    REQUIRE(contents(&q).empty());
    REQUIRE(failures > 50);
    fqueue_destroy(&q);
  }
  REQUIRE(live == start);
}

TEST_CASE("pushing past the length limit reports FQUEUE_OVERFLOW", "[limits]") {
  long start = live;
  {
    Q q;
    for (int i = 0; i < TEST_LEN_LIMIT; ++i) q = q.push(i);
    std::vector<int> expected = contents(q);

    fqueue out;
    REQUIRE(fqueue_push(&out, q.get(), -1) == FQUEUE_OVERFLOW);
    REQUIRE(contents(q) == expected);
    fqueue c;
    fqueue_clone(&c, q.get());
    REQUIRE(fqueue_push_in_place(&c, -1) == FQUEUE_OVERFLOW);
    REQUIRE(contents(&c) == expected);
    fqueue_destroy(&c);

    Q popped = *q.pop(); // there is room again
    REQUIRE(popped.push(-1).size() == TEST_LEN_LIMIT);
  }
  REQUIRE(live == start);
}

TEST_CASE("random operations on many shared versions with saturated counts",
          "[limits]") {
  // Versions are cloned, pushed, popped and destroyed at random while counts
  // saturate again and again; every result is checked against a model, and
  // the sanitizers check that no node is freed while referenced.
  long start = live;
  std::set<void *> blocks;
  track(blocks);
  {
    std::vector<fqueue> versions(1);
    fqueue_init(&versions.front());
    std::vector<std::deque<int>> models{{}};
    uint64_t x = 0x2545F4914F6CDD1Du;
    for (int k = 0; k < 20000; ++k) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      // Half of the operations hit the first few versions, so that the same
      // nodes are shared over and over.
      size_t n =
          (x & 1) ? std::min<size_t>(versions.size(), 4) : versions.size();
      size_t i = (x >> 8) % n;
      std::deque<int> model = models[i];
      fqueue out;
      fqueue_status s;
      if ((x >> 20) % 5 == 0) {
        // In place: versions[j] itself changes. j is drawn from the whole
        // history, so that the hot first versions keep their nodes.
        size_t j = (x >> 24) % versions.size();
        int v = -1;
        bool pop = (x >> 40) & 1;
        s = pop ? fqueue_pop_in_place(&versions[j], &v)
                : fqueue_push_in_place(&versions[j], k);
        if (s == FQUEUE_OVERFLOW) {
          REQUIRE((!pop && models[j].size() == TEST_LEN_LIMIT));
        } else if (s == FQUEUE_EMPTY) {
          REQUIRE(models[j].empty());
        } else {
          REQUIRE(s == FQUEUE_OK);
          if (pop) {
            REQUIRE(v == models[j].front());
            models[j].pop_front();
          } else {
            models[j].push_back(k);
          }
        }
        REQUIRE(fqueue_size(&versions[j]) == models[j].size());
        if (!models[j].empty())
          REQUIRE(front_of(&versions[j]) == models[j].front());
        continue;
      }
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
        fqueue_clone(&out, &versions[i]);
        s = FQUEUE_OK;
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
        REQUIRE(models[i].size() == TEST_LEN_LIMIT);
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
    for (fqueue &v : versions) fqueue_destroy(&v);
  }
  REQUIRE(free_tracked() > 0); // counts did saturate
  REQUIRE(live == start);
}

TEST_CASE("an iterator step that runs out of memory can be retried",
          "[limits]") {
  long start = live;
  {
    // A long queue whose versions all stay alive, so that the iterator's pops
    // share nodes and rotate.
    std::vector<Q> versions(1);
    for (int i = 0; i < TEST_LEN_LIMIT; ++i)
      versions.push_back(i % 3 == 2 ? *versions.back().pop()
                                    : versions.back().push(i));
    const Q &q = versions.back();
    std::vector<int> expected = contents(q);

    fqueue_iter it;
    fqueue_iter_init(&it, q.get());
    std::vector<int> got;
    long failures = 0;
    for (;;) {
      int v = -1;
      long before = live;
      arm(failures % 3); // fail the first, second or third allocation
      fqueue_status s = fqueue_iter_next(&it, &v);
      disarm();
      if (injected) {
        REQUIRE(s == FQUEUE_NOMEM);
        REQUIRE(v == -1);
        REQUIRE(live == before);
        ++failures;
        continue; // retry the same step
      }
      if (s == FQUEUE_EMPTY) break;
      REQUIRE(s == FQUEUE_OK);
      got.push_back(v);
    }
    fqueue_iter_destroy(&it);
    REQUIRE(got == expected);
    REQUIRE(failures > 0);
  }
  REQUIRE(live == start);
}

TEMPLATE_TEST_CASE("a saturated reference count pins its node in both "
                   "instantiations",
                   "[limits]", fqueue, fqueue_sync) {
  using A = Api<TestType>;
  long start = live;
  std::set<void *> blocks;
  track(blocks);
  long nodes;
  {
    // A version in the middle of a rotation, so that its rotation's nodes are
    // shared too.
    TestType q;
    A::init(&q);
    for (int i = 0; i < 9; ++i) REQUIRE(A::push_in_place(&q, i) == FQUEUE_OK);
    REQUIRE(A::pop_in_place(&q, nullptr) == FQUEUE_OK);
    std::vector<int> expected = contents_of(&q);
    nodes = live - start;

    // Far more clones than the limit: none fails, nothing is allocated.
    std::vector<TestType> clones(3 * TEST_REFS_LIMIT);
    for (TestType &c : clones) A::clone(&c, &q);
    REQUIRE(live - start == nodes);

    // Every operation still works, persistent and in place.
    TestType out;
    REQUIRE(A::push(&out, &q, -1) == FQUEUE_OK);
    std::vector<int> pushed = expected;
    pushed.push_back(-1);
    REQUIRE(contents_of(&out) == pushed);
    A::destroy(&out);
    REQUIRE(A::pop(&out, &q) == FQUEUE_OK);
    REQUIRE(contents_of(&out) ==
            std::vector<int>(expected.begin() + 1, expected.end()));
    A::destroy(&out);
    REQUIRE(A::push_in_place(&clones.back(), -1) == FQUEUE_OK);
    REQUIRE(contents_of(&clones.back()) == pushed);
    int v = -1;
    REQUIRE(A::pop_in_place(&clones.front(), &v) == FQUEUE_OK);
    REQUIRE(v == expected.front());

    // Destroying the clones leaves q intact.
    for (TestType &c : clones) A::destroy(&c);
    REQUIRE(contents_of(&q) == expected);
    A::destroy(&q);
  }
  // The saturated nodes stay, and only nodes of q: nothing else leaks.
  long leaked = live - start;
  REQUIRE(leaked > 0);
  REQUIRE(leaked <= nodes);
  REQUIRE(free_tracked() == leaked);
  REQUIRE(live == start);
}

TEMPLATE_TEST_CASE("a pool slab that cannot be allocated is FQUEUE_NOMEM",
                   "[limits][pool]", fqueue_pooled, fqueue_pooled_sync) {
  using A = Api<TestType>;
  auto trim = [] {
    if constexpr (std::is_same_v<TestType, fqueue_pooled>)
      return fqueue_pooled_pool_trim();
    else
      return fqueue_pooled_sync_pool_trim();
  };
  // The thread's cache (allocated once, kept) exists before counting.
  {
    TestType w;
    A::init(&w);
    REQUIRE(A::push_in_place(&w, 0) == FQUEUE_OK);
    A::destroy(&w);
  }
  trim();
  long start = live;
  // An empty pool: the first node needs a slab, and so does every
  // operation until one succeeds.
  TestType q;
  A::init(&q);
  long failures = 0;
  for (int i = 0; i < 3; ++i) {
    std::vector<int> expected(i);
    for (int k = 0; k < i; ++k) expected[k] = k;
    TestType out;
    failures += op_under_oom(
        [i](TestType *o, const TestType *p) { return A::push(o, p, i); }, &out,
        &q, expected);
    A::destroy(&q);
    q = out;
  }
  REQUIRE(failures == 1); // only the first push takes a slab
  // In place: fails before changing q.
  A::destroy(&q);
  trim();
  A::init(&q);
  arm(0);
  REQUIRE(A::push_in_place(&q, 7) == FQUEUE_NOMEM);
  disarm();
  REQUIRE(injected);
  REQUIRE(A::size(&q) == 0);
  REQUIRE(A::push_in_place(&q, 7) == FQUEUE_OK);
  REQUIRE(contents_of(&q) == std::vector<int>{7});
  A::destroy(&q);
  REQUIRE(trim() > 0);
  REQUIRE(live == start);
}

// The rotation schedule per operation is the same in C, Rust and Zig (#9):
// on one history every persistent operation creates the same number of nodes
// (it is determined by how many rotation steps of which phase it makes). The
// sequence is shared with rust/tests/schedule.rs and zig/src/tests.zig.
static const long schedule_expected[] = {
    2, 1, 3, 2, 2, 1, 3, 3, 3, 2, 2, 2, 2, 1, 3, 3, 3, 3, 3, 3, 3, 2, 2,
    2, 2, 2, 2, 2, 2, 1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1,
    2, 1, 2, 1, 2, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 3, 2, 3, 2, 3, 2, 3,
    2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
    3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

TEMPLATE_TEST_CASE("the rotation schedule matches Rust and Zig", "[limits]",
                   fqueue, fqueue_sync) {
  using A = Api<TestType>;
  // 30 push, then 60 times push + pop, then pop until empty.
  std::vector<long> got;
  TestType q;
  A::init(&q);
  auto op = [&](bool push, int x) {
    TestType next;
    long before = allocs;
    REQUIRE((push ? A::push(&next, &q, x) : A::pop(&next, &q)) == FQUEUE_OK);
    got.push_back(allocs - before);
    A::destroy(&q);
    q = next;
  };
  for (int i = 0; i < 30; ++i) op(true, i);
  for (int i = 30; i < 90; ++i) {
    op(true, i);
    op(false, 0);
  }
  while (A::size(&q) > 0) op(false, 0);
  A::destroy(&q);
  REQUIRE(got == std::vector<long>(std::begin(schedule_expected),
                                   std::end(schedule_expected)));
}

TEMPLATE_TEST_CASE("an in-place operation that needs a slab fails without a "
                   "change, wherever it needs it",
                   "[limits][pool]", fqueue_pooled, fqueue_pooled_sync) {
  // Every allocation fails while armed; with the pool only an operation that
  // needs a new slab allocates — for the new rear or in the reserve of the
  // rotation step's nodes (fqueue_impl_npool_reserve). Each such failure must
  // be FQUEUE_NOMEM and leave the queue as it was; disarmed, it succeeds.
  using A = Api<TestType>;
  auto trim = [] {
    if constexpr (std::is_same_v<TestType, fqueue_pooled>)
      return fqueue_pooled_pool_trim();
    else
      return fqueue_pooled_sync_pool_trim();
  };
  trim();
  long start = live;
  TestType q;
  A::init(&q);
  std::deque<int> model;
  std::vector<TestType> kept; // clones: the in-place ops copy shared nodes,
                              // so the live set grows across many slabs
  long failures = 0;
  int v;
  // Below TEST_LEN_LIMIT: grow to 30, then push/pop around it, 20000 ops.
  for (int k = 0; k < 20000; ++k) {
    if (k % 2 == 0) {
      kept.emplace_back();
      A::clone(&kept.back(), &q);
    }
    bool push = model.size() < 30 && (model.size() < 20 || k % 3 != 0);
    arm(0);
    fqueue_status s = push ? A::push_in_place(&q, k) : A::pop_in_place(&q, &v);
    disarm();
    if (injected) {
      ++failures;
      REQUIRE(s == FQUEUE_NOMEM);
      REQUIRE(contents_of(&q) == as_vector(model));
      s = push ? A::push_in_place(&q, k) : A::pop_in_place(&q, &v);
    }
    REQUIRE(s == FQUEUE_OK);
    if (push)
      model.push_back(k);
    else {
      REQUIRE(v == model.front());
      model.pop_front();
    }
  }
  A::destroy(&q);
  for (TestType &c : kept) A::destroy(&c);
  REQUIRE(failures > 0); // 8 here, in the reserve of the step's nodes
  trim();
  REQUIRE(live == start);
}
