// SPDX-License-Identifier: Apache-2.0
// cc::text — L2. Our bundled fonts (never a document's or the network's: AGENTS.md section 7, Theme) loaded with
// FreeType, laid out a line at a time, and drawn from a signed-distance-field atlas that grows as glyphs are needed,
// so text stays crisp at any zoom with one texture.
//
//   auto font = cc::text::Font::load(bytes, {.width = 75});    // Archivo at the "narrow" width the 0.5 view asks for
//   const cc::text::Line line = font->layout("A-101");        // glyphs in em units, the pen at x = 0 on the baseline
//   font->atlas()                                             // the SDF texture; generation() changes when it grows
//
// Units: em (the font size is 1). y grows up. Layout uses the fonts' advances; kerning and complex scripts wait for
// HarfBuzz (docs/gaps.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "cc/base/error.hpp"

namespace cc::text {

/// A glyph's quad: where it sits relative to the pen (em; y up) and where it lies in the atlas (0–1).
struct Quad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct PlacedGlyph {
  float x = 0;  // pen position (em) of this glyph
  Quad quad;
};

struct Line {
  std::vector<PlacedGlyph> glyphs;  // glyphs with no ink (space) are left out
  float width = 0;                  // advance of the whole line (em)
};

/// The distance-field texture: one byte per texel, 128 = the outline, larger inside; `spread` texels of distance.
struct Atlas {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> texels;  // width × height
  float spread_em = 0;               // the distance one full step (0 → 255) spans, in em
  std::uint32_t generation = 0;      // changes whenever texels change
};

struct FontOptions {
  double width = 100;        // the wdth axis, where the font has one (Archivo: 62–125; 75 = condensed)
  double weight = 400;       // the wght axis
  int texture_limit = 4096;  // the atlas never grows past this (the device's limit, from the renderer)
};

class Font {
 public:
  /// The font file's bytes (kept by the Font). Fails on a file FreeType cannot read.
  static base::Result<std::unique_ptr<Font>> load(std::vector<std::byte> bytes, const FontOptions& options = {});
  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  Font(Font&&) = delete;
  Font& operator=(Font&&) = delete;

  /// One line of UTF-8 text (no line breaks: a "\n" is drawn as nothing).
  [[nodiscard]] Line layout(std::string_view utf8);
  [[nodiscard]] float ascender() const noexcept;   // em, positive
  [[nodiscard]] float descender() const noexcept;  // em, negative
  [[nodiscard]] const Atlas& atlas() const noexcept;

  struct Impl;

 private:
  explicit Font(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cc::text
