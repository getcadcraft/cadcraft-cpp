// SPDX-License-Identifier: Apache-2.0
// The exports of every model script's model (tests/m3d/*.json) against CadCraft 0.5's io.js: each format's size and
// CRC-32 (tests/golden/js-0.5-m3d-io.tar.gz, made by tools/parity/m3d-io.mjs --golden-out).
#include "cc/m3d/io.hpp"

#include <array>
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

std::uint32_t crc32(std::string_view s) {
  std::uint32_t c = 0xFFFFFFFFU;
  for (const char ch : s) {
    c ^= static_cast<unsigned char>(ch);
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? (c >> 1U) ^ 0xEDB88320U : c >> 1U;
    }
  }
  return ~c;
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

TEST_CASE("golden: every model script's model exports the bytes CadCraft 0.5's io.js exported") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "m3d-io";
  if (!fs::exists(golden)) {
    SKIP("references missing (tests/golden/js-0.5-m3d-io.tar.gz)");
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
      const Value got = cc::m3d::io::export_all(scripts[i]);
      INFO(entry.path().filename().string() << ": " << cc::base::to_json(scripts[i]["name"]));
      for (const char* k : {"glb", "stl", "obj", "objMtl", "mtl", "dxf", "dae", "ifc"}) {
        const Value& w = want.as_array()[i][k];
        if (!w.is_array()) {
          CHECK_FALSE(got.has(k));
          continue;
        }
        std::string bytes = got[k].is_string() ? got[k].as_string() : std::string();
        if (std::string_view(k) == "glb" || std::string_view(k) == "stl") {
          const auto raw = cc::base::unbase64(bytes);
          bytes.assign(raw.begin(), raw.end());
        }
        INFO(k);
        CHECK(static_cast<double>(bytes.size()) == w.as_array()[0].as_number());
        CHECK(static_cast<double>(crc32(bytes)) == w.as_array()[1].as_number());
      }
      ++checked;
    }
  }
  CHECK(checked > 0);
  cc::m3d::set_words(nullptr);
}

TEST_CASE("ifcGuid and stepString as io.js writes them") {
  CHECK(cc::m3d::io::ifc_guid("a").size() == 22);
  CHECK(cc::m3d::io::step_string("it's \\ é") == "'it''s \\\\ \\X2\\00E9\\X0\\'");
  CHECK(cc::m3d::io::nearest_aci("#ff0000") == 1);
  CHECK(cc::m3d::io::nearest_aci("nope") == 7);
}
