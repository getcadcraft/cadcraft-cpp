// SPDX-License-Identifier: Apache-2.0
// cc::shapes — CadCraft 0.5's lib/shapes/parts.js: mechanical standard parts from public standard tables (ISO 4014,
// 4017, 4032, 7089, 2338, 15; DIN 6885-1; ASME B18.2.1 / B18.2.2; ISO 53 / 54 spur gears). Lengths are millimetres
// (inch families name their sizes in inches). Records are JS-shaped values (base::Value) with the JS's field order, as
// PartCraft's templates, CadCraft's Standard tab and the library items read them.
//
//   base::Value r = shapes::make_part("iso4017", "M8", opts);   // {ok: true, part} | {ok: false, error, key?}
#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::shapes {

struct PartOption {
  std::string_view key, unit;
  double min = 0, max = 0;
  bool integer = false;
};
struct PartFamily {
  std::string_view id, type, name, standard, units, material;
  std::vector<PartOption> options;
};
struct PartStandard {
  std::string_view id, title, url;
};

[[nodiscard]] std::span<const PartStandard> part_standards();
[[nodiscard]] const PartStandard* find_standard(std::string_view id);
[[nodiscard]] std::span<const PartFamily> part_families();
[[nodiscard]] const PartFamily* find_family(std::string_view id);

/// partSizes(id): [{size, label, …dims}] in table order ([] for an unknown family).
[[nodiscard]] base::Value part_sizes(std::string_view id);
/// partOptions(id, size): the default options ({} when the size is not the family's; `size` compares as ===).
[[nodiscard]] base::Value part_options(std::string_view id, const base::Value& size);
/// makePart(id, size, opts): {ok: true, part} or {ok: false, error, key?}; `opts` is untrusted (numbers checked).
[[nodiscard]] base::Value make_part(std::string_view id, const base::Value& size, const base::Value& opts);
/// LIBRARY_SET: [[familyId, size, opts]…].
[[nodiscard]] base::Value library_set();
[[nodiscard]] base::Value standards_value();      // PART_STANDARDS
[[nodiscard]] base::Value families_part_value();  // PART_FAMILIES

}  // namespace cc::shapes
