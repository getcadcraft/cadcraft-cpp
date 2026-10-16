// SPDX-License-Identifier: Apache-2.0
#include "cc/text/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include "cc/base/error.hpp"
#include "cc/base/reader.hpp"

namespace cc::text {

namespace {

constexpr int kSdfPixels = 48;  // the size glyphs are rendered at into the atlas
constexpr int kSpread = 8;      // FreeType's default SDF spread, in pixels
constexpr int kPad = 1;         // between glyphs in the atlas

/// One code point of UTF-8 at i (a stray byte → U+FFFD) and its length.
std::uint32_t next_code_point(std::string_view s, std::size_t& i) {
  const auto c = static_cast<unsigned char>(s[i]);
  auto cont = [&](std::size_t k) {
    return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0U) == 0x80U;
  };
  auto at = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3FU); };
  std::uint32_t cp = 0xFFFD;
  std::size_t n = 1;
  if (c < 0x80) {
    cp = c;
  } else if (c >= 0xC2 && c <= 0xDF && cont(1)) {
    cp = ((c & 0x1FU) << 6U) | at(1);
    n = 2;
  } else if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
    cp = ((c & 0x0FU) << 12U) | (at(1) << 6U) | at(2);
    n = 3;
  } else if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
    cp = ((c & 0x07U) << 18U) | (at(1) << 12U) | (at(2) << 6U) | at(3);
    n = 4;
  }
  i += n;
  return cp;
}

struct Glyph {
  bool ink = false;
  float advance = 0;  // em
  Quad quad;
};

}  // namespace

struct Font::Impl {
  std::vector<std::byte> bytes;  // FreeType reads the face from these for as long as it lives
  FT_Library lib = nullptr;
  FT_Face face = nullptr;
  float em_px = kSdfPixels;  // pixels per em at the atlas size
  int limit = 4096;
  Atlas atlas;
  int shelf_x = 0, shelf_y = 0, shelf_h = 0;
  std::unordered_map<std::uint32_t, Glyph> glyphs;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    if (face != nullptr) {
      FT_Done_Face(face);
    }
    if (lib != nullptr) {
      FT_Done_FreeType(lib);
    }
  }

  void set_axes(const FontOptions& o) const {
    FT_MM_Var* mm = nullptr;
    if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &mm) != 0 || mm == nullptr) {
      return;
    }
    std::vector<FT_Fixed> coords(mm->num_axis);
    const std::span<const FT_Var_Axis> axes(mm->axis, mm->num_axis);
    for (std::size_t a = 0; a < axes.size(); ++a) {
      const FT_ULong tag = axes[a].tag;
      double v = static_cast<double>(axes[a].def) / 65536.0;
      if (tag == FT_MAKE_TAG('w', 'd', 't', 'h')) {
        v = o.width;
      } else if (tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
        v = o.weight;
      }
      v = std::clamp(v, static_cast<double>(axes[a].minimum) / 65536.0, static_cast<double>(axes[a].maximum) / 65536.0);
      coords[a] = static_cast<FT_Fixed>(std::lround(v * 65536.0));
    }
    (void)FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords.data());
    (void)FT_Done_MM_Var(lib, mm);
  }

  /// Room for a w × h bitmap in the atlas (shelves; the atlas doubles in height, then width, up to the limit).
  bool place(int w, int h, int& x, int& y) {
    for (;;) {
      if (shelf_x + w + kPad > atlas.width) {
        shelf_y += shelf_h + kPad;
        shelf_x = 0;
        shelf_h = 0;
      }
      if (shelf_y + h + kPad <= atlas.height && w + kPad <= atlas.width) {
        x = shelf_x;
        y = shelf_y;
        shelf_x += w + kPad;
        shelf_h = std::max(shelf_h, h);
        return true;
      }
      if (atlas.height < limit) {  // grow down: existing texels keep their place (and their uv, rescaled below)
        const int old_h = atlas.height;
        atlas.height = std::min(limit, atlas.height * 2);
        atlas.texels.resize(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height), 0);
        const float k = static_cast<float>(old_h) / static_cast<float>(atlas.height);
        for (auto& [cp, g] : glyphs) {
          g.quad.v0 *= k;
          g.quad.v1 *= k;
        }
        continue;
      }
      return false;  // full: the glyph is drawn as nothing
    }
  }

  const Glyph& glyph(std::uint32_t cp) {
    const auto it = glyphs.find(cp);
    if (it != glyphs.end()) {
      return it->second;
    }
    Glyph g;
    const FT_UInt index = FT_Get_Char_Index(face, cp);
    if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0) {
      g.advance = static_cast<float>(face->glyph->advance.x) / 64.0F / em_px;
      if (face->glyph->outline.n_points > 0 &&
          // FreeType's bitmap-to-SDF path ("bsdf": coverage first, then its distance field) is several times faster
          // than the outline one ("sdf"), which dominated opening a drawing
          FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0 &&
          FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) == 0) {
        const FT_Bitmap& bm = face->glyph->bitmap;
        const int w = static_cast<int>(bm.width);
        const int h = static_cast<int>(bm.rows);
        int x = 0;
        int y = 0;
        if (w > 0 && h > 0 && bm.pitch >= w && bm.buffer != nullptr && place(w, h, x, y)) {
          const std::span<const unsigned char> src(bm.buffer,
                                                   static_cast<std::size_t>(bm.pitch) * static_cast<std::size_t>(h));
          for (int r = 0; r < h; ++r) {
            const auto from = src.subspan(static_cast<std::size_t>(r) * static_cast<std::size_t>(bm.pitch),
                                          static_cast<std::size_t>(w));
            const auto to =
                static_cast<std::size_t>(y + r) * static_cast<std::size_t>(atlas.width) + static_cast<std::size_t>(x);
            std::ranges::copy(from, atlas.texels.begin() + static_cast<std::ptrdiff_t>(to));
          }
          const auto left = static_cast<float>(face->glyph->bitmap_left);
          const auto top = static_cast<float>(face->glyph->bitmap_top);
          g.ink = true;
          g.quad = Quad{left / em_px,
                        (top - static_cast<float>(h)) / em_px,
                        (left + static_cast<float>(w)) / em_px,
                        top / em_px,
                        static_cast<float>(x) / static_cast<float>(atlas.width),
                        static_cast<float>(y) / static_cast<float>(atlas.height),
                        static_cast<float>(x + w) / static_cast<float>(atlas.width),
                        static_cast<float>(y + h) / static_cast<float>(atlas.height)};
          ++atlas.generation;
        }
      }
    }
    return glyphs.emplace(cp, g).first->second;
  }
};

base::Result<std::unique_ptr<Font>> Font::load(std::vector<std::byte> bytes, const FontOptions& options) {
  auto impl = std::make_unique<Impl>();
  impl->bytes = std::move(bytes);
  impl->limit = std::clamp(options.texture_limit, 256, 16384);
  if (FT_Init_FreeType(&impl->lib) != 0) {
    return base::fail(base::Errc::invalid, "FreeType could not start");
  }
  const std::span<const std::uint8_t> data = base::as_u8(impl->bytes);
  if (FT_New_Memory_Face(impl->lib, data.data(), static_cast<FT_Long>(data.size()), 0, &impl->face) != 0) {
    return base::fail(base::Errc::invalid, "not a font FreeType can read");
  }
  impl->set_axes(options);
  if (FT_Set_Pixel_Sizes(impl->face, 0, kSdfPixels) != 0) {
    return base::fail(base::Errc::invalid, "the font has no scalable outlines");
  }
  impl->atlas.width = std::min(1024, impl->limit);
  impl->atlas.height = std::min(512, impl->limit);
  impl->atlas.texels.assign(static_cast<std::size_t>(impl->atlas.width) * static_cast<std::size_t>(impl->atlas.height),
                            0);
  impl->atlas.spread_em = 2.0F * kSpread / static_cast<float>(kSdfPixels);
  return std::unique_ptr<Font>(
      new Font(std::move(impl)));  // NOLINT(cppcoreguidelines-owning-memory) — private constructor
}

Font::Font(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Font::~Font() = default;

Line Font::layout(std::string_view utf8) {
  Line line;
  float pen = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const std::uint32_t cp = next_code_point(utf8, i);
    if (cp == '\n' || cp == '\r') {
      continue;
    }
    const Glyph& g = impl_->glyph(cp);
    if (g.ink) {
      line.glyphs.push_back(PlacedGlyph{pen, g.quad});
    }
    pen += g.advance;
  }
  line.width = pen;
  return line;
}

float Font::ascender() const noexcept {
  return static_cast<float>(impl_->face->ascender) / static_cast<float>(impl_->face->units_per_EM);
}
float Font::descender() const noexcept {
  return static_cast<float>(impl_->face->descender) / static_cast<float>(impl_->face->units_per_EM);
}
const Atlas& Font::atlas() const noexcept {
  return impl_->atlas;
}

}  // namespace cc::text
