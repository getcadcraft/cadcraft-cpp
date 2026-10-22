// SPDX-License-Identifier: Apache-2.0
// builtins.js: the built-in functions that need no drawing — math, comparison, lists, strings, conversions, wildcards,
// geometry, output, files (the interpreter's own, in memory), load, eval / read / apply / mapcar, exit. Defined in the
// JS's order (the global table is a Map: atoms-family lists it in that order). Where the JS tests a raw argument with
// JavaScript's truthiness (`if (a[1])`), js_truthy does the same: a real 0 or "" counts as absent there.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/dims.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp::detail {

// ---------------------------------------------------------------- argument checks (AutoLISP's messages)
LispError bad(std::string_view what, const Value& x) {
  return LispError("bad argument type: " + std::string(what) + ": " + show(x));
}
void fail(std::string message) {
  throw LispError(std::move(message));
}
void arity(const Args& a, std::size_t min, std::size_t max) {
  if (a.size() < min) {
    fail("too few arguments");
  }
  if (max != kMany && a.size() > max) {
    fail("too many arguments");
  }
}
double num(const Value& x) {
  if (x.is_real()) {
    return x.real();
  }
  if (x.is_int()) {
    return x.int_v();
  }
  throw bad("numberp", x);
}
std::int32_t intv(const Value& x) {
  if (x.is_int()) {
    return x.int_v();
  }
  throw bad("fixnump", x);
}
const std::string& str(const Value& x) {
  if (x.is_str()) {
    return x.str();
  }
  throw bad("stringp", x);
}
const Value& lst(const Value& x) {
  if (x.is_list()) {
    return x;  // NOLINT(bugprone-return-const-ref-from-parameter) callers pass an argument, which outlives the call
  }
  throw bad("listp", x);
}
Sym symv(const Value& x) {
  if (x.is_sym()) {
    return x.sym();
  }
  throw bad("symbolp", x);
}
std::vector<double> pt(const Value& x) {
  const std::vector<Value> a = to_vector(lst(x));
  if (a.size() < 2 || !std::ranges::all_of(a, [](const Value& v) { return v.is_num(); })) {
    throw bad("2D/3D point", x);
  }
  std::vector<double> p;
  for (std::size_t k = 0; k < a.size() && k < 3; ++k) {
    p.push_back(a[k].num());
  }
  return p;
}
Value pt_list(const std::vector<double>& p) {
  std::vector<Value> v;
  v.reserve(p.size());
  for (const double d : p) {
    v.emplace_back(d);
  }
  return list(std::move(v));
}

// ---------------------------------------------------------------- equality
namespace {

// NOLINTNEXTLINE(misc-no-recursion) — depth-capped (kMaxEqualDepth)
bool equal_at(Interp& L, const Value& a0, const Value& b0, double fuzz, int depth) {
  if (depth > kMaxEqualDepth) {
    fail(std::string(kMsgDeep));
  }
  L.step();
  if (a0.same(b0)) {
    return true;
  }
  if (a0.is_num() && b0.is_num()) {
    return std::abs(a0.num() - b0.num()) <= fuzz;
  }
  if (a0.is_cons() && b0.is_cons()) {
    const Value* a = &a0;
    const Value* b = &b0;
    while (a->is_cons() && b->is_cons()) {
      if (!equal_at(L, a->cons()->car, b->cons()->car, fuzz, depth + 1)) {
        return false;
      }
      a = &a->cons()->cdr;
      b = &b->cons()->cdr;
      L.step();
    }
    return equal_at(L, *a, *b, fuzz, depth + 1);
  }
  if (a0.is_obj() && b0.is_obj()) {
    return a0.obj()->same(*b0.obj());  // Ename ids; a PickSet only itself; VLA objects their own way
  }
  return false;
}

}  // namespace

bool equal(Interp& L, const Value& a, const Value& b, double fuzz) {
  return equal_at(L, a, b, fuzz, 0);
}
bool eq(const Value& a, const Value& b) {
  if (a.same(b)) {
    return true;
  }
  return a.is_obj() && b.is_obj() && a.obj()->same(*b.obj());
}

namespace {

// ---------------------------------------------------------------- JS helpers
/// String(n) for a JS number
std::string jsnum(double d) {
  std::string s;
  base::append_number(s, d);
  return s;
}
/// Math.round
double js_round(double x) {
  if (!std::isfinite(x) || x == 0) {
    return x;
  }
  const double f = std::floor(x);
  const double r = x - f >= 0.5 ? f + 1 : f;
  return r == 0 && std::signbit(x) ? -0.0 : r;
}
/// Math.pow: NaN for a NaN exponent and for ±1 to an infinite power, where C's pow gives 1.
double js_pow(double b, double e) {
  if (std::isnan(e) || (std::abs(b) == 1 && std::isinf(e))) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::pow(b, e);
}
/// Number#toFixed(digits) for a digits value that may be NaN (ToIntegerOrInfinity: NaN → 0)
std::string fixed(double v, double digits) {
  return base::js_to_fixed(v, std::isnan(digits) ? 0 : static_cast<int>(std::trunc(digits)));
}
bool all_int(const Args& a) {
  return std::ranges::all_of(a, [](const Value& x) { return x.is_int(); });
}
void check_nums(const Args& a) {
  for (const Value& x : a) {
    (void)num(x);
  }
}
/// Math.min / Math.max over numbers (NaN wins; -0 is below +0)
double js_min(const Args& a, bool max) {
  check_spread(a.size());
  double r = max ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
  bool nan = false;
  for (const Value& x : a) {
    const double v = x.num();
    if (std::isnan(v)) {
      nan = true;
    } else if (max ? (v > r || (v == 0 && r == 0 && !std::signbit(v)))
                   : (v < r || (v == 0 && r == 0 && std::signbit(v)))) {
      r = v;
    }
  }
  return nan ? std::numeric_limits<double>::quiet_NaN() : r;
}
bool ascii_upper_eq(char a, char b) {
  return (a >= 'a' && a <= 'z' ? a - 32 : a) == (b >= 'a' && b <= 'z' ? b - 32 : b);
}
/// /\.[^.]*$/: the last dot and what follows it, or nothing
std::optional<std::size_t> ext_at(std::string_view s) {
  const std::size_t k = s.rfind('.');
  return k == std::string_view::npos ? std::nullopt : std::optional<std::size_t>(k);
}
std::string_view last_part(std::string_view p) {
  const std::size_t k = p.rfind('/');
  return k == std::string_view::npos ? p : p.substr(k + 1);
}
std::string slashes(std::string_view s) {
  std::string p(s);
  std::ranges::replace(p, '\\', '/');
  return p;
}
/// \d+ at i: the end, or i when there is none
std::size_t digits_at(std::string_view s, std::size_t i) {
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
  }
  return i;
}
/// The prefix /^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?/i matches, or nothing
std::optional<std::string_view> float_prefix(std::string_view t) {
  std::size_t i = 0;
  if (i < t.size() && (t[i] == '-' || t[i] == '+')) {
    ++i;
  }
  const std::size_t d = digits_at(t, i);
  if (d > i) {
    i = d;
    if (i < t.size() && t[i] == '.') {
      i = digits_at(t, i + 1);
    }
  } else if (i < t.size() && t[i] == '.' && digits_at(t, i + 1) > i + 1) {
    i = digits_at(t, i + 1);
  } else {
    return std::nullopt;
  }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    std::size_t k = i + 1;
    if (k < t.size() && (t[k] == '-' || t[k] == '+')) {
      ++k;
    }
    const std::size_t e = digits_at(t, k);
    if (e > k) {
      i = e;
    }
  }
  return t.substr(0, i);
}
/// sysv then numVal: the host's getvar as plain data (numVal leaves anything but an Int as it is), or the default
/// when there is no host, it refuses or it has no value.
Task<base::Value> sysv(Interp& L, std::string name, double dflt) {
  Host* h = L.host();
  if (h == nullptr || !h->has("getvar")) {
    co_return base::Value(dflt);
  }
  base::Array args;
  args.emplace_back(std::move(name));
  Host::Answer r = co_await h->call("getvar", std::move(args));
  if (r.error || r.cancel || r.value.is_null() || r.value.is_undefined()) {
    co_return base::Value(dflt);
  }
  co_return r.value;  // an integer the host sends ({i}) is no Int: numVal leaves the object as it is
}
/// a[k] as an integer when it is given and not nil, else the system variable (no co_await inside ?:: GCC 14
/// evaluates both arms of one)
Task<base::Value> arg_or_sysvar(const Args& a, std::size_t k, Interp& L, std::string name, double dflt) {
  if (a.size() > k && !a[k].nil()) {
    co_return base::Value(static_cast<double>(intv(a[k])));
  }
  co_return co_await sysv(L, std::move(name), dflt);
}
double js_number_of(const base::Value& v) {
  return base::js_to_number(v);
}
bool strict_num(const base::Value& v, double n) {
  return v.is_number() && v.as_number() == n;
}

// ---------------------------------------------------------------- number formatting
/// rtos: mode 1 scientific, 2 decimal, 3 engineering, 4 architectural, 5 fractional; DIMZIN 8 drops trailing zeros.
std::string rtos(double v, const base::Value& mode, double prec, double dimzin) {
  if (strict_num(mode, 2)) {
    const double small = 0.5 * js_pow(10, -prec);
    std::string s = fixed(std::abs(v) < small ? 0 : v, prec);
    const std::int32_t z = base::js_to_int32(dimzin);
    if ((z & 8) != 0 && s.contains('.')) {
      while (!s.empty() && s.back() == '0') {
        s.pop_back();
      }
      if (!s.empty() && s.back() == '.') {
        s.pop_back();
      }
    }
    if ((z & 4) != 0) {
      const std::size_t at = s.starts_with('-') ? 1 : 0;
      if (s.size() > at + 1 && s[at] == '0' && s[at + 1] == '.') {
        s.erase(at, 1);
      }
    }
    return s;
  }
  return cad::format_length(v, mode, base::Value(prec));
}

std::string dms(double deg, double prec) {
  double d = std::floor(deg + 1e-12);
  const double m = (deg - d) * 60;
  if (prec <= 0) {
    return jsnum(js_round(deg)) + "d";
  }
  double mi = std::floor(m + 1e-9);
  const double s = (m - mi) * 60;
  if (prec <= 2) {
    mi = js_round(m);
    if (mi == 60) {
      d++;
      mi = 0;
    }
    return jsnum(d) + "d" + jsnum(mi) + "'";
  }
  const double sd = std::isnan(prec) ? prec : std::max(0.0, prec - 4);
  double ss = base::js_number(fixed(s, sd));
  if (ss >= 60) {
    ss = 0;
    mi++;
  }
  if (mi >= 60) {
    mi = 0;
    d++;
  }
  return jsnum(d) + "d" + jsnum(mi) + "'" + fixed(ss, sd) + "\"";
}

/// angtos: 0 degrees, 1 deg/min/sec, 2 grads, 3 radians, 4 surveyor's units (and anything else).
std::string angtos(double a, double mode, double prec) {
  constexpr double kTau = std::numbers::pi * 2;
  a = std::fmod(std::fmod(a, kTau) + kTau, kTau);
  if (mode == 0) {
    return fixed(a * 180 / std::numbers::pi, prec);
  }
  if (mode == 2) {
    return fixed(a * 200 / std::numbers::pi, prec) + "g";
  }
  if (mode == 3) {
    return fixed(a, prec) + "r";
  }
  if (mode == 1) {
    return dms(a * 180 / std::numbers::pi, prec);
  }
  const double deg = a * 180 / std::numbers::pi;
  if (std::abs(deg - 90) < 1e-9) {
    return "E";
  }
  if (std::abs(deg - 270) < 1e-9) {
    return "W";
  }
  if (std::abs(deg) < 1e-9 || std::abs(deg - 360) < 1e-9) {
    return "N";
  }
  if (std::abs(deg - 180) < 1e-9) {
    return "S";
  }
  const char* ns = deg < 180 ? "N" : "S";
  const double off = deg < 180 ? std::abs(90 - deg) : std::abs(270 - deg);
  const char* ew = deg > 90 && deg < 270 ? "W" : "E";
  return ns + dms(off, prec) + ew;
}

bool ends_ci(std::string_view t, char c) {
  return !t.empty() && ascii_upper_eq(t.back(), c);
}
bool line_terminator_in(std::string_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' || s[i] == '\r' || s.substr(i).starts_with("\xE2\x80\xA8") ||
        s.substr(i).starts_with("\xE2\x80\xA9")) {
      return true;
    }
  }
  return false;
}
/// \d+(?:\.\d*)? at i: the end, or nothing
std::optional<std::size_t> dec_at(std::string_view s, std::size_t i) {
  const std::size_t d = digits_at(s, i);
  if (d == i) {
    return std::nullopt;
  }
  return d < s.size() && s[d] == '.' ? digits_at(s, d + 1) : d;
}

/// angtof: text in a mode → radians, or nothing.
// NOLINTNEXTLINE(misc-no-recursion) — once: the surveyor form reads its angle in mode 1, which has no surveyor form
std::optional<double> angtof(std::string_view s, double mode) {
  const std::string_view t = base::js_trim(s);
  if (mode == 3 || ends_ci(t, 'r')) {
    const double v = base::js_parse_float(t);
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
  }
  if (mode == 2 || ends_ci(t, 'g')) {
    const double v = base::js_parse_float(t);
    return std::isfinite(v) ? std::optional<double>(v * std::numbers::pi / 200) : std::nullopt;
  }
  // /^([NS])\s*(.*?)\s*([EW])$/i
  if (t.size() >= 2 && (ascii_upper_eq(t[0], 'N') || ascii_upper_eq(t[0], 'S')) &&
      (ascii_upper_eq(t.back(), 'E') || ascii_upper_eq(t.back(), 'W'))) {
    const std::string_view mid = base::js_trim(t.substr(1, t.size() - 2));
    if (!line_terminator_in(mid)) {
      const std::optional<double> off = angtof(mid, 1);
      if (!off) {
        return std::nullopt;
      }
      const bool north = ascii_upper_eq(t[0], 'N');
      const bool east = ascii_upper_eq(t.back(), 'E');
      const double base_deg = north ? 90 : 270;
      const double sgn = north == east ? -1 : 1;
      return std::fmod((base_deg * std::numbers::pi / 180) + sgn * *off + 2 * std::numbers::pi, 2 * std::numbers::pi);
    }
  }
  // /^(-?\d+(?:\.\d*)?)d(?:(\d+(?:\.\d*)?)')?(?:(\d+(?:\.\d*)?)")?$/i
  {
    std::size_t i = !t.empty() && t[0] == '-' ? 1 : 0;
    const std::optional<std::size_t> g1 = dec_at(t, i);
    if (g1 && *g1 < t.size() && ascii_upper_eq(t[*g1], 'd')) {
      const std::string_view m1 = t.substr(0, *g1);
      i = *g1 + 1;
      std::string_view m2;
      std::string_view m3;
      if (const auto g2 = dec_at(t, i); g2 && *g2 < t.size() && t[*g2] == '\'') {
        m2 = t.substr(i, *g2 - i);
        i = *g2 + 1;
      }
      if (const auto g3 = dec_at(t, i); g3 && *g3 < t.size() && t[*g3] == '"') {
        m3 = t.substr(i, *g3 - i);
        i = *g3 + 1;
      }
      if (i == t.size()) {
        const double v2 = m2.empty() ? 0 : base::js_number(m2);
        const double v3 = m3.empty() ? 0 : base::js_number(m3);
        return (base::js_number(m1) + v2 / 60 + v3 / 3600) * std::numbers::pi / 180;
      }
    }
  }
  if (cad::plain_number(t, true)) {
    return base::js_parse_float(t) * std::numbers::pi / 180;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- unit conversion (cvunit)
struct Unit {
  std::string_view name;
  std::string_view kind;
  double f;
};
constexpr double kDeg = std::numbers::pi / 180;
constexpr double kGrad = std::numbers::pi / 200;
constexpr auto kUnits = std::to_array<Unit>({
    {"inch", "len", 0.0254},
    {"inches", "len", 0.0254},
    {"in", "len", 0.0254},
    {"\"", "len", 0.0254},
    {"foot", "len", 0.3048},
    {"feet", "len", 0.3048},
    {"ft", "len", 0.3048},
    {"'", "len", 0.3048},
    {"yard", "len", 0.9144},
    {"yards", "len", 0.9144},
    {"yd", "len", 0.9144},
    {"mile", "len", 1609.344},
    {"miles", "len", 1609.344},
    {"mi", "len", 1609.344},
    {"millimeter", "len", 0.001},
    {"millimeters", "len", 0.001},
    {"mm", "len", 0.001},
    {"centimeter", "len", 0.01},
    {"centimeters", "len", 0.01},
    {"cm", "len", 0.01},
    {"meter", "len", 1},
    {"meters", "len", 1},
    {"m", "len", 1},
    {"kilometer", "len", 1000},
    {"kilometers", "len", 1000},
    {"km", "len", 1000},
    {"degree", "ang", kDeg},
    {"degrees", "ang", kDeg},
    {"radian", "ang", 1},
    {"radians", "ang", 1},
    {"grad", "ang", kGrad},
    {"grads", "ang", kGrad},
    {"kilogram", "mass", 1},
    {"kg", "mass", 1},
    {"gram", "mass", 0.001},
    {"g", "mass", 0.001},
    {"pound", "mass", 0.45359237},
    {"lb", "mass", 0.45359237},
    {"sq inch", "area", 0.00064516},
    {"sq foot", "area", 0.09290304},
    {"sq meter", "area", 1},
    {"sq mm", "area", 1e-6},
    {"acre", "area", 4046.8564224},
    {"hectare", "area", 10000},
    // what a JS object answers for these keys too (Object.prototype): no kind, no factor
    {"constructor", "", std::numeric_limits<double>::quiet_NaN()},
    {"__proto__", "", std::numeric_limits<double>::quiet_NaN()},
});
const Unit* unit_of(std::string_view s) {
  const std::string k = base::js_to_lower(base::js_trim(s));
  if (k.empty()) {
    return nullptr;
  }
  for (const Unit& u : kUnits) {  // a loop: std::array's iterator is a pointer in one libc++, a wrapper in Emscripten's
    if (u.name == k) {
      return &u;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- strings (UTF-16 units, as the JS)
std::size_t units(std::string_view s) {
  return base::utf16_length(s);
}
Value str_of_units(Interp& L, std::u16string_view u) {
  L.check_string(u.size());
  L.charge(u.size());
  return {u8(u)};
}
/// The first UTF-16 unit of s (the JS s[0]), or nothing for "".
std::optional<char16_t> first_unit(std::string_view s) {
  if (s.empty()) {
    return std::nullopt;
  }
  const std::u16string u = u16(s.substr(0, std::min<std::size_t>(4, s.size())));
  return u.empty() ? std::nullopt : std::optional<char16_t>(u[0]);
}
/// UTF-16 order (JS < on strings)
int cmp16(std::string_view a, std::string_view b) {
  const std::u16string x = u16(a);
  const std::u16string y = u16(b);
  if (x < y) {
    return -1;
  }
  return x > y ? 1 : 0;
}

// ---------------------------------------------------------------- the files
LFile* as_file(const Value& f) {
  return f.as<LFile>();
}
void to_file(Interp& L, const Value& f, std::string_view text) {
  LFile* h = as_file(f);
  if (h == nullptr || !h->open || h->mode == 'r') {
    throw bad("filep", f);
  }
  const std::u16string add = u16(text);
  L.check_string(h->buf.size() + add.size());
  L.charge(add.size());
  h->buf += add;
}
void put(Interp& L, const Value& to, const std::string& s) {
  if (js_truthy(to)) {
    to_file(L, to, s);
  } else {
    L.out(s, true);
  }
}

// ================================================================ the functions
// ---- arithmetic
Value f_add(Args& a, Interp& /*L*/) {
  check_nums(a);
  double s = 0;
  if (all_int(a)) {
    for (const Value& x : a) {
      s += x.int_v();
    }
    return int_of(s);
  }
  for (const Value& x : a) {
    s += x.num();
  }
  return {s};
}
Value f_sub(Args& a, Interp& /*L*/) {
  check_nums(a);
  if (a.empty()) {
    return Value(Int{0});
  }
  if (a.size() == 1) {
    return a[0].is_int() ? int_of(-static_cast<double>(a[0].int_v())) : Value(-a[0].real());
  }
  double r = a[0].num();
  for (std::size_t k = 1; k < a.size(); ++k) {
    r -= a[k].num();
  }
  return all_int(a) ? int_of(r) : Value(r);
}
Value f_mul(Args& a, Interp& /*L*/) {
  check_nums(a);
  if (a.empty()) {
    return Value(Int{0});
  }
  if (all_int(a)) {
    std::uint32_t r = 1;
    for (const Value& x : a) {
      r *= static_cast<std::uint32_t>(x.int_v());  // Math.imul
    }
    return Value(Int{static_cast<std::int32_t>(r)});
  }
  double r = 1;
  for (const Value& x : a) {
    r *= x.num();
  }
  return {r};
}
Value f_div(Args& a, Interp& /*L*/) {
  check_nums(a);
  if (a.empty()) {
    return Value(Int{0});
  }
  bool ints = a[0].is_int();
  double r = a[0].num();
  for (std::size_t k = 1; k < a.size(); ++k) {
    const double v = a[k].num();
    if (v == 0) {
      fail("divide by zero");
    }
    ints = ints && a[k].is_int();
    r = ints ? std::trunc(r / v) : r / v;
  }
  return ints ? int_of(r) : Value(r);
}
Value f_not_bits(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return Value(Int{~intv(a[0])});
}
Value f_inc(Args& a, Interp& /*L*/) {
  arity(a, 1);
  (void)num(a[0]);
  return a[0].is_int() ? int_of(static_cast<double>(a[0].int_v()) + 1) : Value(a[0].real() + 1);
}
Value f_dec(Args& a, Interp& /*L*/) {
  arity(a, 1);
  (void)num(a[0]);
  return a[0].is_int() ? int_of(static_cast<double>(a[0].int_v()) - 1) : Value(a[0].real() - 1);
}
Value f_abs(Args& a, Interp& /*L*/) {
  arity(a, 1);
  (void)num(a[0]);
  return a[0].is_int() ? int_of(std::abs(static_cast<double>(a[0].int_v()))) : Value(std::abs(a[0].real()));
}
Value f_fix(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return int_of(std::trunc(num(a[0])));
}
Value f_float(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {num(a[0])};
}
Value f_rem(Args& a, Interp& /*L*/) {
  check_nums(a);
  if (a.empty()) {
    return Value(Int{0});
  }
  double r = a[0].num();
  for (std::size_t k = 1; k < a.size(); ++k) {
    const double v = a[k].num();
    if (v == 0) {
      fail("divide by zero");
    }
    r = std::fmod(r, v);
  }
  return all_int(a) ? int_of(r) : Value(r);
}
double gcd2(double x, double y) {
  x = std::abs(x);
  y = std::abs(y);
  while (y != 0) {
    const double t = std::fmod(x, y);
    x = y;
    y = t;
  }
  return x;
}
Value f_gcd(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  double r = intv(a[0]);
  for (std::size_t k = 1; k < a.size(); ++k) {
    (void)intv(a[k]);
  }
  for (std::size_t k = 1; k < a.size(); ++k) {
    r = gcd2(r, a[k].int_v());
  }
  return int_of(r);
}
Value f_lcm(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  for (const Value& x : a) {
    (void)intv(x);
  }
  double r = a[0].int_v();
  for (std::size_t k = 1; k < a.size(); ++k) {
    const double y = a[k].int_v();
    r = r != 0 && y != 0 ? std::abs(r * y) / gcd2(r, y) : 0;
  }
  return int_of(r);
}
Value f_min(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  check_nums(a);
  const double v = js_min(a, false);
  return all_int(a) ? int_of(v) : Value(v);
}
Value f_max(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  check_nums(a);
  const double v = js_min(a, true);
  return all_int(a) ? int_of(v) : Value(v);
}
Value f_sqrt(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const double v = num(a[0]);
  if (v < 0) {
    fail("function undefined for argument: " + show(a[0]));
  }
  return {std::sqrt(v)};
}
Value f_exp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {std::exp(num(a[0]))};
}
Value f_log(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const double v = num(a[0]);
  if (v <= 0) {
    fail("function undefined for argument: " + show(a[0]));
  }
  return {std::log(v)};
}
Value f_expt(Args& a, Interp& /*L*/) {
  arity(a, 2);
  const double b = num(a[0]);
  const double e = num(a[1]);
  const double r = js_pow(b, e);
  return a[0].is_int() && a[1].is_int() && e >= 0 ? int_of(r) : Value(r);
}
Value f_sin(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {base::js_sin(num(a[0]))};
}
Value f_cos(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {base::js_cos(num(a[0]))};
}
Value f_atan(Args& a, Interp& /*L*/) {
  arity(a, 1, 2);
  if (a.size() == 2) {
    const double y = num(a[0]);
    return {base::js_atan2(y, num(a[1]))};
  }
  return {base::js_atan(num(a[0]))};
}
Value f_minusp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(num(a[0]) < 0);
}
Value f_zerop(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(num(a[0]) == 0);
}
Value f_numberp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].is_num());
}
Value f_boole(Args& a, Interp& /*L*/) {
  arity(a, 2, kMany);
  const std::int32_t op = intv(a[0]);
  auto r = static_cast<std::uint32_t>(intv(a[1]));
  for (std::size_t k = 2; k < a.size(); ++k) {
    const auto b = static_cast<std::uint32_t>(intv(a[k]));
    std::uint32_t o = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
      const unsigned p = (r >> bit) & 1U;
      const unsigned q = (b >> bit) & 1U;
      int kk = 8;  // neither
      if (p != 0 && q != 0) {
        kk = 1;
      } else if (p != 0) {
        kk = 2;
      } else if (q != 0) {
        kk = 4;
      }
      if ((op & kk) != 0) {
        o |= 1U << bit;
      }
    }
    r = o;
  }
  return Value(Int{static_cast<std::int32_t>(r)});
}
Value f_logand(Args& a, Interp& /*L*/) {
  std::int32_t r = -1;
  for (const Value& x : a) {
    r &= intv(x);
  }
  return Value(Int{a.empty() ? 0 : r});
}
Value f_logior(Args& a, Interp& /*L*/) {
  std::int32_t r = 0;
  for (const Value& x : a) {
    r |= intv(x);
  }
  return Value(Int{r});
}
Value f_lsh(Args& a, Interp& /*L*/) {
  arity(a, 0, 2);
  const std::int32_t v = a.empty() ? 0 : intv(a[0]);
  const std::int32_t n = a.size() > 1 ? intv(a[1]) : 0;
  const auto u = static_cast<std::uint32_t>(v);
  if (n >= 0) {
    return Value(Int{static_cast<std::int32_t>(u << (static_cast<std::uint32_t>(n) & 31U))});
  }
  const std::uint32_t c = static_cast<std::uint32_t>(-static_cast<std::int64_t>(n)) & 31U;
  return Value(Int{static_cast<std::int32_t>(u >> c)});  // >>> then | 0
}

// ---- comparison
enum class Rel : std::uint8_t { lt, gt, le, ge };
bool rel_holds(const Value& x, const Value& y, Rel r) {
  if (x.is_str() && y.is_str()) {
    const int c = cmp16(x.str(), y.str());
    switch (r) {
      case Rel::lt: return c < 0;
      case Rel::gt: return c > 0;
      case Rel::le: return c <= 0;
      case Rel::ge: return c >= 0;
    }
  }
  const double a = x.is_str() ? base::js_number(x.str()) : x.num();
  const double b = y.is_str() ? base::js_number(y.str()) : y.num();
  switch (r) {
    case Rel::lt: return a < b;
    case Rel::gt: return a > b;
    case Rel::le: return a <= b;
    case Rel::ge: return a >= b;
  }
  return false;
}
template <Rel R>
Value f_rel(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  for (const Value& x : a) {
    if (!x.is_str()) {
      (void)num(x);
    }
  }
  for (std::size_t k = 1; k < a.size(); ++k) {
    if (!rel_holds(a[k - 1], a[k], R)) {
      return {};
    }
  }
  return {kT()};
}
bool same_val(const Value& x, const Value& y) {
  if (x.is_num() && y.is_num()) {
    return x.num() == y.num();
  }
  if (x.is_str() && y.is_str()) {
    return x.str() == y.str();
  }
  return eq(x, y);
}
Value f_eqn(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  for (std::size_t k = 1; k < a.size(); ++k) {
    if (!same_val(a[k - 1], a[k])) {
      return {};
    }
  }
  return {kT()};
}
Value f_neq(Args& a, Interp& /*L*/) {
  arity(a, 1, kMany);
  for (std::size_t k = 1; k < a.size(); ++k) {
    if (same_val(a[k - 1], a[k])) {
      return {};
    }
  }
  return {kT()};
}
Value f_eq(Args& a, Interp& /*L*/) {
  arity(a, 2);
  return boolv(eq(a[0], a[1]));
}
Value f_equal(Args& a, Interp& L) {
  arity(a, 2, 3);
  return boolv(equal(L, a[0], a[1], a.size() > 2 ? num(a[2]) : 0));
}
Value f_not(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].nil());
}
Value f_atom(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(!a[0].is_cons());
}
Value f_listp(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return boolv(a[0].is_list());
}
Value f_boundp(Args& a, Interp& L) {
  arity(a, 1);
  return boolv(L.bound(symv(a[0])));
}
Value f_type(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::string n = type_name(a[0]);
  return n.empty() ? Value() : Value(sym(n));
}

// ---- lists
Value car(const Value& x) {
  if (x.nil()) {
    return {};
  }
  if (x.is_cons()) {
    return x.cons()->car;
  }
  throw bad("consp", x);
}
Value cdr(const Value& x) {
  if (x.nil()) {
    return {};
  }
  if (x.is_cons()) {
    return x.cons()->cdr;
  }
  throw bad("consp", x);
}
constexpr std::array<std::string_view, 30> kPaths = {"A",    "D",    "AA",   "AD",   "DA",   "DD",   "AAA",  "AAD",
                                                     "ADA",  "ADD",  "DAA",  "DAD",  "DDA",  "DDD",  "AAAA", "AAAD",
                                                     "AADA", "AADD", "ADAA", "ADAD", "ADDA", "ADDD", "DAAA", "DAAD",
                                                     "DADA", "DADD", "DDAA", "DDAD", "DDDA", "DDDD"};
template <std::size_t N>
Value f_cxr(Args& a, Interp& /*L*/) {
  arity(a, 1);
  Value x = a[0];
  constexpr std::string_view p = kPaths[N];
  for (char it : std::views::reverse(p)) {
    x = it == 'A' ? car(x) : cdr(x);
  }
  return x;
}
template <std::size_t... I>
void def_cxr(Interp& L, std::index_sequence<I...> /*paths*/) {
  (L.def("C" + std::string(kPaths[I]) + "R", &f_cxr<I>), ...);
}
Value f_cons(Args& a, Interp& /*L*/) {
  arity(a, 2);
  return cons(a[0], a[1]);
}
Value f_list(Args& a, Interp& /*L*/) {
  return list(std::move(a));
}
Value f_append(Args& a, Interp& L) {
  if (a.empty()) {
    return {};
  }
  std::vector<Value> items;
  for (const Value& x : a) {
    std::vector<Value> v = to_vector(lst(x));
    check_spread(v.size());  // items.push(...toArray(x))
    L.charge(v.size());
    items.insert(items.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  }
  return list(std::move(items));
}
Value f_reverse(Args& a, Interp& L) {
  arity(a, 1);
  std::vector<Value> v = to_vector(lst(a[0]));
  L.charge(v.size());
  std::ranges::reverse(v);
  return list(std::move(v));
}
Value f_length(Args& a, Interp& /*L*/) {
  arity(a, 1);
  (void)lst(a[0]);
  if (!is_proper(a[0])) {
    throw bad("listp", a[0]);
  }
  std::size_t n = 0;
  const Value& first = a[0];
  for (const Value* x = &first; x->is_cons(); x = &x->cons()->cdr) {
    ++n;
  }
  return int_of(static_cast<double>(n));
}
Value f_nth(Args& a, Interp& /*L*/) {
  arity(a, 2);
  std::int32_t n = intv(a[0]);
  const Value* x = &lst(a[1]);
  if (n < 0) {
    return {};
  }
  while (n-- > 0 && x->is_cons()) {
    x = &x->cons()->cdr;
  }
  return x->is_cons() ? x->cons()->car : Value();
}
Value f_last(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const Value* x = &lst(a[0]);
  if (x->nil()) {
    return {};
  }
  while (x->cons()->cdr.is_cons()) {
    x = &x->cons()->cdr;
  }
  return x->cons()->car;
}
Value f_member(Args& a, Interp& L) {
  arity(a, 2);
  for (const Value* x = &lst(a[1]); x->is_cons(); x = &x->cons()->cdr) {
    if (equal(L, a[0], x->cons()->car)) {
      return *x;
    }
  }
  return {};
}
Value f_assoc(Args& a, Interp& L) {
  arity(a, 2);
  for (const Value* x = &lst(a[1]); x->is_cons(); x = &x->cons()->cdr) {
    const Value& e = x->cons()->car;
    if (e.is_cons() && equal(L, a[0], e.cons()->car)) {
      return e;
    }
  }
  return {};
}
Value f_subst(Args& a, Interp& L) {
  arity(a, 3);
  std::vector<Value> v = to_vector(lst(a[2]));
  L.charge(v.size());
  for (Value& x : v) {
    if (equal(L, x, a[1])) {
      x = a[0];
    }
  }
  return list(std::move(v));
}
Value f_strlsort(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::vector<Value> v = to_vector(lst(a[0]));
  std::vector<std::pair<std::u16string, Value>> keyed;
  keyed.reserve(v.size());
  for (const Value& x : v) {
    keyed.emplace_back(u16(base::js_to_upper(str(x))), x);
  }
  std::ranges::stable_sort(keyed, [](const auto& p, const auto& q) { return p.first < q.first; });
  std::vector<Value> out;
  out.reserve(keyed.size());
  for (auto& k : keyed) {
    out.push_back(std::move(k.second));
  }
  return list(std::move(out));
}

// ---- functions
Task<Value> f_apply(Args a, Interp& L) {
  arity(a, 2);
  Value f = L.fn_of(a[0]);
  co_return co_await L.apply(std::move(f), to_vector(lst(a[1])));
}
Task<Value> f_mapcar(Args a, Interp& L) {
  arity(a, 2, kMany);
  const Value f = L.fn_of(a[0]);
  std::vector<std::vector<Value>> ls;
  for (std::size_t k = 1; k < a.size(); ++k) {
    ls.push_back(to_vector(lst(a[k])));
  }
  check_spread(ls.size());
  std::size_t n = std::numeric_limits<std::size_t>::max();
  for (const auto& x : ls) {
    n = std::min(n, x.size());
  }
  std::vector<Value> out;
  for (std::size_t i = 0; i < n; ++i) {
    Args args;
    args.reserve(ls.size());
    for (const auto& x : ls) {
      args.push_back(x[i]);
    }
    out.push_back(co_await L.apply(f, std::move(args)));
    L.charge(1);
  }
  co_return list(std::move(out));
}
Task<Value> f_eval(Args a, Interp& L) {
  arity(a, 1);
  co_return co_await L.eval(a[0]);
}
Value f_set(Args& a, Interp& L) {
  arity(a, 2);
  return L.set(symv(a[0]), a[1]);
}
Value f_read(Args& a, Interp& /*L*/) {
  arity(a, 0, 1);
  if (a.empty()) {
    return {};
  }
  const std::string& s = str(a[0]);
  return base::js_blank(s) ? Value() : read_one(s).form;
}
/// String(x) as the JS gives it for what a routine may pass
std::string js_string_of(const Value& x) {
  if (x.is_str()) {
    return x.str();
  }
  if (x.is_sym()) {
    return x.sym()->name;
  }
  if (x.is_real()) {
    return jsnum(x.real());
  }
  if (x.nil()) {
    return "null";
  }
  return "[object Object]";
}
Value f_atoms_family(Args& a, Interp& L) {
  arity(a, 1, 2);
  std::vector<std::string> names;
  for (const auto& [s, v] : L.global_entries()) {
    names.push_back(s->name);
  }
  if (a.size() > 1) {
    std::vector<Value> out;
    for (const Value& s : to_vector(lst(a[1]))) {
      const std::string u = base::js_to_upper(js_string_of(s));
      if (std::ranges::find(names, u) != names.end()) {
        out.push_back(intv(a[0]) != 0 ? Value(u) : Value(sym(js_string_of(s))));
      } else {
        out.emplace_back();
      }
    }
    return list(std::move(out));
  }
  const bool as_strings = intv(a[0]) != 0;
  std::vector<Value> out;
  out.reserve(names.size());
  for (std::string& n : names) {
    out.push_back(as_strings ? Value(std::move(n)) : Value(sym(n)));
  }
  return list(std::move(out));
}
Value f_exit(Args& /*a*/, Interp& /*L*/) {
  throw LispError(std::string(kMsgQuit));
}
Value f_nothing(Args& /*a*/, Interp& /*L*/) {
  return {};
}
Value f_ver(Args& /*a*/, Interp& /*L*/) {
  return {std::string("CadCraft LISP 0.4 (en)")};
}

// ---- strings
Value f_strcat(Args& a, Interp& L) {
  std::size_t n = 0;
  for (const Value& x : a) {
    n += units(str(x));
  }
  L.check_string(n);
  L.charge(n);
  std::string out;
  for (const Value& x : a) {
    append_js(out, x.str());
  }
  return {std::move(out)};
}
Value f_strlen(Args& a, Interp& /*L*/) {
  double n = 0;
  for (const Value& x : a) {
    n += static_cast<double>(units(str(x)));
  }
  return int_of(n);
}
Value f_substr(Args& a, Interp& L) {
  arity(a, 2, 3);
  const std::string& s = str(a[0]);
  const std::int32_t start = intv(a[1]);
  if (start < 1) {
    fail("bad argument value: positive " + show(a[1]));
  }
  std::optional<std::int32_t> n;
  if (a.size() > 2) {
    n = intv(a[2]);
    if (*n < 0) {
      fail("bad argument value: non-negative " + show(a[2]));
    }
  }
  const std::u16string u = u16(s);
  const auto from = static_cast<std::size_t>(start - 1);
  if (from >= u.size()) {
    return {std::string()};
  }
  const std::size_t len = n ? std::min(static_cast<std::size_t>(*n), u.size() - from) : u.size() - from;
  return str_of_units(L, std::u16string_view(u).substr(from, len));
}
Value f_strcase(Args& a, Interp& L) {
  arity(a, 1, 2);
  const std::string& s = str(a[0]);
  std::string r = a.size() > 1 && a[1].truthy() ? base::js_to_lower(s) : base::js_to_upper(s);
  L.check_string(units(r));
  L.charge(r.size());
  return {std::move(r)};
}
Value f_ascii(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::optional<char16_t> c = first_unit(str(a[0]));
  return Value(Int{c ? static_cast<std::int32_t>(*c) : 0});
}
Value f_chr(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::int32_t n = intv(a[0]);
  if (n == 0) {
    return {std::string()};
  }
  const std::u16string u(1, static_cast<char16_t>(static_cast<std::uint32_t>(n) & 0xFFFFU));
  return {u8(u)};
}
Value f_itoa(Args& a, Interp& /*L*/) {
  arity(a, 1);
  return {std::to_string(intv(a[0]))};
}
Value f_atoi(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::string_view t = base::js_trim(str(a[0]));
  std::size_t i = !t.empty() && (t[0] == '-' || t[0] == '+') ? 1 : 0;
  const std::size_t d = digits_at(t, i);
  if (d == i) {
    return Value(Int{0});
  }
  i = d;
  return int_of(base::js_parse_int(t.substr(0, i)));
}
Value f_atof(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::optional<std::string_view> m = float_prefix(base::js_trim(str(a[0])));
  return {m ? base::js_parse_float(*m) : 0.0};
}
Value f_wcmatch(Args& a, Interp& L) {
  arity(a, 2);
  const std::string& s = str(a[0]);
  return boolv(wcmatch_or_throw(s, str(a[1]), &L));
}
Task<Value> f_rtos(Args a, Interp& L) {
  arity(a, 1, 3);
  const double v = num(a[0]);
  const base::Value mode = co_await arg_or_sysvar(a, 1, L, "LUNITS", 2);
  const base::Value prec = co_await arg_or_sysvar(a, 2, L, "LUPREC", 4);
  const double m = js_number_of(mode);
  if (m < 1 || m > 5) {
    fail("bad argument value: " + show(a.size() > 1 ? a[1] : Value()));
  }
  const base::Value dimzin = co_await sysv(L, "DIMZIN", 0);
  const double p = js_number_of(prec);
  const double clamped = std::isnan(p) ? p : std::max(0.0, std::min(8.0, p));
  co_return Value(rtos(v, mode, clamped, js_number_of(dimzin)));
}
Task<Value> f_angtos(Args a, Interp& L) {
  arity(a, 1, 3);
  const double v = num(a[0]);
  const base::Value mode = co_await arg_or_sysvar(a, 1, L, "AUNITS", 0);
  const base::Value prec = co_await arg_or_sysvar(a, 2, L, "AUPREC", 0);
  const double p = js_number_of(prec);
  const double clamped = std::isnan(p) ? p : std::max(0.0, std::min(8.0, p));
  // mode === 0 … : only a number matches
  const double m = mode.is_number() ? mode.as_number() : std::numeric_limits<double>::quiet_NaN();
  co_return Value(angtos(v, m, clamped));
}
Task<Value> f_distof(Args a, Interp& L) {
  arity(a, 1, 2);
  const base::Value mode = co_await arg_or_sysvar(a, 1, L, "LUNITS", 2);
  const std::string_view s = base::js_trim(str(a[0]));
  if (strict_num(mode, 1) || s.find_first_of("eE") != std::string_view::npos) {
    const double v = base::js_number(s);
    co_return std::isfinite(v) ? Value(v) : Value();
  }
  const base::Value lunits = strict_num(mode, 2) || strict_num(mode, 5) ? base::Value(4.0) : mode;
  const std::optional<double> r = cad::parse_length(s, lunits);
  co_return r ? Value(*r) : Value();
}
Value f_angtof(Args& a, Interp& /*L*/) {
  arity(a, 1, 2);
  const std::string& s = str(a[0]);
  const double mode = a.size() > 1 && !a[1].nil() ? intv(a[1]) : 0;
  const std::optional<double> r = angtof(s, mode);
  return r ? Value(*r) : Value();
}
Value f_cvunit(Args& a, Interp& /*L*/) {
  arity(a, 3);
  const Unit* from = unit_of(str(a[1]));
  const Unit* to = unit_of(str(a[2]));
  if (from == nullptr || to == nullptr || from->kind != to->kind) {
    return {};
  }
  const auto conv = [&](double v) { return v * from->f / to->f; };
  if (a[0].is_num()) {
    return {conv(a[0].num())};
  }
  if (a[0].is_cons()) {
    std::vector<Value> out;
    for (const Value& x : to_vector(a[0])) {
      out.emplace_back(conv(num(x)));
    }
    return list(std::move(out));
  }
  throw bad("numberp", a[0]);
}

// ---- output
Value f_princ(Args& a, Interp& L) {
  arity(a, 0, 2);
  if (a.empty()) {
    return {std::string()};
  }
  put(L, a.size() > 1 ? a[1] : Value(), show_for(L, a[0], false));
  return a[0];
}
Value f_prin1(Args& a, Interp& L) {
  arity(a, 0, 2);
  if (a.empty()) {
    return {std::string()};
  }
  put(L, a.size() > 1 ? a[1] : Value(), show_for(L, a[0], true));
  return a[0];
}
Value f_print(Args& a, Interp& L) {
  arity(a, 0, 2);
  if (a.empty()) {
    return {std::string()};
  }
  put(L, a.size() > 1 ? a[1] : Value(), "\n" + show_for(L, a[0], true) + " ");
  return a[0];
}
Value f_terpri(Args& /*a*/, Interp& L) {
  L.out("\n", true);
  return {};
}
Value f_prompt(Args& a, Interp& L) {
  arity(a, 1);
  L.out(str(a[0]), true);
  return {};
}

// ---- geometry
Value f_polar(Args& a, Interp& /*L*/) {
  arity(a, 3);
  const std::vector<double> p = pt(a[0]);
  const double ang = num(a[1]);
  const double d = num(a[2]);
  std::vector<double> q = {p[0] + d * base::js_cos(ang), p[1] + d * base::js_sin(ang)};
  if (p.size() > 2) {
    q.push_back(p[2]);
  }
  return pt_list(q);
}
Value f_distance(Args& a, Interp& /*L*/) {
  arity(a, 2);
  const std::vector<double> p = pt(a[0]);
  const std::vector<double> q = pt(a[1]);
  const double dz = p.size() > 2 && q.size() > 2 ? p[2] - q[2] : 0;
  return {base::js_hypot(p[0] - q[0], p[1] - q[1], dz)};
}
Value f_angle(Args& a, Interp& /*L*/) {
  arity(a, 2);
  const std::vector<double> p = pt(a[0]);
  const std::vector<double> q = pt(a[1]);
  const double v = base::js_atan2(q[1] - p[1], q[0] - p[0]);
  return {v < 0 ? v + std::numbers::pi * 2 : v};
}
Value f_inters(Args& a, Interp& /*L*/) {
  arity(a, 4, 5);
  const std::vector<double> p1 = pt(a[0]);
  const std::vector<double> p2 = pt(a[1]);
  const std::vector<double> p3 = pt(a[2]);
  const std::vector<double> p4 = pt(a[3]);
  const bool onseg = a.size() < 5 || a[4].truthy();
  const double dx1 = p2[0] - p1[0];
  const double dy1 = p2[1] - p1[1];
  const double dx2 = p4[0] - p3[0];
  const double dy2 = p4[1] - p3[1];
  const double den = dx1 * dy2 - dy1 * dx2;
  double scale = base::js_hypot(dx1, dy1) * base::js_hypot(dx2, dy2);
  if (scale == 0 || std::isnan(scale)) {
    scale = 1;
  }
  if (std::abs(den) < 1e-12 * scale) {
    return {};
  }
  const double u = ((p3[0] - p1[0]) * dy2 - (p3[1] - p1[1]) * dx2) / den;
  const double v = ((p3[0] - p1[0]) * dy1 - (p3[1] - p1[1]) * dx1) / den;
  if (onseg && (u < -1e-10 || u > 1 + 1e-10 || v < -1e-10 || v > 1 + 1e-10)) {
    return {};
  }
  std::vector<double> q = {p1[0] + u * dx1, p1[1] + u * dy1};
  if (p1.size() > 2 && p2.size() > 2) {
    q.push_back(p1[2] + u * (p2[2] - p1[2]));
  }
  return pt_list(q);
}
Value f_trans(Args& a, Interp& /*L*/) {
  arity(a, 3, 4);
  (void)pt(a[0]);
  return a[0];
}

// ---- environment
Value f_getenv(Args& a, Interp& L) {
  arity(a, 1);
  const auto it = L.env.find(base::js_to_upper(str(a[0])));
  return it == L.env.end() ? Value() : Value(it->second);
}
Value f_setenv(Args& a, Interp& L) {
  arity(a, 2);
  std::string k = base::js_to_upper(str(a[0]));
  L.env[std::move(k)] = str(a[1]);
  return a[1];
}

// ---- files (in memory)
Value f_findfile(Args& a, Interp& L) {
  arity(a, 1);
  const Interp::File* f = L.find_file(str(a[0]));
  return f == nullptr ? Value() : Value(f->name);
}
Value f_open(Args& a, Interp& L) {
  arity(a, 2, 3);
  const std::string& path = str(a[0]);
  const std::optional<char16_t> m = first_unit(base::js_to_lower(str(a[1])));
  if (!m || (*m != u'r' && *m != u'w' && *m != u'a')) {
    fail("bad argument value: " + show(a[1]));
  }
  const char mode = static_cast<char>(*m);
  const Interp::File* f = L.find_file(path);
  if (mode == 'r' && f == nullptr) {
    return {};
  }
  auto h = std::make_shared<LFile>(f != nullptr ? f->name : path, mode);
  if (f != nullptr && (mode == 'r' || mode == 'a')) {
    h->buf = u16(f->text);
  }
  return {ObjRef(std::move(h))};
}
Value f_close(Args& a, Interp& L) {
  arity(a, 1);
  LFile* f = as_file(a[0]);
  if (f == nullptr) {
    throw bad("filep", a[0]);
  }
  if (f->open && f->mode != 'r') {
    L.put_file(f->path, u8(f->buf));
  }
  f->open = false;
  return {};
}
Value f_read_line(Args& a, Interp& L) {
  arity(a, 0, 1);
  LFile* f = a.empty() ? nullptr : as_file(a[0]);
  if (f == nullptr || f->mode != 'r' || !f->open || f->pos >= f->buf.size()) {
    return {};
  }
  std::size_t nl = f->buf.find(u'\n', f->pos);
  if (nl == std::u16string::npos) {
    nl = f->buf.size();
  }
  std::u16string_view line = std::u16string_view(f->buf).substr(f->pos, nl - f->pos);
  if (!line.empty() && line.back() == u'\r') {
    line.remove_suffix(1);
  }
  f->pos = nl + 1;
  return str_of_units(L, line);
}
Value f_read_char(Args& a, Interp& /*L*/) {
  arity(a, 0, 1);
  LFile* f = a.empty() ? nullptr : as_file(a[0]);
  if (f == nullptr || f->mode != 'r' || f->pos >= f->buf.size()) {
    return {};
  }
  return Value(Int{static_cast<std::int32_t>(f->buf[f->pos++])});
}
Value f_write_line(Args& a, Interp& L) {
  arity(a, 1, 2);
  const std::string& s = str(a[0]);
  if (a.size() > 1 && js_truthy(a[1])) {
    to_file(L, a[1], s + "\n");
  } else {
    L.out(s, true);
  }
  return a[0];
}
Value f_write_char(Args& a, Interp& L) {
  arity(a, 1, 2);
  const std::u16string u(1, static_cast<char16_t>(static_cast<std::uint32_t>(intv(a[0])) & 0xFFFFU));
  put(L, a.size() > 1 ? a[1] : Value(), u8(u));
  return a[0];
}
Value f_mktemp(Args& a, Interp& /*L*/) {
  const auto arg = [&](std::size_t k) { return k < a.size() ? a[k] : Value(); };
  const std::string pat = js_truthy(arg(0)) ? str(arg(0)) : std::string("$VL~~");
  std::string dir = "temp/";
  if (js_truthy(arg(1))) {
    dir = str(arg(1));
    if (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) {
      dir.pop_back();
    }
    dir += '/';
  }
  const std::string ext = js_truthy(arg(2)) ? str(arg(2)) : std::string();
  const std::optional<std::size_t> dot = ext_at(pat);
  std::string n = std::to_string(next_temp_number());
  while (n.size() < 3) {
    n.insert(n.begin(), '0');
  }
  std::string tail = ext;
  if (tail.empty() && dot) {
    tail = pat.substr(*dot);
  }
  return {dir + pat.substr(0, dot.value_or(pat.size())) + n + tail};
}
Value f_file_delete(Args& a, Interp& L) {
  arity(a, 1);
  return boolv(L.delete_file(str(a[0])));
}
Value f_file_rename(Args& a, Interp& L) {
  arity(a, 2);
  const Interp::File* f = L.find_file(str(a[0]));
  if (f == nullptr || L.find_file(str(a[1])) != nullptr) {
    return {};
  }
  const Interp::File old = *f;
  (void)L.delete_file(old.name);
  L.put_file(str(a[1]), old.text);
  return {kT()};
}
Value f_file_size(Args& a, Interp& L) {
  arity(a, 1);
  const Interp::File* f = L.find_file(str(a[0]));
  return f == nullptr ? Value() : int_of(static_cast<double>(units(f->text)));
}
Value f_filename_base(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::string p = slashes(str(a[0]));
  const std::string_view b = last_part(p);
  const std::optional<std::size_t> d = ext_at(b);
  return {std::string(b.substr(0, d.value_or(b.size())))};
}
Value f_filename_extension(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::string p = slashes(str(a[0]));
  const std::string_view b = last_part(p);
  const std::optional<std::size_t> d = ext_at(b);
  return d ? Value(std::string(b.substr(*d))) : Value();
}
Value f_filename_directory(Args& a, Interp& /*L*/) {
  arity(a, 1);
  const std::string& s = str(a[0]);
  const std::size_t i = s.find_last_of("/\\");
  return {i == std::string::npos ? std::string() : s.substr(0, i)};
}
Task<Value> f_load(Args a, Interp& L) {
  arity(a, 1, 2);
  const std::string& name = str(a[0]);
  const Interp::File* f = L.find_file(name);
  if (f == nullptr) {
    f = L.find_file(name + ".lsp");
  }
  if (f == nullptr) {
    if (a.size() > 1) {
      co_return a[1];
    }
    fail("LOAD failed: " + show(a[0]));
  }
  std::vector<Value> forms = read_all(f->text);  // read before anything runs: a form may change the files
  Value r;
  for (Value& form : forms) {
    r = co_await L.eval(std::move(form));
  }
  co_return r;
}

}  // namespace

void install_builtins(Interp& L) {
  L.set(sym("PI"), Value(std::numbers::pi));
  L.set(sym("PAUSE"), Value(std::string("\\")));
  // arithmetic
  L.def("+", &f_add);
  L.def("-", &f_sub);
  L.def("*", &f_mul);
  L.def("/", &f_div);
  L.def("~", &f_not_bits);
  L.def("1+", &f_inc);
  L.def("1-", &f_dec);
  L.def("ABS", &f_abs);
  L.def("FIX", &f_fix);
  L.def("FLOAT", &f_float);
  L.def("REM", &f_rem);
  L.def("GCD", &f_gcd);
  L.def("LCM", &f_lcm);
  L.def("MIN", &f_min);
  L.def("MAX", &f_max);
  L.def("SQRT", &f_sqrt);
  L.def("EXP", &f_exp);
  L.def("LOG", &f_log);
  L.def("EXPT", &f_expt);
  L.def("SIN", &f_sin);
  L.def("COS", &f_cos);
  L.def("ATAN", &f_atan);
  L.def("MINUSP", &f_minusp);
  L.def("ZEROP", &f_zerop);
  L.def("NUMBERP", &f_numberp);
  L.def("BOOLE", &f_boole);
  L.def("LOGAND", &f_logand);
  L.def("LOGIOR", &f_logior);
  L.def("LSH", &f_lsh);
  // comparison
  L.def("=", &f_eqn);
  L.def("/=", &f_neq);
  L.def("<", &f_rel<Rel::lt>);
  L.def(">", &f_rel<Rel::gt>);
  L.def("<=", &f_rel<Rel::le>);
  L.def(">=", &f_rel<Rel::ge>);
  L.def("EQ", &f_eq);
  L.def("EQUAL", &f_equal);
  L.def("NOT", &f_not);
  L.def("NULL", &f_not);
  L.def("ATOM", &f_atom);
  L.def("LISTP", &f_listp);
  L.def("BOUNDP", &f_boundp);
  L.def("TYPE", &f_type);
  // lists
  def_cxr(L, std::make_index_sequence<kPaths.size()>{});
  L.def("CONS", &f_cons);
  L.def("LIST", &f_list);
  L.def("APPEND", &f_append);
  L.def("REVERSE", &f_reverse);
  L.def("LENGTH", &f_length);
  L.def("NTH", &f_nth);
  L.def("LAST", &f_last);
  L.def("MEMBER", &f_member);
  L.def("ASSOC", &f_assoc);
  L.def("SUBST", &f_subst);
  L.def("ACAD_STRLSORT", &f_strlsort);
  // functions
  L.def("APPLY", &f_apply);
  L.def("MAPCAR", &f_mapcar);
  L.def("EVAL", &f_eval);
  L.def("SET", &f_set);
  L.def("READ", &f_read);
  L.def("ATOMS-FAMILY", &f_atoms_family);
  L.def("EXIT", &f_exit);
  L.def("QUIT", &f_exit);
  for (const std::string_view n :
       {"GC", "TRACE", "UNTRACE", "VL-LOAD-COM", "VL-LOAD-ALL", "VL-LOAD-REACTORS", "VL-ACAD-DEFUN", "VL-ACAD-UNDEFUN",
        "VL-PROPAGATE", "VL-ARX-IMPORT", "VL-DOC-EXPORT", "VL-DOC-IMPORT", "ACAD-PUSH-DBMOD", "ACAD-POP-DBMOD"}) {
    L.def(n, &f_nothing);
  }
  L.def("VER", &f_ver);
  // strings
  L.def("STRCAT", &f_strcat);
  L.def("STRLEN", &f_strlen);
  L.def("SUBSTR", &f_substr);
  L.def("STRCASE", &f_strcase);
  L.def("ASCII", &f_ascii);
  L.def("CHR", &f_chr);
  L.def("ITOA", &f_itoa);
  L.def("ATOI", &f_atoi);
  L.def("ATOF", &f_atof);
  L.def("WCMATCH", &f_wcmatch);
  L.def("RTOS", &f_rtos);
  L.def("ANGTOS", &f_angtos);
  L.def("DISTOF", &f_distof);
  L.def("ANGTOF", &f_angtof);
  L.def("CVUNIT", &f_cvunit);
  // output
  L.def("PRINC", &f_princ);
  L.def("PRIN1", &f_prin1);
  L.def("PRINT", &f_print);
  L.def("TERPRI", &f_terpri);
  L.def("PROMPT", &f_prompt);
  // geometry
  L.def("POLAR", &f_polar);
  L.def("DISTANCE", &f_distance);
  L.def("ANGLE", &f_angle);
  L.def("INTERS", &f_inters);
  L.def("TRANS", &f_trans);
  // environment
  L.def("GETENV", &f_getenv);
  L.def("SETENV", &f_setenv);
  // files
  L.def("FINDFILE", &f_findfile);
  L.def("OPEN", &f_open);
  L.def("CLOSE", &f_close);
  L.def("READ-LINE", &f_read_line);
  L.def("READ-CHAR", &f_read_char);
  L.def("WRITE-LINE", &f_write_line);
  L.def("WRITE-CHAR", &f_write_char);
  L.def("VL-FILENAME-MKTEMP", &f_mktemp);
  L.def("VL-FILE-DELETE", &f_file_delete);
  L.def("VL-FILE-RENAME", &f_file_rename);
  L.def("VL-FILE-SIZE", &f_file_size);
  L.def("VL-FILE-DIRECTORY-P", &f_nothing);
  L.def("VL-FILENAME-BASE", &f_filename_base);
  L.def("VL-FILENAME-EXTENSION", &f_filename_extension);
  L.def("VL-FILENAME-DIRECTORY", &f_filename_directory);
  L.def("LOAD", &f_load);
}

}  // namespace cc::lisp::detail
