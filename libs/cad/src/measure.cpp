// SPDX-License-Identifier: Apache-2.0
// geom.js's area and perimeter (CadCraft 0.5) in C++: polygonMeasure, entityArea (polyMeasure, pointInPoly), with
// the JS's order of operations.
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/js_math.hpp"
#include "cc/cad/edit.hpp"
#include "util.hpp"

namespace cc::cad {

using detail::g;
using detail::num;
using detail::Value;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = kPi * 2;
constexpr double kEps = 1e-9;

double dist(P2 a, P2 b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
P2 xy(const Value& p) {
  return {num(g(p, "x")), num(g(p, "y"))};
}
/// Math.sign
double js_sign(double v) {
  if (std::isnan(v) || v == 0) {
    return v;
  }
  return v > 0 ? 1 : -1;
}

/// geom.js polyMeasure(e): the area a polyline encloses (an open one closed by a straight line) and its length
std::pair<double, double> poly_measure(const Value& e) {
  const Value v = g(e, "pts");
  const std::size_t n = detail::len(v);
  const bool closed = g(e, "closed").truthy();
  double a = 0;
  double len = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Value& u = v.as_array()[i];
    const Value& w = v.as_array()[(i + 1) % n];
    const P2 pu = xy(u);
    const P2 pw = xy(w);
    a += pu.x * pw.y - pw.x * pu.y;
    const bool last = i == n - 1;
    const Value bv = g(u, "b");
    double b = bv.truthy() ? num(bv) : 0;  // u.b || 0, but none on the closing line of an open one
    if (last && !closed) {
      b = 0;
    }
    const double c = dist(pu, pw);
    if (std::abs(b) > 1e-12 && c > kEps) {
      const double th = 4 * base::js_atan(std::abs(b));
      const double r = c / (2 * base::js_sin(th / 2));
      a += js_sign(b) * r * r * (th - base::js_sin(th));  // ×2, like the shoelace terms
      if (!last || closed) {
        len += r * th;
      }
    } else if (!last || closed) {
      len += c;
    }
  }
  return {std::abs(a / 2), len};
}

}  // namespace

bool point_in_poly(P2 q, const std::vector<P2>& pts) {
  bool inside = false;
  for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
    if ((pts[i].y > q.y) != (pts[j].y > q.y) &&
        q.x < (pts[j].x - pts[i].x) * (q.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x) {
      inside = !inside;
    }
  }
  return inside;
}

PolygonMeasure polygon_measure(const std::vector<P2>& pts) {
  double a = 0;
  double p = 0;
  for (std::size_t i = 0, n = pts.size(); i < n; ++i) {
    const P2 u = pts[i];
    const P2 v = pts[(i + 1) % n];
    a += u.x * v.y - v.x * u.y;
    p += dist(u, v);
  }
  return {.area = a / 2, .perimeter = pts.size() >= 2 ? p : 0};
}

std::optional<EntityArea> entity_area(const Value& e) {
  const std::string t = detail::str(g(e, "type"));
  if (t == "circle") {
    const double r = num(g(e, "r"));
    return EntityArea{.area = kPi * r * r, .perimeter = kTau * r, .kind = "circle"};
  }
  if (t == "ellipse") {
    const double a = base::js_hypot(num(g(e, "mx")), num(g(e, "my")));
    const double b = a * num(g(e, "ratio"));
    const double t0 = num(g(e, "t0"));
    const double t1 = num(g(e, "t1"));
    if (t1 - t0 >= kTau - kEps || std::abs(detail::norm_angle(t1 - t0)) < kEps) {
      const double h = std::pow((a - b) / (a + b), 2);
      return EntityArea{.area = kPi * a * b,
                        .perimeter = kPi * (a + b) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h))),
                        .kind = "closed"};
    }
    const std::vector<P2> pts = ellipse_points(e, 720);
    const PolygonMeasure m = polygon_measure(pts);
    double len = 0;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
      len += dist(pts[i], pts[i + 1]);
    }
    return EntityArea{.area = std::abs(m.area), .perimeter = len, .kind = "open"};
  }
  if (t == "poly") {
    if (detail::len(g(e, "pts")) < 2) {
      return std::nullopt;
    }
    const auto [area, length] = poly_measure(e);
    return EntityArea{.area = area, .perimeter = length, .kind = g(e, "closed").truthy() ? "closed" : "open"};
  }
  if (t == "hatch") {
    // even-odd: a loop inside an odd number of others is a hole
    std::vector<std::vector<P2>> rings;
    const Value loops = g(e, "loops");
    for (const Value& l : loops.is_array() ? loops.as_array() : base::Array{}) {
      std::vector<P2> pts;
      const base::Array& f = l.is_array() ? l.as_array() : base::Array{};
      for (std::size_t i = 0; i + 1 < f.size(); i += 2) {
        pts.push_back({num(f[i]), num(f[i + 1])});
      }
      if (pts.size() >= 3) {
        rings.push_back(std::move(pts));
      }
    }
    if (rings.empty()) {
      return std::nullopt;
    }
    double area = 0;
    double perimeter = 0;
    for (std::size_t i = 0; i < rings.size(); ++i) {
      const PolygonMeasure m = polygon_measure(rings[i]);
      std::size_t depth = 0;
      for (std::size_t j = 0; j < rings.size(); ++j) {
        if (j != i && point_in_poly(rings[i][0], rings[j])) {
          ++depth;
        }
      }
      area += (depth % 2 != 0 ? -1 : 1) * std::abs(m.area);
      perimeter += m.perimeter;
    }
    return EntityArea{.area = std::abs(area), .perimeter = perimeter, .kind = "closed"};
  }
  return std::nullopt;
}

}  // namespace cc::cad
