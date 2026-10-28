// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's design modules: superelev.js (superelevation through curves), grading.js (pads daylighted to a
// surface, slope lines, the graded surface, grid volumes) and corridor.js (one assembly swept along an alignment).
// Their inputs come from project files and are untrusted: they are read as the JS reads them (Number() of each field).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/civil/alignment.hpp"
#include "cc/civil/tin.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

// ---- superelev.js
[[nodiscard]] base::Value super_defaults(std::string_view units);
/// cleanSuper(o, units): null when o is not an object.
[[nodiscard]] base::Value clean_super(const base::Value& o, std::string_view units);
struct RateRow {
  double r = 0;
  std::optional<double> e;  // null = normal crown
};
[[nodiscard]] std::vector<RateRow> rate_rows(const base::Value& rate);
/// rateFor(R, rate)
[[nodiscard]] std::optional<double> rate_for(double R, const std::vector<RateRow>& rate);

struct SuperCurve {
  double i = 0, R = 0;
  bool left = false;
  double e = 0, Lr = 0, Lt = 0, pc = 0, pt = 0;
  // stations, in the JS object's order: lc0, fs0, fs1, lc1, nc0, nc1, rc0, rc1
  double lc0 = 0, fs0 = 0, fs1 = 0, lc1 = 0, nc0 = 0, nc1 = 0, rc0 = 0, rc1 = 0;
};
struct SuperPlan {
  std::vector<SuperCurve> curves;
  std::vector<std::string> problems;
};
/// superPlan(al, { super, nc, lane })
[[nodiscard]] SuperPlan super_plan(const Alignment* al, const base::Value& sp, double nc, double lane,
                                   const i18n::Catalog& words);
struct SideSlopes {
  double L, R;
};
[[nodiscard]] SideSlopes slopes_at(const SuperPlan& plan, double sta, double nc);
[[nodiscard]] std::vector<double> super_key_stations(const SuperPlan& plan);
[[nodiscard]] base::Value super_plan_value(const SuperPlan& p);

// ---- grading.js
struct Cast {
  double x0, y0, x, y, z, d;
  const char* kind;  // cut, fill, flat, off
};
/// castSlope(surface, p0, ux, uy, z, cut, fill, { rate, step, far })
[[nodiscard]] Cast cast_slope(const Surface& s, P2 p0, double ux, double uy, double z, double cut, double fill,
                              double rate = 1, double step = 0.25, double far = 1e4);
[[nodiscard]] base::Value cast_value(const Cast& c);
/// selfCrosses(pts): a closed polygon's edges cross (not at shared corners).
[[nodiscard]] bool self_crosses(const std::vector<P2>& pts);
/// daylightPad(surface, pad, { spacing, fanStep, maxDist }) → { rays, daylight, problems, poly } (as JSON values)
[[nodiscard]] base::Value daylight_pad(const Surface& s, const base::Value& pad, const base::Value& options,
                                       const i18n::Catalog& words);
struct GridVolume {
  double cut = 0, fill = 0, net = 0, cells = 0, cell = 0, area = 0;
};
/// gridVolume(base, top, { cell, region, bounds, maxCells })
[[nodiscard]] GridVolume grid_volume(const Surface& base, const Surface& top, const base::Value& options);

/// A patch an item grades in (a pad's or a corridor's): corridorPatch's shape.
struct Patch {
  base::Value value;  // { problems, region, points, breaklines, info }
};
/// One item of gradedSurface: a pad (a JSON object) or a patch maker (a corridor).
struct GradeItem {
  base::Value pad;                                   // when `patch` is empty
  std::string name;                                  // a patch item's name
  std::function<base::Value(const Surface&)> patch;  // corridorPatch on the surface so far
};
struct Graded {
  std::shared_ptr<Surface> surface;
  base::Value report, regions, items;
};
/// gradedSurface(base, basePoints, baseBreaklines, items, buildSurface)
using BuildFn = std::function<Built(const base::Value& points, const base::Value& breaklines)>;
[[nodiscard]] Graded graded_surface(std::shared_ptr<Surface> base, const base::Value& base_points,
                                    const base::Value& base_breaklines, const std::vector<GradeItem>& items,
                                    const i18n::Catalog& words, const BuildFn& build);

// ---- corridor.js
[[nodiscard]] base::Value default_assembly(std::string_view units);
/// corridorPatch(surface, alignment, profile, asm) → { problems, region, points, breaklines, info }
[[nodiscard]] base::Value corridor_patch(const Surface* surface, const Alignment* al, const Profile* prof,
                                         const base::Value& asm_, const i18n::Catalog& words);

}  // namespace cc::civil
