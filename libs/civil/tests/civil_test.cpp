// SPDX-License-Identifier: Apache-2.0
// CivilCraft's engine: the cases of the 0.5 test (test-civilcraft.mjs: units and stations, point files, the TIN) and
// the golden call scripts (tests/golden/js-0.5-civil.tar.gz: what the frozen 0.5 modules gave for the same calls).
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/civil/deeds.hpp"
#include "cc/civil/design.hpp"
#include "cc/civil/points.hpp"
#include "cc/civil/predicates.hpp"
#include "cc/civil/project.hpp"
#include "cc/civil/script.hpp"
#include "cc/civil/tin.hpp"
#include "cc/civil/units.hpp"
#include "cc/i18n/i18n.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using namespace cc::civil;
using cc::base::Value;

namespace {

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog kWords = [] {
    const auto text = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/civilcraft/en.json", std::uint64_t{1} << 26U);
    REQUIRE(text.has_value());
    auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*text));
    REQUIRE(c.has_value());
    return std::move(*c);
  }();
  return kWords;
}

Value read_json(const fs::path& p) {
  const auto text = cc::base::read_file(p, std::uint64_t{1} << 30U);
  REQUIRE(text.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*text));
  REQUIRE(v.has_value());
  return std::move(*v);
}

}  // namespace

TEST_CASE("units and stations") {
  CHECK(format_station(1234.56, "ft") == "12+34.56");
  CHECK(format_station(0, "ft") == "0+00.00");
  CHECK(format_station(-50, "ft") == "-0+50.00");
  CHECK(format_station(99.999, "ft") == "1+00.00");
  CHECK(format_station(1234.5, "m") == "1+234.500");
  CHECK(format_station(1200, "ft", 0) == "12+00");
  CHECK(parse_station("12+34.56", "ft") == 1234.56);
  CHECK(parse_station("1+234.5", "m") == 1234.5);
  CHECK(parse_station("-0+50", "ft") == -50);
  CHECK(parse_station("1234.56", "ft") == 1234.56);
  CHECK_FALSE(parse_station("abc", "ft").has_value());
  CHECK(grouped(1234567.891, 1) == "1,234,567.9");
  CHECK(grouped(1.005, 2) == "1.01");
  CHECK(bearing(std::atan(1)) == "N 45°00'00\" E");
  CHECK(pct(0.02) == "+2.00%");
  CHECK(volume_text(2700, "ft") == "100.0 cu yd");
}

TEST_CASE("point files") {
  const auto r =
      parse_points("1,5000.0,2000.0,100.5,GRD shot\n2,5010,2010,101,EP\n# c\n3,x,1,1,BAD\n", "auto", "PNEZD", words());
  REQUIRE(r.points.size() == 2);
  CHECK(r.format.value_or("") == "PNEZD");
  CHECK(r.points[0].x == 2000.0);
  CHECK(r.points[0].y == 5000.0);
  CHECK(r.points[0].d == "GRD shot");
  CHECK(r.bad_count == 1);
  CHECK(r.bad[0].line == 4);
  CHECK(group_key("  ep1 b") == "EP1");
  const auto h = parse_points("Point,Northing,Easting,Elevation,Description\n7,1,2,3,a\n", "auto", "PNEZD", words());
  CHECK(h.format_from == "header");
  CHECK(h.points.size() == 1);
}

TEST_CASE("TIN: planes are exact, breaklines are forced in") {
  BuildInput in;
  for (int i = 0; i <= 10; ++i) {
    for (int j = 0; j <= 10; ++j) {
      const double x = 500000 + i * 10.0;
      const double y = 4000000 + j * 10.0;
      in.points.push_back({x, y, 100 + 0.02 * (x - 500000) + 0.01 * (y - 4000000)});
    }
  }
  const Built b = build_surface(in, words());
  REQUIRE(b.surface);
  CHECK(b.surface->triangle_count() == 200);
  const auto z = b.surface->elevation_at(500033.3, 4000071.7);
  CHECK(std::abs(z.value_or(0) - (100 + 0.02 * 33.3 + 0.01 * 71.7)) < 1e-9);
  CHECK_FALSE(b.surface->elevation_at(499000, 4000000));
  const auto c = b.surface->contours(1);
  CHECK(!c.value_or(std::vector<Contour>{}).empty());
  BuildInput line;
  line.points = {{0, 0, 1.0}, {1, 1, 1.0}, {2, 2, 1.0}};
  CHECK_FALSE(build_surface(line, words()).surface);
  CHECK(orient2d(0, 0, 1, 0, 0, 1) < 0);
  CHECK(incircle(0, 0, 1, 0, 0, 1, 0.5, 0.5) > 0);
  CHECK(incircle(0, 0, 1, 0, 0, 1, 1, 1) == 0);
}

TEST_CASE("golden: call scripts give what CadCraft 0.5's CivilCraft modules gave") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "civil";
  if (!fs::exists(golden)) {
    SKIP("references missing (tests/golden/js-0.5-civil.tar.gz)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    if (entry.path().extension() != ".json" || entry.path().string().ends_with(".out.json")) {
      continue;
    }
    const Value script = read_json(entry.path());
    fs::path want_path = entry.path();
    want_path.replace_extension(".out.json");
    const Value want = read_json(want_path);
    const Value got = run_script(script, words());
    std::vector<std::string> diffs;
    cc::test::compare(want, got, entry.path().filename().string(), diffs);
    INFO(entry.path().filename().string());
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked > 0);
}

TEST_CASE("TIN: a sliver keeps its lookup grid small (fuzz timeouts, fuzz/corpus/civil_script/regress-*)") {
  BuildInput in;
  for (int i = 0; i < 400; ++i) {
    in.points.push_back({2100000.5 + i * 3.0, 700000.25 + ((i % 2) != 0 ? 1e-3 : 0.0), 100.0 + i});
  }
  in.points.push_back({2100000.5, 7.0052728701917013e16, 5.0});  // a coordinate far out: one cell high, millions wide
  const Built b = build_surface(in, words());
  REQUIRE(b.surface);
  CHECK(b.surface->stats().triangles > 0);
  (void)b.surface->elevation_at(2100100.5, 700000.25);
}

TEST_CASE("alignment: a zero polyline step stops at its cap (fuzz slow units, regress-slow-*)") {
  const auto al = build_alignment({{0, 0, 0}, {100, 0, 50}, {100, 100, 0}}, 0, words());
  REQUIRE(al->ok());
  const auto pl = al->polyline(0, std::numbers::pi / 360);
  CHECK(static_cast<double>(pl.size()) <= kMaxPolylinePieces + 3);
}

TEST_CASE("grading: a grid volume between two planes is exact on whole cells") {
  BuildInput a;
  BuildInput b;
  for (int i = 0; i <= 10; ++i) {
    for (int j = 0; j <= 10; ++j) {
      a.points.push_back({i * 10.0, j * 10.0, 100.0});
      b.points.push_back({i * 10.0, j * 10.0, 102.0});
    }
  }
  const Built ga = build_surface(a, words());
  const Built gb = build_surface(b, words());
  REQUIRE(ga.surface);
  REQUIRE(gb.surface);
  const GridVolume v = grid_volume(*ga.surface, *gb.surface, cc::base::make_object({{"cell", 5}}));
  CHECK(std::abs(v.fill - 2.0 * 100 * 100) < 1e-6);
  CHECK(v.cut == 0);
}

TEST_CASE("bearings, deed calls and a closed traverse") {
  const auto b = parse_bearing(Value("N 45°30'15\" E"));
  REQUIRE(b);
  CHECK(std::abs(b.value_or(0) - (90 - (45 + 30 / 60.0 + 15 / 3600.0)) * std::numbers::pi / 180) < 1e-12);
  CHECK_FALSE(parse_bearing(Value("N 91 E")));
  const Value calls =
      parse_calls(Value("thence N 0 E 100.00 feet\nS 90 E 100\nS 0 E 100\nN 90 W 100\nC R 0 L 5 left"), words());
  CHECK(calls.get("calls").as_array().size() == 4);
  CHECK(calls.get("errors").as_array().size() == 1);
  const Value run = run_calls(cc::base::pt2(0, 0), calls.get("calls"));
  CHECK(run.get("misclosure").get("dist").as_number() < 1e-9);
}

TEST_CASE("the project: a change is one undo step and the file reads back") {
  Project p(words());
  CHECK(p.change("Name", [](Value& s) { s.set("name", "Site"); }));
  CHECK_FALSE(p.change("Same", [](Value& s) { s.set("name", "Site"); }));
  CHECK(p.undo().value_or("") == "Name");
  CHECK(p.state.get("name").as_string() != "Site");
  CHECK(p.redo().value_or("") == "Name");
  const auto L = Project::from_json(p.to_json(), words());
  REQUIRE(L.project);
  CHECK(L.project->state.get("name").as_string() == "Site");
  CHECK(Project::from_json(cc::base::make_object({{"format", "nope"}}), words()).error.has_value());
}
