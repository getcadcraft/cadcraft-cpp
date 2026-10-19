// SPDX-License-Identifier: Apache-2.0
// The Properties panel (CadCraft 0.5 props.js) and Quick Properties (QPMODE) in Dear ImGui: the selection's groups,
// each section's fields from cmd::props (one control per kind), the value every object of the group shares ("Varies"
// otherwise), a typed value read as propexpr.js reads it and set on the whole group in one undo step, a pick button
// beside each point (_PROPPICK). The panel holds ids only; every edit looks the objects up again.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "cc/base/js.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cmd/props.hpp"
#include "cc/ui/viewer.hpp"

namespace cc::ui {

namespace {

using base::Value;
using Choices = std::vector<std::pair<Value, std::string>>;

bool numeric(const std::string& kind) {
  return kind == "len" || kind == "ang" || kind == "num" || kind == "int" || kind == "area";
}
/// ui-util.js fmt(n)
std::string fmt(double n) {
  if (!std::isfinite(n)) {
    return {};
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(std::abs(n) < 1e-10 ? 0 : n, 4)));
  return s;
}
/// QP_DEFAULT: the fields Quick Properties shows per type
std::vector<std::string> quick_keys(const std::string& key) {
  static const std::vector<std::pair<std::string_view, std::vector<std::string>>> kQuick{
      {"line", {"layer", "color", "len", "ang"}},
      {"circle", {"layer", "color", "r", "area"}},
      {"arc", {"layer", "color", "r", "sweep", "arclen"}},
      {"ellipse", {"layer", "color", "major", "minor"}},
      {"poly", {"layer", "color", "closed", "gw", "plen", "area"}},
      {"text", {"layer", "color", "s", "h", "rot", "just"}},
      {"insert", {"layer", "color", "block", "rot", "us"}},
      {"hatch", {"layer", "color", "pattern", "pscale", "pangle"}},
      {"dim", {"layer", "color", "dimstyle", "measured", "dim.text", "dim.dec"}},
      {"mleader", {"layer", "color", "s", "h"}},
      {"point", {"layer", "color", "pos.x", "pos.y"}},
      {"viewport", {"layer", "on", "locked", "vw", "vh"}}};
  for (const auto& [type, keys] : kQuick) {
    if (type == key) {
      return keys;
    }
  }
  return {"layer", "color", "lt"};
}
std::string sig_of(const std::vector<doc::Id>& ids) {
  std::string s;
  for (const doc::Id id : ids) {
    s += std::to_string(id) + ",";
  }
  return s;
}
/// A combo over `opts`: true and `out` set when a choice is made. `cur` nothing: varies.
bool combo(const char* id, const Choices& opts, const std::optional<Value>& cur, const std::string& varies,
           Value& out) {
  std::string preview = varies;
  if (cur) {
    preview = base::js_string(*cur);
    for (const auto& [v, label] : opts) {
      if (v == *cur && v.is_string() == cur->is_string()) {
        preview = label;
        break;
      }
    }
  }
  bool picked = false;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo(id, preview.c_str())) {
    for (std::size_t i = 0; i < opts.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      const bool sel = cur && opts[i].first == *cur;
      if (ImGui::Selectable(opts[i].second.c_str(), sel)) {
        out = opts[i].first;
        picked = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return picked;
}

}  // namespace

std::string Viewer::prop_show(const cmd::props::Def& d, double v) const {
  if (!std::isfinite(v)) {
    return {};
  }
  const bool sheet = cad_ && cad_->layout() != nullptr;
  const Value lunits = cad_ ? cad_->meta.get("lunits") : Value(2);
  const Value lu = lunits.is_undefined() || lunits.is_null() ? Value(2) : lunits;
  const Value prec = cad_ ? cad_->meta.get("luprec") : Value();
  if (d.kind == "len" || d.kind == "ro-len") {
    return sheet && !d.model ? fmt(v) : cad::format_length(v, lu, prec);
  }
  if (d.kind == "area" || d.kind == "ro-area") {
    if (sheet) {
      return fmt(v);
    }
    const int code = cad::length_code(lu);
    const int p = cad::length_prec(prec);
    if (code == 3 || code == 4) {
      return words_->t("units.area.imperial", {{"sqin", cad::format_decimals(v, std::max(p, 2))},
                                               {"sqft", cad::format_decimals(v / 144, std::max(p, 2))}});
    }
    if (code == 1) {
      return cad::format_length(v, Value(1), Value(p));
    }
    return cad::format_decimals(v, code == 5 ? 4 : p);
  }
  if (d.kind == "ang") {
    return fmt(v) + "\xC2\xB0";
  }
  return fmt(v);
}

cmd::props::Ctx Viewer::props_ctx(const std::vector<doc::Id>& ids) const {
  cmd::props::Ctx ctx{.doc = cad_.get(), .words = words_.get(), .ents = {}, .vertex = 0};
  for (const doc::Id id : ids) {
    if (const Value* e = cad_->entities().get(id)) {
      ctx.ents.push_back(*e);
    }
  }
  if (ctx.ents.size() == 1 && props_vertex_of_ == ids.front()) {
    ctx.vertex = props_vertex_;
  }
  return ctx;
}

void Viewer::prop_apply(const std::string& group, const std::vector<doc::Id>& ids, const std::string& key,
                        const Value& raw) {
  if (!cad_) {
    return;
  }
  const cmd::props::Ctx ctx = props_ctx(ids);
  const std::vector<cmd::props::Def> defs = cmd::props::defs_for(group, ctx);
  const auto d = std::ranges::find_if(defs, [&key](const cmd::props::Def& x) { return x.key == key; });
  if (d == defs.end()) {
    return;
  }
  Value v = raw;
  if (numeric(d->kind)) {
    const cmd::ReadCtx rc{
        .lunits = cad_->meta.get("lunits"),
        .units = cad_->meta.get("units").is_string() ? cad_->meta.get("units").as_string() : std::string(),
        .paper =
            cad_->layout() != nullptr && !d->model ? base::js_string(cad_->layout()->paper.get("unit")) : std::string(),
        .gt0 = d->gt0,
        .min = d->min,
        .max = d->max,
        .vars = {}};
    const cmd::ReadResult r = cmd::read_value(base::js_string(raw), d->kind == "area" ? "num" : d->kind, rc, *words_);
    if (!r.ok) {
      message_ = r.error;
      return;
    }
    v = Value(r.value);
  }
  const cmd::props::Applied r = cmd::props::apply_value(*cad_, *d, v, ctx);
  if (!r.ok) {
    message_ = r.error;
  } else if (r.locked > 0) {
    message_ = words_->tn("props.locked", static_cast<double>(r.locked), {});
  } else {
    message_.clear();
  }
  after_input();
}

/// One field's control. `group` / `ids`: what an edit applies to; `uid`: the control's ImGui id.
void Viewer::prop_field(const cmd::props::Def& d, const cmd::props::Ctx& ctx, const std::string& group,
                        const std::vector<doc::Id>& ids, const std::string& uid) {
  const std::optional<Value> v = cmd::props::value_of(d, ctx);
  const std::string varies = words_->t("props.varies");
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(d.label.c_str());
  if (!d.title.empty() && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", words_->t(d.title).c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  ImGui::TableNextColumn();
  ImGui::PushID(uid.c_str());
  const std::string& k = d.kind;
  Value out;
  if (k == "ro" || k == "ro-len" || k == "ro-area") {
    std::string s = varies;
    if (v) {
      s = v->is_number() ? prop_show(d, v->as_number()) : base::js_string(*v);
    }
    ImGui::TextDisabled("%s", s.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  } else if (k == "bool") {
    bool on = v && v->truthy();
    if (ImGui::Checkbox(v ? "##b" : (varies + "##b").c_str(), &on)) {
      prop_apply(group, ids, d.key, Value(on));
    }
  } else if (k == "choice") {
    if (combo("##c", d.choices ? d.choices(ctx) : Choices{}, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "layer") {
    std::vector<std::string> names;
    cad_->layers().each([&](const std::string& n, const Value&) { names.push_back(n); });
    // NOLINTBEGIN(clang-analyzer-cplusplus.Move) — the analyzer follows the sort's swaps into the comparator
    std::ranges::sort(
        names, [](const std::string& a, const std::string& b) { return base::js_locale_compare_numeric(a, b) < 0; });
    // NOLINTEND(clang-analyzer-cplusplus.Move)
    Choices opts;
    for (const std::string& n : names) {
      opts.emplace_back(Value(n), n);
    }
    if (combo("##l", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "color") {
    Choices opts;
    if (d.none) {
      opts.emplace_back(Value("none"), words_->t("props.color.none"));
    }
    opts.emplace_back(Value(nullptr), words_->t("props.color.bylayer"));
    opts.emplace_back(Value(0), words_->t("props.color.byblock"));
    static constexpr std::array<const char*, 9> kAci{"app.aci.red",         "app.aci.yellow", "app.aci.green",
                                                     "app.aci.cyan",        "app.aci.blue",   "app.aci.magenta",
                                                     "app.ink_black_white", "app.aci.gray",   "app.light_gray"};
    for (std::size_t i = 0; i < kAci.size(); ++i) {
      opts.emplace_back(Value(static_cast<double>(i + 1)), words_->t(kAci.at(i)));
    }
    if (v && std::ranges::none_of(opts, [&v](const auto& o) { return o.first == *v; })) {
      opts.emplace_back(*v, base::js_string(*v));
    }
    if (combo("##k", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "lt") {
    Choices opts{{Value(nullptr), words_->t("lt.bylayer")}, {Value("ByBlock"), words_->t("lt.byblock")}};
    for (const std::string& n : cmd::linetype_names(*cad_)) {
      opts.emplace_back(Value(n), cmd::lt_label(*words_, Value(n)));
    }
    if (combo("##t", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "lw") {
    Choices opts{{Value(nullptr), cmd::lw_label(*words_, Value(nullptr))},
                 {Value(-2), cmd::lw_label(*words_, Value(-2))},
                 {Value(-3), cmd::lw_label(*words_, Value(-3))}};
    for (const double w : cmd::kLineweights) {
      opts.emplace_back(Value(w), cmd::lw_label(*words_, Value(w)));
    }
    if (combo("##w", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "tr") {
    Choices opts{{Value(nullptr), words_->t("props.tr.bylayer")}, {Value("ByBlock"), words_->t("props.tr.byblock")}};
    for (int n = 0; n <= 90; n += 10) {
      opts.emplace_back(Value(n), words_->t("props.tr.pct", {{"n", std::to_string(n)}}));
    }
    if (v && std::ranges::none_of(opts, [&v](const auto& o) { return o.first == *v; })) {
      opts.emplace_back(*v, words_->t("props.tr.pct", {{"n", base::js_string(*v)}}));
    }
    if (combo("##r", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "mat") {
    Choices opts{{Value(nullptr), words_->t("props.mat.bylayer")},
                 {Value("ByBlock"), words_->t("props.mat.byblock")},
                 {Value("Global"), words_->t("props.mat.global")}};
    if (v && v->is_string() && std::ranges::none_of(opts, [&v](const auto& o) { return o.first == *v; })) {
      opts.emplace_back(*v, v->as_string());
    }
    if (combo("##m", opts, v, varies, out)) {
      prop_apply(group, ids, d.key, out);
    }
  } else if (k == "vertex") {
    const std::size_t n = ctx.ents.empty() ? 0 : ctx.ents.front().get("pts").as_array().size();
    const int at = v ? static_cast<int>(base::js_to_number(*v)) : 1;
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left) && at > 1) {
      props_vertex_ = at - 2;
      props_vertex_of_ = ids.front();
    }
    ImGui::SameLine();
    ImGui::Text("%d %s", at, words_->t("props.vertex.of", {{"n", std::to_string(n)}}).c_str());  // NOLINT
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right) && std::cmp_less(at, n)) {
      props_vertex_ = at;
      props_vertex_of_ = ids.front();
    }
  } else {
    // a typed value (numbers, lengths, angles) or text: shown formatted, edited as editText / the text itself
    const bool multi = k == "textarea";
    const bool editing = prop_active_ == uid;
    if (!editing) {
      std::string shown;
      if (v && v->is_number() && numeric(k)) {
        shown = prop_show(d, v->as_number());
      } else if (v && !v->is_undefined() && !v->is_null()) {
        shown = base::js_string(*v);
      }
      std::array<char, 256> view{};
      const std::size_t len = std::min(shown.size(), view.size() - 1);
      std::memcpy(view.data(), shown.data(), len);
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##v", v ? "" : varies.c_str(), view.data(), view.size(), ImGuiInputTextFlags_ReadOnly);
      if (ImGui::IsItemClicked() || (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        prop_active_ = uid;
        prop_buf_.fill('\0');
        std::string start;
        if (v && v->is_number() && numeric(k)) {
          start = cmd::edit_text(v->as_number());
        } else if (v && !v->is_undefined() && !v->is_null()) {
          start = base::js_string(*v);
        }
        std::memcpy(prop_buf_.data(), start.data(), std::min(start.size(), prop_buf_.size() - 1));
        prop_focus_ = true;
      }
    } else {
      if (prop_focus_) {
        ImGui::SetKeyboardFocusHere();
        prop_focus_ = false;
      }
      ImGui::SetNextItemWidth(-1);
      const bool enter =
          multi ? ImGui::InputTextMultiline(
                      "##e", prop_buf_.data(), prop_buf_.size(), {-1, 60},
                      ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue)
                : ImGui::InputText("##e", prop_buf_.data(), prop_buf_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
      const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
      if (enter || (ImGui::IsItemDeactivated() && !esc)) {
        const std::string typed(prop_buf_.data());
        prop_active_.clear();
        prop_apply(group, ids, d.key, Value(typed));
      } else if (esc || ImGui::IsItemDeactivated()) {
        prop_active_.clear();
      }
    }
  }
  ImGui::PopID();
}

void Viewer::prop_rows(const std::vector<cmd::props::Def>& defs, const cmd::props::Ctx& ctx, const std::string& group,
                       const std::vector<doc::Id>& ids, const std::string& prefix, bool pick) {
  if (!ImGui::BeginTable(("##" + prefix).c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.45F);
  ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55F);
  for (const cmd::props::Def& d : defs) {
    if (!d.pt_name.empty() && d.pt_axis == "x") {
      // a point's sub-heading, with the button that picks it in the drawing
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const std::string what = words_->t("props.pt." + d.pt_name);
      ImGui::TextDisabled("%s", what.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
      ImGui::TableNextColumn();
      if (pick && engine_ && !engine_->busy()) {
        ImGui::PushID(("pick." + prefix + d.pt_name).c_str());
        if (ImGui::SmallButton("+")) {
          prop_pick_ = PropPick{.group = group, .name = d.pt_name, .label = what, .ids = ids};
          engine_->run("_PROPPICK");
          after_input();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", words_->t("props.pick.title", {{"what", what}}).c_str());  // NOLINT
        }
        ImGui::PopID();
      }
    }
    prop_field(d, ctx, group, ids, prefix + d.key);
  }
  ImGui::EndTable();
}

void Viewer::props_picked(double x, double y) {
  if (!prop_pick_ || !cad_) {
    return;
  }
  const PropPick pk = std::move(*prop_pick_);
  prop_pick_.reset();
  const cmd::props::Ctx ctx = props_ctx(pk.ids);
  if (ctx.ents.empty()) {
    return;
  }
  std::string err;
  cad_->change("PROPERTIES", [&] {
    for (const cmd::props::Def& d : cmd::props::defs_for(pk.group, ctx)) {
      if (d.pt_name != pk.name || (d.pt_axis != "x" && d.pt_axis != "y")) {
        continue;
      }
      const cmd::props::Applied r =
          cmd::props::apply_value(*cad_, d, Value(d.pt_axis == "x" ? x : y), props_ctx(pk.ids));
      if (!r.ok) {
        err = r.error;
      }
    }
  });
  message_ = err;
  after_input();
}

void Viewer::props_panel() {
  if (!props_open_) {
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - props_width(), vp->WorkPos.y});
  ImGui::SetNextWindowSize({props_width(), vp->WorkSize.y - status_height()});
  bool open = true;
  ImGui::Begin((words_->t("props.title") + "###props").c_str(), &open,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (!open) {
    props_open_ = false;
  }
  const std::vector<doc::Id> ids = props_selection();
  if (!cad_) {
    ImGui::TextWrapped("%s", words_->t("ui.drop_dwg_dxf_open").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  } else if (ids.empty()) {
    props_sig_.clear();
    ImGui::TextDisabled("%s", words_->t("props.current").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  } else {
    cmd::props::Ctx all = props_ctx(ids);
    const std::vector<cmd::props::Group> groups = cmd::props::groups_of(all.ents, *words_);
    const std::string sig = sig_of(ids);
    if (sig != props_sig_) {
      props_sig_ = sig;
      props_group_ = groups.front().key;
      if (ids.size() != 1 || props_vertex_of_ != ids.front()) {
        props_vertex_ = 0;
        props_vertex_of_ = ids.size() == 1 ? std::optional<doc::Id>(ids.front()) : std::nullopt;
      }
    }
    if (std::ranges::none_of(groups, [this](const cmd::props::Group& g) { return g.key == props_group_; })) {
      props_group_ = groups.front().key;
    }
    const auto label_of = [this](const cmd::props::Group& g) {
      return g.key == "all" ? words_->t("props.group.all.n", {{"n", std::to_string(g.n)}})
                            : words_->t("props.group.type.n", {{"type", g.label}, {"n", std::to_string(g.n)}});
    };
    const auto g = std::ranges::find_if(groups, [this](const cmd::props::Group& x) { return x.key == props_group_; });
    if (groups.size() > 1) {
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##group", label_of(*g).c_str())) {
        for (const cmd::props::Group& x : groups) {
          if (ImGui::Selectable(label_of(x).c_str(), x.key == props_group_)) {
            props_group_ = x.key;
          }
        }
        ImGui::EndCombo();
      }
    } else {
      ImGui::TextUnformatted(ids.size() == 1
                                 ? g->label.c_str()
                                 : words_->tn("props.n.objects", static_cast<double>(ids.size()), {}).c_str());
    }
    const std::vector<doc::Id> gids = g->ids;
    const cmd::props::Ctx ctx = props_ctx(gids);
    const std::vector<cmd::props::Def> defs = cmd::props::defs_for(props_group_, ctx);
    for (const std::string_view sec : cmd::props::kSections) {
      std::vector<cmd::props::Def> in;
      for (const cmd::props::Def& d : defs) {
        if (d.sec == sec) {
          in.push_back(d);
        }
      }
      if (in.empty()) {
        continue;
      }
      const std::string head = words_->t("props.sec." + std::string(sec)) + "##sec." + std::string(sec);
      if (ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        prop_rows(in, ctx, props_group_, gids, "p." + std::string(sec) + ".", true);
      }
    }
  }
  ImGui::End();
}

void Viewer::quick_props() {
  const std::vector<doc::Id> ids = props_selection();
  if (!qp_on_ || ids.empty() || !engine_ || engine_->busy()) {
    qp_for_.clear();
    return;
  }
  const std::string sig = sig_of(ids);
  if (sig == qp_dismissed_) {
    return;
  }
  if (sig != qp_for_) {  // placed once per selection, by the cursor
    qp_for_ = sig;
    qp_x_ = static_cast<float>(mouse_x_ + 16);
    qp_y_ = static_cast<float>(mouse_y_ + 16);
  }
  const cmd::props::Ctx all = props_ctx(ids);
  const std::vector<cmd::props::Group> groups = cmd::props::groups_of(all.ents, *words_);
  const cmd::props::Group& g = groups.front();
  const cmd::props::Ctx ctx = props_ctx(g.ids);
  std::vector<cmd::props::Def> defs;
  for (const std::string& key : quick_keys(g.key)) {
    for (cmd::props::Def& d : cmd::props::defs_for(g.key, ctx)) {
      if (d.key == key) {
        defs.push_back(std::move(d));
        break;
      }
    }
  }
  ImGui::SetNextWindowPos({qp_x_, qp_y_}, ImGuiCond_Always);
  ImGui::SetNextWindowSize({touch_ ? 300.0F : 260.0F, 0});
  bool open = true;
  const std::string title =
      (ids.size() == 1 ? g.label : words_->tn("props.n.objects", static_cast<double>(ids.size()), {})) +
      "###quickprops";
  ImGui::Begin(title.c_str(), &open,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings);
  prop_rows(defs, ctx, g.key, g.ids, "qp.", false);
  ImGui::End();
  if (!open) {
    qp_dismissed_ = sig;
  }
}

}  // namespace cc::ui
