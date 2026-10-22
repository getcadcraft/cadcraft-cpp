// SPDX-License-Identifier: Apache-2.0
// step.js cases: each expected text is what the frozen 0.5 step.js gives for the same input (node, 2026-10-21).
#include "cc/ifc/step.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {

std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}

/// parseParams of `text` (ending in ";") as JSON, or "!" + the error message.
std::string params(std::string_view text) {
  auto p = cc::ifc::parse_params(bytes_of(text), 0, text.size() - 1);
  if (!p) {
    return "!" + p.error().message;
  }
  std::string out;
  cc::ifc::write_json(*p, out);
  return out;
}

}  // namespace

TEST_CASE("step: parameter values") {
  CHECK(
      params("($,*,#12,'a''b',.T.,\"0AF\",IFCLABEL('x'),(1,2.),1.E-3);") ==
      R"([null,{"derived":true},{"ref":12},"a'b",{"enum":"T"},{"bin":"0AF"},{"typed":"IFCLABEL","value":"x"},[1,2],0.001])");
}

TEST_CASE("step: string escapes and raw UTF-8") {
  CHECK(params(R"(('\X2\00E9\X0\','\X4\0001F600\X0\','\S\A','\X\41','\\','\PA\x','\x2\D800\x0\','\X2\12\X0\');)") ==
        "[\"\xC3\xA9\",\"\xF0\x9F\x98\x80\",\"\xC3\x81\",\"A\",\"\\\\\",\"x\",\"\\ud800\",\"\\\\X2\\\\12\\\\X0\\\\\"]");
  CHECK(params(R"(('\X4\00110000\X0\');)") == "!Invalid code point 1114112");
  // Latin-1 é, UTF-8 é, a UTF-8 byte order mark inside a string (TextDecoder drops it)
  CHECK(params("('\xE9','\xC3\xA9','\xEF\xBB\xBF"
               "A');") == "[\"\xC3\xA9\",\"\xC3\xA9\",\"A\"]");
}

TEST_CASE("step: numbers and enumerations") {
  CHECK(params("(1.,1.E5,-.5,+5,00012,1.e-3,-0,1E-400);") == "[1,100000,-0.5,5,12,0.001,0,0]");
  CHECK(params("(1E400);") == "!Bad number 1E400 (at byte 6)");
  CHECK(params("(1-2);") == "!Bad number 1-2 (at byte 4)");
  CHECK(params("(.t., . x .,.\xC3\xA9.);") == R"([{"enum":"T"},{"enum":"X"},{"enum":"Ã©"}])");
}

TEST_CASE("step: errors worded as step.js") {
  CHECK(params("(#);") == "!Expected a number after # (at byte 1)");
  CHECK(params("(1 2);") == "!Expected , or ) (at byte 3)");
  CHECK(params("(@);") == "!Unexpected character \"@\" (at byte 1)");
  CHECK(params("((") == "!Unexpected character \"\\u0000\" (at byte 2)");
  CHECK(params("('abc);") == "!A text value is not closed (at byte 6)");
  CHECK(params("(IFCX 1);") == "!Expected ( after IFCX (at byte 6)");
  CHECK(params("($,/*c*/ 1 /*d*/);") == "[null,1]");
  CHECK(params("(IFCREAL(1.5),IFCLIST(1,2),IFCE());") ==
        R"([{"typed":"IFCREAL","value":1.5},{"typed":"IFCLIST","value":[1,2]},{"typed":"IFCE","value":[]}])");
  const std::string deep = std::string(5000, '(') + std::string(5000, ')') + ";";
  CHECK(params(deep) == "!Maximum call stack size exceeded");
}

TEST_CASE("step: complex instances and the index") {
  const std::string_view cx = "(IFCA(1)IFCB('x',$));";
  auto parts = cc::ifc::parse_complex(bytes_of(cx), 0, cx.size() - 1);
  REQUIRE(parts.has_value());
  REQUIRE(parts->size() == 2);
  CHECK((*parts)[1].type == "IFCB");
  const std::string_view file =
      "ISO-10303-21;HEADER;FILE_SCHEMA(('ifc4 "
      "'));ENDSEC;DATA;#1=IFCWALL($);#7=(IFCA()IFCB());x;#=1;ENDSEC;END-ISO-10303-21;";
  auto ix = cc::ifc::scan_step(bytes_of(file));
  REQUIRE(ix.has_value());
  CHECK(ix->count == 2);
  CHECK(ix->schema == "IFC4");
  CHECK(ix->max_id == 7);
  CHECK(ix->errors.size() == 2);
  CHECK(!cc::ifc::scan_step(bytes_of("ISO-10303-22;")).has_value());
}

TEST_CASE("step: a DATA with no ; after ENDSEC ends the scan (the JS starts over for ever: fuzz timeout)") {
  const std::string_view file = "ISO-10303-21;DATA;#1=IFCX(2);ENDSEC;DATA";
  auto ix = cc::ifc::scan_step(bytes_of(file));
  REQUIRE(ix.has_value());
  CHECK(ix->count == 1);
}
