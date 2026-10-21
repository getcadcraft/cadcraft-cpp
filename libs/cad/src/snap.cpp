// SPDX-License-Identifier: Apache-2.0
// geom.js snapPoints / tangentFrom / grips / gripMove and view.js snap (CadCraft 0.5) in C++: the object snaps a
// point prompt offers (end, mid, centre, quadrant, intersection, perpendicular, tangent, node, insertion, nearest),
// chosen by priority then distance, and the grips of a selected object with what dragging one does.
#include "cc/cad/snap.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
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

constexpr double kEps = 1e-9;

double dist(P2 a, P2 b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
std::string type_of(const Value& e) {
  return detail::str(g(e, "type"));
}
P2 polar(P2 p, double a, double d) {
  return {p.x + base::js_cos(a) * d, p.y + base::js_sin(a) * d};
}

int priority(const std::string& kind) {
  if (kind == "end" || kind == "int" || kind == "node") {
    return 0;
  }
  if (kind == "mid" || kind == "cen" || kind == "ins") {
    return 1;
  }
  if (kind == "quad" || kind == "perp" || kind == "tan") {
    return 2;
  }
  if (kind == "near") {
    return 9;
  }
  return 5;
}

/// view.js nearPrim(q, p, r)
bool near_prim(const Prim& q, P2 p, double r) {
  if (!q.arc) {
    return std::min(q.x1, q.x2) - r <= p.x && std::max(q.x1, q.x2) + r >= p.x && std::min(q.y1, q.y2) - r <= p.y &&
           std::max(q.y1, q.y2) + r >= p.y;
  }
  return std::abs(base::js_hypot(p.x - q.cx, p.y - q.cy) - q.r) <= r;
}

}  // namespace

std::vector<P2> tangent_from(P2 p, double cx, double cy, double r) {
  const double dx = p.x - cx;
  const double dy = p.y - cy;
  const double d = base::js_hypot(dx, dy);
  if (d < r - kEps) {
    return {};
  }
  if (d <= r + kEps) {
    return {p};
  }
  const double a = base::js_atan2(dy, dx);
  const double b = std::acos(r / d);
  return {{cx + r * base::js_cos(a + b), cy + r * base::js_sin(a + b)},
          {cx + r * base::js_cos(a - b), cy + r * base::js_sin(a - b)}};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's snapPoints, in order
std::vector<Snap> snap_points(const Value& e, P2 q, double r, const SnapKinds& kinds, const BlockLookup& blocks,
                              std::optional<P2> base) {
  std::vector<Snap> out;
  const auto add = [&](std::string_view kind, P2 p) {
    if (kinds.contains(std::string(kind)) && std::abs(p.x - q.x) <= r && std::abs(p.y - q.y) <= r) {
      out.push_back({.kind = std::string(kind), .x = p.x, .y = p.y, .defer = std::nullopt, .pr = std::nullopt});
    }
  };
  const std::string t = type_of(e);
  if (t == "point") {
    add("node", {num(g(e, "x")), num(g(e, "y"))});
    return out;
  }
  if (t == "text") {
    add("ins", {num(g(e, "x")), num(g(e, "y"))});
    return out;
  }
  if (t == "insert") {
    add("ins", {num(g(e, "x")), num(g(e, "y"))});
  }
  if (t == "xline" || t == "ray") {  // no end or mid: only nearest and perpendicular here
    const std::vector<Prim> ps = entity_prims(e, nullptr);
    if (ps.empty()) {
      return out;
    }
    const Prim& p = ps[0];
    if (base && kinds.contains("perp")) {
      const double tt = prim_param(p, *base, false);
      if (tt >= 0 && tt <= 1) {
        add("perp", detail::prim_point(p, tt));
      }
    }
    if (kinds.contains("near")) {
      const P2 n = detail::prim_point(p, prim_param(p, q, true));
      if (dist(n, q) <= r) {
        out.push_back({.kind = "near", .x = n.x, .y = n.y, .defer = std::nullopt, .pr = std::nullopt});
      }
    }
    return out;
  }
  for (const Prim& p : entity_prims(e, blocks)) {
    if (p.box) {
      continue;
    }
    if (!p.arc) {
      add("end", {p.x1, p.y1});
      add("end", {p.x2, p.y2});
      add("mid", {(p.x1 + p.x2) / 2, (p.y1 + p.y2) / 2});
    } else {
      add("cen", {p.cx, p.cy});
      if (!p.full) {
        add("end", detail::prim_point(p, 0));
        add("end", detail::prim_point(p, 1));
        add("mid", detail::prim_point(p, 0.5));
      }
      for (int k = 0; k < 4; ++k) {
        const double a = k * std::numbers::pi / 2;
        if (detail::on_sweep(a, p.a0, p.a1)) {
          add("quad", {p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)});
        }
      }
    }
    if (base && kinds.contains("perp")) {
      if (!p.arc) {
        const double tt = prim_param(p, *base, false);
        if (tt >= 0 && tt <= 1) {
          add("perp", detail::prim_point(p, tt));
        }
      } else {
        const double a = base::js_atan2(base->y - p.cy, base->x - p.cx);
        if (detail::on_sweep(a, p.a0, p.a1)) {
          add("perp", {p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)});
        }
      }
    }
    if (kinds.contains("tan") && p.arc) {  // DR.3: from the base point, the touch points on the arc
      if (base) {
        for (const P2 x : tangent_from(*base, p.cx, p.cy, p.r)) {
          if (detail::on_sweep(base::js_atan2(x.y - p.cy, x.x - p.cx), p.a0, p.a1)) {
            add("tan", x);
          }
        }
      } else {
        const double a = base::js_atan2(q.y - p.cy, q.x - p.cx);
        const P2 n{p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)};
        if (detail::on_sweep(a, p.a0, p.a1) && std::abs(base::js_hypot(q.x - p.cx, q.y - p.cy) - p.r) <= r) {
          out.push_back(
              {.kind = "tan", .x = n.x, .y = n.y, .defer = std::array<double, 3>{p.cx, p.cy, p.r}, .pr = std::nullopt});
        }
      }
    }
    if (kinds.contains("near")) {
      const P2 n = detail::prim_point(p, prim_param(p, q, true));
      if (dist(n, q) <= r) {
        out.push_back({.kind = "near", .x = n.x, .y = n.y, .defer = std::nullopt, .pr = std::nullopt});
      }
    }
  }
  return out;
}

std::optional<Snap> best_snap(const std::vector<const Value*>& nearby, P2 p, double r, const SnapKinds& kinds,
                              const BlockLookup& blocks, std::optional<P2> base,
                              const std::function<std::optional<Snap>(P2, double)>& extra) {
  if (kinds.empty()) {
    return std::nullopt;
  }
  std::optional<Snap> best;
  double best_d = 0;
  int best_pr = 0;
  const auto consider = [&](const Snap& c) {
    const double d = base::js_hypot(c.x - p.x, c.y - p.y);
    if (d > r) {
      return;
    }
    const int pr = c.pr.value_or(priority(c.kind));
    if (!best || pr < best_pr || (pr == best_pr && d < best_d)) {
      best = c;
      best_d = d;
      best_pr = pr;
    }
  };
  for (const Value* e : nearby) {
    for (const Snap& c : snap_points(*e, p, r, kinds, blocks, base)) {
      consider(c);
    }
  }
  if (kinds.contains("int") && nearby.size() > 1) {
    std::vector<std::vector<Prim>> ps;
    for (std::size_t i = 0; i < nearby.size() && i < 60; ++i) {
      std::vector<Prim> keep;
      for (const Prim& q : entity_prims(*nearby[i], blocks)) {
        if (!q.box && near_prim(q, p, r)) {
          keep.push_back(q);
        }
      }
      ps.push_back(std::move(keep));
    }
    for (std::size_t i = 0; i < ps.size(); ++i) {
      for (std::size_t j = i + 1; j < ps.size(); ++j) {
        for (const Prim& a : ps[i]) {
          for (const Prim& b : ps[j]) {
            for (const P2 x : intersect(a, b, false, false)) {
              consider({.kind = "int", .x = x.x, .y = x.y, .defer = std::nullopt, .pr = std::nullopt});
            }
          }
        }
      }
    }
  }
  if (kinds.contains("node") && extra) {  // a point cloud's points (SG.1)
    if (const auto c = extra(p, r)) {
      consider(*c);
    }
  }
  return best;
}

std::vector<Grip> grips(const Value& e) {
  const std::string t = type_of(e);
  const auto N = [&](std::string_view k) { return num(g(e, k)); };
  if (t == "line") {
    return {
        {N("x1"), N("y1"), "p1"}, {(N("x1") + N("x2")) / 2, (N("y1") + N("y2")) / 2, "mid"}, {N("x2"), N("y2"), "p2"}};
  }
  if (t == "circle") {
    std::vector<Grip> out = {{N("cx"), N("cy"), "c"}};
    for (int k = 0; k < 4; ++k) {
      out.push_back({N("cx") + N("r") * base::js_cos(k * std::numbers::pi / 2),
                     N("cy") + N("r") * base::js_sin(k * std::numbers::pi / 2), "r"});
    }
    return out;
  }
  if (t == "arc") {
    const P2 c{N("cx"), N("cy")};
    const P2 a = polar(c, N("a0"), N("r"));
    const P2 b = polar(c, N("a1"), N("r"));
    return {{c.x, c.y, "c"}, {a.x, a.y, "a0"}, {b.x, b.y, "a1"}};
  }
  if (t == "poly" || t == "mleader") {
    std::vector<Grip> out;
    const Value pts = g(e, "pts");
    if (pts.is_array()) {
      for (std::size_t i = 0; i < pts.as_array().size(); ++i) {
        const Value& p = pts.as_array()[i];
        out.push_back({num(g(p, "x")), num(g(p, "y")), "v" + std::to_string(i)});
      }
    }
    return out;
  }
  if (t == "ellipse") {
    return {{N("cx"), N("cy"), "c"}};
  }
  if (t == "text" || t == "point" || t == "insert" || t == "xline" || t == "ray") {
    return {{N("x"), N("y"), "p"}};
  }
  if (t == "dim") {
    std::vector<Grip> out;
    const Value kind = g(e, "kind");
    for (const char* k : {"p1", "p2", "c", "loc"}) {
      const Value p = g(e, k);
      if (!p.truthy() || (std::string_view(k) == "p2" && (kind == Value("radius") || kind == Value("diameter")))) {
        continue;
      }
      out.push_back({num(g(p, "x")), num(g(p, "y")), k});
    }
    return out;
  }
  if (t == "viewport") {
    const double hw = N("w") / 2;
    const double hh = N("h") / 2;
    const double x = N("x");
    const double y = N("y");
    return {
        {x, y, "c"}, {x - hw, y - hh, "k0"}, {x + hw, y - hh, "k1"}, {x + hw, y + hh, "k2"}, {x - hw, y + hh, "k3"}};
  }
  return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS's gripMove
Value grip_move(const Value& e, const std::string& key, P2 to) {
  const std::string t = type_of(e);
  const auto N = [&](std::string_view k) { return num(g(e, k)); };
  Value out = e;
  if (t == "line") {
    if (key == "p1") {
      out.set("x1", to.x);
      out.set("y1", to.y);
    } else if (key == "p2") {
      out.set("x2", to.x);
      out.set("y2", to.y);
    } else {
      const double dx = to.x - (N("x1") + N("x2")) / 2;
      const double dy = to.y - (N("y1") + N("y2")) / 2;
      out.set("x1", N("x1") + dx);
      out.set("y1", N("y1") + dy);
      out.set("x2", N("x2") + dx);
      out.set("y2", N("y2") + dy);
    }
    return out;
  }
  if (t == "circle") {
    if (key == "c") {
      out.set("cx", to.x);
      out.set("cy", to.y);
    } else {
      out.set("r", std::max(dist({N("cx"), N("cy")}, to), kEps));
    }
    return out;
  }
  if (t == "arc") {
    if (key == "c") {
      out.set("cx", to.x);
      out.set("cy", to.y);
    } else {
      out.set(key, detail::norm_angle(base::js_atan2(to.y - N("cy"), to.x - N("cx"))));
    }
    return out;
  }
  if (t == "poly" || t == "mleader") {
    const std::size_t i = key.size() > 1 ? static_cast<std::size_t>(std::stoul(key.substr(1))) : 0;
    const Value pts = g(e, "pts");
    base::Array np = pts.is_array() ? pts.as_array() : base::Array{};
    if (i < np.size()) {
      if (t == "poly") {
        np[i].set("x", to.x);
        np[i].set("y", to.y);
      } else {
        np[i] = base::make_object({{"x", to.x}, {"y", to.y}});
      }
    }
    out.set("pts", std::move(np));
    return out;
  }
  if (t == "ellipse") {
    out.set("cx", to.x);
    out.set("cy", to.y);
    return out;
  }
  if (t == "text" || t == "point" || t == "insert" || t == "xline" || t == "ray") {
    out.set("x", to.x);
    out.set("y", to.y);
    return out;
  }
  if (t == "dim") {
    out.erase("block");
    const Value kind = g(e, "kind");
    if (key == "c" && (kind == Value("radius") || kind == Value("diameter"))) {
      const Value c = g(e, "c");
      const double dx = to.x - num(g(c, "x"));
      const double dy = to.y - num(g(c, "y"));
      for (const char* k : {"c", "p1", "loc"}) {
        const Value p = g(e, k);
        out.set(k, base::make_object({{"x", num(g(p, "x")) + dx}, {"y", num(g(p, "y")) + dy}}));
      }
      return out;
    }
    out.set(key, base::make_object({{"x", to.x}, {"y", to.y}}));
    return out;
  }
  if (t == "viewport") {
    if (key == "c") {
      out.set("x", to.x);
      out.set("y", to.y);
      return out;
    }
    // a corner: the opposite corner stays, and so does the view on the sheet
    const int k = key.size() > 1 ? key[1] - '0' : 0;
    const double hw = N("w") / 2;
    const double hh = N("h") / 2;
    const std::array<P2, 4> c = {P2{N("x") - hw, N("y") - hh}, P2{N("x") + hw, N("y") - hh},
                                 P2{N("x") + hw, N("y") + hh}, P2{N("x") - hw, N("y") + hh}};
    const P2 o = c.at(static_cast<std::size_t>((k + 2) % 4));
    const double w = std::max(std::abs(to.x - o.x), kEps);
    const double h = std::max(std::abs(to.y - o.y), kEps);
    const double x = (to.x + o.x) / 2;
    const double y = (to.y + o.y) / 2;
    const Value twv = g(e, "tw");
    const double tw = twv.truthy() ? num(twv) : 0;
    const double tc = base::js_cos(-tw);
    const double tn = base::js_sin(-tw);
    const double dx = (x - N("x")) / N("s");
    const double dy = (y - N("y")) / N("s");
    out.set("x", x);
    out.set("y", y);
    out.set("w", w);
    out.set("h", h);
    out.set("cx", N("cx") + tc * dx - tn * dy);
    out.set("cy", N("cy") + tn * dx + tc * dy);
    return out;
  }
  return out;
}

}  // namespace cc::cad
