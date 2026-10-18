// SPDX-License-Identifier: Apache-2.0
// Object snaps and grips (geom.js snapPoints / grips / gripMove ports); expected values from Node on the frozen 0.5
// geom.js.
#include "cc/cad/snap.hpp"

#include <cmath>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/value.hpp"

using cc::base::make_object;
using cc::base::Value;

namespace {
cc::cad::SnapKinds kinds() {
  return {"end", "mid", "cen", "quad", "int", "perp", "node", "ins"};
}
}  // namespace

TEST_CASE("snap points of a line and an arc, with and without a base point") {
  const Value line = make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 10}, {"y2", 0}});
  // snapPoints(line, {9.5, 0.3}, 1, kinds, null, {3, 5}) → [{end 10, 0}]
  const auto a = cc::cad::snap_points(line, {9.5, 0.3}, 1, kinds(), nullptr, cc::cad::P2{3, 5});
  REQUIRE(a.size() == 1);
  CHECK(a[0].kind == "end");
  CHECK(a[0].x == 10);
  const Value arc = make_object({{"type", "arc"}, {"cx", 0}, {"cy", 0}, {"r", 5}, {"a0", 0}, {"a1", std::numbers::pi}});
  // → [{mid 3.061616997868383e-16, 5}, {quad 3.061616997868383e-16, 5}]
  const auto b = cc::cad::snap_points(arc, {0.2, 4.9}, 1, kinds(), nullptr, std::nullopt);
  REQUIRE(b.size() == 2);
  CHECK(b[0].kind == "mid");
  CHECK(b[0].x == 3.061616997868383e-16);
  CHECK(b[1].kind == "quad");
  // tangent from (10, 0): → [{tan 2.4999999999999996, 4.330127018922194}]
  const auto c = cc::cad::snap_points(arc, {3, 4}, 10, {"tan"}, nullptr, cc::cad::P2{10, 0});
  REQUIRE(c.size() == 1);
  CHECK(c[0].x == 2.4999999999999996);
  CHECK(c[0].y == 4.330127018922194);
}

TEST_CASE("the best snap prefers an end to a midpoint, and finds intersections") {
  const Value a = make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 10}, {"y2", 10}});
  const Value b = make_object({{"type", "line"}, {"x1", 0}, {"y1", 10}, {"x2", 10}, {"y2", 0}});
  const std::vector<const Value*> near = {&a, &b};
  const auto s = cc::cad::best_snap(near, {5.2, 5.1}, 1, kinds(), nullptr, std::nullopt);
  REQUIRE(s.has_value());
  if (s) {
    CHECK(s->kind == "int");  // the midpoints are there too: an intersection ranks first
    CHECK(std::abs(s->x - 5) < 1e-12);
  }
}

TEST_CASE("grips and moving one") {
  const Value circle = make_object({{"type", "circle"}, {"cx", 1}, {"cy", 2}, {"r", 3}});
  const auto g = cc::cad::grips(circle);
  REQUIRE(g.size() == 5);
  CHECK(g[0].key == "c");
  CHECK(g[2].x == 1.0000000000000002);  // Node: grips(circle)[2] = {1.0000000000000002, 5, 'r'}
  const Value arc = make_object({{"type", "arc"}, {"cx", 0}, {"cy", 0}, {"r", 5}, {"a0", 0}, {"a1", std::numbers::pi}});
  // gripMove(arc, 'a1', {-3, -4}).a1 = 4.068887871591405
  const Value moved = cc::cad::grip_move(arc, "a1", {-3, -4});
  CHECK(moved.get("a1").as_number() == 4.068887871591405);
}
