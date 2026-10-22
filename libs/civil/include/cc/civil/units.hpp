// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's units.js: project units (US feet or metres), stations ("12+34.56", "1+234.567"), number text.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "cc/i18n/i18n.hpp"

namespace cc::civil {

struct Unit {
  std::string_view key, short_name, area, vol;
  double vol_per, sta_len, sta_dec, dec;
  std::string_view landxml;
};
/// unitOf(u): 'ft' or 'm'; anything else is feet.
[[nodiscard]] const Unit& unit_of(std::string_view u);
/// The unit's name in the catalog (units.ft / units.m).
[[nodiscard]] std::string unit_name(const Unit& u, const i18n::Catalog& words);
/// METRES_PER[name], or nullopt.
[[nodiscard]] std::optional<double> metres_per(std::string_view name);

[[nodiscard]] std::string format_station(double sta, std::string_view unit = "ft", std::optional<double> dec = {});
/// parseStation(s, unit): nullopt = null.
[[nodiscard]] std::optional<double> parse_station(std::string_view s, std::string_view unit = "ft");
/// isNum(s) for a string (a number is a number when finite).
[[nodiscard]] bool is_num(std::string_view s);
[[nodiscard]] std::string fixed(double n, double d = 2);
[[nodiscard]] std::string grouped(double n, double d = 0);
[[nodiscard]] std::string pct(double g, double d = 2);
[[nodiscard]] std::string volume_text(double v, std::string_view unit = "ft", double d = 1);
[[nodiscard]] std::string area_text(double a, std::string_view unit = "ft", double d = 0);
[[nodiscard]] std::string bearing(double a);
[[nodiscard]] std::string dms(double deg);

}  // namespace cc::civil
