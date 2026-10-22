// SPDX-License-Identifier: Apache-2.0
// cc::shapes — CadCraft 0.5's lib/shapes/items.js: steel sections and standard parts as "CadCraft library item"
// format 1 documents (docs/LIBRARY-SPEC.md §1), the same item CadCraft's Standard tab places and the public library
// publishes: a plan in millimetres on layers PART / STEEL (-THIN, -CENTER, -HIDDEN, -FILL), params and source_attrs.
#pragma once

#include <string_view>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/shapes/sections.hpp"

namespace cc::shapes {

/// PART_TAGS: {type, std, size, length, material} → the block attribute tags.
[[nodiscard]] base::Value part_tags();
/// CC0: {id, holder, url}.
[[nodiscard]] base::Value cc0();
/// The section's outline loops for a 3D member (outline(sec, 8)).
[[nodiscard]] std::vector<Loop> member_loops(const Section& sec);
/// sectionItem(sec, {view, length}): its section view, or an elevation `length` mm long (view "elevation"); an
/// elevation length outside (0, 1e6] is refused ("length out of range", as the JS throws).
[[nodiscard]] base::Result<base::Value> section_item(const Section& sec, const base::Value& options);
/// partItem(part): `part` is make_part(…)'s part.
[[nodiscard]] base::Value part_item(const base::Value& part);
/// localItem(item): the item with the CC0 licence credited to CadCraft and its table, and provenance {library}.
[[nodiscard]] base::Value local_item(const base::Value& item);

}  // namespace cc::shapes
