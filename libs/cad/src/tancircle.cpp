// SPDX-License-Identifier: Apache-2.0
// tancircle.js (CadCraft 0.5 DR.1 / DR.2) in C++: circles tangent to picked objects — CIRCLE TTR and Tan Tan Tan.
#include "cc/cad/tancircle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "cc/base/js_math.hpp"
#include "prims.hpp"

namespace cc::cad {

namespace {

constexpr double kEps = 1e-9;
constexpr double kFar = 1e9;

double dist(P2 a, P2 b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}

/// Is circle c tangent to o (within a tolerance)?
bool tangent(const Curve& o, const Circle& c, double tol) {
  if (o.line) {
    return std::abs(std::abs((c.cx - o.x) * o.dy - (c.cy - o.y) * o.dx) - c.r) <= tol;
  }
  const double d = base::js_hypot(c.cx - o.cx, c.cy - o.cy);
  return std::abs(d - (o.r + c.r)) <= tol || std::abs(d - std::abs(o.r - c.r)) <= tol;
}

// ---- offsets and their crossings (TTR)
std::vector<Curve> offsets(const Curve& o, double r) {
  std::vector<Curve> out;
  if (o.line) {
    const double nx = -o.dy;
    const double ny = o.dx;
    for (const double s : {1.0, -1.0}) {
      out.push_back({.line = true, .x = o.x + nx * r * s, .y = o.y + ny * r * s, .dx = o.dx, .dy = o.dy});
    }
    return out;
  }
  for (const double big_r : {o.r + r, std::abs(o.r - r)}) {
    if (big_r > kEps) {
      out.push_back({.line = false, .cx = o.cx, .cy = o.cy, .r = big_r});
    }
  }
  return out;
}
// NOLINTNEXTLINE(misc-no-recursion) — a circle × a line turns round once
std::vector<P2> cross(const Curve& a, const Curve& b) {
  if (a.line && b.line) {
    const double den = a.dx * b.dy - a.dy * b.dx;
    if (std::abs(den) < 1e-12) {
      return {};
    }
    const double t = ((b.x - a.x) * b.dy - (b.y - a.y) * b.dx) / den;
    return {{a.x + a.dx * t, a.y + a.dy * t}};
  }
  if (!a.line && b.line) {
    return cross(b, a);
  }
  if (a.line) {  // line × circle
    const double fx = a.x - b.cx;
    const double fy = a.y - b.cy;
    const double bb = fx * a.dx + fy * a.dy;
    const double cc = fx * fx + fy * fy - b.r * b.r;
    const double d = bb * bb - cc;
    if (d < -1e-12 * std::max(1.0, b.r * b.r)) {
      return {};
    }
    const double s = std::sqrt(std::max(0.0, d));
    std::vector<P2> out;
    if (s < 1e-12) {
      out.push_back({a.x + a.dx * -bb, a.y + a.dy * -bb});
    } else {
      for (const double t : {-bb + s, -bb - s}) {
        out.push_back({a.x + a.dx * t, a.y + a.dy * t});
      }
    }
    return out;
  }
  const double dx = b.cx - a.cx;
  const double dy = b.cy - a.cy;
  const double d = base::js_hypot(dx, dy);
  if (d < kEps || d > a.r + b.r + 1e-9 * (a.r + b.r) || d < std::abs(a.r - b.r) - 1e-9 * (a.r + b.r)) {
    return {};
  }
  const double l = (a.r * a.r - b.r * b.r + d * d) / (2 * d);
  const double h = std::sqrt(std::max(0.0, a.r * a.r - l * l));
  const double mx = a.cx + dx * l / d;
  const double my = a.cy + dy * l / d;
  if (h < 1e-12) {
    return {{mx, my}};
  }
  return {{mx - dy * h / d, my + dx * h / d}, {mx + dy * h / d, my - dx * h / d}};
}
double scale_of(std::initializer_list<Curve> os) {
  double s = 1;
  for (const Curve& o : os) {
    s = std::max(s, o.line ? base::js_hypot(o.x, o.y) : base::js_hypot(o.cx, o.cy) + o.r);
  }
  return s;
}

std::optional<Circle> nearest_to_picks(const std::vector<Circle>& cands,
                                       std::initializer_list<std::pair<const Curve*, P2>> list) {
  std::optional<Circle> best;
  double bd = std::numeric_limits<double>::infinity();
  for (const Circle& c : cands) {
    double sc = 0;
    for (const auto& [o, p] : list) {
      sc += dist(touch_point(*o, c), p);
    }
    if (sc < bd - 1e-12) {
      bd = sc;
      best = c;
    }
  }
  return best;
}

// ---- three tangents (TTT): each object's equation with a side sign, solved by Newton from several starts
double residual(const Curve& o, double s, double x, double y, double r) {
  if (o.line) {
    return (x - o.x) * o.dy - (y - o.y) * o.dx - s * r;
  }
  return base::js_hypot(x - o.cx, y - o.cy) - (o.r + s * r);
}
double residual2(const Curve& o, double s, double x, double y, double r) {
  return !o.line && s == -1 ? base::js_hypot(x - o.cx, y - o.cy) - std::abs(o.r - r) : residual(o, s, x, y, r);
}
using M3 = std::array<std::array<double, 3>, 3>;
double det(const M3& m) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
std::optional<Circle> solve3(const std::array<const Curve*, 3>& os, const std::array<double, 3>& ss, double x, double y,
                             double r) {
  for (int it = 0; it < 60; ++it) {
    std::array<double, 3> f{};
    for (std::size_t i = 0; i < 3; ++i) {
      f.at(i) = residual2(*os.at(i), ss.at(i), x, y, r);
    }
    const double big = std::max({1.0, std::abs(x), std::abs(y), r});
    if (std::ranges::all_of(f, [big](double v) { return std::abs(v) < 1e-13 * big; })) {
      break;
    }
    const double h = 1e-7 * std::max({1.0, std::abs(x), std::abs(y), std::abs(r)});
    M3 j{};
    for (std::size_t i = 0; i < 3; ++i) {
      j.at(i) = {(residual2(*os.at(i), ss.at(i), x + h, y, r) - f.at(i)) / h,
                 (residual2(*os.at(i), ss.at(i), x, y + h, r) - f.at(i)) / h,
                 (residual2(*os.at(i), ss.at(i), x, y, r + h) - f.at(i)) / h};
    }
    const double d = det(j);
    if (!std::isfinite(d) || std::abs(d) < 1e-18) {
      return std::nullopt;
    }
    const auto col = [&](std::size_t k) {
      M3 m = j;
      for (std::size_t i = 0; i < 3; ++i) {
        m.at(i).at(k) = -f.at(i);
      }
      return m;
    };
    double dx = det(col(0)) / d;
    double dy = det(col(1)) / d;
    double dr = det(col(2)) / d;
    const double step = base::js_hypot(dx, dy, dr);
    const double lim = std::max(1.0, std::abs(r)) * 2;
    if (step > lim) {
      dx *= lim / step;
      dy *= lim / step;
      dr *= lim / step;
    }
    x += dx;
    y += dy;
    r += dr;
    if (!std::isfinite(x + y + r)) {
      return std::nullopt;
    }
  }
  if (r > kEps) {
    return Circle{.cx = x, .cy = y, .r = r};
  }
  return std::nullopt;
}

}  // namespace

std::optional<Curve> curve_at(const base::Value& e, P2 pick, const BlockLookup& blocks) {
  std::optional<Prim> best;
  double bd = std::numeric_limits<double>::infinity();
  for (const Prim& q : entity_prims(e, blocks)) {
    if (q.box) {
      continue;
    }
    const double d = dist(detail::prim_point(q, prim_param(q, pick)), pick);
    if (d < bd) {
      bd = d;
      best = q;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  if (!best->arc) {
    const double l = base::js_hypot(best->x2 - best->x1, best->y2 - best->y1);
    if (l > kEps) {
      return Curve{
          .line = true, .x = best->x1, .y = best->y1, .dx = (best->x2 - best->x1) / l, .dy = (best->y2 - best->y1) / l};
    }
    return std::nullopt;
  }
  if (best->r > kEps) {
    return Curve{.line = false, .cx = best->cx, .cy = best->cy, .r = best->r};
  }
  return std::nullopt;
}

P2 touch_point(const Curve& o, const Circle& c) {
  if (o.line) {
    const double t = (c.cx - o.x) * o.dx + (c.cy - o.y) * o.dy;
    return {o.x + o.dx * t, o.y + o.dy * t};
  }
  const double d = base::js_hypot(c.cx - o.cx, c.cy - o.cy);
  if (d < kEps) {
    return {o.cx + o.r, o.cy};
  }
  const double k = (c.cx - o.cx) / d;
  const double m = (c.cy - o.cy) / d;
  // the touch point lies on o, on the line through both centres, on the side of c (outside) or away (o inside c)
  const double s = d > o.r || std::abs(d - (o.r - c.r)) < std::abs(d - (o.r + c.r)) ? 1 : -1;
  return {o.cx + k * o.r * s, o.cy + m * o.r * s};
}

std::optional<Circle> circle_ttr(const Curve& a, P2 pa, const Curve& b, P2 pb, double r) {
  if (!(r > kEps)) {
    return std::nullopt;
  }
  const double tol = 1e-7 * scale_of({a, b, Curve{.line = false, .cx = 0, .cy = 0, .r = r}});
  std::vector<Circle> cands;
  for (const Curve& oa : offsets(a, r)) {
    for (const Curve& ob : offsets(b, r)) {
      for (const P2 q : cross(oa, ob)) {
        const Circle c{.cx = q.x, .cy = q.y, .r = r};
        if (std::abs(q.x) < kFar && std::abs(q.y) < kFar && tangent(a, c, tol) && tangent(b, c, tol)) {
          cands.push_back(c);
        }
      }
    }
  }
  return nearest_to_picks(cands, {{&a, pa}, {&b, pb}});
}

std::optional<Circle> circle_ttt(const Curve& a, P2 pa, const Curve& b, P2 pb, const Curve& c, P2 pc) {
  const std::array<const Curve*, 3> os{&a, &b, &c};
  const double tol = 1e-7 * scale_of({a, b, c});
  const P2 m{(pa.x + pb.x + pc.x) / 3, (pa.y + pb.y + pc.y) / 3};
  const double spread = std::max({1e-6, dist(pa, m), dist(pb, m), dist(pc, m)});
  std::vector<std::array<double, 3>> seeds;
  for (const double k : {0.5, 1.0, 2.0}) {
    for (int i = 0; i < 6; ++i) {
      const double t = i * std::numbers::pi / 3;
      seeds.push_back(
          {m.x + base::js_cos(t) * spread * (k - 0.5), m.y + base::js_sin(t) * spread * (k - 0.5), spread * k * 0.6});
    }
  }
  std::vector<Circle> found;
  for (const double s0 : {1.0, -1.0}) {
    for (const double s1 : {1.0, -1.0}) {
      for (const double s2 : {1.0, -1.0}) {
        for (const auto& [x, y, r] : seeds) {
          const auto sol = solve3(os, {s0, s1, s2}, x, y, r);
          if (!sol || !tangent(a, *sol, tol * 10) || !tangent(b, *sol, tol * 10) || !tangent(c, *sol, tol * 10)) {
            continue;
          }
          const bool known = std::ranges::any_of(found, [&](const Circle& f) {
            return std::abs(f.cx - sol->cx) + std::abs(f.cy - sol->cy) + std::abs(f.r - sol->r) < tol * 100;
          });
          if (!known) {
            found.push_back(*sol);
          }
        }
      }
    }
  }
  return nearest_to_picks(found, {{&a, pa}, {&b, pb}, {&c, pc}});
}

}  // namespace cc::cad
