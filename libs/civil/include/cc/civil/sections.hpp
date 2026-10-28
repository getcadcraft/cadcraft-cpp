// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's sections.js: sample lines square to an alignment and the cross sections of surfaces along them
// (exact: a vertex wherever a line crosses a triangle edge), cut and fill areas, average-end-area volumes, the CSV.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

inline constexpr std::size_t kMaxSections = 2000;

struct OZ {
  double o = 0;
  std::optional<double> z;  // a clipped run's end can be null (zAt found nothing)
};
using Run = std::vector<OZ>;
using Runs = std::vector<Run>;

[[nodiscard]] base::Value sample_defaults(std::string_view units);
[[nodiscard]] base::Value clean_sampling(const base::Value& o, std::string_view units);
/// sampleStations(al, { step, extra, from, to })
[[nodiscard]] std::vector<double> sample_stations(const Alignment* al, double step, const std::vector<double>& extra,
                                                  std::optional<double> from, std::optional<double> to);
struct SampleLine {
  double sta = 0;
  P2 c, a, b;
  double dir = 0;
  base::Value left, right;  // as given
};
[[nodiscard]] std::optional<SampleLine> sample_line(const Alignment& al, double sta, const base::Value& left,
                                                    const base::Value& right);
[[nodiscard]] Runs surface_section(const Surface* s, P2 a, P2 b, double off_a, double off_b);
[[nodiscard]] std::optional<double> z_at(const Runs& runs, double o);
struct Part {
  double o0, o1;
  bool cut;
};
struct CutFill {
  double cut = 0, fill = 0;
  std::vector<Part> parts;
};
[[nodiscard]] CutFill cut_fill(const Runs& ground, const Runs& design);
/// featureMarks(lines, line): lines = { code: [{x, y, z}] } (a JSON object)
[[nodiscard]] base::Value feature_marks(const base::Value& lines, const SampleLine& line);
[[nodiscard]] std::vector<std::pair<double, double>> region_spans(const SampleLine& line,
                                                                  const std::vector<std::vector<P2>>& regions);
[[nodiscard]] Runs clip_runs(const Runs& runs, const std::vector<std::pair<double, double>>& spans);

struct SectionInput {
  const Alignment* alignment = nullptr;
  const Surface* ground = nullptr;
  const Surface* design = nullptr;
  std::optional<std::vector<std::vector<P2>>> regions;
  base::Value corridor;  // feature lines, or null
  base::Value sampling;  // { step, left, right, extra }, or null
  std::optional<double> from, to;
};
/// buildSections(…) → { sections, rows, totals } as a JSON value
[[nodiscard]] base::Value build_sections(const SectionInput& in);
/// sectionsCsv(set, { units })
[[nodiscard]] std::string sections_csv(const base::Value& set, std::string_view units, const i18n::Catalog& words);

// JSON forms
[[nodiscard]] Runs runs_from(const base::Value& v);
[[nodiscard]] base::Value runs_value(const Runs& r);
[[nodiscard]] base::Value line_value(const SampleLine& l);
[[nodiscard]] SampleLine line_from(const base::Value& v);

}  // namespace cc::civil
