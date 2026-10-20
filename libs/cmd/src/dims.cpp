// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.2: dimensions — DIMLINEAR, DIMALIGNED, DIMRADIUS, DIMDIAMETER, DIMANGULAR,
// DIMCONTINUE, DIMBASELINE, the DIMSCALE … DIMEXO settings and DIMLUNIT.
#include "cc/cad/dims.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

Value pt(Pt p) {
  return base::make_object({{"x", p.x}, {"y", p.y}});
}
Pt xy(const Value& v) {
  return {base::js_to_number(v.get("x")), base::js_to_number(v.get("y"))};
}
double num(const Value& e, std::string_view k) {
  return base::js_to_number(e.get(k));
}
std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
/// String(v) of a number
std::string js_num(double v) {
  return base::js_string(Value(v));
}

/// activeUcs(doc): the UCS while the model is open, else none
std::optional<Value> active_ucs(const Ctx& c) {
  if (c.doc.in_model() && c.doc.ucs.truthy()) {
    return c.doc.ucs;
  }
  return std::nullopt;
}
double ucs_ang(const Ctx& c) {
  const auto u = active_ucs(c);
  if (!u) {
    return 0;
  }
  const Value a = u->get("ang");
  const double d = base::js_to_number(a);
  return a.truthy() && !std::isnan(d) ? d : 0;  // activeUcs(c.doc)?.ang || 0
}
/// ucs.js toUcs(u, p)
Pt to_ucs(const std::optional<Value>& u, Pt p) {
  if (!u) {
    return p;
  }
  const double ang = num(*u, "ang");
  const double co = base::js_cos(ang);
  const double si = base::js_sin(ang);
  const Pt v{p.x - num(*u, "x"), p.y - num(*u, "y")};
  return {v.x * co + v.y * si, -v.x * si + v.y * co};
}

/// commands.js dimEnt(c, e): a dimension with the drawing's current style
Value dim_ent(const Ctx& c, const Value& e) {
  Value d = base::make_object({{"type", "dim"}, {"text", ""}, {"st", c.doc.current.get("dim")}});
  for (const base::Member* m : base::js_members(e.as_object())) {
    d.set(m->key, m->value);
  }
  return c.ent(d);
}
/// dims.js formatMeasure(e)
std::string format_measure(const Value& e) {
  return cad::dim_value_main(e, cad::dim_measure(e));
}

using Make = std::function<std::optional<Value>(Pt cur, std::optional<double> rot, const std::string& text)>;

/// commands.js placeDim(c, make, { linear }): where the dimension line goes, with options for text and direction
Task<Value> place_dim(Ctx& c, Make make, bool linear) {
  std::string text;
  std::optional<double> fixed_rot;
  for (;;) {
    c.preview([&make, &fixed_rot, &text](Pt cur) {
      auto d = make(cur, fixed_rot, text);
      return d ? base::Array{std::move(*d)} : base::Array{};
    });
    Ask ap;
    ap.options = {c.opt("T", "Text")};
    if (linear) {
      ap.options.push_back(c.opt("H", "Horizontal"));
      ap.options.push_back(c.opt("V", "Vertical"));
      ap.options.push_back(c.opt("R", "Rotated"));
    }
    const Answer p = co_await c.point(c.t("cmd.explode.specify_dimension_line_location"), std::move(ap));
    if (p.is("T")) {
      const Answer s = co_await c.string("Enter dimension text (<> is the measured value, Enter keeps it)");
      const std::string v = s.kind == Answer::Kind::text ? s.text : std::string();
      text = v.find_first_not_of(" \t\n\r\f\v") == std::string::npos ? std::string() : v;
      continue;
    }
    if (p.is("H")) {  // along the UCS (DR.8)
      fixed_rot = norm_angle(ucs_ang(c));
      continue;
    }
    if (p.is("V")) {
      fixed_rot = norm_angle(ucs_ang(c) + kPi / 2);
      continue;
    }
    if (p.is("R")) {
      Ask aa;
      aa.def = ucs_ang(c);
      aa.ucs = true;
      const Answer a = co_await c.angle(c.t("cmd.explode.specify_angle_dimension_line"), std::move(aa));
      fixed_rot = a.kind == Answer::Kind::number ? a.number : 0;
      continue;
    }
    std::optional<Value> d = make(p.p, fixed_rot, text);
    if (!d) {
      c.print(c.t("cmd.explode.pick_point_inside_angle"));
      continue;
    }
    c.preview(nullptr);
    Value made;
    c.doc.change("DIM", [&] { made = c.doc.add(*d); });
    c.engine.state.set("lastDim", made.get("id"));
    c.print("Dimension text = " + format_measure(made));
    co_return made;
  }
}

/// commands.js linearRot(p1, p2, cur, u): horizontal or vertical by where the dimension line goes, in the UCS
double linear_rot(Pt p1, Pt p2, Pt cur, const std::optional<Value>& u) {
  const Pt a = to_ucs(u, p1);
  const Pt b = to_ucs(u, p2);
  const Pt q = to_ucs(u, cur);
  const double base = u ? num(*u, "ang") : 0;
  const double min_x = std::min(a.x, b.x);
  const double max_x = std::max(a.x, b.x);
  const double min_y = std::min(a.y, b.y);
  const double max_y = std::max(a.y, b.y);
  double out_x = 0;
  if (q.x < min_x) {
    out_x = min_x - q.x;
  } else if (q.x > max_x) {
    out_x = q.x - max_x;
  }
  double out_y = 0;
  if (q.y < min_y) {
    out_y = min_y - q.y;
  } else if (q.y > max_y) {
    out_y = q.y - max_y;
  }
  return norm_angle(base + (out_y >= out_x ? 0 : kPi / 2));
}

cad::P2 prim_at(const cad::Prim& p, double t) {
  if (!p.arc) {
    return {p.x1 + (p.x2 - p.x1) * t, p.y1 + (p.y2 - p.y1) * t};
  }
  const double a = p.a0 + (p.a1 - p.a0) * t;
  return {p.cx + p.r * base::js_cos(a), p.cy + p.r * base::js_sin(a)};
}

/// commands.js twoOrigins(c): two picked points, or (Enter / S) an object's ends
Task<std::pair<Pt, Pt>> two_origins(Ctx& c) {
  Ask aa;
  aa.none = true;
  aa.options = {c.opt("S", c.t("cmd.area.object"))};
  const Answer a = co_await c.point(c.t("cmd.explode.specify_first_extension_line"), std::move(aa));
  if (a.kind == Answer::Kind::point) {
    Ask ab;
    ab.base = a.p;
    const Pt b = (co_await c.point(c.t("cmd.explode.specify_second_extension_line"), std::move(ab))).p;
    co_return std::pair{a.p, b};
  }
  for (;;) {
    const Answer pick = co_await c.entity(c.t("cmd.explode.select_object_dimension"));
    const Value* ep = c.doc.entities().get(pick.id);
    if (ep == nullptr) {
      continue;
    }
    const Value& e = *ep;
    const std::string t = type_of(e);
    if (t == "line") {
      co_return std::pair{Pt{num(e, "x1"), num(e, "y1")}, Pt{num(e, "x2"), num(e, "y2")}};
    }
    if (t == "arc") {
      const Pt cen{num(e, "cx"), num(e, "cy")};
      co_return std::pair{polar(cen, num(e, "a0"), num(e, "r")), polar(cen, num(e, "a1"), num(e, "r"))};
    }
    if (t == "circle") {
      co_return std::pair{Pt{num(e, "cx") - num(e, "r"), num(e, "cy")}, Pt{num(e, "cx") + num(e, "r"), num(e, "cy")}};
    }
    if (t == "poly") {
      const std::vector<cad::Prim> ps = cad::entity_prims(e, nullptr);
      if (!ps.empty()) {
        const cad::Prim* best = ps.data();
        double bd = std::numeric_limits<double>::infinity();
        for (const cad::Prim& q : ps) {
          const cad::P2 m = prim_at(q, 0.5);
          const double d = base::js_hypot(m.x - pick.p.x, m.y - pick.p.y);
          if (d < bd) {
            bd = d;
            best = &q;
          }
        }
        const cad::P2 s = prim_at(*best, 0);
        const cad::P2 f = prim_at(*best, 1);
        co_return std::pair{Pt{s.x, s.y}, Pt{f.x, f.y}};
      }
    }
    c.print(c.t("cmd.explode.pick_line_arc_circle"));
  }
}

Task<void> dimlinear_cmd(Ctx& c) {
  const auto [p1, p2] = co_await two_origins(c);
  const std::optional<Value> u = active_ucs(c);
  (void)co_await place_dim(
      c,
      [&c, p1, p2, u](Pt cur, std::optional<double> rot, const std::string& text) -> std::optional<Value> {
        return dim_ent(c, base::make_object({{"kind", "linear"},
                                             {"p1", pt(p1)},
                                             {"p2", pt(p2)},
                                             {"loc", pt(cur)},
                                             {"rot", rot ? *rot : linear_rot(p1, p2, cur, u)},
                                             {"text", text}}));
      },
      true);
}

Task<void> dimaligned_cmd(Ctx& c) {
  const auto [p1, p2] = co_await two_origins(c);
  if (dist(p1, p2) < 1e-12) {
    c.print(c.t("cmd.dimaligned.two_points_same"));
    co_return;
  }
  (void)co_await place_dim(
      c,
      [&c, p1, p2](Pt cur, std::optional<double>, const std::string& text) -> std::optional<Value> {
        return dim_ent(c, base::make_object(
                              {{"kind", "aligned"}, {"p1", pt(p1)}, {"p2", pt(p2)}, {"loc", pt(cur)}, {"text", text}}));
      },
      false);
}

Task<void> radial(Ctx& c, std::string kind) {
  for (;;) {
    const Answer pick = co_await c.entity(c.t("cmd.mea.arc"));
    const Value* ep = c.doc.entities().get(pick.id);
    if (ep == nullptr || (type_of(*ep) != "circle" && type_of(*ep) != "arc")) {
      c.print(c.t("cmd.mea.arc.only"));
      continue;
    }
    const Pt cen{num(*ep, "cx"), num(*ep, "cy")};
    const double r = num(*ep, "r");
    const Pt p1 = polar(cen, angle_of(cen, pick.p), r);
    (void)co_await place_dim(
        c,
        [&c, kind, cen, r, p1](Pt cur, std::optional<double>, const std::string& text) -> std::optional<Value> {
          const Pt at = dist(cen, cur) > 1e-12 ? polar(cen, angle_of(cen, cur), r) : p1;
          return dim_ent(
              c, base::make_object({{"kind", kind}, {"c", pt(cen)}, {"p1", pt(at)}, {"loc", pt(cur)}, {"text", text}}));
        },
        false);
    co_return;
  }
}
Task<void> dimradius_cmd(Ctx& c) {
  co_await radial(c, "radius");
}
Task<void> dimdiameter_cmd(Ctx& c) {
  co_await radial(c, "diameter");
}

struct Angular {
  Pt c, p1, p2;
};
/// dims.js angularFromLines(l1, l2, loc) (cc::cad)
std::optional<Angular> angular_from_lines(const Value& l1, const Value& l2, Pt loc) {
  const auto r = cad::angular_from_lines(l1, l2, loc.x, loc.y);
  if (!r) {
    return std::nullopt;
  }
  return Angular{{r->cx, r->cy}, {r->x1, r->y1}, {r->x2, r->y2}};
}

Task<void> dimangular_cmd(Ctx& c) {
  Ask ae;
  ae.none = true;
  const Answer pick = co_await c.entity(c.t("cmd.dimangular.select_arc_circle_line"), std::move(ae));
  std::function<std::optional<Angular>(Pt)> make;  // → { c, p1, p2 } and the loc it is for
  std::function<Pt(Pt)> loc_of = [](Pt cur) { return cur; };
  if (pick.kind != Answer::Kind::entity) {
    const Pt v = (co_await c.point(c.t("cmd.mea.vertex"))).p;
    Ask a1;
    a1.base = v;
    const Pt a = (co_await c.point(c.t("cmd.mea.end1"), std::move(a1))).p;
    Ask a2;
    a2.base = v;
    const Pt b = (co_await c.point(c.t("cmd.mea.end2"), std::move(a2))).p;
    make = [v, a, b](Pt) { return std::optional<Angular>(Angular{v, a, b}); };
  } else {
    const Value* ep = c.doc.entities().get(pick.id);
    if (ep == nullptr) {
      co_return;
    }
    const Value e = *ep;
    const std::string t = type_of(e);
    if (t == "arc") {
      const Pt cen{num(e, "cx"), num(e, "cy")};
      const double r = num(e, "r");
      const double a0 = num(e, "a0");
      const Pt p1 = polar(cen, a0, r);
      const Pt p2 = polar(cen, num(e, "a1"), r);
      const Pt mid = polar(cen, a0 + norm_angle(num(e, "a1") - a0) / 2, r);
      make = [cen, p1, p2](Pt) { return std::optional<Angular>(Angular{cen, p1, p2}); };
      loc_of = [cen, mid](Pt cur) { return dist(cen, cur) > 1e-12 ? cur : mid; };
    } else if (t == "circle") {
      const Pt cen{num(e, "cx"), num(e, "cy")};
      const Pt p1 = polar(cen, angle_of(cen, pick.p), num(e, "r"));
      Ask a2;
      a2.base = cen;
      const Pt b = (co_await c.point(c.t("cmd.mea.end2"), std::move(a2))).p;
      make = [cen, p1, b](Pt) { return std::optional<Angular>(Angular{cen, p1, b}); };
    } else if (t == "line") {
      IdSet sel;
      sel.add(pick.id);
      c.host.set_selection(std::move(sel));
      Value f;
      for (;;) {
        const Answer q = co_await c.entity(c.t("cmd.mea.line2"));
        const Value* fp = c.doc.entities().get(q.id);
        if (fp != nullptr && type_of(*fp) == "line" && !(fp->get("id") == e.get("id"))) {
          f = *fp;
          break;
        }
        c.print(c.t("cmd.dimangular.pick_second_different_line"));
      }
      c.host.set_selection({});
      const double cross = (num(e, "x2") - num(e, "x1")) * (num(f, "y2") - num(f, "y1")) -
                           (num(e, "y2") - num(e, "y1")) * (num(f, "x2") - num(f, "x1"));
      if (std::abs(cross) < 1e-12 * base::js_hypot(num(e, "x2") - num(e, "x1"), num(e, "y2") - num(e, "y1")) *
                                base::js_hypot(num(f, "x2") - num(f, "x1"), num(f, "y2") - num(f, "y1"))) {
        c.print(c.t("cmd.chamfer.err.parallel"));
        co_return;
      }
      make = [e, f](Pt cur) { return angular_from_lines(e, f, cur); };
    } else {
      c.print(c.t("cmd.dimangular.pick_arc_circle_line"));
      co_return;
    }
  }
  (void)co_await place_dim(
      c,
      [&c, make, loc_of](Pt cur, std::optional<double>, const std::string& text) -> std::optional<Value> {
        const std::optional<Angular> g = make(cur);
        if (!g) {
          return std::nullopt;
        }
        const Pt loc = loc_of(cur);
        if (!(dist(g->c, loc) > 1e-12)) {
          return std::nullopt;
        }
        return dim_ent(c, base::make_object({{"kind", "angular"},
                                             {"c", pt(g->c)},
                                             {"p1", pt(g->p1)},
                                             {"p2", pt(g->p2)},
                                             {"loc", pt(loc)},
                                             {"text", text}}));
      },
      false);
}

/// commands.js chainFrom(c): the dimension DIMCONTINUE / DIMBASELINE start from (the last one made, else a pick)
Task<Value> chain_from(Ctx& c) {
  const Value last = c.engine.state.get("lastDim");
  const Value* d = last.truthy() ? c.doc.entities().get(id_of(base::make_object({{"id", last}}))) : nullptr;
  const auto ok = [](const Value& v) {
    const std::string k = base::js_string(v.get("kind"));
    return k == "linear" || k == "aligned";
  };
  if (d != nullptr && ok(*d)) {
    co_return *d;
  }
  for (;;) {
    const Answer pick = co_await c.entity(c.t("cmd.dimangular.select_linear_aligned_dimension"));
    const Value* p = c.doc.entities().get(pick.id);
    if (p != nullptr && type_of(*p) == "dim" && ok(*p)) {
      co_return *p;
    }
    c.print(c.t("cmd.dimangular.pick_linear_aligned_dimension"));
  }
}

Task<void> chain(Ctx& c, bool baseline) {
  Value d = co_await chain_from(c);
  const Value first = d;
  int n = 0;
  for (;;) {
    const Pt from = baseline ? xy(first.get("p1")) : xy(d.get("p2"));
    const Value st = d.get("st").truthy() ? d.get("st") : c.doc.current.get("dim");
    const Value rv = d.get("rot");
    double rot = rv.truthy() && !std::isnan(base::js_to_number(rv)) ? base::js_to_number(rv) : 0;  // d.rot || 0
    if (base::js_string(d.get("kind")) == "aligned") {
      rot = angle_of(xy(d.get("p1")), xy(d.get("p2")));
    }
    const Pt nrm{-base::js_sin(rot), base::js_cos(rot)};
    const Pt fl = xy(first.get("loc"));
    const Pt fp = xy(first.get("p1"));
    const double sg = (fl.x - fp.x) * nrm.x + (fl.y - fp.y) * nrm.y;
    const double side = sg < 0 ? -1 : 1;  // Math.sign(…) || 1
    const Value dli = st.get("dli");
    const Value sc = st.get("scale");
    const double step = (dli.is_undefined() || dli.is_null() ? 3.75 : base::js_to_number(dli)) *
                        (sc.truthy() && !std::isnan(base::js_to_number(sc)) ? base::js_to_number(sc) : 1) * side;
    const Pt dl = xy(d.get("loc"));
    const Pt loc = baseline ? Pt{dl.x + nrm.x * step, dl.y + nrm.y * step} : dl;
    const auto make = [&c, from, loc, rot, st](Pt cur) {
      return dim_ent(
          c, base::make_object(
                 {{"kind", "linear"}, {"p1", pt(from)}, {"p2", pt(cur)}, {"loc", pt(loc)}, {"rot", rot}, {"st", st}}));
    };
    c.preview([make](Pt cur) { return base::Array{make(cur)}; });
    Ask ap;
    ap.none = true;
    ap.base = from;
    ap.options = {c.opt("U", "Undo")};
    const Answer p = co_await c.point(c.t("cmd.explode.specify_second_extension_line"), std::move(ap));
    if (!p.truthy()) {
      co_return;
    }
    if (p.is("U")) {
      if (n != 0) {
        undo_inside(c);
        --n;
        const Value last = c.engine.state.get("lastDim");
        const Value* ld = c.doc.entities().get(id_of(base::make_object({{"id", last}})));
        d = ld != nullptr ? *ld : first;
      } else {
        c.print(c.t("cmd.extend.nothing_undo_command"));
      }
      continue;
    }
    Value made;
    c.doc.change("DIM", [&] { made = c.doc.add(make(p.p)); });
    c.engine.state.set("lastDim", made.get("id"));
    c.print("Dimension text = " + format_measure(made));
    d = made;
    ++n;
  }
}
Task<void> dimcontinue_cmd(Ctx& c) {
  co_await chain(c, false);
}
Task<void> dimbaseline_cmd(Ctx& c) {
  co_await chain(c, true);
}

/// commands.js DIMVARS: the dimension settings typed at the command line
Task<void> dimvar(Ctx& c, const char* name_s, const char* key_s) {
  const std::string name = name_s;
  const std::string key = key_s;
  Value dim = c.doc.current.get("dim");
  const Value cur = dim.get(key);
  Ask a;
  a.def = cur;
  const Answer v = co_await c.number(c.t("cmd.dimvar.enter", {{"name", name}}), std::move(a));
  if (v.kind != Answer::Kind::number || !(v.number >= 0) || (key == "scale" && v.number == 0)) {
    c.print(c.t("cmd.dimvar.stays", {{"name", name}, {"value", base::js_string(cur)}}));
    co_return;
  }
  dim.set(key, key == "dec" ? Value(std::min(8.0, base::js_round(v.number))) : Value(v.number));
  c.doc.current.set("dim", dim);
  c.print(c.t("cmd.dimvar.set", {{"name", name}, {"value", base::js_string(dim.get(key))}}));
}

/// units.js lunitCode(v)
int lunit_code(double v) {
  const auto i = base::js_to_int32(v);
  return i >= 1 && i <= 5 && v == i ? i : 2;
}
std::string length_format_name(const Ctx& c, int code) {
  static constexpr std::array<const char*, 5> kNames{"opt.lunit.scientific", "opt.lunit.decimal",
                                                     "opt.lunit.engineering", "opt.lunit.architectural",
                                                     "opt.lunit.fractional"};
  return c.t(kNames.at(static_cast<std::size_t>(code - 1)));
}

Task<void> dimlunit_cmd(Ctx& c) {
  Value dim = c.doc.current.get("dim");
  const Value lu = dim.get("lunit");
  const double cur = lu.truthy() && !std::isnan(base::js_to_number(lu)) ? base::js_to_number(lu) : 2;
  Ask a;
  a.def = cur;
  const Answer v = co_await c.number(c.t("cmd.dimlunit.ask"), std::move(a));
  const bool known = v.kind == Answer::Kind::number &&
                     ((v.number >= 1 && v.number <= 5 && std::floor(v.number) == v.number) || v.number == 6);
  if (!known) {
    c.print(c.t("cmd.dimlunit.stays", {{"v", js_num(cur)}}));
    co_return;
  }
  const int code = lunit_code(v.number);
  dim.set("lunit", code);
  c.doc.current.set("dim", dim);
  c.print(c.t("cmd.dimlunit.set", {{"v", std::to_string(code)}, {"format", length_format_name(c, code)}}));
}

struct DimVar {
  const char* name;
  const char* key;
};
constexpr std::array<DimVar, 7> kDimVars{{{"DIMSCALE", "scale"},
                                          {"DIMTXT", "txt"},
                                          {"DIMASZ", "asz"},
                                          {"DIMDEC", "dec"},
                                          {"DIMGAP", "gap"},
                                          {"DIMEXE", "exe"},
                                          {"DIMEXO", "exo"}}};

}  // namespace

void add_dims(std::vector<CommandDef>& t) {
  t.push_back(def("DIMLINEAR", {"DLI", "DIMLIN"}, dimlinear_cmd, "cmd.dimlinear.horizontal_vertical_distance_r"));
  t.push_back(def("DIMALIGNED", {"DAL", "DIMALI"}, dimaligned_cmd, "cmd.dimaligned.true_distance_between_two"));
  t.push_back(def("DIMRADIUS", {"DRA", "DIMRAD"}, dimradius_cmd, "cmd.dimradius.radius_arc_circle"));
  t.push_back(def("DIMDIAMETER", {"DDI", "DIMDIA"}, dimdiameter_cmd, "cmd.dimdiameter.diameter_arc_circle"));
  t.push_back(def("DIMANGULAR", {"DAN", "DIMANG"}, dimangular_cmd, "cmd.dimangular.angle_between_two_lines"));
  t.push_back(def("DIMCONTINUE", {"DCO", "DIMCONT"}, dimcontinue_cmd, "cmd.dimcontinue.chain_dimensions_each_end"));
  t.push_back(def("DIMBASELINE", {"DBA", "DIMBASE"}, dimbaseline_cmd, "cmd.dimbaseline.dimensions_stacked_one_base"));
  for (const DimVar& v : kDimVars) {
    const char* name = v.name;
    const char* key = v.key;
    CommandDef d = def(name, {}, [name, key](Ctx& c) { return dimvar(c, name, key); }, "cmd.dimvar.help", true);
    d.keep_selection = true;
    t.push_back(std::move(d));
  }
  CommandDef lu = def("DIMLUNIT", {}, dimlunit_cmd, "cmd.dimlunit.help", true);
  lu.keep_selection = true;
  t.push_back(std::move(lu));
}

}  // namespace cc::cmd::detail
