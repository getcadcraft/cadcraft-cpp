// SPDX-License-Identifier: Apache-2.0
// RFC 8259 JSON → Value. Recursive descent with an explicit depth limit (checked before descending).
#include "cc/base/json.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <system_error>

namespace cc::base {

namespace {

class Parser {
 public:
  Parser(std::string_view t, const JsonLimits& l) : t_(t), limits_(l) {}

  Result<Value> document() {
    skip_ws();
    auto r = value(0);
    if (!r) {
      return std::unexpected(r.error());
    }
    Value v = std::move(*r);
    skip_ws();
    if (p_ != t_.size()) {
      return err("unexpected text after the JSON value");
    }
    return v;
  }

 private:
  [[nodiscard]] std::unexpected<Error> err(const std::string& what) const {
    return fail(Errc::invalid, "JSON: " + what + " at byte " + std::to_string(p_));
  }
  [[nodiscard]] bool at_end() const { return p_ >= t_.size(); }
  [[nodiscard]] char peek() const { return at_end() ? '\0' : t_[p_]; }
  static bool digit(char c) { return c >= '0' && c <= '9'; }
  void skip_ws() {
    while (!at_end() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) {
      ++p_;
    }
  }
  bool literal(std::string_view w) {
    if (t_.substr(p_, w.size()) == w) {
      p_ += w.size();
      return true;
    }
    return false;
  }

  // Depth is checked before each descent, so the recursion is bounded by limits_.max_depth.
  // NOLINTNEXTLINE(misc-no-recursion)
  Result<Value> value(std::size_t depth) {
    switch (peek()) {
      case '{': return object(depth + 1);
      case '[': return array(depth + 1);
      case '"': {
        std::string s;
        if (auto r = string(s); !r) {
          return std::unexpected(r.error());
        }
        return Value(std::move(s));
      }
      case 't':
        if (literal("true")) {
          return Value(true);
        }
        return err("bad literal");
      case 'f':
        if (literal("false")) {
          return Value(false);
        }
        return err("bad literal");
      case 'n':
        if (literal("null")) {
          return Value(nullptr);
        }
        return err("bad literal");
      default: return number();
    }
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  Result<Value> object(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return err("nested too deep");
    }
    ++p_;  // {
    Value o = Value::object();
    skip_ws();
    if (peek() == '}') {
      ++p_;
      return o;
    }
    for (;;) {
      skip_ws();
      if (peek() != '"') {
        return err("expected a key");
      }
      std::string key;
      if (auto r = string(key); !r) {
        return std::unexpected(r.error());
      }
      skip_ws();
      if (peek() != ':') {
        return err("expected ':'");
      }
      ++p_;
      skip_ws();
      auto v = value(depth);
      if (!v) {
        return std::unexpected(v.error());
      }
      o.set(key, std::move(*v));
      skip_ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == '}') {
        ++p_;
        return o;
      }
      return err("expected ',' or '}'");
    }
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  Result<Value> array(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return err("nested too deep");
    }
    ++p_;  // [
    Value a = Value::array();
    skip_ws();
    if (peek() == ']') {
      ++p_;
      return a;
    }
    for (;;) {
      skip_ws();
      auto v = value(depth);
      if (!v) {
        return std::unexpected(v.error());
      }
      a.push(std::move(*v));
      skip_ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == ']') {
        ++p_;
        return a;
      }
      return err("expected ',' or ']'");
    }
  }

  Result<void> hex4(std::uint32_t& out) {
    if (p_ + 4 > t_.size()) {
      return err("short \\u escape");
    }
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_++];
      std::uint32_t d = 0;
      if (c >= '0' && c <= '9') {
        d = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return err("bad \\u escape");
      }
      out = (out << 4U) | d;
    }
    return {};
  }

  Result<void> string(std::string& s) {
    ++p_;  // "
    for (;;) {
      if (at_end()) {
        return err("unterminated string");
      }
      if (s.size() > limits_.max_string) {
        return fail(Errc::too_big, "JSON: a string is over the limit");
      }
      const auto c = static_cast<unsigned char>(t_[p_]);
      if (c == '"') {
        ++p_;
        return {};
      }
      if (c < 0x20) {
        return err("control character in a string");
      }
      if (c != '\\') {
        // copy a run of plain bytes at once
        const std::size_t start = p_;
        while (!at_end() && t_[p_] != '"' && t_[p_] != '\\' && static_cast<unsigned char>(t_[p_]) >= 0x20) {
          ++p_;
        }
        s.append(t_.substr(start, p_ - start));
        continue;
      }
      ++p_;
      if (at_end()) {
        return err("unterminated escape");
      }
      const char e = t_[p_++];
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          std::uint32_t u = 0;
          if (auto r = hex4(u); !r) {
            return r;
          }
          if (u >= 0xD800 && u <= 0xDBFF && t_.substr(p_, 2) == "\\u") {
            const std::size_t save = p_;
            p_ += 2;
            std::uint32_t lo = 0;
            if (auto r = hex4(lo); !r) {
              return r;
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              append_code_point(s, 0x10000U + ((u - 0xD800U) << 10U) + (lo - 0xDC00U));
              break;
            }
            p_ = save;  // not a pair: keep the lone high surrogate, read the next escape on its own
          }
          append_code_point(s, u);  // lone surrogates stay as WTF-8, as write_json writes them back
          break;
        }
        default: return err("bad escape");
      }
    }
  }

  Result<Value> number() {
    const std::size_t start = p_;
    if (peek() == '-') {
      ++p_;
    }
    if (peek() == '0') {
      ++p_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') {
        ++p_;
      }
    } else {
      return err("unexpected character");
    }
    if (peek() == '.') {
      ++p_;
      if (!digit(peek())) {
        return err("bad number");
      }
      while (peek() >= '0' && peek() <= '9') {
        ++p_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++p_;
      if (peek() == '+' || peek() == '-') {
        ++p_;
      }
      if (!digit(peek())) {
        return err("bad number");
      }
      while (peek() >= '0' && peek() <= '9') {
        ++p_;
      }
    }
    const std::span<const char> num(t_.substr(start, p_ - start));
    double d = 0;
    const auto [ptr, ec] = std::from_chars(num.data(), std::to_address(num.end()), d);
    if (ec == std::errc::result_out_of_range) {
      // JSON.parse turns out-of-range numbers into ±Infinity or ±0; from_chars refuses them.
      const bool neg = !num.empty() && num.front() == '-';
      const std::string_view sv(num.data(), num.size());
      const bool tiny = sv.contains("e-") || sv.contains("E-");
      if (tiny) {
        return Value(neg ? -0.0 : 0.0);
      }
      return Value(neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    if (ec != std::errc{} || ptr != std::to_address(num.end())) {
      return err("bad number");
    }
    return Value(d);
  }

  std::string_view t_;
  JsonLimits limits_;
  std::size_t p_ = 0;
};

}  // namespace

Result<Value> parse_json(std::string_view text, const JsonLimits& limits) {
  if (text.size() > limits.max_bytes) {
    return fail(Errc::too_big, "JSON: the input is over the size limit");
  }
  try {
    Parser p(text, limits);
    return p.document();
  } catch (const std::bad_alloc&) {
    return fail(Errc::too_big, "JSON: out of memory");
  }
}

}  // namespace cc::base
