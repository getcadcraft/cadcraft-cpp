// SPDX-License-Identifier: Apache-2.0
// What a drawing draws as (the walk.js port): the public fixtures opened as CadCraft opens them give the JS walk's
// events in every space (tests/golden/README.md); and the pieces walk uses — dashes, pattern hatches, colours.
#include "cc/docview/walk.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/cad/import.hpp"
#include "cc/doc/doc.hpp"
#include "cc/dwg/read.hpp"
#include "cc/i18n/i18n.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog kWords = [] {
    const auto text = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
    REQUIRE(text.has_value());
    auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*text));
    REQUIRE(c.has_value());
    return std::move(*c);
  }();
  return kWords;
}

}  // namespace

TEST_CASE("golden: the public fixtures draw as CadCraft 0.5 draws them (walk.js), every space") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "walk";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().stem().string();  // "<file>.dwg"
    fs::path src = corpus / "fixtures/dwg" / name;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / name;
    }
    REQUIRE(fs::exists(src));
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    const Value model = cc::dwg::read_dwg(*bytes);
    cc::doc::Doc d;
    d.load(cc::cad::import_dwg(model, cc::base::as_text(std::span<const std::byte>(*bytes).first(6)), words()),
           words());
    cc::base::Array lays;
    for (const cc::doc::Layout& l : d.layouts()) {
      lays.push_back(cc::docview::walk_events(d, *l.entities));
    }
    const Value got = cc::base::make_object({{"model", cc::docview::walk_events(d, *d.model())}, {"layouts", lays}});
    const auto ref = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref));
    REQUIRE(want.has_value());
    INFO(name);
    std::vector<std::string> diffs;
    cc::test::compare(*want, got, "", diffs);
    for (const auto& x : diffs) {
      UNSCOPED_INFO(x);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 4);
}

TEST_CASE("dashes follow the pattern along a run of lines and carry over from one to the next (values from Node)") {
  const auto xs = [](const std::vector<cc::cad::Prim>& d) {
    std::vector<std::pair<double, double>> out;
    out.reserve(d.size());
    for (const cc::cad::Prim& q : d) {
      out.emplace_back(q.x1, q.x2);
    }
    return out;
  };
  cc::cad::Prim a;
  a.x2 = 3;
  cc::cad::Prim b;
  b.x1 = 3;
  b.x2 = 6;
  // dashPrims([L(0→3), L(3→6)], [2, -1], 1) → [[0, 2], [3, 5]]
  CHECK(xs(cc::docview::dash_prims({a, b}, {2, -1}, 1)) == std::vector<std::pair<double, double>>{{0, 2}, {3, 5}});
  cc::cad::Prim c;
  c.x2 = 10;
  // a dot (0) is a dash one hundredth of the period long: [[0,1],[2,2.03],[3,4],[5,5.03],[6,7],[8,8.03],[9,10]]
  const auto dots = xs(cc::docview::dash_prims({c}, {1, -1, 0, -1}, 1));
  REQUIRE(dots.size() == 7);
  CHECK(dots[1].first == 2);
  CHECK(std::abs(dots[1].second - 2.03) < 1e-12);
  CHECK(dots[6] == std::pair<double, double>{9, 10});
}

TEST_CASE("a clipped reference draws only what lies inside its clip (xclip.js), texts by their insertion point") {
  cc::doc::Doc d;
  d.change("SET UP", [&] {
    (void)d.define_block(cc::base::make_object(
        {{"name", "B"},
         {"base", cc::base::make_object({{"x", 0}, {"y", 0}})},
         {"entities",
          cc::base::Array{
              cc::base::make_object({{"type", "line"}, {"layer", "0"}, {"x1", 0}, {"y1", 5}, {"x2", 20}, {"y2", 5}}),
              cc::base::make_object({{"type", "circle"}, {"layer", "0"}, {"cx", 10}, {"cy", 5}, {"r", 3}}),
              cc::base::make_object(
                  {{"type", "text"}, {"layer", "0"}, {"x", 15}, {"y", 5}, {"h", 1}, {"rot", 0}, {"s", "out"}}),
              cc::base::make_object(
                  {{"type", "text"}, {"layer", "0"}, {"x", 9}, {"y", 5}, {"h", 1}, {"rot", 0}, {"s", "in"}})}}}));
    // the clip: x from 5 to 12 in the block's coordinates; the reference moved by (100, 0)
    cc::base::Array pts;
    for (const auto& [x, y] :
         {std::pair{5.0, 0.0}, std::pair{12.0, 0.0}, std::pair{12.0, 10.0}, std::pair{5.0, 10.0}}) {
      pts.emplace_back(cc::base::make_object({{"x", x}, {"y", y}}));
    }
    (void)d.add(cc::base::make_object({{"type", "insert"},
                                       {"layer", "0"},
                                       {"block", "B"},
                                       {"x", 100},
                                       {"y", 0},
                                       {"sx", 1},
                                       {"sy", 1},
                                       {"rot", 0},
                                       {"clip", cc::base::make_object({{"pts", cc::base::Value(std::move(pts))}})}}));
  });
  std::vector<cc::cad::Prim> lines;
  std::vector<std::string> texts;
  cc::docview::Out out;
  out.stroke = [&](const std::string&, const std::vector<cc::cad::Prim>& ps, double, const cc::docview::Opts&) {
    lines.insert(lines.end(), ps.begin(), ps.end());
  };
  out.text = [&](const std::string&, const cc::base::Value& e, const cc::docview::Opts&) {
    texts.push_back(cc::base::js_string(e.get("s")));
  };
  out.fill = [](const std::string&, const std::vector<std::vector<double>>&, bool, const cc::docview::Opts&) {};
  std::vector<const cc::base::Value*> ents;
  d.model()->each([&](const cc::doc::Id&, const cc::base::Value& e) { ents.push_back(&e); });
  cc::docview::walk(d, ents, out);
  // the line is cut to x 105–112; the circle (x 7–13) keeps its arcs inside, none past x 112
  bool cut_line = false;
  for (const cc::cad::Prim& p : lines) {
    if (!p.arc) {
      cut_line =
          cut_line || (std::abs(std::min(p.x1, p.x2) - 105) < 1e-9 && std::abs(std::max(p.x1, p.x2) - 112) < 1e-9);
    } else {
      for (const double a : {p.a0, p.a1}) {
        const double x = p.cx + p.r * std::cos(a);
        CHECK(x >= 105 - 1e-6);
        CHECK(x <= 112 + 1e-6);
      }
    }
  }
  CHECK(cut_line);
  CHECK(texts == std::vector<std::string>{"in"});
}
