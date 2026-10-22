// SPDX-License-Identifier: Apache-2.0
// What the built-ins share (builtins.js's argument checks and equality), inside libs/lisp only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/lisp/lisp.hpp"

namespace cc::lisp::detail {

[[nodiscard]] Sym kQuote();
[[nodiscard]] Sym kT();

inline constexpr std::size_t kMany = std::numeric_limits<std::size_t>::max();

/// AutoLISP's "bad argument type: <what>: <value>"
[[nodiscard]] LispError bad(std::string_view what, const Value& x);
[[noreturn]] void fail(std::string message);
void arity(const Args& a, std::size_t min, std::size_t max);
inline void arity(const Args& a, std::size_t n) {
  arity(a, n, n);
}
[[nodiscard]] double num(const Value& x);
[[nodiscard]] std::int32_t intv(const Value& x);
[[nodiscard]] const std::string& str(const Value& x);
[[nodiscard]] const Value& lst(const Value& x);
[[nodiscard]] Sym symv(const Value& x);
/// A point argument: a list of 2 or 3 (or more: the first 3 kept) numbers.
[[nodiscard]] std::vector<double> pt(const Value& x);
[[nodiscard]] Value pt_list(const std::vector<double>& p);
[[nodiscard]] inline Value boolv(bool b) {
  return b ? Value(kT()) : Value();
}
/// int(v): a JS number wrapped to 32 bits (v | 0)
[[nodiscard]] inline Value int_of(double v) noexcept {
  return Value(Int{base::js_to_int32(v)});
}
/// builtins.js equal: numbers within fuzz, lists element by element. Each node visited is a step of L (Esc and the
/// step budget stop a long comparison); nested deeper than kMaxEqualDepth in the car is V8's stack overflow.
[[nodiscard]] bool equal(Interp& L, const Value& a, const Value& b, double fuzz = 0);
inline constexpr int kMaxEqualDepth = 7000;  // V8: about 7 190 (measured, Node 24)
[[nodiscard]] bool eq(const Value& a, const Value& b);

/// UTF-16 units of a string (what JS indexes) and back (WTF-8)
[[nodiscard]] std::u16string u16(std::string_view s);
[[nodiscard]] std::string u8(std::u16string_view s);
/// a + b as JS joins two strings (a lone high surrogate before a lone low one makes a pair)
void append_js(std::string& a, std::string_view b);

/// Frees what only these values hold, without recursion (Cons and Lambda destructors).
void release(std::vector<Value>& pending);
/// show() for a routine: "Invalid string length" past `max_units` UTF-16 units (V8's String join).
[[nodiscard]] std::string show_limited(const Value& x, bool readable, std::size_t max_units);
/// show_limited for a routine's own printing; the text made is charged as steps (Options::max_steps)
[[nodiscard]] inline std::string show_for(Interp& L, const Value& x, bool readable) {
  std::string s = show_limited(x, readable, L.max_string());
  L.charge(s.size());
  return s;
}
/// Any exception as the LispError it becomes (interp.js lispError), copied.
[[nodiscard]] LispError to_lisp_error(const std::exception_ptr& ep);
/// JavaScript truthiness of the value a JS built-in tests raw (`if (a[1])`): nil, a real 0 or NaN and "" are false;
/// an integer is an object in the JS, so always true.
[[nodiscard]] inline bool js_truthy(const Value& x) {
  if (x.nil()) {
    return false;
  }
  if (x.is_real()) {
    return x.real() != 0 && x.real() == x.real();
  }
  if (x.is_str()) {
    return !x.str().empty();
  }
  return true;
}
/// The number of elements a JS call can spread (`f(...array)`) before V8 overflows its stack (measured in Node 24:
/// 125 520–125 573, depending on the stack in use); a longer one is "Maximum call stack size exceeded".
inline constexpr std::size_t kMaxSpread = 125000;
void check_spread(std::size_t n);

/// reader.js readAll / readOne (LispError on broken text)
[[nodiscard]] std::vector<Value> read_all(std::string_view text);
struct ReadOne {
  Value form;
  std::string rest;
};
[[nodiscard]] ReadOne read_one(std::string_view text);

/// One element of a wildcard (wild.js) or file pattern, over UTF-16 units.
struct GlobEl {
  enum class K : std::uint8_t { lit, any, star, cls };
  K k = K::lit;
  char16_t c = 0;
  std::vector<std::pair<char16_t, char16_t>> ranges;  // cls
  bool inv = false;                                   // cls: [^…]
  bool nolt = false;                                  // any / star: `.` (not a line terminator), not [\s\S]
};
[[nodiscard]] bool glob_match(const std::vector<GlobEl>& p, std::u16string_view s, bool icase);
/// wcmatch; LispError for a pattern the JS RegExp refuses
[[nodiscard]] bool wcmatch_or_throw(std::string_view s, std::string_view pattern, Interp* L);

void install_specials(Interp& L);
void install_builtins(Interp& L);
void install_vl(Interp& L);

}  // namespace cc::lisp::detail
