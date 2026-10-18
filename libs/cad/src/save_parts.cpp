// SPDX-License-Identifier: Apache-2.0
// doc.js colours, dxf.js unit codes, hatch.js patterns, dwg-vports.js records, pictures.js placements, xclip.js filters
// and xref.js roots, from CadCraft 0.5 (the dwg-props.js write side is in props.cpp, next to its tables).
#include "cc/cad/save_parts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/files.hpp"
#include "util.hpp"

namespace cc::cad {

using base::Array;
using base::Value;
using detail::g;
using detail::num;
using detail::str;

namespace {

/// parseInt(s, 16) of a short string: the leading hex digits, NaN when there are none.
double parse_hex(std::string_view s) {
  double v = 0;
  bool any = false;
  for (const char c : s) {
    int d = -1;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    }
    if (d < 0) {
      break;
    }
    v = v * 16 + d;
    any = true;
  }
  return any ? v : detail::kNaN;
}

std::string hsv_hex(double h, double s, double v) {
  const auto f = [&](double n) {
    const double k = std::fmod(n + h / 60, 6);
    return v - v * s * std::max(0.0, std::min({k, 4 - k, 1.0}));
  };
  std::string out = "#";
  for (const double x : {f(5), f(3), f(1)}) {
    const std::string hx = base::hex_lower(static_cast<std::uint64_t>(base::js_round(x * 255)));
    out += (hx.size() < 2 ? "0" : "") + hx;
  }
  return out;
}

Value line_fam(double angle, double x, double y, double dx, double dy, Array dashes = {}) {
  return base::make_object(
      {{"angle", angle}, {"x", x}, {"y", y}, {"dx", dx}, {"dy", dy}, {"dashes", std::move(dashes)}});
}

const Value& patterns() {
  static const Value kV = [] {
    const auto P = [](std::initializer_list<Value> lines) { return base::make_object({{"lines", Array(lines)}}); };
    return base::make_object({
        {"ANSI31", P({line_fam(45, 0, 0, 0, 3.175)})},
        {"ANSI32", P({line_fam(45, 0, 0, 0, 9.525), line_fam(45, 4.490128, 0, 0, 9.525)})},
        {"ANSI33", P({line_fam(45, 0, 0, 0, 6.35), line_fam(45, 4.490128, 0, 0, 6.35, {3.175, -1.5875})})},
        {"ANSI34", P({line_fam(45, 0, 0, 0, 19.05), line_fam(45, 4.490128, 0, 0, 19.05),
                      line_fam(45, 8.980256, 0, 0, 19.05), line_fam(45, 13.470384, 0, 0, 19.05)})},
        {"ANSI35", P({line_fam(45, 0, 0, 0, 6.35), line_fam(45, 4.490128, 0, 0, 6.35, {7.9375, -1.5875, 0, -1.5875})})},
        {"ANSI36", P({line_fam(45, 0, 0, 5.55625, 3.175, {7.9375, -1.5875, 0, -1.5875})})},
        {"ANSI37", P({line_fam(45, 0, 0, 0, 3.175), line_fam(135, 0, 0, 0, 3.175)})},
        {"ANSI38", P({line_fam(45, 0, 0, 0, 3.175), line_fam(135, 0, 0, 6.35, 3.175, {7.9375, -4.7625})})},
        {"LINE", P({line_fam(0, 0, 0, 0, 3.175)})},
        {"NET", P({line_fam(0, 0, 0, 0, 3.175), line_fam(90, 0, 0, 0, 3.175)})},
        {"NET3", P({line_fam(0, 0, 0, 0, 3.175), line_fam(60, 0, 0, 0, 3.175), line_fam(120, 0, 0, 0, 3.175)})},
        {"DASH", P({line_fam(0, 0, 0, 3.175, 3.175, {3.175, -3.175})})},
        {"SQUARE", P({line_fam(0, 0, 0, 0, 3.175, {3.175, -3.175}), line_fam(90, 0, 0, 0, 3.175, {3.175, -3.175})})},
        {"BRICK", P({line_fam(0, 0, 0, 0, 6.35), line_fam(90, 0, 0, 6.35, 6.35, {6.35, -6.35}),
                     line_fam(90, 3.175, 0, 6.35, 6.35, {-6.35, 6.35})})},
        {"DOTS", P({line_fam(0, 0, 0, 0.79375, 1.5875, {0, -1.5875})})},
        {"EARTH", P({line_fam(0, 0, 0, 6.35, 6.35, {6.35, -6.35}), line_fam(0, 0, 2.38125, 6.35, 6.35, {6.35, -6.35}),
                     line_fam(0, 0, 4.7625, 6.35, 6.35, {6.35, -6.35}),
                     line_fam(90, 0.79375, 5.55625, 6.35, 6.35, {6.35, -6.35}),
                     line_fam(90, 3.175, 5.55625, 6.35, 6.35, {6.35, -6.35}),
                     line_fam(90, 5.55625, 5.55625, 6.35, 6.35, {6.35, -6.35})})},
    });
  }();
  return kV;
}

double finite_or0(const Value& x) {
  return detail::finite(x) ? x.as_number() : 0;
}

double clamp_split(const Value& f) {
  return detail::finite(f) ? std::max(0.1, std::min(0.9, f.as_number())) : 0.5;
}

}  // namespace

// ---------------------------------------------------------------- colours, units

std::string aci_hex(double i) {
  static constexpr std::array<std::string_view, 9> kBase = {"#ff0000", "#ffff00", "#00ff00", "#00ffff", "#0000ff",
                                                            "#ff00ff", "#ffffff", "#808080", "#c0c0c0"};
  static constexpr std::array<std::string_view, 6> kGrays = {"#333333", "#505050", "#696969",
                                                             "#828282", "#bebebe", "#ffffff"};
  if (i == 7) {
    return "ink";
  }
  if (i >= 1 && i <= 9 && i == std::floor(i)) {
    return std::string(kBase.at(static_cast<std::size_t>(i) - 1));
  }
  if (i >= 250 && i <= 255 && i == std::floor(i)) {
    return std::string(kGrays.at(static_cast<std::size_t>(i) - 250));
  }
  if (i < 10 || i > 249) {
    return "ink";
  }
  const double hue = std::floor((i - 10) / 10) * 15;
  const double j = std::fmod(i - 10, 10);
  static constexpr std::array<double, 10> kV = {1, 1, 0.8, 0.8, 0.6, 0.6, 0.5, 0.5, 0.3, 0.3};
  const double v = kV.at(static_cast<std::size_t>(j));
  const double s = std::fmod(j, 2) == 0 ? 1 : 0.5;
  return hsv_hex(hue, s, v);
}

int nearest_aci(std::string_view hex) {
  const auto rgb = [](std::string_view h) {
    std::array<double, 3> c{};
    for (std::size_t k = 0; k < 3; ++k) {
      const std::size_t i = 1 + 2 * k;
      c.at(k) = parse_hex(i < h.size() ? h.substr(i, 2) : std::string_view());
    }
    return c;
  };
  const auto [r, g0, b] = rgb(hex);
  int best = 7;
  double bd = std::numeric_limits<double>::infinity();
  for (int i = 1; i <= 255; ++i) {
    const std::string a = aci_hex(i);
    const auto [r2, g2, b2] = rgb(a == "ink" ? std::string("#ffffff") : a);
    const double d = (r - r2) * (r - r2) + (g0 - g2) * (g0 - g2) + (b - b2) * (b - b2);
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

int unit_code(std::string_view u) {
  static constexpr std::array<std::string_view, 10> kNames = {
      "inches", "feet", "miles", "millimeters", "centimeters", "meters", "kilometers", "microinches", "mils", "yards"};
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    if (kNames.at(i) == u) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- hatch.js

const Value* hatch_pattern(std::string_view upper_name) {
  const Value* p = patterns().find(upper_name);
  return p;
}

bool hatch_is_metric(const Value& e, const Value& meta) {
  const Value piso = g(e, "piso");
  if (piso.is_bool()) {
    return piso.as_bool();
  }
  const Value u = g(meta, "units");
  const Value lu = g(meta, "lunits");
  return !(u == Value("inches") || u == Value("feet") || lu == Value(3) || lu == Value(4));
}

// ---------------------------------------------------------------- dwg-vports.js

Array vport_records(const Value& vports, const Value& fallback, double twist) {
  const Value v = clean_panes(vports);
  Value fb;
  if (fallback.truthy() && finite_or0(g(fallback, "h")) > 0) {
    fb = base::make_object(
        {{"cx", finite_or0(g(fallback, "cx"))}, {"cy", finite_or0(g(fallback, "cy"))}, {"h", g(fallback, "h")}});
  } else {
    fb = base::make_object({{"cx", nullptr}, {"cy", nullptr}, {"h", nullptr}});
  }
  constexpr double kAspect = 1.6;
  const auto with = [](Value o, const Value& more) {
    for (const base::Member& m : more.as_object()) {
      o.set(m.key, m.value);
    }
    return o;
  };
  if (!v.truthy()) {
    Value r = with(base::make_object({{"x0", 0}, {"y0", 0}, {"x1", 1}, {"y1", 1}}), fb);
    r.set("aspect", kAspect);
    r.set("tw", std::isfinite(twist) ? twist : 0);
    return base::array_of(std::move(r));
  }
  const double X = clamp_split(g(v, "fx"));
  const double Y = clamp_split(g(v, "fy"));
  const double yu = 1 - Y;
  const auto C = [](double x0, double y0, double x1, double y1) {
    return base::make_object({{"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}});
  };
  const Value layout = g(v, "layout");
  Array corners;
  if (layout == Value("2v")) {
    corners = {C(0, 0, X, 1), C(X, 0, 1, 1)};
  } else if (layout == Value("2h")) {
    corners = {C(0, yu, 1, 1), C(0, 0, 1, yu)};
  } else if (layout == Value("3r")) {
    corners = {C(0, yu, X, 1), C(0, 0, X, yu), C(X, 0, 1, 1)};
  } else if (layout == Value("3l")) {
    corners = {C(0, 0, X, 1), C(X, yu, 1, 1), C(X, 0, 1, yu)};
  } else if (layout == Value("4")) {
    corners = {C(0, yu, X, 1), C(X, yu, 1, 1), C(0, 0, X, yu), C(X, 0, 1, yu)};
  } else {
    corners = {C(0, 0, 1, 1)};
  }
  const Value list = g(v, "list");
  Array recs;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const Value& c = corners[i];
    const Value p = detail::item(list, i);
    const bool own = p.truthy() && !g(p, "sheet").truthy();
    const Value view = own ? base::make_object({{"cx", g(p, "cx")}, {"cy", g(p, "cy")}, {"h", g(p, "h")}}) : fb;
    Value r = with(c, view);
    r.set("aspect", ((num(c["x1"]) - num(c["x0"])) / (num(c["y1"]) - num(c["y0"]))) * kAspect);
    r.set("tw", own ? finite_or0(g(p, "tw")) : 0);
    if (p.truthy() && g(p, "sheet").truthy()) {
      r.set("sheet", g(p, "sheet"));
    }
    recs.push_back(std::move(r));
  }
  const auto a = static_cast<std::size_t>(num(g(v, "active")));
  Array out;
  if (a < recs.size()) {
    out.push_back(recs[a]);
  } else {
    out.emplace_back();  // recs[a] of an index past the end: undefined
  }
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (i != a) {
      out.push_back(recs[i]);
    }
  }
  return out;
}

// ---------------------------------------------------------------- pictures.js, xclip.js, xref.js

std::string picture_key(const Value& file) {
  const std::string s = file.truthy() ? str(file) : std::string();
  const std::size_t k = s.find_last_of("\\/");
  return base::js_to_lower(k == std::string::npos ? s : s.substr(k + 1));
}

Value picture_placement(const Value& ref, const Value& block) {
  const Value ents = g(block, "entities");
  if (!ents.is_array()) {
    return {nullptr};
  }
  const Value* fr = nullptr;
  for (const Value& e : ents.as_array()) {
    if (e["type"] == Value("poly") && e["pic"].truthy()) {
      fr = &e;
      break;
    }
  }
  if (fr == nullptr) {
    return {nullptr};
  }
  for (const Value& e : ents.as_array()) {
    if (&e != fr && !e["picName"].truthy()) {
      return {nullptr};
    }
  }
  const Value p = (*fr)["pic"];
  const detail::Mat m = detail::insert_matrix(ref, block);
  const double px = num(g(p, "x"));
  const double py = num(g(p, "y"));
  const double ax = m[0] * px + m[2] * py + m[4];
  const double ay = m[1] * px + m[3] * py + m[5];
  const auto vec = [&](double x, double y) { return std::pair{m[0] * x + m[2] * y, m[1] * x + m[3] * y}; };
  const auto [ux, uy] = vec(num(g(p, "ux")), num(g(p, "uy")));
  const auto [vx, vy] = vec(num(g(p, "vx")), num(g(p, "vy")));
  const Value fade = g(p, "fade");
  Value out = base::make_object({{"file", g(p, "file")},
                                 {"insert", base::pt2(ax, ay)},
                                 {"u", base::pt2(ux, uy)},
                                 {"v", base::pt2(vx, vy)},
                                 {"size", base::make_object({{"x", g(p, "w")}, {"y", g(p, "h")}})},
                                 {"fade", detail::finite(fade) ? fade : Value(0)}});
  if (g(p, "clipped").truthy()) {
    const double det = ux * vy - uy * vx;
    if (std::abs(det) > 1e-300) {
      Array clip;
      const double h = num(g(p, "h"));
      for (const Value& q : (*fr)["pts"].as_array()) {
        const double qx = num(g(q, "x"));
        const double qy = num(g(q, "y"));
        const double wx = m[0] * qx + m[2] * qy + m[4];
        const double wy = m[1] * qx + m[3] * qy + m[5];
        const double dx = wx - ax;
        const double dy = wy - ay;
        const double a = (dx * vy - dy * vx) / det;
        const double b = (ux * dy - uy * dx) / det;
        clip.push_back(base::pt2(a - 0.5, h - 0.5 - b));
      }
      out.set("clip", std::move(clip));
    }
  }
  return out;
}

Value clip_poly(const Value& ref) {
  const Value clip = g(ref, "clip");
  const Value pts = clip.truthy() ? g(clip, "pts") : Value();
  Array out;
  if (pts.is_array()) {
    for (const Value& p : pts.as_array()) {
      if (p.truthy() && detail::finite(g(p, "x")) && detail::finite(g(p, "y"))) {
        out.push_back(p);
      }
    }
  }
  return out.size() >= 3 ? Value(std::move(out)) : Value(nullptr);
}

Value filter_of(const Value& pts) {
  const Array id = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  Array points;
  for (const Value& p : pts.as_array()) {
    points.push_back(base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}}));
  }
  return base::make_object({{"type", "SPATIAL_FILTER"},
                            {"points", std::move(points)},
                            {"extrusion", base::pt3(0, 0, 1)},
                            {"origin", base::pt3(0, 0, 0)},
                            {"displayBoundary", 1},
                            {"frontClip", nullptr},
                            {"backClip", nullptr},
                            {"inverseBlockTransform", id},
                            {"clipTransform", id}});
}

bool is_xref_root(const Value& b) {
  const Value x = g(b, "xref");
  return b.truthy() && x.truthy() && g(x, "of").is_undefined();
}

}  // namespace cc::cad
