// SPDX-License-Identifier: Apache-2.0
// pcommands.js (CadCraft 0.5) in C++: the parametric commands, AutoCAD's names and prompts — GEOMCONSTRAINT (GCON) and
// GCHORIZONTAL … GCFIX; DIMCONSTRAINT (DCON) and DCLINEAR … DCCONVERT; DELCONSTRAINT, AUTOCONSTRAIN, PARAMETERS,
// -PARAMETERS, CONSTRAINTBAR, DCDISPLAY. Picking: a click on an object takes the constraint point nearest the click
// where a point is asked for, and the line / circle / polyline segment under it where an object is; the first object
// stays and the second moves, as in AutoCAD.
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/gsolve.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/doc/params_doc.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

namespace gs = cad::gsolve;
namespace gc = doc::gcons;
namespace pd = doc::params_doc;

double num(const Value& v) {
  return base::js_to_number(v);
}
std::string type_of(const Value& e) {
  const Value t = e.get("type");
  return t.is_string() ? t.as_string() : std::string();
}
std::string numtext(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}
std::string round6s(double v) {
  std::string s;
  base::append_number(s, base::js_round(v * 1e6) / 1e6);
  return s;
}
Value ref(double e, const std::string& k) {
  return base::make_object({{"e", e}, {"k", k}});
}

/// commandRegistry.option(key, label): the label as given (the registry does not translate it)
Option opt(std::string key, std::string label) {
  return {.key = std::move(key), .label = std::move(label), .en = {}, .hidden = false};
}

bool model_only(Ctx& c) {
  if (c.doc.in_model()) {
    return true;
  }
  c.print(c.t("cmd.constraints_work_model_tab"));
  return false;
}
bool say(Ctx& c, const pd::Result& r, const std::optional<std::string>& done = std::nullopt) {
  if (r.ok) {
    if (done) {
      c.print(*done);
    }
    if (!r.message.empty()) {
      c.print(r.message);
    }
    return true;
  }
  c.print(r.error);
  return false;
}

/// What a test makes of a picked object: a reference key, or nothing
using Test = std::function<std::optional<std::string>(const Value& e, Pt p)>;

struct Picked {
  std::optional<std::string> option;
  Value ref;
  Id id = 0;
  Pt point;
};

std::string option_key(const std::vector<Option>& options, const std::string& label) {
  for (const Option& o : options) {
    if (o.label == label || o.key == label) {
      return o.key;
    }
  }
  return label;
}

/// pick(c, prompt, test, why, opts): ask for an object until `test` accepts it; an option comes back as { option }
Task<std::optional<Picked>> pick(Ctx& c, std::string prompt, Test test, std::string why, Ask opts = {}) {
  for (;;) {
    Ask o = opts;
    const Answer p = co_await c.entity(prompt, std::move(o));
    if (p.kind == Answer::Kind::text) {
      co_return Picked{.option = option_key(opts.options, p.text), .ref = {}, .id = 0, .point = {}};
    }
    if (p.kind == Answer::Kind::option) {
      co_return Picked{.option = p.option, .ref = {}, .id = 0, .point = {}};
    }
    if (p.kind != Answer::Kind::entity) {
      co_return std::nullopt;
    }
    const Value* e = c.doc.entities().get(p.id);
    std::optional<std::string> r;
    if (e != nullptr && gs::solvable(*e)) {
      r = test(*e, p.p);
    }
    if (r) {
      co_return Picked{.option = std::nullopt, .ref = ref(static_cast<double>(p.id), *r), .id = p.id, .point = p.p};
    }
    c.print(why);
  }
}

std::optional<std::string> as_point(const Value& e, Pt p) {
  const auto r = gc::nearest_point_ref(e, p.x, p.y);
  return r ? std::optional(r->k) : std::nullopt;
}
/// asKind(...kinds): the curve under the pick when it is one of `kinds` ('L', 'R')
std::optional<std::string> curve_of(const Value& e, Pt p, std::string_view kinds) {
  const auto k = gc::curve_ref(e, p.x, p.y);
  if (!k) {
    return std::nullopt;
  }
  const auto kind = gs::ref_kind(&e, Value(*k));
  return kind && kinds.contains(*kind) ? k : std::nullopt;
}
std::optional<std::string> as_line(const Value& e, Pt p) {
  return curve_of(e, p, "L");
}
std::optional<std::string> as_round(const Value& e, Pt p) {
  return curve_of(e, p, "R");
}
std::optional<std::string> as_curve(const Value& e, Pt p) {
  return curve_of(e, p, "LR");
}
std::optional<std::string> as_whole(const Value& e, Pt /*p*/) {
  return gs::solvable(e) ? std::optional<std::string>("*") : std::nullopt;
}
std::optional<std::string> as_line_or_arc(const Value& e, Pt p) {
  return type_of(e) == "arc" ? std::optional<std::string>("R") : as_line(e, p);
}

Task<std::optional<Picked>> first_point(Ctx& c, std::string prompt, std::vector<Option> options = {},
                                        Value def = Value()) {
  Ask a;
  a.options = std::move(options);
  a.def = std::move(def);
  co_return co_await pick(c, std::move(prompt), as_point, c.t("cmd.select_line_polyline_circle"), std::move(a));
}

void add(Ctx& c, const std::string& type, const base::Array& refs, const pd::AddOpts& extra = {}) {
  const gc::TypeInfo* T = gc::type_info(type);
  const pd::Result r = pd::add_constraint(c.doc, type, Value(refs), extra, c.words);
  (void)say(c, r,
            T != nullptr && !T->dimensional ? std::optional(c.t("cmd.gc.added", {{"what", c.t(T->label_key)}}))
                                            : std::nullopt);
}

void set_selection(Ctx& c, std::optional<Id> id) {
  IdSet s;
  if (id) {
    s.add(*id);
  }
  c.host.set_selection(std::move(s));
}

// ---------------------------------------------------------------- geometric

Task<void> horizontal_like(Ctx& c, std::string type) {
  Ask a;
  a.options = {opt("2P", "2Points")};
  a.def = "2Points";
  const auto p0 =
      co_await pick(c, c.t("cmd.select_object"), as_line, c.t("cmd.select_line_straight_polyline"), std::move(a));
  if (!p0) {
    co_return;
  }
  if (p0->option == "2P") {
    const auto p = co_await first_point(c, c.t("cmd.select_first_point"));
    if (!p || p->option) {
      co_return;
    }
    const auto q = co_await first_point(c, c.t("cmd.select_second_point"));
    if (!q || q->option) {
      co_return;
    }
    add(c, type, {p->ref, q->ref});
    co_return;
  }
  if (p0->option) {
    co_return;  // add(c, type, [a.ref]) with no ref: the JS's makeConstraint refuses it
  }
  add(c, type, {p0->ref});
}

Task<std::optional<std::pair<Picked, Picked>>> two_curves(Ctx& c, Test t1, Test t2, std::string why) {
  const auto a = co_await pick(c, c.t("cmd.fillet.first"), std::move(t1), why);
  if (!a || a->option) {
    co_return std::nullopt;
  }
  set_selection(c, a->id);
  const auto b = co_await pick(c, c.t("cmd.select_second_object"), std::move(t2), why);
  set_selection(c, std::nullopt);
  if (!b || b->option) {
    co_return std::nullopt;
  }
  co_return std::pair{*a, *b};
}

char kind_of(Ctx& c, const Picked& p) {
  return gs::ref_kind(c.doc.entities().get(p.id), p.ref.get("k")).value_or(0);
}

Task<void> autoconstrain(Ctx& c, std::optional<std::string> only, std::optional<IdSet> ids);

Task<void> gc_run(Ctx& c, const std::string& name) {
  if (name == "GCHORIZONTAL" || name == "GCVERTICAL") {
    co_await horizontal_like(c, name == "GCHORIZONTAL" ? "horizontal" : "vertical");
  } else if (name == "GCPERPENDICULAR" || name == "GCPARALLEL" || name == "GCCOLLINEAR") {
    const auto p = co_await two_curves(c, as_line, as_line, c.t("cmd.select_line_straight_polyline"));
    if (p) {
      std::string type = "collinear";
      if (name == "GCPERPENDICULAR") {
        type = "perpendicular";
      } else if (name == "GCPARALLEL") {
        type = "parallel";
      }
      add(c, type, {p->first.ref, p->second.ref});
    }
  } else if (name == "GCCONCENTRIC") {
    const auto p = co_await two_curves(c, as_round, as_round, c.t("cmd.select_circle_arc_polyline"));
    if (p) {
      add(c, "concentric", {p->first.ref, p->second.ref});
    }
  } else if (name == "GCEQUAL") {
    const auto p = co_await two_curves(c, as_curve, as_curve, c.t("cmd.select_line_circle_arc"));
    if (!p) {
      co_return;
    }
    if (kind_of(c, p->first) != kind_of(c, p->second)) {
      c.print(c.t("cmd.equal_takes_two_lines"));
      co_return;
    }
    add(c, "equal", {p->first.ref, p->second.ref});
  } else if (name == "GCTANGENT") {
    const auto p = co_await two_curves(c, as_curve, as_curve, c.t("cmd.select_line_circle_arc"));
    if (!p) {
      co_return;
    }
    const char k0 = kind_of(c, p->first);
    const char k1 = kind_of(c, p->second);
    if (k0 == 'L' && k1 == 'L') {
      c.print(c.t("cmd.tangent_needs_circle_arc"));
      co_return;
    }
    const base::Array refs =
        k0 == 'R' && k1 == 'L' ? base::Array{p->second.ref, p->first.ref} : base::Array{p->first.ref, p->second.ref};
    pd::AddOpts o;
    o.pin_entity = gs::ref_id(p->first.ref);
    add(c, "tangent", refs, o);
  } else if (name == "GCCOINCIDENT") {
    std::vector<Option> options1{opt("O", "Object"), opt("A", "Autoconstrain")};
    const auto a = co_await first_point(c, c.t("cmd.select_first_point"), std::move(options1), Value("Object"));
    if (!a) {
      co_return;
    }
    if (a->option == "A") {
      co_await autoconstrain(c, std::string("coincident"), std::nullopt);
      co_return;
    }
    if (a->option == "O") {
      const auto o = co_await pick(c, c.t("cmd.select_object"), as_curve, c.t("cmd.select_line_circle_arc"));
      if (!o || o->option) {
        co_return;
      }
      set_selection(c, o->id);
      const auto p = co_await first_point(c, c.t("cmd.select_point"));
      set_selection(c, std::nullopt);
      if (!p || p->option) {
        co_return;
      }
      pd::AddOpts ao;
      ao.pin_entity = gs::ref_id(o->ref);
      add(c, "coincident", {p->ref, o->ref}, ao);
      co_return;
    }
    if (a->option) {
      co_return;
    }
    set_selection(c, a->id);
    std::vector<Option> options2{opt("O", "Object")};
    const auto b = co_await first_point(c, c.t("cmd.select_second_point"), std::move(options2), Value("Object"));
    set_selection(c, std::nullopt);
    if (!b) {
      co_return;
    }
    pd::AddOpts ao;
    ao.pin_entity = gs::ref_id(a->ref);
    if (b->option == "O") {
      const auto o = co_await pick(c, c.t("cmd.select_object"), as_curve, c.t("cmd.select_line_circle_arc"));
      if (!o || o->option) {
        co_return;
      }
      add(c, "coincident", {a->ref, o->ref}, ao);
      co_return;
    }
    if (b->option) {
      co_return;
    }
    add(c, "coincident", {b->ref, a->ref}, ao);
  } else if (name == "GCFIX") {
    std::vector<Option> options3{opt("O", "Object")};
    const auto a = co_await first_point(c, c.t("cmd.select_point"), std::move(options3), Value("Object"));
    if (!a) {
      co_return;
    }
    if (a->option == "O") {
      const auto o = co_await pick(c, c.t("cmd.area.object"), as_whole, c.t("cmd.select_line_polyline_circle"));
      if (!o || o->option) {
        co_return;
      }
      add(c, "fix", {o->ref});
      co_return;
    }
    if (a->option) {
      co_return;
    }
    add(c, "fix", {a->ref});
  } else if (name == "GCSYMMETRIC") {
    Ask ao;
    ao.options = {opt("2P", "2Points")};
    ao.def = "2Points";
    const auto a =
        co_await pick(c, c.t("cmd.fillet.first"), as_curve, c.t("cmd.select_line_circle_arc_2"), std::move(ao));
    if (!a) {
      co_return;
    }
    base::Array refs;
    if (a->option == "2P") {
      const auto p = co_await first_point(c, c.t("cmd.select_first_point"));
      if (!p || p->option) {
        co_return;
      }
      const auto q = co_await first_point(c, c.t("cmd.select_second_point"));
      if (!q || q->option) {
        co_return;
      }
      refs = {p->ref, q->ref};
    } else {
      if (a->option) {
        co_return;
      }
      set_selection(c, a->id);
      const auto b = co_await pick(c, c.t("cmd.select_second_object"), as_curve, c.t("cmd.select_line_circle_arc_2"));
      if (!b || b->option) {
        co_return;
      }
      refs = {a->ref, b->ref};
    }
    const auto m = co_await pick(c, c.t("cmd.select_symmetry_line"), as_line, c.t("cmd.select_line_straight_polyline"));
    set_selection(c, std::nullopt);
    if (!m || m->option) {
      co_return;
    }
    refs.push_back(m->ref);
    add(c, "symmetric", refs);
  }
}

constexpr std::array<std::string_view, 11> kGcNames{"GCHORIZONTAL", "GCVERTICAL",   "GCPERPENDICULAR", "GCPARALLEL",
                                                    "GCCOLLINEAR",  "GCCONCENTRIC", "GCEQUAL",         "GCTANGENT",
                                                    "GCCOINCIDENT", "GCFIX",        "GCSYMMETRIC"};
constexpr std::array<std::string_view, 11> kGcHelp{
    "cmd.constraint_line_two_points",    "cmd.constraint_line_two_points_2", "cmd.constraint_two_lines_stay",
    "cmd.constraint_two_lines_stay_2",   "cmd.constraint_two_lines_stay_3",  "cmd.constraint_two_circles_arcs",
    "cmd.constraint_two_lines_stay_4",   "cmd.constraint_line_circle_arc",   "cmd.constraint_two_points_meet",
    "cmd.constraint_point_whole_object", "cmd.constraint_two_points_lines"};

std::vector<Option> gc_options() {
  return {opt("H", "Horizontal"),   opt("V", "Vertical"),    opt("P", "Perpendicular"),
          opt("PA", "PArallel"),    opt("T", "Tangent"),     opt("C", "Coincident"),
          opt("CON", "CONcentric"), opt("COL", "COLlinear"), opt("S", "Symmetric"),
          opt("E", "Equal"),        opt("F", "Fix")};
}
std::string gc_run_name(const std::string& key) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> kRun{{{"H", "GCHORIZONTAL"},
                                                                                       {"V", "GCVERTICAL"},
                                                                                       {"P", "GCPERPENDICULAR"},
                                                                                       {"PA", "GCPARALLEL"},
                                                                                       {"T", "GCTANGENT"},
                                                                                       {"C", "GCCOINCIDENT"},
                                                                                       {"CON", "GCCONCENTRIC"},
                                                                                       {"COL", "GCCOLLINEAR"},
                                                                                       {"S", "GCSYMMETRIC"},
                                                                                       {"E", "GCEQUAL"},
                                                                                       {"F", "GCFIX"}}};
  for (const auto& [k, v] : kRun) {
    if (k == key) {
      return std::string(v);
    }
  }
  return {};
}

Task<void> geomconstraint_cmd(Ctx& c) {
  if (!model_only(c)) {
    co_return;
  }
  Ask a;
  a.options = gc_options();
  a.def = c.engine.last_gc.empty() ? std::string("Horizontal") : c.engine.last_gc;
  const Answer k = co_await c.keyword(c.t("cmd.geomconstraint.enter_constraint_type"), std::move(a));
  std::string key;
  if (k.kind == Answer::Kind::option) {
    key = k.option;
  } else if (k.kind == Answer::Kind::text) {
    key = option_key(gc_options(), k.text);
  }
  if (key.empty()) {
    key = "H";
  }
  const std::string run = gc_run_name(key);
  if (run.empty()) {
    co_return;
  }
  for (const Option& o : gc_options()) {
    if (o.key == key) {
      c.engine.last_gc = o.label;
    }
  }
  co_await gc_run(c, run);
}

// ---------------------------------------------------------------- dimensional

std::string linear_rot(gs::RefPoint p1, gs::RefPoint p2, Pt cur) {
  const double min_x = std::min(p1.x, p2.x);
  const double max_x = std::max(p1.x, p2.x);
  const double min_y = std::min(p1.y, p2.y);
  const double max_y = std::max(p1.y, p2.y);
  double out_x = 0;
  if (cur.x < min_x) {
    out_x = min_x - cur.x;
  } else if (cur.x > max_x) {
    out_x = cur.x - max_x;
  }
  double out_y = 0;
  if (cur.y < min_y) {
    out_y = min_y - cur.y;
  } else if (cur.y > max_y) {
    out_y = cur.y - max_y;
  }
  return out_y >= out_x ? "hdist" : "vdist";
}

gs::RefPoint rp(Ctx& c, const Value& r) {
  const Value* e = c.doc.entities().get(static_cast<Id>(gs::ref_id(r)));
  const auto p = e != nullptr ? gs::ref_point(*e, r.get("k")) : std::nullopt;
  return p.value_or(gs::RefPoint{std::nan(""), std::nan("")});
}

/// two point references: picked points, or a line's (segment's) two ends
Task<std::optional<base::Array>> two_points(Ctx& c) {
  std::vector<Option> options4{opt("O", "Object")};
  const auto a = co_await first_point(c, c.t("cmd.geomconstraint.specify_first_constraint_point"), std::move(options4),
                                      Value("Object"));
  if (!a) {
    co_return std::nullopt;
  }
  if (a->option == "O") {
    const auto o = co_await pick(
        c, c.t("cmd.area.object"),
        [](const Value& e, Pt p) -> std::optional<std::string> {
          const auto k = gc::curve_ref(e, p.x, p.y);
          return k && gs::ref_kind(&e, Value(*k)) == 'L' ? k : std::nullopt;
        },
        c.t("cmd.select_line_straight_polyline"));
    if (!o || o->option) {
      co_return std::nullopt;
    }
    const Value* e = c.doc.entities().get(o->id);
    const auto id = static_cast<double>(o->id);
    if (type_of(*e) == "line") {
      co_return base::Array{ref(id, "p1"), ref(id, "p2")};
    }
    const std::string k = base::js_string(o->ref.get("k"));
    const auto i = static_cast<std::size_t>(base::js_number(std::string_view(k).substr(1)));
    const std::size_t n = e->get("pts").as_array().size();
    co_return base::Array{ref(id, "v" + std::to_string(i)), ref(id, "v" + std::to_string((i + 1) % n))};
  }
  if (a->option) {
    co_return std::nullopt;
  }
  set_selection(c, a->id);
  const auto b = co_await first_point(c, c.t("cmd.geomconstraint.specify_second_constraint_point"));
  set_selection(c, std::nullopt);
  if (!b || b->option) {
    co_return std::nullopt;
  }
  co_return base::Array{a->ref, b->ref};
}

/// What a dimension is made of: its type (DCLINEAR: hdist or vdist by where the cursor is from its two points) and refs
struct Placing {
  std::string type;
  std::optional<std::pair<gs::RefPoint, gs::RefPoint>> linear;
  base::Array refs;
  [[nodiscard]] std::string type_at(Pt cur) const {
    return linear ? linear_rot(linear->first, linear->second, cur) : type;
  }
};

/// placeAndAdd(c, typeFor, refs): where the dimension goes (with a live preview), then its value; adds it
Task<void> place_and_add(Ctx& c, Placing placing) {
  struct Made {
    std::optional<Value> c;
    std::string type, error;
    std::optional<Value> d;
  };
  const auto st = std::make_shared<const Placing>(std::move(placing));  // the preview keeps it (copies cannot throw)
  const auto& refs = st->refs;
  const auto make = [&c, st](Pt cur) {
    const std::string type = st->type_at(cur);
    gc::MakeOpts mo;
    mo.loc = std::pair{cur.x, cur.y};
    mo.id = "k0";
    auto [k, why] = gc::make_constraint(c.doc, type, Value(st->refs), mo, c.words);
    Made m;
    m.type = type;
    if (!k) {
      m.error = why;
      return m;
    }
    m.d = gc::dim_entity_for(c.doc, *k, nullptr, c.words);
    m.c = std::move(k);
    return m;
  };
  const auto cur = c.host.cursor();
  const gs::RefPoint r0 = rp(c, refs.front());
  const Made probe = make(cur ? *cur : Pt{r0.x, r0.y});
  if (!probe.c) {
    c.print(probe.error);
    co_return;
  }
  c.preview([make](Pt p) {
    const Made m = make(p);
    return m.d ? base::Array{*m.d} : base::Array{};
  });
  const Answer loc = co_await c.point(c.t("cmd.explode.specify_dimension_line_location"));
  c.preview(nullptr);
  if (loc.kind != Answer::Kind::point) {
    co_return;
  }
  const Made m = make(loc.p);
  if (!m.c) {
    c.print(m.error);
    co_return;
  }
  const std::string name0 = base::js_string(m.c->get("name"));
  const Answer answer = co_await c.string("Dimension text <" + name0 + "=" + round6s(num(m.c->get("value"))) + ">");
  const std::string typed(base::js_trim(answer.kind == Answer::Kind::text ? answer.text : std::string()));
  pd::AddOpts o;
  o.loc = std::pair{loc.p.x, loc.p.y};
  if (!typed.empty()) {
    const pd::DimInput in = pd::parse_dim_input(typed);
    if (!in.expr.empty()) {
      o.expr = in.expr;
    }
    if (in.name && *in.name != name0) {
      o.name = in.name;
    }
  }
  const pd::Result r = pd::add_constraint(c.doc, m.type, Value(refs), o, c.words);
  if (say(c, r) && r.c) {
    const Value& k = *r.c;
    c.print(c.t("cmd.dc.added", {{"name", base::js_string(k.get("name"))},
                                 {"value", round6s(num(k.get("value"))) +
                                               (base::js_string(k.get("type")) == "angle" ? "\xC2\xB0" : "")}}));
  }
}

/// convertDim(doc, d): a plain dimension → the constraint it stands for, when its points sit on constrainable geometry
std::optional<std::pair<std::string, base::Array>> convert_dim(const doc::Doc& d0, const Value& d) {
  std::vector<const Value*> ents;
  d0.model()->each([&ents](const Id&, const Value& e) {
    if (gs::solvable(e)) {
      ents.push_back(&e);
    }
  });
  double size = 1;
  for (const Value* e : ents) {
    for (const gc::PointRef& p : gc::constraint_points(*e)) {
      size = std::max({size, std::abs(p.x), std::abs(p.y)});
    }
  }
  const double tol = 1e-7 * size;
  const auto point_at = [&](const Value& p) -> std::optional<Value> {
    const double px = num(p.get("x"));
    const double py = num(p.get("y"));
    for (const Value* e : ents) {
      for (const gc::PointRef& q : gc::constraint_points(*e)) {
        if (base::js_hypot(q.x - px, q.y - py) <= tol) {
          return ref(num(e->get("id")), q.k);
        }
      }
    }
    return std::nullopt;
  };
  const std::string kind = base::js_string(d.get("kind"));
  if (kind == "linear" || kind == "aligned") {
    const auto a = point_at(d.get("p1"));
    const auto b = point_at(d.get("p2"));
    if (!a || !b) {
      return std::nullopt;
    }
    if (kind == "aligned") {
      return std::pair{std::string("dist"), base::Array{*a, *b}};
    }
    const double pi = std::numbers::pi;
    const double r0 = num(d.get("rot"));
    const double rot =
        std::fmod(std::fmod(r0 != 0 && !std::isnan(r0) ? r0 : 0, pi) + pi, pi);  // ((d.rot || 0) % π + π) % π
    if (std::abs(rot) < 1e-9 || std::abs(rot - pi) < 1e-9) {
      return std::pair{std::string("hdist"), base::Array{*a, *b}};
    }
    if (std::abs(rot - pi / 2) < 1e-9) {
      return std::pair{std::string("vdist"), base::Array{*a, *b}};
    }
    return std::pair{std::string("dist"), base::Array{*a, *b}};
  }
  const Value cen = d.get("c");
  if (kind == "radius" || kind == "diameter") {
    const Value p1 = d.get("p1");
    const double r = base::js_hypot(num(p1.get("x")) - num(cen.get("x")), num(p1.get("y")) - num(cen.get("y")));
    for (const Value* e : ents) {
      const std::string t = type_of(*e);
      if ((t == "circle" || t == "arc") &&
          base::js_hypot(num(e->get("cx")) - num(cen.get("x")), num(e->get("cy")) - num(cen.get("y"))) <= tol &&
          std::abs(num(e->get("r")) - r) <= tol) {
        return std::pair{kind, base::Array{ref(num(e->get("id")), "R")}};
      }
    }
    return std::nullopt;
  }
  if (kind == "angular") {
    const auto on_line = [tol](const Value& l, const Value& p) {
      const double ux = num(l.get("x2")) - num(l.get("x1"));
      const double uy = num(l.get("y2")) - num(l.get("y1"));
      const double len = base::js_hypot(ux, uy);
      const double L = len != 0 && !std::isnan(len) ? len : 1e-300;
      return std::abs(ux * (num(p.get("y")) - num(l.get("y1"))) - uy * (num(p.get("x")) - num(l.get("x1")))) / L <=
             tol * 10;
    };
    const auto through = [&](const Value& a, const Value& b) -> const Value* {
      for (const Value* e : ents) {
        if (type_of(*e) == "line" && on_line(*e, a) && on_line(*e, b)) {
          return e;
        }
      }
      return nullptr;
    };
    const Value* l1 = through(cen, d.get("p1"));
    const Value* l2 = through(cen, d.get("p2"));
    if (l1 != nullptr && l2 != nullptr && l1 != l2) {
      return std::pair{std::string("angle"), base::Array{ref(num(l1->get("id")), "L"), ref(num(l2->get("id")), "L")}};
    }
    for (const Value* e : ents) {
      if (type_of(*e) == "arc" &&
          base::js_hypot(num(e->get("cx")) - num(cen.get("x")), num(e->get("cy")) - num(cen.get("y"))) <= tol) {
        return std::pair{std::string("angle"), base::Array{ref(num(e->get("id")), "R")}};
      }
    }
  }
  return std::nullopt;
}

Task<void> dc_run(Ctx& c, const std::string& name) {
  const auto fixed = [](std::string_view t, base::Array refs) {
    return Placing{.type = std::string(t), .linear = std::nullopt, .refs = std::move(refs)};
  };
  if (name == "DCLINEAR") {
    const auto r = co_await two_points(c);
    if (r) {
      const gs::RefPoint a = rp(c, (*r)[0]);
      const gs::RefPoint b = rp(c, (*r)[1]);
      Placing placing = Placing{.type = {}, .linear = std::pair{a, b}, .refs = *r};
      co_await place_and_add(c, std::move(placing));
    }
  } else if (name == "DCHORIZONTAL" || name == "DCVERTICAL") {
    const auto r = co_await two_points(c);
    if (r) {
      Placing placing = fixed(name == "DCHORIZONTAL" ? "hdist" : "vdist", *r);
      co_await place_and_add(c, std::move(placing));
    }
  } else if (name == "DCALIGNED") {
    std::vector<Option> aligned{opt("O", "Object"), opt("P", c.t("cmd.geomconstraint.point_line")),
                                opt("2L", "2Lines")};
    const auto a = co_await first_point(c, c.t("cmd.geomconstraint.specify_first_constraint_point"), std::move(aligned),
                                        Value("Object"));
    if (!a) {
      co_return;
    }
    const std::string w_line = c.t("cmd.select_line_straight_polyline");
    if (a->option == "P") {
      const auto p = co_await first_point(c, c.t("cmd.geomconstraint.specify_constraint_point"));
      if (!p || p->option) {
        co_return;
      }
      const auto l = co_await pick(c, c.t("cmd.geomconstraint.select_line"), as_line, w_line);
      if (!l || l->option) {
        co_return;
      }
      Placing placing = fixed("dist", {p->ref, l->ref});
      co_await place_and_add(c, std::move(placing));
      co_return;
    }
    if (a->option == "2L") {
      const auto l1 = co_await pick(c, c.t("cmd.chamfer.first"), as_line, w_line);
      if (!l1 || l1->option) {
        co_return;
      }
      const auto l2 = co_await pick(c, c.t("cmd.geomconstraint.select_second_line_make"), as_line, w_line);
      if (!l2 || l2->option) {
        co_return;
      }
      if (!say(c, pd::add_constraint(c.doc, "parallel", Value(base::Array{l1->ref, l2->ref}), {}, c.words))) {
        co_return;
      }
      Placing placing = fixed("dist", {l1->ref, l2->ref});
      co_await place_and_add(c, std::move(placing));
      co_return;
    }
    base::Array refs;
    if (a->option == "O") {
      const auto o = co_await pick(c, c.t("cmd.area.object"), as_line, w_line);
      if (!o || o->option) {
        co_return;
      }
      const Value* e = c.doc.entities().get(o->id);
      const auto id = static_cast<double>(o->id);
      if (type_of(*e) == "line") {
        refs = {ref(id, "p1"), ref(id, "p2")};
      } else {
        const std::string k = base::js_string(o->ref.get("k"));
        const auto i = static_cast<std::size_t>(base::js_number(std::string_view(k).substr(1)));
        refs = {ref(id, "v" + k.substr(1)), ref(id, "v" + std::to_string((i + 1) % e->get("pts").as_array().size()))};
      }
    } else {
      if (a->option) {
        co_return;
      }
      const auto b = co_await first_point(c, c.t("cmd.geomconstraint.specify_second_constraint_point"));
      if (!b || b->option) {
        co_return;
      }
      refs = {a->ref, b->ref};
    }
    Placing placing = fixed("dist", refs);
    co_await place_and_add(c, std::move(placing));
  } else if (name == "DCRADIUS" || name == "DCDIAMETER") {
    const auto o = co_await pick(c, c.t("cmd.mea.arc"), as_round, c.t("cmd.select_circle_arc_polyline"));
    if (o && !o->option) {
      Placing placing = fixed(name == "DCRADIUS" ? "radius" : "diameter", {o->ref});
      co_await place_and_add(c, std::move(placing));
    }
  } else if (name == "DCANGULAR") {
    const auto a = co_await pick(c, c.t("cmd.geomconstraint.select_first_line_arc"), as_line_or_arc,
                                 c.t("cmd.geomconstraint.select_line_arc"));
    if (!a || a->option) {
      co_return;
    }
    if (type_of(*c.doc.entities().get(a->id)) == "arc") {
      Placing placing = fixed("angle", {a->ref});
      co_await place_and_add(c, std::move(placing));
      co_return;
    }
    set_selection(c, a->id);
    const auto b = co_await pick(c, c.t("cmd.mea.line2"), as_line, c.t("cmd.select_line_straight_polyline"));
    set_selection(c, std::nullopt);
    if (!b || b->option) {
      co_return;
    }
    Placing placing = fixed("angle", {a->ref, b->ref});
    co_await place_and_add(c, std::move(placing));
  } else if (name == "DCCONVERT") {
    const Answer sel = co_await c.select(c.t("cmd.geomconstraint.select_dimensions_convert"));
    double n = 0;
    double skipped = 0;
    for (const Id id : sel.set) {
      const Value* dp = c.doc.entities().get(id);
      if (dp == nullptr || type_of(*dp) != "dim") {
        ++skipped;
        continue;
      }
      const Value d = *dp;
      const auto conv = convert_dim(c.doc, d);
      if (!conv) {
        ++skipped;
        continue;
      }
      pd::AddOpts o;
      const Value lv = d.get("loc");
      o.loc = std::pair{num(lv.get("x")), num(lv.get("y"))};
      const pd::Result r = pd::add_constraint(c.doc, conv->first, Value(conv->second), o, c.words);
      if (!r.ok) {
        std::string ids;
        base::append_number(ids, static_cast<double>(id));
        c.print(c.t("cmd.dcconvert.dim_error", {{"id", ids}, {"why", r.error}}));
        ++skipped;
        continue;
      }
      c.doc.change("DCCONVERT", [&] { c.doc.remove(id); });
      ++n;
    }
    set_selection(c, std::nullopt);
    c.print(c.words.tn("cmd.dcconvert.converted", n, {}) +
            (skipped > 0 ? " " + c.words.tn("cmd.dcconvert.skipped", skipped, {}) : std::string()));
  }
}

constexpr std::array<std::string_view, 8> kDcNames{"DCLINEAR", "DCHORIZONTAL", "DCVERTICAL", "DCALIGNED",
                                                   "DCRADIUS", "DCDIAMETER",   "DCANGULAR",  "DCCONVERT"};
constexpr std::array<std::string_view, 8> kDcAlias{"DCLIN", "DCHOR", "DCVER", "DCALI", "DCRAD", "DCDIA", "DCANG", ""};
constexpr std::array<std::string_view, 8> kDcHelp{"cmd.geomconstraint.dimensional_constraint_horizontal_vertical",
                                                  "cmd.geomconstraint.dimensional_constraint_horizontal_distance",
                                                  "cmd.geomconstraint.dimensional_constraint_vertical_distance",
                                                  "cmd.geomconstraint.dimensional_constraint_true_distance",
                                                  "cmd.geomconstraint.dimensional_constraint_radius_circle",
                                                  "cmd.geomconstraint.dimensional_constraint_diameter_circle",
                                                  "cmd.geomconstraint.dimensional_constraint_angle_between",
                                                  "cmd.geomconstraint.turn_dimensions_drawn_constrainable"};

std::vector<Option> dc_options() {
  return {opt("LI", "LInear"),  opt("H", "Horizontal"), opt("V", "Vertical"), opt("AL", "ALigned"),
          opt("AN", "ANgular"), opt("R", "Radial"),     opt("D", "Diameter"), opt("C", "Convert")};
}
std::string dc_run_name(const std::string& key) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kRun{{{"LI", "DCLINEAR"},
                                                                                      {"H", "DCHORIZONTAL"},
                                                                                      {"V", "DCVERTICAL"},
                                                                                      {"AL", "DCALIGNED"},
                                                                                      {"AN", "DCANGULAR"},
                                                                                      {"R", "DCRADIUS"},
                                                                                      {"D", "DCDIAMETER"},
                                                                                      {"C", "DCCONVERT"}}};
  for (const auto& [k, v] : kRun) {
    if (k == key) {
      return std::string(v);
    }
  }
  return {};
}

Task<void> dimconstraint_cmd(Ctx& c) {
  if (!model_only(c)) {
    co_return;
  }
  Ask a;
  a.options = dc_options();
  a.def = c.engine.last_dc.empty() ? std::string("ALigned") : c.engine.last_dc;
  const Answer p = co_await c.entity(c.t("cmd.dimconstraint.select_associative_dimension_convert"), std::move(a));
  if (p.kind == Answer::Kind::text || p.kind == Answer::Kind::option) {
    const std::string key = p.kind == Answer::Kind::option ? p.option : option_key(dc_options(), p.text);
    const std::string run = dc_run_name(key);
    if (run.empty()) {
      co_return;
    }
    for (const Option& o : dc_options()) {
      if (o.key == key) {
        c.engine.last_dc = o.label;
      }
    }
    co_await dc_run(c, run);
    co_return;
  }
  if (p.kind != Answer::Kind::entity) {
    co_return;
  }
  const Value* dp = c.doc.entities().get(p.id);
  if (dp == nullptr || type_of(*dp) != "dim") {
    c.print(c.t("cmd.dimconstraint.not_dimension_choose_option"));
    co_return;
  }
  const Value d = *dp;
  const auto conv = convert_dim(c.doc, d);
  if (!conv) {
    c.print(c.t("cmd.dimconstraint.dimension_not_lines_circles"));
    co_return;
  }
  pd::AddOpts o;
  const Value lv = d.get("loc");
  o.loc = std::pair{num(lv.get("x")), num(lv.get("y"))};
  const pd::Result r = pd::add_constraint(c.doc, conv->first, Value(conv->second), o, c.words);
  if (!say(c, r)) {
    co_return;
  }
  c.doc.change("DCCONVERT", [&] { c.doc.remove(p.id); });
  c.print(c.t("cmd.dcconvert.to", {{"name", base::js_string(r.c.value_or(Value()).get("name"))}}));
}

// ---------------------------------------------------------------- managing

Task<void> delconstraint_cmd(Ctx& c) {
  if (!model_only(c)) {
    co_return;
  }
  const Answer sel = co_await c.select(c.t("cmd.sim.select"));
  if (sel.set.empty()) {
    co_return;
  }
  std::vector<std::string> gone;
  for (const Value& k : c.doc.constraints.as_array()) {
    if (std::ranges::any_of(k.get("refs").as_array(),
                            [&sel](const Value& r) { return sel.set.has(static_cast<Id>(gs::ref_id(r))); })) {
      gone.push_back(base::js_string(k.get("id")));
    }
  }
  const pd::Result r = pd::remove_constraints(c.doc, gone, "DELCONSTRAINT", c.words);
  set_selection(c, std::nullopt);
  c.print(r.removed > 0 ? c.words.tn("cmd.delconstraint.removed", r.removed, {})
                        : c.t("cmd.delconstraint.those_objects_have_no"));
}

Task<void> autoconstrain(Ctx& c, std::optional<std::string> only, std::optional<IdSet> ids) {
  std::pair<double, double>& st = c.engine.autoc;
  IdSet set;
  if (ids) {
    set = std::move(*ids);
  } else {
    for (;;) {
      Ask a;
      if (!only) {
        a.options = {opt("S", "Settings")};
      }
      const Answer s = co_await c.select(c.t("cmd.sim.select"), std::move(a));
      if (s.kind == Answer::Kind::option && s.option == "S") {
        Ask ad;
        ad.def = st.first;
        const Answer d = co_await c.distance(c.t("cmd.delconstraint.specify_distance_tolerance"), std::move(ad));
        if (d.kind == Answer::Kind::number && d.number >= 0) {
          st.first = d.number;
        }
        Ask aa;
        aa.def = st.second;
        const Answer an = co_await c.number(c.t("cmd.delconstraint.specify_angle_tolerance_degrees"), std::move(aa));
        if (an.kind == Answer::Kind::number && an.number >= 0 && an.number < 45) {
          st.second = an.number;
        }
        c.print(c.t("cmd.autoconstrain.tolerances", {{"dist", numtext(st.first)}, {"angle", numtext(st.second)}}));
        continue;
      }
      set = s.set;
      break;
    }
  }
  std::vector<Value> ents;
  for (const Id id : set) {
    const Value* e = c.doc.entities().get(id);
    if (e != nullptr && gs::solvable(*e)) {
      ents.push_back(*e);
    }
  }
  if (ents.empty()) {
    c.print(c.t("cmd.delconstraint.nothing_there_takes_constraints"));
    co_return;
  }
  const auto has = [&c](const std::string& type, const base::Array& refs) {
    return std::ranges::any_of(c.doc.constraints.as_array(), [&](const Value& k) {
      const Value kr = k.get("refs");
      if (base::js_string(k.get("type")) != type || kr.as_array().size() != refs.size()) {
        return false;
      }
      return std::ranges::all_of(refs, [&kr](const Value& r) {
        return std::ranges::any_of(
            kr.as_array(), [&r](const Value& q) { return gs::ref_id(q) == gs::ref_id(r) && q.get("k") == r.get("k"); });
      });
    });
  };
  double n = 0;
  // coincident: ends within the distance tolerance (one constraint per pair of neighbours, chained)
  if (!only || *only == "coincident") {
    struct End {
      double e = 0;
      std::string k;
      double x = 0, y = 0;
    };
    std::vector<End> ends;
    for (const Value& e : ents) {
      const std::string t = type_of(e);
      std::vector<std::string> ks;
      if (t == "line") {
        ks = {"p1", "p2"};
      } else if (t == "arc") {
        ks = {"s", "e"};
      } else if (t == "point") {
        ks = {"p"};
      } else if (t == "poly" && !e.get("closed").truthy()) {
        ks = {"v0", "v" + std::to_string(e.get("pts").as_array().size() - 1)};
      }
      for (const std::string& k : ks) {
        const auto p = gs::ref_point(e, Value(k));
        ends.push_back({num(e.get("id")), k, p ? p->x : std::nan(""), p ? p->y : std::nan("")});
      }
    }
    std::set<std::size_t> joined;
    for (std::size_t i = 0; i < ends.size(); ++i) {
      for (std::size_t j = i + 1; j < ends.size(); ++j) {
        const End& a = ends[i];
        const End& b = ends[j];
        if (a.e == b.e || joined.contains(j) || base::js_hypot(a.x - b.x, a.y - b.y) > st.first) {
          continue;
        }
        const base::Array refs{ref(b.e, b.k), ref(a.e, a.k)};
        if (has("coincident", refs)) {
          joined.insert(j);
          continue;
        }
        pd::AddOpts o;
        o.pin_first = false;
        if (pd::add_constraint(c.doc, "coincident", Value(refs), o, c.words).ok) {
          ++n;
          joined.insert(j);
        }
      }
    }
  }
  // horizontal / vertical: lines and straight polyline segments within the angle tolerance
  if (!only) {
    const double tol = st.second * std::numbers::pi / 180;
    for (const Value& e0 : ents) {
      const Value* cur = c.doc.entities().get(static_cast<Id>(num(e0.get("id"))));
      const Value e = cur != nullptr ? *cur : e0;
      std::vector<std::string> segs;
      if (type_of(e) == "line") {
        segs = {"L"};
      } else if (type_of(e) == "poly") {
        for (std::size_t i = 0; static_cast<double>(i) < gs::seg_count(e); ++i) {
          if (gs::ref_kind(&e, Value("s" + std::to_string(i))) == 'L') {
            segs.push_back("s" + std::to_string(i));
          }
        }
      }
      for (const std::string& k : segs) {
        const gs::RefLine l = gs::ref_line(e, Value(k));
        const double a = base::js_atan2(l.y2 - l.y1, l.x2 - l.x1);
        const bool h = std::abs(base::js_sin(a)) <= base::js_sin(tol);
        const bool v = std::abs(base::js_cos(a)) <= base::js_sin(tol);
        if (!h && !v) {
          continue;
        }
        const std::string type = h ? "horizontal" : "vertical";
        const base::Array one{ref(num(e.get("id")), k)};
        if (has(type, one) || has(type == "horizontal" ? "vertical" : "horizontal", one)) {
          continue;
        }
        pd::AddOpts o;
        o.pin_first = false;
        if (pd::add_constraint(c.doc, type, Value(one), o, c.words).ok) {
          ++n;
        }
      }
    }
  }
  set_selection(c, std::nullopt);
  c.print(n > 0 ? c.words.tn("cmd.autoconstrain.applied", n, {}) + " " + gc::status_text(c.doc, c.words)
                : c.t("cmd.autoconstrain.none", {{"dist", numtext(st.first)}, {"angle", numtext(st.second)}}));
}

void list_params(Ctx& c) {
  const pd::Described d = pd::describe(c.doc, c.words);
  if (d.rows.empty()) {
    c.print(c.t("cmd.parameters.no_parameters_yet_dimensional"));
  }
  for (const pd::Row& r : d.rows) {
    std::string line = "  " + r.name + " = " + r.expr;
    if (!r.driving) {
      line += "  \xE2\x86\x92 " + (r.value ? round6s(*r.value) : std::string("?"));
    }
    line += "   (" +
            (r.kind == "dim" ? c.t("cmd.parameters.kind.dim", {{"type", base::js_to_lower(r.type)}})
                             : c.t("cmd.parameters.kind.user")) +
            ")";
    if (r.error) {
      line += "  " + c.t("cmd.parameters.error", {{"why", *r.error}});
    }
    c.print(line);
  }
  c.print(d.status);
}

std::string trimmed(const Answer& a) {
  return std::string(base::js_trim(a.kind == Answer::Kind::text ? a.text : std::string()));
}

Task<void> minus_parameters_cmd(Ctx& c) {
  Ask a;
  a.options = {opt("N", "New"), opt("E", "Edit"), opt("R", "Rename"), opt("D", "Delete"), opt("?", "?")};
  a.def = "?";
  const Answer k = co_await c.keyword(c.t("cmd.mea.option"), std::move(a));
  std::string key;
  if (k.kind == Answer::Kind::option) {
    key = k.option;
  } else if (k.kind == Answer::Kind::text) {
    key = k.text;
  }
  if (key == "N") {
    const std::string name = trimmed(co_await c.string(c.t("cmd.parameters.enter_name_new_parameter")));
    if (name.empty()) {
      co_return;
    }
    std::string expr = trimmed(co_await c.string("Enter expression <0>"));
    if (expr.empty()) {
      expr = "0";
    }
    (void)say(c, pd::set_parameter(c.doc, name, expr, std::nullopt, std::nullopt, c.words), name + " = " + expr + ".");
  } else if (key == "E") {
    const std::string name = trimmed(co_await c.string(c.t("cmd.parameters.enter_name_parameter_edit")));
    if (name.empty()) {
      co_return;
    }
    std::optional<std::string> con;
    for (const Value& x : c.doc.constraints.as_array()) {
      if (gc::is_dim(x) && base::js_string(x.get("name")) == name) {
        con = base::js_string(x.get("id"));
        break;
      }
    }
    const cad::params::Params p = cad::params::from_value(c.doc.params);
    const cad::params::Var* cur = cad::params::var_by_name(p, name);
    if (cur == nullptr) {
      c.print(c.t("cmd.parameters.none_called", {{"name", name}}));
      co_return;
    }
    const std::string expr = trimmed(co_await c.string("Enter expression <" + cur->expr + ">"));
    if (expr.empty()) {
      co_return;
    }
    (void)say(c,
              con ? pd::set_dim_input(c.doc, *con, expr, c.words)
                  : pd::set_parameter(c.doc, name, expr, std::nullopt, std::nullopt, c.words),
              name + " = " + expr + ".");
  } else if (key == "R") {
    const std::string from = trimmed(co_await c.string(c.t("cmd.parameters.enter_name_parameter_rename")));
    if (from.empty()) {
      co_return;
    }
    const std::string to = trimmed(co_await c.string(c.t("cmd.parameters.enter_new_name")));
    if (to.empty()) {
      co_return;
    }
    const cad::params::Params p = cad::params::from_value(c.doc.params);
    const cad::params::Var* cur = cad::params::var_by_name(p, from);
    if (cur == nullptr) {
      c.print(c.t("cmd.parameters.none_called", {{"name", from}}));
      co_return;
    }
    (void)say(c, pd::set_parameter(c.doc, to, cur->expr, std::nullopt, from, c.words),
              c.t("params.is_now", {{"name", from}, {"value", to}}));
  } else if (key == "D") {
    const std::string name = trimmed(co_await c.string(c.t("cmd.parameters.enter_name_parameter_delete")));
    if (name.empty()) {
      co_return;
    }
    pd::Result r = pd::remove_parameter(c.doc, name, false, c.words);
    if (!r.ok && r.used) {
      Ask yn;
      yn.options = {opt("Y", "Yes"), opt("N", "No")};
      yn.def = "No";
      const Answer y = co_await c.keyword(r.error + " " + c.t("cmd.parameters.inline_ask"), std::move(yn));
      const bool yes = (y.kind == Answer::Kind::option && y.option == "Y") ||
                       (y.kind == Answer::Kind::text && (y.text == "Y" || y.text == "Yes"));
      if (!yes) {
        co_return;
      }
      r = pd::remove_parameter(c.doc, name, true, c.words);
    }
    (void)say(c, r, name + " deleted.");
  } else {
    list_params(c);
  }
}

Task<void> constraintbar_cmd(Ctx& c) {
  ConstraintView& v = c.host.constraint_view();
  Ask a;
  a.options = {opt("S", "Showall"), opt("H", "Hideall")};
  const Answer s = co_await c.select(c.t("cmd.sim.select"), std::move(a));
  if (s.kind == Answer::Kind::option) {
    v.bars = s.option == "S" ? "all" : "none";
    v.hidden.clear();
    c.print(s.option == "S" ? c.t("cmd.constraintbar.constraint_bars_shown")
                            : c.t("cmd.constraintbar.constraint_bars_hidden"));
  } else if (!s.set.empty()) {
    Ask ak;
    ak.options = {opt("S", "Show"), opt("H", "Hide")};
    ak.def = "Show";
    const Answer k = co_await c.keyword(c.t("cmd.mea.option"), std::move(ak));
    const bool hide = (k.kind == Answer::Kind::option && k.option == "H") ||
                      (k.kind == Answer::Kind::text && (k.text == "H" || k.text == "Hide"));
    const bool show = !hide;
    for (const Id id : s.set) {
      if (show) {
        v.hidden.erase(id);
      } else {
        v.hidden.insert(id);
      }
    }
    if (show && v.bars == "none") {
      v.bars = "all";
      c.doc.model()->each([&](const Id& id, const Value&) {
        if (!s.set.has(id)) {
          v.hidden.insert(id);
        }
      });
    }
    c.print(c.words.tn(show ? "cmd.constraintbar.shown_for" : "cmd.constraintbar.hidden_for",
                       static_cast<double>(s.set.size()), {}));
  }
  set_selection(c, std::nullopt);
  c.host.constraint_view_changed();
}

Task<void> dcdisplay_cmd(Ctx& c) {
  ConstraintView& v = c.host.constraint_view();
  Ask a;
  a.options = {opt("S", "Show"), opt("H", "Hide")};
  a.def = v.dims ? "Hide" : "Show";
  const Answer k = co_await c.keyword(c.t("cmd.mea.option"), std::move(a));
  v.dims = (k.kind == Answer::Kind::option && k.option == "S") || (k.kind == Answer::Kind::text && k.text == "Show");
  c.print(c.t(v.dims ? "cmd.dcdisplay.shown" : "cmd.dcdisplay.hidden"));
  c.host.constraint_view_changed();
}

Task<void> parameters_cmd(Ctx& c) {
  if (c.host.show_parameters()) {
    co_return;
  }
  list_params(c);
}

Task<void> autoconstrain_cmd(Ctx& c) {
  if (model_only(c)) {
    co_await autoconstrain(c, std::nullopt, std::nullopt);
  }
}

template <std::size_t I>
Task<void> gc_cmd(Ctx& c) {
  if (model_only(c)) {
    co_await gc_run(c, std::string(kGcNames[I]));
  }
}
template <std::size_t I>
Task<void> dc_cmd(Ctx& c) {
  if (model_only(c)) {
    co_await dc_run(c, std::string(kDcNames[I]));
  }
}
template <std::size_t... I>
void add_gc(std::vector<CommandDef>& t, std::index_sequence<I...> /*is*/) {
  (t.push_back(def(std::string(kGcNames[I]), {}, gc_cmd<I>, std::string(kGcHelp[I]))), ...);
}
template <std::size_t... I>
void add_dc(std::vector<CommandDef>& t, std::index_sequence<I...> /*is*/) {
  (t.push_back(
       def(std::string(kDcNames[I]),
           kDcAlias[I].empty() ? std::vector<std::string>{} : std::vector<std::string>{std::string(kDcAlias[I])},
           dc_cmd<I>, std::string(kDcHelp[I]))),
   ...);
}

}  // namespace

void add_parametric(std::vector<CommandDef>& t) {
  add_gc(t, std::make_index_sequence<kGcNames.size()>{});
  t.push_back(def("GEOMCONSTRAINT", {"GCON"}, geomconstraint_cmd,
                  "cmd.geomconstraint.geometric_constraints_horizontal_vertical"));
  add_dc(t, std::make_index_sequence<kDcNames.size()>{});
  t.push_back(
      def("DIMCONSTRAINT", {"DCON"}, dimconstraint_cmd, "cmd.dimconstraint.dimensional_constraints_linear_aligned"));
  t.push_back(
      def("DELCONSTRAINT", {"DELCON"}, delconstraint_cmd, "cmd.delconstraint.remove_every_constraint_geometric"));
  t.push_back(def("AUTOCONSTRAIN", {}, autoconstrain_cmd, "cmd.autoconstrain.add_coincident_horizontal_vertical"));
  CommandDef par = def("PARAMETERS", {"PAR", "PARAMETERSMANAGER", "PARAM"}, parameters_cmd,
                       "cmd.parameters.parameters_panel_user_parameters", true);
  par.keep_selection = true;
  t.push_back(std::move(par));
  t.push_back(def("-PARAMETERS", {}, minus_parameters_cmd, "cmd.parameters.parameters_command_line_new"));
  t.push_back(def("CONSTRAINTBAR", {"CBAR"}, constraintbar_cmd, "cmd.constraintbar.show_hide_constraint_bars"));
  CommandDef dcd = def("DCDISPLAY", {}, dcdisplay_cmd, "cmd.dcdisplay.show_hide_dimensional_constraints", true);
  dcd.keep_selection = true;
  t.push_back(std::move(dcd));
}

}  // namespace cc::cmd::detail
