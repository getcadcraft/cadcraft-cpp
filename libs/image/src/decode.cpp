// SPDX-License-Identifier: Apache-2.0
#include "cc/image/decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "wuffs.h"

namespace cc::image {

namespace {

/// PNG and JPEG only, decoded to straight RGBA.
class Callbacks final : public wuffs_aux::DecodeImageCallbacks {
 public:
  wuffs_base__image_decoder::unique_ptr SelectDecoder(std::uint32_t fourcc, wuffs_base__slice_u8 prefix_data,
                                                      bool prefix_closed) override {
    if (fourcc != WUFFS_BASE__FOURCC__PNG && fourcc != WUFFS_BASE__FOURCC__JPEG) {
      return {nullptr};
    }
    return wuffs_aux::DecodeImageCallbacks::SelectDecoder(fourcc, prefix_data, prefix_closed);
  }
  wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config& /*config*/) override {
    return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
  }
};

}  // namespace

std::optional<Rgba> decode_picture(std::span<const std::uint8_t> bytes, double max_pixels, std::uint32_t max_dim) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  Callbacks cb;
  wuffs_aux::sync_io::MemoryInput input(bytes.data(), bytes.size());
  wuffs_aux::DecodeImageResult r = wuffs_aux::DecodeImage(
      cb, input, wuffs_aux::DecodeImageArgQuirks::DefaultValue(), wuffs_aux::DecodeImageArgFlags::DefaultValue(),
      wuffs_aux::DecodeImageArgPixelBlend::DefaultValue(), wuffs_aux::DecodeImageArgBackgroundColor::DefaultValue(),
      wuffs_aux::DecodeImageArgMaxInclDimension(max_dim));
  if (!r.error_message.empty()) {
    return std::nullopt;
  }
  const std::uint32_t w = r.pixbuf.pixcfg.width();
  const std::uint32_t h = r.pixbuf.pixcfg.height();
  if (w == 0 || h == 0 || static_cast<double>(w) * h > max_pixels) {
    return std::nullopt;
  }
  const wuffs_base__table_u8 t = r.pixbuf.plane(0);
  if (t.width < std::size_t{w} * 4 || t.height < h) {
    return std::nullopt;
  }
  const std::span<const std::uint8_t> plane(t.ptr, (t.height - 1) * t.stride + t.width);
  Rgba out{.w = w, .h = h, .px = {}};
  out.px.reserve(std::size_t{w} * h * 4);
  for (std::size_t y = 0; y < h; ++y) {
    const auto row = plane.subspan(y * t.stride, std::size_t{w} * 4);
    out.px.insert(out.px.end(), row.begin(), row.end());
  }
  return out;
}

Rgba fit_rgba(const Rgba& img, std::uint32_t max_dim) {
  const std::uint32_t big = std::max(img.w, img.h);
  if (max_dim == 0 || big <= max_dim) {
    return img;
  }
  const std::uint64_t w2 = std::max<std::uint64_t>(1, std::uint64_t{img.w} * max_dim / big);
  const std::uint64_t h2 = std::max<std::uint64_t>(1, std::uint64_t{img.h} * max_dim / big);
  Rgba out{.w = static_cast<std::uint32_t>(w2), .h = static_cast<std::uint32_t>(h2), .px = {}};
  out.px.resize(static_cast<std::size_t>(w2 * h2 * 4));
  for (std::uint64_t y = 0; y < h2; ++y) {
    const std::uint64_t y0 = y * img.h / h2;
    const std::uint64_t y1 = std::max(y0 + 1, (y + 1) * img.h / h2);
    for (std::uint64_t x = 0; x < w2; ++x) {
      const std::uint64_t x0 = x * img.w / w2;
      const std::uint64_t x1 = std::max(x0 + 1, (x + 1) * img.w / w2);
      std::array<std::uint64_t, 4> sum{};
      for (std::uint64_t yy = y0; yy < y1; ++yy) {
        for (std::uint64_t xx = x0; xx < x1; ++xx) {
          for (std::size_t c = 0; c < 4; ++c) {
            sum[c] += img.px[static_cast<std::size_t>((yy * img.w + xx) * 4) + c];
          }
        }
      }
      const std::uint64_t n = (y1 - y0) * (x1 - x0);
      for (std::size_t c = 0; c < 4; ++c) {
        out.px[static_cast<std::size_t>((y * w2 + x) * 4) + c] = static_cast<std::uint8_t>(sum[c] / n);
      }
    }
  }
  return out;
}

}  // namespace cc::image
