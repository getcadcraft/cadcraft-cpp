// SPDX-License-Identifier: Apache-2.0
#include "cc/drawing/compile.hpp"

#include <cmath>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/value.hpp"

using cc::base::Array;
using cc::base::make_object;
using cc::base::pt2;
using cc::base::pt3;
using cc::base::Value;

namespace {
Value model_with(Array model_space, Array blocks = {}) {
  blocks.push_back(make_object(
      {{"name", "*Model_Space"}, {"record", "1F"}, {"base", pt3(0, 0, 0)}, {"entities", std::move(model_space)}}));
  return make_object({{"tables", make_object({{"layers", Array{make_object({{"name", "Walls"},
                                                                            {"color", make_object({{"index", 1}})},
                                                                            {"lineweight", 50}})}}})},
                      {"blocks", std::move(blocks)},
                      {"layouts", Array{}},
                      {"objects", Array{}}});
}
}  // namespace

TEST_CASE("a line lands in its layer's batch with the layer's colour and weight") {
  const Value m = model_with(
      {make_object({{"type", "LINE"}, {"layer", "Walls"}, {"start", pt3(0, 0, 0)}, {"end", pt3(10, 5, 0)}})});
  const auto d = cc::drawing::compile_drawing(m);
  REQUIRE(d.spaces.size() == 1);
  const auto& s = d.spaces[0];
  REQUIRE(s.batches.size() == 1);
  CHECK(s.batches[0].layer == "Walls");
  CHECK(s.batches[0].color == "#ff0000");
  CHECK(s.batches[0].lw == 0.5);
  REQUIRE(s.batches[0].lines.size() == 1);
  CHECK(s.batches[0].lines[0] == std::vector<double>{0, 0, 10, 5});
  CHECK(s.bounds.max_x == 10);
  CHECK(d.stats.prims == 1);
}

TEST_CASE("an insert places its block with the insert's transform; layer 0 inherits") {
  Array blk{make_object({{"type", "LINE"}, {"layer", "0"}, {"start", pt3(0, 0, 0)}, {"end", pt3(1, 0, 0)}})};
  Array blocks{make_object({{"name", "B"}, {"record", "2A"}, {"base", pt3(0, 0, 0)}, {"entities", std::move(blk)}})};
  const Value ins = make_object({{"type", "INSERT"},
                                 {"layer", "Walls"},
                                 {"block", "B"},
                                 {"insert", pt3(5, 5, 0)},
                                 {"scale", pt3(2, 2, 1)},
                                 {"rotation", 0}});
  const auto d = cc::drawing::compile_drawing(model_with({ins}, std::move(blocks)));
  REQUIRE(d.spaces[0].batches.size() == 1);
  CHECK(d.spaces[0].batches[0].layer == "Walls");
  CHECK(d.spaces[0].batches[0].lines[0] == std::vector<double>{5, 5, 7, 5});
}

TEST_CASE("unsupported types are counted, a missing block is reported") {
  const Value m = model_with({make_object({{"type", "UNKNOWN"}, {"dxfName", "WIDGET"}}),
                              make_object({{"type", "INSERT"}, {"block", "nope"}, {"insert", pt2(0, 0)}})});
  const auto d = cc::drawing::compile_drawing(m);
  REQUIRE(d.stats.unsupported.size() == 2);
  CHECK(d.stats.unsupported[0].first == "WIDGET");
  CHECK(d.stats.unsupported[1].first == "INSERT (missing block)");
}

TEST_CASE("ACI colours as the JS computes them") {
  CHECK(cc::drawing::aci_color(7) == "ink");
  CHECK(cc::drawing::aci_color(1) == "#ff0000");
  CHECK(cc::drawing::aci_color(30) == "#ff8000");
  CHECK(cc::drawing::aci_color(41) == "#ffdf80");
  CHECK(cc::drawing::aci_color(129) == "#264d43");
  CHECK(cc::drawing::aci_color(251) == "#505050");
}

TEST_CASE(
    "a spline whose degree its control points cannot carry draws its control polygon (fuzz regress-34b55eb339a1)") {
  Array knots;
  for (int i = 0; i < 8; ++i) {
    knots.emplace_back(i);
  }
  const Value s = make_object(
      {{"type", "SPLINE"}, {"degree", 5}, {"knots", std::move(knots)}, {"control", Array{pt3(0, 0, 0), pt3(1, 1, 0)}}});
  const auto d = cc::drawing::compile_drawing(model_with({s}));
  REQUIRE(d.spaces[0].batches.size() == 1);
  CHECK(d.spaces[0].batches[0].lines[0] == std::vector<double>{0, 0, 1, 1});
}

TEST_CASE("a runaway MINSERT stops at the caps and is marked truncated (fuzz regress-bbdfe993a32f)") {
  Array blk{make_object({{"type", "CIRCLE"}, {"center", pt3(0, 0, 0)}, {"radius", 1}})};
  Array blocks{make_object({{"name", "B"}, {"record", "2A"}, {"base", pt3(0, 0, 0)}, {"entities", std::move(blk)}})};
  const Value ins = make_object({{"type", "MINSERT"},
                                 {"block", "B"},
                                 {"insert", pt3(0, 0, 0)},
                                 {"cols", 1e9},
                                 {"rows", 1e9},
                                 {"colSpacing", 3},
                                 {"rowSpacing", 3}});
  cc::drawing::CompileOptions opt;
  opt.max_coords = 100'000;
  const auto d = cc::drawing::compile_drawing(model_with({ins}, std::move(blocks)), opt);
  CHECK(d.stats.truncated);
  CHECK(d.stats.prims < 1000);
}
