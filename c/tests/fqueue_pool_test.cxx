// The node pool of fqueue_pooled and fqueue_pooled_sync (docs/pool.md): its
// statistics, trim, thread caches. The queue semantics of the pooled
// instantiations are checked with the others (fqueue_sync_test.cxx). Every
// test leaves no live node behind: the pools are global.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

#include "fqueue_api.hpp"

template <class Q> struct Pool;
template <> struct Pool<fqueue_pooled> {
  static fqueue_pool_stats stats() {
    fqueue_pool_stats s;
    fqueue_pooled_pool_stats(&s);
    return s;
  }
  static size_t trim() { return fqueue_pooled_pool_trim(); }
};
template <> struct Pool<fqueue_pooled_sync> {
  static fqueue_pool_stats stats() {
    fqueue_pool_stats s;
    fqueue_pooled_sync_pool_stats(&s);
    return s;
  }
  static size_t trim() { return fqueue_pooled_sync_pool_trim(); }
};

// Slab size, and nodes per slab of 16-byte nodes: the first one is the header.
static constexpr size_t SLAB = 16384;
static constexpr size_t PER_SLAB = SLAB / 16 - 1;

template <class Q> static Q filled(int n) {
  Q q;
  Api<Q>::init(&q);
  for (int i = 0; i < n; ++i)
    REQUIRE(Api<Q>::push_in_place(&q, i) == FQUEUE_OK);
  return q;
}

// The same for other threads (Catch2 assertions are not thread-safe): build,
// check the length, destroy; false on any failure.
template <class Q> static bool fill_and_destroy(int n) {
  Q q;
  Api<Q>::init(&q);
  bool ok = true;
  for (int i = 0; i < n; ++i) ok &= Api<Q>::push_in_place(&q, i) == FQUEUE_OK;
  ok &= Api<Q>::size(&q) == (size_t)n;
  Api<Q>::destroy(&q);
  return ok;
}

TEMPLATE_TEST_CASE("the pool counts live nodes and trims when none is left",
                   "[pool]", fqueue_pooled, fqueue_pooled_sync) {
  using A = Api<TestType>;
  using P = Pool<TestType>;
  P::trim();
  REQUIRE(P::stats().slabs == 0);
  uint64_t frees_before = P::stats().slab_frees;

  TestType q = filled<TestType>(20000);
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.slabs > 0);
  REQUIRE(s.slab_bytes == s.slabs * SLAB);
  REQUIRE(s.capacity == s.slabs * PER_SLAB);
  REQUIRE(s.live + s.free == s.capacity);
  // A queue of n elements holds at least n nodes (more during a rotation).
  REQUIRE(s.live >= 20000);
  REQUIRE(s.live <= size_t{3} * 20000);
  REQUIRE(P::trim() == 0); // q is alive

  TestType c;
  A::clone(&c, &q);
  REQUIRE(P::stats().live == s.live); // clone shares every node
  A::destroy(&q);
  REQUIRE(P::stats().live == s.live);
  A::destroy(&c);
  s = P::stats();
  REQUIRE(s.live == 0);
  REQUIRE(s.slab_frees == frees_before);
  size_t bytes = s.slab_bytes, slabs = s.slabs;
  REQUIRE(P::trim() == bytes);
  s = P::stats();
  REQUIRE(s.slabs == 0);
  REQUIRE(s.capacity == 0);
  REQUIRE(s.slab_frees == frees_before + slabs);

  // The pool works again after a trim.
  TestType r = filled<TestType>(100);
  REQUIRE(contents_of(&r).size() == 100);
  A::destroy(&r);
  REQUIRE(P::stats().live == 0);
}

TEMPLATE_TEST_CASE("memory is kept at the peak of live nodes until trim",
                   "[pool]", fqueue_pooled, fqueue_pooled_sync) {
  using A = Api<TestType>;
  using P = Pool<TestType>;
  P::trim();
  TestType q = filled<TestType>(100000);
  size_t peak = P::stats().slabs;
  for (int i = 0; i < 99000; ++i)
    REQUIRE(A::pop_in_place(&q, nullptr) == FQUEUE_OK);
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.slabs == peak); // nothing returned automatically
  REQUIRE(s.live < size_t{3} * 1000);
  // Growing again reuses the free nodes: no new slab.
  for (int i = 0; i < 90000; ++i) REQUIRE(A::push_in_place(&q, i) == FQUEUE_OK);
  REQUIRE(P::stats().slabs == peak);
  A::destroy(&q);
  REQUIRE(P::trim() == peak * SLAB);
}

TEMPLATE_TEST_CASE("nodes freed by another thread return to their owner",
                   "[pool]", fqueue_pooled, fqueue_pooled_sync) {
  using A = Api<TestType>;
  using P = Pool<TestType>;
  P::trim();
  fqueue_pool_stats before = P::stats();
  TestType q = filled<TestType>(10000);
  size_t slabs = P::stats().slabs;
  std::thread([&q] { Api<TestType>::destroy(&q); }).join();
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.live == 0); // on this thread's remote stack
  // This thread takes them back for a new queue: no new slab.
  TestType r = filled<TestType>(10000);
  s = P::stats();
  REQUIRE(s.collects > before.collects);
  REQUIRE(s.slabs == slabs);
  A::destroy(&r);
  REQUIRE(P::trim() == slabs * SLAB);
}

TEMPLATE_TEST_CASE("the cache of an exited thread is adopted", "[pool]",
                   fqueue_pooled, fqueue_pooled_sync) {
  using P = Pool<TestType>;
  P::trim();
  fqueue_pool_stats before = P::stats();
  bool ok1 = false, ok2 = false;
  std::thread([&ok1] { ok1 = fill_and_destroy<TestType>(10000); }).join();
  size_t slabs = P::stats().slabs;
  // The next thread adopts the first one's cache with its free nodes.
  std::thread([&ok2] { ok2 = fill_and_destroy<TestType>(10000); }).join();
  REQUIRE(ok1);
  REQUIRE(ok2);
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.adoptions > before.adoptions);
  REQUIRE(s.slabs == slabs);
  REQUIRE(s.live == 0);
  REQUIRE(P::trim() == slabs * SLAB);
}

TEMPLATE_TEST_CASE("an exiting thread returns its cache and its slab", "[pool]",
                   fqueue_pooled, fqueue_pooled_sync) {
  using P = Pool<TestType>;
  P::trim();
  // Short queues: the nodes stay in the thread's cache, the slab is barely
  // carved; after the thread exits they still count as free. The threads
  // hold a queue each until all four have one, so that they overlap and
  // each carves its own slab (otherwise, on few cores, a thread may exit
  // and hand its slab to the next one before it starts).
  std::vector<std::thread> threads;
  threads.reserve(4);
  std::vector<int> ok(4, 1);
  std::latch all_live(4);
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&ok, &all_live, t] {
      using A = Api<TestType>;
      TestType q;
      A::init(&q);
      for (int i = 0; i < 50; ++i)
        ok[t] &= A::push_in_place(&q, i) == FQUEUE_OK;
      all_live.arrive_and_wait();
      A::destroy(&q);
      for (int k = 0; k < 100; ++k) ok[t] &= fill_and_destroy<TestType>(50);
    });
  for (std::thread &th : threads) th.join();
  for (int v : ok) REQUIRE(v);
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.slabs >= 4); // a slab per thread
  REQUIRE(s.live == 0);
  REQUIRE(P::trim() == s.slabs * SLAB);
}

TEST_CASE("readers dropping the last references return the nodes", "[pool]") {
  using A = Api<fqueue_pooled_sync>;
  using P = Pool<fqueue_pooled_sync>;
  P::trim();
  // Writer allocates, readers drop the last references: the nodes go back to
  // the writer's cache in batches (the readers exit, pushing what is left).
  enum { N = 30000, READERS = 4 };
  fqueue_pooled_sync base = filled<fqueue_pooled_sync>(N);
  std::vector<fqueue_pooled_sync> clones(READERS);
  for (auto &c : clones) A::clone(&c, &base);
  A::destroy(&base);
  std::vector<std::thread> threads;
  threads.reserve(clones.size());
  for (auto &c : clones)
    threads.emplace_back([&c] { Api<fqueue_pooled_sync>::destroy(&c); });
  for (std::thread &th : threads) th.join();
  fqueue_pool_stats s = P::stats();
  REQUIRE(s.live == 0);
  REQUIRE(P::trim() == s.slabs * SLAB);
}
