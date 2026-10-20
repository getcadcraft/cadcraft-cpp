// SPDX-License-Identifier: Apache-2.0
// cc::image — a picture's pixels for the screen (0.5 draws them with the browser's createImageBitmap): PNG and JPEG
// through wuffs, which is memory-safe by construction, as RGBA (straight alpha, rows from the top). Untrusted input:
// anything else, damaged or over the limits gives nothing.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cc::image {

struct Rgba {
  std::uint32_t w = 0, h = 0;
  std::vector<std::uint8_t> px;  // w × h × 4
};
/// The picture's pixels (PNG or JPEG, by content), at most `max_pixels` and `max_dim` on a side; else nothing.
[[nodiscard]] std::optional<Rgba> decode_picture(std::span<const std::uint8_t> bytes, double max_pixels = 100e6,
                                                 std::uint32_t max_dim = 65535);
/// The picture box-filtered to fit `max_dim` on its longer side (a GPU's largest texture), or itself when it does.
[[nodiscard]] Rgba fit_rgba(const Rgba& img, std::uint32_t max_dim);

}  // namespace cc::image
