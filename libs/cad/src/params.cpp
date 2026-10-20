// SPDX-License-Identifier: Apache-2.0
// params.js (CadCraft 0.5) in C++.
#include "cc/cad/params.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"

namespace cc::cad::params {

using base::Value;

namespace {

/// text(s, max): control characters out, at most `max` UTF-16 units
std::string text(const Value& s, std::size_t max) {
  const std::string in = s.is_null() || s.is_undefined() ? std::string() : base::js_string(s);
  std::string clean;
  for (const char c : in) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7f) {
      clean += c;
    }
  }
  const std::vector<std::uint16_t> units = base::to_utf16(clean);
  if (units.size() <= max) {
    return clean;
  }
  std::string out;
  base::append_utf16(out, std::span(units).first(max));
  return out;
}

std::set<std::string> names_of(const Params& p) {
  std::set<std::string> s;
  for (const Var& v : p.vars) {
    s.insert(v.name);
  }
  return s;
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
  std::string s;
  for (std::size_t i = 0; i < v.size(); ++i) {
    s += (i ? std::string(sep) : std::string()) + v[i];
  }
  return s;
}

double jmax(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::nan("");
  }
  return a > b ? a : b;
}
double jsign(double x) {
  if (std::isnan(x)) {
    return x;
  }
  if (x == 0) {
    return x;
  }
  return x > 0 ? 1 : -1;
}
/// Math.sign(a) !== Math.sign(b) (NaN is never equal; +0 and -0 are)
bool sign_differs(double a, double b) {
  const double sa = jsign(a);
  const double sb = jsign(b);
  return !(sa == sb);
}

}  // namespace

std::optional<double> Evaluated::value(std::string_view name) const {
  for (const auto& [k, v] : values) {
    if (k == name) {
      return v;
    }
  }
  return std::nullopt;
}
std::map<std::string, double> Evaluated::value_map() const {
  return {values.begin(), values.end()};
}

Params from_value(const Value& v) {
  Params p;
  const Value vars = v.get("vars");
  if (vars.is_array()) {
    for (const Value& x : vars.as_array()) {
      Var w{.name = base::js_string(x.get("name")), .expr = base::js_string(x.get("expr")), .note = std::nullopt};
      if (const Value n = x.get("note"); n.is_string()) {
        w.note = n.as_string();
      }
      p.vars.push_back(std::move(w));
    }
  }
  const Value links = v.get("links");
  if (links.is_array()) {
    for (const Value& l : links.as_array()) {
      p.links.push_back({.target = base::js_string(l.get("target")), .expr = base::js_string(l.get("expr"))});
    }
  }
  return p;
}

Value to_value(const Params& p) {
  base::Array vars;
  for (const Var& v : p.vars) {
    Value o = base::make_object({{"name", v.name}, {"expr", v.expr}});
    if (v.note) {
      o.set("note", *v.note);
    }
    vars.push_back(std::move(o));
  }
  base::Array links;
  for (const Link& l : p.links) {
    links.push_back(base::make_object({{"target", l.target}, {"expr", l.expr}}));
  }
  return base::make_object({{"vars", Value(std::move(vars))}, {"links", Value(std::move(links))}});
}

bool has_params(const Params& p) {
  return !p.vars.empty() || !p.links.empty();
}
Var* var_by_name(Params& p, std::string_view name) {
  const auto it = std::ranges::find_if(p.vars, [name](const Var& v) { return v.name == name; });
  return it == p.vars.end() ? nullptr : &*it;
}
const Var* var_by_name(const Params& p, std::string_view name) {
  const auto it = std::ranges::find_if(p.vars, [name](const Var& v) { return v.name == name; });
  return it == p.vars.end() ? nullptr : &*it;
}
Link* link_for(Params& p, std::string_view key) {
  const auto it = std::ranges::find_if(p.links, [key](const Link& l) { return l.target == key; });
  return it == p.links.end() ? nullptr : &*it;
}
bool is_driving(const Var& v, const i18n::Catalog& words) {
  return expr::literal(v.expr, words).has_value();
}

std::optional<std::string> name_problem(const Value& name, const Params* p, const Var* self,
                                        const i18n::Catalog& words) {
  if (!name.is_string() || !expr::valid_name(name.as_string())) {
    return words.t("param.name_starts_letter_has");
  }
  const std::string& n = name.as_string();
  if (expr::reserved(n) || expr::reserved(base::js_to_lower(n))) {
    return words.t("param.function_constant_unit_name", {{"name", n}});
  }
  if (n == "__proto__" || n == "constructor" || n == "prototype") {
    return words.t("param.is_reserved", {{"name", n}});
  }
  if (p != nullptr && std::ranges::any_of(p->vars, [&](const Var& v) { return v.name == n && &v != self; })) {
    return words.t("param.there_already_parameter_called", {{"name", n}});
  }
  return std::nullopt;
}

std::pair<std::optional<Params>, std::string> read_params(const Value& raw, const std::set<std::string>* target_keys,
                                                          const i18n::Catalog& words) {
  const auto fail = [](const std::string& why) {
    return std::pair<std::optional<Params>, std::string>{std::nullopt, why};
  };
  if (!raw.is_object() || raw.is_point()) {
    return fail(words.t("param.parameters_file_bad"));
  }
  const Value vars0 = raw.get("vars");
  const Value links0 = raw.get("links");
  const Value vars = vars0.is_null() || vars0.is_undefined() ? Value::array() : vars0;
  const Value links = links0.is_null() || links0.is_undefined() ? Value::array() : links0;
  if (!vars.is_array() || vars.as_array().size() > Limits::vars) {
    return fail(words.t("param.parameter_list_file_bad"));
  }
  if (!links.is_array() || links.as_array().size() > Limits::links) {
    return fail(words.t("param.equation_list_file_bad"));
  }
  Params out;
  for (const Value& v : vars.as_array()) {
    if (!v.is_object() || v.is_point()) {
      return fail(words.t("param.parameter_file_bad"));
    }
    if (const auto bad = name_problem(v.get("name"), &out, nullptr, words)) {
      return fail(words.t("param.parameter", {{"name", text(v.get("name"), 40)}, {"bad", *bad}}));
    }
    const Value e = v.get("expr");
    if (!e.is_string() || base::utf16_length(e.as_string()) > Limits::expr ||
        !expr::parse(e.as_string(), std::nullopt, words).ok) {
      return fail(words.t("param.parameter_value_not_valid", {{"name", v.get("name").as_string()}}));
    }
    Var g{.name = v.get("name").as_string(), .expr = e.as_string(), .note = std::nullopt};
    if (const Value n = v.get("note"); n.truthy()) {
      g.note = text(n, Limits::note);
    }
    out.vars.push_back(std::move(g));
  }
  std::set<std::string> seen;
  for (const Value& l : links.as_array()) {
    const Value t = l.is_object() && !l.is_point() ? l.get("target") : Value();
    // TARGET_RE /^[A-Za-z0-9_.:@#-]{1,120}$/
    const bool target_ok = t.is_string() && !t.as_string().empty() && t.as_string().size() <= 120 &&
                           std::ranges::all_of(t.as_string(), [](char c) {
                             return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                    c == '_' || c == '.' || c == ':' || c == '@' || c == '#' || c == '-';
                           });
    if (!target_ok || seen.contains(t.as_string())) {
      return fail(words.t("param.equation_file_bad"));
    }
    if (target_keys != nullptr && !target_keys->contains(t.as_string())) {
      return fail(words.t("param.equation_drives_which_file", {{"target", text(t, 60)}}));
    }
    const Value e = l.get("expr");
    if (!e.is_string() || base::utf16_length(e.as_string()) > Limits::expr ||
        !expr::parse(e.as_string(), std::nullopt, words).ok) {
      return fail(words.t("param.equation_not_valid_expression", {{"target", t.as_string()}}));
    }
    seen.insert(t.as_string());
    out.links.push_back({.target = t.as_string(), .expr = e.as_string()});
  }
  return {std::move(out), std::string()};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — evaluateVars, as the JS
Evaluated evaluate_vars(const Params& p, const std::vector<std::pair<std::string, double>>* overrides,
                        const i18n::Catalog& words) {
  const auto override_of = [overrides](const std::string& n) -> std::optional<double> {
    if (overrides == nullptr) {
      return std::nullopt;
    }
    for (const auto& [k, v] : *overrides) {
      if (k == n) {
        return v;
      }
    }
    return std::nullopt;
  };
  Evaluated out;
  const std::set<std::string> names = names_of(p);
  std::map<std::string, expr::Ast> parsed;
  const auto set_error = [&out](const std::string& n, std::string msg) {
    for (auto& [k, v] : out.errors) {
      if (k == n) {
        v = std::move(msg);
        return;
      }
    }
    out.errors.emplace_back(n, std::move(msg));
  };
  const auto has_error = [&out](const std::string& n) {
    return std::ranges::any_of(out.errors, [&n](const auto& e) { return e.first == n; });
  };
  for (const Var& v : p.vars) {
    if (override_of(v.name)) {
      out.uses[v.name] = {};
      continue;
    }
    const expr::Parsed r = expr::parse(v.expr, names, words);
    if (!r.ok) {
      set_error(v.name, r.error);
      out.uses[v.name] = {};
      continue;
    }
    parsed[v.name] = r.ast;
    out.uses[v.name] = r.uses;
  }
  // dependency order (DFS); a cycle marks every variable on it
  std::map<std::string, int> state;
  std::vector<std::string> stack;
  std::function<void(const std::string&, int)> visit = [&](const std::string& n, int depth) {
    if (state[n] == 2 || depth > 600) {
      return;
    }
    if (state[n] == 1) {
      const auto at = std::ranges::find(stack, n);
      const std::vector<std::string> loop(at, stack.end());
      const std::string msg = join(loop, ", ") + " " +
                              (loop.size() == 1 ? words.t("param.uses_itself") : words.t("param.use_each_other")) + ".";
      for (const std::string& m : loop) {
        set_error(m, msg);
      }
      return;
    }
    state[n] = 1;
    stack.push_back(n);
    const auto it = out.uses.find(n);
    if (it != out.uses.end()) {
      const std::vector<std::string> deps = it->second;
      for (const std::string& d : deps) {
        visit(d, depth + 1);
      }
    }
    stack.pop_back();
    state[n] = 2;
    out.order.push_back(n);
  };
  for (const Var& v : p.vars) {
    visit(v.name, 0);
  }
  std::map<std::string, double> values;
  for (const std::string& n : out.order) {
    if (const auto o = override_of(n)) {
      values[n] = *o;
      out.values.emplace_back(n, *o);
      continue;
    }
    if (has_error(n)) {
      continue;
    }
    std::vector<std::string> missing;
    const auto it = out.uses.find(n);
    if (it != out.uses.end()) {
      for (const std::string& d : it->second) {
        if (!values.contains(d)) {
          missing.push_back(d);
        }
      }
    }
    if (!missing.empty()) {
      set_error(
          n, words.tn("param.uses_which_no_value", static_cast<double>(missing.size()), {{"v", join(missing, ", ")}}));
      continue;
    }
    const auto pit = parsed.find(n);
    if (pit == parsed.end()) {
      continue;  // a name used but not defined: parse() refused it already
    }
    const expr::Result r = expr::evaluate(pit->second, values, words);
    if (!r.ok) {
      set_error(n, r.error);
      continue;
    }
    values[n] = r.value;
    out.values.emplace_back(n, r.value);
  }
  return out;
}

std::optional<std::string> value_problem(const Target& t, double v, const i18n::Catalog& words) {
  if (!std::isfinite(v)) {
    return words.t("param.not_number");
  }
  if (t.integer || t.kind == "count") {
    if (std::abs(v - base::js_round(v)) > 1e-9) {
      return words.t("param.has_whole_number_came", {{"label", t.label}, {"v", expr::format_number(v)}});
    }
  }
  if (t.min && v < *t.min - 1e-12) {
    return words.t("param.has_least_came_out",
                   {{"label", t.label}, {"min", expr::format_number(*t.min)}, {"v", expr::format_number(v)}});
  }
  if (t.max && v > *t.max + 1e-12) {
    return words.t("param.can_most_came_out",
                   {{"label", t.label}, {"max", expr::format_number(*t.max)}, {"v", expr::format_number(v)}});
  }
  return std::nullopt;
}

Applied apply_params(const Params& p, const Adapter& a, const i18n::Catalog& words) {
  const Evaluated ev = evaluate_vars(p, nullptr, words);
  Applied out;
  out.errors = ev.errors;
  const auto targets = a.targets ? a.targets() : std::vector<std::pair<std::string, Target>>{};
  std::set<std::string> known;
  for (const auto& [k, v] : ev.values) {
    known.insert(k);
  }
  for (const auto& [k, v] : ev.errors) {
    known.insert(k);
  }
  const std::map<std::string, double> values = ev.value_map();
  for (const Link& l : p.links) {
    const auto ti = std::ranges::find_if(targets, [&l](const auto& t) { return t.first == l.target; });
    if (ti == targets.end()) {
      out.errors.emplace_back(l.target, words.t("param.equation_drives_something_gone"));
      continue;
    }
    const Target& t = ti->second;
    const expr::Parsed r = expr::parse(l.expr, known, words);
    if (!r.ok) {
      out.errors.emplace_back(l.target, r.error);
      continue;
    }
    std::vector<std::string> bad;
    for (const std::string& n : r.uses) {
      if (!values.contains(n)) {
        bad.push_back(n);
      }
    }
    if (!bad.empty()) {
      out.errors.emplace_back(
          l.target, words.tn("param.uses_which_no_value", static_cast<double>(bad.size()), {{"v", join(bad, ", ")}}));
      continue;
    }
    const expr::Result e = expr::evaluate(r.ast, values, words);
    if (!e.ok) {
      out.errors.emplace_back(l.target, e.error);
      continue;
    }
    double v = e.value;
    if (t.integer || t.kind == "count") {
      v = base::js_round(v);
    }
    if (const auto vp = value_problem(t, v, words)) {
      out.errors.emplace_back(l.target, *vp);
      continue;
    }
    const double cur = t.get();
    if (!(std::abs(cur - v) <= 1e-12 * jmax(1, std::abs(v)))) {
      t.set(v);
      out.changed.push_back(l.target);
    }
  }
  if (a.after_apply) {
    for (auto& e : a.after_apply(out.changed)) {
      out.errors.push_back(std::move(e));
    }
  }
  out.ok = out.errors.empty();
  return out;
}

namespace {

/// drivingBehind(p, src): the driving variables an expression ends up depending on
std::vector<std::string> driving_behind(const Params& p, std::string_view src, const i18n::Catalog& words) {
  const expr::Parsed r = expr::parse(src, std::nullopt, words);
  if (!r.ok) {
    return {};
  }
  const Evaluated ev = evaluate_vars(p, nullptr, words);
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::function<void(const std::string&)> walk = [&](const std::string& n) {
    if (!seen.insert(n).second) {
      return;
    }
    const Var* v = var_by_name(p, n);
    if (v == nullptr) {
      return;
    }
    if (is_driving(*v, words)) {
      if (std::ranges::find(out, n) == out.end()) {
        out.push_back(n);
      }
      return;
    }
    const auto it = ev.uses.find(n);
    if (it != ev.uses.end()) {
      for (const std::string& d : it->second) {
        walk(d);
      }
    }
  };
  for (const std::string& u : r.uses) {
    walk(u);
  }
  return out;
}

/// nice(x): the roundest number near x that still solves it
double nice(double x, const std::function<double(double)>& f, const std::function<bool(double)>& close) {
  for (int d = 0; d <= 10; ++d) {
    const double k = std::pow(10.0, d);
    const double r = base::js_round(x * k) / k;
    if (close(f(r))) {
      return r;
    }
  }
  const double r = base::js_number(base::js_to_exponential(x, 11));
  return close(f(r)) ? r : x;
}

std::optional<double> solve_chain(const std::function<double(double)>& chain, double x0, double value) {
  const auto f = [&chain, value](double x) { return chain(x) - value; };
  const double scale = jmax(jmax(1, std::abs(value)), std::abs(x0));
  const auto close = [scale](double y) { return std::isfinite(y) && std::abs(y) <= 1e-10 * scale; };
  double a = x0;
  double fa = f(a);
  if (close(fa)) {
    return a;
  }
  double b = x0 == 0 ? 1e-3 : x0 * (1 + 1e-4);
  double fb = f(b);
  for (int k = 0; k < 80 && std::isfinite(fa) && std::isfinite(fb) && fb != fa; ++k) {
    const double c = b - fb * (b - a) / (fb - fa);
    const double fc = f(c);
    if (close(fc)) {
      return nice(c, f, close);
    }
    a = b;
    fa = fb;
    b = c;
    fb = fc;
  }
  const double g0 = f(x0);
  double step = jmax(1e-6, std::abs(x0) * 1e-3);
  while (step < 1e9) {
    for (const double s : {x0 + step, x0 - step}) {
      const double gs = f(s);
      if (close(gs)) {
        return nice(s, f, close);
      }
      if (std::isfinite(g0) && std::isfinite(gs) && sign_differs(gs, g0)) {
        double lo = x0;
        double hi = s;
        double glo = g0;
        for (int k = 0; k < 200; ++k) {
          const double mid = (lo + hi) / 2;
          const double gm = f(mid);
          if (close(gm) || std::abs(hi - lo) <= 1e-15 * jmax(1, std::abs(mid))) {
            return nice(mid, f, close);
          }
          if (!sign_differs(gm, glo)) {
            lo = mid;
            glo = gm;
          } else {
            hi = mid;
          }
        }
      }
    }
    step *= 2;
  }
  return std::nullopt;
}

Drive solve_back(const Params& p, std::string_view src, double value, const std::optional<std::string>& prefer,
                 const std::string& label, const i18n::Catalog& words) {
  Drive out;
  const std::vector<std::string> behind = driving_behind(p, src, words);
  if (behind.empty()) {
    out.error = words.t("param.fixed_expression_change_equation", {{"label", label}, {"src", std::string(src)}});
    return out;
  }
  std::string name = prefer.value_or("");
  if (!name.empty() && std::ranges::find(behind, name) == behind.end()) {
    out.error = words.t("param.does_not_depend", {{"label", label}, {"name", name}});
    out.choices = behind;
    return out;
  }
  if (name.empty()) {
    if (behind.size() > 1) {
      out.error = words.t("param.depends_pick_which_one", {{"label", label}, {"v", join(behind, ", ")}});
      out.choices = behind;
      return out;
    }
    name = behind.front();
  }
  const expr::Ast ast = expr::parse(src, std::nullopt, words).ast;
  const Evaluated base0 = evaluate_vars(p, nullptr, words);
  const double from = base0.value(name).value_or(std::nan(""));
  // f(x): re-evaluate the whole chain with `name` = x, then the expression (a throw is NaN)
  const auto chain = [&](double x) {
    const std::vector<std::pair<std::string, double>> ov{{name, x}};
    const Evaluated ev = evaluate_vars(p, &ov, words);
    const expr::Result r = expr::evaluate(ast, ev.value_map(), words);
    return r.ok ? r.value : std::nan("");
  };
  const auto res = solve_chain(chain, from, value);
  if (!res) {
    out.error =
        words.t("param.cannot_changing", {{"label", label}, {"value", expr::format_number(value)}, {"name", name}});
    out.choices = behind;
    return out;
  }
  out.ok = true;
  out.var = name;
  out.from = from;
  out.to = *res;
  return out;
}

}  // namespace

Drive drive_back(Params& p, const Adapter& a, std::string_view key, double value,
                 const std::optional<std::string>& prefer, const i18n::Catalog& words) {
  Link* l = link_for(p, key);
  Drive out;
  if (l == nullptr) {
    out.ok = true;
    out.direct = true;
    return out;
  }
  const auto targets = a.targets ? a.targets() : std::vector<std::pair<std::string, Target>>{};
  const auto ti = std::ranges::find_if(targets, [key](const auto& t) { return t.first == key; });
  const std::string label = ti != targets.end() ? ti->second.label : std::string(key);
  if (ti != targets.end()) {
    if (const auto vp = value_problem(ti->second, value, words)) {
      out.error = *vp;
      return out;
    }
  }
  const expr::Parsed parsed = expr::parse(l->expr, std::nullopt, words);
  if (expr::literal(l->expr, words) || !parsed.ok || parsed.uses.empty()) {
    out.ok = true;
    out.link = std::string(key);
    out.from_text = l->expr;
    l->expr = expr::format_number(value);
    out.to_text = l->expr;
    return out;
  }
  Drive r = solve_back(p, l->expr, value, prefer, label, words);
  if (!r.ok) {
    return r;
  }
  var_by_name(p, r.var)->expr = expr::format_number(r.to);
  return r;
}

Drive drive_var(Params& p, std::string_view name, double value, const std::optional<std::string>& prefer,
                const i18n::Catalog& words) {
  Var* v = var_by_name(p, name);
  Drive out;
  if (v == nullptr) {
    out.error = words.t("param.there_no_parameter_called", {{"name", std::string(name)}});
    return out;
  }
  if (is_driving(*v, words)) {
    out.ok = true;
    out.var = std::string(name);
    out.from_text = v->expr;
    v->expr = expr::format_number(value);
    out.to_text = v->expr;
    return out;
  }
  Drive r = solve_back(p, v->expr, value, prefer, std::string(name), words);
  if (!r.ok) {
    return r;
  }
  var_by_name(p, r.var)->expr = expr::format_number(r.to);
  return r;
}

Edit set_var(Params& p, std::string_view name, std::string_view ex, const std::optional<std::string>& note,
             const std::optional<std::string>& rename, const i18n::Catalog& words) {
  Edit out;
  const expr::Parsed r = expr::parse(ex, std::nullopt, words);
  if (!r.ok) {
    out.error = r.error;
    return out;
  }
  const Params before = p;
  Var* v = var_by_name(p, rename ? std::string_view(*rename) : name);
  if (rename && *rename != name) {
    if (v == nullptr) {
      out.error = words.t("param.there_no_parameter_called_2", {{"rename", *rename}});
      return out;
    }
    if (const auto bad = name_problem(Value(std::string(name)), &p, v, words)) {
      out.error = *bad;
      return out;
    }
    for (Var& w : p.vars) {
      w.expr = expr::rename_in(w.expr, *rename, name);
    }
    for (Link& l : p.links) {
      l.expr = expr::rename_in(l.expr, *rename, name);
    }
    v->name = std::string(name);
  }
  if (v == nullptr) {
    if (const auto bad = name_problem(Value(std::string(name)), &p, nullptr, words)) {
      out.error = *bad;
      return out;
    }
    if (p.vars.size() >= Limits::vars) {
      out.error = words.t("param.part_holds_parameters_most", {{"vars", std::to_string(Limits::vars)}});
      return out;
    }
    p.vars.push_back({.name = std::string(name), .expr = std::string(ex), .note = std::nullopt});
    v = &p.vars.back();
  }
  v->expr = std::string(ex);
  if (note) {
    if (!note->empty()) {
      v->note = text(Value(*note), Limits::note);
    } else {
      v->note.reset();
    }
  }
  const Evaluated ev = evaluate_vars(p, nullptr, words);
  for (const auto& [k, msg] : ev.errors) {
    if (k == name && (msg.contains("each other") || msg.contains("itself"))) {
      out.error = msg;
      out.cycle = true;
      p = before;
      return out;
    }
  }
  out.ok = true;
  return out;
}

Uses uses_of(const Params& p, std::string_view name, const i18n::Catalog& words) {
  Uses u;
  const auto uses = [&](const std::string& src) {
    const expr::Parsed r = expr::parse(src, std::nullopt, words);
    return r.ok && std::ranges::find(r.uses, name) != r.uses.end();
  };
  for (const Var& v : p.vars) {
    if (v.name != name && uses(v.expr)) {
      u.vars.push_back(v.name);
    }
  }
  for (const Link& l : p.links) {
    if (uses(l.expr)) {
      u.links.push_back(l.target);
    }
  }
  return u;
}

Edit remove_var(Params& p, std::string_view name, bool inline_value, const i18n::Catalog& words) {
  Edit out;
  const Var* v = var_by_name(p, name);
  if (v == nullptr) {
    out.error = words.t("param.there_no_parameter_called", {{"name", std::string(name)}});
    return out;
  }
  const Uses u = uses_of(p, name, words);
  if ((!u.vars.empty() || !u.links.empty()) && !inline_value) {
    std::vector<std::string> all = u.vars;
    all.insert(all.end(), u.links.begin(), u.links.end());
    out.error = words.t("param.is_used_by", {{"name", std::string(name)}, {"v", join(all, ", ")}});
    out.used = true;
    return out;
  }
  if (inline_value) {
    const auto val = evaluate_vars(p, nullptr, words).value(name);
    if (!val) {
      out.error = words.t("param.has_no_value_write", {{"name", std::string(name)}});
      return out;
    }
    const std::string lit = "(" + expr::format_number(*val) + ")";
    for (Var& w : p.vars) {
      if (&w != v) {
        w.expr = expr::rename_in(w.expr, name, lit);
      }
    }
    for (Link& l : p.links) {
      l.expr = expr::rename_in(l.expr, name, lit);
    }
  }
  std::erase_if(p.vars, [v](const Var& w) { return &w == v; });
  out.ok = true;
  return out;
}

bool remove_link(Params& p, std::string_view key) {
  const auto it = std::ranges::find_if(p.links, [key](const Link& l) { return l.target == key; });
  if (it == p.links.end()) {
    return false;
  }
  p.links.erase(it);
  return true;
}

}  // namespace cc::cad::params
