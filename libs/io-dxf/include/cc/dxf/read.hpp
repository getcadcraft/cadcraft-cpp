// SPDX-License-Identifier: Apache-2.0
// cc::dxf — L3. An ASCII DXF file → the same drawing model cc::dwg::read_dwg returns: the C++ port of BimCraft's
// dxf-model.js (CadCraft 0.5), written from the public DXF reference (group codes). The views read DWG and DXF through
// one model.
//
//   read_dxf_model(text) → the model, or an Error (binary DXF, a line that is not a group code) worded as the JS does.
//   decode_text(bytes)   → the file's bytes as the JS reads them (UTF-8, invalid bytes → U+FFFD, a leading BOM
//                          dropped: TextDecoder's defaults).
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::dxf {

[[nodiscard]] base::Result<base::Value> read_dxf_model(std::string_view text);

[[nodiscard]] std::string decode_text(std::span<const std::byte> bytes);

}  // namespace cc::dxf
