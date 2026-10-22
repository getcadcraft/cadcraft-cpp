// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's points.js: survey point files (CSV / TXT; PNEZD and the other column orders; comma, tab, semicolon
// or spaces), point groups, lasso picks and the PNEZD writer.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/i18n/i18n.hpp"

namespace cc::civil {

struct SurveyPoint {
  std::string n;  // point number or name
  double x = 0, y = 0;
  std::optional<double> z;  // null when the file has none
  std::string d;            // description
};

struct BadRow {
  double line = 0;
  std::string text, why;
};

struct ParsedPoints {
  std::vector<SurveyPoint> points;
  std::optional<std::string> format;  // null for an empty file
  std::string format_from;
  std::optional<std::string> delimiter;  // ",", "\t", ";" or "ws"
  std::optional<std::vector<std::string>> header;
  std::vector<BadRow> bad;
  double bad_count = 0, duplicates = 0;
  bool latlon = false;  // present (true) only for a latitude / longitude header
};

[[nodiscard]] ParsedPoints parse_points(std::string_view text, std::string_view format, std::string_view prefer,
                                        const i18n::Catalog& words);
[[nodiscard]] std::vector<std::string> split_row(std::string_view s, std::string_view delim);
[[nodiscard]] std::string group_key(std::string_view desc);
struct GroupCount {
  std::string key;
  double count = 0;
};
[[nodiscard]] std::vector<GroupCount> point_groups(const std::vector<SurveyPoint>& points);
/// The points of a group in point-number order (indices into `points`).
[[nodiscard]] std::vector<std::size_t> group_points(const std::vector<SurveyPoint>& points, std::string_view key);
/// byNumber(a, b)
[[nodiscard]] double by_number(const SurveyPoint& a, const SurveyPoint& b);
[[nodiscard]] std::string write_points(const std::vector<SurveyPoint>& points, double dec = 3);
struct XY {
  double x = 0, y = 0;
};
/// lassoPoints(points, path, mode, r): indices of the points picked.
[[nodiscard]] std::vector<std::size_t> lasso_points(const std::vector<SurveyPoint>& points, const std::vector<XY>& path,
                                                    std::string_view mode, double r);

}  // namespace cc::civil
