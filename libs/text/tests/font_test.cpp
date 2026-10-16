// SPDX-License-Identifier: Apache-2.0
#include "cc/text/font.hpp"

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"

namespace {
std::vector<std::byte> font_bytes() {
  auto b = cc::base::read_file(CC_FONT, std::uint64_t{1} << 24U);
  REQUIRE(b.has_value());
  return std::move(*b);
}
}  // namespace

TEST_CASE("Archivo lays out a line and fills the distance-field atlas") {
  auto font = cc::text::Font::load(font_bytes(), {.width = 75});
  REQUIRE(font.has_value());
  const auto gen0 = (*font)->atlas().generation;
  const cc::text::Line line = (*font)->layout("A-101 W");
  CHECK(line.glyphs.size() == 6);  // the space has no ink
  CHECK(line.width > 2.0F);
  CHECK(line.width < 5.0F);
  CHECK((*font)->atlas().generation > gen0);
  CHECK((*font)->ascender() > 0.5F);
  CHECK((*font)->descender() < 0.0F);
  for (const auto& g : line.glyphs) {
    CHECK(g.quad.u1 > g.quad.u0);
    CHECK(g.quad.y1 > g.quad.y0);
  }
  const auto gen1 = (*font)->atlas().generation;
  (void)(*font)->layout("A-101");  // known glyphs: the atlas does not change
  CHECK((*font)->atlas().generation == gen1);
}

TEST_CASE("narrow is narrower than normal") {
  auto narrow = cc::text::Font::load(font_bytes(), {.width = 75});
  auto normal = cc::text::Font::load(font_bytes(), {.width = 100});
  REQUIRE(narrow.has_value());
  REQUIRE(normal.has_value());
  CHECK((*narrow)->layout("Kitchen equipment").width < (*normal)->layout("Kitchen equipment").width);
}

TEST_CASE("a file that is not a font is refused") {
  std::vector<std::byte> junk(1000, std::byte{0x41});
  CHECK_FALSE(cc::text::Font::load(junk).has_value());
}

TEST_CASE("the atlas grows instead of failing when many glyphs are needed") {
  auto font = cc::text::Font::load(font_bytes(), {.texture_limit = 1024});
  REQUIRE(font.has_value());
  std::string all;
  for (std::uint32_t c = 0x21; c < 0x17F; ++c) {
    if (c < 0x80) {
      all += static_cast<char>(c);
    } else {
      all += static_cast<char>(0xC0U | (c >> 6U));
      all += static_cast<char>(0x80U | (c & 0x3FU));
    }
  }
  const auto line = (*font)->layout(all);
  CHECK(line.glyphs.size() > 300);
  CHECK((*font)->atlas().height <= 1024);
}
