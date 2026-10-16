// SPDX-License-Identifier: Apache-2.0
// The reader against the JS reader's output for the public fixtures (tests/golden/README.md).
#include "golden.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/dwg/read.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

using cc::test::compare;

TEST_CASE("the reader matches the JS reader on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dwg";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();  // kitchen-equipment.dwg.json
    const std::string dwg = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / dwg;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / dwg;
    }
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    Value got = cc::dwg::read_dwg(*bytes);
    got.find("report")->erase("ms");
    std::vector<std::string> diffs;
    compare(*want, got, dwg, diffs);
    INFO(dwg);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 4);
}

TEST_CASE("the model is the same whatever the number of decoding threads") {
  const fs::path f = fs::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dwg";
  if (!fs::exists(f)) {
    SKIP("public fixtures not fetched");
  }
  const auto bytes = cc::base::read_file(f, std::uint64_t{1} << 30U);
  REQUIRE(bytes.has_value());
  Value one = cc::dwg::read_dwg(*bytes, {.threads = 1});
  Value many = cc::dwg::read_dwg(*bytes, {.threads = 8});
  one.find("report")->erase("ms");
  many.find("report")->erase("ms");
  CHECK(cc::base::to_json(one) == cc::base::to_json(many));
}
