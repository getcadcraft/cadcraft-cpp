// SPDX-License-Identifier: Apache-2.0
// The writing half of cc-dwg crc.js: the R2007 data page checksum (5.4.1), the 64-bit CRCs and their seed updaters
// (5.12), the CRC random encoder (5.11) and the R2004 magic sequence (4.1). The 16-bit CRC, CRC-32 and the R2004 page
// checksum are the reader's (container.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace cc::dwg::w {

using CBytes = std::span<const std::uint8_t>;

[[nodiscard]] std::uint32_t r2007_page_checksum(std::uint32_t seed, CBytes b);
[[nodiscard]] std::uint64_t crc64_normal(CBytes b, std::uint64_t seed);
[[nodiscard]] std::uint64_t crc64_mirrored(CBytes b, std::uint64_t seed);
[[nodiscard]] std::uint64_t update_seed1(std::uint64_t seed, std::uint64_t len);
[[nodiscard]] std::uint64_t update_seed2(std::uint64_t seed, std::uint64_t len);
[[nodiscard]] std::vector<std::uint8_t> r2004_magic(std::size_t n = 0x100);

/// 5.11 CrcRandom (+ r2007-write.js RandomEncoder's cycling padding reader).
class CrcRandom {
 public:
  explicit CrcRandom(std::uint64_t seed);
  std::uint64_t seed;
  std::array<std::uint8_t, 512> padding{};
  std::uint64_t next64();
  std::uint64_t encode(std::uint32_t value);
  std::uint8_t pad_byte() {
    const std::uint8_t b = padding[pad_pos_];
    pad_pos_ = (pad_pos_ + 1) % padding.size();
    return b;
  }

 private:
  std::array<std::uint32_t, 0x270> table_{};
  std::size_t index_ = 0;
  std::size_t pad_pos_ = 0;
};

}  // namespace cc::dwg::w
