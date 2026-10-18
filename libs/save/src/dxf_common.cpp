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

const std::vector<std::pair<std::string, std::vector<double>>>& standard_linetypes() {
  static const auto kV = [] {
    const std::vector<std::pair<std::string, std::vector<double>>> base_lt = {
        {"DASHED", {0.5, -0.25}},
        {"HIDDEN", {0.25, -0.125}},
        {"CENTER", {1.25, -0.25, 0.25, -0.25}},
        {"PHANTOM", {1.25, -0.25, 0.25, -0.25, 0.25, -0.25}},
        {"DOT", {0, -0.25}},
        {"DASHDOT", {0.5, -0.25, 0, -0.25}},
        {"BORDER", {0.5, -0.25, 0.5, -0.25, 0, -0.25}},
        {"DIVIDE", {0.5, -0.25, 0, -0.25, 0, -0.25}},
    };
    std::vector<std::pair<std::string, std::vector<double>>> out;
    const auto X = [](double k, const std::vector<double>& p) {
      std::vector<double> o;
      o.reserve(p.size());
      for (const double v : p) {
        o.push_back(v * k);
      }
      return o;
    };
    for (const auto& [n, p] : base_lt) {
      out.emplace_back(n, p);
      out.emplace_back(n + "2", X(0.5, p));
      out.emplace_back(n + "X2", X(2, p));
    }
    return out;
  }();
  return kV;
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
  const std::string low = base::js_to_lower(name);
  if (name.empty() || low == "continuous" || low == "bylayer" || low == "byblock") {
    return {nullptr};
  }
  const Value* own = doc.linetypes.get(name);
  if (own == nullptr) {
    const std::string up = base::js_to_upper(name);
    doc.linetypes.each([&](const std::string&, const Value& l) {
      if (own == nullptr && base::js_to_upper(base::js_string(l.get("name"))) == up) {
        own = &l;
      }
    });
  }
  if (own != nullptr) {
    const Value p = own->get("pattern");
    bool ok = p.is_array() && p.as_array().size() >= 2;
    double sum = 0;
    bool neg = false;
    if (ok) {
      for (const Value& v : p.as_array()) {
        ok = ok && v.is_number() && std::isfinite(v.as_number());
        sum += std::abs(base::js_to_number(v));
        neg = neg || base::js_to_number(v) < 0;
      }
    }
    return ok && sum > 1e-12 && neg ? p : Value(nullptr);
  }
  const std::string up = base::js_to_upper(name);
  for (const auto& [n, pat] : standard_linetypes()) {
    if (n == up) {
      const bool metric = cad::hatch_is_metric(Value::object(), doc.meta);
      base::Array out;
      for (const double v : pat) {
        out.emplace_back(metric ? v * 25.4 : v);
      }
      return out;
    }
  }
  return {nullptr};
}

}  // namespace cc::save::detail
