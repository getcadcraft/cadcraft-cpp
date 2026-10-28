// SPDX-License-Identifier: Apache-2.0
// The driver's expr.* and params.* ops: expr.js and params.js (libs/cad's ports, shared with CadCraft).
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cad/params.hpp"
#include "ops.hpp"

namespace cc::partcraft::ops {
namespace {

namespace ex = cad::expr;
namespace pr = cad::params;
using base::Value;

/// String(src ?? '')
std::string src_text(const Value& v) {
  return v.is_undefined() || v.is_null() ? std::string() : base::js_string(v);
}

/// A byte offset into `src` (always on a code point) as the JS's UTF-16 index.
double utf16_pos(std::string_view src, std::size_t byte) {
  return static_cast<double>(base::utf16_length(src.substr(0, std::min(byte, src.size()))));
}
Value pos_value(std::string_view src, const std::optional<std::size_t>& pos) {
  return pos ? Value(utf16_pos(src, *pos)) : Value(nullptr);
}

/// The JS's AST objects: {k, v, name?} / {k, name, pos} / {k, a} / {k, op, a, b, pos?} / {k, name, args, pos}.
// NOLINTNEXTLINE(misc-no-recursion) — bounded by the parser's depth cap (100)
Value ast_value(const ex::Node& n, std::string_view src) {
  using K = ex::Node::Kind;
  switch (n.kind) {
    case K::num: {
      Value o = base::make_object({{"k", "num"}, {"v", n.v}});
      if (!n.name.empty()) {
        o.append("name", n.name);
      }
      return o;
    }
    case K::var: return base::make_object({{"k", "var"}, {"name", n.name}, {"pos", utf16_pos(src, n.pos)}});
    case K::neg: return base::make_object({{"k", "neg"}, {"a", ast_value(*n.args[0], src)}});
    case K::cmp:
      return base::make_object(
          {{"k", "cmp"}, {"op", n.op}, {"a", ast_value(*n.args[0], src)}, {"b", ast_value(*n.args[1], src)}});
    case K::bin: {
      Value o = base::make_object(
          {{"k", "bin"}, {"op", n.op}, {"a", ast_value(*n.args[0], src)}, {"b", ast_value(*n.args[1], src)}});
      if (n.op != "+" && n.op != "-") {
        o.append("pos", utf16_pos(src, n.pos));
      }
      return o;
    }
    case K::call: {
      base::Array args;
      for (const auto& a : n.args) {
        args.push_back(ast_value(*a, src));
      }
      return base::make_object(
          {{"k", "call"}, {"name", n.name}, {"args", std::move(args)}, {"pos", utf16_pos(src, n.pos)}});
    }
  }
  return {};
}

Value strings(const std::vector<std::string>& v) {
  base::Array a;
  a.reserve(v.size());
  for (const auto& s : v) {
    a.emplace_back(s);
  }
  return a;
}

/// An object's own number members (what evaluate's typeof check lets through), finite or not.
std::map<std::string, double> values_of(const Value& v) {
  std::map<std::string, double> out;
  if (v.is_object()) {
    for (const base::Member* m : base::js_members(v.is_point() ? Value(v).as_object() : v.as_object())) {
      if (m->value.is_number() && std::isfinite(m->value.as_number())) {
        out[m->key] = m->value.as_number();
      }
    }
  }
  return out;
}

/// The driver's num(): "$NaN" / "$Inf" / "$-Inf" stand for those numbers.
Value num(const Value& v) {
  if (v.is_string()) {
    const std::string& s = v.as_string();
    if (s == "$NaN") {
      return std::numeric_limits<double>::quiet_NaN();
    }
    if (s == "$Inf") {
      return std::numeric_limits<double>::infinity();
    }
    if (s == "$-Inf") {
      return -std::numeric_limits<double>::infinity();
    }
  }
  return v;
}

/// The driver's omap(): an object's members as overrides (num() on each value; non-numbers stay as JS would see
/// them: a Map value that is not a number makes evaluate fail, here NaN).
std::vector<std::pair<std::string, double>> omap(const Value& v) {
  std::vector<std::pair<std::string, double>> out;
  if (v.is_object() && !v.is_point()) {
    for (const base::Member* m : base::js_members(v.as_object())) {
      const Value n = num(m->value);
      out.emplace_back(m->key, n.is_number() ? n.as_number() : std::numeric_limits<double>::quiet_NaN());
    }
  }
  return out;
}

Value evaluated_value(const pr::Evaluated& ev) {
  Value values = Value::object();
  for (const auto& [k, v] : ev.values) {
    values.set(k, v);
  }
  Value errors = Value::object();
  for (const auto& [k, m] : ev.errors) {
    errors.set(k, m);
  }
  Value uses = Value::object();
  for (const std::string& n : ev.order) {
    const auto it = ev.uses.find(n);
    if (it != ev.uses.end()) {
      uses.set(n, strings(it->second));
    }
  }
  for (const auto& [k, u] : ev.uses) {
    if (!uses.has(k)) {
      uses.set(k, strings(u));
    }
  }
  return base::make_object(
      {{"values", std::move(values)}, {"errors", std::move(errors)}, {"order", strings(ev.order)}, {"uses", uses}});
}

Value edit_value(const pr::Edit& e) {
  if (e.ok) {
    return base::make_object({{"ok", true}});
  }
  Value o = base::make_object({{"ok", false}, {"error", e.error}});
  if (e.cycle) {
    o.append("cycle", true);
  }
  return o;
}

}  // namespace

void add_params(Table& t, const i18n::Catalog& words) {
  const i18n::Catalog* w = &words;
  // ---- expr.js
  t["expr.parse"] = [w](Args a) {
    const std::string src = src_text(arg(a, 0));
    std::optional<std::set<std::string>> names;
    if (arg(a, 1).is_array()) {
      names.emplace();
      for (const Value& n : arg(a, 1).as_array()) {
        if (n.is_string()) {
          names->insert(n.as_string());
        }
      }
    }
    const ex::Parsed r = ex::parse(src, names, *w);
    if (!r.ok) {
      return base::make_object({{"ok", false}, {"error", r.error}, {"pos", pos_value(src, r.pos)}});
    }
    return base::make_object({{"ok", true}, {"ast", ast_value(*r.ast, src)}, {"uses", strings(r.uses)}});
  };
  t["expr.compute"] = [w](Args a) {
    const std::string src = src_text(arg(a, 0));
    const ex::Result r = ex::compute(src, values_of(arg(a, 1)), *w);
    if (!r.ok) {
      return base::make_object({{"ok", false}, {"error", r.error}, {"pos", pos_value(src, r.pos)}});
    }
    return base::make_object({{"ok", true}, {"value", r.value}, {"uses", strings(r.uses)}});
  };
  t["expr.literal"] = [w](Args a) {
    const auto v = ex::literal(src_text(arg(a, 0)), *w);
    return v ? Value(*v) : Value(nullptr);
  };
  t["expr.formatNumber"] = [](Args a) {
    const Value v = num(arg(a, 0));
    return Value(v.is_number() ? ex::format_number(v.as_number()) : base::js_string(v));
  };
  t["expr.solveFor"] = [w](Args a) {
    const std::string src = src_text(arg(a, 0));
    const ex::Parsed p = ex::parse(src, std::nullopt, *w);
    if (!p.ok) {
      return base::make_object({{"ok", false}, {"error", p.error}, {"pos", pos_value(src, p.pos)}});
    }
    const Value target = num(arg(a, 3));
    const ex::Solved r = ex::solve_for(p.ast, values_of(arg(a, 1)), base::js_string(arg(a, 2)),
                                       target.is_number() ? target.as_number() : base::js_to_number(target), *w);
    if (!r.ok) {
      return base::make_object({{"ok", false}, {"error", r.error}});
    }
    return base::make_object({{"ok", true}, {"value", r.value}});
  };
  t["expr.renameIn"] = [](Args a) {
    const std::string src = base::js_string(arg(a, 0));
    if (!arg(a, 1).is_string()) {
      return Value(src);
    }
    return Value(ex::rename_in(src, arg(a, 1).as_string(), base::js_string(arg(a, 2))));
  };
  // ---- params.js
  t["params.read"] = [w](Args a) {
    std::set<std::string> keys;
    const bool has_keys = arg(a, 1).is_array();
    if (has_keys) {
      for (const Value& k : arg(a, 1).as_array()) {
        if (k.is_string()) {
          keys.insert(k.as_string());
        }
      }
    }
    auto [p, error] = pr::read_params(arg(a, 0), has_keys ? &keys : nullptr, *w);
    if (!p) {
      throw JsError(error);
    }
    return pr::to_value(*p);
  };
  t["params.evaluateVars"] = [w](Args a) {
    const pr::Params p = pr::from_value(arg(a, 0));
    if (arg(a, 1).is_object()) {
      const auto ov = omap(arg(a, 1));
      return evaluated_value(pr::evaluate_vars(p, &ov, *w));
    }
    return evaluated_value(pr::evaluate_vars(p, nullptr, *w));
  };
  t["params.setVar"] = [w](Args a) {
    pr::Params p = pr::from_value(arg(a, 0));
    const Value opts = arg(a, 3);
    std::optional<std::string> note;
    std::optional<std::string> rename;
    if (opts.is_object()) {
      const Value n = opts.get("note");
      if (!n.is_undefined()) {
        note = n.truthy() ? base::js_string(n) : std::string();
      }
      const Value r = opts.get("rename");
      if (!r.is_undefined()) {
        rename = base::js_string(r);
      }
    }
    const pr::Edit e = pr::set_var(p, base::js_string(arg(a, 1)), src_text(arg(a, 2)), note, rename, *w);
    return base::make_object({{"res", edit_value(e)}, {"p", pr::to_value(p)}});
  };
  t["params.removeVar"] = [w](Args a) {
    pr::Params p = pr::from_value(arg(a, 0));
    const std::string name = base::js_string(arg(a, 1));
    const pr::Uses u = pr::uses_of(p, name, *w);
    const pr::Edit e = pr::remove_var(p, name, arg(a, 2).truthy(), *w);
    Value res = edit_value(e);
    if (e.used) {
      res.append("uses", base::make_object({{"vars", strings(u.vars)}, {"links", strings(u.links)}}));
    }
    return base::make_object({{"res", std::move(res)}, {"p", pr::to_value(p)}});
  };
  t["params.driveVar"] = [w](Args a) {
    pr::Params p = pr::from_value(arg(a, 0));
    const std::string name = base::js_string(arg(a, 1));
    const pr::Var* var = pr::var_by_name(p, name);
    const bool found = var != nullptr;
    const bool driving = found && pr::is_driving(*var, *w);
    const pr::Evaluated before = pr::evaluate_vars(p, nullptr, *w);
    const Value v = num(arg(a, 2));
    std::optional<std::string> prefer;
    if (arg(a, 3).truthy()) {
      prefer = base::js_string(arg(a, 3));
    }
    const pr::Drive r = pr::drive_var(p, name, v.is_number() ? v.as_number() : base::js_to_number(v), prefer, *w);
    Value res;
    if (!r.ok) {
      res = base::make_object({{"ok", false}, {"error", r.error}});
      if (r.has_choices) {  // solveBack's refusals name the driving variables to pick from
        res.append("choices", strings(r.choices));
      }
    } else if (driving) {
      res = base::make_object({{"ok", true}, {"var", r.var}, {"from", r.from_text}, {"to", r.to_text}});
    } else {
      const auto from = before.value(r.var);
      res = base::make_object({{"ok", true}, {"var", r.var}, {"from", from ? Value(*from) : Value()}, {"to", r.to}});
    }
    return base::make_object({{"res", std::move(res)}, {"p", pr::to_value(p)}});
  };
  t["params.drivingBehind"] = [w](Args a) {
    return strings(pr::driving_behind(pr::from_value(arg(a, 0)), src_text(arg(a, 1)), *w));
  };
  t["params.usesOf"] = [w](Args a) {
    const pr::Uses u = pr::uses_of(pr::from_value(arg(a, 0)), base::js_string(arg(a, 1)), *w);
    return base::make_object({{"vars", strings(u.vars)}, {"links", strings(u.links)}});
  };
  t["params.readDesignTable"] = [w](Args a) {
    const pr::DesignTable r = pr::read_design_table(base::js_string(arg(a, 0)), pr::from_value(arg(a, 1)), *w);
    if (!r.thrown.empty()) {
      throw JsError(r.thrown);
    }
    if (!r.ok) {
      return base::make_object({{"ok", false}, {"error", r.error}});
    }
    base::Array rows;
    for (const auto& row : r.rows) {
      Value ov = Value::object();
      for (const auto& [k, v] : row.overrides) {
        ov.set(k, v);
      }
      rows.push_back(base::make_object({{"name", row.name}, {"overrides", std::move(ov)}}));
    }
    return base::make_object({{"ok", true}, {"rows", std::move(rows)}, {"columns", strings(r.columns)}});
  };
  t["params.applyOverrides"] = [w](Args a) {
    pr::Params p = pr::from_value(arg(a, 0));
    if (auto e = pr::apply_overrides(p, omap(arg(a, 1)), *w)) {
      throw JsError(*e);
    }
    return pr::to_value(p);
  };
}

}  // namespace cc::partcraft::ops
