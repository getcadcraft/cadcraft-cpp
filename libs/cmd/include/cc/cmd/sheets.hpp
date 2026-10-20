// SPDX-License-Identifier: Apache-2.0
// cc::cmd::sheets — CadCraft 0.5 sheets.js: paper sizes, drawing scales, viewports aimed at the model, a new sheet's
// border and title block. A layout's paper is { w, h, unit: 'mm' | 'in', x0, y0, key } in paper units; a viewport's
// `s` is paper units per model unit, and a scale 1:N means one real unit on paper stands for N in the model.
//
//   const Paper& p = sheets::default_paper(doc);
//   auto made = sheets::sheet_contents(doc, sheets::make_paper(p.key), {.sheet_no = 2, .date_ms = now}, words);
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/geom.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cmd::sheets {

/// One of PAPERS (landscape: w ≥ h). `label_key`: a language key when the label is translated, else empty.
struct Paper {
  std::string_view key, label, label_key;
  double w = 0, h = 0;
  std::string_view unit;  // "mm" | "in"
};
[[nodiscard]] const std::vector<Paper>& papers();
[[nodiscard]] const Paper* paper_by_key(std::string_view key);
/// What a paper is called (translated where 0.5 translates it).
[[nodiscard]] std::string paper_label(const Paper& p, const i18n::Catalog& words);
/// matchPaper(w, h, unit): the standard paper a sheet is, either way round, within 0.5 %.
[[nodiscard]] const Paper* match_paper(double w, double h, std::string_view unit);

/// UNIT_MM[unit], or nothing for a unit it does not know.
[[nodiscard]] std::optional<double> unit_mm(std::string_view unit);
[[nodiscard]] bool is_imperial(const doc::Doc& doc);
[[nodiscard]] double model_mm(const doc::Doc& doc, std::string_view paper_unit);
[[nodiscard]] double s_of(const doc::Doc& doc, std::string_view paper_unit, double n);
[[nodiscard]] double n_of(const doc::Doc& doc, std::string_view paper_unit, double s);

struct Scale {
  double n = 1;
  std::string label;
  double s = 0;  // bestScale only
};
[[nodiscard]] std::vector<Scale> scale_list(const doc::Doc& doc);
[[nodiscard]] std::string scale_label(const doc::Doc& doc, std::string_view paper_unit, double s);
[[nodiscard]] Scale best_scale(const doc::Doc& doc, std::string_view paper_unit, double s_fit);

/// The bounds of the model's visible objects.
[[nodiscard]] cad::Bounds model_extents(const doc::Doc& doc);
struct Aim {
  double cx = 0, cy = 0, s = 1;
};
/// aimViewport(doc, unit, w, h, win, { exact }): centre on the window, at a standard scale that fits (or exactly).
[[nodiscard]] Aim aim_viewport(const doc::Doc& doc, std::string_view paper_unit, double w, double h,
                               const cad::Bounds& win, bool exact = false);

/// defaultPaper(doc): A3 for metric or unitless drawings, Tabloid for imperial ones.
[[nodiscard]] const Paper& default_paper(const doc::Doc& doc);
/// makePaper(key, portrait): { key, unit, w, h, x0: 0, y0: 0 }.
[[nodiscard]] base::Value make_paper(std::string_view key, bool portrait = false);

struct SheetOptions {
  std::optional<std::string> title;  // doc.meta.name || 'Drawing1'
  int sheet_no = 1;
  double date_ms = 0;  // the date in the title block (ms since 1970, UTC): the host's clock
};
struct SheetContents {
  base::Value paper;
  base::Array entities;
  Aim scale;
  base::Array layers;  // [{ name, color, plot? }] to add when missing
};
/// sheetContents(doc, paper, opts): a border, the title block (layer "Title block") and one viewport onto the model.
[[nodiscard]] SheetContents sheet_contents(const doc::Doc& doc, const base::Value& paper, const SheetOptions& o,
                                           const i18n::Catalog& words);

/// "2026-10-16": a date as Date.toISOString().slice(0, 10).
[[nodiscard]] std::string iso_date(double ms);

}  // namespace cc::cmd::sheets
