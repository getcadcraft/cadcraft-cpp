// SPDX-License-Identifier: Apache-2.0
// cc::save — L4. A CadCraft drawing → ASCII DXF: the C++ ports of CadCraft 0.5's dxf.js writeDxf (R12, AC1009) and
// dxf2000.js writeDxf2000 (R2000, AC1015), at parity with them (the same text for the same document;
// tools/parity/dxf-save.mjs).
//
//   auto r = cc::save::write_dxf2000(doc, words);   // r.text (ASCII lines, CR LF); r.report: { notes, counts }
//
// R2000 goes through the writer model DWG saving builds (save_dwg with model_only and explode_leaders), so it keeps
// what a DWG keeps. R12 writes the document directly: what R12 lacks becomes what it draws, with a note.
//
// Constraints go in as 0.5 writes them (CCREF xdata, block CADCRAFT_PARAMETRIC).
#pragma once

#include <string>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::save {

struct DxfSaveResult {
  std::string text;    // the file (UTF-8; what the JS's string holds)
  base::Value report;  // { notes, counts, extents? }
};

/// dxf.js writeDxf(doc): DXF R12. Throws std::invalid_argument where the JS throws.
[[nodiscard]] DxfSaveResult write_dxf(const doc::Doc& doc, const i18n::Catalog& words);
/// dxf2000.js writeDxf2000(doc): DXF R2000.
[[nodiscard]] DxfSaveResult write_dxf2000(const doc::Doc& doc, const i18n::Catalog& words);

}  // namespace cc::save
