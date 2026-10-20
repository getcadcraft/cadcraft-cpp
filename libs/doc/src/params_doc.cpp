// SPDX-License-Identifier: Apache-2.0
// params-doc.js (CadCraft 0.5) in C++.
#include "cc/doc/params_doc.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cad/gsolve.hpp"
#include "cc/doc/gcons.hpp"

namespace cc::doc::params_doc {

using base::Value;
namespace gs = cad::gsolve;
namespace pr = cad::params;

namespace {

/// a[i] of an array value (anything else: undefined)
Value at(const Value& a, std::size_t i) {
  return a.is_array() && i < a.as_array().size() ? a.as_array()[i] : Value();
}

double num(const Value& v) {
  return base::js_to_number(v);
}
double round6(double v) {
  return base::js_round(v * 1e6) / 1e6;
}
std::string type_of(const Value& e) {
  const Value t = e.get("type");
  return t.is_string() ? t.as_string() : std::string();
}
const Value* entity(const Doc& doc, double id) {
  if (!std::isfinite(id) || id < 0 || id > 9e15 || id != std::floor(id)) {
    return nullptr;
  }
  return doc.model()->get(static_cast<Id>(id));
}
std::string cid_of(const Value& c) {
  return base::js_string(c.get("id"));
}
std::string key_of(double id, std::string_view f) {
  std::string s;
  base::append_number(s, id);
  s += '|';
  s += f;
  return s;
}
Result fail(std::string error) {
  Result r;
  r.error = std::move(error);
  return r;
}

/// docAdapter over a draft's constraints (mutable: set writes the value in)
pr::Adapter adapter(const Doc& doc, base::Array& draft, std::vector<std::string>* changed, const i18n::Catalog& words) {
  pr::Adapter a;
  a.targets = [&doc, &draft, &words]() {
    std::vector<std::pair<std::string, pr::Target>> m;
    for (Value& c : draft) {
      if (!gcons::is_dim(c)) {
        continue;
      }
      const std::string type = base::js_string(c.get("type"));
      const gcons::TypeInfo* T = gcons::type_info(type);
      const Value refs = c.get("refs");
      const Value* e0 = refs.as_array().empty() ? nullptr : entity(doc, gs::ref_id(refs.as_array().front()));
      const bool arc = type == "angle" && e0 != nullptr && type_of(*e0) == "arc" && refs.as_array().size() == 1;
      Value* cp = &c;
      pr::Target t;
      t.label = base::js_string(c.get("name")) + " (" + base::js_to_lower(words.t(T->label_key)) + ")";
      t.kind = std::string(T->kind);
      t.get = [cp] { return num(cp->get("value")); };
      t.set = [cp](double v) { cp->set("value", v); };
      t.min = type == "radius" || type == "diameter" || type == "angle" ? 1e-9 : 0;
      if (type == "angle") {
        t.max = arc ? 360 - 1e-9 : 180;
      }
      m.emplace_back(gcons::target_key(c), std::move(t));
    }
    return m;
  };
  a.after_apply = [changed](const std::vector<std::string>& ch) {
    *changed = ch;
    return std::vector<std::pair<std::string, std::string>>{};
  };
  return a;
}

/// softPins(doc, cs): fields held while a dimension changes — the first point it measures from
std::set<std::string> soft_pins(const Doc& doc, const base::Array& cs) {
  std::set<std::string> pins;
  for (const Value& c : cs) {
    const Value r = at(c.get("refs"), 0);
    const double id = gs::ref_id(r);
    const Value* e = entity(doc, id);
    if (e == nullptr) {
      continue;
    }
    const auto add = [&](std::initializer_list<std::string> fs) {
      for (const std::string& f : fs) {
        pins.insert(key_of(id, f));
      }
    };
    const std::string k = base::js_string(r.get("k"));
    const std::string t = type_of(*e);
    const auto kind = gs::ref_kind(e, r.get("k"));
    if (kind == 'P') {
      if (t == "line") {
        if (k == "p1") {
          add({"x1", "y1"});
        } else if (k == "p2") {
          add({"x2", "y2"});
        }
      } else if (t == "poly") {
        std::string i;
        base::append_number(i, base::js_number(std::string_view(k).substr(1)));
        add({i + "x", i + "y"});
      } else if (t == "point") {
        add({"x", "y"});
      } else if ((t == "circle" || t == "arc") && k == "c") {
        add({"cx", "cy"});
      }
    } else if (kind == 'R') {
      if (t != "poly") {
        add({"cx", "cy"});
      }
    } else if (kind == 'L') {
      if (t == "line") {
        add({"x1", "y1"});
      }
    }
  }
  return pins;
}

base::Array copy_constraints(const Doc& doc) {
  return doc.constraints.is_array() ? doc.constraints.as_array() : base::Array{};
}

Value* find_c(base::Array& cs, std::string_view cid) {
  const auto it = std::ranges::find_if(cs, [cid](const Value& c) { return cid_of(c) == cid; });
  return it == cs.end() ? nullptr : &*it;
}

Result drive_constraint_in(Doc& doc, base::Array cs, pr::Params params, std::string_view cid, double value,
                           const std::optional<std::string>& prefer, const i18n::Catalog& words) {
  Value* c = find_c(cs, cid);
  if (c == nullptr || !gcons::is_dim(*c)) {
    return fail(words.t("params.constraint_gone"));
  }
  std::vector<std::string> changed;
  const pr::Adapter a = adapter(doc, cs, &changed, words);
  const pr::Drive r = pr::drive_back(params, a, gcons::target_key(*c), value, prefer, words);
  if (!r.ok) {
    Result f = fail(r.error);
    f.choices = r.choices;
    return f;
  }
  if (r.direct) {
    if (pr::Var* v = pr::var_by_name(params, base::js_string(c->get("name")))) {
      v->expr = cad::expr::format_number(value);
    }
  }
  const std::string name = base::js_string(c->get("name"));
  Result res = commit(doc, "CONSTRAINT VALUE", cs, params, {}, words);
  if (res.ok && !r.var.empty() && r.var != name) {
    res.message = words.t("params.is_now", {{"name", r.var}, {"value", cad::expr::format_number(r.to)}});
  }
  return res;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — commit, as the JS
Result commit(Doc& doc, const std::string& label, const base::Array& constraints, const pr::Params& params,
              const CommitOpts& o, const i18n::Catalog& words) {
  base::Array draft = constraints;
  std::vector<std::string> changed;
  const pr::Adapter a = adapter(doc, draft, &changed, words);
  const pr::Applied ap = pr::apply_params(params, a, words);
  std::vector<std::pair<std::string, std::string>> errors;
  for (const auto& e : ap.errors) {
    const bool target = e.first.starts_with("c:");
    if (!target || std::ranges::any_of(draft, [&e](const Value& c) { return gcons::target_key(c) == e.first; })) {
      errors.push_back(e);
    }
  }
  if (!errors.empty()) {
    std::string text;
    for (std::size_t i = 0; i < errors.size() && i < 3; ++i) {
      const auto it =
          std::ranges::find_if(draft, [&](const Value& c) { return gcons::target_key(c) == errors[i].first; });
      text += (i ? " " : "") + (it != draft.end() ? base::js_string(it->get("name")) : errors[i].first) + ": " +
              errors[i].second;
    }
    return fail(text);
  }
  base::Array changed_cs;
  for (const Value& c : draft) {
    if (std::ranges::find(changed, gcons::target_key(c)) != changed.end()) {
      changed_cs.push_back(c);
    }
  }
  std::set<double> only = o.touch.value_or(std::set<double>{});
  for (const Value& c : changed_cs) {
    for (const Value& r : c.get("refs").as_array()) {
      only.insert(gs::ref_id(r));
    }
  }
  std::vector<std::pair<double, Value>> updates;
  if (!only.empty()) {
    const std::set<std::string> pins = o.pinned ? *o.pinned : soft_pins(doc, changed_cs);
    const gcons::Solved r = gcons::solve_constraints(doc, draft, pins.empty() ? nullptr : &pins, &only, false);
    if (!r.ok) {
      std::vector<std::string> names;
      std::set<std::string> seen;
      for (const std::string& id : r.conflicts) {
        if (!seen.insert(id).second || names.size() >= 3) {
          continue;
        }
        if (const Value* c = find_c(draft, id)) {
          names.push_back(gcons::describe_constraint(doc, *c, words));
        }
      }
      std::string joined;
      for (std::size_t i = 0; i < names.size(); ++i) {
        joined += (i ? "; " : "") + names[i];
      }
      Result f = fail(words.t("params.conflicts", {{"names", joined}}));
      f.conflicts = r.conflicts;
      return f;
    }
    updates = r.updates;
    if (!o.added.empty()) {
      const gs::Getter get = [&](double id) -> const Value* {
        for (const auto& [k, v] : updates) {
          if (k == id) {
            return &v;
          }
        }
        return entity(doc, id);
      };
      for (const base::Array& g : gs::components(draft)) {
        if (std::ranges::none_of(
                g, [&o](const Value& c) { return std::ranges::find(o.added, cid_of(c)) != o.added.end(); })) {
          continue;
        }
        const gs::Analysis an = gs::analyze(g, get);
        std::vector<std::string> bad;
        for (const std::string& id : o.added) {
          if (std::ranges::find(an.redundant, id) != an.redundant.end()) {
            bad.push_back(id);
          }
        }
        if (!bad.empty()) {
          const Value* c = find_c(draft, bad.front());
          std::string msg = words.t("params.redundant", {{"what", gcons::describe_constraint(doc, *c, words)}});
          if (gcons::is_dim(*c)) {
            msg += " " + words.t("params.use_plain_dimension_dimlinear");
          }
          Result f = fail(msg);
          f.redundant = bad;
          return f;
        }
      }
    }
  }
  doc.solving = true;
  try {
    doc.change(label, [&] {
      doc.set_parametric(Value(draft), pr::to_value(params));
      for (const auto& [id, e] : updates) {
        (void)doc.update(static_cast<Id>(id), e, doc.model());
      }
    });
  } catch (...) {
    doc.solving = false;
    throw;
  }
  doc.solving = false;
  Result ok;
  ok.ok = true;
  return ok;
}

DimInput parse_dim_input(std::string_view text) {
  const std::string s(base::js_trim(text));
  // /^([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=)\s*(.*)$/
  std::size_t i = 0;
  const auto word = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (c >= '0' && c <= '9');
  };
  if (!s.empty() && word(s[0]) && (s[0] < '0' || s[0] > '9')) {
    while (i < s.size() && word(s[i])) {
      ++i;
    }
    std::size_t j = i;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
      ++j;
    }
    if (j < s.size() && s[j] == '=' && (j + 1 >= s.size() || s[j + 1] != '=') && !s.contains('\n')) {
      return {.name = s.substr(0, i), .expr = std::string(base::js_trim(std::string_view(s).substr(j + 1)))};
    }
  }
  return {.name = std::nullopt, .expr = s};
}

Result add_constraint(Doc& doc, std::string_view type, const Value& refs, const AddOpts& o,
                      const i18n::Catalog& words) {
  gcons::MakeOpts mo;
  mo.loc = o.loc;
  mo.name = o.name;
  auto [made, why] = gcons::make_constraint(doc, type, refs, mo, words);
  if (!made) {
    return fail(why);
  }
  const Value c = *made;
  pr::Params params = pr::from_value(doc.params);
  if (gcons::is_dim(c)) {
    if (o.name) {
      if (const auto bad = pr::name_problem(Value(*o.name), &params, nullptr, words)) {
        return fail(*bad);
      }
    }
    const std::string e = !o.expr || o.expr->empty() ? cad::expr::format_number(round6(num(c.get("value")))) : *o.expr;
    std::set<std::string> names;
    for (const pr::Var& v : params.vars) {
      names.insert(v.name);
    }
    const auto r = cad::expr::parse(e, names, words);
    if (!r.ok) {
      return fail(r.error);
    }
    params.vars.push_back({.name = base::js_string(c.get("name")), .expr = e, .note = std::nullopt});
    params.links.push_back({.target = gcons::target_key(c), .expr = base::js_string(c.get("name"))});
  }
  base::Array cs = copy_constraints(doc);
  cs.push_back(c);
  // as in AutoCAD, the first object stays and the second one moves to meet the constraint (when it can)
  const auto& rl = refs.as_array();
  const double pin_id = o.pin_entity.value_or(gs::ref_id(rl.front()));
  const Value* first = entity(doc, pin_id);
  std::set<std::string> pinned;
  if (o.pin_first && !gcons::is_dim(c) && rl.size() > 1 && first != nullptr &&
      std::ranges::any_of(rl, [pin_id](const Value& r) { return gs::ref_id(r) != pin_id; })) {
    for (const std::string& f : gs::fields_of(*first)) {
      pinned.insert(key_of(pin_id, f));
    }
  }
  // a symmetry line stays where it is too
  if (type == "symmetric" && rl.size() > 2) {
    const double axis_id = gs::ref_id(rl[2]);
    const Value* axis = entity(doc, axis_id);
    if (o.pin_first && axis != nullptr && axis_id != gs::ref_id(rl[0]) && axis_id != gs::ref_id(rl[1])) {
      for (const std::string& f : gs::fields_of(*axis)) {
        pinned.insert(key_of(axis_id, f));
      }
    }
  }
  CommitOpts co;
  co.touch = std::set<double>{};
  for (const Value& r : rl) {
    co.touch->insert(gs::ref_id(r));
  }
  co.added = {cid_of(c)};
  if (!pinned.empty()) {
    co.pinned = pinned;
  }
  const gcons::TypeInfo* T = gcons::type_info(type);
  Result res = commit(doc, base::js_to_upper(words.t(T->label_key)), cs, params, co, words);
  if (res.ok) {
    for (const Value& k : doc.constraints.as_array()) {
      if (cid_of(k) == cid_of(c)) {
        res.c = k;
      }
    }
  }
  return res;
}

Result set_dim_input(Doc& doc, std::string_view cid, std::string_view text, const i18n::Catalog& words) {
  base::Array cs = copy_constraints(doc);
  const Value* c0 = find_c(cs, cid);
  if (c0 == nullptr || !gcons::is_dim(*c0)) {
    return fail(words.t("params.constraint_gone"));
  }
  const std::string old_name = base::js_string(c0->get("name"));
  const DimInput in = parse_dim_input(text);
  if (in.expr.empty()) {
    return fail(words.t("params.type_value_expression_like"));
  }
  pr::Params params = pr::from_value(doc.params);
  Value* me = find_c(cs, cid);
  if (in.name && *in.name != old_name) {
    const pr::Var* v = pr::var_by_name(params, old_name);
    const std::string e = v != nullptr ? v->expr : cad::expr::format_number(num(me->get("value")));
    const pr::Edit r = pr::set_var(params, *in.name, e, std::nullopt, old_name, words);
    if (!r.ok) {
      return fail(r.error);
    }
    me->set("name", *in.name);
  }
  const auto lit = cad::expr::literal(in.expr, words);
  const pr::Var* cur = pr::var_by_name(params, base::js_string(me->get("name")));
  if (lit && cur != nullptr && !cad::expr::literal(cur->expr, words)) {
    // a number typed over an expression: solve the parameter behind it (two-way)
    return drive_constraint_in(doc, std::move(cs), std::move(params), cid, *lit, std::nullopt, words);
  }
  const pr::Edit r = pr::set_var(params, base::js_string(me->get("name")), in.expr, std::nullopt, std::nullopt, words);
  if (!r.ok) {
    return fail(r.error);
  }
  return commit(doc, "CONSTRAINT VALUE", cs, params, {}, words);
}

Result drive_constraint(Doc& doc, std::string_view cid, double value, const std::optional<std::string>& prefer,
                        const i18n::Catalog& words) {
  return drive_constraint_in(doc, copy_constraints(doc), pr::from_value(doc.params), cid, value, prefer, words);
}

Result set_parameter(Doc& doc, std::string_view name, std::string_view expr, const std::optional<std::string>& note,
                     const std::optional<std::string>& rename, const i18n::Catalog& words) {
  pr::Params params = pr::from_value(doc.params);
  base::Array cs = copy_constraints(doc);
  const pr::Edit r = pr::set_var(params, name, base::js_trim(expr), note, rename, words);
  if (!r.ok) {
    return fail(r.error);
  }
  const bool renamed = rename && *rename != name;
  if (renamed) {
    for (Value& c : cs) {
      if (gcons::is_dim(c) && base::js_string(c.get("name")) == *rename) {
        c.set("name", std::string(name));
      }
    }
  }
  return commit(doc, renamed ? "RENAME PARAMETER" : "PARAMETER", cs, params, {}, words);
}

Result drive_parameter(Doc& doc, std::string_view name, double value, const std::optional<std::string>& prefer,
                       const i18n::Catalog& words) {
  pr::Params params = pr::from_value(doc.params);
  const pr::Drive r = pr::drive_var(params, name, value, prefer, words);
  if (!r.ok) {
    Result f = fail(r.error);
    f.choices = r.choices;
    return f;
  }
  Result res = commit(doc, "PARAMETER", copy_constraints(doc), params, {}, words);
  if (res.ok && !r.var.empty() && r.var != name) {
    res.message = words.t("params.is_now", {{"name", r.var}, {"value", cad::expr::format_number(r.to)}});
  }
  return res;
}

Result remove_parameter(Doc& doc, std::string_view name, bool inline_value, const i18n::Catalog& words) {
  for (const Value& c : copy_constraints(doc)) {
    if (gcons::is_dim(c) && base::js_string(c.get("name")) == name) {
      return fail(words.t("params.is_dimensional", {{"name", std::string(name)}}));
    }
  }
  pr::Params params = pr::from_value(doc.params);
  const pr::Edit r = pr::remove_var(params, name, inline_value, words);
  if (!r.ok) {
    Result f = fail(r.error);
    f.used = r.used;
    return f;
  }
  return commit(doc, "DELETE PARAMETER", copy_constraints(doc), params, {}, words);
}

Result remove_constraints(Doc& doc, const std::vector<std::string>& ids, const std::string& label,
                          const i18n::Catalog& words) {
  base::Array gone;
  base::Array keep;
  for (const Value& c : copy_constraints(doc)) {
    (std::ranges::find(ids, cid_of(c)) != ids.end() ? gone : keep).push_back(c);
  }
  Result out;
  out.ok = true;
  if (gone.empty()) {
    return out;
  }
  const pr::Params params = gcons::drop_dim_params(pr::from_value(doc.params), gone, words);
  doc.solving = true;
  try {
    doc.change(label, [&] { doc.set_parametric(Value(keep), pr::to_value(params)); });
  } catch (...) {
    doc.solving = false;
    throw;
  }
  doc.solving = false;
  out.removed = static_cast<double>(gone.size());
  return out;
}

Result solve_all(Doc& doc, const std::string& label, const i18n::Catalog& words) {
  CommitOpts o;
  o.touch = std::set<double>{};
  for (const Value& c : copy_constraints(doc)) {
    for (const Value& r : c.get("refs").as_array()) {
      o.touch->insert(gs::ref_id(r));
    }
  }
  return commit(doc, label, copy_constraints(doc), pr::from_value(doc.params), o, words);
}

Described describe(const Doc& doc, const i18n::Catalog& words) {
  Described out;
  const pr::Params p = pr::from_value(doc.params);
  const pr::Evaluated ev = pr::evaluate_vars(p, nullptr, words);
  const base::Array cs = copy_constraints(doc);
  for (const pr::Var& v : p.vars) {
    const auto it = std::ranges::find_if(
        cs, [&v](const Value& k) { return gcons::is_dim(k) && base::js_string(k.get("name")) == v.name; });
    const Value* c = it != cs.end() ? &*it : nullptr;
    const pr::Uses u = pr::uses_of(p, v.name, words);
    Row row;
    row.name = v.name;
    row.expr = v.expr;
    row.note = v.note.value_or("");
    row.value = ev.value(v.name);
    for (const auto& [k, msg] : ev.errors) {
      if (k == v.name) {
        row.error = msg;
      }
    }
    row.kind = c != nullptr ? "dim" : "user";
    if (c != nullptr) {
      row.constraint = *c;
      row.type = words.t(gcons::type_info(base::js_string(c->get("type")))->label_key);
    } else {
      row.type = words.t("params.user_parameter");
    }
    row.driving = cad::expr::literal(v.expr, words).has_value();
    row.used_by = u.vars;
    for (const std::string& l : u.links) {
      if (c == nullptr || l != gcons::target_key(*c)) {
        row.used_by.push_back(l);
      }
    }
    if (c != nullptr) {
      row.label = gcons::label_of(*c, &p, words);
      row.shown = gcons::fmt_value(*c, num(c->get("value")));
    }
    out.rows.push_back(std::move(row));
  }
  out.status = gcons::status_text(doc, words);
  return out;
}

}  // namespace cc::doc::params_doc
