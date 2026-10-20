// SPDX-License-Identifier: Apache-2.0
// gsolve.js (CadCraft 0.5) in C++ — step for step, in the JS's orders (Maps and Sets keep insertion order).
#include "cc/cad/gsolve.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <numbers>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::cad::gsolve {

using base::Value;

namespace {

/// a[i] of an array value (anything else: undefined)
Value at(const Value& a, std::size_t i) {
  return a.is_array() && i < a.as_array().size() ? a.as_array()[i] : Value();
}

using V2 = std::array<double, 2>;
constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2 * kPi;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// `a || d` for a number
double jor(double a, double d) {
  return a != 0 && !std::isnan(a) ? a : d;
}
/// Math.max of two (NaN wins)
double jmax(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a > b ? a : b;
}
double jmin(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a < b ? a : b;
}
double norm_angle(double a) {
  a = std::fmod(a, kTau);
  if (a < 0) {
    a += kTau;
  }
  return a;
}
double wrap(double d) {
  while (d > kPi) {
    d -= kTau;
  }
  while (d <= -kPi) {
    d += kTau;
  }
  return d;
}
double hyp(double a, double b) {
  return base::js_hypot(a, b);
}
double num(const Value& v) {
  return base::js_to_number(v);
}
std::string type_of(const Value& e) {
  const Value t = e.get("type");
  return t.is_string() ? t.as_string() : std::string();
}
std::string key_of(double id, std::string_view f) {
  std::string s;
  base::append_number(s, id);
  s += '|';
  s += f;
  return s;
}

const std::vector<std::string>& fixed_fields(std::string_view type) {
  static const std::vector<std::string> kLine{"x1", "y1", "x2", "y2"};
  static const std::vector<std::string> kCircle{"cx", "cy", "r"};
  static const std::vector<std::string> kArc{"cx", "cy", "r", "a0", "a1"};
  static const std::vector<std::string> kPoint{"x", "y"};
  static const std::vector<std::string> kNone;
  if (type == "line") {
    return kLine;
  }
  if (type == "circle") {
    return kCircle;
  }
  if (type == "arc") {
    return kArc;
  }
  if (type == "point") {
    return kPoint;
  }
  return kNone;
}

const base::Array* pts_of(const Value& e) {
  const Value* p = e.find("pts");
  return p != nullptr && p->is_array() ? &p->as_array() : nullptr;
}
std::size_t n_pts(const Value& e) {
  const auto* p = pts_of(e);
  return p != nullptr ? p->size() : 0;
}

// ---------------------------------------------------------------- the system

struct Ent {
  double id = 0;
  Value e;
  std::string type;
  std::vector<std::string> fields;
  std::vector<double> raw;           // each field's value now
  std::vector<std::ptrdiff_t> vars;  // its variable, or -1 (pinned)
  std::vector<double> bulge;         // poly: b || 0 per vertex
  std::size_t n = 0;                 // poly: vertices
};

struct RK {
  std::size_t ent = 0;
  std::string k;
  double idx = 0;  // +k.slice(1)
  char kind = 0;
};

struct Circ {
  V2 c{};
  double r = 0;
};

struct Eq {
  std::string cid;
  std::function<double()> f;
  const std::vector<std::size_t>* deps = nullptr;
};

class System {
 public:
  std::vector<std::pair<std::string, std::size_t>> var_list;  // key → index, in order
  std::unordered_map<std::string, std::size_t> var_index;
  std::vector<double> x;
  std::vector<Ent> ents;
  std::unordered_map<double, std::size_t> ent_index;
  std::vector<Eq> eqs;
  std::deque<std::vector<std::size_t>> deps;  // stable addresses for Eq::deps

  System(const base::Array& constraints, const Getter& get, const std::set<std::string>* pinned) {
    for (const Value& c : constraints) {
      const Value refs = c.get("refs");
      if (!refs.is_array()) {
        continue;
      }
      for (const Value& r : refs.as_array()) {
        const double id = ref_id(r);
        if (ent_index.contains(id)) {
          continue;
        }
        const Value* e = get(id);
        if (e == nullptr) {
          continue;
        }
        Ent en;
        en.id = id;
        en.e = *e;
        en.type = type_of(*e);
        en.fields = fields_of(*e);
        en.n = n_pts(*e);
        if (en.type == "poly") {
          for (const Value& p : *pts_of(*e)) {
            en.bulge.push_back(jor(num(p.get("b")), 0));
          }
        }
        for (const std::string& f : en.fields) {
          en.raw.push_back(get_field(*e, f));
          const std::string key = key_of(id, f);
          if (pinned != nullptr && pinned->contains(key)) {
            en.vars.push_back(-1);
            continue;
          }
          en.vars.push_back(static_cast<std::ptrdiff_t>(x.size()));
          var_index.emplace(key, x.size());
          var_list.emplace_back(key, x.size());
          x.push_back(en.raw.back());
        }
        ent_index.emplace(id, ents.size());
        ents.push_back(std::move(en));
      }
    }
    for (const Value& c : constraints) {
      equations_for(c);
    }
  }

  // ---- geometry over the current values (geo() in the JS)
  [[nodiscard]] double V(const Ent& en, std::string_view f) const {
    for (std::size_t j = 0; j < en.fields.size(); ++j) {
      if (en.fields[j] == f) {
        return en.vars[j] >= 0 ? x[static_cast<std::size_t>(en.vars[j])] : en.raw[j];
      }
    }
    return get_field(en.e, f);  // a field the entity does not have: getField
  }
  [[nodiscard]] double Vi(const Ent& en, std::size_t j) const {
    return en.vars[j] >= 0 ? x[static_cast<std::size_t>(en.vars[j])] : en.raw[j];
  }
  /// poly vertex i ("<i>x", "<i>y"); a vertex past the end reads as NaN
  [[nodiscard]] V2 pt(const Ent& en, double i) const {
    if (!(i >= 0) || i >= static_cast<double>(en.n) || i != std::floor(i)) {
      return {kNaN, kNaN};
    }
    const auto k = static_cast<std::size_t>(i);
    return {Vi(en, 2 * k), Vi(en, 2 * k + 1)};
  }
  [[nodiscard]] Circ bulge_circle(const Ent& en, double i) const {
    const auto n = static_cast<double>(en.n);
    const V2 p = pt(en, i);
    const V2 q = pt(en, std::fmod(i + 1, n));
    const double b = i >= 0 && i < n && i == std::floor(i) ? en.bulge[static_cast<std::size_t>(i)] : 0;
    const double th = 4 * base::js_atan(b);
    const double cx = q[0] - p[0];
    const double cy = q[1] - p[1];
    const double c = jor(hyp(cx, cy), 1e-300);
    const double r = std::abs(c / (2 * base::js_sin(th / 2)));
    const double h = r * base::js_cos(th / 2) * (b > 0 ? 1 : -1);
    return {.c = {(p[0] + q[0]) / 2 - (cy / c) * h, (p[1] + q[1]) / 2 + (cx / c) * h}, .r = r};
  }
  [[nodiscard]] V2 point(const RK& r) const {
    const Ent& en = ents[r.ent];
    if (en.type == "line") {
      const V2 a{V(en, "x1"), V(en, "y1")};
      const V2 b{V(en, "x2"), V(en, "y2")};
      if (r.k == "p1") {
        return a;
      }
      if (r.k == "p2") {
        return b;
      }
      return {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
    }
    if (en.type == "circle") {
      return {V(en, "cx"), V(en, "cy")};
    }
    if (en.type == "arc") {
      const double cx = V(en, "cx");
      const double cy = V(en, "cy");
      const double rr = V(en, "r");
      const double a0 = V(en, "a0");
      const double a1 = V(en, "a1");
      if (r.k == "c") {
        return {cx, cy};
      }
      double a = a0 + norm_angle(a1 - a0) / 2;
      if (r.k == "s") {
        a = a0;
      } else if (r.k == "e") {
        a = a1;
      }
      return {cx + rr * base::js_cos(a), cy + rr * base::js_sin(a)};
    }
    if (en.type == "point") {
      return {V(en, "x"), V(en, "y")};
    }
    if (en.type == "poly") {
      return pt(en, r.idx);
    }
    return {0, 0};
  }
  [[nodiscard]] std::array<V2, 2> line(const RK& r) const {
    const Ent& en = ents[r.ent];
    if (en.type == "line") {
      return {V2{V(en, "x1"), V(en, "y1")}, V2{V(en, "x2"), V(en, "y2")}};
    }
    const double i = r.idx;
    if (en.type != "poly") {  // pt(i) of something else: fields it does not have
      return {V2{kNaN, kNaN}, V2{kNaN, kNaN}};
    }
    return {pt(en, i), pt(en, std::fmod(i + 1, static_cast<double>(en.n)))};
  }
  [[nodiscard]] Circ circle(const RK& r) const {
    const Ent& en = ents[r.ent];
    if (en.type == "poly") {
      return bulge_circle(en, r.idx);
    }
    return {.c = {V(en, "cx"), V(en, "cy")}, .r = V(en, "r")};
  }
  /// fieldValue(g, e, f): one raw field through the geometry reader
  [[nodiscard]] double field_value(std::size_t ent, std::string_view f) const {
    const Ent& en = ents[ent];
    const RK whole{.ent = ent, .k = "L", .idx = 0, .kind = 'L'};
    if (en.type == "line") {
      const auto l = line(whole);
      if (f == "x1") {
        return l[0][0];
      }
      if (f == "y1") {
        return l[0][1];
      }
      if (f == "x2") {
        return l[1][0];
      }
      return f == "y2" ? l[1][1] : kNaN;
    }
    if (en.type == "circle" || en.type == "arc") {
      const Circ k = circle(RK{.ent = ent, .k = "R", .idx = 0, .kind = 'R'});
      if (f == "cx") {
        return k.c[0];
      }
      if (f == "cy") {
        return k.c[1];
      }
      if (f == "r") {
        return k.r;
      }
      if (en.type == "circle") {
        return kNaN;
      }
      // angles: from the end points (the centre and radius already come from their own fields)
      const V2 p = point(RK{.ent = ent, .k = f == "a0" ? "s" : "e", .idx = 0, .kind = 'P'});
      const double ref = num(en.e.get(f == "a0" ? "a0" : "a1"));
      return ref + wrap(base::js_atan2(p[1] - k.c[1], p[0] - k.c[0]) - ref);
    }
    if (en.type == "point") {
      const V2 p = point(RK{.ent = ent, .k = "p", .idx = 0, .kind = 'P'});
      return f == "x" ? p[0] : p[1];
    }
    if (en.type == "poly") {
      const double i = base::js_parse_int(f);
      const V2 p = pt(en, i);
      return f.ends_with('x') ? p[0] : p[1];
    }
    return 0;
  }

 private:
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) — equationsFor, one case per constraint type
  void equations_for(const Value& c) {
    const Value refs_v = c.get("refs");
    if (!refs_v.is_array()) {
      return;
    }
    std::vector<RK> r;
    for (const Value& q : refs_v.as_array()) {
      const auto it = ent_index.find(ref_id(q));
      if (it == ent_index.end()) {
        return;
      }
      const Value k = q.get("k");
      const auto kind = ref_kind(&ents[it->second].e, k);
      if (!kind) {
        return;
      }
      const std::string& ks = k.as_string();
      r.push_back({.ent = it->second, .k = ks, .idx = base::js_number(std::string_view(ks).substr(1)), .kind = *kind});
    }
    std::vector<std::size_t> dep_list;
    {
      std::unordered_set<std::size_t> seen;
      for (const RK& q : r) {
        for (const std::ptrdiff_t v : ents[q.ent].vars) {
          if (v >= 0 && seen.insert(static_cast<std::size_t>(v)).second) {
            dep_list.push_back(static_cast<std::size_t>(v));
          }
        }
      }
    }
    deps.push_back(std::move(dep_list));
    const std::vector<std::size_t>* dp = &deps.back();
    const std::string cid = base::js_string(c.get("id"));
    const auto push = [&](std::function<double()> f) { eqs.push_back({.cid = cid, .f = std::move(f), .deps = dp}); };
    const System* s = this;
    // the references, shared by the equations (a shared_ptr copies without throwing)
    const auto rp = std::make_shared<const std::vector<RK>>(std::move(r));
    const auto P = [s, rp](std::size_t i) { return s->point((*rp)[i]); };
    const auto L = [s, rp](std::size_t i) { return s->line((*rp)[i]); };
    const auto C = [s, rp](std::size_t i) { return s->circle((*rp)[i]); };
    const auto K = [rp](std::size_t i) { return (*rp)[i].kind; };
    const auto dir = [L](std::size_t i) {
      const auto l = L(i);
      return V2{l[1][0] - l[0][0], l[1][1] - l[0][1]};
    };
    const double sg = jor(num(c.get("sign")), 1);
    const std::string type = base::js_string(c.get("type"));
    const double value = num(c.get("value"));
    if (type == "coincident") {
      const char k2 = K(1);
      if (k2 == 'P') {
        push([P] { return P(0)[0] - P(1)[0]; });
        push([P] { return P(0)[1] - P(1)[1]; });
      } else if (k2 == 'L') {
        push([P, L] { return sd(P(0), L(1)); });
      } else {
        push([P, C] {
          const Circ k = C(1);
          const V2 p = P(0);
          return hyp(p[0] - k.c[0], p[1] - k.c[1]) - k.r;
        });
      }
    } else if (type == "concentric") {
      push([C] { return C(0).c[0] - C(1).c[0]; });
      push([C] { return C(0).c[1] - C(1).c[1]; });
    } else if (type == "collinear") {
      push([L] { return sd(L(1)[0], L(0)); });
      push([L] { return sd(L(1)[1], L(0)); });
    } else if (type == "fix") {
      if (K(0) == 'P') {
        const Value v = c.get("value");
        const bool has = v.truthy();
        const double v0 = has ? num(at(v, 0)) : 0;
        const double v1 = has ? num(at(v, 1)) : 0;
        push([P, v0] { return P(0)[0] - v0; });
        push([P, v1] { return P(0)[1] - v1; });
      } else {
        const Value fx = c.get("fixed");
        const std::size_t ent = (*rp)[0].ent;
        for (std::size_t j = 0; j < ents[ent].fields.size(); ++j) {
          const Value fv = fx.is_object() && !fx.is_point() ? fx.get(ents[ent].fields[j]) : Value();
          if (!fv.is_number()) {
            continue;
          }
          const double want = fv.as_number();
          push([s, ent, j, want] { return s->field_value(ent, s->ents[ent].fields[j]) - want; });
        }
      }
    } else if (type == "parallel") {
      push([dir] {
        const V2 u = dir(0);
        const V2 v = dir(1);
        return cross(u, v) / jor(len(u) * len(v), 1e-300);
      });
    } else if (type == "perpendicular") {
      push([dir] {
        const V2 u = dir(0);
        const V2 v = dir(1);
        return dot(u, v) / jor(len(u) * len(v), 1e-300);
      });
    } else if (type == "horizontal" || type == "vertical") {
      const std::size_t k = type == "horizontal" ? 1 : 0;
      if (K(0) == 'L') {
        push([L, k] {
          const auto l = L(0);
          return l[1][k] - l[0][k];
        });
      } else {
        push([P, k] { return P(1)[k] - P(0)[k]; });
      }
    } else if (type == "tangent") {
      if (K(0) == 'L') {
        push([L, C, sg] { return sg * sd(C(1).c, L(0)) - C(1).r; });
      } else {
        const bool inside = c.get("kind") == Value("int");
        push([C, sg, inside] {
          const Circ a = C(0);
          const Circ b = C(1);
          const double d = hyp(b.c[0] - a.c[0], b.c[1] - a.c[1]);
          return inside ? d - sg * (a.r - b.r) : d - (a.r + b.r);
        });
      }
    } else if (type == "equal") {
      if (K(0) == 'L') {
        push([dir] { return len(dir(0)) - len(dir(1)); });
      } else {
        push([C] { return C(0).r - C(1).r; });
      }
    } else if (type == "symmetric") {
      // two points (or the matching ends of two lines, or two centres) mirror each other across line refs[2]
      const char mode = K(0);
      const bool sw = num(c.get("pair")) == 1;
      // the pair's two points: 'P' P(0), P(1); 'L' the matching ends of the two lines; 'R' the two centres
      const auto pair_of = [P, L, C, mode, sw](std::size_t k) -> std::array<V2, 2> {
        if (mode == 'P') {
          return {P(0), P(1)};
        }
        if (mode == 'L') {
          const std::size_t other = sw ? 1 - k : k;
          return {L(0)[k], L(1)[other]};
        }
        return {C(0).c, C(1).c};
      };
      for (std::size_t k = 0; k < (mode == 'L' ? 2U : 1U); ++k) {
        push([pair_of, L, k] {
          const auto [a, b] = pair_of(k);
          return sd({(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}, L(2));
        });
        push([pair_of, dir, k] {
          const auto [a, b] = pair_of(k);
          const V2 u = dir(2);
          return dot({b[0] - a[0], b[1] - a[1]}, u) / jor(len(u), 1e-300);
        });
      }
      if (K(0) == 'R') {
        push([C] { return C(0).r - C(1).r; });
      }
    } else if (type == "hdist" || type == "vdist") {
      const std::size_t k = type == "hdist" ? 0 : 1;
      push([P, sg, k, value] { return sg * (P(1)[k] - P(0)[k]) - value; });
    } else if (type == "dist") {
      if (K(1) == 'P') {
        push([P, value] {
          const V2 a = P(0);
          const V2 b = P(1);
          return hyp(b[0] - a[0], b[1] - a[1]) - value;
        });
      } else if (K(0) == 'P') {
        push([P, L, sg, value] { return sg * sd(P(0), L(1)) - value; });
      } else {
        push([L, sg, value] { return sg * sd(L(1)[0], L(0)) - value; });
      }
    } else if (type == "radius") {
      push([C, value] { return C(0).r - value; });
    } else if (type == "diameter") {
      push([C, value] { return 2 * C(0).r - value; });
    } else if (type == "angle") {
      if (K(0) == 'R') {
        const std::size_t ent = (*rp)[0].ent;
        push([s, ent, value] {
          return wrap(s->field_value(ent, "a1") - s->field_value(ent, "a0") - value * kPi / 180);
        });
      } else {
        const Value dirs = c.get("dirs");
        const double s1 = dirs.truthy() ? num(at(dirs, 0)) : 1;
        const double s2 = dirs.truthy() ? num(at(dirs, 1)) : 1;
        push([dir, s1, s2, sg, value] {
          const V2 d0 = dir(0);
          const V2 d1 = dir(1);
          const V2 u{d0[0] * s1, d0[1] * s1};
          const V2 v{d1[0] * s2, d1[1] * s2};
          return wrap(base::js_atan2(cross(u, v), dot(u, v)) - sg * value * kPi / 180);
        });
      }
    }
  }

 public:
  static double cross(V2 u, V2 v) { return u[0] * v[1] - u[1] * v[0]; }
  static double dot(V2 u, V2 v) { return u[0] * v[0] + u[1] * v[1]; }
  static double len(V2 u) { return hyp(u[0], u[1]); }
  /// signed distance of p from the infinite line a→b (left positive)
  static double sd(V2 p, const std::array<V2, 2>& l) {
    const V2 u{l[1][0] - l[0][0], l[1][1] - l[0][1]};
    return cross(u, {p[0] - l[0][0], p[1] - l[0][1]}) / jor(len(u), 1e-300);
  }
};

// ---------------------------------------------------------------- numerics (as partcraft/dev/solver.js)

/// A Jacobian row: variable → derivative, in the order met (a Map)
using Row = std::vector<std::pair<std::size_t, double>>;

std::vector<double> residuals(const std::vector<Eq>& eqs) {
  std::vector<double> r;
  r.reserve(eqs.size());
  for (const Eq& q : eqs) {
    r.push_back(q.f());
  }
  return r;
}
double norm2(const std::vector<double>& r) {
  double s = 0;
  for (const double v : r) {
    s += v * v;
  }
  return s;
}

std::vector<Row> jacobian(const std::vector<Eq>& eqs, std::vector<double>& x) {
  std::vector<Row> out;
  out.reserve(eqs.size());
  for (const Eq& q : eqs) {
    Row row;
    for (const std::size_t k : *q.deps) {
      const double x0 = x[k];
      const double h = 1e-6 * jmax(1, std::abs(x0));
      x[k] = x0 + h;
      const double fp = q.f();
      x[k] = x0 - h;
      const double fm = q.f();
      x[k] = x0;
      const double d = (fp - fm) / (2 * h);
      if (d != 0 && std::isfinite(d)) {
        row.emplace_back(k, d);
      }
    }
    out.push_back(std::move(row));
  }
  return out;
}

/// sparseDot(a, b): over the smaller row, looking each up in the other
double sparse_dot(const Row& a0, const Row& b0, const std::vector<std::unordered_map<std::size_t, double>>& lookup,
                  std::size_t ia, std::size_t ib) {
  const bool swap = a0.size() > b0.size();
  const Row& a = swap ? b0 : a0;
  const auto& b = lookup[swap ? ia : ib];
  double s = 0;
  for (const auto& [k, v] : a) {
    if (const auto it = b.find(k); it != b.end()) {
      s += v * it->second;
    }
  }
  return s;
}

std::optional<std::vector<double>> chol_solve(const std::vector<std::vector<double>>& A, const std::vector<double>& b) {
  const std::size_t n = b.size();
  std::vector<std::vector<double>> L = A;
  for (std::size_t j = 0; j < n; ++j) {
    double s = L[j][j];
    for (std::size_t k = 0; k < j; ++k) {
      s -= L[j][k] * L[j][k];
    }
    if (!(s > 0)) {
      return std::nullopt;
    }
    const double d = std::sqrt(s);
    L[j][j] = d;
    for (std::size_t i = j + 1; i < n; ++i) {
      double t = L[i][j];
      for (std::size_t k = 0; k < j; ++k) {
        t -= L[i][k] * L[j][k];
      }
      L[i][j] = t / d;
    }
  }
  std::vector<double> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    double s = b[i];
    for (std::size_t k = 0; k < i; ++k) {
      s -= L[i][k] * y[k];
    }
    y[i] = s / L[i][i];
  }
  std::vector<double> z(n);
  for (std::size_t ii = n; ii-- > 0;) {
    double s = y[ii];
    for (std::size_t k = ii + 1; k < n; ++k) {
      s -= L[k][ii] * z[k];
    }
    z[ii] = s / L[ii][ii];
  }
  return z;
}

struct NewtonResult {
  bool ok = false;
  double it = 0;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — newton(), as the JS
NewtonResult newton(const std::vector<Eq>& eqs, std::vector<double>& x, double tol, int max_iter) {
  std::vector<double> r = residuals(eqs);
  double f = norm2(r);
  double mu = 1e-12;
  int it = 0;
  const auto met = [&r, tol] { return std::ranges::all_of(r, [tol](double v) { return std::abs(v) <= tol; }); };
  for (; it < max_iter; ++it) {
    if (met()) {
      return {.ok = true, .it = static_cast<double>(it)};
    }
    if (x.empty()) {
      break;
    }
    const std::vector<Row> J = jacobian(eqs, x);
    const std::size_t m = J.size();
    std::vector<std::unordered_map<std::size_t, double>> lookup(m);
    for (std::size_t i = 0; i < m; ++i) {
      for (const auto& [k, v] : J[i]) {
        lookup[i].emplace(k, v);
      }
    }
    std::vector<std::vector<double>> A(m, std::vector<double>(m, 0));
    double scale = 0;
    for (std::size_t i = 0; i < m; ++i) {
      for (std::size_t j = 0; j <= i; ++j) {
        const double v = sparse_dot(J[i], J[j], lookup, i, j);
        A[i][j] = v;
        if (j < i) {
          A[j][i] = v;
        }
      }
      scale = jmax(scale, A[i][i]);
    }
    bool improved = false;
    for (int tries = 0; tries < 12 && !improved; ++tries) {
      std::vector<std::vector<double>> B = A;
      for (std::size_t i = 0; i < m; ++i) {
        B[i][i] = A[i][i] + mu * jmax(scale, 1e-12) + 1e-300;
      }
      std::vector<double> neg(m);
      for (std::size_t i = 0; i < m; ++i) {
        neg[i] = -r[i];
      }
      const auto y = chol_solve(B, neg);
      if (!y) {
        mu *= 100;
        continue;
      }
      std::vector<double> dx(x.size(), 0);
      for (std::size_t i = 0; i < m; ++i) {
        for (const auto& [k, v] : J[i]) {
          dx[k] += v * (*y)[i];
        }
      }
      const std::vector<double> x0 = x;
      double alpha = 1;
      for (int ls = 0; ls < 20; ++ls) {
        for (std::size_t k = 0; k < x.size(); ++k) {
          x[k] = x0[k] + alpha * dx[k];
        }
        std::vector<double> r2 = residuals(eqs);
        const double f2 = norm2(r2);
        if (f2 < f) {
          r = std::move(r2);
          f = f2;
          improved = true;
          break;
        }
        alpha /= 2;
      }
      if (!improved) {
        x = x0;
        mu *= 100;
        if (mu > 1e6) {
          break;
        }
      } else {
        mu = jmax(1e-12, mu / 10);
      }
    }
    if (!improved) {
      return {.ok = met(), .it = static_cast<double>(it)};
    }
  }
  return {.ok = met(), .it = static_cast<double>(it)};
}

/// extentOf(ents): the size of the geometry (for tolerances)
double extent_of(const std::vector<Ent>& ents) {
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = min_x;
  double max_x = -min_x;
  double max_y = -min_x;
  const auto grow = [&](double x, double y) {
    if (std::isfinite(x) && std::isfinite(y)) {
      min_x = jmin(min_x, x);
      max_x = jmax(max_x, x);
      min_y = jmin(min_y, y);
      max_y = jmax(max_y, y);
    }
  };
  for (const Ent& en : ents) {
    const Value& e = en.e;
    if (en.type == "line") {
      grow(num(e.get("x1")), num(e.get("y1")));
      grow(num(e.get("x2")), num(e.get("y2")));
    } else if (en.type == "circle" || en.type == "arc") {
      const double cx = num(e.get("cx"));
      const double cy = num(e.get("cy"));
      const double r = num(e.get("r"));
      grow(cx - r, cy - r);
      grow(cx + r, cy + r);
    } else if (en.type == "point") {
      grow(num(e.get("x")), num(e.get("y")));
    } else if (en.type == "poly") {
      for (const Value& p : *pts_of(e)) {
        grow(num(p.get("x")), num(p.get("y")));
      }
    }
  }
  if (!std::isfinite(min_x)) {
    return 1;
  }
  double m = jmax(1, max_x - min_x);
  for (const double v : {max_y - min_y, std::abs(min_x), std::abs(max_x), std::abs(min_y), std::abs(max_y)}) {
    m = jmax(m, v);
  }
  return m;
}
double tolerance(const std::vector<Ent>& ents) {
  return 1e-9 * extent_of(ents);
}

}  // namespace

// ---------------------------------------------------------------- entities and references

bool solvable(const Value& e) {
  const std::string t = type_of(e);
  return std::ranges::find(kSolvable, t) != kSolvable.end();
}

std::vector<std::string> fields_of(const Value& e) {
  const std::string t = type_of(e);
  if (t == "poly") {
    std::vector<std::string> out;
    const std::size_t n = n_pts(e);
    for (std::size_t i = 0; i < n; ++i) {
      out.push_back(std::to_string(i) + "x");
      out.push_back(std::to_string(i) + "y");
    }
    return out;
  }
  return fixed_fields(t);
}

double get_field(const Value& e, std::string_view f) {
  if (type_of(e) == "poly") {
    const double i = base::js_parse_int(f);
    const auto* pts = pts_of(e);
    if (pts == nullptr || !(i >= 0) || i >= static_cast<double>(pts->size())) {
      return kNaN;
    }
    return num((*pts)[static_cast<std::size_t>(i)].get(f.ends_with('x') ? "x" : "y"));
  }
  const Value* v = e.find(f);
  return v != nullptr ? num(*v) : kNaN;
}

Value with_fields(const Value& e, const std::vector<std::pair<std::string, double>>& vals) {
  const auto val = [&vals](std::string_view f) -> std::optional<double> {
    for (const auto& [k, v] : vals) {
      if (k == f) {
        return v;
      }
    }
    return std::nullopt;
  };
  const std::string t = type_of(e);
  if (t == "poly") {
    Value out = e;
    base::Array pts;
    std::size_t i = 0;
    for (const Value& p : *pts_of(e)) {
      const auto x = val(std::to_string(i) + "x");
      const auto y = val(std::to_string(i) + "y");
      if (!x && !y) {
        pts.push_back(p);
      } else {
        Value q = p;
        q.set("x", x ? Value(*x) : p.get("x"));
        q.set("y", y ? Value(*y) : p.get("y"));
        pts.push_back(std::move(q));
      }
      ++i;
    }
    out.set("pts", Value(std::move(pts)));
    return out;
  }
  Value out = e;
  for (const auto& [f, v] : vals) {
    out.set(f, v);
  }
  if ((t == "circle" || t == "arc") && num(out.get("r")) < 0) {
    out.set("r", -num(out.get("r")));
    if (t == "arc") {
      out.set("a0", num(out.get("a0")) + kPi);
      out.set("a1", num(out.get("a1")) + kPi);
    }
  }
  if (t == "arc") {
    out.set("a0", norm_angle(num(out.get("a0"))));
    out.set("a1", norm_angle(num(out.get("a1"))));
  }
  return out;
}

double seg_count(const Value& e) {
  const std::size_t n = n_pts(e);
  if (n < 2) {
    return 0;
  }
  return static_cast<double>(e.get("closed").truthy() ? n : n - 1);
}

std::optional<char> ref_kind(const Value* e, const Value& k) {
  if (e == nullptr || !k.is_string()) {
    return std::nullopt;
  }
  const std::string& s = k.as_string();
  if (s == "*") {
    return solvable(*e) ? std::optional('E') : std::nullopt;
  }
  const std::string t = type_of(*e);
  if (t == "line") {
    if (s == "L") {
      return 'L';
    }
    return s == "p1" || s == "p2" || s == "mid" ? std::optional('P') : std::nullopt;
  }
  if (t == "circle") {
    if (s == "R") {
      return 'R';
    }
    return s == "c" ? std::optional('P') : std::nullopt;
  }
  if (t == "arc") {
    if (s == "R") {
      return 'R';
    }
    return s == "c" || s == "s" || s == "e" || s == "mid" ? std::optional('P') : std::nullopt;
  }
  if (t == "point") {
    return s == "p" ? std::optional('P') : std::nullopt;
  }
  if (t == "poly") {
    // /^([vs])(\d{1,6})$/
    if (s.size() < 2 || s.size() > 7 || (s[0] != 'v' && s[0] != 's') ||
        !std::ranges::all_of(s.substr(1), [](char ch) { return ch >= '0' && ch <= '9'; })) {
      return std::nullopt;
    }
    const double i = base::js_number(std::string_view(s).substr(1));
    const auto* pts = pts_of(*e);
    if (s[0] == 'v') {
      return pts != nullptr && i < static_cast<double>(pts->size()) ? std::optional('P') : std::nullopt;
    }
    if (i >= seg_count(*e)) {
      return std::nullopt;
    }
    return std::abs(jor(num((*pts)[static_cast<std::size_t>(i)].get("b")), 0)) < 1e-12 ? 'L' : 'R';
  }
  return std::nullopt;
}

double ref_id(const Value& ref) {
  return num(ref.get("e"));
}

namespace {
/// a one-entity system over `e` as it is (refPoint, refLine, refCircle read through the same geometry)
struct Single {
  System sys;
  explicit Single(const Value& e)
      : sys(
            base::Array{base::make_object(
                {{"id", "k0"}, {"type", "?"}, {"refs", base::Array{base::make_object({{"e", 0}, {"k", "*"}})}}})},
            [&e](double) { return &e; }, nullptr) {}
  [[nodiscard]] RK rk(const Value& k) const {
    const std::string ks = k.is_string() ? k.as_string() : std::string();
    return {.ent = 0, .k = ks, .idx = base::js_number(std::string_view(ks).substr(ks.empty() ? 0 : 1)), .kind = 0};
  }
};
}  // namespace

std::optional<RefPoint> ref_point(const Value& e, const Value& k) {
  const auto kind = ref_kind(&e, k);
  if (!kind) {
    return std::nullopt;
  }
  const Single g(e);
  if (*kind == 'P') {
    const V2 p = g.sys.point(g.rk(k));
    return RefPoint{p[0], p[1]};
  }
  if (*kind == 'L') {
    const auto l = g.sys.line(g.rk(k));
    return RefPoint{(l[0][0] + l[1][0]) / 2, (l[0][1] + l[1][1]) / 2};
  }
  if (*kind == 'R') {
    const Circ c = g.sys.circle(g.rk(k));
    return RefPoint{c.c[0], c.c[1]};
  }
  return std::nullopt;
}
RefLine ref_line(const Value& e, const Value& k) {
  const Single g(e);
  const auto l = g.sys.line(g.rk(k));
  return {l[0][0], l[0][1], l[1][0], l[1][1]};
}
RefCircle ref_circle(const Value& e, const Value& k) {
  const Single g(e);
  const Circ c = g.sys.circle(g.rk(k));
  return {c.c[0], c.c[1], c.r};
}

// ---------------------------------------------------------------- solving and analysing

SolveResult solve_system(const base::Array& constraints, const Getter& get, const std::set<std::string>* pinned,
                         int max_iter) {
  System sys(constraints, get, pinned);
  const double tol = tolerance(sys.ents);
  const NewtonResult res = newton(sys.eqs, sys.x, tol, max_iter);
  SolveResult out;
  out.ok = res.ok;
  out.iterations = res.it;
  if (res.ok) {
    for (const Ent& en : sys.ents) {
      std::vector<std::pair<std::string, double>> vals;
      bool moved = false;
      for (std::size_t j = 0; j < en.fields.size(); ++j) {
        if (en.vars[j] < 0) {
          continue;
        }
        const double v = sys.x[static_cast<std::size_t>(en.vars[j])];
        vals.emplace_back(en.fields[j], v);
        const double was = en.raw[j];
        if (std::abs(v - was) > 1e-12 * jmax(1, std::abs(was))) {
          moved = true;
        }
      }
      if (moved) {
        out.updates.emplace_back(en.id, with_fields(en.e, vals));
      }
    }
  }
  const std::vector<double> resid = residuals(sys.eqs);
  out.residual = std::sqrt(norm2(resid));
  for (std::size_t i = 0; i < resid.size(); ++i) {
    if (!(std::abs(resid[i]) <= tol * 100) && std::ranges::find(out.worst, sys.eqs[i].cid) == out.worst.end()) {
      out.worst.push_back(sys.eqs[i].cid);
    }
  }
  return out;
}

bool residual_ok(const base::Array& constraints, const Getter& get) {
  const System sys(constraints, get, nullptr);
  const double tol = tolerance(sys.ents) * 1000;
  return std::ranges::all_of(sys.eqs, [tol](const Eq& q) { return std::abs(q.f()) <= tol; });
}

Analysis analyze(const base::Array& constraints, const Getter& get) {
  System sys(constraints, get, nullptr);
  const double tol = tolerance(sys.ents);
  const std::vector<Row> J = jacobian(sys.eqs, sys.x);
  const std::size_t n = sys.x.size();
  std::vector<std::vector<double>> Q;
  Analysis out;
  const auto add = [](std::vector<std::string>& set, const std::string& id) {
    if (std::ranges::find(set, id) == set.end()) {
      set.push_back(id);
    }
  };
  for (std::size_t i = 0; i < sys.eqs.size(); ++i) {
    const Eq& q = sys.eqs[i];
    if (std::abs(q.f()) > tol * 1000) {
      add(out.conflicts, q.cid);
    }
    std::vector<double> v(n, 0);
    double vn = 0;
    for (const auto& [k, d] : J[i]) {
      v[k] = d;
      vn += d * d;
    }
    vn = std::sqrt(vn);
    if (vn < 1e-12) {
      add(out.redundant, q.cid);
      continue;
    }
    for (double& w : v) {
      w /= vn;
    }
    for (int pass = 0; pass < 2; ++pass) {
      for (const std::vector<double>& b : Q) {
        double d = 0;
        for (std::size_t k = 0; k < n; ++k) {
          d += b[k] * v[k];
        }
        if (d != 0) {
          for (std::size_t k = 0; k < n; ++k) {
            v[k] -= d * b[k];
          }
        }
      }
    }
    double wn = 0;
    for (const double w : v) {
      wn += w * w;
    }
    wn = std::sqrt(wn);
    if (wn < 1e-7) {
      add(out.redundant, q.cid);
      continue;
    }
    for (double& w : v) {
      w /= wn;
    }
    Q.push_back(std::move(v));
  }
  std::vector<double> fixed_var(n, 0);
  for (const std::vector<double>& b : Q) {
    for (std::size_t k = 0; k < n; ++k) {
      fixed_var[k] += b[k] * b[k];
    }
  }
  for (const Ent& en : sys.ents) {
    const bool all = std::ranges::all_of(
        en.vars, [&fixed_var](std::ptrdiff_t k) { return k < 0 || fixed_var[static_cast<std::size_t>(k)] > 1 - 1e-6; });
    if (all) {
      out.determined.push_back(en.id);
    }
  }
  out.vars = static_cast<double>(n);
  out.rank = static_cast<double>(Q.size());
  out.dof = out.vars - out.rank;
  return out;
}

std::vector<base::Array> components(const base::Array& constraints) {
  std::unordered_map<double, double> parent;
  const auto find = [&parent](double a) {
    while (parent[a] != a) {
      parent[a] = parent[parent[a]];
      a = parent[a];
    }
    return a;
  };
  for (const Value& c : constraints) {
    const Value refs = c.get("refs");
    if (refs.is_array()) {
      for (const Value& r : refs.as_array()) {
        parent.try_emplace(ref_id(r), ref_id(r));
      }
    }
  }
  for (const Value& c : constraints) {
    const Value refs = c.get("refs");
    if (!refs.is_array()) {
      continue;
    }
    const auto& rs = refs.as_array();
    for (std::size_t i = 1; i < rs.size(); ++i) {
      const double ra = find(ref_id(rs[0]));
      const double rb = find(ref_id(rs[i]));
      if (ra != rb) {
        parent[ra] = rb;
      }
    }
  }
  std::vector<std::pair<double, base::Array>> groups;
  for (const Value& c : constraints) {
    const Value refs = c.get("refs");
    if (!refs.is_array() || refs.as_array().empty()) {
      continue;
    }
    const double k = find(ref_id(refs.as_array().front()));
    auto it = std::ranges::find_if(groups, [k](const auto& g) { return g.first == k; });
    if (it == groups.end()) {
      groups.emplace_back(k, base::Array{});
      it = std::prev(groups.end());
    }
    it->second.push_back(c);
  }
  std::vector<base::Array> out;
  out.reserve(groups.size());
  for (auto& [k, g] : groups) {
    out.push_back(std::move(g));
  }
  return out;
}

}  // namespace cc::cad::gsolve
