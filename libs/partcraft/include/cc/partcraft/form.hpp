// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::form — PartCraft 0.5's form.js, the part a part file needs: the freeform cage format
// ({format:'partcraft-form', version:1, verts, faces, creases, corners, sym}), its reader (an oriented 2-manifold with
// boundary, ids and sizes checked) and the box primitive. Catmull–Clark subdivision and the Bézier conversion come with
// the Form workspace.
#pragma once

#include <cstddef>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::form {

inline constexpr std::string_view kFormat = "partcraft-form";
inline constexpr double kVersion = 1;
inline constexpr double kSharp = 10;
inline constexpr std::size_t kMaxVerts = 20000;
inline constexpr std::size_t kMaxFaces = 20000;

/// readForm(raw): a clean cage from untrusted JSON (a value or its text), or the reason.
[[nodiscard]] base::Result<base::Value> read_form(const base::Value& raw, const i18n::Catalog& words);
/// box(nx, ny, nz, sx, sy, sz): nx × ny × nz faces per side (1–32), sx × sy × sz, centred on the origin.
[[nodiscard]] base::Value box(double nx = 1, double ny = 1, double nz = 1, double sx = 100, double sy = 100,
                              double sz = 100);

}  // namespace cc::partcraft::form
