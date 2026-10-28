// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's featureline.js: feature lines with grades (JSON objects as the project keeps them), their crossings
// and runs in a profile view, and the screen overlay of grades and elevations.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

[[nodiscard]] base::Value feature_defaults(std::string_view units);
[[nodiscard]] base::Value clean_feature(const base::Value& o, std::string_view units, const i18n::Catalog& words);
[[nodiscard]] std::vector<double> stations(const base::Value& f);
struct FlSegment {
  std::size_t i, j;
  double len, dz;
  std::optional<double> grade;
};
[[nodiscard]] std::vector<FlSegment> segments(const base::Value& f);
[[nodiscard]] base::Value segments_value(const std::vector<FlSegment>& s);
[[nodiscard]] base::Value set_elevation(const base::Value& f, const base::Value& i, const base::Value& z);
[[nodiscard]] base::Value set_grade(const base::Value& f, const base::Value& i, const base::Value& grade);
[[nodiscard]] base::Value raise(const base::Value& f, const base::Value& dz);
[[nodiscard]] base::Value even_grade(const base::Value& f, const base::Value& i, const base::Value& j);
/// fromSurface(f, surface, which) → { f, missing }
[[nodiscard]] base::Value from_surface(const base::Value& f, const Surface* surface, const base::Value& which);
/// offsetFeature(f, dist, { dz, slope }) → { f | null, problem }
[[nodiscard]] base::Value offset_feature(const base::Value& f, const base::Value& dist, const base::Value& opts,
                                         const i18n::Catalog& words);
[[nodiscard]] base::Value feature_breaklines(const base::Value& features, const base::Value& use);
/// featureItem(f), with a patch item's patch() already called ({…, patch: {problems, region, …}}); null for none
[[nodiscard]] base::Value feature_item(const base::Value& f);
[[nodiscard]] std::string grade_label(const base::Value& g);
struct FlCrossing {
  double sta, z;
  std::size_t i;
  std::optional<double> grade;
};
[[nodiscard]] std::vector<FlCrossing> alignment_crossings(const Alignment* al, const base::Value& f);
struct FlRunPt {
  double sta, z, off;
};
[[nodiscard]] std::vector<std::vector<FlRunPt>> projected_runs(const Alignment* al, const base::Value& f,
                                                               double within);
[[nodiscard]] base::Value crossings_value(const std::vector<FlCrossing>& c);
[[nodiscard]] base::Value runs_value(const std::vector<std::vector<FlRunPt>>& runs);
[[nodiscard]] base::Value feature_profile(const Alignment* al, const base::Value& features, double within);
/// gradeOverlay(features, (x, y) → ((x − ox)·k, −(y − oy)·k), { minSeg, minGap, skip })
[[nodiscard]] base::Value grade_overlay(const base::Value& features, double k, double ox, double oy, double min_seg,
                                        double min_gap, const base::Value& skip);

}  // namespace cc::civil
