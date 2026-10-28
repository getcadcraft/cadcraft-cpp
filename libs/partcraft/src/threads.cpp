// SPDX-License-Identifier: Apache-2.0
// threads.js (PartCraft 0.5, WL.29): thread tables, profiles, the helical cross-section and callouts.
#include "cc/partcraft/threads.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsx.hpp"

namespace cc::partcraft::threads {
namespace {

constexpr double PI = std::numbers::pi;

struct Row {
  std::string_view size;
  double d, p;
};
constexpr std::array<Row, 29> kIso = {{
    {"M1.6", 1.6, 0.35}, {"M2", 2, 0.4},   {"M2.5", 2.5, 0.45}, {"M3", 3, 0.5},   {"M3.5", 3.5, 0.6}, {"M4", 4, 0.7},
    {"M5", 5, 0.8},      {"M6", 6, 1},     {"M8", 8, 1.25},     {"M10", 10, 1.5}, {"M12", 12, 1.75},  {"M14", 14, 2},
    {"M16", 16, 2},      {"M18", 18, 2.5}, {"M20", 20, 2.5},    {"M22", 22, 2.5}, {"M24", 24, 3},     {"M27", 27, 3},
    {"M30", 30, 3.5},    {"M33", 33, 3.5}, {"M36", 36, 4},      {"M39", 39, 4},   {"M42", 42, 4.5},   {"M45", 45, 4.5},
    {"M48", 48, 5},      {"M52", 52, 5},   {"M56", 56, 5.5},    {"M60", 60, 5.5}, {"M64", 64, 6},
}};
constexpr std::array<Row, 19> kTr = {{
    {"Tr8x1_5", 8, 1.5}, {"Tr10x2", 10, 2}, {"Tr12x3", 12, 3}, {"Tr14x3", 14, 3}, {"Tr16x4", 16, 4},
    {"Tr18x4", 18, 4},   {"Tr20x4", 20, 4}, {"Tr22x5", 22, 5}, {"Tr24x5", 24, 5}, {"Tr26x5", 26, 5},
    {"Tr28x5", 28, 5},   {"Tr30x6", 30, 6}, {"Tr32x6", 32, 6}, {"Tr36x6", 36, 6}, {"Tr40x7", 40, 7},
    {"Tr44x7", 44, 7},   {"Tr48x8", 48, 8}, {"Tr52x8", 52, 8}, {"Tr60x9", 60, 9},
}};

std::span<const Row> table(std::string_view form) {
  if (form == "iso") {
    return kIso;
  }
  if (form == "tr") {
    return kTr;
  }
  return {};
}

/// Names every plain object answers through its prototype (THREAD_TABLES[form][size] finds them).
bool inherited(std::string_view k) {
  constexpr std::array<std::string_view, 12> kProto{
      "constructor",      "hasOwnProperty",   "isPrototypeOf",    "propertyIsEnumerable",
      "toLocaleString",   "toString",         "valueOf",          "__proto__",
      "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"};
  return std::ranges::find(kProto, k) != kProto.end();
}

std::string fmt(double v) {
  return base::js_string(base::Value(base::js_number(base::js_to_fixed(v, 3))));
}

}  // namespace

base::Result<std::optional<Dims>> thread_dims(std::string_view form, std::string_view size) {
  const auto rows = table(form);
  if (form == "iso" || form == "tr") {
    if (inherited(size)) {
      return base::fail(base::Errc::invalid, "row is not iterable");
    }
  }
  for (const Row& r : rows) {
    if (r.size == size) {
      const double depth = form == "tr" ? r.p / 2 : 5 * std::numbers::sqrt3 / 16 * r.p;
      return Dims{.d = r.d,
                  .p = r.p,
                  .d1 = r.d - 2 * depth,
                  .d2 = form == "tr" ? r.d - r.p / 2 : r.d - 0.649519 * r.p,
                  .depth = depth};
    }
  }
  return std::optional<Dims>{};
}

std::vector<std::string_view> sizes(std::string_view form) {
  std::vector<std::string_view> out;
  for (const Row& r : table(form)) {
    out.push_back(r.size);
  }
  return out;
}

std::string size_label(std::string_view form, std::string_view size) {
  std::string s(size);
  if (form != "tr") {
    return s;
  }
  if (const auto x = s.find('x'); x != std::string::npos) {
    s.replace(x, 1, "×");
  }
  if (const auto u = s.find('_'); u != std::string::npos) {
    s.replace(u, 1, ".");
  }
  return s;
}

std::optional<std::string_view> size_for(std::string_view form, double dia, bool external) {
  std::optional<std::string_view> best;
  double bd = std::numeric_limits<double>::infinity();
  for (const Row& r : table(form)) {
    const auto dims = thread_dims(form, r.size);
    if (!dims || !*dims) {
      continue;
    }
    const Dims g = **dims;
    const double want = external ? g.d : (g.d1 + g.d2) / 2;
    const double dd = std::abs(dia - want);
    if (dd < bd) {
      bd = dd;
      best = r.size;
    }
  }
  if (best && bd <= 0.12 * dia) {
    return best;
  }
  return std::nullopt;
}

base::Result<std::optional<Corners>> profile_corners(std::string_view form, std::string_view size, double shift) {
  auto g0 = thread_dims(form, size);
  if (!g0) {
    return std::unexpected(g0.error());
  }
  if (!*g0) {
    return std::optional<Corners>{};
  }
  const Dims& g = **g0;
  const double R = g.d / 2 + shift;
  const double r = g.d1 / 2 + shift;
  const double p = g.p;
  const double crest = form == "tr" ? p * (0.5 - std::tan(PI / 12) / 2) : p / 8;
  const double root = form == "tr" ? crest : p / 4;
  const double flank = (p - crest - root) / 2;
  return Corners{{{0, R}, {crest, R}, {crest + flank, r}, {crest + flank + root, r}, {p, R}}};
}

double profile_at(const Corners& corners, double u) {
  const double p = corners.back()[0];
  const double w = std::fmod(std::fmod(u, p) + p, p);
  for (std::size_t k = 1; k < corners.size(); ++k) {
    const auto [u0, r0] = corners[k - 1];
    const auto [u1, r1] = corners[k];
    if (w <= u1 + 1e-15) {
      return u1 - u0 < 1e-15 ? r1 : r0 + (r1 - r0) * (w - u0) / (u1 - u0);
    }
  }
  return corners[0][1];
}

std::vector<std::array<double, 2>> bolt_section(const Corners& corners, bool left, double segments) {
  const double p = corners.back()[0];
  const double s = left ? 1 : -1;
  std::vector<double> th{0, 2 * PI};
  const auto add = [&](double a) {
    if (std::ranges::find(th, a) == th.end()) {  // a Set: 0 and -0 are one value
      th.push_back(a);
    }
  };
  for (const auto& c : corners) {
    add(std::fmod(std::fmod(s * c[0] / p, 1) + 1, 1) * 2 * PI);
  }
  const double n = jsx::max(24, segments);
  for (std::int64_t k = 1; static_cast<double>(k) < n; ++k) {
    add(2 * PI * static_cast<double>(k) / n);
  }
  std::ranges::stable_sort(th);
  std::vector<double> list;
  for (std::size_t i = 0; i < th.size(); ++i) {
    if (i == 0 || th[i] - th[i - 1] > 1e-9) {
      list.push_back(th[i]);
    }
  }
  if (2 * PI - list.back() < 1e-9) {
    list.pop_back();
  }
  std::vector<std::array<double, 2>> out;
  out.reserve(list.size());
  for (const double a : list) {
    const double r = profile_at(corners, s * p * a / (2 * PI));
    out.push_back({r * base::js_cos(a), r * base::js_sin(a)});
  }
  return out;
}

base::Result<std::string> thread_callout(std::string_view form, std::string_view size, const CalloutOptions& o,
                                         const i18n::Catalog& w) {
  auto g = thread_dims(form, size);
  if (!g) {
    return std::unexpected(g.error());
  }
  if (!*g) {
    return std::string();
  }
  const std::string name = form == "tr" ? size_label(form, size) : std::string(size) + "×" + fmt((*g)->p);
  std::string cls = o.external ? "6g" : "6H";
  if (form == "tr") {
    cls = o.external ? "7e" : "7H";
  }
  std::string_view key = "thread.callout.deep";
  if (o.through) {
    key = "thread.callout.through";
  } else if (o.external) {
    key = "thread.callout.long";
  }
  const std::string text = w.t(key, {{"size", name}, {"cls", cls}, {"length", fmt(o.length)}});
  return o.modelled ? w.t("thread.callout.modelled", {{"callout", text}}) : text;
}

}  // namespace cc::partcraft::threads
