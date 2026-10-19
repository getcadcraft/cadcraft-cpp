// SPDX-License-Identifier: Apache-2.0
#include "cc/ui/viewer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/import.hpp"
#include "cc/cad/snap.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/docview/drawing.hpp"
#include "cc/engine/open.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"

namespace cc::ui {

namespace {

ImVec4 im(const render::Rgba& c) {
  return {c.r, c.g, c.b, c.a};
}
render::Rgba rgba(std::string_view hex) {
  return render::parse_hex(hex);
}
std::string fixed(double v, int places) {
  if (!std::isfinite(v)) {
    return "–";
  }
  std::array<char, 64> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%.*f", places, v);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  return n > 0 ? std::string(buf.data(), static_cast<std::size_t>(std::min<int>(n, static_cast<int>(buf.size()) - 1)))
               : std::string();
}

std::unique_ptr<i18n::Catalog> load_words(const std::string& assets, const std::string& locale) {
  auto bytes = base::read_file(assets + "/lang/" + locale + ".json", std::uint64_t{1} << 24U);
  if (!bytes) {
    return std::make_unique<i18n::Catalog>();
  }
  auto c = i18n::Catalog::parse(locale, base::as_text(*bytes));
  return std::make_unique<i18n::Catalog>(c ? std::move(*c) : i18n::Catalog());
}

std::unique_ptr<text::Font> load_font(const std::string& assets, int limit) {
  auto bytes = base::read_file(assets + "/fonts/archivo/Archivo-VF.ttf", std::uint64_t{1} << 24U);
  if (!bytes) {
    return nullptr;
  }
  // the 0.5 view asks for "Archivo Narrow": the variable font's condensed width
  auto f = text::Font::load(std::move(*bytes), {.width = 75, .texture_limit = limit});
  return f ? std::move(*f) : nullptr;
}

enum class Flag : std::uint8_t { on, frozen, locked, plot };

/// A layer switch: an 18 px icon button (drawn in code: eye, snowflake, padlock, printer), pressed = `active`.
bool flag_button(const char* id, Flag f, bool active, const std::string& tip, float size) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool hit = ImGui::InvisibleButton(id, {size, size});
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tip.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 ink = ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
  const float s = size / 20.0F;  // the 20 × 20 design grid
  const auto at = [&](float x, float y) { return ImVec2{p.x + x * s, p.y + y * s}; };
  switch (f) {
    case Flag::on:  // an eye; struck through when the layer is off
      dl->AddBezierQuadratic(at(3, 10), at(10, 3), at(17, 10), ink, 1.2F);
      dl->AddBezierQuadratic(at(3, 10), at(10, 17), at(17, 10), ink, 1.2F);
      dl->AddCircleFilled(at(10, 10), 2.2F * s, ink);
      if (active) {
        dl->AddLine(at(4, 16), at(16, 4), ink, 1.4F);
      }
      break;
    case Flag::frozen:  // a snowflake
      for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) * 1.0471976F;
        dl->AddLine(at(10 - 6 * std::cos(a), 10 - 6 * std::sin(a)), at(10 + 6 * std::cos(a), 10 + 6 * std::sin(a)), ink,
                    1.2F);
      }
      break;
    case Flag::locked:  // a padlock, open when unlocked
      dl->AddRect(at(5, 9), at(15, 16), ink, 1.0F, 1.2F);
      dl->PathArcTo(at(10, active ? 9 : 7), 3.5F * s, std::numbers::pi_v<float>, 2 * std::numbers::pi_v<float>, 8);
      dl->PathStroke(ink, 1.2F);
      break;
    case Flag::plot:  // a printer, struck through when the layer does not plot
      dl->AddRect(at(6, 3), at(14, 8), ink, 0, 1.2F);
      dl->AddRect(at(3, 8), at(17, 14), ink, 1.0F, 1.2F);
      dl->AddRect(at(6, 12), at(14, 17), ink, 0, 1.2F);
      if (active) {
        dl->AddLine(at(4, 17), at(16, 3), ink, 1.4F);
      }
      break;
  }
  return hit;
}

}  // namespace

/// What the command line asks of the viewer: prints, the prompt, the selection, picks under the cursor.
class Viewer::CmdHost final : public cmd::Host {
 public:
  explicit CmdHost(Viewer& v) : v_(v) {}
  void print(const std::string& text) override {
    v_.printed_.push_back(text);
    if (v_.printed_.size() > 200) {
      v_.printed_.erase(v_.printed_.begin());
    }
  }
  void prompt(const std::string& text) override { v_.prompt_ = text; }
  // NOLINTNEXTLINE(readability-make-member-function-const) — point_for keeps the snap it found
  [[nodiscard]] std::optional<cmd::Pt> cursor() const override {
    if (v_.mouse_x_ < v_.cx_ || v_.mouse_y_ < v_.cy_ || v_.space_ >= v_.views_.size()) {
      return std::nullopt;
    }
    const auto w = v_.point_for(v_.world_at(v_.mouse_x_, v_.mouse_y_));
    return cmd::Pt{w[0], w[1]};
  }
  [[nodiscard]] const cmd::IdSet& selection() const override { return sel_; }
  void set_selection(cmd::IdSet s) override {
    sel_ = std::move(s);
    v_.redraw_ = std::max(v_.redraw_, 2);
  }
  std::optional<doc::Id> pick_at(cmd::Pt raw) override {
    if (!v_.cad_ || v_.space_ >= v_.views_.size()) {
      return std::nullopt;
    }
    const double tol = 6 / std::max(v_.views_[v_.space_].zoom, 1e-300);  // the pick box, in drawing units
    const cad::BlockLookup blocks = [this](const std::string& n) { return v_.cad_->blocks().get(n); };
    std::optional<doc::Id> best;
    double bd = tol;
    v_.cad_->entities().each([&](const doc::Id& id, const base::Value& e) {
      if (!v_.cad_->visible(e)) {
        return;
      }
      double d = std::numeric_limits<double>::infinity();
      if (e.get("type") == base::Value("text")) {
        const auto c = cad::text_corners(e);
        bool inside = false;
        for (std::size_t i = 0, j = 3; i < 4; j = i++) {
          if ((c.at(i).y > raw.y) != (c.at(j).y > raw.y) &&
              raw.x < (c.at(j).x - c.at(i).x) * (raw.y - c.at(i).y) / (c.at(j).y - c.at(i).y) + c.at(i).x) {
            inside = !inside;
          }
        }
        if (inside) {
          d = 0;
        }
      }
      for (const cad::Prim& p : cad::entity_prims(e, blocks)) {
        d = std::min(d, cad::dist_to_prim(p, {raw.x, raw.y}));
      }
      if (d <= bd) {
        bd = d;
        best = id;
      }
    });
    return best;
  }
  // ---- the view (ZOOM, NAMEDVIEW) and files (SAVE, SAVEAS, DXFOUT) for commands
  [[nodiscard]] render::View* view() { return v_.space_ < v_.views_.size() ? &v_.views_[v_.space_] : nullptr; }
  void remember_view() {
    if (const render::View* v = view()) {
      v_.prev_view_ = *v;
    }
  }
  void zoom_extents() override {
    if (render::View* v = view(); v != nullptr && v_.space_ < v_.drawing_.spaces.size()) {
      remember_view();
      *v = render::fit(v_.drawing_.spaces[v_.space_], *v);
      v_.redraw_ = std::max(v_.redraw_, 2);
    }
  }
  bool zoom_previous() override {
    render::View* v = view();
    if (v == nullptr || !v_.prev_view_) {
      return false;
    }
    std::swap(*v, *v_.prev_view_);
    v_.redraw_ = std::max(v_.redraw_, 2);
    return true;
  }
  void zoom_by(double factor) override {
    if (render::View* v = view(); v != nullptr && factor > 0 && std::isfinite(factor)) {
      remember_view();
      *v = render::zoom_at(*v, v->width / 2, v->height / 2, factor);
      v_.redraw_ = std::max(v_.redraw_, 2);
    }
  }
  void zoom_window(cmd::Pt a, cmd::Pt b, bool exact) override {
    render::View* v = view();
    const double w = std::abs(b.x - a.x);
    const double h = std::abs(b.y - a.y);
    if (v == nullptr || !(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h)) {
      return;
    }
    remember_view();
    v->cx = (a.x + b.x) / 2;
    v->cy = (a.y + b.y) / 2;
    v->zoom = std::clamp(std::min(v->width / w, v->height / h) * (exact ? 1 : 0.95), 1e-9, 1e9);
    v_.redraw_ = std::max(v_.redraw_, 2);
  }
  [[nodiscard]] std::optional<cmd::ViewRect> view_rect() const override {
    if (v_.space_ >= v_.views_.size()) {
      return std::nullopt;
    }
    const render::View& v = v_.views_[v_.space_];
    return cmd::ViewRect{.cx = v.cx, .cy = v.cy, .w = v.width / v.zoom, .h = v.height / v.zoom};
  }
  // the viewer saves once the command has ended (a save while one runs is refused)
  void save_file(const std::string& format, bool /*remember*/) override {
    v_.save_request_ = std::pair{format.empty() ? v_.save_format() : format, false};
  }
  void save_as() override { v_.save_request_ = std::pair{v_.save_format(), true}; }
  std::vector<doc::Id> pick_window(const cmd::Rect& r, bool crossing) override {
    std::vector<doc::Id> out;
    if (!v_.cad_) {
      return out;
    }
    const cad::BlockLookup blocks = [this](const std::string& n) { return v_.cad_->blocks().get(n); };
    const cad::Rect2 w{.min_x = r.min_x, .min_y = r.min_y, .max_x = r.max_x, .max_y = r.max_y};
    v_.cad_->entities().each([&](const doc::Id& id, const base::Value& e) {
      if (v_.cad_->editable(e) && cad::in_window(e, w, crossing, blocks)) {
        out.push_back(id);
      }
    });
    return out;
  }
  // the Properties panel (propcmds.js)
  void props_open() override { v_.props_open_ = true; }
  void props_close() override { v_.props_open_ = false; }
  void props_toggle_quick() override {
    v_.qp_on_ = !v_.qp_on_;
    v_.qp_dismissed_.clear();
    v_.message_ = v_.words_->t(v_.qp_on_ ? "props.qp.on" : "props.qp.off");
  }
  [[nodiscard]] std::string props_pick_label() const override {
    return v_.prop_pick_ ? v_.prop_pick_->label : std::string();
  }
  void props_picked(cmd::Pt p) override { v_.props_picked(p.x, p.y); }
  void preview(const base::Array& entities) override {
    v_.preview_ = entities;
    v_.redraw_ = std::max(v_.redraw_, 2);
  }

 private:
  Viewer& v_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) — owned by the viewer it reports to
  cmd::IdSet sel_;
};

void Inbox::post(Item item) {
  const std::scoped_lock lock(m_);
  items_.push_back(std::move(item));
}

std::vector<Inbox::Item> Inbox::take() {
  const std::scoped_lock lock(m_);
  return std::exchange(items_, {});
}

Viewer::Viewer(const Gpu& gpu, wgpu::TextureFormat target, Services services, const std::string& assets,
               const std::string& locale)
    : services_(std::move(services)), renderer_(gpu.device, target) {
  english_ = load_words(assets, "en");
  words_ = locale == "en" ? nullptr : load_words(assets, locale);
  if (words_) {
    words_->set_fallback(english_.get());
  } else {
    words_ = std::make_unique<i18n::Catalog>(*english_);
  }
  font_ = load_font(assets, static_cast<int>(gpu.max_texture));
  if (!font_) {
    message_ = "the drawing font is missing (" + assets + "/fonts)";
  } else {
    // the glyphs most drawings use, made now (while a file is on its way) rather than inside the first open
    (void)font_->layout(
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~°±⌀");
    renderer_.upload_atlas(font_->atlas());
  }
  apply_theme();
}

Viewer::~Viewer() {
  engine_.reset();  // its frames point into the document and the host
  cmd_host_.reset();
}

void Viewer::apply_theme() const {
  // the site's tokens (CadCraft 0.5 assets/css/tokens.css): model (dark) or paper (light)
  const render::Rgba sheet = dark_ ? rgba("#1D232B") : rgba("#F3F6F8");
  const render::Rgba panel = dark_ ? rgba("#252D37") : rgba("#FFFFFF");
  const render::Rgba ink = dark_ ? rgba("#E8EEF4") : rgba("#17202B");
  const render::Rgba ink2 = dark_ ? rgba("#C3CDD8") : rgba("#34404E");
  const render::Rgba grid = dark_ ? rgba("#36414E") : rgba("#D5DDE5");
  const render::Rgba line = dark_ ? rgba("#6C7A8A") : rgba("#7A8796");
  const render::Rgba action = rgba("#1E48C8");
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 2;
  st.FrameRounding = 2;
  st.TabRounding = 2;
  st.WindowBorderSize = 1;
  st.FrameBorderSize = 1;
  st.WindowPadding = {8, 6};
  // touch: every control 40 px tall (the font + twice the padding), more room between them, wider scroll bars
  const ImGuiIO& io = ImGui::GetIO();
  float font_px = 13.0F;  // ImGui's own font, before the app's is in
  if (st.FontSizeBase > 0) {
    font_px = st.FontSizeBase;
  } else if (!io.Fonts->Fonts.empty() && io.Fonts->Fonts[0]->LegacySize > 0) {
    font_px = io.Fonts->Fonts[0]->LegacySize;
  }
  st.FramePadding = touch_ ? ImVec2{12, std::max(3.0F, (40.0F - font_px) / 2)} : ImVec2{6, 3};
  st.ItemSpacing = touch_ ? ImVec2{10, 6} : ImVec2{6, 4};
  st.ScrollbarSize = touch_ ? 24.0F : 14.0F;
  st.GrabMinSize = touch_ ? 24.0F : 12.0F;
  st.TouchExtraPadding = touch_ ? ImVec2{2, 2} : ImVec2{0, 0};
  ImVec4* c = st.Colors;  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  const std::span<ImVec4> colors(c, ImGuiCol_COUNT);
  colors[ImGuiCol_Text] = im(ink);
  colors[ImGuiCol_TextDisabled] = im(line);
  colors[ImGuiCol_WindowBg] = im(panel);
  colors[ImGuiCol_ChildBg] = im(panel);
  colors[ImGuiCol_PopupBg] = im(panel);
  colors[ImGuiCol_MenuBarBg] = im(panel);
  colors[ImGuiCol_Border] = im(grid);
  colors[ImGuiCol_FrameBg] = im(sheet);
  colors[ImGuiCol_FrameBgHovered] = im(grid);
  colors[ImGuiCol_FrameBgActive] = im(grid);
  colors[ImGuiCol_TitleBg] = im(panel);
  colors[ImGuiCol_TitleBgActive] = im(panel);
  colors[ImGuiCol_Header] = im(grid);
  colors[ImGuiCol_HeaderHovered] = im(grid);
  colors[ImGuiCol_HeaderActive] = im(action);
  colors[ImGuiCol_Button] = im(sheet);
  colors[ImGuiCol_ButtonHovered] = im(grid);
  colors[ImGuiCol_ButtonActive] = im(action);
  colors[ImGuiCol_CheckMark] = im(ink);
  colors[ImGuiCol_Tab] = im(panel);
  colors[ImGuiCol_TabHovered] = im(grid);
  colors[ImGuiCol_TabSelected] = im(sheet);
  colors[ImGuiCol_TabSelectedOverline] = im(action);
  colors[ImGuiCol_ScrollbarBg] = im(panel);
  colors[ImGuiCol_ScrollbarGrab] = im(grid);
  colors[ImGuiCol_Separator] = im(grid);
  colors[ImGuiCol_NavCursor] = im(action);
  (void)ink2;
}

render::Rgba Viewer::background() const {
  const bool paper = doc_ && space_ < drawing_.spaces.size() && drawing_.spaces[space_].kind == "paper";
  if (print_) {
    return render::Theme::print(paper).sheet;
  }
  return dark_ ? render::Theme::model().sheet : render::Theme::light().sheet;
}

void Viewer::show(std::unique_ptr<engine::OpenDrawing> d) {
  engine_.reset();
  cmd_host_.reset();
  cad_.reset();
  doc_ = std::move(d);
  stats_.open = doc_->times;
  printed_.clear();
  prompt_.clear();
  preview_.clear();
  const std::uint64_t t0 = SDL_GetTicksNS();
  if (doc_->kind == "dwg") {
    // CadCraft opens a DWG as its document (readDwg → importDwg → Doc.load) and draws that
    cad_ = std::make_unique<doc::Doc>();
    // as cadcraft.js opens a file: meta.name = the file's name without its extension, meta.source = the file's name
    base::Value drawing = cad::import_dwg(doc_->model, doc_->options.version_tag, *words_);
    base::Value meta = drawing.get("meta").is_object() ? drawing.get("meta") : base::Value::object();
    const std::string& file = doc_->name;
    const std::size_t dot = file.find_last_of('.');
    meta.set("name", dot == std::string::npos ? file : file.substr(0, dot));
    meta.set("source", file);
    meta.set("format", "dwg");
    drawing.set("meta", std::move(meta));
    cad_->load(std::move(drawing), *words_);
    cmd_host_ = std::make_unique<CmdHost>(*this);
    engine_ = std::make_unique<cmd::Engine>(*cad_, *cmd_host_, *words_);
    drawing_ = docview::to_drawing(*cad_);
    drawn_version_ = cad_->version();
  } else {
    drawing_ = doc_->drawing;
  }
  scene_ = font_ ? render::build_scene(drawing_, *font_) : render::Scene{};
  const std::uint64_t t1 = SDL_GetTicksNS();
  renderer_.upload(scene_);
  if (font_) {
    renderer_.upload_atlas(font_->atlas());
  }
  stats_.scene_ms = static_cast<double>(t1 - t0) / 1e6;
  stats_.upload_ms = static_cast<double>(SDL_GetTicksNS() - t1) / 1e6;
  hidden_.assign(drawing_.layers.size(), false);
  for (std::size_t i = 0; i < hidden_.size(); ++i) {
    hidden_[i] = !drawing_.layers[i].on;
  }
  views_.assign(drawing_.spaces.size(), render::View{});
  fit_pending_ = true;
  space_ = 0;
  message_.clear();
  redraw_ = 2;
}

void Viewer::open_item(Inbox::Item item) {
  source_path_ = item.path;
  auto d = item.path.empty() ? engine::open_bytes(item.name, item.bytes) : engine::open_file(item.path);
  if (!d) {
    message_ = item.name + ": " + d.error().message;
    redraw_ = 2;
    return;
  }
  show(std::move(*d));
}

void Viewer::take_inbox() {
  for (Inbox::Item& it : inbox_.take()) {
    if (it.save_target) {
      save_to(it.path);
    } else {
      open_item(std::move(it));
    }
  }
}

namespace {
bool ends_with_ci(std::string_view s, std::string_view tail) {
  return s.size() >= tail.size() && base::js_to_lower(s.substr(s.size() - tail.size())) == tail;
}
}  // namespace

std::string Viewer::save_format() const {
  if (!cad_) {
    return "dwg";
  }
  const base::Value f = cad_->meta.get("saveFormat");
  if (f == base::Value("dwg") || f == base::Value("dxf") || f == base::Value("dxf2000")) {
    return f.as_string();
  }
  const std::string src = base::js_string(cad_->meta.get("source"));
  if (ends_with_ci(src, ".dxf")) {
    // a newer DXF saves as DXF 2000
    const std::string v = base::js_string(cad_->meta.get("version"));
    return v.size() == 6 && v.starts_with("AC10") && v >= "AC1015" ? "dxf2000" : "dxf";
  }
  if (ends_with_ci(src, ".dwg")) {
    return "dwg";
  }
  return last_format_.empty() ? "dwg" : last_format_;
}

std::string Viewer::dwg_version() const {
  if (cad_) {
    if (std::string k = save::dwg_version_key(base::js_string(cad_->meta.get("dwgVersion"))); !k.empty()) {
      return k;
    }
    if (ends_with_ci(base::js_string(cad_->meta.get("source")), ".dwg")) {
      if (std::string k = save::dwg_version_key(base::js_string(cad_->meta.get("version"))); !k.empty()) {
        return k;
      }
    }
  }
  return last_version_.empty() ? "2000" : last_version_;
}

std::string Viewer::format_label(const std::string& format) const {
  if (format == "dwg") {
    return words_->t("save.format.dwg", {{"version", words_->t("dwg.version", {{"v", dwg_version()}})}});
  }
  if (format == "dxf2000") {
    return words_->t("save.format.dxf2000");
  }
  return "DXF R12";
}

void Viewer::save(std::string format, bool as) {
  if (!cad_ || !cmd_host_) {
    return;
  }
  if (engine_ && engine_->busy()) {
    cmd_host_->print(words_->t("app.finish_cancel_current_command"));
    return;
  }
  if (format != "dwg" && format != "dxf" && format != "dxf2000") {
    format = "dwg";
  }
  PendingSave ps;
  ps.format = format;
  try {
    base::Value report;
    if (format == "dwg") {
      save::DwgSaveOptions o;
      o.version = dwg_version();
      save::DwgSaveResult r = save::save_dwg(*cad_, o, *words_);
      ps.bytes.resize(r.bytes.size());
      std::ranges::transform(r.bytes, ps.bytes.begin(), [](std::uint8_t b) { return static_cast<std::byte>(b); });
      report = r.report;
    } else {
      save::DxfSaveResult r =
          format == "dxf2000" ? save::write_dxf2000(*cad_, *words_) : save::write_dxf(*cad_, *words_);
      ps.bytes.resize(r.text.size());
      std::ranges::transform(r.text, ps.bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
      report = r.report;
    }
    if (const base::Value notes = report.get("notes"); notes.is_array()) {
      ps.notes = notes.as_array();
    }
  } catch (const std::exception& e) {
    cmd_host_->print(words_->t(format == "dwg" ? "app.save.failed.dwg" : "app.save.failed",
                               {{"format", format_label(format)}, {"why", e.what()}}));
    return;
  }
  const base::Value nm = cad_->meta.get("name");
  ps.name = (nm.truthy() ? base::js_string(nm) : std::string("Drawing1")) + (format == "dwg" ? ".dwg" : ".dxf");
  ps.version = cad_->version();
  const std::string ext = format == "dwg" ? ".dwg" : ".dxf";
  cad_->meta.set("saveFormat", format);  // later saves use this format (put back when nothing is saved)
  if (services_.pick_save) {
    pending_save_ = std::move(ps);
    if (!as && !source_path_.empty() && ends_with_ci(source_path_, ext)) {
      save_to(source_path_);  // the drawing's own file, in place
    } else {
      services_.pick_save(pending_save_->name, format == "dwg" ? "dwg" : "dxf");
    }
    return;
  }
  if (services_.download) {
    services_.download(ps.name, ps.bytes);
    std::string line = words_->t(format == "dxf" ? "app.save.done.dxf" : "app.save.done",
                                 {{"file", ps.name}, {"format", format_label(format)}});
    const std::string src = base::js_string(cad_->meta.get("source"));
    if (!src.empty() && src != "undefined" && src != ps.name) {
      line += " " + words_->t("app.save.original_unchanged", {{"file", src}});
    }
    cmd_host_->print(line);
    for (const base::Value& n : ps.notes) {
      cmd_host_->print("  " + base::js_string(n));
    }
    if (cad_->version() == ps.version) {
      cad_->mark_saved();
    }
  }
}

void Viewer::save_to(const std::string& path) {
  if (!pending_save_ || !cad_ || !cmd_host_) {
    return;
  }
  PendingSave ps = std::move(*pending_save_);
  pending_save_.reset();
  if (path.empty()) {
    cmd_host_->print(words_->t("app.not_saved"));
    return;
  }
  if (const auto ok = base::write_file_atomic(path, ps.bytes); !ok) {
    cmd_host_->print(words_->t("app.save.failed", {{"format", format_label(ps.format)}, {"why", ok.error().message}}));
    return;
  }
  source_path_ = path;
  cmd_host_->print(words_->t("app.save.written", {{"file", path}, {"format", format_label(ps.format)}}));
  for (const base::Value& n : ps.notes) {
    cmd_host_->print("  " + base::js_string(n));
  }
  if (cad_->version() == ps.version) {
    cad_->mark_saved();
  }
  redraw_ = std::max(redraw_, 2);
}

void Viewer::save_as_dialog() {
  if (save_as_open_) {
    ImGui::OpenPopup("##saveas");
    save_as_open_ = false;
  }
  if (!ImGui::BeginPopupModal("##saveas", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextUnformatted(words_->t("app.save_copy").c_str());
  ImGui::Separator();
  const std::array<std::string, 3> keys = {"dwg", "dxf2000", "dxf"};
  for (int i = 0; i < 3; ++i) {
    const std::string label = format_label(keys.at(static_cast<std::size_t>(i)));
    ImGui::RadioButton(label.c_str(), &save_as_format_, i);
  }
  if (save_as_format_ == 0) {
    const auto versions = save::dwg_versions();
    std::vector<std::string> names;
    for (const save::DwgVersion& v : versions) {
      names.push_back(words_->t("dwg.version", {{"v", std::string(v.key)}}));
    }
    save_as_version_ = std::clamp(save_as_version_, 0, static_cast<int>(names.size()) - 1);
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(words_->t("save.version").c_str(),
                          names.at(static_cast<std::size_t>(save_as_version_)).c_str())) {
      for (int i = 0; std::cmp_less(i, names.size()); ++i) {
        if (ImGui::Selectable(names.at(static_cast<std::size_t>(i)).c_str(), i == save_as_version_)) {
          save_as_version_ = i;
        }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::Separator();
  if (ImGui::Button(words_->t("ui.save").c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    const std::string& format = keys.at(static_cast<std::size_t>(save_as_format_));
    last_format_ = format;
    if (format == "dwg" && cad_) {
      const std::string v(save::dwg_versions()[static_cast<std::size_t>(save_as_version_)].key);
      last_version_ = v;
      cad_->meta.set("dwgVersion", v);
    }
    ImGui::CloseCurrentPopup();
    save(format, true);
  }
  ImGui::SameLine();
  if (ImGui::Button(words_->t("dlg.qs.cancel").c_str()) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    if (cmd_host_) {
      cmd_host_->print(words_->t("app.not_saved"));
    }
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void Viewer::set_space(std::size_t s) {
  if (doc_ && s < drawing_.spaces.size() && s != space_) {
    space_ = s;
    if (cad_) {
      if (engine_) {
        (void)engine_->escape();  // a command does not carry over to another space
      }
      cad_->set_space(s == 0 || s > cad_->layouts().size() ? std::string_view()
                                                           : std::string_view(cad_->layouts()[s - 1].name));
    }
    redraw_ = 2;
  }
}

std::array<double, 2> Viewer::world_at(double x, double y) const {
  return render::to_world(views_[space_], x - cx_, y - cy_);
}

void Viewer::rebuild() {
  if (!cad_) {
    return;
  }
  drawing_ = docview::to_drawing(*cad_);
  scene_ = font_ ? render::build_scene(drawing_, *font_) : render::Scene{};
  renderer_.upload(scene_);
  if (font_) {
    renderer_.upload_atlas(font_->atlas());
  }
  if (views_.size() != drawing_.spaces.size()) {
    views_.resize(drawing_.spaces.size(), views_.empty() ? render::View{} : views_.front());
  }
  hidden_.assign(drawing_.layers.size(), false);
  drawn_version_ = cad_->version();
  bounds_.clear();
  grip_.reset();
  redraw_ = std::max(redraw_, 2);
}

std::vector<doc::Id> Viewer::props_selection() const {
  std::vector<doc::Id> ids;
  if (cad_ && cmd_host_) {
    for (const doc::Id id : cmd_host_->selection()) {
      if (cad_->entities().get(id) != nullptr) {
        ids.push_back(id);
      }
    }
  }
  return ids;
}

void Viewer::after_input() {
  if (cad_ && cad_->version() != drawn_version_) {
    rebuild();
  }
  if (save_request_ && engine_ && !engine_->busy()) {  // SAVE / SAVEAS / DXFOUT asked for it
    const auto [format, as] = *save_request_;
    save_request_.reset();
    if (as) {
      save_as_dialog();
    } else {
      save(format, false);
    }
  }
  redraw_ = std::max(redraw_, 2);
}

void Viewer::command(std::string_view line) {
  if (engine_) {
    engine_->text(line);
    after_input();
  }
}

bool Viewer::wants_point() const {
  if (grip_) {
    return true;
  }
  const std::string k = engine_ ? engine_->expects() : std::string();
  return k == "point" || k == "distance" || k == "angle";
}

bool Viewer::can_select() const {
  return engine_ && (!engine_->busy() || engine_->expects() == "select");
}

std::vector<const base::Value*> Viewer::near_cursor(std::array<double, 2> p, double r) {
  std::vector<const base::Value*> out;
  if (!cad_) {
    return out;
  }
  const cad::BlockLookup blocks = [this](const std::string& n) { return cad_->blocks().get(n); };
  cad_->entities().each([&](const doc::Id& id, const base::Value& e) {
    if (out.size() > 400 || !cad_->visible(e)) {
      return;
    }
    const base::Value t = e.get("type");
    if (t == base::Value("xline") || t == base::Value("ray")) {
      for (const cad::Prim& q : cad::entity_prims(e, nullptr)) {
        if (cad::dist_to_prim(q, {p[0], p[1]}) <= r) {
          out.push_back(&e);
          break;
        }
      }
      return;
    }
    auto it = bounds_.find(id);
    if (it == bounds_.end()) {
      it = bounds_.emplace(id, cad::entity_bounds(e, blocks)).first;
    }
    const cad::Bounds& b = it->second;
    if (b.valid() && p[0] >= b.min_x - r && p[0] <= b.max_x + r && p[1] >= b.min_y - r && p[1] <= b.max_y + r) {
      out.push_back(&e);
    }
  });
  return out;
}

std::array<double, 2> Viewer::point_for(std::array<double, 2> raw) {
  snap_.reset();
  if (!cad_ || space_ >= views_.size() || !wants_point()) {
    return raw;
  }
  std::optional<cmd::Pt> base_pt;
  if (grip_) {
    base_pt = cmd::Pt{grip_->x, grip_->y};
  } else if (engine_) {
    base_pt = engine_->rubber_base;
  }
  const std::optional<cad::P2> base = base_pt ? std::optional<cad::P2>(cad::P2{base_pt->x, base_pt->y}) : std::nullopt;
  if (osnap_) {
    const double r = 10 / std::max(views_[space_].zoom, 1e-300);
    const cad::BlockLookup blocks = [this](const std::string& n) { return cad_->blocks().get(n); };
    snap_ = cad::best_snap(near_cursor(raw, r), {raw[0], raw[1]}, r, kinds_, blocks, base);
    if (snap_) {
      return {snap_->x, snap_->y};
    }
  }
  if (ortho_ && base) {
    const double dx = raw[0] - base->x;
    const double dy = raw[1] - base->y;
    return std::abs(dx) >= std::abs(dy) ? std::array<double, 2>{base->x + dx, base->y}
                                        : std::array<double, 2>{base->x, base->y + dy};
  }
  return raw;
}

void Viewer::canvas_window(double x0, double y0, double x1, double y1) {
  if (!engine_ || !cmd_host_ || space_ >= views_.size()) {
    return;
  }
  const auto a = world_at(x0, y0);
  const auto b = world_at(x1, y1);
  const cmd::Rect r{.min_x = std::min(a[0], b[0]),
                    .min_y = std::min(a[1], b[1]),
                    .max_x = std::max(a[0], b[0]),
                    .max_y = std::max(a[1], b[1])};
  const bool crossing = x1 < x0;  // right to left: what it touches
  if (engine_->busy()) {
    engine_->window(r, crossing);
  } else {
    cmd::IdSet sel = cmd_host_->selection();
    for (const doc::Id id : cmd_host_->pick_window(r, crossing)) {
      sel.add(id);
    }
    cmd_host_->set_selection(std::move(sel));
  }
  after_input();
}

void Viewer::canvas_click(double x, double y, bool right) {
  if (!engine_ || space_ >= views_.size()) {
    return;
  }
  if (right) {
    grip_.reset();
    engine_->text("");  // a right click is Enter
    after_input();
    return;
  }
  const auto raw = world_at(x, y);
  if (grip_ && cad_) {
    // the grip picked goes where this click (snapped) says: one undo step
    const GripEdit ge = *grip_;
    const auto to = point_for(raw);
    grip_.reset();
    if (const base::Value* e = cad_->entities().get(ge.id)) {
      const base::Value moved = cad::grip_move(*e, ge.key, {to[0], to[1]});
      cad_->change("GRIP", [&] { (void)cad_->update(ge.id, moved); });
    }
    after_input();
    return;
  }
  if (engine_->busy()) {
    const auto p = wants_point() ? point_for(raw) : raw;
    (void)engine_->click({p[0], p[1]}, cmd::Pt{raw[0], raw[1]});
  } else if (cmd_host_ && cad_) {
    // a grip of the selection under the click picks it up
    const double tol = 6 / std::max(views_[space_].zoom, 1e-300);
    for (const doc::Id id : cmd_host_->selection()) {
      const base::Value* e = cad_->entities().get(id);
      if (e == nullptr || !cad_->editable(*e)) {
        continue;
      }
      for (const cad::Grip& gr : cad::grips(*e)) {
        if (std::abs(gr.x - raw[0]) <= tol && std::abs(gr.y - raw[1]) <= tol) {
          grip_ = GripEdit{.id = id, .key = gr.key, .x = gr.x, .y = gr.y};
          redraw_ = std::max(redraw_, 2);
          return;
        }
      }
    }
    // noun-verb: a click on an object adds it to the selection (on nothing: clears it)
    cmd::IdSet sel = cmd_host_->selection();
    if (const auto id = cmd_host_->pick_at({raw[0], raw[1]})) {
      if (!sel.erase(*id)) {
        sel.add(*id);
      }
    } else {
      sel.clear();
    }
    cmd_host_->set_selection(std::move(sel));
  }
  after_input();
}

void Viewer::event(const SDL_Event& e, bool imgui_wants_mouse) {
  redraw_ = std::max(redraw_, 4);  // ImGui feeds a click over two frames and settles it (a tab, a menu) on the next
  if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP ||
      e.type == SDL_EVENT_FINGER_CANCELED) {
    finger(e);  // also with no drawing open: the touch layout
    return;
  }
  if (!doc_ || space_ >= views_.size()) {
    return;
  }
  if (e.type == SDL_EVENT_KEY_DOWN && engine_ && (e.key.key == SDLK_F3 || e.key.key == SDLK_F8)) {
    bool& flag = e.key.key == SDLK_F3 ? osnap_ : ortho_;
    flag = !flag;
    printed_.push_back((e.key.key == SDLK_F3 ? words_->t("app.object_snap") : std::string("Ortho")) +
                       (flag ? " on" : " off"));
    redraw_ = std::max(redraw_, 2);
    return;
  }
  if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && grip_) {
    grip_.reset();
    redraw_ = std::max(redraw_, 2);
  }
  if (e.type == SDL_EVENT_KEY_DOWN && engine_ && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0 &&
      line_[0] == '\0' && !engine_->busy()) {
    if (e.key.key == SDLK_S) {
      if ((e.key.mod & SDL_KMOD_SHIFT) != 0) {
        save_as_open_ = true;
      } else {
        save(save_format(), false);
      }
      redraw_ = std::max(redraw_, 2);
      return;
    }
    // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y: the UNDO and REDO commands (the command line's own box would take them)
    if (e.key.key == SDLK_Z) {
      engine_->run((e.key.mod & SDL_KMOD_SHIFT) != 0 ? "REDO" : "UNDO");
      after_input();
      return;
    }
    if (e.key.key == SDLK_Y) {
      engine_->run("REDO");
      after_input();
      return;
    }
  }
  render::View& v = views_[space_];
  auto inside = [&](double x, double y) { return x >= cx_ && y >= cy_ && x < cx_ + cw_ && y < cy_ + ch_; };
  switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (fingers_.size() >= 2 && e.button.which == SDL_TOUCH_MOUSEID) {
        break;  // the first finger of a pinch, as a mouse: the pinch has it
      }
      if (!imgui_wants_mouse && inside(static_cast<double>(e.button.x), static_cast<double>(e.button.y))) {
        if (e.button.clicks >= 2 && e.button.button == SDL_BUTTON_LEFT && (!engine_ || !engine_->busy())) {
          v = render::fit(drawing_.spaces[space_], v);
        } else if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE ||
                   e.button.button == SDL_BUTTON_RIGHT) {
          dragging_ = e.button.button == SDL_BUTTON_MIDDLE || (e.button.button == SDL_BUTTON_LEFT && !can_select());
          press_ = true;
          moved_ = false;
          press_button_ = e.button.button;
          press_x_ = last_x_ = static_cast<double>(e.button.x);
          press_y_ = last_y_ = static_cast<double>(e.button.y);
        }
        redraw_ = 2;
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (press_ && !moved_ && press_button_ != SDL_BUTTON_MIDDLE) {
        canvas_click(press_x_, press_y_, press_button_ == SDL_BUTTON_RIGHT);
      } else if (press_ && moved_ && press_button_ == SDL_BUTTON_LEFT && can_select()) {
        canvas_window(press_x_, press_y_, static_cast<double>(e.button.x), static_cast<double>(e.button.y));
      }
      press_ = false;
      dragging_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (e.motion.which != SDL_TOUCH_MOUSEID && touch_) {
        set_touch(false);  // a real mouse again: back to the dense layout
      }
      if (fingers_.size() >= 2 && e.motion.which == SDL_TOUCH_MOUSEID) {
        break;
      }
      mouse_x_ = static_cast<double>(e.motion.x);
      mouse_y_ = static_cast<double>(e.motion.y);
      if (press_ && std::hypot(mouse_x_ - press_x_, mouse_y_ - press_y_) > 4) {
        moved_ = true;  // beyond a click: a drag (it pans)
      }
      if (dragging_ && moved_) {
        v = render::pan_by(v, static_cast<double>(e.motion.x) - last_x_, static_cast<double>(e.motion.y) - last_y_);
        last_x_ = static_cast<double>(e.motion.x);
        last_y_ = static_cast<double>(e.motion.y);
      }
      redraw_ = 2;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (!imgui_wants_mouse && inside(static_cast<double>(e.wheel.mouse_x), static_cast<double>(e.wheel.mouse_y)) &&
          e.wheel.y != 0) {
        // dwg-view.js: exp(−sign(deltaY) · min(|deltaY|, 120) / 120 · 0.2); one notch ≈ 100 deltaY, up = zoom in
        const double notch = std::min(std::abs(static_cast<double>(e.wheel.y)) * 100.0, 120.0) / 120.0;
        const double f = std::exp((e.wheel.y > 0 ? 1.0 : -1.0) * notch * 0.2);
        v = render::zoom_at(v, static_cast<double>(e.wheel.mouse_x) - cx_, static_cast<double>(e.wheel.mouse_y) - cy_,
                            f);
        redraw_ = 2;
      }
      break;
    default: break;
  }
}

void Viewer::set_touch(bool on) {
  if (touch_ != on) {
    touch_ = on;
    apply_theme();
    redraw_ = std::max(redraw_, 2);
  }
}

// Fingers: one finger pans through the mouse SDL makes of it (drag, double tap = fit); two pinch about their midpoint
// and pan with it. SDL gives finger positions as 0–1 of the window.
void Viewer::finger(const SDL_Event& e) {
  set_touch(true);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const double x = static_cast<double>(e.tfinger.x) * static_cast<double>(size.x);
  const double y = static_cast<double>(e.tfinger.y) * static_cast<double>(size.y);
  const auto it = std::ranges::find(fingers_, e.tfinger.fingerID, &Finger::id);
  if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
    if (it != fingers_.end()) {
      fingers_.erase(it);
    }
    const bool tap = e.type == SDL_EVENT_FINGER_UP && fingers_.empty() && tap_.alone &&
                     e.tfinger.timestamp - tap_.down_ns < 300'000'000U && std::hypot(x - tap_.x, y - tap_.y) < 12;
    if (tap) {
      tap_.up_ns = e.tfinger.timestamp;
      const bool second = last_tap_.up_ns != 0 && tap_.down_ns - last_tap_.up_ns < 400'000'000U &&
                          std::hypot(tap_.x - last_tap_.x, tap_.y - last_tap_.y) < 30;
      if (second && doc_ && space_ < views_.size() && x >= cx_ && y >= cy_ && x < cx_ + cw_ && y < cy_ + ch_ &&
          !ImGui::GetIO().WantCaptureMouse) {
        views_[space_] = render::fit(drawing_.spaces[space_], views_[space_]);
        redraw_ = 2;
        last_tap_ = {};
      } else {
        last_tap_ = tap_;
      }
    }
    return;
  }
  if (e.type == SDL_EVENT_FINGER_DOWN) {
    if (it == fingers_.end() && fingers_.size() < 10) {
      fingers_.push_back({.id = e.tfinger.fingerID, .x = x, .y = y});
    }
    if (fingers_.size() == 1) {
      tap_ = {.down_ns = e.tfinger.timestamp, .up_ns = 0, .x = x, .y = y, .alone = true};
    } else {
      tap_.alone = false;
      dragging_ = false;  // the pinch takes over from the one-finger pan
    }
    return;
  }
  if (it == fingers_.end()) {
    return;
  }
  if (fingers_.size() < 2 || it - fingers_.begin() >= 2 || space_ >= views_.size()) {
    it->x = x;
    it->y = y;
    return;
  }
  // the first two fingers: zoom by the change of their distance about their midpoint, pan by the midpoint's move
  const Finger a = fingers_[0];
  const Finger b = fingers_[1];
  const double d0 = std::hypot(a.x - b.x, a.y - b.y);
  const double mx0 = (a.x + b.x) / 2;
  const double my0 = (a.y + b.y) / 2;
  it->x = x;
  it->y = y;
  const Finger& a1 = fingers_[0];
  const Finger& b1 = fingers_[1];
  const double d1 = std::hypot(a1.x - b1.x, a1.y - b1.y);
  const double mx1 = (a1.x + b1.x) / 2;
  const double my1 = (a1.y + b1.y) / 2;
  if (mx0 < cx_ || my0 < cy_ || mx0 >= cx_ + cw_ || my0 >= cy_ + ch_) {
    return;  // a pinch over the panels is not the canvas's
  }
  render::View& v = views_[space_];
  if (d0 > 1 && d1 > 1) {
    const double f = std::clamp(d1 / d0, 0.5, 2.0);  // one event never jumps more than 2×
    v = render::zoom_at(v, mx0 - cx_, my0 - cy_, f);
  }
  v = render::pan_by(v, mx1 - mx0, my1 - my0);
  redraw_ = 2;
}

void Viewer::layers_panel() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float top = vp->WorkPos.y;
  ImGui::SetNextWindowPos({vp->WorkPos.x, top});
  ImGui::SetNextWindowSize({layers_width(), vp->WorkSize.y - status_height()});
  ImGui::Begin(words_->t("ui.layers").c_str(), nullptr,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (doc_) {
    const std::string all_on = words_->t("input.cmd.layers.on");
    if (touch_ ? ImGui::Button(all_on.c_str())
               : ImGui::SmallButton(all_on.c_str())) {  // a small button is 40 px on touch
      std::ranges::fill(hidden_, false);
      redraw_ = 2;
    }
    ImGui::Separator();
    if (cad_) {
      editable_layers();
    } else {
      const auto& layers = drawing_.layers;  // a DXF, view-only: the switch shows or hides a layer here
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(layers.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const auto idx = static_cast<std::size_t>(i);
          bool on = !hidden_[idx];
          ImGui::PushID(i);
          const render::Rgba c =
              render::draw_color(layers[idx].color, dark_ ? render::Theme::model() : render::Theme::light(), false);
          ImGui::ColorButton("##c", im(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, {12, 12});
          ImGui::SameLine();
          if (ImGui::Checkbox(layers[idx].name.c_str(), &on)) {
            hidden_[idx] = !on;
            redraw_ = 2;
          }
          ImGui::PopID();
        }
      }
    }
  } else {
    ImGui::TextWrapped("%s", words_->t("ui.drop_dwg_dxf_open").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  ImGui::End();
}

/// The drawing's layers (cadcraft.js renderLayers): "0" first, then by name; the current one marked; a click on a name
/// makes it current; the switches turn a layer off, freeze, lock or keep it from plotting — each one undo step.
void Viewer::editable_layers() {
  std::vector<base::Value> list;
  cad_->layers().each([&](const std::string&, const base::Value& l) { list.push_back(l); });
  std::ranges::stable_sort(list, [](const base::Value& a, const base::Value& b) {
    const std::string na = base::js_string(a.get("name"));
    const std::string nb = base::js_string(b.get("name"));
    if (na == "0" || nb == "0") {
      return na == "0" && nb != "0";
    }
    return base::js_locale_compare_numeric(na, nb) < 0;
  });
  const std::string cur = base::js_string(cad_->current.get("layer"));
  const float flag = touch_ ? 40.0F : 18.0F;
  const std::size_t n = list.size();
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(n));
  while (clip.Step()) {
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      const base::Value& l = list[static_cast<std::size_t>(i)];
      const std::string name = base::js_string(l.get("name"));
      const bool on = !(l.get("on") == base::Value(false));
      const bool frozen = l.get("frozen").truthy();
      const bool locked = l.get("locked").truthy();
      const bool noplot = l.get("plot") == base::Value(false);
      ImGui::PushID(i);
      std::optional<std::pair<std::string, base::Value>> patch;
      if (flag_button("##on", Flag::on, !on, words_->t(on ? "app.turn_off" : "app.turn_on"), flag)) {
        if (name == cur && on) {
          message_ = words_->t("app.current_layer_make_another");
        } else {
          patch = std::pair{std::string("on"), base::Value(!on)};
        }
      }
      ImGui::SameLine(0, 2);
      if (flag_button("##frz", Flag::frozen, frozen, words_->t(frozen ? "app.layer.thaw" : "app.layer.freeze"), flag)) {
        patch = std::pair{std::string("frozen"), base::Value(!frozen)};
      }
      ImGui::SameLine(0, 2);
      if (flag_button("##lock", Flag::locked, locked, words_->t(locked ? "app.layer.unlock" : "app.layer.lock"),
                      flag)) {
        patch = std::pair{std::string("locked"), base::Value(!locked)};
      }
      ImGui::SameLine(0, 2);
      if (flag_button("##plot", Flag::plot, noplot, words_->t(noplot ? "app.plot_layer" : "app.do_not_plot_layer"),
                      flag)) {
        patch = std::pair{std::string("plot"), base::Value(noplot)};
      }
      ImGui::SameLine();
      const auto drawn = std::ranges::find_if(drawing_.layers, [&name](const auto& dl) { return dl.name == name; });
      const render::Rgba c = render::draw_color(drawn != drawing_.layers.end() ? drawn->color : std::string_view("7"),
                                                dark_ ? render::Theme::model() : render::Theme::light(), false);
      ImGui::ColorButton("##c", im(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, {12, 12});
      ImGui::SameLine();
      if (ImGui::Selectable(name.c_str(), name == cur, 0, {0, flag}) && name != cur) {  // highlighted: the current one
        cad_->set_layer(name, base::Value::object(), true);  // turns it on and thaws it too; one undo step
        message_ = words_->t("app.layer.current_status", {{"name", name}});
        after_input();
      }
      if (!on || frozen) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", words_->t(frozen ? "app.layer.freeze" : "app.turn_on").c_str());  // NOLINT
      }
      ImGui::PopID();
      if (patch) {
        cad_->set_layer(name, base::make_object({{patch->first, patch->second}}));
        after_input();
        redraw_ = 2;
      }
    }
  }
}

void Viewer::status_bar() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - status_height()});
  ImGui::SetNextWindowSize({vp->WorkSize.x, status_height()});
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  std::string s;
  if (!message_.empty()) {
    s = message_;
  } else if (doc_ && space_ < views_.size()) {
    const auto& sp = scene_.spaces.size() > space_ ? scene_.spaces[space_] : render::SceneSpace{};
    if (mouse_x_ >= cx_ && mouse_y_ >= cy_ && mouse_x_ < cx_ + cw_ && mouse_y_ < cy_ + ch_) {
      const auto w = render::to_world(views_[space_], mouse_x_ - cx_, mouse_y_ - cy_);
      s = fixed(w[0], 4) + ", " + fixed(w[1], 4) + "    ";
    }
    s += doc_->name + "  ·  " + (drawing_.release.empty() ? doc_->kind : drawing_.release) + "  ·  " +
         std::to_string(sp.segments) + " segments, " + std::to_string(sp.glyphs) + " glyphs  ·  " +
         fixed(stats_.frame_ms, 2) + " ms, " + std::to_string(stats_.draws) + " draws";
  }
  ImGui::TextUnformatted(s.c_str());
  ImGui::End();
}

void Viewer::ui(float dpr) {
  take_inbox();
  dpr_ = static_cast<double>(dpr);
  frame_start_ = SDL_GetTicksNS();
  if (touch_) {
    // menu rows (the popups too): the text + the spacing = 40 px
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{10, std::max(4.0F, 40.0F - ImGui::GetFontSize())});
  }
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu(words_->t("toolbar.group.file").c_str())) {
      if (ImGui::MenuItem(words_->t("ui.open").c_str(), "Ctrl+O") && services_.pick_open) {
        services_.pick_open();
      }
      if (ImGui::MenuItem(words_->t("ui.save").c_str(), "Ctrl+S", false, cad_ != nullptr)) {
        save(save_format(), false);
      }
      if (ImGui::MenuItem(words_->t("app.save_copy").c_str(), "Ctrl+Shift+S", false, cad_ != nullptr)) {
        save_as_open_ = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(words_->t("toolbar.group.view").c_str())) {
      if (ImGui::MenuItem(words_->t("ui.zoom_everything").c_str(), nullptr, false, doc_ != nullptr) &&
          space_ < views_.size()) {
        views_[space_] = render::fit(drawing_.spaces[space_], views_[space_]);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(words_->t("input.m.model").c_str(), nullptr, dark_)) {
        dark_ = true;
        apply_theme();
      }
      if (ImGui::MenuItem(words_->t("ui.paper").c_str(), nullptr, !dark_)) {
        dark_ = false;
        apply_theme();
      }
      ImGui::Separator();
      if (ImGui::MenuItem(words_->t(print_ ? "status.lwt.on" : "status.lwt.off").c_str(), nullptr, print_)) {
        print_ = !print_;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    redraw_ = std::max(redraw_, 1);
  }
  if (touch_) {
    ImGui::PopStyleVar();
  }
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_O) && services_.pick_open) {
    services_.pick_open();
  }
  save_as_dialog();
  layers_panel();
  // the space tabs, above the canvas
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x + layers_width(), vp->WorkPos.y});
  ImGui::SetNextWindowSize({middle_width(vp->WorkSize.x), tabs_height()});
  ImGui::Begin("##spaces", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  if (doc_ && ImGui::BeginTabBar("spaces", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (std::size_t s = 0; s < drawing_.spaces.size(); ++s) {
      const auto& sp = drawing_.spaces[s];
      const std::string label = (sp.kind == "model" ? words_->t("app.tab.model") : sp.name) + "##" + sp.key;
      if (ImGui::BeginTabItem(label.c_str())) {
        set_space(s);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
  status_bar();
  command_line();
  props_panel();  // after the command line: a field it focuses wins over the command line's focus
  quick_props();
  // the canvas: what is left
  cx_ = static_cast<double>(vp->WorkPos.x + layers_width());
  cy_ = static_cast<double>(vp->WorkPos.y + tabs_height());
  cw_ = std::max(1.0, static_cast<double>(middle_width(vp->WorkSize.x)));
  ch_ = std::max(
      1.0, static_cast<double>(vp->WorkSize.y - tabs_height() - status_height() - (engine_ ? command_height() : 0.0F)));
  for (render::View& v : views_) {
    v.width = cw_;  // keep the centre and the zoom when the window changes size
    v.height = ch_;
    v.dpr = dpr_;
  }
  if (fit_pending_ && doc_) {
    for (std::size_t s = 0; s < views_.size(); ++s) {
      views_[s] = render::fit(drawing_.spaces[s], views_[s]);
    }
    fit_pending_ = false;
  }
  overlay();
  if (redraw_ > 0) {
    --redraw_;
  }
}

void Viewer::command_line() {
  if (!engine_) {
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = command_height();
  ImGui::SetNextWindowPos({vp->WorkPos.x + layers_width(), vp->WorkPos.y + vp->WorkSize.y - status_height() - h});
  ImGui::SetNextWindowSize({middle_width(vp->WorkSize.x), h});
  ImGui::Begin("##command", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  // the last lines it printed, then the prompt and what is being typed
  const std::size_t shown = 2;
  for (std::size_t i = printed_.size() > shown ? printed_.size() - shown : 0; i < printed_.size(); ++i) {
    ImGui::TextDisabled("%s", printed_[i].c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  const std::string label = prompt_.empty() ? words_->t("ui.command") : prompt_;
  ImGui::TextUnformatted(label.c_str());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    ImGui::SetKeyboardFocusHere();  // typing goes to the command line, as in CadCraft
  }
  const bool enter = ImGui::InputText("##line", line_.data(), line_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
  std::string typed(line_.data());
  // Space sends the line too, except where a text is asked for
  const bool space = !typed.empty() && typed.back() == ' ' && engine_->expects() != "string";
  if (enter || space) {
    if (space) {
      typed.pop_back();
    }
    line_.fill('\0');
    engine_->text(typed);
    after_input();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    line_.fill('\0');
    (void)engine_->escape();
    after_input();
  }
  ImGui::End();
}

void Viewer::overlay() {
  if (!cad_ || space_ >= views_.size()) {
    return;
  }
  const render::View& v = views_[space_];
  const render::Affine m = render::world_to_device(v);
  const auto screen = [&](double x, double y) {
    return ImVec2{static_cast<float>((m[0] * x + m[2] * y + m[4]) / dpr_ + cx_),
                  static_cast<float>((m[1] * x + m[3] * y + m[5]) / dpr_ + cy_)};
  };
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->PushClipRect({static_cast<float>(cx_), static_cast<float>(cy_)},
                   {static_cast<float>(cx_ + cw_), static_cast<float>(cy_ + ch_)}, true);
  const cad::BlockLookup blocks = [this](const std::string& n) { return cad_->blocks().get(n); };
  const auto draw = [&](const base::Value& e, ImU32 color, float width) {
    for (const cad::Prim& p : cad::entity_prims(e, blocks)) {
      if (!p.arc) {
        dl->AddLine(screen(p.x1, p.y1), screen(p.x2, p.y2), color, width);
        continue;
      }
      const int n = std::clamp(static_cast<int>(std::ceil((p.a1 - p.a0) / (2 * std::numbers::pi) * 96)), 2, 96);
      ImVec2 prev = screen(p.cx + p.r * std::cos(p.a0), p.cy + p.r * std::sin(p.a0));
      for (int i = 1; i <= n; ++i) {
        const double a = p.a0 + (p.a1 - p.a0) * i / n;
        const ImVec2 q = screen(p.cx + p.r * std::cos(a), p.cy + p.r * std::sin(a));
        dl->AddLine(prev, q, color, width);
        prev = q;
      }
    }
  };
  const ImU32 action = IM_COL32(0x1E, 0x48, 0xC8, 0xFF);  // --action
  const ImU32 select = IM_COL32(0x4C, 0x8D, 0xFF, 0xFF);
  if (cmd_host_) {
    for (const doc::Id id : cmd_host_->selection()) {
      if (const base::Value* e = cad_->entities().get(id)) {
        draw(*e, select, 2.0F);
      }
    }
  }
  const bool hover = mouse_x_ >= cx_ && mouse_y_ >= cy_ && mouse_x_ < cx_ + cw_ && mouse_y_ < cy_ + ch_;
  const ImU32 grip_color = IM_COL32(0x1E, 0x48, 0xC8, 0xFF);
  const ImU32 hot = IM_COL32(0xE0, 0x40, 0x30, 0xFF);
  if (engine_ && !engine_->busy() && cmd_host_) {
    for (const doc::Id id : cmd_host_->selection()) {
      if (const base::Value* e = cad_->entities().get(id)) {
        for (const cad::Grip& gr : cad::grips(*e)) {
          const ImVec2 c = screen(gr.x, gr.y);
          const bool is_hot = grip_ && grip_->id == id && grip_->key == gr.key;
          dl->AddRectFilled(ImVec2{c.x - 4, c.y - 4}, ImVec2{c.x + 4, c.y + 4}, is_hot ? hot : grip_color);
        }
      }
    }
  }
  if (grip_ && hover) {
    const GripEdit ge = *grip_;
    const auto to = point_for(world_at(mouse_x_, mouse_y_));
    if (const base::Value* e = cad_->entities().get(ge.id)) {
      draw(cad::grip_move(*e, ge.key, {to[0], to[1]}), action, 1.0F);
    }
  }
  if (press_ && moved_ && press_button_ == SDL_BUTTON_LEFT && can_select()) {
    const bool crossing = mouse_x_ < press_x_;
    const ImU32 box = crossing ? IM_COL32(0x30, 0xA0, 0x50, 0xFF) : IM_COL32(0x1E, 0x48, 0xC8, 0xFF);
    const ImVec2 a{static_cast<float>(press_x_), static_cast<float>(press_y_)};
    const ImVec2 b{static_cast<float>(mouse_x_), static_cast<float>(mouse_y_)};
    dl->AddRectFilled(ImVec2{std::min(a.x, b.x), std::min(a.y, b.y)}, ImVec2{std::max(a.x, b.x), std::max(a.y, b.y)},
                      (box & 0x00FFFFFFU) | 0x30000000U);
    dl->AddRect(ImVec2{std::min(a.x, b.x), std::min(a.y, b.y)}, ImVec2{std::max(a.x, b.x), std::max(a.y, b.y)}, box);
  }
  if (snap_ && hover && wants_point()) {
    const ImVec2 c = screen(snap_->x, snap_->y);
    const ImU32 sc = IM_COL32(0xA8, 0x4A, 0x00, 0xFF);  // --layer-orange, as view.js
    if (snap_->kind == "end") {
      dl->AddRect(ImVec2{c.x - 5, c.y - 5}, ImVec2{c.x + 5, c.y + 5}, sc, 0.0F, 2.0F);
    } else if (snap_->kind == "mid") {
      dl->AddTriangle({c.x, c.y - 6}, {c.x - 6, c.y + 5}, {c.x + 6, c.y + 5}, sc, 2);
    } else if (snap_->kind == "cen") {
      dl->AddCircle(c, 6, sc, 16, 2);
    } else if (snap_->kind == "int") {
      dl->AddLine({c.x - 5, c.y - 5}, {c.x + 5, c.y + 5}, sc, 2);
      dl->AddLine({c.x - 5, c.y + 5}, {c.x + 5, c.y - 5}, sc, 2);
    } else {
      dl->AddQuad({c.x, c.y - 6}, {c.x + 6, c.y}, {c.x, c.y + 6}, {c.x - 6, c.y}, sc, 2);
    }
  }
  if (engine_ && engine_->busy() && hover) {
    const auto w = point_for(world_at(mouse_x_, mouse_y_));
    for (const base::Value& e : engine_->preview({w[0], w[1]})) {
      draw(e, action, 1.0F);
    }
    // the crosshair at the cursor
    const auto mx = static_cast<float>(mouse_x_);
    const auto my = static_cast<float>(mouse_y_);
    dl->AddLine({mx - 20, my}, {mx + 20, my}, action, 1.0F);
    dl->AddLine({mx, my - 20}, {mx, my + 20}, action, 1.0F);
  }
  dl->PopClipRect();
}

void Viewer::render(const wgpu::RenderPassEncoder& pass, std::uint32_t width, std::uint32_t height) {
  stats_.draws = 0;
  if (!doc_ || space_ >= views_.size()) {
    return;
  }
  if (font_) {
    renderer_.upload_atlas(font_->atlas());
  }
  render::FrameOptions fo;
  const bool paper = drawing_.spaces[space_].kind == "paper";
  if (print_) {
    fo.theme = render::Theme::print(paper);
  } else {
    fo.theme = dark_ ? render::Theme::model() : render::Theme::light();
  }
  fo.print = print_;
  fo.hidden = &hidden_;
  std::vector<render::DrawBatch> draws = render::frame_batches(scene_, drawing_, space_, views_[space_], fo);
  const double ox = cx_ * dpr_;
  const double oy = cy_ * dpr_;
  for (render::DrawBatch& d : draws) {  // the canvas sits below the tabs, right of the layers panel
    d.to_device[4] += ox;
    d.to_device[5] += oy;
    d.scissor[0] += static_cast<float>(ox);
    d.scissor[1] += static_cast<float>(oy);
  }
  renderer_.draw(
      pass, draws, width, height,
      {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(cw_ * dpr_), static_cast<float>(ch_ * dpr_)});
  stats_.draws = renderer_.last_draw_calls();
  stats_.frame_ms = static_cast<double>(SDL_GetTicksNS() - frame_start_) / 1e6;
}

}  // namespace cc::ui
