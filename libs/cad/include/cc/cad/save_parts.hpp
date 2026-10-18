// SPDX-License-Identifier: Apache-2.0
// cc::cad — the pieces of CadCraft 0.5's modules that saving a drawing needs (dwg-save.js and, next, dxf.js): the
// write side of dwg-props.js (transparency words, the xdata for fields without a group code, the dimension variables
// and their DSTYLE overrides), doc.js colours, dxf.js unit codes, hatch.js patterns, dwg-vports.js records,
// pictures.js placements and xclip.js filters. Ported function for function.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "cc/base/value.hpp"

namespace cc::cad {

// ---- dwg-props.js
/// trWord(tr): 'ByBlock' → 0x01000000, 0–90 % → 0x02000000 | alpha; nullopt = by layer (undefined).
[[nodiscard]] std::optional<double> tr_word(const base::Value& tr);
/// propsXdata(e, { r12 }): { CADCRAFT?, PE_URL?, AcadAnnotative?, HATCHBACKGROUNDCOLOR?, …e.xd } for e's fields that
/// have no group code.
[[nodiscard]] base::Value props_xdata(const base::Value& e, bool r12);
/// varsOf(st): { DIMxxx: value } for a DIMSTYLE record (every variable).
[[nodiscard]] base::Value vars_of(const base::Value& st);
/// dstyleItems(st, { r12 }): the ACAD "DSTYLE" override items [[code, value]…].
[[nodiscard]] base::Array dstyle_items(const base::Value& st, bool r12);
/// dimstylePairs(st, { r12 }): the DIMSTYLE table's [[group code, value]…] (the DSTYLE items' pairs).
[[nodiscard]] base::Array dimstyle_pairs(const base::Value& st, bool r12);
/// stFromVars(vars, {}): a style from variables by name (a DIMSTYLE record's vars).
[[nodiscard]] base::Value st_from_vars(const base::Value& vars);
/// ccDimItems(st, { r12 }): CADCRAFT "CCDIMST:<json>" items for what no variable holds exactly ([] when none).
[[nodiscard]] base::Array cc_dim_items(const base::Value& st, bool r12);

// ---- doc.js colours, dxf.js units
/// aciHex(i): '#rrggbb' for an ACI number, 'ink' for 7 (and anything outside 1–255).
[[nodiscard]] std::string aci_hex(double i);
/// nearestAci(hex): the nearest of ACI 1–255 for '#rrggbb' (7 when nothing is nearer than infinity).
[[nodiscard]] int nearest_aci(std::string_view hex);
/// dxf.js unitCode(name): the $INSUNITS code of a unit name ('' and unknown → 0).
[[nodiscard]] int unit_code(std::string_view units);

// ---- hatch.js
/// PATTERNS[NAME] → { lines: [{ angle, x, y, dx, dy, dashes }] } (millimetres), or nullptr.
[[nodiscard]] const base::Value* hatch_pattern(std::string_view upper_name);
/// hatchIsMetric(e, doc): e.piso when a boolean, else not inches / feet / an imperial length format.
[[nodiscard]] bool hatch_is_metric(const base::Value& e, const base::Value& meta);

// ---- dwg-vports.js
/// vportRecords(vports, fallback, twist): the VPORT records to write, the active pane first.
[[nodiscard]] base::Array vport_records(const base::Value& vports, const base::Value& fallback, double twist);

// ---- pictures.js, xclip.js, xref.js
/// pictureKey(file): the base name, lower case.
[[nodiscard]] std::string picture_key(const base::Value& file);
/// picturePlacement(ref, block): { file, insert, u, v, size, fade, clip? } or null when the block is not a picture.
[[nodiscard]] base::Value picture_placement(const base::Value& ref, const base::Value& block);
/// clipPoly(ref): the reference's usable clip polygon (≥ 3 finite points) or null.
[[nodiscard]] base::Value clip_poly(const base::Value& ref);
/// filterOf({ pts }): the SPATIAL_FILTER fields for a clip.
[[nodiscard]] base::Value filter_of(const base::Value& pts);
/// isXrefRoot(b): a linked drawing's own block (not one of its parts).
[[nodiscard]] bool is_xref_root(const base::Value& b);

}  // namespace cc::cad
