// SPDX-License-Identifier: Apache-2.0
// propexpr.js (CadCraft 0.5) in C++: what a person types into a number field of the Properties panel, read as a value
// — lengths in the drawing's format or with a unit, angles in degrees / d°m's" / radians / grads, and expressions
// (expr.js) with or without "=". A bad entry is refused with the reason, never read as 0.
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/cmd/props.hpp"

namespace cc::cmd {

namespace {

bool digit(char c) {
  return c >= '0' && c <= '9';
}
bool word(char c) {
  return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
/// NUM = \d+(?:\.\d*)?|\.\d+ at i → its end, or npos
std::size_t num_at(std::string_view s, std::size_t i) {
  std::size_t j = i;
  while (j < s.size() && digit(s[j])) {
    ++j;
  }
  if (j > i) {
    if (j < s.size() && s[j] == '.') {
      ++j;
      while (j < s.size() && digit(s[j])) {
        ++j;
      }
    }
    return j;
  }
  if (i < s.size() && s[i] == '.' && i + 1 < s.size() && digit(s[i + 1])) {
    j = i + 1;
    while (j < s.size() && digit(s[j])) {
      ++j;
    }
    return j;
  }
  return std::string_view::npos;
}
std::size_t skip_ws(std::string_view s, std::size_t i) {
  while (i < s.size() &&
         (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) {
    ++i;
  }
  return i;
}
double numv(std::string_view s) {
  return base::js_number(s);
}

/// FT_IN: (NUM)\s*'\s*-?\s*(?:(\d+)(?:[\s-]+(\d+)\/(\d+))?|(\d+)\/(\d+)|(NUM))?\s*"? at i → its end and the mm
struct FtIn {
  std::size_t end;
  std::optional<double> mm;  // nothing: leave the text as it is (a fraction over 0)
};
std::optional<FtIn> ft_in_at(std::string_view s, std::size_t i) {
  const std::size_t a = num_at(s, i);
  if (a == std::string_view::npos) {
    return std::nullopt;
  }
  const double ft = numv(s.substr(i, a - i));
  std::size_t j = skip_ws(s, a);
  if (j >= s.size() || s[j] != '\'') {
    return std::nullopt;
  }
  ++j;
  std::size_t k = skip_ws(s, j);
  if (k < s.size() && s[k] == '-') {
    ++k;
  }
  k = skip_ws(s, k);
  double inch = 0;
  bool bad = false;
  std::size_t end = k;
  // (\d+)(?:[\s-]+(\d+)\/(\d+))?   |   (\d+)\/(\d+)   |   (NUM)
  std::size_t d1 = k;
  while (d1 < s.size() && digit(s[d1])) {
    ++d1;
  }
  bool matched = false;
  if (d1 > k) {
    // whole inches, maybe a fraction after a space or dash
    std::size_t f = d1;
    while (f < s.size() && (s[f] == ' ' || s[f] == '-' || s[f] == '\t')) {
      ++f;
    }
    std::size_t n1 = f;
    while (n1 < s.size() && digit(s[n1])) {
      ++n1;
    }
    if (f > d1 && n1 > f && n1 < s.size() && s[n1] == '/') {
      std::size_t n2 = n1 + 1;
      while (n2 < s.size() && digit(s[n2])) {
        ++n2;
      }
      if (n2 > n1 + 1) {
        const double fd = numv(s.substr(n1 + 1, n2 - n1 - 1));
        bad = fd == 0;
        inch = numv(s.substr(k, d1 - k)) + numv(s.substr(f, n1 - f)) / fd;
        end = n2;
        matched = true;
      }
    }
    // (\d+)\/(\d+) is never reached: the first alternative's (\d+) already matched, and the regex keeps it
    if (!matched) {
      inch = numv(s.substr(k, d1 - k));
      end = d1;
    }
  } else if (const std::size_t n = num_at(s, k); n != std::string_view::npos) {
    inch = numv(s.substr(k, n - k));  // a decimal inch (.5)
    end = n;
  } else {
    end = j;  // feet alone
  }
  std::size_t q = skip_ws(s, end);
  if (q < s.size() && s[q] == '"') {
    end = q + 1;
  }
  if (bad) {
    return FtIn{end, std::nullopt};
  }
  return FtIn{end, ft * 304.8 + inch * 25.4};
}

/// WITH_UNIT: (NUM)\s*(mm|cm|m|in|ft|"|')(?![A-Za-z0-9_]) at i → its end and the mm
std::optional<std::pair<std::size_t, double>> unit_at(std::string_view s, std::size_t i) {
  const std::size_t a = num_at(s, i);
  if (a == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t u = skip_ws(s, a);
  static constexpr std::array<std::pair<std::string_view, double>, 7> kSuffix{
      {{"mm", 1}, {"cm", 10}, {"m", 1000}, {"in", 25.4}, {"ft", 304.8}, {"\"", 25.4}, {"'", 304.8}}};
  for (const auto& [text, mm] : kSuffix) {
    if (s.substr(u, text.size()) != text) {
      continue;
    }
    const std::size_t after = u + text.size();
    if (after < s.size() && word(s[after])) {
      continue;
    }
    return std::pair{after, numv(s.substr(i, a - i)) * mm};
  }
  return std::nullopt;
}

/// lengthsToUnits(src, mmU): feet-inch and unit-suffixed literals → drawing units, "(…)"; used: a unit was converted
std::pair<std::string, bool> lengths_to_units(std::string_view src, std::optional<double> mm_u) {
  bool used = false;
  const auto put = [&](double mm) {
    used = true;
    return mm_u ? "(" + edit_text(mm / *mm_u) + ")" : std::string("(0)");
  };
  // pass 1: feet and inches (only where a ' is in the match)
  std::string s1;
  for (std::size_t i = 0; i < src.size();) {
    // the JS regexes are global and unanchored: a match is tried at every position
    if (const auto f = ft_in_at(src, i)) {
      s1 += f->mm ? put(*f->mm) : std::string(src.substr(i, f->end - i));
      i = f->end;
      continue;
    }
    s1 += src[i++];
  }
  // pass 2: a number with a unit
  std::string s2;
  for (std::size_t i = 0; i < s1.size();) {
    if (const auto u = unit_at(s1, i)) {
      s2 += put(u->second);
      i = u->first;
      continue;
    }
    s2 += s1[i++];
  }
  return {s2, used};
}

/// DMS: (NUM)\s*[d°]\s*(?:(NUM)\s*')?\s*(?:(NUM)\s*")? at i → its end and the degrees
std::optional<std::pair<std::size_t, double>> dms_at(std::string_view s, std::size_t i) {
  const std::size_t a = num_at(s, i);
  if (a == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t j = skip_ws(s, a);
  if (j < s.size() && s[j] == 'd') {
    ++j;
  } else if (s.substr(j, 2) == "\xC2\xB0") {
    j += 2;
  } else {
    return std::nullopt;
  }
  double v = numv(s.substr(i, a - i));
  std::size_t k = skip_ws(s, j);
  std::size_t end = j;
  if (const std::size_t m = num_at(s, k); m != std::string_view::npos) {
    const std::size_t q = skip_ws(s, m);
    if (q < s.size() && s[q] == '\'') {
      v += numv(s.substr(k, m - k)) / 60;
      end = q + 1;
      k = skip_ws(s, end);
    }
  }
  if (const std::size_t sec = num_at(s, k); sec != std::string_view::npos) {
    const std::size_t q = skip_ws(s, sec);
    if (q < s.size() && s[q] == '"') {
      v += numv(s.substr(k, sec - k)) / 3600;
      end = q + 1;
    }
  }
  return std::pair{end, v};
}
std::string dms_to_degrees(std::string_view src) {
  std::string out;
  for (std::size_t i = 0; i < src.size();) {
    if (const auto d = dms_at(src, i)) {
      out += "(" + edit_text(d->second) + ")";
      i = d->first;
      continue;
    }
    out += src[i++];
  }
  return out;
}

/// PLAIN: ^[-+]?NUM$
bool plain(std::string_view s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    ++i;
  }
  const std::size_t e = num_at(s, i);
  return e != std::string_view::npos && e == s.size();
}
/// ^[-+]?\d+(\.\d+)?e[-+]?\d+$ (case-insensitive)
bool sci(std::string_view s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    ++i;
  }
  const std::size_t d0 = i;
  while (i < s.size() && digit(s[i])) {
    ++i;
  }
  if (i == d0) {
    return false;
  }
  if (i < s.size() && s[i] == '.') {
    const std::size_t f0 = ++i;
    while (i < s.size() && digit(s[i])) {
      ++i;
    }
    if (i == f0) {
      return false;
    }
  }
  if (i >= s.size() || (s[i] != 'e' && s[i] != 'E')) {
    return false;
  }
  ++i;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    ++i;
  }
  const std::size_t x0 = i;
  while (i < s.size() && digit(s[i])) {
    ++i;
  }
  return i > x0 && i == s.size();
}

ReadResult finish(double v, std::string_view kind, const ReadCtx& ctx, const i18n::Catalog& w) {
  if (!std::isfinite(v)) {
    return {.ok = false, .value = 0, .error = w.t("props.err.number")};
  }
  if (kind == "int") {
    if (std::abs(v - base::js_round(v)) > 1e-9) {
      return {.ok = false, .value = 0, .error = w.t("props.err.int")};
    }
    v = base::js_round(v);
  }
  if (ctx.gt0 && !(v > 0)) {
    return {.ok = false, .value = 0, .error = w.t("props.err.gt0")};
  }
  if (ctx.min && v < *ctx.min - 1e-12) {
    return {.ok = false, .value = 0, .error = w.t("props.err.min", {{"min", edit_text(*ctx.min)}})};
  }
  if (ctx.max && v > *ctx.max + 1e-12) {
    return {.ok = false, .value = 0, .error = w.t("props.err.max", {{"max", edit_text(*ctx.max)}})};
  }
  return {.ok = true, .value = v, .error = {}};
}

ReadResult expression(std::string_view src, std::string_view kind, const ReadCtx& ctx, const i18n::Catalog& w) {
  std::string s(src);
  if (kind == "len") {
    const std::optional<double> mm = mm_per_unit(ctx);
    auto [conv, used] = lengths_to_units(s, mm);
    if (used && !mm) {
      return {.ok = false, .value = 0, .error = w.t("props.err.nounits")};
    }
    s = std::move(conv);
  } else if (kind == "ang") {
    s = dms_to_degrees(s);
  }
  const cad::expr::Result r = cad::expr::compute(s, ctx.vars, w);
  if (!r.ok) {
    return {.ok = false, .value = 0, .error = w.t("props.err.expr", {{"why", r.error}})};
  }
  return finish(r.value, kind, ctx, w);
}

}  // namespace

std::optional<double> mm_per_unit(const ReadCtx& ctx) {
  if (!ctx.paper.empty()) {
    return ctx.paper == "in" ? 25.4 : 1;
  }
  static constexpr std::array<std::pair<std::string_view, double>, 10> kUnitMm{{{"millimeters", 1},
                                                                                {"centimeters", 10},
                                                                                {"meters", 1000},
                                                                                {"inches", 25.4},
                                                                                {"feet", 304.8},
                                                                                {"decimeters", 100},
                                                                                {"kilometers", 1e6},
                                                                                {"miles", 1609344},
                                                                                {"yards", 914.4},
                                                                                {"microns", 0.001}}};
  const std::string u = base::js_to_lower(ctx.units);
  for (const auto& [name, mm] : kUnitMm) {
    if (u == name) {
      return mm;
    }
  }
  const double code = base::js_to_number(ctx.lunits);
  const bool imperial = code == 3 || code == 4;  // isImperialFormat(lunitCode(lunits ?? 2))
  return imperial ? std::optional<double>(25.4) : std::nullopt;
}

std::string edit_text(double v) {
  if (!std::isfinite(v)) {
    return {};
  }
  const double r = base::js_number(base::js_to_exponential(v, 11));  // Number(v.toPrecision(12))
  if (r == 0) {
    return "0";
  }
  if (std::abs(r) >= 1e-6 && std::abs(r) < 1e15) {
    const int digits = std::max(0, 11 - static_cast<int>(std::floor(std::log10(std::abs(r)))));
    return base::js_string(base::Value(base::js_number(base::js_to_fixed(r, std::min(digits, 100)))));
  }
  return base::js_string(base::Value(r));
}

ReadResult read_value(std::string_view raw, std::string_view kind, const ReadCtx& ctx, const i18n::Catalog& w) {
  const std::string_view s = base::js_trim(raw);
  if (s.empty()) {
    return {.ok = false, .value = 0, .error = w.t("props.err.empty")};
  }
  if (base::utf16_length(s) > 500) {
    return {.ok = false, .value = 0, .error = w.t("props.err.long")};
  }
  if (s[0] == '=') {
    const std::string_view body = base::js_trim(s.substr(1));
    return body.empty() ? ReadResult{.ok = false, .value = 0, .error = w.t("props.err.empty")}
                        : expression(body, kind, ctx, w);
  }
  if (kind == "len") {
    const auto v = parse_length(s, !ctx.paper.empty() ? base::Value(2) : ctx.lunits);
    if (v) {
      return finish(*v, kind, ctx, w);
    }
  } else if (kind == "ang") {
    if (plain(s)) {
      return finish(base::js_parse_float(s), kind, ctx, w);
    }
    // ^([-+]?)(NUM)\s*(r|rad|g|grad)$ (any case)
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      neg = s[i] == '-';
      ++i;
    }
    if (const std::size_t e = num_at(s, i); e != std::string_view::npos) {
      const std::string u = base::js_to_lower(s.substr(skip_ws(s, e)));
      if (u == "r" || u == "rad" || u == "g" || u == "grad") {
        const double v = base::js_parse_float(s.substr(i, e - i)) * (u[0] == 'r' ? 180 / std::numbers::pi : 0.9);
        return finish(neg ? -v : v, kind, ctx, w);
      }
    }
    // ^([-+]?)\s*DMS$
    std::size_t k = 0;
    bool dneg = false;
    if (k < s.size() && (s[k] == '-' || s[k] == '+')) {
      dneg = s[k] == '-';
      ++k;
    }
    k = skip_ws(s, k);
    if (const auto d = dms_at(s, k); d && d->first == s.size()) {
      return finish(dneg ? -d->second : d->second, kind, ctx, w);
    }
  } else if (plain(s) || sci(s)) {
    return finish(base::js_parse_float(s), kind, ctx, w);
  }
  // not a plain value: an expression typed without "=", or a length with a unit in a decimal drawing
  return expression(s, kind, ctx, w);
}

}  // namespace cc::cmd
