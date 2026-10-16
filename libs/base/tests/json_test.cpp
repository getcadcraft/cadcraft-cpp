// SPDX-License-Identifier: Apache-2.0
#include "cc/base/json.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using cc::base::parse_json;

TEST_CASE("JSON round-trips through parse_json and write_json") {
  const std::string text =
      R"({"a":1,"b":[true,false,null,"x\"y\\z\n",{"$base64":"AQID"}],"c":{"d":-0.5,"e":1e+21,"f":"\ud83d\ude00"},"g":"$NaN"})";
  const auto v = parse_json(text);
  REQUIRE(v.has_value());
  CHECK(cc::base::to_json(*v) ==
        R"({"a":1,"b":[true,false,null,"x\"y\\z\n",{"$base64":"AQID"}],"c":{"d":-0.5,"e":1e+21,"f":"😀"},"g":"$NaN"})");
}

TEST_CASE("lone surrogates survive the round trip as escapes") {
  const auto v = parse_json(R"(["\ud800x","\udc00"])");
  REQUIRE(v.has_value());
  CHECK(cc::base::to_json(*v) == R"(["\ud800x","\udc00"])");
}

TEST_CASE("duplicate keys: the last value in the first place, as JSON.parse") {
  const auto v = parse_json(R"({"a":1,"b":2,"a":3})");
  REQUIRE(v.has_value());
  CHECK(cc::base::to_json(*v) == R"({"a":3,"b":2})");
}

TEST_CASE("malformed and oversized input is refused, never crashes") {
  for (const char* bad : {"", "{", "[1,]", R"({"a" 1})", "01", "1.", "-", R"("\x")", "\"a", "tru", "[1] 2", "\"\x01\"",
                          R"({"a":})", "1e", R"("\u12")"}) {
    CHECK_FALSE(parse_json(bad).has_value());
  }
  std::string deep(600, '[');
  deep += std::string(600, ']');
  const auto r = parse_json(deep);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.contains("deep"));
  cc::base::JsonLimits small;
  small.max_bytes = 3;
  CHECK_FALSE(parse_json("[1,2]", small).has_value());
  CHECK(parse_json("1e400")->as_number() > 1e308);
  CHECK(parse_json(" 2.5 ")->as_number() == 2.5);
}
