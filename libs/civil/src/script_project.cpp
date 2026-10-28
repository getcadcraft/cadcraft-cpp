// SPDX-License-Identifier: Apache-2.0
// Script ops for project.js and the project-level ops of point-edit.js, sections.js and masshaul.js
// (tools/parity/civil/ops-civil.mjs has their JS twins).
#include <algorithm>
#include <memory>

#include "cc/base/json.hpp"
#include "cc/civil/codes.hpp"
#include "cc/civil/jsmap.hpp"
#include "cc/civil/project.hpp"
#include "cc/civil/script.hpp"
#include "cc/civil/survey.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::Value;

Value pview(const Project& p) {
  Array undo;
  for (const auto& s : p.undo_stack) {
    undo.emplace_back(s.label);
  }
  Array redo;
  for (const auto& s : p.redo_stack) {
    redo.emplace_back(s.label);
  }
  Value gf = Value::object();
  for (const auto& m : p.gis_features.as_object()) {
    Array rows;
    for (const Value& f : arr(m.value)) {
      rows.emplace_back(Array{field(f, "g"), field(f, "p")});
    }
    gf.append(m.key, std::move(rows));
  }
  return make_object({{"state", p.state},
                      {"points", p.points},
                      {"pointsFile", p.points_file},
                      {"version", p.version},
                      {"pointsVersion", p.points_version},
                      {"gisVersion", p.gis_version},
                      {"undo", std::move(undo)},
                      {"redo", std::move(redo)},
                      {"dirty", p.dirty()},
                      {"gisFeatures", std::move(gf)}});
}

Project& self_project(const ScriptCtx& c) {
  return self_as<Project>(c);
}

std::optional<Value> alignment_of(const Project& p, const Value& id) {
  for (const Value& a : arr(field(p.state, "alignments"))) {
    if (strict_equal(field(a, "id"), id)) {
      return a;
    }
  }
  return std::nullopt;
}

Value opt_str(const std::optional<std::string>& s) {
  return s ? Value(*s) : Value(nullptr);
}

}  // namespace

void add_project_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["project.new"] = [](const Array&, ScriptCtx& c) -> Value {
    auto p = std::make_shared<Project>(*c.words);
    c.keep(p);
    return pview(*p);
  };
  m["project.fromJSON"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto L = Project::from_json(arg(a, 0), *c.words);
    if (L.error) {
      throw ScriptError{*L.error};
    }
    c.keep(L.project);
    return make_object({{"notes", L.notes}, {"view", pview(*L.project)}});
  };
  m["project.reload"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto j = base::parse_json(base::js_stringify(self_project(c).to_json()));
    if (!j) {
      throw ScriptError{"the project file could not be read back"};
    }
    apply_ops(*j, arg(a, 0));
    auto L = Project::from_json(*j, *c.words);
    if (L.error) {
      throw ScriptError{*L.error};
    }
    c.keep(L.project);
    return make_object({{"notes", L.notes}, {"view", pview(*L.project)}});
  };
  m["project.view"] = [](const Array&, ScriptCtx& c) -> Value { return pview(self_project(c)); };
  m["project.setPoints"] = [](const Array& a, ScriptCtx& c) -> Value {
    self_project(c).set_points(arg(a, 0), nullish(arg(a, 1)) ? std::string() : str(arg(a, 1)));
    return nullptr;
  };
  m["project.editPoints"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value e = arg(a, 1).truthy() ? arg(a, 1) : Value::object();
    return self_project(c).edit_points(str(arg(a, 0)), [&](Value pts, Value&) -> std::optional<Value> {
      if (field(e, "none").truthy()) {
        return std::nullopt;
      }
      Value out = std::move(pts);
      if (field(e, "set").truthy()) {
        out = Value(arr(field(e, "set")));
      }
      if (field(e, "remove").truthy()) {
        const Array& rm = arr(field(e, "remove"));
        Array kept;
        for (const Value& q : arr(out)) {
          if (std::ranges::none_of(rm, [&](const Value& r) { return strict_equal(r, field(q, "n")); })) {
            kept.push_back(q);
          }
        }
        out = std::move(kept);
      }
      if (field(e, "add").truthy()) {
        for (const Value& q : arr(field(e, "add"))) {
          out.push(q);
        }
      }
      return out;
    });
  };
  m["project.change"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value ops = arg(a, 1);
    return self_project(c).change(str(arg(a, 0)), [&](Value& st) { apply_ops(st, ops); });
  };
  m["project.setDisplay"] = [](const Array& a, ScriptCtx& c) -> Value {
    self_project(c).set_display(str(arg(a, 0)), arg(a, 1));
    return nullptr;
  };
  m["project.undo"] = [](const Array&, ScriptCtx& c) -> Value { return opt_str(self_project(c).undo()); };
  m["project.redo"] = [](const Array&, ScriptCtx& c) -> Value { return opt_str(self_project(c).redo()); };
  m["project.nextId"] = [](const Array& a, ScriptCtx& c) -> Value { return self_project(c).next_id(str(arg(a, 0))); };
  m["project.setTin"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value ops = arg(a, 2);
    if (ops.is_array() && !ops.as_array().empty()) {
      const std::function<void(Value&)> fn = [&](Value& st) { apply_ops(st, ops); };
      return self_project(c).set_tin(str(arg(a, 0)), arg(a, 1), &fn);
    }
    return self_project(c).set_tin(str(arg(a, 0)), arg(a, 1), nullptr);
  };
  m["project.setCoding"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value label = arg(a, 2);
    return self_project(c).set_coding(arg(a, 0), arg(a, 1), nullish(label) ? c.words->t("codes.undo") : str(label));
  };
  m["project.groups"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).groups(); };
  m["project.figures"] = [](const Array& a, ScriptCtx& c) -> Value {
    if (!arg(a, 0).truthy()) {
      return self_project(c).figures(nullptr);
    }
    std::vector<std::string> only;
    for (const Value& g : arr(arg(a, 0))) {
      only.push_back(str(g));
    }
    return self_project(c).figures(&only);
  };
  m["project.egBreaklines"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).eg_breaklines(); };
  m["project.egPoints"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).eg_points(); };
  m["project.eg"] = [](const Array&, ScriptCtx& c) -> Value {
    const auto& e = self_project(c).eg();
    return make_object({{"surface", e.surface ? surface_view(*e.surface) : Value(nullptr)}, {"report", e.report}});
  };
  m["project.contours"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).contours(); };
  m["project.alignment"] = [](const Array& a, ScriptCtx& c) -> Value {
    Project& p = self_project(c);
    const auto al = alignment_of(p, arg(a, 0));
    return al ? al_view_of(*p.alignment(*al)) : Value(nullptr);
  };
  m["project.egProfile"] = [](const Array& a, ScriptCtx& c) -> Value {
    Project& p = self_project(c);
    const auto al = alignment_of(p, arg(a, 0));
    return al ? p.eg_profile(*al) : Value(nullptr);
  };
  m["project.designProfile"] = [](const Array& a, ScriptCtx& c) -> Value {
    Project& p = self_project(c);
    const auto al = alignment_of(p, arg(a, 0));
    return al ? prof_view_of(*p.design_profile(*al)) : Value(nullptr);
  };
  m["project.graded"] = [](const Array&, ScriptCtx& c) -> Value {
    const auto& g = self_project(c).graded();
    return make_object({{"surface", g.surface ? surface_view(*g.surface) : Value(nullptr)},
                        {"report", g.report},
                        {"regions", g.regions},
                        {"items", g.items}});
  };
  m["project.sections"] = [](const Array& a, ScriptCtx& c) -> Value {
    Project& p = self_project(c);
    const auto al = alignment_of(p, arg(a, 0));
    if (!al) {
      return nullptr;
    }
    const auto s = p.sections(*al);
    return s ? *s : Value(nullptr);
  };
  m["project.volumes"] = [](const Array& a, ScriptCtx& c) -> Value {
    return self_project(c).volumes(nullish(arg(a, 0)) ? Value() : arg(a, 0));
  };
  m["project.networkCheck"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).network_check(); };
  m["project.toJSON"] = [](const Array&, ScriptCtx& c) -> Value { return self_project(c).to_json(); };
  m["project.editPoint"] = [](const Array& a, ScriptCtx& c) -> Value {
    Project& p = self_project(c);
    const Value n = arg(a, 0);
    const Value patch = nullish(arg(a, 1)) ? Value() : arg(a, 1);
    Value r = check_point_edit(p.points, n, patch, *c.words);
    if (!field(r, "ok").truthy()) {
      return r;
    }
    const Value old = r.get("old");
    const Value point = r.get("point");
    const auto index = static_cast<std::size_t>(num(r.get("index")));
    double followed = 0;
    const bool moved = !strict_equal(field(point, "x"), field(old, "x")) ||
                       !strict_equal(field(point, "y"), field(old, "y")) ||
                       !strict_equal(field(point, "z"), field(old, "z"));
    const Value label_v = arg(a, 2);
    const std::string label =
        label_v.truthy() ? str(label_v) : c.words->t("pt.edit.undo", {{"n", str(field(old, "n"))}});
    (void)p.edit_points(label, [&](Value pts, Value& s) -> std::optional<Value> {
      if (index < arr(pts).size()) {
        pts.as_array()[index] = point;
      }
      if (moved) {
        Value* surf = s.find("surface");
        Value* bls = surf != nullptr ? surf->find("breaklines") : nullptr;
        if (bls != nullptr && bls->is_array()) {
          for (Value& b : bls->as_array()) {
            Value* bp = b.is_object() ? b.find("pts") : nullptr;
            if (bp == nullptr || !bp->is_array()) {
              continue;
            }
            for (Value& v : bp->as_array()) {
              if (!strict_equal(field(v, "x"), field(old, "x")) || !strict_equal(field(v, "y"), field(old, "y"))) {
                continue;
              }
              v.set("x", field(point, "x"));
              v.set("y", field(point, "y"));
              if (!field(point, "z").is_null()) {
                v.set("z", field(point, "z"));
              }
              followed++;
            }
          }
        }
      }
      return pts;
    });
    std::string msg = c.words->t("pt.edit.done", {{"n", str(field(point, "n"))}});
    if (!strict_equal(field(point, "n"), field(old, "n"))) {
      msg += " " + c.words->t("pt.edit.renumbered", {{"from", str(field(old, "n"))}, {"to", str(field(point, "n"))}});
    }
    if (followed != 0) {
      msg += " " + c.words->tn("pt.edit.followed", followed, {{"n", str(Value(followed))}});
    }
    if (moved && field(point, "z").is_null() && followed != 0) {
      msg += " " + c.words->t("pt.edit.no.z");
    }
    if (field(field(p.state, "surface"), "tin").truthy()) {
      msg += " " + c.words->t("pt.edit.tin");
    }
    r.set("followed", followed);
    r.set("message", msg);
    return r;
  };
  m["sec.project"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto p = reg_get<Project>(c, str(arg(a, 0)));
    if (!p) {
      throw ScriptError{"Cannot read properties of undefined (reading 'state')"};
    }
    const auto al = alignment_of(*p, arg(a, 1));
    if (!al) {
      return nullptr;
    }
    auto set = p->sections(*al);
    if (!set) {
      return nullptr;
    }
    c.keep(set);
    return *set;
  };
  m["mh.project"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto p = reg_get<Project>(c, str(arg(a, 0)));
    if (!p) {
      throw ScriptError{"Cannot read properties of undefined (reading 'state')"};
    }
    const auto al = alignment_of(*p, arg(a, 1));
    if (!al) {
      return nullptr;
    }
    auto set = p->sections(*al);
    if (!set) {
      return nullptr;
    }
    const Value f = field(arg(a, 2), "factor");
    auto mh = std::make_shared<Value>(mass_haul(set->get("rows"), f.is_undefined() ? Value(1) : f));
    c.keep(mh);
    return *mh;
  };
}

}  // namespace cc::civil
