// SPDX-License-Identifier: Apache-2.0
// plot.js in C++: Windows-1252 text, arcs as Béziers, a model plot and a sheet plot as PDF files whose structure holds
// (the byte-for-byte check against 0.5 is tools/parity/plot.mjs over the corpus).
#include "cc/plot/plot.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/sheets/sheets.hpp"

namespace {

std::string text_of(const std::vector<std::uint8_t>& b) {
  return {b.begin(), b.end()};
}

cc::doc::Doc small_drawing() {
  cc::doc::Doc d;
  d.change("DRAW", [&] {
    (void)d.add(cc::base::make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 100}, {"y2", 50}}));
    (void)d.add(cc::base::make_object({{"type", "circle"}, {"cx", 50}, {"cy", 25}, {"r", 10}}));
    (void)d.add(cc::base::make_object({{"type", "text"},
                                       {"x", 5},
                                       {"y", 40},
                                       {"h", 5},
                                       {"rot", 0},
                                       {"s", "Caf\xC3\xA9 (\xE2\x82\xAC) \xE6\xBC\xA2"},
                                       {"ha", "left"},
                                       {"va", "baseline"},
                                       {"wf", 1}}));
  });
  return d;
}

}  // namespace

TEST_CASE("plot: text in Windows-1252, escaped, with what has no glyph counted") {
  const cc::plot::WinAnsi a = cc::plot::win_ansi("a(b)\\c");
  CHECK(a.str == "a\\(b\\)\\\\c");
  CHECK(a.missing == 0);
  const cc::plot::WinAnsi e = cc::plot::win_ansi("\xE2\x82\xAC \xC3\xA9 \xE6\xBC\xA2");  // € é 漢
  CHECK(e.str == "\\200 \\351 ?");
  CHECK(e.missing == 1);
  const cc::plot::WinAnsi m = cc::plot::win_ansi("\xE2\x88\x92\xE2\x8C\x80");  // − ⌀ → - Ø
  CHECK(m.str == "-\\330");
}

TEST_CASE("plot: arcs become quarter Béziers at most") {
  CHECK(cc::plot::arc_beziers(0, 0, 1, 0, std::numbers::pi).size() == 2);
  const auto q = cc::plot::arc_beziers(0, 0, 10, 0, std::numbers::pi / 2);
  REQUIRE(q.size() == 1);
  CHECK(q[0][4] == Catch::Approx(0).margin(1e-9));
  CHECK(q[0][5] == Catch::Approx(10));
}

TEST_CASE("plot: the model at its extents, and a new sheet, as PDF files") {
  cc::doc::Doc d = small_drawing();
  const auto ext = cc::plot::extents_of(d);
  REQUIRE(ext.has_value());
  const cc::cad::Bounds area = ext.value_or(cc::cad::Bounds{});
  CHECK(area.min_x <= 0);
  CHECK(area.max_x >= 100);
  const cc::plot::Sheet sheet = cc::plot::model_sheet(d, area);
  REQUIRE(sheet.scale.has_value());
  CHECK(sheet.scale.value_or(cc::plot::ScaleInfo{}).label.ends_with("(fitted)"));
  const cc::plot::DisplayList list = cc::plot::display_list(d, sheet);
  REQUIRE(list.views.size() == 1);
  CHECK(list.views[0].texts.size() == 1);
  const cc::plot::PdfResult pdf = cc::plot::to_pdf(list, {.pen = 0.25, .title = {}, .date_ms = 0, .compress = {}});
  const std::string s = text_of(pdf.bytes);
  CHECK(s.starts_with("%PDF-1.4\n"));
  CHECK(s.ends_with("%%EOF\n"));
  CHECK(s.contains("/CreationDate (D:19700101000000Z)"));
  CHECK(s.contains("/BaseFont /Helvetica /Encoding /WinAnsiEncoding"));
  CHECK(s.contains("(Caf\\351 \\(\\200\\) ?) Tj"));
  CHECK(pdf.missing == 1);
  CHECK(pdf.texts == 1);
  CHECK_FALSE(pdf.compressed);
  // the xref's offsets point at the objects
  const std::size_t x = s.rfind("startxref\n");
  REQUIRE(x != std::string::npos);
  const std::size_t at = std::stoul(s.substr(x + 10));
  CHECK(s.substr(at, 5) == "xref\n");
  const std::size_t first = s.find("0000000000 65535 f \n", at);
  REQUIRE(first != std::string::npos);
  const std::size_t obj1 = std::stoul(s.substr(first + 20, 10));
  CHECK(s.substr(obj1, 8) == "1 0 obj\n");

  // a sheet: the title block on paper, the viewport onto the model
  const cc::sheets::SheetContents made =
      cc::sheets::sheet_contents(d, cc::sheets::make_paper("iso-a3"), {.title = "T", .sheet_no = 1, .date_ms = 0}, {});
  auto map = std::make_shared<cc::doc::EntityMap>();
  d.change("LAYOUT", [&] {
    d.set_layouts({{.name = "Layout1", .paper = made.paper, .entities = map}});
    for (const cc::base::Value& e : made.entities) {
      (void)d.add(e, map);
    }
  });
  const cc::plot::Sheet ls = cc::plot::layout_sheet(d, d.layouts().front());
  CHECK(ls.w == Catch::Approx(420));
  CHECK(ls.h == Catch::Approx(297));
  REQUIRE(ls.views.size() == 2);  // the viewport's view, then the paper
  CHECK(ls.views[0].clip);
  CHECK_FALSE(ls.views[1].clip);
  const cc::plot::PdfResult two = cc::plot::to_pdf_pages(
      {cc::plot::display_list(d, ls), list}, {.pen = 0.35, .title = "Set", .date_ms = 0, .compress = {}}, {"A", "B"});
  const std::string t = text_of(two.bytes);
  CHECK(two.pages == 2);
  CHECK(t.contains("/Outlines 5 0 R /PageMode /UseOutlines"));
  CHECK(t.contains("/Title (A) /Parent 5 0 R"));
}
