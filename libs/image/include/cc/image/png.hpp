// SPDX-License-Identifier: Apache-2.0
// cc::image — L3. PNG encoding for screenshots and exports (zlib for the compression). Decoding untrusted images is not
// here: that waits for wuffs (AGENTS.md section 3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cc/base/error.hpp"

namespace cc::image {

/// RGBA, 8 bits per channel, rows top to bottom, `width * 4` bytes per row.
[[nodiscard]] base::Result<std::vector<std::byte>> encode_png(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                              std::uint32_t height);

/// zlib's compress (the zlib format, level 6): what PDF's /FlateDecode reads. Empty when zlib refuses.
[[nodiscard]] std::vector<std::uint8_t> zlib_compress(std::span<const std::uint8_t> data);

}  // namespace cc::image
