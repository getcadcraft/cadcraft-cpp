// SPDX-License-Identifier: Apache-2.0
// The JavaScript semantics PartCraft's ported modules lean on, over base::Value: String(x ?? ''), the control-character
// strip the readers apply, String#slice in UTF-16 units, object spread and Object.entries in property order (index keys
// first), a property read on any value, and Math.max with NaN.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/value.hpp"

namespace cc::partcraft::jsx {

/// String(v ?? '')
[[nodiscard]] inline std::string text_of(const base::Value& v) {
  return v.is_undefined() || v.is_null() ? std::string() : base::js_string(v);
}
/// s.replace(/[\u0000-\u001f\u007f]/g, '')
[[nodiscard]] inline std::string strip_control(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7f) {
      out += c;
    }
  }
  return out;
}
/// s.slice(0, n) in UTF-16 code units (a pair cut in half keeps its lead surrogate, as WTF-8).
[[nodiscard]] inline std::string utf16_slice(std::string_view s, std::size_t n) {
  if (base::utf16_length(s) <= n) {
    return std::string(s);
  }
  const std::vector<std::uint16_t> units = base::to_utf16(s);
  std::string out;
  base::append_utf16(out, std::span(units).first(n));
  return out;
}
/// Object.entries(o): own members in property order (index keys first); none for a non-object.
[[nodiscard]] inline std::vector<std::pair<std::string, base::Value>> entries(const base::Value& o) {
  std::vector<std::pair<std::string, base::Value>> out;
  if (o.is_array()) {
    const auto& a = o.as_array();
    for (std::size_t i = 0; i < a.size(); ++i) {
      out.emplace_back(std::to_string(i), a[i]);
    }
    return out;
  }
  if (!o.is_object()) {
    return out;
  }
  const base::Value* src = &o;
  base::Value expanded;  // a compact point expands
  if (o.is_point()) {
    expanded = o;
    (void)expanded.as_object();
    src = &expanded;
  }
  for (const base::Member* m : base::js_members(src->as_object())) {
    out.emplace_back(m->key, m->value);
  }
  return out;
}
/// { ...o }: a plain copy in property order.
[[nodiscard]] inline base::Value spread(const base::Value& o) {
  base::Value out = base::Value::object();
  for (auto& [k, v] : entries(o)) {
    out.append(k, std::move(v));
  }
  return out;
}
/// o[key] for an object (undefined when absent or not an object).
[[nodiscard]] inline base::Value get(const base::Value& o, std::string_view key) {
  return o.is_object() ? o.get(key) : base::Value{};
}
/// Math.max(a, b): NaN when either is.
[[nodiscard]] inline double max(double a, double b) noexcept {
  if (std::isnan(a) || std::isnan(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return a > b ? a : b;
}
/// JSON.stringify(v, null, indent): JavaScript's property order, undefined members dropped, NaN / ±Infinity as null,
/// `indent` spaces per level (0: compact, as js_stringify).
// NOLINTNEXTLINE(misc-no-recursion) — bounded by the value's depth
inline void stringify_to(const base::Value& v, std::size_t indent, std::size_t level, std::string& out) {
  using K = base::Value::Kind;
  switch (v.kind()) {
    case K::undefined:
    case K::null: out += "null"; return;
    case K::boolean: out += v.as_bool() ? "true" : "false"; return;
    case K::number:
      if (!std::isfinite(v.as_number())) {
        out += "null";
      } else {
        base::append_number(out, v.as_number());
      }
      return;
    case K::string: base::append_json_string(out, v.as_string()); return;
    case K::bytes: out += "{}"; return;
    case K::array: {
      const auto& a = v.as_array();
      if (a.empty()) {
        out += "[]";
        return;
      }
      out += '[';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i > 0) {
          out += ',';
        }
        if (indent > 0) {
          out += '\n';
          out.append(indent * (level + 1), ' ');
        }
        stringify_to(a[i], indent, level + 1, out);
      }
      if (indent > 0) {
        out += '\n';
        out.append(indent * level, ' ');
      }
      out += ']';
      return;
    }
    case K::object: {
      const base::Value* src = &v;
      base::Value expanded;  // a compact point expands
      if (v.is_point()) {
        expanded = v;
        (void)expanded.as_object();
        src = &expanded;
      }
      bool first = true;
      out += '{';
      for (const base::Member* m : base::js_members(src->as_object())) {
        if (m->value.is_undefined()) {
          continue;
        }
        if (!first) {
          out += ',';
        }
        first = false;
        if (indent > 0) {
          out += '\n';
          out.append(indent * (level + 1), ' ');
        }
        base::append_json_string(out, m->key);
        out += indent > 0 ? ": " : ":";
        stringify_to(m->value, indent, level + 1, out);
      }
      if (!first && indent > 0) {
        out += '\n';
        out.append(indent * level, ' ');
      }
      out += '}';
      return;
    }
  }
}
[[nodiscard]] inline std::string stringify(const base::Value& v, std::size_t indent) {
  std::string out;
  stringify_to(v, indent, 0, out);
  return out;
}

}  // namespace cc::partcraft::jsx
