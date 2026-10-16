// SPDX-License-Identifier: Apache-2.0
// The DXF reader against BimCraft 0.5's readDxfModel output for the public fixtures (tests/golden/README.md).
#include "golden.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/dxf/read.hpp"

namespace fs = std::filesystem;

TEST_CASE("the DXF reader matches the JS reader on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dxf";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (std::getenv("CC_REQUIRE_CORPUS") != nullptr) {  // NOLINT(concurrency-mt-unsafe)
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();  // kitchen-equipment.dxf.json
    const std::string dxf = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / dxf;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / dxf;
    }
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    const auto got = cc::dxf::read_dxf_model(cc::dxf::decode_text(*bytes));
    REQUIRE(got.has_value());
    std::vector<std::string> diffs;
    cc::test::compare(*want, *got, dxf, diffs);
    INFO(dxf);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 3);
}
