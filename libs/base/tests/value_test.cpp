// SPDX-License-Identifier: Apache-2.0
#include "cc/base/value.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using cc::base::Value;

namespace {
std::string num(double d) {
  std::string s;
  cc::base::append_number(s, d);
  return s;
}
}  // namespace

TEST_CASE("numbers print as JavaScript prints them") {
  CHECK(num(0.0) == "0");
  CHECK(num(-0.0) == "0");
  CHECK(num(1.0) == "1");
  CHECK(num(-2.5) == "-2.5");
  CHECK(num(0.1) == "0.1");
  CHECK(num(1e21) == "1e+21");
  CHECK(num(123456789012345680000.0) == "123456789012345680000");
  CHECK(num(1e-7) == "1e-7");
  CHECK(num(0.000001) == "0.000001");
  CHECK(num(1.5e-10) == "1.5e-10");
  CHECK(num(412148564080.0) == "412148564080");
  CHECK(num(4294967295.0) == "4294967295");
  CHECK(num(0.30000000000000004) == "0.30000000000000004");
  // String(NaN) / String(±Infinity): once read past the end of to_chars' "inf" (fuzz regress-c0449ba28405)
  CHECK(num(std::numeric_limits<double>::quiet_NaN()) == "NaN");
  CHECK(num(std::numeric_limits<double>::infinity()) == "Infinity");
  CHECK(num(-std::numeric_limits<double>::infinity()) == "-Infinity");
}

TEST_CASE("objects keep insertion order, reassignment keeps the place, undefined members are dropped") {
  Value o = Value::object();
  o.set("b", 1);
  o.set("a", true);
  o.set("c", Value{});
  o.set("b", "x");
  CHECK(cc::base::to_json(o) == R"({"b":"x","a":true})");
  CHECK(o.has("c"));
  o.erase("c");
  CHECK_FALSE(o.has("c"));
  CHECK(o["missing"].is_undefined());
}

TEST_CASE("JSON escapes like JSON.stringify and marks what JSON cannot hold") {
  Value a = Value::array();
  a.push(std::numeric_limits<double>::quiet_NaN());
  a.push(-std::numeric_limits<double>::infinity());
  a.push(Value{});
  a.push("q\"\\\n\x01");
  a.push(cc::base::ByteVec{1, 2, 3});
  CHECK(cc::base::to_json(a) == R"(["$NaN","$-Inf",null,"q\"\\\n\u0001",{"$base64":"AQID"}])");
  std::string lone;
  const std::vector<std::uint16_t> units{0x41, 0xD800, 0x42, 0xD83D, 0xDE00};
  cc::base::append_utf16(lone, units);
  CHECK(cc::base::to_json(Value(lone)) == "\"A\\ud800B\xF0\x9F\x98\x80\"");
}

TEST_CASE("text helpers") {
  const std::vector<std::uint8_t> b{0x4D, 0x61, 0x6E, 0x00};
  CHECK(cc::base::base64(b) == "TWFuAA==");
  CHECK(cc::base::hex_upper(0) == "0");
  CHECK(cc::base::hex_upper(0x1F) == "1F");
  CHECK(cc::base::to_upper("abc\xC3\xA4") == "ABC\xC3\x84");
  CHECK(Value(0.0).truthy() == false);
  CHECK(Value("").truthy() == false);
  CHECK(Value::array().truthy());
  CHECK_FALSE(Value(std::nan("")).truthy());
}
