// SPDX-License-Identifier: Apache-2.0
// geom.js's corners (CadCraft 0.5) in C++: FILLET and CHAMFER on two objects, on two segments of one polyline, or on
// every corner of a polyline (cornerTarget, filletPair, chamferPair, polyCorners), with the JS's order of operations
// and tolerances.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
constexpr double kPi = std::numbers::pi;
constexpr double kEps = 1e-9;

double dist(P2 a, P2 b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
std::string type_of(const Value& e) {
  return detail::str(g(e, "type"));
}
P2 xy(const Value& p) {
  return {num(g(p, "x")), num(g(p, "y"))};
}
P2 polar(P2 c, double a, double d) {
  return {c.x + base::js_cos(a) * d, c.y + base::js_sin(a) * d};
}
double prim_length(const Prim& p) {
  return !p.arc ? base::js_hypot(p.x2 - p.x1, p.y2 - p.y1) : p.r * (p.a1 - p.a0);
}
std::optional<P2> unit(double dx, double dy) {
  const double l = base::js_hypot(dx, dy);
  return l > 0 ? std::optional<P2>(P2{dx / l, dy / l}) : std::nullopt;
}
double dot(P2 a, P2 b) {
  return a.x * b.x + a.y * b.y;
}
double clamp1(double v) {
  return std::max(-1.0, std::min(1.0, v));
}
/// Math.sign
double js_sign(double v) {
  if (std::isnan(v)) {
    return v;
  }
  if (v > 0) {
    return 1;
  }
  return v < 0 ? -1 : v;
}
/// v.b || 0
double bulge(const Value& v) {
  const Value b = g(v, "b");
  return b.truthy() ? num(b) : 0;
}
std::vector<Prim> poly_prims(const Value& e) {
  std::vector<Prim> out;
  detail::poly_prims(e, out);
  return out;
}
bool straight(const Prim& p) {
  return !p.arc;
}
std::optional<P2> line_dir(const Prim& p) {
  return unit(p.x2 - p.x1, p.y2 - p.y1);
}
Prim full_of(const Prim& p) {
  Prim f;
  f.arc = true;
  f.cx = p.cx;
  f.cy = p.cy;
  f.r = p.r;
  f.a0 = 0;
  f.a1 = kTau;
  f.full = true;
  return f;
}
Prim line_prim(P2 a, P2 b) {
  return detail::line_prim(a.x, a.y, b.x, b.y);
}
Value vtx(double x, double y, double b) {
  return base::make_object({{"x", x}, {"y", y}, {"b", b}});
}

/// Which part of a piece a cut keeps: a unit vector for a straight piece, 'ccw' / 'cw' for an arc.
struct Keep {
  bool arc = false;
  P2 u;
  bool ccw = false;
};
Keep keep_u(P2 u) {
  return {.arc = false, .u = u, .ccw = false};
}
Keep keep_turn(bool ccw) {
  return {.arc = true, .u = {}, .ccw = ccw};
}

struct Cut {
  std::string error;
  Value e;
  bool unchanged = false;  // the object itself, untouched (a circle)
};

/// geom.js cutTarget(t, T, keep, e): one end of the target's piece moved to T, keeping the part that runs from T
/// in the direction `keep`.
Cut cut_target(const CornerTarget& t, P2 tt, const Keep& keep, const Value& e) {
  if (type_of(e) == "circle") {
    return {.error = {}, .e = e, .unchanged = true};
  }
  const Prim& p = t.prim;
  bool start = false;  // which end of the primitive T replaces: the start (x1 / a0) or the end
  if (!p.arc) {
    const double ds = (p.x1 - tt.x) * keep.u.x + (p.y1 - tt.y) * keep.u.y;
    const double de = (p.x2 - tt.x) * keep.u.x + (p.y2 - tt.y) * keep.u.y;
    if (std::max(ds, de) <= 1e-9 * std::max(1.0, prim_length(p))) {
      return {.error = "large", .e = {}, .unchanged = false};
    }
    start = de >= ds;
  } else {
    const double a = base::js_atan2(tt.y - p.cy, tt.x - p.cx);
    start = keep.ccw;
    if (detail::on_sweep(a, p.a0, p.a1, 1e-9)) {
      if ((start ? detail::norm_angle(p.a1 - a) : detail::norm_angle(a - p.a0)) < 1e-9) {
        return {.error = "large", .e = {}, .unchanged = false};
      }
    } else {
      const double to_start = detail::norm_angle(p.a0 - a);  // an extension grows the nearer end
      const double to_end = detail::norm_angle(a - p.a1);
      if (start ? to_start > to_end : to_end > to_start) {
        return {.error = "large", .e = {}, .unchanged = false};
      }
    }
  }
  const std::string type = type_of(e);
  if (type == "line") {
    Value r = e;
    r.set(start ? "x1" : "x2", tt.x);
    r.set(start ? "y1" : "y2", tt.y);
    return {.error = {}, .e = std::move(r), .unchanged = false};
  }
  if (type == "arc") {
    const double a = detail::norm_angle(base::js_atan2(tt.y - num(g(e, "cy")), tt.x - num(g(e, "cx"))));
    Value r = e;
    r.set(start ? "a0" : "a1", a);
    return {.error = {}, .e = std::move(r), .unchanged = false};
  }
  if (type != "poly" || !t.seg) {
    return {.error = "type", .e = {}, .unchanged = false};
  }
  // a polyline segment i runs from vertex i (P) to vertex i+1 (Q); a reversed arc primitive starts at Q
  const Value src = g(e, "pts");
  const std::size_t n = detail::len(src);
  const std::size_t i = *t.seg;
  const bool closed = g(e, "closed").truthy();
  const std::size_t segs = closed ? n : n - 1;
  const bool at_start = !p.arc || !p.rev ? start : !start;  // T replaces P (else Q)
  if (closed || (at_start ? i != 0 : i + 1 != segs)) {
    return {.error = "middle", .e = {}, .unchanged = false};
  }
  base::Array pts = src.as_array();
  const std::size_t k = at_start ? i : i + 1;
  if (k >= pts.size()) {
    return {.error = "middle", .e = {}, .unchanged = false};
  }
  pts[k].set("x", tt.x);
  pts[k].set("y", tt.y);
  if (p.arc) {
    const double a = base::js_atan2(tt.y - p.cy, tt.x - p.cx);
    const double a0 = start ? a : p.a0;
    const double a1 = !start ? a : p.a1;
    pts[i].set("b", std::tan(detail::norm_angle(a1 - a0) / 4) * (p.rev ? -1 : 1));
  }
  Value r = e;
  r.set("pts", std::move(pts));
  return {.error = {}, .e = std::move(r), .unchanged = false};
}

/// geom.js llFillet(x, ua, ub, r): the fillet of two straight pieces at x along the unit rays ua and ub
struct LlFillet {
  double t;
  P2 a, b, c;
};
std::optional<LlFillet> ll_fillet(P2 x, P2 ua, P2 ub, double r) {
  const double half = std::acos(clamp1(dot(ua, ub))) / 2;
  if (half < 1e-9 || half > kPi / 2 - 1e-9) {
    return std::nullopt;
  }
  const double t = r / std::tan(half);
  const std::optional<P2> bis = unit(ua.x + ub.x, ua.y + ub.y);
  if (!bis) {
    return std::nullopt;
  }
  return LlFillet{.t = t,
                  .a = polar(x, base::js_atan2(ua.y, ua.x), t),
                  .b = polar(x, base::js_atan2(ub.y, ub.x), t),
                  .c = {x.x + bis->x * r / std::sin(half), x.y + bis->y * r / std::sin(half)}};
}

/// geom.js cornerArc(c, r, a, b, like): the arc of circle c between a and b, the short way round
Value corner_arc(P2 c, double r, P2 a, P2 b, const Value& like) {
  double a0 = base::js_atan2(a.y - c.y, a.x - c.x);
  double a1 = base::js_atan2(b.y - c.y, b.x - c.x);
  if (detail::norm_angle(a1 - a0) > kPi) {
    std::swap(a0, a1);
  }
  return base::make_object({{"type", "arc"},
                            {"layer", g(like, "layer")},
                            {"color", g(like, "color")},
                            {"cx", c.x},
                            {"cy", c.y},
                            {"r", r},
                            {"a0", detail::norm_angle(a0)},
                            {"a1", detail::norm_angle(a1)}});
}

/// geom.js keepRay(p, x, pick): the ray from corner x along a straight piece toward the part its pick keeps
struct Ray {
  P2 u;
  double reach;
};
std::optional<Ray> keep_ray(const Prim& p, P2 x, P2 pick) {
  P2 far{p.x1, p.y1};
  P2 other{p.x2, p.y2};
  if (dist(other, x) - dist(far, x) > 0) {  // sort((u, v) => dist(v, x) - dist(u, x)): the farther end first
    std::swap(far, other);
  }
  const std::optional<P2> u0 = unit(far.x - x.x, far.y - x.y);
  if (!u0) {
    return std::nullopt;
  }
  if ((pick.x - x.x) * u0->x + (pick.y - x.y) * u0->y >= 0 || dist(other, x) <= kEps) {
    return Ray{*u0, dist(far, x)};
  }
  const std::optional<P2> u1 = unit(other.x - x.x, other.y - x.y);
  if (!u1) {
    return std::nullopt;
  }
  return Ray{*u1, dist(other, x)};
}

/// geom.js sharedVertex(A, B): the vertex between two adjacent segments of one polyline
struct Shared {
  std::size_t k, in_seg, out_seg;
};
std::optional<Shared> shared_vertex(const CornerTarget& a, const CornerTarget& b) {
  const std::size_t n = detail::len(g(a.e, "pts"));
  const bool closed = g(a.e, "closed").truthy();
  const std::size_t segs = closed ? n : n - 1;
  if (segs < 2 || !a.seg || !b.seg || n == 0) {
    return std::nullopt;
  }
  const std::size_t i = *a.seg;
  const std::size_t j = *b.seg;
  if ((i + 1) % n == j && (closed || i + 1 < n - 1)) {
    return Shared{j, i, j};
  }
  if ((j + 1) % n == i && (closed || j + 1 < n - 1)) {
    return Shared{i, j, i};
  }
  return std::nullopt;
}

/// geom.js dedupe(pts, closed): no vertex on top of the one before
base::Array dedupe(base::Array pts, bool closed) {
  base::Array out;
  for (Value& v : pts) {
    if (!out.empty() && dist(xy(out.back()), xy(v)) <= 1e-9) {
      out.back().set("b", bulge(v));
      continue;
    }
    out.push_back(std::move(v));
  }
  if (closed && out.size() > 1 && dist(xy(out.front()), xy(out.back())) <= 1e-9) {
    out.pop_back();
  }
  return out;
}

/// geom.js spliceCorner(e, k, a, bulge, b): vertex k replaced by the corner points a (bulge toward b) and b
Value splice_corner(const Value& e, std::size_t k, P2 a, double bul, P2 b) {
  base::Array pts = g(e, "pts").as_array();
  const double kb = bulge(pts[k]);
  pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(k));
  pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(k), {vtx(a.x, a.y, bul), vtx(b.x, b.y, kb)});
  Value r = e;
  r.set("pts", dedupe(std::move(pts), g(e, "closed").truthy()));
  return r;
}

/// geom.js vertexCorner(e, k, inSeg, outSeg): the corner of two adjacent straight segments
struct VCorner {
  P2 x, ua, ub;
  double la, lb, turn;
};
std::optional<VCorner> vertex_corner(const Value& e, std::size_t k, std::size_t in_seg, std::size_t out_seg) {
  const Value pts = g(e, "pts");
  const std::size_t n = detail::len(pts);
  if (n == 0 || k >= n || in_seg >= n) {
    return std::nullopt;
  }
  const P2 v = xy(pts.as_array()[k]);
  const P2 pin = xy(pts.as_array()[in_seg]);
  const P2 pout = xy(pts.as_array()[(out_seg + 1) % n]);
  const std::optional<P2> ua = unit(pin.x - v.x, pin.y - v.y);
  const std::optional<P2> ub = unit(pout.x - v.x, pout.y - v.y);
  if (!ua || !ub) {
    return std::nullopt;
  }
  return VCorner{.x = v,
                 .ua = *ua,
                 .ub = *ub,
                 .la = dist(pin, v),
                 .lb = dist(pout, v),
                 .turn = js_sign(-ua->x * ub->y + ua->y * ub->x)};
}

/// geom.js layerOf(a, b, current): { layer, color } of the new piece
Value layer_of(const Value& a, const Value& b, const Value& current) {
  const Value ca = detail::nn(g(a, "color"), Value(nullptr));
  const Value cb = detail::nn(g(b, "color"), Value(nullptr));
  if (g(a, "layer") == g(b, "layer") && ca == cb) {
    return base::make_object({{"layer", g(a, "layer")}, {"color", ca}});
  }
  const Value cl = current.is_object() ? g(current, "layer") : Value();
  const Value cc = current.is_object() ? g(current, "color") : Value();
  return base::make_object(
      {{"layer", detail::nullish(cl) ? g(a, "layer") : cl}, {"color", detail::nullish(cc) ? Value(nullptr) : cc}});
}

bool same_object(const CornerTarget& a, const CornerTarget& b) {
  const Value ia = g(a.e, "id");
  return !detail::nullish(ia) && ia == g(b.e, "id");
}

CornerResult fail(std::string code) {
  CornerResult r;
  r.ok = false;
  r.error = std::move(code);
  return r;
}

/// geom.js cutBoth(A, B, ca, cb, { trim, add, same })
struct CutAt {
  P2 at;
  Keep keep;
};
CornerResult cut_both(const CornerTarget& a, const CornerTarget& b, const std::optional<CutAt>& ca,
                      const std::optional<CutAt>& cb, bool trim, base::Array add, bool same = false) {
  if (same && (!ca || !cb)) {
    return fail("middle");
  }
  CornerResult out;
  out.ok = true;
  if (!trim) {
    out.add = std::move(add);
    return out;
  }
  Cut ra = ca ? cut_target(a, ca->at, ca->keep, a.e) : Cut{.error = {}, .e = a.e, .unchanged = true};
  if (!ra.error.empty()) {
    return fail(ra.error);
  }
  Cut rb = cb ? cut_target(b, cb->at, cb->keep, same ? ra.e : b.e) : Cut{.error = {}, .e = b.e, .unchanged = true};
  if (!rb.error.empty()) {
    return fail(rb.error);
  }
  if (same) {
    out.edits.push_back(std::move(rb.e));
  } else {
    if (!ra.unchanged) {
      out.edits.push_back(std::move(ra.e));
    }
    if (!rb.unchanged) {
      out.edits.push_back(std::move(rb.e));
    }
  }
  out.add = std::move(add);
  return out;
}

/// geom.js sharpKeep(p, x, pick): for a sharp corner on a curve, which way from x the picked part lies
Keep sharp_keep(const Prim& p, P2 x, P2 pick) {
  if (straight(p)) {
    const P2 u = line_dir(p).value_or(P2{});
    return dot(u, {pick.x - x.x, pick.y - x.y}) >= 0 ? keep_u(u) : keep_u({-u.x, -u.y});
  }
  const double a = base::js_atan2(x.y - p.cy, x.x - p.cx);
  const double ap = base::js_atan2(pick.y - p.cy, pick.x - p.cx);
  if (detail::on_sweep(a, p.a0, p.a1, 1e-9) && !p.full) {
    return keep_turn(!(detail::norm_angle(ap - p.a0) <= detail::norm_angle(a - p.a0)));
  }
  if (p.full) {
    return keep_turn(detail::norm_angle(ap - a) <= kPi);
  }
  return keep_turn(detail::norm_angle(p.a0 - a) <= detail::norm_angle(a - p.a1));  // off the arc: keep all of it
}

/// geom.js chamferCuts(spec, ua, ub): the cut on each side of a corner
struct Cuts {
  double da, db;
};
double or0(double v) {
  return v != 0 && !std::isnan(v) ? v : 0;
}
std::optional<Cuts> chamfer_cuts(const ChamferSpec& spec, P2 ua, P2 ub) {
  if (spec.method == "A") {
    const double theta = std::acos(clamp1(dot(ua, ub)));
    const double al = or0(spec.ang);
    if (!(spec.len > kEps)) {
      return Cuts{0, 0};
    }
    if (!(al > 1e-9) || theta + al >= kPi - 1e-9) {
      return std::nullopt;
    }
    return Cuts{spec.len, spec.len * std::sin(al) / std::sin(theta + al)};
  }
  return Cuts{std::max(0.0, or0(spec.d1)), std::max(0.0, spec.d2 ? *spec.d2 : spec.d1)};
}

}  // namespace

std::optional<CornerTarget> corner_target(const Value& e, P2 pick) {
  const std::string t = type_of(e);
  if (t == "line") {
    if (!(dist({num(g(e, "x1")), num(g(e, "y1"))}, {num(g(e, "x2")), num(g(e, "y2"))}) > kEps)) {
      return std::nullopt;
    }
    return CornerTarget{.e = e, .seg = std::nullopt, .prim = entity_prims(e, nullptr).at(0), .pick = pick};
  }
  if (t == "arc" || t == "circle") {
    if (!(num(g(e, "r")) > kEps)) {
      return std::nullopt;
    }
    return CornerTarget{.e = e, .seg = std::nullopt, .prim = entity_prims(e, nullptr).at(0), .pick = pick};
  }
  if (t == "poly") {
    std::optional<std::size_t> best;
    double bd = std::numeric_limits<double>::infinity();
    const std::vector<Prim> ps = poly_prims(e);
    for (std::size_t i = 0; i < ps.size(); ++i) {
      if (prim_length(ps[i]) <= kEps) {
        continue;
      }
      const double d = dist_to_prim(ps[i], pick);
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return CornerTarget{.e = e, .seg = best, .prim = ps[*best], .pick = pick};
  }
  return std::nullopt;
}

CornerResult fillet_pair(const CornerTarget& a, const CornerTarget& b, double r, bool trim, const Value& current) {
  const Value like = layer_of(a.e, b.e, current);
  if (same_object(a, b)) {
    if (type_of(a.e) != "poly" || a.seg == b.seg) {
      return fail("same");
    }
    const std::optional<Shared> sv = shared_vertex(a, b);
    if (!sv) {
      return cut_both(a, b, std::nullopt, std::nullopt, true, {}, true);
    }
    const std::vector<Prim> ps = poly_prims(a.e);
    if (sv->in_seg >= ps.size() || sv->out_seg >= ps.size() || !straight(ps[sv->in_seg]) ||
        !straight(ps[sv->out_seg])) {
      return fail("type");
    }
    const std::optional<VCorner> vc = vertex_corner(a.e, sv->k, sv->in_seg, sv->out_seg);
    if (!vc) {
      return fail("none");
    }
    if (r <= kEps) {  // already a sharp corner
      CornerResult ok;
      ok.ok = true;
      return ok;
    }
    const std::optional<LlFillet> f = ll_fillet(vc->x, vc->ua, vc->ub, r);
    if (!f) {
      return fail("collinear");
    }
    if (f->t > vc->la + 1e-9 || f->t > vc->lb + 1e-9) {
      return fail("large");
    }
    const double sweep = kPi - 2 * std::acos(clamp1(dot(vc->ua, vc->ub))) / 2;
    CornerResult ok;
    ok.ok = true;
    ok.edits.push_back(splice_corner(a.e, sv->k, f->a, std::tan(sweep / 4) * vc->turn, f->b));
    return ok;
  }
  const Prim& pa = a.prim;
  const Prim& pb = b.prim;
  if (straight(pa) && straight(pb)) {
    const std::vector<P2> xs = intersect(pa, pb, true, true);
    if (xs.empty()) {
      // parallel lines: a half circle across the gap
      const P2 ua = line_dir(pa).value_or(P2{});
      const P2 ub = line_dir(pb).value_or(P2{});
      P2 ee{pa.x1, pa.y1};
      if (dist({pa.x2, pa.y2}, a.pick) - dist(ee, a.pick) < 0) {
        ee = {pa.x2, pa.y2};
      }
      const double s = (ee.x - pb.x1) * ub.x + (ee.y - pb.y1) * ub.y;
      const P2 ff{pb.x1 + ub.x * s, pb.y1 + ub.y * s};
      const double gap = dist(ee, ff);
      if (gap <= 1e-9 * std::max(1.0, prim_length(pa))) {
        return fail("collinear");
      }
      const P2 other = dist(ee, {pa.x1, pa.y1}) < dist(ee, {pa.x2, pa.y2}) ? P2{pa.x2, pa.y2} : P2{pa.x1, pa.y1};
      const P2 k = unit(other.x - ee.x, other.y - ee.y).value_or(ua);
      const P2 m{(ee.x + ff.x) / 2, (ee.y + ff.y) / 2};
      const double rr = gap / 2;
      const double ae = base::js_atan2(ee.y - m.y, ee.x - m.x);
      const double af = base::js_atan2(ff.y - m.y, ff.x - m.x);
      const bool ccw_from_e = (ee.x - m.x) * -k.y - (ee.y - m.y) * -k.x > 0;
      Value arc = base::make_object({{"type", "arc"},
                                     {"layer", g(like, "layer")},
                                     {"color", g(like, "color")},
                                     {"cx", m.x},
                                     {"cy", m.y},
                                     {"r", rr},
                                     {"a0", detail::norm_angle(ccw_from_e ? ae : af)},
                                     {"a1", detail::norm_angle(ccw_from_e ? af : ae)}});
      CornerResult res = cut_both(a, b, CutAt{ee, keep_u(k)}, CutAt{ff, keep_u(k)}, trim, {std::move(arc)});
      res.semicircle = true;
      return res;
    }
    const P2 x = xs[0];
    const std::optional<Ray> ra = keep_ray(pa, x, a.pick);
    const std::optional<Ray> rb = keep_ray(pb, x, b.pick);
    if (!ra || !rb) {
      return fail("none");
    }
    if (r <= kEps) {
      return cut_both(a, b, CutAt{x, keep_u(ra->u)}, CutAt{x, keep_u(rb->u)}, trim, {});
    }
    const std::optional<LlFillet> f = ll_fillet(x, ra->u, rb->u, r);
    if (!f) {
      return fail("collinear");
    }
    if (f->t >= ra->reach - 1e-9 || f->t >= rb->reach - 1e-9) {
      return fail("large");
    }
    return cut_both(a, b, CutAt{f->a, keep_u(ra->u)}, CutAt{f->b, keep_u(rb->u)}, trim,
                    {corner_arc(f->c, r, f->a, f->b, like)});
  }
  // a line, arc or circle with an arc or circle
  const auto inf = [](const Prim& p) { return straight(p) ? p : full_of(p); };
  if (r <= kEps) {
    std::optional<std::pair<double, P2>> best;
    for (const P2 x : intersect(inf(pa), inf(pb), true, true)) {
      const double s = dist(x, a.pick) + dist(x, b.pick);
      if (!best || s < best->first) {
        best = {s, x};
      }
    }
    if (!best) {
      return fail("none");
    }
    return cut_both(a, b, CutAt{best->second, sharp_keep(pa, best->second, a.pick)},
                    CutAt{best->second, sharp_keep(pb, best->second, b.pick)}, trim, {});
  }
  const auto offsets = [r](const Prim& p) {
    std::vector<Prim> out;
    if (straight(p)) {
      const P2 u = line_dir(p).value_or(P2{});
      const P2 n{-u.y * r, u.x * r};
      for (const double s : {1.0, -1.0}) {
        out.push_back(detail::line_prim(p.x1 + n.x * s, p.y1 + n.y * s, p.x2 + n.x * s, p.y2 + n.y * s));
      }
      return out;
    }
    for (const double rr : {p.r + r, p.r - r}) {
      if (rr > kEps) {
        Prim f = full_of(p);
        f.r = rr;
        out.push_back(f);
      }
    }
    return out;
  };
  const auto touch = [](const Prim& p, P2 c) -> std::optional<P2> {
    if (straight(p)) {
      return detail::prim_point(p, prim_param(p, c, false));
    }
    const std::optional<P2> u = unit(c.x - p.cx, c.y - p.cy);
    if (!u) {
      return std::nullopt;
    }
    return P2{p.cx + u->x * p.r, p.cy + u->y * p.r};
  };
  struct Best {
    double s;
    P2 c, ta, tb;
  };
  std::optional<Best> best;
  for (const Prim& oa : offsets(pa)) {
    for (const Prim& ob : offsets(pb)) {
      for (const P2 c : intersect(oa, ob, true, true)) {
        const std::optional<P2> ta = touch(pa, c);
        const std::optional<P2> tb = touch(pb, c);
        if (!ta || !tb || dist(*ta, *tb) <= 1e-9) {
          continue;
        }
        const double s = dist(*ta, a.pick) + dist(*tb, b.pick);
        if (!best || s < best->s) {
          best = Best{s, c, *ta, *tb};
        }
      }
    }
  }
  if (!best) {
    return fail("none");
  }
  const auto away = [](const Prim& p, P2 at, P2 to) {  // the original runs away from the fillet arc at its tangent
    if (straight(p)) {
      const P2 u = line_dir(p).value_or(P2{});
      const double s = dot(u, {to.x - at.x, to.y - at.y});
      return s > 0 ? keep_u({-u.x, -u.y}) : keep_u(u);
    }
    const double ang = base::js_atan2(at.y - p.cy, at.x - p.cx);
    return keep_turn(!((to.x - at.x) * -base::js_sin(ang) + (to.y - at.y) * base::js_cos(ang) > 0));
  };
  return cut_both(a, b, CutAt{best->ta, away(pa, best->ta, best->tb)}, CutAt{best->tb, away(pb, best->tb, best->ta)},
                  trim, {corner_arc(best->c, r, best->ta, best->tb, like)});
}

CornerResult chamfer_pair(const CornerTarget& a, const CornerTarget& b, const ChamferSpec& spec, bool trim,
                          const Value& current) {
  if (!straight(a.prim) || !straight(b.prim)) {
    return fail("type");
  }
  const Value like = layer_of(a.e, b.e, current);
  if (same_object(a, b)) {
    if (type_of(a.e) != "poly" || a.seg == b.seg) {
      return fail("same");
    }
    const std::optional<Shared> sv = shared_vertex(a, b);
    if (!sv) {
      return fail("middle");
    }
    // the first pick's segment takes the first distance
    const std::optional<VCorner> vc = vertex_corner(a.e, sv->k, sv->in_seg, sv->out_seg);
    if (!vc) {
      return fail("none");
    }
    const bool first_in = a.seg == sv->in_seg;
    const std::optional<Cuts> cuts = chamfer_cuts(spec, first_in ? vc->ua : vc->ub, first_in ? vc->ub : vc->ua);
    if (!cuts) {
      return fail("angle");
    }
    const double d_in = first_in ? cuts->da : cuts->db;
    const double d_out = first_in ? cuts->db : cuts->da;
    if (std::abs(dot(vc->ua, vc->ub)) > 1 - 1e-12) {
      return fail("collinear");
    }
    if (d_in > vc->la + 1e-9 || d_out > vc->lb + 1e-9) {
      return fail("large");
    }
    CornerResult ok;
    ok.ok = true;
    if (d_in <= kEps && d_out <= kEps) {
      return ok;
    }
    ok.edits.push_back(splice_corner(a.e, sv->k, polar(vc->x, base::js_atan2(vc->ua.y, vc->ua.x), d_in), 0,
                                     polar(vc->x, base::js_atan2(vc->ub.y, vc->ub.x), d_out)));
    return ok;
  }
  const std::vector<P2> xs = intersect(a.prim, b.prim, true, true);
  if (xs.empty()) {
    return fail("parallel");
  }
  const P2 x = xs[0];
  const std::optional<Ray> ra = keep_ray(a.prim, x, a.pick);
  const std::optional<Ray> rb = keep_ray(b.prim, x, b.pick);
  if (!ra || !rb) {
    return fail("none");
  }
  if (std::abs(dot(ra->u, rb->u)) > 1 - 1e-12) {
    return fail("collinear");
  }
  const std::optional<Cuts> cuts = chamfer_cuts(spec, ra->u, rb->u);
  if (!cuts) {
    return fail("angle");
  }
  if (cuts->da > ra->reach + 1e-9 || cuts->db > rb->reach + 1e-9) {
    return fail("large");
  }
  const P2 qa = polar(x, base::js_atan2(ra->u.y, ra->u.x), cuts->da);
  const P2 qb = polar(x, base::js_atan2(rb->u.y, rb->u.x), cuts->db);
  base::Array add;
  if (dist(qa, qb) > kEps) {
    add.push_back(base::make_object({{"type", "line"},
                                     {"layer", g(like, "layer")},
                                     {"color", g(like, "color")},
                                     {"x1", qa.x},
                                     {"y1", qa.y},
                                     {"x2", qb.x},
                                     {"y2", qb.y}}));
  }
  return cut_both(a, b, CutAt{qa, keep_u(ra->u)}, CutAt{qb, keep_u(rb->u)}, trim, std::move(add));
}

namespace {

/// geom.js unfilletPoly(e): a polyline without the fillet arcs it already has
std::pair<Value, std::size_t> unfillet_poly(const Value& e) {
  const Value src = g(e, "pts");
  const std::size_t n = detail::len(src);
  const bool closed = g(e, "closed").truthy();
  if (n < 4) {
    return {e, 0};
  }
  const base::Array& v = src.as_array();
  const auto dir = [](P2 p, P2 q) { return base::js_atan2(q.y - p.y, q.x - p.x); };
  const auto same = [](double a, double b) {
    const double d = detail::norm_angle(a - b);
    return d < 1e-6 || d > kTau - 1e-6;
  };
  std::set<std::size_t> drop;
  std::map<std::size_t, P2> at;
  for (std::size_t k = 0; k < n; ++k) {
    const double b = bulge(v[k]);
    if (std::abs(b) < 1e-12) {
      continue;
    }
    const std::size_t pk = (k + n - 1) % n;
    const std::size_t k1 = (k + 1) % n;
    const std::size_t k2 = (k + 2) % n;
    if (!closed && (k < 1 || k + 2 > n - 1)) {
      continue;
    }
    if (std::abs(bulge(v[pk])) > 1e-12 || std::abs(bulge(v[k1])) > 1e-12 || drop.contains(k) || drop.contains(k1) ||
        at.contains(k1)) {
      continue;
    }
    const double chord = dir(xy(v[k]), xy(v[k1]));
    const double th = 4 * base::js_atan(b);
    if (!same(dir(xy(v[pk]), xy(v[k])), chord - th / 2) || !same(dir(xy(v[k1]), xy(v[k2])), chord + th / 2)) {
      continue;
    }
    const std::vector<P2> xs = intersect(line_prim(xy(v[pk]), xy(v[k])), line_prim(xy(v[k1]), xy(v[k2])), true, true);
    if (xs.empty()) {
      continue;
    }
    at[k] = xs[0];
    drop.insert(k1);
  }
  if (at.empty()) {
    return {e, 0};
  }
  base::Array pts;
  for (std::size_t k = 0; k < n; ++k) {
    if (drop.contains(k)) {
      continue;
    }
    if (const auto it = at.find(k); it != at.end()) {
      pts.push_back(vtx(it->second.x, it->second.y, 0));
    } else {
      pts.push_back(v[k]);
    }
  }
  Value r = e;
  r.set("pts", std::move(pts));
  return {r, at.size()};
}

}  // namespace

PolyCornersResult poly_corners(const Value& e, bool fillet, double radius, const ChamferSpec& spec) {
  PolyCornersResult out;
  Value src = e;
  if (fillet) {
    auto [u, n] = unfillet_poly(e);
    src = std::move(u);
    out.removed = n;
  }
  const Value vv = g(src, "pts");
  const std::size_t n = detail::len(vv);
  const bool closed = g(src, "closed").truthy();
  if (n == 0) {
    out.e = out.removed != 0 ? src : e;
    return out;
  }
  const base::Array& v = vv.as_array();
  const std::size_t segs = closed ? n : n - 1;
  const auto is_line = [&v](std::size_t i) { return std::abs(bulge(v[i])) < 1e-12; };
  std::vector<double> used(segs, 0);
  const auto seg_len = [&v, n](std::size_t i) { return dist(xy(v[i]), xy(v[(i + 1) % n])); };
  struct Repl {
    P2 a, b;
    double bul = 0;
  };
  std::map<std::size_t, Repl> repl;
  for (std::size_t k = closed ? 0 : 1; k < (closed ? n : n - 1); ++k) {
    const std::size_t i = (k + n - 1) % n;
    if (!is_line(i) || !is_line(k)) {
      ++out.arcs;
      continue;
    }
    const std::optional<VCorner> vc = vertex_corner(src, k, i, k);
    if (!vc || std::abs(dot(vc->ua, vc->ub)) > 1 - 1e-12) {
      continue;  // no corner there
    }
    P2 a;
    P2 b;
    double bul = 0;
    if (fillet) {
      if (!(radius > kEps)) {
        continue;
      }
      const std::optional<LlFillet> f = ll_fillet(vc->x, vc->ua, vc->ub, radius);
      if (!f) {
        continue;
      }
      a = f->a;
      b = f->b;
      const double sweep = kPi - std::acos(clamp1(dot(vc->ua, vc->ub)));
      bul = std::tan(sweep / 4) * vc->turn;
    } else {
      const std::optional<Cuts> cuts = chamfer_cuts(spec, vc->ua, vc->ub);
      if (!cuts) {
        ++out.short_;
        continue;
      }
      if (cuts->da <= kEps && cuts->db <= kEps) {
        continue;
      }
      a = polar(vc->x, base::js_atan2(vc->ua.y, vc->ua.x), cuts->da);
      b = polar(vc->x, base::js_atan2(vc->ub.y, vc->ub.x), cuts->db);
    }
    const double ta = dist(vc->x, a);
    const double tb = dist(vc->x, b);
    if (i >= segs || k >= segs || ta > seg_len(i) - used[i] + 1e-9 || tb > seg_len(k) - used[k] + 1e-9) {
      ++out.short_;
      continue;
    }
    used[i] += ta;
    used[k] += tb;
    repl[k] = {a, b, bul};
  }
  if (repl.empty()) {
    out.e = out.removed != 0 ? src : e;
    return out;
  }
  base::Array pts;
  for (std::size_t k = 0; k < n; ++k) {
    if (const auto it = repl.find(k); it != repl.end()) {
      pts.push_back(vtx(it->second.a.x, it->second.a.y, it->second.bul));
      pts.push_back(vtx(it->second.b.x, it->second.b.y, bulge(v[k])));
    } else {
      pts.push_back(v[k]);
    }
  }
  out.done = repl.size();
  Value r = src;
  r.set("pts", dedupe(std::move(pts), closed));
  out.e = std::move(r);
  return out;
}

}  // namespace cc::cad
