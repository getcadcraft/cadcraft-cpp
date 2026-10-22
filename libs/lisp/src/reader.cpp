// SPDX-License-Identifier: Apache-2.0
// reader.js: LISP source text → forms. Syntax (the AutoLISP reference): ( ) ' . "strings" with \\ \" \n \r \t \e and
// \nnn (octal), ; comments to the end of the line and ;| inline |; comments, integers (32-bit; larger ones read as
// reals), reals (1. .5 1e3), symbols (any other run of characters, read in upper case). A ( … . x) dotted pair needs
// spaces round the dot. The JS reader recurses on V8's stack; this one stops at kMaxNesting with V8's words.
#include "cc/base/reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp {

namespace {

constexpr int kMaxNesting = 4096;

LispError reader_error(std::string m, bool incomplete) {
  LispError e(std::move(m));
  e.incomplete = incomplete;
  return e;
}

/// DELIM = /[\s()'";]/ at byte i
bool delim_at(std::string_view s, std::size_t i) {
  if (i >= s.size()) {
    return false;
  }
  const char c = s[i];
  return c == '(' || c == ')' || c == '\'' || c == '"' || c == ';' || base::js_space_at(s, i) > 0;
}

bool ascii_digits(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/// /^[+-]?\d+$/
bool int_token(std::string_view t) {
  if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
    t.remove_prefix(1);
  }
  return ascii_digits(t);
}

/// /^[+-]?(?:\d+\.\d*|\.\d+|\d+)(?:e[+-]?\d+)?$/i
bool real_token(std::string_view t) {
  std::size_t i = 0;
  if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
    ++i;
  }
  const auto digits = [&] {
    const std::size_t k = i;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
      ++i;
    }
    return i - k;
  };
  const std::size_t whole = digits();
  if (i < t.size() && t[i] == '.') {
    ++i;
    const std::size_t frac = digits();
    if (whole == 0 && frac == 0) {
      return false;
    }
  } else if (whole == 0) {
    return false;
  }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    ++i;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
      ++i;
    }
    if (digits() == 0) {
      return false;
    }
  }
  return i == t.size();
}

Value atom(std::string_view tok) {
  if (int_token(tok)) {
    const double n = base::js_number(tok);
    if (n >= -2147483648.0 && n <= 2147483647.0) {
      return Value(Int{static_cast<std::int32_t>(n)});
    }
    return {n};
  }
  if (real_token(tok)) {
    return {base::js_number(tok)};
  }
  if (tok.size() == 3 && (tok[0] == 'n' || tok[0] == 'N') && (tok[1] == 'i' || tok[1] == 'I') &&
      (tok[2] == 'l' || tok[2] == 'L')) {
    return {};
  }
  return {sym(tok)};
}

class Src {
  /// What is open around the form being read: a list (its elements so far), a quote, or a list's dotted tail.
  struct Open {
    enum class K : std::uint8_t { list, quote, dot };
    K k = K::list;
    std::vector<Value> items;
  };

 public:
  explicit Src(std::string_view text) : s_(text) {}
  [[nodiscard]] std::size_t at() const noexcept { return i_; }
  [[nodiscard]] std::size_t size() const noexcept { return s_.size(); }
  [[nodiscard]] std::string_view rest() const { return s_.substr(i_); }

  void skip() {
    for (;;) {
      while (i_ < s_.size()) {
        const std::size_t n = base::js_space_at(s_, i_);
        if (n == 0) {
          break;
        }
        i_ += n;
      }
      if (i_ >= s_.size() || s_[i_] != ';') {
        return;
      }
      if (i_ + 1 < s_.size() && s_[i_ + 1] == '|') {  // ;| inline comment |;
        const std::size_t end = s_.find("|;", i_ + 2);
        i_ = end == std::string_view::npos ? s_.size() : end + 2;
      } else {
        while (i_ < s_.size() && s_[i_] != '\n') {
          ++i_;
        }
      }
    }
  }

  /// The next form; nothing at the end. reader.js recurses (form → form); this keeps the open lists, quotes and dotted
  /// tails on an explicit stack, so deep text costs heap, not native stack: a form read inside k of them is at depth k.
  std::optional<Value> form() {
    std::vector<Open> st;
    for (;;) {
      std::optional<Value> done;
      if (!st.empty() && st.back().k == Open::K::list) {  // the list's own step before each element
        skip();
        if (i_ >= s_.size()) {
          throw reader_error("malformed list on input", true);
        }
        if (s_[i_] == ')') {
          ++i_;
          done = list(std::move(st.back().items));
          st.pop_back();
        } else if (s_[i_] == '.' && (i_ + 1 >= s_.size() || delim_at(s_, i_ + 1)) && !st.back().items.empty()) {
          ++i_;
          st.back().k = Open::K::dot;  // the tail comes next
        }
      }
      if (!done) {
        if (st.size() > static_cast<std::size_t>(kMaxNesting)) {
          throw LispError(std::string(kMsgDeep));
        }
        skip();
        if (i_ >= s_.size()) {
          if (st.empty()) {
            return std::nullopt;
          }
          throw reader_error("malformed list on input", true);  // a list, quote or tail with nothing to read
        }
        const char ch = s_[i_];
        if (ch == '(') {
          ++i_;
          st.push_back(Open{.k = Open::K::list, .items = {}});
          continue;
        }
        if (ch == ')') {
          ++i_;
          if (st.empty()) {
            throw reader_error("extra right paren on input", false);
          }
          throw reader_error("malformed list on input", true);  // after a quote or a dot: nothing read
        }
        if (ch == '\'') {
          ++i_;
          st.push_back(Open{.k = Open::K::quote, .items = {}});
          continue;
        }
        if (ch == '"') {
          done = string();
        } else {
          std::size_t j = i_;
          while (j < s_.size() && !delim_at(s_, j)) {
            ++j;
          }
          const std::string_view tok = s_.substr(i_, j - i_);
          i_ = j;
          done = atom(tok);
        }
      }
      // hand the form to what is open around it
      for (;;) {
        if (st.empty()) {
          return done;
        }
        Open& o = st.back();
        if (o.k == Open::K::list) {
          o.items.push_back(std::move(*done));
          break;
        }
        if (o.k == Open::K::quote) {
          done = list({Value(detail::kQuote()), std::move(*done)});
        } else {  // a dotted tail: the list ends here
          skip();
          if (i_ >= s_.size() || s_[i_] != ')') {
            throw reader_error("malformed list on input", false);
          }
          ++i_;
          done = list(std::move(o.items), std::move(*done));
        }
        st.pop_back();
      }
    }
  }

 private:
  std::string_view s_;
  std::size_t i_ = 0;

  Value string() {
    std::string out;
    ++i_;
    for (;;) {
      if (i_ >= s_.size()) {
        throw reader_error("malformed string on input", true);
      }
      const char c = s_[i_++];
      if (c == '"') {
        return {std::move(out)};
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) {
        ++i_;  // the JS reads `undefined` past the end, adds nothing, and fails on the next round
        continue;
      }
      const char e = s_[i_++];
      if (e == 'n') {
        out += '\n';
      } else if (e == 'r') {
        out += '\r';
      } else if (e == 't') {
        out += '\t';
      } else if (e == 'e') {
        out += '\x1b';
      } else if (e >= '0' && e <= '7') {
        auto oct = static_cast<std::uint32_t>(e - '0');
        int n = 1;
        while (n < 3 && i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '7') {
          oct = oct * 8 + static_cast<std::uint32_t>(s_[i_++] - '0');
          ++n;
        }
        base::append_code_point(out, oct);
      } else {
        out += e;  // the escaped character itself (a multi-byte one follows byte by byte)
      }
    }
  }
};

}  // namespace

std::vector<Value> detail::read_all(std::string_view text) {
  Src src(text);
  std::vector<Value> out;
  for (;;) {
    auto f = src.form();
    if (!f) {
      src.skip();
      if (src.at() >= src.size()) {
        break;
      }
      continue;
    }
    out.push_back(std::move(*f));
  }
  return out;
}

detail::ReadOne detail::read_one(std::string_view text) {
  Src src(text);
  auto f = src.form();
  return {f ? std::move(*f) : Value(), std::string(src.rest())};
}

bool balanced(std::string_view text) {
  try {
    (void)detail::read_all(text);
    return true;
  } catch (const LispError& e) {
    return !e.incomplete;
  }
}

// ---------------------------------------------------------------- decodeSource
namespace {

/// TextDecoder('utf-8', { fatal: true }) accepts it
bool valid_utf8(std::span<const std::byte> b) {
  const std::size_t n = b.size();
  std::size_t i = 0;
  const auto at = [&](std::size_t k) { return std::to_integer<unsigned>(b[k]); };
  while (i < n) {
    const unsigned c = at(i);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t need = 0;
    unsigned lo = 0x80;
    unsigned hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      lo = c == 0xE0 ? 0xA0 : 0x80;
      hi = c == 0xED ? 0x9F : 0xBF;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      lo = c == 0xF0 ? 0x90 : 0x80;
      hi = c == 0xF4 ? 0x8F : 0xBF;
    } else {
      return false;
    }
    if (i + need >= n) {
      return false;
    }
    if (at(i + 1) < lo || at(i + 1) > hi) {
      return false;
    }
    for (std::size_t k = 2; k <= need; ++k) {
      if (at(i + k) < 0x80 || at(i + k) > 0xBF) {
        return false;
      }
    }
    i += need + 1;
  }
  return true;
}

}  // namespace

std::string decode_source(std::span<const std::byte> bytes) {
  if (valid_utf8(bytes)) {
    std::string_view t = base::as_text(bytes);
    // the decoder drops a leading BOM, then .replace(/^﻿/, '') drops one more
    for (int k = 0; k < 2 && t.starts_with("\xEF\xBB\xBF"); ++k) {
      t.remove_prefix(3);
    }
    return std::string(t);
  }
  static constexpr std::array<std::uint16_t, 32> kHigh = {
      0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
  std::string w;
  w.reserve(bytes.size());
  for (const std::byte x : bytes) {
    const auto c = std::to_integer<unsigned>(x);
    base::append_code_point(w, c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80U] : c);
  }
  return w;
}

}  // namespace cc::lisp
