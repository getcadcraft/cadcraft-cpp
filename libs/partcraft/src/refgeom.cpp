// SPDX-License-Identifier: Apache-2.0
// refgeom.js, in the JS's order of evaluation (which reference is resolved first decides which failure is said).
#include "cc/partcraft/refgeom.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::partcraft::refgeom {
namespace {

using base::Value;
using vec::V3;
namespace V = vec;

constexpr double kEps = 1e-9;

[[noreturn]] void fail(const Resolvers& R, const std::string& m) {
  R.fail(m);
  throw std::logic_error("refgeom: fail returned");  // R.fail always throws
}

/// Math.max(a, b) (NaN wins).
double js_max(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::nan("");
  }
  return a > b ? a : b;
}

/// x?.[i] (undefined past the end or when x is not an array).
Value at(const Value& x, std::size_t i) {
  if (!x.is_array() || i >= x.as_array().size()) {
    return {};
  }
  return x.as_array()[i];
}

V3 unit(const V3& v, const Resolvers& R, const std::string& what) {
  const double l = V::len(v);
  if (!(l > kEps)) {
    fail(R, what);
  }
  return V::mul(v, 1 / l);
}

V3 project(const V::Frame& frame, const V3& p) {
  return V::sub(p, V::mul(frame.n, V::dot(V::sub(p, frame.o), frame.n)));
}

Geom plane_of(const V3& n, const V3& o, const V3& center) {
  Geom g;
  g.kind = "plane";
  g.frame = V::frame_for(n, o);
  g.center = center;
  return g;
}

struct Line {
  V3 o{}, d{};
  std::optional<std::pair<V3, V3>> ab;
};

struct Ctx {
  const Value& f;          // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives for one call
  const Resolvers& R;      // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives for one call
  const i18n::Catalog& w;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives for one call

  [[nodiscard]] std::string t(std::string_view key) const { return w.t(key); }
  [[noreturn]] void no(std::string_view key) const { fail(R, w.t(key)); }

  [[nodiscard]] V3 P(std::size_t i) const {
    const Value r = at(f.get("points"), i);
    if (!r.truthy()) {
      fail(R, w.t("model.refgeom.pick_point", {{"n", std::to_string(i + 1)}}));
    }
    return R.point(r);
  }
  [[nodiscard]] Plane PL(std::size_t i) const {
    const Value r = at(f.get("planes"), i);
    if (!r.truthy()) {
      no(i != 0 ? "model.refgeom.pick_second_plane" : "model.refgeom.pick_plane");
    }
    return R.plane(r);
  }
  [[nodiscard]] double num(std::string_view k) const { return base::js_to_number(f.get(k)); }
  [[nodiscard]] V3 flip(const V3& n) const { return f.get("flip").truthy() ? V::mul(n, -1) : n; }

  /// lineOf: a straight edge or a direction reference as a line.
  [[nodiscard]] Line line_of(std::string_view what) const {
    if (f.get("edge").truthy()) {
      const Edge e = R.edge(f.get("edge"));
      if (e.type != "line") {
        no("model.refgeom.edge_curved_line");
      }
      return {e.a, unit(V::sub(e.b, e.a), R, t("model.refgeom.edge_no_length")), std::pair{e.a, e.b}};
    }
    if (f.get("axis").truthy()) {
      const Dir ax = R.dir(f.get("axis"));
      return {ax.o, V::norm(ax.d), std::nullopt};
    }
    no(what);
  }
  /// midOf: half way along a straight edge, or between points 1 and 2: {p, t}.
  [[nodiscard]] std::pair<V3, V3> mid_of() const {
    if (f.get("edge").truthy()) {
      const Edge e = R.edge(f.get("edge"));
      if (e.type == "line") {
        return {V::mul(V::add(e.a, e.b), 0.5), V::sub(e.b, e.a)};
      }
      no("model.refgeom.mid_curved_exact");
    }
    if (!at(f.get("points"), 0).truthy() || !at(f.get("points"), 1).truthy()) {
      no("model.refgeom.pick_mid");
    }
    const V3 a = P(0);
    const V3 b = P(1);
    return {V::mul(V::add(a, b), 0.5), V::sub(b, a)};
  }
  /// meet: where a line or a curved edge meets a plane, a flat face or a cylinder.
  [[nodiscard]] V3 meet() const {
    Face surf;
    if (f.get("face").truthy()) {
      surf = R.face(f.get("face"));
    } else if (at(f.get("planes"), 0).truthy()) {
      const Plane pl = R.plane(at(f.get("planes"), 0));
      surf.type = "plane";
      surf.n = pl.frame.n;
      surf.o = pl.frame.o;
    } else {
      no("model.refgeom.pick_meet_surface");
    }
    if (f.get("edge").truthy()) {
      const Edge curved = R.edge(f.get("edge"));
      if (curved.type != "line") {
        if (surf.type != "plane") {
          no("model.refgeom.curved_curved");
        }
        no("model.refgeom.curved_cross_exact");
      }
    }
    const Line ln = line_of(t("model.refgeom.pick_meet_line"));
    const V3 nan3{std::nan(""), std::nan(""), std::nan("")};
    if (surf.type == "plane") {
      const V3 sn = surf.n.value_or(nan3);
      const V3 so = surf.o.value_or(nan3);
      const double dn = V::dot(ln.d, sn);
      if (std::abs(dn) < 1e-12) {
        no("model.refgeom.line_parallel_plane");
      }
      return V::add(ln.o, V::mul(ln.d, V::dot(V::sub(so, ln.o), sn) / dn));
    }
    if (surf.type == "cylinder" && surf.axis) {
      const V3 a = V::norm(surf.axis->d);
      const V3 w0 = V::sub(ln.o, surf.axis->o);
      const V3 dp = V::sub(ln.d, V::mul(a, V::dot(ln.d, a)));
      const V3 wp = V::sub(w0, V::mul(a, V::dot(w0, a)));
      const double A = V::dot(dp, dp);
      const double B = 2 * V::dot(dp, wp);
      const double C = V::dot(wp, wp) - surf.r * surf.r;
      if (A < 1e-18) {
        no("model.refgeom.line_along_axis");
      }
      const double disc = B * B - 4 * A * C;
      if (disc < 0) {
        no("model.refgeom.line_misses_cyl");
      }
      const double q = std::sqrt(disc);
      const double t0 = ln.ab ? V::dot(V::sub(V::mul(V::add(ln.ab->first, ln.ab->second), 0.5), ln.o), ln.d) : 0;
      std::array<double, 2> ts{(-B - q) / (2 * A), (-B + q) / (2 * A)};
      // [a, b].sort(cmp): V8's insertion sort swaps when cmp(a, b) > 0
      if (std::abs(ts[0] - t0) - std::abs(ts[1] - t0) > 0) {
        std::swap(ts[0], ts[1]);
      }
      return V::add(ln.o, V::mul(ln.d, ts[f.get("flip").truthy() ? 1 : 0]));
    }
    no("model.refgeom.meet_other_curved");
  }

  [[nodiscard]] Geom point() const {
    Geom g;
    g.kind = "point";
    const Value mode = f.get("mode");
    if (mode == Value("xyz")) {
      const Value p = f.get("p");
      if (!p.is_array()) {
        throw std::runtime_error("f.p is not iterable");
      }
      g.p = V::v3(p);
    } else if (mode == Value("at")) {
      g.p = P(0);
    } else if (mode == Value("mid")) {
      g.p = mid_of().first;
    } else if (mode == Value("meet")) {
      g.p = meet();
    } else if (mode == Value("planes3")) {
      const Plane a = PL(0);
      const Plane b = PL(1);
      const Value r2 = at(f.get("planes"), 2);
      if (!r2.truthy()) {
        no("model.refgeom.pick_third_plane");
      }
      const Plane c = R.plane(r2);
      const auto& an = a.frame.n;
      const auto& bn = b.frame.n;
      const auto& cn = c.frame.n;
      const auto p = V::solve_dense({{an[0], an[1], an[2]}, {bn[0], bn[1], bn[2]}, {cn[0], cn[1], cn[2]}},
                                    {V::dot(an, a.frame.o), V::dot(bn, b.frame.o), V::dot(cn, c.frame.o)});
      const double det = V::dot(an, V::cross(bn, cn));
      if (!p || std::abs(det) < 1e-9) {
        no("model.refgeom.planes3_no_point");
      }
      g.p = {(*p)[0], (*p)[1], (*p)[2]};
    } else {
      if (!f.get("edge").truthy()) {
        no("model.refgeom.pick_circular_edge");
      }
      const Edge e = R.edge(f.get("edge"));
      if (e.type != "circle" || !e.axis) {
        no("model.refgeom.centre_needs_circle");
      }
      g.p = e.axis->o;
    }
    return g;
  }

  [[nodiscard]] Geom axis() const {
    const Value mode = f.get("mode");
    V3 o{};
    V3 d{};
    if (mode == Value("2pt")) {
      o = P(0);
      d = unit(V::sub(P(1), o), R, t("model.refgeom.same_point"));
    } else if (mode == Value("cyl")) {
      if (!f.get("face").truthy()) {
        no("model.refgeom.pick_cyl_cone_face");
      }
      const Face fc = R.face(f.get("face"));
      if (!fc.axis) {
        no("model.refgeom.face_no_axis");
      }
      o = fc.axis->o;
      d = fc.axis->d;
    } else if (mode == Value("planes")) {
      const Plane a = PL(0);
      const Plane b = PL(1);
      d = V::cross(a.frame.n, b.frame.n);
      if (V::len(d) < 1e-9) {
        no("model.refgeom.planes_parallel");
      }
      d = V::norm(d);
      const double ca = V::dot(a.frame.n, a.frame.o);
      const double cb = V::dot(b.frame.n, b.frame.o);
      const double cd = V::dot(d, V::mul(V::add(a.frame.o, b.frame.o), 0.5));
      const auto& an = a.frame.n;
      const auto& bn = b.frame.n;
      const auto s = V::solve_dense({{an[0], an[1], an[2]}, {bn[0], bn[1], bn[2]}, {d[0], d[1], d[2]}}, {ca, cb, cd});
      if (!s) {
        no("model.refgeom.planes_no_line");
      }
      o = {(*s)[0], (*s)[1], (*s)[2]};
    } else if (mode == Value("edge")) {
      if (!f.get("edge").truthy()) {
        no("model.refgeom.pick_edge_or_circle");
      }
      const Edge e = R.edge(f.get("edge"));
      if (e.type == "line") {
        o = e.a;
        d = unit(V::sub(e.b, e.a), R, t("model.refgeom.edge_no_length"));
      } else if (e.type == "circle" && e.axis) {
        o = e.axis->o;
        d = e.axis->d;
      } else {
        no("model.refgeom.edge_curved_axis");
      }
    } else if (mode == Value("normal")) {
      const Plane pl = PL(0);
      o = P(0);
      d = pl.frame.n;
    } else {
      no("model.refgeom.unknown_axis");
    }
    Geom g;
    g.kind = "axis";
    g.o = o;
    g.d = V::norm(d);
    return g;
  }

  [[nodiscard]] Geom plane() const {
    const Value mode = f.get("mode");
    if (mode == Value("offset")) {
      const Plane b = PL(0);
      const V3 n = b.frame.n;
      const double dd = num("d");
      const V3 o = V::add(b.frame.o, V::mul(n, dd));
      return plane_of(flip(n), o, V::add(b.center.value_or(b.frame.o), V::mul(n, dd)));
    }
    if (mode == Value("angle")) {
      const Plane b = PL(0);
      if (!f.get("axis").truthy()) {
        no("model.refgeom.pick_turn_axis");
      }
      const Dir ax = R.dir(f.get("axis"));
      const V3 u = V::norm(ax.d);
      const V3 perp = V::sub(b.frame.n, V::mul(u, V::dot(b.frame.n, u)));
      if (V::len(perp) < 1e-9) {
        no("model.refgeom.axis_square");
      }
      const V3 n0 = V::norm(perp);
      const V3 n = rotate(n0, u, num("angle") * std::numbers::pi / 180);
      return plane_of(flip(n), ax.o, ax.o);
    }
    if (mode == Value("3pt")) {
      const V3 a = P(0);
      const V3 b = P(1);
      const V3 c = P(2);
      const V3 n = V::cross(V::sub(b, a), V::sub(c, a));
      if (V::len(n) < 1e-9 * js_max(1, V::len(V::sub(b, a)) * V::len(V::sub(c, a)))) {
        no("model.refgeom.points_on_line");
      }
      return plane_of(flip(V::norm(n)), a, V::mul(V::add(V::add(a, b), c), 1.0 / 3));
    }
    if (mode == Value("mid")) {
      const Plane a = PL(0);
      const Plane b = PL(1);
      const V3 diff = V::sub(a.frame.n, b.frame.n);
      V3 n{};
      V3 o{};
      if (V::len(diff) > 1e-9) {
        const double L = V::len(diff);
        n = V::mul(diff, 1 / L);
        const double c = (V::dot(a.frame.o, a.frame.n) - V::dot(b.frame.o, b.frame.n)) / L;
        o = V::mul(n, c);
      } else {
        n = a.frame.n;
        o = V::add(a.frame.o, V::mul(n, V::dot(V::sub(b.frame.o, a.frame.o), n) / 2));
      }
      const V3 ca = a.center.value_or(a.frame.o);
      const V3 cb = b.center.value_or(b.frame.o);
      const V3 mid = V::mul(V::add(ca, cb), 0.5);
      const V::Frame fr = V::frame_for(n, o);
      return plane_of(flip(n), o, project(fr, mid));
    }
    if (mode == Value("tangent")) {
      if (!f.get("face").truthy()) {
        no("model.refgeom.pick_cyl_face");
      }
      const Face fc = R.face(f.get("face"));
      if (fc.type != "cylinder" || !fc.axis) {
        no("model.refgeom.tangent_needs_cyl");
      }
      const V3 p = P(0);
      const V3 d = V::norm(fc.axis->d);
      const V3 w0 = V::sub(p, fc.axis->o);
      const V3 radial = V::sub(w0, V::mul(d, V::dot(w0, d)));
      if (V::len(radial) < 1e-9) {
        no("model.refgeom.point_on_axis");
      }
      const V3 n = V::norm(radial);
      const V3 foot = V::add(fc.axis->o, V::mul(d, V::dot(w0, d)));
      const V3 o = V::add(foot, V::mul(n, fc.r));
      return plane_of(flip(n), o, o);
    }
    if (mode == Value("edgept")) {
      const Line ln = line_of(t("model.refgeom.pick_line_through"));
      const V3 p = P(0);
      const V3 n = V::cross(ln.d, V::sub(p, ln.o));
      if (V::len(n) < 1e-9 * js_max(1, V::len(V::sub(p, ln.o)))) {
        no("model.refgeom.point_on_line");
      }
      return plane_of(flip(V::norm(n)), ln.o, V::add(ln.o, V::mul(V::sub(p, ln.o), 0.5)));
    }
    if (mode == Value("midnormal")) {
      const auto [mp, mt] = mid_of();
      if (V::len(mt) < 1e-12) {  // `!m.t ||`: t is always an array here
        no("model.refgeom.same_point_no_dir");
      }
      return plane_of(flip(V::norm(mt)), mp, mp);
    }
    if (mode == Value("normal")) {
      const V3 p = P(0);
      V3 at_{};
      V3 tt{};
      if (f.get("edge").truthy()) {
        const Edge e = R.edge(f.get("edge"));
        if (e.type == "line") {
          const V3 u = unit(V::sub(e.b, e.a), R, t("model.refgeom.edge_no_length"));
          tt = u;
          at_ = V::add(e.a, V::mul(u, V::dot(V::sub(p, e.a), u)));
        } else {
          no("model.refgeom.normal_curved_exact");
        }
      } else if (f.get("axis").truthy()) {
        const Dir ax = R.dir(f.get("axis"));
        tt = V::norm(ax.d);
        at_ = V::add(ax.o, V::mul(tt, V::dot(V::sub(p, ax.o), tt)));
      } else {
        no("model.refgeom.pick_normal_edge");
      }
      return plane_of(flip(V::norm(tt)), at_, at_);
    }
    no("model.refgeom.unknown_plane");
  }
};

}  // namespace

V3 rotate(const V3& v, const V3& u, double a) {
  const double c = base::js_cos(a);
  const double s = base::js_sin(a);
  return V::add(V::add(V::mul(v, c), V::mul(V::cross(u, v), s)), V::mul(u, V::dot(u, v) * (1 - c)));
}

Geom ref_feature(const Value& f, const Resolvers& R, const i18n::Catalog& words) {
  const Ctx c{f, R, words};
  const Value type = f.get("type");
  if (type == Value("point")) {
    return c.point();
  }
  if (type == Value("axis")) {
    return c.axis();
  }
  return c.plane();
}

Value to_value(const Geom& g) {
  if (g.kind == "point") {
    return base::make_object({{"kind", "point"}, {"p", V::value(g.p)}});
  }
  if (g.kind == "axis") {
    return base::make_object({{"kind", "axis"}, {"o", V::value(g.o)}, {"d", V::value(g.d)}});
  }
  return base::make_object({{"kind", "plane"}, {"frame", V::value(g.frame)}, {"center", V::value(g.center)}});
}

Value plane_quad(const V::Frame& frame, const V3& center, double s) {
  const auto c = V::to_plane(frame, center);
  base::Array out;
  for (const auto& [u, v] : std::array<std::pair<double, double>, 4>{{{-s, -s}, {s, -s}, {s, s}, {-s, s}}}) {
    out.push_back(V::value(V::to_world(frame, c[0] + u, c[1] + v)));
  }
  return out;
}

}  // namespace cc::partcraft::refgeom
