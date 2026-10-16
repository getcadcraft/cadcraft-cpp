// SPDX-License-Identifier: Apache-2.0
#include "cc/base/js.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace cc::base;

TEST_CASE("parseFloat, parseInt and trim as in JS") {
  CHECK(js_parse_float("  1.5e3xyz") == 1500);
  CHECK(js_parse_float("-.5") == -0.5);
  CHECK(js_parse_float("1e") == 1);
  CHECK(std::isnan(js_parse_float("e5")));
  CHECK(std::isinf(js_parse_float("-Infinity")));
  CHECK(js_parse_float("1e999") == std::numeric_limits<double>::infinity());
  CHECK(js_parse_int(" -42.9") == -42);
  CHECK(std::isnan(js_parse_int("x1")));
  CHECK(js_trim("\xC2\xA0 a b \xE2\x80\xA8\t") == "a b");
  CHECK(js_blank(" \t\xE3\x80\x80"));
  CHECK(utf16_length("a\xC3\xA9\xF0\x9F\x98\x80") == 4);
}

TEST_CASE("localeCompare with numeric collation orders as Node's ICU does") {
  // Node 22 / ICU 78: ["a","A","b",…].sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
  std::vector<std::string> t = {"a",     "A",        "b",        "B",   "\xC3\xA1", "\xC3\x81", "a1",    "a10",
                                "a2",    "A-1",      "a_1",      "a 1", "a01",      "a1b",      "layer", "Layer",
                                "LAYER", "\xC3\xA9", "e",        "f",   "\xC3\xB1", "n",        "o",     "\xC3\xB8",
                                "Z",     "z",        "\xC3\x9F", "ss",  "0",        "00",       "1",     "01"};
  std::ranges::stable_sort(
      t, [](const std::string& a, const std::string& b) { return js_locale_compare_numeric(a, b) < 0; });
  const std::vector<std::string> want = {
      "0",     "00",    "1",   "01",       "a",   "A",        "\xC3\xA1", "\xC3\x81", "a 1",      "a_1", "A-1",
      "a1",    "a01",   "a1b", "a2",       "a10", "b",        "B",        "e",        "\xC3\xA9", "f",   "layer",
      "Layer", "LAYER", "n",   "\xC3\xB1", "o",   "\xC3\xB8", "ss",       "\xC3\x9F", "z",        "Z"};
  CHECK(t == want);
  std::string punct = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$0123456789aAbB";
  std::string shuffled = punct;
  std::ranges::reverse(shuffled);
  std::vector<std::string> chars;
  for (const char c : shuffled) {
    chars.emplace_back(1, c);
  }
  std::ranges::stable_sort(
      chars, [](const std::string& a, const std::string& b) { return js_locale_compare_numeric(a, b) < 0; });
  std::string back;
  for (const auto& c : chars) {
    back += c;
  }
  CHECK(back == punct);
}

TEST_CASE("ignorable characters and combining marks collate as in ICU") {
  CHECK(js_locale_compare_numeric("\xC2\x9D\xC2\x9D"
                                  "0",
                                  "0") == 0);  // C1 controls
  CHECK(js_locale_compare_numeric("\xE2\x80\x8B"
                                  "0",
                                  "0") == 0);                  // zero-width space
  CHECK(js_locale_compare_numeric("a\xCC\x81", "a") > 0);      // a + combining acute
  CHECK(js_locale_compare_numeric("\xE2\x84\xA2", "tm") > 0);  // ™ sorts as "tm", after it
  CHECK(js_locale_compare_numeric("\xE2\x84\xA2", "tn") < 0);
}
