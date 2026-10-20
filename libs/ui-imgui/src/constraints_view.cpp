// SPDX-License-Identifier: Apache-2.0
// gview.js and params-panel.js (CadCraft 0.5) in the viewer. On the drawing (model space only, nothing plotted):
// constraint bars — a row of glyph boxes beside each constrained line, segment, circle or point; coincident ones a
// small square on the point — and dimensional constraints drawn as dimensions in their own colour, "d1=40" (or "fx:
// d1=width/2"), at a fixed size on screen. A click on a glyph or a label selects that constraint (Delete removes it; a
// double click on a label edits it). The Parameters panel (PARAMETERS): every parameter with its expression and value;
// typing an expression links it, typing a number into a driven value solves the parameter behind it (two-way; several
// candidates: it asks which); each edit one undo step (params_doc).
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <imgui.h>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/expr.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/gsolve.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/doc/params_doc.hpp"
#include "cc/ui/viewer.hpp"

namespace cc::ui {

namespace {

namespace gs = cad::gsolve;
namespace gc = doc::gcons;
namespace pd = doc::params_doc;
using base::Value;

constexpr float kBox = 15;

double num(const Value& v) {
  return base::js_to_number(v);
}
std::string round6s(double v) {
  std::string s;
  base::append_number(s, base::js_round(v * 1e6) / 1e6);
  return s;
}

struct Colours {
  ImU32 dim, bar, bar_bg, bg, sel;
};

/// Where a reference's bar sits: a point on it, and which way on screen to push the bar off it (gview.js anchor).
struct BarAnchor {
  double x = 0, y = 0, dx = 0, dy = 0;
};
std::optional<BarAnchor> bar_anchor(const Value& e, const std::string& k) {
  const std::string type = base::js_string(e.get("type"));
  std::string kk = k;
  std::optional<char> kind;
  if (k == "*") {
    if (type == "line") {
      kind = 'L';
      kk = "L";
    } else if (type == "poly") {
      kind = 'L';
      kk = "s0";
    } else if (type == "point") {
      kind = 'P';
      kk = "p";
    } else {
      kind = 'R';
      kk = "R";
    }
  } else {
    kind = gs::ref_kind(&e, Value(k));
  }
  if (kind == 'P') {
    const auto p = gs::ref_point(e, Value(kk));
    if (!p) {
      return std::nullopt;
    }
    return BarAnchor{p->x, p->y, 1, 1};
  }
  if (kind == 'L') {
    const gs::RefLine l = gs::ref_line(e, Value(kk));
    const double len = std::hypot(l.x2 - l.x1, l.y2 - l.y1);
    const double L = len > 0 ? len : 1;
    // screen y runs down: the left-hand normal (−uy, ux) in world is (−uy, −ux) on screen
    return BarAnchor{(l.x1 + l.x2) / 2, (l.y1 + l.y2) / 2, (l.y2 - l.y1) / L, (l.x2 - l.x1) / L};
  }
  if (kind == 'R') {
    const gs::RefCircle c = gs::ref_circle(e, Value(kk));
    return BarAnchor{c.cx + c.r * std::numbers::sqrt2 / 2, c.cy + c.r * std::numbers::sqrt2 / 2, 1, -1};
  }
  return std::nullopt;
}

/// A geometric constraint's glyph in a box (gcons.js GEOMETRIC[type].glyph), drawn in lines: the UI font has
/// letters, not ⊥ ∥ ◎ ⫽ ⇔ ⊕.
void glyph(ImDrawList* dl, const std::string& type, ImVec2 o, ImU32 ink) {
  const auto at = [o](float x, float y) { return ImVec2{o.x + x, o.y + y}; };
  const float w = 1.4F;
  if (type == "perpendicular") {
    dl->AddLine(at(7.5F, 3.5F), at(7.5F, 11.5F), ink, w);
    dl->AddLine(at(3.5F, 11.5F), at(11.5F, 11.5F), ink, w);
  } else if (type == "parallel") {
    dl->AddLine(at(5.5F, 3.5F), at(5.5F, 11.5F), ink, w);
    dl->AddLine(at(9.5F, 3.5F), at(9.5F, 11.5F), ink, w);
  } else if (type == "collinear") {
    dl->AddLine(at(4, 11.5F), at(8, 3.5F), ink, w);
    dl->AddLine(at(7, 11.5F), at(11, 3.5F), ink, w);
  } else if (type == "concentric") {
    dl->AddCircle(at(7.5F, 7.5F), 4.5F, ink, 16, w);
    dl->AddCircle(at(7.5F, 7.5F), 2, ink, 12, w);
  } else if (type == "symmetric") {
    dl->AddLine(at(3, 7.5F), at(12, 7.5F), ink, w);
    dl->AddLine(at(3, 7.5F), at(5.5F, 5), ink, w);
    dl->AddLine(at(3, 7.5F), at(5.5F, 10), ink, w);
    dl->AddLine(at(12, 7.5F), at(9.5F, 5), ink, w);
    dl->AddLine(at(12, 7.5F), at(9.5F, 10), ink, w);
  } else if (type == "fix") {
    dl->AddCircle(at(7.5F, 7.5F), 4.5F, ink, 16, w);
    dl->AddLine(at(7.5F, 3), at(7.5F, 12), ink, w);
    dl->AddLine(at(3, 7.5F), at(12, 7.5F), ink, w);
  } else if (type == "coincident") {
    dl->AddCircleFilled(at(7.5F, 7.5F), 2.5F, ink, 12);
  } else {
    std::string s = "?";
    if (type == "horizontal") {
      s = "H";
    } else if (type == "vertical") {
      s = "V";
    } else if (type == "tangent") {
      s = "T";
    } else if (type == "equal") {
      s = "=";
    }
    const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
    dl->AddText(at(7.5F - sz.x / 2, 7.5F - sz.y / 2), ink, s.c_str());
  }
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — drawParametric, step for step
void Viewer::constraints_overlay(ImDrawList* dl, const std::function<ImVec2(double, double)>& screen) {
  c_hits_.clear();
  if (!cad_ || !cmd_host_ || !cad_->in_model() || !cad_->constraints.is_array() ||
      cad_->constraints.as_array().empty() || space_ != 0) {
    return;
  }
  const cmd::ConstraintView& state = constraint_view();
  const render::Rgba bgc = background();
  const bool dark = bgc.r * 0.3F + bgc.g * 0.59F + bgc.b * 0.11F < 0.5F;
  const Colours col{.dim = dark ? IM_COL32(0xB7, 0x9C, 0xF0, 0xFF) : IM_COL32(0x6A, 0x3F, 0xB5, 0xFF),
                    .bar = dark ? IM_COL32(0xC8, 0xCC, 0xD4, 0xFF) : IM_COL32(0x44, 0x4A, 0x55, 0xFF),
                    .bar_bg = ImGui::ColorConvertFloat4ToU32({bgc.r, bgc.g, bgc.b, 1}),
                    .bg = ImGui::ColorConvertFloat4ToU32({bgc.r, bgc.g, bgc.b, 1}),
                    .sel = IM_COL32(0xFF, 0xB0, 0x20, 0xFF)};
  const cad::BlockLookup blocks = [this](const std::string& n) { return cad_->blocks().get(n); };
  const auto trace = [&](const Value& e, ImU32 c, float width) {
    for (const cad::Prim& p : cad::entity_prims(e, blocks)) {
      if (!p.arc) {
        dl->AddLine(screen(p.x1, p.y1), screen(p.x2, p.y2), c, width);
        continue;
      }
      const int n = std::clamp(static_cast<int>(std::ceil((p.a1 - p.a0) / (2 * std::numbers::pi) * 64)), 2, 64);
      ImVec2 prev = screen(p.cx + p.r * std::cos(p.a0), p.cy + p.r * std::sin(p.a0));
      for (int i = 1; i <= n; ++i) {
        const double a = p.a0 + (p.a1 - p.a0) * i / n;
        const ImVec2 q = screen(p.cx + p.r * std::cos(a), p.cy + p.r * std::sin(a));
        dl->AddLine(prev, q, c, width);
        prev = q;
      }
    }
  };
  const base::Array& cs = cad_->constraints.as_array();
  const auto ref_entity = [this](const Value& r) -> const Value* {
    const double id = gs::ref_id(r);
    return std::isfinite(id) && id >= 0 ? cad_->model()->get(static_cast<doc::Id>(id)) : nullptr;
  };
  // the selected constraint's objects, highlighted
  for (const Value& c : cs) {
    if (base::js_string(c.get("id")) != c_selected_) {
      continue;
    }
    for (const Value& r : c.get("refs").as_array()) {
      if (const Value* e = ref_entity(r)) {
        trace(*e, (col.sel & 0x00FFFFFFU) | 0x80000000U, 3);
      }
    }
  }
  const double zoom = space_ < views_.size() ? views_[space_].zoom : 1;
  // dimensional constraints
  if (state.dims) {
    for (const Value& c : cs) {
      if (!gc::is_dim(c)) {
        continue;
      }
      Value st = cad_->current.get("dim");
      if (!st.is_object() || st.is_point()) {
        st = Value::object();
      }
      const double txt = num(st.get("txt"));
      st.set("scale", 11 / (zoom * (txt != 0 && !std::isnan(txt) ? txt : 2.5)));  // fixed size on screen
      const auto de = gc::dim_entity_for(*cad_, c, &st, *words_);
      if (!de) {
        continue;
      }
      const bool on = base::js_string(c.get("id")) == c_selected_;
      const ImU32 colr = on ? col.sel : col.dim;
      for (const Value& p : cad::dim_geometry(*de)) {
        const std::string t = base::js_string(p.get("type"));
        if (t == "hatch") {
          for (const Value& loop : p.get("loops").as_array()) {
            std::vector<ImVec2> pts;
            const auto& l = loop.as_array();
            for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
              pts.push_back(screen(num(l[i]), num(l[i + 1])));
            }
            if (pts.size() >= 3) {
              dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), colr);
            }
          }
        } else if (t != "text") {
          trace(p, colr, 1);
        }
      }
      for (const Value& p : cad::dim_geometry(*de)) {
        if (base::js_string(p.get("type")) != "text") {
          continue;
        }
        const ImVec2 s = screen(num(p.get("x")), num(p.get("y")));
        const std::string text = base::js_string(p.get("s"));
        const float rot = static_cast<float>(num(p.get("rot")));
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        const std::string ha = base::js_string(p.get("ha"));
        const std::string va = base::js_string(p.get("va"));
        float x0 = 0;
        if (ha == "center") {
          x0 = -sz.x / 2;
        } else if (ha == "right") {
          x0 = -sz.x;
        }
        float y0 = -sz.y + 2;  // alphabetic
        if (va == "middle") {
          y0 = -sz.y / 2;
        } else if (va == "top") {
          y0 = 0;
        }
        // drawn level, then turned about the anchor (the canvas's rotate(−rot))
        const int first = dl->VtxBuffer.Size;
        dl->AddRectFilled({s.x + x0 - 2, s.y + y0 - 1}, {s.x + x0 + sz.x + 2, s.y + y0 + sz.y + 1},
                          (col.bg & 0x00FFFFFFU) | 0xD8000000U);
        dl->AddText({s.x + x0, s.y + y0}, colr, text.c_str());
        const float cr = std::cos(-rot);
        const float sr = std::sin(-rot);
        for (int i = first; i < dl->VtxBuffer.Size; ++i) {
          ImDrawVert& v = dl->VtxBuffer[i];
          const float dx = v.pos.x - s.x;
          const float dy = v.pos.y - s.y;
          v.pos = {s.x + dx * cr - dy * sr, s.y + dx * sr + dy * cr};
        }
        const float c0 = std::cos(rot);
        const float s0 = std::sin(rot);
        const float hx = s.x + (x0 + sz.x / 2) * c0 + (y0 + sz.y / 2) * s0;
        const float hy = s.y - (x0 + sz.x / 2) * s0 + (y0 + sz.y / 2) * c0;
        const float hw = std::abs(c0) * sz.x / 2 + std::abs(s0) * sz.y / 2;
        const float hh = std::abs(s0) * sz.x / 2 + std::abs(c0) * sz.y / 2;
        c_hits_.push_back(
            {.x = hx - hw, .y = hy - hh, .w = 2 * hw, .h = 2 * hh, .cid = base::js_string(c.get("id")), .dim = true});
      }
    }
  }
  if (state.bars == "none") {
    return;
  }
  // coincident: a small square on the point
  for (const Value& c : cs) {
    if (base::js_string(c.get("type")) != "coincident") {
      continue;
    }
    const Value refs = c.get("refs");
    if (std::ranges::any_of(refs.as_array(), [&state](const Value& r) {
          return state.hidden.contains(static_cast<doc::Id>(gs::ref_id(r)));
        })) {
      continue;
    }
    const Value r0 = refs.as_array().front();
    const Value* e = ref_entity(r0);
    const auto p = e != nullptr ? gs::ref_point(*e, r0.get("k")) : std::nullopt;
    if (!p) {
      continue;
    }
    const ImVec2 s = screen(p->x, p->y);
    const std::string id = base::js_string(c.get("id"));
    dl->AddRectFilled({s.x - 3, s.y - 3}, {s.x + 3, s.y + 3}, id == c_selected_ ? col.sel : col.bar);
    c_hits_.push_back({.x = s.x - 5, .y = s.y - 5, .w = 10, .h = 10, .cid = id, .dim = false});
  }
  // bars: one per constrained line / segment / circle / point
  struct Bar {
    Value r;
    std::vector<const Value*> list;
  };
  std::vector<std::pair<std::string, Bar>> bars;
  for (const Value& c : cs) {
    if (gc::is_dim(c) || base::js_string(c.get("type")) == "coincident") {
      continue;
    }
    for (const Value& r : c.get("refs").as_array()) {
      if (state.hidden.contains(static_cast<doc::Id>(gs::ref_id(r)))) {
        continue;
      }
      std::string key;
      base::append_number(key, gs::ref_id(r));
      key += "|" + base::js_string(r.get("k"));
      auto it = std::ranges::find_if(bars, [&key](const auto& b) { return b.first == key; });
      if (it == bars.end()) {
        bars.emplace_back(key, Bar{.r = r, .list = {}});
        it = std::prev(bars.end());
      }
      if (std::ranges::find(it->second.list, &c) == it->second.list.end()) {
        it->second.list.push_back(&c);
      }
    }
  }
  for (const auto& [key, b] : bars) {
    const Value* e = ref_entity(b.r);
    if (e == nullptr) {
      continue;
    }
    const auto at = bar_anchor(*e, base::js_string(b.r.get("k")));
    if (!at) {
      continue;
    }
    const ImVec2 s = screen(at->x, at->y);
    if (s.x < static_cast<float>(cx_) - 50 || s.y < static_cast<float>(cy_) - 50 ||
        s.x > static_cast<float>(cx_ + cw_) + 50 || s.y > static_cast<float>(cy_ + ch_) + 50) {
      continue;
    }
    const float x0 = s.x + static_cast<float>(at->dx) * 14 - static_cast<float>(b.list.size()) * kBox / 2;
    const float y0 = s.y + static_cast<float>(at->dy) * 14 - kBox / 2;
    for (std::size_t i = 0; i < b.list.size(); ++i) {
      const Value& c = *b.list[i];
      const float x = x0 + static_cast<float>(i) * kBox;
      const std::string id = base::js_string(c.get("id"));
      const bool on = id == c_selected_;
      dl->AddRectFilled({x, y0}, {x + kBox, y0 + kBox}, on ? col.sel : col.bar_bg);
      dl->AddRect({x + 0.5F, y0 + 0.5F}, {x + kBox - 0.5F, y0 + kBox - 0.5F}, col.bar);
      glyph(dl, base::js_string(c.get("type")), {x, y0}, on ? col.bg : col.bar);
      c_hits_.push_back({.x = x, .y = y0, .w = kBox, .h = kBox, .cid = id, .dim = false});
    }
  }
}

bool Viewer::constraint_click(double x, double y, bool dbl) {
  const auto fx = static_cast<float>(x);
  const auto fy = static_cast<float>(y);
  for (const bool dim : {true, false}) {  // labels first, then bars
    for (const ConstraintHit& h : c_hits_ | std::views::reverse) {
      if (h.dim == dim && fx >= h.x - 1 && fx <= h.x + h.w + 1 && fy >= h.y - 1 && fy <= h.y + h.h + 1) {
        c_selected_ = h.cid;
        if (dbl && dim) {
          dc_edit_ = h.cid;
          dc_text_.clear();
        }
        redraw_ = std::max(redraw_, 2);
        return true;
      }
    }
  }
  if (!c_selected_.empty()) {
    c_selected_.clear();
    redraw_ = std::max(redraw_, 2);
  }
  return false;
}

bool Viewer::constraint_delete() {
  if (c_selected_.empty() || !cad_ || (engine_ && engine_->busy())) {
    return false;
  }
  const pd::Result r = pd::remove_constraints(*cad_, {c_selected_}, "DELCONSTRAINT", *words_);
  c_selected_.clear();
  if (r.removed > 0) {
    printed_.push_back(words_->tn("cmd.delconstraint.removed", r.removed));
  }
  after_input();
  return true;
}

/// The result of a parameter edit: its message (or error) in the panel and the history
void Viewer::params_said(const pd::Result& r, const std::string& done) {
  if (!r.ok) {
    params_message_ = r.error;
  } else {
    params_message_ = r.message.empty() ? done : r.message;
  }
  if (r.ok && !done.empty()) {
    printed_.push_back("  " + done + (r.message.empty() ? std::string() : " " + r.message));
  } else if (!r.ok) {
    printed_.push_back("  " + r.error);
  }
  after_input();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the panel, row by row
void Viewer::params_panel() {
  if (!params_open_) {
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - props_width() - params_width(), vp->WorkPos.y});
  ImGui::SetNextWindowSize({params_width(), vp->WorkSize.y - status_height()});
  bool open = true;
  ImGui::Begin((words_->t("ui.parameters") + "###params").c_str(), &open,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (!open) {
    params_open_ = false;
  }
  if (!cad_) {
    ImGui::TextWrapped("%s", words_->t("ui.drop_dwg_dxf_open").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
    ImGui::End();
    return;
  }
  const pd::Described d = pd::describe(*cad_, *words_);
  ImGui::TextWrapped("%s", d.status.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (d.rows.empty()) {
    ImGui::TextDisabled("%s", words_->t("params.panel.empty").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  } else if (ImGui::BeginTable("params", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(words_->t("params.panel.name").c_str(), ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn(words_->t("params.panel.expression").c_str());
    ImGui::TableSetupColumn(words_->t("params.panel.value").c_str(), ImGuiTableColumnFlags_WidthFixed, 72);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 22);
    ImGui::TableHeadersRow();
    for (const pd::Row& r : d.rows) {
      ImGui::PushID(r.name.c_str());
      ImGui::TableNextRow();
      const bool selected = r.constraint && base::js_string(r.constraint->get("id")) == c_selected_;
      if (selected) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(0xFF, 0xB0, 0x20, 0x40));
      }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(r.name.c_str());
      if (ImGui::IsItemHovered()) {
        std::string tip = r.note.empty() ? words_->t("params.panel.user_parameter") : r.note;
        if (r.constraint) {
          tip = gc::describe_constraint(*cad_, *r.constraint, *words_);
        }
        ImGui::SetTooltip("%s", tip.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
      }
      // the expression: typed and Enter links it
      ImGui::TableNextColumn();
      std::string& ex = params_expr_[r.name];
      if (!ImGui::IsItemActive() && params_expr_of_[r.name] != r.expr) {
        ex = r.expr;
        params_expr_of_[r.name] = r.expr;
      }
      ImGui::SetNextItemWidth(-1);
      std::array<char, 1024> buf{};
      std::ranges::copy(ex.substr(0, buf.size() - 1), buf.begin());
      if (ImGui::InputText("##expr", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string typed(base::js_trim(std::string(buf.data())));
        const pd::Result res = r.constraint
                                   ? pd::set_dim_input(*cad_, base::js_string(r.constraint->get("id")), typed, *words_)
                                   : pd::set_parameter(*cad_, r.name, typed, std::nullopt, std::nullopt, *words_);
        params_said(res, r.name + " = " + typed + ".");
        params_expr_of_[r.name].clear();
      } else {
        ex = std::string(buf.data());
      }
      if (r.error) {
        const std::string& err = *r.error;
        ImGui::TextColored({0.9F, 0.3F, 0.25F, 1}, "%s", err.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
      }
      // the value: a driven one takes a number (solved back)
      ImGui::TableNextColumn();
      std::string shown = r.value ? round6s(*r.value) : std::string("?");
      if (r.shown) {
        shown = *r.shown;
      }
      if (r.driving) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(shown.c_str());
      } else {
        std::array<char, 64> vbuf{};
        std::ranges::copy(shown.substr(0, vbuf.size() - 1), vbuf.begin());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##value", vbuf.data(), vbuf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
          const auto lit = cad::expr::literal(std::string(vbuf.data()), *words_);
          if (!lit) {
            params_message_ = words_->t("params.panel.type_number");
          } else {
            const std::string cid = r.constraint ? base::js_string(r.constraint->get("id")) : std::string();
            params_drive_ = Drive{.name = r.name, .cid = cid, .value = *lit, .choices = {}};
            run_drive(std::nullopt);
          }
        }
        if (ImGui::IsItemHovered()) {
          const std::string tip = words_->t("params.panel.driven_by", {{"expr", r.expr}});
          ImGui::SetTooltip("%s", tip.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
        }
      }
      // delete: a dimensional row removes its constraint; a used parameter is written into what uses it, on asking
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x")) {
        if (r.constraint) {
          const pd::Result res =
              pd::remove_constraints(*cad_, {base::js_string(r.constraint->get("id"))}, "DELCONSTRAINT", *words_);
          params_said(res, words_->tn("cmd.delconstraint.removed", res.removed));
        } else {
          const pd::Result res = pd::remove_parameter(*cad_, r.name, false, *words_);
          if (!res.ok && res.used) {
            params_inline_ = r.name;
            params_message_ = res.error + " " + words_->t("params.panel.inline_hint");
          } else {
            params_said(res, r.name + " deleted.");
          }
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!params_inline_.empty()) {
    if (ImGui::Button(words_->t("params.panel.write_value_delete").c_str())) {
      const pd::Result res = pd::remove_parameter(*cad_, params_inline_, true, *words_);
      params_said(res, params_inline_ + " deleted.");
      params_inline_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(words_->t("opt.cancel").c_str())) {
      params_inline_.clear();
      params_message_ = words_->t("params.panel.nothing_changed");
    }
  }
  // several driving parameters behind a typed value: which one changes
  if (params_drive_ && params_drive_->choices.size() > 1) {
    const std::string which = words_->t("params.panel.which_parameter_changes");
    ImGui::TextWrapped("%s", which.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
    for (const std::string& n : params_drive_->choices) {
      if (ImGui::Button(n.c_str())) {
        run_drive(n);
        break;
      }
      ImGui::SameLine();
    }
    if (params_drive_ && ImGui::Button(words_->t("opt.cancel").c_str())) {
      params_drive_.reset();
      params_message_ = words_->t("params.panel.nothing_changed");
    }
  }
  // a new user parameter
  ImGui::Separator();
  ImGui::SetNextItemWidth(70);
  ImGui::InputTextWithHint("##newname", words_->t("params.panel.name").c_str(), new_name_.data(), new_name_.size());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-60);
  ImGui::InputTextWithHint("##newexpr", words_->t("params.panel.value_expression").c_str(), new_expr_.data(),
                           new_expr_.size());
  ImGui::SameLine();
  if (ImGui::Button(words_->t("params.panel.add").c_str())) {
    const std::string name(base::js_trim(std::string(new_name_.data())));
    std::string expr(base::js_trim(std::string(new_expr_.data())));
    if (expr.empty()) {
      expr = "0";
    }
    if (!name.empty()) {
      const pd::Result res = pd::set_parameter(*cad_, name, expr, std::nullopt, std::nullopt, *words_);
      params_said(res, name + " = " + expr + ".");
      if (res.ok) {
        new_name_.fill('\0');
        new_expr_.fill('\0');
      }
    }
  }
  if (!params_message_.empty()) {
    ImGui::TextWrapped("%s", params_message_.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  ImGui::End();
}

void Viewer::run_drive(const std::optional<std::string>& prefer) {
  if (!params_drive_ || !cad_) {
    return;
  }
  const Drive dv = *params_drive_;
  const pd::Result res = dv.cid.empty() ? pd::drive_parameter(*cad_, dv.name, dv.value, prefer, *words_)
                                        : pd::drive_constraint(*cad_, dv.cid, dv.value, prefer, *words_);
  if (!res.ok && res.choices.size() > 1) {
    params_drive_->choices = res.choices;
    params_message_ = res.error;
    return;
  }
  params_drive_.reset();
  params_said(res, words_->t("params.panel.set_to", {{"name", dv.name}, {"value", round6s(dv.value)}}));
}

/// A double click on a dimensional constraint's label: its value or expression ("d2=width/2" renames it)
void Viewer::dc_edit_dialog() {
  if (dc_edit_.empty() || !cad_) {
    return;
  }
  const Value* c = nullptr;
  for (const Value& k : cad_->constraints.as_array()) {
    if (base::js_string(k.get("id")) == dc_edit_) {
      c = &k;
    }
  }
  if (c == nullptr) {
    dc_edit_.clear();
    return;
  }
  const std::string name = base::js_string(c->get("name"));
  if (dc_text_.empty()) {
    const cad::params::Params p = cad::params::from_value(cad_->params);
    const cad::params::Var* v = cad::params::var_by_name(p, name);
    dc_text_ = name + "=" + (v != nullptr ? v->expr : round6s(num(c->get("value"))));
  }
  ImGui::OpenPopup("###dcedit");
  if (ImGui::BeginPopupModal((words_->t("params.panel.dc_title", {{"name", name}}) + "###dcedit").c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(words_->t("params.panel.value_expression_number_driven").c_str());
    std::array<char, 512> buf{};
    std::ranges::copy(dc_text_.substr(0, buf.size() - 1), buf.begin());
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    const bool enter = ImGui::InputText("##dc", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    dc_text_ = std::string(buf.data());
    const std::string about = gc::describe_constraint(*cad_, *c, *words_) + ".";
    ImGui::TextDisabled("%s", about.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (!dc_error_.empty()) {
      ImGui::TextColored({0.9F, 0.3F, 0.25F, 1}, "%s", dc_error_.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    bool close = false;
    if (ImGui::Button(words_->t("opt.cancel").c_str()) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply") || enter) {
      const pd::Result r = pd::set_dim_input(*cad_, dc_edit_, dc_text_, *words_);
      if (r.ok) {
        params_said(r, dc_text_ + ".");
        close = true;
      } else {
        dc_error_ = r.error;
      }
    }
    if (close) {
      dc_edit_.clear();
      dc_text_.clear();
      dc_error_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace cc::ui
