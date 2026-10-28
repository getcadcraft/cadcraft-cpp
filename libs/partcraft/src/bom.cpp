// SPDX-License-Identifier: Apache-2.0
// bom.js (PartCraft 0.5): part properties, materials and an assembly's bill of materials.
#include "cc/partcraft/bom.hpp"

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "cc/base/js.hpp"
#include "jsx.hpp"

namespace cc::partcraft::bom {
namespace {

using base::Value;

struct Preset {
  std::string_view key;
  double density;  // g/cm³
};
// materials.js's list (kg/m³ / 1000) first, then the ones only the bill of materials needs
constexpr std::array<Preset, 13> kPresets = {{
    {"study.mat.steel", 7850.0 / 1000},
    {"study.mat.ss304", 8000.0 / 1000},
    {"study.mat.al6061", 2700.0 / 1000},
    {"study.mat.ti64", 4430.0 / 1000},
    {"study.mat.abs", 1050.0 / 1000},
    {"study.mat.pla", 1240.0 / 1000},
    {"study.mat.nylon", 1140.0 / 1000},
    {"xch.bom.mat.cast_iron", 7.2},
    {"xch.bom.mat.brass", 8.5},
    {"xch.bom.mat.copper", 8.96},
    {"xch.bom.mat.acetal", 1.41},
    {"xch.bom.mat.polycarbonate", 1.2},
    {"xch.bom.mat.oak", 0.75},
}};

/// String(s ?? '') with control characters out, trimmed, cut to `max` UTF-16 units.
std::string clean(const Value& s, std::size_t max) {
  return jsx::utf16_slice(base::js_trim(jsx::strip_control(jsx::text_of(s))), max);
}

base::Error err(std::string m) {
  return base::Error{base::Errc::invalid, std::move(m)};
}

/// String(Number(v.toFixed(3))), '' for null.
std::string n3(const Value& v) {
  if (v.is_null()) {
    return {};
  }
  return base::js_string(Value(base::js_number(base::js_to_fixed(v.as_number(), 3))));
}

}  // namespace

std::vector<Material> materials(const i18n::Catalog& w) {
  std::vector<Material> out;
  out.reserve(kPresets.size());
  for (const auto& p : kPresets) {
    out.push_back({w.t(p.key), p.density});
  }
  return out;
}

double preset_of(const Value& material, const i18n::Catalog& w) {
  if (!material.truthy()) {
    return -1;
  }
  const std::vector<Material> list = materials(w);
  const Value name = jsx::get(material, "name");
  const Value density = jsx::get(material, "density");
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (name == Value(list[i].name) && density == Value(list[i].density)) {
      return static_cast<double>(i);
    }
  }
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (density == Value(list[i].density) && !(name == Value("Custom"))) {
      return static_cast<double>(i);
    }
  }
  return -1;
}

base::Result<Value> read_material(const Value& m, const i18n::Catalog& w) {
  if (m.is_null() || m.is_undefined()) {
    return Value(nullptr);
  }
  if (!m.is_object() && !m.is_array()) {
    return std::unexpected(err(w.t("xch.bom.bad_material")));
  }
  const Value d = jsx::get(m, "density");
  if (!d.is_number() || !std::isfinite(d.as_number()) || d.as_number() <= 0 || d.as_number() > kMaxDensity) {
    return std::unexpected(err(w.t("xch.bom.bad_density", {{"max", "25"}})));
  }
  std::string name = clean(jsx::get(m, "name"), 60);
  if (name.empty()) {
    name = "Custom";
  }
  return base::make_object({{"name", name}, {"density", d}});
}

base::Result<Value> read_part_props(const Value& p, const i18n::Catalog& w) {
  Value out = Value::object();
  if (!p.truthy() || (!p.is_object() && !p.is_array())) {
    return out;
  }
  for (const auto& [key, max] : {std::pair<std::string_view, std::size_t>{"number", 40}, {"description", 200}}) {
    const Value v = jsx::get(p, key);
    if (!v.is_undefined()) {
      std::string c = clean(v, max);
      if (!c.empty()) {
        out.append(key, std::move(c));
      }
    }
  }
  auto m = read_material(jsx::get(p, "material"), w);
  if (!m) {
    return std::unexpected(m.error());
  }
  if (m->truthy()) {
    out.append("material", std::move(*m));
  }
  return out;
}

Value bom_rows(const Value& assembly, const std::vector<std::pair<std::string, Value>>& stats) {
  base::Array rows;
  std::map<std::string, std::size_t> by_pid;
  const Value parts = assembly.get("parts");
  for (const Value& inst : assembly.get("instances").as_array()) {
    const std::string pid = base::js_string(inst.get("part"));
    auto it = by_pid.find(pid);
    if (it == by_pid.end()) {
      const Value p = parts.get(pid);
      Value st;
      for (const auto& [k, s] : stats) {
        if (k == pid) {
          st = s;
        }
      }
      const Value volume = st.is_object() ? st.get("volume") : Value(nullptr);
      const Value material = p.get("material");
      const Value density = material.truthy() ? material.get("density") : Value(nullptr);
      Value mass_each = nullptr;
      if (!volume.is_null() && !density.is_null()) {
        mass_each = base::js_to_number(volume) * base::js_to_number(density) / 1000;
      }
      const Value number = p.get("number");
      const Value description = p.get("description");
      rows.push_back(base::make_object({{"item", static_cast<double>(rows.size() + 1)},
                                        {"pid", pid},
                                        {"qty", 0},
                                        {"number", number.truthy() ? number : Value("")},
                                        {"name", p.get("name")},
                                        {"description", description.truthy() ? description : Value("")},
                                        {"material", material.truthy() ? material.get("name") : Value("")},
                                        {"density", density},
                                        {"volume", volume},
                                        {"massEach", mass_each},
                                        {"massTotal", nullptr},
                                        {"exact", st.is_object() && st.get("exact").truthy()}}));
      it = by_pid.emplace(pid, rows.size() - 1).first;
    }
    Value& r = rows[it->second];
    r.set("qty", r.get("qty").as_number() + 1);
  }
  for (Value& r : rows) {
    const Value each = r.get("massEach");
    r.set("massTotal", each.is_null() ? Value(nullptr) : Value(each.as_number() * r.get("qty").as_number()));
  }
  return rows;
}

std::string csv_cell(const Value& v, bool text) {
  std::string s = v.is_null() || v.is_undefined() ? std::string() : base::js_string(v);
  if (text && !s.empty() &&
      (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r')) {
    s = "'" + s;
  }
  const bool edge = !s.empty() && base::js_trim(s).size() != s.size();
  if (s.find_first_of("\",\r\n") == std::string::npos && !edge) {
    return s;
  }
  std::string q = "\"";
  for (const char c : s) {
    q += c;
    if (c == '"') {
      q += '"';
    }
  }
  return q + "\"";
}

std::string bom_csv(const Value& rows) {
  static constexpr std::array<std::string_view, 10> kHeader = {
      "Item",     "Quantity",        "Part number",       "Name",          "Description",
      "Material", "Density (g/cm³)", "Volume each (mm³)", "Mass each (g)", "Mass total (g)"};
  std::string out = "\xEF\xBB\xBF";
  const auto join = [](const std::vector<std::string>& cells) {
    std::string l;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) {
        l += ',';
      }
      l += cells[i];
    }
    return l;
  };
  std::vector<std::string> head;
  head.reserve(kHeader.size());
  for (const std::string_view h : kHeader) {
    head.push_back(csv_cell(Value(h), true));
  }
  out += join(head);
  double qty = 0;
  bool all = true;
  double total = 0;
  const auto& list = rows.as_array();
  for (const Value& r : list) {
    const Value density = r.get("density");
    out += "\r\n";
    out += join({csv_cell(r.get("item"), false), csv_cell(r.get("qty"), false), csv_cell(r.get("number"), true),
                 csv_cell(r.get("name"), true), csv_cell(r.get("description"), true), csv_cell(r.get("material"), true),
                 csv_cell(density.is_null() || density.is_undefined() ? Value("") : density, false),
                 csv_cell(Value(n3(r.get("volume"))), false), csv_cell(Value(n3(r.get("massEach"))), false),
                 csv_cell(Value(n3(r.get("massTotal"))), false)});
    qty += base::js_to_number(r.get("qty"));
    if (r.get("massTotal").is_null()) {
      all = false;
    } else {
      total += r.get("massTotal").as_number();
    }
  }
  const Value tot = all && !list.empty() ? Value(total) : Value(nullptr);
  out += "\r\n";
  out += join({"", csv_cell(Value(qty), false), "", csv_cell(Value("Total"), true), "", "", "", "", "",
               csv_cell(Value(n3(tot)), false)});
  out += "\r\n";
  return out;
}

}  // namespace cc::partcraft::bom
