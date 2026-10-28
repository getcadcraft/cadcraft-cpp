// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's masshaul.js (the mass-haul diagram), solve-why.js (why curves do not fit, and what would) and
// staking.js (stakes along an alignment, survey points against it, the CSV; the PDF pages come with the plot port).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

// ---- masshaul.js
/// massHaul(rows, { factor }) as a JSON value { points, balance, spans, totals, factor }
[[nodiscard]] base::Value mass_haul(const base::Value& rows, const base::Value& factor);
[[nodiscard]] std::string mass_haul_csv(const base::Value& res, std::string_view units, const i18n::Catalog& words);

// ---- solve-why.js
[[nodiscard]] base::Value curve_why(const base::Value& pis, const base::Value& opts);
[[nodiscard]] double max_radius(const base::Value& pis, const base::Value& i);
[[nodiscard]] base::Value vertical_why(const base::Value& pvis);

// ---- staking.js
[[nodiscard]] std::vector<double> stake_stations(const Alignment& al, const base::Value& opts);
/// design(sta, off, x, y) and ground(x, y): elevation or nullopt
using ZAt = std::function<std::optional<double>(double, double, double, double)>;
[[nodiscard]] base::Value stake_rows(const Alignment& al, const base::Value& opts, const ZAt& design,
                                     const ZAt& ground);
[[nodiscard]] base::Value point_rows(const Alignment& al, const base::Value& points, const ZAt& design,
                                     const ZAt* ground);
/// parseOffsets(text) → [offset] | { error }
[[nodiscard]] base::Value parse_offsets(const base::Value& text);
[[nodiscard]] std::string cut_fill_text(const base::Value& cf);
[[nodiscard]] std::string offset_text(double off);
[[nodiscard]] std::string staking_csv(const base::Value& rows, const base::Value& opts, const i18n::Catalog& words);

}  // namespace cc::civil
