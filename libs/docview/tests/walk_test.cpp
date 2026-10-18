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
