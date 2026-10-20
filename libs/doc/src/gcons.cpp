// SPDX-License-Identifier: Apache-2.0
// gcons.js (CadCraft 0.5) in C++.
#include "cc/doc/gcons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/json.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cad/gsolve.hpp"

namespace cc::doc::gcons {

using base::Value;
namespace gs = cad::gsolve;
namespace pr = cad::params;

namespace {

/// a[i] of an array value (anything else: undefined)
Value at(const Value& a, std::size_t i) {
  return a.is_array() && i < a.as_array().size() ? a.as_array()[i] : Value();
}

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2 * kPi;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double num(const Value& v) {
  return base::js_to_number(v);
}
double hyp(double a, double b) {
  return base::js_hypot(a, b);
}
double jor(double a, double d) {
  return a != 0 && !std::isnan(a) ? a : d;
}
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
double round6(double v) {
  return base::js_round(v * 1e6) / 1e6;
}
std::string numtext(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}
std::string type_of(const Value& e) {
  const Value t = e.get("type");
  return t.is_string() ? t.as_string() : std::string();
}
/// the entity a reference names in the model (nullptr: gone)
const Value* entity(const Doc& doc, double id) {
  if (!std::isfinite(id) || id < 0 || id > 9e15 || id != std::floor(id)) {
    return nullptr;
  }
  return doc.model()->get(static_cast<Id>(id));
}
const Value* ref_entity(const Doc& doc, const Value& r) {
  return entity(doc, gs::ref_id(r));
}

constexpr std::array<TypeInfo, 17> kTypes{{
    {"horizontal", "gcons.type.horizontal", {"L", "PP"}, "H", "", "", false},
    {"vertical", "gcons.type.vertical", {"L", "PP"}, "V", "", "", false},
    {"perpendicular", "gcons.type.perpendicular", {"LL"}, "\xE2\x8A\xA5", "", "", false},
    {"parallel", "gcons.type.parallel", {"LL"}, "\xE2\x88\xA5", "", "", false},
    {"tangent", "gcons.type.tangent", {"LR", "RR"}, "T", "", "", false},
    {"coincident", "gcons.type.coincident", {"PP", "PL", "PR"}, "\xE2\x80\xA2", "", "", false},
    {"concentric", "gcons.type.concentric", {"RR"}, "\xE2\x97\x8E", "", "", false},
    {"collinear", "gcons.type.collinear", {"LL"}, "\xE2\xAB\xBD", "", "", false},
    {"symmetric", "gcons.type.symmetric", {"PPL", "LLL", "RRL"}, "\xE2\x87\x94", "", "", false},
    {"equal", "gcons.type.equal", {"LL", "RR"}, "=", "", "", false},
    {"fix", "gcons.type.fix", {"P", "E"}, "\xE2\x8A\x95", "", "", false},
    {"hdist", "gcons.dimtype.hdist", {"PP"}, "", "d", "length", true},
    {"vdist", "gcons.dimtype.vdist", {"PP"}, "", "d", "length", true},
    {"dist", "gcons.dimtype.dist", {"PP", "PL", "LL"}, "", "d", "length", true},
    {"radius", "gcons.dimtype.radius", {"R"}, "", "rad", "length", true},
    {"diameter", "gcons.dimtype.diameter", {"R"}, "", "dia", "length", true},
    {"angle", "gcons.dimtype.angle", {"LL", "R"}, "", "ang", "angle", true},
}};

struct XY {
  double x = 0, y = 0;
};
XY P(const Doc& doc, const Value& refs, std::size_t i) {
  const Value r = at(refs, i);
  const Value* e = ref_entity(doc, r);
  const auto p = e != nullptr ? gs::ref_point(*e, r.get("k")) : std::nullopt;
  return p ? XY{p->x, p->y} : XY{kNaN, kNaN};
}
gs::RefLine L(const Doc& doc, const Value& refs, std::size_t i) {
  const Value r = at(refs, i);
  const Value* e = ref_entity(doc, r);
  return e != nullptr ? gs::ref_line(*e, r.get("k")) : gs::RefLine{kNaN, kNaN, kNaN, kNaN};
}
gs::RefCircle C(const Doc& doc, const Value& refs, std::size_t i) {
  const Value r = at(refs, i);
  const Value* e = ref_entity(doc, r);
  return e != nullptr ? gs::ref_circle(*e, r.get("k")) : gs::RefCircle{kNaN, kNaN, kNaN};
}
std::optional<char> K(const Doc& doc, const Value& refs, std::size_t i) {
  const Value r = at(refs, i);
  return gs::ref_kind(ref_entity(doc, r), r.get("k"));
}

/// sdist(p, l): signed distance of a point from the infinite line l, left of its direction positive
double sdist(double px, double py, const gs::RefLine& l) {
  const double ux = l.x2 - l.x1;
  const double uy = l.y2 - l.y1;
  return (ux * (py - l.y1) - uy * (px - l.x1)) / jor(hyp(ux, uy), 1e-300);
}
std::pair<double, double> mirror_gap(XY a, XY b, const gs::RefLine& l) {
  const double ux = l.x2 - l.x1;
  const double uy = l.y2 - l.y1;
  const double l2 = jor(ux * ux + uy * uy, 1e-300);
  const double t = ((a.x - l.x1) * ux + (a.y - l.y1) * uy) / l2;
  const double fx = l.x1 + t * ux;
  const double fy = l.y1 + t * uy;
  return {2 * fx - a.x - b.x, 2 * fy - a.y - b.y};
}
XY intersect_lines(const gs::RefLine& l1, const gs::RefLine& l2) {
  const double d1x = l1.x2 - l1.x1;
  const double d1y = l1.y2 - l1.y1;
  const double d2x = l2.x2 - l2.x1;
  const double d2y = l2.y2 - l2.y1;
  const double den = d1x * d2y - d1y * d2x;
  if (std::abs(den) < 1e-300) {
    return {l1.x1, l1.y1};
  }
  const double t = ((l2.x1 - l1.x1) * d2y - (l2.y1 - l1.y1) * d2x) / den;
  return {l1.x1 + d1x * t, l1.y1 + d1y * t};
}
XY foot(XY p, const gs::RefLine& l) {
  const double ux = l.x2 - l.x1;
  const double uy = l.y2 - l.y1;
  const double l2 = jor(ux * ux + uy * uy, 1e-300);
  const double t = ((p.x - l.x1) * ux + (p.y - l.y1) * uy) / l2;
  return {l.x1 + t * ux, l.y1 + t * uy};
}
/// dimPoints(c): the two points a linear dimensional constraint measures between
std::pair<XY, XY> dim_points(const Doc& doc, const Value& refs) {
  if (K(doc, refs, 1) == 'P') {
    return {P(doc, refs, 0), P(doc, refs, 1)};
  }
  if (K(doc, refs, 0) == 'P') {
    const XY p = P(doc, refs, 0);
    return {p, foot(p, L(doc, refs, 1))};
  }
  const gs::RefLine l2 = L(doc, refs, 1);
  const XY p{l2.x1, l2.y1};
  return {foot(p, L(doc, refs, 0)), p};
}

struct Anchor {
  double x = 0, y = 0;
  std::optional<std::pair<double, double>> n;
  double size = kNaN, r = kNaN;
};
/// anchor(doc, c): the point a dimension's label hangs from
std::optional<Anchor> anchor(const Doc& doc, const Value& c) {
  const Value refs = c.get("refs");
  for (const Value& r : refs.as_array()) {
    if (ref_entity(doc, r) == nullptr) {
      return std::nullopt;  // the JS's try: a reference to nothing throws
    }
  }
  const std::string type = base::js_string(c.get("type"));
  if (type == "hdist" || type == "vdist" || type == "dist") {
    const auto [p, q] = dim_points(doc, refs);
    const double dx = q.x - p.x;
    const double dy = q.y - p.y;
    const double l2 = jor(hyp(dx, dy), 1);
    std::pair<double, double> n{-dy / l2, dx / l2};
    if (type == "hdist") {
      n = {0, 1};
    } else if (type == "vdist") {
      n = {-1, 0};
    }
    return Anchor{.x = (p.x + q.x) / 2, .y = (p.y + q.y) / 2, .n = n, .size = jmax(l2, 1e-6), .r = kNaN};
  }
  if (type == "radius" || type == "diameter") {
    const gs::RefCircle k = C(doc, refs, 0);
    return Anchor{.x = k.cx, .y = k.cy, .n = std::nullopt, .size = kNaN, .r = k.r};
  }
  if (type == "angle") {
    if (K(doc, refs, 0) == 'R') {
      const Value* e = ref_entity(doc, at(refs, 0));
      return Anchor{
          .x = num(e->get("cx")), .y = num(e->get("cy")), .n = std::nullopt, .size = kNaN, .r = num(e->get("r"))};
    }
    const gs::RefLine l1 = L(doc, refs, 0);
    const gs::RefLine l2 = L(doc, refs, 1);
    const XY x = intersect_lines(l1, l2);
    return Anchor{.x = x.x,
                  .y = x.y,
                  .n = std::nullopt,
                  .size = kNaN,
                  .r = jmin(hyp(l1.x2 - l1.x1, l1.y2 - l1.y1), hyp(l2.x2 - l2.x1, l2.y2 - l2.y1))};
  }
  return std::nullopt;
}
/// labelOffset(doc, c, loc)
std::pair<double, double> label_offset(const Doc& doc, const Value& c,
                                       const std::optional<std::pair<double, double>>& loc) {
  const auto a = anchor(doc, c);
  if (!a) {
    return {0, 0};
  }
  const std::string type = base::js_string(c.get("type"));
  if (type == "angle") {
    const double r = loc ? hyp(loc->first - a->x, loc->second - a->y) : a->r * 0.6;
    return {jmax(r, 1e-9), 0};
  }
  if (!loc) {
    if (type == "radius" || type == "diameter") {
      return {a->r * 0.7071 * 1.6, a->r * 0.7071 * 1.6};
    }
    const auto n = a->n.value_or(std::pair{0.0, 1.0});
    return {n.first * a->size * 0.25, n.second * a->size * 0.25};
  }
  return {loc->first - a->x, loc->second - a->y};
}

std::string sig_words(const TypeInfo& t, const i18n::Catalog& words) {
  const auto w = [&words](char k) {
    switch (k) {
      case 'P': return words.t("gcons.sig.point");
      case 'L': return words.t("gcons.sig.line");
      case 'R': return words.t("gcons.circle_arc");
      default: return words.t("gcons.an_object");
    }
  };
  std::string all;
  for (std::size_t i = 0; i < t.sigs.size() && !t.sigs[i].empty(); ++i) {
    std::string one;
    for (std::size_t j = 0; j < t.sigs[i].size(); ++j) {
      one = j == 0 ? w(t.sigs[i][j]) : words.t("gcons.sig.and", {{"a", one}, {"b", w(t.sigs[i][j])}});
    }
    all = i == 0 ? one : words.t("gcons.sig.or", {{"a", all}, {"b", one}});
  }
  return all;
}

}  // namespace

std::span<const TypeInfo> types() {
  return kTypes;
}
bool has_sig(const TypeInfo& t, std::string_view sig) {
  return !sig.empty() && std::ranges::find(t.sigs, sig) != t.sigs.end();
}
const TypeInfo* type_info(std::string_view type) {
  // NOLINTNEXTLINE(readability-qualified-auto) — a pointer natively, a class under Emscripten
  const auto it = std::ranges::find_if(kTypes, [type](const TypeInfo& t) { return t.type == type; });
  return it == kTypes.end() ? nullptr : &*it;
}
bool is_dim(const Value& c) {
  const TypeInfo* t = c.is_object() && !c.is_point() ? type_info(base::js_string(c.get("type"))) : nullptr;
  return t != nullptr && t->dimensional;
}
std::string target_key(const Value& c) {
  return "c:" + base::js_string(c.get("id"));
}

// ---------------------------------------------------------------- references

std::vector<PointRef> constraint_points(const Value& e) {
  std::vector<PointRef> out;
  const auto add = [&](const std::string& k) {
    if (const auto p = gs::ref_point(e, Value(k))) {
      out.push_back({.k = k, .x = p->x, .y = p->y, .d = 0});
    }
  };
  const std::string t = type_of(e);
  if (t == "line") {
    add("p1");
    add("p2");
    add("mid");
  } else if (t == "circle") {
    add("c");
  } else if (t == "arc") {
    add("s");
    add("e");
    add("c");
    add("mid");
  } else if (t == "point") {
    add("p");
  } else if (t == "poly") {
    const Value pts = e.get("pts");
    for (std::size_t i = 0; pts.is_array() && i < pts.as_array().size(); ++i) {
      add("v" + std::to_string(i));
    }
  }
  return out;
}

std::optional<PointRef> nearest_point_ref(const Value& e, double px, double py) {
  std::optional<PointRef> best;
  for (const PointRef& q : constraint_points(e)) {
    const double d = hyp(q.x - px, q.y - py);
    if (!best || d < best->d) {
      best = PointRef{.k = q.k, .x = q.x, .y = q.y, .d = d};
    }
  }
  return best;
}

std::optional<std::string> curve_ref(const Value& e, double px, double py) {
  const std::string t = type_of(e);
  if (t == "line") {
    return "L";
  }
  if (t == "circle" || t == "arc") {
    return "R";
  }
  if (t != "poly") {
    return std::nullopt;
  }
  std::optional<std::string> best;
  double bd = std::numeric_limits<double>::infinity();
  const Value pts = e.get("pts");
  const auto& pa = pts.as_array();
  const double n = gs::seg_count(e);
  for (std::size_t i = 0; static_cast<double>(i) < n; ++i) {
    const Value& a = pa[i];
    const Value& b = pa[(i + 1) % pa.size()];
    double d = 0;
    if (std::abs(jor(num(a.get("b")), 0)) > 1e-12) {
      const gs::RefCircle k = gs::ref_circle(e, Value("s" + std::to_string(i)));
      d = std::abs(hyp(px - k.cx, py - k.cy) - k.r);
    } else {
      const double ax = num(a.get("x"));
      const double ay = num(a.get("y"));
      const double dx = num(b.get("x")) - ax;
      const double dy = num(b.get("y")) - ay;
      const double l2 = jor(dx * dx + dy * dy, 1e-300);
      const double t0 = jmax(0, jmin(1, ((px - ax) * dx + (py - ay) * dy) / l2));
      d = hyp(px - (ax + t0 * dx), py - (ay + t0 * dy));
    }
    if (d < bd) {
      bd = d;
      best = "s" + std::to_string(i);
    }
  }
  return best;
}

std::optional<std::string> signature(const Doc& doc, const Value& refs) {
  std::string s;
  if (!refs.is_array()) {
    return std::nullopt;
  }
  for (const Value& r : refs.as_array()) {
    const auto k = gs::ref_kind(ref_entity(doc, r), r.get("k"));
    if (!k) {
      return std::nullopt;
    }
    s += *k;
  }
  return s;
}

std::string describe_ref(const Doc& doc, const Value& r, const i18n::Catalog& words) {
  const Value* e = ref_entity(doc, r);
  if (e == nullptr) {
    return words.t("gcons.deleted_object");
  }
  const std::string type = type_of(*e);
  const bool known = type == "line" || type == "circle" || type == "arc" || type == "poly" || type == "point";
  std::string n = words.t("gcons.ref.object",
                          {{"type", known ? words.t("gcons.word." + type) : type}, {"id", numtext(gs::ref_id(r))}});
  const std::string k = base::js_string(r.get("k"));
  if (k == "*" || k == "L" || k == "R" || k == "p") {
    return n;
  }
  if (k == "p1" || k == "s") {
    return words.t("gcons.ref.start", {{"obj", n}});
  }
  if (k == "p2" || k == "e") {
    return words.t("gcons.ref.end", {{"obj", n}});
  }
  if (k == "mid") {
    return words.t("gcons.ref.mid", {{"obj", n}});
  }
  if (k == "c") {
    return words.t("gcons.ref.centre", {{"obj", n}});
  }
  const double i = base::js_number(std::string_view(k).substr(k.empty() ? 0 : 1)) + 1;
  return words.t(!k.empty() && k[0] == 'v' ? "gcons.ref.vertex" : "gcons.ref.segment", {{"obj", n}, {"i", numtext(i)}});
}

std::string describe_constraint(const Doc& doc, const Value& c, const i18n::Catalog& words) {
  const TypeInfo* t = type_info(base::js_string(c.get("type")));
  std::string what;
  const Value refs = c.get("refs");
  for (std::size_t i = 0; refs.is_array() && i < refs.as_array().size(); ++i) {
    what += (i ? ", " : "") + describe_ref(doc, refs.as_array()[i], words);
  }
  const std::string label = t != nullptr ? words.t(t->label_key) : std::string();
  if (is_dim(c)) {
    return words.t("gcons.describe.dim",
                   {{"name", base::js_string(c.get("name"))}, {"kind", base::js_to_lower(label)}, {"what", what}});
  }
  return words.t("gcons.describe", {{"label", label}, {"what", what}});
}

// ---------------------------------------------------------------- making constraints

std::string next_id(const Value& constraints) {
  double m = 0;
  if (constraints.is_array()) {
    for (const Value& c : constraints.as_array()) {
      const std::string id = base::js_string(c.get("id"));
      const double v = base::js_parse_int(std::string_view(id).substr(id.empty() ? 0 : 1));
      m = jmax(m, std::isnan(v) ? 0 : v);  // parseInt(…) || 0
    }
  }
  return "k" + numtext(m + 1);
}

std::string next_name(const pr::Params& p, std::string_view prefix, const std::set<std::string>& taken) {
  for (int i = 1;; ++i) {
    std::string n = std::string(prefix) + std::to_string(i);
    if (pr::var_by_name(p, n) == nullptr && !taken.contains(n)) {
      return n;
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — makeConstraint, one case per type
std::pair<std::optional<Value>, std::string> make_constraint(const Doc& doc, std::string_view type, const Value& refs,
                                                             const MakeOpts& o, const i18n::Catalog& words) {
  const auto fail = [](const std::string& why) {
    return std::pair<std::optional<Value>, std::string>{std::nullopt, why};
  };
  const TypeInfo* T = type_info(type);
  if (T == nullptr) {
    return fail(words.t("gcons.unknown", {{"type", std::string(type)}}));
  }
  if (!doc.in_model() && !o.model_space) {
    return fail(words.t("gcons.constraints_work_model_tab"));
  }
  for (const Value& r : refs.as_array()) {
    const Value* e = ref_entity(doc, r);
    if (e == nullptr || !gs::solvable(*e)) {
      return fail(words.t("gcons.constraints_work_lines_polylines"));
    }
  }
  const auto sig = signature(doc, refs);
  if (!sig || !has_sig(*T, *sig)) {
    return fail(words.t("gcons.needs", {{"what", words.t(T->label_key)}, {"sigs", sig_words(*T, words)}}));
  }
  {
    std::set<std::string> seen;
    for (const Value& r : refs.as_array()) {
      seen.insert(numtext(gs::ref_id(r)) + ":" + base::js_string(r.get("k")));
    }
    if (seen.size() != refs.as_array().size()) {
      return fail(words.t("gcons.pick_two_different_things"));
    }
  }
  base::Array rs;
  for (const Value& r : refs.as_array()) {
    rs.push_back(base::make_object({{"e", r.get("e")}, {"k", r.get("k")}}));
  }
  Value c = base::make_object({{"id", o.id ? Value(*o.id) : Value(next_id(doc.constraints))},
                               {"type", std::string(type)},
                               {"refs", Value(rs)}});
  const std::string t(type);
  if (t == "fix") {
    if (*sig == "P") {
      const XY p = P(doc, refs, 0);
      c.set("value", base::Array{p.x, p.y});
    } else {
      const Value* e = ref_entity(doc, at(refs, 0));
      Value fixed = Value::object();
      for (const std::string& f : gs::fields_of(*e)) {
        fixed.set(f, gs::get_field(*e, f));
      }
      c.set("fixed", fixed);
    }
  } else if (t == "tangent") {
    if (*sig == "LR") {
      const gs::RefCircle k = C(doc, refs, 1);
      c.set("sign", sdist(k.cx, k.cy, L(doc, refs, 0)) >= 0 ? 1 : -1);
    } else {
      const gs::RefCircle a = C(doc, refs, 0);
      const gs::RefCircle b = C(doc, refs, 1);
      const double d = hyp(b.cx - a.cx, b.cy - a.cy);
      if (d < jmax(a.r, b.r) - 1e-12) {
        c.set("kind", "int");
        c.set("sign", a.r >= b.r ? 1 : -1);
      }
    }
  } else if (t == "symmetric") {
    if (*sig == "LLL") {
      const gs::RefLine a = L(doc, refs, 0);
      const gs::RefLine b = L(doc, refs, 1);
      const gs::RefLine m = L(doc, refs, 2);
      const auto h = [](std::pair<double, double> g) { return hyp(g.first, g.second); };
      const double same = h(mirror_gap({a.x1, a.y1}, {b.x1, b.y1}, m)) + h(mirror_gap({a.x2, a.y2}, {b.x2, b.y2}, m));
      const double swap = h(mirror_gap({a.x1, a.y1}, {b.x2, b.y2}, m)) + h(mirror_gap({a.x2, a.y2}, {b.x1, b.y1}, m));
      c.set("pair", swap < same ? 1 : 0);
    }
  } else if (t == "hdist" || t == "vdist") {
    const XY a = P(doc, refs, 0);
    const XY b = P(doc, refs, 1);
    const double d = t == "hdist" ? b.x - a.x : b.y - a.y;
    c.set("sign", d >= 0 ? 1 : -1);
  } else if (t == "dist") {
    if (*sig == "PL") {
      const XY p = P(doc, refs, 0);
      c.set("sign", sdist(p.x, p.y, L(doc, refs, 1)) >= 0 ? 1 : -1);
    } else if (*sig == "LL") {
      const gs::RefLine l2 = L(doc, refs, 1);
      c.set("sign", sdist(l2.x1, l2.y1, L(doc, refs, 0)) >= 0 ? 1 : -1);
    }
  } else if (t == "angle") {
    if (*sig == "LL") {
      const gs::RefLine l1 = L(doc, refs, 0);
      const gs::RefLine l2 = L(doc, refs, 1);
      const std::array<double, 2> u{l1.x2 - l1.x1, l1.y2 - l1.y1};
      const std::array<double, 2> v{l2.x2 - l2.x1, l2.y2 - l2.y1};
      if (std::abs(u[0] * v[1] - u[1] * v[0]) < 1e-9 * hyp(u[0], u[1]) * hyp(v[0], v[1])) {
        return fail(words.t("gcons.those_lines_parallel_there"));
      }
      double s1 = 1;
      double s2 = 1;
      if (o.loc) {
        // the sector holding the label: each leg points from the corner toward the side the label is on
        const XY x = intersect_lines(l1, l2);
        const std::array<double, 2> w{o.loc->first - x.x, o.loc->second - x.y};
        const auto bis = [](std::array<double, 2> a, std::array<double, 2> b) {
          return std::array<double, 2>{a[0] / hyp(a[0], a[1]) + b[0] / hyp(b[0], b[1]),
                                       a[1] / hyp(a[0], a[1]) + b[1] / hyp(b[0], b[1])};
        };
        double best = -std::numeric_limits<double>::infinity();
        for (const double a : {1.0, -1.0}) {
          for (const double b : {1.0, -1.0}) {
            const auto m = bis({u[0] * a, u[1] * a}, {v[0] * b, v[1] * b});
            const double sc = (m[0] * w[0] + m[1] * w[1]) / jor(hyp(m[0], m[1]), 1);
            if (sc > best) {
              best = sc;
              s1 = a;
              s2 = b;
            }
          }
        }
      }
      c.set("dirs", base::Array{s1, s2});
      const std::array<double, 2> uu{u[0] * s1, u[1] * s1};
      const std::array<double, 2> vv{v[0] * s2, v[1] * s2};
      c.set("sign", base::js_atan2(uu[0] * vv[1] - uu[1] * vv[0], uu[0] * vv[0] + uu[1] * vv[1]) >= 0 ? 1 : -1);
    } else if (type_of(*ref_entity(doc, at(refs, 0))) != "arc") {
      return fail(words.t("gcons.angle_needs_two_lines"));
    }
  }
  if (T->dimensional) {
    const pr::Params params = pr::from_value(doc.params);
    c.set("name", o.name ? *o.name : next_name(params, T->prefix));
    c.set("value", o.value ? *o.value : measure(doc, c));
    const auto off = label_offset(doc, c, o.loc);
    c.set("off", base::Array{off.first, off.second});
  }
  return {std::move(c), std::string()};
}

double measure(const Doc& doc, const Value& c) {
  const Value refs = c.get("refs");
  const double s = jor(num(c.get("sign")), 1);
  const std::string type = base::js_string(c.get("type"));
  if (type == "hdist") {
    return s * (P(doc, refs, 1).x - P(doc, refs, 0).x);
  }
  if (type == "vdist") {
    return s * (P(doc, refs, 1).y - P(doc, refs, 0).y);
  }
  if (type == "dist") {
    if (K(doc, refs, 1) == 'P') {
      const XY a = P(doc, refs, 0);
      const XY b = P(doc, refs, 1);
      return hyp(b.x - a.x, b.y - a.y);
    }
    if (K(doc, refs, 0) == 'P') {
      const XY p = P(doc, refs, 0);
      return s * sdist(p.x, p.y, L(doc, refs, 1));
    }
    const gs::RefLine l2 = L(doc, refs, 1);
    return s * sdist(l2.x1, l2.y1, L(doc, refs, 0));
  }
  if (type == "radius") {
    return C(doc, refs, 0).r;
  }
  if (type == "diameter") {
    return 2 * C(doc, refs, 0).r;
  }
  if (type == "angle") {
    if (K(doc, refs, 0) == 'R') {
      const Value* e = ref_entity(doc, at(refs, 0));
      double sw = std::fmod(num(e->get("a1")) - num(e->get("a0")), kTau);
      if (sw <= 0) {
        sw += kTau;
      }
      return sw * 180 / kPi;
    }
    const gs::RefLine l1 = L(doc, refs, 0);
    const gs::RefLine l2 = L(doc, refs, 1);
    const Value dirs = c.get("dirs");
    const double s1 = dirs.truthy() ? num(at(dirs, 0)) : 1;
    const double s2 = dirs.truthy() ? num(at(dirs, 1)) : 1;
    const std::array<double, 2> u{(l1.x2 - l1.x1) * s1, (l1.y2 - l1.y1) * s1};
    const std::array<double, 2> v{(l2.x2 - l2.x1) * s2, (l2.y2 - l2.y1) * s2};
    return s * base::js_atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]) * 180 / kPi;
  }
  return 0;
}

std::string fmt_value(const Value& c, double v) {
  const std::string s = cad::expr::format_number(round6(v));
  return base::js_string(c.get("type")) == "angle" ? s + "\xC2\xB0" : s;
}

std::string label_of(const Value& c, const pr::Params* p, const i18n::Catalog& words) {
  const std::string name = base::js_string(c.get("name"));
  const pr::Var* v = p != nullptr ? pr::var_by_name(*p, name) : nullptr;
  const double value = num(c.get("value"));
  const std::string expr = v != nullptr ? v->expr : cad::expr::format_number(round6(value));
  if (cad::expr::literal(expr, words)) {
    return name + "=" + fmt_value(c, value);
  }
  return "fx: " + name + "=" + expr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — dimEntityFor, one case per type
std::optional<Value> dim_entity_for(const Doc& doc, const Value& c, const Value* st, const i18n::Catalog& words) {
  const Value refs = c.get("refs");
  for (const Value& r : refs.as_array()) {
    const Value* e = ref_entity(doc, r);
    if (e == nullptr || !gs::ref_kind(e, r.get("k"))) {
      return std::nullopt;
    }
  }
  const auto a = anchor(doc, c);
  if (!a) {
    return std::nullopt;
  }
  const Value offv = c.get("off");
  const double o0 = offv.truthy() ? num(at(offv, 0)) : 0;
  const double o1 = offv.truthy() ? num(at(offv, 1)) : 0;
  const pr::Params params = pr::from_value(doc.params);
  Value stv = st != nullptr && st->truthy() ? *st : doc.current.get("dim");
  Value stc = Value::object();
  if (stv.is_object() && !stv.is_point()) {
    stc = stv;  // { ...st }
  }
  Value base = base::make_object(
      {{"type", "dim"}, {"layer", "0"}, {"color", nullptr}, {"text", label_of(c, &params, words)}, {"st", stc}});
  const auto pt = [](double x, double y) { return base::make_object({{"x", x}, {"y", y}}); };
  const std::string type = base::js_string(c.get("type"));
  if (type == "hdist" || type == "vdist" || type == "dist") {
    const auto [p1, p2] = dim_points(doc, refs);
    if (hyp(p2.x - p1.x, p2.y - p1.y) < 1e-12) {
      return std::nullopt;
    }
    base.set("kind", type == "dist" ? "aligned" : "linear");
    base.set("p1", pt(p1.x, p1.y));
    base.set("p2", pt(p2.x, p2.y));
    base.set("loc", pt(a->x + o0, a->y + o1));
    if (type != "dist") {
      base.set("rot", type == "hdist" ? 0 : kPi / 2);
    }
    return base;
  }
  if (type == "radius" || type == "diameter") {
    const gs::RefCircle k = C(doc, refs, 0);
    const double d = jor(hyp(o0, o1), 1);
    const double ux = o0 / d;
    const double uy = o1 / d;
    base.set("kind", type);
    base.set("c", pt(k.cx, k.cy));
    base.set("p1", pt(k.cx + ux * k.r, k.cy + uy * k.r));
    base.set("loc", pt(k.cx + o0, k.cy + o1));
    return base;
  }
  if (type == "angle") {
    const double R = jor(o0, a->r * 0.6);
    if (K(doc, refs, 0) == 'R') {
      const Value* e = ref_entity(doc, at(refs, 0));
      const double a0 = num(e->get("a0"));
      const double a1 = num(e->get("a1"));
      const double r = num(e->get("r"));
      const double cx = num(e->get("cx"));
      const double cy = num(e->get("cy"));
      double sw = std::fmod(a1 - a0, kTau);
      if (sw <= 0) {
        sw += kTau;
      }
      const double m = a0 + sw / 2;
      base.set("kind", "angular");
      base.set("c", pt(cx, cy));
      base.set("p1", pt(cx + base::js_cos(a0) * r, cy + base::js_sin(a0) * r));
      base.set("p2", pt(cx + base::js_cos(a1) * r, cy + base::js_sin(a1) * r));
      base.set("loc", pt(cx + base::js_cos(m) * R, cy + base::js_sin(m) * R));
      return base;
    }
    const gs::RefLine l1 = L(doc, refs, 0);
    const gs::RefLine l2 = L(doc, refs, 1);
    const Value dirs = c.get("dirs");
    const double s1 = dirs.truthy() ? num(at(dirs, 0)) : 1;
    const double s2 = dirs.truthy() ? num(at(dirs, 1)) : 1;
    const std::array<double, 2> u{(l1.x2 - l1.x1) * s1, (l1.y2 - l1.y1) * s1};
    const std::array<double, 2> v{(l2.x2 - l2.x1) * s2, (l2.y2 - l2.y1) * s2};
    const double lu = hyp(u[0], u[1]);
    const double lv = hyp(v[0], v[1]);
    if (std::abs(u[0] * v[1] - u[1] * v[0]) < 1e-9 * lu * lv) {
      return std::nullopt;
    }
    const std::array<double, 2> b{u[0] / lu + v[0] / lv, u[1] / lu + v[1] / lv};
    const double lb = jor(hyp(b[0], b[1]), 1);
    base.set("kind", "angular");
    base.set("c", pt(a->x, a->y));
    base.set("p1", pt(a->x + u[0] / lu * R, a->y + u[1] / lu * R));
    base.set("p2", pt(a->x + v[0] / lv * R, a->y + v[1] / lv * R));
    base.set("loc", pt(a->x + b[0] / lb * R, a->y + b[1] / lb * R));
    return base;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- solving

namespace {

/// collapsed(was, now): did solving shrink a line, a polyline segment or a radius to (almost) nothing?
bool collapsed(const Value& was, const Value& now) {
  const auto tiny = [](double a, double b) { return b < 1e-6 * jmax(a, 1e-300) && a > 1e-9; };
  const std::string t = type_of(now);
  if (t == "line") {
    return tiny(hyp(num(was.get("x2")) - num(was.get("x1")), num(was.get("y2")) - num(was.get("y1"))),
                hyp(num(now.get("x2")) - num(now.get("x1")), num(now.get("y2")) - num(now.get("y1"))));
  }
  if (t == "circle" || t == "arc") {
    return tiny(num(was.get("r")), num(now.get("r")));
  }
  if (t == "poly") {
    const Value wp = was.get("pts");
    const Value np = now.get("pts");
    const auto& w = wp.as_array();
    const auto& n = np.as_array();
    const double segs = gs::seg_count(now);
    for (std::size_t i = 0; static_cast<double>(i) < segs; ++i) {
      const std::size_t j = (i + 1) % n.size();
      if (i >= w.size() || j >= w.size()) {
        continue;
      }
      if (tiny(hyp(num(w[j].get("x")) - num(w[i].get("x")), num(w[j].get("y")) - num(w[i].get("y"))),
               hyp(num(n[j].get("x")) - num(n[i].get("x")), num(n[j].get("y")) - num(n[i].get("y"))))) {
        return true;
      }
    }
  }
  return false;
}

bool refs_hit(const Value& c, const std::set<double>& ids) {
  const Value refs = c.get("refs");
  return refs.is_array() &&
         std::ranges::any_of(refs.as_array(), [&ids](const Value& r) { return ids.contains(gs::ref_id(r)); });
}

}  // namespace

Solved solve_constraints(const Doc& doc, const base::Array& constraints, const std::set<std::string>* pinned,
                         const std::set<double>* only, bool strict,
                         const std::vector<std::pair<double, Value>>* overlay) {
  Solved out;
  std::vector<std::pair<double, Value>> updates;
  const auto find_update = [&updates](double id) -> const Value* {
    for (const auto& [k, v] : updates) {
      if (k == id) {
        return &v;
      }
    }
    return nullptr;
  };
  const auto base_get = [&doc, overlay](double id) -> const Value* {
    if (overlay != nullptr) {
      for (const auto& [k, v] : *overlay) {
        if (k == id) {
          return &v;
        }
      }
    }
    return entity(doc, id);
  };
  for (const base::Array& g : gs::components(constraints)) {
    if (only != nullptr && !std::ranges::any_of(g, [only](const Value& c) { return refs_hit(c, *only); })) {
      continue;
    }
    const gs::Getter cur = [&](double id) -> const Value* {
      const Value* u = find_update(id);
      return u != nullptr ? u : base_get(id);
    };
    gs::SolveResult r = gs::solve_system(g, cur, pinned);
    if (!r.ok && pinned != nullptr && !pinned->empty() && !strict) {
      r = gs::solve_system(g, cur, nullptr);
      out.freed = true;
    }
    if (r.ok) {
      // a solution that collapses a line, a segment or a radius to nothing is no solution (H + V on one line)
      std::set<double> flat;
      for (const auto& [id, e] : r.updates) {
        if (const Value* was = cur(id); was != nullptr && collapsed(*was, e)) {
          flat.insert(id);
        }
      }
      if (!flat.empty()) {
        for (const Value& c : g) {
          if (refs_hit(c, flat)) {
            out.conflicts.push_back(base::js_string(c.get("id")));
          }
        }
        continue;
      }
    }
    if (!r.ok) {
      if (!r.worst.empty()) {
        out.conflicts.insert(out.conflicts.end(), r.worst.begin(), r.worst.end());
      } else {
        for (const Value& c : g) {
          out.conflicts.push_back(base::js_string(c.get("id")));
        }
      }
      continue;
    }
    for (auto& [id, e] : r.updates) {
      bool set = false;
      for (auto& [k, v] : updates) {
        if (k == id) {
          v = e;
          set = true;
        }
      }
      if (!set) {
        updates.emplace_back(id, e);
      }
    }
  }
  out.ok = out.conflicts.empty();
  out.updates = std::move(updates);
  return out;
}

Status status(const Doc& doc) {
  Status s;
  s.count = doc.constraints.is_array() ? static_cast<double>(doc.constraints.as_array().size()) : 0;
  base::Array cs;
  if (doc.constraints.is_array()) {
    for (const Value& c : doc.constraints.as_array()) {
      const Value refs = c.get("refs");
      if (std::ranges::all_of(refs.as_array(), [&doc](const Value& r) { return ref_entity(doc, r) != nullptr; })) {
        cs.push_back(c);
      }
    }
  }
  const gs::Getter get = [&doc](double id) { return entity(doc, id); };
  for (const base::Array& g : gs::components(cs)) {
    const gs::Analysis a = gs::analyze(g, get);
    s.dof += a.dof;
    s.conflicts.insert(s.conflicts.end(), a.conflicts.begin(), a.conflicts.end());
    s.redundant.insert(s.redundant.end(), a.redundant.begin(), a.redundant.end());
    for (const double id : a.determined) {
      if (std::ranges::find(s.determined, id) == s.determined.end()) {
        s.determined.push_back(id);
      }
    }
  }
  std::set<double> objects;
  for (const Value& c : cs) {
    for (const Value& r : c.get("refs").as_array()) {
      objects.insert(gs::ref_id(r));
    }
  }
  s.objects = static_cast<double>(objects.size());
  return s;
}

std::string status_text(const Doc& doc, const i18n::Catalog& words) {
  const Status s = status(doc);
  if (s.count == 0) {
    return words.t("gcons.no_constraints");
  }
  const std::string count =
      words.tn("gcons.status.count", s.count, {{"objects", words.tn("gcons.n.objects", s.objects, {})}});
  std::string state;
  if (!s.conflicts.empty()) {
    state = words.tn("gcons.status.notmet", static_cast<double>(s.conflicts.size()), {});
  } else if (!s.redundant.empty()) {
    state = words.tn("gcons.status.over", static_cast<double>(s.redundant.size()), {});
  } else if (s.dof == 0) {
    state = words.t("gcons.fully_constrained");
  } else {
    state = words.tn("gcons.status.dof", s.dof, {});
  }
  return words.t("gcons.status.line", {{"count", count}, {"state", state}});
}

pr::Params drop_dim_params(const pr::Params& params, const base::Array& dropped, const i18n::Catalog& words) {
  pr::Params p = params;
  for (const Value& c : dropped) {
    if (!is_dim(c)) {
      continue;
    }
    (void)pr::remove_link(p, target_key(c));
    const std::string name = base::js_string(c.get("name"));
    if (pr::var_by_name(p, name) != nullptr) {
      (void)pr::remove_var(p, name, true, words);
    }
  }
  return p;
}

bool fits(const Doc& doc, const Value& c) {
  const TypeInfo* t = type_info(base::js_string(c.get("type")));
  const auto sig = signature(doc, c.get("refs"));
  return t != nullptr && sig && has_sig(*t, *sig);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — afterEdit, as the JS
std::optional<std::string> after_edit(Doc& doc, const std::vector<Op>& ops, const i18n::Catalog& words) {
  if (doc.solving || !doc.constraints.is_array() || doc.constraints.as_array().empty()) {
    return std::nullopt;
  }
  const MapPtr& model = doc.model();
  std::vector<std::pair<double, Value>> before;  // id → the entity before this change (undefined: added)
  for (const Op& op : ops) {
    if (op.kind != Op::Kind::entity || op.map != model) {
      continue;
    }
    const auto id = static_cast<double>(op.id);
    if (std::ranges::none_of(before, [id](const auto& b) { return b.first == id; })) {
      before.emplace_back(id, op.before);
    }
  }
  if (before.empty()) {
    return std::nullopt;
  }
  std::set<double> ref_ids;
  for (const Value& c : doc.constraints.as_array()) {
    for (const Value& r : c.get("refs").as_array()) {
      ref_ids.insert(gs::ref_id(r));
    }
  }
  std::vector<double> hit;
  for (const auto& [id, b] : before) {
    if (ref_ids.contains(id)) {
      hit.push_back(id);
    }
  }
  if (hit.empty()) {
    return std::nullopt;
  }
  base::Array keep;
  base::Array dropped;
  for (const Value& c : doc.constraints.as_array()) {
    (fits(doc, c) ? keep : dropped).push_back(c);
  }
  const auto before_of = [&before](double id) -> const Value* {
    for (const auto& [k, v] : before) {
      if (k == id) {
        return v.is_object() && !v.is_point() ? &v : nullptr;
      }
    }
    return nullptr;
  };
  // what moved: each changed field of each constrained object is held where the edit put it
  std::set<std::string> pinned;
  std::vector<double> pinned_ids;  // in the order pinned (a Set of +k.split('|')[0])
  for (const double id : hit) {
    const Value* was = before_of(id);
    const Value* now = entity(doc, id);
    if (was == nullptr || now == nullptr || type_of(*was) != type_of(*now)) {
      continue;
    }
    if (type_of(*now) == "poly") {
      const Value wp = was->get("pts");
      const Value np = now->get("pts");
      if (!wp.is_array() || !np.is_array() || wp.as_array().size() != np.as_array().size() ||
          !(was->get("closed") == now->get("closed"))) {
        continue;
      }
    }
    for (const std::string& f : gs::fields_of(*now)) {
      const double a = gs::get_field(*was, f);
      const double b = gs::get_field(*now, f);
      if (std::abs(a - b) > 1e-12 * jmax(1, std::abs(a))) {
        std::string key;
        base::append_number(key, id);
        key += '|';
        key += f;
        pinned.insert(key);
        if (std::ranges::find(pinned_ids, id) == pinned_ids.end()) {
          pinned_ids.push_back(id);
        }
      }
    }
  }
  std::vector<std::string> notes;
  if (!dropped.empty()) {
    const pr::Params p = drop_dim_params(pr::from_value(doc.params), dropped, words);
    doc.set_parametric(Value(keep), pr::to_value(p));
    notes.push_back(words.tn("gcons.note.dropped", static_cast<double>(dropped.size()), {}));
  }
  if (!pinned.empty()) {
    const std::set<double> moved(pinned_ids.begin(), pinned_ids.end());
    const gs::Getter was_get = [&](double id) -> const Value* {
      for (const auto& [k, v] : before) {
        if (k == id) {
          return v.is_object() && !v.is_point() ? &v : nullptr;
        }
      }
      return entity(doc, id);
    };
    // a group that was not met before this edit (an imported drawing, say) is left as it is
    base::Array groups;
    for (const base::Array& g : gs::components(keep)) {
      if (std::ranges::any_of(g, [&moved](const Value& c) { return refs_hit(c, moved); }) &&
          gs::residual_ok(g, was_get)) {
        groups.insert(groups.end(), g.begin(), g.end());
      }
    }
    if (!groups.empty()) {
      const Solved r = solve_constraints(doc, groups, &pinned, &moved, false);
      if (!r.ok) {
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const std::string& id : r.conflicts) {
          if (!seen.insert(id).second || names.size() >= 3) {
            continue;
          }
          for (const Value& c : keep) {
            if (base::js_string(c.get("id")) == id) {
              names.push_back(describe_constraint(doc, c, words));
              break;
            }
          }
        }
        std::string joined;
        for (std::size_t i = 0; i < names.size(); ++i) {
          joined += (i ? "; " : "") + names[i];
        }
        return words.t("gcons.refuse.follow", {{"names", joined}});
      }
      // pins off and everything went straight back: the edit cannot happen at all (fully constrained, or fixed)
      if (r.freed) {
        const bool back = std::ranges::all_of(pinned_ids, [&](double id) {
          const Value* e = nullptr;
          for (const auto& [k, v] : r.updates) {
            if (k == id) {
              e = &v;
            }
          }
          const Value* was = before_of(id);
          if (was == nullptr || e == nullptr) {
            return true;
          }
          const std::vector<std::string> fs = gs::fields_of(*e);
          return std::ranges::all_of(fs, [&](const std::string& f) {
            return std::abs(gs::get_field(*e, f) - gs::get_field(*was, f)) <=
                   1e-7 * jmax(1, std::abs(gs::get_field(*was, f)));
          });
        });
        if (back) {
          std::vector<std::string> held;
          for (const Value& c : keep) {
            if (held.size() >= 3) {
              break;
            }
            if (refs_hit(c, moved) && (base::js_string(c.get("type")) == "fix" || is_dim(c))) {
              held.push_back(describe_constraint(doc, c, words));
            }
          }
          std::string list;
          for (std::size_t i = 0; i < held.size(); ++i) {
            list += (i ? "; " : "") + held[i];
          }
          return words.tn(held.empty() ? "gcons.refuse.hold" : "gcons.refuse.hold.list",
                          static_cast<double>(moved.size()), {{"list", list}});
        }
      }
      std::size_t followers = 0;
      for (const auto& [id, e] : r.updates) {
        if (!moved.contains(id)) {
          ++followers;
        }
      }
      for (const auto& [id, e] : r.updates) {
        (void)doc.update(static_cast<Id>(id), e, model);
      }
      if (r.freed) {
        notes.push_back(words.t("gcons.constraints_held_what_you"));
      } else if (followers > 0) {
        notes.push_back(words.tn("gcons.note.followed", static_cast<double>(followers), {}));
      }
    }
  }
  if (!notes.empty()) {
    std::string all;
    for (std::size_t i = 0; i < notes.size(); ++i) {
      all += (i ? " " : "") + notes[i];
    }
    doc.note(all);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- files

std::optional<ForFile> for_file(const Doc& doc) {
  base::Array cs;
  if (doc.constraints.is_array()) {
    for (const Value& c : doc.constraints.as_array()) {
      const Value refs = c.get("refs");
      if (std::ranges::all_of(refs.as_array(), [&doc](const Value& r) { return ref_entity(doc, r) != nullptr; })) {
        cs.push_back(c);
      }
    }
  }
  const pr::Params params = pr::from_value(doc.params);
  if (cs.empty() && !pr::has_params(params)) {
    return std::nullopt;
  }
  ForFile out;
  Value types = Value::object();
  const auto key_of = [&out](double id) -> std::optional<std::string> {
    for (const auto& [k, v] : out.refs) {
      if (k == id) {
        return v;
      }
    }
    return std::nullopt;
  };
  for (const Value& c : cs) {
    for (const Value& r : c.get("refs").as_array()) {
      const double id = gs::ref_id(r);
      if (!key_of(id)) {
        const std::string key = std::to_string(out.refs.size() + 1);
        out.refs.emplace_back(id, key);
        types.set(key, type_of(*ref_entity(doc, r)));
      }
    }
  }
  base::Array list;
  for (const Value& c : cs) {
    Value k = c;  // { ...c, refs: [{ r: key, k }] }
    base::Array refs;
    for (const Value& r : c.get("refs").as_array()) {
      refs.push_back(base::make_object({{"r", key_of(gs::ref_id(r)).value_or("")}, {"k", r.get("k")}}));
    }
    k.set("refs", Value(std::move(refs)));
    list.push_back(std::move(k));
  }
  const Value payload = base::make_object({{"format", "cadcraft-parametric"},
                                           {"version", 1},
                                           {"refs", types},
                                           {"constraints", Value(std::move(list))},
                                           {"params", pr::to_value(params)}});
  // JSON.stringify, then everything outside 0x21–0x7E as \uXXXX (UTF-16 units)
  const std::string json = base::js_stringify(payload);
  for (const std::uint16_t u : base::to_utf16(json)) {
    if (u >= 0x21 && u <= 0x7e) {
      out.text += static_cast<char>(u);
    } else {
      static constexpr std::string_view kHex = "0123456789abcdef";
      out.text += "\\u";
      for (const unsigned sh : {12U, 8U, 4U, 0U}) {
        out.text += kHex[(u >> sh) & 0xFU];
      }
    }
  }
  for (std::size_t i = 0; i < out.text.size(); i += 230) {
    out.points.push_back("CCPARAM:" + std::to_string(out.points.size() + 1) + ":" + out.text.substr(i, 230));
  }
  return out;
}

std::optional<std::string> ref_key_in(const Value& strings) {
  if (!strings.is_array()) {
    return std::nullopt;
  }
  for (const Value& s0 : strings.as_array()) {
    const std::string s = base::js_string(s0);
    // /^CCREF:(\d{1,9})$/
    if (s.starts_with("CCREF:") && s.size() > 6 && s.size() <= 15 &&
        std::ranges::all_of(s.substr(6), [](char c) { return c >= '0' && c <= '9'; })) {
      return s.substr(6);
    }
  }
  return std::nullopt;
}

namespace {

bool ref_key_ok(const std::string& k) {
  // /^(p1|p2|mid|L|c|R|s|e|p|\*|[vs]\d{1,6})$/
  static const std::array<std::string_view, 10> kFixed{"p1", "p2", "mid", "L", "c", "R", "s", "e", "p", "*"};
  if (std::ranges::find(kFixed, k) != kFixed.end()) {
    return true;
  }
  return k.size() >= 2 && k.size() <= 7 && (k[0] == 'v' || k[0] == 's') &&
         std::ranges::all_of(k.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}
bool field_key_ok(const std::string& f) {
  // /^(x1|y1|x2|y2|cx|cy|r|a0|a1|x|y|\d{1,6}[xy])$/
  static const std::array<std::string_view, 11> kFixed{"x1", "y1", "x2", "y2", "cx", "cy", "r", "a0", "a1", "x", "y"};
  if (std::ranges::find(kFixed, f) != kFixed.end()) {
    return true;
  }
  return f.size() >= 2 && f.size() <= 7 && (f.back() == 'x' || f.back() == 'y') &&
         std::ranges::all_of(f.substr(0, f.size() - 1), [](char c) { return c >= '0' && c <= '9'; });
}
bool fin(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}

/// cleanConstraint(c, refMap): an untrusted constraint → a clean one, or nothing
std::optional<Value> clean_constraint(const Value& c,
                                      const std::vector<std::pair<std::string, std::pair<double, Value>>>* refs_map) {
  if (!c.is_object() || c.is_point()) {
    return std::nullopt;
  }
  const std::string type = c.get("type").is_string() ? c.get("type").as_string() : std::string();
  const TypeInfo* T = type_info(type);
  const Value id = c.get("id");
  const Value refs = c.get("refs");
  // /^k\d{1,9}$/
  const bool id_ok = id.is_string() && id.as_string().size() >= 2 && id.as_string().size() <= 10 &&
                     id.as_string()[0] == 'k' &&
                     std::ranges::all_of(id.as_string().substr(1), [](char ch) { return ch >= '0' && ch <= '9'; });
  if (T == nullptr || !id_ok || !refs.is_array() || refs.as_array().empty() || refs.as_array().size() > 3) {
    return std::nullopt;
  }
  base::Array out_refs;
  std::string sig;
  for (const Value& r : refs.as_array()) {
    const Value k = r.is_object() && !r.is_point() ? r.get("k") : Value();
    if (!k.is_string() || !ref_key_ok(k.as_string())) {
      return std::nullopt;
    }
    double eid = 0;
    if (refs_map != nullptr) {
      const std::string key = base::js_string(r.get("r"));
      const auto it = std::ranges::find_if(*refs_map, [&key](const auto& m) { return m.first == key; });
      if (it == refs_map->end()) {
        return std::nullopt;
      }
      eid = it->second.first;
      if (it->second.second.is_object()) {  // the object read must still fit
        const auto kind = gs::ref_kind(&it->second.second, k);
        if (!kind) {
          return std::nullopt;
        }
        sig += *kind;
      }
    } else {
      const Value e = r.get("e");
      if (!e.is_number() || e.as_number() != std::floor(e.as_number()) || !std::isfinite(e.as_number())) {
        return std::nullopt;
      }
      eid = e.as_number();
    }
    out_refs.push_back(base::make_object({{"e", eid}, {"k", k}}));
  }
  if (!sig.empty() && !has_sig(*T, sig)) {
    return std::nullopt;
  }
  Value k = base::make_object({{"id", id}, {"type", type}, {"refs", Value(std::move(out_refs))}});
  const Value sign = c.get("sign");
  if (sign == Value(1) || sign == Value(-1)) {
    k.set("sign", sign);
  }
  if (c.get("kind") == Value("int")) {
    k.set("kind", "int");
  }
  const Value pair = c.get("pair");
  if (pair == Value(0) || pair == Value(1)) {
    k.set("pair", pair);
  }
  const Value dirs = c.get("dirs");
  if (dirs.is_array() && dirs.as_array().size() == 2 &&
      std::ranges::all_of(dirs.as_array(), [](const Value& d) { return d == Value(1) || d == Value(-1); })) {
    k.set("dirs", dirs);
  }
  if (type == "fix") {
    const Value v = c.get("value");
    const Value fixed = c.get("fixed");
    if (v.is_array() && v.as_array().size() == 2 && std::ranges::all_of(v.as_array(), fin)) {
      k.set("value", v);
    } else if (fixed.is_object() && !fixed.is_point()) {
      Value f = Value::object();
      for (const base::Member* m : base::js_members(fixed.as_object())) {
        if (!field_key_ok(m->key) || !fin(m->value)) {
          return std::nullopt;
        }
        f.set(m->key, m->value);
      }
      k.set("fixed", f);
    } else {
      return std::nullopt;
    }
  } else if (T->dimensional) {
    const Value v = c.get("value");
    const Value name = c.get("name");
    if (!fin(v) || !name.is_string() || !cad::expr::valid_name(name.as_string())) {
      return std::nullopt;
    }
    k.set("value", v);
    k.set("name", name);
    const Value off = c.get("off");
    k.set("off", off.is_array() && off.as_array().size() == 2 && std::ranges::all_of(off.as_array(), fin)
                     ? off
                     : Value(base::Array{0, 0}));
  }
  return k;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — resolveParametric, as the JS
Resolved resolve_parametric(const Value& raw, const std::vector<std::pair<std::string, std::pair<double, Value>>>& refs,
                            const i18n::Catalog& words) {
  Resolved out;
  Value p = raw;
  if (raw.is_string()) {
    if (base::utf16_length(raw.as_string()) > kMaxPayload) {
      out.notes.push_back(words.t("gcons.file.too_large"));
      return out;
    }
    auto parsed = base::parse_json(raw.as_string());
    if (!parsed) {
      out.notes.push_back(words.t("gcons.file.unreadable"));
      return out;
    }
    p = std::move(*parsed);
  }
  if (!p.is_object() || p.is_point() || !(p.get("format") == Value("cadcraft-parametric")) ||
      !(p.get("version") == Value(1)) || !p.get("constraints").is_array()) {
    out.notes.push_back(words.t("gcons.file.unknown_form"));
    return out;
  }
  const Value list = p.get("constraints");
  if (list.as_array().size() > kMaxConstraints) {
    out.notes.push_back(words.t("gcons.file.too_many"));
    return out;
  }
  base::Array kept0;
  double lost = 0;
  std::set<std::string> ids;
  for (const Value& c : list.as_array()) {
    auto k = clean_constraint(c, &refs);
    if (!k || ids.contains(base::js_string(k->get("id")))) {
      ++lost;
      continue;
    }
    ids.insert(base::js_string(k->get("id")));
    kept0.push_back(std::move(*k));
  }
  // names of dimensional constraints must be unique and valid
  std::set<std::string> names;
  base::Array kept;
  for (Value& c : kept0) {
    if (is_dim(c)) {
      const std::string n = base::js_string(c.get("name"));
      if (names.contains(n)) {
        ++lost;
        continue;
      }
      names.insert(n);
    }
    kept.push_back(std::move(c));
  }
  pr::Params params;
  const Value pp = p.get("params");
  if (pp.truthy()) {
    std::set<std::string> keys;
    for (const Value& c : kept) {
      if (is_dim(c)) {
        keys.insert(target_key(c));
      }
    }
    const Value links0 = pp.get("links");
    base::Array links;
    if (links0.is_array()) {
      for (const Value& l : links0.as_array()) {
        const Value t = l.is_object() && !l.is_point() ? l.get("target") : Value();
        if (t.is_string() && keys.contains(t.as_string())) {
          links.push_back(l);
        }
      }
    }
    const double dropped_links =
        (links0.is_array() ? static_cast<double>(links0.as_array().size()) : 0) - static_cast<double>(links.size());
    const Value vars = pp.get("vars");
    auto [got, why] =
        pr::read_params(base::make_object({{"vars", vars.is_null() || vars.is_undefined() ? Value::array() : vars},
                                           {"links", Value(std::move(links))}}),
                        &keys, words);
    if (got) {
      params = std::move(*got);
    } else {
      out.notes.push_back(words.t("gcons.note.params_unreadable", {{"why", why}}));
    }
    if (dropped_links > 0) {
      out.notes.push_back(words.tn("gcons.note.links_dropped", dropped_links, {}));
    }
  }
  // every dimensional constraint has its parameter and link (a file from elsewhere may lack them)
  for (Value& c : kept) {
    if (!is_dim(c)) {
      continue;
    }
    std::string name = base::js_string(c.get("name"));
    if (pr::var_by_name(params, name) == nullptr) {
      if (pr::name_problem(Value(name), &params, nullptr, words)) {
        name = next_name(params, type_info(base::js_string(c.get("type")))->prefix);
        c.set("name", name);
      }
      params.vars.push_back(
          {.name = name, .expr = cad::expr::format_number(round6(num(c.get("value")))), .note = std::nullopt});
    }
    const std::string tk = target_key(c);
    if (std::ranges::none_of(params.links, [&tk](const pr::Link& l) { return l.target == tk; })) {
      params.links.push_back({.target = tk, .expr = name});
    }
  }
  if (lost > 0) {
    out.notes.push_back(words.tn("gcons.note.lost", lost, {}));
  }
  out.constraints = Value(std::move(kept));
  out.params = std::move(params);
  return out;
}

// ---------------------------------------------------------------- clipboard

namespace {

/// literalWord(w): a unit, a function or a constant name (any case)
bool literal_word(std::string_view w0) {
  const std::string w = base::js_to_lower(w0);
  static constexpr std::array<std::string_view, 30> kWords{
      "mm",  "cm",   "m",    "in",    "ft",    "deg",   "rad", "pi",  "sin", "cos",
      "tan", "asin", "acos", "atan",  "atan2", "sqrt",  "pow", "exp", "ln",  "log",
      "abs", "sign", "ceil", "floor", "int",   "round", "min", "max", "if",  "e"};
  return std::ranges::find(kWords, w) != kWords.end();
}
/// /^(<names>|\d+<axis>)$/ for a fixed field of axis 'x' or 'y'
bool on_axis(const std::string& key, char axis) {
  const bool named = axis == 'x' ? (key == "x1" || key == "x2" || key == "cx" || key == "x")
                                 : (key == "y1" || key == "y2" || key == "cy" || key == "y");
  if (named) {
    return true;
  }
  return key.size() >= 2 && key.back() == axis &&
         std::ranges::all_of(key.substr(0, key.size() - 1), [](char ch) { return ch >= '0' && ch <= '9'; });
}
/// fitsIn(model, c)
bool fits_in(const Doc& doc, const Value& c) {
  return fits(doc, c);
}

}  // namespace

base::Array clip_constraints(const Doc& doc, const std::vector<std::pair<double, double>>& index) {
  base::Array out;
  if (!doc.in_model() || !doc.constraints.is_array()) {
    return out;
  }
  const auto at = [&index](double id) -> std::optional<double> {
    for (const auto& [k, i] : index) {
      if (k == id) {
        return i;
      }
    }
    return std::nullopt;
  };
  const pr::Params params = pr::from_value(doc.params);
  for (const Value& c : doc.constraints.as_array()) {
    const Value refs = c.get("refs");
    if (!std::ranges::all_of(refs.as_array(), [&at](const Value& r) { return at(gs::ref_id(r)).has_value(); })) {
      continue;
    }
    Value k = c;  // { ...c, refs: [{ i, k }] }
    base::Array rs;
    for (const Value& r : refs.as_array()) {
      rs.push_back(base::make_object({{"i", at(gs::ref_id(r)).value_or(0)}, {"k", r.get("k")}}));
    }
    k.set("refs", Value(std::move(rs)));
    if (is_dim(c)) {
      const pr::Var* v = pr::var_by_name(params, base::js_string(c.get("name")));
      k.set("expr", v != nullptr ? v->expr : cad::expr::format_number(num(c.get("value"))));
    }
    out.push_back(std::move(k));
  }
  return out;
}

double paste_constraints(Doc& doc, const base::Array& list, const std::vector<std::optional<double>>& ids, double dx,
                         double dy, const i18n::Catalog& words) {
  if (list.empty() || !doc.in_model()) {
    return 0;
  }
  base::Array cs = doc.constraints.is_array() ? doc.constraints.as_array() : base::Array{};
  pr::Params params = pr::from_value(doc.params);
  double n = 0;
  const auto id_at = [&ids](const Value& r) -> std::optional<double> {
    const double i = num(r.get("i"));
    if (!(i >= 0) || i != std::floor(i) || i >= static_cast<double>(ids.size())) {
      return std::nullopt;
    }
    return ids[static_cast<std::size_t>(i)];
  };
  for (const Value& c : list) {
    const Value refs = c.get("refs");
    if (!std::ranges::all_of(refs.as_array(), [&](const Value& r) {
          const auto id = id_at(r);
          return id && entity(doc, *id) != nullptr;
        })) {
      continue;
    }
    Value k = c;
    k.set("id", next_id(Value(cs)));
    base::Array rs;
    for (const Value& r : refs.as_array()) {
      rs.push_back(base::make_object({{"e", id_at(r).value_or(0)}, {"k", r.get("k")}}));
    }
    k.set("refs", Value(std::move(rs)));
    k.erase("expr");
    if (base::js_string(k.get("type")) == "fix") {
      const Value v = k.get("value");
      if (v.truthy()) {
        k.set("value", base::Array{num(at(v, 0)) + dx, num(at(v, 1)) + dy});
      }
      const Value fixed = k.get("fixed");
      if (fixed.truthy()) {
        Value f = fixed;
        for (const base::Member* m : base::js_members(fixed.as_object())) {
          const std::string& key = m->key;
          if (on_axis(key, 'x')) {
            f.set(key, num(m->value) + dx);
          } else if (on_axis(key, 'y')) {
            f.set(key, num(m->value) + dy);
          }
        }
        k.set("fixed", f);
      }
    }
    if (!fits_in(doc, k)) {
      continue;
    }
    if (is_dim(k)) {
      const TypeInfo* T = type_info(base::js_string(k.get("type")));
      const std::string name = next_name(params, T->prefix);
      k.set("name", name);
      const std::string expr = base::js_string(c.get("expr"));
      // the names it uses: /[A-Za-z_][A-Za-z0-9_]*/g, units and functions left out
      std::vector<std::string> uses;
      for (std::size_t i = 0; i < expr.size();) {
        const char ch = expr[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_') {
          std::size_t j = i + 1;
          while (j < expr.size() && ((expr[j] >= 'A' && expr[j] <= 'Z') || (expr[j] >= 'a' && expr[j] <= 'z') ||
                                     (expr[j] >= '0' && expr[j] <= '9') || expr[j] == '_')) {
            ++j;
          }
          const std::string w = expr.substr(i, j - i);
          if (!literal_word(w)) {
            uses.push_back(w);
          }
          i = j;
        } else {
          ++i;
        }
      }
      const bool keep_expr =
          std::ranges::all_of(uses,
                              [&params](const std::string& w) { return pr::var_by_name(params, w) != nullptr; }) &&
          !cad::expr::literal(expr, words);
      params.vars.push_back({.name = name,
                             .expr = keep_expr ? expr : cad::expr::format_number(round6(num(k.get("value")))),
                             .note = std::nullopt});
      params.links.push_back({.target = target_key(k), .expr = name});
    }
    cs.push_back(std::move(k));
    ++n;
  }
  if (n > 0) {
    doc.set_parametric(Value(cs), pr::to_value(params));
  }
  return n;
}

}  // namespace cc::doc::gcons
