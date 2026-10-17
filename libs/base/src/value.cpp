// SPDX-License-Identifier: Apache-2.0
#include "cc/base/value.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include "cc/base/js.hpp"

namespace cc::base {

bool Value::truthy() const noexcept {
  switch (kind()) {
    case Kind::undefined:
    case Kind::null: return false;
    case Kind::boolean: return *std::get_if<bool>(&v_);
    case Kind::number: {
      const double d = *std::get_if<double>(&v_);
      return d != 0.0 && !std::isnan(d);
    }
    case Kind::string: return !std::get_if<std::string>(&v_)->empty();
    default: return true;
  }
}

namespace {

[[noreturn]] void point_access(std::string_view what) {
  throw std::logic_error(std::string(what) + " on a compact point: read it with get()");
}

/// x → 0, y → 1, z → 2, anything else → -1.
int axis(std::string_view key) noexcept {
  if (key.size() != 1) {
    return -1;
  }
  switch (key[0]) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    default: return -1;
  }
}

}  // namespace

void Value::expand() {
  if (const Pt2* p = std::get_if<Pt2>(&v_)) {
    Object o;
    o.reserve(2);
    o.push_back(Member{"x", Value(p->x)});
    o.push_back(Member{"y", Value(p->y)});
    v_ = std::move(o);
  } else if (const Pt3* q = std::get_if<Pt3>(&v_)) {
    Object o;
    o.reserve(3);
    o.push_back(Member{"x", Value(q->x)});
    o.push_back(Member{"y", Value(q->y)});
    o.push_back(Member{"z", Value(q->z)});
    v_ = std::move(o);
  }
}

bool Value::same_point(const Value& point, const Value& other) {
  std::array<double, 3> a{};
  std::size_t n = 0;
  if (const Pt2* p = std::get_if<Pt2>(&point.v_)) {
    a = {p->x, p->y, 0};
    n = 2;
  } else {
    const Pt3& q = std::get<Pt3>(point.v_);
    a = {q.x, q.y, q.z};
    n = 3;
  }
  if (other.is_point()) {
    const Pt2* p = std::get_if<Pt2>(&other.v_);
    if ((p != nullptr) != (n == 2)) {
      return false;
    }
    if (p != nullptr) {
      return a[0] == p->x && a[1] == p->y;
    }
    const Pt3& q = std::get<Pt3>(other.v_);
    return a[0] == q.x && a[1] == q.y && a[2] == q.z;
  }
  const Object* o = std::get_if<Object>(&other.v_);
  if (o == nullptr || o->size() != n) {
    return false;
  }
  static constexpr std::array<std::string_view, 3> kKeys = {"x", "y", "z"};
  for (std::size_t i = 0; i < n; ++i) {
    const Member& m = (*o)[i];
    if (m.key != kKeys[i] || !m.value.is_number() || m.value.as_number() != a[i]) {
      return false;
    }
  }
  return true;
}

const Object& Value::as_object() const {
  if (is_point()) {
    point_access("as_object() const");
  }
  return std::get<Object>(v_);
}

Value Value::get(std::string_view key) const {
  if (const Pt2* p = std::get_if<Pt2>(&v_)) {
    switch (axis(key)) {
      case 0: return p->x;
      case 1: return p->y;
      default: return {};
    }
  }
  if (const Pt3* q = std::get_if<Pt3>(&v_)) {
    switch (axis(key)) {
      case 0: return q->x;
      case 1: return q->y;
      case 2: return q->z;
      default: return {};
    }
  }
  if (!is_object()) {
    return {};
  }
  const Value* v = find(key);
  return v != nullptr ? *v : Value();
}

bool Value::has(std::string_view key) const {
  if (std::holds_alternative<Pt2>(v_)) {
    const int a = axis(key);
    return a == 0 || a == 1;
  }
  if (std::holds_alternative<Pt3>(v_)) {
    return axis(key) >= 0;
  }
  return find(key) != nullptr;
}

Value& Value::set(std::string_view key, Value v) {
  if (is_point() && v.is_number()) {  // x / y / z to a number: stays compact (z turns a 2D point into a 3D one)
    const double d = v.as_number();
    if (Pt2* p = std::get_if<Pt2>(&v_)) {
      switch (axis(key)) {
        case 0: p->x = d; return *this;
        case 1: p->y = d; return *this;
        case 2: v_ = Pt3{p->x, p->y, d}; return *this;
        default: break;
      }
    } else if (Pt3* q = std::get_if<Pt3>(&v_)) {
      switch (axis(key)) {
        case 0: q->x = d; return *this;
        case 1: q->y = d; return *this;
        case 2: q->z = d; return *this;
        default: break;
      }
    }
  }
  Object& o = as_object();
  for (Member& m : o) {
    if (m.key == key) {
      m.value = std::move(v);
      return m.value;
    }
  }
  o.push_back(Member{std::string(key), std::move(v)});
  return o.back().value;
}

Value& Value::append(std::string_view key, Value v) {
  Object& o = as_object();  // a compact point is expanded
  o.push_back(Member{std::string(key), std::move(v)});
  return o.back().value;
}

void Value::assign_from(Value other, std::span<const std::string_view> known) {
  Object& src = other.as_object();  // compact points are expanded
  Object& dst = as_object();
  dst.reserve(dst.size() + src.size());
  for (Member& m : src) {
    bool maybe = false;
    for (const std::string_view k : known) {
      if (k == m.key) {
        maybe = true;
        break;
      }
    }
    if (maybe) {
      set(m.key, std::move(m.value));
    } else {
      dst.push_back(Member{std::move(m.key), std::move(m.value)});
    }
  }
  src.clear();
}

const Value* Value::find(std::string_view key) const {
  if (is_point()) {
    point_access("find()");
  }
  for (const Member& m : std::get<Object>(v_)) {
    if (m.key == key) {
      return &m.value;
    }
  }
  return nullptr;
}

Value* Value::find(std::string_view key) {
  for (Member& m : as_object()) {  // a compact point is expanded
    if (m.key == key) {
      return &m.value;
    }
  }
  return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
  static const Value kUndefined;
  if (!is_object()) {
    return kUndefined;
  }
  if (is_point()) {
    point_access("operator[]");
  }
  const Value* v = find(key);
  return v != nullptr ? *v : kUndefined;
}

void Value::erase(std::string_view key) {
  Object& o = as_object();  // a compact point is expanded
  std::erase_if(o, [&](const Member& m) { return m.key == key; });
}

Value make_object(std::initializer_list<Init> members) {
  // {x, y} / {x, y, z} of numbers: a compact point
  if (members.size() == 2 || members.size() == 3) {
    static constexpr std::array<std::string_view, 3> kKeys = {"x", "y", "z"};
    bool point = true;
    std::size_t i = 0;
    for (const Init& m : members) {
      point = point && m.key == kKeys[i] && m.value.is_number();
      ++i;
    }
    if (point) {
      const auto* it = members.begin();
      const double x = it->value.as_number();
      const double y = std::next(it)->value.as_number();
      return members.size() == 2 ? pt2(x, y) : pt3(x, y, std::next(it, 2)->value.as_number());
    }
  }
  Object o;
  o.reserve(members.size());
  for (const Init& m : members) {
    o.push_back(Member{std::string(m.key), std::move(m.value)});
  }
  return {std::move(o)};
}

// ---------------------------------------------------------------- numbers
void append_number(std::string& out, double d) {
  if (std::isnan(d)) {  // String(NaN), String(±Infinity)
    out += "NaN";
    return;
  }
  if (std::isinf(d)) {
    out += d > 0 ? "Infinity" : "-Infinity";
    return;
  }
  if (d == 0.0) {
    out += '0';
    return;
  }
  std::array<char, 32> buf{};
  // Shortest round-trip digits in scientific form: d.ddddde±x.
  const std::span<char> room(buf);
  const auto res = std::to_chars(room.data(), std::to_address(room.end()), d, std::chars_format::scientific);
  const std::string_view sci(room.data(), static_cast<std::size_t>(std::distance(room.data(), res.ptr)));
  std::size_t i = 0;
  const bool neg = sci[0] == '-';
  if (neg) {
    i = 1;
  }
  const std::size_t e = sci.find('e');
  std::string digits;
  for (std::size_t k = i; k < e; ++k) {
    if (sci[k] != '.') {
      digits += sci[k];
    }
  }
  int exp10 = 0;
  std::string_view tail = sci.substr(e + 1);
  if (!tail.empty() && tail.front() == '+') {
    tail.remove_prefix(1);
  }
  const std::span<const char> tail_chars(tail);
  std::from_chars(tail_chars.data(), std::to_address(tail_chars.end()), exp10);
  // JS Number#toString: n = exponent + 1 (position of the decimal point), k = digit count.
  const int k = static_cast<int>(digits.size());
  const int n = exp10 + 1;
  if (neg) {
    out += '-';
  }
  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<std::size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out.append(digits, 0, static_cast<std::size_t>(n));
    out += '.';
    out.append(digits, static_cast<std::size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-n), '0');
    out += digits;
  } else {
    out += digits[0];
    if (k > 1) {
      out += '.';
      out.append(digits, 1);
    }
    out += 'e';
    out += n - 1 >= 0 ? '+' : '-';
    out += std::to_string(std::abs(n - 1));
  }
}

// ---------------------------------------------------------------- text
std::string base64(std::span<const std::uint8_t> b) {
  static constexpr std::string_view kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((b.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= b.size(); i += 3) {
    const std::uint32_t v = (std::uint32_t{b[i]} << 16U) | (std::uint32_t{b[i + 1]} << 8U) | b[i + 2];
    out += kAlpha[(v >> 18U) & 63U];
    out += kAlpha[(v >> 12U) & 63U];
    out += kAlpha[(v >> 6U) & 63U];
    out += kAlpha[v & 63U];
  }
  if (i < b.size()) {
    std::uint32_t v = std::uint32_t{b[i]} << 16U;
    if (i + 1 < b.size()) {
      v |= std::uint32_t{b[i + 1]} << 8U;
    }
    out += kAlpha[(v >> 18U) & 63U];
    out += kAlpha[(v >> 12U) & 63U];
    out += i + 1 < b.size() ? kAlpha[(v >> 6U) & 63U] : '=';
    out += '=';
  }
  return out;
}

void append_code_point(std::string& out, std::uint32_t cp) {
  if (cp < 0x80U) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800U) {
    out += static_cast<char>(0xC0U | (cp >> 6U));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  } else if (cp < 0x10000U) {
    out += static_cast<char>(0xE0U | (cp >> 12U));
    out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  } else {
    out += static_cast<char>(0xF0U | (cp >> 18U));
    out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (cp & 0x3FU));
  }
}

void append_utf16(std::string& out, std::span<const std::uint16_t> u) {
  for (std::size_t i = 0; i < u.size(); ++i) {
    const std::uint32_t c = u[i];
    if (c >= 0xD800U && c <= 0xDBFFU && i + 1 < u.size() && u[i + 1] >= 0xDC00U && u[i + 1] <= 0xDFFFU) {
      append_code_point(out, 0x10000U + ((c - 0xD800U) << 10U) + (u[i + 1] - 0xDC00U));
      ++i;
    } else {
      append_code_point(out, c);
    }
  }
}

std::string to_upper(std::string_view s) {
  return js_to_upper(s);
}

std::string hex_upper(std::uint64_t n) {
  if (n == 0) {
    return "0";
  }
  std::array<char, 16> b{};
  std::size_t k = b.size();
  while (n != 0) {
    b[--k] = "0123456789ABCDEF"[n & 15U];
    n >>= 4U;
  }
  return std::string(std::string_view(b.data(), b.size()).substr(k));
}

// ---------------------------------------------------------------- JSON
namespace {

void write_string(std::string& out, std::string_view s) {
  out += '"';
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\b': out += "\\b"; continue;
      case '\f': out += "\\f"; continue;
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      default: break;
    }
    if (c < 0x20U) {
      static constexpr std::string_view kHex = "0123456789abcdef";
      out += "\\u00";
      out += kHex[c >> 4U];
      out += kHex[c & 15U];
    } else if (c == 0xED && i + 2 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xE0U) == 0xA0U) {
      // A lone surrogate kept as WTF-8 (ED A0..BF xx): JSON.stringify writes it as an escape.
      const std::uint32_t cp =
          0xD000U | ((static_cast<unsigned>(s[i + 1]) & 0x3FU) << 6U) | (static_cast<unsigned>(s[i + 2]) & 0x3FU);
      static constexpr std::string_view kHex = "0123456789abcdef";
      out += "\\u";
      out += kHex[(cp >> 12U) & 15U];
      out += kHex[(cp >> 8U) & 15U];
      out += kHex[(cp >> 4U) & 15U];
      out += kHex[cp & 15U];
      i += 2;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
}

void write_number(std::string& out, double d) {
  if (std::isnan(d)) {
    out += "\"$NaN\"";
  } else if (std::isinf(d)) {
    out += d > 0 ? "\"$Inf\"" : "\"$-Inf\"";
  } else {
    append_number(out, d);
  }
}

// Recursion depth = the value's depth; the readers build models of bounded depth (their own limits).
// NOLINTNEXTLINE(misc-no-recursion)
void write_value(const Value& v, std::string& out) {
  if (v.is_point()) {
    out += "{\"x\":";
    write_number(out, v.get("x").as_number());
    out += ",\"y\":";
    write_number(out, v.get("y").as_number());
    if (v.has("z")) {
      out += ",\"z\":";
      write_number(out, v.get("z").as_number());
    }
    out += '}';
    return;
  }
  switch (v.kind()) {
    case Value::Kind::undefined:
    case Value::Kind::null: out += "null"; return;
    case Value::Kind::boolean: out += v.as_bool() ? "true" : "false"; return;
    case Value::Kind::number: write_number(out, v.as_number()); return;
    case Value::Kind::string: write_string(out, v.as_string()); return;
    case Value::Kind::array: {
      out += '[';
      bool first = true;
      for (const Value& x : v.as_array()) {
        if (!first) {
          out += ',';
        }
        first = false;
        write_value(x, out);
      }
      out += ']';
      return;
    }
    case Value::Kind::object: {
      out += '{';
      bool first = true;
      for (const Member& m : v.as_object()) {
        if (m.value.is_undefined()) {
          continue;
        }
        if (!first) {
          out += ',';
        }
        first = false;
        write_string(out, m.key);
        out += ':';
        write_value(m.value, out);
      }
      out += '}';
      return;
    }
    case Value::Kind::bytes:
      out += R"({"$base64":")";
      out += base64(v.as_bytes());
      out += "\"}";
      return;
  }
}

}  // namespace

void append_json_string(std::string& out, std::string_view s) {
  write_string(out, s);
}

void write_json(const Value& v, std::string& out) {
  write_value(v, out);
}

std::string to_json(const Value& v) {
  std::string s;
  write_value(v, s);
  return s;
}

}  // namespace cc::base
