// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's parcels.js (deed calls, courses, areas, legal descriptions, plan labels) and traverse.js (a traverse
// from angles or bearings, its closure and the compass adjustment). The JS reads deed calls and traverse lines with
// regular expressions; here each expression is matched by hand, with the same backtracking where it matters.
#pragma once

#include <optional>
#include <string>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

[[nodiscard]] base::Value clean_parcel(const base::Value& o);
[[nodiscard]] std::optional<double> parse_angle(const base::Value& x);
[[nodiscard]] std::optional<double> parse_bearing(const base::Value& s);
[[nodiscard]] base::Value parse_calls(const base::Value& text, const i18n::Catalog& words);
[[nodiscard]] base::Value run_calls(const base::Value& start, const base::Value& calls);
[[nodiscard]] base::Value courses(const base::Value& pts);
[[nodiscard]] double parcel_area(const base::Value& pts);
[[nodiscard]] double perimeter(const base::Value& pts);
[[nodiscard]] base::Value centroid(const base::Value& pts);
[[nodiscard]] std::string area_words(double a, std::string_view units, const i18n::Catalog& words);
[[nodiscard]] std::string legal_description(const base::Value& parcel, const base::Value& opts,
                                            const i18n::Catalog& words);
[[nodiscard]] base::Value parcel_labels(const base::Value& parcel, const base::Value& opts);

[[nodiscard]] base::Value parse_traverse(const base::Value& text, std::string_view mode, const i18n::Catalog& words);
[[nodiscard]] base::Value run_traverse(const base::Value& o, const i18n::Catalog& words);
[[nodiscard]] std::string traverse_report(const base::Value& r, std::string_view units, const i18n::Catalog& words);
[[nodiscard]] double dir2az(double d);

}  // namespace cc::civil
