// SPDX-License-Identifier: Apache-2.0
// A JSON-shaped value with JavaScript's distinctions: undefined (dropped when written, like JSON.stringify), null,
// boolean, number (always a double, as in JS), string (UTF-8; lone UTF-16 surrogates kept as WTF-8), array, object
// (insertion-ordered keys; assigning an existing key keeps its place, as in JS) and bytes.
//
// The ported engines build their models with it so the parity gate can compare them field for field with the
// JavaScript reference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cc::base {

struct Undefined {
  friend bool operator==(Undefined, Undefined) = default;
};

class Value;
struct Member;
using Array = std::vector<Value>;
using Object = std::vector<Member>;
using ByteVec = std::vector<std::uint8_t>;

class Value {
 public:
  enum class Kind : std::uint8_t { undefined, null, boolean, number, string, array, object, bytes };

  Value() noexcept = default;
  Value(std::nullptr_t) noexcept;  // NOLINT(google-explicit-constructor)
  Value(bool b) noexcept;          // NOLINT(google-explicit-constructor)
  Value(double d) noexcept;        // NOLINT(google-explicit-constructor)
  /// Any integer becomes a number, as in JS (every integer type, so size_t works on 32- and 64-bit targets).
  template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  Value(T i) noexcept;            // NOLINT(google-explicit-constructor)
  Value(std::string s) noexcept;  // NOLINT(google-explicit-constructor)
  Value(std::string_view s);      // NOLINT(google-explicit-constructor)
  Value(const char* s);           // NOLINT(google-explicit-constructor)
  Value(Array a) noexcept;        // NOLINT(google-explicit-constructor)
  Value(Object o) noexcept;       // NOLINT(google-explicit-constructor)
  Value(ByteVec b) noexcept;      // NOLINT(google-explicit-constructor)

  static Value object();
  static Value array();

  [[nodiscard]] Kind kind() const noexcept;
  [[nodiscard]] bool is_undefined() const noexcept;
  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;
  [[nodiscard]] bool is_bytes() const noexcept;

  /// JavaScript truthiness (undefined, null, false, 0, NaN, "" are falsy; arrays/objects/bytes are truthy).
  [[nodiscard]] bool truthy() const noexcept;

  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] double as_number() const;
  /// The number, or `def` when this is not a number.
  [[nodiscard]] double num_or(double def) const noexcept;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] std::string& as_string();
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] Array& as_array();
  [[nodiscard]] const Object& as_object() const;
  [[nodiscard]] Object& as_object();
  [[nodiscard]] const ByteVec& as_bytes() const;

  // ---- object helpers (this must be an object)
  /// Sets `key`; an existing key keeps its place (JS assignment).
  Value& set(std::string_view key, Value v);
  /// Appends a member without looking for an existing one: the caller knows `key` is new.
  Value& append(std::string_view key, Value v);
  /// JS Object.assign(this, other): each member of `other` is set (an existing key keeps its place). `other` is
  /// consumed. Keys of `other` that `this` cannot already hold may skip the search: `known` lists this object's
  /// keys that `other` might repeat (nullptr = search every key).
  void assign_from(Value other, std::span<const std::string_view> known);
  /// The member's value, or nullptr when the key is absent (an undefined member is present).
  [[nodiscard]] const Value* find(std::string_view key) const;
  [[nodiscard]] Value* find(std::string_view key);
  /// The member's value, or a shared undefined when absent.
  [[nodiscard]] const Value& operator[](std::string_view key) const;
  [[nodiscard]] bool has(std::string_view key) const;
  /// JS `delete o[key]`.
  void erase(std::string_view key);

  // ---- array helpers (this must be an array)
  void push(Value v);

  friend bool operator==(const Value& a, const Value& b);  // defined once Member is complete

 private:
  std::variant<Undefined, std::nullptr_t, bool, double, std::string, Array, Object, ByteVec> v_;
};

struct Member {
  std::string key;
  Value value;
  friend bool operator==(const Member& a, const Member& b) = default;
};

inline bool operator==(const Value& a, const Value& b) {
  return a.v_ == b.v_;
}

inline Value::Value(std::nullptr_t) noexcept : v_(nullptr) {}
inline Value::Value(bool b) noexcept : v_(b) {}
inline Value::Value(double d) noexcept : v_(d) {}
template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
inline Value::Value(T i) noexcept : v_(static_cast<double>(i)) {}
inline Value::Value(std::string s) noexcept : v_(std::move(s)) {}
inline Value::Value(std::string_view s) : v_(std::string(s)) {}
inline Value::Value(const char* s) : v_(std::string(s)) {}
inline Value::Value(Array a) noexcept : v_(std::move(a)) {}
inline Value::Value(Object o) noexcept : v_(std::move(o)) {}
inline Value::Value(ByteVec b) noexcept : v_(std::move(b)) {}
inline Value Value::object() {
  return {Object{}};
}
inline Value Value::array() {
  return {Array{}};
}
inline Value::Kind Value::kind() const noexcept {
  return static_cast<Kind>(v_.index());
}
inline bool Value::is_undefined() const noexcept {
  return kind() == Kind::undefined;
}
inline bool Value::is_null() const noexcept {
  return kind() == Kind::null;
}
inline bool Value::is_bool() const noexcept {
  return kind() == Kind::boolean;
}
inline bool Value::is_number() const noexcept {
  return kind() == Kind::number;
}
inline bool Value::is_string() const noexcept {
  return kind() == Kind::string;
}
inline bool Value::is_array() const noexcept {
  return kind() == Kind::array;
}
inline bool Value::is_object() const noexcept {
  return kind() == Kind::object;
}
inline bool Value::is_bytes() const noexcept {
  return kind() == Kind::bytes;
}
inline bool Value::as_bool() const {
  return std::get<bool>(v_);
}
inline double Value::as_number() const {
  return std::get<double>(v_);
}
inline const std::string& Value::as_string() const {
  return std::get<std::string>(v_);
}
inline std::string& Value::as_string() {
  return std::get<std::string>(v_);
}
inline const Array& Value::as_array() const {
  return std::get<Array>(v_);
}
inline Array& Value::as_array() {
  return std::get<Array>(v_);
}
inline const Object& Value::as_object() const {
  return std::get<Object>(v_);
}
inline Object& Value::as_object() {
  return std::get<Object>(v_);
}
inline const ByteVec& Value::as_bytes() const {
  return std::get<ByteVec>(v_);
}
inline bool Value::has(std::string_view key) const {
  return find(key) != nullptr;
}
inline void Value::push(Value v) {
  as_array().push_back(std::move(v));
}
inline double Value::num_or(double def) const noexcept {
  const double* d = std::get_if<double>(&v_);
  return d != nullptr ? *d : def;
}

/// Builds an object from {key, value} pairs in order.
Value make_object(std::initializer_list<std::pair<std::string_view, Value>> members);

/// A point {x, y} or {x, y, z} (built in place: points are the most common values of a drawing model).
inline Value pt2(double x, double y) {
  Object o;
  o.reserve(2);
  o.push_back(Member{"x", Value(x)});
  o.push_back(Member{"y", Value(y)});
  return {std::move(o)};
}
inline Value pt3(double x, double y, double z) {
  Object o;
  o.reserve(3);
  o.push_back(Member{"x", Value(x)});
  o.push_back(Member{"y", Value(y)});
  o.push_back(Member{"z", Value(z)});
  return {std::move(o)};
}
/// An empty object with room for n members.
inline Value object_with_capacity(std::size_t n) {
  Object o;
  o.reserve(n);
  return {std::move(o)};
}

// ---- JSON (the parity kit's form: NaN/±Infinity as "$NaN"/"$Inf"/"$-Inf", bytes as {"$base64": "…"}, -0 as 0,
// undefined members dropped, undefined array items as null, lone surrogates as \uXXXX escapes)
void write_json(const Value& v, std::string& out);
[[nodiscard]] std::string to_json(const Value& v);

/// Appends the shortest decimal that reads back as `d` (JS Number#toString digits; exponent form for |d| ≥ 1e21
/// or < 1e-6).
void append_number(std::string& out, double d);

// ---- text helpers
[[nodiscard]] std::string base64(std::span<const std::uint8_t> bytes);
/// UTF-16 code units → UTF-8; valid pairs combine, a lone surrogate becomes its 3-byte WTF-8 form.
void append_utf16(std::string& out, std::span<const std::uint16_t> units);
/// Appends one code point (0..0x10FFFF, surrogates allowed as WTF-8).
void append_code_point(std::string& out, std::uint32_t cp);
/// Upper-cases ASCII and Latin-1 letters (enough for the name comparisons the readers make).
[[nodiscard]] std::string to_upper(std::string_view s);
/// Hex digits of n, upper case, no leading zeros ("0" for 0) — JS n.toString(16).toUpperCase().
[[nodiscard]] std::string hex_upper(std::uint64_t n);

}  // namespace cc::base
