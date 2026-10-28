// SPDX-License-Identifier: Apache-2.0
// run_script on hostile scripts: every input the part_script fuzzer kept (fuzz/corpus/part_script, the regressions
// among them) answers one result per call, and NaN-sized curves and huge counts are refused or answered, never
// undefined behaviour (the sanitizer presets run this).
#include "cc/partcraft/script.hpp"

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/i18n/i18n.hpp"

namespace fs = std::filesystem;

TEST_CASE("script: the fuzz corpus and its regressions run to the end") {
  static const cc::i18n::Catalog kWords;
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(CC_SCRIPT_SEEDS)) {
    const auto bytes = cc::base::read_file(e.path(), std::uint64_t{1} << 20U);
    REQUIRE(bytes.has_value());
    const auto script = cc::base::parse_json(cc::base::as_text(*bytes));
    if (!script) {
      continue;
    }
    const cc::base::Value out = cc::partcraft::run_script(*script, kWords);
    CHECK(out.is_array());
    ++n;
  }
  CHECK(n > 0);
}

TEST_CASE("script: a curve with no degree and a huge segment count") {
  static const cc::i18n::Catalog kWords;
  auto s = cc::base::parse_json(
      R"({"calls":[{"op":"spline.areaTerm","args":["x",{}]},{"op":"spline.derivsAt","args":[{"deg":"x"},0,"x"]},
          {"op":"threads.boltSection","args":[[[0,1],[1,1]],{"segments":1e300}]}]})");
  REQUIRE(s.has_value());
  const cc::base::Value out = cc::partcraft::run_script(*s, kWords);
  REQUIRE(out.as_array().size() == 3);
  CHECK(out.as_array()[0].get("ok").as_number() == 0);
  CHECK(out.as_array()[2].has("err"));
}
