// SPDX-License-Identifier: Apache-2.0
// cc::cad — L2. CadCraft's drawing (what the 0.5 app's doc.load() takes) made from the readers' models: CadCraft 0.5's
// dwg-import.js `importDwg`, ported function for function so the drawing equals the JS one on the corpus
// (tools/parity/import.mjs; M4.1).
//
//   base::Value db = cc::dwg::read_dwg(bytes);
//   base::Value drawing = cc::cad::import_dwg(db, "AC1032", words);   // { entities, layers, blocks, layouts, … }
//
// The drawing is JS-shaped data (base::Value), in the 0.5 app's own units and conventions (doc.js header): entities
// line circle arc poly ellipse text point insert hatch dim viewport mleader xline ray, plus face3d in `faces3d`.
#pragma once

#include <string_view>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cad {

/// Limits the import keeps (the JS has none; a hostile file must not make the copies unbounded).
struct ImportLimits {
  std::size_t minsert_copies = 100000;  // rows × columns of one MINSERT
};

/// importDwg(db, { versionTag }): a cc-dwg drawing model → the drawing. `version_tag` = the file's first six bytes
/// ("AC1032"); `words` = the language the report's notes are written in (English for parity).
[[nodiscard]] base::Value import_dwg(const base::Value& db, std::string_view version_tag, const i18n::Catalog& words,
                                     const ImportLimits& limits = {});

}  // namespace cc::cad
