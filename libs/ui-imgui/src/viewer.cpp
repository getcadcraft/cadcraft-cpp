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
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/import.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/docview/drawing.hpp"
#include "cc/engine/open.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"

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
  [[nodiscard]] std::optional<cmd::Pt> cursor() const override {
    if (v_.mouse_x_ < v_.cx_ || v_.mouse_y_ < v_.cy_ || v_.space_ >= v_.views_.size()) {
      return std::nullopt;
    }
    const auto w = v_.world_at(v_.mouse_x_, v_.mouse_y_);
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
    cad_->load(cad::import_dwg(doc_->model, doc_->options.version_tag, *words_), *words_);
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
    open_item(std::move(it));
  }
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
  redraw_ = std::max(redraw_, 2);
}

void Viewer::after_input() {
  if (cad_ && cad_->version() != drawn_version_) {
    rebuild();
  }
  redraw_ = std::max(redraw_, 2);
}

void Viewer::command(std::string_view line) {
  if (engine_) {
    engine_->text(line);
    after_input();
  }
}

void Viewer::canvas_click(double x, double y, bool right) {
  if (!engine_ || space_ >= views_.size()) {
    return;
  }
  if (right) {
    engine_->text("");  // a right click is Enter
    after_input();
    return;
  }
  const auto w = world_at(x, y);
  if (engine_->busy()) {
    (void)engine_->click({w[0], w[1]});
  } else if (cmd_host_) {
    // noun-verb: a click on an object adds it to the selection (on nothing: clears it)
    cmd::IdSet sel = cmd_host_->selection();
    if (const auto id = cmd_host_->pick_at({w[0], w[1]})) {
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
  if (e.type == SDL_EVENT_KEY_DOWN && engine_ && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0 &&
      line_[0] == '\0' && !engine_->busy()) {
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
          dragging_ = e.button.button != SDL_BUTTON_RIGHT;
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
    const auto& layers = drawing_.layers;
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
          if (cad_) {
            const std::string name = layers[idx].name;
            cad_->change("LAYER", [&] { cad_->set_layer(name, base::make_object({{"on", on}})); });
            after_input();
          } else {
            hidden_[idx] = !on;
          }
          redraw_ = 2;
        }
        ImGui::PopID();
      }
    }
  } else {
    ImGui::TextWrapped("%s", words_->t("ui.drop_dwg_dxf_open").c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
  }
  ImGui::End();
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
  layers_panel();
  // the space tabs, above the canvas
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x + layers_width(), vp->WorkPos.y});
  ImGui::SetNextWindowSize({vp->WorkSize.x - layers_width(), tabs_height()});
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
  // the canvas: what is left
  cx_ = static_cast<double>(vp->WorkPos.x + layers_width());
  cy_ = static_cast<double>(vp->WorkPos.y + tabs_height());
  cw_ = std::max(1.0, static_cast<double>(vp->WorkSize.x - layers_width()));
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
  ImGui::SetNextWindowSize({vp->WorkSize.x - layers_width(), h});
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
  if (engine_ && engine_->busy() && mouse_x_ >= cx_ && mouse_y_ >= cy_ && mouse_x_ < cx_ + cw_ &&
      mouse_y_ < cy_ + ch_) {
    const auto w = world_at(mouse_x_, mouse_y_);
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
