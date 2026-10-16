// SPDX-License-Identifier: Apache-2.0
// Drives ByteReader and BitReader with a script taken from the input itself: each byte picks an operation.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cc/base/reader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  cc::base::ByteReader br{bytes};
  cc::base::BitReader bits{bytes};
  std::vector<std::uint8_t> buf;
  for (std::size_t i = 0; i < size; ++i) {
    const auto op = std::to_integer<unsigned>(bytes[i]) & 7U;
    try {
      switch (op) {
        case 0: (void)bits.bit(); break;
        case 1: (void)bits.bits2(); break;
        case 2: (void)bits.rc(); break;
        case 3: (void)bits.bits(std::to_integer<unsigned>(bytes[i]) >> 3U); break;
        case 4: bits.raw(std::to_integer<std::size_t>(bytes[i]) >> 3U, buf); break;
        case 5: (void)br.read_u32_le(); break;
        case 6: (void)br.read_f64_le(); break;
        default: (void)br.skip(std::to_integer<std::size_t>(bytes[i]) >> 3U); break;
      }
    } catch (const cc::base::ReadOverrun&) {
      bits.set_pos(0);
    }
  }
  return 0;
}
