// SPDX-License-Identifier: Apache-2.0
#include "bits.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

#include "messages.hpp"

namespace cc::dwg {

std::string error_text(const std::exception& e) {
  if (const auto* o = dynamic_cast<const base::ReadOverrun*>(&e)) {
    return msg("ccdwg.err.read_past_end",
               {{"at", static_cast<double>(o->at())}, {"end", static_cast<double>(o->end())}});
  }
  return e.what();
}

// ---------------------------------------------------------------- code pages
namespace {

// Spec gap (bits-read.js): DWGCODEPAGE numbers found by experiment with the ODA File Converter.
constexpr auto kCodepages = std::to_array<std::string_view>({
    "UNDEFINED", "ASCII",     "ISO8859-1", "ISO8859-2", "ISO8859-3", "ISO8859-4", "ISO8859-5", "ISO8859-6",
    "ISO8859-7", "ISO8859-8", "ISO8859-9", "DOS437",    "DOS850",    "DOS852",    "DOS855",    "DOS857",
    "DOS860",    "DOS861",    "DOS863",    "DOS864",    "DOS865",    "DOS869",    "DOS932",    "MAC-ROMAN",
    "BIG5",      "KSC5601",   "JOHAB",     "DOS866",    "ANSI_1250", "ANSI_1251", "ANSI_1252", "GB2312",
    "ANSI_1253", "ANSI_1254", "ANSI_1255", "ANSI_1256", "ANSI_1257", "ANSI_874",  "ANSI_932",  "ANSI_936",
    "ANSI_949",  "ANSI_950",  "ANSI_1361", "ANSI_1200", "ANSI_1258", "CNT",
});
// Names the JS decodes with the platform's TextDecoder (bits-read.js LABELS).
constexpr auto kLabelled = std::to_array<std::string_view>({
    "ANSI_1250", "ANSI_1251", "ANSI_1253", "ANSI_1254", "ANSI_1255", "ANSI_1256", "ANSI_1257", "ANSI_1258", "ANSI_874",
    "ANSI_932",  "DOS932",    "ANSI_936",  "GB2312",    "ANSI_949",  "KSC5601",   "ANSI_950",  "BIG5",      "DOS866",
    "ISO8859-2", "ISO8859-3", "ISO8859-4", "ISO8859-5", "ISO8859-6", "ISO8859-7", "ISO8859-8", "ISO8859-9", "MAC-ROMAN",
});

constexpr auto kCp1252High = std::to_array<std::uint16_t>({
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178,
});

}  // namespace

Codepage codepage_for(int num) {
  Codepage cp;
  const bool in_table = num >= 0 && static_cast<std::size_t>(num) < kCodepages.size();
  const std::string_view name = in_table ? kCodepages[static_cast<std::size_t>(num)] : std::string_view{};
  if (!in_table || name == "ANSI_1252" || name == "UNDEFINED" || name == "ASCII" || name == "ISO8859-1") {
    cp.name = in_table ? std::string(name) : "ANSI_1252";
    cp.known = in_table;
    return cp;
  }
  cp.name = std::string(name);
  // Gap (docs/gaps.md): the JS decodes these with TextDecoder; this port decodes every code page as
  // Windows-1252 for now (no corpus file uses another). "known" follows the JS so the notes stay the same.
  for (const std::string_view l : kLabelled) {
    if (l == name) {
      cp.known = true;
    }
  }
  return cp;
}

U16 decode_bytes(const Codepage& /*cp*/, Bytes b) {
  U16 out;
  out.reserve(b.size());
  for (const std::uint8_t c : b) {
    out.push_back(c >= 0x80 && c < 0xA0 ? kCp1252High[c - 0x80U] : c);
  }
  return out;
}

void unescape_u(U16& s) {
  // bits-read.js: a quick test for "\U+" or "\u+", then the regex /\\U\+([0-9A-Fa-f]{4})/ (upper-case U only).
  bool any = false;
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    if (s[i] == '\\' && (s[i + 1] == 'U' || s[i + 1] == 'u') && s[i + 2] == '+') {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  auto hexv = [](std::uint16_t c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  U16 out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 7 <= s.size() && s[i + 1] == 'U' && s[i + 2] == '+') {
      int v = 0;
      bool ok = true;
      for (std::size_t k = 3; k < 7; ++k) {
        const int h = hexv(s[i + k]);
        if (h < 0) {
          ok = false;
          break;
        }
        v = v * 16 + h;
      }
      if (ok) {
        out.push_back(static_cast<std::uint16_t>(v));
        i += 6;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  s = std::move(out);
}

std::string utf8(const U16& s) {
  std::string out;
  out.reserve(s.size());
  base::append_utf16(out, s);
  return out;
}

std::string decode_text(const Codepage& cp, Bytes b) {
  U16 u = decode_bytes(cp, b);
  unescape_u(u);
  return utf8(u);
}

Value colour_from_rgb_word(int index, std::uint32_t word) {
  const std::uint32_t method = word >> 24U;
  Value c = Value::object();
  if (method == 0xC2) {
    c.set("rgb", word & 0xFFFFFFU);
  } else if (method == 0xC3) {
    c.set("index", word & 0xFFU);
  } else if (method == 0xC0) {
    c.set("index", 256);
  } else if (method == 0xC1) {
    c.set("index", 0);
  } else if (method == 0xC8) {
    c.set("index", 257);
  } else {
    c.set("index", index & 0x1FF);
  }
  return c;
}

// ---------------------------------------------------------------- the reader
std::uint32_t Bits::B3() {
  std::uint32_t v = 0;
  for (int i = 0; i < 3; ++i) {
    const std::uint32_t b = B();
    v = (v << 1U) | b;
    if (b == 0) {
      break;
    }
  }
  return v;
}

std::int32_t Bits::RS() {
  const std::uint32_t a = RC();
  const std::uint32_t b = RC();
  return static_cast<std::int16_t>(static_cast<std::uint16_t>((b << 8U) | a));
}

std::uint32_t Bits::RSu() {
  const std::uint32_t a = RC();
  return (RC() << 8U) | a;
}

std::uint32_t Bits::RL() {
  const std::uint32_t a = RC();
  const std::uint32_t b = RC();
  const std::uint32_t c = RC();
  const std::uint32_t d = RC();
  return a | (b << 8U) | (c << 16U) | (d << 24U);
}

double Bits::RLL() {
  const double lo = RL();
  const double hi = RL();
  return hi * 4294967296.0 + lo;
}

double Bits::RD() {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(RC()) << (8U * i);
  }
  return std::bit_cast<double>(v);
}

std::vector<std::uint8_t> Bits::bytes(std::size_t n) {
  std::vector<std::uint8_t> out;
  r_.raw(n, out);
  return out;
}

std::int32_t Bits::BS() {
  switch (BB()) {
    case 0: return RS();
    case 1: return static_cast<std::int32_t>(RC());
    case 2: return 0;
    default: return 256;
  }
}

std::int32_t Bits::BL() {
  switch (BB()) {
    case 0: return RLs();
    case 1: return static_cast<std::int32_t>(RC());
    case 2: return 0;
    default: throw DecodeError(msg("ccdwg.err.bitlong"));
  }
}

double Bits::BLL() {
  const std::uint32_t n = bits(3);
  double v = 0;
  double m = 1;
  for (std::uint32_t i = 0; i < n; ++i) {
    v += RC() * m;
    m *= 256;
  }
  return v;
}

double Bits::BD() {
  switch (BB()) {
    case 0: return RD();
    case 1: return 1.0;
    case 2: return 0.0;
    default: throw DecodeError(msg("ccdwg.err.bitdouble"));
  }
}

double Bits::DD(double def) {
  switch (BB()) {
    case 0: return def;
    case 1: {
      auto v = std::bit_cast<std::uint64_t>(def);
      v &= ~std::uint64_t{0xFFFFFFFF};
      for (unsigned i = 0; i < 4; ++i) {
        v |= static_cast<std::uint64_t>(RC()) << (8U * i);
      }
      return std::bit_cast<double>(v);
    }
    case 2: {
      auto v = std::bit_cast<std::uint64_t>(def);
      const std::uint64_t b4 = RC();
      const std::uint64_t b5 = RC();
      v &= ~std::uint64_t{0xFFFFFFFFFFFF};
      v |= (b4 << 32U) | (b5 << 40U);
      for (unsigned i = 0; i < 4; ++i) {
        v |= static_cast<std::uint64_t>(RC()) << (8U * i);
      }
      return std::bit_cast<double>(v);
    }
    default: return RD();
  }
}

Value Bits::RD2() {
  const double x = RD();
  return base::pt2(x, RD());
}
Value Bits::RD3() {
  const double x = RD();
  const double y = RD();
  return base::pt3(x, y, RD());
}
Value Bits::BD2() {
  const double x = BD();
  return base::pt2(x, BD());
}
Value Bits::BD3() {
  const double x = BD();
  const double y = BD();
  return base::pt3(x, y, BD());
}
Value Bits::DD3(const Value& d) {
  const double x = DD(d["x"].num_or(0));
  const double y = DD(d["y"].num_or(0));
  return base::pt3(x, y, DD(d["z"].num_or(0)));
}

Value Bits::BE() {
  if (version_ >= 15 && B() != 0) {
    return base::pt3(0, 0, 1);
  }
  return BD3();
}

double Bits::BT() {
  if (version_ >= 15 && B() != 0) {
    return 0;
  }
  return BD();
}

std::int32_t Bits::OT() {
  if (version_ < 24) {
    return BS();
  }
  switch (BB()) {
    case 0: return static_cast<std::int32_t>(RC());
    case 1: return static_cast<std::int32_t>(RC() + 0x1F0U);
    default: return static_cast<std::int32_t>(RSu());
  }
}

std::int64_t Bits::H(std::int64_t ref) {
  const std::uint32_t b = RC();
  const int code = static_cast<int>(b >> 4U);
  const std::uint32_t n = b & 15U;
  double v = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    v = v * 256 + RC();
  }
  hcode = code;
  // Handles above 2^53 do not occur in practice; the JS keeps them as doubles, this keeps them in range.
  constexpr double kMax = 9007199254740992.0;
  const auto iv = static_cast<std::int64_t>(v < kMax ? v : kMax);
  switch (code) {
    case 6: return ref + 1;
    case 8: return ref - 1;
    case 0xA: return ref + iv;
    case 0xC: return ref - iv;
    default: return iv;
  }
}

std::string Bits::T() {
  const std::int32_t n = BS();
  if (n <= 0) {
    return {};
  }
  std::vector<std::uint8_t> b = bytes(static_cast<std::size_t>(n));
  std::size_t e = b.size();
  while (e > 0 && b[e - 1] == 0) {
    --e;
  }
  return decode_text(*cp_, Bytes(b).first(e));
}

U16 Bits::TU16() {
  const auto n = static_cast<std::uint32_t>(BS()) & 0xFFFFU;
  if (n == 0) {
    return {};
  }
  if (static_cast<std::int64_t>(n) * 16 > left()) {
    r_.overrun(static_cast<std::uint64_t>(n) * 16U);
  }
  U16 codes(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t lo = RC();
    codes[i] = static_cast<std::uint16_t>(lo | (RC() << 8U));
  }
  while (!codes.empty() && codes.back() == 0) {
    codes.pop_back();
  }
  return codes;
}

std::string Bits::TU() {
  U16 s = TU16();
  // R2007–R2013 "\U+XXXX" escapes are decoded; R2018 text is kept as stored (found by experiment, bits-read.js).
  if (version_ < 32) {
    unescape_u(s);
  }
  return utf8(s);
}

std::string Bits::TV() {
  if (version_ >= 21) {
    return str != nullptr ? str->TU() : TU();
  }
  return T();
}

Value Bits::CMC() {
  if (version_ < 18) {
    return base::make_object({{"index", BS()}});
  }
  const std::int32_t index = BS();
  const auto rgb = static_cast<std::uint32_t>(BL());
  const std::uint32_t flags = RC();
  Value c = colour_from_rgb_word(index, rgb);
  if ((flags & 1U) != 0) {
    c.set("name", TV());
  }
  if ((flags & 2U) != 0) {
    c.set("book", TV());
  }
  return c;
}

Enc Bits::ENC() {
  Enc e;
  if (version_ < 18) {
    e.color = base::make_object({{"index", BS()}});
    return e;
  }
  const std::uint32_t v = static_cast<std::uint32_t>(BS()) & 0xFFFFU;
  const std::uint32_t flags = v & 0xFF00U;
  const auto index = static_cast<int>(v & 0x1FFU);
  if ((flags & 0x8000U) != 0) {
    const auto rgb = static_cast<std::uint32_t>(BL());
    e.color = colour_from_rgb_word(index, rgb);
    e.has_book = (flags & 0x4000U) != 0;
  } else {
    e.color = base::make_object({{"index", v & 0xFFFU}});
  }
  if ((flags & 0x2000U) != 0) {
    e.has_transparency = true;
    e.transparency = static_cast<std::uint32_t>(BL());
  }
  return e;
}

// ---------------------------------------------------------------- modular numbers
namespace {
std::uint8_t at(Bytes b, std::size_t& pos) {
  if (pos >= b.size()) {
    throw DecodeError(msg("ccdwg.err.offset_outside"));
  }
  return b[pos++];
}
}  // namespace

double read_mc(Bytes b, std::size_t& pos) {
  double v = 0;
  double scale = 1;  // 2 ** shift
  for (;;) {
    const std::uint8_t byte = at(b, pos);
    if ((byte & 0x80U) != 0) {
      v += (byte & 0x7FU) * scale;
      scale *= 128;
      continue;
    }
    v += (byte & 0x3FU) * scale;
    return (byte & 0x40U) != 0 ? -v : v;
  }
}

double read_mcu(Bytes b, std::size_t& pos) {
  double v = 0;
  double scale = 1;
  std::uint8_t byte = 0;
  do {
    byte = at(b, pos);
    v += (byte & 0x7FU) * scale;
    scale *= 128;
  } while ((byte & 0x80U) != 0);
  return v;
}

double read_ms(Bytes b, std::size_t& pos) {
  double v = 0;
  double scale = 1;
  std::uint32_t w = 0;
  do {
    const std::uint32_t lo = at(b, pos);
    w = lo | (static_cast<std::uint32_t>(at(b, pos)) << 8U);
    v += (w & 0x7FFFU) * scale;
    scale *= 32768;
  } while ((w & 0x8000U) != 0);
  return v;
}

}  // namespace cc::dwg
