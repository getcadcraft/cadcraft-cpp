// SPDX-License-Identifier: Apache-2.0
// A command script run headless (cadcraft-cli script, tools/parity/commands.mjs, the tests): a fresh drawing, a host
// that keeps what is printed and prompted, and steps — typed lines, clicks (with the object a pick finds), Esc, the
// cursor, windows, a preselection.
#include "cc/cmd/script.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/cmd/props.hpp"

namespace cc::cmd {

using base::Value;

namespace {

class ScriptHost final : public Host {
 public:
  base::Array log;
  std::optional<Pt> cur;
  IdSet sel;
  std::optional<Id> next_pick;
  std::vector<Id> window_ids;
  bool shift_down = false;

  void print(const std::string& text) override { log.emplace_back(base::Array{"print", text}); }
  void prompt(const std::string& text) override { log.emplace_back(base::Array{"prompt", text}); }
  [[nodiscard]] std::optional<Pt> cursor() const override { return cur; }
  [[nodiscard]] const IdSet& selection() const override { return sel; }
  void set_selection(IdSet s) override { sel = std::move(s); }
  std::optional<Id> pick_at(Pt /*raw*/) override { return std::exchange(next_pick, std::nullopt); }
  std::vector<Id> pick_window(const Rect& /*r*/, bool /*crossing*/) override { return std::exchange(window_ids, {}); }
  [[nodiscard]] bool shift() const override { return shift_down; }
  /// One clock for every script (a new sheet's title block is dated): 2026-10-16, UTC, as the parity harness's.
  [[nodiscard]] double now_ms() const override { return 1792108800000.0; }
};

double num(const Value& v) {
  return base::js_to_number(v);
}
Pt pt(const Value& a) {
  return a.is_array() && a.as_array().size() >= 2 ? Pt{num(a.as_array()[0]), num(a.as_array()[1])} : Pt{};
}
/// An id from a script (a whole number an entity could have), else nothing.
std::optional<Id> id_of(const Value& v) {
  if (!v.is_number()) {
    return std::nullopt;
  }
  const double d = v.as_number();
  if (!std::isfinite(d) || d < 0 || d >= 9e15) {
    return std::nullopt;
  }
  return static_cast<Id>(d);
}
std::vector<Id> ids(const Value& a) {
  std::vector<Id> out;
  if (a.is_array()) {
    for (const Value& v : a.as_array()) {
      if (const auto id = id_of(v)) {
        out.push_back(*id);
      }
    }
  }
  return out;
}

constexpr std::array<std::string_view, 5> kNumeric{"len", "ang", "num", "int", "area"};

/// The Properties panel (props.js): {props, ids?, group?, vertex?} lists the group's fields with the value they share;
/// {prop, value, ids?, group?, vertex?} sets one (a number field's text read as props.js reads it).
void props_step(doc::Doc& d, ScriptHost& host, const Value& s, const i18n::Catalog& words) {
  base::Array& log = host.log;
  std::vector<Value> ents;
  const Value sids = s.get("ids");
  for (const Id id : sids.is_array() ? ids(sids) : host.sel.ids()) {
    if (const Value* e = d.entities().get(id)) {
      ents.push_back(*e);
    }
  }
  const std::vector<props::Group> groups = props::groups_of(ents, words);
  const Value gk = s.get("group");
  std::string want = groups.empty() ? std::string() : groups[0].key;
  if (gk.is_string()) {
    want = gk.as_string();
  }
  const auto g = std::ranges::find_if(groups, [&want](const props::Group& x) { return x.key == want; });
  if (g == groups.end()) {
    log.emplace_back(base::Array{"props", nullptr});
    return;
  }
  props::Ctx ctx{.doc = &d, .words = &words, .ents = {}, .vertex = 0};
  for (const Id id : g->ids) {
    if (const Value* e = d.entities().get(id)) {
      ctx.ents.push_back(*e);
    }
  }
  if (ctx.ents.size() == 1) {
    ctx.vertex = base::js_to_int32(base::js_to_number(s.get("vertex")));
  }
  const std::vector<props::Def> defs = props::defs_for(g->key, ctx);
  if (s.get("props").truthy()) {
    base::Array gs;
    for (const props::Group& x : groups) {
      gs.emplace_back(base::Array{x.key, static_cast<double>(x.n), x.label});
    }
    base::Array fields;
    for (const props::Def& def : defs) {
      const auto v = props::value_of(def, ctx);
      Value shown = v ? *v : Value(std::string("\0varies", 7));
      if (v && v->is_undefined()) {
        shown = Value(nullptr);
      }
      fields.emplace_back(base::Array{def.key, def.sec, def.kind, def.label, std::move(shown)});
    }
    log.emplace_back(base::Array{"props", std::move(gs), std::move(fields)});
    return;
  }
  const std::string key = s.get("prop").as_string();
  const auto def = std::ranges::find_if(defs, [&key](const props::Def& x) { return x.key == key; });
  if (def == defs.end()) {
    log.emplace_back(base::Array{"prop", key, "missing"});
    return;
  }
  Value v = s.get("value");
  if (std::ranges::find(kNumeric, def->kind) != kNumeric.end()) {
    const ReadCtx rc{.lunits = d.meta.get("lunits"),
                     .units = d.meta.get("units").is_string() ? d.meta.get("units").as_string() : std::string(),
                     .paper = {},
                     .gt0 = def->gt0,
                     .min = def->min,
                     .max = def->max,
                     .vars = {}};
    const ReadResult r = read_value(base::js_string(v), def->kind == "area" ? "num" : def->kind, rc, words);
    if (!r.ok) {
      log.emplace_back(base::Array{"prop", key, "read", r.error});
      return;
    }
    v = Value(r.value);
  }
  const props::Applied r = props::apply_value(d, *def, v, ctx);
  log.emplace_back(base::Array{"prop", key, r.ok, static_cast<double>(r.changed), static_cast<double>(r.locked),
                               r.error.empty() ? Value(nullptr) : Value(r.error)});
}

}  // namespace

Value run_script(const Value& script, const i18n::Catalog& words, const ScriptLimits& limits) {
  doc::Doc d;
  ScriptHost host;
  Engine eng(d, host, words);
  const Value& steps = script["steps"];
  std::size_t n = 0;
  if (steps.is_array()) {
    for (const Value& s : steps.as_array()) {
      if (++n > limits.max_steps) {
        host.print("*script step limit*");
        break;
      }
      if (s.is_string()) {
        eng.text(s.as_string());
      } else if (s.is_object() && !s.is_point()) {
        if (const Value* c = s.find("click")) {
          const Value pk = s.get("pick");
          host.next_pick = id_of(pk);
          (void)eng.click(pt(*c));
        } else if (const Value* cu = s.find("cursor")) {
          host.cur = pt(*cu);
        } else if (s.find("esc") != nullptr) {
          (void)eng.escape();
        } else if (const Value* w = s.find("window")) {
          host.window_ids = ids(s.get("ids"));
          const Value& wa = *w;
          if (wa.is_array() && wa.as_array().size() >= 4) {
            const auto& a = wa.as_array();
            eng.window({.min_x = num(a[0]), .min_y = num(a[1]), .max_x = num(a[2]), .max_y = num(a[3])},
                       s.get("crossing").truthy());
          }
        } else if (const Value* se = s.find("select")) {
          IdSet set;
          for (const Id id : ids(*se)) {
            set.add(id);
          }
          host.sel = std::move(set);
        } else if (const Value* sh = s.find("shift")) {
          host.shift_down = sh->truthy();
        } else if (const Value* ly = s.find("layer"); ly != nullptr && ly->is_string()) {
          // what the Layers panel does (doc.setLayer): a layer made or changed, made current
          const Value patch = s.get("patch");
          d.set_layer(ly->as_string(), patch.is_object() && !patch.is_point() ? patch : Value::object(),
                      s.get("current").truthy());
        } else if (s.get("props").truthy() || s.get("prop").is_string()) {
          props_step(d, host, s, words);
        } else if (const Value* sp = s.find("space")) {
          // the space tabs (doc.setSpace): a sheet by name, anything else the model
          const bool known = sp->is_string() && std::ranges::any_of(d.layouts(), [sp](const doc::Layout& l) {
                               return l.name == sp->as_string();
                             });
          d.set_space(known ? sp->as_string() : std::string());
        }
      }
    }
  }
  (void)eng.escape();
  base::Array undo;
  for (const std::string& l : d.undo_labels()) {
    undo.emplace_back(l);
  }
  base::Array redo;
  for (const std::string& l : d.redo_labels()) {
    redo.emplace_back(l);
  }
  base::Array model;
  d.model()->each(
      [&](const doc::Id& id, const Value& e) { model.emplace_back(base::Array{static_cast<double>(id), e}); });
  base::Array layers;
  d.layers().each([&](const std::string& name, const Value& l) { layers.emplace_back(base::Array{name, l}); });
  base::Array blocks;
  d.blocks().each([&](const std::string& name, const Value& b) { blocks.emplace_back(base::Array{name, b}); });
  base::Array layouts;
  for (const doc::Layout& l : d.layouts()) {
    base::Array ents;
    l.entities->each(
        [&](const doc::Id& id, const Value& e) { ents.emplace_back(base::Array{static_cast<double>(id), e}); });
    layouts.emplace_back(base::Array{l.name, l.paper, std::move(ents)});
  }
  const doc::Layout* open = d.layout();
  return base::make_object({{"name", script.get("name")},
                            {"log", std::move(host.log)},
                            {"model", std::move(model)},
                            {"blocks", std::move(blocks)},
                            {"layers", std::move(layers)},
                            {"meta", d.meta},
                            {"ucs", d.ucs},
                            {"ucsNamed", d.ucs_named},
                            {"views", d.views},
                            {"current", d.current},
                            {"nextId", static_cast<double>(d.next_id())},
                            {"layouts", std::move(layouts)},
                            {"space", open != nullptr ? Value(open->name) : Value(nullptr)},
                            {"undo", std::move(undo)},
                            {"redo", std::move(redo)}});
}

}  // namespace cc::cmd
