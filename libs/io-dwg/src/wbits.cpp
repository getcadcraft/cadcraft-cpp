// SPDX-License-Identifier: Apache-2.0
// bits-write.js in C++ (see wbits.hpp).
#include "wbits.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

#include "cc/base/js.hpp"

namespace cc::dwg::w {

namespace {

constexpr std::array<std::string_view, 8> kOrder = {"AC1012", "AC1014", "AC1015", "AC1018",
                                                    "AC1021", "AC1024", "AC1027", "AC1032"};

/// Math.trunc(v) || 0
double trunc0(double v) noexcept {
  const double t = std::trunc(v);
  return t == 0 || t != t ? 0 : t;
}
std::uint8_t lo8(double v) noexcept {
  return static_cast<std::uint8_t>(base::js_to_int32(v) & 0xFF);
}
/// p?.[k] || 0 as a number
double or0(const Value& p, std::string_view k) {
  if (!p.is_object()) {
    return 0;
  }
  const Value v = p.get(k);
  if (!v.truthy()) {
    return 0;
  }
  return v.is_number() ? v.as_number() : base::js_to_number(v);
}

}  // namespace

int v_index(std::string_view v) noexcept {
  for (std::size_t i = 0; i < kOrder.size(); ++i) {
    if (kOrder[i] == v) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

TextLoss& text_loss() noexcept {
  thread_local TextLoss loss;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): one write per thread
  return loss;
}

std::string clean_text(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == 0) {
      continue;
    }
    out.push_back((c < 0x20 && c != 0x09) || c == 0x7F ? ' ' : ch);
  }
  return out;
}

std::string clean_text_of(const Value& s) {
  if (s.is_undefined() || s.is_null()) {
    return {};
  }
  return s.is_string() ? clean_text(s.as_string()) : clean_text(base::js_string(s));
}

ByteBuf encode1252(std::string_view utf8) {
  const std::string s = clean_text(utf8);
  ByteBuf out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c0 = static_cast<unsigned char>(s[i]);
    std::uint32_t c = c0;
    std::size_t len = 1;
    if (c0 >= 0xF0 && i + 3 < s.size()) {
      c = ((c0 & 0x07U) << 18U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12U) |
          ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6U) | (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
      len = 4;
    } else if (c0 >= 0xE0 && i + 2 < s.size()) {
      c = ((c0 & 0x0FU) << 12U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
          (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
      len = 3;
    } else if (c0 >= 0xC0 && i + 1 < s.size()) {
      c = ((c0 & 0x1FU) << 6U) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
      len = 2;
    }
    i += len;
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
      out.push_back(static_cast<std::uint8_t>(c));
    } else if (c > 0xFFFF) {
      out.push_back(0x3F);
      text_loss().lost += 1;
    } else {
      std::string hex = "\\U+";
      for (int k = 12; k >= 0; k -= 4) {
        const unsigned d = (c >> static_cast<unsigned>(k)) & 15U;
        hex.push_back(static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
      }
      for (const char ch : hex) {
        out.push_back(static_cast<std::uint8_t>(ch));
      }
    }
  }
  return out;
}

NormColor norm_color(const Value& c) {
  NormColor n;
  if (c.is_undefined() || c.is_null()) {
    n.index = 256;
    return n;
  }
  if (c.is_number()) {
    n.index = c;
    return n;
  }
  const Value rgb = c.get("rgb");
  if (!rgb.is_undefined() && !rgb.is_null()) {
    n.rgb_set = true;
    n.rgb = base::js_to_uint32(base::js_to_number(rgb));
    n.index = c.get("index");
  } else {
    const Value i = c.get("index");
    n.index =
        i.is_number() && std::isfinite(i.as_number()) && std::trunc(i.as_number()) == i.as_number() ? i : Value(256);
  }
  n.name = c.get("name");
  n.book = c.get("book");
  return n;
}

std::vector<std::uint8_t> handle_bytes(double v) {
  std::vector<std::uint8_t> out;
  v = std::max(0.0, trunc0(v));
  while (v > 0) {
    out.insert(out.begin(), static_cast<std::uint8_t>(std::fmod(v, 256.0)));
    v = std::floor(v / 256);
  }
  return out;
}

std::vector<std::uint8_t> handle_bytes(const Value& v) {
  if (v.is_string()) {
    // parseInt(value, 16) || 0
    std::string_view t = base::js_trim_start(v.as_string());
    bool neg = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
      neg = t[0] == '-';
      t.remove_prefix(1);
    }
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
      t.remove_prefix(2);
    }
    double p = 0;
    for (const char ch : t) {
      int d = -1;
      if (ch >= '0' && ch <= '9') {
        d = ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        d = ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        d = ch - 'A' + 10;
      }
      if (d < 0) {
        break;
      }
      p = p * 16 + d;
    }
    return handle_bytes(neg ? -p : p);
  }
  return handle_bytes(v.truthy() ? base::js_to_number(v) : 0.0);
}

// ---------------------------------------------------------------- BitWriter

BitWriter::BitWriter(std::string_view v, std::size_t size)
    : version(v),
      r2004(at_least(v, "AC1018")),
      r2007(at_least(v, "AC1021")),
      r2010(at_least(v, "AC1024")),
      r2013(at_least(v, "AC1027")),
      r2018(at_least(v, "AC1032")),
      buf(size, 0) {}

void BitWriter::grow(std::uint64_t n) {
  const std::uint64_t need = ((bit + n) >> 3U) + 2;
  if (need > buf.size()) {
    buf.resize(std::max(static_cast<std::size_t>(need), buf.size() * 2), 0);
  }
}

void BitWriter::B(bool v) {
  grow(1);
  if (v) {
    buf[static_cast<std::size_t>(bit >> 3U)] =
        static_cast<std::uint8_t>(buf[static_cast<std::size_t>(bit >> 3U)] | (0x80U >> (bit & 7U)));
  }
  ++bit;
}

void BitWriter::bits(double v, int n) {
  const std::uint32_t u = base::js_to_uint32(v);
  for (int i = n - 1; i >= 0; --i) {
    B(((u >> static_cast<unsigned>(i)) & 1U) != 0);
  }
}

void BitWriter::B3(double v) {
  if (v == 0) {
    B(false);
  } else if (v == 2) {
    bits(2, 2);
  } else if (v == 6) {
    bits(6, 3);
  } else {
    bits(7, 3);
  }
}

void BitWriter::RC8(std::uint8_t v) {
  grow(8);
  const auto i = static_cast<std::size_t>(bit >> 3U);
  const unsigned o = bit & 7U;
  if (o == 0) {
    buf[i] = v;
  } else {
    buf[i] = static_cast<std::uint8_t>(buf[i] | (v >> o));
    buf[i + 1] = static_cast<std::uint8_t>(buf[i + 1] | ((static_cast<unsigned>(v) << (8U - o)) & 0xFFU));
  }
  bit += 8;
}

void BitWriter::RC(double v) {
  RC8(lo8(v));
}

void BitWriter::RS(double v) {
  const std::int32_t x = base::js_to_int32(v);
  RC8(static_cast<std::uint8_t>(x & 0xFF));
  RC8(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void BitWriter::RL(double v) {
  const std::uint32_t x = base::js_to_uint32(v);
  for (unsigned k = 0; k < 4; ++k) {
    RC8(static_cast<std::uint8_t>((x >> (8U * k)) & 0xFFU));
  }
}

void BitWriter::RD(double v) {
  const auto u = std::bit_cast<std::uint64_t>(v);
  for (unsigned k = 0; k < 8; ++k) {
    RC8(static_cast<std::uint8_t>((u >> (8U * k)) & 0xFFU));
  }
}

void BitWriter::BS(double v) {
  v = trunc0(v);
  if (v == 0) {
    BB(2);
  } else if (v == 256) {
    BB(3);
  } else if (v > 0 && v < 256) {
    BB(1);
    RC(v);
  } else {
    BB(0);
    RS(static_cast<double>(base::js_to_int32(v) & 0xFFFF));
  }
}

void BitWriter::BL(double v) {
  v = trunc0(v);
  if (v == 0) {
    BB(2);
  } else if (v > 0 && v < 256) {
    BB(1);
    RC(v);
  } else {
    BB(0);
    RL(static_cast<double>(base::js_to_uint32(v)));
  }
}

void BitWriter::BLL(double v) {
  // BigInt(v || 0): the values written are whole numbers
  const double t = v == v ? std::trunc(v) : 0;
  auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(t));
  std::vector<std::uint8_t> b;
  while (x > 0) {
    b.push_back(static_cast<std::uint8_t>(x & 0xFFU));
    x >>= 8U;
  }
  bits(static_cast<double>(b.size()), 3);
  for (const std::uint8_t c : b) {
    RC8(c);
  }
}

void BitWriter::BD(double v) {
  if (v == 0 && !std::signbit(v)) {
    BB(2);
  } else if (v == 1) {
    BB(1);
  } else {
    BB(0);
    RD(v);
  }
}

void BitWriter::DD(double v, double def) {
  if (v == def || (v != v && def != def)) {
    BB(0);
  } else {
    BB(3);
    RD(v);
  }
}

void BitWriter::BT(double v) {
  if (v == 0 || v != v) {
    B(true);
  } else {
    B(false);
    BD(v);
  }
}

void BitWriter::BE(double x, double y, double z) {
  if (x == 0 && y == 0 && z == 1) {
    B(true);
  } else {
    B(false);
    BD(x);
    BD(y);
    BD(z);
  }
}

void BitWriter::P3(const Value& p) {
  BD3(or0(p, "x"), or0(p, "y"), or0(p, "z"));
}
void BitWriter::P2(const Value& p) {
  BD2(or0(p, "x"), or0(p, "y"));
}

namespace {
double cmc_long(const NormColor& c) {
  if (c.rgb_set) {
    return static_cast<double>(base::js_to_int32(static_cast<double>(0xC2000000U | (c.rgb & 0xFFFFFFU))));
  }
  const double i = c.index.is_undefined() || c.index.is_null() ? 256 : base::js_to_number(c.index);
  if (i == 256) {
    return static_cast<double>(base::js_to_int32(static_cast<double>(0xC0000000U)));
  }
  if (i == 0) {
    return static_cast<double>(base::js_to_int32(static_cast<double>(0xC1000000U)));
  }
  if (i == 257) {
    return static_cast<double>(base::js_to_int32(static_cast<double>(0xC8000000U)));
  }
  const auto lo = static_cast<std::uint32_t>(base::js_to_int32(std::abs(i)) & 0xFF);
  return static_cast<double>(base::js_to_int32(static_cast<double>(0xC3000000U | lo)));
}
double index_or(const Value& i, double d) {
  return i.is_undefined() || i.is_null() ? d : base::js_to_number(i);
}
}  // namespace

void BitWriter::CMC(const Value& color) {
  const NormColor c = norm_color(color);
  if (!r2004) {
    BS(index_or(c.index, 256));
    return;
  }
  BS(0);
  BL(cmc_long(c));
  const bool name = c.name.truthy();
  const bool book = c.book.truthy();
  RC(static_cast<double>((name ? 1 : 0) | (book ? 2 : 0)));
  if (name) {
    TV(c.name);
  }
  if (book) {
    TV(c.book);
  }
}

void BitWriter::ENC(const Value& color, const Value& transparency) {
  const NormColor c = norm_color(color);
  if (!r2004) {
    BS(index_or(c.index, 256));
    return;
  }
  const bool has_t = !transparency.is_undefined() && !transparency.is_null();
  if (c.rgb_set) {
    BS(static_cast<double>(0x8000 | (has_t ? 0x2000 : 0) | (base::js_to_int32(index_or(c.index, 0)) & 0xFF)));
    BL(static_cast<double>(base::js_to_int32(static_cast<double>(0xC2000000U | (c.rgb & 0xFFFFFFU)))));
  } else {
    BS(static_cast<double>((has_t ? 0x2000 : 0) | (base::js_to_int32(index_or(c.index, 256)) & 0x1FF)));
  }
  if (has_t) {
    BL(static_cast<double>(to_i32(transparency)));
  }
}

void BitWriter::OT(double t) {
  if (!r2010) {
    BS(t);
    return;
  }
  if (t < 256) {
    BB(0);
    RC(t);
  } else if (t >= 0x1F0 && t < 0x1F0 + 256) {
    BB(1);
    RC(t - 0x1F0);
  } else {
    BB(2);
    RS(t);
  }
}

void BitWriter::H(double code, const Value& value) {
  const auto b = handle_bytes(value);
  RC(static_cast<double>(((base::js_to_int32(code) & 15) << 4) | static_cast<int>(b.size())));
  for (const std::uint8_t c : b) {
    RC8(c);
  }
}
void BitWriter::H(double code, double value) {
  const auto b = handle_bytes(value);
  RC(static_cast<double>(((base::js_to_int32(code) & 15) << 4) | static_cast<int>(b.size())));
  for (const std::uint8_t c : b) {
    RC8(c);
  }
}

void BitWriter::T8_bytes(const ByteBuf& b) {
  if (b.empty()) {
    BS(0);
    return;
  }
  BS(static_cast<double>(b.size() + 1));
  for (const std::uint8_t c : b) {
    RC8(c);
  }
  RC8(std::uint8_t{0});
}

void BitWriter::T8(const Value& s) {
  if (s.is_undefined() || s.is_null()) {
    T8_bytes({});
    return;
  }
  T8_bytes(encode1252(s.is_string() ? s.as_string() : base::js_string(s)));
}

void BitWriter::TU(const Value& s) {
  const auto u = base::to_utf16(clean_text_of(s));
  BS(static_cast<double>(u.size()));
  for (const std::uint16_t c : u) {
    RS(static_cast<double>(c));
  }
}

void BitWriter::TU16(const std::vector<std::uint16_t>& u) {
  std::vector<std::uint16_t> c;
  c.reserve(u.size());
  for (const std::uint16_t x : u) {
    if (x == 0) {
      continue;
    }
    c.push_back((x < 0x20 && x != 0x09) || x == 0x7F ? std::uint16_t{0x20} : x);
  }
  BS(static_cast<double>(c.size()));
  for (const std::uint16_t x : c) {
    RS(static_cast<double>(x));
  }
}

void BitWriter::TV16(const std::vector<std::uint16_t>& u) {
  if (str != nullptr) {
    str->TU16(u);
    str_used = true;
  } else {
    TU16(u);
  }
}

void BitWriter::TV(const Value& s) {
  if (r2007 && str != nullptr) {
    str->TU(s);
    str_used = true;
  } else if (r2007) {
    TU(s);
  } else {
    T8(s);
  }
}

void BitWriter::append(const BitWriter& w) {
  append(w, w.bit);
}

void BitWriter::append(const BitWriter& w, std::uint64_t n) {
  std::uint64_t i = 0;
  for (; i + 8 <= n; i += 8) {
    RC8(w.buf[static_cast<std::size_t>(i >> 3U)]);
  }
  for (; i < n; ++i) {
    B(((w.buf[static_cast<std::size_t>(i >> 3U)] >> (7U - (i & 7U))) & 1U) != 0);
  }
}

void BitWriter::patch_RL(std::uint64_t at, double v) {
  const std::uint32_t x = base::js_to_uint32(v);
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned b = (x >> (8U * i)) & 0xFFU;
    for (int k = 7; k >= 0; --k) {
      const std::uint64_t pos = at++;
      const auto mask = static_cast<std::uint8_t>(0x80U >> (pos & 7U));
      if (((b >> static_cast<unsigned>(k)) & 1U) != 0) {
        buf[static_cast<std::size_t>(pos >> 3U)] =
            static_cast<std::uint8_t>(buf[static_cast<std::size_t>(pos >> 3U)] | mask);
      } else {
        buf[static_cast<std::size_t>(pos >> 3U)] =
            static_cast<std::uint8_t>(buf[static_cast<std::size_t>(pos >> 3U)] & ~mask);
      }
    }
  }
}

ByteBuf BitWriter::bytes() const {
  const auto n = static_cast<std::size_t>((bit + 7) >> 3U);
  return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(std::min(n, buf.size()))};
}

// ---------------------------------------------------------------- ByteWriter

void ByteWriter::RS(double v) {
  const std::int32_t x = base::js_to_int32(v);
  buf.push_back(static_cast<std::uint8_t>(x & 0xFF));
  buf.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void ByteWriter::RL(double v) {
  const std::uint32_t x = base::js_to_uint32(v);
  for (unsigned k = 0; k < 4; ++k) {
    buf.push_back(static_cast<std::uint8_t>((x >> (8U * k)) & 0xFFU));
  }
}

void ByteWriter::RLL(std::uint64_t v) {
  for (unsigned k = 0; k < 8; ++k) {
    buf.push_back(static_cast<std::uint8_t>((v >> (8U * k)) & 0xFFU));
  }
}

void ByteWriter::RLL(double v) {
  RLL(static_cast<std::uint64_t>(static_cast<std::int64_t>(v == v ? std::trunc(v) : 0)));
}

void ByteWriter::RD(double v) {
  RLL(std::bit_cast<std::uint64_t>(v));
}

void ByteWriter::set_RL(std::size_t at, double v) {
  const std::uint32_t x = base::js_to_uint32(v);
  for (unsigned k = 0; k < 4; ++k) {
    buf.at(at + k) = static_cast<std::uint8_t>((x >> (8U * k)) & 0xFFU);
  }
}

ByteBuf unb64(std::string_view s) {
  ByteBuf out;
  out.reserve(s.size() * 3 / 4);
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char ch : s) {
    int v = -1;
    if (ch >= 'A' && ch <= 'Z') {
      v = ch - 'A';
    } else if (ch >= 'a' && ch <= 'z') {
      v = ch - 'a' + 26;
    } else if (ch >= '0' && ch <= '9') {
      v = ch - '0' + 52;
    } else if (ch == '+' || ch == '-') {
      v = 62;
    } else if (ch == '/' || ch == '_') {
      v = 63;
    } else if (ch == '=') {
      break;
    }
    if (v < 0) {
      continue;
    }
    acc = (acc << 6U) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  return out;
}

ByteBuf slice_bits(std::span<const std::uint8_t> buf, std::uint64_t from, std::uint64_t to) {
  const std::uint64_t n = to > from ? to - from : 0;
  ByteBuf out(static_cast<std::size_t>((n + 7) >> 3U), 0);
  const unsigned sh = from & 7U;
  const auto b0 = static_cast<std::size_t>(from >> 3U);
  const auto at = [&](std::size_t i) -> unsigned { return i < buf.size() ? buf[i] : 0U; };
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = sh == 0 ? static_cast<std::uint8_t>(at(b0 + i))
                     : static_cast<std::uint8_t>(((at(b0 + i) << sh) | (at(b0 + i + 1) >> (8U - sh))) & 0xFFU);
  }
  if ((n & 7U) != 0 && !out.empty()) {
    out.back() = static_cast<std::uint8_t>(out.back() & ((0xFFU << (8U - (n & 7U))) & 0xFFU));
  }
  return out;
}

void append_bits(BitWriter& w, std::span<const std::uint8_t> u8, std::uint64_t n) {
  const auto at = [&](std::uint64_t i) -> std::uint8_t {
    const auto k = static_cast<std::size_t>(i >> 3U);
    return k < u8.size() ? u8[k] : std::uint8_t{0};
  };
  std::uint64_t i = 0;
  for (; i + 8 <= n; i += 8) {
    w.RC8(at(i));
  }
  for (; i < n; ++i) {
    w.B(((at(i) >> (7U - (i & 7U))) & 1U) != 0);
  }
}

std::vector<std::uint8_t> mc_bytes(double v) {
  const bool neg = v < 0;
  double a = std::abs(v);
  std::vector<std::uint8_t> out;
  for (;;) {
    const int b = base::js_to_int32(a) & 0x7F;
    a = std::floor(a / 128);
    if (a == 0 && (b & 0x40) == 0) {
      out.push_back(static_cast<std::uint8_t>(b | (neg ? 0x40 : 0)));
      break;
    }
    out.push_back(static_cast<std::uint8_t>(b | 0x80));
    if (a == 0) {
      out.push_back(neg ? 0x40 : 0);
      break;
    }
  }
  return out;
}

std::vector<std::uint8_t> umc_bytes(double v) {
  std::vector<std::uint8_t> out;
  do {
    int b = base::js_to_int32(v) & 0x7F;
    v = std::floor(v / 128);
    if (v != 0) {
      b |= 0x80;
    }
    out.push_back(static_cast<std::uint8_t>(b));
  } while (v != 0);
  return out;
}

void ByteWriter::MC(double v) {
  raw(mc_bytes(v));
}
void ByteWriter::UMC(double v) {
  raw(umc_bytes(v));
}

void ByteWriter::MS(double v) {
  for (;;) {
    const int s = base::js_to_int32(v) & 0x7FFF;
    v = std::floor(v / 32768);
    if (v == 0) {
      RS(s);
      break;
    }
    RS(s | 0x8000);
  }
}

}  // namespace cc::dwg::w
