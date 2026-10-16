// SPDX-License-Identifier: Apache-2.0
// The draw lists against BimCraft 0.5's compileDrawing for the public fixtures (tests/golden/README.md).
#include "golden.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/drawing/compile.hpp"
#include "cc/dwg/read.hpp"
#include "cc/dxf/read.hpp"

namespace fs = std::filesystem;

TEST_CASE("the draw lists match the JS compileDrawing on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "compile";
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
    const std::string file = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / file;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / file;
    }
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    cc::drawing::CompileOptions opt;
    cc::base::Value model;
    if (file.ends_with(".dxf")) {
      auto m = cc::dxf::read_dxf_model(cc::dxf::decode_text(*bytes));
      REQUIRE(m.has_value());
      model = std::move(*m);
    } else {
      model = cc::dwg::read_dwg(*bytes);
      for (std::size_t i = 0; i < 6; ++i) {
        opt.version_tag += static_cast<char>(std::to_integer<unsigned char>((*bytes)[i]));
      }
    }
    // xrefs: "<name>.dwg" beside the drawing, as the CLI and the JS reference load them
    std::vector<cc::base::Value> xmodels;
    for (const cc::base::Value& b : model["blocks"].as_array()) {
      if ((static_cast<std::int64_t>(b["flags"].num_or(0)) & 4) != 0) {
        const fs::path x = src.parent_path() / (b["name"].as_string() + ".dwg");
        if (fs::exists(x)) {
          const auto xb = cc::base::read_file(x, std::uint64_t{1} << 30U);
          REQUIRE(xb.has_value());
          xmodels.push_back(cc::dwg::read_dwg(*xb));
          std::string key = b["name"].as_string();
          std::ranges::transform(key, key.begin(),
                                 [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
          opt.xrefs.push_back(cc::drawing::Xref{key, nullptr});
        }
      }
    }
    for (std::size_t i = 0; i < xmodels.size(); ++i) {
      opt.xrefs[i].model = &xmodels[i];
    }
    std::string json;
    cc::drawing::write_json(cc::drawing::compile_drawing(model, opt), json);
    const auto got = cc::base::parse_json(json);
    REQUIRE(got.has_value());
    std::vector<std::string> diffs;
    cc::test::compare(*want, *got, file, diffs);
    INFO(file);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 7);
}
