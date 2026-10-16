// SPDX-License-Identifier: Apache-2.0
#include "cc/dwg/read.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a file that is not a DWG gives a model with an error, never a throw") {
  std::array<std::byte, 200> junk{};
  for (std::size_t i = 0; i < junk.size(); ++i) {
    junk.at(i) = std::byte{static_cast<unsigned char>(i * 7U)};
  }
  const cc::base::Value m = cc::dwg::read_dwg(junk);
  REQUIRE(m["report"]["error"].is_string());
  CHECK(m["report"]["error"].as_string().starts_with("not a DWG file"));
}

TEST_CASE("a file too short is refused with the JS wording") {
  const std::array<std::byte, 3> tiny{std::byte{'A'}, std::byte{'C'}, std::byte{'1'}};
  const cc::base::Value m = cc::dwg::read_dwg(tiny);
  CHECK(m["report"]["error"].as_string() == "too short to be a DWG file (3 bytes)");
  CHECK(m["version"].as_string() == "AC1");
}

#include <filesystem>

#include "cc/base/file.hpp"

TEST_CASE("inputs the fuzzer once broke the reader with still read (regression corpus)") {
  const std::filesystem::path dir = std::filesystem::path(CC_SOURCE_DIR) / "fuzz/corpus/dwg";
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.path().filename().string().starts_with("regress-")) {
      continue;
    }
    const auto bytes = cc::base::read_file(e.path(), std::uint64_t{1} << 24U);
    REQUIRE(bytes.has_value());
    const cc::base::Value m = cc::dwg::read_dwg(*bytes);
    CHECK(m.is_object());
    ++n;
  }
  CHECK(n >= 3);
}
