// SPDX-License-Identifier: Apache-2.0
// cc::engine — what a command script may ask of its drawing besides its state (the parity kit, tools/parity/
// commands.mjs): {"save": ["dxf", "dwg2018", …]} saves it as 0.5 does (a DWG dated with the harness's clock, its GUIDs
// from the harness's seeded bytes) and opens each file again, so the constraints and parameters that come back are
// compared too.
#pragma once

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::engine {

/// result.saved = { dxf: text, dxfReopened: { constraints, params }, dwg2018: base64, dwg2018Reopened: … }
void script_save(const base::Value& save, const doc::Doc& doc, double now_ms, const i18n::Catalog& words,
                 base::Value& result);

}  // namespace cc::engine
