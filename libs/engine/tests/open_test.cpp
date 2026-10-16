// SPDX-License-Identifier: Apache-2.0
#include "cc/engine/open.hpp"

#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {
std::span<const std::byte> bytes_of(std::string_view s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}
}  // namespace

TEST_CASE("DWG and DXF are told apart by their bytes, as the 0.5 apps do") {
  CHECK(cc::engine::kind_of("x.bin", bytes_of("AC1032....")) == "dwg");
  CHECK(cc::engine::kind_of("plan.DXF", bytes_of("garbage")) == "dxf");
  CHECK(cc::engine::kind_of("noext", bytes_of("  0\nSECTION\n")) == "dxf");
  CHECK(cc::engine::kind_of("photo.jpg", bytes_of("\xFF\xD8\xFF")).empty());
  CHECK_FALSE(cc::engine::open_bytes("photo.jpg", bytes_of("\xFF\xD8\xFF")).has_value());
}

TEST_CASE("a DXF from memory opens and compiles") {
  const auto d = cc::engine::open_bytes(
      "t.dxf", bytes_of("0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n0\n20\n0\n11\n1\n21\n1\n0\nENDSEC\n0\nEOF\n"));
  REQUIRE(d.has_value());
  REQUIRE(!(*d)->drawing.spaces.empty());
  CHECK((*d)->drawing.spaces[0].batches.size() == 1);
}

TEST_CASE("the kitchen DXF opens with its xref (base-plan.dwg beside it)") {
  const auto f = std::filesystem::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dxf";
  if (!std::filesystem::exists(f)) {
    SKIP("public fixtures not fetched");
  }
  const auto d = cc::engine::open_file(f);
  REQUIRE(d.has_value());
  CHECK((*d)->kind == "dxf");
  REQUIRE((*d)->drawing.xrefs.size() == 1);
  CHECK((*d)->drawing.xrefs[0].second);
}
