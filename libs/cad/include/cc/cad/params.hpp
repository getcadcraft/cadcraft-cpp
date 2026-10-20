// SPDX-License-Identifier: Apache-2.0
// cc::cad::params — params.js (CadCraft 0.5): the parameter table — variables and equations, resolved in both
// directions. params = { vars: [{ name, expr, note? }], links: [{ target, expr }] }. A variable whose expr is a plain
// number drives; the rest are driven. A link sets the document value `target` to `expr`; typing a value into a driven
// one solves the driving variable behind it (driveBack, driveVar: secant, then bracket and bisect).
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cad::params {

struct Limits {
  static constexpr std::size_t vars = 500, links = 4000, expr = 1000, note = 200;
};

struct Var {
  std::string name, expr;
  std::optional<std::string> note;
};
struct Link {
  std::string target, expr;
};
struct Params {
  std::vector<Var> vars;
  std::vector<Link> links;
};

[[nodiscard]] Params from_value(const base::Value& v);  // a document's params (already checked)
[[nodiscard]] base::Value to_value(const Params& p);
[[nodiscard]] bool has_params(const Params& p);
[[nodiscard]] Var* var_by_name(Params& p, std::string_view name);
[[nodiscard]] const Var* var_by_name(const Params& p, std::string_view name);
[[nodiscard]] Link* link_for(Params& p, std::string_view key);
[[nodiscard]] bool is_driving(const Var& v, const i18n::Catalog& words);

/// nameProblem(name, p, self): why a name cannot be a variable, or nothing
[[nodiscard]] std::optional<std::string> name_problem(const base::Value& name, const Params* p, const Var* self,
                                                      const i18n::Catalog& words);
/// readParams(raw, { targetKeys }): untrusted params (a file's) → checked ones, or the reason (thrown in the JS)
[[nodiscard]] std::pair<std::optional<Params>, std::string> read_params(const base::Value& raw,
                                                                        const std::set<std::string>* target_keys,
                                                                        const i18n::Catalog& words);

struct Evaluated {
  std::vector<std::pair<std::string, double>> values;  // in dependency order (a Map)
  std::vector<std::pair<std::string, std::string>> errors;
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> uses;
  [[nodiscard]] std::optional<double> value(std::string_view name) const;
  [[nodiscard]] std::map<std::string, double> value_map() const;
};
/// evaluateVars(p, overrides)
[[nodiscard]] Evaluated evaluate_vars(const Params& p, const std::vector<std::pair<std::string, double>>* overrides,
                                      const i18n::Catalog& words);

/// What an app lets an equation drive (the adapter's targets).
struct Target {
  std::string label;
  std::string kind;  // "length" | "angle" | "count" | "number"
  std::function<double()> get;
  std::function<void(double)> set;
  std::optional<double> min, max;
  bool integer = false;
};
struct Adapter {
  std::function<std::vector<std::pair<std::string, Target>>()> targets;
  std::function<std::vector<std::pair<std::string, std::string>>(const std::vector<std::string>& changed)> after_apply;
};
[[nodiscard]] std::optional<std::string> value_problem(const Target& t, double v, const i18n::Catalog& words);

struct Applied {
  bool ok = false;
  std::vector<std::pair<std::string, std::string>> errors;  // where, message
  std::vector<std::string> changed;
};
/// applyParams(doc, p, adapter): every variable, then every link, written through the adapter
[[nodiscard]] Applied apply_params(const Params& p, const Adapter& a, const i18n::Catalog& words);

struct Drive {
  bool ok = false;
  bool direct = false;  // not linked: the caller writes the value
  std::string error;
  std::vector<std::string> choices;
  std::string var, link, from_text, to_text;
  double from = 0, to = 0;
};
/// driveBack(p, adapter, doc, key, value, { prefer }) — mutates p
[[nodiscard]] Drive drive_back(Params& p, const Adapter& a, std::string_view key, double value,
                               const std::optional<std::string>& prefer, const i18n::Catalog& words);
/// driveVar(p, name, value, { prefer }) — mutates p
[[nodiscard]] Drive drive_var(Params& p, std::string_view name, double value, const std::optional<std::string>& prefer,
                              const i18n::Catalog& words);

struct Edit {
  bool ok = false;
  std::string error;
  bool cycle = false;
  bool used = false;  // remove_var: refused because something uses it (the JS's `uses`)
};
/// setVar(p, name, expr, { note, rename })
[[nodiscard]] Edit set_var(Params& p, std::string_view name, std::string_view expr,
                           const std::optional<std::string>& note, const std::optional<std::string>& rename,
                           const i18n::Catalog& words);
struct Uses {
  std::vector<std::string> vars, links;
};
[[nodiscard]] Uses uses_of(const Params& p, std::string_view name, const i18n::Catalog& words);
/// removeVar(p, name, { inline })
[[nodiscard]] Edit remove_var(Params& p, std::string_view name, bool inline_value, const i18n::Catalog& words);
bool remove_link(Params& p, std::string_view key);

}  // namespace cc::cad::params
