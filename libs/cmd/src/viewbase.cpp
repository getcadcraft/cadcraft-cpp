// SPDX-License-Identifier: Apache-2.0
// VIEWBASE (FLATSHOT), flatview.js installFlatView: a flat 2D view of the 3D model — which view, hidden lines shown or
// not, the scale (on a sheet the largest standard scale that fits), where — placed as one block reference
// "VIEW-<View>-<n>": visible edges on VIEW-VISIBLE (0.5 mm), hidden ones on VIEW-HIDDEN (linetype HIDDEN, 0.25 mm). One
// undo step; a snapshot (change the solids, run it again).
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/m3d/drawing3d.hpp"
#include "cc/m3d/flatview.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

using base::Value;

constexpr std::array<double, 17> kStd{10,        5,          2,          1,          1.0 / 2,    1.0 / 5,
                                      1.0 / 10,  1.0 / 20,   1.0 / 25,   1.0 / 50,   1.0 / 100,  1.0 / 200,
                                      1.0 / 500, 1.0 / 1000, 1.0 / 2000, 1.0 / 5000, 1.0 / 10000};

std::string num(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}
std::string scale_label(double s) {
  return s >= 1 ? num(base::js_round(s * 1000) / 1000) + ":1" : "1:" + num(base::js_round(1000 / s) / 1000);
}

/// layertools.js's opt (commands.js's commandRegistry.option): the label as given
Option opt(std::string key, std::string label) {
  return {.key = std::move(key), .label = std::move(label), .en = {}, .hidden = false};
}

struct ViewKey {
  std::string_view key, name;
};
constexpr std::array<ViewKey, 7> kViews{
    {{"F", "front"}, {"T", "top"}, {"R", "right"}, {"L", "left"}, {"BA", "back"}, {"BO", "bottom"}, {"I", "iso"}}};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS command, prompt by prompt
Task<void> viewbase_cmd(Ctx& c) {
  if (c.doc.model3d.empty()) {
    c.print(c.t("viewbase.empty"));
    co_return;
  }
  Ask a1;
  for (const ViewKey& v : kViews) {
    a1.options.push_back(opt(std::string(v.key), c.t("viewbase.view." + std::string(v.name))));
  }
  a1.def = "F";
  const Answer k1 = co_await c.keyword(c.t("viewbase.which"), std::move(a1));
  const std::string o1 = k1.key();
  if (o1.empty()) {
    co_return;
  }
  std::string view = "front";
  const std::string up = base::js_to_upper(o1);
  for (const ViewKey& v : kViews) {
    if (v.key == up) {
      view = std::string(v.name);
    }
  }
  Ask a2;
  a2.options = {opt("S", c.t("opt.show")), opt("H", c.t("opt.hide"))};
  a2.def = "S";
  const Answer k2 = co_await c.keyword(c.t("viewbase.hidden"), std::move(a2));
  const std::string o2 = k2.key();
  if (o2.empty()) {
    co_return;
  }
  const m3d::flat::Projected pr = m3d::flat::project_model(c.doc.model3d, view, o2 != "H");
  if (!pr.box) {
    c.print(c.t("viewbase.empty"));
    co_return;
  }

  // drawing units per meter of the model, times the view's scale (sheets: paper units)
  const doc::Layout* L = c.doc.layout();
  const Value units = c.doc.meta.get("units");
  const double k = m3d::m_per_unit(units.truthy() ? base::js_string(units) : std::string());
  const auto paper_num = [L](std::string_view f, double def) {
    if (L == nullptr) {
      return def;
    }
    const double v = base::js_to_number(L->paper.get(f));
    return v != 0 && !std::isnan(v) ? v : def;
  };
  const bool inches = L != nullptr && base::js_string(L->paper.get("unit")) == "in";
  double paper_per_m = 1 / k;
  if (L != nullptr) {
    paper_per_m = inches ? 1 / 0.0254 : 1000;
  }
  const auto& box = *pr.box;
  const double wM = box[2] - box[0];
  const double hM = box[3] - box[1];
  double scale = 1;
  if (L != nullptr) {
    const double pw = paper_num("w", 420);
    const double ph = paper_num("h", 297);
    scale = kStd.back();
    for (const double s : kStd) {
      if (wM * paper_per_m * s <= pw * 0.45 && hM * paper_per_m * s <= ph * 0.45) {
        scale = s;
        break;
      }
    }
  }
  for (;;) {
    const Answer v = co_await c.string(c.t("viewbase.scale", {{"scale", scale_label(scale)}}));
    if (v.kind != Answer::Kind::text) {
      co_return;
    }
    if (base::js_blank(v.text)) {
      break;
    }
    if (const auto s = m3d::flat::parse_scale(v.text)) {
      scale = *s;
      break;
    }
    c.print(c.t("viewbase.badscale"));
  }
  const double f = paper_per_m * scale;
  const double cu = (box[0] + box[2]) / 2;
  const double cv = (box[1] + box[3]) / 2;
  base::Array ents;
  const auto line = [&](const char* layer, const std::array<double, 4>& s) {
    ents.emplace_back(base::make_object({{"type", "line"},
                                         {"layer", layer},
                                         {"x1", (s[0] - cu) * f},
                                         {"y1", (s[1] - cv) * f},
                                         {"x2", (s[2] - cu) * f},
                                         {"y2", (s[3] - cv) * f}}));
  };
  for (const auto& s : pr.visible) {
    line("VIEW-VISIBLE", s);
  }
  for (const auto& s : pr.hidden) {
    line("VIEW-HIDDEN", s);
  }
  const double hw = wM * f / 2;
  const double hh = hM * f / 2;
  c.preview([hw, hh](Pt cur) {
    base::Array out;
    const std::array<std::array<double, 4>, 4> sides{
        {{-hw, -hh, hw, -hh}, {hw, -hh, hw, hh}, {hw, hh, -hw, hh}, {-hw, hh, -hw, -hh}}};
    for (const auto& [x1, y1, x2, y2] : sides) {
      out.emplace_back(base::make_object({{"type", "line"},
                                          {"layer", "0"},
                                          {"x1", cur.x + x1},
                                          {"y1", cur.y + y1},
                                          {"x2", cur.x + x2},
                                          {"y2", cur.y + y2}}));
    }
    return out;
  });
  const Answer at = co_await c.point(c.t("viewbase.at"));
  c.preview(nullptr);
  if (at.kind != Answer::Kind::point) {
    co_return;
  }
  std::string base_name = "VIEW-";
  for (const char ch : base::js_to_upper(c.t("viewbase.view." + view))) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      base_name += ch;
    }
  }
  int n = 1;
  while (c.doc.blocks().get(base_name + "-" + std::to_string(n)) != nullptr) {
    ++n;
  }
  const std::string name = base_name + "-" + std::to_string(n);
  const bool any_hidden = !pr.hidden.empty();
  c.doc.change("VIEWBASE", [&] {
    if (c.doc.layers().get("VIEW-VISIBLE") == nullptr) {
      c.doc.set_layer("VIEW-VISIBLE", base::make_object({{"color", 7}, {"lineweight", 0.5}}));
    }
    if (any_hidden && c.doc.layers().get("VIEW-HIDDEN") == nullptr) {
      c.doc.set_layer("VIEW-HIDDEN", base::make_object({{"color", 8}, {"linetype", "HIDDEN"}, {"lineweight", 0.25}}));
    }
    (void)c.doc.define_block(base::make_object(
        {{"name", name}, {"base", base::make_object({{"x", 0}, {"y", 0}})}, {"entities", Value(std::move(ents))}}));
    (void)c.doc.add(base::make_object({{"type", "insert"},
                                       {"layer", c.doc.current.get("layer")},
                                       {"block", name},
                                       {"x", at.p.x},
                                       {"y", at.p.y},
                                       {"sx", 1},
                                       {"sy", 1},
                                       {"rot", 0}}));
  });
  c.print(c.t("viewbase.done", {{"name", name},
                                {"view", c.t("viewbase.view." + view)},
                                {"scale", scale_label(scale)},
                                {"visible", std::to_string(pr.visible.size())},
                                {"hidden", std::to_string(pr.hidden.size())}}));
}

}  // namespace

void add_viewbase(std::vector<CommandDef>& t) {
  t.push_back(def("VIEWBASE", {"FLATSHOT"}, viewbase_cmd, "viewbase.help"));
}

}  // namespace cc::cmd::detail
