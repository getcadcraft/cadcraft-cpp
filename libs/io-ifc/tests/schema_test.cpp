// SPDX-License-Identifier: Apache-2.0
// The schema tables against cc-ifc 0.5's schema.js: a digest of every entity (name, code, parent, abstract, every
// attribute with its parsed type code, derived attributes, subtypes in order) and every defined type, computed the same
// way in node from the frozen js-0.5 (2026-10-21), and the type codes.
#include "cc/ifc/schema.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

std::string digest_text(const cc::ifc::Schema& s) {
  std::string txt;
  for (const std::string_view up : s.entity_names()) {
    const cc::ifc::Entity* e = s.entity(up);
    REQUIRE(e != nullptr);
    txt += std::string(up) + "|" + std::string(e->name) + "|" + std::to_string(e->code) + "|" + std::string(e->parent) +
           "|" + (e->abstract ? "1" : "0") + "|";
    for (std::size_t k = 0; k < e->attrs.size(); ++k) {
      const cc::ifc::Attr& a = e->attrs[k];
      txt += (k > 0 ? ";" : "") + std::string(a.name) + ":" + std::to_string(a.t.agg) + std::string(a.t.base) + "," +
             std::string(a.t.tname) + (a.t.opt ? "?" : "");
    }
    std::vector<std::string_view> d(e->derived.begin(), e->derived.end());
    std::ranges::sort(d);
    txt += "|";
    for (std::size_t k = 0; k < d.size(); ++k) {
      txt += (k > 0 ? ";" : "") + std::string(d[k]);
    }
    txt += "|";
    const auto& subs = s.subtypes(up);
    for (std::size_t k = 0; k < subs.size(); ++k) {
      txt += (k > 0 ? ";" : "") + std::string(subs[k]);
    }
    txt += "\n";
  }
  for (const std::string_view up : s.type_names()) {
    const auto members = s.select_members(up);
    txt += std::string(up) + "|" + std::string(s.type_code(up).value_or("null")) + "|" +
           std::string(s.type_name(up).value_or("null")) + "|";
    for (std::size_t k = 0; k < members.size(); ++k) {
      txt += (k > 0 ? ";" : "") + std::string(members[k]);
    }
    txt += std::string("|") + (s.is_real_type(up) ? "1" : "0") + "\n";
  }
  return txt;
}

}  // namespace

TEST_CASE("schema: the tables read as schema.js reads them") {
  struct Want {
    std::string_view key;
    std::string_view source;
    std::uint32_t crc;
    std::size_t size;
  };
  for (const Want& w :
       {Want{"IFC2X3", "IFC2X3_TC1.exp", 1733080065U, 229364}, Want{"IFC4", "IFC4_ADD2_TC1.exp", 1166331703U, 295265},
        Want{"IFC4X3", "IFC4X3_ADD2.exp", 3969205747U, 334961}}) {
    const cc::ifc::Schema& s = cc::ifc::schema_named(w.key);
    INFO(w.key);
    CHECK(s.source() == w.source);
    const std::string txt = digest_text(s);
    CHECK(txt.size() == w.size);
    CHECK(cc::ifc::crc32_name(txt) == w.crc);
  }
}

TEST_CASE("schema: type codes and schema names") {
  CHECK(cc::ifc::type_code("IFCWALL") == 2391406946U);
  CHECK(cc::ifc::name_of_code(cc::ifc::type_code("ifcwall")) == "IFCWALL");
  CHECK(cc::ifc::name_of_code(1390159747U) == "FILE_NAME");
  CHECK(cc::ifc::schema_for("ifc4x3_add2").schema->name() == "IFC4X3");
  CHECK(cc::ifc::schema_for("XYZ").guessed);
  CHECK(cc::ifc::schema_for(" ifc2x3 ").schema->name() == "IFC2X3");
  const cc::ifc::Schema& s = cc::ifc::schema_named("IFC4");
  CHECK(s.is_a("IFCWALLSTANDARDCASE", "IFCWALL"));
  CHECK(!s.is_a("IFCWALL", "IFCSLAB"));
  CHECK(s.entity("IFCWALL")->at("Name") == 2);
}
