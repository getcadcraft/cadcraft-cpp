// SPDX-License-Identifier: Apache-2.0
// The model scripts (tests/m3d/*.json) against what CadCraft 0.5's ModelCraft kernel did with them
// (tests/golden/js-0.5-m3d.tar.gz, made by tools/parity/m3d.mjs --golden-out): every step's result or error words,
// and the whole model after it, numbers at abs 1e-9 + rel 1e-12.
#include "cc/m3d/script.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/m3d/model.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

Value read_json(const fs::path& p) {
  const auto bytes = cc::base::read_file(p, std::uint64_t{1} << 30U);
  REQUIRE(bytes);
  const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());  // NOLINT
  auto v = cc::base::parse_json(text);
  REQUIRE(v);
  return std::move(*v);
}

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog c = [] {
    const auto bytes = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());  // NOLINT
    return *cc::i18n::Catalog::parse("en", text);
  }();
  return c;
}

}  // namespace

TEST_CASE("golden: the model scripts give what CadCraft 0.5's ModelCraft kernel gave") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "m3d";
  if (!fs::exists(golden)) {
    SKIP("references missing (tests/golden/js-0.5-m3d.tar.gz)");
  }
  cc::m3d::set_words(&words());
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(CC_SCRIPTS_DIR)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    const Value file = read_json(entry.path());
    const Value want = read_json(golden / entry.path().filename());
    const auto& scripts = file["scripts"].as_array();
    REQUIRE(want.as_array().size() == scripts.size());
    for (std::size_t i = 0; i < scripts.size(); ++i) {
      const Value got = cc::m3d::run_script(scripts[i]);
      INFO(entry.path().filename().string() << ": " << cc::base::to_json(scripts[i]["name"]));
      std::vector<std::string> diffs;
      cc::test::compare(want.as_array()[i], got, "", diffs);
      for (const auto& d : diffs) {
        UNSCOPED_INFO(d);
      }
      CHECK(diffs.empty());
      ++checked;
    }
  }
  CHECK(checked > 0);
  cc::m3d::set_words(nullptr);
}
