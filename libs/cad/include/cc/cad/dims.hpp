// SPDX-License-Identifier: Apache-2.0
// cc::cad — CadCraft 0.5's dimensions: dims.js (what a dimension measures and the parts it draws), dimstyle.js (the
// style applied to those parts: arrowheads, colours, tolerances, text background) and units.js formatLength. Ported
// with the JS's order of operations: a saved file carries these doubles and strings unchanged.
//
//   { type: 'dim', kind: 'linear' | 'aligned' | 'angular' | 'radius' | 'diameter', p1, p2, c, loc, rot, text, st,
//   block? }
#pragma once

#include <optional>
#include <string>

#include "cc/base/value.hpp"

namespace cc::cad {

/// { ...DIM_DEFAULTS, ...(e.st || {}) }: the style a dimension is drawn and saved with.
[[nodiscard]] base::Value dim_style(const base::Value& e);
/// dims.js dimMeasure(e): drawing units, radians for angular.
[[nodiscard]] double dim_measure(const base::Value& e);
/// dimstyle.js dimValue(e, v).main: the text "<>" stands for.
[[nodiscard]] std::string dim_value_main(const base::Value& e, double v);
/// dims.js dimText(e): what the dimension's text says.
[[nodiscard]] std::string dim_text(const base::Value& e);
/// dims.js dimGeometry(e): lines, arcs, arrowheads (solid hatches) and one text in world coordinates, on layer 0 with
/// colour by block, with the dimension's style applied (dimstyle.js styleDimParts).
[[nodiscard]] base::Array dim_geometry(const base::Value& e);
/// dims.js dimTextPoint(e): the middle of the text (DXF group 11), else e.loc.
[[nodiscard]] base::Value dim_text_point(const base::Value& e);

/// units.js formatLength(v, { lunits, prec }): a length as text in a format (1 scientific, 2 decimal, 3 engineering,
/// 4 architectural, 5 fractional); prec null = the format's default (4).
[[nodiscard]] std::string format_length(double v, const base::Value& lunits, const base::Value& prec = base::Value());

/// propdefs.js dst(e): { ...DIM_DEFAULTS, ...DIM_EXTRA, ...e.st } — every style key a dimension has.
[[nodiscard]] base::Value dim_style_full(const base::Value& e);
/// dimstyle.js dimStyleText(e, v): the text with the override, tolerances added.
[[nodiscard]] std::string dim_style_text(const base::Value& e, double v);

/// units.js lunitCode(v): a length format code 1 … 5 (anything else: 2, decimal).
[[nodiscard]] int length_code(const base::Value& lunits);
/// units.js precOf(code, prec): the precision a format uses (prec rounded into 0 … 8, else 4).
[[nodiscard]] int length_prec(const base::Value& prec);
/// units.js decimals(v, prec): Number(v.toFixed(prec)) as text, a value under half the last digit as 0.
[[nodiscard]] std::string format_decimals(double v, int prec);

}  // namespace cc::cad
