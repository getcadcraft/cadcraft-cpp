// SPDX-License-Identifier: Apache-2.0
// cc::plot — L5. CadCraft 0.5's plot.js: a sheet → a vector PDF. A layout plots at 1:1 with its viewports showing the
// model; the model plots on a paper size, at 1:n or fitted. Lines are true vectors (arcs as Béziers), fills keep
// their holes (even-odd), text is Helvetica (one of the 14 fonts every PDF reader has, so nothing is embedded; its
// Windows-1252 set covers Western European text, other characters print as "?" and the report counts them). Layers
// set not to plot are left out; viewport frames follow their layer.
//
//   auto list = plot::display_list(doc, plot::layout_sheet(doc, doc.layouts()[0]));
//   auto pdf = plot::to_pdf(list, {.title = "Plan", .date_ms = now});   // pdf.bytes, pdf.missing, pdf.texts
//
// Pictures (WL.13) go in as image XObjects at their own resolution (`max_dpi`: a cap on paper), under the linework,
// clipped to their frames; only pictures whose file the drawing has.
// Not ported yet (docs/gaps.md): per-viewport layer overrides (VP.3), the canvas preview.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "cc/doc/doc.hpp"
#include "cc/docview/walk.hpp"

namespace cc::plot {

/// PEN_WIDTHS: the pen widths a plot offers (mm).
inline constexpr std::array<double, 6> kPenWidths{0.13, 0.18, 0.25, 0.35, 0.5, 0.7};

/// One view of a sheet: what it draws (`entities`, valid while the document is not edited) through `m` (entity →
/// mm), clipped to `clip` (mm), without the layers frozen in it (upper case).
struct View {
  std::optional<std::vector<cad::P2>> clip;
  std::optional<cad::Mat2> m;
  std::vector<const base::Value*> entities;
  std::set<std::string> frozen;
};
struct ScaleInfo {
  double s = 1, n = 1;
  std::string label;
  bool fits = true;
};
struct Sheet {
  double w = 0, h = 0;  // mm
  std::vector<View> views;
  std::string name;
  std::optional<ScaleInfo> scale;
};
/// layoutSheet(doc, layout): a layout at 1:1, its viewports (not turned off) showing the model.
[[nodiscard]] Sheet layout_sheet(const doc::Doc& doc, const doc::Layout& layout);
/// modelSheet(doc, { area, paper, portrait, n }): the model window `area` on a paper, at 1:n or fitted (n absent).
[[nodiscard]] Sheet model_sheet(const doc::Doc& doc, const cad::Bounds& area, std::string_view paper = "iso-a3",
                                bool portrait = false, std::optional<double> n = std::nullopt);

struct Fill {
  std::string color;
  std::vector<std::vector<double>> loops;
  double alpha = 1;
};
struct Stroke {
  std::string color;
  double lw = -3, alpha = 1;
  std::vector<cad::Prim> prims;
};
struct Text {
  std::string color;
  base::Value e;
  double alpha = 1;
};
struct Mask {
  std::string color;
  std::vector<double> loop;
};
struct Point {
  std::string color;
  double x = 0, y = 0;
};
struct ListView {
  std::optional<std::vector<cad::P2>> clip;
  std::vector<Fill> fills;
  std::vector<Mask> masks;
  std::vector<Stroke> strokes;  // grouped by colour, lineweight and alpha, in first-seen order
  std::vector<Point> points;
  std::vector<Text> texts;
  std::vector<docview::PictureOut> pictures;  // in mm on the sheet
};
struct DisplayList {
  double w = 0, h = 0;
  std::string name;
  std::vector<ListView> views;
  /// the files the pictures use (as the drawing names them → the drawing's file; valid while it keeps them)
  std::vector<std::pair<std::string, const doc::PictureFile*>> picture_files;
};
/// displayList(doc, sheet, { mono }): what to draw, in millimetres on the sheet (mono: everything black).
[[nodiscard]] DisplayList display_list(const doc::Doc& doc, const Sheet& sheet, bool mono = true);

struct PdfOptions {
  double pen = 0.25;                 // the default pen (mm)
  std::optional<std::string> title;  // the list's name when absent
  double date_ms = 0;                // /CreationDate (ms since 1970, UTC): the host's clock
  /// Compresses the page content in the zlib format (/FlateDecode; cc::image::zlib_compress); empty: left as written.
  std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)> compress;
  double max_dpi = 0;  // > 0: pictures printed denser than this many pixels per inch are shrunk (a JPEG is kept)
};
struct PdfResult {
  std::vector<std::uint8_t> bytes;
  std::size_t missing = 0, texts = 0, pages = 1;
  bool compressed = false;
  std::size_t pictures = 0, shrunk = 0, kept = 0;  // to_pdf: the pictures on the page, shrunk, kept as they are
};
/// toPdf(list, { pen, title }): one page.
[[nodiscard]] PdfResult to_pdf(const DisplayList& list, const PdfOptions& o);
/// toPdfPages(lists, { pen, title, names }): one page each at its own paper size, a bookmark per page.
[[nodiscard]] PdfResult to_pdf_pages(const std::vector<DisplayList>& lists, const PdfOptions& o,
                                     const std::vector<std::string>& names = {});

/// The PDF a command script asks for (`plot`: {mono (default true), maxDpi}): the model at its extents, fitted on the
/// default paper, as base64 in result.pdf with result.pdfReport {pictures, shrunk, kept}; nothing for an empty model.
void script_pdf(const base::Value& plot, const doc::Doc& doc, double date_ms, base::Value& result);
/// A PDF as the parity kit compares it: every Flate stream inflated (zlib builds deflate the same bytes differently:
/// Node's is Chromium's), "/Length n" and the cross-reference table masked; everything else byte for byte.
[[nodiscard]] std::string pdf_canonical(std::span<const std::uint8_t> pdf);

/// extentsOf(doc): the model area a plot covers when asked for its extents, or nothing for an empty model.
[[nodiscard]] std::optional<cad::Bounds> extents_of(const doc::Doc& doc);

/// arcBeziers(cx, cy, r, a, b): cubic Béziers for an arc from a to b, ≤ 90° each: [x1 y1 x2 y2 x3 y3].
[[nodiscard]] std::vector<std::array<double, 6>> arc_beziers(double cx, double cy, double r, double a, double b);
/// winAnsi(s): the PDF literal (escaped, WinAnsiEncoding), its width (1/1000 em) and the characters it had no glyph
/// for.
struct WinAnsi {
  std::string str;
  double width = 0;
  std::size_t missing = 0;
};
[[nodiscard]] WinAnsi win_ansi(std::string_view s);

}  // namespace cc::plot
