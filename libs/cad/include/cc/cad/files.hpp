// SPDX-License-Identifier: Apache-2.0
// cc::cad — the pieces of CadCraft 0.5's modules that loading a drawing into the document needs (doc.js load()).
#pragma once

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cad {

/// dims.js DIM_DEFAULTS: the ISO-25-like dimension style a new drawing starts with.
[[nodiscard]] const base::Value& dim_defaults();
/// panes.js cleanPanes: a file's split-screen setup made safe ({ layout, fx, fy, active, list }), or null.
[[nodiscard]] base::Value clean_panes(const base::Value& v);
/// model3d.js split3d: 3D faces out of a drawing (in place; notes into its report through `words`) →
/// { text: CadCraft's 3D JSON or null, faces, view }.
base::Value split3d(base::Value& drawing, const i18n::Catalog& words);

}  // namespace cc::cad
