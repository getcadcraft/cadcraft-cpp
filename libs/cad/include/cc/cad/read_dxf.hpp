// SPDX-License-Identifier: Apache-2.0
// cc::cad::read_dxf — CadCraft 0.5's own DXF reader (dxf.js readDxf): an ASCII DXF as the drawing doc.load() takes,
// the same shape import_dwg gives for a DWG. Multileaders, pictures and wipeouts are read through the drawing model the
// DWG import uses: the caller hands in that reader (cc::dxf::read_dxf_model, one layer up).
//
//   auto drawing = cc::cad::read_dxf(text, words, [](std::string_view t) { return cc::dxf::read_dxf_model(t); });
#pragma once

#include <functional>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cad {

/// readDxf(text): the drawing, or the reason it cannot be read (a binary DXF, a line that is not a group code).
[[nodiscard]] base::Result<base::Value> read_dxf(
    std::string_view text, const i18n::Catalog& words,
    const std::function<base::Result<base::Value>(std::string_view)>& read_model);

}  // namespace cc::cad
