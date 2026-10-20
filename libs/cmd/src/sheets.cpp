// SPDX-License-Identifier: Apache-2.0
// sheets.js (CadCraft 0.5) in C++: paper sizes, drawing scales, new sheets.
#include "cc/cmd/sheets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"

namespace cc::cmd::sheets {

namespace {

using base::Value;

/// Number(n.toPrecision(6)).toString()
std::string fmt_n(double n) {
  std::string s;
  base::append_number(s, base::js_number(base::js_to_exponential(n, 5)));
  return s;
}
std::string ratio_label(double n) {
  return n >= 1 ? "1:" + fmt_n(n) : fmt_n(1 / n) + ":1";
}
std::string units_of(const doc::Doc& doc) {
  const Value u = doc.meta.get("units");
  return u.is_string() ? u.as_string() : std::string();
}

}  // namespace

const std::vector<Paper>& papers() {
  static const std::vector<Paper> kPapers{
      {.key = "iso-a4", .label = "ISO A4 (297 \xC3\x97 210 mm)", .label_key = "", .w = 297, .h = 210, .unit = "mm"},
      {.key = "iso-a3", .label = "ISO A3 (420 \xC3\x97 297 mm)", .label_key = "", .w = 420, .h = 297, .unit = "mm"},
      {.key = "iso-a2", .label = "ISO A2 (594 \xC3\x97 420 mm)", .label_key = "", .w = 594, .h = 420, .unit = "mm"},
      {.key = "iso-a1", .label = "ISO A1 (841 \xC3\x97 594 mm)", .label_key = "", .w = 841, .h = 594, .unit = "mm"},
      {.key = "iso-a0", .label = "ISO A0 (1189 \xC3\x97 841 mm)", .label_key = "", .w = 1189, .h = 841, .unit = "mm"},
      {.key = "letter", .label = "", .label_key = "sheets.paper.letter", .w = 11, .h = 8.5, .unit = "in"},
      {.key = "legal", .label = "", .label_key = "sheets.paper.legal", .w = 14, .h = 8.5, .unit = "in"},
      {.key = "ansi-b", .label = "", .label_key = "sheets.paper.ansi_b", .w = 17, .h = 11, .unit = "in"},
      {.key = "ansi-c", .label = "ANSI C (22 \xC3\x97 17 in)", .label_key = "", .w = 22, .h = 17, .unit = "in"},
      {.key = "ansi-d", .label = "ANSI D (34 \xC3\x97 22 in)", .label_key = "", .w = 34, .h = 22, .unit = "in"},
      {.key = "arch-c", .label = "ARCH C (24 \xC3\x97 18 in)", .label_key = "", .w = 24, .h = 18, .unit = "in"},
      {.key = "arch-d", .label = "ARCH D (36 \xC3\x97 24 in)", .label_key = "", .w = 36, .h = 24, .unit = "in"},
      {.key = "arch-e", .label = "ARCH E (48 \xC3\x97 36 in)", .label_key = "", .w = 48, .h = 36, .unit = "in"}};
  return kPapers;
}

const Paper* paper_by_key(std::string_view key) {
  for (const Paper& p : papers()) {
    if (p.key == key) {
      return &p;
    }
  }
  return nullptr;
}

std::string paper_label(const Paper& p, const i18n::Catalog& words) {
  return p.label_key.empty() ? std::string(p.label) : words.t(p.label_key);
}

std::optional<double> unit_mm(std::string_view unit) {
  static constexpr std::array<std::pair<std::string_view, double>, 12> kUnitMm{{{"millimeters", 1},
                                                                                {"centimeters", 10},
                                                                                {"meters", 1000},
                                                                                {"kilometers", 1e6},
                                                                                {"inches", 25.4},
                                                                                {"feet", 304.8},
                                                                                {"yards", 914.4},
                                                                                {"miles", 1609344},
                                                                                {"microinches", 25.4e-6},
                                                                                {"mils", 0.0254},
                                                                                {"mm", 1},
                                                                                {"in", 25.4}}};
  for (const auto& [name, mm] : kUnitMm) {
    if (name == unit) {
      return mm;
    }
  }
  return std::nullopt;
}

const Paper* match_paper(double w, double h, std::string_view unit) {
  const double k = unit_mm(unit).value_or(1);
  const double a = std::max(w, h) * k;
  const double b = std::min(w, h) * k;
  for (const Paper& p : papers()) {
    const double pk = unit_mm(p.unit).value_or(1);
    if (std::abs(p.w * pk - a) <= a * 0.005 && std::abs(p.h * pk - b) <= b * 0.005) {
      return &p;
    }
  }
  return nullptr;
}

bool is_imperial(const doc::Doc& doc) {
  const std::string u = units_of(doc);
  return u == "inches" || u == "feet" || u == "yards" || u == "miles" || u == "microinches" || u == "mils";
}
double model_mm(const doc::Doc& doc, std::string_view paper_unit) {
  if (const auto m = unit_mm(units_of(doc)); m && *m != 0) {
    return *m;
  }
  if (const auto m = unit_mm(paper_unit); m && *m != 0) {
    return *m;
  }
  return 1;
}
double s_of(const doc::Doc& doc, std::string_view paper_unit, double n) {
  return model_mm(doc, paper_unit) / unit_mm(paper_unit).value_or(std::nan("")) / n;
}
double n_of(const doc::Doc& doc, std::string_view paper_unit, double s) {
  return model_mm(doc, paper_unit) / unit_mm(paper_unit).value_or(std::nan("")) / s;
}

std::vector<Scale> scale_list(const doc::Doc& doc) {
  static constexpr std::array<double, 20> kMetric{1.0 / 10, 1.0 / 5, 1.0 / 2, 1,   2,    5,    10,   20,   25,   50,
                                                  100,      200,     250,     500, 1000, 1250, 2000, 2500, 5000, 10000};
  std::vector<Scale> list;
  list.reserve(kMetric.size());
  for (const double n : kMetric) {
    list.push_back({.n = n, .label = ratio_label(n), .s = 0});
  }
  if (!is_imperial(doc)) {
    return list;
  }
  static constexpr std::array<std::pair<std::string_view, double>, 17> kArch{{{"1/16\" = 1'-0\"", 192},
                                                                              {"3/32\" = 1'-0\"", 128},
                                                                              {"1/8\" = 1'-0\"", 96},
                                                                              {"3/16\" = 1'-0\"", 64},
                                                                              {"1/4\" = 1'-0\"", 48},
                                                                              {"3/8\" = 1'-0\"", 32},
                                                                              {"1/2\" = 1'-0\"", 24},
                                                                              {"3/4\" = 1'-0\"", 16},
                                                                              {"1\" = 1'-0\"", 12},
                                                                              {"1-1/2\" = 1'-0\"", 8},
                                                                              {"3\" = 1'-0\"", 4},
                                                                              {"1\" = 10'", 120},
                                                                              {"1\" = 20'", 240},
                                                                              {"1\" = 30'", 360},
                                                                              {"1\" = 40'", 480},
                                                                              {"1\" = 50'", 600},
                                                                              {"1\" = 100'", 1200}}};
  std::vector<Scale> arch;
  arch.reserve(kArch.size() + list.size());
  for (const auto& [label, n] : kArch) {
    arch.push_back({.n = n, .label = std::string(label), .s = 0});
  }
  std::ranges::stable_sort(arch, [](const Scale& a, const Scale& b) { return a.n < b.n; });
  for (Scale& s : list) {
    arch.push_back(std::move(s));
  }
  return arch;
}

std::string scale_label(const doc::Doc& doc, std::string_view paper_unit, double s) {
  const double n = n_of(doc, paper_unit, s);
  for (const Scale& x : scale_list(doc)) {
    if (std::abs(x.n - n) < 1e-6 * n) {
      return x.label;
    }
  }
  return ratio_label(n);
}

Scale best_scale(const doc::Doc& doc, std::string_view paper_unit, double s_fit) {
  std::optional<Scale> best;
  for (const Scale& x : scale_list(doc)) {
    const double s = s_of(doc, paper_unit, x.n);
    if (s <= s_fit * (1 + 1e-9) && (!best || s > best->s)) {
      best = Scale{.n = x.n, .label = x.label, .s = s};
    }
  }
  if (best) {
    return *best;
  }
  const double n = n_of(doc, paper_unit, s_fit);
  return {.n = n, .label = ratio_label(n), .s = s_fit};
}

cad::Bounds model_extents(const doc::Doc& doc) {
  cad::Bounds b;
  const cad::BlockLookup blocks = [&doc](const std::string& n) { return doc.blocks().get(n); };
  doc.model()->each([&](const doc::Id&, const Value& e) {
    if (!doc.visible(e)) {
      return;
    }
    const cad::Bounds eb = cad::entity_bounds(e, blocks);
    if (eb.valid()) {
      b.min_x = std::min(b.min_x, eb.min_x);
      b.min_y = std::min(b.min_y, eb.min_y);
      b.max_x = std::max(b.max_x, eb.max_x);
      b.max_y = std::max(b.max_y, eb.max_y);
    }
  });
  return b;
}

Aim aim_viewport(const doc::Doc& doc, std::string_view paper_unit, double w, double h, const cad::Bounds& win,
                 bool exact) {
  if (!win.valid()) {
    return {.cx = 0, .cy = 0, .s = s_of(doc, paper_unit, 1)};
  }
  const double bw = std::max(win.max_x - win.min_x, 1e-9);
  const double bh = std::max(win.max_y - win.min_y, 1e-9);
  const double s_fit = std::min(w / bw, h / bh);
  return {.cx = (win.min_x + win.max_x) / 2,
          .cy = (win.min_y + win.max_y) / 2,
          .s = exact ? s_fit : best_scale(doc, paper_unit, s_fit).s};
}

const Paper& default_paper(const doc::Doc& doc) {
  return *paper_by_key(is_imperial(doc) ? "ansi-b" : "iso-a3");
}

Value make_paper(std::string_view key, bool portrait) {
  const Paper* p = paper_by_key(key);
  if (p == nullptr) {
    p = &papers()[1];
  }
  return base::make_object({{"key", std::string(p->key)},
                            {"unit", std::string(p->unit)},
                            {"w", portrait ? p->h : p->w},
                            {"h", portrait ? p->w : p->h},
                            {"x0", 0},
                            {"y0", 0}});
}

std::string iso_date(double ms) {
  // days since 1970 → civil date (proleptic Gregorian, UTC)
  const auto days = static_cast<std::int64_t>(std::floor(ms / 86'400'000));
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  return std::to_string(y) + "-" + two(m) + "-" + two(d);
}

SheetContents sheet_contents(const doc::Doc& doc, const Value& paper, const SheetOptions& o,
                             const i18n::Catalog& words) {
  const std::string unit = base::js_string(paper.get("unit"));
  const double k = 1 / unit_mm(unit).value_or(std::nan(""));  // designed in millimetres
  const double pw = base::js_to_number(paper.get("w"));
  const double ph = base::js_to_number(paper.get("h"));
  const double px0 = base::js_to_number(paper.get("x0"));
  const double py0 = base::js_to_number(paper.get("y0"));
  const double big_w = pw / k;
  const double big_h = ph / k;
  const double m = 10;
  base::Array out;
  const std::string layer = words.t("sheets.title_block");
  const auto at = [&](double x, double y) { return std::pair{px0 + x * k, py0 + y * k}; };
  const auto line = [&](double x1, double y1, double x2, double y2) {
    const auto [ax, ay] = at(x1, y1);
    const auto [bx, by] = at(x2, y2);
    out.emplace_back(base::make_object(
        {{"type", "line"}, {"layer", layer}, {"color", nullptr}, {"x1", ax}, {"y1", ay}, {"x2", bx}, {"y2", by}}));
  };
  const auto rect = [&](double x1, double y1, double x2, double y2) {
    base::Array pts;
    for (const auto& [x, y] : {at(x1, y1), at(x2, y1), at(x2, y2), at(x1, y2)}) {
      pts.emplace_back(base::make_object({{"x", x}, {"y", y}, {"b", 0}}));
    }
    out.emplace_back(base::make_object(
        {{"type", "poly"}, {"layer", layer}, {"color", nullptr}, {"closed", true}, {"pts", std::move(pts)}}));
  };
  const auto text = [&](double x, double y, double h, const std::string& s) {
    const auto [tx, ty] = at(x, y);
    out.emplace_back(base::make_object({{"type", "text"},
                                        {"layer", layer},
                                        {"color", nullptr},
                                        {"x", tx},
                                        {"y", ty},
                                        {"h", h * k},
                                        {"rot", 0},
                                        {"s", s},
                                        {"ha", "left"},
                                        {"va", "baseline"},
                                        {"wf", 1}}));
  };

  rect(m, m, big_w - m, big_h - m);
  const double tw = std::min(180.0, big_w - 2 * m);
  const double th = 32;
  const double x0 = big_w - m - tw;
  const double y0 = m;
  rect(x0, y0, big_w - m, y0 + th);
  line(x0, y0 + 14, big_w - m, y0 + 14);
  const double cut = tw * 2 / 3;
  line(x0 + cut, y0 + 14, x0 + cut, y0 + th);
  const std::array<double, 5> cells{0, tw / 3, tw / 2, tw * 3 / 4, tw};
  for (std::size_t i = 1; i + 1 < cells.size(); ++i) {
    line(x0 + cells.at(i), y0, x0 + cells.at(i), y0 + 14);
  }

  // the viewport: the space above the title block, inside the border
  const double vx0 = m + 5;
  const double vx1 = big_w - m - 5;
  const double vy0 = m + th + 5;
  const double vy1 = big_h - m - 5;
  const double vw = (vx1 - vx0) * k;
  const double vh = (vy1 - vy0) * k;
  const Aim aim = aim_viewport(doc, unit, vw, vh, model_extents(doc));
  const auto [vcx, vcy] = at((vx0 + vx1) / 2, (vy0 + vy1) / 2);
  out.emplace_back(base::make_object({{"type", "viewport"},
                                      {"layer", "Viewports"},
                                      {"color", nullptr},
                                      {"x", vcx},
                                      {"y", vcy},
                                      {"w", vw},
                                      {"h", vh},
                                      {"cx", aim.cx},
                                      {"cy", aim.cy},
                                      {"s", aim.s}}));

  const Value name = doc.meta.get("name");
  const std::string doc_name = name.truthy() ? base::js_string(name) : std::string();
  std::string title = doc_name.empty() ? std::string("Drawing1") : doc_name;
  if (o.title) {
    title = *o.title;
  }
  text(x0 + 2, y0 + th - 4, 1.8, "TITLE");
  text(x0 + 2, y0 + 18, 5, title);
  text(x0 + cut + 2, y0 + th - 4, 1.8, "DRAWN BY");
  const std::array<std::pair<std::string, std::string>, 4> fields{{{"PROJECT", doc_name},
                                                                   {"SCALE", scale_label(doc, unit, aim.s)},
                                                                   {"DATE", iso_date(o.date_ms)},
                                                                   {"SHEET", std::to_string(o.sheet_no)}}};
  for (std::size_t i = 0; i < fields.size(); ++i) {
    text(x0 + cells.at(i) + 2, y0 + 10, 1.8, fields.at(i).first);
    text(x0 + cells.at(i) + 2, y0 + 3.5, 2.5, fields.at(i).second);
  }
  // an empty value would be invisible and unpickable: drop it (the cell stays for writing in)
  base::Array kept;
  for (Value& e : out) {
    if (!(e.get("type") == Value("text") && e.get("s") == Value(""))) {
      kept.push_back(std::move(e));
    }
  }
  base::Array layers;
  layers.emplace_back(base::make_object({{"name", layer}, {"color", 7}}));
  layers.emplace_back(base::make_object({{"name", "Viewports"}, {"color", 8}, {"plot", false}}));
  return {.paper = paper, .entities = std::move(kept), .scale = aim, .layers = std::move(layers)};
}

}  // namespace cc::cmd::sheets
