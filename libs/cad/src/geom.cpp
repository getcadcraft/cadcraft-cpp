// SPDX-License-Identifier: Apache-2.0
// font.js, geom.js (text layout, primitives and bounds, attribute texts), attrs.js worldAttribs and leader.js, ported
// from CadCraft 0.5 with the JS's order of operations.
#include "cc/cad/geom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "prims.hpp"
#include "props.hpp"
#include "util.hpp"

namespace cc::cad {

using detail::g;
using detail::Mat;
using detail::num;
using detail::or_;
using detail::str;

namespace {

constexpr double kEps = 1e-9;

// ---------------------------------------------------------------- font.js

constexpr std::array<std::uint16_t, 95> kHelv = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333,  278, 278, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667,
    611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667,  611, 722, 667, 944, 667,
    667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278,  556, 556, 222, 222, 500,
    222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500,  500, 334, 260, 334, 584};

struct WidthRun {
  std::uint32_t first, last;
  std::uint16_t width;
};
constexpr auto kWidthRuns = std::to_array<WidthRun>({
#include "font_widths.inc"
});

double char_advance(std::uint32_t cp) {
  if (cp >= 32 && cp < 127) {
    return kHelv.at(cp - 32);
  }
  // an iterator: a pointer with some standard libraries, a class with others (Emscripten's)
  // NOLINTNEXTLINE(readability-qualified-auto)
  const auto it = std::ranges::lower_bound(kWidthRuns, cp, {}, &WidthRun::last);
  if (it != kWidthRuns.end() && it->first <= cp) {
    return it->width;
  }
  return cp >= 0x2e80 ? 1000 : 556;
}

/// One code point of UTF-8 / WTF-8 text at i (a bad byte counts as itself); i advances.
std::uint32_t next_code_point(std::string_view s, std::size_t& i) {
  const auto b = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[k])); };
  const std::uint32_t c = b(i);
  std::size_t n = 0;
  std::uint32_t cp = c;
  if (c >= 0xF0 && i + 3 < s.size() + 0) {
    n = 3;
    cp = c & 0x07U;
  } else if (c >= 0xE0 && i + 2 < s.size()) {
    n = 2;
    cp = c & 0x0FU;
  } else if (c >= 0xC0 && i + 1 < s.size()) {
    n = 1;
    cp = c & 0x1FU;
  }
  for (std::size_t k = 1; k <= n; ++k) {
    cp = (cp << 6U) | (b(i + k) & 0x3FU);
  }
  i += n + 1;
  return cp;
}

}  // namespace

// ---------------------------------------------------------------- primitives

namespace detail {

P2 m_apply(const Mat& m, double x, double y) {
  return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

double dist(double ax, double ay, double bx, double by) {
  return base::js_hypot(bx - ax, by - ay);
}

/// geom.js onSweep(a, a0, a1, tol)
bool on_sweep(double a, double a0, double a1, double tol) {
  const double sweep = a1 - a0;
  if (sweep >= kTau - tol) {
    return true;
  }
  const double d = detail::norm_angle(a - a0);
  return d <= sweep + tol || d >= kTau - tol;
}

std::pair<double, double> arc_range(double a0, double a1) {
  a0 = detail::norm_angle(a0);
  a1 = detail::norm_angle(a1);
  if (a1 <= a0 + kEps) {
    a1 += kTau;
  }
  return {a0, a1};
}

P2 prim_point(const Prim& p, double t) {
  if (!p.arc) {
    return {p.x1 + (p.x2 - p.x1) * t, p.y1 + (p.y2 - p.y1) * t};
  }
  const double a = p.a0 + (p.a1 - p.a0) * t;
  return {p.cx + p.r * cc::base::js_cos(a), p.cy + p.r * cc::base::js_sin(a)};
}

Prim line_prim(double x1, double y1, double x2, double y2) {
  Prim p;
  p.x1 = x1;
  p.y1 = y1;
  p.x2 = x2;
  p.y2 = y2;
  return p;
}

/// geom.js bulgeArc(p, q, b)
Prim bulge_arc(double px, double py, double qx, double qy, double b) {
  const double theta = 4 * base::js_atan(b);
  const double c = dist(px, py, qx, qy);
  const double r = std::abs(c / (2 * cc::base::js_sin(theta / 2)));
  const double mx = (px + qx) / 2;
  const double my = (py + qy) / 2;
  const double h = r * cc::base::js_cos(theta / 2);
  const double ux = (qx - px) / c;
  const double uy = (qy - py) / c;
  const double s = b > 0 ? 1 : -1;
  const double cx = mx - uy * h * s;
  const double cy = my + ux * h * s;
  double a0 = base::js_atan2(py - cy, px - cx);
  double a1 = base::js_atan2(qy - cy, qx - cx);
  if (b < 0) {
    std::swap(a0, a1);
  }
  a0 = detail::norm_angle(a0);
  a1 = detail::norm_angle(a1);
  if (a1 <= a0) {
    a1 += kTau;
  }
  Prim p;
  p.arc = true;
  p.cx = cx;
  p.cy = cy;
  p.r = r;
  p.a0 = a0;
  p.a1 = a1;
  p.rev = b < 0;
  return p;
}

/// geom.js mSimilar(m): { s, flip } of a uniform scale + rotation (+ mirror), else false.
static bool m_similar(const Mat& m, double& s, bool& flip) {
  const double sx = base::js_hypot(m[0], m[1]);
  const double sy = base::js_hypot(m[2], m[3]);
  const double dot = m[0] * m[2] + m[1] * m[3];
  if (std::abs(sx - sy) > 1e-9 * detail::js_max(sx, sy) || std::abs(dot) > 1e-9 * sx * sy) {
    return false;
  }
  s = sx;
  flip = detail::m_det(m) < 0;
  return true;
}

/// geom.js txPrim(p, m)
void tx_prim(const Prim& p, const Mat& m, std::vector<Prim>& out) {
  if (!p.arc) {
    const P2 a = m_apply(m, p.x1, p.y1);
    const P2 b = m_apply(m, p.x2, p.y2);
    out.push_back(line_prim(a.x, a.y, b.x, b.y));
    return;
  }
  double s = 1;
  bool flip = false;
  if (m_similar(m, s, flip)) {
    const P2 c = m_apply(m, p.cx, p.cy);
    Prim q;
    q.arc = true;
    q.cx = c.x;
    q.cy = c.y;
    q.r = p.r * s;
    if (p.full) {
      q.a0 = 0;
      q.a1 = kTau;
      q.full = true;
      out.push_back(q);
      return;
    }
    const P2 p0 = prim_point(p, flip ? 1 : 0);
    const P2 st = m_apply(m, p0.x, p0.y);
    const double a0 = detail::norm_angle(base::js_atan2(st.y - c.y, st.x - c.x));
    q.a0 = a0;
    q.a1 = a0 + (p.a1 - p.a0);
    out.push_back(q);
    return;
  }
  const double n = std::max(8.0, std::ceil(48 * (p.a1 - p.a0) / kTau));
  P2 q0 = prim_point(p, 0);
  P2 prev = m_apply(m, q0.x, q0.y);
  // JS's `for (i = 1; i <= n; i++)`: n is a whole number (a NaN sweep runs no step; a hostile one stops at 10^6)
  const auto steps = std::isfinite(n) ? static_cast<std::int64_t>(std::min(n, 1e6)) : 0;
  for (std::int64_t k = 1; k <= steps; ++k) {
    const auto i = static_cast<double>(k);
    const P2 qi = prim_point(p, i / n);
    const P2 q = m_apply(m, qi.x, qi.y);
    out.push_back(line_prim(prev.x, prev.y, q.x, q.y));
    prev = q;
  }
}

static void ring_prims(const base::Value& flat, std::vector<Prim>& out) {
  if (!flat.is_array()) {
    return;
  }
  const auto& f = flat.as_array();
  for (std::size_t i = 0; i + 3 < f.size(); i += 2) {
    out.push_back(line_prim(num(f[i]), num(f[i + 1]), num(f[i + 2]), num(f[i + 3])));
  }
}

void poly_prims(const base::Value& e, std::vector<Prim>& out) {
  const base::Value pts = g(e, "pts");
  if (!pts.is_array()) {
    return;
  }
  const auto& v = pts.as_array();
  const std::size_t n = v.size();
  if (n == 0) {
    return;
  }
  const std::size_t segs = g(e, "closed").truthy() ? n : n - 1;
  for (std::size_t i = 0; i < segs; ++i) {
    const base::Value& p = v[i];
    const base::Value& q = v[(i + 1) % n];
    const double px = num(g(p, "x"));
    const double py = num(g(p, "y"));
    const double qx = num(g(q, "x"));
    const double qy = num(g(q, "y"));
    const base::Value pb = g(p, "b");
    const double b = pb.truthy() ? num(pb) : 0;
    if (std::abs(b) > 1e-12 && dist(px, py, qx, qy) > kEps) {
      out.push_back(bulge_arc(px, py, qx, qy, b));
    } else {
      out.push_back(line_prim(px, py, qx, qy));
    }
  }
}

std::vector<P2> ellipse_points(const base::Value& e, double steps) {
  const double mx = num(g(e, "mx"));
  const double my = num(g(e, "my"));
  const double ma = base::js_hypot(mx, my);
  const double mi = ma * num(g(e, "ratio"));
  const double rot = base::js_atan2(my, mx);
  const double e0 = num(g(e, "t0"));
  const double e1 = num(g(e, "t1"));
  double t0 = 0;
  double t1 = kTau;
  if (!(e1 - e0 >= kTau - kEps)) {
    std::tie(t0, t1) = arc_range(e0, e1);
  }
  const double n = std::max(8.0, std::ceil(steps * (t1 - t0) / kTau));
  const double c = cc::base::js_cos(rot);
  const double s = cc::base::js_sin(rot);
  const double cx = num(g(e, "cx"));
  const double cy = num(g(e, "cy"));
  std::vector<P2> pts;
  const auto n_steps = std::isfinite(n) ? static_cast<std::int64_t>(std::min(n, 1e6)) : -1;
  for (std::int64_t k = 0; k <= n_steps; ++k) {  // JS's `for (i = 0; i <= n; i++)`, n whole
    const auto i = static_cast<double>(k);
    const double t = t0 + (t1 - t0) * i / n;
    const double x = ma * cc::base::js_cos(t);
    const double y = mi * cc::base::js_sin(t);
    pts.push_back({cx + x * c - y * s, cy + x * s + y * c});
  }
  return pts;
}

static void text_box_prims(const base::Value& e, std::vector<Prim>& out) {
  const auto c = text_corners(e);
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(line_prim(c.at(i).x, c.at(i).y, c.at((i + 1) % 4).x, c.at((i + 1) % 4).y));
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — bounded by depth (17 levels), as geom.js
void prims(const base::Value& e, const BlockLookup& blocks, int depth, std::vector<Prim>& out) {
  const base::Value type = g(e, "type");
  const std::string t = type.is_string() ? type.as_string() : std::string();
  if (t == "line") {
    out.push_back(line_prim(num(g(e, "x1")), num(g(e, "y1")), num(g(e, "x2")), num(g(e, "y2"))));
  } else if (t == "circle") {
    Prim p;
    p.arc = true;
    p.cx = num(g(e, "cx"));
    p.cy = num(g(e, "cy"));
    p.r = num(g(e, "r"));
    p.a1 = kTau;
    p.full = true;
    out.push_back(p);
  } else if (t == "arc") {
    Prim p;
    p.arc = true;
    p.cx = num(g(e, "cx"));
    p.cy = num(g(e, "cy"));
    p.r = num(g(e, "r"));
    std::tie(p.a0, p.a1) = arc_range(num(g(e, "a0")), num(g(e, "a1")));
    out.push_back(p);
  } else if (t == "poly") {
    poly_prims(e, out);
  } else if (t == "ellipse") {
    const auto p = ellipse_points(e);
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
      out.push_back(line_prim(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y));
    }
  } else if (t == "text") {
    const std::size_t from = out.size();
    text_box_prims(e, out);
    for (std::size_t i = from; i < out.size(); ++i) {
      out[i].box = true;
    }
  } else if (t == "hatch") {
    const base::Value loops = g(e, "loops");
    if (loops.is_array()) {
      for (const base::Value& l : loops.as_array()) {
        ring_prims(l, out);
      }
    }
  } else if (t == "insert") {
    const base::Value bn = g(e, "block");
    const base::Value* blk = blocks ? blocks(str(bn)) : nullptr;
    if (blk == nullptr || depth > 16) {
      return;
    }
    const Mat m = detail::insert_matrix(e, *blk);
    std::vector<Prim> sub;
    const base::Value ents = g(*blk, "entities");
    if (ents.is_array()) {
      for (const base::Value& c : ents.as_array()) {
        sub.clear();
        prims(c, blocks, depth + 1, sub);
        for (const Prim& p : sub) {
          tx_prim(p, m, out);
        }
      }
    }
    for (const base::Value& at : attr_texts(e, *blk)) {
      sub.clear();
      prims(at, blocks, 0, sub);
      for (const Prim& p : sub) {
        const std::size_t from = out.size();
        tx_prim(p, m, out);
        for (std::size_t i = from; i < out.size(); ++i) {
          out[i].box = true;  // attribute text counts for picking and bounds, not for snaps
        }
      }
    }
  } else if (t == "dim") {
    // dimParts: the imported block while it has one, else the dimension's own geometry
    const base::Value bn = g(e, "block");
    const base::Value* blk = bn.truthy() && blocks ? blocks(str(bn)) : nullptr;
    base::Value parts = blk != nullptr ? g(*blk, "entities") : base::Value(dim_geometry(e));
    if (depth > 16 || !parts.is_array()) {
      return;
    }
    for (const base::Value& c : parts.as_array()) {
      // arrowheads and the text box count for picking, not for snaps
      const bool box = g(c, "type") == base::Value("hatch") || g(c, "type") == base::Value("text");
      const std::size_t from = out.size();
      prims(c, blocks, depth + 1, out);
      for (std::size_t i = from; box && i < out.size(); ++i) {
        out[i].box = true;
      }
    }
  } else if (t == "viewport") {
    const double hw = num(g(e, "w")) / 2;
    const double hh = num(g(e, "h")) / 2;
    const double x = num(g(e, "x"));
    const double y = num(g(e, "y"));
    const std::array<P2, 4> c = {P2{x - hw, y - hh}, P2{x + hw, y - hh}, P2{x + hw, y + hh}, P2{x - hw, y + hh}};
    for (std::size_t i = 0; i < 4; ++i) {
      out.push_back(line_prim(c.at(i).x, c.at(i).y, c.at((i + 1) % 4).x, c.at((i + 1) % 4).y));
    }
  } else if (t == "xline" || t == "ray") {
    constexpr double kFar = 1e9;
    const double s = t == "ray" ? 0 : -kFar;
    const double x = num(g(e, "x"));
    const double y = num(g(e, "y"));
    const double dx = num(g(e, "dx"));
    const double dy = num(g(e, "dy"));
    out.push_back(line_prim(x + dx * s, y + dy * s, x + dx * kFar, y + dy * kFar));
  } else if (t == "mleader") {
    for (const base::Value& c : leader_geometry(e)) {
      const bool box = g(c, "type") == base::Value("hatch") || g(c, "type") == base::Value("text");
      const std::size_t from = out.size();
      prims(c, blocks, depth + 1, out);
      for (std::size_t i = from; box && i < out.size(); ++i) {
        out[i].box = true;
      }
    }
  }
}

}  // namespace detail

namespace {

using detail::m_apply;
using detail::on_sweep;
using detail::prim_point;
using detail::prims;

void grow_pt(Bounds& b, double x, double y) {
  // `if (x < b.minX) b.minX = x` (a NaN changes nothing, unlike std::min's argument order)
  b.min_x = x < b.min_x ? x : b.min_x;
  b.min_y = y < b.min_y ? y : b.min_y;
  b.max_x = x > b.max_x ? x : b.max_x;
  b.max_y = y > b.max_y ? y : b.max_y;
}

void prim_bounds(const Prim& p, Bounds& b) {
  if (!p.arc) {
    grow_pt(b, p.x1, p.y1);
    grow_pt(b, p.x2, p.y2);
    return;
  }
  const P2 s = prim_point(p, 0);
  const P2 e = prim_point(p, 1);
  grow_pt(b, s.x, s.y);
  grow_pt(b, e.x, e.y);
  for (int k = 0; k < 4; ++k) {
    const double a = k * std::numbers::pi / 2;
    if (on_sweep(a, p.a0, p.a1)) {
      grow_pt(b, p.cx + p.r * cc::base::js_cos(a), p.cy + p.r * cc::base::js_sin(a));
    }
  }
}

/// Break one line into lines no wider than the wrap width (geom.js wrapLine).
std::vector<std::string> wrap_line(const std::string& line, const base::Value& e, double ww) {
  if (text_width(line, e) <= ww) {
    return {line};
  }
  std::vector<std::string> out;
  std::string cur;
  // line.split(/(?<= )/): pieces that each end in a space (the last may not)
  std::size_t start = 0;
  while (start < line.size()) {
    std::size_t sp = line.find(' ', start);
    const std::size_t end = sp == std::string::npos ? line.size() : sp + 1;
    const std::string word = line.substr(start, end - start);
    start = end;
    const std::string joined = cur + word;
    if (!cur.empty() && text_width(base::js_trim_end(joined), e) > ww) {
      out.emplace_back(base::js_trim_end(cur));
      cur = std::string(base::js_trim_start(word));
    } else {
      cur += word;
    }
  }
  if (!cur.empty() || out.empty()) {
    out.emplace_back(base::js_trim_end(cur));
  }
  return out;
}

base::Value text_entity_at(const std::string& tag, const std::string& s, const base::Value& d) {
  // { type: 'text', layer: d.layer ?? '0', color: d.color ?? null, x: +d.x || 0, … }
  const auto plus_or0 = [](const base::Value& v) {
    const double x = num(v);
    return std::isnan(x) || x == 0 ? 0.0 : x;
  };
  const base::Value h = g(d, "h");
  const base::Value rot = g(d, "rot");
  const base::Value ha = g(d, "ha");
  const base::Value va = g(d, "va");
  const base::Value wf = g(d, "wf");
  base::Value o = base::make_object({{"type", "text"},
                                     {"layer", detail::nn(g(d, "layer"), "0")},
                                     {"color", detail::nn(g(d, "color"), nullptr)},
                                     {"x", plus_or0(g(d, "x"))},
                                     {"y", plus_or0(g(d, "y"))},
                                     {"h", num(h) > 0 ? h : base::Value(2.5)},
                                     {"rot", or_(rot, 0)},
                                     {"ha", or_(ha, "left")},
                                     {"va", or_(va, "baseline")},
                                     {"wf", or_(wf, 1)},
                                     {"s", s},
                                     {"tag", tag}});
  if (g(d, "invisible").truthy()) {
    o.set("invisible", true);
  }
  return o;
}

}  // namespace

// ---------------------------------------------------------------- font.js / text

double text_advance(std::string_view s) {
  double w = 0;
  for (std::size_t i = 0; i < s.size();) {
    w += char_advance(next_code_point(s, i));
  }
  return w;
}

double text_width(std::string_view s, const base::Value& e) {
  const base::Value wf = g(e, "wf");
  return text_advance(s) / 1000 * num(g(e, "h")) * kFontEm * (wf.truthy() ? num(wf) : 1);
}

TextLayout text_layout(const base::Value& e) {
  TextLayout lay;
  const std::string s = str(g(e, "s"));
  std::vector<std::string> lines;
  for (std::size_t start = 0;;) {
    const std::size_t nl = s.find('\n', start);
    lines.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) {
      break;
    }
    start = nl + 1;
  }
  const base::Value wwv = g(e, "ww");
  if (num(wwv) > 0) {
    const double ww = num(wwv);
    std::vector<std::string> wrapped;
    for (const auto& l : lines) {
      for (auto& w : wrap_line(l, e, ww)) {
        wrapped.push_back(std::move(w));
      }
    }
    lines = std::move(wrapped);
  }
  const double h = num(g(e, "h"));
  double w = -std::numeric_limits<double>::infinity();
  for (const auto& l : lines) {
    const double tw = text_width(l, e);
    lay.widths.push_back(tw);
    w = detail::js_max(w, tw);
  }
  w = detail::js_max(w, h * 0.4);
  const base::Value lsv = g(e, "ls");
  const double ls = kLineSpacing * (lsv.truthy() ? num(lsv) : 1);
  const double bh = h * (1 + (static_cast<double>(lines.size()) - 1) * ls);
  const base::Value ha = g(e, "ha");
  const base::Value va = g(e, "va");
  double x = 0;
  if (ha == base::Value("center")) {
    x = -w / 2;
  } else if (ha == base::Value("right")) {
    x = -w;
  }
  double top = h;
  if (va == base::Value("top")) {
    top = 0;
  } else if (va == base::Value("middle")) {
    top = bh / 2;
  } else if (va == base::Value("bottom")) {
    top = bh;
  }
  lay.box_x = x;
  lay.box_y = top - bh;
  lay.box_w = w;
  lay.box_h = bh;
  for (std::size_t k = 0; k < lines.size(); ++k) {
    lay.baselines.push_back(top - h - static_cast<double>(k) * h * ls);
  }
  lay.lines = std::move(lines);
  return lay;
}

std::array<P2, 4> text_corners(const base::Value& e) {
  const TextLayout lay = text_layout(e);
  const base::Value rot = g(e, "rot");
  const Mat m =
      detail::m_mul(detail::m_translate(num(g(e, "x")), num(g(e, "y"))), detail::m_rotate(rot.truthy() ? num(rot) : 0));
  const double x0 = lay.box_x;
  const double y0 = lay.box_y;
  return {m_apply(m, x0, y0), m_apply(m, x0 + lay.box_w, y0), m_apply(m, x0 + lay.box_w, y0 + lay.box_h),
          m_apply(m, x0, y0 + lay.box_h)};
}

Bounds entity_bounds(const base::Value& e, const BlockLookup& blocks) {
  Bounds b;
  const base::Value t = g(e, "type");
  if (t == base::Value("xline") || t == base::Value("ray")) {
    return b;
  }
  if (t == base::Value("point")) {
    grow_pt(b, num(g(e, "x")), num(g(e, "y")));
    return b;
  }
  std::vector<Prim> ps;
  prims(e, blocks, 0, ps);
  for (const Prim& p : ps) {
    prim_bounds(p, b);
  }
  if (t == base::Value("insert") && !b.valid()) {
    grow_pt(b, num(g(e, "x")), num(g(e, "y")));
  }
  return b;
}

// ---------------------------------------------------------------- attributes

namespace {

/// attrTexts over the reference's values and the block's definitions. drop_constant: as worldAttribs calls it — the
/// constant definitions and the values under their tags left out (without copying the reference or the block).
base::Array attr_texts_of(const base::Value& vals, const base::Value& attloc, const base::Value& defs,
                          bool drop_constant, bool all) {
  base::Array out;
  std::unordered_set<std::string> constant;
  if (drop_constant && defs.is_array()) {
    for (const base::Value& d : defs.as_array()) {
      if (g(d, "constant").truthy()) {
        constant.insert(str(g(d, "tag")));
      }
    }
  }
  const bool plain = vals.is_object() && !vals.is_point();
  const auto has = [&](const std::string& tag) {
    return plain && vals.find(tag) != nullptr && !constant.contains(tag);
  };
  const auto make = [&](const std::string& tag, const base::Value& value, const base::Value& d) {
    const std::string s = str(detail::nn(value, ""));
    if (!d.truthy() || (!all && (g(d, "invisible").truthy() || base::js_blank(s)))) {
      return;
    }
    out.push_back(text_entity_at(tag, s, d));
  };
  const auto loc_of = [&](const std::string& tag) { return attloc.is_object() ? attloc.get(tag) : base::Value(); };
  std::unordered_set<std::string> seen;
  if (defs.is_array()) {
    for (const base::Value& d : defs.as_array()) {
      const bool is_constant = g(d, "constant").truthy();
      if (drop_constant && is_constant) {
        continue;
      }
      const std::string tag = str(g(d, "tag"));
      seen.insert(tag);
      if (is_constant) {
        make(tag, g(d, "def"), d);
      } else if (has(tag)) {
        const base::Value loc = loc_of(tag);
        make(tag, vals.get(tag), loc.truthy() ? loc : d);
      }
    }
  }
  if (plain) {
    for (const base::Member* m : base::js_members(vals.as_object())) {
      if (!seen.contains(m->key) && !constant.contains(m->key)) {
        const base::Value loc = loc_of(m->key);
        if (loc.truthy()) {
          make(m->key, m->value, loc);
        }
      }
    }
  }
  return out;
}

}  // namespace

base::Array attr_texts(const base::Value& e, const base::Value& blk, bool all) {
  const base::Value attrs = g(e, "attrs");
  const base::Value defs = g(blk, "attdefs");
  const bool has_defs = defs.is_array() && !defs.as_array().empty();
  if (!attrs.truthy() && !has_defs) {
    return {};
  }
  return attr_texts_of(attrs.truthy() ? attrs : base::Value::object(), g(e, "attloc"), defs, false, all);
}

base::Array world_attribs(const base::Value& e, const base::Value& blk) {
  const Mat m = detail::insert_matrix(e, blk);
  // attrTexts({ ...e, attrs: nonConstant(e, blk) }, { ...blk, attdefs: the non-constant ones }, true)
  const base::Value ea = g(e, "attrs");
  base::Array out;
  for (const base::Value& t :
       attr_texts_of(ea.is_object() ? ea : base::Value::object(), g(e, "attloc"), g(blk, "attdefs"), true, true)) {
    base::Value w = detail::tx_text(t, m);
    w.set("tag", g(t, "tag"));
    w.set("invisible", g(t, "invisible").truthy());
    out.push_back(std::move(w));
  }
  return out;
}

std::string one_line(std::string_view s) {
  return detail::one_line(s);
}

// ---------------------------------------------------------------- leader.js

base::Value leader_style(const base::Value& e) {
  base::Value st = base::make_object({{"asz", 2.5}, {"dogleg", 2.5}, {"gap", 1}});
  const base::Value own = g(e, "st");
  if (own.is_object() && !own.is_point()) {
    for (const base::Member* m : base::js_members(own.as_object())) {
      st.set(m->key, m->value);
    }
  }
  return st;
}

Landing landing(const base::Value& e) {
  const base::Value st = leader_style(e);
  const base::Value pts = g(e, "pts");
  const base::Value last = pts.as_array().back();
  const double dir = g(e, "dir") == base::Value(-1) ? -1 : 1;
  Landing L;
  L.start = {num(g(last, "x")), num(g(last, "y"))};
  L.end = {L.start.x + dir * num(st["dogleg"]), L.start.y};
  L.text = {L.end.x + dir * num(st["gap"]), L.end.y};
  return L;
}

base::Array leader_geometry(const base::Value& e) {
  base::Array out;
  const base::Value pts = g(e, "pts");
  if (!pts.is_array() || pts.as_array().size() < 2) {
    return out;
  }
  const base::Value st = leader_style(e);
  const double dir = g(e, "dir") == base::Value(-1) ? -1 : 1;
  const Landing L = landing(e);
  base::Array poly;
  for (const base::Value& p : pts.as_array()) {
    poly.push_back(base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}, {"b", 0}}));
  }
  if (num(st["dogleg"]) > 0) {
    poly.push_back(base::make_object({{"x", L.end.x}, {"y", L.end.y}, {"b", 0}}));
  }
  out.push_back(
      base::make_object({{"layer", "0"}, {"color", 0}, {"type", "poly"}, {"closed", false}, {"pts", std::move(poly)}}));
  const base::Value& tip = pts.as_array()[0];
  const base::Value& nxt = pts.as_array()[1];
  const double tx = num(g(tip, "x"));
  const double ty = num(g(tip, "y"));
  const double dx = tx - num(g(nxt, "x"));
  const double dy = ty - num(g(nxt, "y"));
  const double len = base::js_hypot(dx, dy);
  const double asz = num(st["asz"]);
  if (len > 1e-12 && asz > 0) {
    const double ux = dx / len;
    const double uy = dy / len;
    const double a = asz;
    const double w = a / 6;
    const double bx = tx - ux * a;
    const double by = ty - uy * a;
    base::Array ring = {tx, ty, bx - uy * w, by + ux * w, bx + uy * w, by - ux * w};
    out.push_back(base::make_object({{"layer", "0"},
                                     {"color", 0},
                                     {"type", "hatch"},
                                     {"solid", true},
                                     {"loops", base::array_of(std::move(ring))}}));
  }
  const std::string s = str(detail::nn(g(e, "s"), ""));
  if (!base::js_blank(s)) {
    const double hv = num(g(e, "h"));
    const double h = hv > 0 ? hv : asz;
    base::Value t = base::make_object({{"layer", "0"},
                                       {"color", 0},
                                       {"type", "text"},
                                       {"x", L.text.x},
                                       {"y", L.text.y + h / 2},
                                       {"h", h},
                                       {"rot", 0},
                                       {"s", s},
                                       {"ha", dir == 1 ? "left" : "right"},
                                       {"va", "top"},
                                       {"wf", 1}});
    if (num(g(e, "ww")) > 0) {
      t.set("ww", g(e, "ww"));
    }
    const base::Value ls = g(e, "ls");
    if (ls.truthy() && !(ls == base::Value(1))) {
      t.set("ls", ls);
    }
    out.push_back(std::move(t));
  }
  return out;
}

// ---------------------------------------------------------------- for the PDF writer (plot.js)

double char_width(std::uint32_t cp) {
  return char_advance(cp);
}
std::uint32_t next_char(std::string_view s, std::size_t& i) {
  return next_code_point(s, i);
}
void grow_prim(const Prim& p, Bounds& b) {
  prim_bounds(p, b);
}

}  // namespace cc::cad
