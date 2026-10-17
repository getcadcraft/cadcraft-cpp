// SPDX-License-Identifier: Apache-2.0
#include "cc/dxf/read.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {
std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}
}  // namespace

TEST_CASE("a minimal DXF gives a model with one line in model space") {
  const std::string_view text =
      "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n0\nENDSEC\n"
      "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nWalls\n10\n1.5\n20\n2\n30\n0\n11\n4\n21\n6\n31\n0\n0\nENDSEC\n0\nEOF\n";
  const auto m = cc::dxf::read_dxf_model(text);
  REQUIRE(m.has_value());
  CHECK((*m)["version"].as_string() == "AC1009");
  const auto& blocks = (*m)["blocks"].as_array();
  REQUIRE(!blocks.empty());
  const cc::base::Value* ms = nullptr;
  for (const auto& b : blocks) {
    if (b["name"].as_string() == "*Model_Space") {
      ms = &b;
    }
  }
  REQUIRE(ms != nullptr);
  const auto& ents = (*ms)["entities"].as_array();
  REQUIRE(ents.size() == 1);
  CHECK(ents[0]["type"].as_string() == "LINE");
  CHECK(ents[0]["layer"].as_string() == "Walls");
  CHECK(ents[0]["start"].get("x").as_number() == 1.5);
  CHECK(ents[0]["end"].get("y").as_number() == 6);
  CHECK(ents[0]["space"].as_string() == "model");
  CHECK((*m)["report"]["counts"]["LINE"].as_number() == 1);
}

TEST_CASE("binary DXF and bad group codes are refused with the JS wording") {
  auto b = cc::dxf::read_dxf_model("AutoCAD Binary DXF\r\n\x1a");
  REQUIRE(!b.has_value());
  CHECK(b.error().message == "This is a binary DXF. Save it as ASCII DXF and try again.");
  auto bad = cc::dxf::read_dxf_model("0\nSECTION\nxyz\nfoo\n");
  REQUIRE(!bad.has_value());
  CHECK(bad.error().message == "This file is not a readable DXF (bad group code on line 3).");
}

TEST_CASE("carets decode as in the JS") {
  const auto m = cc::dxf::read_dxf_model("0\nSECTION\n2\nENTITIES\n0\nTEXT\n1\na^ b^Jc\n0\nENDSEC\n0\nEOF\n");
  REQUIRE(m.has_value());
  for (const auto& blk : (*m)["blocks"].as_array()) {
    for (const auto& e : blk["entities"].as_array()) {
      CHECK(e["value"].as_string() == "a^b\nc");
    }
  }
}

TEST_CASE("text decodes as UTF-8, else as Windows-1252 like BimCraft") {
  CHECK(cc::dxf::decode_text(bytes_of("\xEF\xBB\xBFok")) == "ok");
  CHECK(cc::dxf::decode_text(bytes_of("caf\xC3\xA9")) == "caf\xC3\xA9");
  CHECK(cc::dxf::decode_text(bytes_of("caf\xE9 \x80")) == "caf\xC3\xA9 \xE2\x82\xAC");
}

#include <filesystem>

#include "cc/base/file.hpp"

TEST_CASE("inputs the fuzzer once broke the reader with still read (regression corpus)") {
  // e.g. regress-6138c338d51f: a group 70 of 7.8e74 (a double → int64 cast; now JS's ToInt32)
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(CC_SOURCE_DIR) / "fuzz/corpus/dxf")) {
    if (!e.path().filename().string().starts_with("regress-")) {
      continue;
    }
    const auto bytes = cc::base::read_file(e.path(), std::uint64_t{1} << 26U);
    REQUIRE(bytes.has_value());
    const auto model = cc::dxf::read_dxf_model(cc::dxf::decode_text(*bytes));
    (void)model;  // read or refused: either way no crash
    ++n;
  }
  CHECK(n >= 1);
}

TEST_CASE("a DIMENSION of type 7 is linear, as DIM_TYPES[7] || 'DIMENSION_LINEAR' in the JS") {
  const auto m = cc::dxf::read_dxf_model("0\nSECTION\n2\nENTITIES\n0\nDIMENSION\n70\n7\n0\nENDSEC\n0\nEOF\n");
  REQUIRE(m.has_value());
  CHECK((*m)["report"]["counts"]["DIMENSION_LINEAR"].as_number() == 1);
}
