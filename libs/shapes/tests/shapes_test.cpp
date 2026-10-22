// SPDX-License-Identifier: Apache-2.0
// libs/shapes by hand: the checks 0.5's bin/test-shapes.mjs makes of its tables (each section's area from its
// dimensions against the published area, the published weight against that area in steel), lookups in any case, and
// the part records' rules. Parity with the JS, call for call, is tools/parity/part.mjs --only shapes (and the
// partcraft golden test).
#include <cmath>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/shapes/items.hpp"
#include "cc/shapes/parts.hpp"
#include "cc/shapes/sections.hpp"

namespace sh = cc::shapes;

TEST_CASE("sections: every row's area and weight agree with its published values") {
  const auto& rows = sh::section_rows();
  REQUIRE(rows.size() == 1335);
  std::set<std::string> names;
  for (const auto& s : rows) {
    names.insert(s.name);
    // the drawn outline (square corners where the table has none) is within 8 % of the published area
    const double a = sh::exact_area(s);
    INFO(s.name);
    REQUIRE(s.area_pub.has_value());
    REQUIRE(s.mass_pub.has_value());
    const double area = s.area_pub.value_or(0);
    const double mass = s.mass_pub.value_or(0);
    CHECK(std::abs(a - area) / area < 0.08);
    // the published weight is the published area in steel, to 7 % (C3x3.5 is 6 % off in the database itself; hollow
    // sections: AISC weighs the nominal wall and gives the area of the design wall, 0.93 of it)
    const double hollow = s.kind == "RHS" || s.kind == "CHS" ? 0.93 : 1;
    CHECK(std::abs(area / hollow * 1e-6 * sh::kSteelDensity - mass) / mass < 0.07);
    const auto loops = sh::outline(s);
    REQUIRE(!loops.empty());
    CHECK(sh::shoelace(loops[0]) > 0);  // the outer loop is counter-clockwise
    for (std::size_t i = 1; i < loops.size(); ++i) {
      CHECK(sh::shoelace(loops[i]) < 0);  // holes clockwise
    }
  }
  CHECK(names.size() == rows.size());
}

TEST_CASE("sections: names in any case, AISC names printed with a lower-case x") {
  CHECK(sh::display_name("W12X26") == "W12x26");
  CHECK(sh::display_name("HSS4X4X1/4") == "HSS4x4x1/4");
  CHECK(sh::display_name("X1X") == "X1X");
  REQUIRE(sh::find_section("  w12x26 ") != nullptr);
  CHECK(sh::find_section("w12X26")->name == "W12x26");
  CHECK(sh::find_section("IPE300")->family == "IPE");
  CHECK(sh::find_section("W12X27") == nullptr);
  CHECK(sh::family_rows("Pipe").size() == 51);
}

TEST_CASE("parts: defaults, refusals and records") {
  const cc::base::Value bolt = sh::make_part("iso4017", "M8", cc::base::Value::object());
  REQUIRE(bolt.get("ok").as_bool());
  CHECK(bolt.get("part").get("designation").as_string() == "ISO 4017 M8 × 32");
  const cc::base::Value bad = sh::make_part("iso4017", "M8", cc::base::make_object({{"length", 501}}));
  CHECK_FALSE(bad.get("ok").as_bool());
  CHECK(bad.get("error").as_string() == "bad length");
  CHECK(sh::make_part("nope", "M8", {}).get("error").as_string() == "unknown family");
  CHECK(sh::make_part("iso4032", "M7", {}).get("error").as_string() == "unknown size");
  const cc::base::Value gear = sh::make_part("spur-gear", "m2", cc::base::make_object({{"teeth", 20.5}}));
  CHECK(gear.get("error").as_string() == "bad teeth");
  const cc::base::Value inch = sh::make_part("asme-hex-bolt", "1/2", cc::base::make_object({{"length", 1.5}}));
  CHECK(inch.get("part").get("sizeText").as_string() == "1/2-13 UNC × 1-1/2 in");
  for (const cc::base::Value& row : sh::library_set().as_array()) {
    const auto& r = row.as_array();
    const cc::base::Value p = sh::make_part(r[0].as_string(), r[1], r[2]);
    INFO(r[0].as_string() + " " + r[1].as_string());
    REQUIRE(p.get("ok").as_bool());
    const cc::base::Value it = sh::part_item(p.get("part"));
    CHECK(it.get("format").as_string() == "cadcraft-library-item");
    CHECK(!it.get("plan").get("entities").as_array().empty());
  }
}

TEST_CASE("items: an elevation refuses a length out of range") {
  const sh::Section* s = sh::find_section("W12X26");
  REQUIRE(s != nullptr);
  CHECK_FALSE(sh::section_item(*s, cc::base::make_object({{"view", "elevation"}, {"length", 0}})).has_value());
  const auto it = sh::section_item(*s, cc::base::make_object({{"view", "elevation"}, {"length", 6000}}));
  REQUIRE(it.has_value());
  CHECK(it->get("name").as_string() == "W12x26 elevation, 6,000 mm");
}
