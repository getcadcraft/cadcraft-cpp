// SPDX-License-Identifier: Apache-2.0
// live-client.js Part 1 (MessagePack), ported. See msgpack.hpp.
#include "cc/live/msgpack.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "cc/base/value.hpp"

namespace cc::live {

namespace {

constexpr double kMaxSafe = 9007199254740991.0;  // Number.MAX_SAFE_INTEGER
constexpr std::uint64_t kMaxSafeU = 9007199254740991U;
constexpr std::size_t kMaxDepth = 512;

base::Result<Mp> fail(const char* what) {
  return base::fail(base::Errc::invalid, std::string("msgpack: ") + what);
}

std::uint64_t index_of(std::string_view key) noexcept {
  std::uint64_t n = 0;
  for (const char c : key) {
    n = n * 10U + static_cast<std::uint64_t>(c - '0');
  }
  return n;
}

}  // namespace

bool is_array_index(std::string_view key) noexcept {
  return base::js_index_key(key);
}

// ---------------------------------------------------------------------------------------------------- Map
Mp& Map::set(std::string_view key, Mp v) {
  if (Mp* had = find(key)) {
    *had = std::move(v);
    return *had;
  }
  auto at = items_.end();
  if (is_array_index(key)) {
    const std::uint64_t n = index_of(key);
    at =
        std::ranges::find_if(items_, [n](const Entry& e) { return !is_array_index(e.first) || index_of(e.first) > n; });
  }
  return items_.insert(at, Entry{std::string(key), std::move(v)})->second;
}

const Mp* Map::find(std::string_view key) const {
  for (const Entry& e : items_) {
    if (e.first == key) {
      return &e.second;
    }
  }
  return nullptr;
}

Mp* Map::find(std::string_view key) {
  for (Entry& e : items_) {
    if (e.first == key) {
      return &e.second;
    }
  }
  return nullptr;
}

void Map::erase(std::string_view key) {
  std::erase_if(items_, [key](const Entry& e) { return e.first == key; });
}

bool operator==(const Map& a, const Map& b) {
  return a.items_ == b.items_;
}

// ---------------------------------------------------------------------------------------------------- Mp
Mp Mp::from_signed(std::int64_t i) noexcept {
  if (i >= -static_cast<std::int64_t>(kMaxSafeU) && std::cmp_less_equal(i, kMaxSafeU)) {
    return {static_cast<double>(i)};
  }
  // |INT64_MIN| = 2^63 fits the magnitude
  const std::uint64_t mag = i < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(i) : static_cast<std::uint64_t>(i);
  return {Big{i < 0, mag}};
}

Mp Mp::from_unsigned(std::uint64_t u) noexcept {
  if (u <= kMaxSafeU) {
    return {static_cast<double>(u)};
  }
  return {Big{false, u}};
}

const Mp* Mp::get(std::string_view key) const {
  return is_map() ? as_map().find(key) : nullptr;
}

bool Mp::to_uint(std::uint64_t& out) const noexcept {
  if (const auto* d = std::get_if<double>(&v_)) {
    if (!(*d >= 0) || *d > kMaxSafe || std::floor(*d) != *d) {
      return false;
    }
    out = static_cast<std::uint64_t>(*d);
    return true;
  }
  if (const auto* b = std::get_if<Big>(&v_)) {
    if (b->neg) {
      return false;
    }
    out = b->mag;
    return true;
  }
  return false;
}

std::string_view Mp::str_or(std::string_view def) const noexcept {
  if (const auto* s = std::get_if<std::string>(&v_)) {
    return *s;
  }
  return def;
}

// ---------------------------------------------------------------------------------------------------- writer
namespace {

class Writer {
 public:
  explicit Writer(MpBytes out) : out_(std::move(out)) {}
  [[nodiscard]] MpBytes take() && { return std::move(out_); }

  // NOLINTNEXTLINE(misc-no-recursion) — nesting is capped at kMaxDepth (512)
  void any(const Mp& v) {
    switch (v.kind()) {
      case Mp::Kind::nil: {
        u8(0xc0);
        return;
      }
      case Mp::Kind::boolean: {
        u8(v.as_bool() ? 0xc3 : 0xc2);
        return;
      }
      case Mp::Kind::number: {
        const double d = v.as_number();
        // Number.isInteger(v) && Number.isSafeInteger(v)
        if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) <= kMaxSafe) {
          {
            integer(d);
          }
          return;
        }
        float64(d);
        return;
      }
      case Mp::Kind::big: {
        big(v.as_big());
        return;
      }
      case Mp::Kind::string: {
        str(v.as_string());
        return;
      }
      case Mp::Kind::bytes: {
        bin(v.as_bytes());
        return;
      }
      case Mp::Kind::array: {
        head(v.as_array().size(), 0x90, 0xdc, 0xdd);
        for (const Mp& x : v.as_array()) {
          any(x);
        }
        return;
      }
      case Mp::Kind::map: {
        head(v.as_map().size(), 0x80, 0xde, 0xdf);
        for (const auto& [k, x] : v.as_map().entries()) {
          str(k);
          any(x);
        }
        return;
      }
    }
  }

 private:
  void u8(unsigned x) { out_.push_back(static_cast<std::uint8_t>(x & 0xffU)); }
  void be(std::uint64_t x, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
      u8(static_cast<unsigned>((x >> (8 * i)) & 0xffU));
    }
  }
  void head(std::size_t n, unsigned fix, unsigned b16, unsigned b32) {
    if (n < 16) {
      u8(fix | static_cast<unsigned>(n));
    } else if (n < 0x10000) {
      u8(b16);
      be(n, 2);
    } else {
      u8(b32);
      be(n, 4);
    }
  }
  void integer(double d) {
    if (d >= 0) {
      const auto v = static_cast<std::uint64_t>(d);
      if (v < 128) {
        {
          u8(static_cast<unsigned>(v));
        }
        return;
      }
      if (v < 0x100) {
        u8(0xcc);
        u8(static_cast<unsigned>(v));
        return;
      }
      if (v < 0x10000) {
        u8(0xcd);
        be(v, 2);
        return;
      }
      if (v < 0x100000000ULL) {
        u8(0xce);
        be(v, 4);
        return;
      }
      u8(0xcf);
      be(v, 8);
      return;
    }
    const auto v = static_cast<std::int64_t>(d);
    const auto bits = static_cast<std::uint64_t>(v);
    if (v >= -32) {
      {
        u8(static_cast<unsigned>(bits & 0xffU));
      }
      return;
    }
    if (v >= -128) {
      u8(0xd0);
      be(bits, 1);
      return;
    }
    if (v >= -32768) {
      u8(0xd1);
      be(bits, 2);
      return;
    }
    if (v >= -2147483648LL) {
      u8(0xd2);
      be(bits, 4);
      return;
    }
    u8(0xd3);
    be(bits, 8);
  }
  void big(const Big& b) {
    if (!b.neg) {
      if (b.mag < 0x100000000ULL) {
        {
          integer(static_cast<double>(b.mag));
        }
        return;
      }
      u8(0xcf);
      be(b.mag, 8);
      return;
    }
    if (b.mag <= 2147483648ULL) {
      {
        integer(-static_cast<double>(b.mag));
      }
      return;
    }
    // magnitudes past 2^63 cannot be written (JS throws "integer too small"); a Big made here never has one
    const std::uint64_t bits = std::uint64_t{0} - std::min<std::uint64_t>(b.mag, std::uint64_t{1} << 63U);
    u8(0xd3);
    be(bits, 8);
  }
  void float64(double d) {
    u8(0xcb);
    be(std::bit_cast<std::uint64_t>(d), 8);
  }
  void str(std::string_view s) {
    // TextEncoder: a lone surrogate (WTF-8 ED A0..BF xx) becomes U+FFFD
    const auto surrogate_at = [s](std::size_t i) {
      return i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xed &&
             (static_cast<unsigned char>(s[i + 1]) & 0xe0U) == 0xa0U;
    };
    std::string fixed;
    std::string_view text = s;
    if (s.contains('\xed')) {
      bool any = false;
      for (std::size_t j = 0; j < s.size(); ++j) {
        if (surrogate_at(j)) {
          fixed += "\xef\xbf\xbd";
          j += 2;
          any = true;
        } else {
          fixed += s[j];
        }
      }
      if (any) {
        text = fixed;
      }
    }
    const std::size_t n = text.size();
    if (n < 32) {
      u8(0xa0U | static_cast<unsigned>(n));
    } else if (n < 0x100) {
      u8(0xd9);
      u8(static_cast<unsigned>(n));
    } else if (n < 0x10000) {
      u8(0xda);
      be(n, 2);
    } else {
      u8(0xdb);
      be(n, 4);
    }
    for (const char c : text) {
      out_.push_back(static_cast<std::uint8_t>(c));
    }
  }
  void bin(const MpBytes& b) {
    const std::size_t n = b.size();
    if (n < 0x100) {
      u8(0xc4);
      u8(static_cast<unsigned>(n));
    } else if (n < 0x10000) {
      u8(0xc5);
      be(n, 2);
    } else {
      u8(0xc6);
      be(n, 4);
    }
    out_.insert(out_.end(), b.begin(), b.end());
  }

  MpBytes out_;
};

// ---------------------------------------------------------------------------------------------------- reader
/// TextDecoder('utf-8') (not fatal): a leading BOM dropped, every ill-formed sequence → U+FFFD (maximal subparts).
std::string decode_utf8(std::span<const std::uint8_t> b) {
  std::string out;
  out.reserve(b.size());
  std::size_t i = 0;
  if (b.size() >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
    i = 3;
  }
  const auto bad = [&out] { out += "\xef\xbf\xbd"; };
  while (i < b.size()) {
    const std::uint8_t c = b[i];
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    std::size_t need = 0;
    std::uint8_t lo = 0x80;
    std::uint8_t hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) {
      need = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
      need = 2;
      if (c == 0xe0) {
        lo = 0xa0;
      }
      if (c == 0xed) {
        hi = 0x9f;
      }
    } else if (c >= 0xf0 && c <= 0xf4) {
      need = 3;
      if (c == 0xf0) {
        lo = 0x90;
      }
      if (c == 0xf4) {
        hi = 0x8f;
      }
    } else {
      bad();
      ++i;
      continue;
    }
    std::size_t j = 1;
    for (; j <= need; ++j) {
      if (i + j >= b.size()) {
        break;
      }
      const std::uint8_t x = b[i + j];
      const std::uint8_t l = j == 1 ? lo : std::uint8_t{0x80};
      const std::uint8_t h = j == 1 ? hi : std::uint8_t{0xbf};
      if (x < l || x > h) {
        break;
      }
    }
    if (j <= need) {
      bad();
      i += j;
      continue;
    }
    for (std::size_t k = 0; k <= need; ++k) {
      out += static_cast<char>(b[i + k]);
    }
    i += need + 1;
  }
  return out;
}

/// String(k) of a decoded non-string key.
// NOLINTNEXTLINE(misc-no-recursion) — nesting is capped at kMaxDepth (512)
std::string key_string(const Mp& k) {
  switch (k.kind()) {
    case Mp::Kind::nil: return "null";
    case Mp::Kind::boolean: return k.as_bool() ? "true" : "false";
    case Mp::Kind::number: {
      std::string s;
      base::append_number(s, k.as_number());
      return s;
    }
    case Mp::Kind::big: return (k.as_big().neg ? "-" : "") + std::to_string(k.as_big().mag);
    case Mp::Kind::string: return k.as_string();
    case Mp::Kind::bytes: {  // Uint8Array#toString: the bytes joined with ","
      std::string s;
      for (std::size_t i = 0; i < k.as_bytes().size(); ++i) {
        if (i) {
          s += ',';
        }
        s += std::to_string(k.as_bytes()[i]);
      }
      return s;
    }
    case Mp::Kind::array: {
      std::string s;
      for (std::size_t i = 0; i < k.as_array().size(); ++i) {
        if (i) {
          s += ',';
        }
        if (!k.as_array()[i].is_nil()) {
          s += key_string(k.as_array()[i]);
        }
      }
      return s;
    }
    case Mp::Kind::map: return "[object Object]";
  }
  return {};
}

/// The byte width of a length or number whose type bytes run first, first + 1, …: 1, 2, 4 (8).
constexpr int width(std::uint8_t t, std::uint8_t first) {
  return 1 << (t - first);
}

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}
  [[nodiscard]] std::size_t pos() const noexcept { return p_; }

  // NOLINTNEXTLINE(misc-no-recursion) — nesting is capped at kMaxDepth (512)
  base::Result<Mp> any(std::size_t depth) {
    if (depth > kMaxDepth) {
      return fail("too deep");
    }
    if (p_ >= b_.size()) {
      return fail("truncated");
    }
    const std::uint8_t t = b_[p_++];
    if (t < 0x80) {
      return Mp(static_cast<double>(t));
    }
    if (t >= 0xe0) {
      return Mp(static_cast<double>(static_cast<int>(t) - 0x100));
    }
    if ((t & 0xf0U) == 0x80U) {
      return map(t & 0x0fU, depth);
    }
    if ((t & 0xf0U) == 0x90U) {
      return arr(t & 0x0fU, depth);
    }
    if ((t & 0xe0U) == 0xa0U) {
      return text(t & 0x1fU);
    }
    std::uint64_t n = 0;
    switch (t) {
      case 0xc0: return Mp(nullptr);
      case 0xc2: return Mp(false);
      case 0xc3: return Mp(true);
      case 0xc4:
      case 0xc5:
      case 0xc6:
        if (!be(width(t, 0xc4), n)) {
          return fail("truncated");
        }
        return bytes(n);
      case 0xc7:
      case 0xc8:
      case 0xc9:
        if (!be(width(t, 0xc7), n)) {
          return fail("truncated");
        }
        return ext(n);
      case 0xca: {
        if (!be(4, n)) {
          return fail("truncated");
        }
        return Mp(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(n))));
      }
      case 0xcb:
        if (!be(8, n)) {
          return fail("truncated");
        }
        return Mp(std::bit_cast<double>(n));
      case 0xcc:
      case 0xcd:
      case 0xce:
      case 0xcf:
        if (!be(width(t, 0xcc), n)) {
          return fail("truncated");
        }
        return Mp::from_unsigned(n);
      case 0xd0:
      case 0xd1:
      case 0xd2:
      case 0xd3: {
        const int w = width(t, 0xd0);
        if (!be(w, n)) {
          return fail("truncated");
        }
        // sign-extend from w bytes
        const auto shift = static_cast<unsigned>(64 - (8 * w));
        const auto v = static_cast<std::int64_t>(n << shift) >> shift;
        return Mp::from_signed(v);
      }
      case 0xd4: return ext(1);
      case 0xd5: return ext(2);
      case 0xd6: return ext(4);
      case 0xd7: return ext(8);
      case 0xd8: return ext(16);
      case 0xd9:
      case 0xda:
      case 0xdb:
        if (!be(width(t, 0xd9), n)) {
          return fail("truncated");
        }
        return text(n);
      case 0xdc:
      case 0xdd:
        if (!be(t == 0xdc ? 2 : 4, n)) {
          return fail("truncated");
        }
        return arr(n, depth);
      case 0xde:
      case 0xdf:
        if (!be(t == 0xde ? 2 : 4, n)) {
          return fail("truncated");
        }
        return map(n, depth);
      default: return fail("unknown type");
    }
  }

 private:
  [[nodiscard]] std::size_t left() const noexcept { return b_.size() - p_; }
  bool be(int w, std::uint64_t& out) {
    if (left() < static_cast<std::size_t>(w)) {
      return false;
    }
    out = 0;
    for (int i = 0; i < w; ++i) {
      out = (out << 8U) | b_[p_++];
    }
    return true;
  }
  base::Result<Mp> text(std::uint64_t n) {
    if (n > left()) {
      return fail("truncated");
    }
    auto s = decode_utf8(b_.subspan(p_, static_cast<std::size_t>(n)));
    p_ += static_cast<std::size_t>(n);
    return Mp(std::move(s));
  }
  base::Result<Mp> bytes(std::uint64_t n) {
    if (n > left()) {
      return fail("truncated");
    }
    const auto s = b_.subspan(p_, static_cast<std::size_t>(n));
    p_ += static_cast<std::size_t>(n);
    return Mp(MpBytes(s.begin(), s.end()));
  }
  base::Result<Mp> ext(std::uint64_t n) {
    if (left() < 1) {
      return fail("truncated");
    }
    const auto type = static_cast<std::int8_t>(b_[p_++]);
    auto data = bytes(n);
    if (!data) {
      return data;
    }
    Map m;
    m.set("ext", Mp(static_cast<double>(type)));
    m.set("data", std::move(*data));
    return Mp(std::move(m));
  }
  // NOLINTNEXTLINE(misc-no-recursion) — nesting is capped at kMaxDepth (512)
  base::Result<Mp> arr(std::uint64_t n, std::size_t depth) {
    if (n > left()) {
      return fail("truncated");
    }
    MpArray a;
    a.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
      auto x = any(depth + 1);
      if (!x) {
        return x;
      }
      a.push_back(std::move(*x));
    }
    return Mp(std::move(a));
  }
  // NOLINTNEXTLINE(misc-no-recursion) — nesting is capped at kMaxDepth (512)
  base::Result<Mp> map(std::uint64_t n, std::size_t depth) {
    if (n > left() / 2) {
      return fail("truncated");
    }
    Map m;
    for (std::uint64_t i = 0; i < n; ++i) {
      auto k = any(depth + 1);
      if (!k) {
        return k;
      }
      std::string key = k->is_string() ? k->as_string() : key_string(*k);
      auto x = any(depth + 1);
      if (!x) {
        return x;
      }
      if (key == "__proto__") {
        continue;
      }
      m.set(key, std::move(*x));
    }
    return Mp(std::move(m));
  }

  std::span<const std::uint8_t> b_;
  std::size_t p_ = 0;
};

}  // namespace

void pack_into(const Mp& value, MpBytes& out) {
  Writer w(std::move(out));
  w.any(value);
  out = std::move(w).take();
}

MpBytes pack(const Mp& value) {
  MpBytes out;
  out.reserve(256);
  pack_into(value, out);
  return out;
}

base::Result<Mp> unpack(std::span<const std::uint8_t> bytes) {
  Reader r(bytes);
  auto v = r.any(0);
  if (!v) {
    return v;
  }
  if (r.pos() != bytes.size()) {
    return fail("trailing bytes");
  }
  return v;
}

}  // namespace cc::live
