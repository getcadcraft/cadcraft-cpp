// SPDX-License-Identifier: Apache-2.0
// cc-skp's shared pieces (lib/skp/util.js, messages.js): the error the readers raise, LIMITS, the bounds-checked
// little-endian Reader, the messages, the notes counter. Internal to libs/io-skp.
//
// Errors inside the readers are exceptions (as cc-skp's are), caught by read_skp and never let out of the library:
// `Fail` carries an Error (the JS SkpError), legacy.cpp's `Misparse` a record it could not make sense of, which the
// classic walk catches where the JS does. This is the second deliberate internal exception in the fork beside
// cc::base::ReadOverrun (docs/architecture.md, Errors): the JS decides which failures a walk may recover from by
// where it catches, and the port keeps those places exactly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/skp/skp.hpp"

namespace cc::skp::detail {

struct Fail {
  Error error;
};

/// Runs `f` when the scope ends (the JS's `finally`).
template <class F>
class ScopeExit {
 public:
  explicit ScopeExit(F f) : f_(std::move(f)) {}
  ~ScopeExit() { f_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit(ScopeExit&&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;

 private:
  F f_;
};

/// util.js LIMITS.
struct Limits {
  static constexpr std::uint64_t file_bytes = 1024ULL * 1024 * 1024;
  static constexpr std::uint64_t entry_bytes = 512ULL * 1024 * 1024;
  static constexpr std::uint64_t inflated_bytes = 2ULL * 1024 * 1024 * 1024;
  static constexpr std::uint64_t vertices = 5'000'000;
  static constexpr int depth = 64;
};

using Fills = std::initializer_list<std::pair<std::string_view, std::string>>;

/// messages.js msg(key, fills): the catalog's words when it has the key, else cc-skp's English. A plural message
/// picks `one` when the fill n is "1".
[[nodiscard]] std::string msg(const i18n::Catalog* cat, std::string_view key, Fills fills = {});

[[noreturn]] void fail(std::string code, std::string message);
/// SkpError('damaged', msg('ccskp.damaged', {reason: msg(key, fills)})).
[[noreturn]] void damaged(const i18n::Catalog* cat, std::string_view key, Fills fills = {});

/// util.js okNum: finite and within ±1e9 inches.
[[nodiscard]] inline bool ok_num(double x) noexcept {
  return std::isfinite(x) && std::fabs(x) < 1e9;
}

/// JS Number → String (for fills).
[[nodiscard]] std::string num_text(double x);
/// Number#toLocaleString('en-US') of a whole number: thousands with commas.
[[nodiscard]] std::string grouped(std::uint64_t n);

/// util.js Reader: bounds-checked little-endian reads over [pos, end); a read past end raises
/// SkpError damaged (past_end).
class Reader {
 public:
  Reader(std::span<const std::uint8_t> b, std::size_t pos, const i18n::Catalog* cat)
      : b_(b), pos(pos), end(b.size()), cat_(cat) {}
  Reader(std::span<const std::uint8_t> b, std::size_t pos, std::size_t end, const i18n::Catalog* cat)
      : b_(b), pos(pos), end(std::min(end, b.size())), cat_(cat) {}

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return b_; }
  [[nodiscard]] std::size_t left() const noexcept { return end > pos ? end - pos : 0; }
  void need(std::size_t n) const {
    if (pos > end || n > end - pos) {
      damaged(cat_, "ccskp.damaged.past_end");
    }
  }
  void skip(std::size_t n) {
    need(n);
    pos += n;
  }
  std::uint8_t u8() {
    need(1);
    return b_[pos++];
  }
  std::uint16_t u16() {
    need(2);
    const auto v = static_cast<std::uint16_t>(b_[pos] | (b_[pos + 1] << 8U));
    pos += 2;
    return v;
  }
  std::uint32_t u32() {
    need(4);
    const std::uint32_t v = static_cast<std::uint32_t>(b_[pos]) | (static_cast<std::uint32_t>(b_[pos + 1]) << 8U) |
                            (static_cast<std::uint32_t>(b_[pos + 2]) << 16U) |
                            (static_cast<std::uint32_t>(b_[pos + 3]) << 24U);
    pos += 4;
    return v;
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  double f64() {
    need(8);
    const double v = f64_at(b_, pos);
    pos += 8;
    return v;
  }
  std::span<const std::uint8_t> take(std::size_t n) {
    need(n);
    const auto v = b_.subspan(pos, n);
    pos += n;
    return v;
  }
  /// UTF-16LE text of n code units (as JS keeps it; lone surrogates as WTF-8).
  std::string utf16(std::size_t n);
  std::string ascii(std::size_t n);

  [[nodiscard]] static double f64_at(std::span<const std::uint8_t> b, std::size_t p) noexcept;

 private:
  std::span<const std::uint8_t> b_;

 public:
  std::size_t pos;
  std::size_t end;

 private:
  const i18n::Catalog* cat_;
};

/// vff.js Notes: a count per kind of thing skipped (and its fills), listed in first-seen order.
class Notes {
 public:
  void add(std::string_view key, std::uint64_t n = 1, std::vector<std::pair<std::string, std::string>> fills = {});
  [[nodiscard]] std::vector<std::string> list(const i18n::Catalog* cat) const;

 private:
  struct Entry {
    std::string key;
    std::vector<std::pair<std::string, std::string>> fills;
    std::uint64_t n = 0;
  };
  std::vector<Entry> entries_;
};

/// The message with runtime fills (a vector), for the notes.
[[nodiscard]] std::string msg_v(const i18n::Catalog* cat, std::string_view key,
                                const std::vector<std::pair<std::string, std::string>>& fills);

/// TextDecoder('utf-8', {fatal: true}): nullopt on ill-formed input. A leading BOM is dropped.
[[nodiscard]] bool utf8_valid(std::span<const std::uint8_t> b) noexcept;
/// TextDecoder('utf-8') (not fatal): U+FFFD for each ill-formed subpart, a leading BOM dropped.
[[nodiscard]] std::string utf8_lossy(std::span<const std::uint8_t> b);
/// Latin-1 bytes → UTF-8 (String.fromCharCode of each byte).
[[nodiscard]] std::string latin1(std::span<const std::uint8_t> b);

}  // namespace cc::skp::detail
