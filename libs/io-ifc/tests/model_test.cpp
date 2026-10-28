// SPDX-License-Identifier: Apache-2.0
// model.js cases: the expected values are what the frozen 0.5 IfcFile gives for the same file and edits (node,
// 2026-10-21). The corpus check is tools/parity/ifc-model.mjs.
#include "cc/ifc/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::string_view kFile =
    "ISO-10303-21;HEADER;FILE_DESCRIPTION(('ViewDefinition [x]'),'2;1');FILE_NAME('a.ifc','2020-01-01T00:00:00',('me'),"
    "('org'),'pre','app','');FILE_SCHEMA(('IFC4'));X_EXTRA(1,.T.);ENDSEC;DATA;\n"
    "#1=IFCWALL('g',$,'Wall',$,$,#2,#3,$,.STANDARD.);\n#2=IFCLOCALPLACEMENT($,#4);\n"
    "#3=IFCPRODUCTDEFINITIONSHAPE($,$,(#5));\n#5=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#6));\n"
    "#7=IFCRELDEFINESBYPROPERTIES('h',$,$,$,(#1,#2),#8);\n#9=(IFCA()IFCB(#1));\n#10=IFCSLAB('s',$,$,$,$,$,$,$,$);\n"
    "ENDSEC;END-ISO-10303-21;\n";

cc::ifc::Raw str(std::string s) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::string;
  r.text = std::move(s);
  return r;
}
cc::ifc::Raw num(double d) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::number;
  r.num = d;
  return r;
}
cc::ifc::Raw ref(double d) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::ref;
  r.num = d;
  return r;
}
cc::ifc::Raw enm(std::string s) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::enumeration;
  r.text = std::move(s);
  return r;
}
cc::ifc::Raw list(cc::ifc::RawList items) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::list;
  r.items = std::move(items);
  return r;
}
cc::ifc::Raw typed(std::string n, cc::ifc::Raw v) {
  cc::ifc::Raw r;
  r.kind = cc::ifc::Raw::Kind::typed;
  r.text = std::move(n);
  r.items.push_back(std::move(v));
  return r;
}

}  // namespace

TEST_CASE("model: lines, types, inverse references, edits and saving as model.js") {
  std::vector<std::byte> bytes;
  for (const char c : kFile) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  auto opened = cc::ifc::IfcFile::open(std::move(bytes));
  REQUIRE(opened.has_value());
  cc::ifc::IfcFile& f = **opened;
  CHECK(f.count() == 7);
  const std::vector<std::pair<std::string, std::size_t>> types{{"IFCWALL", 1},
                                                               {"IFCLOCALPLACEMENT", 1},
                                                               {"IFCPRODUCTDEFINITIONSHAPE", 1},
                                                               {"IFCSHAPEREPRESENTATION", 1},
                                                               {"IFCRELDEFINESBYPROPERTIES", 1},
                                                               {"IFCSLAB", 1}};
  CHECK(f.types_present() == types);
  CHECK(f.of_type("IFCBUILDINGELEMENT", true) == std::vector<cc::ifc::Id>{1, 10});
  CHECK(f.refs_to(1) == std::vector<cc::ifc::Id>{7, 9});
  const auto& inv = f.inverse("IFCRELDEFINESBYPROPERTIES", "RelatedObjects");
  CHECK(inv.size() == 2);
  CHECK(inv.at(2) == std::vector<cc::ifc::Id>{7});
  CHECK(f.attr(1, "Name")->text == "Wall");
  CHECK(f.attr(1, "Nope").undefined());
  CHECK(f.type(9) == "");
  CHECK(!f.type(4).has_value());

  REQUIRE(
      f.set(1, "IFCWALL", {str("g2"), {}, str("W\xC3\xA9"), {}, {}, ref(2), ref(3), {}, enm("STANDARD")}).has_value());
  REQUIRE(f.add("ifcSlab", {num(1.5), num(2), num(1e21), num(-0.0), list({num(1), list({num(2)})}),
                            typed("IFCLENGTHMEASURE", num(3)), typed("IFCINTEGER", num(4))})
              .has_value());
  CHECK(f.remove(10));
  CHECK(!f.set(0, "IFCWALL", {}).has_value());
  const auto saved = f.save("2026-10-21T00:00:00");
  REQUIRE(saved.has_value());
  CHECK(*saved ==
        "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition "
        "[x]'),'2;1');\nFILE_NAME('a.ifc','2026-10-21T00:00:00',("
        "'me'),('org'),'pre','app','');\nFILE_SCHEMA(('IFC4'));\nX_EXTRA(1,.T.);\nENDSEC;\nDATA;\n"
        "#1=IFCWALL('g2',$,'W\\X2\\00E9\\X0\\',$,$,#2,#3,$,.STANDARD.);\n#2=IFCLOCALPLACEMENT($,#4);\n"
        "#3=IFCPRODUCTDEFINITIONSHAPE($,$,(#5));\n#5=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#6));\n"
        "#7=IFCRELDEFINESBYPROPERTIES('h',$,$,$,(#1,#2),#8);\n#9=(IFCA()IFCB(#1));\n"
        "#11=IFCSLAB(1.5,2,1e+21,0,(1,(2)),IFCLENGTHMEASURE(3.),IFCINTEGER(4));\nENDSEC;\nEND-ISO-10303-21;\n");
}
