// SPDX-License-Identifier: Apache-2.0
// geom.js txEntity: an entity through a 2D affine matrix (MOVE, COPY, ROTATE, SCALE, MIRROR, a block's parts into the
// world). The JS's spread keeps the entity's other fields and their order: Value::set does the same.
#include <array>
#include <cmath>
#include <numbers>

#include "cc/base/js_math.hpp"
#include "cc/cad/geom.hpp"
#include "util.hpp"

namespace cc::cad {

using detail::g;
using detail::num;
using detail::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;
constexpr double kEps = 1e-9;

struct Sim {
  double s = 1, rot = 0;
  bool flip = false;
};
/// mSimilar(m) || { s: √|det|, rot: atan2(m1, m0), flip: det < 0 }
Sim sim_of(const Mat2& m) {
  const double sx = base::js_hypot(m[0], m[1]);
  const double sy = base::js_hypot(m[2], m[3]);
  const double dot = m[0] * m[2] + m[1] * m[3];
  const double det = m[0] * m[3] - m[1] * m[2];
  Sim r{.s = sx, .rot = base::js_atan2(m[1], m[0]), .flip = det < 0};
  if (std::abs(sx - sy) > 1e-9 * std::max(sx, sy) || std::abs(dot) > 1e-9 * sx * sy) {
    r.s = std::sqrt(std::abs(det));
  }
  return r;
}
P2 app(const Mat2& m, double x, double y) {
  return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}
double or0(const Value& v) {
  return v.truthy() ? num(v) : 0;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — the JS's switch
Value tx_entity(const Value& e, const Mat2& m) {
  const Sim sim = sim_of(m);
  const std::string type = detail::str(g(e, "type"));
  Value out = e;
  if (type == "line") {
    const P2 a = app(m, num(g(e, "x1")), num(g(e, "y1")));
    const P2 b = app(m, num(g(e, "x2")), num(g(e, "y2")));
    out.set("x1", a.x);
    out.set("y1", a.y);
    out.set("x2", b.x);
    out.set("y2", b.y);
  } else if (type == "point") {
    const P2 a = app(m, num(g(e, "x")), num(g(e, "y")));
    out.set("x", a.x);
    out.set("y", a.y);
  } else if (type == "circle") {
    const P2 c = app(m, num(g(e, "cx")), num(g(e, "cy")));
    out.set("cx", c.x);
    out.set("cy", c.y);
    out.set("r", num(g(e, "r")) * sim.s);
  } else if (type == "arc") {
    const double cx = num(g(e, "cx"));
    const double cy = num(g(e, "cy"));
    const double r = num(g(e, "r"));
    const double a0 = num(g(e, "a0"));
    const double a1 = num(g(e, "a1"));
    const P2 c = app(m, cx, cy);
    const P2 s = app(m, cx + r * base::js_cos(a0), cy + r * base::js_sin(a0));
    const P2 t = app(m, cx + r * base::js_cos(a1), cy + r * base::js_sin(a1));
    double b0 = base::js_atan2(s.y - c.y, s.x - c.x);
    double b1 = base::js_atan2(t.y - c.y, t.x - c.x);
    if (sim.flip) {
      std::swap(b0, b1);
    }
    out.set("cx", c.x);
    out.set("cy", c.y);
    out.set("r", r * sim.s);
    out.set("a0", detail::norm_angle(b0));
    out.set("a1", detail::norm_angle(b1));
  } else if (type == "poly") {
    base::Array pts;
    const Value& src = detail::at(e, "pts");
    if (src.is_array()) {
      pts.reserve(src.as_array().size());
      for (const Value& p : src.as_array()) {
        const P2 q = app(m, num(g(p, "x")), num(g(p, "y")));
        const double b = or0(g(p, "b"));
        // { ...P(p.x, p.y), b }: a fresh point with just x, y, b
        Value v = base::make_object({{"x", q.x}, {"y", q.y}, {"b", sim.flip ? -b : b}});
        pts.push_back(std::move(v));
      }
    }
    out.set("pts", std::move(pts));
  } else if (type == "ellipse") {
    const double cx = num(g(e, "cx"));
    const double cy = num(g(e, "cy"));
    const P2 c = app(m, cx, cy);
    const P2 a = app(m, cx + num(g(e, "mx")), cy + num(g(e, "my")));
    double t0 = num(g(e, "t0"));
    double t1 = num(g(e, "t1"));
    if (sim.flip && t1 - t0 < kTau - kEps) {
      const double n0 = kTau - t1;
      const double n1 = kTau - t0;
      t0 = n0;
      t1 = n1;
    }
    out.set("cx", c.x);
    out.set("cy", c.y);
    out.set("mx", a.x - c.x);
    out.set("my", a.y - c.y);
    out.set("t0", t0);
    out.set("t1", t1);
  } else if (type == "text") {
    const P2 a = app(m, num(g(e, "x")), num(g(e, "y")));
    double rot = or0(g(e, "rot")) + sim.rot;
    if (sim.flip) {
      // Mirrored text is placed mirrored but stays readable (MIRRTEXT 0)
      rot = base::js_atan2(m[1], m[0]) - or0(g(e, "rot"));
      if (base::js_cos(rot) < -1e-9) {
        rot += std::numbers::pi;
      }
    }
    out.set("x", a.x);
    out.set("y", a.y);
    out.set("h", num(g(e, "h")) * sim.s);
    out.set("rot", detail::norm_angle(rot));
    if (const Value ww = g(e, "ww"); ww.truthy()) {
      out.set("ww", num(ww) * sim.s);
    }
  } else if (type == "insert") {
    const P2 a = app(m, num(g(e, "x")), num(g(e, "y")));
    const double r0 = or0(g(e, "rot"));
    const double rot = sim.flip ? sim.rot + std::numbers::pi - r0 : r0 + sim.rot;
    const Value sx = g(e, "sx");
    const Value sy = g(e, "sy");
    out.set("x", a.x);
    out.set("y", a.y);
    out.set("rot", detail::norm_angle(rot));
    out.set("sx", num(detail::nn(sx, 1)) * sim.s * (sim.flip ? -1 : 1));
    out.set("sy", num(detail::nn(sy, 1)) * sim.s);
  } else if (type == "hatch") {
    base::Array loops;
    const Value& src = detail::at(e, "loops");
    if (src.is_array()) {
      for (const Value& l : src.as_array()) {
        base::Array o;
        if (l.is_array()) {
          const base::Array& f = l.as_array();
          for (std::size_t i = 0; i + 1 < f.size(); i += 2) {
            const P2 p = app(m, num(f[i]), num(f[i + 1]));
            o.emplace_back(p.x);
            o.emplace_back(p.y);
          }
        }
        loops.emplace_back(std::move(o));
      }
    }
    out.set("loops", std::move(loops));
  } else if (type == "dim") {
    out.erase("block");  // its imported geometry no longer fits: draw it again
    for (const char* k : {"p1", "p2", "c", "loc"}) {
      if (const Value p = g(e, k); p.truthy()) {
        const P2 q = app(m, num(g(p, "x")), num(g(p, "y")));
        out.set(k, base::make_object({{"x", q.x}, {"y", q.y}}));
      }
    }
    if (g(e, "kind") == Value("linear")) {
      const double r = or0(g(e, "rot"));
      const double ux = m[0] * base::js_cos(r) + m[2] * base::js_sin(r);
      const double uy = m[1] * base::js_cos(r) + m[3] * base::js_sin(r);
      out.set("rot", detail::norm_angle(base::js_atan2(uy, ux)));
    }
  } else if (type == "viewport") {
    const P2 c = app(m, num(g(e, "x")), num(g(e, "y")));
    out.set("x", c.x);
    out.set("y", c.y);
    out.set("w", num(g(e, "w")) * sim.s);
    out.set("h", num(g(e, "h")) * sim.s);
    out.set("s", num(g(e, "s")) * sim.s);
  } else if (type == "xline" || type == "ray") {
    const P2 a = app(m, num(g(e, "x")), num(g(e, "y")));
    const double dx = num(g(e, "dx"));
    const double dy = num(g(e, "dy"));
    const double vx = m[0] * dx + m[2] * dy;
    const double vy = m[1] * dx + m[3] * dy;
    const double L = base::js_hypot(vx, vy);
    out.set("x", a.x);
    out.set("y", a.y);
    if (L > kEps) {  // unitDir(…) || { dx, dy }
      out.set("dx", vx / L);
      out.set("dy", vy / L);
    } else {
      out.set("dx", dx);
      out.set("dy", dy);
    }
  } else if (type == "mleader") {
    base::Array pts;
    const Value& src = detail::at(e, "pts");
    Value last;
    if (src.is_array()) {
      for (const Value& p : src.as_array()) {
        const P2 q = app(m, num(g(p, "x")), num(g(p, "y")));
        pts.push_back(base::make_object({{"x", q.x}, {"y", q.y}}));
        last = p;
      }
    }
    // the landing keeps pointing the way it pointed; the text stays level
    const P2 far = app(m, num(g(last, "x")) + (g(e, "dir") == Value(-1) ? -1 : 1), num(g(last, "y")));
    const double qx = pts.empty() ? detail::kNaN : num(g(pts.back(), "x"));
    const Value st = leader_style(e);
    const double k = sim.s;
    out.set("pts", std::move(pts));
    out.set("dir", far.x >= qx ? 1 : -1);
    const Value h = g(e, "h");
    out.set("h", num(h.truthy() ? h : g(st, "asz")) * k);
    if (const Value ww = g(e, "ww"); ww.truthy()) {
      out.set("ww", num(ww) * k);
    }
    Value st2 = st;
    st2.set("asz", num(g(st, "asz")) * k);
    st2.set("dogleg", num(g(st, "dogleg")) * k);
    st2.set("gap", num(g(st, "gap")) * k);
    out.set("st", std::move(st2));
  }
  return out;
}

}  // namespace cc::cad
