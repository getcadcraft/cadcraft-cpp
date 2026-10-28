// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::gdt — PartCraft 0.5's gdt.js: GD&T (ASME Y14.5 datum symbols and feature control frames) and surface
// finish (ISO 1302) annotations on drawings: making them, checking them, reading them from a file, describing them, and
// their geometry as strokes, filled polygons and Helvetica texts in sheet millimetres. JS-shaped values in and out.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::gdt {

/// newAnnotation(kind, props)
[[nodiscard]] base::Result<base::Value> new_annotation(const base::Value& kind, const base::Value& props,
                                                       const i18n::Catalog& words);
/// validate(a): why the annotation is wrong, or nothing.
[[nodiscard]] std::optional<std::string> validate(const base::Value& a, const i18n::Catalog& words);
/// readNotes(raw): clean copies of a file's notes, or the Error the JS throws.
[[nodiscard]] base::Result<base::Value> read_notes(const base::Value& raw, const i18n::Catalog& words);
/// describe(a): the short description lists show.
[[nodiscard]] base::Result<std::string> describe(const base::Value& a, const i18n::Catalog& words);
/// Text width in mm at a size (Helvetica).
[[nodiscard]] double text_width(std::string_view s, double size);
/// annotationGeometry(a, anchor): {strokes, fills, texts, parts?, frameBox?, triangle?, circle?, box}.
[[nodiscard]] base::Result<base::Value> annotation_geometry(const base::Value& a, const std::array<double, 2>& anchor,
                                                            const i18n::Catalog& words);

}  // namespace cc::partcraft::gdt
