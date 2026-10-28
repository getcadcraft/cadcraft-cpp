// SPDX-License-Identifier: Apache-2.0
// configs.js (PartCraft 0.5, P18): configurations, design-table rows saved in the part.
#include "cc/partcraft/configs.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/expr.hpp"
#include "jsx.hpp"

namespace cc::partcraft::configs {
namespace {

using base::Value;
namespace pr = cad::params;

std::set<std::string> var_names(const Value& params) {
  std::set<std::string> s;
  const Value vars = params.is_object() ? params.get("vars") : Value{};
  if (vars.is_array()) {
    for (const Value& v : vars.as_array()) {
      const Value n = v.is_object() ? v.get("name") : Value{};
      if (n.is_string()) {
        s.insert(n.as_string());
      }
    }
  }
  return s;
}

/// A row's {name, values} with its values copied in the JS's property order.
Value copy_row(const Value& name, const Value& values) {
  return base::make_object({{"name", name}, {"values", jsx::spread(values)}});
}

base::Error err(std::string m) {
  return base::Error{base::Errc::invalid, std::move(m)};
}

}  // namespace

std::string clean_name(const Value& s) {
  return jsx::utf16_slice(std::string(base::js_trim(jsx::strip_control(jsx::text_of(s)))), kNameMax);
}

base::Result<Value> read_configs(const Value& raw, const Value& params, const i18n::Catalog& w) {
  if (raw.is_null() || raw.is_undefined()) {
    return Value(nullptr);
  }
  const Value rows_v = raw.is_object() ? raw.get("rows") : Value{};
  if (!raw.is_object() && !raw.is_array()) {
    return std::unexpected(err(w.t("configs.err.list")));
  }
  if (!rows_v.is_array() || rows_v.as_array().size() > kMaxRows) {
    return std::unexpected(err(w.t("configs.err.list")));
  }
  const std::set<std::string> names = var_names(params);
  std::set<std::string> seen;
  base::Array rows;
  for (const Value& r : rows_v.as_array()) {
    const Value rn = r.is_object() ? r.get("name") : Value{};
    const std::string name = rn.is_string() ? clean_name(rn) : std::string();
    if (name.empty() || seen.contains(base::js_to_lower(name))) {
      return std::unexpected(err(w.t("configs.err.name")));
    }
    seen.insert(base::js_to_lower(name));
    const Value vals = r.get("values");
    if (!vals.is_object()) {
      return std::unexpected(err(w.t("configs.err.values", {{"name", name}})));
    }
    Value values = Value::object();
    for (const auto& [k, v] : jsx::entries(vals)) {
      if (!names.contains(k)) {
        return std::unexpected(err(w.t("configs.err.unknown", {{"name", name}, {"param", jsx::utf16_slice(k, 40)}})));
      }
      if (!v.is_number() || !std::isfinite(v.as_number())) {
        return std::unexpected(err(w.t("configs.err.number", {{"name", name}, {"param", k}})));
      }
      values.set(k, v);
    }
    rows.push_back(base::make_object({{"name", name}, {"values", std::move(values)}}));
  }
  if (rows.empty()) {
    return Value(nullptr);
  }
  const Value act = raw.get("active");
  const bool known = act.is_string() && std::ranges::any_of(rows, [&](const Value& r) { return r.get("name") == act; });
  return base::make_object({{"rows", std::move(rows)}, {"active", known ? act : Value(nullptr)}});
}

Value row_from_current(const Value& params, const Value& name, const i18n::Catalog& w) {
  const pr::Params p = pr::from_value(params);
  const pr::Evaluated ev = pr::evaluate_vars(p, nullptr, w);
  Value values = Value::object();
  for (const pr::Var& v : p.vars) {
    const auto val = ev.value(v.name);
    if (pr::is_driving(v, w) && val && std::isfinite(*val)) {
      values.set(v.name, *val);
    }
  }
  return base::make_object({{"name", clean_name(name)}, {"values", std::move(values)}});
}

base::Result<Value> save_row(const Value& cfg, const Value& row, const i18n::Catalog& w) {
  base::Array rows;
  Value active = nullptr;
  if (cfg.truthy()) {
    for (const Value& r : cfg.get("rows").as_array()) {
      rows.push_back(copy_row(r.get("name"), r.get("values")));
    }
    active = cfg.get("active");
  }
  const std::string want = base::js_to_lower(base::js_string(row.get("name")));
  const auto it = std::ranges::find_if(
      rows, [&](const Value& r) { return base::js_to_lower(base::js_string(r.get("name"))) == want; });
  if (it != rows.end()) {
    *it = copy_row(it->get("name"), row.get("values"));
  } else {
    if (rows.size() >= kMaxRows) {
      return std::unexpected(err(w.t("configs.err.many", {{"n", std::to_string(kMaxRows)}})));
    }
    rows.push_back(copy_row(row.get("name"), row.get("values")));
  }
  return base::make_object({{"rows", std::move(rows)}, {"active", active}});
}

base::Result<Value> apply_row(Value params, const Value& row, const i18n::Catalog& w) {
  Value* vars = params.is_object() ? params.find("vars") : nullptr;
  for (const auto& [k, v] : jsx::entries(row.get("values"))) {
    Value* hit = nullptr;
    if (vars != nullptr && vars->is_array()) {
      for (Value& x : vars->as_array()) {
        if (x.is_object() && x.get("name") == Value(k)) {
          hit = &x;
          break;
        }
      }
    }
    if (hit == nullptr) {
      return std::unexpected(
          err(w.t("configs.err.missing", {{"name", base::js_string(row.get("name"))}, {"param", k}})));
    }
    hit->set("expr", v.is_number() ? cad::expr::format_number(v.as_number()) : base::js_string(v));
  }
  return params;
}

bool row_matches(const Value& params, const Value& row, const i18n::Catalog& w) {
  const pr::Evaluated ev = pr::evaluate_vars(pr::from_value(params), nullptr, w);
  return std::ranges::all_of(jsx::entries(row.get("values")), [&](const auto& kv) {
    const auto c = ev.value(kv.first);
    const double want = base::js_to_number(kv.second);
    return c && std::isfinite(*c) && std::abs(*c - want) <= 1e-12 * jsx::max(1, std::abs(want));
  });
}

base::Result<Value> rename_row(const Value& cfg, const Value& from, const Value& to, const i18n::Catalog& w) {
  const std::string name = clean_name(to);
  if (name.empty()) {
    return std::unexpected(err(w.t("configs.err.name")));
  }
  const std::string low = base::js_to_lower(name);
  for (const Value& r : cfg.get("rows").as_array()) {
    if (!(r.get("name") == from) && base::js_to_lower(base::js_string(r.get("name"))) == low) {
      return std::unexpected(err(w.t("configs.err.taken", {{"name", name}})));
    }
  }
  base::Array rows;
  for (const Value& r : cfg.get("rows").as_array()) {
    rows.push_back(copy_row(r.get("name") == from ? Value(name) : r.get("name"), r.get("values")));
  }
  return base::make_object(
      {{"rows", std::move(rows)}, {"active", cfg.get("active") == from ? Value(name) : cfg.get("active")}});
}

Value delete_row(const Value& cfg, const Value& name) {
  base::Array rows;
  for (const Value& r : cfg.get("rows").as_array()) {
    if (!(r.get("name") == name)) {
      rows.push_back(copy_row(r.get("name"), r.get("values")));
    }
  }
  if (rows.empty()) {
    return nullptr;
  }
  return base::make_object(
      {{"rows", std::move(rows)}, {"active", cfg.get("active") == name ? Value(nullptr) : cfg.get("active")}});
}

Value follow_params(const Value& cfg, const Value& before, const Value& after) {
  if (!cfg.truthy()) {
    return nullptr;
  }
  const auto vars_of = [](const Value& p) {
    const Value v = p.is_object() ? p.get("vars") : Value{};
    return v.truthy() && v.is_array() ? v.as_array() : base::Array{};
  };
  const base::Array b = vars_of(before);
  const base::Array a = vars_of(after);
  std::vector<std::pair<std::string, Value>> ren;
  if (b.size() == a.size()) {
    for (std::size_t i = 0; i < b.size(); ++i) {
      if (!(b[i].get("name") == a[i].get("name"))) {
        const std::string k = base::js_string(b[i].get("name"));
        const auto it = std::ranges::find_if(ren, [&](const auto& kv) { return kv.first == k; });
        if (it != ren.end()) {
          it->second = a[i].get("name");
        } else {
          ren.emplace_back(k, a[i].get("name"));
        }
      }
    }
  }
  const std::set<std::string> names = var_names(after);
  base::Array rows;
  for (const Value& r : cfg.get("rows").as_array()) {
    Value values = Value::object();
    for (const auto& [k, v] : jsx::entries(r.get("values"))) {
      std::string n = k;
      const auto it = std::ranges::find_if(ren, [&](const auto& kv) { return kv.first == k; });
      if (it != ren.end() && it->second.truthy()) {
        n = base::js_string(it->second);
      }
      if (names.contains(n)) {
        values.set(n, v);
      }
    }
    rows.push_back(base::make_object({{"name", r.get("name")}, {"values", std::move(values)}}));
  }
  return base::make_object({{"rows", std::move(rows)}, {"active", cfg.get("active")}});
}

base::Result<Value> rows_from_table(std::string_view csv, const Value& params, const i18n::Catalog& w) {
  const pr::DesignTable r = pr::read_design_table(csv, pr::from_value(params), w);
  if (!r.thrown.empty()) {
    return std::unexpected(err(r.thrown));
  }
  if (!r.ok) {
    return std::unexpected(err(r.error));
  }
  base::Array rows;
  for (const auto& row : r.rows) {
    Value values = Value::object();
    for (const auto& [k, v] : row.overrides) {
      values.set(k, v);
    }
    rows.push_back(base::make_object({{"name", clean_name(Value(row.name))}, {"values", std::move(values)}}));
  }
  return Value(std::move(rows));
}

std::string configs_csv(const Value& cfg) {
  std::vector<std::string> cols;
  const Value rows = cfg.truthy() ? cfg.get("rows") : Value{};
  const base::Array none;
  const base::Array& list = rows.is_array() ? rows.as_array() : none;
  for (const Value& r : list) {
    for (const auto& [k, v] : jsx::entries(r.get("values"))) {
      if (std::ranges::find(cols, k) == cols.end()) {
        cols.push_back(k);
      }
    }
  }
  const auto cell = [](const std::string& s) {
    std::string x = s;
    const bool formula = !x.empty() && (x[0] == '=' || x[0] == '+' || x[0] == '-' || x[0] == '@');
    const bool number =
        !x.empty() && ((x[0] >= '0' && x[0] <= '9') || (x[0] == '-' && x.size() > 1 && x[1] >= '0' && x[1] <= '9'));
    if (formula && !number) {
      x = "'" + x;
    }
    if (x.find_first_of("\",;\r\n") == std::string::npos) {
      return x;
    }
    std::string q = "\"";
    for (const char c : x) {
      q += c;
      if (c == '"') {
        q += '"';
      }
    }
    return q + "\"";
  };
  std::string out;
  const auto line = [&](const std::vector<std::string>& cells) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      out += cell(cells[i]);
    }
    out += "\r\n";
  };
  std::vector<std::string> head{"name"};
  head.insert(head.end(), cols.begin(), cols.end());
  line(head);
  for (const Value& r : list) {
    std::vector<std::string> cells{base::js_string(r.get("name"))};
    const Value values = r.get("values");
    for (const std::string& c : cols) {
      const Value v = jsx::get(values, c);
      if (v.is_undefined()) {
        cells.emplace_back();
      } else {
        cells.push_back(v.is_number() ? cad::expr::format_number(v.as_number()) : base::js_string(v));
      }
    }
    line(cells);
  }
  return out;
}

}  // namespace cc::partcraft::configs
