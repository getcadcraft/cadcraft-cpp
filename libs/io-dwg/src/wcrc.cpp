// SPDX-License-Identifier: Apache-2.0
// crc.js (writing half) in C++ (see wcrc.hpp).
#include "wcrc.hpp"

#include <algorithm>

namespace cc::dwg::w {

namespace {

constexpr std::uint64_t kPolyN = 0x42F0E1EBA9EA3693ULL;
constexpr std::uint64_t kPolyM = 0x95AC9329AC4BC9B5ULL;

constexpr std::array<std::uint64_t, 256> make_t64n() {
  std::array<std::uint64_t, 256> t{};
  for (std::size_t i = 0; i < 256; ++i) {
    std::uint64_t c = static_cast<std::uint64_t>(i) << 56U;
    for (int k = 0; k < 8; ++k) {
      c = (c & (1ULL << 63U)) != 0 ? (c << 1U) ^ kPolyN : c << 1U;
    }
    t[i] = c;
  }
  return t;
}
constexpr std::array<std::uint64_t, 256> make_t64m() {
  std::array<std::uint64_t, 256> t{};
  for (std::size_t i = 0; i < 256; ++i) {
    std::uint64_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? (c >> 1U) ^ kPolyM : c >> 1U;
    }
    t[i] = c;
  }
  return t;
}
constexpr auto kT64N = make_t64n();
constexpr auto kT64M = make_t64m();

/// 5.12 byte order within a block of n bytes
constexpr std::array<std::array<std::uint8_t, 8>, 9> kOrder = {{{},
                                                                {0},
                                                                {0, 1},
                                                                {0, 1, 2},
                                                                {2, 3, 0, 1},
                                                                {2, 3, 0, 1, 4},
                                                                {2, 3, 0, 1, 4, 5},
                                                                {2, 3, 0, 1, 4, 5, 6},
                                                                {6, 7, 4, 5, 2, 3, 0, 1}}};

std::uint64_t crc64(CBytes b, std::uint64_t c, bool mirrored) {
  const std::size_t n = b.size();
  std::size_t p = 0;
  const auto step = [&](std::uint8_t x) {
    if (mirrored) {
      c = kT64M[(c ^ x) & 0xFFU] ^ (c >> 8U);
    } else {
      c = kT64N[(x ^ (c >> 56U)) & 0xFFU] ^ (c << 8U);
    }
  };
  while (n - p >= 8) {
    for (std::size_t k = 0; k < 8; ++k) {
      step(b[p + kOrder[8][k]]);
    }
    p += 8;
  }
  const std::size_t r = n - p;
  for (std::size_t k = 0; k < r; ++k) {
    step(b[p + kOrder[r][k]]);
  }
  return mirrored ? c : ~c;
}

}  // namespace

std::uint32_t r2007_page_checksum(std::uint32_t seed, CBytes b) {
  std::size_t length = b.size();
  const std::uint64_t s = (static_cast<std::uint64_t>(seed) + length) * 0x343FDULL + 0x269EC3ULL;
  std::uint32_t s1 = s & 0xFFFFU;
  std::uint32_t s2 = (s >> 16U) & 0xFFFFU;
  std::size_t p = 0;
  const auto one = [&](std::size_t k) {
    s1 += b[k];
    s2 += s1;
  };
  const auto two = [&](std::size_t k) {
    one(k);
    one(k + 1);
  };
  while (length != 0) {
    const std::size_t big = std::min<std::size_t>(0x15B0, length);
    length -= big;
    for (std::size_t small = big >> 3U; small > 0; --small) {
      two(p + 6);
      two(p + 4);
      two(p + 2);
      two(p);
      p += 8;
    }
    const std::size_t rem = big & 7U;
    switch (rem) {
      case 1: one(p); break;
      case 2: two(p); break;
      case 3:
        two(p);
        one(p + 2);
        break;
      case 4:
        two(p + 2);
        two(p);
        break;
      case 5:
        two(p + 2);
        two(p);
        one(p + 4);
        break;
      case 6:
        two(p + 2);
        two(p);
        two(p + 4);
        break;
      case 7:
        two(p + 2);
        two(p);
        two(p + 4);
        one(p + 6);
        break;
      default: break;
    }
    p += rem;
    s1 %= 0xFFF1U;
    s2 %= 0xFFF1U;
  }
  return (s2 << 16U) | (s1 & 0xFFFFU);
}

std::uint64_t crc64_normal(CBytes b, std::uint64_t seed) {
  return crc64(b, seed, false);
}
std::uint64_t crc64_mirrored(CBytes b, std::uint64_t seed) {
  return crc64(b, seed, true);
}

std::uint64_t update_seed1(std::uint64_t seed, std::uint64_t len) {
  std::uint64_t s = (seed + len) * 0x343FDULL + 0x269EC3ULL;
  s = s | (s * (0x343FDULL << 32U) + (0x269EC3ULL << 32U));
  return ~s;
}
std::uint64_t update_seed2(std::uint64_t seed, std::uint64_t len) {
  std::uint64_t s = (seed + len) * 0x343FDULL + 0x269EC3ULL;
  s = s * ((1ULL << 32U) + 0x343FDULL) + (len + 0x269EC3ULL);
  return ~s;
}

std::vector<std::uint8_t> r2004_magic(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  std::uint32_t s = 1;
  for (std::size_t i = 0; i < n; ++i) {
    s = s * 0x343FDU + 0x269EC3U;
    out[i] = static_cast<std::uint8_t>((s >> 16U) & 0xFFU);
  }
  return out;
}

CrcRandom::CrcRandom(std::uint64_t s) : seed(s) {
  table_[0] = static_cast<std::uint32_t>(s & 0xFFFFFFFFU) * 0x343FDU + 0x269EC3U;
  table_[1] = static_cast<std::uint32_t>((s >> 32U) & 0xFFFFFFFFU) * 0x343FDU + 0x269EC3U;
  std::uint32_t v = table_[1];
  for (std::uint32_t i = 2; i < 0x270; ++i) {
    v = ((v >> 30U) ^ v) * 0x6C078965U + i;
    table_[i] = v;
  }
  for (std::size_t i = 0; i < 0x80; ++i) {
    if (index_ >= 0x270) {
      index_ = 0;
    }
    const std::uint32_t x = table_[index_];
    for (unsigned k = 0; k < 4; ++k) {
      padding[i * 4 + k] = static_cast<std::uint8_t>((x >> (8U * k)) & 0xFFU);
    }
    ++index_;
  }
}

std::uint64_t CrcRandom::next64() {
  index_ += 2;
  if (index_ >= 0x270) {
    index_ = 0;
  }
  const std::uint64_t lo = table_[index_];
  const std::uint64_t hi = table_[(index_ + 1) % 0x270];
  return (hi << 32U) | lo;
}

std::uint64_t CrcRandom::encode(std::uint32_t value) {
  const std::uint64_t r = next64();
  auto lo = static_cast<std::uint32_t>(r & 0xDF7DF7DFULL);
  auto hi = static_cast<std::uint32_t>((r >> 32U) & 0xF7DF7DF7ULL);
  if ((value & 0x200U) != 0) {
    lo |= 0x20U;
  }
  if ((value & 0x100U) != 0) {
    lo |= 0x800U;
  }
  if ((value & 0x80U) != 0) {
    lo |= 0x20000U;
  }
  if ((value & 0x40U) != 0) {
    lo |= 0x800000U;
  }
  if ((value & 0x20U) != 0) {
    lo |= 0x20000000U;
  }
  if ((value & 0x10U) != 0) {
    hi |= 0x08U;
  }
  if ((value & 0x8U) != 0) {
    hi |= 0x200U;
  }
  if ((value & 0x4U) != 0) {
    hi |= 0x8000U;
  }
  if ((value & 0x2U) != 0) {
    hi |= 0x200000U;
  }
  if ((value & 0x1U) != 0) {
    hi |= 0x8000000U;
  }
  return (static_cast<std::uint64_t>(hi) << 32U) | lo;
}

}  // namespace cc::dwg::w
