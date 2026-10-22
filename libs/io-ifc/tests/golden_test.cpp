// SPDX-License-Identifier: Apache-2.0
// The STEP reader against cc-ifc 0.5's step.js for the public IFC fixture (tests/golden/README.md): the head and every
// instance's parameters, exactly (the kit's ifc-step tolerance is 0).
#include "golden.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/ifc/step.hpp"

namespace fs = std::filesystem;

TEST_CASE("the STEP reader matches step.js on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "ifc-step";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();
    const std::string ifc = name.substr(0, name.size() - 5);
    const fs::path src = corpus / "fixtures/showcase" / ifc;
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    const auto ix = cc::ifc::scan_step(*bytes);
    REQUIRE(ix.has_value());
    std::vector<std::string> diffs;
    const auto head = cc::base::parse_json(cc::ifc::step_head_json(*ix));
    REQUIRE(head.has_value());
    cc::test::compare((*want)["head"], *head, ifc + ".head", diffs);
    const auto& lines = (*want)["lines"].as_array();
    REQUIRE(lines.size() == ix->count);  // the fixture has no repeated instance numbers
    for (std::size_t i = 0; i < ix->count && diffs.size() < 20; ++i) {
      std::string line;
      cc::ifc::step_line_json(*bytes, *ix, i, line);
      const auto got = cc::base::parse_json(line);
      REQUIRE(got.has_value());
      cc::test::compare(lines[i], *got, ifc + "#" + std::to_string(ix->ids[i]), diffs);
    }
    INFO(ifc);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 1);
}
