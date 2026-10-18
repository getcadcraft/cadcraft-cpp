// SPDX-License-Identifier: Apache-2.0
// geom.js's editing geometry (CadCraft 0.5) in C++: intersections of primitives, OFFSET (offsetEntity), TRIM in pieces
// (trimSplit, pieceAt, pieceEntity, trimResult, pathCrossings) and EXTEND (extendEntity, extensionPart), with the JS's
// order of operations and tolerances.
#include "cc/cad/edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js_math.hpp"
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
bool unbounded(const std::string& t) {
  return t == "xline" || t == "ray";
}
std::string type_of(const Value& e) {
  return detail::str(g(e, "type"));
}
P2 xy(const Value& p) {
  return {num(g(p, "x")), num(g(p, "y"))};
}

// ---- intersections (geom.js llInter, lcInter, ccInter, onPrim)

double prim_length(const Prim& p) {
  return !p.arc ? base::js_hypot(p.x2 - p.x1, p.y2 - p.y1) : p.r * (p.a1 - p.a0);
}

std::vector<P2> ll_inter(const Prim& a, const Prim& b) {
  const double d1x = a.x2 - a.x1;
  const double d1y = a.y2 - a.y1;
  const double d2x = b.x2 - b.x1;
  const double d2y = b.y2 - b.y1;
  const double den = d1x * d2y - d1y * d2x;
  if (std::abs(den) < 1e-12 * base::js_hypot(d1x, d1y) * base::js_hypot(d2x, d2y)) {
    return {};
  }
  const double t = ((b.x1 - a.x1) * d2y - (b.y1 - a.y1) * d2x) / den;
  return {{a.x1 + d1x * t, a.y1 + d1y * t}};
}

std::vector<P2> lc_inter(const Prim& l, const Prim& c) {
  const double dx = l.x2 - l.x1;
  const double dy = l.y2 - l.y1;
  const double fx = l.x1 - c.cx;
  const double fy = l.y1 - c.cy;
  const double A = dx * dx + dy * dy;
  const double B = 2 * (fx * dx + fy * dy);
  const double C = fx * fx + fy * fy - c.r * c.r;
  if (A < kEps * kEps) {
    return {};
  }
  double disc = B * B - 4 * A * C;
  if (disc < 0) {
    if (disc > -1e-9 * B * B) {
      disc = 0;
    } else {
      return {};
    }
  }
  const double s = std::sqrt(disc);
  std::vector<P2> out;
  if (disc == 0) {
    const double t = -B / (2 * A);
    out.push_back({l.x1 + dx * t, l.y1 + dy * t});
  } else {
    for (const double t : {(-B - s) / (2 * A), (-B + s) / (2 * A)}) {
      out.push_back({l.x1 + dx * t, l.y1 + dy * t});
    }
  }
  return out;
}

std::vector<P2> cc_inter(const Prim& a, const Prim& b) {
  const double d = base::js_hypot(b.cx - a.cx, b.cy - a.cy);
  if (d < kEps || d > a.r + b.r + 1e-9 || d < std::abs(a.r - b.r) - 1e-9) {
    return {};
  }
  const double x = (d * d + a.r * a.r - b.r * b.r) / (2 * d);
  const double h = std::sqrt(std::max(0.0, a.r * a.r - x * x));
  const double ux = (b.cx - a.cx) / d;
  const double uy = (b.cy - a.cy) / d;
  const double px = a.cx + ux * x;
  const double py = a.cy + uy * x;
  if (h < 1e-12) {
    return {{px, py}};
  }
  return {{px - uy * h, py + ux * h}, {px + uy * h, py - ux * h}};
}

bool on_prim(const Prim& p, P2 x, double tol) {
  if (!p.arc) {
    const double t = prim_param(p, x, false);
    const double L = prim_length(p);
    return t >= -tol / std::max(L, kEps) - 1e-9 && t <= 1 + tol / std::max(L, kEps) + 1e-9;
  }
  return detail::on_sweep(base::js_atan2(x.y - p.cy, x.x - p.cx), p.a0, p.a1, 1e-7);
}

Prim seg(P2 a, P2 b) {
  return detail::line_prim(a.x, a.y, b.x, b.y);
}

std::vector<Prim> prims_of(const Value& e, const BlockLookup& blocks) {
  std::vector<Prim> out;
  detail::prims(e, blocks, 0, out);
  return out;
}
std::vector<Prim> own_prims(const Value& e, const BlockLookup& blocks) {
  std::vector<Prim> out;
  if (type_of(e) == "poly") {
    detail::poly_prims(e, out);
  } else {
    detail::prims(e, blocks, 0, out);
  }
  return out;
}

/// geom.js entityParams(e, cutters, blocks, edgeExtend): the cut parameters along e (poly: segment index + local t)
std::vector<double> entity_params(const Value& e, const std::vector<Prim>& own,
                                  const std::vector<const Value*>& cutters, const BlockLookup& blocks,
                                  bool edge_extend) {
  const bool poly = type_of(e) == "poly";
  std::vector<double> ts;
  for (std::size_t i = 0; i < own.size(); ++i) {
    const Prim& p = own[i];
    // EDGEMODE 1: a cutting line or arc counts as the whole line or circle it lies on
    for (const Value* c : cutters) {
      const std::string ct = type_of(*c);
      const bool ext = edge_extend && (ct == "line" || ct == "arc");
      for (const Prim& q : prims_of(*c, blocks)) {
        for (const P2 x : intersect(p, q, false, ext)) {
          double t = prim_param(p, x, true);
          if (p.rev) {
            t = 1 - t;
          }
          ts.push_back((poly ? static_cast<double>(i) : 0) + t);
        }
      }
    }
  }
  if (unbounded(type_of(e))) {  // a stand-in segment 2·FAR long: keep it exact
    std::ranges::sort(ts);
    std::vector<double> u;
    for (std::size_t i = 0; i < ts.size(); ++i) {
      if (i == 0 || ts[i] - ts[i - 1] > 1e-15) {
        u.push_back(ts[i]);
      }
    }
    return u;
  }
  std::set<double> seen;
  std::vector<double> u;
  for (const double t : ts) {
    const double r = base::js_round(t * 1e9) / 1e9;
    if (seen.insert(r).second) {
      u.push_back(r);
    }
  }
  std::ranges::sort(u);
  return u;
}

/// geom.js pickParam(e, own, q)
double pick_param(const Value& e, const std::vector<Prim>& own, P2 q) {
  const bool poly = type_of(e) == "poly";
  double best = 0;
  double bd = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < own.size(); ++i) {
    const double d = dist_to_prim(own[i], q);
    if (d < bd) {
      bd = d;
      double t = prim_param(own[i], q, true);
      if (own[i].rev) {
        t = 1 - t;
      }
      best = (poly ? static_cast<double>(i) : 0) + t;
    }
  }
  return best;
}

/// { ...v } of a poly vertex list: fresh copies
base::Array copy_pts(const Value& pts) {
  return pts.is_array() ? pts.as_array() : base::Array{};
}

}  // namespace

// ---------------------------------------------------------------- primitives

double prim_param(const Prim& p, P2 q, bool clamp) {
  if (!p.arc) {
    const double dx = p.x2 - p.x1;
    const double dy = p.y2 - p.y1;
    const double L = dx * dx + dy * dy;
    const double t = L < kEps * kEps ? 0 : ((q.x - p.x1) * dx + (q.y - p.y1) * dy) / L;
    return clamp ? std::max(0.0, std::min(1.0, t)) : t;
  }
  const double a = base::js_atan2(q.y - p.cy, q.x - p.cx);
  const double sweep = p.a1 - p.a0;
  const double d = detail::norm_angle(a - p.a0);
  if (d > sweep + kEps) {  // outside the sweep: nearest end (or extrapolate)
    if (!clamp) {
      return d / sweep;
    }
    const double after = d - sweep;
    const double before = kTau - d;
    return after < before ? 1 : 0;
  }
  return d / sweep;
}

double dist_to_prim(const Prim& p, P2 q) {
  return dist(q, detail::prim_point(p, prim_param(p, q, true)));
}

std::vector<P2> intersect(const Prim& p, const Prim& q, bool ext1, bool ext2) {
  std::vector<P2> pts;
  if (!p.arc && !q.arc) {
    pts = ll_inter(p, q);
  } else if (!p.arc) {
    pts = lc_inter(p, q);
  } else if (!q.arc) {
    pts = lc_inter(q, p);
  } else {
    pts = cc_inter(p, q);
  }
  constexpr double kTol = 1e-7;
  std::vector<P2> out;
  for (const P2 x : pts) {
    if ((ext1 || on_prim(p, x, kTol)) && (ext2 || on_prim(q, x, kTol))) {
      out.push_back(x);
    }
  }
  return out;
}

std::vector<Prim> entity_prims(const Value& e, const BlockLookup& blocks) {
  return prims_of(e, blocks);
}

// ---------------------------------------------------------------- OFFSET

namespace {

std::optional<Value> offset_poly(const Value& e, double d, P2 side) {
  // Signed offset: decide the side from the nearest segment, then offset every segment the same way.
  std::vector<Prim> ps;
  detail::poly_prims(e, ps);
  if (ps.empty()) {
    return std::nullopt;
  }
  const Prim* near = ps.data();
  double nd = std::numeric_limits<double>::infinity();
  for (const Prim& p : ps) {
    const double x = dist_to_prim(p, side);
    if (x < nd) {
      nd = x;
      near = &p;
    }
  }
  const auto left_of = [](const Prim& p, P2 q) {
    if (!p.arc) {
      return (p.x2 - p.x1) * (q.y - p.y1) - (p.y2 - p.y1) * (q.x - p.x1) > 0;
    }
    const bool outside = dist({p.cx, p.cy}, q) > p.r;
    return p.rev ? outside : !outside;  // CCW arc: left = inside
  };
  const double sgn = left_of(*near, side) ? 1 : -1;  // +1 = offset to the left of travel
  std::vector<Prim> off;
  off.reserve(ps.size());
  for (const Prim& p : ps) {
    if (!p.arc) {
      double L = base::js_hypot(p.x2 - p.x1, p.y2 - p.y1);
      if (L == 0 || std::isnan(L)) {
        L = 1;  // `|| 1`
      }
      const double nx = -(p.y2 - p.y1) / L * d * sgn;
      const double ny = (p.x2 - p.x1) / L * d * sgn;
      Prim q = p;
      q.x1 = p.x1 + nx;
      q.y1 = p.y1 + ny;
      q.x2 = p.x2 + nx;
      q.y2 = p.y2 + ny;
      off.push_back(q);
      continue;
    }
    const bool to_left_inside = !p.rev;  // travelling CCW, the left side is the centre
    const double r = p.r + ((sgn > 0) == to_left_inside ? -d : d);
    if (!(r > kEps)) {
      return std::nullopt;
    }
    Prim q = p;
    q.r = r;
    off.push_back(q);
  }
  const auto start = [](const Prim& p) { return !p.arc ? P2{p.x1, p.y1} : detail::prim_point(p, p.rev ? 1 : 0); };
  const auto end = [](const Prim& p) { return !p.arc ? P2{p.x2, p.y2} : detail::prim_point(p, p.rev ? 0 : 1); };
  const std::size_t n = off.size();
  const bool closed = g(e, "closed").truthy();
  const Value src = g(e, "pts");
  base::Array pts;
  for (std::size_t i = 0; i < n; ++i) {
    const Prim& p = off[i];
    P2 s = start(p);
    if (i > 0 || closed) {
      const Prim& prev = off[(i + n - 1) % n];
      if (!prev.arc && !p.arc) {
        const auto x = intersect(prev, p, true, true);
        if (!x.empty()) {
          s = x[0];
        }
      }
    }
    const Value b = src.is_array() && i < src.as_array().size() ? g(src.as_array()[i], "b") : Value();
    pts.push_back(base::make_object({{"x", s.x}, {"y", s.y}, {"b", b.truthy() ? b : Value(0)}}));
  }
  if (!closed) {
    const P2 last = end(off[n - 1]);
    pts.push_back(base::make_object({{"x", last.x}, {"y", last.y}, {"b", 0}}));
  }
  Value out = e;
  out.set("pts", std::move(pts));
  return out;
}

}  // namespace

std::optional<Value> offset_entity(const Value& e, double d, P2 side) {
  const std::string t = type_of(e);
  if (t == "line") {
    const double x1 = num(g(e, "x1"));
    const double y1 = num(g(e, "y1"));
    const double x2 = num(g(e, "x2"));
    const double y2 = num(g(e, "y2"));
    const double L = base::js_hypot(x2 - x1, y2 - y1);
    if (L < kEps) {
      return std::nullopt;
    }
    double nx = -(y2 - y1) / L;
    double ny = (x2 - x1) / L;
    if ((side.x - x1) * nx + (side.y - y1) * ny < 0) {
      nx = -nx;
      ny = -ny;
    }
    Value out = e;
    out.set("x1", x1 + nx * d);
    out.set("y1", y1 + ny * d);
    out.set("x2", x2 + nx * d);
    out.set("y2", y2 + ny * d);
    return out;
  }
  if (t == "circle" || t == "arc") {
    const double r0 = num(g(e, "r"));
    const bool out_side = dist({num(g(e, "cx")), num(g(e, "cy"))}, side) > r0;
    const double r = r0 + (out_side ? d : -d);
    if (!(r > kEps)) {
      return std::nullopt;
    }
    Value out = e;
    out.set("r", r);
    return out;
  }
  if (t == "poly") {
    return offset_poly(e, d, side);
  }
  if (t == "xline" || t == "ray") {
    double nx = -num(g(e, "dy"));
    double ny = num(g(e, "dx"));
    const double x = num(g(e, "x"));
    const double y = num(g(e, "y"));
    if ((side.x - x) * nx + (side.y - y) * ny < 0) {
      nx = -nx;
      ny = -ny;
    }
    Value out = e;
    out.set("x", x + nx * d);
    out.set("y", y + ny * d);
    return out;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- TRIM

Value sub_entity(const Value& e, double t0, double t1) {
  const std::string t = type_of(e);
  if (t1 - t0 < (unbounded(t) ? 1e-15 : 1e-9)) {
    return {nullptr};
  }
  if (unbounded(t)) {
    // along prims' stand-in segment: an end at 0 (xline) or 1 is still infinite; a cut end is a real point
    const std::vector<Prim> ps = prims_of(e, nullptr);
    if (ps.empty()) {
      return {nullptr};
    }
    const Prim p = ps[0];
    const auto at = [&p](double u) { return detail::prim_point(p, u); };
    const bool open0 = t == "xline" && t0 <= 1e-12;
    const bool open1 = t1 >= 1 - 1e-12;
    Value out = e;
    if (open0 && open1) {
      return out;
    }
    if (open1) {
      const P2 a = at(t0);
      out.set("type", "ray");
      out.set("x", a.x);
      out.set("y", a.y);
      return out;
    }
    if (open0) {
      const P2 b = at(t1);
      out.set("type", "ray");
      out.set("x", b.x);
      out.set("y", b.y);
      out.set("dx", -num(g(e, "dx")));
      out.set("dy", -num(g(e, "dy")));
      return out;
    }
    const P2 a = at(t0);
    const P2 b = at(t1);
    out.set("type", "line");
    out.set("x1", a.x);
    out.set("y1", a.y);
    out.set("x2", b.x);
    out.set("y2", b.y);
    out.erase("x");
    out.erase("y");
    out.erase("dx");
    out.erase("dy");
    return out;
  }
  if (t == "line") {
    const double x1 = num(g(e, "x1"));
    const double y1 = num(g(e, "y1"));
    const double x2 = num(g(e, "x2"));
    const double y2 = num(g(e, "y2"));
    Value out = e;
    out.set("x1", x1 + (x2 - x1) * t0);
    out.set("y1", y1 + (y2 - y1) * t0);
    out.set("x2", x1 + (x2 - x1) * t1);
    out.set("y2", y1 + (y2 - y1) * t1);
    return out;
  }
  if (t == "arc") {
    const auto [a0, a1] = detail::arc_range(num(g(e, "a0")), num(g(e, "a1")));
    const double s = a1 - a0;
    Value out = e;
    out.set("a0", detail::norm_angle(a0 + s * t0));
    out.set("a1", detail::norm_angle(a0 + s * t1));
    return out;
  }
  // open poly
  const Value pv = g(e, "pts");
  if (!pv.is_array() || pv.as_array().size() < 2) {
    return {nullptr};
  }
  const base::Array& P = pv.as_array();
  const auto last_seg = static_cast<double>(P.size() - 2);
  const double i0d = std::floor(t0);
  const double i1d = std::min(std::ceil(t1) - 1, last_seg);
  if (!(i0d >= 0) || !(i1d < static_cast<double>(P.size())) || i1d - i0d > 1e6) {
    return {nullptr};
  }
  const auto at = [&P](std::size_t i, double u) {
    const Value& p = P[i];
    const Value& q = P[i + 1];
    const Value bv = g(p, "b");
    const double b = bv.truthy() ? num(bv) : 0;
    const P2 a = xy(p);
    const P2 c = xy(q);
    if (std::abs(b) < 1e-12) {
      return P2{a.x + (c.x - a.x) * u, a.y + (c.y - a.y) * u};
    }
    const Prim arc = detail::bulge_arc(a.x, a.y, c.x, c.y, b);
    return detail::prim_point(arc, arc.rev ? 1 - u : u);
  };
  const auto part_bulge = [&P](std::size_t i, double ta, double tb) {
    const Value bv = g(P[i], "b");
    const double b = bv.truthy() ? num(bv) : 0;
    return std::abs(b) < 1e-12 ? 0 : std::tan(base::js_atan(b) * (tb - ta));
  };
  base::Array pts;
  const auto steps = static_cast<std::int64_t>(i1d - i0d);
  for (std::int64_t k = 0; k <= steps; ++k) {
    const double id = i0d + static_cast<double>(k);
    const auto i = static_cast<std::size_t>(id);
    const double ta = id == i0d ? t0 - id : 0;
    const double tb = id == i1d ? t1 - id : 1;
    const P2 a = at(i, ta);
    pts.push_back(base::make_object({{"x", a.x}, {"y", a.y}, {"b", part_bulge(i, ta, tb)}}));
    if (id == i1d) {
      const P2 b = at(i, tb);
      pts.push_back(base::make_object({{"x", b.x}, {"y", b.y}, {"b", 0}}));
    }
  }
  Value out = e;
  out.set("closed", false);
  out.set("pts", std::move(pts));
  return out;
}

std::optional<TrimSplit> trim_split(const Value& e, const std::vector<const Value*>& cutters, const BlockLookup& blocks,
                                    bool edge_extend) {
  const std::string t = type_of(e);
  if (!(t == "line" || t == "arc" || t == "circle" || t == "poly" || t == "xline" || t == "ray")) {
    return std::nullopt;
  }
  const Value pv = g(e, "pts");
  if (t == "poly" && (!pv.is_array() || pv.as_array().size() < 2)) {
    return std::nullopt;
  }
  const Value eid = g(e, "id");
  std::vector<const Value*> cs;
  for (const Value* c : cutters) {
    if (c != &e && (g(*c, "id").is_undefined() || !(g(*c, "id") == eid))) {
      cs.push_back(c);
    }
  }
  TrimSplit sp;
  sp.e = e;
  sp.own = own_prims(e, blocks);
  const std::vector<double> ts = entity_params(e, sp.own, cs, blocks, edge_extend);
  sp.closed = t == "circle" || (t == "poly" && g(e, "closed").truthy());
  const auto n_pts = static_cast<double>(pv.is_array() ? pv.as_array().size() : 0);
  sp.max_t = 1;
  if (t == "poly") {
    sp.max_t = g(e, "closed").truthy() ? n_pts : n_pts - 1;
  }
  if (sp.closed) {
    std::vector<double> cuts;
    for (const double x : ts) {
      double u = std::fmod(std::fmod(x, sp.max_t) + sp.max_t, sp.max_t);
      if (std::abs(u - sp.max_t) < 1e-9) {
        u = 0;
      }
      if (std::ranges::find(cuts, u) == cuts.end()) {
        cuts.push_back(u);
      }
    }
    std::ranges::sort(cuts);
    if (cuts.size() < 2) {
      sp.pieces.push_back({.t0 = 0, .t1 = sp.max_t, .whole = true});
    } else {
      for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (cuts[i + 1] - cuts[i] > 1e-9) {
          sp.pieces.push_back({.t0 = cuts[i], .t1 = cuts[i + 1], .whole = false});
        }
      }
      sp.pieces.push_back({.t0 = cuts.back(), .t1 = cuts[0] + sp.max_t, .whole = false});
    }
  } else {
    const double tiny = unbounded(t) ? 1e-15 : 1e-9;  // t along a construction line's 2·FAR stand-in
    std::vector<double> at = {0};
    for (const double x : ts) {
      if (x > tiny && x < sp.max_t - tiny) {
        at.push_back(x);
      }
    }
    at.push_back(sp.max_t);
    for (std::size_t i = 0; i + 1 < at.size(); ++i) {
      if (at[i + 1] - at[i] > tiny) {
        sp.pieces.push_back({.t0 = at[i], .t1 = at[i + 1], .whole = false});
      }
    }
  }
  return sp;
}

std::size_t piece_at(const TrimSplit& sp, P2 q) {
  double t = pick_param(sp.e, sp.own, q);
  if (sp.closed && sp.pieces.size() > 1 && t < sp.pieces[0].t0) {
    t += sp.max_t;
  }
  std::size_t best = 0;
  double bd = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < sp.pieces.size(); ++i) {
    const TrimPiece& pc = sp.pieces[i];
    double d = 0;
    if (t < pc.t0) {
      d = pc.t0 - t;
    } else if (t > pc.t1) {
      d = t - pc.t1;
    }
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

namespace {

Value doubled_ring(const Value& e) {
  // { ...e, closed: false, pts: [...e.pts, ...e.pts, { ...e.pts[0] }] }
  base::Array pts = copy_pts(g(e, "pts"));
  const base::Array once = pts;
  for (const Value& p : once) {
    pts.push_back(p);
  }
  if (!once.empty()) {
    pts.push_back(once[0]);
  }
  Value open = e;
  open.set("closed", false);
  open.set("pts", std::move(pts));
  return open;
}

Value span_entity(const TrimSplit& sp, double t0, double t1) {
  const std::string t = type_of(sp.e);
  if (t == "circle") {
    Value out = sp.e;
    out.set("type", "arc");
    out.set("a0", detail::norm_angle(t0 * kTau));
    out.set("a1", detail::norm_angle(t1 * kTau));
    return out;
  }
  if (t == "poly" && g(sp.e, "closed").truthy()) {
    return sub_entity(doubled_ring(sp.e), t0, t1 > t0 ? t1 : t1 + sp.max_t);
  }
  return sub_entity(sp.e, t0, t1);
}

}  // namespace

Value piece_entity(const TrimSplit& sp, std::size_t i) {
  if (i >= sp.pieces.size()) {
    return {nullptr};
  }
  const TrimPiece& pc = sp.pieces[i];
  if (pc.whole) {
    return sp.e;
  }
  return span_entity(sp, pc.t0, pc.t1);
}

std::optional<base::Array> trim_result(const TrimSplit& sp, const std::set<std::size_t>& gone) {
  bool any = false;
  for (std::size_t i = 0; i < sp.pieces.size(); ++i) {
    any = any || gone.contains(i);
  }
  if (!any) {
    return std::nullopt;
  }
  struct Run {
    double t0, t1;
  };
  std::vector<Run> runs;
  if (sp.closed) {
    if (sp.pieces.size() == 1) {
      return base::Array{};
    }
    const std::size_t n = sp.pieces.size();
    std::size_t start = 0;
    while (start < n && !gone.contains(start)) {
      ++start;
    }
    std::optional<Run> run;
    for (std::size_t k = 1; k <= n; ++k) {
      const std::size_t i = (start + k) % n;
      const TrimPiece& pc = sp.pieces[i];
      if (gone.contains(i)) {
        if (run) {
          runs.push_back(*run);
        }
        run.reset();
        continue;
      }
      const double t0 = run ? run->t0 : pc.t0;
      double t1 = pc.t1;
      for (int guard = 0; t1 < t0 - 1e-12 && guard < 64; ++guard) {
        t1 += sp.max_t;  // the run carries on past the start round the loop
      }
      run = Run{t0, t1};
    }
    if (run) {
      runs.push_back(*run);
    }
  } else {
    std::optional<Run> run;
    for (std::size_t i = 0; i < sp.pieces.size(); ++i) {
      const TrimPiece& pc = sp.pieces[i];
      if (gone.contains(i)) {
        if (run) {
          runs.push_back(*run);
        }
        run.reset();
        continue;
      }
      run = run ? Run{run->t0, pc.t1} : Run{pc.t0, pc.t1};
    }
    if (run) {
      runs.push_back(*run);
    }
  }
  base::Array out;
  for (const Run& r : runs) {
    Value v = span_entity(sp, r.t0, r.t1);
    if (v.truthy()) {
      out.push_back(std::move(v));
    }
  }
  return out;
}

std::vector<P2> path_crossings(const Value& e, const std::vector<P2>& pts, const BlockLookup& blocks) {
  std::vector<P2> out;
  const std::vector<Prim> own = own_prims(e, blocks);
  for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
    if (dist(pts[i], pts[i + 1]) <= kEps) {
      continue;
    }
    const Prim s = seg(pts[i], pts[i + 1]);
    for (const Prim& p : own) {
      for (const P2 x : intersect(p, s, false, false)) {
        out.push_back(x);
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------- EXTEND

Value extension_part(const Value& old, const Value& nu) {
  if (!old.truthy() || !nu.truthy()) {
    return {nullptr};
  }
  const std::string t = type_of(old);
  if (t == "line") {
    const bool s = dist({num(g(old, "x1")), num(g(old, "y1"))}, {num(g(nu, "x1")), num(g(nu, "y1"))}) > kEps;
    if (s) {
      return base::make_object(
          {{"type", "line"}, {"x1", g(old, "x1")}, {"y1", g(old, "y1")}, {"x2", g(nu, "x1")}, {"y2", g(nu, "y1")}});
    }
    return base::make_object(
        {{"type", "line"}, {"x1", g(old, "x2")}, {"y1", g(old, "y2")}, {"x2", g(nu, "x2")}, {"y2", g(nu, "y2")}});
  }
  if (t == "arc") {
    const double d = detail::norm_angle(num(g(old, "a0")) - num(g(nu, "a0")));
    Value out = old;
    if (std::abs(d) > 1e-12 && std::abs(d - kTau) > 1e-12) {
      out.set("a0", g(nu, "a0"));
      out.set("a1", g(old, "a0"));
    } else {
      out.set("a0", g(old, "a1"));
      out.set("a1", g(nu, "a1"));
    }
    return out;
  }
  if (t == "poly") {
    const Value op = g(old, "pts");
    const Value np = g(nu, "pts");
    if (!op.is_array() || !np.is_array() || op.as_array().empty() || np.as_array().size() < op.as_array().size()) {
      return {nullptr};
    }
    const std::size_t n = op.as_array().size();
    const std::size_t i = dist(xy(op.as_array()[0]), xy(np.as_array()[0])) > kEps ? 0 : n - 1;
    const P2 a = xy(op.as_array()[i]);
    const P2 b = xy(np.as_array()[i]);
    return base::make_object({{"type", "line"}, {"x1", a.x}, {"y1", a.y}, {"x2", b.x}, {"y2", b.y}});
  }
  return {nullptr};
}

// NOLINTNEXTLINE(misc-no-recursion) — a polyline's end segment once, as a line
std::optional<Value> extend_entity(const Value& e, const std::vector<const Value*>& boundaries, P2 pick,
                                   const BlockLookup& blocks, bool edge_extend) {
  // EDGEMODE 1: a boundary line or arc counts as the whole line or circle it lies on
  struct Bp {
    Prim q;
    bool ext;
  };
  std::vector<Bp> bps;
  const Value eid = g(e, "id");
  for (const Value* b : boundaries) {
    if (b == &e || (!g(*b, "id").is_undefined() && g(*b, "id") == eid)) {
      continue;
    }
    const std::string bt = type_of(*b);
    for (const Prim& q : prims_of(*b, blocks)) {
      bps.push_back({q, edge_extend && (bt == "line" || bt == "arc")});
    }
  }
  const std::string t = type_of(e);
  if (t == "line") {
    const P2 s{num(g(e, "x1")), num(g(e, "y1"))};
    const P2 f{num(g(e, "x2")), num(g(e, "y2"))};
    const bool at_start = dist(pick, s) < dist(pick, f);
    const P2 from = at_start ? f : s;
    const P2 to = at_start ? s : f;
    if (dist(from, to) < kEps) {
      return std::nullopt;
    }
    const Prim ray = seg(from, to);
    std::optional<std::pair<double, P2>> best;
    for (const Bp& b : bps) {
      for (const P2 x : intersect(ray, b.q, true, b.ext)) {
        const double tt = prim_param(ray, x, false);
        if (tt > 1 + 1e-9 && (!best || tt < best->first)) {
          best = std::pair{tt, x};
        }
      }
    }
    if (!best) {
      return std::nullopt;
    }
    Value out = e;
    out.set(at_start ? "x1" : "x2", best->second.x);
    out.set(at_start ? "y1" : "y2", best->second.y);
    return out;
  }
  if (t == "arc") {
    const auto [a0, a1] = detail::arc_range(num(g(e, "a0")), num(g(e, "a1")));
    const double cx = num(g(e, "cx"));
    const double cy = num(g(e, "cy"));
    const double r = num(g(e, "r"));
    const P2 ps{cx + base::js_cos(a0) * r, cy + base::js_sin(a0) * r};
    const P2 pe{cx + base::js_cos(a1) * r, cy + base::js_sin(a1) * r};
    const bool at_start = dist(pick, ps) < dist(pick, pe);
    Prim full;
    full.arc = true;
    full.cx = cx;
    full.cy = cy;
    full.r = r;
    full.a0 = 0;
    full.a1 = kTau;
    full.full = true;
    std::optional<std::pair<double, double>> best;  // gap, angle
    for (const Bp& b : bps) {
      for (const P2 x : intersect(full, b.q, true, b.ext)) {
        const double a = detail::norm_angle(base::js_atan2(x.y - cy, x.x - cx));
        const double gap = at_start ? detail::norm_angle(a0 - a) : detail::norm_angle(a - a1);
        if (gap > 1e-9 && gap < kTau - (a1 - a0) && (!best || gap < best->first)) {
          best = std::pair{gap, a};
        }
      }
    }
    if (!best) {
      return std::nullopt;
    }
    Value out = e;
    out.set(at_start ? "a0" : "a1", best->second);
    return out;
  }
  const Value pv = g(e, "pts");
  if (t == "poly" && !g(e, "closed").truthy() && pv.is_array() && pv.as_array().size() >= 2) {
    const base::Array& P = pv.as_array();
    const std::size_t n = P.size();
    const bool at_start = dist(pick, xy(P[0])) < dist(pick, xy(P[n - 1]));
    const Value& p = at_start ? P[1] : P[n - 2];
    const Value& q = at_start ? P[0] : P[n - 1];
    const Value bv = g(at_start ? P[0] : P[n - 2], "b");
    if (std::abs(bv.truthy() ? num(bv) : 0) > 1e-12) {
      return std::nullopt;  // arc ends: not yet
    }
    const Value tmp_line = base::make_object(
        {{"type", "line"}, {"x1", g(p, "x")}, {"y1", g(p, "y")}, {"x2", g(q, "x")}, {"y2", g(q, "y")}});
    const auto tmp = extend_entity(tmp_line, boundaries, xy(q), blocks, edge_extend);
    if (!tmp) {
      return std::nullopt;
    }
    base::Array pts = P;
    Value& v = pts[at_start ? 0 : n - 1];
    v.set("x", g(*tmp, "x2"));
    v.set("y", g(*tmp, "y2"));
    Value out = e;
    out.set("pts", std::move(pts));
    return out;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- selection

bool in_window(const Value& e, const Rect2& w, bool crossing, const BlockLookup& blocks) {
  const std::string t = type_of(e);
  if (unbounded(t)) {  // never inside a window (DR.6)
    if (!crossing) {
      return false;
    }
    // clipInfinite({x, y}, {dx, dy}, w, ray) is non-null
    double t0 = t == "ray" ? 0 : -std::numeric_limits<double>::infinity();
    double t1 = std::numeric_limits<double>::infinity();
    const std::array<std::array<double, 4>, 2> axes = {
        {{num(g(e, "dx")), w.min_x, w.max_x, num(g(e, "x"))}, {num(g(e, "dy")), w.min_y, w.max_y, num(g(e, "y"))}}};
    for (const auto& [q, lo, hi, v] : axes) {
      if (std::abs(q) < 1e-15) {
        if (v < lo || v > hi) {
          return false;
        }
        continue;
      }
      double a = (lo - v) / q;
      double b = (hi - v) / q;
      if (a > b) {
        std::swap(a, b);
      }
      t0 = std::isnan(a) ? a : std::max(t0, a);
      t1 = std::isnan(b) ? b : std::min(t1, b);
      if (t0 > t1) {
        return false;
      }
    }
    return std::isfinite(t0) && std::isfinite(t1);
  }
  const Bounds b = entity_bounds(e, blocks);
  if (!b.valid()) {
    return false;
  }
  const bool inside = b.min_x >= w.min_x && b.max_x <= w.max_x && b.min_y >= w.min_y && b.max_y <= w.max_y;
  if (inside || !crossing) {
    return inside;
  }
  if (b.max_x < w.min_x || b.min_x > w.max_x || b.max_y < w.min_y || b.min_y > w.max_y) {
    return false;
  }
  const std::array<Prim, 4> edges = {
      seg({w.min_x, w.min_y}, {w.max_x, w.min_y}), seg({w.max_x, w.min_y}, {w.max_x, w.max_y}),
      seg({w.max_x, w.max_y}, {w.min_x, w.max_y}), seg({w.min_x, w.max_y}, {w.min_x, w.min_y})};
  for (const Prim& p : prims_of(e, blocks)) {
    const P2 s = detail::prim_point(p, 0);
    if (s.x >= w.min_x && s.x <= w.max_x && s.y >= w.min_y && s.y <= w.max_y) {
      return true;
    }
    for (const Prim& ed : edges) {
      if (!intersect(p, ed, false, false).empty()) {
        return true;
      }
    }
  }
  return t == "text" || t == "hatch";  // the window sits inside its box
}

}  // namespace cc::cad
