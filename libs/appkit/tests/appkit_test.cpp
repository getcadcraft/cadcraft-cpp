// SPDX-License-Identifier: Apache-2.0
// lasso-core.js and the JS helpers: the cases of the 0.5 test (test-civilcraft-points.mjs, lasso) and JS semantics.
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/appkit/jsops.hpp"
#include "cc/appkit/lasso.hpp"

using namespace cc::appkit;

TEST_CASE("Math.round and String(n)") {
  CHECK(js_round(2.5) == 3);
  CHECK(js_round(-2.5) == -2);
  CHECK(std::signbit(js_round(-0.4)));
  CHECK(js_round(0.49999999999999994) == 0);
  CHECK(num_str(1e21) == "1e+21");
  CHECK(num_str(0.1 + 0.2) == "0.30000000000000004");
}

TEST_CASE("Array#sort is stable and survives NaN") {
  std::vector<std::pair<double, int>> v{{2, 0}, {1, 1}, {2, 2}, {std::nan(""), 3}, {0, 4}, {1, 5}};
  js_sort(v, [](const auto& a, const auto& b) { return a.first - b.first; });
  REQUIRE(v.size() == 6);
  std::vector<int> big(300);
  for (int i = 0; i < 300; ++i) {
    big[static_cast<std::size_t>(i)] = (i * 7919) % 300;
  }
  js_sort(big, [](int a, int b) { return static_cast<double>(a - b); });
  for (std::size_t i = 0; i < big.size(); ++i) {
    CHECK(std::cmp_equal(big[i], i));
  }
}

TEST_CASE("lasso modes, windows, crossings and fences") {
  CHECK(lasso_mode({0, 0}, {10, 1}) == "window");
  CHECK(lasso_mode({0, 0}, {-10, 1}) == "crossing");
  CHECK(lasso_mode({0, 0}, {3, 1}).empty());
  CHECK(next_mode("window") == "crossing");
  CHECK(next_mode("fence") == "window");
  const std::vector<LPt> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  CHECK(point_in_poly({5, 5}, square));
  CHECK_FALSE(point_in_poly({15, 5}, square));
  CHECK(segs_cross(0, 0, 10, 10, 0, 10, 10, 0));
  CHECK(segs_cross(0, 0, 10, 0, 5, 0, 15, 0));  // collinear overlap
  CHECK_FALSE(segs_cross(0, 0, 1, 0, 2, 0, 3, 0));
  auto pt = [](double x, double y) {
    LassoItem it;
    it.box = LBox{x - 0.1, y - 0.1, x + 0.1, y + 0.1};
    it.segs = {{x - 0.1, y, x + 0.1, y}, {x, y - 0.1, x, y + 0.1}};
    return it;
  };
  const std::vector<LassoItem> items{pt(5, 5), pt(10, 5), pt(20, 20)};
  CHECK(lasso_select(items, square, "window") == std::vector<std::size_t>{0});
  CHECK(lasso_select(items, square, "crossing") == std::vector<std::size_t>{0, 1});
  CHECK(lasso_select(items, {{10, 0}, {10, 10}}, "fence") == std::vector<std::size_t>{1});
  CHECK(lasso_select(items, {{0, 0}}, "crossing").empty());
  CHECK(thin({{0, 0}, {0.1, 0}, {5, 0}, {5.05, 0}}, 1).size() == 3);
}
