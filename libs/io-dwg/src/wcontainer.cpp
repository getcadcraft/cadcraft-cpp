// SPDX-License-Identifier: Apache-2.0
// container-write.js, lz-r2004-write.js and r2007-write.js in C++ (see wcontainer.hpp).
#include "wcontainer.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>

#include "container.hpp"
#include "wcrc.hpp"

namespace cc::dwg::w {

namespace {

using Span = std::span<const std::uint8_t>;

const std::vector<std::uint8_t>& magic() {
  static const std::vector<std::uint8_t> m = r2004_magic(0x100);
  return m;
}

constexpr std::array<std::uint8_t, 16> kFilehdrEnd = {0x95, 0xA0, 0x4E, 0x28, 0x99, 0x82, 0x1A, 0xE5,
                                                      0x5E, 0x41, 0xE0, 0x5F, 0x9D, 0x3A, 0x4D, 0x00};
constexpr std::array<std::uint8_t, 16> kHeader = {0xCF, 0x7B, 0x1F, 0x23, 0xFD, 0xDE, 0x38, 0xA9,
                                                  0x5F, 0x7C, 0x68, 0xB8, 0x4E, 0x6D, 0x33, 0x5F};
constexpr std::array<std::uint8_t, 16> kHeaderEnd = {0x30, 0x84, 0xE0, 0xDC, 0x02, 0x21, 0xC7, 0x56,
                                                     0xA0, 0x83, 0x97, 0x47, 0xB1, 0x92, 0xCC, 0xA0};
constexpr std::array<std::uint8_t, 16> kClasses = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
                                                   0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A};
constexpr std::array<std::uint8_t, 16> kClassesEnd = {0x72, 0x5E, 0x3B, 0x47, 0x3B, 0x56, 0x07, 0x3A,
                                                      0x3F, 0x23, 0x0B, 0xA0, 0x18, 0x30, 0x49, 0x75};
constexpr std::array<std::uint8_t, 16> kPreview = {0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
                                                   0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B};
constexpr std::array<std::uint8_t, 16> kPreviewEnd = {0xE0, 0xDA, 0x92, 0xF8, 0x2B, 0xC9, 0xD7, 0xD7,
                                                      0x62, 0xA8, 0x35, 0xC0, 0x62, 0xBB, 0xEF, 0xD4};
constexpr std::array<std::uint8_t, 16> kSecond = {0xD4, 0x7B, 0x21, 0xCE, 0x28, 0x93, 0x9F, 0xBF,
                                                  0x53, 0x24, 0x40, 0x09, 0x12, 0x3C, 0xAA, 0x01};
constexpr std::array<std::uint8_t, 16> kSecondEnd = {0x2B, 0x84, 0xDE, 0x31, 0xD7, 0x6C, 0x60, 0x40,
                                                     0xAC, 0xDB, 0xBF, 0xF6, 0xED, 0xC3, 0x55, 0xFE};

std::uint32_t crc16(std::uint32_t seed, Span b) {
  return crc8(seed, b);
}

bool has_hi_size(std::string_view v) {
  const int m = version_info(v).maint;
  return v == "AC1032" || ((v == "AC1024" || v == "AC1027") && m > 3);
}

ByteBuf wrap_section(Span begin, Span end, const ByteBuf& data, std::string_view version) {
  ByteWriter o(data.size() + 64);
  o.raw(begin);
  const std::size_t from = o.pos();
  o.RL(static_cast<double>(data.size()));
  if (has_hi_size(version)) {
    o.RL(0);
  }
  o.raw(data);
  o.RS(crc16(0xC0C1, Span(o.buf).subspan(from)));
  o.raw(end);
  return o.buf;
}
ByteBuf header_section(const ByteBuf& d, std::string_view v) {
  return wrap_section(kHeader, kHeaderEnd, d, v);
}
ByteBuf classes_section(const ByteBuf& d, std::string_view v) {
  return wrap_section(kClasses, kClassesEnd, d, v);
}

ByteBuf preview_section() {
  ByteWriter o(64);
  o.raw(kPreview);
  o.RL(1);
  o.RC(0);
  o.raw(kPreviewEnd);
  return o.buf;
}

ByteBuf object_map(const std::vector<std::pair<double, double>>& map) {
  ByteWriter out(map.size() * 6 + 64);
  std::size_t i = 0;
  for (;;) {
    ByteWriter sec(2048);
    const std::size_t start = i;
    double last_h = 0;
    double last_loc = 0;
    while (i < map.size() && sec.pos() < 2020) {
      const auto [h, loc] = map[i];
      sec.MC(h - last_h);
      sec.MC(loc - last_loc);
      last_h = h;
      last_loc = loc;
      ++i;
    }
    const std::size_t size = sec.pos() + 2;
    ByteBuf chunk(size);
    chunk[0] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
    chunk[1] = static_cast<std::uint8_t>(size & 0xFFU);
    std::ranges::copy(sec.buf, chunk.begin() + 2);
    const std::uint32_t c = crc16(0xC0C1, chunk);
    out.raw(chunk);
    out.RC(c >> 8U);
    out.RC(c & 0xFFU);
    if (start == i) {
      break;
    }
  }
  return out.buf;
}

ByteBuf obj_free_space(double count, const JulianDate& jd, double objects_at) {
  ByteWriter o(64);
  o.RL(0);
  o.RL(count);
  o.RL(jd.day);
  o.RL(jd.ms);
  o.RL(objects_at);
  o.RC(4);
  for (const double v : {double{0x32}, double{0x64}, double{0x200}, 4294967295.0}) {
    o.RL(v);
    o.RL(0);
  }
  return o.buf;
}

ByteBuf template_section(bool measurement, std::string_view version) {
  ByteWriter o(8);
  if (version == "AC1015" || version == "AC1018") {
    o.RS(0);
  } else {
    o.RS(1);
    o.RS(0);
  }
  o.RS(measurement ? 1 : 0);
  return o.buf;
}

ByteBuf aux_header(std::string_view version, const Parts& p) {
  const auto [code, maint] = version_info(version);
  ByteWriter o(160);
  o.raw({0xFF, 0x77, 0x01});
  o.RS(code);
  o.RS(maint);
  o.RL(1);
  o.RL(-1);
  o.RS(1);
  o.RS(0);
  o.RL(0);
  o.RS(code);
  o.RS(maint);
  o.RS(code);
  o.RS(maint);
  o.RS(0x0005);
  o.RS(0x0893);
  o.RS(0x0005);
  o.RS(0x0893);
  o.RS(0);
  o.RS(1);
  for (int i = 0; i < 5; ++i) {
    o.RL(0);
  }
  o.RL(p.created.day);
  o.RL(p.created.ms);
  o.RL(p.updated.day);
  o.RL(p.updated.ms);
  o.RL(p.handseed < 0x7FFFFFFF ? p.handseed : -1);
  o.RL(0);
  o.RS(0);
  o.RS(1);
  o.RL(0);
  o.RL(0);
  o.RL(0);
  o.RL(1);
  o.RL(0);
  o.RL(0);
  o.RL(0);
  o.RL(0);
  if (version == "AC1032") {
    o.RS(0);
    o.RS(0);
    o.RS(0);
  }
  return o.buf;
}

ByteBuf summary_info(std::string_view version, const Parts& p) {
  const bool wide = version != "AC1018";
  ByteWriter o(256);
  const auto str = [&](std::string_view s) {
    o.RS(static_cast<double>(s.size() + 1));  // ASCII only
    for (const char ch : s) {
      if (wide) {
        o.RS(static_cast<unsigned char>(ch));
      } else {
        o.RC(static_cast<unsigned char>(ch));
      }
    }
    if (wide) {
      o.RS(0);
    } else {
      o.RC(0);
    }
  };
  for (const std::string_view s : {"", "", "", "", "", "cc-dwg", "", ""}) {
    str(s);
  }
  o.RL(0);
  o.RL(0);
  o.RL(p.created.day);
  o.RL(p.created.ms);
  o.RL(p.updated.day);
  o.RL(p.updated.ms);
  o.RS(0);
  o.RL(0);
  o.RL(0);
  return o.buf;
}

ByteBuf rev_history(std::string_view version) {
  ByteWriter o(16);
  if (version == "AC1018") {
    o.RL(0);
    o.RL(0);
    o.RL(0);
  } else {
    o.RL(0);
    o.RL(0);
    o.RL(1);
    o.RL(0);
  }
  return o.buf;
}

void put_u32(ByteBuf& b, std::size_t at, std::uint32_t v) {
  for (unsigned k = 0; k < 4; ++k) {
    b.at(at + k) = static_cast<std::uint8_t>((v >> (8U * k)) & 0xFFU);
  }
}

// ---------------------------------------------------------------- R2004 LZ77 (lz-r2004-write.js)

constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxDist = 0x3FFF + 0x3FFF + 1;
constexpr unsigned kHashBits = 15;
constexpr int kMaxChain = 64;

bool encodable(std::size_t len, std::size_t dist) {
  const std::size_t co = dist - 1;
  if (co <= 0x3FFF) {
    return len >= 3;
  }
  if (co <= 0x3FFF + 0x3FFF) {
    return len >= 4;
  }
  return false;
}

struct LzOut {
  ByteBuf buf;
  void byte(std::size_t v) { buf.push_back(static_cast<std::uint8_t>(v & 0xFFU)); }
  void literal_length(std::size_t v) {
    if (v == 0) {
      return;
    }
    if (v < 4) {
      throw std::logic_error("literal run shorter than 4");
    }
    if (v <= 18) {
      byte(v - 3);
      return;
    }
    byte(0);
    std::size_t r = v - 3 - 0x0F;
    while (r > 0xFF) {
      byte(0);
      r -= 0xFF;
    }
    byte(r);
  }
  void long_value(std::size_t v) {
    if (v <= 0xFF) {
      byte(v);
      return;
    }
    byte(0);
    std::size_t r = v - 0xFF;
    while (r > 0xFF) {
      byte(0);
      r -= 0xFF;
    }
    byte(r);
  }
  void two_byte(std::size_t off, std::size_t lit) {
    byte(((off & 0x3FU) << 2U) | lit);
    byte(off >> 6U);
  }
  void match(std::size_t len, std::size_t dist, std::size_t lit_count) {
    const std::size_t lc = lit_count <= 3 ? lit_count : 0;
    const std::size_t co = dist - 1;
    if (co <= 0x3FF && len <= 14) {
      byte(((len + 1) << 4U) | ((co & 3U) << 2U) | lc);
      byte(co >> 2U);
    } else if (co <= 0x3FFF) {
      if (len <= 33) {
        byte(0x1E + len);
      } else {
        byte(0x20);
        long_value(len - 0x21);
      }
      two_byte(co, lc);
    } else {
      std::size_t c2 = co - 0x3FFF;
      std::size_t hi = 0;
      if (c2 > 0x3FFF) {
        c2 -= 0x4000;
        hi = 8;
      }
      if (len <= 9) {
        byte(0x10U | hi | (len - 2));
      } else {
        byte(0x10U | hi);
        long_value(len - 9);
      }
      two_byte(c2, lc);
    }
  }
};

// ---------------------------------------------------------------- R2007 (r2007-write.js)

struct Galois {
  std::array<std::uint8_t, 512> exp{};
  std::array<std::uint8_t, 256> log{};
};
Galois galois(unsigned poly) {
  Galois g;
  unsigned x = 1;
  for (unsigned i = 0; i < 255; ++i) {
    g.exp[i] = static_cast<std::uint8_t>(x);
    g.log[x] = static_cast<std::uint8_t>(i);
    x <<= 1U;
    if ((x & 0x100U) != 0) {
      x ^= poly;
    }
  }
  for (unsigned i = 255; i < 512; ++i) {
    g.exp[i] = g.exp[i - 255];
  }
  return g;
}

struct RsCode {
  std::size_t k = 0, nk = 0;
  std::vector<std::array<std::uint8_t, 256>> tab;  // per generator coefficient
  RsCode(unsigned poly, std::size_t kk, std::size_t nkk) : k(kk), nk(nkk) {
    const Galois G = galois(poly);
    const auto mul = [&](unsigned a, unsigned b) -> unsigned {
      return a != 0 && b != 0 ? G.exp[static_cast<std::size_t>(G.log[a]) + G.log[b]] : 0U;
    };
    std::vector<unsigned> g = {1};
    for (std::size_t i = 1; i <= nk; ++i) {
      const unsigned r = G.exp[i % 255];
      std::vector<unsigned> ng(g.size() + 1, 0);
      for (std::size_t j = 0; j < g.size(); ++j) {
        ng[j] ^= mul(g[j], r);
        ng[j + 1] ^= g[j];
      }
      g = std::move(ng);
    }
    tab.resize(nk);
    for (std::size_t j = 0; j < nk; ++j) {
      for (unsigned v = 0; v < 256; ++v) {
        tab[j][v] = static_cast<std::uint8_t>(mul(v, g[j]));
      }
    }
  }
  /// parity (nk bytes, lowest degree first) of k data bytes at `from`
  void encode(const ByteBuf& data, std::size_t from, std::uint8_t* rem) const {
    std::fill(rem, rem + nk, std::uint8_t{0});  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::span<std::uint8_t> r(rem, nk);
    for (std::size_t i = k; i-- > 0;) {
      const std::uint8_t fb = data[from + i] ^ r[nk - 1];
      for (std::size_t j = nk - 1; j > 0; --j) {
        r[j] = r[j - 1] ^ tab[j][fb];
      }
      r[0] = tab[0][fb];
    }
  }
};

const RsCode& rs_sys() {
  static const RsCode c(0x169, 239, 16);
  return c;
}
const RsCode& rs_data() {
  static const RsCode c(0x11D, 251, 4);
  return c;
}

template <class Pad>
ByteBuf rs_encode(Span data, const RsCode& code, bool interleaved, const Pad& pad) {
  const std::size_t k = code.k;
  const std::size_t nk = code.nk;
  const std::size_t blocks = std::max<std::size_t>(1, (data.size() + k - 1) / k);
  ByteBuf full(blocks * k, 0);
  std::ranges::copy(data, full.begin());
  for (std::size_t i = data.size(); i < full.size(); ++i) {
    full[i] = pad();
  }
  ByteBuf out(blocks * 255, 0);
  std::array<std::uint8_t, 16> par{};
  for (std::size_t b = 0; b < blocks; ++b) {
    code.encode(full, b * k, par.data());
    if (interleaved) {
      for (std::size_t i = 0; i < k; ++i) {
        out[i * blocks + b] = full[b * k + i];
      }
      for (std::size_t i = 0; i < nk; ++i) {
        out[(k + i) * blocks + b] = par[i];
      }
    } else {
      std::copy_n(full.begin() + static_cast<std::ptrdiff_t>(b * k), k,
                  out.begin() + static_cast<std::ptrdiff_t>(b * k));
      std::copy_n(par.begin(), nk, out.begin() + static_cast<std::ptrdiff_t>(blocks * k + b * nk));
    }
  }
  return out;
}

/// 5.10.1 the literal copy order: PERM[n][out index] = source index within an n-byte block
const std::array<std::vector<std::uint8_t>, 33>& perm() {
  static const std::array<std::vector<std::uint8_t>, 33> P = [] {
    using L = std::vector<std::pair<int, int>>;
    const std::array<L, 33> lit = {
        L{},
        L{{1, 0}},
        L{{1, 1}, {1, 0}},
        L{{1, 2}, {1, 1}, {1, 0}},
        L{{1, 0}, {1, 1}, {1, 2}, {1, 3}},
        L{{1, 4}, {4, 0}},
        L{{1, 5}, {4, 1}, {1, 0}},
        L{{2, 5}, {4, 1}, {1, 0}},
        L{{4, 0}, {4, 4}},
        L{{1, 8}, {8, 0}},
        L{{1, 9}, {8, 1}, {1, 0}},
        L{{2, 9}, {8, 1}, {1, 0}},
        L{{4, 8}, {8, 0}},
        L{{1, 12}, {4, 8}, {8, 0}},
        L{{1, 13}, {4, 9}, {8, 1}, {1, 0}},
        L{{2, 13}, {4, 9}, {8, 1}, {1, 0}},
        L{{8, 8}, {8, 0}},
        L{{8, 9}, {1, 8}, {8, 0}},
        L{{1, 17}, {16, 1}, {1, 0}},
        L{{3, 16}, {16, 0}},
        L{{4, 16}, {16, 0}},
        L{{1, 20}, {4, 16}, {16, 0}},
        L{{2, 20}, {4, 16}, {16, 0}},
        L{{3, 20}, {4, 16}, {16, 0}},
        L{{8, 16}, {16, 0}},
        L{{8, 17}, {1, 16}, {16, 0}},
        L{{1, 25}, {8, 17}, {1, 16}, {16, 0}},
        L{{2, 25}, {8, 17}, {1, 16}, {16, 0}},
        L{{4, 24}, {8, 16}, {16, 0}},
        L{{1, 28}, {4, 24}, {8, 16}, {16, 0}},
        L{{2, 28}, {4, 24}, {8, 16}, {16, 0}},
        L{{1, 30}, {4, 26}, {8, 18}, {16, 2}, {2, 0}},
        L{{16, 16}, {16, 0}},
    };
    // each block order is built from smaller blocks (k < n), so filling them in increasing n settles every one
    std::array<std::vector<std::uint8_t>, 33> p;
    for (std::size_t n = 1; n <= 32; ++n) {
      std::vector<std::uint8_t> out;
      for (const auto& [k, off] : lit[n]) {
        if (k == 1) {
          out.push_back(static_cast<std::uint8_t>(off));
        } else {
          for (const std::uint8_t s : p[static_cast<std::size_t>(k)]) {
            out.push_back(static_cast<std::uint8_t>(off + s));
          }
        }
      }
      p[n] = std::move(out);
    }
    return p;
  }();
  return P;
}

std::uint64_t rot(std::uint64_t value, std::uint64_t control) {
  const unsigned s = control & 0x1FU;
  return s != 0 ? (value << s) | (value >> (64U - s)) : value;
}

ByteBuf pad8(const ByteBuf& b) {
  const std::size_t n = (b.size() + 7) & ~std::size_t{7};
  ByteBuf o = b;
  o.resize(n, 0);
  return o;
}
std::size_t align20(std::size_t n) {
  return (n + 0x1F) & ~std::size_t{0x1F};
}

std::size_t system_page_size(std::size_t data_size) {
  const std::size_t aligned = (data_size + 7) & ~std::size_t{7};
  const std::size_t p = ((aligned * 2 + 239 - 1) / 239) * 255;
  return p < 0x400 ? 0x400 : align20(p);
}

struct SysPage21 {
  ByteBuf bytes;
  std::size_t size = 0, factor = 0, comp_size = 0;
  std::uint64_t crc_uncompressed = 0, crc_compressed = 0;
};
SysPage21 system_page21(const ByteBuf& data, CrcRandom& rnd) {
  SysPage21 r;
  r.size = system_page_size(data.size());
  const ByteBuf z = compress_r2007(data);
  const ByteBuf& stored = z.size() < data.size() ? z : data;
  const ByteBuf d8 = pad8(stored);
  const std::size_t max_pre = (r.size / 255) * 239;
  r.factor = std::max<std::size_t>(1, max_pre / d8.size());
  ByteBuf buf;
  buf.reserve(d8.size() * r.factor);
  for (std::size_t i = 0; i < r.factor; ++i) {
    buf.insert(buf.end(), d8.begin(), d8.end());
  }
  const ByteBuf rs = rs_encode(buf, rs_sys(), true, [&] { return rnd.pad_byte(); });
  r.bytes.assign(r.size, 0);
  std::ranges::copy(rs, r.bytes.begin());
  for (std::size_t i = rs.size(); i < r.size; ++i) {
    r.bytes[i] = rnd.pad_byte();
  }
  r.comp_size = stored.size();
  r.crc_uncompressed = crc64_mirrored(data, update_seed1(0, data.size()));
  r.crc_compressed = crc64_mirrored(stored, update_seed1(0, stored.size()));
  return r;
}

std::uint32_t name_hash(std::string_view n) {
  static const std::map<std::string_view, std::uint32_t> h = {
      {"AcDb:SummaryInfo", 0x717A060F}, {"AcDb:Preview", 0x40AA0473},      {"AcDb:RevHistory", 0x60A205B3},
      {"AcDb:AcDbObjects", 0x674C05A9}, {"AcDb:ObjFreeSpace", 0x77E2061F}, {"AcDb:Template", 0x4A1404CE},
      {"AcDb:Handles", 0x3F6E0450},     {"AcDb:Classes", 0x3F54045F},      {"AcDb:AuxHeader", 0x54F0050A},
      {"AcDb:Header", 0x32B803D9}};
  const auto it = h.find(n);
  return it == h.end() ? 0 : it->second;
}

void put_ascii(ByteWriter& o, std::string_view s) {
  for (const char c : s) {
    o.RC(static_cast<unsigned char>(c));
  }
}

}  // namespace

VersionInfo version_info(std::string_view v) {
  if (v == "AC1015") {
    return {23, 6};
  }
  if (v == "AC1018") {
    return {25, 0};
  }
  if (v == "AC1021") {
    return {27, 0};
  }
  if (v == "AC1024") {
    return {29, 6};
  }
  if (v == "AC1027") {
    return {31, 8};
  }
  if (v == "AC1032") {
    return {33, 0};
  }
  return {};
}

// ---------------------------------------------------------------- compressors

ByteBuf compress_r2004(Span src_in) {
  ByteBuf padded;
  Span src = src_in;
  if (src.size() < 4) {  // the first literal run must be at least 4 bytes long
    padded.assign(4, 0);
    std::ranges::copy(src_in, padded.begin());
    src = padded;
  }
  const std::size_t n = src.size();
  LzOut out;
  out.buf.reserve(n + (n >> 3U) + 64);
  std::vector<std::int32_t> head(std::size_t{1} << kHashBits, -1);
  std::vector<std::int32_t> prev(n, -1);
  const auto hash = [&](std::size_t i) -> std::size_t {
    const std::uint64_t v =
        (static_cast<std::uint64_t>(src[i]) << 16U) | (static_cast<std::uint64_t>(src[i + 1]) << 8U) | src[i + 2];
    return static_cast<std::uint32_t>(v * 2654435761ULL) >> (32U - kHashBits);
  };
  const auto insert = [&](std::size_t i) {
    if (i + 2 < n) {
      const std::size_t h = hash(i);
      prev[i] = head[h];
      head[h] = static_cast<std::int32_t>(i);
    }
  };
  struct Tok {
    std::size_t at, len, dist;
  };
  std::vector<Tok> tokens;
  for (std::size_t k = 0; k < 4 && k < n; ++k) {
    insert(k);
  }
  const auto best = [&](std::size_t pos, std::size_t& blen, std::size_t& bdist) -> bool {
    if (pos + kMinMatch > n) {
      return false;
    }
    std::int32_t cand = head[hash(pos)];
    std::size_t best_len = 0;
    std::size_t best_dist = 0;
    int chain = 0;
    const std::size_t max_len = n - pos;
    while (cand >= 0 && chain++ < kMaxChain) {
      const auto c = static_cast<std::size_t>(cand);
      const std::size_t dist = pos - c;
      if (dist > kMaxDist) {
        break;
      }
      if (pos + best_len < n && src[c + best_len] == src[pos + best_len] && src[c] == src[pos]) {
        std::size_t l = 0;
        while (l < max_len && src[c + l] == src[pos + l]) {
          ++l;
        }
        if (l > best_len && encodable(l, dist)) {
          best_len = l;
          best_dist = dist;
          if (l == max_len) {
            break;
          }
        }
      }
      cand = prev[c];
    }
    if (best_len >= kMinMatch) {
      blen = best_len;
      bdist = best_dist;
      return true;
    }
    return false;
  };
  std::size_t i = 4;
  while (i < n) {
    std::size_t len = 0;
    std::size_t dist = 0;
    if (!best(i, len, dist)) {
      insert(i);
      ++i;
      continue;
    }
    insert(i);
    std::size_t len2 = 0;
    std::size_t dist2 = 0;
    const bool m2 = i + 1 < n && best(i + 1, len2, dist2);
    std::size_t at = i;
    if (m2 && len2 > len) {
      at = i + 1;
      len = len2;
      dist = dist2;
      insert(at);
    }
    tokens.push_back({at, len, dist});
    for (std::size_t k = at + 1; k < at + len; ++k) {
      insert(k);
    }
    i = at + len;
  }
  const std::size_t first = tokens.empty() ? n : tokens[0].at;
  out.literal_length(first);
  out.buf.insert(out.buf.end(), src.begin(), src.begin() + static_cast<std::ptrdiff_t>(first));
  for (std::size_t t = 0; t < tokens.size(); ++t) {
    const auto [start, len, dist] = tokens[t];
    const std::size_t end = start + len;
    const std::size_t next_start = t + 1 < tokens.size() ? tokens[t + 1].at : n;
    const std::size_t lit = next_start - end;
    out.match(len, dist, lit);
    if (lit > 3) {
      out.literal_length(lit);
    }
    out.buf.insert(out.buf.end(), src.begin() + static_cast<std::ptrdiff_t>(end),
                   src.begin() + static_cast<std::ptrdiff_t>(next_start));
  }
  out.byte(0x11);
  return out.buf;
}

ByteBuf compress_r2007(Span src) {
  const std::size_t n = src.size();
  ByteWriter out(n + (n >> 2U) + 64);
  constexpr unsigned kHb = 15;
  std::vector<std::int32_t> head(std::size_t{1} << kHb, -1);
  std::vector<std::int32_t> prev(n, -1);
  const auto hash = [&](std::size_t i) -> std::size_t {
    const std::uint64_t v =
        (static_cast<std::uint64_t>(src[i]) << 16U) | (static_cast<std::uint64_t>(src[i + 1]) << 8U) | src[i + 2];
    return static_cast<std::uint32_t>(v * 2654435761ULL) >> (32U - kHb);
  };
  const auto insert = [&](std::size_t i) {
    if (i + 2 < n) {
      const std::size_t h = hash(i);
      prev[i] = head[h];
      head[h] = static_cast<std::int32_t>(i);
    }
  };
  struct Tok {
    std::size_t at, len, dist;
  };
  std::vector<Tok> tokens;
  std::size_t i = 0;
  while (i < n) {
    std::size_t best_len = 0;
    std::size_t best_dist = 0;
    if (i + 3 <= n) {
      std::int32_t c = head[hash(i)];
      int chain = 0;
      while (c >= 0 && chain++ < 48) {
        const auto cc = static_cast<std::size_t>(c);
        const std::size_t d = i - cc;
        if (d > 0xF000) {
          break;
        }
        if (src[cc] == src[i] && i + best_len < n && src[cc + best_len] == src[i + best_len]) {
          std::size_t l = 0;
          const std::size_t max = std::min<std::size_t>(n - i, 0x100 + 0xFFFF);
          while (l < max && src[cc + l] == src[i + l]) {
            ++l;
          }
          if (l > best_len) {
            best_len = l;
            best_dist = d;
          }
        }
        c = prev[cc];
      }
    }
    if (best_len >= 3 && i != 0) {
      tokens.push_back({i, best_len, best_dist});
      for (std::size_t k = i; k < i + best_len; ++k) {
        insert(k);
      }
      i += best_len;
    } else {
      insert(i);
      ++i;
    }
  }
  const auto& P = perm();
  const auto literal = [&](std::size_t a, std::size_t b) {
    std::size_t p = a;
    std::array<std::uint8_t, 32> blk{};
    while (b - p >= 32) {
      const auto& pm = P[32];
      for (std::size_t o = 0; o < 32; ++o) {
        blk[pm[o]] = src[p + o];
      }
      out.raw(std::span<const std::uint8_t>(blk));
      p += 32;
    }
    if (b - p != 0) {
      const std::size_t m = b - p;
      const auto& pm = P[m];
      for (std::size_t o = 0; o < m; ++o) {
        blk[pm[o]] = src[p + o];
      }
      out.raw(std::span<const std::uint8_t>(blk).first(m));
    }
  };
  const auto lit_len_op = [&](std::size_t len) {
    if (len <= 22) {
      out.RC(static_cast<double>(len - 8));
      return;
    }
    out.RC(0x0F);
    std::size_t r = len - 0x17;
    if (r < 0xFF) {
      out.RC(static_cast<double>(r));
      return;
    }
    out.RC(0xFF);
    r -= 0xFF;
    while (r >= 0xFFFF) {
      out.RS(0xFFFF);
      r -= 0xFFFF;
    }
    out.RS(static_cast<double>(r));
  };
  const std::size_t first_end = tokens.empty() ? n : tokens[0].at;
  if (first_end >= 8) {
    lit_len_op(first_end);
    literal(0, first_end);
  } else {
    out.raw({0x20, 0, 0, static_cast<std::uint8_t>(first_end & 7U)});
    literal(0, first_end);
  }
  bool after_match = false;
  for (std::size_t t = 0; t < tokens.size(); ++t) {
    const auto [start, len, dist] = tokens[t];
    const std::size_t next = t + 1 < tokens.size() ? tokens[t + 1].at : n;
    const std::size_t lit = next - (start + len);
    const std::size_t lit3 = lit >= 1 && lit <= 7 ? lit : 0;
    const std::size_t d1 = dist - 1;
    const auto rc = [&](std::size_t v) { out.RC(static_cast<double>(v & 0xFFU)); };
    if ((len <= 14 || (len == 15 && !after_match)) && d1 < 512) {
      rc((len << 4U) | (d1 & 15U));
      rc(((d1 >> 4U) << 3U) | lit3);
    } else if (len <= 18 && d1 < 8192) {
      rc(0x10U | (len - 3));
      rc(d1 & 0xFFU);
      rc(((d1 >> 8U) << 3U) | lit3);
    } else if (len >= 0x13 && len <= 0x32 && d1 < 4096) {
      std::size_t L = len - 0x13;
      const bool hi = L >= 16;
      if (hi) {
        L -= 16;
      }
      rc(after_match ? (0xF0U | L) : L);
      rc(d1 & 0xFFU);
      rc(((d1 >> 8U) << 3U) | (hi ? 0x80U : 0U) | lit3);
    } else if (len < 0x100 && dist <= 0xFFFF) {
      rc(0x20U | (len & 7U));
      rc(dist & 0xFFU);
      rc(dist >> 8U);
      rc((len & 0xF8U) | lit3);
    } else {
      const std::size_t L = len - 0x100;
      rc(0x28U | (L & 7U));
      rc(d1 & 0xFFU);
      rc(d1 >> 8U);
      rc((L >> 3U) & 0xFFU);
      rc(((L >> 11U) << 3U) | lit3);
    }
    if (lit >= 8) {
      lit_len_op(lit);
    }
    literal(start + len, next);
    after_match = lit == 0;
  }
  return out.buf;
}

// ---------------------------------------------------------------- R13–R15

ByteBuf layout_r2000(const Parts& p) {
  constexpr std::string_view version = "AC1015";
  const auto [code, maint] = version_info(version);
  constexpr int kCodepage = 30;
  ByteWriter out(1U << 16U);
  constexpr std::size_t kHdrLen = 0x19 + 6 * 9 + 2 + 16;
  out.zeros(kHdrLen);
  const ByteBuf aux = aux_header(version, p);
  const std::size_t ax_at = out.pos();
  out.raw(aux);
  const std::size_t pv_at = out.pos();
  out.raw(preview_section());
  const std::size_t hv_at = out.pos();
  const ByteBuf hv = header_section(p.header, version);
  out.raw(hv);
  const std::size_t cl_at = out.pos();
  const ByteBuf cl = classes_section(p.classes, version);
  out.raw(cl);
  out.zeros(0x200);
  const std::size_t obj_start = out.pos();
  std::vector<std::pair<double, double>> map;
  map.reserve(p.objects.size());
  for (const auto& o : p.objects) {
    map.emplace_back(o.handle, static_cast<double>(out.pos()));
    out.raw(o.data);
  }
  const std::size_t om_at = out.pos();
  const ByteBuf om = object_map(map);
  out.raw(om);
  const std::size_t fs_at = out.pos();
  const ByteBuf fs = obj_free_space(static_cast<double>(map.size()), p.updated, static_cast<double>(obj_start));
  out.raw(fs);
  struct Loc {
    int n;
    std::size_t at, size;
  };
  const std::array<Loc, 4> loc = {Loc{0, hv_at, hv.size()}, Loc{1, cl_at, cl.size()}, Loc{2, om_at, om.size()},
                                  Loc{3, fs_at, fs.size()}};
  const std::size_t sh_at = out.pos();
  {
    BitWriter w(version, 160);
    w.BL(static_cast<double>(sh_at));
    for (const int c : {0x41, 0x43, 0x31, 0x30, 0x31, 0x35, 0, 0, 0, 0, 0}) {
      w.RC(c);
    }
    w.RC(maint);
    w.RC(1);
    w.BS(code | (maint << 8));
    w.RS(kCodepage);
    w.BS(6);
    std::vector<Loc> all(loc.begin(), loc.end());
    all.push_back({4, 0, 1});
    all.push_back({5, ax_at, aux.size()});
    for (const auto& [n, at, size] : all) {
      w.RC(n);
      w.BL(static_cast<double>(at));
      w.BL(static_cast<double>(size));
    }
    w.BS(static_cast<double>(p.keys.size()));
    for (std::size_t i = 0; i < p.keys.size(); ++i) {
      const auto b = handle_bytes(p.keys[i]);
      w.RC(static_cast<double>(b.size()));
      w.RC(static_cast<double>(i));
      for (const std::uint8_t c : b) {
        w.RC(c);
      }
    }
    const ByteBuf data = w.bytes();
    out.raw(kSecond);
    const std::size_t size_at = out.pos();
    out.RL(static_cast<double>(4 + data.size() + 2 + 8));
    out.raw(data);
    out.RS(crc16(0xC0C1, Span(out.buf).subspan(size_at)));
    out.zeros(8);
    out.raw(kSecondEnd);
  }
  const std::size_t tpl_at = out.pos();
  const ByteBuf tpl = template_section(p.measurement, "AC1015");
  out.raw(tpl);
  ByteWriter fh(128);
  out.buf.reserve(out.buf.size());
  put_ascii(fh, "AC1015");
  fh.raw({0, 0, 0, 0, 0, static_cast<std::uint8_t>(maint), 1});
  fh.RL(static_cast<double>(pv_at));
  fh.RC(code);
  fh.RC(maint);
  fh.RS(kCodepage);
  fh.RL(6);
  std::vector<Loc> all(loc.begin(), loc.end());
  all.push_back({4, tpl_at, tpl.size()});
  all.push_back({5, ax_at, aux.size()});
  for (const auto& [n, at, size] : all) {
    fh.RC(n);
    fh.RL(static_cast<double>(at));
    fh.RL(static_cast<double>(size));
  }
  fh.RS(crc16(0, fh.buf) ^ 0x8461U);
  fh.raw(kFilehdrEnd);
  if (fh.pos() != kHdrLen) {
    throw std::logic_error("DWG file header size mismatch");
  }
  std::ranges::copy(fh.buf, out.buf.begin());
  return out.buf;
}

// ---------------------------------------------------------------- R2004 family

namespace {

constexpr std::uint32_t kPageData = 0x4163043B;
constexpr std::uint32_t kPageMap = 0x41630E3B;
constexpr std::uint32_t kSectionMap = 0x4163003B;

ByteBuf system_page04(std::uint32_t type, const ByteBuf& data) {
  const ByteBuf z = compress_r2004(data);
  ByteWriter h(0x14);
  h.RL(type);
  h.RL(static_cast<double>(data.size()));
  h.RL(static_cast<double>(z.size()));
  h.RL(2);
  h.RL(0);
  ByteBuf hb = h.buf;
  const std::uint32_t sum = page_checksum(page_checksum(0, hb), z);
  put_u32(hb, 16, sum);
  ByteWriter o(z.size() + 0x28);
  o.raw(hb);
  o.raw(z);
  o.RL(type);
  o.RL(0);
  o.RL(0);
  o.RL(2);
  o.RL(0);
  return o.buf;
}

}  // namespace

ByteBuf layout_r2004(const Parts& p, std::string_view version) {
  const auto [code, maint] = version_info(version);
  constexpr int kCodepage = 30;
  const auto& M = magic();
  ByteWriter ob(1U << 16U);
  ob.RL(0x0DCA);
  std::vector<std::pair<double, double>> map;
  map.reserve(p.objects.size());
  for (const auto& o : p.objects) {
    map.emplace_back(o.handle, static_cast<double>(ob.pos()));
    ob.raw(o.data);
  }
  struct Sec {
    std::string name;
    ByteBuf data;
    int compressed;
    std::size_t max_size;
  };
  std::vector<Sec> sections;
  sections.push_back({"AcDb:SummaryInfo", summary_info(version, p), 1, 0x100});
  sections.push_back({"AcDb:Preview", preview_section(), 1, 0x400});
  if (p.acds) {
    sections.push_back({"AcDb:AcDsPrototype_1b", *p.acds, 2, 0x7400});
  }
  sections.push_back({"AcDb:RevHistory", rev_history(version), 2, 0x7400});
  sections.push_back({"AcDb:AcDbObjects", std::move(ob.buf), 2, 0x7400});
  sections.push_back({"AcDb:ObjFreeSpace", obj_free_space(static_cast<double>(map.size()), p.updated, 0), 2, 0x7400});
  sections.push_back({"AcDb:Template", template_section(p.measurement, version), 2, 0x7400});
  sections.push_back({"AcDb:Handles", object_map(map), 2, 0x7400});
  sections.push_back({"AcDb:Classes", classes_section(p.classes, version), 2, 0x7400});
  sections.push_back({"AcDb:AuxHeader", aux_header(version, p), 2, 0x7400});
  sections.push_back({"AcDb:Header", header_section(p.header, version), 2, 0x7400});
  std::vector<std::string> map_order;
  if (p.acds) {
    map_order.emplace_back("AcDb:AcDsPrototype_1b");
  }
  for (const char* n : {"AcDb:Preview", "AcDb:SummaryInfo", "AcDb:RevHistory", "AcDb:AcDbObjects", "AcDb:ObjFreeSpace",
                        "AcDb:Template", "AcDb:Handles", "AcDb:Classes", "AcDb:AuxHeader", "AcDb:Header"}) {
    map_order.emplace_back(n);
  }
  std::map<std::string, std::size_t> ids;
  for (std::size_t i = 0; i < map_order.size(); ++i) {
    ids[map_order[i]] = map_order.size() - i;
  }
  ByteWriter out(1U << 16U);
  out.zeros(0x100);
  struct Page {
    std::size_t id, addr, size;
  };
  struct DescPage {
    std::size_t id, data_size, start;
  };
  struct Desc {
    std::string name;
    std::size_t size, max_size;
    int compressed;
    std::size_t id;
    std::vector<DescPage> pages;
  };
  std::vector<Page> pages;
  std::vector<Desc> descs;
  std::size_t page_id = 0;
  const auto pad32 = [&] {
    while (out.pos() % 0x20 != 0) {
      out.RC(M[out.pos() % 0x100]);
    }
  };
  for (const auto& [name, data, compressed, max_size] : sections) {
    const std::size_t id = ids[name];
    Desc desc{name, data.size(), max_size, compressed, id, {}};
    for (std::size_t start = 0; start < std::max<std::size_t>(1, data.size()); start += max_size) {
      ByteBuf full(max_size, 0);
      const std::size_t take = std::min(data.size(), start + max_size) - std::min(data.size(), start);
      std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), start)), take, full.begin());
      const ByteBuf payload = compressed == 2 ? compress_r2004(full) : full;
      const std::size_t addr = out.pos();
      const std::size_t page_size = (32 + payload.size() + 0x1F) & ~std::size_t{0x1F};
      const std::uint32_t data_sum = page_checksum(0, payload);
      ByteWriter hdr(32);
      hdr.RL(kPageData);
      hdr.RL(static_cast<double>(id));
      hdr.RL(static_cast<double>(payload.size()));
      hdr.RL(static_cast<double>(page_size));
      hdr.RLL(static_cast<std::uint64_t>(start));
      hdr.RL(0);
      hdr.RL(data_sum);
      ByteBuf h = hdr.buf;
      const std::uint32_t hdr_sum = page_checksum(data_sum, h);
      put_u32(h, 24, hdr_sum);
      const std::uint32_t mask = 0x4164536BU ^ static_cast<std::uint32_t>(addr);
      for (std::size_t k = 0; k < 32; k += 4) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(h[k]) | (static_cast<std::uint32_t>(h[k + 1]) << 8U) |
             (static_cast<std::uint32_t>(h[k + 2]) << 16U) | (static_cast<std::uint32_t>(h[k + 3]) << 24U)) ^
            mask;
        put_u32(h, k, v);
      }
      out.raw(h);
      out.raw(payload);
      pad32();
      const std::size_t pid = ++page_id;
      pages.push_back({pid, addr, out.pos() - addr});
      desc.pages.push_back({pid, payload.size(), start});
      if (data.empty()) {
        break;
      }
    }
    descs.push_back(std::move(desc));
  }
  const std::size_t section_map_id = page_id + 3;
  const std::size_t page_map_id = page_id + 4;
  ByteWriter sm(4096);
  std::vector<const Desc*> all;
  const Desc empty{"", 0, 0x7400, 2, 0, {}};
  all.push_back(&empty);
  for (const auto& n : map_order) {
    for (const auto& d : descs) {
      if (d.name == n) {
        all.push_back(&d);
        break;
      }
    }
  }
  sm.RL(static_cast<double>(all.size()));
  sm.RL(2);
  sm.RL(0x7400);
  sm.RL(0);
  sm.RL(static_cast<double>(all.size()));
  for (const Desc* d : all) {
    sm.RLL(static_cast<std::uint64_t>(d->size));
    sm.RL(static_cast<double>(d->pages.size()));
    sm.RL(static_cast<double>(d->max_size));
    sm.RL(1);
    sm.RL(d->compressed);
    sm.RL(static_cast<double>(d->id));
    sm.RL(0);
    std::array<std::uint8_t, 64> nm{};
    std::copy_n(d->name.begin(), std::min<std::size_t>(64, d->name.size()), nm.begin());
    sm.raw(nm);
    for (const auto& pg : d->pages) {
      sm.RL(static_cast<double>(pg.id));
      sm.RL(static_cast<double>(pg.data_size));
      sm.RLL(static_cast<std::uint64_t>(pg.start));
    }
  }
  const std::size_t sm_addr = out.pos();
  out.raw(system_page04(kSectionMap, sm.buf));
  pad32();
  pages.push_back({section_map_id, sm_addr, out.pos() - sm_addr});
  const std::size_t pm_addr = out.pos();
  const auto pm_entries = [&](std::size_t pm_size) {
    ByteWriter pm(pages.size() * 8 + 16);
    for (const auto& pg : pages) {
      pm.RL(static_cast<double>(pg.id));
      pm.RL(static_cast<double>(pg.size));
    }
    pm.RL(static_cast<double>(page_map_id));
    pm.RL(static_cast<double>(pm_size));
    return pm.buf;
  };
  std::size_t pm_size = 0x20;
  ByteBuf pm_bytes;
  for (int k = 0; k < 4; ++k) {
    pm_bytes = system_page04(kPageMap, pm_entries(pm_size));
    const std::size_t s = (pm_bytes.size() + 0x1F) & ~std::size_t{0x1F};
    if (s <= pm_size) {
      break;
    }
    pm_size = s;
  }
  if (pm_bytes.size() < pm_size) {
    pm_bytes.resize(pm_size, 0);
  }
  out.raw(pm_bytes);
  const std::size_t second_at = out.pos();
  ByteWriter fhd(0x6C);
  put_ascii(fhd, "AcFssFcAJMB");
  fhd.RC(0);
  fhd.RL(0);
  fhd.RL(0x6C);
  fhd.RL(0x04);
  fhd.RL(0);
  fhd.RL(0);
  fhd.RL(0);
  fhd.RL(1);
  fhd.RL(static_cast<double>(page_map_id));
  fhd.RLL(static_cast<std::uint64_t>(pm_addr + pm_size - 0x100));
  fhd.RLL(static_cast<std::uint64_t>(second_at));
  fhd.RL(0);
  fhd.RL(static_cast<double>(page_id + 2));
  fhd.RL(0x20);
  fhd.RL(0x80);
  fhd.RL(0x40);
  fhd.RL(static_cast<double>(page_map_id));
  fhd.RLL(static_cast<std::uint64_t>(pm_addr - 0x100));
  fhd.RL(static_cast<double>(section_map_id));
  fhd.RL(static_cast<double>(page_map_id));
  fhd.RL(0);
  fhd.RL(0);
  ByteBuf plain = fhd.buf;
  const std::uint32_t c = crc32(plain, 0);
  put_u32(plain, 0x68, c);
  ByteBuf enc(plain.size());
  for (std::size_t i = 0; i < plain.size(); ++i) {
    enc[i] = plain[i] ^ M[i];
  }
  out.raw(enc);
  ByteWriter head(0x100);
  put_ascii(head, version);
  head.raw({0, 0, 0, 0, 0});
  head.RC(maint);
  head.RC(3);
  const auto sec_index = [&](std::string_view n) {
    for (std::size_t i = 0; i < sections.size(); ++i) {
      if (sections[i].name == n) {
        return i;
      }
    }
    return std::size_t{0};
  };
  const Page& pv = pages.at(sec_index("AcDb:Preview"));
  const Page& si = pages.at(sec_index("AcDb:SummaryInfo"));
  head.RL(static_cast<double>(pv.addr + 0x20));
  head.RC(code);
  head.RC(maint);
  head.RS(kCodepage);
  head.raw({0, 0, 0});
  head.RL(0);
  head.RL(0);
  head.RL(static_cast<double>(si.addr + 0x20));
  head.RL(0);
  head.RL(0x80);
  head.zeros(0x80 - head.pos());
  head.raw(enc);
  head.raw(std::span<const std::uint8_t>(M).subspan(0xEC, 0x100 - 0xEC));
  std::ranges::copy(head.buf, out.buf.begin());
  return out.buf;
}

// ---------------------------------------------------------------- R2007

ByteBuf layout_r2007(const Parts& p) {
  constexpr std::string_view version = "AC1021";
  const auto [code, maint] = version_info(version);
  CrcRandom rnd(p.random_seed);
  ByteWriter ob(1U << 16U);
  ob.RL(0x0DCA);
  std::vector<std::pair<double, double>> map;
  map.reserve(p.objects.size());
  for (const auto& o : p.objects) {
    map.emplace_back(o.handle, static_cast<double>(ob.pos()));
    ob.raw(o.data);
  }
  struct Sec {
    std::string name;
    ByteBuf data;
    int encoding;
    std::size_t max_size;
  };
  std::vector<Sec> sections;
  sections.push_back({"AcDb:SummaryInfo", summary_info(version, p), 1, 0x80});
  {
    ByteBuf pv = preview_section();
    pv.resize(pv.size() + 32, 0);
    sections.push_back({"AcDb:Preview", std::move(pv), 1, 0x400});
  }
  sections.push_back({"AcDb:RevHistory", rev_history(version), 4, 0x1000});
  sections.push_back({"AcDb:AcDbObjects", std::move(ob.buf), 4, 0xF800});
  sections.push_back({"AcDb:ObjFreeSpace", obj_free_space(static_cast<double>(map.size()), p.updated, 0), 4, 0xF800});
  sections.push_back({"AcDb:Template", template_section(p.measurement, version), 4, 0x400});
  sections.push_back({"AcDb:Handles", object_map(map), 4, 0xF800});
  sections.push_back({"AcDb:Classes", classes_section(p.classes, version), 4, 0xF800});
  sections.push_back({"AcDb:AuxHeader", aux_header(version, p), 4, 0x800});
  sections.push_back({"AcDb:Header", header_section(p.header, version), 4, 0x800});
  if (sections[0].data.size() > 0x80) {
    sections[0].max_size = (sections[0].data.size() + 0x7F) & ~std::size_t{0x7F};
  }
  struct DataPage {
    std::size_t id;
    ByteBuf bytes;
    std::size_t size;
  };
  struct DescPage {
    std::size_t offset, size, id, uncomp, comp;
    std::uint64_t checksum, crc;
  };
  struct Desc {
    std::string name;
    std::size_t size, max_size;
    int encoding;
    std::vector<DescPage> pages;
  };
  std::vector<DataPage> data_pages;
  std::vector<Desc> descs;
  std::size_t pid = 0;
  for (const auto& [name, data, encoding, max_size] : sections) {
    Desc desc{name, data.size(), max_size, encoding, {}};
    const auto add_page = [&](std::size_t offset, Span chunk, Span comp, std::size_t size_field) {
      const std::uint32_t checksum = r2007_page_checksum(0, chunk);
      const std::uint64_t crc = crc64_mirrored(comp, update_seed1(0, comp.size()));
      ByteBuf stored;
      if (encoding == 4) {
        ByteBuf c8(comp.begin(), comp.end());
        c8 = pad8(c8);
        stored = rs_encode(c8, rs_data(), true, [&] { return rnd.pad_byte(); });
      } else {
        stored.assign(max_size + 0x20, 0);
        std::ranges::copy(chunk, stored.begin());
        for (std::size_t i = (chunk.size() + 7) & ~std::size_t{7}; i < stored.size(); ++i) {
          stored[i] = rnd.pad_byte();
        }
      }
      const std::size_t size = align20(stored.size());
      ByteBuf bytes(size, 0);
      std::ranges::copy(stored, bytes.begin());
      const std::size_t id = ++pid;
      data_pages.push_back({id, std::move(bytes), size});
      desc.pages.push_back({offset, size_field, id, chunk.size(), comp.size(), checksum, crc});
    };
    for (std::size_t start = 0; start < data.size() || (start == 0 && data.empty()); start += max_size) {
      const Span chunk = Span(data).subspan(start, std::min(data.size(), start + max_size) - start);
      ByteBuf z;
      if (encoding == 4) {
        z = compress_r2007(chunk);
      }
      const Span zs = encoding == 4 ? Span(z) : chunk;
      if (encoding != 4 || zs.size() < chunk.size() || chunk.size() <= 248) {
        add_page(start, chunk, zs.size() < chunk.size() ? zs : chunk,
                 chunk.size() == max_size ? max_size : std::max(max_size, chunk.size()));
      } else {
        for (std::size_t s0 = 0; s0 < chunk.size(); s0 += 248) {
          const Span piece = chunk.subspan(s0, std::min(chunk.size(), s0 + 248) - s0);
          add_page(start + s0, piece, piece, piece.size());
        }
      }
      if (data.empty()) {
        break;
      }
    }
    descs.push_back(std::move(desc));
  }
  const std::size_t sm_id1 = pid + 1;
  const std::size_t sm_id2 = pid + 2;
  const std::size_t pm_id1 = pid + 3;
  const std::size_t pm_id2 = pid + 4;
  ByteWriter sm(4096);
  for (const auto& d : descs) {
    const std::size_t name_len = d.name.empty() ? 0 : (d.name.size() + 1) * 2;
    sm.RLL(static_cast<std::uint64_t>(d.size));
    sm.RLL(static_cast<std::uint64_t>(d.max_size));
    sm.RLL(std::uint64_t{0});
    sm.RLL(static_cast<std::uint64_t>(d.name.empty() ? 0 : name_hash(d.name)));
    sm.RLL(static_cast<std::uint64_t>(name_len));
    sm.RLL(std::uint64_t{0});
    sm.RLL(static_cast<std::uint64_t>(d.encoding));
    sm.RLL(static_cast<std::uint64_t>(d.pages.size()));
    if (!d.name.empty()) {
      for (const char ch : d.name) {
        sm.RS(static_cast<unsigned char>(ch));
      }
      sm.RS(0);
    }
    for (const auto& pg : d.pages) {
      sm.RLL(static_cast<std::uint64_t>(pg.offset));
      sm.RLL(static_cast<std::uint64_t>(pg.size));
      sm.RLL(static_cast<std::uint64_t>(pg.id));
      sm.RLL(static_cast<std::uint64_t>(pg.uncomp));
      sm.RLL(static_cast<std::uint64_t>(pg.comp));
      sm.RLL(pg.checksum);
      sm.RLL(pg.crc);
    }
  }
  const ByteBuf sm_data = sm.buf;
  const SysPage21 sm_page1 = system_page21(sm_data, rnd);
  const SysPage21 sm_page2 = system_page21(sm_data, rnd);
  const std::size_t n_pages = 4 + data_pages.size();
  const std::size_t pm_size = system_page_size(n_pages * 16);
  ByteWriter pm(n_pages * 16);
  pm.RLL(static_cast<std::uint64_t>(pm_size));
  pm.RLL(static_cast<std::uint64_t>(pm_id1));
  pm.RLL(static_cast<std::uint64_t>(pm_size));
  pm.RLL(static_cast<std::uint64_t>(pm_id2));
  for (const auto& dp : data_pages) {
    pm.RLL(static_cast<std::uint64_t>(dp.size));
    pm.RLL(static_cast<std::uint64_t>(dp.id));
  }
  pm.RLL(static_cast<std::uint64_t>(sm_page1.size));
  pm.RLL(static_cast<std::uint64_t>(sm_id1));
  pm.RLL(static_cast<std::uint64_t>(sm_page2.size));
  pm.RLL(static_cast<std::uint64_t>(sm_id2));
  const ByteBuf pm_data = pm.buf;
  const SysPage21 pm_page1 = system_page21(pm_data, rnd);
  const SysPage21 pm_page2 = system_page21(pm_data, rnd);
  if (pm_page1.size != pm_size) {
    throw std::logic_error("R2007 page map size");
  }
  ByteWriter out(0x480 + pm_size * 2 + sm_page1.size * 2 + 0x400);
  out.zeros(0x480);
  out.raw(pm_page1.bytes);
  out.raw(pm_page2.bytes);
  std::map<std::size_t, std::size_t> page_at;
  for (const auto& dp : data_pages) {
    page_at[dp.id] = out.pos();
    out.raw(dp.bytes);
  }
  out.raw(sm_page1.bytes);
  out.raw(sm_page2.bytes);
  const std::size_t header2_at = out.pos();
  const std::size_t file_size = header2_at + 0x400;
  constexpr std::uint64_t kCrcSeed = 0;
  ByteWriter fh(0x110);
  const std::uint64_t sections_map_crc_seed = rnd.encode(0);
  const std::uint64_t pages_map_crc_seed = rnd.encode(0);
  const std::uint64_t random1 = rnd.next64();
  const std::uint64_t random2 = rnd.next64();
  const std::uint64_t encoded_crc_seed = rnd.encode(0);
  const auto b64 = [](std::initializer_list<std::uint64_t> arr) {
    ByteWriter o(64);
    for (const std::uint64_t v : arr) {
      o.RLL(v);
    }
    return o.buf;
  };
  const std::uint64_t n0 = rot(random1, random2);
  const std::uint64_t n1 = rot(n0, n0);
  const std::uint64_t n2 = rot(random2, n1);
  const std::uint64_t n3 = rot(n2, n2);
  const std::uint64_t n4 = rot(random1, n3);
  const std::uint64_t n5 = rot(n4, n4);
  const std::uint64_t n6 = rot(n5, n5);
  const std::uint64_t n7 = rot(n6, n6);
  const std::uint64_t normal_crc = crc64_normal(b64({n0, n1, n2, n3, n4, n5, n6, n7}), ~random2);
  const std::uint64_t m0 = rot(random1, random2);
  const std::uint64_t m1 = rot(normal_crc, m0);
  const std::uint64_t m2 = rot(random2, m1);
  const std::uint64_t m3 = rot(normal_crc, m2);
  const std::uint64_t m4 = rot(random1, m3);
  const std::uint64_t m5 = rot(normal_crc, m4);
  const std::uint64_t m6 = rot(random2, m5);
  const std::uint64_t m7 = rot(m6, m6);
  const std::uint64_t mirrored_crc = crc64_mirrored(b64({m0, m1, m2, m3, m4, m5, m6, m7}), ~random1);
  const std::uint64_t crc_seed_encoded = rnd.encode(0);
  const auto L = [&](std::size_t v) { fh.RLL(static_cast<std::uint64_t>(v)); };
  L(0x70);
  L(file_size);
  fh.RLL(pm_page1.crc_compressed);
  L(pm_page1.factor);
  fh.RLL(pages_map_crc_seed);
  L(pm_size);
  L(pm_id2);
  L(0);
  L(pm_id1);
  L(header2_at - 0x480);
  L(pm_page1.comp_size);
  L(pm_data.size());
  L(n_pages);
  L(pm_id2);
  L(0x20);
  L(0x40);
  fh.RLL(pm_page1.crc_uncompressed);
  L(0xF800);
  L(4);
  L(1);
  L(descs.size() + 1);
  fh.RLL(sm_page1.crc_uncompressed);
  L(sm_page1.comp_size);
  L(sm_id2);
  L(sm_id1);
  L(sm_data.size());
  fh.RLL(sm_page1.crc_compressed);
  L(sm_page1.factor);
  fh.RLL(sections_map_crc_seed);
  L(0x60100);
  fh.RLL(kCrcSeed);
  fh.RLL(crc_seed_encoded);
  fh.RLL(rnd.seed);
  L(0);
  ByteBuf hd = fh.buf;
  const std::uint64_t hcrc = crc64_normal(hd, update_seed2(0, hd.size()));
  for (unsigned k = 0; k < 8; ++k) {
    hd.at(0x108 + k) = static_cast<std::uint8_t>((hcrc >> (8U * k)) & 0xFFU);
  }
  const ByteBuf hz0 = compress_r2007(hd);
  const bool shrank = hz0.size() < hd.size();
  const ByteBuf& hz = shrank ? hz0 : hd;
  const std::uint64_t data_crc = crc64_normal(hz, update_seed2(0, hz.size()));
  const std::uint64_t seq0 = rnd.next64();
  const std::uint64_t seq1 = rot(seq0, seq0);
  const ByteBuf seq_bytes = b64({seq0, seq1});
  const std::uint64_t seq_crc = crc64_normal(seq_bytes, update_seed1(0, 16));
  ByteWriter blk(0x140);
  blk.RLL(seq_crc);
  blk.RLL(seq0);
  blk.RLL(data_crc);
  blk.RL(shrank ? static_cast<double>(hz.size()) : -static_cast<double>(hd.size()));
  blk.RL(0);
  blk.raw(hz);
  while (blk.pos() % 8 != 0) {
    blk.RC(rnd.pad_byte());
  }
  const ByteBuf& block = blk.buf;
  ByteBuf pre(std::size_t{3} * 239, 0);
  std::size_t q = 0;
  while (q + block.size() <= pre.size()) {
    std::ranges::copy(block, pre.begin() + static_cast<std::ptrdiff_t>(q));
    q += block.size();
  }
  for (; q < pre.size(); ++q) {
    pre[q] = rnd.pad_byte();
  }
  const ByteBuf rs_hdr = rs_encode(pre, rs_sys(), true, [&] { return rnd.pad_byte(); });
  ByteBuf page(0x400, 0);
  std::ranges::copy(rs_hdr, page.begin());
  for (std::size_t i = rs_hdr.size(); i < 0x3D8; ++i) {
    page[i] = rnd.pad_byte();
  }
  const ByteBuf chk = b64({normal_crc, mirrored_crc, random1, random2, encoded_crc_seed});
  std::copy_n(chk.begin(), 0x28, page.begin() + 0x3D8);
  std::ranges::copy(page, out.buf.begin() + 0x80);
  out.raw(page);
  ByteWriter meta(0x80);
  put_ascii(meta, version);
  meta.raw({0, 0, 0, 0, 0});
  meta.RC(maint);
  meta.RC(3);
  meta.RL(static_cast<double>(page_at.at(data_pages.at(1).id)));
  meta.RC(code);
  meta.RC(maint);
  meta.RS(30);
  meta.raw({0, 0, 0});
  meta.RL(0);
  meta.RL(0);
  meta.RL(static_cast<double>(page_at.at(data_pages.at(0).id)));
  meta.RL(0);
  meta.RL(0x80);
  meta.RL(0);
  std::ranges::copy(meta.buf, out.buf.begin());
  return out.buf;
}

}  // namespace cc::dwg::w
