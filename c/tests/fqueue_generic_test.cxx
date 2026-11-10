// Instantiations of fqueue_template.h other than int (tests/fqueue_generic.*):
// a struct element and a pointer element, two queues in one translation unit.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <deque>
#include <vector>

#include "fqueue_generic.h"

static bool operator==(point a, point b) { return a.x == b.x && a.y == b.y; }

TEST_CASE("pointq: struct elements in a persistent history", "[generic]") {
  // Push two, pop one, keeping every version alive; then check each version
  // against its model through the iterator.
  std::vector<pointq>             versions(1);
  std::vector<std::deque<point>> models{{}};
  pointq_init(&versions[0]);
  for (int i = 0; i < 300; ++i) {
    pointq next;
    std::deque<point> model = models.back();
    if (i % 3 == 2) {
      point p;
      REQUIRE(pointq_front(&versions.back(), &p));
      REQUIRE(p == model.front());
      REQUIRE(pointq_pop(&next, &versions.back()) == FQUEUE_OK);
      model.pop_front();
    } else {
      point p = {i, -i};
      REQUIRE(pointq_push(&next, &versions.back(), p) == FQUEUE_OK);
      model.push_back(p);
    }
    REQUIRE(pointq_size(&next) == model.size());
    versions.push_back(next);
    models.push_back(model);
  }
  for (size_t k = 0; k < versions.size(); ++k) {
    std::vector<point> got;
    pointq_iter it;
    REQUIRE(pointq_iter_init(&it, &versions[k]) == FQUEUE_OK);
    point p;
    while (pointq_iter_next(&it, &p) == FQUEUE_OK)
      got.push_back(p);
    pointq_iter_destroy(&it);
    REQUIRE(got == std::vector<point>(models[k].begin(), models[k].end()));
  }
  for (pointq &q : versions)
    pointq_destroy(&q);
}

TEST_CASE("strq: pointer elements are stored, not copied deeply",
          "[generic]") {
  static const char *words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
  strq a, b;
  strq_init(&a);
  for (const char *w : words) {
    REQUIRE(strq_push(&b, &a, w) == FQUEUE_OK);
    strq_destroy(&a);
    a = b; // move
  }
  REQUIRE(strq_size(&a) == 5);
  for (const char *w : words) {
    const char *got;
    REQUIRE(strq_front(&a, &got));
    REQUIRE(got == w); // the same pointer
    REQUIRE(strq_pop(&b, &a) == FQUEUE_OK);
    strq_destroy(&a);
    a = b;
  }
  REQUIRE(strq_is_empty(&a));
  strq_destroy(&a);
}
