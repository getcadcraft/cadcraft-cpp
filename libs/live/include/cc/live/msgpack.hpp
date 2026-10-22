// SPDX-License-Identifier: Apache-2.0
// cc::live — L2. Studio Live's MessagePack (docs/LIVE-PROTOCOL.md § 2), as CadCraft 0.5's live-client.js `pack` /
// `unpack` write and read it, so both sides produce the bytes of tools/live-test/vectors.json.
//
// Values follow the JS ones: a number is a double (as in JS); an integer outside JS's safe range is a BigInt, kept here
// as `Big` (sign + 64-bit magnitude); bin is bytes; a map is a JS plain object — string keys (others turned into
// strings), keys that are array indices ("0", "7", …) first in ascending order, the others in the order they came, a
// repeated key keeps its first place with the last value, `__proto__` dropped. `Map::set` keeps the same order, so a
// map built here writes the bytes the JS object would. An ext value decodes as JS reads it: `{ext, data}`.
//
// Writing (live-client.js Writer): nil, booleans, an integer-valued double in the safe range as the smallest int
// encoding (positive: fixint … uint32, then uint64; negative: fixint … int32, then int64), any other double as float64,
// strings as fixstr / str8 / str16 / str32 by UTF-8 length, bin8/16/32, arrays and maps by count.
// Reading refuses truncated input, trailing bytes, the type byte 0xc1, nesting deeper than 512, and a count of more
// elements than bytes are left (before anything is allocated).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cc/base/error.hpp"

namespace cc::live {

class Mp;
struct MapEntry;

/// An integer outside ±(2^53 − 1): a JS BigInt. `neg` with `mag` = |value| (≤ 2^63 when negative).
struct Big {
  bool neg = false;
  std::uint64_t mag = 0;
  friend bool operator==(const Big&, const Big&) = default;
};

/// A JS plain object (see the top of the file).
class Map {
 public:
  using Entry = MapEntry;
  /// Sets `key` as a JS assignment would: an existing key keeps its place; a new array-index key goes among the index
  /// keys in ascending order, any other new key last. Returns the value set.
  Mp& set(std::string_view key, Mp v);
  [[nodiscard]] const Mp* find(std::string_view key) const;
  [[nodiscard]] Mp* find(std::string_view key);
  void erase(std::string_view key);
  [[nodiscard]] std::span<const Entry> entries() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  friend bool operator==(const Map& a, const Map& b);

 private:
  std::vector<Entry> items_;
};

using MpArray = std::vector<Mp>;
using MpBytes = std::vector<std::uint8_t>;

class Mp {
 public:
  enum class Kind : std::uint8_t { nil, boolean, number, big, string, bytes, array, map };

  Mp() noexcept = default;
  Mp(std::nullptr_t) noexcept;  // NOLINT(google-explicit-constructor)
  Mp(bool b) noexcept;          // NOLINT(google-explicit-constructor)
  Mp(double d) noexcept;        // NOLINT(google-explicit-constructor)
  /// Any integer: the JS number it is; past ±(2^53 − 1) a Big (what JS code would have to write).
  template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  Mp(T i) noexcept;  // NOLINT(google-explicit-constructor)
  [[nodiscard]] static Mp from_signed(std::int64_t i) noexcept;
  [[nodiscard]] static Mp from_unsigned(std::uint64_t u) noexcept;
  Mp(Big b) noexcept;          // NOLINT(google-explicit-constructor)
  Mp(std::string s) noexcept;  // NOLINT(google-explicit-constructor)
  Mp(std::string_view s);      // NOLINT(google-explicit-constructor)
  Mp(const char* s);           // NOLINT(google-explicit-constructor)
  Mp(MpBytes b) noexcept;      // NOLINT(google-explicit-constructor)
  Mp(MpArray a) noexcept;      // NOLINT(google-explicit-constructor)
  Mp(Map m) noexcept;          // NOLINT(google-explicit-constructor)

  [[nodiscard]] Kind kind() const noexcept;
  [[nodiscard]] bool is_nil() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_big() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_bytes() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_map() const noexcept;

  /// Throw std::bad_variant_access on the wrong kind (a caller's bug; check kind() first on input).
  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] double as_number() const;
  [[nodiscard]] const Big& as_big() const;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const MpBytes& as_bytes() const;
  [[nodiscard]] const MpArray& as_array() const;
  [[nodiscard]] MpArray& as_array();
  [[nodiscard]] const Map& as_map() const;
  [[nodiscard]] Map& as_map();

  /// A map member, or nullptr (also when this is not a map).
  [[nodiscard]] const Mp* get(std::string_view key) const;
  /// A non-negative integer (a number that is one, ≤ 2^53 − 1, or a non-negative Big) into `out`; false otherwise.
  [[nodiscard]] bool to_uint(std::uint64_t& out) const noexcept;
  /// The string, or `def` when this is not one.
  [[nodiscard]] std::string_view str_or(std::string_view def) const noexcept;

  friend bool operator==(const Mp&, const Mp&) = default;

 private:
  std::variant<std::monostate, bool, double, Big, std::string, MpBytes, MpArray, Map> v_;
};

struct MapEntry {
  std::string first;
  Mp second;
  friend bool operator==(const MapEntry&, const MapEntry&) = default;
};

template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
inline Mp::Mp(T i) noexcept
    : Mp(std::is_signed_v<T> ? from_signed(static_cast<std::int64_t>(i))
                             : from_unsigned(static_cast<std::uint64_t>(i))) {}
inline Mp::Mp(std::nullptr_t) noexcept {}
inline Mp::Mp(bool b) noexcept : v_(b) {}
inline Mp::Mp(double d) noexcept : v_(d) {}
inline Mp::Mp(Big b) noexcept : v_(b) {}
inline Mp::Mp(std::string s) noexcept : v_(std::move(s)) {}
inline Mp::Mp(std::string_view s) : v_(std::string(s)) {}
inline Mp::Mp(const char* s) : v_(std::string(s)) {}
inline Mp::Mp(MpBytes b) noexcept : v_(std::move(b)) {}
inline Mp::Mp(MpArray a) noexcept : v_(std::move(a)) {}
inline Mp::Mp(Map m) noexcept : v_(std::move(m)) {}
inline Mp::Kind Mp::kind() const noexcept {
  return static_cast<Kind>(v_.index());
}
inline bool Mp::is_nil() const noexcept {
  return kind() == Kind::nil;
}
inline bool Mp::is_bool() const noexcept {
  return kind() == Kind::boolean;
}
inline bool Mp::is_number() const noexcept {
  return kind() == Kind::number;
}
inline bool Mp::is_big() const noexcept {
  return kind() == Kind::big;
}
inline bool Mp::is_string() const noexcept {
  return kind() == Kind::string;
}
inline bool Mp::is_bytes() const noexcept {
  return kind() == Kind::bytes;
}
inline bool Mp::is_array() const noexcept {
  return kind() == Kind::array;
}
inline bool Mp::is_map() const noexcept {
  return kind() == Kind::map;
}
inline bool Mp::as_bool() const {
  return std::get<bool>(v_);
}
inline double Mp::as_number() const {
  return std::get<double>(v_);
}
inline const Big& Mp::as_big() const {
  return std::get<Big>(v_);
}
inline const std::string& Mp::as_string() const {
  return std::get<std::string>(v_);
}
inline const MpBytes& Mp::as_bytes() const {
  return std::get<MpBytes>(v_);
}
inline const MpArray& Mp::as_array() const {
  return std::get<MpArray>(v_);
}
inline MpArray& Mp::as_array() {
  return std::get<MpArray>(v_);
}
inline const Map& Mp::as_map() const {
  return std::get<Map>(v_);
}
inline Map& Mp::as_map() {
  return std::get<Map>(v_);
}
inline std::span<const MapEntry> Map::entries() const noexcept {
  return items_;
}
inline std::size_t Map::size() const noexcept {
  return items_.size();
}
inline bool Map::empty() const noexcept {
  return items_.empty();
}

/// live-client.js pack(value).
[[nodiscard]] MpBytes pack(const Mp& value);
/// Appends pack(value) to `out`.
void pack_into(const Mp& value, MpBytes& out);
/// live-client.js unpack(bytes): exactly one value, no trailing bytes.
[[nodiscard]] base::Result<Mp> unpack(std::span<const std::uint8_t> bytes);

/// Is `key` a JS array index ("0" … "4294967294", no leading zeros)? Such object keys come first, in number order.
[[nodiscard]] bool is_array_index(std::string_view key) noexcept;

}  // namespace cc::live
