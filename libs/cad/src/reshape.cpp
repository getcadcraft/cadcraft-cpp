// SPDX-License-Identifier: Apache-2.0
// geom.js's reshaping geometry (CadCraft 0.5) in C++: BREAK (paramOn, breakEntity), LENGTHEN (curveLength,
// lengthenEntity), JOIN (joinEntities), STRETCH (stretchEntity) and EXPLODE (explodeEntity), with the JS's order of
// operations and tolerances.
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js_math.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
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
std::string type_of(const Value& e) {
  return detail::str(g(e, "type"));
}
P2 xy(const Value& p) {
  return {num(g(p, "x")), num(g(p, "y"))};
}
P2 polar(P2 c, double a, double d) {
  return {c.x + base::js_cos(a) * d, c.y + base::js_sin(a) * d};
}
bool unbounded(const std::string& t) {
  return t == "xline" || t == "ray";
}
double prim_length(const Prim& p) {
  return !p.arc ? base::js_hypot(p.x2 - p.x1, p.y2 - p.y1) : p.r * (p.a1 - p.a0);
}
std::vector<Prim> poly_prims(const Value& e) {
  std::vector<Prim> out;
  detail::poly_prims(e, out);
  return out;
}
/// e's own primitives: a polyline's per segment, else prims(e)
std::vector<Prim> own_prims(const Value& e) {
  if (type_of(e) == "poly") {
    return poly_prims(e);
  }
  std::vector<Prim> out;
  detail::prims(e, nullptr, 0, out);
  return out;
}
/// { ...p }: a copy (set() turns a compact point into an object when it needs to)
Value plain(const Value& p) {
  return p;
}
Value pt_obj(P2 p) {
  return base::make_object({{"x", p.x}, {"y", p.y}});
}
Value vtx(double x, double y, double b) {
  return base::make_object({{"x", x}, {"y", y}, {"b", b}});
}
/// e.color ?? null
Value color_or_null(const Value& e) {
  return detail::nn(g(e, "color"), Value(nullptr));
}

void grow(Bounds& b, double x, double y) {
  b.min_x = x < b.min_x ? x : b.min_x;
  b.min_y = y < b.min_y ? y : b.min_y;
  b.max_x = x > b.max_x ? x : b.max_x;
  b.max_y = y > b.max_y ? y : b.max_y;
}
/// geom.js primBounds(p, b)
void prim_bounds(const Prim& p, Bounds& b) {
  if (!p.arc) {
    grow(b, p.x1, p.y1);
    grow(b, p.x2, p.y2);
    return;
  }
  const P2 s = detail::prim_point(p, 0);
  const P2 t = detail::prim_point(p, 1);
  grow(b, s.x, s.y);
  grow(b, t.x, t.y);
  for (int k = 0; k < 4; ++k) {
    const double a = k * std::numbers::pi / 2;
    if (detail::on_sweep(a, p.a0, p.a1)) {
      grow(b, p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- BREAK and LENGTHEN

double param_on(const Value& e, P2 q) {
  const bool poly = type_of(e) == "poly";
  const std::vector<Prim> own = own_prims(e);
  double best = 0;
  double bd = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < own.size(); ++i) {
    const double d = dist_to_prim(own[i], q);
    if (d < bd) {
      bd = d;
      double t = prim_param(own[i], q);
      if (own[i].rev) {
        t = 1 - t;
      }
      best = (poly ? static_cast<double>(i) : 0) + t;
    }
  }
  return best;
}

BreakResult break_entity(const Value& e, P2 p1, P2 p2) {
  BreakResult out;
  const bool same = dist(p1, p2) <= kEps * std::max(1.0, base::js_hypot(p1.x, p1.y));
  const std::string t = type_of(e);
  if (t == "circle") {
    if (same) {
      out.error = "circle";
      return out;
    }
    const double cx = num(g(e, "cx"));
    const double cy = num(g(e, "cy"));
    const double a1 = base::js_atan2(p1.y - cy, p1.x - cx);
    const double a2 = base::js_atan2(p2.y - cy, p2.x - cx);
    Value arc = e;
    arc.set("type", "arc");
    arc.set("a0", detail::norm_angle(a2));
    arc.set("a1", detail::norm_angle(a1));
    out.pieces.push_back(std::move(arc));
    return out;
  }
  if (t != "line" && t != "arc" && t != "poly" && t != "xline" && t != "ray") {
    out.error = "type";
    return out;
  }
  double t1 = param_on(e, p1);
  double t2 = same ? t1 : param_on(e, p2);
  const auto keep = [&out](Value v) {
    if (v.truthy()) {
      out.pieces.push_back(std::move(v));
    }
  };
  if (t == "poly" && g(e, "closed").truthy()) {
    const Value pts = g(e, "pts");
    const std::size_t n = detail::len(pts);
    base::Array ring;
    ring.reserve(n * 2 + 1);
    for (int k = 0; k < 2; ++k) {
      for (const Value& p : pts.as_array()) {
        ring.push_back(p);
      }
    }
    if (n > 0) {
      ring.push_back(plain(pts.as_array()[0]));
    }
    Value open = e;
    open.set("closed", false);
    open.set("pts", std::move(ring));
    const auto dn = static_cast<double>(n);
    if (same) {
      keep(sub_entity(open, t1, t1 + dn));
      return out;
    }
    if (t2 < t1) {
      t2 += dn;
    }
    keep(sub_entity(open, t2, t1 + dn));
    return out;
  }
  if (t2 < t1) {
    std::swap(t1, t2);
  }
  const double max_t = t == "poly" ? static_cast<double>(detail::len(g(e, "pts"))) - 1 : 1;
  const double tiny = unbounded(t) ? 1e-15 : 1e-9;
  if (t1 > tiny || t == "xline") {
    keep(sub_entity(e, 0, t1));
  }
  if (t2 < max_t - tiny) {
    keep(sub_entity(e, t2, max_t));
  }
  return out;
}

std::optional<double> curve_length(const Value& e) {
  const std::string t = type_of(e);
  if (t == "line") {
    return base::js_hypot(num(g(e, "x2")) - num(g(e, "x1")), num(g(e, "y2")) - num(g(e, "y1")));
  }
  if (t == "arc") {
    const double sw = detail::norm_angle(num(g(e, "a1")) - num(g(e, "a0")));
    return num(g(e, "r")) * (sw != 0 && !std::isnan(sw) ? sw : kTau);
  }
  if (t == "poly") {
    double s = 0;
    for (const Prim& p : poly_prims(e)) {
      s += prim_length(p);
    }
    return s;
  }
  return std::nullopt;
}

LengthenResult lengthen_entity(const Value& e, P2 pick, double delta, std::optional<double> d_angle) {
  LengthenResult out;
  const std::string t = type_of(e);
  if (t == "line") {
    const double x1 = num(g(e, "x1"));
    const double y1 = num(g(e, "y1"));
    const double x2 = num(g(e, "x2"));
    const double y2 = num(g(e, "y2"));
    const double l = base::js_hypot(x2 - x1, y2 - y1);
    const double nl = l + delta;
    if (!(nl > kEps)) {
      out.error = "short";
      return out;
    }
    const bool at_start = dist(pick, {x1, y1}) < dist(pick, {x2, y2});
    const double ux = (x2 - x1) / l;
    const double uy = (y2 - y1) / l;
    Value r = e;
    if (at_start) {
      r.set("x1", x2 - ux * nl);
      r.set("y1", y2 - uy * nl);
    } else {
      r.set("x2", x1 + ux * nl);
      r.set("y2", y1 + uy * nl);
    }
    out.e = std::move(r);
    return out;
  }
  if (t == "arc") {
    const double a0 = num(g(e, "a0"));
    const double a1 = num(g(e, "a1"));
    const double rr = num(g(e, "r"));
    double sw = detail::norm_angle(a1 - a0);
    sw = sw != 0 && !std::isnan(sw) ? sw : kTau;
    const double ns = sw + (d_angle ? *d_angle : delta / rr);
    if (!(ns > 1e-9)) {
      out.error = "short";
      return out;
    }
    if (ns >= kTau - 1e-9) {
      out.error = "full";
      return out;
    }
    const P2 c{num(g(e, "cx")), num(g(e, "cy"))};
    const bool at_start = dist(pick, polar(c, a0, rr)) < dist(pick, polar(c, a1, rr));
    Value r = e;
    if (at_start) {
      r.set("a0", detail::norm_angle(a1 - ns));
    } else {
      r.set("a1", detail::norm_angle(a0 + ns));
    }
    out.e = std::move(r);
    return out;
  }
  const Value src = g(e, "pts");
  if (t == "poly" && !g(e, "closed").truthy() && detail::len(src) >= 2) {
    const std::size_t n = detail::len(src);
    base::Array pts;
    pts.reserve(n);
    for (const Value& p : src.as_array()) {
      pts.push_back(plain(p));
    }
    const bool at_start = dist(pick, xy(pts[0])) < dist(pick, xy(pts[n - 1]));
    const std::size_t i = at_start ? 0 : n - 2;  // the end segment
    const P2 pp = xy(pts[i]);
    const P2 qq = xy(pts[i + 1]);
    const double b = g(pts[i], "b").truthy() ? num(g(pts[i], "b")) : 0;
    if (std::abs(b) < 1e-12) {
      const double l = dist(pp, qq);
      const double nl = l + delta;
      if (!(nl > kEps)) {
        out.error = "short";
        return out;
      }
      const double ux = (qq.x - pp.x) / l;
      const double uy = (qq.y - pp.y) / l;
      if (at_start) {
        pts[0].set("x", qq.x - ux * nl);
        pts[0].set("y", qq.y - uy * nl);
      } else {
        pts[n - 1].set("x", pp.x + ux * nl);
        pts[n - 1].set("y", pp.y + uy * nl);
      }
      Value r = e;
      r.set("pts", std::move(pts));
      out.e = std::move(r);
      return out;
    }
    const Prim a = detail::bulge_arc(pp.x, pp.y, qq.x, qq.y, b);
    const double sw = a.a1 - a.a0;
    const double ns = sw + (d_angle ? *d_angle : delta / a.r);
    if (!(ns > 1e-9)) {
      out.error = "short";
      return out;
    }
    if (ns >= kTau - 1e-9) {
      out.error = "full";
      return out;
    }
    // travel P → Q: counter-clockwise when b > 0; the far end of the segment moves, the near one stays
    const P2 c{a.cx, a.cy};
    const double dir = b > 0 ? 1 : -1;
    if (at_start) {
      const double aq = base::js_atan2(qq.y - c.y, qq.x - c.x);
      const P2 np = polar(c, aq - dir * ns, a.r);
      pts[0].set("x", np.x);
      pts[0].set("y", np.y);
    } else {
      const double ap = base::js_atan2(pp.y - c.y, pp.x - c.x);
      const P2 nq = polar(c, ap + dir * ns, a.r);
      pts[n - 1].set("x", nq.x);
      pts[n - 1].set("y", nq.y);
    }
    pts[i].set("b", dir * std::tan(ns / 4));
    Value r = e;
    r.set("pts", std::move(pts));
    out.e = std::move(r);
    return out;
  }
  out.error = "type";
  return out;
}

// ---------------------------------------------------------------- JOIN

namespace {

struct Vert {
  double x = 0, y = 0, b = 0;
};
using Verts = std::vector<Vert>;

/// geom.js pieceVerts(e): a piece as polyline vertices from its start to its end (null: not a piece)
std::optional<Verts> piece_verts(const Value& e) {
  const std::string t = type_of(e);
  if (t == "line") {
    return Verts{{num(g(e, "x1")), num(g(e, "y1")), 0}, {num(g(e, "x2")), num(g(e, "y2")), 0}};
  }
  if (t == "arc") {
    const P2 c{num(g(e, "cx")), num(g(e, "cy"))};
    const double r = num(g(e, "r"));
    double sw = detail::norm_angle(num(g(e, "a1")) - num(g(e, "a0")));
    sw = sw != 0 && !std::isnan(sw) ? sw : kTau;
    const P2 s = polar(c, num(g(e, "a0")), r);
    const P2 f = polar(c, num(g(e, "a1")), r);
    return Verts{{s.x, s.y, std::tan(sw / 4)}, {f.x, f.y, 0}};
  }
  const Value pts = g(e, "pts");
  if (t == "poly" && !g(e, "closed").truthy() && detail::len(pts) >= 2) {
    Verts v;
    for (const Value& p : pts.as_array()) {
      const Value b = g(p, "b");
      v.push_back({num(g(p, "x")), num(g(p, "y")), b.truthy() ? num(b) : 0});
    }
    return v;
  }
  return std::nullopt;
}
/// geom.js reverseVerts(v): the same run travelled the other way
Verts reverse_verts(const Verts& v) {
  Verts out;
  for (std::size_t k = v.size(); k-- > 0;) {
    out.push_back({v[k].x, v[k].y, k > 0 ? -v[k - 1].b : 0});
  }
  return out;
}

}  // namespace

JoinResult join_entities(const std::vector<Value>& ents, std::optional<double> tol) {
  JoinResult out;
  std::vector<const Value*> list;
  for (const Value& e : ents) {
    const std::string t = type_of(e);
    if ((t == "line" || t == "arc" || t == "poly") && !(t == "poly" && g(e, "closed").truthy())) {
      list.push_back(&e);
    } else {
      out.left.push_back(g(e, "id"));
    }
  }
  if (list.size() < 2) {
    out.left.clear();
    for (const Value& e : ents) {
      out.left.push_back(g(e, "id"));
    }
    return out;
  }
  Bounds bb;
  for (const Value* e : list) {
    for (const Prim& p : own_prims(*e)) {
      prim_bounds(p, bb);
    }
  }
  const double tt = tol ? *tol : std::max({bb.max_x - bb.min_x, bb.max_y - bb.min_y, 1.0}) * 1e-9;
  const auto ids_of = [&list] {
    base::Array a;
    for (const Value* e : list) {
      a.push_back(g(*e, "id"));
    }
    return a;
  };
  // all lines on one line → one line from the extremes
  if (std::ranges::all_of(list, [](const Value* e) { return type_of(*e) == "line"; })) {
    const Value& a = *list[0];
    const double ax = num(g(a, "x1"));
    const double ay = num(g(a, "y1"));
    const double ux = num(g(a, "x2")) - ax;
    const double uy = num(g(a, "y2")) - ay;
    const double l = base::js_hypot(ux, uy);
    if (l > kEps) {
      const auto on = [&](double x, double y) { return std::abs((x - ax) * uy - (y - ay) * ux) / l <= tt * 1e3; };
      if (std::ranges::all_of(list, [&](const Value* e) {
            return on(num(g(*e, "x1")), num(g(*e, "y1"))) && on(num(g(*e, "x2")), num(g(*e, "y2")));
          })) {
        double t0 = std::numeric_limits<double>::infinity();
        double t1 = -std::numeric_limits<double>::infinity();
        for (const Value* e : list) {
          for (const char* k : {"1", "2"}) {
            const double x = num(g(*e, std::string("x") + k));
            const double y = num(g(*e, std::string("y") + k));
            const double tv = ((x - ax) * ux + (y - ay) * uy) / (l * l);
            t0 = tv < t0 ? tv : t0;
            t1 = tv > t1 ? tv : t1;
          }
        }
        Value to = a;
        to.set("x1", ax + ux * t0);
        to.set("y1", ay + uy * t0);
        to.set("x2", ax + ux * t1);
        to.set("y2", ay + uy * t1);
        out.results.push_back({ids_of(), std::move(to)});
        return out;
      }
    }
  }
  // all arcs on one circle → one arc counter-clockwise from the first, or the circle
  if (std::ranges::all_of(list, [](const Value* e) { return type_of(*e) == "arc"; })) {
    const Value& a = *list[0];
    const double cx = num(g(a, "cx"));
    const double cy = num(g(a, "cy"));
    const double r = num(g(a, "r"));
    if (std::ranges::all_of(list, [&](const Value* e) {
          return base::js_hypot(num(g(*e, "cx")) - cx, num(g(*e, "cy")) - cy) <= tt * 1e3 &&
                 std::abs(num(g(*e, "r")) - r) <= tt * 1e3;
        })) {
      // merge the arcs' spans round the circle; the largest empty gap stays open, smaller gaps are filled
      std::vector<std::pair<double, double>> spans;
      spans.reserve(list.size());
      for (const Value* e : list) {
        spans.push_back(detail::arc_range(num(g(*e, "a0")), num(g(*e, "a1"))));
      }
      std::ranges::stable_sort(spans, [](const auto& p, const auto& q) { return p.first < q.first; });
      std::vector<std::pair<double, double>> merged;
      for (const auto& [s0, s1] : spans) {
        if (!merged.empty() && s0 <= merged.back().second + 1e-9) {
          merged.back().second = std::max(merged.back().second, s1);
        } else {
          merged.emplace_back(s0, s1);
        }
      }
      const auto last = merged.back();
      if (merged.size() > 1 && last.second - kTau >= merged[0].first - 1e-9) {
        merged[0] = {last.first - kTau, std::max(merged[0].second, last.second - kTau)};
        merged.pop_back();
      }
      const bool whole = merged.size() == 1 && merged[0].second - merged[0].first >= kTau - 1e-9;
      if (whole) {
        Value to = base::make_object({{"type", "circle"},
                                      {"layer", g(a, "layer")},
                                      {"color", color_or_null(a)},
                                      {"cx", cx},
                                      {"cy", cy},
                                      {"r", r}});
        out.results.push_back({ids_of(), std::move(to)});
        return out;
      }
      bool have = false;
      double best_gap = 0;
      double best_a0 = 0;
      double best_a1 = 0;
      for (std::size_t k = 0; k < merged.size(); ++k) {
        const auto& m = merged[k];
        const auto& nx = merged[(k + 1) % merged.size()];
        const double gap = (k + 1 < merged.size() ? nx.first : nx.first + kTau) - m.second;
        if (!have || gap > best_gap) {
          have = true;
          best_gap = gap;
          best_a0 = nx.first;
          best_a1 = m.second;
        }
      }
      Value to = a;
      to.set("a0", detail::norm_angle(best_a0));
      to.set("a1", detail::norm_angle(best_a1));
      out.results.push_back({ids_of(), std::move(to)});
      return out;
    }
  }
  // chains of pieces meeting end to end
  struct Piece {
    const Value* e;
    Verts v;
  };
  std::vector<Piece> pieces;
  for (const Value* e : list) {
    if (auto v = piece_verts(*e)) {
      pieces.push_back({e, std::move(*v)});
    }
  }
  const auto near = [tt](const Vert& p, const Vert& q) {
    return std::abs(p.x - q.x) <= tt * 1e3 && std::abs(p.y - q.y) <= tt * 1e3;
  };
  std::vector<bool> used(pieces.size(), false);
  for (std::size_t s = 0; s < pieces.size(); ++s) {
    if (used[s]) {
      continue;
    }
    used[s] = true;
    Verts chain = pieces[s].v;
    base::Array from{g(*pieces[s].e, "id")};
    for (bool grew = true; grew;) {
      grew = false;
      for (std::size_t k = 0; k < pieces.size(); ++k) {
        if (used[k]) {
          continue;
        }
        const Verts& pv = pieces[k].v;
        const Vert head = chain.front();
        const Vert tail = chain.back();
        Verts add;
        bool found = true;
        bool at_end = true;
        if (near(tail, pv.front())) {
          add = pv;
        } else if (near(tail, pv.back())) {
          add = reverse_verts(pv);
        } else if (near(head, pv.back())) {
          add = pv;
          at_end = false;
        } else if (near(head, pv.front())) {
          add = reverse_verts(pv);
          at_end = false;
        } else {
          found = false;
        }
        if (!found) {
          continue;
        }
        used[k] = true;
        from.push_back(g(*pieces[k].e, "id"));
        grew = true;
        if (at_end) {
          chain.pop_back();
          chain.insert(chain.end(), add.begin(), add.end());
        } else {
          add.pop_back();
          add.insert(add.end(), chain.begin(), chain.end());
          chain = std::move(add);
        }
      }
    }
    if (from.size() < 2) {
      out.left.push_back(g(*pieces[s].e, "id"));
      continue;
    }
    const bool closed = chain.size() > 2 && near(chain.front(), chain.back());
    if (closed) {
      chain.pop_back();
    }
    const Value& src = *pieces[s].e;
    base::Array pts;
    for (const Vert& q : chain) {
      pts.push_back(vtx(q.x, q.y, q.b));
    }
    Value to = base::make_object({{"type", "poly"},
                                  {"layer", g(src, "layer")},
                                  {"color", color_or_null(src)},
                                  {"closed", closed},
                                  {"pts", std::move(pts)}});
    out.results.push_back({std::move(from), std::move(to)});
  }
  return out;
}

// ---------------------------------------------------------------- STRETCH

namespace {

/// geom.js keepSagitta(b, c0, c1): the bulge that keeps an arc's height when its chord changes from c0 to c1
double keep_sagitta(double b, double c0, double c1) {
  if (!(std::abs(b) > 1e-12) || !(c1 > kEps)) {
    return b;
  }
  return (b * c0 / 2) * 2 / c1;
}

}  // namespace

std::optional<Value> stretch_entity(const Value& e, const std::function<bool(P2)>& inside, double dx, double dy) {
  const auto mv = [dx, dy](P2 p) { return P2{p.x + dx, p.y + dy}; };
  const Mat2 move{1, 0, 0, 1, dx, dy};
  const std::string t = type_of(e);
  if (t == "xline" || t == "ray") {  // its point in the window: the whole line moves (DR.6)
    if (!inside(xy(e))) {
      return std::nullopt;
    }
    Value r = e;
    r.set("x", num(g(e, "x")) + dx);
    r.set("y", num(g(e, "y")) + dy);
    return r;
  }
  if (t == "line") {
    const bool a = inside({num(g(e, "x1")), num(g(e, "y1"))});
    const bool b = inside({num(g(e, "x2")), num(g(e, "y2"))});
    if (!a && !b) {
      return std::nullopt;
    }
    Value r = e;
    if (a) {
      r.set("x1", num(g(e, "x1")) + dx);
      r.set("y1", num(g(e, "y1")) + dy);
    }
    if (b) {
      r.set("x2", num(g(e, "x2")) + dx);
      r.set("y2", num(g(e, "y2")) + dy);
    }
    return r;
  }
  if (t == "poly") {
    const Value src = g(e, "pts");
    const std::size_t n = detail::len(src);
    std::vector<bool> hit(n);
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
      hit[i] = inside(xy(src.as_array()[i]));
      any = any || hit[i];
    }
    if (!any) {
      return std::nullopt;
    }
    base::Array pts;
    for (std::size_t i = 0; i < n; ++i) {
      Value p = plain(src.as_array()[i]);
      if (hit[i]) {
        const P2 m = mv(xy(p));
        p.set("x", m.x);
        p.set("y", m.y);
      }
      pts.push_back(std::move(p));
    }
    const std::size_t segs = g(e, "closed").truthy() ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
      const std::size_t j = (i + 1) % n;
      const Value bv = g(src.as_array()[i], "b");
      const double b = bv.truthy() ? num(bv) : 0;
      if (hit[i] != hit[j] && std::abs(b) > 1e-12) {
        pts[i].set("b",
                   keep_sagitta(b, dist(xy(src.as_array()[i]), xy(src.as_array()[j])), dist(xy(pts[i]), xy(pts[j]))));
      }
    }
    Value r = e;
    r.set("pts", std::move(pts));
    return r;
  }
  if (t == "arc") {
    const P2 c{num(g(e, "cx")), num(g(e, "cy"))};
    const double rr = num(g(e, "r"));
    const double a0 = num(g(e, "a0"));
    const double a1 = num(g(e, "a1"));
    const P2 s = polar(c, a0, rr);
    const P2 f = polar(c, a1, rr);
    const bool a = inside(s);
    const bool b = inside(f);
    if (!a && !b) {
      return inside(c) ? std::optional<Value>(tx_entity(e, move)) : std::nullopt;
    }
    if (a && b) {
      return tx_entity(e, move);
    }
    double sweep = detail::norm_angle(a1 - a0);
    sweep = sweep != 0 && !std::isnan(sweep) ? sweep : kTau;
    const double b0 = std::tan(sweep / 4);
    const P2 s1 = a ? mv(s) : s;
    const P2 f1 = b ? mv(f) : f;
    const double nb = keep_sagitta(b0, dist(s, f), dist(s1, f1));
    if (dist(s1, f1) <= kEps) {
      return std::nullopt;
    }
    const Prim gg = detail::bulge_arc(s1.x, s1.y, f1.x, f1.y, nb);
    Value r = e;
    r.set("cx", gg.cx);
    r.set("cy", gg.cy);
    r.set("r", gg.r);
    r.set("a0", detail::norm_angle(gg.a0));
    r.set("a1", detail::norm_angle(gg.a1));
    return r;
  }
  if (t == "circle" || t == "ellipse") {
    return inside({num(g(e, "cx")), num(g(e, "cy"))}) ? std::optional<Value>(tx_entity(e, move)) : std::nullopt;
  }
  if (t == "text" || t == "point" || t == "insert") {
    return inside(xy(e)) ? std::optional<Value>(tx_entity(e, move)) : std::nullopt;
  }
  if (t == "mleader") {
    const Value src = g(e, "pts");
    bool any = false;
    base::Array pts;
    for (const Value& p : src.is_array() ? src.as_array() : base::Array{}) {
      if (inside(xy(p))) {
        any = true;
        pts.push_back(pt_obj(mv(xy(p))));
      } else {
        pts.push_back(p);
      }
    }
    if (!any) {
      return std::nullopt;
    }
    Value r = e;
    r.set("pts", std::move(pts));
    return r;
  }
  if (t == "dim") {
    Value r = e;
    bool any = false;
    for (const char* k : {"p1", "p2", "c", "loc"}) {
      const Value p = g(e, k);
      if (p.truthy() && inside(xy(p))) {
        any = true;
        r.set(k, pt_obj(mv(xy(p))));
      }
    }
    if (!any) {
      return std::nullopt;
    }
    r.erase("block");  // drawn again from its new points
    return r;
  }
  if (t == "hatch") {
    bool any = false;
    base::Array loops;
    const Value src = g(e, "loops");
    for (const Value& l : src.is_array() ? src.as_array() : base::Array{}) {
      base::Array o = l.is_array() ? l.as_array() : base::Array{};
      for (std::size_t i = 0; i + 1 < o.size(); i += 2) {
        if (inside({num(o[i]), num(o[i + 1])})) {
          o[i] = num(o[i]) + dx;
          o[i + 1] = num(o[i + 1]) + dy;
          any = true;
        }
      }
      loops.emplace_back(std::move(o));
    }
    if (!any) {
      return std::nullopt;
    }
    Value r = e;
    r.set("loops", std::move(loops));
    return r;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- EXPLODE

std::optional<base::Array> explode_entity(const Value& e, const BlockLookup& blocks) {
  const std::string t = type_of(e);
  if (t == "poly") {
    base::Array out;
    for (const Prim& p : poly_prims(e)) {
      if (!p.arc) {
        out.push_back(base::make_object({{"type", "line"},
                                         {"layer", g(e, "layer")},
                                         {"color", color_or_null(e)},
                                         {"x1", p.x1},
                                         {"y1", p.y1},
                                         {"x2", p.x2},
                                         {"y2", p.y2}}));
      } else {
        out.push_back(base::make_object({{"type", "arc"},
                                         {"layer", g(e, "layer")},
                                         {"color", color_or_null(e)},
                                         {"cx", p.cx},
                                         {"cy", p.cy},
                                         {"r", p.r},
                                         {"a0", detail::norm_angle(p.a0)},
                                         {"a1", detail::norm_angle(p.a1)}}));
      }
    }
    return out;
  }
  if (t == "mleader") {
    base::Array out = leader_geometry(e);
    for (Value& c : out) {
      c.set("layer", g(e, "layer"));
      c.set("color", color_or_null(e));
    }
    return out;
  }
  if (t != "insert" && t != "dim") {
    return std::nullopt;
  }
  base::Array parts;
  std::optional<Mat2> m;
  if (t == "insert") {
    const Value* blk = blocks ? blocks(detail::str(g(e, "block"))) : nullptr;
    if (blk == nullptr) {
      return std::nullopt;
    }
    // its visible attributes become plain text with their values
    const Value ents = g(*blk, "entities");
    if (ents.is_array()) {
      parts = ents.as_array();
    }
    for (Value a : attr_texts(e, *blk)) {
      a.erase("tag");
      a.erase("invisible");
      parts.push_back(std::move(a));
    }
    m = insert_matrix(e, *blk);
  } else {
    const Value bn = g(e, "block");
    const Value* blk = bn.truthy() && blocks ? blocks(detail::str(bn)) : nullptr;
    if (blk != nullptr) {
      const Value ents = g(*blk, "entities");
      if (ents.is_array()) {
        parts = ents.as_array();
      }
    } else {
      parts = dim_geometry(e);
    }
  }
  base::Array out;
  out.reserve(parts.size());
  for (const Value& c : parts) {
    Value x = m ? tx_entity(c, *m) : plain(c);
    if (g(x, "layer") == Value("0")) {
      x.set("layer", g(e, "layer"));
    }
    const Value col = g(x, "color");
    if (col.is_number() && col.as_number() == 0) {
      x.set("color", color_or_null(e));
    }
    x.erase("id");
    out.push_back(std::move(x));
  }
  return out;
}

}  // namespace cc::cad
