// SPDX-License-Identifier: Apache-2.0
// step.js (cc-ifc, CadCraft 0.5) in C++: see cc/ifc/step.hpp. The JS indexes `b[i]` anywhere in the file's buffer
// (an index past its end reads `undefined`), so every byte here is read through at(), which answers -1 there; the
// statement bounds are the JS's own `end` arguments.
#include "cc/ifc/step.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"

namespace cc::ifc {
namespace {

using U8 = std::span<const std::uint8_t>;

constexpr int kHash = 35;
constexpr int kEq = 61;
constexpr int kSemi = 59;
constexpr int kLp = 40;
constexpr int kRp = 41;
constexpr int kComma = 44;
constexpr int kQuote = 39;
constexpr int kDquote = 34;
constexpr int kSlash = 47;
constexpr int kStar = 42;
constexpr int kDollar = 36;
constexpr int kDot = 46;
constexpr int kMinus = 45;
constexpr int kPlus = 43;
constexpr int kBsl = 92;

inline int at(U8 b, std::size_t i) noexcept {
  return i < b.size() ? static_cast<int>(b[i]) : -1;
}
inline bool is_space(int c) noexcept {
  return c == 32 || c == 10 || c == 13 || c == 9 || c == 12 || c == 11;
}
inline bool is_digit(int c) noexcept {
  return c >= 48 && c <= 57;
}
inline bool is_key_char(int c) noexcept {
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || is_digit(c) || c == 95 || c == kMinus || c == 33;
}

StepError step_error(std::string message, std::optional<std::size_t> offset) {
  if (offset) {
    message += " (at byte " + std::to_string(*offset) + ")";
  }
  return StepError{"StepError", std::move(message)};
}

/// Skip blanks and comments from i; → the first other byte's offset (may pass `end` after an unclosed comment).
std::size_t skip(U8 b, std::size_t i, std::size_t end) {
  for (;;) {
    while (i < end && is_space(at(b, i))) {
      ++i;
    }
    if (i + 1 < end && at(b, i) == kSlash && at(b, i + 1) == kStar) {
      i += 2;
      while (i + 1 < end && (at(b, i) != kStar || at(b, i + 1) != kSlash)) {
        ++i;
      }
      i += 2;
      continue;
    }
    return i;
  }
}

/// Offset just past a quoted string starting at i (b[i] is the quote).
StepResult<std::size_t> past_string(U8 b, std::size_t i, std::size_t end) {
  ++i;
  for (;;) {
    while (i < end && at(b, i) != kQuote) {
      ++i;
    }
    if (i >= end) {
      return std::unexpected(step_error("A text value is not closed", i));
    }
    if (i + 1 < end && at(b, i + 1) == kQuote) {
      i += 2;
      continue;
    }
    return i + 1;
  }
}

/// Offset of the ";" ending the statement that continues at i, or -1.
StepResult<std::ptrdiff_t> statement_end(U8 b, std::size_t i, std::size_t end) {
  while (i < end) {
    const int c = at(b, i);
    if (c == kSemi) {
      return static_cast<std::ptrdiff_t>(i);
    }
    if (c == kQuote) {
      auto p = past_string(b, i, end);
      if (!p) {
        return std::unexpected(std::move(p.error()));
      }
      i = *p;
      continue;
    }
    if (c == kDquote) {
      ++i;
      while (i < end && at(b, i) != kDquote) {
        ++i;
      }
      ++i;
      continue;
    }
    if (c == kSlash && i + 1 < end && at(b, i + 1) == kStar) {
      i += 2;
      while (i + 1 < end && (at(b, i) != kStar || at(b, i + 1) != kSlash)) {
        ++i;
      }
      i += 2;
      continue;
    }
    ++i;
  }
  return -1;
}

struct Keyword {
  std::string name;
  std::size_t next = 0;
};
Keyword keyword_at(U8 b, std::size_t i, std::size_t end) {
  std::size_t j = i;
  while (j < end && is_key_char(at(b, j))) {
    ++j;
  }
  Keyword k;
  k.name.reserve(j - i);
  for (std::size_t q = i; q < j; ++q) {
    const int c = at(b, q);
    k.name += static_cast<char>(c >= 97 && c <= 122 ? c - 32 : c);
  }
  k.next = j;
  return k;
}

/// Find an ASCII token (upper case) at a statement boundary from `from`; -1 when absent.
StepResult<std::ptrdiff_t> find_token(U8 b, std::string_view token, std::size_t from, std::size_t end) {
  const int t0 = static_cast<unsigned char>(token[0]);
  const std::size_t n = token.size();
  std::size_t i = from;
  while (i < end) {
    i = skip(b, i, end);
    if (i >= end) {
      return -1;
    }
    if (at(b, i) == t0 || at(b, i) == t0 + 32) {
      bool ok = true;
      for (std::size_t k = 1; k < n && ok; ++k) {
        const int c = at(b, i + k);
        const int want = static_cast<unsigned char>(token[k]);
        ok = c == want || (want >= 65 && want <= 90 && c == want + 32);
      }
      if (ok && !is_key_char(at(b, i + n) < 0 ? 0 : at(b, i + n))) {
        return static_cast<std::ptrdiff_t>(i);
      }
    }
    auto e = statement_end(b, i, end);
    if (!e) {
      return std::unexpected(std::move(e.error()));
    }
    if (*e < 0) {
      return -1;
    }
    i = static_cast<std::size_t>(*e) + 1;
  }
  return -1;
}

// ---------------------------------------------------------------- strings

/// Latin-1 bytes (String.fromCharCode of each) → UTF-8.
std::string latin1(U8 b, std::size_t s, std::size_t e) {
  std::string t;
  t.reserve(e > s ? e - s : 0);
  for (std::size_t k = s; k < e; ++k) {
    base::append_code_point(t, static_cast<std::uint32_t>(at(b, k) < 0 ? 0 : at(b, k)));
  }
  return t;
}

/// TextDecoder('utf-8', {fatal: true}).decode: the bytes when they are well-formed UTF-8 (a leading BOM dropped).
std::optional<std::string> utf8_fatal(U8 b, std::size_t s, std::size_t e) {
  std::size_t i = s;
  while (i < e) {
    const int c = at(b, i);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t n = 0;
    int lo = 0x80;
    int hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      n = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      n = 2;
      if (c == 0xE0) {
        lo = 0xA0;
      } else if (c == 0xED) {
        hi = 0x9F;
      }
    } else if (c >= 0xF0 && c <= 0xF4) {
      n = 3;
      if (c == 0xF0) {
        lo = 0x90;
      } else if (c == 0xF4) {
        hi = 0x8F;
      }
    } else {
      return std::nullopt;
    }
    for (std::size_t k = 1; k <= n; ++k) {
      if (i + k >= e) {
        return std::nullopt;
      }
      const int d = at(b, i + k);
      if (k == 1 ? (d < lo || d > hi) : (d < 0x80 || d > 0xBF)) {
        return std::nullopt;
      }
    }
    i += n + 1;
  }
  std::size_t from = s;
  if (e - s >= 3 && at(b, s) == 0xEF && at(b, s + 1) == 0xBB && at(b, s + 2) == 0xBF) {
    from += 3;
  }
  std::string t;
  t.reserve(e - from);
  for (std::size_t k = from; k < e; ++k) {
    t += static_cast<char>(at(b, k));
  }
  return t;
}

bool is_hex(std::uint16_t c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
std::uint32_t hex_of(std::span<const std::uint16_t> u) noexcept {
  std::uint32_t v = 0;
  for (const std::uint16_t c : u) {
    const std::uint32_t d = c <= '9' ? c - '0' : (c | 0x20U) - 'a' + 10;
    v = v * 16 + d;
  }
  return v;
}

/// \\ \S\c \P?\ \X\hh \X2\hhhh…\X0\ \X4\hhhhhhhh…\X0\ (ISO 10303-21 7.3.3) on UTF-16 units, as step.js unescapeStep.
StepResult<std::vector<std::uint16_t>> unescape_step(const std::vector<std::uint16_t>& t) {
  std::vector<std::uint16_t> out;
  out.reserve(t.size());
  const std::size_t n = t.size();
  std::optional<std::vector<std::uint16_t>> upper;  // t.toUpperCase(), made on the first \X2\ or \X4\ seen
  auto u = [&](std::size_t k) -> int { return k < n ? t[k] : -1; };
  std::size_t i = 0;
  while (i < n) {
    const std::uint16_t c = t[i];
    if (c != '\\') {
      out.push_back(c);
      ++i;
      continue;
    }
    const int c1 = u(i + 1);
    if (c1 == '\\') {
      out.push_back('\\');
      i += 2;
      continue;
    }
    if ((c1 == 'S' || c1 == 's' || c1 == 0x17F) && u(i + 2) == '\\' && i + 3 < n) {
      out.push_back(static_cast<std::uint16_t>(t[i + 3] + 128U));
      i += 4;
      continue;
    }
    if ((c1 == 'P' || c1 == 'p') && u(i + 3) == '\\') {
      i += 4;
      continue;
    }
    const bool x = c1 == 'X' || c1 == 'x';
    if (x && u(i + 2) == '\\' && i + 4 < n && is_hex(t[i + 3]) && is_hex(t[i + 4])) {
      out.push_back(static_cast<std::uint16_t>(hex_of(std::span(t).subspan(i + 3, 2))));
      i += 5;
      continue;
    }
    if (x && (u(i + 2) == '2' || u(i + 2) == '4') && u(i + 3) == '\\') {
      const std::size_t w = u(i + 2) == '2' ? 4 : 8;
      if (!upper) {
        std::string s8;
        base::append_utf16(s8, t);
        upper = base::to_utf16(base::js_to_upper(s8));
      }
      const std::vector<std::uint16_t>& up = *upper;
      std::size_t close = 0;
      bool found = false;
      for (std::size_t k = i + 4; k + 3 < up.size(); ++k) {
        if (up[k] == '\\' && up[k + 1] == 'X' && up[k + 2] == '0' && up[k + 3] == '\\') {
          close = k;
          found = true;
          break;
        }
      }
      if (found && close > 0) {
        const std::size_t hs = i + 4;
        const std::size_t he = close < n ? close : n;
        const std::size_t len = he > hs ? he - hs : 0;
        bool ok = len % w == 0;
        for (std::size_t k = hs; ok && k < hs + len; ++k) {
          ok = is_hex(t[k]);
        }
        if (ok) {
          for (std::size_t k = 0; k < len; k += w) {
            const std::uint32_t cp = hex_of(std::span(t).subspan(hs + k, w));
            if (w == 8 && cp > 0x10FFFF) {
              return std::unexpected(StepError{"RangeError", "Invalid code point " + std::to_string(cp)});
            }
            if (cp >= 0x10000) {
              out.push_back(static_cast<std::uint16_t>(0xD800U + ((cp - 0x10000U) >> 10U)));
              out.push_back(static_cast<std::uint16_t>(0xDC00U + ((cp - 0x10000U) & 0x3FFU)));
            } else {
              out.push_back(static_cast<std::uint16_t>(cp));
            }
          }
          i = close + 4;
          continue;
        }
      }
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

/// The text of a string token b[s..e) (between the quotes), as step.js decodeString.
StepResult<std::string> decode_string(U8 b, std::size_t s, std::size_t e) {
  bool ascii = true;
  bool quote = false;
  bool bsl = false;
  for (std::size_t k = s; k < e; ++k) {
    const int c = at(b, k);
    if (c >= 128) {
      ascii = false;
    } else if (c == kQuote) {
      quote = true;
    } else if (c == kBsl) {
      bsl = true;
    }
  }
  std::string t;
  if (ascii) {
    t.reserve(e - s);
    for (std::size_t k = s; k < e; ++k) {
      t += static_cast<char>(at(b, k));
    }
  } else if (auto d = utf8_fatal(b, s, e)) {
    t = std::move(*d);
  } else {
    t = latin1(b, s, e);
  }
  if (quote) {
    std::string r;
    r.reserve(t.size());
    for (std::size_t k = 0; k < t.size(); ++k) {
      r += t[k];
      if (t[k] == '\'' && k + 1 < t.size() && t[k + 1] == '\'') {
        ++k;
      }
    }
    t = std::move(r);
  }
  if (!bsl) {
    return t;
  }
  auto un = unescape_step(base::to_utf16(t));
  if (!un) {
    return std::unexpected(std::move(un.error()));
  }
  std::string out;
  out.reserve(un->size());
  base::append_utf16(out, *un);
  return out;
}

// ---------------------------------------------------------------- parser

class Parser {
 public:
  Parser(U8 b, std::size_t i, std::size_t end) noexcept : b_(b), i_(i), end_(end) {}

  [[nodiscard]] std::size_t pos() const noexcept { return i_; }

  // NOLINTNEXTLINE(misc-no-recursion) — bounded by kMaxDepth
  StepResult<RawList> list() {
    if (++depth_ > kMaxDepth) {
      return std::unexpected(StepError{"RangeError", "Maximum call stack size exceeded"});
    }
    i_ = skip(b_, i_, end_);
    if (at(b_, i_) != kLp) {
      return fail("Expected (");
    }
    ++i_;
    RawList out;
    i_ = skip(b_, i_, end_);
    if (at(b_, i_) == kRp) {
      ++i_;
      --depth_;
      return out;
    }
    for (;;) {
      auto v = value();
      if (!v) {
        return std::unexpected(std::move(v.error()));
      }
      out.push_back(std::move(*v));
      i_ = skip(b_, i_, end_);
      const int c = at(b_, i_);
      if (c == kComma) {
        ++i_;
        continue;
      }
      if (c == kRp) {
        ++i_;
        --depth_;
        return out;
      }
      return fail("Expected , or )");
    }
  }

 private:
  [[nodiscard]] std::unexpected<StepError> fail(std::string msg) const {
    return std::unexpected(step_error(std::move(msg), i_));
  }

  // NOLINTNEXTLINE(misc-no-recursion) — bounded by kMaxDepth (list)
  StepResult<Raw> value() {
    i_ = skip(b_, i_, end_);
    const int c = at(b_, i_);
    Raw r;
    if (c == kDollar) {
      ++i_;
      return r;
    }
    if (c == kStar) {
      ++i_;
      r.kind = Raw::Kind::derived;
      return r;
    }
    if (c == kHash) {
      std::size_t j = i_ + 1;
      double n = 0;
      while (is_digit(at(b_, j))) {
        n = n * 10 + (at(b_, j) - 48);
        ++j;
      }
      if (j == i_ + 1) {
        return fail("Expected a number after #");
      }
      i_ = j;
      r.kind = Raw::Kind::ref;
      r.num = n;
      return r;
    }
    if (c == kQuote) {
      const std::size_t s = i_ + 1;
      auto e = past_string(b_, i_, end_);
      if (!e) {
        return std::unexpected(std::move(e.error()));
      }
      i_ = *e;
      auto t = decode_string(b_, s, *e - 1);
      if (!t) {
        return std::unexpected(std::move(t.error()));
      }
      r.kind = Raw::Kind::string;
      r.text = std::move(*t);
      return r;
    }
    if (c == kDot) {
      std::size_t j = i_ + 1;
      while (j < end_ && at(b_, j) != kDot) {
        ++j;
      }
      bool plain = true;
      for (std::size_t k = i_ + 1; k < j && plain; ++k) {
        const int ch = at(b_, k);
        plain = ch > 32 && ch < 127;
      }
      r.kind = Raw::Kind::enumeration;
      if (plain) {
        r.text.reserve(j - i_);
        for (std::size_t k = i_ + 1; k < j; ++k) {
          const int ch = at(b_, k);
          r.text += static_cast<char>(ch >= 97 && ch <= 122 ? ch - 32 : ch);
        }
      } else {
        r.text = base::js_to_upper(base::js_trim(latin1(b_, i_ + 1, j)));
      }
      i_ = j + 1;
      return r;
    }
    if (c == kLp) {
      auto l = list();
      if (!l) {
        return std::unexpected(std::move(l.error()));
      }
      r.kind = Raw::Kind::list;
      r.items = std::move(*l);
      return r;
    }
    if (c == kDquote) {
      std::size_t j = i_ + 1;
      while (j < end_ && at(b_, j) != kDquote) {
        ++j;
      }
      r.kind = Raw::Kind::binary;
      r.text = latin1(b_, i_ + 1, j);
      i_ = j + 1;
      return r;
    }
    if (is_digit(c) || c == kMinus || c == kPlus || c == kDot) {
      return number();
    }
    if (is_key_char(c)) {
      Keyword kw = keyword_at(b_, i_, end_);
      i_ = skip(b_, kw.next, end_);
      if (at(b_, i_) != kLp) {
        return fail("Expected ( after " + kw.name);
      }
      auto inner = list();
      if (!inner) {
        return std::unexpected(std::move(inner.error()));
      }
      r.kind = Raw::Kind::typed;
      r.text = std::move(kw.name);
      if (inner->size() == 1) {
        r.items.push_back(std::move((*inner)[0]));
      } else {
        Raw l;
        l.kind = Raw::Kind::list;
        l.items = std::move(*inner);
        r.items.push_back(std::move(l));
      }
      return r;
    }
    std::string ch;
    base::append_code_point(ch, static_cast<std::uint32_t>(c < 0 ? 0 : c));
    std::string quoted;
    base::append_json_string(quoted, ch);
    return fail("Unexpected character " + quoted);
  }

  StepResult<Raw> number() {
    std::size_t j = i_;
    std::string s;
    while (j < end_) {
      const int c = at(b_, j);
      if (is_digit(c) || c == kMinus || c == kPlus || c == kDot || c == 69 || c == 101) {
        s += static_cast<char>(c);
        ++j;
      } else {
        break;
      }
    }
    i_ = j;
    // "1." and "1.E5" are valid Part 21 reals; Number() takes both (step.js adds the 0)
    std::string t = s;
    if (!t.empty() && t.back() == '.') {
      t += '0';
    } else {
      for (std::size_t k = 0; k + 1 < t.size(); ++k) {
        if (t[k] == '.' && (t[k + 1] == 'e' || t[k + 1] == 'E')) {
          t.insert(k + 1, "0");
          break;
        }
      }
    }
    const double n = base::js_number(t);
    if (!std::isfinite(n)) {
      return fail("Bad number " + s);
    }
    Raw r;
    r.kind = Raw::Kind::number;
    r.num = n;
    return r;
  }

  U8 b_;
  std::size_t i_;
  std::size_t end_;
  std::size_t depth_ = 0;
};

StepResult<RawList> parse_params_u8(U8 b, std::size_t start, std::size_t end) {
  Parser p(b, start, end);
  return p.list();
}

StepResult<void> read_header(U8 b, std::size_t i, std::size_t end, base::Value& header) {
  while (i < end) {
    i = skip(b, i, end);
    if (i >= end) {
      break;
    }
    Keyword kw = keyword_at(b, i, end);
    auto e = statement_end(b, i, end);
    if (!e) {
      return std::unexpected(std::move(e.error()));
    }
    if (*e < 0 || kw.name == "ENDSEC") {
      break;
    }
    const std::size_t p = skip(b, kw.next, end);
    if (at(b, p) == kLp) {
      auto params = parse_params_u8(b, p, static_cast<std::size_t>(*e));
      if (params) {
        base::Value v = to_value(*params);
        if (kw.name == "FILE_SCHEMA") {
          base::Array schema;
          if (!params->empty() && (*params)[0].is_list()) {
            for (const Raw& s : (*params)[0].items) {
              if (s.is_string()) {
                schema.emplace_back(base::js_to_upper(base::js_trim(s.text)));
              }
            }
          }
          header.set("schema", std::move(schema));
        }
        if (kw.name == "FILE_DESCRIPTION") {
          header.set("description", v);
        }
        if (kw.name == "FILE_NAME") {
          header.set("name", v);
        }
        header.find("raw")->set(kw.name, std::move(v));
      }
    }
    i = static_cast<std::size_t>(*e) + 1;
  }
  return {};
}

}  // namespace

StepResult<RawList> parse_params(Bytes bytes, std::size_t start, std::size_t end) {
  return parse_params_u8(base::as_u8(bytes), start, end);
}

StepResult<std::vector<ComplexPart>> parse_complex(Bytes bytes, std::size_t start, std::size_t end) {
  const U8 b = base::as_u8(bytes);
  std::vector<ComplexPart> out;
  std::size_t i = skip(b, start + 1, end);
  while (i < end && at(b, i) != kRp) {
    Keyword kw = keyword_at(b, i, end);
    Parser p(b, skip(b, kw.next, end), end);
    auto l = p.list();
    if (!l) {
      return std::unexpected(std::move(l.error()));
    }
    out.push_back(ComplexPart{std::move(kw.name), std::move(*l)});
    i = skip(b, p.pos(), end);
  }
  return out;
}

StepResult<StepIndex> scan_step(Bytes bytes) {
  const U8 b = base::as_u8(bytes);
  const std::size_t end = b.size();
  if (end > 0xFFFFFFFFU) {
    return std::unexpected(StepError{"StepError", "The file is too large (over 4 GB)"});
  }
  std::size_t i = skip(b, 0, end);
  if (at(b, i) == 0xEF && at(b, i + 1) == 0xBB && at(b, i + 2) == 0xBF) {
    i = skip(b, i + 3, end);
  }
  const Keyword magic = keyword_at(b, i, end);
  if (magic.name != "ISO-10303-21") {
    return std::unexpected(step_error("Not an IFC (STEP) file: it does not start with ISO-10303-21;", std::nullopt));
  }

  StepIndex ix;
  ix.header = base::make_object(
      {{"description", nullptr}, {"name", nullptr}, {"schema", base::Value::array()}, {"raw", base::Value::object()}});
  auto hdr = find_token(b, "HEADER", i, end);
  if (!hdr) {
    return std::unexpected(std::move(hdr.error()));
  }
  auto data0 = find_token(b, "DATA", i, end);
  if (!data0) {
    return std::unexpected(std::move(data0.error()));
  }
  if (*data0 < 0) {
    return std::unexpected(step_error("This file has no DATA section.", std::nullopt));
  }
  if (*hdr >= 0 && *hdr < *data0) {
    auto he = statement_end(b, static_cast<std::size_t>(*hdr), end);
    if (!he) {
      return std::unexpected(std::move(he.error()));
    }
    const auto from = static_cast<std::size_t>(*he + 1);
    if (auto r = read_header(b, from, static_cast<std::size_t>(*data0), ix.header); !r) {
      return std::unexpected(std::move(r.error()));
    }
  }

  const std::size_t guess = end / 60 < (std::size_t{1} << 24U) ? end / 60 : (std::size_t{1} << 24U);
  ix.ids.reserve(guess);
  ix.starts.reserve(guess);
  ix.ends.reserve(guess);
  ix.types.reserve(guess);
  ix.names.emplace_back();
  std::unordered_map<std::string, std::size_t> name_idx{{"", 0}};
  auto push_error = [&](std::size_t off, std::string msg) {
    ix.errors.push_back(ScanError{static_cast<double>(off), std::move(msg)});
  };

  auto first = statement_end(b, static_cast<std::size_t>(*data0), end);
  if (!first) {
    return std::unexpected(std::move(first.error()));
  }
  i = static_cast<std::size_t>(*first + 1);
  while (i < end) {
    i = skip(b, i, end);
    if (i >= end) {
      break;
    }
    if (at(b, i) != kHash) {
      const Keyword kw = keyword_at(b, i, end);
      if (kw.name == "ENDSEC") {
        auto more = find_token(b, "DATA", kw.next, end);
        if (!more) {
          return std::unexpected(std::move(more.error()));
        }
        if (*more < 0) {
          break;
        }
        auto me = statement_end(b, static_cast<std::size_t>(*more), end);
        if (!me) {
          return std::unexpected(std::move(me.error()));
        }
        if (*me < 0) {
          // A DATA with no ";" after it: the JS starts over at byte 0 (-1 + 1) and meets this ENDSEC again, for ever
          // (docs/gaps.md). Here the scan ends.
          break;
        }
        i = static_cast<std::size_t>(*me + 1);
        continue;
      }
      auto e = statement_end(b, i, end);
      if (!e) {
        return std::unexpected(std::move(e.error()));
      }
      push_error(i, "Unexpected text in the DATA section");
      if (*e < 0) {
        break;
      }
      i = static_cast<std::size_t>(*e) + 1;
      continue;
    }
    std::size_t j = i + 1;
    double id = 0;
    while (j < end && is_digit(at(b, j))) {
      id = id * 10 + (at(b, j) - 48);
      ++j;
    }
    j = skip(b, j, end);
    auto er = statement_end(b, j, end);
    if (!er) {
      return std::unexpected(std::move(er.error()));
    }
    if (*er < 0) {
      push_error(i, "An instance is not closed with ;");
      break;
    }
    const auto e = static_cast<std::size_t>(*er);
    if (j == i + 1 || at(b, j) != kEq) {
      push_error(i, "Expected #n=");
      i = e + 1;
      continue;
    }
    j = skip(b, j + 1, end);
    std::size_t t = 0;
    std::size_t start = 0;
    if (at(b, j) == kLp) {
      start = j;
    } else {
      Keyword kw = keyword_at(b, j, end);
      if (kw.name.empty()) {
        push_error(i, "Expected an entity name");
        i = e + 1;
        continue;
      }
      if (auto it = name_idx.find(kw.name); it != name_idx.end()) {
        t = it->second;
      } else {
        t = ix.names.size();
        name_idx.emplace(kw.name, t);
        ix.names.push_back(kw.name);
      }
      start = skip(b, kw.next, end);
      if (at(b, start) != kLp) {
        push_error(i, "Expected ( after " + kw.name);
        i = e + 1;
        continue;
      }
    }
    ix.ids.push_back(base::js_to_int32(id));
    ix.starts.push_back(static_cast<std::uint32_t>(start));
    ix.ends.push_back(static_cast<std::uint32_t>(e));
    ix.types.push_back(static_cast<std::uint16_t>(t & 0xFFFFU));
    ++ix.count;
    ix.max_id = std::max(id, ix.max_id);
    i = e + 1;
  }
  const base::Value schema0 =
      ix.header["schema"].as_array().empty() ? base::Value() : ix.header["schema"].as_array()[0];
  ix.schema = schema0.is_string() && !schema0.as_string().empty() ? schema0.as_string() : std::string();
  return ix;
}

// ---------------------------------------------------------------- JSON

namespace {
void append_num(std::string& out, double d) {
  if (std::isnan(d)) {
    out += "\"$NaN\"";
  } else if (std::isinf(d)) {
    out += d > 0 ? "\"$Inf\"" : "\"$-Inf\"";
  } else {
    base::append_number(out, d);
  }
}
}  // namespace

// NOLINTNEXTLINE(misc-no-recursion) — a parsed value is at most kMaxDepth deep
void write_json(const Raw& r, std::string& out) {
  switch (r.kind) {
    case Raw::Kind::null: out += "null"; return;
    case Raw::Kind::derived: out += "{\"derived\":true}"; return;
    case Raw::Kind::number: append_num(out, r.num); return;
    case Raw::Kind::string: base::append_json_string(out, r.text); return;
    case Raw::Kind::ref:
      out += "{\"ref\":";
      append_num(out, r.num);
      out += '}';
      return;
    case Raw::Kind::enumeration:
      out += "{\"enum\":";
      base::append_json_string(out, r.text);
      out += '}';
      return;
    case Raw::Kind::binary:
      out += "{\"bin\":";
      base::append_json_string(out, r.text);
      out += '}';
      return;
    case Raw::Kind::typed:
      out += "{\"typed\":";
      base::append_json_string(out, r.text);
      out += ",\"value\":";
      if (r.items.empty()) {
        out += "null";
      } else {
        write_json(r.items[0], out);
      }
      out += '}';
      return;
    case Raw::Kind::list: write_json(r.items, out); return;
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — as above
void write_json(const RawList& list, std::string& out) {
  out += '[';
  for (std::size_t k = 0; k < list.size(); ++k) {
    if (k > 0) {
      out += ',';
    }
    write_json(list[k], out);
  }
  out += ']';
}

// NOLINTNEXTLINE(misc-no-recursion) — a parsed value is at most kMaxDepth deep
base::Value to_value(const Raw& r) {
  switch (r.kind) {
    case Raw::Kind::null: return nullptr;
    case Raw::Kind::derived: return base::make_object({{"derived", true}});
    case Raw::Kind::number: return r.num;
    case Raw::Kind::string: return r.text;
    case Raw::Kind::ref: return base::make_object({{"ref", r.num}});
    case Raw::Kind::enumeration: return base::make_object({{"enum", r.text}});
    case Raw::Kind::binary: return base::make_object({{"bin", r.text}});
    case Raw::Kind::typed:
      return base::make_object(
          {{"typed", r.text}, {"value", r.items.empty() ? base::Value(nullptr) : to_value(r.items[0])}});
    case Raw::Kind::list: return to_value(r.items);
  }
  return {};
}

// NOLINTNEXTLINE(misc-no-recursion) — as above
base::Value to_value(const RawList& list) {
  base::Array a;
  a.reserve(list.size());
  for (const Raw& r : list) {
    a.push_back(to_value(r));
  }
  return a;
}

// ---------------------------------------------------------------- the kit's engine

std::string step_head_json(const StepIndex& ix) {
  base::Value types = base::Value::object();
  std::vector<double> counts(ix.names.size(), 0.0);
  for (const std::uint16_t t : ix.types) {
    counts[t] += 1;
  }
  for (std::size_t k = 0; k < ix.names.size(); ++k) {
    if (counts[k] > 0) {
      types.append(k == 0 ? std::string_view("(complex)") : std::string_view(ix.names[k]), counts[k]);
    }
  }
  base::Array errors;
  errors.reserve(ix.errors.size());
  for (const ScanError& e : ix.errors) {
    errors.push_back(base::make_object({{"offset", e.offset}, {"message", e.message}}));
  }
  const base::Value head = base::make_object({{"schema", ix.schema},
                                              {"header", ix.header},
                                              {"count", ix.count},
                                              {"maxId", ix.max_id},
                                              {"types", std::move(types)},
                                              {"errors", std::move(errors)}});
  return base::to_json(head);
}

void step_line_json(Bytes bytes, const StepIndex& ix, std::size_t i, std::string& out) {
  out += '[';
  base::append_number(out, ix.ids[i]);
  out += ',';
  const std::string& type = ix.names[ix.types[i]];
  base::append_json_string(out, type);
  out += ',';
  auto put_error = [&](const StepError& e) {
    out += "{\"$error\":";
    base::append_json_string(out, e.message);
    out += '}';
  };
  if (!type.empty()) {
    auto p = parse_params(bytes, ix.starts[i], ix.ends[i]);
    if (p) {
      write_json(*p, out);
    } else {
      put_error(p.error());
    }
  } else {
    auto p = parse_complex(bytes, ix.starts[i], ix.ends[i]);
    if (p) {
      out += '[';
      for (std::size_t k = 0; k < p->size(); ++k) {
        if (k > 0) {
          out += ',';
        }
        out += "{\"type\":";
        base::append_json_string(out, (*p)[k].type);
        out += ",\"params\":";
        write_json((*p)[k].params, out);
        out += '}';
      }
      out += ']';
    } else {
      put_error(p.error());
    }
  }
  out += ']';
}

std::size_t step_parse_all(Bytes bytes, const StepIndex& ix) {
  std::size_t ok = 0;
  for (std::size_t i = 0; i < ix.count; ++i) {
    if (!ix.names[ix.types[i]].empty()) {
      ok += parse_params(bytes, ix.starts[i], ix.ends[i]).has_value() ? 1U : 0U;
    } else {
      ok += parse_complex(bytes, ix.starts[i], ix.ends[i]).has_value() ? 1U : 0U;
    }
  }
  return ok;
}

}  // namespace cc::ifc
