// SPDX-License-Identifier: Apache-2.0
// cc::cad internals: dwg-props.js (the Properties panel's fields as DWG / DXF keep them: transparency, the xdata
// CadCraft writes, the dimension variables) and the style tables of dims.js / dimstyle.js. Reading side for M4.1.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util.hpp"

namespace cc::cad::detail {

/// dims.js DIM_DEFAULTS (ISO-25-like), in its key order.
const Value& dim_defaults();
/// dimstyle.js DIM_EXTRA
const Value& dim_extra();

/// trFrom: a DXF 440 word or a cc-dwg transparency → 'ByBlock', 0–90 (%), or undefined.
Value tr_from(const Value& v);
/// propsFromXdata: xdata by application → { mat?, ox?, oy?, assoc?, tr?, url?, ann?, bg?, xd? }.
Value props_from_xdata(const Value& xd);
/// stFromVars: fill st from variables by name (a DIMSTYLE record's vars, the DWG header).
void st_from_vars(const Value& vars, Value& st);
/// stFromCodes: fill st from DXF group codes; `get(code)` → the raw value or undefined.
void st_from_codes(const std::function<Value(double)>& get, Value& st);
/// ccDimFrom: CADCRAFT "CCDIMST:<json>" strings → st keys (untrusted: only known keys and kinds).
void cc_dim_from(const std::vector<std::string>& strs, Value& st);
/// dstyleMap: an entity's ACAD xdata list → (group code → value) of its "DSTYLE" { … } overrides.
std::vector<std::pair<double, Value>> dstyle_map(const Value& items);

/// The model's xdata of one application (names compared without case) → its [[code, value]…] (an empty array
/// when absent).
const Value& xdata_of(const Value& xdata, std::string_view app_upper);
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved) — of a temporary: by value (see at())
inline Value xdata_of(Value&& xdata, std::string_view app_upper) {
  return xdata_of(static_cast<const Value&>(xdata), app_upper);
}
/// The CADCRAFT xdata's values as strings (`xdataOf(xdata, 'CADCRAFT').map(([, v]) => String(v))`).
std::vector<std::string> cadcraft_strings(const Value& xdata);

/// Math.max / Math.min (NaN wins; one of two arguments).
double js_max(double a, double b);
double js_min(double a, double b);

}  // namespace cc::cad::detail
