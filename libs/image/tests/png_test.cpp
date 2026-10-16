// SPDX-License-Identifier: Apache-2.0
#include "cc/image/png.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a PNG has the signature, an IHDR of the right size and refuses a wrong buffer") {
  std::vector<std::uint8_t> px(std::size_t{3} * 2 * 4, 200);
  const auto png = cc::image::encode_png(px, 3, 2);
  REQUIRE(png.has_value());
  REQUIRE(png->size() > 33);
  CHECK(std::to_integer<int>((*png)[1]) == 'P');
  CHECK(std::to_integer<int>((*png)[12]) == 'I');
  CHECK(std::to_integer<int>((*png)[19]) == 3);  // width, big-endian
  CHECK(std::to_integer<int>((*png)[23]) == 2);  // height
  CHECK_FALSE(cc::image::encode_png(px, 4, 2).has_value());
  CHECK_FALSE(cc::image::encode_png({}, 0, 0).has_value());
}
