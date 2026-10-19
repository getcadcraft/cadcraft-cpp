// SPDX-License-Identifier: Apache-2.0
// geom.js's revision clouds (CadCraft 0.5) in C++: pathOf (an object's outline as points), resamplePath, revcloud.
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/edit.hpp"
#include "prims.hpp"
#include "util.hpp"

namespace cc::cad {

using detail::g;
using detail::num;
using detail::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;
constexpr double kEps = 1e-9;

double dist(P2 a, P2 b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}

}  // namespace

std::optional<Path> path_of(const Value& e) {
  const std::string t = detail::str(g(e, "type"));
  Path out;
  if (t == "circle") {
    const double cx = num(g(e, "cx"));
    const double cy = num(g(e, "cy"));
    const double r = num(g(e, "r"));
    constexpr int kN = 96;
    for (int i = 0; i < kN; ++i) {
      const double a = (static_cast<double>(i) / kN) * kTau;
      out.pts.push_back({cx + base::js_cos(a) * r, cy + base::js_sin(a) * r});
    }
    out.closed = true;
    return out;
  }
  if (t == "ellipse") {
    out.pts = ellipse_points(e, 144);
    out.closed = num(g(e, "t1")) - num(g(e, "t0")) >= kTau - 1e-9;
    if (out.closed && !out.pts.empty()) {
      out.pts.pop_back();
    }
    return out;
  }
  if (t == "line") {
    out.pts = {{num(g(e, "x1")), num(g(e, "y1"))}, {num(g(e, "x2")), num(g(e, "y2"))}};
    return out;
  }
  if (t == "arc") {
    const Prim p = entity_prims(e, nullptr).at(0);
    const int n = std::max(8, static_cast<int>(std::ceil((p.a1 - p.a0) / (kTau / 96))));
    for (int i = 0; i <= n; ++i) {
      out.pts.push_back(detail::prim_point(p, static_cast<double>(i) / n));
    }
    return out;
  }
  const Value pts = g(e, "pts");
  if (t == "poly" && detail::len(pts) >= 2) {
    std::vector<Prim> ps;
    detail::poly_prims(e, ps);
    for (const Prim& p : ps) {
      if (!p.arc) {
        out.pts.push_back({p.x1, p.y1});
        continue;
      }
      const int n = std::max(4, static_cast<int>(std::ceil((p.a1 - p.a0) / (kTau / 96))));
      for (int i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) / n;
        out.pts.push_back(detail::prim_point(p, p.rev ? 1 - u : u));
      }
    }
    out.closed = g(e, "closed").truthy();
    if (!out.closed) {
      const Value& last = pts.as_array().back();
      out.pts.push_back({num(g(last, "x")), num(g(last, "y"))});
    }
    return out;
  }
  return std::nullopt;
}

std::vector<P2> resample_path(const std::vector<P2>& pts, bool closed, double step) {
  std::vector<P2> ring = pts;
  if (closed && !pts.empty()) {
    ring.push_back(pts[0]);
  }
  std::vector<double> lens;
  double l = 0;
  for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
    const double d = dist(ring[i], ring[i + 1]);
    lens.push_back(d);
    l += d;
  }
  if (!(l > kEps) || !(step > kEps)) {
    return pts;
  }
  const double rn = base::js_round(l / step);
  const double n = std::max(closed ? 3.0 : 1.0, rn);
  std::vector<P2> out;
  std::size_t seg = 0;
  double acc = 0;
  const auto count = static_cast<std::size_t>(n);  // a whole number ≥ 1 (Math.round of a finite length ratio)
  for (std::size_t ki = 0; ki <= count; ++ki) {
    if (closed && ki == count) {
      break;
    }
    const double target = (l * static_cast<double>(ki)) / n;
    while (seg + 1 < lens.size() && acc + lens[seg] < target - 1e-12) {
      acc += lens[seg];
      ++seg;
    }
    const double t = lens[seg] > 0 ? (target - acc) / lens[seg] : 0;
    out.push_back(
        {ring[seg].x + (ring[seg + 1].x - ring[seg].x) * t, ring[seg].y + (ring[seg + 1].y - ring[seg].y) * t});
  }
  return out;
}

Value revcloud(const std::vector<P2>& pts, bool closed, double arc_len, bool reverse) {
  const std::vector<P2> q = resample_path(pts, closed, arc_len);
  double a = 0;
  for (std::size_t i = 0; i < q.size(); ++i) {
    const P2 u = q[i];
    const P2 v = q[(i + 1) % q.size()];
    a += u.x * v.y - v.x * u.y;
  }
  // travelling counter-clockwise the outside is on the right, where a positive bulge goes
  double sign = reverse ? -1 : 1;
  if (closed && !(a >= 0)) {  // (a >= 0 ? 1 : -1): NaN counts as negative
    sign = -sign;
  }
  const double cloud_bulge = std::tan(kTau / 3 / 4);
  base::Array out;
  out.reserve(q.size());
  for (std::size_t i = 0; i < q.size(); ++i) {
    const double b = closed || i + 1 < q.size() ? sign * cloud_bulge : 0;
    out.push_back(base::make_object({{"x", q[i].x}, {"y", q[i].y}, {"b", b}}));
  }
  return base::make_object({{"type", "poly"}, {"closed", closed}, {"pts", std::move(out)}});
}

}  // namespace cc::cad
