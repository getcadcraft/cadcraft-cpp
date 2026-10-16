// SPDX-License-Identifier: Apache-2.0
#include "cc/image/png.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "cc/base/checked.hpp"
#include "cc/base/error.hpp"

namespace cc::image {

namespace {

void put32(std::vector<std::byte>& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(s)) & 0xFFU));
  }
}

void chunk(std::vector<std::byte>& out, std::string_view type, std::span<const std::uint8_t> data) {
  put32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  for (const char c : type) {
    out.push_back(static_cast<std::byte>(c));
  }
  for (const std::uint8_t b : data) {
    out.push_back(static_cast<std::byte>(b));
  }
  uLong crc = crc32(0L, Z_NULL, 0);
  for (std::size_t i = start; i < out.size(); ++i) {
    const auto b = std::to_integer<Bytef>(out[i]);
    crc = crc32(crc, &b, 1);
  }
  put32(out, static_cast<std::uint32_t>(crc));
}

}  // namespace

base::Result<std::vector<std::byte>> encode_png(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                std::uint32_t height) {
  const auto row = base::checked_mul<std::size_t>(width, 4);
  if (!row || width == 0 || height == 0 || width > 32768 || height > 32768) {
    return base::fail(base::Errc::too_big, "image size out of range");
  }
  if (rgba.size() != *row * height) {
    return base::fail(base::Errc::invalid, "pixel buffer does not match the image size");
  }
  // scanlines with the Sub filter (1): each byte minus the byte four to its left; compresses screenshots well
  std::vector<std::uint8_t> raw;
  raw.reserve((*row + 1) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto line = rgba.subspan(static_cast<std::size_t>(y) * *row, *row);
    raw.push_back(1);
    for (std::size_t x = 0; x < *row; ++x) {
      const std::uint8_t left = x >= 4 ? line[x - 4] : 0;
      raw.push_back(static_cast<std::uint8_t>(line[x] - left));
    }
  }
  uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
  std::vector<std::uint8_t> packed(packed_size);
  if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
    return base::fail(base::Errc::invalid, "zlib could not compress the image");
  }
  packed.resize(packed_size);
  std::vector<std::byte> out;
  for (const std::uint8_t b : std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}) {
    out.push_back(static_cast<std::byte>(b));
  }
  std::array<std::uint8_t, 13> ihdr{};
  for (int i = 0; i < 4; ++i) {
    ihdr.at(static_cast<std::size_t>(i)) = static_cast<std::uint8_t>(width >> static_cast<unsigned>(24 - 8 * i));
    ihdr.at(4 + static_cast<std::size_t>(i)) = static_cast<std::uint8_t>(height >> static_cast<unsigned>(24 - 8 * i));
  }
  ihdr[8] = 8;  // bits per channel
  ihdr[9] = 6;  // RGBA
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", packed);
  chunk(out, "IEND", {});
  return out;
}

}  // namespace cc::image
