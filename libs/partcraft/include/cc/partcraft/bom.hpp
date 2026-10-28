// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::bom — PartCraft 0.5's bom.js: part properties (number, description, material), the material presets
// (materials.js's list plus a few by density only) and an assembly's bill of materials and its CSV (RFC 4180, CRLF,
// formula-looking text cells prefixed with ', UTF-8 BOM). JS-shaped values in and out.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::bom {

inline constexpr double kMaxDensity = 25;  // g/cm³

struct Material {
  std::string name;
  double density;  // g/cm³
};
/// MATERIALS in their order, named in the catalog's language.
[[nodiscard]] std::vector<Material> materials(const i18n::Catalog& words);
/// presetOf(material): its index in MATERIALS (by name and density, else by density unless "Custom"), or -1.
[[nodiscard]] double preset_of(const base::Value& material, const i18n::Catalog& words);
/// readMaterial(m): {name, density} from untrusted input, null for none; an Error for a bad density.
[[nodiscard]] base::Result<base::Value> read_material(const base::Value& m, const i18n::Catalog& words);
/// readPartProps(p): {number?, description?, material?}.
[[nodiscard]] base::Result<base::Value> read_part_props(const base::Value& p, const i18n::Catalog& words);
/// bomRows(asm, builds): one row per part in first-use order; `stats` maps a part id to its rebuild's stats.
[[nodiscard]] base::Value bom_rows(const base::Value& assembly,
                                   const std::vector<std::pair<std::string, base::Value>>& stats);
/// csvCell(v, {text}): one CSV field.
[[nodiscard]] std::string csv_cell(const base::Value& v, bool text = true);
/// bomCsv(rows)
[[nodiscard]] std::string bom_csv(const base::Value& rows);

}  // namespace cc::partcraft::bom
