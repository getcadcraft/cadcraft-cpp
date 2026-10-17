// SPDX-License-Identifier: Apache-2.0
// Port of BimCraft dwg-compile.js (CadCraft 0.5). Same traversal, same numbers: JS `||`, `??`, NaN propagation and
// Math.max/min are reproduced where a model field may be missing. Where the JS would hang or overflow its stack on a
// hostile model (an infinite MINSERT count, a dimension block that contains itself) this port stops instead
// (docs/gaps.md).
#include "cc/drawing/compile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/value.hpp"

namespace cc::drawing {

using base::Array;
using base::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;
constexpr double kMaxPrims = 3'000'000;  // safety stop for runaway nesting (the JS MAX_PRIMS)
constexpr double kLwDefault = 0.25;
constexpr int kMaxDepth = 200;  // the JS has no limit on table/dimension/multileader nesting: its stack is the limit
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------- JS value semantics
/// ToNumber as arithmetic applies it.
double num(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::number: return v.as_number();
    case Value::Kind::boolean: return v.as_bool() ? 1 : 0;
    case Value::Kind::null: return 0;
    case Value::Kind::string: {
      const std::string_view t = base::js_trim(v.as_string());
      return t.empty() ? 0 : base::js_parse_float(t);
    }
    default: return kNaN;
  }
}
/// `v || d`
double or_num(const Value& v, double d) {
  return v.truthy() ? num(v) : d;
}
/// `v ?? d`
double nn(const Value& v, double d) {
  return v.is_undefined() || v.is_null() ? d : num(v);
}
/// Math.max / Math.min (NaN wins)
double js_max(double a, double b) {
  return std::isnan(a) || std::isnan(b) ? kNaN : std::max(a, b);
}
double js_min(double a, double b) {
  return std::isnan(a) || std::isnan(b) ? kNaN : std::min(a, b);
}
/// How many times `for (let i = 0; i < n; i++)` runs (NaN and n <= 0: none).
std::size_t loops_below(double n) {
  return n > 0 ? base::clamp_cast<std::size_t>(std::ceil(n)) : 0;
}
/// How many times `for (let i = 0; i <= n; i++)` runs.
std::size_t loops_upto(double n) {
  return n >= 0 ? base::clamp_cast<std::size_t>(std::floor(n)) + 1 : 0;
}
std::int32_t int32(const Value& v) {
  return base::js_to_int32(num(v));
}
/// String(v) for the strings a model holds.
std::string text_of(const Value& v) {
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_undefined()) {
    return "undefined";
  }
  if (v.is_null()) {
    return "null";
  }
  if (v.is_bool()) {
    return v.as_bool() ? "true" : "false";
  }
  std::string s;
  if (v.is_number()) {
    base::append_number(s, v.as_number());
  }
  return s;
}
/// String(v || '')
std::string text_or_empty(const Value& v) {
  return v.truthy() ? text_of(v) : std::string();
}
bool is_type(const Value& e, std::string_view t) {
  const Value& v = e["type"];
  return v.is_string() && v.as_string() == t;
}
const Array& array_or_empty(const Value& v) {
  static const Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}
/// Lower case for ASCII and Latin-1 (block names → xref keys; the CLI makes its keys the same way).
std::string to_lower(std::string_view s) {
  std::string o(s);
  for (std::size_t i = 0; i < o.size(); ++i) {
    const auto c = static_cast<unsigned char>(o[i]);
    if (c >= 'A' && c <= 'Z') {
      o[i] = static_cast<char>(c + 32);
    } else if (c == 0xC3 && i + 1 < o.size()) {
      const auto d = static_cast<unsigned char>(o[i + 1]);
      if (d >= 0x80 && d <= 0x9E && d != 0x97) {
        o[i + 1] = static_cast<char>(d + 32);
      }
      ++i;
    }
  }
  return o;
}
bool iequals(std::string_view a, std::string_view b) {
  return to_lower(a) == to_lower(b);
}

// ---------------------------------------------------------------- geometry
constexpr Affine kIdentity = {1, 0, 0, 1, 0, 0};
Affine translate(double x, double y) {
  return {1, 0, 0, 1, x, y};
}
Affine scale(double sx, double sy) {
  return {sx, 0, 0, sy, 0, 0};
}
Affine rotate(double r) {
  const double c = std::cos(r);
  const double s = std::sin(r);
  return {c, s, -s, c, 0, 0};
}
Affine mul(const Affine& p, const Affine& q) {
  return {p[0] * q[0] + p[2] * q[1], p[1] * q[0] + p[3] * q[1],        p[0] * q[2] + p[2] * q[3],
          p[1] * q[2] + p[3] * q[3], p[0] * q[4] + p[2] * q[5] + p[4], p[1] * q[4] + p[3] * q[5] + p[5]};
}
/// Entities drawn with a -Z extrusion are mirrored in X (arbitrary-axis rule, plan views).
Affine ocs(const Affine& m, const Value& ext) {
  return ext.truthy() && num(ext.get("z")) < -0.5 ? mul(m, scale(-1, 1)) : m;
}
std::vector<double> tx(const Affine& m, std::span<const double> pts) {
  std::vector<double> out(pts.size());
  for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
    const double x = pts[i];
    const double y = pts[i + 1];
    out[i] = m[0] * x + m[2] * y + m[4];
    out[i + 1] = m[1] * x + m[3] * y + m[5];
  }
  return out;
}
void grow(Bounds& b, std::span<const double> pts) {
  for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
    const double x = pts[i];
    const double y = pts[i + 1];
    if (!std::isfinite(x) || !std::isfinite(y)) {
      continue;
    }
    b.min_x = std::min(b.min_x, x);
    b.max_x = std::max(b.max_x, x);
    b.min_y = std::min(b.min_y, y);
    b.max_y = std::max(b.max_y, y);
  }
}

/// Segments for an arc: about 5° each, 8 to 128.
double seg_count(double sweep) {
  return js_max(8, js_min(128, std::ceil(std::abs(sweep) / kTau * 72)));
}
/// `while (sweep <= 0) sweep += TAU`, without the JS's endless loop on a sweep of -Infinity or one too large to move.
double wrap_positive(double sweep) {
  for (int k = 0; k < 64 && sweep <= 0; ++k) {
    sweep += kTau;
  }
  if (sweep <= 0) {
    const double next = sweep + kTau * std::ceil(-sweep / kTau);
    sweep = next > 0 && std::isfinite(next) ? next : kNaN;
  }
  return sweep;
}

std::vector<double> arc_points(double cx, double cy, double r, double a0, double a1, bool ccw) {
  double sweep = wrap_positive(ccw ? a1 - a0 : a0 - a1);
  if (a1 - a0 == kTau) {
    sweep = kTau;
  }
  const double n = seg_count(sweep);
  const double dir = ccw ? 1 : -1;
  std::vector<double> pts;
  if (n >= 0) {
    pts.reserve(static_cast<std::size_t>(n + 1) * 2);
  }
  for (std::size_t k = 0, count = loops_upto(n); k < count; ++k) {
    const auto i = static_cast<double>(k);
    const double a = a0 + dir * sweep * (i / n);
    pts.push_back(cx + r * std::cos(a));
    pts.push_back(cy + r * std::sin(a));
  }
  return pts;
}

std::vector<double> ellipse_points(const Value& c, const Value& maj, double ratio, double p0, double p1) {
  const double mx = num(maj.get("x"));
  const double my = num(maj.get("y"));
  const double len = std::hypot(mx, my);
  const double rot = std::atan2(my, mx);
  const double sweep = wrap_positive(p1 - p0);
  const double n = seg_count(sweep);
  const double cr = std::cos(rot);
  const double sr = std::sin(rot);
  const double cx = num(c.get("x"));
  const double cy = num(c.get("y"));
  std::vector<double> pts;
  for (std::size_t k = 0, count = loops_upto(n); k < count; ++k) {
    const auto i = static_cast<double>(k);
    const double t = p0 + sweep * (i / n);
    const double x = len * std::cos(t);
    const double y = len * ratio * std::sin(t);
    pts.push_back(cx + x * cr - y * sr);
    pts.push_back(cy + x * sr + y * cr);
  }
  return pts;
}

/// Vertices with bulges (LWPOLYLINE, POLYLINE2D, hatch polyline paths) → points.
std::vector<double> bulge_polyline(std::span<const Value* const> vs, bool closed) {
  std::vector<double> pts;
  const std::size_t n = vs.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Value& a = *vs[i];
    const Value& b = *vs[(i + 1) % n];
    const double ax = num(a.get("x"));
    const double ay = num(a.get("y"));
    pts.push_back(ax);
    pts.push_back(ay);
    if (i == n - 1 && !closed) {
      break;
    }
    const double bulge = or_num(a["bulge"], 0);
    const double bx = num(b.get("x"));
    const double by = num(b.get("y"));
    if (std::abs(bulge) > 1e-9) {
      const double theta = 4 * std::atan(bulge);
      const double chord = std::hypot(bx - ax, by - ay);
      if (chord > 0) {
        const double r = chord / (2 * std::sin(theta / 2));
        const double mx = (ax + bx) / 2;
        const double my = (ay + by) / 2;
        const double h = r * std::cos(theta / 2);
        const double ux = -(by - ay) / chord;
        const double uy = (bx - ax) / chord;
        const double cx = mx + ux * h;
        const double cy = my + uy * h;
        const double a0 = std::atan2(ay - cy, ax - cx);
        const double steps = seg_count(theta);
        for (std::size_t q = 1, count = loops_below(steps); q < count; ++q) {  // k = 1 … while k < steps
          const auto k = static_cast<double>(q);
          const double t = a0 + theta * (k / steps);
          pts.push_back(cx + std::abs(r) * std::cos(t));
          pts.push_back(cy + std::abs(r) * std::sin(t));
        }
      }
    }
    if (i == n - 1 && closed) {
      pts.push_back(bx);
      pts.push_back(by);
    }
  }
  return pts;
}
std::vector<const Value*> refs(const Array& a) {
  std::vector<const Value*> r;
  r.reserve(a.size());
  for (const Value& v : a) {
    r.push_back(&v);
  }
  return r;
}

void flat_xy(const Array& pts, std::vector<double>& out) {
  for (const Value& p : pts) {
    out.push_back(num(p.get("x")));
    out.push_back(num(p.get("y")));
  }
}

/// Fit points when present, else the B-spline evaluated from its control points (de Boor).
std::vector<double> spline_points(const Value& e) {
  std::vector<double> pts;
  const Value& fit = e["fit"];
  if (fit.truthy() && fit.is_array() && fit.as_array().size() > 1) {
    flat_xy(fit.as_array(), pts);
    return pts;
  }
  const Array& cp = array_or_empty(e["control"]);
  const Array& kv = array_or_empty(e["knots"]);
  const double pd = or_num(e["degree"], 3);
  if (cp.size() < 2) {
    return pts;
  }
  if (static_cast<double>(kv.size()) != static_cast<double>(cp.size()) + pd + 1 || pd < 0 ||
      pd >= static_cast<double>(cp.size())) {
    // (a negative degree, or one the control points cannot carry, throws in the JS: here the control polygon is drawn)
    flat_xy(cp, pts);
    return pts;
  }
  const auto p = static_cast<std::size_t>(pd);
  std::vector<double> k(kv.size());
  for (std::size_t i = 0; i < kv.size(); ++i) {
    k[i] = num(kv[i]);
  }
  const Value& wv = e["weights"];
  const bool weighted = wv.truthy() && wv.is_array() && wv.as_array().size() == cp.size();
  const double lo = k[p];
  const double hi = k[k.size() - p - 1];
  const double n = std::max(16.0, static_cast<double>(cp.size()) * 8);
  std::vector<std::array<double, 3>> d(p + 1);
  for (std::size_t step = 0, count = loops_upto(n); step < count; ++step) {
    const auto s = static_cast<double>(step);
    const double u = s == n ? hi - 1e-9 * (hi - lo) : lo + (hi - lo) * (s / n);
    std::size_t span = p;
    while (span < cp.size() - 1 && u >= k[span + 1]) {
      ++span;
    }
    for (std::size_t j = 0; j <= p; ++j) {
      const Value& q = cp[span - p + j];
      const double wt = weighted ? num(wv.as_array()[span - p + j]) : 1;
      d[j] = {num(q.get("x")) * wt, num(q.get("y")) * wt, wt};
    }
    for (std::size_t r = 1; r <= p; ++r) {
      for (std::size_t j = p; j >= r; --j) {
        const std::size_t i = span - p + j;
        const double den = k[i + p - r + 1] - k[i];
        const double a = den == 0 ? 0 : (u - k[i]) / den;
        d[j] = {(1 - a) * d[j - 1][0] + a * d[j][0], (1 - a) * d[j - 1][1] + a * d[j][1],
                (1 - a) * d[j - 1][2] + a * d[j][2]};
      }
    }
    pts.push_back(d[p][0] / d[p][2]);
    pts.push_back(d[p][1] / d[p][2]);
  }
  return pts;
}

/// HATCH loops (polyline or edges: line, arc, ellipse, spline; angles in radians) → flat rings in the hatch's OCS.
std::vector<std::vector<double>> hatch_rings(const Value& e) {
  std::vector<std::vector<double>> rings;
  for (const Value& loop : array_or_empty(e["loops"])) {
    const Value& pl = loop["polyline"];
    if (pl.truthy()) {
      const auto vs = refs(array_or_empty(pl["vertices"]));
      rings.push_back(bulge_polyline(vs, true));
      continue;
    }
    std::vector<double> pts;
    auto append = [&](const std::vector<double>& more) { pts.insert(pts.end(), more.begin(), more.end()); };
    for (const Value& ed : array_or_empty(loop["edges"])) {
      const Value& t = ed["type"];
      const std::string_view type = t.is_string() ? std::string_view(t.as_string()) : std::string_view();
      const bool cw = ed["ccw"].is_bool() && !ed["ccw"].as_bool();
      if (type == "line") {
        pts.push_back(num(ed["start"].get("x")));
        pts.push_back(num(ed["start"].get("y")));
        pts.push_back(num(ed["end"].get("x")));
        pts.push_back(num(ed["end"].get("y")));
      } else if (type == "arc") {  // a clockwise edge keeps its angles mirrored (DXF 73 = 0)
        const double s = num(ed["startAngle"]);
        const double f = num(ed["endAngle"]);
        const double cx = num(ed["center"].get("x"));
        const double cy = num(ed["center"].get("y"));
        const double r = num(ed["radius"]);
        append(cw ? arc_points(cx, cy, r, -s, -f, false) : arc_points(cx, cy, r, s, f, true));
      } else if (type == "ellipse") {
        const double s = cw ? -num(ed["endAngle"]) : num(ed["startAngle"]);
        const double f = cw ? -num(ed["startAngle"]) : num(ed["endAngle"]);
        append(ellipse_points(ed["center"], ed["majorAxis"], or_num(ed["ratio"], 1), s, f));
      } else if (type == "spline") {
        append(spline_points(ed));
      }
    }
    if (pts.size() >= 6) {
      rings.push_back(std::move(pts));
    }
  }
  return rings;
}

// ---------------------------------------------------------------- text
/// One pass of a replacement over UTF-8 text: `match(s, i)` returns the length matched at i (0 = none) and the text
/// to put in its place.
template <class Match>
std::string replace_all(std::string_view s, Match match) {
  std::string out;
  out.reserve(s.size());
  std::string with;
  for (std::size_t i = 0; i < s.size();) {
    with.clear();
    const std::size_t n = match(s, i, with);
    if (n == 0) {
      out += s[i];
      ++i;
    } else {
      out += with;
      i += n;
    }
  }
  return out;
}
bool hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// plainText: %%c %%d %%p %%% specials, %%u %%o dropped, \U+XXXX → the character.
std::string plain_text(std::string s) {
  s = replace_all(s, [](std::string_view t, std::size_t i, std::string& with) -> std::size_t {
    if (t.substr(i, 2) != "%%" || i + 2 >= t.size()) {
      return 0;
    }
    switch (t[i + 2]) {
      case 'c':
      case 'C': with = "\xC3\xB8"; return 3;  // ø
      case 'd':
      case 'D': with = "\xC2\xB0"; return 3;  // °
      case 'p':
      case 'P': with = "\xC2\xB1"; return 3;  // ±
      case '%': with = "%"; return 3;
      default: return 0;
    }
  });
  s = replace_all(s, [](std::string_view t, std::size_t i, std::string&) -> std::size_t {
    if (t.substr(i, 2) != "%%" || i + 2 >= t.size()) {
      return 0;
    }
    const char c = t[i + 2];
    return c == 'u' || c == 'U' || c == 'o' || c == 'O' ? 3 : 0;
  });
  s = replace_all(s, [](std::string_view t, std::size_t i, std::string& with) -> std::size_t {
    if (t.substr(i, 3) != "\\U+" || i + 7 > t.size()) {
      return 0;
    }
    for (std::size_t k = 3; k < 7; ++k) {
      if (!hex_digit(t[i + k])) {
        return 0;
      }
    }
    std::uint32_t cp = 0;
    for (std::size_t k = 3; k < 7; ++k) {
      const char c = t[i + k];
      const auto d = static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
      cp = cp * 16 + d;
    }
    base::append_code_point(with, cp);  // String.fromCharCode: one UTF-16 unit (a surrogate stays alone, as WTF-8)
    return 7;
  });
  return s;
}

/// MTEXT codes → plain lines: \P breaks, {\f…;} \H…; \C…; formatting dropped, stacked fractions as a/b.
std::vector<std::string> mtext_lines(const std::string& value) {
  std::string t = plain_text(value);
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string& with) -> std::size_t {
    if (s.substr(i, 2) == "\\P") {
      with = "\n";
      return 2;
    }
    return 0;
  });
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string& with) -> std::size_t {
    if (s.substr(i, 2) == "\\~") {
      with = "\xC2\xA0";  // the JS writes a no-break space (U+00A0) for MTEXT's \~
      return 2;
    }
    return 0;
  });
  // /\\S([^;]*?)[#^/]([^;]*?);/ → '$1/$2'
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string& with) -> std::size_t {
    if (s.substr(i, 2) != "\\S") {
      return 0;
    }
    const std::size_t semi = s.find(';', i + 2);
    if (semi == std::string_view::npos) {
      return 0;
    }
    const std::size_t sep = s.find_first_of("#^/", i + 2);
    if (sep == std::string_view::npos || sep > semi) {
      return 0;
    }
    with = std::string(s.substr(i + 2, sep - i - 2)) + "/" + std::string(s.substr(sep + 1, semi - sep - 1));
    return semi + 1 - i;
  });
  // /\\[ACcFfHhQqTtWwp][^;\\]*;/ → ''
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string&) -> std::size_t {
    if (s[i] != '\\' || i + 1 >= s.size() || !std::string_view("ACcFfHhQqTtWwp").contains(s[i + 1])) {
      return 0;
    }
    const std::size_t stop = s.find_first_of(";\\", i + 2);
    if (stop == std::string_view::npos || s[stop] != ';') {
      return 0;
    }
    return stop + 1 - i;
  });
  // /\\[LlOoKkNn]/ → ''
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string&) -> std::size_t {
    return s[i] == '\\' && i + 1 < s.size() && std::string_view("LlOoKkNn").contains(s[i + 1]) ? 2 : 0;
  });
  std::erase_if(t, [](char c) { return c == '{' || c == '}'; });
  t = replace_all(t, [](std::string_view s, std::size_t i, std::string& with) -> std::size_t {
    if (s.substr(i, 2) == "\\\\") {
      with = "\\";
      return 2;
    }
    return 0;
  });
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= t.size(); ++i) {
    if (i == t.size() || t[i] == '\n') {
      lines.emplace_back(t.substr(start, i - start));
      start = i + 1;
    }
  }
  return lines;
}

std::string rgb_hex(double n) {
  const auto v = static_cast<std::uint32_t>(base::js_to_int32(n) & 0xFFFFFF);
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s = "#";
  for (int sh = 20; sh >= 0; sh -= 4) {
    s += kHex[(v >> static_cast<unsigned>(sh)) & 15U];
  }
  return s;
}

std::string hsv_hex(double h, double s, double v) {
  auto f = [&](double n) {
    const double k = std::fmod(n + h / 60, 6);
    return v - v * s * js_max(0, js_min(js_min(k, 4 - k), 1));
  };
  std::string out = "#";
  for (const double x : {f(5), f(3), f(1)}) {
    const double r = std::floor(x * 255 + 0.5);  // Math.round
    if (std::isnan(r)) {
      out += "NaN";
      continue;
    }
    static constexpr std::string_view kHex = "0123456789abcdef";
    const auto b = static_cast<unsigned>(std::clamp(r, 0.0, 255.0));
    out += kHex[b >> 4U];
    out += kHex[b & 15U];
  }
  return out;
}

bool layer_plots(const Value& l) {
  const Value& p = l["plot"];
  return (!p.is_bool() || p.as_bool()) && !iequals(text_or_empty(l["name"]), "defpoints");
}
std::string layer_color(const Value& l) {
  const Value& c0 = l["color"];
  if (c0.truthy() && !c0["rgb"].is_undefined()) {
    return rgb_hex(num(c0["rgb"]));
  }
  if (!c0.truthy()) {
    return aci_color(7);
  }
  return aci_color(std::abs(nn(c0["index"], 7)));
}
double lw_of(double v) {
  return v >= 0 && v <= 211 ? v / 100 : kLwDefault;
}
std::string base_name(const std::string& p) {
  const std::size_t k = p.find_last_of("\\/");
  return k == std::string::npos ? p : p.substr(k + 1);
}

// ---------------------------------------------------------------- the compiler
struct LayerProps {
  std::string color;
  bool on = true;
  double lw = kLwDefault;
  bool plot = true;
};
using LayerTable = std::unordered_map<std::string, LayerProps>;

struct Parts {
  std::unordered_map<std::string, const Value*> blocks;
  std::unordered_map<std::string, const Value*> by_record;
  const Value* space = nullptr;
  std::unordered_map<std::string, const Value*> image_defs;
};
Parts model_parts(const Value& model) {
  Parts p;
  for (const Value& b : array_or_empty(model["blocks"])) {
    p.blocks[text_of(b["name"])] = &b;
    if (b["record"].truthy()) {
      p.by_record[text_of(b["record"])] = &b;
    }
    if (p.space == nullptr && iequals(text_of(b["name"]), "*model_space")) {
      p.space = &b;
    }
  }
  for (const Value& o : array_or_empty(model["objects"])) {
    if (is_type(o, "IMAGEDEF")) {
      p.image_defs[text_of(o["handle"])] = &o;
    }
  }
  return p;
}

struct XInfo {
  Parts parts;
  const Array* model = nullptr;
  LayerTable layers;
  Value base;
};

struct Ctx {
  Affine m = kIdentity;
  std::optional<std::string> layer;
  std::optional<std::string> color;
  std::optional<std::string> layer_color;
  std::optional<double> lw;
  int depth = 0;
  const Parts* parts = nullptr;
  std::string xp;
  const LayerTable* xlayers = nullptr;
  const std::unordered_map<std::string, std::string>* attrs = nullptr;
};

bool is_dim(std::string_view t) {
  return t.starts_with("DIMENSION") || t == "ARC_DIMENSION" || t == "LARGE_RADIAL_DIMENSION";
}

class Compiler {
 public:
  Compiler(const Value& db, const CompileOptions& opt)
      : db_(&db), parts_(model_parts(db)), max_coords_(opt.max_coords), max_visits_(opt.max_visits) {
    for (const Value& l : array_or_empty(db["tables"]["layers"])) {
      set_layer(Layer{text_of(l["name"]), layer_color(l), !l["off"].truthy() && !l["frozen"].truthy(),
                      lw_of(num(l["lineweight"])), layer_plots(l)});
    }
    for (const Xref& x : opt.xrefs) {
      if (x.model == nullptr) {
        continue;
      }
      XInfo info;
      info.parts = model_parts(*x.model);
      info.model =
          info.parts.space != nullptr ? &array_or_empty((*info.parts.space)["entities"]) : &array_or_empty(Value());
      for (const Value& l : array_or_empty((*x.model)["tables"]["layers"])) {
        info.layers[text_of(l["name"])] = LayerProps{layer_color(l), !l["off"].truthy() && !l["frozen"].truthy(),
                                                     lw_of(num(l["lineweight"])), layer_plots(l)};
      }
      const Value& ib = (*x.model)["header"]["INSBASE"];
      info.base = ib.truthy() ? ib : base::pt2(0, 0);
      xinfo_.insert_or_assign(x.key, std::move(info));
    }
  }

  Drawing run(const CompileOptions& opt) {
    Drawing d;
    if (parts_.space != nullptr) {
      SpaceOut s = compile_space(array_or_empty((*parts_.space)["entities"]));
      Space sp;
      sp.key = "model";
      sp.name = "Model";
      sp.kind = "model";
      sp.batches = std::move(s.batches);
      sp.bounds = s.bounds;
      sp.images = std::move(s.pics);
      d.spaces.push_back(std::move(sp));
    }
    std::vector<const Value*> layouts;
    for (const Value& l : array_or_empty((*db_)["layouts"])) {
      if (!iequals(text_or_empty(l["name"]), "model")) {
        layouts.push_back(&l);
      }
    }
    std::ranges::stable_sort(
        layouts, [](const Value* a, const Value* b) { return nn((*a)["tabOrder"], 0) - nn((*b)["tabOrder"], 0) < 0; });
    for (const Value* l : layouts) {
      const auto it = parts_.by_record.find(text_of((*l)["block"]));
      if (it == parts_.by_record.end() || it->second == parts_.space) {
        continue;
      }
      SpaceOut s = compile_space(array_or_empty((*it->second)["entities"]));
      const Value& lim = (*l)["limits"];
      std::optional<Bounds> paper;
      if (lim.truthy() && lim["min"].truthy() && lim["max"].truthy() &&
          num(lim["max"].get("x")) > num(lim["min"].get("x"))) {
        paper = Bounds{num(lim["min"].get("x")), num(lim["min"].get("y")), num(lim["max"].get("x")),
                       num(lim["max"].get("y"))};
      }
      std::vector<Viewport> vps;
      for (std::size_t k = 1; k < s.viewports.size(); ++k) {  // the first viewport is the sheet itself
        const Value& v = *s.viewports[k];
        // NOLINTNEXTLINE(readability-simplify-boolean-expr) — kept as the JS filter: NaN sizes must fail it
        if (!(num(v["width"]) > 0 && num(v["height"]) > 0 && num(v["viewHeight"]) > 0)) {
          continue;
        }
        Viewport vp;
        vp.cx = num(v["center"].get("x"));
        vp.cy = num(v["center"].get("y"));
        vp.w = num(v["width"]);
        vp.h = num(v["height"]);
        vp.vx = or_num(v["viewTarget"].get("x"), 0) + or_num(v["viewCenter"].get("x"), 0);
        vp.vy = or_num(v["viewTarget"].get("y"), 0) + or_num(v["viewCenter"].get("y"), 0);
        vp.view_height = num(v["viewHeight"]);
        vp.twist = or_num(v["twist"], 0);
        std::unordered_set<std::string> seen;
        for (const Value& f : array_or_empty(v["frozenLayers"])) {
          std::string h = text_of(f);
          if (seen.insert(h).second) {
            vp.frozen.push_back(std::move(h));
          }
        }
        vps.push_back(std::move(vp));
      }
      const bool empty = s.batches.empty() && vps.empty();
      if (empty && !paper) {
        continue;
      }
      if (paper) {
        const std::array<double, 4> c = {paper->min_x, paper->min_y, paper->max_x, paper->max_y};
        grow(s.bounds, c);
      }
      Space sp;
      sp.key = "layout:" + text_of((*l)["handle"]);
      sp.name = text_of((*l)["name"]);
      sp.kind = "paper";
      sp.batches = std::move(s.batches);
      sp.bounds = s.bounds;
      sp.paper = paper;
      sp.paper_unit = paper && paper->max_x - paper->min_x > 100 ? "mm" : "in";
      sp.viewports = std::move(vps);
      sp.images = std::move(s.pics);
      d.spaces.push_back(std::move(sp));
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kVersions = {{{"AC1009", "R12"},
                                                                                                {"AC1012", "R13"},
                                                                                                {"AC1014", "R14"},
                                                                                                {"AC1015", "2000"},
                                                                                                {"AC1018", "2004"},
                                                                                                {"AC1021", "2007"},
                                                                                                {"AC1024", "2010"},
                                                                                                {"AC1027", "2013"},
                                                                                                {"AC1032", "2018"}}};
    d.version = opt.version_tag;
    for (const auto& [tag, rel] : kVersions) {
      if (tag == opt.version_tag) {
        d.release = std::string(rel);
      }
    }
    static constexpr std::array<std::string_view, 11> kUnits = {
        "",       "inches",     "feet",        "miles", "millimeters", "centimeters",
        "meters", "kilometers", "microinches", "mils",  "yards"};
    const Value& iu = (*db_)["header"]["INSUNITS"];
    if (iu.is_number() && iu.as_number() >= 0 && iu.as_number() <= 10 && iu.as_number() == std::floor(iu.as_number())) {
      d.units = std::string(kUnits[static_cast<std::size_t>(iu.as_number())]);
    }
    for (const Value& b : array_or_empty((*db_)["blocks"])) {
      if ((int32(b["flags"]) & 4) != 0) {
        const std::string name = text_of(b["name"]);
        d.xrefs.emplace_back(name, xinfo_.contains(to_lower(name)));
      }
    }
    d.images = std::move(images_);
    d.layers = std::move(layers_);
    std::ranges::stable_sort(
        d.layers, [](const Layer& a, const Layer& b) { return base::js_locale_compare_numeric(a.name, b.name) < 0; });
    d.stats = std::move(stats_);
    return d;
  }

 private:
  struct SpaceOut {
    std::vector<Batch> batches;
    Bounds bounds;
    std::vector<const Value*> viewports;
    std::vector<Picture> pics;
  };

  void set_layer(Layer l) {
    const auto it = layer_index_.find(l.name);
    if (it != layer_index_.end()) {
      layers_[it->second] = std::move(l);  // a Map keeps the first place
      return;
    }
    layer_index_.emplace(l.name, layers_.size());
    layers_.push_back(std::move(l));
  }
  /// The layer (a copy: the table may grow while it is used), made from the fallback or the defaults when missing.
  Layer layer_of(const std::string& name, const LayerProps* fallback) {
    const auto it = layer_index_.find(name);
    if (it != layer_index_.end()) {
      return layers_[it->second];
    }
    Layer l = fallback != nullptr ? Layer{name, fallback->color, fallback->on, fallback->lw, fallback->plot}
                                  : Layer{name, "ink", true, kLwDefault, true};
    set_layer(l);
    return l;
  }
  void skip(const std::string& t) {
    for (auto& [k, n] : stats_.unsupported) {
      if (k == t) {
        ++n;
        return;
      }
    }
    stats_.unsupported.emplace_back(t, 1);
  }

  struct Space_ {
    std::vector<Batch>* batches;
    std::unordered_map<std::string, std::size_t>* index;
    Bounds* bounds;
    std::vector<const Value*>* viewports;
    std::vector<Picture>* pics;
  };

  SpaceOut compile_space(const Array& entities) {
    SpaceOut out;
    std::unordered_map<std::string, std::size_t> index;
    Space_ sp{&out.batches, &index, &out.bounds, &out.viewports, &out.pics};
    for (const Value& e : entities) {
      ++stats_.entities;
      Ctx top;
      top.parts = &parts_;
      emit(sp, e, top);
    }
    return out;
  }

  static Batch& batch(Space_& sp, const std::string& layer, const std::string& color, double lw) {
    std::string key = layer;
    key += '\0';
    key += color;
    key += '\0';
    base::append_number(key, lw);
    const auto it = sp.index->find(key);
    if (it != sp.index->end()) {
      return (*sp.batches)[it->second];
    }
    sp.index->emplace(std::move(key), sp.batches->size());
    Batch b;
    b.layer = layer;
    b.color = color;
    b.lw = lw;
    sp.batches->push_back(std::move(b));
    return sp.batches->back();
  }

  struct Style {
    std::string layer;
    std::string color;
    Layer lay;
    double lw = 0;
    Affine m{};
  };

  void line(Space_& sp, const Style& st, std::span<const double> pts) {
    if (pts.size() >= 4) {
      std::vector<double> t = tx(st.m, pts);
      coords_ += static_cast<double>(t.size());
      grow(*sp.bounds, t);
      batch(sp, st.layer, st.color, st.lw).lines.push_back(std::move(t));
      ++stats_.prims;
    }
  }
  void fill(Space_& sp, const Style& st, std::vector<std::vector<double>> rings, bool pattern) {
    for (const auto& r : rings) {
      coords_ += static_cast<double>(r.size());
      grow(*sp.bounds, r);
    }
    batch(sp, st.layer, st.color, st.lw).fills.push_back(Fill{std::move(rings), pattern});
  }

  static Ctx sub_ctx(const Ctx& ctx, const Affine& m, const Style& st, const XInfo* x, const std::string& prefix) {
    Ctx c;
    c.m = m;
    c.layer = st.layer;
    c.color = st.color;
    c.layer_color = st.lay.color;
    c.lw = st.lw;
    c.depth = ctx.depth + 1;
    if (x != nullptr) {
      c.parts = &x->parts;
      c.xp = prefix;
      c.xlayers = &x->layers;
    } else {
      c.parts = ctx.parts;
      c.xp = ctx.xp;
      c.xlayers = ctx.xlayers;
    }
    return c;
  }

  // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity) — depth bounded by kMaxDepth
  void emit(Space_& sp, const Value& e, const Ctx& ctx) {
    if (stats_.prims > kMaxPrims || coords_ > max_coords_ || ++visits_ > max_visits_) {
      stats_.truncated = true;
      return;
    }
    if (e["invisible"].truthy() || ctx.depth > kMaxDepth) {
      return;
    }
    Style st;
    const std::string src = e["layer"].truthy() ? text_of(e["layer"]) : std::string("0");
    if (src == "0" && ctx.layer && !ctx.layer->empty()) {
      st.layer = *ctx.layer;
    } else {
      st.layer = ctx.xp.empty() ? src : ctx.xp + src;
    }
    const LayerProps* fb = nullptr;
    if (ctx.xlayers != nullptr) {
      const auto it = ctx.xlayers->find(src);
      fb = it != ctx.xlayers->end() ? &it->second : nullptr;
    }
    st.lay = layer_of(st.layer, fb);
    const Value& c = e["color"];
    const bool e_layer0 = e["layer"].is_string() && e["layer"].as_string() == "0";
    if (c.truthy() && !c["rgb"].is_undefined()) {
      st.color = rgb_hex(num(c["rgb"]));
    } else {
      const Value& idx = c.truthy() ? c["index"] : Value(256);
      if (idx.is_number() && idx.as_number() == 0) {
        st.color = ctx.color && !ctx.color->empty() ? *ctx.color : "ink";
      } else if ((idx.is_number() && idx.as_number() == 256) || idx.is_undefined()) {
        st.color = e_layer0 && ctx.layer_color && !ctx.layer_color->empty() ? *ctx.layer_color : st.lay.color;
      } else {
        st.color = aci_color(num(idx));
      }
    }
    const Value& lwv = e["lineweight"];
    const double lwi = lwv.is_undefined() || lwv.is_null() ? -1 : num(lwv);
    const bool lw_number = lwv.is_number() || lwv.is_undefined() || lwv.is_null();
    if (lw_number && lwi == -1) {
      st.lw = st.lay.lw;
    } else if (lw_number && lwi == -2) {
      st.lw = ctx.lw ? *ctx.lw : kLwDefault;
    } else {
      st.lw = lw_of(lwi);
    }
    st.m = ocs(ctx.m, e["extrusion"]);

    const Value& tv = e["type"];
    const std::string type = tv.is_string() ? tv.as_string() : text_of(tv);
    std::vector<double> pts;
    auto xy = [](const Value& p, std::vector<double>& out) {
      out.push_back(num(p.get("x")));
      out.push_back(num(p.get("y")));
    };
    if (type == "LINE") {
      xy(e["start"], pts);
      xy(e["end"], pts);
      line(sp, st, pts);
    } else if (type == "LWPOLYLINE") {
      const auto vs = refs(array_or_empty(e["vertices"]));
      line(sp, st, bulge_polyline(vs, e["closed"].truthy()));
    } else if (type == "POLYLINE_2D" || type == "POLYLINE_3D") {
      std::vector<const Value*> vs;
      for (const Value& v : array_or_empty(e["vertices"])) {
        if ((int32(v["flags"]) & 16) == 0) {
          vs.push_back(&v);
        }
      }
      line(sp, st, bulge_polyline(vs, e["closed"].truthy()));
    } else if (type == "CIRCLE") {
      line(sp, st, arc_points(num(e["center"].get("x")), num(e["center"].get("y")), num(e["radius"]), 0, kTau, true));
    } else if (type == "ARC") {
      line(sp, st,
           arc_points(num(e["center"].get("x")), num(e["center"].get("y")), num(e["radius"]), num(e["startAngle"]),
                      num(e["endAngle"]), true));
    } else if (type == "ELLIPSE") {
      line(sp, st,
           ellipse_points(e["center"], e["majorAxis"], num(e["ratio"]), num(e["startParam"]), num(e["endParam"])));
    } else if (type == "SPLINE") {
      line(sp, st, spline_points(e));
    } else if (type == "LEADER") {
      flat_xy(array_or_empty(e["vertices"]), pts);
      line(sp, st, pts);
    } else if (type == "MLINE") {
      const Array& vs = array_or_empty(e["vertices"]);
      for (const Value& v : vs) {
        xy(v["point"], pts);
      }
      if (e["closed"].truthy() && !vs.empty()) {
        xy(vs[0]["point"], pts);
      }
      line(sp, st, pts);
    } else if (type == "SOLID" || type == "TRACE") {
      const Array& p = array_or_empty(e["points"]);
      auto at = [&](std::size_t k) -> const Value& {
        static const Value kNone;
        return k < p.size() ? p[k] : kNone;
      };
      const Value& p3 = at(3).truthy() ? at(3) : at(2);  // DXF order 1,2,3,4 → outline 1,2,4,3
      for (const Value* q : {&at(0), &at(1), &p3, &at(2)}) {
        if (q->truthy()) {
          xy(*q, pts);
        }
      }
      std::vector<std::vector<double>> rings;
      rings.push_back(tx(st.m, pts));
      fill(sp, st, std::move(rings), false);
      ++stats_.prims;
    } else if (type == "3DFACE") {
      const Array& p = array_or_empty(e["points"]);
      if (p.size() >= 3) {
        const Value& p3 = p.size() > 3 && p[3].truthy() ? p[3] : p[2];
        for (const Value* q : {&p.front(), &p[1], &p[2], &p3, &p.front()}) {
          xy(*q, pts);
        }
        line(sp, st, pts);
      }
    } else if (type ==
               "POLYLINE_PFACE") {  // seen from above: each face's visible edges; a negative index hides its edge
      const Array& v = array_or_empty(e["vertices"]);
      std::unordered_set<std::string> done;
      for (const Value& f : array_or_empty(e["faces"])) {
        std::vector<double> idx;
        for (const Value& i : array_or_empty(f)) {
          if (i.truthy()) {
            idx.push_back(num(i));
          }
        }
        for (std::size_t k = 0; k < idx.size(); ++k) {
          const double i = idx[k];
          const double j = idx[(k + 1) % idx.size()];
          if (i < 0) {
            continue;
          }
          auto vertex = [&](double n) -> const Value* {
            const double at = std::abs(n) - 1;
            if (!(at >= 0) || at != std::floor(at) || at >= static_cast<double>(v.size())) {
              return nullptr;
            }
            const Value* p = &v[static_cast<std::size_t>(at)];
            return p->truthy() ? p : nullptr;
          };
          const Value* a = vertex(i);
          const Value* b = vertex(j);
          if (a == nullptr || b == nullptr) {
            continue;
          }
          std::string key;
          base::append_number(key, std::min(std::abs(i), std::abs(j)));
          key += '|';
          base::append_number(key, std::max(std::abs(i), std::abs(j)));
          if (!done.insert(std::move(key)).second) {
            continue;  // an edge two faces share is drawn once
          }
          std::vector<double> seg;
          xy(*a, seg);
          xy(*b, seg);
          line(sp, st, seg);
        }
      }
    } else if (type == "POLYLINE_MESH") {  // seen from above: the M × N grid of lines
      const Array& v = array_or_empty(e["vertices"]);
      const double M = or_num(e["m"], 0);
      const double N = or_num(e["n"], 0);
      if (M * N > static_cast<double>(v.size()) || M < 1 || N < 1 || std::isnan(M * N)) {
        skip("POLYLINE_MESH (damaged)");
      } else {
        auto at = [&](double i, double j) -> const Value& {
          static const Value kNone;
          const double k = i * N + j;
          return k >= 0 && k < static_cast<double>(v.size()) && k == std::floor(k) ? v[static_cast<std::size_t>(k)]
                                                                                   : kNone;
        };
        const bool cm = (int32(e["flags"]) & 1) != 0;
        const bool cn = (int32(e["flags"]) & 32) != 0;
        for (std::size_t qi = 0, mi = loops_below(M); qi < mi; ++qi) {
          const auto i = static_cast<double>(qi);
          std::vector<double> r;
          for (std::size_t qj = 0, nj = loops_below(N); qj < nj; ++qj) {
            xy(at(i, static_cast<double>(qj)), r);
          }
          if (cn && N > 2) {
            xy(at(i, 0), r);
          }
          line(sp, st, r);
        }
        for (std::size_t qj = 0, nj = loops_below(N); qj < nj; ++qj) {
          const auto j = static_cast<double>(qj);
          std::vector<double> r;
          for (std::size_t qi = 0, mi = loops_below(M); qi < mi; ++qi) {
            xy(at(static_cast<double>(qi), j), r);
          }
          if (cm && M > 2) {
            xy(at(0, j), r);
          }
          line(sp, st, r);
        }
      }
    } else if (type == "HATCH") {
      std::vector<std::vector<double>> rings;
      for (const auto& r : hatch_rings(e)) {
        std::vector<double> t = tx(st.m, r);
        if (t.size() >= 6) {
          rings.push_back(std::move(t));
        }
      }
      if (!rings.empty()) {
        fill(sp, st, std::move(rings), !e["solid"].truthy());
        ++stats_.prims;
      }
    } else if (type == "TEXT") {
      text(sp, st, e, nullptr);
    } else if (type == "ATTRIB") {
      if ((int32(e["flags"]) & 1) == 0) {  // not an invisible attribute
        if (num(e["attType"]) > 1 && e["mtext"].truthy()) {
          mtext(sp, st, e["mtext"]);
        } else {
          text(sp, st, e, nullptr);
        }
      }
    } else if (type == "MTEXT") {
      mtext(sp, st, e);
    } else if (type == "INSERT" || type == "MINSERT") {
      insert(sp, st, e, ctx);
    } else if (type == "ACAD_TABLE") {
      const Value& bn = e["block"];
      const Value* blk = bn.truthy() ? find_block(ctx, bn) : nullptr;
      if (blk == nullptr) {
        skip(type);
      } else {
        const Value& ipv = e["insert"];
        const Value ip = ipv.truthy() ? ipv : base::pt2(0, 0);
        const Affine local =
            mul(mul(translate(or_num(ip.get("x"), 0), or_num(ip.get("y"), 0)), rotate(or_num(e["rotation"], 0))),
                scale(or_num(e["scale"].get("x"), 1), or_num(e["scale"].get("y"), 1)));
        const Ctx sub = sub_ctx(ctx, mul(st.m, local), st, nullptr, {});
        for (const Value& be : array_or_empty((*blk)["entities"])) {
          emit(sp, be, sub);
        }
      }
    } else if (type == "MULTILEADER") {
      mleader(sp, st, e, ctx);
    } else if (type == "IMAGE") {
      image(sp, st, e, ctx);
    } else if (type == "VIEWPORT") {
      if (ctx.depth == 0) {
        sp.viewports->push_back(&e);
      }
    } else if (type ==
               "ATTDEF") {  // only a multileader's block shows its attribute definitions, with the leader's values
      if (ctx.attrs != nullptr) {
        const auto it = ctx.attrs->find(text_of(e["handle"]));
        if (it != ctx.attrs->end()) {
          text(sp, st, e, &it->second);
        }
      }
    } else if (type == "POINT" || type == "XLINE" || type == "RAY") {
      // not drawn on a sheet
    } else if (is_dim(type)) {  // drawn from its anonymous block (*D…), already in this space's coordinates
      const Value& bn = e["block"];
      const Value* blk = bn.truthy() ? find_block(ctx, bn) : nullptr;
      if (blk == nullptr) {
        skip("DIMENSION");
      } else {
        const Value& ipv = e["insert"];
        const Value ip = ipv.truthy() ? ipv : base::pt2(0, 0);
        const Ctx sub =
            sub_ctx(ctx, mul(st.m, translate(or_num(ip.get("x"), 0), or_num(ip.get("y"), 0))), st, nullptr, {});
        for (const Value& be : array_or_empty((*blk)["entities"])) {
          emit(sp, be, sub);
        }
      }
    } else if (type == "UNKNOWN") {
      std::string name = "UNKNOWN";
      if (e["dxfName"].truthy()) {
        name = text_of(e["dxfName"]);
      } else if (e["className"].truthy()) {
        name = text_of(e["className"]);
      }
      skip(name);
    } else {
      skip(type);
    }
  }

  static const Value* find_block(const Ctx& ctx, const Value& name) {
    if (!name.is_string()) {
      return nullptr;
    }
    const auto it = ctx.parts->blocks.find(name.as_string());
    return it != ctx.parts->blocks.end() ? it->second : nullptr;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  void insert(Space_& sp, const Style& st, const Value& e, const Ctx& ctx) {
    const Value* blk = find_block(ctx, e["block"]);
    if (blk == nullptr) {
      skip("INSERT (missing block)");
      return;
    }
    if (ctx.depth > 20) {
      skip("INSERT (nested too deep)");
      return;
    }
    const bool is_xref = (int32((*blk)["flags"]) & 4) != 0;
    const XInfo* x = nullptr;
    if (is_xref) {
      const auto it = xinfo_.find(to_lower(e["block"].as_string()));
      if (it == xinfo_.end()) {
        return;  // linked drawing not loaded: listed in drawing.xrefs
      }
      x = &it->second;
    }
    Value base_pt = base::pt2(0, 0);
    if (x != nullptr) {
      base_pt = x->base;
    } else if ((*blk)["base"].truthy()) {
      base_pt = (*blk)["base"];
    }
    const Array& content = x != nullptr ? *x->model : array_or_empty((*blk)["entities"]);
    const double sx = or_num(e["scale"].get("x"), 1);
    const double sy = or_num(e["scale"].get("y"), 1);
    const double cols = js_max(1, or_num(e["cols"], 1));
    const double rows = js_max(1, or_num(e["rows"], 1));
    const double bx = num(base_pt.get("x"));
    const double by = num(base_pt.get("y"));
    const std::string prefix = x != nullptr ? e["block"].as_string() + "|" : std::string();
    double cells = 0;
    for (std::size_t qr = 0, nr = loops_below(rows); qr < nr; ++qr) {
      const auto r = static_cast<double>(qr);
      for (std::size_t qc = 0, nc = loops_below(cols); qc < nc; ++qc) {
        const auto c = static_cast<double>(qc);
        if (++cells > kMaxPrims || (stats_.truncated && x == nullptr) || content.empty()) {
          break;  // the JS would go on (for ever, with an infinite count) emitting nothing
        }
        const Affine local =
            mul(mul(translate(num(e["insert"].get("x")), num(e["insert"].get("y"))), rotate(or_num(e["rotation"], 0))),
                mul(scale(sx, sy),
                    translate(-bx + c * or_num(e["colSpacing"], 0) / sx, -by + r * or_num(e["rowSpacing"], 0) / sy)));
        const Ctx sub = sub_ctx(ctx, mul(st.m, local), st, x, prefix);
        for (const Value& be : content) {
          if (!is_type(be, "ATTDEF")) {
            if (x != nullptr) {
              ++stats_.xref_entities;
            }
            emit(sp, be, sub);
          }
        }
      }
    }
    for (const Value& a : array_or_empty(e["attribs"])) {
      emit(sp, a, ctx);  // attributes are placed in the parent's space
    }
  }

  void image(Space_& sp, const Style& st, const Value& e, const Ctx& ctx) {
    const auto dit = ctx.parts->image_defs.find(text_of(e["def"]));
    const Value* def = dit != ctx.parts->image_defs.end() ? dit->second : nullptr;
    const Value& wv = e["size"].get("x");
    const Value& hv = e["size"].get("y");
    const Value& u = e["u"];
    const Value& v = e["v"];
    const Value& p = e["insert"];
    if (def == nullptr || !wv.truthy() || !hv.truthy() || !u.truthy() || !v.truthy() || !p.truthy()) {
      skip("IMAGE (no picture file named)");
      return;
    }
    const double w = num(wv);
    const double h = num(hv);
    // pixel (x right, y down) → world: the position is the image's lower-left corner
    const Affine pm = mul(st.m, {num(u.get("x")), num(u.get("y")), -num(v.get("x")), -num(v.get("y")),
                                 num(p.get("x")) + h * num(v.get("x")), num(p.get("y")) + h * num(v.get("y"))});
    const std::string ref = text_or_empty((*def)["fileName"]);
    sp.pics->push_back(Picture{pm, w, h, ref, st.layer});
    const std::array<double, 8> corners = {0, 0, w, 0, w, h, 0, h};
    grow(*sp.bounds, tx(pm, corners));
    bool found = false;
    for (ImageRef& img : images_) {
      if (img.ref == ref) {
        ++img.uses;
        found = true;
        break;
      }
    }
    if (!found) {
      images_.push_back(ImageRef{ref, base_name(ref), 1});
    }
    ++stats_.prims;
  }

  /// Multileader: leader lines (+ closed arrowhead and landing), then its text or block.
  // NOLINTNEXTLINE(misc-no-recursion)
  void mleader(Space_& sp, const Style& st, const Value& e, const Ctx& ctx) {
    const Value& cx = e["context"];
    const double arrow = nn(cx["arrowSize"], nn(e["arrowSize"], 0));
    for (const Value& sec : array_or_empty(cx["roots"])) {
      const Value& last = sec["connection"];
      for (const Value& ll : array_or_empty(sec["lines"])) {
        std::vector<const Value*> vs;
        for (const Value& p : array_or_empty(ll["points"])) {
          if (p.truthy()) {
            vs.push_back(&p);
          }
        }
        if (vs.empty() && !last.truthy()) {
          continue;
        }
        if (last.truthy()) {
          vs.push_back(&last);
        }
        const Value& lt = e["leaderType"];
        if (!lt.is_number() || lt.as_number() != 0) {
          std::vector<double> pts;
          for (const Value* p : vs) {
            pts.push_back(num((*p).get("x")));
            pts.push_back(num((*p).get("y")));
          }
          line(sp, st, pts);
        }
        if (arrow > 0 && vs.size() > 1) {  // closed filled arrowhead at the first point
          const double ax = num((*vs[0]).get("x"));
          const double ay = num((*vs[0]).get("y"));
          const double bxx = num((*vs[1]).get("x"));
          const double byy = num((*vs[1]).get("y"));
          const double d = std::hypot(bxx - ax, byy - ay);
          if (d > 0) {
            const double ux = (bxx - ax) / d;
            const double uy = (byy - ay) / d;
            const double bx = ax + ux * arrow;
            const double by = ay + uy * arrow;
            const double hw = arrow / 6;
            const std::array<double, 6> tri = {ax, ay, bx - uy * hw, by + ux * hw, bx + uy * hw, by - ux * hw};
            std::vector<std::vector<double>> rings;
            rings.push_back(tx(st.m, tri));
            fill(sp, st, std::move(rings), false);
          }
        }
      }
      const Value& ld = sec["landingDistance"];
      const Value& dlv = ld.is_undefined() || ld.is_null() ? sec["doglegLength"] : ld;
      const Value& dir = sec["direction"];
      if (last.truthy() && e["doglegEnabled"].truthy() && dir.truthy() && dlv.truthy()) {
        const double dl = num(dlv);
        const double lx = num(last.get("x"));
        const double ly = num(last.get("y"));
        const std::array<double, 4> seg = {lx, ly, lx + num(dir.get("x")) * dl, ly + num(dir.get("y")) * dl};
        line(sp, st, seg);
      }
    }
    const Value& t = cx["hasText"].truthy() ? cx["text"] : cx["hasText"];
    if (t.truthy() && t["label"].truthy() && t["location"].truthy()) {
      Value m = Value::object();
      m.set("value", t["label"]);
      m.set("height", cx["textHeight"]);
      m.set("insert", t["location"]);
      m.set("attach", t["alignment"]);
      m.set("xAxis", t["direction"]);
      m.set("lineSpacing", t["lineSpacing"]);
      mtext(sp, st, m);
    }
    const Value& bc = cx["hasBlock"].truthy() ? cx["block"] : cx["hasBlock"];
    if (bc.truthy()) {
      const Value& bh =
          bc["blockHandle"].is_undefined() || bc["blockHandle"].is_null() ? bc["handle"] : bc["blockHandle"];
      const auto it = ctx.parts->by_record.find(text_of(bh));
      const Value* blk = it != ctx.parts->by_record.end() ? it->second : nullptr;
      if (blk != nullptr) {
        const Value& tm = bc["matrix"];
        Affine bm{};
        if (tm.truthy() && tm.is_array() && tm.as_array().size() == 16) {
          const Array& a = tm.as_array();
          bm = {num(a[0]), num(a[4]), num(a[1]), num(a[5]), num(a[3]), num(a[7])};
        } else {
          bm = mul(translate(num(bc["location"].get("x")), num(bc["location"].get("y"))),
                   mul(rotate(or_num(bc["rotation"], 0)),
                       scale(or_num(bc["scale"].get("x"), 1), or_num(bc["scale"].get("y"), 1))));
        }
        const Value& bb = (*blk)["base"];
        const double bx = bb.truthy() ? num(bb.get("x")) : 0;
        const double by = bb.truthy() ? num(bb.get("y")) : 0;
        Ctx sub = sub_ctx(ctx, mul(st.m, mul(bm, translate(-bx, -by))), st, nullptr, {});
        std::unordered_map<std::string, std::string> attrs;
        for (const Value& a : array_or_empty(e["blockLabels"])) {
          attrs.insert_or_assign(text_of(a["attdefHandle"]), text_of(a["text"]));
        }
        sub.attrs = &attrs;
        for (const Value& be : array_or_empty((*blk)["entities"])) {
          emit(sp, be, sub);
        }
      }
    }
    ++stats_.prims;
  }

  /// TEXT / ATTRIB / ATTDEF (model fields: value, height, insert, align, hAlign, vAlign, rotation, generation).
  void text(Space_& sp, const Style& st, const Value& t, const std::string* value) {
    const std::string s = plain_text(value != nullptr ? *value : text_or_empty(t["value"]));
    if (base::js_blank(s) || !t["height"].truthy()) {
      return;
    }
    const double h_align = or_num(t["hAlign"], 0);
    const double v_align = or_num(t["vAlign"], 0);
    const bool aligned = h_align != 0 || v_align != 0;
    const Value& al = t["align"];
    const Value& p = aligned && al.truthy() && (al.get("x").truthy() || al.get("y").truthy()) ? al : t["insert"];
    static constexpr std::array<double, 6> kHa = {0, 1, 2,
                                                  0, 1, 0};  // left, centre, right; aligned/fit → left; middle → centre
    std::optional<double> ha;
    if (h_align >= 0 && h_align < 6 && h_align == std::floor(h_align)) {
      ha = kHa[static_cast<std::size_t>(h_align)];
    }
    const Value& hv = t["hAlign"];
    const double va = hv.is_number() && hv.as_number() == 4 ? 2 : v_align;  // 0 baseline, 1 bottom, 2 middle, 3 top
    place(sp, st, num(p.get("x")), num(p.get("y")), num(t["height"]), or_num(t["rotation"], 0), s, ha, va,
          (int32(t["generation"]) & 2) == 2);
  }

  /// MTEXT (model fields: value, height, insert, attach, xAxis, lineSpacing).
  void mtext(Space_& sp, const Style& st, const Value& e) {
    std::vector<std::string> lines = mtext_lines(text_or_empty(e["value"]));
    {
      std::vector<std::string> kept;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!base::js_blank(lines[i]) || i + 1 < lines.size()) {
          kept.push_back(std::move(lines[i]));
        }
      }
      lines = std::move(kept);
    }
    if (lines.empty() || !e["height"].truthy() || !e["insert"].truthy()) {
      return;
    }
    const Value& xa = e["xAxis"];
    const double rot = xa.truthy() && (xa.get("x").truthy() || xa.get("y").truthy())
                           ? std::atan2(num(xa.get("y")), num(xa.get("x")))
                           : or_num(e["rotation"], 0);
    const double ap = or_num(e["attach"], 1) - 1;
    const double ha = std::fmod(ap, 3);
    const double row = std::floor(ap / 3);  // 0 top, 1 middle, 2 bottom
    const double h = num(e["height"]);
    const double gap = h * 1.667 * or_num(e["lineSpacing"], 1);
    const double total = gap * static_cast<double>(lines.size() - 1);
    double top = total;  // baseline of the first line, relative to the insertion point
    if (row == 0) {
      top = -h;
    } else if (row == 1) {
      top = total / 2 - h / 2;
    }
    const double cr = std::cos(rot);
    const double sr = std::sin(rot);
    const double ix = num(e["insert"].get("x"));
    const double iy = num(e["insert"].get("y"));
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (base::js_blank(lines[i])) {
        continue;
      }
      const double dy = top - gap * static_cast<double>(i);
      place(sp, st, ix - dy * sr, iy + dy * cr, h, rot, lines[i], ha, 0, false);
    }
  }

  /// One line of text into the batch, transformed by m (position, height and rotation).
  void place(Space_& sp, const Style& st, double x, double y, double h, double rot, const std::string& s,
             std::optional<double> ha, double va, bool mirror) {
    const Affine& m = st.m;
    const double X = m[0] * x + m[2] * y + m[4];
    const double Y = m[1] * x + m[3] * y + m[5];
    const double sx = std::hypot(m[0], m[1]);
    const double sy = std::hypot(m[2], m[3]);
    const double det = m[0] * m[3] - m[1] * m[2];
    const double angle = std::atan2(m[1], m[0]) + (det < 0 ? -rot : rot);
    const double H = h * sy;
    batch(sp, st.layer, st.color, st.lw).texts.push_back(Text{X, Y, H, angle, s, ha, va, mirror != (det < 0)});
    coords_ += 4 + static_cast<double>(s.size()) / 8;
    // Rough extent so fit-to-drawing includes text: assume 0.6 h per character.
    const double w =
        static_cast<double>(base::utf16_length(s)) * H * 0.6 * (sx / (sy != 0 && !std::isnan(sy) ? sy : 1));
    const std::array<double, 4> ext = {X, Y, X + w * std::cos(angle), Y + w * std::sin(angle)};
    grow(*sp.bounds, ext);
  }

  const Value* db_;  // the model compile_drawing was given: it outlives the compiler
  Parts parts_;
  std::unordered_map<std::string, XInfo> xinfo_;
  std::vector<Layer> layers_;
  std::unordered_map<std::string, std::size_t> layer_index_;
  std::vector<ImageRef> images_;
  Stats stats_;
  double coords_ = 0;
  double visits_ = 0;
  double max_coords_;
  double max_visits_;
};

// ---------------------------------------------------------------- JSON
void num_json(std::string& o, double d) {
  if (std::isfinite(d)) {
    base::append_number(o, d);
  } else {
    o += "null";
  }
}
void key(std::string& o, std::string_view k) {
  base::append_json_string(o, k);
  o += ':';
}
void flat_json(std::string& o, const std::vector<double>& v) {
  o += '[';
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) {
      o += ',';
    }
    num_json(o, v[i]);
  }
  o += ']';
}
void bounds_json(std::string& o, const Bounds& b) {
  o += '{';
  key(o, "minX");
  num_json(o, b.min_x);
  o += ',';
  key(o, "minY");
  num_json(o, b.min_y);
  o += ',';
  key(o, "maxX");
  num_json(o, b.max_x);
  o += ',';
  key(o, "maxY");
  num_json(o, b.max_y);
  o += '}';
}
template <class T, class F>
void list_json(std::string& o, const std::vector<T>& v, F each) {
  o += '[';
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) {
      o += ',';
    }
    each(v[i]);
  }
  o += ']';
}

}  // namespace

std::string aci_color(double i) {
  if (i == 7 || i == 255 || i <= 0 || i > 255) {
    return "ink";
  }
  static constexpr std::array<std::string_view, 10> kBase = {"",        "#ff0000", "#ffff00", "#00ff00", "#00ffff",
                                                             "#0000ff", "#ff00ff", "",        "#808080", "#c0c0c0"};
  if (i >= 1 && i <= 9 && i == std::floor(i) && !kBase[static_cast<std::size_t>(i)].empty()) {
    return std::string(kBase[static_cast<std::size_t>(i)]);
  }
  if (i >= 250) {
    static constexpr std::array<std::string_view, 5> kGrey = {"#333333", "#505050", "#696969", "#828282", "#bebebe"};
    if (i == std::floor(i)) {
      return std::string(kGrey[static_cast<std::size_t>(i - 250)]);
    }
  }
  const double hue = std::floor((i - 10) / 10) * 15;
  const double j = std::fmod(i - 10, 10);
  static constexpr std::array<double, 10> kV = {1, 1, 0.8, 0.8, 0.6, 0.6, 0.5, 0.5, 0.3, 0.3};
  // NOLINTNEXTLINE(readability-simplify-boolean-expr) — NaN must take this branch
  if (!(j >= 0 && j < 10 && j == std::floor(j))) {
    return hsv_hex(hue, 0.5, kNaN);  // the JS looks up v[j] = undefined
  }
  const auto ji = static_cast<std::size_t>(j);
  return hsv_hex(hue, ji % 2 == 0 ? 1 : 0.5, kV[ji]);
}

Drawing compile_drawing(const Value& model, const CompileOptions& options) {
  Compiler c(model, options);
  return c.run(options);
}

void write_json(const Drawing& d, std::string& o) {
  o += '{';
  key(o, "version");
  base::append_json_string(o, d.version);
  o += ',';
  key(o, "release");
  base::append_json_string(o, d.release);
  o += ',';
  key(o, "units");
  base::append_json_string(o, d.units);
  o += ',';
  key(o, "xrefs");
  list_json(o, d.xrefs, [&](const auto& x) {
    o += '{';
    key(o, "name");
    base::append_json_string(o, x.first);
    o += ',';
    key(o, "loaded");
    o += x.second ? "true" : "false";
    o += '}';
  });
  o += ',';
  key(o, "images");
  list_json(o, d.images, [&](const ImageRef& x) {
    o += '{';
    key(o, "ref");
    base::append_json_string(o, x.ref);
    o += ',';
    key(o, "name");
    base::append_json_string(o, x.name);
    o += ',';
    key(o, "uses");
    num_json(o, x.uses);
    o += '}';
  });
  o += ',';
  key(o, "layers");
  list_json(o, d.layers, [&](const Layer& l) {
    o += '{';
    key(o, "name");
    base::append_json_string(o, l.name);
    o += ',';
    key(o, "color");
    base::append_json_string(o, l.color);
    o += ',';
    key(o, "on");
    o += l.on ? "true" : "false";
    o += ',';
    key(o, "lw");
    num_json(o, l.lw);
    o += ',';
    key(o, "plot");
    o += l.plot ? "true" : "false";
    o += '}';
  });
  o += ',';
  key(o, "spaces");
  list_json(o, d.spaces, [&](const Space& s) {
    o += '{';
    key(o, "key");
    base::append_json_string(o, s.key);
    o += ',';
    key(o, "name");
    base::append_json_string(o, s.name);
    o += ',';
    key(o, "kind");
    base::append_json_string(o, s.kind);
    o += ',';
    key(o, "batches");
    list_json(o, s.batches, [&](const Batch& b) {
      o += '{';
      key(o, "layer");
      base::append_json_string(o, b.layer);
      o += ',';
      key(o, "color");
      base::append_json_string(o, b.color);
      o += ',';
      key(o, "lw");
      num_json(o, b.lw);
      o += ',';
      key(o, "lines");
      list_json(o, b.lines, [&](const std::vector<double>& l) { flat_json(o, l); });
      o += ',';
      key(o, "fills");
      list_json(o, b.fills, [&](const Fill& f) {
        o += '{';
        key(o, "rings");
        list_json(o, f.rings, [&](const std::vector<double>& r) { flat_json(o, r); });
        o += ',';
        key(o, "pattern");
        o += f.pattern ? "true" : "false";
        o += '}';
      });
      o += ',';
      key(o, "texts");
      list_json(o, b.texts, [&](const Text& t) {
        o += '{';
        key(o, "x");
        num_json(o, t.x);
        o += ',';
        key(o, "y");
        num_json(o, t.y);
        o += ',';
        key(o, "h");
        num_json(o, t.h);
        o += ',';
        key(o, "rot");
        num_json(o, t.rot);
        o += ',';
        key(o, "s");
        base::append_json_string(o, t.s);
        if (t.ha) {
          o += ',';
          key(o, "ha");
          num_json(o, *t.ha);
        }
        o += ',';
        key(o, "va");
        num_json(o, t.va);
        o += ',';
        key(o, "mirror");
        o += t.mirror ? "true" : "false";
        o += '}';
      });
      o += '}';
    });
    o += ',';
    key(o, "bounds");
    bounds_json(o, s.bounds);
    if (s.kind == "paper") {
      o += ',';
      key(o, "paper");
      if (s.paper) {
        bounds_json(o, *s.paper);
      } else {
        o += "null";
      }
      o += ',';
      key(o, "paperUnit");
      base::append_json_string(o, s.paper_unit);
    }
    o += ',';
    key(o, "viewports");
    list_json(o, s.viewports, [&](const Viewport& v) {
      o += '{';
      const std::array<std::pair<std::string_view, double>, 8> f = {{{"cx", v.cx},
                                                                     {"cy", v.cy},
                                                                     {"w", v.w},
                                                                     {"h", v.h},
                                                                     {"vx", v.vx},
                                                                     {"vy", v.vy},
                                                                     {"viewHeight", v.view_height},
                                                                     {"twist", v.twist}}};
      for (const auto& [k, x] : f) {
        key(o, k);
        num_json(o, x);
        o += ',';
      }
      key(o, "frozen");
      list_json(o, v.frozen, [&](const std::string& h) { base::append_json_string(o, h); });
      o += '}';
    });
    o += ',';
    key(o, "images");
    list_json(o, s.images, [&](const Picture& p) {
      o += '{';
      key(o, "m");
      flat_json(o, std::vector<double>(p.m.begin(), p.m.end()));
      o += ',';
      key(o, "w");
      num_json(o, p.w);
      o += ',';
      key(o, "h");
      num_json(o, p.h);
      o += ',';
      key(o, "ref");
      base::append_json_string(o, p.ref);
      o += ',';
      key(o, "layer");
      base::append_json_string(o, p.layer);
      o += '}';
    });
    o += '}';
  });
  o += ',';
  key(o, "stats");
  o += '{';
  key(o, "entities");
  num_json(o, d.stats.entities);
  o += ',';
  key(o, "unsupported");
  o += '{';
  for (std::size_t i = 0; i < d.stats.unsupported.size(); ++i) {
    if (i != 0) {
      o += ',';
    }
    key(o, d.stats.unsupported[i].first);
    num_json(o, d.stats.unsupported[i].second);
  }
  o += '}';
  o += ',';
  key(o, "truncated");
  o += d.stats.truncated ? "true" : "false";
  o += ',';
  key(o, "prims");
  num_json(o, d.stats.prims);
  o += ',';
  key(o, "xrefEntities");
  num_json(o, d.stats.xref_entities);
  o += "}}";
}

}  // namespace cc::drawing
