// SPDX-License-Identifier: Apache-2.0
#include "dxf_common.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/doc/linetypes.hpp"

namespace cc::save::detail {

namespace {

/// The shortest decimal of n into `out` with to_chars over a span (no pointer arithmetic).
std::string_view chars(std::span<char> room, double n, int precision) {
  const auto res = std::to_chars(room.data(), std::to_address(room.end()), n, std::chars_format::scientific, precision);
  return {room.data(), static_cast<std::size_t>(std::distance(room.data(), res.ptr))};
}

void append_hex4(std::string& out, std::uint16_t u) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  out += "\\U+";
  for (int sh = 12; sh >= 0; sh -= 4) {
    out += kHex[(static_cast<unsigned>(u) >> static_cast<unsigned>(sh)) & 0xFU];
  }
}

bool printable(std::string_view s) {
  return std::ranges::all_of(s, [](char c) { return c >= 0x20 && c <= 0x7e; });
}

/// Every UTF-16 unit outside 0x20–0x7e as \U+XXXX.
std::string escape_units(std::string_view s) {
  if (printable(s)) {
    return std::string(s);
  }
  std::string out;
  out.reserve(s.size() + 8);
  for (const std::uint16_t u : base::to_utf16(s)) {
    if (u >= 0x20 && u <= 0x7e) {
      out += static_cast<char>(u);
    } else {
      append_hex4(out, u);
    }
  }
  return out;
}

}  // namespace

void DxfLines::code_line(int code) {
  std::array<char, 16> buf{};
  const std::span<char> room(buf);
  const auto res = std::to_chars(room.data(), std::to_address(room.end()), code);
  const auto n = static_cast<std::size_t>(std::distance(room.data(), res.ptr));
  for (std::size_t i = n; i < 3; ++i) {
    s_ += ' ';
  }
  s_.append(room.data(), n);
  s_ += "\r\n";
}

void DxfLines::w(int code, const Value& v) {
  if (v.is_string()) {
    w(code, std::string_view(v.as_string()));
  } else if (v.is_number()) {
    w(code, v.as_number());
  } else {
    w(code, base::js_string(v));
  }
}

void DxfLines::w(int code, double n) {
  code_line(code);
  base::append_number(s_, n);
  s_ += "\r\n";
}

void DxfLines::w(int code, int n) {
  code_line(code);
  std::array<char, 16> buf{};
  const std::span<char> room(buf);
  const auto res = std::to_chars(room.data(), std::to_address(room.end()), n);
  s_.append(room.data(), static_cast<std::size_t>(std::distance(room.data(), res.ptr)));
  s_ += "\r\n";
}

std::string num_str(double n) {
  std::string s;
  base::append_number(s, n);
  return s;
}

double round15(double n) {
  if (!std::isfinite(n) || n == 0) {
    return n;
  }
  std::array<char, 64> b15{};
  std::array<char, 64> b16{};
  const std::string_view s15 = chars(b15, n, 14);
  double out = 0;
  // An exact half after the 15th digit: to_chars rounds it to even, toPrecision away from zero. 16 digits ending in 5
  // that are the whole exact expansion (the next 784 digits all zeros) make a half.
  const std::string_view s16 = chars(b16, n, 15);
  const std::size_t e16 = s16.find('e');
  if (e16 != std::string_view::npos && e16 > 0 && s16[e16 - 1] == '5') {
    std::vector<char> big(1100);
    const std::string_view sx = chars(big, n, 800);
    const std::size_t ex = sx.find('e');
    const std::size_t lead = s16.starts_with('-') ? 1 : 0;
    // digits after the 16th significant one: "d." + 15 digits come first
    const std::string_view tail = sx.substr(lead + 17, ex - (lead + 17));
    if (std::ranges::all_of(tail, [](char c) { return c == '0'; })) {
      // the 15 digits of s16, one unit added to the last (away from zero), then the exponent
      std::string m(s16.substr(0, e16 - 1));
      std::size_t i = m.size();
      bool carry = true;
      while (carry && i > lead) {
        --i;
        if (m[i] == '.') {
          continue;
        }
        if (m[i] == '9') {
          m[i] = '0';
        } else {
          ++m[i];
          carry = false;
        }
      }
      std::string t = m;
      if (carry) {
        t.insert(lead, "1");  // 9.99… → 10.00…: the parse below reads the exponent as given
      }
      t += s16.substr(e16);
      std::from_chars(t.data(), std::to_address(t.end()), out);
      return out;
    }
  }
  const std::span<const char> d15 = std::span<const char>(b15).first(s15.size());
  std::from_chars(d15.data(), std::to_address(d15.end()), out);
  return out;
}

std::string f12(double n) {
  if (std::abs(n) < 1e-12) {
    return "0.0";
  }
  return num_str(round15(n));
}

std::string f12(const Value& n) {
  if (n.is_number()) {
    return f12(n.as_number());
  }
  if (std::abs(base::js_to_number(n)) < 1e-12) {
    return "0.0";
  }
  throw std::invalid_argument("n.toPrecision is not a function");
}

std::string f2k(double n) {
  if (!std::isfinite(n)) {
    return "0.0";
  }
  return f12(n);
}

std::string f2k(const Value& n) {
  return n.is_number() ? f2k(n.as_number()) : std::string("0.0");
}

std::string encode_r12(std::string_view s) {
  if (printable(s)) {
    return std::string(s);
  }
  std::string t;
  t.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const std::string_view rest = s.substr(i);
    if (rest.starts_with("⌀")) {
      t += "%%c";
      i += 2;
    } else if (rest.starts_with("°")) {
      t += "%%d";
      i += 1;
    } else if (rest.starts_with("±")) {
      t += "%%p";
      i += 1;
    } else {
      t += s[i];
    }
  }
  return escape_units(t);
}

std::string enc2000(std::string_view s) {
  if (printable(s)) {
    return std::string(s);
  }
  std::string t;
  t.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' || s[i] == '\n') {
      while (i + 1 < s.size() && (s[i + 1] == '\r' || s[i + 1] == '\n')) {
        ++i;
      }
      t += ' ';
    } else {
      t += s[i];
    }
  }
  return escape_units(t);
}

double picture_refs(const doc::Doc& doc) {
  std::vector<std::string> pic;
  doc.blocks().each([&](const std::string&, const Value& b) {
    const Value* es = b.find("entities");
    if (es == nullptr || !es->is_array()) {
      return;
    }
    for (const Value& e : es->as_array()) {
      if (e.get("type") == Value("poly") && e.get("pic").truthy()) {
        pic.push_back(base::js_string(b.get("name")));
        return;
      }
    }
  });
  if (pic.empty()) {
    return 0;
  }
  double n = 0;
  const auto look = [&](const doc::Id&, const Value& e) {
    if (e.get("type") == Value("insert") && std::ranges::find(pic, base::js_string(e.get("block"))) != pic.end()) {
      ++n;
    }
  };
  doc.model()->each(look);
  for (const doc::Layout& l : doc.layouts()) {
    l.entities->each(look);
  }
  return n;
}

double viewport_overrides(const doc::Doc& doc) {
  double n = 0;
  for (const doc::Layout& l : doc.layouts()) {
    l.entities->each([&](const doc::Id&, const Value& e) {
      const Value lov = e.get("lov");
      if (e.get("type") == Value("viewport") && lov.truthy() && lov.is_object() && !lov.is_point() &&
          !base::js_members(lov.as_object()).empty()) {
        ++n;
      }
    });
  }
  return n;
}

std::string utf16_prefix(std::string_view s, std::size_t n) {
  if (s.size() <= n) {
    return std::string(s);
  }
  const std::vector<std::uint16_t> u = base::to_utf16(s);
  if (u.size() <= n) {
    return std::string(s);
  }
  std::string out;
  base::append_utf16(out, std::span<const std::uint16_t>(u).first(n));
  return out;
}

bool is_standard(const Value& v) {
  return v.is_string() && base::js_to_lower(v.as_string()) == "standard";
}

Value pattern_of(const doc::Doc& doc, const std::string& name) {
  return doc::pattern_of(doc, name);
}

}  // namespace cc::save::detail
