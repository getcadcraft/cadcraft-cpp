// SPDX-License-Identifier: Apache-2.0
// A JSON-shaped value with JavaScript's distinctions: undefined (dropped when written, like JSON.stringify), null,
// boolean, number (always a double, as in JS), string (UTF-8; lone UTF-16 surrogates kept as WTF-8), array, object
// (insertion-ordered keys; assigning an existing key keeps its place, as in JS) and bytes.
//
// Points: an object that is exactly {x, y} or {x, y, z} with numbers (made by pt2/pt3, or make_object with those
// members) is stored inline as doubles, with no allocation; it still is an object to JS semantics, the JSON writer and
// ==. A model is mostly points, so this is most of its objects. The one difference is access: its members are not
// stored as Values, so read them with get() (by value); find(), operator[] and a const as_object() on a compact point
// throw std::logic_error rather than answer wrongly. Setting x/y/z to a number keeps it compact; any other change turns
// it into an ordinary object first (as_object() non-const, set of another key or a non-number, erase, append).
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
/// A compact {x, y} / {x, y, z} (see the top of this file).
struct Pt2 {
  double x = 0, y = 0;
};
struct Pt3 {
  double x = 0, y = 0, z = 0;
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
  Value(Pt2 p) noexcept;          // NOLINT(google-explicit-constructor)
  Value(Pt3 p) noexcept;          // NOLINT(google-explicit-constructor)

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
  /// A compact point (is_object() is true for it too).
  [[nodiscard]] bool is_point() const noexcept;

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
  /// Throws std::logic_error on a compact point (use get(), or the non-const as_object(), which expands it).
  [[nodiscard]] const Object& as_object() const;
  /// A compact point is expanded into an ordinary object first.
  [[nodiscard]] Object& as_object();
  [[nodiscard]] const ByteVec& as_bytes() const;

  // ---- object helpers (this must be an object)
  /// Sets `key`; an existing key keeps its place (JS assignment). Returns the member's value — except on a compact
  /// point given a number for x, y or z, where it stays compact and the point itself is returned.
  Value& set(std::string_view key, Value v);
  /// Appends a member without looking for an existing one: the caller knows `key` is new.
  Value& append(std::string_view key, Value v);
  /// JS Object.assign(this, other): each member of `other` is set (an existing key keeps its place). `other` is
  /// consumed. Keys of `other` that `this` cannot already hold may skip the search: `known` lists this object's
  /// keys that `other` might repeat (nullptr = search every key).
  void assign_from(Value other, std::span<const std::string_view> known);
  /// The member's value, or nullptr when the key is absent (an undefined member is present). Throws std::logic_error
  /// on a compact point (const); the non-const one expands the point.
  [[nodiscard]] const Value* find(std::string_view key) const;
  [[nodiscard]] Value* find(std::string_view key);
  /// The member's value, or a shared undefined when absent or this is not an object. Throws on a compact point.
  [[nodiscard]] const Value& operator[](std::string_view key) const;
  /// The member's value by value (undefined when absent or not an object): the way to read a point's x, y, z.
  [[nodiscard]] Value get(std::string_view key) const;
  [[nodiscard]] bool has(std::string_view key) const;
  /// JS `delete o[key]`.
  void erase(std::string_view key);

  // ---- array helpers (this must be an array)
  void push(Value v);

  friend bool operator==(const Value& a, const Value& b);  // JS-shaped: a compact point equals the same plain object

 private:
  void expand();  // a compact point → an ordinary object (no-op otherwise)
  [[nodiscard]] static bool same_point(const Value& point, const Value& other);
  // the first eight are the Kinds in order; Pt2 and Pt3 are objects
  std::variant<Undefined, std::nullptr_t, bool, double, std::string, Array, Object, ByteVec, Pt2, Pt3> v_;
};

struct Member {
  std::string key;
  Value value;
  friend bool operator==(const Member& a, const Member& b) = default;
};

inline bool operator==(const Value& a, const Value& b) {
  if (a.is_point() || b.is_point()) {
    return a.is_point() ? Value::same_point(a, b) : Value::same_point(b, a);
  }
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
inline Value::Value(Pt2 p) noexcept : v_(p) {}
inline Value::Value(Pt3 p) noexcept : v_(p) {}
inline Value Value::object() {
  return {Object{}};
}
inline Value Value::array() {
  return {Array{}};
}
inline Value::Kind Value::kind() const noexcept {
  const std::size_t i = v_.index();
  return i < 8 ? static_cast<Kind>(i) : Kind::object;
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
inline bool Value::is_point() const noexcept {
  return v_.index() >= 8;
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
inline Object& Value::as_object() {
  expand();
  return std::get<Object>(v_);
}
inline const ByteVec& Value::as_bytes() const {
  return std::get<ByteVec>(v_);
}
inline void Value::push(Value v) {
  as_array().push_back(std::move(v));
}
inline double Value::num_or(double def) const noexcept {
  const double* d = std::get_if<double>(&v_);
  return d != nullptr ? *d : def;
}

/// One {key, value} of a braced member list. An initializer_list's items are const, so a plain pair could only be
/// copied out; the value is `mutable` so make_object (and the readers' spreads) move it out instead: a temporary
/// argument is then moved twice and never copied (whole point and entity objects otherwise were, per member).
struct Init {
  Init(std::string_view k, Value v) noexcept : key(k), value(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  std::string_view key;
  mutable Value value;
};

/// Builds an object from {key, value} pairs in order (each value moved out of the list).
Value make_object(std::initializer_list<Init> members);

/// A point {x, y} or {x, y, z}: compact (no allocation; see the top of this file).
inline Value pt2(double x, double y) {
  return {Pt2{x, y}};
}
inline Value pt3(double x, double y, double z) {
  return {Pt3{x, y, z}};
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
/// Appends `s` as a JSON string literal, escaped as JSON.stringify escapes it.
void append_json_string(std::string& out, std::string_view s);

/// Appends the shortest decimal that reads back as `d` (JS Number#toString digits; exponent form for |d| ≥ 1e21
/// or < 1e-6).
void append_number(std::string& out, double d);

// ---- text helpers
[[nodiscard]] std::string base64(std::span<const std::uint8_t> bytes);
/// UTF-16 code units → UTF-8; valid pairs combine, a lone surrogate becomes its 3-byte WTF-8 form.
void append_utf16(std::string& out, std::span<const std::uint16_t> units);
/// Appends one code point (0..0x10FFFF, surrogates allowed as WTF-8).
void append_code_point(std::string& out, std::uint32_t cp);
/// String#toUpperCase (cc::base::js_to_upper).
[[nodiscard]] std::string to_upper(std::string_view s);
/// Hex digits of n, upper case, no leading zeros ("0" for 0) — JS n.toString(16).toUpperCase().
[[nodiscard]] std::string hex_upper(std::uint64_t n);
/// JS n.toString(16) for a non-negative integer: lower case, no leading zeros.
[[nodiscard]] std::string hex_lower(std::uint64_t n);

// ---- JS conversions of a whole value
/// String(v): numbers as Number#toString, arrays joined with "," (null / undefined items empty), objects
/// "[object Object]", bytes "" (not a JS value; never asked of the ports).
[[nodiscard]] std::string js_string(const Value& v);
/// Number(v): undefined NaN, null 0, booleans 0 / 1, strings through js_number, arrays through their String,
/// objects NaN.
[[nodiscard]] double js_to_number(const Value& v);
/// An object's members in JavaScript's property order (what Object.keys / values / entries and for…in give): keys
/// that are array indices ("0" … "4294967294", no leading zeros) first, ascending; then the others as inserted.
/// A Value object keeps insertion order only; ports iterate through this where the JS iterates an object.
[[nodiscard]] std::vector<const Member*> js_members(const Object& o);
/// True for a canonical array-index key.
[[nodiscard]] bool js_index_key(std::string_view k) noexcept;

}  // namespace cc::base
