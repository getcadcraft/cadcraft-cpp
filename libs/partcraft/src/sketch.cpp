// SPDX-License-Identifier: Apache-2.0
// sketch.js (PartCraft 0.5): the constrained 2D sketch — lookup, building, relations, measuring, removal, fillets,
// profiles and the file reader. The equations are sketch_eq.cpp, the solver solver.cpp.
#include "cc/partcraft/sketch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsx.hpp"
#include "sketch_js.hpp"

namespace cc::partcraft::sketch {
namespace detail {

const std::vector<Def>& defs() {
  static const std::vector<Def> d = {
      {"coincident", "model.sketch.rel.coincident", {"PP", "PL", "PR"}},
      {"horizontal", "model.sketch.rel.horizontal", {"L", "PP"}},
      {"vertical", "model.sketch.rel.vertical", {"L", "PP"}},
      {"parallel", "model.sketch.rel.parallel", {"LL"}},
      {"perpendicular", "model.sketch.rel.perpendicular", {"LL"}},
      {"tangent", "model.sketch.rel.tangent", {"LR", "RR", "LS", "RS", "SS"}},
      {"equal", "model.sketch.rel.equal", {"LL", "RR"}},
      {"concentric", "model.sketch.rel.concentric", {"RR", "PR"}},
      {"midpoint", "model.sketch.rel.midpoint", {"PL"}},
      {"fix", "model.sketch.rel.fix", {"P", "L", "R", "S"}},
      {"symmetric", "model.sketch.rel.symmetric", {"PPL"}},
      {"offsetlink", "model.sketch.rel.offsetlink", {"LL", "RR"}, false, true},
      {"distance", "model.sketch.rel.distance", {"PP", "PL", "L", "LL"}, true},
      {"hdistance", "model.sketch.rel.hdistance", {"PP", "L"}, true},
      {"vdistance", "model.sketch.rel.vdistance", {"PP", "L"}, true},
      {"radius", "model.sketch.rel.radius", {"R"}, true},
      {"diameter", "model.sketch.rel.diameter", {"R"}, true},
      {"angle", "model.sketch.rel.angle", {"LL"}, true},
      {"offset", "model.sketch.rel.offset", {"LL", "RR"}, true, true},
  };
  return d;
}

DefRef def_of(const Value& type) {
  if (type.is_undefined()) {
    return {};
  }
  const std::string k = base::js_string(type);
  for (const Def& d : defs()) {
    if (k == d.type) {
      return {.d = &d, .proto = false};
    }
  }
  for (const std::string_view n :
       {"constructor", "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "toLocaleString", "toString",
        "valueOf", "__proto__", "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"}) {
    if (k == n) {
      return {.d = nullptr, .proto = true};
    }
  }
  return {};
}

const std::vector<std::string_view>& sigs_of(const DefRef& d) {
  if (d.d == nullptr) {
    type_error(Value{}, "includes");
  }
  return d.d->sigs;
}

const base::Array& entities_of(const Value& sk) {
  const Value* e = sk.is_object() ? sk.find("entities") : nullptr;
  if (e == nullptr || !e->is_array()) {
    type_error(e == nullptr ? Value{} : *e, "find");
  }
  return e->as_array();
}
Value* entities(Value& sk) {
  Value* e = sk.is_object() ? sk.find("entities") : nullptr;
  if (e == nullptr || !e->is_array()) {
    type_error(e == nullptr ? Value{} : *e, "find");
  }
  return e;
}
const base::Array& constraints_of(const Value& sk) {
  const Value* c = sk.is_object() ? sk.find("constraints") : nullptr;
  if (c == nullptr || !c->is_array()) {
    type_error(c == nullptr ? Value{} : *c, "filter");
  }
  return c->as_array();
}

const Value* find_entity(const Value& sk, const Value& id) {
  for (const Value& e : entities_of(sk)) {
    if (prop(e, "id") == id) {
      return &e;
    }
  }
  return nullptr;
}
Value* find_entity(Value& sk, const Value& id) {
  for (Value& e : entities(sk)->as_array()) {
    if (prop(e, "id") == id) {
      return &e;
    }
  }
  return nullptr;
}

Value origin() {
  return base::make_object({{"id", kOrigin}, {"type", "point"}, {"x", 0}, {"y", 0}, {"origin", true}});
}

Value ent_by_id(const Value& sk, const Value& id) {
  if (id.is_string() && id.as_string() == kOrigin) {
    return origin();
  }
  const Value* e = find_entity(sk, id);
  return e != nullptr ? *e : Value(nullptr);
}

double num(const Value& v) {
  return v.is_number() ? v.as_number() : base::js_to_number(v);
}

std::optional<P2> point_xy(const Value& sk, const Value& id) {
  const Value p = ent_by_id(sk, id);
  if (p.is_null()) {
    return std::nullopt;
  }
  return P2{num(p.get("x")), num(p.get("y"))};
}

char kind_of(const Value& e) {
  if (e.is_null() || e.is_undefined()) {
    return 0;
  }
  const Value t = e.get("type");
  if (t == Value("point")) {
    return 'P';
  }
  if (t == Value("line")) {
    return 'L';
  }
  if (t == Value("spline")) {
    return 'S';
  }
  return 'R';
}

double radius_of(const Value& sk, const Value& e) {
  if (prop(e, "type") == Value("circle")) {
    return num(e.get("r"));
  }
  const P2 c = xy(point_xy(sk, e.get("c")));
  const P2 s = xy(point_xy(sk, e.get("s")));
  return base::js_hypot(s[0] - c[0], s[1] - c[1]);
}

std::string next_id(Value& sk, std::string_view prefix) {
  const double seq = num(sk.get("seq"));
  sk.set("seq", seq + 1);
  return std::string(prefix) + base::js_string(Value(seq));
}

double signed_dist(P2 p, const std::array<P2, 2>& line) {
  const auto& [a, b] = line;
  const double dx = b[0] - a[0];
  const double dy = b[1] - a[1];
  double l = base::js_hypot(dx, dy);
  if (l == 0 || std::isnan(l)) {
    l = 1e-300;
  }
  return (dx * (p[1] - a[1]) - dy * (p[0] - a[0])) / l;
}

spline::Curve spline_curve(const Value& sk, const Value& e, const std::function<std::optional<P2>(const Value&)>& pget,
                           const i18n::Catalog& w) {
  const Value pts = prop(e, "pts");
  if (!pts.is_array()) {
    throw Thrown("e.pts.map is not a function");
  }
  const bool control = e.get("kind") == Value("control");
  std::vector<P2> Q;
  for (const Value& id : pts.as_array()) {
    const auto p = pget ? pget(id) : point_xy(sk, id);
    if (!p) {
      type_error(Value(nullptr), control ? "slice" : "0");
    }
    Q.push_back(*p);
  }
  const bool closed = e.get("closed").truthy();
  if (control) {
    const Value k = e.get("knots");
    std::optional<std::vector<double>> knots;
    if (k.truthy()) {
      if (!k.is_array()) {
        throw Thrown("knots.slice is not a function");
      }
      knots.emplace();
      for (const Value& v : k.as_array()) {
        knots->push_back(num(v));
      }
    }
    return spline::control_curve(Q, closed, knots);
  }
  spline::FitOptions o;
  o.closed = closed;
  for (const auto& [key, slot] : {std::pair{"a0", &o.a0}, std::pair{"a1", &o.a1}}) {
    const Value v = e.get(key);
    if (v.is_number() && std::isfinite(v.as_number())) {
      *slot = v.as_number();
    }
  }
  auto c = spline::interp_fit(Q, o, w);
  if (!c) {
    throw Thrown(c.error().message);
  }
  return std::move(*c);
}

P2 spline_end_tangent(const Value& sk, const Value& e, char end,
                      const std::function<std::optional<P2>(const Value&)>& pget,
                      const std::function<Value(const Value&, std::string_view)>& aget, const i18n::Catalog& w) {
  const std::string_view key = end == 's' ? "a0" : "a1";
  const Value ang = aget ? aget(e, key) : e.get(key);
  const Value kind = e.get("kind");
  if (kind == Value("fit") && ang.is_number() && std::isfinite(ang.as_number())) {
    return {base::js_cos(ang.as_number()), base::js_sin(ang.as_number())};
  }
  if (kind == Value("control")) {
    std::vector<P2> P;
    for (const Value& id : prop(e, "pts").as_array()) {
      P.push_back(xy(pget ? pget(id) : point_xy(sk, id)));
    }
    if (P.size() < 2) {
      type_error(Value{}, "0");
    }
    const P2 d = end == 's' ? P2{P[1][0] - P[0][0], P[1][1] - P[0][1]}
                            : P2{P[P.size() - 1][0] - P[P.size() - 2][0], P[P.size() - 1][1] - P[P.size() - 2][1]};
    double l = base::js_hypot(d[0], d[1]);
    if (l == 0 || std::isnan(l)) {
      l = 1e-300;
    }
    return {d[0] / l, d[1] / l};
  }
  const spline::Curve c = spline_curve(sk, e, pget, w);
  auto t = spline::derivs_at(c, spline::domain(c)[end == 's' ? 0 : 1], 1);
  if (!t) {
    throw Thrown(t.error().message);
  }
  const P2 d = (*t)[1];
  double l = base::js_hypot(d[0], d[1]);
  if (l == 0 || std::isnan(l)) {
    l = 1e-300;
  }
  return {d[0] / l, d[1] / l};
}

}  // namespace detail

using namespace detail;  // NOLINT(google-build-using-namespace): the module's own helpers

namespace {

constexpr double PI = std::numbers::pi;

bool includes(const std::vector<Value>& list, const Value& v) {
  return std::ranges::find(list, v) != list.end();
}

}  // namespace

Value new_sketch() {
  return base::make_object({{"seq", 1}, {"entities", base::Array{}}, {"constraints", base::Array{}}});
}

Value by_id(const Value& sk, const Value& id) {
  return detail::ent_by_id(sk, id);
}

std::vector<Value> points_of(const Value& e) {
  if (!e.truthy()) {
    return {};
  }
  const Value t = e.get("type");
  if (t == Value("point")) {
    return {e.get("id")};
  }
  if (t == Value("line")) {
    return {e.get("p1"), e.get("p2")};
  }
  if (t == Value("circle")) {
    return {e.get("c")};
  }
  if (t == Value("spline")) {
    const Value pts = e.get("pts");
    if (!pts.is_array()) {
      type_error(pts, "slice");
    }
    return pts.as_array();
  }
  return {e.get("c"), e.get("s"), e.get("e")};
}

std::vector<Value> ends_of(const Value& e) {
  if (!e.truthy()) {
    return {};
  }
  const Value t = e.get("type");
  if (t == Value("line")) {
    return {e.get("p1"), e.get("p2")};
  }
  if (t == Value("arc")) {
    return {e.get("s"), e.get("e")};
  }
  if (t == Value("spline") && !e.get("closed").truthy()) {
    const Value pts = e.get("pts");
    const auto& a = pts.is_array() ? pts.as_array() : base::Array{};
    return {a.empty() ? Value{} : a.front(), a.empty() ? Value{} : a.back()};
  }
  return {};
}

namespace detail {

std::optional<std::array<Value, 2>> meet_of(const Value& sk, const Value& a, const Value& b) {
  const std::vector<Value> ea = ends_of(a);
  const std::vector<Value> eb = ends_of(b);
  for (const Value& p : ea) {
    if (includes(eb, p)) {
      return std::array<Value, 2>{p, p};
    }
  }
  for (const Value& k : constraints_of(sk)) {
    const Value refs = prop(k, "refs");
    if (!(prop(k, "type") == Value("coincident")) || !refs.is_array() || refs.as_array().size() != 2) {
      continue;
    }
    const Value& u = refs.as_array()[0];
    const Value& v = refs.as_array()[1];
    if (includes(ea, u) && includes(eb, v)) {
      return std::array<Value, 2>{u, v};
    }
    if (includes(ea, v) && includes(eb, u)) {
      return std::array<Value, 2>{v, u};
    }
  }
  return std::nullopt;
}

std::optional<Value> shared_end(const Value& sk, const Value& a, const Value& b) {
  if (kind_of(a) == 'S' || kind_of(b) == 'S') {
    return std::nullopt;
  }
  const auto m = meet_of(sk, a, b);
  return m ? std::optional<Value>((*m)[0]) : std::nullopt;
}

std::optional<std::pair<std::string, std::vector<Value>>> match(const Value& sk, const Value& type, const Value& refs) {
  const DefRef def = def_of(type);
  if (!def.found()) {
    return std::nullopt;
  }
  if (!refs.is_array()) {
    throw Thrown("refs.map is not a function");
  }
  std::vector<Value> ents;
  for (const Value& r : refs.as_array()) {
    ents.push_back(ent_by_id(sk, r));
  }
  if (std::ranges::any_of(ents, [](const Value& e) { return !e.truthy(); })) {
    return std::nullopt;
  }
  const auto order = [](char k) {
    switch (k) {
      case 'P': return 0;
      case 'L': return 1;
      case 'R': return 2;
      default: return 3;
    }
  };
  std::vector<std::pair<Value, char>> pairs;
  pairs.reserve(ents.size());
  for (std::size_t i = 0; i < ents.size(); ++i) {
    pairs.emplace_back(refs.as_array()[i], kind_of(ents[i]));
  }
  std::ranges::stable_sort(pairs, [&](const auto& p, const auto& q) { return order(p.second) < order(q.second); });
  std::string sig;
  for (const auto& p : pairs) {
    sig += p.second;
  }
  const auto& sigs = sigs_of(def);
  if (std::ranges::find(sigs, sig) == sigs.end()) {
    return std::nullopt;
  }
  std::vector<Value> uniq;
  for (const Value& r : refs.as_array()) {
    if (!includes(uniq, r)) {
      uniq.push_back(r);
    }
  }
  if (uniq.size() != refs.as_array().size()) {
    return std::nullopt;
  }
  std::vector<Value> out;
  out.reserve(pairs.size());
  for (auto& p : pairs) {
    out.push_back(std::move(p.first));
  }
  return std::pair{sig, out};
}

static std::array<P2, 2> line_of(const Value& sk, const Value& id) {
  const Value l = ent_by_id(sk, id);
  return {xy(point_xy(sk, prop(l, "p1"))), xy(point_xy(sk, prop(l, "p2")))};
}

Value measure(const Value& sk, const Value& type, const std::vector<Value>& refs,
              const std::optional<std::string>& sig) {
  const auto P = [&](const Value& id) { return xy(point_xy(sk, id)); };
  const auto E = [&](const Value& id) { return ent_by_id(sk, id); };
  const auto L = [&](const Value& id) { return line_of(sk, id); };
  const auto sgn = [](double v) { return v < 0 ? -1.0 : 1.0; };
  const auto ref = [&](std::size_t i) { return i < refs.size() ? refs[i] : Value{}; };
  const auto sig_is = [&](std::string_view s) { return sig && *sig == s; };
  const std::string t = type.is_string() ? type.as_string() : std::string();
  if (t == "fix") {
    const Value e = E(ref(0));
    const Value et = prop(e, "type");
    base::Array v;
    const auto put = [&](P2 p) {
      v.emplace_back(p[0]);
      v.emplace_back(p[1]);
    };
    if (et == Value("point")) {
      v = base::Array{e.get("x"), e.get("y")};
    } else if (et == Value("line")) {
      put(P(e.get("p1")));
      put(P(e.get("p2")));
    } else if (et == Value("circle")) {
      put(P(e.get("c")));
      v.push_back(e.get("r"));
    } else if (et == Value("spline")) {
      for (const Value& id : prop(e, "pts").as_array()) {
        put(P(id));
      }
    } else {
      put(P(e.get("c")));
      put(P(e.get("s")));
      put(P(e.get("e")));
    }
    return base::make_object({{"value", std::move(v)}});
  }
  if (t == "distance") {
    if (sig_is("PP")) {
      const P2 a = P(ref(0));
      const P2 b = P(ref(1));
      return base::make_object({{"value", base::js_hypot(b[0] - a[0], b[1] - a[1])}});
    }
    if (sig_is("L")) {
      const auto [a, b] = L(ref(0));
      return base::make_object({{"value", base::js_hypot(b[0] - a[0], b[1] - a[1])}});
    }
    const P2 p = sig_is("PL") ? P(ref(0)) : L(ref(1))[0];
    const double s = signed_dist(p, L(sig_is("PL") ? ref(1) : ref(0)));
    return base::make_object({{"value", std::abs(s)}, {"sign", sgn(s)}});
  }
  if (t == "hdistance" || t == "vdistance") {
    std::array<P2, 2> ab{};
    if (sig_is("L")) {
      ab = L(ref(0));
    } else {
      ab = {P(ref(0)), P(ref(1))};
    }
    const double d = t == "hdistance" ? ab[1][0] - ab[0][0] : ab[1][1] - ab[0][1];
    return base::make_object({{"value", std::abs(d)}, {"sign", sgn(d)}});
  }
  if (t == "offset" || t == "offsetlink") {
    const double s =
        sig_is("LL") ? signed_dist(L(ref(1))[0], L(ref(0))) : radius_of(sk, E(ref(1))) - radius_of(sk, E(ref(0)));
    return base::make_object({{"value", std::abs(s)}, {"sign", sgn(s)}});
  }
  if (t == "radius") {
    return base::make_object({{"value", radius_of(sk, E(ref(0)))}});
  }
  if (t == "diameter") {
    return base::make_object({{"value", 2 * radius_of(sk, E(ref(0)))}});
  }
  if (t == "angle") {
    const auto [a1, b1] = L(ref(0));
    const auto [a2, b2] = L(ref(1));
    const P2 u{b1[0] - a1[0], b1[1] - a1[1]};
    const P2 v{b2[0] - a2[0], b2[1] - a2[1]};
    const double ang = base::js_atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]);
    return base::make_object({{"value", std::abs(ang) * 180 / PI}, {"sign", sgn(ang)}});
  }
  if (t == "tangent") {
    if (!sig) {
      type_error(Value{}, "includes");
    }
    if (sig->contains('S')) {
      return Value::object();
    }
    if (*sig == "LR") {
      const Value r = E(ref(1));
      return base::make_object({{"sign", sgn(signed_dist(P(prop(r, "c")), L(ref(0))))}});
    }
    const Value a = E(ref(0));
    const Value b = E(ref(1));
    const P2 ca = P(prop(a, "c"));
    const P2 cb = P(prop(b, "c"));
    const double ra = radius_of(sk, a);
    const double rb = radius_of(sk, b);
    const double d = base::js_hypot(cb[0] - ca[0], cb[1] - ca[1]);
    if (std::abs(d - (ra + rb)) <= std::abs(d - std::abs(ra - rb))) {
      return base::make_object({{"kind", "ext"}});
    }
    return base::make_object({{"kind", "int"}, {"sign", sgn(ra - rb)}});
  }
  return Value::object();
}

}  // namespace detail

std::string add_point(Value& sk, double x, double y, const Value& opts) {
  std::string id = next_id(sk, "p");
  Value p = base::make_object({{"id", id}, {"type", "point"}, {"x", x}, {"y", y}});
  if (jsx::get(opts, "construction").truthy()) {
    p.append("construction", true);
  }
  entities(sk)->push(std::move(p));
  return id;
}

namespace {

/// a, b: [x, y] (a new point) or an existing point id (a new point + coincident relation).
std::string end_point(Value& sk, const Value& v, std::vector<Value>& made, const i18n::Catalog& w) {
  if (v.is_string()) {
    const P2 q = xy(point_xy(sk, v));
    std::string id = add_point(sk, q[0], q[1], Value::object());
    auto c = add_constraint(sk, "coincident", base::Array{id, v}, Value{}, Value::object(), w);
    if (!c) {
      throw Thrown(c.error().message);
    }
    made.push_back(c->get("id"));
    return id;
  }
  if (!v.is_array() && !v.is_string()) {
    type_error(v, "0");
  }
  const auto& a = v.as_array();
  return add_point(sk, a.empty() ? NaN : num(a[0]), a.size() < 2 ? NaN : num(a[1]), Value::object());
}

P2 pair_of(const Value& sk, const Value& v) {
  if (v.is_string()) {
    return xy(point_xy(sk, v));
  }
  if (!v.is_array()) {
    type_error(v, "0");
  }
  const auto& a = v.as_array();
  return {a.empty() ? NaN : num(a[0]), a.size() < 2 ? NaN : num(a[1])};
}

Value ids_value(const std::vector<Value>& v) {
  return base::Array(v.begin(), v.end());
}

}  // namespace

base::Result<Value> add_line(Value& sk, const Value& a, const Value& b, const Value& opts, const i18n::Catalog& w) {
  return guard([&] {
    std::vector<Value> made;
    const std::string p1 = end_point(sk, a, made, w);
    const std::string p2 = end_point(sk, b, made, w);
    const std::string id = next_id(sk, "l");
    Value l = base::make_object({{"id", id}, {"type", "line"}, {"p1", p1}, {"p2", p2}});
    if (jsx::get(opts, "construction").truthy()) {
      l.append("construction", true);
    }
    entities(sk)->push(std::move(l));
    return base::make_object({{"id", id}, {"p1", p1}, {"p2", p2}, {"constraints", ids_value(made)}});
  });
}

base::Result<Value> add_circle(Value& sk, const Value& c, const Value& r, const Value& opts, const i18n::Catalog& w) {
  return guard([&] {
    std::vector<Value> made;
    const std::string cid = end_point(sk, c, made, w);
    const std::string id = next_id(sk, "c");
    Value e = base::make_object({{"id", id}, {"type", "circle"}, {"c", cid}, {"r", std::abs(num(r))}});
    if (jsx::get(opts, "construction").truthy()) {
      e.append("construction", true);
    }
    entities(sk)->push(std::move(e));
    return base::make_object({{"id", id}, {"c", cid}, {"constraints", ids_value(made)}});
  });
}

base::Result<Value> add_arc(Value& sk, const Value& c, const Value& s, const Value& e, const Value& opts,
                            const i18n::Catalog& w) {
  return guard([&] {
    std::vector<Value> made;
    const P2 cxy = pair_of(sk, c);
    const P2 sxy = pair_of(sk, s);
    const double r = base::js_hypot(sxy[0] - cxy[0], sxy[1] - cxy[1]);
    const P2 exy0 = pair_of(sk, e);
    const double ang = base::js_atan2(exy0[1] - cxy[1], exy0[0] - cxy[0]);
    const P2 exy{cxy[0] + r * base::js_cos(ang), cxy[1] + r * base::js_sin(ang)};
    const std::string cid = end_point(sk, c, made, w);
    const std::string sid = end_point(sk, s, made, w);
    const std::string eid = e.is_string() ? end_point(sk, e, made, w) : add_point(sk, exy[0], exy[1], Value::object());
    const std::string id = next_id(sk, "a");
    Value a = base::make_object({{"id", id}, {"type", "arc"}, {"c", cid}, {"s", sid}, {"e", eid}});
    if (jsx::get(opts, "construction").truthy()) {
      a.append("construction", true);
    }
    entities(sk)->push(std::move(a));
    return base::make_object({{"id", id}, {"c", cid}, {"s", sid}, {"e", eid}, {"constraints", ids_value(made)}});
  });
}

base::Result<Value> add_rect(Value& sk, const Value& a, const Value& b, const Value& opts, const i18n::Catalog& w) {
  return guard([&] {
    const P2 p0 = pair_of(sk, a.is_string() ? Value(base::Array{}) : a);
    const P2 p1 = pair_of(sk, b.is_string() ? Value(base::Array{}) : b);
    const auto [x0, y0] = p0;
    const auto [x1, y1] = p1;
    const std::array<P2, 4> pts{{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
    std::vector<Value> lines;
    for (std::size_t i = 0; i < 4; i++) {
      auto l = add_line(sk, base::Array{pts[i][0], pts[i][1]}, base::Array{pts[(i + 1) % 4][0], pts[(i + 1) % 4][1]},
                        opts, w);
      if (!l) {
        throw Thrown(l.error().message);
      }
      lines.push_back(std::move(*l));
    }
    std::vector<Value> made;
    const auto add = [&](std::string_view type, base::Array refs) {
      auto c = add_constraint(sk, type, std::move(refs), Value{}, Value::object(), w);
      if (!c) {
        throw Thrown(c.error().message);
      }
      made.push_back(c->get("id"));
    };
    for (std::size_t i = 0; i < 4; i++) {
      add("coincident", base::Array{lines[i].get("p2"), lines[(i + 1) % 4].get("p1")});
    }
    add("horizontal", base::Array{lines[0].get("id")});
    add("horizontal", base::Array{lines[2].get("id")});
    add("vertical", base::Array{lines[1].get("id")});
    add("vertical", base::Array{lines[3].get("id")});
    base::Array ids;
    for (const Value& l : lines) {
      ids.push_back(l.get("id"));
    }
    return base::make_object({{"lines", std::move(ids)}, {"constraints", ids_value(made)}});
  });
}

base::Result<Value> add_spline(Value& sk, const Value& pts, const Value& opts, const i18n::Catalog& w) {
  return guard([&] {
    const std::string kind = jsx::get(opts, "kind") == Value("control") ? "control" : "fit";
    if (!pts.is_array()) {
      throw Thrown("pts.map is not a function");
    }
    base::Array ids;
    for (const Value& v : pts.as_array()) {
      if (v.is_string()) {
        ids.push_back(v);
      } else {
        const P2 p = pair_of(sk, v);
        ids.emplace_back(add_point(sk, p[0], p[1], Value::object()));
      }
    }
    const bool closed = jsx::get(opts, "closed").truthy();
    if (ids.size() < (closed ? 3U : 2U)) {
      throw Thrown(closed ? w.t("model.sketch_spline.closed_three") : w.t("model.sketch_spline.open_two"));
    }
    const std::string id = next_id(sk, "s");
    Value e = base::make_object({{"id", id}, {"type", "spline"}, {"kind", kind}, {"pts", ids}});
    if (closed) {
      e.append("closed", true);
    }
    if (jsx::get(opts, "construction").truthy()) {
      e.append("construction", true);
    }
    if (kind == "fit" && !closed) {
      for (const std::string_view k : {"a0", "a1"}) {
        const Value v = jsx::get(opts, k);
        if (v.is_number() && std::isfinite(v.as_number())) {
          e.append(k, v);
        }
      }
    }
    const Value knots = jsx::get(opts, "knots");
    if (kind == "control" && !closed && knots.truthy()) {
      if (!knots.is_array()) {
        throw Thrown("opts.knots.slice is not a function");
      }
      e.append("knots", knots);
    }
    (void)spline_curve(sk, e, nullptr, w);  // throws on points on top of each other
    entities(sk)->push(std::move(e));
    return base::make_object({{"id", id}, {"pts", std::move(ids)}});
  });
}

base::Result<Value> match_sig(const Value& sk, const Value& type, const Value& refs) {
  return guard([&] {
    const auto m = match(sk, type, refs);
    if (!m) {
      return Value(nullptr);
    }
    return base::make_object({{"sig", m->first}, {"refs", ids_value(m->second)}});
  });
}

namespace {

P2 line_vec(const Value& sk, const Value& id) {
  const auto [a, b] = line_of(sk, id);
  return {b[0] - a[0], b[1] - a[1]};
}
bool nearly_parallel(const Value& sk, const std::vector<Value>& refs) {
  const P2 u = line_vec(sk, refs.at(0));
  const P2 v = line_vec(sk, refs.at(1));
  double den = base::js_hypot(u[0], u[1]) * base::js_hypot(v[0], v[1]);
  if (den == 0 || std::isnan(den)) {
    den = 1;
  }
  const double c = (u[0] * v[1] - u[1] * v[0]) / den;
  return std::abs(c) < 1e-6;
}

}  // namespace

base::Result<Value> applicable(const Value& sk, const Value& refs) {
  return guard([&] {
    base::Array out;
    for (const Def& d : defs()) {
      if (d.autom) {
        continue;
      }
      const Value type(d.type);
      const auto m = match(sk, type, refs);
      if (!m) {
        continue;
      }
      if (d.type == "tangent" && m->first.contains('S') &&
          !meet_of(sk, ent_by_id(sk, m->second[0]), ent_by_id(sk, m->second[1]))) {
        continue;
      }
      if (d.type == "symmetric" && m->second[0] == Value(kOrigin) && m->second[1] == Value(kOrigin)) {
        continue;
      }
      if (d.type == "distance" && m->first == "LL" && !nearly_parallel(sk, m->second)) {
        continue;
      }
      if (d.type == "angle" && nearly_parallel(sk, m->second)) {
        continue;
      }
      out.emplace_back(d.type);
    }
    return Value(std::move(out));
  });
}

base::Result<Value> add_constraint(Value& sk, const Value& type, const Value& refs, const Value& value,
                                   const Value& extra, const i18n::Catalog& w) {
  return guard([&] {
    const auto m = match(sk, type, refs);
    const DefRef def = def_of(type);
    if (!m) {
      if (def.d != nullptr) {
        throw Thrown(w.t("model.sketch.not_apply", {{"relation", w.t(def.d->label_key)}}));
      }
      if (type.is_null() || type.is_undefined()) {
        throw Thrown(w.t("model.sketch.not_apply"));
      }
      throw Thrown(w.t("model.sketch.not_apply", {{"relation", base::js_string(type)}}));
    }
    const std::string t = base::js_string(type);
    if (t == "tangent" && m->first.contains('S')) {
      Value* a = find_entity(sk, m->second[0]);
      Value* b = find_entity(sk, m->second[1]);
      const Value av = a != nullptr ? *a : ent_by_id(sk, m->second[0]);
      const Value bv = b != nullptr ? *b : ent_by_id(sk, m->second[1]);
      const auto meet = meet_of(sk, av, bv);
      if (!meet) {
        throw Thrown(w.t("model.sketch.tangent_spline_end"));
      }
      for (const auto& [e, pid] : {std::pair{a, (*meet)[0]}, std::pair{b, (*meet)[1]}}) {
        if (e == nullptr || !(e->get("type") == Value("spline")) || !(e->get("kind") == Value("fit"))) {
          continue;
        }
        const Value pts = e->get("pts");
        const bool start = pts.is_array() && !pts.as_array().empty() && pid == pts.as_array()[0];
        const std::string_view key = start ? "a0" : "a1";
        const Value cur = e->get(key);
        if (!(cur.is_number() && std::isfinite(cur.as_number()))) {
          const P2 tn = spline_end_tangent(sk, *e, start ? 's' : 'e', nullptr, nullptr, w);
          e->set(key, base::js_atan2(tn[1], tn[0]));
        }
      }
    }
    if (t == "symmetric" && m->second[0] == m->second[1]) {
      throw Thrown(w.t("model.sketch.symmetric_two_points"));
    }
    if (t == "offsetlink") {
      const Value of = jsx::get(extra, "of");
      const bool has = std::ranges::any_of(
          constraints_of(sk), [&](const Value& k) { return prop(k, "id") == of && k.get("type") == Value("offset"); });
      if (!has) {
        throw Thrown(w.t("model.sketch.offsetlink_needs_dim"));
      }
    }
    Value c = base::make_object({{"id", next_id(sk, "k")}, {"type", type}, {"refs", ids_value(m->second)}});
    if (t == "offsetlink") {
      c.append("of", jsx::get(extra, "of"));
    }
    const Value meas = measure(sk, type, m->second, m->first);
    if (const Value s = meas.get("sign"); !s.is_undefined()) {
      c.append("sign", s);
    }
    if (const Value k = meas.get("kind"); !k.is_undefined()) {
      c.append("kind", k);
    }
    if (t == "fix") {
      c.append("value", meas.get("value"));
    } else if (def.d != nullptr && def.d->dim) {
      const double v = value.is_undefined() ? num(meas.get("value")) : base::js_to_number(value);
      if (!std::isfinite(v)) {
        throw Thrown(w.t("model.sketch.not_number"));
      }
      if (t == "angle" ? (v < 0 || v > 180) : v < 0) {
        throw Thrown(t == "angle" ? w.t("model.sketch.angle_range") : w.t("model.sketch.dimension_negative"));
      }
      if ((t == "radius" || t == "diameter") && v <= 0) {
        throw Thrown(w.t("model.sketch.radius_zero"));
      }
      c.append("value", v);
    }
    (void)constraints_of(sk);  // sk.constraints.push: an array, or the TypeError
    sk.find("constraints")->push(c);
    return c;
  });
}

base::Result<Value> measure(const Value& sk, const Value& type, const Value& refs) {
  return guard([&] {
    const auto m = match(sk, type, refs);
    std::vector<Value> r;
    if (refs.is_array()) {
      r = refs.as_array();
    }
    return detail::measure(sk, type, r, m ? std::optional<std::string>(m->first) : std::nullopt);
  });
}

Value describe(const Value& c, const i18n::Catalog& w) {
  const DefRef def = def_of(c.get("type"));
  if (!def.found()) {
    return c.get("type");
  }
  if (def.d == nullptr) {
    return {};  // a prototype member's label: undefined
  }
  if (!def.d->dim) {
    return w.t(def.d->label_key);
  }
  const std::string v = base::js_string(Value(base::js_number(base::js_to_fixed(num(c.get("value")), 4))));
  if (def.d->type == "angle") {
    return w.t("model.sketch.describe.angle", {{"value", v}});
  }
  if (def.d->type == "diameter") {
    return w.t("model.sketch.describe.diameter", {{"value", v}});
  }
  if (def.d->type == "radius") {
    return w.t("model.sketch.describe.radius", {{"value", v}});
  }
  if (def.d->type == "offset") {
    return w.t("model.sketch.describe.offset", {{"value", v}});
  }
  return w.t("model.sketch.describe.dim", {{"label", w.t(def.d->label_key)}, {"value", v}});
}

namespace {

void drop_orphan_links(Value& sk) {
  std::vector<Value> masters;
  for (const Value& c : constraints_of(sk)) {
    if (c.get("type") == Value("offset")) {
      masters.push_back(c.get("id"));
    }
  }
  Value* cons = sk.find("constraints");
  base::Array keep;
  for (Value& c : cons->as_array()) {
    if (!(c.get("type") == Value("offsetlink")) || includes(masters, c.get("of"))) {
      keep.push_back(std::move(c));
    }
  }
  *cons = Value(std::move(keep));
}

double spline_min(const Value& e) {
  return e.get("closed").truthy() ? 3 : 2;
}

}  // namespace

base::Result<Value> remove_entities(Value& sk, const Value& ids_v) {
  return guard([&] {
    if (!ids_v.is_array()) {
      throw Thrown("ids is not iterable");
    }
    const std::vector<Value>& ids = ids_v.as_array();
    std::vector<Value> gone;
    const auto add = [&](const Value& v) {
      if (!includes(gone, v)) {
        gone.push_back(v);
      }
    };
    const auto del = [&](const Value& v) { std::erase(gone, v); };
    const auto has = [&](const Value& v) { return includes(gone, v); };
    for (const Value& v : ids) {
      add(v);
    }
    del(Value(kOrigin));
    for (const Value& id : ids) {
      const Value e = ent_by_id(sk, id);
      if (e.truthy() && !(e.get("type") == Value("point"))) {
        for (const Value& p : points_of(e)) {
          add(p);
        }
      }
    }
    for (const Value& e : entities_of(sk)) {
      if (has(e.get("id")) || e.get("type") == Value("point")) {
        continue;
      }
      for (const Value& p : points_of(e)) {
        if (has(p) && !includes(ids, p)) {
          del(p);
        }
      }
    }
    for (Value& e : entities(sk)->as_array()) {
      if (e.get("type") == Value("point") || has(e.get("id"))) {
        continue;
      }
      const std::vector<Value> pts = points_of(e);
      if (!std::ranges::any_of(pts, has)) {
        continue;
      }
      if (e.get("type") == Value("spline")) {
        base::Array keep;
        for (const Value& p : pts) {
          if (!has(p)) {
            keep.push_back(p);
          }
        }
        if (static_cast<double>(keep.size()) >= spline_min(e)) {
          if (!(keep.front() == pts.front())) {
            e.erase("a0");
          }
          if (!(keep.back() == pts.back())) {
            e.erase("a1");
          }
          e.set("pts", std::move(keep));
          e.erase("knots");
          continue;
        }
      }
      add(e.get("id"));
    }
    base::Array ents;
    for (Value& e : entities(sk)->as_array()) {
      if (!has(e.get("id"))) {
        ents.push_back(std::move(e));
      }
    }
    *entities(sk) = Value(std::move(ents));
    const std::size_t before = constraints_of(sk).size();
    base::Array cons;
    for (const Value& c : constraints_of(sk)) {
      const Value refs = prop(c, "refs");
      if (!refs.is_array()) {
        throw Thrown("c.refs.some is not a function");
      }
      if (!std::ranges::any_of(refs.as_array(), has)) {
        cons.push_back(c);
      }
    }
    sk.set("constraints", std::move(cons));
    drop_orphan_links(sk);
    return base::make_object({{"entities", static_cast<double>(gone.size())},
                              {"constraints", static_cast<double>(before - constraints_of(sk).size())}});
  });
}

void remove_constraints(Value& sk, const Value& ids) {
  std::vector<Value> set;
  if (ids.is_array()) {
    set = ids.as_array();
  }
  base::Array keep;
  for (const Value& c : constraints_of(sk)) {
    if (!includes(set, c.get("id"))) {
      keep.push_back(c);
    }
  }
  sk.set("constraints", std::move(keep));
  drop_orphan_links(sk);
}

double extent(const Value& sk) {
  double m = 0;
  for (const Value& e : entities_of(sk)) {
    const Value t = e.get("type");
    if (t == Value("point")) {
      m = jsx::max(jsx::max(m, std::abs(num(e.get("x")))), std::abs(num(e.get("y"))));
    }
    if (t == Value("circle")) {
      const P2 c = xy(point_xy(sk, e.get("c")));
      const double r = num(e.get("r"));
      m = jsx::max(jsx::max(m, std::abs(c[0]) + r), std::abs(c[1]) + r);
    }
  }
  return m;
}

namespace {

struct ArcAngles {
  P2 c;
  double r, a0, sweep;
};
ArcAngles arc_angles(const Value& sk, const Value& a) {
  const P2 c = xy(point_xy(sk, prop(a, "c")));
  const P2 s = xy(point_xy(sk, a.get("s")));
  const P2 e = xy(point_xy(sk, a.get("e")));
  const double a0 = base::js_atan2(s[1] - c[1], s[0] - c[0]);
  const double a1 = base::js_atan2(e[1] - c[1], e[0] - c[0]);
  double sweep = a1 - a0;
  while (sweep <= 1e-12) {
    sweep += 2 * PI;
  }
  while (sweep > 2 * PI) {
    sweep -= 2 * PI;
  }
  return {c, base::js_hypot(s[0] - c[0], s[1] - c[1]), a0, sweep};
}

std::vector<P2> tess_arc_pts(const Value& sk, const Value& a, double seg_full) {
  const ArcAngles g = arc_angles(sk, a);
  const double n = jsx::max(1, std::ceil(g.sweep / (2 * PI) * seg_full - 1e-9));
  std::vector<P2> pts;
  for (std::int64_t i = 0; static_cast<double>(i) <= n; i++) {
    const double t = g.a0 + g.sweep * static_cast<double>(i) / n;
    pts.push_back({g.c[0] + g.r * base::js_cos(t), g.c[1] + g.r * base::js_sin(t)});
  }
  return pts;
}

std::vector<P2> tess_circle(P2 c, double r, double seg_full) {
  std::vector<P2> pts;
  for (std::int64_t i = 0; static_cast<double>(i) < seg_full; i++) {
    const double t = 2 * PI * static_cast<double>(i) / seg_full;
    pts.push_back({c[0] + r * base::js_cos(t), c[1] + r * base::js_sin(t)});
  }
  return pts;
}

std::vector<P2> tess_spline_pts(const Value& sk, const Value& e, double per_span, const i18n::Catalog& w) {
  auto t = spline::tessellate(spline_curve(sk, e, nullptr, w), per_span);
  if (!t) {
    throw Thrown(t.error().message);
  }
  return std::move(*t);
}

Value pts_value(const std::vector<P2>& pts) {
  return spline::points_value(pts);
}

std::string fx(double v) {
  return base::js_string(Value(base::js_number(base::js_to_fixed(v, 3))));
}

}  // namespace

base::Result<Value> fillet_corner(Value& sk, const Value& point_id, double r, const i18n::Catalog& w) {
  return guard([&] {
    if (!(r > 0)) {
      throw Thrown(w.t("model.sketch.fillet_radius_zero"));
    }
    const auto at0 = point_xy(sk, point_id);
    if (!at0) {
      throw Thrown(w.t("model.sketch.fillet_pick_corner"));
    }
    const P2 at = *at0;
    const double tol = 1e-6 * jsx::max(1, extent(sk));
    struct End {
      Value line_id, mine, other;
    };
    std::vector<End> ends;
    for (const Value& e : entities_of(sk)) {
      if (!(e.get("type") == Value("line")) || e.get("construction").truthy()) {
        continue;
      }
      for (const auto& [mine, other] : {std::pair{e.get("p1"), e.get("p2")}, std::pair{e.get("p2"), e.get("p1")}}) {
        const P2 q = xy(point_xy(sk, mine));
        if (base::js_hypot(q[0] - at[0], q[1] - at[1]) <= tol) {
          ends.push_back({e.get("id"), mine, other});
        }
      }
    }
    if (ends.size() != 2 || ends[0].line_id == ends[1].line_id) {
      throw Thrown(w.t("model.sketch.fillet_two_lines"));
    }
    const P2 P = at;
    const End& A = ends[0];
    const End& B = ends[1];
    const P2 fa = xy(point_xy(sk, A.other));
    const P2 fb = xy(point_xy(sk, B.other));
    const double la = base::js_hypot(fa[0] - P[0], fa[1] - P[1]);
    const double lb = base::js_hypot(fb[0] - P[0], fb[1] - P[1]);
    const P2 u1{(fa[0] - P[0]) / la, (fa[1] - P[1]) / la};
    const P2 u2{(fb[0] - P[0]) / lb, (fb[1] - P[1]) / lb};
    const double dot = u1[0] * u2[0] + u1[1] * u2[1];
    const double cos_t = std::isnan(dot) ? dot : std::max(-1.0, std::min(1.0, dot));
    const double th = std::acos(cos_t);
    if (th < 1e-6 || PI - th < 1e-6) {
      throw Thrown(w.t("model.sketch.fillet_in_line"));
    }
    const double d = r / std::tan(th / 2);
    if (d >= la - tol || d >= lb - tol) {
      throw Thrown(w.t("model.sketch.fillet_too_big"));
    }
    const P2 t1{P[0] + u1[0] * d, P[1] + u1[1] * d};
    const P2 t2{P[0] + u2[0] * d, P[1] + u2[1] * d};
    const P2 bis{u1[0] + u2[0], u1[1] + u2[1]};
    const double bl = base::js_hypot(bis[0], bis[1]);
    const double h = r / base::js_sin(th / 2);
    const P2 c{P[0] + bis[0] / bl * h, P[1] + bis[1] / bl * h};
    Value ida = A.mine;
    Value idb = B.mine;
    if (ida == idb) {
      idb = Value(add_point(sk, P[0], P[1], Value::object()));
      Value* l = find_entity(sk, B.line_id);
      if (l->get("p1") == ida) {
        l->set("p1", idb);
      } else {
        l->set("p2", idb);
      }
    }
    std::size_t removed = 0;
    base::Array keep;
    for (const Value& k : constraints_of(sk)) {
      const Value refs = prop(k, "refs");
      const auto& rr = refs.is_array() ? refs.as_array() : base::Array{};
      if (includes(rr, ida) || includes(rr, idb)) {
        ++removed;
      } else {
        keep.push_back(k);
      }
    }
    sk.set("constraints", std::move(keep));
    const auto set_pt = [&](const Value& id, P2 p) {
      Value* e = find_entity(sk, id);
      if (e == nullptr) {
        type_error(Value(nullptr), "x");
      }
      e->set("x", p[0]);
      e->set("y", p[1]);
    };
    set_pt(ida, t1);
    set_pt(idb, t2);
    const std::string cid = add_point(sk, c[0], c[1], Value::object());
    const bool ccw = (t1[0] - c[0]) * (t2[1] - c[1]) - (t1[1] - c[1]) * (t2[0] - c[0]) > 0;
    const std::string aid = next_id(sk, "a");
    entities(sk)->push(
        base::make_object({{"id", aid}, {"type", "arc"}, {"c", cid}, {"s", ccw ? ida : idb}, {"e", ccw ? idb : ida}}));
    for (const Value& line : {A.line_id, B.line_id}) {
      auto k = add_constraint(sk, "tangent", base::Array{line, aid}, Value{}, Value::object(), w);
      if (!k) {
        throw Thrown(k.error().message);
      }
    }
    auto k = add_constraint(sk, "radius", base::Array{aid}, r, Value::object(), w);
    if (!k) {
      throw Thrown(k.error().message);
    }
    return base::make_object({{"arc", aid}, {"removed", static_cast<double>(removed)}});
  });
}

base::Result<Value> bounds(const Value& sk, const i18n::Catalog& w) {
  return guard([&] {
    constexpr double inf = std::numeric_limits<double>::infinity();
    double x0 = inf;
    double y0 = inf;
    double x1 = -inf;
    double y1 = -inf;
    const auto jmin = [](double a, double b) { return std::isnan(a) || std::isnan(b) ? NaN : std::min(a, b); };
    const auto inc = [&](double x, double y) {
      x0 = jmin(x0, x);
      y0 = jmin(y0, y);
      x1 = jsx::max(x1, x);
      y1 = jsx::max(y1, y);
    };
    for (const Value& e : entities_of(sk)) {
      const Value t = e.get("type");
      if (t == Value("point")) {
        inc(num(e.get("x")), num(e.get("y")));
      } else if (t == Value("circle")) {
        const P2 c = xy(point_xy(sk, e.get("c")));
        const double r = num(e.get("r"));
        inc(c[0] - r, c[1] - r);
        inc(c[0] + r, c[1] + r);
      } else if (t == Value("arc")) {
        for (const P2& p : tess_arc_pts(sk, e, 32)) {
          inc(p[0], p[1]);
        }
      } else if (t == Value("spline")) {
        try {
          for (const P2& p : tess_spline_pts(sk, e, 8, w)) {
            inc(p[0], p[1]);
          }
        } catch (const Thrown&) {  // NOLINT(bugprone-empty-catch): points on top of each other draw nothing, as the JS
        }
      }
    }
    if (x0 == inf) {
      return Value(nullptr);
    }
    return base::make_object({{"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1}});
  });
}

base::Result<Value> loose_points(const Value& sk) {
  return guard([&] {
    std::vector<Value> used;
    for (const Value& e : entities_of(sk)) {
      if (!(e.get("type") == Value("point"))) {
        for (const Value& p : points_of(e)) {
          used.push_back(p);
        }
      }
    }
    base::Array out;
    for (const Value& e : entities_of(sk)) {
      if (e.get("type") == Value("point") && !includes(used, e.get("id"))) {
        out.push_back(e);
      }
    }
    return Value(std::move(out));
  });
}

base::Result<Value> end_meet(const Value& sk, const Value& a, const Value& b) {
  return guard([&] {
    const auto m = meet_of(sk, a, b);
    return m ? Value(base::Array{(*m)[0], (*m)[1]}) : Value(nullptr);
  });
}

base::Result<Value> tess_arc(const Value& sk, const Value& arc, double seg_full) {
  return guard([&] { return pts_value(tess_arc_pts(sk, arc, seg_full)); });
}

base::Result<Value> tess_spline(const Value& sk, const Value& e, double per_span, const i18n::Catalog& w) {
  return guard([&] { return pts_value(tess_spline_pts(sk, e, per_span, w)); });
}

base::Result<Value> profile(const Value& sk, double seg_full, const i18n::Catalog& w) {
  return guard([&]() -> Value {
    const double tol = 1e-6 * jsx::max(1, extent(sk));
    std::vector<std::vector<P2>> loops;
    base::Array curves;
    struct Seg {
      Value id;
      std::vector<P2> pts;
      std::size_t n0 = 0, n1 = 0;
    };
    std::vector<Seg> segs;
    const auto err = [](std::string m) { return base::make_object({{"error", std::move(m)}}); };
    for (const Value& e : entities_of(sk)) {
      if (e.get("construction").truthy()) {
        continue;
      }
      const Value t = e.get("type");
      if (t == Value("circle")) {
        if (!(num(e.get("r")) > tol)) {
          return err(w.t("model.sketch.circle_no_size"));
        }
        loops.push_back(tess_circle(xy(point_xy(sk, e.get("c"))), num(e.get("r")), seg_full));
        curves.push_back(base::make_object({{"id", e.get("id")}, {"loop", static_cast<double>(loops.size() - 1)}}));
      } else if (t == Value("line")) {
        const P2 a = xy(point_xy(sk, e.get("p1")));
        const P2 b = xy(point_xy(sk, e.get("p2")));
        if (base::js_hypot(b[0] - a[0], b[1] - a[1]) <= tol) {
          return err(w.t("model.sketch.line_no_length"));
        }
        segs.push_back({e.get("id"), {a, b}});
      } else if (t == Value("arc")) {
        segs.push_back({e.get("id"), tess_arc_pts(sk, e, seg_full)});
      } else if (t == Value("spline")) {
        std::vector<P2> pts;
        try {
          pts = tess_spline_pts(sk, e, jsx::max(4, std::ceil(seg_full / 8)), w);
        } catch (const Thrown& x) {
          return err(x.what());
        }
        if (e.get("closed").truthy()) {
          pts.pop_back();
          loops.push_back(std::move(pts));
          curves.push_back(base::make_object({{"id", e.get("id")}, {"loop", static_cast<double>(loops.size() - 1)}}));
        } else {
          segs.push_back({e.get("id"), std::move(pts)});
        }
      }
    }
    struct Node {
      P2 p;
      std::vector<std::size_t> ends;
    };
    std::vector<Node> nodes;
    const auto node_of = [&](P2 p) {
      for (std::size_t i = 0; i < nodes.size(); i++) {
        if (base::js_hypot(nodes[i].p[0] - p[0], nodes[i].p[1] - p[1]) <= tol * 10) {
          return i;
        }
      }
      nodes.push_back({p, {}});
      return nodes.size() - 1;
    };
    for (std::size_t i = 0; i < segs.size(); i++) {
      Seg& s = segs[i];
      s.n0 = node_of(s.pts.front());
      s.n1 = node_of(s.pts.back());
      nodes[s.n0].ends.push_back(i);
      nodes[s.n1].ends.push_back(i);
    }
    for (const Node& n : nodes) {
      if (n.ends.size() == 1 || n.ends.size() > 2) {
        Value o = err(w.t(n.ends.size() == 1 ? "model.sketch.profile_open" : "model.sketch.profile_branches",
                          {{"x", fx(n.p[0])}, {"y", fx(n.p[1])}}));
        o.append("at", base::Array{n.p[0], n.p[1]});
        return o;
      }
    }
    std::vector<bool> used(segs.size(), false);
    for (std::size_t s0 = 0; s0 < segs.size(); s0++) {
      if (used[s0]) {
        continue;
      }
      std::vector<P2> loop;
      const std::size_t start = segs[s0].n0;
      std::size_t s = s0;
      std::size_t from = start;
      const std::size_t li = loops.size();
      for (std::size_t g = 0; g <= segs.size(); g++) {
        used[s] = true;
        curves.push_back(base::make_object({{"id", segs[s].id}, {"loop", static_cast<double>(li)}}));
        const bool fwd = segs[s].n0 == from;
        std::vector<P2> pts = segs[s].pts;
        if (!fwd) {
          std::ranges::reverse(pts);
        }
        for (std::size_t i = 0; i + 1 < pts.size(); i++) {
          loop.push_back(pts[i]);
        }
        const std::size_t to = fwd ? segs[s].n1 : segs[s].n0;
        if (to == start) {
          break;
        }
        std::optional<std::size_t> nx;
        for (const std::size_t q : nodes[to].ends) {
          if (q != s) {
            nx = q;
            break;
          }
        }
        if (!nx || used[*nx]) {
          break;
        }
        from = to;
        s = *nx;
      }
      if (loop.size() < 3) {
        return err(w.t("model.sketch.loop_three_sides"));
      }
      loops.push_back(std::move(loop));
    }
    base::Array lv;
    for (const auto& l : loops) {
      lv.push_back(pts_value(l));
    }
    return base::make_object({{"loops", std::move(lv)}, {"curves", std::move(curves)}});
  });
}

bool inside_loops(const Value& loops, std::array<double, 2> p) {
  bool inside = false;
  if (!loops.is_array()) {
    return inside;
  }
  for (const Value& lv : loops.as_array()) {
    const std::vector<P2> loop = spline::points_of(lv);
    if (loop.empty()) {
      continue;
    }
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
      const P2& a = loop[i];
      const P2& b = loop[j];
      if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) {
        inside = !inside;
      }
    }
  }
  return inside;
}

double loop_area(const Value& loop_v) {
  const std::vector<P2> loop = spline::points_of(loop_v);
  double s = 0;
  if (loop.empty()) {
    return 0;
  }
  for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
    s += (loop[j][0] * loop[i][1] - loop[i][0] * loop[j][1]);
  }
  return s / 2;
}

namespace {

bool id_ok(const Value& v) {
  if (!v.is_string()) {
    return false;
  }
  const std::string& s = v.as_string();
  const bool letter = !s.empty() && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'));
  if (!letter || s.size() > 41) {
    return false;
  }
  return std::ranges::all_of(s, [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
           c == '-';
  });
}
bool fnum(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) < 1e9;
}

}  // namespace

base::Result<Value> read_sketch(const Value& raw, const i18n::Catalog& w) {
  return guard([&] {
    const auto bad = [&](std::string_view key) { throw Thrown(w.t(key)); };
    const Value ents_v = raw.is_object() ? raw.get("entities") : Value{};
    const Value cons_v = raw.is_object() ? raw.get("constraints") : Value{};
    if (!raw.truthy() || (!raw.is_object() && !raw.is_array()) || !ents_v.is_array() || !cons_v.is_array()) {
      bad("model.sketch.file.no_entities");
    }
    const Value seq_v = raw.get("seq");
    const bool seq_ok = seq_v.is_number() && std::isfinite(seq_v.as_number()) &&
                        seq_v.as_number() == std::trunc(seq_v.as_number()) && seq_v.as_number() > 0;
    Value sk = base::make_object(
        {{"seq", seq_ok ? seq_v : Value(1)}, {"entities", base::Array{}}, {"constraints", base::Array{}}});
    std::vector<Value> ids{Value(kOrigin)};
    const auto take = [&](const Value& id) {
      if (!id_ok(id) || includes(ids, id)) {
        bad("model.sketch.file.bad_id");
      }
      ids.push_back(id);
      return id;
    };
    const auto& all = ents_v.as_array();
    const std::size_t ne = std::min<std::size_t>(all.size(), 5000);
    for (std::size_t i = 0; i < ne; i++) {
      const Value& e = all[i];
      const bool cons = prop(e, "construction") == Value(true);
      if (e.get("type") == Value("point")) {
        if (!fnum(e.get("x")) || !fnum(e.get("y"))) {
          bad("model.sketch.file.bad_point");
        }
        Value p =
            base::make_object({{"id", take(e.get("id"))}, {"type", "point"}, {"x", e.get("x")}, {"y", e.get("y")}});
        if (cons) {
          p.append("construction", true);
        }
        entities(sk)->push(std::move(p));
      }
    }
    const auto is_pt = [&](const Value& id) {
      if (id == Value(kOrigin)) {
        return true;
      }
      return std::ranges::any_of(entities_of(sk),
                                 [&](const Value& e) { return e.get("id") == id && e.get("type") == Value("point"); });
    };
    for (std::size_t i = 0; i < ne; i++) {
      const Value& e = all[i];
      const bool cons = prop(e, "construction") == Value(true);
      const Value t = e.get("type");
      Value out;
      if (t == Value("line")) {
        const Value p1 = e.get("p1");
        const Value p2 = e.get("p2");
        if (!is_pt(p1) || !is_pt(p2) || p1 == Value(kOrigin) || p2 == Value(kOrigin)) {
          bad("model.sketch.file.bad_line");
        }
        out = base::make_object({{"id", take(e.get("id"))}, {"type", "line"}, {"p1", p1}, {"p2", p2}});
      } else if (t == Value("circle")) {
        const Value c = e.get("c");
        const Value r = e.get("r");
        if (!is_pt(c) || c == Value(kOrigin) || !fnum(r) || r.as_number() <= 0) {
          bad("model.sketch.file.bad_circle");
        }
        out = base::make_object({{"id", take(e.get("id"))}, {"type", "circle"}, {"c", c}, {"r", r}});
      } else if (t == Value("arc")) {
        for (const std::string_view k : {"c", "s", "e"}) {
          if (!is_pt(e.get(k)) || e.get(k) == Value(kOrigin)) {
            bad("model.sketch.file.bad_arc");
          }
        }
        out = base::make_object(
            {{"id", take(e.get("id"))}, {"type", "arc"}, {"c", e.get("c")}, {"s", e.get("s")}, {"e", e.get("e")}});
      } else if (t == Value("spline")) {
        const Value kind = e.get("kind");
        if (!(kind == Value("fit")) && !(kind == Value("control"))) {
          bad("model.sketch.spline_bad.kind");
        }
        const Value pts = e.get("pts");
        if (!pts.is_array() || pts.as_array().size() > 500) {
          bad("model.sketch.spline_bad.points");
        }
        const bool closed = e.get("closed") == Value(true);
        if (pts.as_array().size() < (closed ? 3U : 2U)) {
          bad("model.sketch.spline_bad.too_few");
        }
        std::vector<Value> seen;
        for (const Value& p : pts.as_array()) {
          if (!p.is_string() || !is_pt(p) || p == Value(kOrigin) || includes(seen, p)) {
            bad("model.sketch.spline_bad.point_twice");
          }
          seen.push_back(p);
        }
        out = base::make_object({{"id", take(e.get("id"))}, {"type", "spline"}, {"kind", kind}, {"pts", pts}});
        if (closed) {
          out.append("closed", true);
        }
        for (const std::string_view k : {"a0", "a1"}) {
          const Value v = e.get(k);
          if (v.is_undefined()) {
            continue;
          }
          if (!(kind == Value("fit")) || closed || !fnum(v)) {
            bad("model.sketch.spline_bad.end_angle");
          }
          out.append(k, v);
        }
        const Value knots = e.get("knots");
        if (!knots.is_undefined()) {
          if (!(kind == Value("control")) || closed) {
            bad("model.sketch.spline_bad.knots_kind");
          }
          if (const auto why = spline::knot_problem(knots, static_cast<double>(pts.as_array().size()), w)) {
            throw Thrown(*why);
          }
          out.append("knots", knots);
        }
      } else if (!(t == Value("point"))) {
        bad("model.sketch.file.unknown_geometry");
      }
      if (!out.is_undefined()) {
        if (cons) {
          out.append("construction", true);
        }
        entities(sk)->push(std::move(out));
      }
    }
    for (const Value& e : entities_of(sk)) {
      if (e.get("type") == Value("spline")) {
        try {
          (void)spline_curve(sk, e, nullptr, w);
        } catch (const Thrown&) {
          bad("model.sketch.file.spline_coincident");
        }
      }
    }
    const auto& craw = cons_v.as_array();
    const std::size_t nc = std::min<std::size_t>(craw.size(), 10000);
    for (std::size_t i = 0; i < nc; i++) {
      const Value& c = craw[i];
      const DefRef def = c.truthy() ? def_of(c.get("type")) : DefRef{};
      if (!c.truthy() || !def.found() || !c.get("refs").is_array()) {
        bad("model.sketch.file.unknown_relation");
      }
      base::Array refs;
      for (const Value& r : c.get("refs").as_array()) {
        refs.emplace_back(r.is_undefined() ? std::string("undefined") : base::js_string(r));
      }
      Value k = base::make_object({{"id", take(c.get("id"))}, {"type", c.get("type")}, {"refs", refs}});
      const auto m = match(sk, k.get("type"), k.get("refs"));
      if (!m) {
        if (def.d == nullptr) {
          type_error(Value{}, "toLowerCase");
        }
        throw Thrown(w.t("model.sketch.file.relation_wrong", {{"relation", base::js_to_lower(w.t(def.d->label_key))}}));
      }
      const std::string t = base::js_string(c.get("type"));
      const Value value = c.get("value");
      if (t == "fix") {
        if (!value.is_array() || !std::ranges::all_of(value.as_array(), fnum)) {
          bad("model.sketch.file.bad_fix");
        }
        k.append("value", value);
      } else if (def.d != nullptr && def.d->dim) {
        if (!fnum(value) || value.as_number() < 0) {
          bad("model.sketch.file.bad_dimension");
        }
        k.append("value", value);
      }
      const Value sign = c.get("sign");
      if (sign == Value(1) || sign == Value(-1)) {
        k.append("sign", sign);
      }
      const Value kind = c.get("kind");
      if (kind == Value("ext") || kind == Value("int")) {
        k.append("kind", kind);
      }
      for (const std::string_view key : {"at", "off"}) {
        const Value v = c.get(key);
        if (v.is_array() && v.as_array().size() == 2 && std::ranges::all_of(v.as_array(), fnum)) {
          k.append(key, v);
        }
      }
      if (t == "offsetlink") {
        const Value of = c.get("of");
        k.append("of", of.is_undefined() ? std::string("undefined") : base::js_string(of));
      }
      if (t == "tangent" && m->first.contains('S') &&
          !meet_of(sk, ent_by_id(sk, k.get("refs").as_array()[0]), ent_by_id(sk, k.get("refs").as_array()[1]))) {
        bad("model.sketch.file.tangent_spline_end");
      }
      if (t == "symmetric" && k.get("refs").as_array()[0] == k.get("refs").as_array()[1]) {
        bad("model.sketch.file.symmetric_wrong");
      }
      sk.find("constraints")->push(std::move(k));
    }
    for (const Value& k : constraints_of(sk)) {
      if (k.get("type") == Value("offsetlink")) {
        const bool has = std::ranges::any_of(constraints_of(sk), [&](const Value& m) {
          return m.get("id") == k.get("of") && m.get("type") == Value("offset");
        });
        if (!has) {
          bad("model.sketch.file.offset_lost");
        }
      }
    }
    for (const Value& id : ids) {
      const std::string& s = id.as_string();
      if (s.size() >= 2 && s[0] >= 'a' && s[0] <= 'z' &&
          std::ranges::all_of(s.substr(1), [](char c) { return c >= '0' && c <= '9'; })) {
        sk.set("seq", jsx::max(num(sk.get("seq")), base::js_number(s.substr(1)) + 1));
      }
    }
    return sk;
  });
}

}  // namespace cc::partcraft::sketch
