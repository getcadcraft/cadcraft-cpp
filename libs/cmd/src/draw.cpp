// SPDX-License-Identifier: Apache-2.0
// commands.js's draw commands (CadCraft 0.5) in C++: LINE, PLINE, CIRCLE, XLINE, RAY, ARC, RECTANG, POLYGON, TEXT —
// the same prompts, options and entities, in the same order.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/tancircle.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

constexpr double kEps = 1e-9;

Ask with_base(std::optional<Pt> base) {
  Ask a;
  a.base = base;
  return a;
}

/// geom.js unitDir(dx, dy) → a unit direction, or nothing for a zero one
std::optional<Pt> unit_dir(double dx, double dy) {
  const double L = base::js_hypot(dx, dy);
  if (L > kEps) {
    return Pt{dx / L, dy / L};
  }
  return std::nullopt;
}

/// commands.js circle3(a, b, c)
std::optional<std::array<double, 3>> circle3(Pt a, Pt b, Pt c) {
  const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
  if (std::abs(d) < 1e-12) {
    return std::nullopt;
  }
  const double a2 = a.x * a.x + a.y * a.y;
  const double b2 = b.x * b.x + b.y * b.y;
  const double c2 = c.x * c.x + c.y * c.y;
  const double cx = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
  const double cy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
  return std::array<double, 3>{cx, cy, base::js_hypot(a.x - cx, a.y - cy)};
}
Value circle_e(double cx, double cy, double r) {
  return base::make_object({{"type", "circle"}, {"cx", cx}, {"cy", cy}, {"r", r}});
}
/// commands.js arc3(s, m, e): the arc through start, a point on it and end
std::optional<Value> arc3(Pt s, Pt m, Pt e) {
  const auto k = circle3(s, m, e);
  if (!k) {
    return std::nullopt;
  }
  const Pt c{(*k)[0], (*k)[1]};
  const bool ccw = (m.x - s.x) * (e.y - s.y) - (m.y - s.y) * (e.x - s.x) > 0;  // a left turn s→m→e travels CCW
  double a0 = angle_of(c, s);
  double a1 = angle_of(c, e);
  if (!ccw) {
    std::swap(a0, a1);
  }
  return base::make_object({{"type", "arc"},
                            {"cx", (*k)[0]},
                            {"cy", (*k)[1]},
                            {"r", (*k)[2]},
                            {"a0", norm_angle(a0)},
                            {"a1", norm_angle(a1)}});
}

/// The answer's point (only a point answer has one).
Pt P(const Answer& a) {
  return a.p;
}

// ---- LINE

Task<void> line_cmd(Ctx& c) {
  Ask a0;
  a0.none = true;
  Answer r = co_await c.point(c.t("cmd.mea.p1"), std::move(a0));
  Pt first;
  if (!r.truthy()) {
    if (!c.engine.last_point) {
      co_return;
    }
    first = *c.engine.last_point;
  } else {
    first = r.p;
  }
  const Pt start = first;
  Pt prev = first;
  std::vector<Value> made;
  for (;;) {
    c.preview([&prev](Pt cur) { return base::Array{line_e(prev.x, prev.y, cur.x, cur.y)}; });
    Ask a;
    a.base = prev;
    a.none = true;
    a.options.push_back(c.opt("U", "Undo"));
    if (made.size() >= 2) {
      a.options.push_back(c.opt("C", "Close"));
    }
    const Answer p = co_await c.point(c.t("cmd.line.specify_next_point"), std::move(a));
    if (!p.truthy()) {
      co_return;
    }
    if (p.is("U")) {
      if (made.empty()) {
        c.print(c.t("cmd.line.all_segments_already_undone"));
        continue;
      }
      const Value last = made.back();
      made.pop_back();
      c.doc.change("LINE", [&] { c.doc.remove(id_of(last)); });
      prev = made.empty() ? start
                          : Pt{base::js_to_number(made.back().get("x2")), base::js_to_number(made.back().get("y2"))};
      c.engine.last_point = prev;
      continue;
    }
    if (p.is("C")) {
      c.add(c.ent(line_e(prev.x, prev.y, start.x, start.y)));
      c.engine.last_point = start;
      co_return;
    }
    if (p.kind != Answer::Kind::point) {
      continue;
    }
    if (dist(prev, p.p) < 1e-12) {
      continue;
    }
    made.push_back(c.add(c.ent(line_e(prev.x, prev.y, p.p.x, p.p.y))));
    prev = p.p;
  }
}

// ---- PLINE

struct PlVertex {
  double x = 0, y = 0, b = 0;
};

/// geom.js bulgeArc(p, q, b): the arc's centre (the one thing PLINE's tangent needs)
Pt bulge_centre(PlVertex p, PlVertex q, double b) {
  const double theta = 4 * base::js_atan(b);
  const double c = base::js_hypot(q.x - p.x, q.y - p.y);
  const double r = std::abs(c / (2 * base::js_sin(theta / 2)));
  const double mx = (p.x + q.x) / 2;
  const double my = (p.y + q.y) / 2;
  const double h = r * base::js_cos(theta / 2);
  const double ux = (q.x - p.x) / c;
  const double uy = (q.y - p.y) / c;
  const double s = b > 0 ? 1 : -1;
  return {mx - uy * h * s, my + ux * h * s};
}

Value poly_e(const std::vector<PlVertex>& pts, bool closed) {
  base::Array a;
  a.reserve(pts.size());
  for (const PlVertex& v : pts) {
    a.push_back(vtx(v.x, v.y, v.b));
  }
  return base::make_object({{"type", "poly"}, {"closed", closed}, {"pts", std::move(a)}});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's PLINE
Task<void> pline_cmd(Ctx& c) {
  const Answer s0 = co_await c.point(c.t("cmd.pline.specify_start_point"));
  std::vector<PlVertex> pts = {{s0.p.x, s0.p.y, 0}};
  bool arc_mode = false;
  bool closed = false;
  // direction at the end of the last segment
  const auto tangent = [&pts]() -> std::optional<double> {
    if (pts.size() < 2) {
      return std::nullopt;
    }
    const PlVertex p = pts[pts.size() - 2];
    const PlVertex q = pts.back();
    if (p.b == 0) {
      return angle_of({p.x, p.y}, {q.x, q.y});
    }
    const Pt ce = bulge_centre(p, q, p.b);
    const double ang = base::js_atan2(q.y - ce.y, q.x - ce.x);
    return ang + (p.b > 0 ? kPi / 2 : -kPi / 2);
  };
  const auto arc_bulge = [&](PlVertex p, Pt q) {
    const double t = tangent().value_or(0);
    double phi = norm_angle(angle_of({p.x, p.y}, q) - t);
    if (phi > kPi) {
      phi -= kTau;
    }
    return std::tan(phi / 2);
  };
  for (;;) {
    const PlVertex last = pts.back();
    c.preview([&, last](Pt cur) {
      std::vector<PlVertex> v(pts.begin(), pts.end() - 1);
      PlVertex l = pts.back();
      l.b = arc_mode ? arc_bulge(last, cur) : 0;
      v.push_back(l);
      v.push_back({cur.x, cur.y, 0});
      return base::Array{poly_e(v, false)};
    });
    Ask a;
    a.base = Pt{last.x, last.y};
    a.none = true;
    if (!arc_mode) {
      a.options.push_back(c.opt("A", "Arc"));
      if (pts.size() > 2) {
        a.options.push_back(c.opt("C", "Close"));
      }
    } else {
      a.options.push_back(c.opt("L", "Line"));
      if (pts.size() > 2) {
        a.options.push_back(c.opt("CL", "CLose"));
      }
    }
    a.options.push_back(c.opt("U", "Undo"));
    const Answer p = co_await c.point(
        !arc_mode ? c.t("cmd.line.specify_next_point") : c.t("cmd.pline.specify_endpoint_arc"), std::move(a));
    if (!p.truthy()) {
      break;
    }
    if (p.is("A")) {
      arc_mode = true;
      continue;
    }
    if (p.is("L")) {
      arc_mode = false;
      continue;
    }
    if (p.is("U")) {
      if (pts.size() > 1) {
        pts.pop_back();
        pts.back().b = 0;
        c.engine.last_point = Pt{pts.back().x, pts.back().y};
      }
      continue;
    }
    if (p.is("C") || p.is("CL")) {
      if (arc_mode) {
        pts.back().b = arc_bulge(pts.back(), {pts[0].x, pts[0].y});
      }
      closed = true;
      break;
    }
    if (p.kind != Answer::Kind::point) {
      continue;
    }
    if (dist({last.x, last.y}, p.p) < 1e-12) {
      continue;
    }
    pts.back().b = arc_mode ? arc_bulge(pts.back(), p.p) : 0;
    pts.push_back({p.p.x, p.p.y, 0});
  }
  if (pts.size() >= 2) {
    c.add(c.ent(base::make_object({{"type", "poly"}, {"pts", poly_e(pts, false).get("pts")}, {"closed", closed}})));
  }
}

// ---- CIRCLE

/// pickTangent(c, prompt): an object a circle can touch (a line, a circle, an arc …) and where it was picked
Task<std::pair<cad::Curve, Pt>> pick_tangent(Ctx& c, std::string prompt) {
  for (;;) {
    const Answer pick = co_await c.entity(prompt);
    const Value* e = c.doc.entities().get(pick.id);
    const std::optional<cad::Curve> o =
        e != nullptr
            ? cad::curve_at(*e, {pick.p.x, pick.p.y}, [&c](const std::string& n) { return c.doc.blocks().get(n); })
            : std::nullopt;
    if (o) {
      co_return std::pair{*o, pick.p};
    }
    c.print(c.t("cmd.circle.tan.not_curve"));
  }
}
Value circle_of(const cad::Circle& k) {
  return circle_e(k.cx, k.cy, k.r);
}
/// CIRCLE Ttr / TTR (0.5 DR.1): tangent to two objects, with a radius
Task<void> circle_ttr_cmd(Ctx& c) {
  const auto [ao, at] = co_await pick_tangent(c, c.t("cmd.circle.tan.first"));
  const auto [bo, bt] = co_await pick_tangent(c, c.t("cmd.circle.tan.second"));
  c.preview([ao, at, bo, bt](Pt cur) {
    const auto k = cad::circle_ttr(ao, {at.x, at.y}, bo, {bt.x, bt.y}, std::max(dist(at, cur), 1e-9));
    return k ? base::Array{circle_of(*k)} : base::Array{};
  });
  Ask ar;
  ar.base = at;
  ar.def = c.engine.state.get("lastRadius");
  const Answer ra = co_await c.distance(c.t("cmd.circle.specify_radius_circle"), std::move(ar));
  const double r = ra.kind == Answer::Kind::number ? ra.number : base::js_to_number(ra.typed);
  if (!(r > 0)) {
    co_return;
  }
  const auto k = cad::circle_ttr(ao, {at.x, at.y}, bo, {bt.x, bt.y}, r);
  if (!k) {
    c.print(c.t("cmd.circle.no_circle"));
    co_return;
  }
  c.engine.state.set("lastRadius", r);
  c.add(c.ent(circle_of(*k)));
}
/// CIRCLE TTT (0.5 DR.2): tangent to three objects
Task<void> circle_ttt_cmd(Ctx& c) {
  const auto [ao, at] = co_await pick_tangent(c, c.t("cmd.circle.tan.first"));
  const auto [bo, bt] = co_await pick_tangent(c, c.t("cmd.circle.tan.second"));
  const auto [co, ct] = co_await pick_tangent(c, c.t("cmd.circle.tan.third"));
  const auto k = cad::circle_ttt(ao, {at.x, at.y}, bo, {bt.x, bt.y}, co, {ct.x, ct.y});
  if (!k) {
    c.print(c.t("cmd.circle.no_circle"));
    co_return;
  }
  c.add(c.ent(circle_of(*k)));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's CIRCLE
Task<void> circle_cmd(Ctx& c) {
  Ask a0;
  a0.options = {c.opt("3P", "3P"),
                c.opt("2P", "2P"),
                c.opt("T", "Ttr (tan tan radius)"),
                {.key = "TTT", .label = "TTT", .en = {}, .hidden = true}};
  const Answer r0 = co_await c.point(c.t("cmd.circle.specify_center_point_circle"), std::move(a0));
  if (r0.is("T")) {
    co_await circle_ttr_cmd(c);
    co_return;
  }
  if (r0.is("TTT")) {
    co_await circle_ttt_cmd(c);
    co_return;
  }
  if (r0.is("2P")) {
    const Pt a = P(co_await c.point(c.t("cmd.circle.specify_first_end_point")));
    c.preview([a](Pt cur) { return base::Array{circle_e((a.x + cur.x) / 2, (a.y + cur.y) / 2, dist(a, cur) / 2)}; });
    const Pt b = P(co_await c.point(c.t("cmd.circle.specify_second_end_point"), with_base(a)));
    c.add(c.ent(circle_e((a.x + b.x) / 2, (a.y + b.y) / 2, dist(a, b) / 2)));
    co_return;
  }
  if (r0.is("3P")) {
    const Pt a = P(co_await c.point(c.t("cmd.circle.specify_first_point_circle")));
    const Pt b = P(co_await c.point(c.t("cmd.circle.specify_second_point_circle"), with_base(a)));
    c.preview([a, b](Pt cur) {
      const auto k = circle3(a, b, cur);
      return k ? base::Array{circle_e((*k)[0], (*k)[1], (*k)[2])} : base::Array{};
    });
    const Pt d = P(co_await c.point(c.t("cmd.circle.specify_third_point_circle"), with_base(b)));
    const auto k = circle3(a, b, d);
    if (!k) {
      c.print(c.t("cmd.circle.three_points_line"));
      co_return;
    }
    c.add(c.ent(circle_e((*k)[0], (*k)[1], (*k)[2])));
    co_return;
  }
  const Pt center = r0.p;
  bool diameter = false;
  for (;;) {
    c.preview([center, &diameter](Pt cur) {
      return base::Array{circle_e(center.x, center.y, std::max(dist(center, cur) / (diameter ? 2 : 1), 1e-9))};
    });
    Ask a;
    a.base = center;
    if (!diameter) {
      a.options.push_back(c.opt("D", "Diameter"));
    }
    a.def = c.engine.state.get("lastRadius");
    const Answer r = co_await c.distance(
        diameter ? c.t("cmd.circle.specify_diameter_circle") : c.t("cmd.circle.specify_radius_circle"), std::move(a));
    if (r.is("D")) {
      diameter = true;
      continue;
    }
    if (!r.truthy()) {
      co_return;
    }
    const double radius = diameter ? r.number / 2 : r.number;
    c.engine.state.set("lastRadius", radius);
    c.add(c.ent(circle_e(center.x, center.y, radius)));
    co_return;
  }
}

// ---- XLINE and RAY (0.5 DR.6)

Value xl(std::string_view type, double x, double y, Pt u) {
  return base::make_object({{"type", type}, {"x", x}, {"y", y}, {"dx", u.x}, {"dy", u.y}});
}

/// The direction of a straight object at the pick (a line, a construction line, or the polyline segment nearest it).
std::optional<Pt> straight_dir(const Value* e, Pt at) {
  if (e == nullptr) {
    return std::nullopt;
  }
  const std::string type = base::js_string(e->get("type"));
  const auto N = [&](std::string_view k) { return base::js_to_number(e->get(k)); };
  if (type == "line") {
    return unit_dir(N("x2") - N("x1"), N("y2") - N("y1"));
  }
  if (type == "xline" || type == "ray") {
    return Pt{N("dx"), N("dy")};
  }
  if (type == "poly") {
    // prims(e): the straight segments (a bulged one is an arc); the one nearest the pick
    const Value pv = e->get("pts");
    if (!pv.is_array()) {
      return std::nullopt;
    }
    const base::Array& v = pv.as_array();
    const std::size_t n = v.size();
    const bool closed = e->get("closed").truthy();
    std::size_t segs = n;
    if (!closed) {
      segs = n == 0 ? 0 : n - 1;
    }
    double bd = std::numeric_limits<double>::infinity();
    std::optional<std::array<double, 4>> best;
    for (std::size_t i = 0; i < segs; ++i) {
      const Pt p = pt_of(v[i]);
      const Pt q = pt_of(v[(i + 1) % n]);
      const Value b = v[i].get("b");
      if (std::abs(b.truthy() ? base::js_to_number(b) : 0) > 1e-12 && dist(p, q) > kEps) {
        continue;
      }
      const double dx = q.x - p.x;
      const double dy = q.y - p.y;
      const double L = dx * dx + dy * dy;
      const double t = L != 0 ? std::max(0.0, std::min(1.0, ((at.x - p.x) * dx + (at.y - p.y) * dy) / L)) : 0;
      const double d = dist(at, {p.x + dx * t, p.y + dy * t});
      if (d < bd) {
        bd = d;
        best = std::array<double, 4>{p.x, p.y, q.x, q.y};
      }
    }
    if (best) {
      return unit_dir((*best)[2] - (*best)[0], (*best)[3] - (*best)[1]);
    }
  }
  return std::nullopt;
}

struct Straight {
  Pt u;
  Pt at;
  Id id = 0;
};
Task<Straight> pick_straight(Ctx& c, std::string prompt) {
  for (;;) {
    const Answer pick = co_await c.entity(prompt);
    const Value* e = c.doc.entities().get(pick.id);
    if (const auto u = straight_dir(e, pick.p)) {
      co_return Straight{.u = *u, .at = pick.p, .id = pick.id};
    }
    c.print(c.t("cmd.xline.not_straight"));
  }
}

/// Through points until Enter, each one more construction line of `type`.
Task<void> through_points(Ctx& c, std::string /*type*/, std::function<std::optional<Value>(Pt)> make) {
  for (;;) {
    c.preview([make](Pt cur) {
      const auto x = make(cur);
      return x ? base::Array{*x} : base::Array{};
    });
    Ask a;
    a.none = true;
    const Answer p = co_await c.point(c.t("cmd.xline.through"), std::move(a));
    if (!p.truthy() || p.kind != Answer::Kind::point) {
      co_return;
    }
    if (const auto x = make(p.p)) {
      c.add(c.ent(*x));
    } else {
      c.print(c.t("cmd.xline.same_point"));
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's XLINE
Task<void> xline_cmd(Ctx& c) {
  Ask a0;
  a0.options = {c.opt("H", "Hor"), c.opt("V", "Ver"), c.opt("A", "Ang"), c.opt("B", "Bisect"), c.opt("O", "Offset")};
  const Answer first = co_await c.point(c.t("cmd.xline.first"), std::move(a0));
  const auto fixed = [](Pt u) { return [u](Pt p) -> std::optional<Value> { return xl("xline", p.x, p.y, u); }; };
  if (first.kind != Answer::Kind::option) {
    const Pt f = first.p;
    co_await through_points(c, "xline", [f](Pt p) -> std::optional<Value> {
      const auto u = unit_dir(p.x - f.x, p.y - f.y);
      return u ? std::optional<Value>(xl("xline", f.x, f.y, *u)) : std::nullopt;
    });
    co_return;
  }
  if (first.is("H")) {
    co_await through_points(c, "xline", fixed({1, 0}));
    co_return;
  }
  if (first.is("V")) {
    co_await through_points(c, "xline", fixed({0, 1}));
    co_return;
  }
  if (first.is("A")) {
    Ask a;
    a.options = {c.opt("R", "Reference")};
    a.def = c.doc.in_model() && c.doc.ucs.truthy() ? Value(base::js_to_number(c.doc.ucs.get("ang"))) : Value(0);
    a.ucs = true;
    const Answer an = co_await c.angle(c.t("cmd.xline.angle"), std::move(a));
    double ang = an.kind == Answer::Kind::number ? an.number : 0;
    if (an.is("R")) {
      const Straight s = co_await pick_straight(c, c.t("cmd.xline.ref_line"));
      Ask b;
      b.def = 0;
      const Answer r = co_await c.angle(c.t("cmd.xline.angle"), std::move(b));
      ang = base::js_atan2(s.u.y, s.u.x) + (r.kind == Answer::Kind::number ? r.number : 0);
    }
    co_await through_points(c, "xline", fixed({base::js_cos(ang), base::js_sin(ang)}));
    co_return;
  }
  if (first.is("B")) {
    const Pt v = P(co_await c.point(c.t("cmd.xline.vertex")));
    const Pt s = P(co_await c.point(c.t("cmd.xline.start"), with_base(v)));
    const auto u0 = unit_dir(s.x - v.x, s.y - v.y);
    if (!u0) {
      c.print(c.t("cmd.xline.same_point"));
      co_return;
    }
    const auto half = [v, u0](Pt p) -> std::optional<Value> {
      const auto u1 = unit_dir(p.x - v.x, p.y - v.y);
      if (!u1) {
        return std::nullopt;
      }
      const Pt b = unit_dir(u0->x + u1->x, u0->y + u1->y).value_or(Pt{-u0->y, u0->x});  // opposite rays: perpendicular
      return xl("xline", v.x, v.y, b);
    };
    for (;;) {
      c.preview([half](Pt cur) {
        const auto x = half(cur);
        return x ? base::Array{*x} : base::Array{};
      });
      Ask a;
      a.base = v;
      a.none = true;
      const Answer p = co_await c.point(c.t("cmd.xline.end"), std::move(a));
      if (!p.truthy() || p.kind != Answer::Kind::point) {
        co_return;
      }
      if (const auto x = half(p.p)) {
        c.add(c.ent(*x));
      } else {
        c.print(c.t("cmd.xline.same_point"));
      }
    }
  }
  if (first.is("O")) {
    const Value d0 = c.engine.state.get("xlineOffset");
    Ask a;
    a.options = {c.opt("T", "Through")};
    a.def = d0.is_undefined() || d0.is_null() ? Value("Through") : d0;
    const Answer d = co_await c.distance(c.t("cmd.xline.offset"), std::move(a));
    const bool through = (d.kind == Answer::Kind::text && d.text == "Through") || d.is("T");
    if (!through) {
      c.engine.state.set("xlineOffset", d.kind == Answer::Kind::number ? Value(d.number) : Value());
    }
    const double dd = d.number;
    for (;;) {
      Ask pa;
      pa.none = true;
      const Answer pick = co_await c.entity(c.t("cmd.xline.offset_line"), std::move(pa));
      if (!pick.truthy()) {
        co_return;
      }
      const Value* e = c.doc.entities().get(pick.id);
      const auto u = straight_dir(e, pick.p);
      if (!u) {
        c.print(c.t("cmd.xline.not_straight"));
        continue;
      }
      const std::string type = base::js_string(e->get("type"));
      Pt on{base::js_to_number(e->get("x")), base::js_to_number(e->get("y"))};
      if (type == "line") {
        on = {base::js_to_number(e->get("x1")), base::js_to_number(e->get("y1"))};
      } else if (type == "poly") {
        on = pick.p;
      }
      // the picked line's own point nearest the pick, so the new line is parallel to the very segment picked
      const Pt uu = *u;
      const auto foot = [on, uu](Pt q) {
        const double k = (q.x - on.x) * uu.x + (q.y - on.y) * uu.y;
        return Pt{on.x + uu.x * k, on.y + uu.y * k};
      };
      if (through) {
        c.preview([uu](Pt cur) { return base::Array{xl("xline", cur.x, cur.y, uu)}; });
        const Pt p = P(co_await c.point(c.t("cmd.xline.through")));
        c.add(c.ent(xl("xline", p.x, p.y, uu)));
      } else {
        const Pt f = foot(pick.p);
        c.preview([f, foot, uu, dd](Pt cur) {
          const Pt g = foot(cur);
          const auto n = unit_dir(cur.x - g.x, cur.y - g.y);
          return n ? base::Array{xl("xline", f.x + n->x * dd, f.y + n->y * dd, uu)} : base::Array{};
        });
        const Pt side = P(co_await c.point(c.t("cmd.xline.side")));
        const Pt g = foot(side);
        const auto n = unit_dir(side.x - g.x, side.y - g.y);
        if (!n) {
          c.print(c.t("cmd.xline.side_on_line"));
          continue;
        }
        c.add(c.ent(xl("xline", f.x + n->x * dd, f.y + n->y * dd, uu)));
      }
    }
  }
}

Task<void> ray_cmd(Ctx& c) {
  const Pt a = P(co_await c.point(c.t("cmd.ray.start")));
  co_await through_points(c, "ray", [a](Pt p) -> std::optional<Value> {
    const auto u = unit_dir(p.x - a.x, p.y - a.y);
    return u ? std::optional<Value>(xl("ray", a.x, a.y, *u)) : std::nullopt;
  });
}

// ---- ARC, RECTANG, POLYGON

Value arc_e(Pt center, double r, double a0, double a1) {
  return base::make_object({{"type", "arc"}, {"cx", center.x}, {"cy", center.y}, {"r", r}, {"a0", a0}, {"a1", a1}});
}

Task<void> arc_cmd(Ctx& c) {
  Ask a0;
  a0.options = {c.opt("C", "Center")};
  const Answer p0 = co_await c.point(c.t("cmd.arc.specify_start_point_arc"), std::move(a0));
  if (p0.is("C")) {
    const Pt center = P(co_await c.point(c.t("cmd.arc.specify_center_point_arc")));
    const Pt s = P(co_await c.point(c.t("cmd.arc.specify_start_point_arc"), with_base(center)));
    const double r = dist(center, s);
    const double b0 = angle_of(center, s);
    c.preview([center, r, b0](Pt cur) { return base::Array{arc_e(center, r, b0, angle_of(center, cur))}; });
    const Pt e = P(co_await c.point(c.t("cmd.arc.specify_end_point_arc"), with_base(center)));
    c.add(c.ent(arc_e(center, r, norm_angle(b0), norm_angle(angle_of(center, e)))));
    co_return;
  }
  const Pt s0 = p0.p;
  const Pt p1 = P(co_await c.point(c.t("cmd.arc.specify_second_point_arc"), with_base(s0)));
  c.preview([s0, p1](Pt cur) {
    const auto a = arc3(s0, p1, cur);
    return a ? base::Array{*a} : base::Array{line_e(s0.x, s0.y, p1.x, p1.y)};
  });
  const Pt p2 = P(co_await c.point(c.t("cmd.arc.specify_end_point_arc"), with_base(p1)));
  const auto a = arc3(s0, p1, p2);
  if (!a) {
    c.print(c.t("cmd.circle.three_points_line"));
    co_return;
  }
  c.add(c.ent(*a));
}

Task<void> rectang_cmd(Ctx& c) {
  const Pt a = P(co_await c.point(c.t("cmd.area.first")));
  c.preview([a](Pt cur) { return base::Array{rect(a, cur)}; });
  const Pt b = P(co_await c.point(c.t("cmd.rectang.specify_other_corner_point"), with_base(a)));
  if (std::abs(b.x - a.x) < 1e-12 || std::abs(b.y - a.y) < 1e-12) {
    c.print(c.t("cmd.rectang.rectangle_needs_width_height"));
    co_return;
  }
  c.add(c.ent(rect(a, b)));
}

Task<void> polygon_cmd(Ctx& c) {
  Ask an;
  const Value ls = c.engine.state.get("lastSides");
  an.def = ls.truthy() ? ls : Value(4);
  const Answer n = co_await c.number(c.t("cmd.polygon.enter_number_sides"), std::move(an));
  const double sides_d = base::js_round(n.number);
  if (std::isnan(sides_d) || sides_d < 3 || sides_d > 1024) {
    c.print(c.t("cmd.polygon.requires_whole_number_3"));
    co_return;
  }
  const int sides = static_cast<int>(sides_d);
  c.engine.state.set("lastSides", sides);
  const Pt center = P(co_await c.point(c.t("cmd.polygon.specify_center_polygon")));
  Ask ak;
  ak.options = {c.opt("I", c.t("cmd.polygon.inscribed_circle")),
                c.opt("C", c.t("cmd.polygon.circumscribed_about_circle"))};
  const Value lm = c.engine.state.get("lastPolyMode");
  ak.def = lm.truthy() ? lm : Value("I");
  const Answer k = co_await c.keyword(c.t("cmd.mea.option"), std::move(ak));
  std::string mode = k.key();
  if (k.kind != Answer::Kind::option && k.kind != Answer::Kind::text) {
    mode = "I";
  }
  if (mode.empty()) {
    mode = "I";
  }
  c.engine.state.set("lastPolyMode", mode);
  const bool circ = mode == "C";
  const auto make = [center, sides, circ](Pt cur) {
    const double r = dist(center, cur);
    const double a = angle_of(center, cur);
    const double R = circ ? r / base::js_cos(kPi / sides) : r;
    const double b0 = circ ? a + kPi / sides : a;
    base::Array pts;
    for (int i = 0; i < sides; ++i) {
      const Pt q = polar(center, b0 + i * kTau / sides, R);
      pts.push_back(vtx(q.x, q.y, 0));
    }
    return base::make_object({{"type", "poly"}, {"closed", true}, {"pts", std::move(pts)}});
  };
  c.preview([make](Pt cur) { return base::Array{make(cur)}; });
  const Pt p = P(co_await c.point(c.t("cmd.circle.specify_radius_circle"), with_base(center)));
  c.add(c.ent(make(p)));
}

// ---- TEXT

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's TEXT
Task<void> text_cmd(Ctx& c) {
  Ask a0;
  a0.options = {c.opt("J", "Justify")};
  const Answer p = co_await c.point(c.t("cmd.attdef.at"), std::move(a0));
  std::string ha = "left";
  std::string va = "baseline";
  Pt at = p.p;
  if (p.is("J")) {
    Ask aj;
    aj.options = {c.opt("L", "Left"),   c.opt("C", "Center"), c.opt("R", "Right"),
                  c.opt("M", "Middle"), c.opt("TL", "TL"),    c.opt("MC", "MC")};
    aj.def = "L";
    const Answer j = co_await c.keyword(c.t("cmd.mea.option"), std::move(aj));
    const std::string k = j.kind == Answer::Kind::option && !j.option.empty() ? j.option : "L";
    if (k == "C" || k == "M" || k == "MC") {
      ha = "center";
    } else if (k == "R") {
      ha = "right";
    } else if (k == "L" || k == "TL") {
      ha = "left";
    } else {
      ha = "";  // { … }[k]: undefined
    }
    if (k == "M" || k == "MC") {
      va = "middle";
    } else if (k == "TL") {
      va = "top";
    }
    at = P(co_await c.point(c.t("cmd.text.specify_insertion_point_text")));
  }
  Ask ah;
  ah.base = at;
  ah.def = c.doc.current.get("textHeight");
  const Answer hh = co_await c.distance(c.t("cmd.mtext.height"), std::move(ah));
  if (!hh.truthy()) {
    co_return;
  }
  const double h = hh.number;
  c.doc.current.set("textHeight", h);
  Ask ar;
  ar.base = at;
  ar.def = c.doc.in_model() && c.doc.ucs.truthy() ? Value(base::js_to_number(c.doc.ucs.get("ang"))) : Value(0);
  ar.ucs = true;
  const Answer rot_a = co_await c.angle(c.t("cmd.attdef.rot"), std::move(ar));  // new text runs along the UCS (DR.8)
  const double rot = rot_a.kind == Answer::Kind::number && !std::isnan(rot_a.number) ? rot_a.number : 0;
  double y = 0;
  for (;;) {
    const Answer line = co_await c.string(c.t("cmd.text.enter_text"));
    if (!line.truthy()) {
      co_return;
    }
    const Pt pos = polar(at, rot - kPi / 2, y);
    Value e = base::make_object(
        {{"type", "text"}, {"x", pos.x}, {"y", pos.y}, {"h", h}, {"rot", norm_angle(rot)}, {"s", line.text}});
    if (ha.empty()) {
      e.set("ha", Value());
    } else {
      e.set("ha", ha);
    }
    e.set("va", va);
    e.set("wf", 1);
    c.add(c.ent(e));
    y += h * 1.667;  // DTEXT line spacing
  }
}

}  // namespace

Value rect(Pt a, Pt b) {
  return base::make_object({{"type", "poly"},
                            {"closed", true},
                            {"pts", base::Array{vtx(a.x, a.y), vtx(b.x, a.y), vtx(b.x, b.y), vtx(a.x, b.y)}}});
}

void add_draw(std::vector<CommandDef>& t) {
  t.push_back(def("LINE", {"L"}, line_cmd, "cmd.line.straight_segments_each_own"));
  t.push_back(def("PLINE", {"PL"}, pline_cmd, "cmd.pline.polyline_line_arc_segments"));
  t.push_back(def("TTR", {}, circle_ttr_cmd, "cmd.ttr.help"));
  t.push_back(def("CIRCLE", {"C"}, circle_cmd, "cmd.circle.center_radius_diameter_2"));
  t.push_back(def("XLINE", {"XL"}, xline_cmd, "cmd.xline.help"));
  t.push_back(def("RAY", {}, ray_cmd, "cmd.ray.help"));
  t.push_back(def("ARC", {"A"}, arc_cmd, "cmd.arc.three_points_center_start"));
  t.push_back(def("RECTANG", {"REC", "RECTANGLE"}, rectang_cmd, "cmd.rectang.closed_polyline_two_corners"));
  t.push_back(def("POLYGON", {"POL"}, polygon_cmd, "cmd.polygon.regular_polygon"));
  t.push_back(def("TEXT", {"DT", "DTEXT"}, text_cmd, "cmd.text.help"));
}

}  // namespace cc::cmd::detail
