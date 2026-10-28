// SPDX-License-Identifier: Apache-2.0
// params-part.js (PartCraft 0.5): the parametric engine's adapter for a part.
#include "cc/partcraft/params_part.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"
#include "cc/partcraft/sketch.hpp"
#include "jsx.hpp"

namespace cc::partcraft::pp {
namespace {

using base::Value;
namespace pr = cad::params;

constexpr double POS = 1e-6;
constexpr double MAXL = 1e6;

using When = bool (*)(const Value&);
struct Field {
  std::string_view type, field, label_key, label_text, kind;  // label_text: a fixed label (movebody's ΔX)
  // NOLINTNEXTLINE(readability-redundant-member-init): the table leaves them out (-Wmissing-field-initializers)
  std::optional<double> min = {}, max = {};
  bool integer = false;
  When when = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): scalebody sx/sy/sz fill model.bodies.field.scale_axis {axis}
  std::string_view fill_axis = {};
};

bool is(const Value& f, std::string_view k, std::string_view v) {
  return f.get(k) == Value(v);
}

// PARAM_FIELDS in the order 0.5 builds it: the static table, DESIGN_FIELDS, then the modules part.js loads (bodies.js,
// sheetmetal.js, rib.js, threads.js)
const std::vector<Field>& fields() {
  static const std::vector<Field> f = {
      {"extrude", "depth", "model.params_part.field.depth", "", "length", POS, MAXL, false,
       [](const Value& g) { return is(g, "end", "blind") || is(g, "end", "mid") || g.get("end").is_undefined(); }},
      {"revolve", "angle", "model.params_part.field.angle", "", "angle", 0.001, 360},
      {"hole", "d", "model.params_part.field.diameter", "", "length", POS, MAXL},
      {"hole", "depth", "model.params_part.field.depth", "", "length", POS, MAXL, false,
       [](const Value& g) { return !g.get("through").truthy(); }},
      {"hole", "cboreD", "model.params_part.field.counterbore_diameter", "", "length", POS, MAXL, false,
       [](const Value& g) { return is(g, "kind", "cbore"); }},
      {"hole", "cboreDepth", "model.params_part.field.counterbore_depth", "", "length", POS, MAXL, false,
       [](const Value& g) { return is(g, "kind", "cbore"); }},
      {"lpattern", "count", "model.params_part.field.count", "", "count", 1, 1000, true},
      {"lpattern", "spacing", "model.params_part.field.spacing", "", "length", -MAXL, MAXL},
      {"cpattern", "count", "model.params_part.field.count", "", "count", 1, 1000, true},
      {"cpattern", "angle", "model.params_part.field.angle", "", "angle", 0.001, 360},
      {"fillet", "r", "model.params_part.field.radius", "", "length", POS, 1e5},
      {"chamfer", "d", "model.params_part.field.distance", "", "length", POS, 1e5},
      {"shell", "t", "model.params_part.field.wall_thickness", "", "length", POS, 1e5},
      // DESIGN_FIELDS
      {"extrude", "depth2", "model.params_part.field.second_depth", "", "length", POS, MAXL, false,
       [](const Value& g) { return g.get("two").truthy(); }},
      {"extrude", "taper", "model.params_part.field.taper_angle", "", "angle", -89, 89},
      {"extrude", "offset", "model.params_part.field.offset", "", "length", -MAXL, MAXL, false,
       [](const Value& g) { return is(g, "end", "offset"); }},
      {"hole", "csinkD", "model.params_part.field.countersink_diameter", "", "length", POS, MAXL, false,
       [](const Value& g) { return is(g, "kind", "csink"); }},
      {"hole", "threadDepth", "model.params_part.field.thread_depth", "", "length", POS, MAXL, false,
       [](const Value& g) { return is(g, "kind", "tapped"); }},
      {"chamfer", "d2", "model.params_part.field.second_distance", "", "length", POS, 1e5, false,
       [](const Value& g) { return is(g, "mode", "two"); }},
      {"chamfer", "angle", "model.params_part.field.angle", "", "angle", 0.001, 89.999, false,
       [](const Value& g) { return is(g, "mode", "angle"); }},
      {"fillet", "r2", "model.params_part.field.end_radius", "", "length", POS, 1e5},
      {"plane", "d", "model.params_part.field.distance", "", "length", -1e9, 1e9, false,
       [](const Value& g) { return is(g, "mode", "offset"); }},
      {"plane", "angle", "model.params_part.field.angle", "", "angle", -360, 360, false,
       [](const Value& g) { return is(g, "mode", "angle"); }},
      {"draft", "angle", "model.params_part.field.angle", "", "angle", -89, 89},
      {"sweep", "scale", "model.params_part.field.end_scale", "", "number", 1e-3, 1e3},
      // bodies.js
      {"movebody", "dx", "", "ΔX", "length", -1e6, 1e6, false,
       [](const Value& g) { return is(g, "mode", "translate"); }},
      {"movebody", "dy", "", "ΔY", "length", -1e6, 1e6, false,
       [](const Value& g) { return is(g, "mode", "translate"); }},
      {"movebody", "dz", "", "ΔZ", "length", -1e6, 1e6, false,
       [](const Value& g) { return is(g, "mode", "translate"); }},
      {"movebody", "angle", "model.params_part.field.angle", "", "angle", -3600, 3600, false,
       [](const Value& g) { return is(g, "mode", "rotate"); }},
      {"scalebody", "s", "model.bodies.field.scale", "", "number", 1e-6, 1e6, false,
       [](const Value& g) { return g.get("uniform").truthy(); }},
      {"scalebody", "sx", "model.bodies.field.scale_axis", "", "number", 1e-6, 1e6, false,
       [](const Value& g) { return !g.get("uniform").truthy(); }, "X"},
      {"scalebody", "sy", "model.bodies.field.scale_axis", "", "number", 1e-6, 1e6, false,
       [](const Value& g) { return !g.get("uniform").truthy(); }, "Y"},
      {"scalebody", "sz", "model.bodies.field.scale_axis", "", "number", 1e-6, 1e6, false,
       [](const Value& g) { return !g.get("uniform").truthy(); }, "Z"},
      {"pathpattern", "count", "model.params_part.field.count", "", "count", 1, 1000, true},
      {"pathpattern", "d", "model.params_part.field.spacing", "", "length", 1e-6, 1e6, false,
       [](const Value& g) { return is(g, "spacing", "distance"); }},
      // sheetmetal.js
      {"sheetbase", "t", "sheet.param.t", "", "length", 0.01, 100},
      {"sheetbase", "r", "sheet.param.r", "", "length", 0, 1000},
      {"sheetbase", "k", "sheet.param.k", "", "number", 0, 0.5},
      {"sheetbase", "depth", "sheet.param.depth", "", "length", 0.01, 1e5},
      {"sheetflange", "angle", "sheet.param.angle", "", "angle", 1, 180},
      {"sheetflange", "length", "sheet.param.length", "", "length", 0.01, 1e5},
      {"sheetflange", "reliefWidth", "sheet.param.relief_width", "", "length", 0.01, 100, false,
       [](const Value& g) { return is(g, "relief", "rect"); }},
      // rib.js, threads.js
      {"rib", "t", "rib.param.t", "", "length", 1e-4, 1e5},
      {"thread", "length", "thread.param.length", "", "length", 1e-3, 1e5},
      {"thread", "clearance", "thread.param.clearance", "", "length", 0, 5},
  };
  return f;
}

std::string label_of(const Field& f, const i18n::Catalog& w) {
  std::string s;
  if (!f.label_text.empty()) {
    s = f.label_text;
  } else if (!f.fill_axis.empty()) {
    s = w.t(f.label_key, {{"axis", std::string(f.fill_axis)}});
  } else {
    s = w.t(f.label_key);
  }
  return jsx::utf16_slice(s, 60);
}

std::vector<const Field*> fields_of(std::string_view type) {
  std::vector<const Field*> out;
  for (const Field& f : fields()) {
    if (f.type == type) {
      out.push_back(&f);
    }
  }
  return out;
}

bool dim_type(const Value& t) {
  constexpr std::array<std::string_view, 7> kDims{"distance", "hdistance", "vdistance", "radius",
                                                  "diameter", "angle",     "offset"};
  return std::ranges::any_of(kDims, [&](std::string_view d) { return t == Value(d); });
}
std::string dim_label(const Value& type, const i18n::Catalog& w) {
  const std::string t = base::js_string(type);
  for (const std::string_view d : {"coincident", "horizontal", "vertical", "parallel", "perpendicular", "tangent",
                                   "equal", "concentric", "midpoint", "fix", "symmetric", "offsetlink", "distance",
                                   "hdistance", "vdistance", "radius", "diameter", "angle", "offset"}) {
    if (t == d) {
      return base::js_to_lower(w.t("model.sketch.rel." + std::string(d)));
    }
  }
  return base::js_to_lower(t);
}

Value* features(Value& part) {
  Value* f = part.find("features");
  return f != nullptr && f->is_array() ? f : nullptr;
}

}  // namespace

std::vector<Target> targets(Value& part, const i18n::Catalog& w) {
  std::vector<Target> out;
  Value* fs = features(part);
  if (fs == nullptr) {
    return out;
  }
  for (Value& f : fs->as_array()) {
    const std::string type = f.get("type").is_string() ? f.get("type").as_string() : std::string();
    const std::string fid = base::js_string(f.get("id"));
    const std::string fname = base::js_string(f.get("name"));
    for (const Field* s : fields_of(type)) {
      const Value v = f.get(s->field);
      if (!v.is_number() || !std::isfinite(v.as_number())) {
        continue;
      }
      Target t;
      t.key = fid + "." + std::string(s->field);
      t.label = fname + " " + label_of(*s, w);
      t.kind = std::string(s->kind);
      t.min = s->min;
      t.max = s->max;
      t.integer = s->integer || s->kind == "count";
      t.feature = fid;
      t.field = std::string(s->field);
      t.slot = &f;
      out.push_back(std::move(t));
    }
    if (type == "sketch" && f.get("sketch").truthy()) {
      Value* sk = f.find("sketch");
      Value* cons = sk->find("constraints");
      if (cons == nullptr || !cons->is_array()) {
        continue;
      }
      std::size_t i = 0;
      for (Value& c : cons->as_array()) {
        if (!dim_type(c.get("type"))) {
          continue;
        }
        ++i;
        const bool ang = c.get("type") == Value("angle");
        const bool round = c.get("type") == Value("radius") || c.get("type") == Value("diameter");
        Target t;
        t.key = fid + "." + base::js_string(c.get("id"));
        t.label = fname + " D" + std::to_string(i) + " (" + dim_label(c.get("type"), w) + ")";
        t.kind = ang ? "angle" : "length";
        t.min = round ? POS : 0;
        t.max = ang ? 180 : MAXL;
        t.integer = false;
        t.feature = fid;
        t.constraint = base::js_string(c.get("id"));
        t.dim = "D" + std::to_string(i);
        t.slot = &c;
        t.field = "value";
        out.push_back(std::move(t));
      }
    }
  }
  return out;
}

Value target_value(const Target& t) {
  Value o = base::make_object({{"label", t.label}, {"kind", t.kind}});
  if (t.min) {
    o.append("min", *t.min);
  }
  if (t.max) {
    o.append("max", *t.max);
  }
  o.append("integer", t.integer);
  o.append("feature", t.feature);
  if (!t.constraint.empty()) {
    o.append("constraint", t.constraint);
    o.append("dim", t.dim);
  } else {
    o.append("field", t.field);
  }
  o.append("value", t.slot->get(t.field));
  return o;
}

bool field_in_use(const Value& f, const Value& field) {
  if (!field.is_string() || !f.get("type").is_string()) {
    return false;
  }
  for (const Field* s : fields_of(f.get("type").as_string())) {
    if (s->field == field.as_string()) {
      return s->when == nullptr || s->when(f);
    }
  }
  return false;
}

std::pair<std::string, std::string> split_key(std::string_view key) {
  const auto i = key.find('.');
  if (i == std::string_view::npos) {
    return {std::string(key), ""};
  }
  return {std::string(key.substr(0, i)), std::string(key.substr(i + 1))};
}

PartAdapter::PartAdapter(Value& part, const i18n::Catalog& words) : part_(part), words_(words) {
  adapter_.targets = [this] {
    targets_ = targets(part_, words_);
    std::vector<std::pair<std::string, pr::Target>> out;
    for (const Target& t : targets_) {
      pr::Target x;
      x.label = t.label;
      x.kind = t.kind;
      x.min = t.min;
      x.max = t.max;
      x.integer = t.integer;
      Value* slot = t.slot;
      const std::string field = t.field;
      const std::string key = t.key;
      x.get = [slot, field] {  // NOLINT(bugprone-exception-escape): reads a member of the part
        return base::js_to_number(slot->get(field));
      };
      x.set = [this, slot, field, key](double v) {  // NOLINT(bugprone-exception-escape): writes a member of the part
        if (std::ranges::none_of(journal_, [&](const auto& j) { return j.first == key; })) {
          journal_.emplace_back(key, base::js_to_number(slot->get(field)));
        }
        slot->set(field, v);
      };
      out.emplace_back(t.key, std::move(x));
    }
    return out;
  };
  adapter_.after_apply = [this](const std::vector<std::string>& changed) { return after_apply(changed); };
}

std::vector<std::pair<std::string, std::string>> PartAdapter::after_apply(const std::vector<std::string>& changed) {
  Value& part = part_;
  const i18n::Catalog& w = words_;
  std::vector<std::pair<std::string, std::string>> errors;
  const auto j = std::move(journal_);
  journal_.clear();
  std::vector<std::pair<Value*, std::vector<std::string>>> by_sketch;
  Value* fs = features(part);
  for (const std::string& key : changed) {
    const auto [fid, sub] = split_key(key);
    Value* f = nullptr;
    for (Value& g : fs->as_array()) {
      if (g.get("id") == Value(fid)) {
        f = &g;
        break;
      }
    }
    if (f == nullptr || !(f->get("type") == Value("sketch"))) {
      continue;
    }
    auto it = std::ranges::find_if(by_sketch, [f](const auto& p) { return p.first == f; });
    if (it == by_sketch.end()) {
      by_sketch.emplace_back(f, std::vector<std::string>{});
      it = by_sketch.end() - 1;
    }
    it->second.push_back(sub);
  }
  for (auto& [f, cids] : by_sketch) {
    Value* sk = f->find("sketch");
    auto r = sketch::solve_sketch(*sk, {}, {}, w);
    if (r && r->get("ok").truthy()) {
      continue;
    }
    std::vector<std::string> names;
    const std::string fname = base::js_string(f->get("name"));
    for (const std::string& cid : cids) {
      Value* c = nullptr;
      std::size_t di = 0;
      std::size_t n = 0;
      for (Value& k : sk->find("constraints")->as_array()) {
        if (dim_type(k.get("type"))) {
          ++n;
        }
        if (k.get("id") == Value(cid)) {
          c = &k;
          di = n;
          break;
        }
      }
      const std::string key = base::js_string(f->get("id")) + "." + cid;
      const auto old = std::ranges::find_if(j, [&](const auto& e) { return e.first == key; });
      if (c != nullptr && old != j.end()) {
        const double v = base::js_to_number(c->get("value"));
        names.push_back(fname + " D" + std::to_string(di) + " = " +
                        base::js_string(Value(base::js_number(base::js_to_exponential(v, 11)))));
        c->set("value", old->second);
      }
    }
    const auto again = sketch::solve_sketch(*sk, {}, {}, w);  // the old dimensions back: solved as they were
    (void)again;
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
      joined += (i > 0 ? ", " : "") + names[i];
    }
    errors.emplace_back(fname, w.tn("model.params_part.cannot_solve", static_cast<double>(names.size()),
                                    {{"name", fname}, {"values", joined}}));
  }
  return errors;
}

// resolveTarget(part, text): the key itself, "<feature name or id>[ .]<field or its label or D-number>"
std::optional<std::string> resolve_target(Value& part, const Value& text, const i18n::Catalog& w) {
  std::string s(base::js_trim(jsx::text_of(text)));
  const std::vector<Target> all = targets(part, w);
  for (const Target& t : all) {
    if (t.key == s) {
      return s;
    }
  }
  // /^(.+?)[ .]([A-Za-z][A-Za-z0-9 ]*)$/: the shortest name before a space or dot that leaves a field after it
  std::optional<std::pair<std::string, std::string>> m;
  for (std::size_t i = 1; i + 1 < s.size() && !m; ++i) {  // . in the name part matches any char except line ends
    if (s[i] != ' ' && s[i] != '.') {
      continue;
    }
    const std::string name = s.substr(0, i);
    if (name.find_first_of("\n\r") != std::string::npos) {
      break;
    }
    const std::string field = s.substr(i + 1);
    const auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    const auto alnum_sp = [&](char c) { return alpha(c) || (c >= '0' && c <= '9') || c == ' '; };
    if (!field.empty() && alpha(field[0]) && std::ranges::all_of(field, alnum_sp)) {
      m = std::pair{name, field};
    }
  }
  if (!m) {
    return std::nullopt;
  }
  const auto& [fname, field] = *m;
  Value* fs = part.find("features");
  std::vector<const Value*> hits;
  if (fs != nullptr && fs->is_array()) {
    for (const Value& f : fs->as_array()) {
      if (f.get("name") == Value(fname) || f.get("id") == Value(fname)) {
        hits.push_back(&f);
      }
    }
  }
  if (hits.size() != 1) {
    return std::nullopt;
  }
  const Value& f = *hits[0];
  const std::string fid = base::js_string(f.get("id"));
  const std::string low = base::js_to_lower(field);
  for (const Target& t : all) {
    if (t.feature != fid) {
      continue;
    }
    if (t.constraint.empty()) {
      if (t.field == field) {
        return t.key;
      }
      for (const Field* s2 : fields_of(base::js_string(f.get("type")))) {
        if (s2->field == t.field && base::js_to_lower(label_of(*s2, w)) == low) {
          return t.key;
        }
      }
    }
    if (!t.dim.empty() && base::js_to_lower(t.dim) == low) {
      return t.key;
    }
  }
  return std::nullopt;
}

std::string target_name(Value& part, std::string_view key, const i18n::Catalog& w) {
  for (const Target& t : targets(part, w)) {
    if (t.key == key) {
      for (const Value& f : features(part)->as_array()) {
        if (f.get("id") == Value(t.feature)) {
          return base::js_string(f.get("name")) + "." + (t.constraint.empty() ? t.field : t.dim);
        }
      }
    }
  }
  return std::string(key);
}

base::Result<Value> read_part_params(const Value& raw, Value& part, const i18n::Catalog& w) {
  std::set<std::string> keys;
  for (const Target& t : targets(part, w)) {
    keys.insert(t.key);
  }
  auto [p, error] = pr::read_params(raw, &keys, w);
  if (!p) {
    return base::fail(base::Errc::invalid, error);
  }
  return pr::has_params(*p) ? pr::to_value(*p) : Value(nullptr);
}

namespace {

Value errors_value(const std::vector<std::pair<std::string, std::string>>& errors) {
  base::Array a;
  for (const auto& [where, message] : errors) {
    a.push_back(base::make_object({{"where", where}, {"message", message}}));
  }
  return a;
}

}  // namespace

Value apply_part_params(Value& part, const std::vector<std::pair<std::string, double>>* overrides,
                        const i18n::Catalog& w) {
  const Value pv = part.get("params");
  const pr::Params p = pr::from_value(pv);
  if (!pv.truthy() || !pr::has_params(p)) {
    return base::make_object(
        {{"ok", true}, {"values", Value::object()}, {"errors", base::Array{}}, {"changed", base::Array{}}});
  }
  PartAdapter a(part, w);
  const pr::Applied r = pr::apply_params(p, a.adapter(), w, overrides);
  Value values = Value::object();
  for (const auto& [k, v] : r.values) {
    values.set(k, v);
  }
  base::Array changed(r.changed.begin(), r.changed.end());
  return base::make_object({{"ok", r.ok},
                            {"values", std::move(values)},
                            {"errors", errors_value(r.errors)},
                            {"changed", std::move(changed)}});
}

Value prune_deleted(Value& before, Value& after, const i18n::Catalog& w) {
  Value* pv = after.find("params");
  if (pv == nullptr || !pv->truthy()) {
    return base::Array{};
  }
  std::set<std::string> keep;
  for (const Target& t : targets(after, w)) {
    keep.insert(t.key);
  }
  const std::vector<Target> labels = targets(before, w);
  base::Array gone;
  base::Array links;
  for (const Value& l : pv->get("links").as_array()) {
    const std::string target = base::js_string(l.get("target"));
    if (keep.contains(target)) {
      links.push_back(l);
    } else {
      const auto it = std::ranges::find_if(labels, [&](const Target& t) { return t.key == target; });
      gone.emplace_back(it != labels.end() ? it->label : target);
    }
  }
  pv->set("links", std::move(links));
  if (!pr::has_params(pr::from_value(*pv))) {
    after.erase("params");
  }
  return gone;
}

Value params_report(Value& part, const i18n::Catalog& w) {
  const Value pv = part.get("params");
  const pr::Params p = pv.truthy() ? pr::from_value(pv) : pr::Params{};
  const pr::Evaluated ev = pr::evaluate_vars(p, nullptr, w);
  base::Array vars;
  for (const pr::Var& v : p.vars) {
    const auto val = ev.value(v.name);
    const auto err = std::ranges::find_if(ev.errors, [&](const auto& e) { return e.first == v.name; });
    vars.push_back(base::make_object({{"name", v.name},
                                      {"expr", v.expr},
                                      {"note", v.note && !v.note->empty() ? Value(*v.note) : Value("")},
                                      {"value", val ? Value(*val) : Value(nullptr)},
                                      {"error", err != ev.errors.end() ? Value(err->second) : Value(nullptr)}}));
  }
  PartAdapter a(part, w);
  base::Array links;
  for (const pr::LinkInfo& l : pr::describe_links(p, a.adapter(), w)) {
    Value o = base::make_object({{"target", l.target}, {"expr", l.expr}, {"label", l.label}});
    if (l.kind) {
      o.append("kind", *l.kind);
    }
    o.append("value", l.value ? Value(*l.value) : Value(nullptr));
    o.append("result", l.result ? Value(*l.result) : Value(nullptr));
    o.append("error", l.error ? Value(*l.error) : Value(nullptr));
    o.append("name", target_name(part, l.target, w));
    links.push_back(std::move(o));
  }
  return base::make_object({{"vars", std::move(vars)}, {"links", std::move(links)}});
}

namespace {

double built(const Value& part) {
  const double n = static_cast<double>(part.get("features").as_array().size());
  const Value rb = part.get("rollback");
  if (rb.is_null() || rb.is_undefined()) {
    return n;
  }
  const double r = base::js_to_number(rb);
  return std::isnan(r) ? r : std::min(r, n);
}

}  // namespace

std::string rollback_words(const Value& part, const i18n::Catalog& w) {
  const double b = built(part);
  const auto& fs = part.get("features").as_array();
  const auto n = static_cast<double>(fs.size());
  const auto num = [](double v) { return base::js_string(Value(v)); };
  if (b >= n) {
    return w.tn("model.params_part.rollback.all", n, {{"n", num(n)}});
  }
  if (b == 0) {
    return w.t("model.params_part.rollback.nothing", {{"name", base::js_string(fs.at(0).get("name"))}});
  }
  return w.tn(
      "model.params_part.rollback.after", n,
      {{"name", base::js_string(fs.at(static_cast<std::size_t>(b) - 1).get("name"))}, {"b", num(b)}, {"n", num(n)}});
}

Value step_rollback(Value& part, double delta, const i18n::Catalog& w) {
  const double from = built(part);
  const double n = static_cast<double>(part.get("features").as_array().size());
  const double to = std::max(0.0, std::min(n, from + delta));
  if (to == from) {
    std::string m;
    if (delta < 0) {
      m = n != 0 ? w.t("model.params_part.rollback.at_top") : w.t("model.params_part.rollback.none");
    } else {
      m = w.t("model.params_part.rollback.at_end");
    }
    return base::make_object({{"ok", false}, {"from", from}, {"to", to}, {"message", m}});
  }
  part.set("rollback", to >= n ? Value(nullptr) : Value(to));
  return base::make_object({{"ok", true}, {"from", from}, {"to", to}, {"message", rollback_words(part, w)}});
}

Value roll_to(Value& part, const Value& id, const i18n::Catalog& w) {
  if (id.is_null()) {
    const double from = built(part);
    part.set("rollback", nullptr);
    return base::make_object({{"ok", true},
                              {"from", from},
                              {"to", static_cast<double>(part.get("features").as_array().size())},
                              {"message", rollback_words(part, w)}});
  }
  const auto& fs = part.get("features").as_array();
  std::optional<std::size_t> i;
  for (std::size_t k = 0; k < fs.size(); ++k) {
    if (fs[k].get("id") == id) {
      i = k;
      break;
    }
  }
  if (!i) {
    return base::make_object({{"ok", false}, {"message", w.t("model.params_part.no_such_feature")}});
  }
  const double from = built(part);
  part.set("rollback", static_cast<double>(*i));
  return base::make_object(
      {{"ok", true}, {"from", from}, {"to", static_cast<double>(*i)}, {"message", rollback_words(part, w)}});
}

}  // namespace cc::partcraft::pp
