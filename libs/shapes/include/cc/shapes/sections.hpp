// SPDX-License-Identifier: Apache-2.0
// cc::shapes — CadCraft 0.5's lib/shapes/sections.js: the structural steel section table (AISC Shapes Database v16.0:
// W, S, C, L, HSS, Pipe; EN 10365:2017: IPE, HEA, HEB, UPN) and the section geometry DetailCraft and CadCraft draw
// with. Records are in millimetres, in the JS's order (W, C, L, HSS rectangular, HSS round, IPE/HEA/HEB, S, unequal L,
// Pipe by OD then wall, UPN), with the JS's field order (to_value gives the JS record).
//
//   const Section* s = shapes::find_section("w12x26");   // any case
//   auto loops = shapes::outline(*s);                     // outer CCW, then holes CW, centred on the bounding box
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::shapes {

inline constexpr double kSteelDensity = 7850;  // kg/m³
inline constexpr double kIn = 25.4;            // mm per inch
inline constexpr double kLbFt = 1.48816394;    // kg/m per lb/ft

struct SectionSource {
  std::string_view id, title, publisher;
  double year = 0;
  std::string_view units, url;
};
struct SectionFamily {
  std::string_view id, source, kind;
};

/// A section record (sectionRows()); a hand-made one (a plate: kind "PL", b, t) works with outline and exact_area.
struct Section {
  std::string name, family, kind, source;
  std::vector<std::pair<std::string, double>> dims;  // d, bf, tw, tf, r … in the JS record's order
  std::optional<double> area_pub, mass_pub;          // mm², kg/m
  std::string pub_units;                             // "in" / "mm"
  std::vector<std::pair<std::string, double>> pub;   // the row as published
  /// The dimension, or NaN when the record has none (a JS undefined in arithmetic).
  [[nodiscard]] double get(std::string_view key) const noexcept;
  [[nodiscard]] bool has(std::string_view key) const noexcept;
};

using Point = std::array<double, 2>;
using Loop = std::vector<Point>;

struct Box {
  double x0, y0, x1, y1, w, h;
};

[[nodiscard]] std::span<const SectionSource> section_sources();
[[nodiscard]] const SectionSource* section_source(std::string_view id);
[[nodiscard]] std::span<const SectionFamily> section_families();

/// "W12X26" → "W12x26": an X between a digit (or / or .) and a digit prints lower case.
[[nodiscard]] std::string display_name(std::string_view name);
/// Every section (built once; the same vector each call).
[[nodiscard]] const std::vector<Section>& section_rows();
/// A section by name, any case, trimmed; nullptr when there is none.
[[nodiscard]] const Section* find_section(std::string_view name);
[[nodiscard]] std::vector<const Section*> family_rows(std::string_view family);

/// Exact area (mm²), arcs included exactly; 0 for an unknown kind.
[[nodiscard]] double exact_area(const Section& p);
/// The outline as loops of [u, v]: the first outer (CCW), the others holes (CW), centred on the bounding box.
[[nodiscard]] std::vector<Loop> outline(const Section& p, double seg = 6);
[[nodiscard]] Box bbox(const Loop& loop);
[[nodiscard]] double shoelace(const Loop& loop);
[[nodiscard]] double loop_length(const Loop& loop);

/// The JS record ({name, family, kind, source, …dims, areaPub, massPub, pub}).
[[nodiscard]] base::Value to_value(const Section& s);
/// A section from a JS-shaped record (numbers only; other members are left out): what the parity script hands over.
[[nodiscard]] Section from_value(const base::Value& v);
[[nodiscard]] base::Value loops_value(const std::vector<Loop>& loops);
[[nodiscard]] base::Value box_value(const Box& b);
[[nodiscard]] base::Value sources_value();   // SECTION_SOURCES
[[nodiscard]] base::Value families_value();  // FAMILIES

}  // namespace cc::shapes
