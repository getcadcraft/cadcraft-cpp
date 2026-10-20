// SPDX-License-Identifier: Apache-2.0
// xclip.js (CadCraft 0.5 WL.16) in C++: what a clipped reference keeps — lines and arcs cut at the boundary, fills
// cut to it, texts and points kept when their insertion point is inside.
#include "cc/docview/xclip.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::docview {

namespace {

constexpr double kEps = 1e-12;
constexpr double kTau = 2 * std::numbers::pi;

using cad::P2;
using cad::Prim;

/// where segment a→b crosses the polygon's edges: parameters t in (0, 1)
std::vector<double> seg_crossings(P2 a, P2 b, const std::vector<P2>& poly) {
  std::vector<double> out;
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const P2 p = poly[j];
    const P2 q = poly[i];
    const double ex = q.x - p.x;
    const double ey = q.y - p.y;
    const double den = dx * ey - dy * ex;
    if (std::abs(den) < kEps) {
      continue;
    }
    const double tt = ((p.x - a.x) * ey - (p.y - a.y) * ex) / den;
    const double u = ((p.x - a.x) * dy - (p.y - a.y) * dx) / den;
    if (tt > kEps && tt < 1 - kEps && u >= -kEps && u <= 1 + kEps) {
      out.push_back(tt);
    }
  }
  return out;
}

bool convex(const std::vector<P2>& poly) {
  double sign = 0;
  const std::size_t n = poly.size();
  for (std::size_t i = 0; i < n; ++i) {
    const P2 a = poly[i];
    const P2 b = poly[(i + 1) % n];
    const P2 c = poly[(i + 2) % n];
    const double z = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    if (std::abs(z) < kEps) {
      continue;
    }
    const double s = z > 0 ? 1 : -1;
    if (sign != 0 && s != sign) {
      return false;
    }
    sign = s;
  }
  return true;
}

}  // namespace

bool inside_poly(P2 p, const std::vector<P2>& poly) {
  bool c = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const P2 a = poly[i];
    const P2 b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < ((b.x - a.x) * (p.y - a.y)) / (b.y - a.y) + a.x) {
      c = !c;
    }
  }
  return c;
}

std::vector<Prim> clip_line(const Prim& q, const std::vector<P2>& poly) {
  const P2 a{q.x1, q.y1};
  const P2 b{q.x2, q.y2};
  std::vector<double> ts{0};
  std::vector<double> cr = seg_crossings(a, b, poly);
  std::ranges::sort(cr);
  ts.insert(ts.end(), cr.begin(), cr.end());
  ts.push_back(1);
  struct Run {
    double t0, t1;
  };
  std::vector<Run> runs;
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    const double t0 = ts[i];
    const double t1 = ts[i + 1];
    if (t1 - t0 < kEps) {
      continue;
    }
    const double m = (t0 + t1) / 2;
    if (!inside_poly({a.x + (b.x - a.x) * m, a.y + (b.y - a.y) * m}, poly)) {
      continue;
    }
    if (!runs.empty() && std::abs(runs.back().t1 - t0) < kEps) {
      runs.back().t1 = t1;
    } else {
      runs.push_back({t0, t1});
    }
  }
  std::vector<Prim> out;
  out.reserve(runs.size());
  for (const auto& [t0, t1] : runs) {
    Prim c = q;
    c.x1 = a.x + (b.x - a.x) * t0;
    c.y1 = a.y + (b.y - a.y) * t0;
    c.x2 = a.x + (b.x - a.x) * t1;
    c.y2 = a.y + (b.y - a.y) * t1;
    out.push_back(c);
  }
  return out;
}

std::vector<Prim> clip_arc(const Prim& q, const std::vector<P2>& poly) {
  double sweep = q.a1 - q.a0;
  while (sweep <= 0) {
    sweep += kTau;
  }
  std::vector<double> ang;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const P2 p = poly[j];
    const P2 r = poly[i];
    const double dx = r.x - p.x;
    const double dy = r.y - p.y;
    const double fx = p.x - q.cx;
    const double fy = p.y - q.cy;
    const double aa = dx * dx + dy * dy;
    const double bb = 2 * (fx * dx + fy * dy);
    const double cc = fx * fx + fy * fy - q.r * q.r;
    const double d = bb * bb - 4 * aa * cc;
    if (aa < kEps || d < 0) {
      continue;
    }
    for (const double s : {(-bb - std::sqrt(d)) / (2 * aa), (-bb + std::sqrt(d)) / (2 * aa)}) {
      if (s < -kEps || s > 1 + kEps) {
        continue;
      }
      double a = base::js_atan2(p.y + dy * s - q.cy, p.x + dx * s - q.cx) - q.a0;
      while (a < 0) {
        a += kTau;
      }
      while (a >= kTau) {
        a -= kTau;
      }
      if (a > kEps && a < sweep - kEps) {
        ang.push_back(a);
      }
    }
  }
  std::ranges::sort(ang);
  std::vector<double> ts{0};
  ts.insert(ts.end(), ang.begin(), ang.end());
  ts.push_back(sweep);
  struct Run {
    double s0, s1;
  };
  std::vector<Run> runs;
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    const double s0 = ts[i];
    const double s1 = ts[i + 1];
    if (s1 - s0 < 1e-9) {
      continue;
    }
    const double m = q.a0 + (s0 + s1) / 2;
    if (!inside_poly({q.cx + q.r * base::js_cos(m), q.cy + q.r * base::js_sin(m)}, poly)) {
      continue;
    }
    if (!runs.empty() && std::abs(runs.back().s1 - s0) < 1e-9) {
      runs.back().s1 = s1;
    } else {
      runs.push_back({s0, s1});
    }
  }
  std::vector<Prim> out;
  out.reserve(runs.size());
  for (const auto& [s0, s1] : runs) {
    Prim c = q;  // the JS spreads the primitive: `full` and `rev` come along
    c.a0 = q.a0 + s0;
    c.a1 = q.a0 + s1;
    out.push_back(c);
  }
  return out;
}

std::optional<std::vector<double>> clip_ring(const std::vector<double>& l, const std::vector<P2>& poly) {
  std::vector<P2> pts;
  for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
    pts.push_back({l[i], l[i + 1]});
  }
  if (pts.size() > 1 && std::abs(pts.front().x - pts.back().x) < kEps &&
      std::abs(pts.front().y - pts.back().y) < kEps) {
    pts.pop_back();
  }
  const auto ins = static_cast<std::size_t>(std::ranges::count_if(pts, [&poly](P2 p) { return inside_poly(p, poly); }));
  if (ins == pts.size() && std::ranges::none_of(poly, [&pts](P2 p) { return inside_poly(p, pts); })) {
    return l;
  }
  if (!convex(poly)) {
    return ins > 0 ? std::optional(l) : std::nullopt;
  }
  double ccw = 0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const P2 a = poly[i];
    const P2 b = poly[(i + 1) % poly.size()];
    ccw += a.x * b.y - b.x * a.y;
  }
  const auto side = [ccw](P2 p, P2 a, P2 b) {
    return ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * (ccw >= 0 ? 1 : -1) >= -kEps;
  };
  for (std::size_t i = 0; i < poly.size() && !pts.empty(); ++i) {
    const P2 a = poly[i];
    const P2 b = poly[(i + 1) % poly.size()];
    const std::vector<P2> inp = std::move(pts);
    pts.clear();
    for (std::size_t k = 0; k < inp.size(); ++k) {
      const P2 p = inp[k];
      const P2 q = inp[(k + 1) % inp.size()];
      const bool pi = side(p, a, b);
      const bool qi = side(q, a, b);
      if (pi) {
        pts.push_back(p);
      }
      if (pi != qi) {
        const double dx = q.x - p.x;
        const double dy = q.y - p.y;
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double den = dx * ey - dy * ex;
        if (std::abs(den) > kEps) {
          const double tt = ((a.x - p.x) * ey - (a.y - p.y) * ex) / den;
          pts.push_back({p.x + dx * tt, p.y + dy * tt});
        }
      }
    }
  }
  if (pts.size() < 3) {
    return std::nullopt;
  }
  std::vector<double> out;
  out.reserve(pts.size() * 2 + 2);
  for (const P2 p : pts) {
    out.push_back(p.x);
    out.push_back(p.y);
  }
  out.push_back(pts.front().x);
  out.push_back(pts.front().y);
  return out;
}

std::optional<std::vector<P2>> clip_poly(const base::Value& ref) {
  const base::Value clip = ref.get("clip");
  const base::Value pts = clip.is_object() ? clip.get("pts") : base::Value();
  std::vector<P2> out;
  if (pts.is_array()) {
    for (const base::Value& p : pts.as_array()) {
      const base::Value x = p.get("x");
      const base::Value y = p.get("y");
      if (x.is_number() && std::isfinite(x.as_number()) && y.is_number() && std::isfinite(y.as_number())) {
        out.push_back({x.as_number(), y.as_number()});
      }
    }
  }
  if (out.size() >= 3) {
    return out;
  }
  return std::nullopt;
}

}  // namespace cc::docview
