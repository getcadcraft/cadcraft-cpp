// SPDX-License-Identifier: Apache-2.0
#include "cc/image/png.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/image/picture.hpp"

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

TEST_CASE("decodePng reads back what encode_png writes; shrink box-filters; pdfImage keeps a JPEG as it is") {
  constexpr std::uint32_t kW = 5;
  constexpr std::uint32_t kH = 3;
  std::vector<std::uint8_t> rgba;
  for (std::uint32_t i = 0; i < kW * kH; ++i) {
    rgba.insert(rgba.end(), {static_cast<std::uint8_t>(i * 17), static_cast<std::uint8_t>(255 - i),
                             static_cast<std::uint8_t>(i * 3), static_cast<std::uint8_t>(i == 4 ? 0 : 255)});
  }
  const auto png = cc::image::encode_png(rgba, kW, kH);
  REQUIRE(png);
  std::vector<std::uint8_t> bytes;
  for (const std::byte b : *png) {
    bytes.push_back(std::to_integer<std::uint8_t>(b));
  }
  const auto decoded = cc::image::decode_png(bytes);
  REQUIRE(decoded.has_value());
  const cc::image::Decoded img = decoded.value_or(cc::image::Decoded{});
  CHECK((img.w == kW && img.h == kH));
  const std::vector<std::uint8_t> alpha = img.alpha.value_or(std::vector<std::uint8_t>{});
  bool same = alpha.size() == std::size_t{kW} * kH;
  for (std::size_t i = 0; same && i < std::size_t{kW} * kH; ++i) {
    same = img.rgb[i * 3] == rgba[i * 4] && img.rgb[i * 3 + 1] == rgba[i * 4 + 1] &&
           img.rgb[i * 3 + 2] == rgba[i * 4 + 2] && alpha[i] == rgba[i * 4 + 3];
  }
  CHECK(same);
  CHECK_FALSE(cc::image::decode_png(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 20)));  // cut short
  CHECK_FALSE(cc::image::decode_png(bytes, 10));                                                   // over the cap
  const cc::image::Decoded small = cc::image::shrink(img, 1, 1);
  CHECK(small.rgb.size() == 3);
  const auto pdf = cc::image::pdf_image("image/png", bytes, 1, 25.4, 25.4);  // 1 px per inch: shrunk to 1 × 1
  CHECK((pdf && pdf->shrunk && pdf->w == 1 && pdf->filter == "FlateDecode" && pdf->smask));
  const std::vector<std::uint8_t> jpg{0xff, 0xd8, 0xff, 0xc0, 0, 17, 8, 0, 30, 0, 50, 3, 0, 0, 0, 0};
  const auto j = cc::image::pdf_image("image/jpeg", jpg, 1, 25.4, 25.4);
  CHECK((j && j->kept && j->filter == "DCTDecode" && j->data == jpg && j->w == 50));
}
