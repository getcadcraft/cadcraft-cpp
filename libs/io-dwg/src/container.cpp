// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg container-read.js, container-util.js, r2007.js, lz-r2004.js and crc.js (reading). The spec gaps the
// JS found by experiment are kept with their notes.
#include "container.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "messages.hpp"

namespace cc::dwg {

namespace {

// ---------------------------------------------------------------- little-endian reads with the JS DataView's checks
[[noreturn]] void dv_range() {
  throw DecodeError("Offset is outside the bounds of the DataView");
}

std::uint32_t u8at(Bytes b, std::size_t at) {
  if (at >= b.size()) {
    dv_range();
  }
  return b[at];
}
std::uint32_t u16(Bytes b, std::size_t at) {
  if (at + 2 > b.size()) {
    dv_range();
  }
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8U);
}
std::uint32_t u32(Bytes b, std::size_t at) {
  if (at + 4 > b.size()) {
    dv_range();
  }
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8U) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16U) | (static_cast<std::uint32_t>(b[at + 3]) << 24U);
}
std::int32_t i32(Bytes b, std::size_t at) {
  return static_cast<std::int32_t>(u32(b, at));
}
std::uint64_t u64(Bytes b, std::size_t at) {
  return static_cast<std::uint64_t>(u32(b, at)) | (static_cast<std::uint64_t>(u32(b, at + 4)) << 32U);
}
std::int64_t i64(Bytes b, std::size_t at) {
  return static_cast<std::int64_t>(u64(b, at));
}

std::string hex_lower(std::uint32_t v) {
  std::string s = base::hex_upper(v);
  for (char& ch : s) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return s;
}

std::string ascii(Bytes b, std::size_t from, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n && from + i < b.size(); ++i) {
    s += static_cast<char>(b[from + i]);
  }
  return s;
}

/// container-util.js plausibleSize: sizes read from a damaged map can be absurd.
double plausible(double n, std::size_t file_length) {
  const double cap = std::max(static_cast<double>(1U << 26U), static_cast<double>(file_length) * 64);
  if (!(n >= 0) || n > cap) {
    std::string s;
    base::append_number(s, n);
    throw DecodeError("implausible size " + s + " (damaged map)");
  }
  return n;
}

// ---------------------------------------------------------------- 2.14 / 4.1
constexpr std::array<std::uint16_t, 256> make_t16() {
  std::array<std::uint16_t, 256> t{};
  for (unsigned i = 0; i < 256; ++i) {
    unsigned c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? (c >> 1U) ^ 0xA001U : c >> 1U;
    }
    t[i] = static_cast<std::uint16_t>(c);
  }
  return t;
}
constexpr auto kT16 = make_t16();
constexpr std::array<std::uint32_t, 256> make_t32() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? (c >> 1U) ^ 0xEDB88320U : c >> 1U;
    }
    t[i] = c;
  }
  return t;
}
constexpr auto kT32 = make_t32();

std::vector<std::uint8_t> r2004_magic(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  std::uint32_t s = 1;
  for (std::size_t i = 0; i < n; ++i) {
    s = s * 0x343FDU + 0x269EC3U;
    out[i] = static_cast<std::uint8_t>((s >> 16U) & 0xFFU);
  }
  return out;
}

/// 2.14.1: the R13–R15 file header CRC is XORed with a value chosen by the number of section locators.
std::uint32_t r13_header_xor(std::uint32_t n) {
  switch (n) {
    case 3: return 0xA598;
    case 4: return 0x8101;
    case 5: return 0x3CC4;
    case 6: return 0x8461;
    default: return 0;
  }
}

}  // namespace

std::uint32_t crc8(std::uint32_t seed, Bytes b) {
  std::uint32_t dx = seed & 0xFFFFU;
  for (const std::uint8_t c : b) {
    dx = (dx >> 8U) ^ kT16[(c ^ dx) & 0xFFU];
  }
  return dx;
}

std::uint32_t crc32(Bytes b, std::uint32_t seed) {
  std::uint32_t c = ~seed;
  for (const std::uint8_t x : b) {
    c = (c >> 8U) ^ kT32[(c ^ x) & 0xFFU];
  }
  return ~c;
}

std::uint32_t page_checksum(std::uint32_t seed, Bytes b) {
  std::uint32_t s1 = seed & 0xFFFFU;
  std::uint32_t s2 = (seed >> 16U) & 0xFFFFU;
  std::size_t i = 0;
  while (i < b.size()) {
    const std::size_t end = std::min(b.size(), i + 0x15B0);
    for (; i < end; ++i) {
      s1 += b[i];
      s2 += s1;
    }
    s1 %= 0xFFF1U;
    s2 %= 0xFFF1U;
  }
  return (s2 << 16U) | (s1 & 0xFFFFU);
}

int version_num(std::string_view v) {
  if (v == "AC1012") {
    return 12;
  }
  if (v == "AC1014") {
    return 14;
  }
  if (v == "AC1015") {
    return 15;
  }
  if (v == "AC1018") {
    return 18;
  }
  if (v == "AC1021") {
    return 21;
  }
  if (v == "AC1024") {
    return 24;
  }
  if (v == "AC1027") {
    return 27;
  }
  if (v == "AC1032") {
    return 32;
  }
  return 0;
}

// ---------------------------------------------------------------- 4.7 R2004 LZ77
namespace {

std::vector<std::uint8_t> lz2004(Bytes src, std::size_t from, std::size_t to, std::size_t out_size) {
  std::vector<std::uint8_t> out(out_size);
  std::size_t ip = from;
  std::size_t op = 0;
  auto next = [&]() -> std::uint32_t {
    if (ip >= src.size()) {
      throw DecodeError(msg("ccdwg.err.lz_opcode", {{"op", "undefined"}}));
    }
    return src[ip++];
  };
  auto literal_length = [&]() -> std::size_t {
    if (ip >= src.size()) {
      return 0;
    }
    const std::uint32_t b = src[ip];
    if (b == 0) {
      ++ip;
      std::size_t total = 0x0F;
      std::uint32_t c = 0;
      while ((c = next()) == 0) {
        total += 0xFF;
      }
      return total + c + 3;
    }
    if ((b & 0xF0U) != 0) {
      return 0;
    }
    ++ip;
    return b + 3;
  };
  auto long_count = [&]() -> std::size_t {
    std::uint32_t c = next();
    if (c != 0) {
      return c;
    }
    std::size_t total = 0xFF;
    while ((c = next()) == 0) {
      total += 0xFF;
    }
    return total + c;
  };
  auto copy_lit = [&](std::size_t n) {
    const std::size_t k = std::min(n, out_size - op);
    const std::size_t avail = ip < src.size() ? std::min(k, src.size() - ip) : 0;
    std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(ip), avail, out.begin() + static_cast<std::ptrdiff_t>(op));
    ip += n;
    op += k;
  };
  auto copy_back = [&](std::size_t offset, std::size_t n) {
    if (offset + 1 > op) {
      throw DecodeError(msg("ccdwg.err.lz_before_start"));
    }
    std::size_t s = op - offset - 1;
    if (op + n > out_size) {
      n = out_size - op;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out[op++] = out[s++];
    }
  };

  copy_lit(literal_length());
  while (ip < to) {
    const std::uint32_t op1 = next();
    if (op1 == 0x11) {
      break;
    }
    std::size_t count = 0;
    std::size_t offset = 0;
    std::size_t lit = 0;
    if (op1 >= 0x40) {
      count = ((op1 & 0xF0U) >> 4U) - 1;
      const std::uint32_t op2 = next();
      offset = (op2 << 2U) | ((op1 & 0x0CU) >> 2U);
      lit = op1 & 3U;
    } else {
      if (op1 == 0x10) {
        count = long_count() + 9;
      } else if (op1 >= 0x12 && op1 <= 0x1F) {
        count = (op1 & 0x0FU) + 2;
      } else if (op1 == 0x20) {
        count = long_count() + 0x21;
      } else if (op1 >= 0x21) {
        count = op1 - 0x1E;
      } else {
        std::string hx;
        static constexpr std::string_view kHex = "0123456789abcdef";
        if (op1 >= 16) {
          hx += kHex[op1 >> 4U];
        }
        hx += kHex[op1 & 15U];
        throw DecodeError(msg("ccdwg.err.lz_opcode", {{"op", hx}}));
      }
      const std::uint32_t b1 = next();
      const std::uint32_t b2 = next();
      offset = (b1 >> 2U) | (b2 << 6U);
      lit = b1 & 3U;
      if (op1 < 0x20) {
        offset += 0x3FFF;
      }
    }
    copy_back(offset, count);
    if (lit == 0) {
      lit = literal_length();
    }
    copy_lit(lit);
    if (op >= out_size) {
      break;
    }
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> decompress_r2004(Bytes src, std::size_t out_size) {
  return lz2004(src, 0, src.size(), out_size);
}

// ---------------------------------------------------------------- 5.13 Reed-Solomon (decode)
namespace {

struct Field {
  std::array<std::uint8_t, 512> exp{};
  std::array<std::int16_t, 256> log{};
};
Field make_field(unsigned poly) {
  Field f;
  unsigned x = 1;
  for (int i = 0; i < 255; ++i) {
    f.exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
    f.log[x] = static_cast<std::int16_t>(i);
    x <<= 1U;
    if ((x & 0x100U) != 0) {
      x ^= poly;
    }
  }
  for (std::size_t i = 255; i < 512; ++i) {
    f.exp[i] = f.exp[i - 255];
  }
  f.log[0] = -1;
  return f;
}
const Field& rs_system() {
  static const Field f = make_field(0x169);
  return f;
}
const Field& rs_data() {
  static const Field f = make_field(0x11D);
  return f;
}

std::uint32_t gmul(const Field& f, std::uint32_t a, std::uint32_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  return f.exp[static_cast<std::size_t>(f.log[a] + f.log[b])];
}

using Word = std::array<std::uint8_t, 255>;

/// Syndromes of the reversed word; empty when all are zero.
std::vector<std::uint32_t> syndromes(const Field& f, const Word& c, int nk) {
  std::vector<std::uint32_t> s(static_cast<std::size_t>(nk) + 1);
  bool zero = true;
  for (int i = 1; i <= nk; ++i) {
    const std::uint32_t a = f.exp[static_cast<std::size_t>(i % 255)];
    std::uint32_t v = 0;
    for (std::size_t j = 0; j < 255; ++j) {
      v = gmul(f, v, a) ^ c[j];
    }
    s[static_cast<std::size_t>(i)] = v;
    if (v != 0) {
      zero = false;
    }
  }
  if (zero) {
    s.clear();
  }
  return s;
}

struct RsResult {
  bool ok = true;
  int corrected = 0;
};

RsResult rs_decode_reversed(Word& c, int k, bool sys) {
  const Field& f = sys ? rs_system() : rs_data();
  const int nk = 255 - k;
  const std::vector<std::uint32_t> s = syndromes(f, c, nk);
  if (s.empty()) {
    return {};
  }
  std::vector<std::uint32_t> lam{1};
  std::vector<std::uint32_t> bb{1};
  int l = 0;
  int m = 1;
  std::uint32_t b = 1;
  auto at = [](const std::vector<std::uint32_t>& v, std::size_t i) { return i < v.size() ? v[i] : 0U; };
  for (int n = 0; n < nk; ++n) {
    std::uint32_t d = s[static_cast<std::size_t>(n) + 1];
    for (int i = 1; i <= l; ++i) {
      d ^= gmul(f, at(lam, static_cast<std::size_t>(i)), s[static_cast<std::size_t>(n + 1 - i)]);
    }
    if (d == 0) {
      ++m;
      continue;
    }
    const std::uint32_t coef = gmul(f, d, f.exp[static_cast<std::size_t>((255 - f.log[b]) % 255)]);
    const std::vector<std::uint32_t> t = lam;
    const std::size_t need = bb.size() + static_cast<std::size_t>(m);
    while (lam.size() < need) {
      lam.push_back(0);
    }
    for (std::size_t i = 0; i < bb.size(); ++i) {
      lam[i + static_cast<std::size_t>(m)] ^= gmul(f, coef, bb[i]);
    }
    if (2 * l <= n) {
      l = n + 1 - l;
      bb = t;
      b = d;
      m = 1;
    } else {
      ++m;
    }
  }
  std::vector<std::size_t> errs;
  for (std::size_t j = 0; j < 255; ++j) {
    const std::uint32_t xinv = f.exp[(255 - (254 - j)) % 255];
    std::uint32_t v = 0;
    for (std::size_t i = lam.size(); i-- > 0;) {
      v = gmul(f, v, xinv) ^ lam[i];
    }
    if (v == 0) {
      errs.push_back(j);
    }
  }
  if (std::cmp_not_equal(errs.size(), l) || l > nk / 2) {
    return {false, 0};
  }
  std::vector<std::uint32_t> om(static_cast<std::size_t>(nk), 0);
  for (int i = 0; i < nk; ++i) {
    for (int j = 0; j <= i && static_cast<std::size_t>(j) < lam.size(); ++j) {
      om[static_cast<std::size_t>(i)] ^=
          gmul(f, lam[static_cast<std::size_t>(j)], s[static_cast<std::size_t>(i) - static_cast<std::size_t>(j) + 1]);
    }
  }
  for (const std::size_t j : errs) {
    const std::uint32_t xinv = f.exp[(255 - (254 - j)) % 255];
    std::uint32_t num = 0;
    for (int i = nk - 1; i >= 0; --i) {
      num = gmul(f, num, xinv) ^ om[static_cast<std::size_t>(i)];
    }
    std::uint32_t den = 0;
    for (std::size_t i = 1; i < lam.size(); i += 2) {
      den ^= gmul(f, lam[i], f.exp[static_cast<std::size_t>((f.log[xinv] * static_cast<int>(i - 1)) % 255)]);
    }
    if (den == 0) {
      return {false, 0};
    }
    const std::uint32_t e = gmul(f, num, f.exp[static_cast<std::size_t>((255 - f.log[den]) % 255)]);
    c[j] = static_cast<std::uint8_t>(c[j] ^ e);
  }
  if (!syndromes(f, c, nk).empty()) {
    return {false, 0};
  }
  return {true, static_cast<int>(errs.size())};
}

struct RsStats {
  int bad = 0;
  int corrected = 0;
};

/// Interleaved codewords: byte i of block b at position i*blocks + b; returns the data bytes block after block.
std::vector<std::uint8_t> deinterleave(Bytes src, std::size_t from, std::size_t blocks, int k, bool sys,
                                       RsStats* stats) {
  const auto ku = static_cast<std::size_t>(k);
  std::vector<std::uint8_t> out(blocks * ku);
  Word cw{};
  for (std::size_t b = 0; b < blocks; ++b) {
    for (std::size_t i = 0; i < 255; ++i) {
      const std::size_t at = from + i * blocks + b;
      cw[i] = at < src.size() ? src[at] : 0;
    }
    if (stats != nullptr) {
      Word rev{};
      for (std::size_t i = 0; i < 255; ++i) {
        rev[i] = cw[254 - i];
      }
      const RsResult r = rs_decode_reversed(rev, k, sys);
      if (r.corrected != 0) {
        for (std::size_t i = 0; i < 255; ++i) {
          cw[i] = rev[254 - i];
        }
      }
      if (!r.ok) {
        ++stats->bad;
      } else {
        stats->corrected += r.corrected;
      }
    }
    std::copy_n(cw.begin(), ku, out.begin() + static_cast<std::ptrdiff_t>(b * ku));
  }
  return out;
}

// ---------------------------------------------------------------- 5.10 R2007 LZ77
using Order = std::vector<std::uint8_t>;
std::array<Order, 33> make_lit_order() {
  // 5.10.1: the copy order of a literal run of 1..32 bytes, as [count, source offset] blocks in output order.
  static const std::array<std::vector<std::pair<int, int>>, 33> kT = {{
      {},
      {{1, 0}},
      {{1, 1}, {1, 0}},
      {{1, 2}, {1, 1}, {1, 0}},
      {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
      {{1, 4}, {4, 0}},
      {{1, 5}, {4, 1}, {1, 0}},
      {{2, 5}, {4, 1}, {1, 0}},
      {{4, 0}, {4, 4}},
      {{1, 8}, {8, 0}},
      {{1, 9}, {8, 1}, {1, 0}},
      {{2, 9}, {8, 1}, {1, 0}},
      {{4, 8}, {8, 0}},
      {{1, 12}, {4, 8}, {8, 0}},
      {{1, 13}, {4, 9}, {8, 1}, {1, 0}},
      {{2, 13}, {4, 9}, {8, 1}, {1, 0}},
      {{8, 8}, {8, 0}},
      {{8, 9}, {1, 8}, {8, 0}},
      {{1, 17}, {16, 1}, {1, 0}},
      {{3, 16}, {16, 0}},
      {{4, 16}, {16, 0}},
      {{1, 20}, {4, 16}, {16, 0}},
      {{2, 20}, {4, 16}, {16, 0}},
      {{3, 20}, {4, 16}, {16, 0}},
      {{8, 16}, {16, 0}},
      {{8, 17}, {1, 16}, {16, 0}},
      {{1, 25}, {8, 17}, {1, 16}, {16, 0}},
      {{2, 25}, {8, 17}, {1, 16}, {16, 0}},
      {{4, 24}, {8, 16}, {16, 0}},
      {{1, 28}, {4, 24}, {8, 16}, {16, 0}},
      {{2, 28}, {4, 24}, {8, 16}, {16, 0}},
      {{1, 30}, {4, 26}, {8, 18}, {16, 2}, {2, 0}},
      {{16, 16}, {16, 0}},
  }};
  std::array<Order, 33> r{};
  // expand each block "k [i]" with the order of the k-byte copy itself
  // Recursion bounded by n ≤ 32 (each step expands a smaller block).
  // NOLINTNEXTLINE(misc-no-recursion)
  auto expand = [&](auto&& self, int n) -> Order {
    if (n == 1) {
      return {0};
    }
    Order out;
    for (const auto& [k, i] : kT[static_cast<std::size_t>(n)]) {
      if (k == n) {
        for (int j = 0; j < k; ++j) {
          out.push_back(static_cast<std::uint8_t>(i + j));
        }
        continue;
      }
      for (const std::uint8_t j : self(self, k)) {
        out.push_back(static_cast<std::uint8_t>(i + j));
      }
    }
    return out;
  };
  for (int n = 1; n <= 32; ++n) {
    r[static_cast<std::size_t>(n)] = expand(expand, n);
  }
  return r;
}
const std::array<Order, 33>& lit_order() {
  static const std::array<Order, 33> t = make_lit_order();
  return t;
}

std::vector<std::uint8_t> lz21(Bytes src, std::size_t from, std::size_t len, std::size_t out_size) {
  std::vector<std::uint8_t> out(out_size);
  const std::size_t end = from + len;
  std::size_t ip = from;
  std::size_t op = 0;
  auto get = [&](std::size_t i) -> std::uint32_t { return i < src.size() ? src[i] : 0U; };
  auto lit_copy = [&](std::size_t n) {
    while (n > 0) {
      const std::size_t k = std::min<std::size_t>(32, n);
      const Order& ord = lit_order()[k];
      for (std::size_t j = 0; j < k && op < out_size; ++j) {
        out[op++] = static_cast<std::uint8_t>(get(ip + ord[j]));
      }
      ip += k;
      n -= k;
    }
  };
  auto lit_length = [&](std::uint32_t oc) -> std::size_t {
    std::size_t length = oc + 8;
    if (length == 0x17) {
      std::uint32_t n = get(ip++);
      length += n;
      if (n == 0xFF) {
        do {
          n = get(ip) | (get(ip + 1) << 8U);
          ip += 2;
          length += n;
        } while (n == 0xFFFF);
      }
    }
    return length;
  };
  std::size_t length = 0;
  std::uint32_t oc = get(ip);
  // Spec gap (r2007.js): the length is the 4th byte after the opcode.
  if ((oc >> 4U) == 2) {
    ip += 3;
    length = get(ip++) & 7U;
  }
  int pending = -1;
  while (ip < end && op < out_size) {
    if (length == 0) {
      const std::uint32_t c = pending >= 0 ? static_cast<std::uint32_t>(pending) : get(ip++);
      pending = -1;
      length = lit_length(c);
    }
    lit_copy(length);
    if (ip >= end) {
      break;
    }
    oc = get(ip++);
    std::size_t offset = 0;
    std::size_t count = 0;
    auto instr = [&]() {
      switch (oc >> 4U) {
        case 0:
          count = (oc & 0xFU) + 0x13;
          offset = get(ip++);
          oc = get(ip++);
          count += (oc >> 3U) & 0x10U;
          offset += ((oc & 0x78U) << 5U) + 1;
          break;
        case 1:
          count = (oc & 0xFU) + 3;
          offset = get(ip++);
          oc = get(ip++);
          offset += ((oc & 0xF8U) << 5U) + 1;
          break;
        case 2:
          offset = get(ip++);
          offset |= (get(ip++) << 8U) & 0xFF00U;
          count = oc & 7U;
          if ((oc & 8U) == 0) {
            oc = get(ip++);
            count += oc & 0xF8U;
          } else {
            ++offset;
            count += get(ip++) << 3U;
            oc = get(ip++);
            count += ((oc & 0xF8U) << 8U) + 0x100;
          }
          break;
        default:
          count = oc >> 4U;
          offset = oc & 15U;
          oc = get(ip++);
          offset += ((oc & 0xF8U) << 1U) + 1;
      }
    };
    instr();
    for (;;) {
      if (offset > op) {
        throw DecodeError(msg("ccdwg.err.r2007_lz"));
      }
      std::size_t s = op - offset;
      for (std::size_t i = 0; i < count && op < out_size; ++i) {
        out[op++] = out[s++];
      }
      length = oc & 7U;
      if (length != 0 || ip >= end) {
        break;
      }
      oc = get(ip++);
      if ((oc >> 4U) == 0) {
        pending = static_cast<int>(oc);
        break;
      }
      if ((oc >> 4U) == 15) {
        oc &= 15U;
      }
      instr();
    }
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> decompress_r21(Bytes src, std::size_t out_size) {
  return lz21(src, 0, src.size(), out_size);
}

// ---------------------------------------------------------------- R13–R15 (ch. 3)
namespace {

Container read_r13(Bytes b, const std::string& version, int ver) {
  Container c;
  c.version = version;
  c.ver = ver;
  c.maintenance = static_cast<int>(u8at(b, 0x0B));
  c.codepage_num = static_cast<int>(u16(b, 0x13));
  const std::uint32_t n = u32(b, 0x15);
  if (n > 16) {
    throw Unsupported(msg("ccdwg.open.r13_header", {{"n", n}}));
  }
  struct Loc {
    std::uint32_t at = 0;
    std::uint32_t size = 0;
    bool set = false;
  };
  std::array<Loc, 256> loc{};
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::size_t at = 0x19 + static_cast<std::size_t>(i) * 9;
    Loc& l = loc[u8at(b, at)];
    l.at = u32(b, at + 1);
    l.size = u32(b, at + 5);
    l.set = true;
  }
  const std::size_t crc_at = 0x19 + static_cast<std::size_t>(n) * 9;
  const std::uint32_t want = u16(b, crc_at);
  const std::uint32_t got = crc8(0, b.first(crc_at)) ^ r13_header_xor(n);
  if (want != got) {
    c.notes.push_back(msg("ccdwg.read.file_header_crc"));
  }
  auto sec = [&](int k) -> Section {
    const Loc& l = loc[static_cast<std::size_t>(k)];
    if (l.set && static_cast<std::uint64_t>(l.at) + l.size <= b.size()) {
      return Section{{}, b.subspan(l.at, l.size)};
    }
    return Section{};
  };
  const Section header = sec(0);
  const Section handles = sec(2);
  if (header.view.data() == nullptr || handles.view.data() == nullptr) {
    throw Unsupported(msg("ccdwg.open.r13_locators"));
  }
  c.sections["AcDb:Header"] = header;
  if (Section s = sec(1); s.view.data() != nullptr) {
    c.sections["AcDb:Classes"] = s;
  }
  c.sections["AcDb:Handles"] = handles;
  c.sections["AcDb:AcDbObjects"] = Section{{}, b};
  if (loc[3].set) {
    c.sections["AcDb:ObjFreeSpace"] = sec(3);
  }
  if (loc[4].set) {
    c.sections["AcDb:Template"] = sec(4);
  }
  if (loc[5].set) {
    c.sections["AcDb:AuxHeader"] = sec(5);
  }
  return c;
}

// ---------------------------------------------------------------- R2004 family (ch. 4, 6, 7, 8)
std::vector<std::uint8_t> read_system_page(Bytes b, std::uint64_t addr, std::uint32_t type, Container& c,
                                           const std::string& what) {
  if (addr + 0x14 > b.size()) {
    throw Unsupported(msg("ccdwg.open.past_end", {{"what", what}}));
  }
  const auto a = static_cast<std::size_t>(addr);
  const std::uint32_t t = u32(b, a);
  if (t != type) {
    throw Unsupported(msg("ccdwg.open.page_type", {{"what", what}, {"type", hex_lower(t)}}));
  }
  const auto usize = static_cast<std::size_t>(plausible(u32(b, a + 4), b.size()));
  const std::uint32_t csize = u32(b, a + 8);
  if (a + 0x14 + static_cast<std::uint64_t>(csize) > b.size()) {
    throw Unsupported(msg("ccdwg.open.runs_past", {{"what", what}}));
  }
  const std::uint32_t ctype = u32(b, a + 12);
  const std::uint32_t sum = u32(b, a + 16);
  std::array<std::uint8_t, 0x14> hdr{};
  std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(a), 0x14, hdr.begin());
  hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
  const std::uint32_t check = page_checksum(page_checksum(0, hdr), b.subspan(a + 0x14, csize));
  if (check != sum) {
    c.notes.push_back(msg("ccdwg.read.system_checksum", {{"what", what}}));
  }
  if (ctype == 2) {
    return lz2004(b, a + 0x14, a + 0x14 + csize, usize);
  }
  // b.slice(addr + 0x14, addr + 0x14 + usize): clipped at the end of the file
  const std::size_t from = a + 0x14;
  const std::size_t n = std::min(usize, b.size() - from);
  return {b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(from + n)};
}

struct PageLoc {
  std::uint64_t addr = 0;
  std::uint64_t size = 0;
};
struct PageRef {
  std::uint32_t num = 0;
  std::uint32_t data_size = 0;
  std::uint64_t start = 0;
};

std::vector<std::uint8_t> read_data_section(Bytes b, const std::map<std::int64_t, PageLoc>& pages,
                                            const std::vector<PageRef>& list, double size, double max_size,
                                            bool compressed, const std::string& name, Container& c) {
  plausible(size, b.size());
  plausible(max_size, b.size());
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  int bad = 0;
  for (const PageRef& pg : list) {
    const auto it = pages.find(pg.num);
    if (it == pages.end()) {
      ++bad;
      continue;
    }
    const std::uint64_t at = it->second.addr;
    if (at + 32 > b.size()) {
      ++bad;
      continue;
    }
    const auto a = static_cast<std::size_t>(at);
    const std::uint32_t mask = 0x4164536BU ^ static_cast<std::uint32_t>(at);
    auto h = [&](std::size_t k) { return u32(b, a + k) ^ mask; };
    const std::uint32_t ptype = h(0);
    const std::uint32_t data_size = h(8);
    const double start = h(16) + h(20) * 4294967296.0;
    const std::uint32_t data_sum = h(0x1C);
    if (ptype != 0x4163043BU) {
      ++bad;
      continue;
    }
    const std::size_t from = a + 32;
    if (from + static_cast<std::uint64_t>(data_size) > b.size()) {
      ++bad;
      continue;
    }
    if (page_checksum(0, b.subspan(from, data_size)) != data_sum) {
      c.notes.push_back(msg("ccdwg.read.page_checksum", {{"name", name}}));
    }
    const double logical = std::min(max_size, size - start);
    if (logical <= 0) {
      continue;
    }
    const auto st = static_cast<std::size_t>(start);
    const auto lg = static_cast<std::size_t>(logical);
    if (compressed) {
      const std::vector<std::uint8_t> page = lz2004(b, from, from + data_size, static_cast<std::size_t>(max_size));
      std::copy_n(page.begin(), lg, out.begin() + static_cast<std::ptrdiff_t>(st));
    } else {
      const std::size_t n = std::min(lg, b.size() - from);
      std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(from), n, out.begin() + static_cast<std::ptrdiff_t>(st));
    }
  }
  if (bad != 0) {
    c.notes.push_back(msg("ccdwg.read.pages_bad", {{"n", bad}, {"name", name}}));
  }
  return out;
}

Container read_r2004(Bytes b, const std::string& version, int ver) {
  Container c;
  c.version = version;
  c.ver = ver;
  c.maintenance = static_cast<int>(u8at(b, 0x0B));
  c.codepage_num = static_cast<int>(u16(b, 0x13));
  const std::uint32_t security = u32(b, 0x18);
  if ((security & 1U) != 0) {
    c.notes.push_back(msg("ccdwg.read.password"));
  }
  // 4.1 encrypted file header data at 0x80
  const std::vector<std::uint8_t> magic = r2004_magic(0x6C);
  std::vector<std::uint8_t> h(0x6C);
  for (std::size_t i = 0; i < 0x6C; ++i) {
    h[i] = static_cast<std::uint8_t>(u8at(b, 0x80 + i) ^ magic[i]);
  }
  if (ascii(h, 0, 11) != "AcFssFcAJMB") {
    throw Unsupported(msg("ccdwg.open.no_id", {{"version", version}}));
  }
  {
    const std::uint32_t want = u32(h, 0x68);
    std::vector<std::uint8_t> tmp = h;
    tmp[0x68] = tmp[0x69] = tmp[0x6A] = tmp[0x6B] = 0;
    if (crc32(tmp, 0) != want) {
      c.notes.push_back(msg("ccdwg.read.file_header_crc32"));
    }
  }
  const std::uint64_t page_map_addr = u64(h, 0x54) + 0x100;
  const std::uint32_t section_map_id = u32(h, 0x5C);
  // 4.4 section page map (system page)
  const std::vector<std::uint8_t> pm = read_system_page(b, page_map_addr, 0x41630E3B, c, msg("ccdwg.part.page_map"));
  std::map<std::int64_t, PageLoc> pages;
  std::uint64_t addr = 0x100;
  for (std::size_t i = 0; i + 8 <= pm.size();) {
    const std::int32_t num = i32(pm, i);
    const std::uint32_t size = u32(pm, i + 4);
    i += 8;
    if (num < 0) {
      i += 16;
    } else {
      pages[num] = PageLoc{addr, size};
    }
    addr += size;
  }
  const auto sm = pages.find(section_map_id);
  if (sm == pages.end()) {
    throw Unsupported(msg("ccdwg.open.no_section_map", {{"version", version}, {"id", section_map_id}}));
  }
  // 4.5 data section map
  const std::vector<std::uint8_t> sm_data =
      read_system_page(b, sm->second.addr, 0x4163003B, c, msg("ccdwg.part.section_map"));
  const std::uint32_t count = u32(sm_data, 0);
  std::size_t p = 0x14;
  for (std::uint32_t s = 0; s < count && p + 0x60 <= sm_data.size(); ++s) {
    const auto size = static_cast<double>(u64(sm_data, p));
    const std::uint32_t page_count = u32(sm_data, p + 8);
    const std::uint32_t max_size = u32(sm_data, p + 0x0C);
    const std::uint32_t compressed = u32(sm_data, p + 0x14);
    const std::uint32_t encrypted = u32(sm_data, p + 0x1C);
    std::string name;
    for (std::size_t i = 0; i < 64 && p + 0x20 + i < sm_data.size() && sm_data[p + 0x20 + i] != 0; ++i) {
      name += static_cast<char>(sm_data[p + 0x20 + i]);
    }
    p += 0x60;
    std::vector<PageRef> list;
    if (page_count > pages.size() + 1 || p + static_cast<std::size_t>(page_count) * 16 > sm_data.size()) {
      c.notes.push_back(msg("ccdwg.read.section_map_damaged", {{"n", page_count}}));
      break;
    }
    for (std::uint32_t k = 0; k < page_count; ++k) {
      list.push_back(PageRef{u32(sm_data, p), u32(sm_data, p + 4), u64(sm_data, p + 8)});
      p += 16;
    }
    if (name.empty()) {
      continue;
    }
    if (encrypted == 1) {
      c.notes.push_back(msg("ccdwg.read.section_encrypted", {{"name", name}}));
      continue;
    }
    try {
      Section sec;
      sec.owned = read_data_section(b, pages, list, size, max_size, compressed == 2, name, c);
      c.sections[name] = std::move(sec);
    } catch (const std::exception& e) {
      c.notes.push_back(msg("ccdwg.read.section_failed", {{"name", name}, {"reason", error_text(e)}}));
    }
  }
  return c;
}

// ---------------------------------------------------------------- R2007 (ch. 5)
std::vector<std::uint8_t> load_sys_page(Bytes b, double at, double csize, double usize, double factor, Container& c,
                                        const std::string& what) {
  plausible(usize, b.size());
  plausible(csize * factor, b.size());
  const auto aligned = static_cast<double>((static_cast<std::uint64_t>(csize) + 7U) & ~std::uint64_t{7});
  const double blocks = std::ceil(aligned * factor / 239);
  if (at + blocks * 255 > static_cast<double>(b.size())) {
    throw Unsupported(msg("ccdwg.open.r2007_past_end", {{"what", what}}));
  }
  RsStats stats;
  std::vector<std::uint8_t> data =
      deinterleave(b, static_cast<std::size_t>(at), static_cast<std::size_t>(blocks), 239, true, &stats);
  if (stats.bad != 0) {
    c.notes.push_back(msg("ccdwg.read.uncorrectable", {{"what", what}}));
  }
  if (csize < usize) {
    return lz21(data, 0, static_cast<std::size_t>(csize), static_cast<std::size_t>(usize));
  }
  data.resize(std::min(data.size(), static_cast<std::size_t>(usize)));
  return data;
}

struct Page21 {
  std::int64_t offset = 0;
  std::int64_t size = 0;
  std::int64_t id = 0;
};
struct PageRef21 {
  double offset = 0;
  double size = 0;
  double id = 0;
  double usize = 0;
  double csize = 0;
};

std::vector<std::uint8_t> read_data_section21(Bytes b, const std::map<std::int64_t, Page21>& pages,
                                              const std::vector<PageRef21>& list, double size, double encoding,
                                              const std::string& name, Container& c) {
  plausible(size, b.size());
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  int bad = 0;
  for (const PageRef21& pg : list) {
    const auto it = pages.find(static_cast<std::int64_t>(pg.id));
    if (it == pages.end()) {
      ++bad;
      continue;
    }
    const double at = 0x480 + static_cast<double>(it->second.offset);
    const auto aligned = static_cast<double>((static_cast<std::uint64_t>(pg.csize) + 7U) & ~std::uint64_t{7});
    const auto blocks = static_cast<std::size_t>(std::ceil(aligned / 251));
    std::vector<std::uint8_t> data;
    if (encoding == 4) {
      data = deinterleave(b, static_cast<std::size_t>(at), blocks, 251, false, nullptr);
    } else {
      data.assign(blocks * 251, 0);
      for (std::size_t k = 0; k < blocks; ++k) {
        const auto from = static_cast<std::size_t>(at) + k * 255;
        for (std::size_t i = 0; i < 251 && from + i < b.size(); ++i) {
          data[k * 251 + i] = b[from + i];
        }
      }
    }
    plausible(pg.usize, b.size());
    std::vector<std::uint8_t> page;
    if (pg.csize < pg.usize) {
      page = lz21(data, 0, static_cast<std::size_t>(pg.csize), static_cast<std::size_t>(pg.usize));
    } else {
      page.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(
                                                   std::min(data.size(), static_cast<std::size_t>(pg.usize))));
    }
    const auto start = static_cast<std::size_t>(pg.offset);
    if (start < out.size()) {
      const std::size_t n = std::min(page.size(), out.size() - start);
      std::copy_n(page.begin(), n, out.begin() + static_cast<std::ptrdiff_t>(start));
    }
  }
  if (bad != 0) {
    c.notes.push_back(msg("ccdwg.read.pages_missing", {{"n", bad}, {"name", name}}));
  }
  return out;
}

Container read_r2007(Bytes b, const std::string& version, int ver) {
  Container c;
  c.version = version;
  c.ver = ver;
  c.maintenance = static_cast<int>(u8at(b, 0x0B));
  c.codepage_num = static_cast<int>(u16(b, 0x13));
  const std::uint32_t security = u32(b, 0x18);
  if ((security & 1U) != 0) {
    c.notes.push_back(msg("ccdwg.read.password"));
  }
  if (b.size() < 0x480) {
    throw Unsupported(msg("ccdwg.open.r2007_short"));
  }
  RsStats stats;
  const std::vector<std::uint8_t> hdr = deinterleave(b, 0x80, 3, 239, true, &stats);
  if (stats.bad != 0) {
    c.notes.push_back(msg("ccdwg.read.r2007_header_bad"));
  } else if (stats.corrected != 0) {
    c.notes.push_back(msg("ccdwg.read.r2007_corrected", {{"n", stats.corrected}}));
  }
  const std::int32_t compr_len = i32(hdr, 0x18);
  std::vector<std::uint8_t> fh;
  if (compr_len < 0) {
    const auto n = static_cast<std::size_t>(-static_cast<std::int64_t>(compr_len));
    fh.assign(hdr.begin() + 0x20, hdr.begin() + static_cast<std::ptrdiff_t>(std::min(hdr.size(), 0x20 + n)));
  } else {
    fh = lz21(hdr, 0x20, static_cast<std::size_t>(compr_len), 0x110);
  }
  auto f64 = [&](std::size_t at) { return static_cast<double>(u64(fh, at)); };
  const double pages_map_correction = f64(0x18);
  const double pages_map_offset = f64(0x38);
  const double pages_map_size_comp = f64(0x50);
  const double pages_map_size_uncomp = f64(0x58);
  const double sections_map_size_comp = f64(0xB0);
  const double sections_map_id = f64(0xC0);
  const double sections_map_size_uncomp = f64(0xC8);
  const double sections_map_correction = f64(0xD8);
  const std::vector<std::uint8_t> pm =
      load_sys_page(b, 0x480 + pages_map_offset, pages_map_size_comp, pages_map_size_uncomp, pages_map_correction, c,
                    msg("ccdwg.part.page_map"));
  std::map<std::int64_t, Page21> pages;
  std::int64_t offset = 0;
  for (std::size_t p = 0; p + 16 <= pm.size(); p += 16) {
    const std::int64_t size = i64(pm, p);
    const std::int64_t id = i64(pm, p + 8);
    pages[id < 0 ? -id : id] = Page21{offset, size, id};
    offset += size;
  }
  const auto smp = pages.find(static_cast<std::int64_t>(sections_map_id));
  if (smp == pages.end()) {
    throw Unsupported(msg("ccdwg.open.r2007_no_section_map"));
  }
  const std::vector<std::uint8_t> sm =
      load_sys_page(b, 0x480 + static_cast<double>(smp->second.offset), sections_map_size_comp,
                    sections_map_size_uncomp, sections_map_correction, c, msg("ccdwg.part.section_map"));
  std::size_t p = 0;
  while (p + 0x40 <= sm.size()) {
    const auto data_size = static_cast<double>(u64(sm, p));
    const auto encryption = static_cast<double>(u64(sm, p + 0x10));
    const std::uint64_t name_len = u64(sm, p + 0x20);
    const auto encoding = static_cast<double>(u64(sm, p + 0x30));
    const std::uint64_t num_pages = u64(sm, p + 0x38);
    p += 0x40;
    // Spec gap (r2007.js): the name length is in bytes and counts the closing NUL.
    U16 units;
    for (std::uint64_t i = 0; i + 1 < name_len; i += 2) {
      units.push_back(static_cast<std::uint16_t>(u16(sm, p + static_cast<std::size_t>(i))));
    }
    while (!units.empty() && units.back() == 0) {
      units.pop_back();
    }
    const std::string name = utf8(units);
    if (name_len > sm.size()) {
      break;
    }
    p += static_cast<std::size_t>(name_len);
    std::vector<PageRef21> list;
    for (std::uint64_t k = 0; k < num_pages && p + 56 <= sm.size(); ++k) {
      list.push_back(PageRef21{static_cast<double>(u64(sm, p)), static_cast<double>(u64(sm, p + 8)),
                               static_cast<double>(u64(sm, p + 16)), static_cast<double>(u64(sm, p + 24)),
                               static_cast<double>(u64(sm, p + 32))});
      p += 56;
    }
    if (name.empty()) {
      continue;
    }
    if (encryption == 1) {
      c.notes.push_back(msg("ccdwg.read.section_encrypted", {{"name", name}}));
      continue;
    }
    try {
      Section sec;
      sec.owned = read_data_section21(b, pages, list, data_size, encoding, name, c);
      c.sections[name] = std::move(sec);
    } catch (const std::exception& e) {
      c.notes.push_back(msg("ccdwg.read.section_failed", {{"name", name}, {"reason", error_text(e)}}));
    }
  }
  return c;
}

}  // namespace

Container read_container(Bytes file) {
  if (file.size() < 0x80) {
    throw Unsupported(msg("ccdwg.open.too_short", {{"n", file.size()}}));
  }
  const std::string version = ascii(file, 0, 6);
  const int ver = version_num(version);
  if (ver == 0) {
    const bool old = (version.starts_with("AC10") && version.size() == 6 &&
                      std::isdigit(static_cast<unsigned char>(version[4])) != 0 &&
                      std::isdigit(static_cast<unsigned char>(version[5])) != 0) ||
                     (version.starts_with("AC1.") || version.starts_with("AC2.")) || version.starts_with("MC0.0");
    if (old) {
      throw Unsupported(msg("ccdwg.open.version", {{"version", version}}));
    }
    std::string start;
    for (const char ch : version) {
      start += ch >= 0x20 && ch <= 0x7E ? ch : '?';
    }
    throw Unsupported(msg("ccdwg.open.not_dwg", {{"start", start}}));
  }
  Container c;
  if (ver <= 15) {
    c = read_r13(file, version, ver);
  } else if (ver == 21) {
    c = read_r2007(file, version, ver);
  } else {
    c = read_r2004(file, version, ver);
  }
  // Owned sections were moved into the map; refresh their views.
  for (auto& [name, s] : c.sections) {
    if (!s.owned.empty() || s.view.data() == nullptr) {
      s.view = Bytes(s.owned);
    }
  }
  return c;
}

}  // namespace cc::dwg
