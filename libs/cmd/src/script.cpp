// SPDX-License-Identifier: Apache-2.0
// A command script run headless (cadcraft-cli script, tools/parity/commands.mjs, the tests): a fresh drawing, a host
// that keeps what is printed and prompted, and steps — typed lines, clicks (with the object a pick finds), Esc, the
// cursor, windows, a preselection.
#include "cc/cmd/script.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cmd/engine.hpp"

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
};

double num(const Value& v) {
  return base::js_to_number(v);
}
Pt pt(const Value& a) {
  return a.is_array() && a.as_array().size() >= 2 ? Pt{num(a.as_array()[0]), num(a.as_array()[1])} : Pt{};
}
std::vector<Id> ids(const Value& a) {
  std::vector<Id> out;
  if (a.is_array()) {
    for (const Value& v : a.as_array()) {
      const double d = num(v);
      if (std::isfinite(d) && d >= 0 && d < 9e15) {
        out.push_back(static_cast<Id>(d));
      }
    }
  }
  return out;
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
          host.next_pick = pk.is_number() ? std::optional<Id>(static_cast<Id>(num(pk))) : std::nullopt;
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
  return base::make_object({{"name", script.get("name")},
                            {"log", std::move(host.log)},
                            {"model", std::move(model)},
                            {"current", d.current},
                            {"nextId", static_cast<double>(d.next_id())},
                            {"undo", std::move(undo)},
                            {"redo", std::move(redo)}});
}

}  // namespace cc::cmd
