// SPDX-License-Identifier: Apache-2.0
#include "cc/dwg/read.hpp"

#include <array>
#include <cstddef>
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
