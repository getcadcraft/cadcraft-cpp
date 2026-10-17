// SPDX-License-Identifier: Apache-2.0
#include "cc/ui/viewer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "cc/base/file.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
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

Viewer::~Viewer() = default;

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
  st.ScrollbarSize = touch_ ? 24 : 14;
  st.GrabMinSize = touch_ ? 24 : 12;
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
  const bool paper = doc_ && space_ < doc_->drawing.spaces.size() && doc_->drawing.spaces[space_].kind == "paper";
  if (print_) {
    return render::Theme::print(paper).sheet;
  }
  return dark_ ? render::Theme::model().sheet : render::Theme::light().sheet;
}

void Viewer::show(std::unique_ptr<engine::OpenDrawing> d) {
  doc_ = std::move(d);
  stats_.open = doc_->times;
  const std::uint64_t t0 = SDL_GetTicksNS();
  scene_ = font_ ? render::build_scene(doc_->drawing, *font_) : render::Scene{};
  const std::uint64_t t1 = SDL_GetTicksNS();
  renderer_.upload(scene_);
  if (font_) {
    renderer_.upload_atlas(font_->atlas());
  }
  stats_.scene_ms = static_cast<double>(t1 - t0) / 1e6;
  stats_.upload_ms = static_cast<double>(SDL_GetTicksNS() - t1) / 1e6;
  hidden_.assign(doc_->drawing.layers.size(), false);
  for (std::size_t i = 0; i < hidden_.size(); ++i) {
    hidden_[i] = !doc_->drawing.layers[i].on;
  }
  views_.assign(doc_->drawing.spaces.size(), render::View{});
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
  if (doc_ && s < doc_->drawing.spaces.size() && s != space_) {
    space_ = s;
    redraw_ = 2;
  }
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
  render::View& v = views_[space_];
  auto inside = [&](double x, double y) { return x >= cx_ && y >= cy_ && x < cx_ + cw_ && y < cy_ + ch_; };
  switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (fingers_.size() >= 2 && e.button.which == SDL_TOUCH_MOUSEID) {
        break;  // the first finger of a pinch, as a mouse: the pinch has it
      }
      if (!imgui_wants_mouse && inside(static_cast<double>(e.button.x), static_cast<double>(e.button.y))) {
        if (e.button.clicks >= 2 && e.button.button == SDL_BUTTON_LEFT) {
          v = render::fit(doc_->drawing.spaces[space_], v);
        } else if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE) {
          dragging_ = true;
          last_x_ = static_cast<double>(e.button.x);
          last_y_ = static_cast<double>(e.button.y);
        }
        redraw_ = 2;
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP: dragging_ = false; break;
    case SDL_EVENT_MOUSE_MOTION:
      if (e.motion.which != SDL_TOUCH_MOUSEID && touch_) {
        set_touch(false);  // a real mouse again: back to the dense layout
      }
      if (fingers_.size() >= 2 && e.motion.which == SDL_TOUCH_MOUSEID) {
        break;
      }
      mouse_x_ = static_cast<double>(e.motion.x);
      mouse_y_ = static_cast<double>(e.motion.y);
      if (dragging_) {
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
        views_[space_] = render::fit(doc_->drawing.spaces[space_], views_[space_]);
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
    const auto& layers = doc_->drawing.layers;
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
    s += doc_->name + "  ·  " + (doc_->drawing.release.empty() ? doc_->kind : doc_->drawing.release) + "  ·  " +
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
        views_[space_] = render::fit(doc_->drawing.spaces[space_], views_[space_]);
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
    for (std::size_t s = 0; s < doc_->drawing.spaces.size(); ++s) {
      const auto& sp = doc_->drawing.spaces[s];
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
  // the canvas: what is left
  cx_ = static_cast<double>(vp->WorkPos.x + layers_width());
  cy_ = static_cast<double>(vp->WorkPos.y + tabs_height());
  cw_ = std::max(1.0, static_cast<double>(vp->WorkSize.x - layers_width()));
  ch_ = std::max(1.0, static_cast<double>(vp->WorkSize.y - tabs_height() - status_height()));
  for (render::View& v : views_) {
    v.width = cw_;  // keep the centre and the zoom when the window changes size
    v.height = ch_;
    v.dpr = dpr_;
  }
  if (fit_pending_ && doc_) {
    for (std::size_t s = 0; s < views_.size(); ++s) {
      views_[s] = render::fit(doc_->drawing.spaces[s], views_[s]);
    }
    fit_pending_ = false;
  }
  if (redraw_ > 0) {
    --redraw_;
  }
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
  const bool paper = doc_->drawing.spaces[space_].kind == "paper";
  if (print_) {
    fo.theme = render::Theme::print(paper);
  } else {
    fo.theme = dark_ ? render::Theme::model() : render::Theme::light();
  }
  fo.print = print_;
  fo.hidden = &hidden_;
  std::vector<render::DrawBatch> draws = render::frame_batches(scene_, doc_->drawing, space_, views_[space_], fo);
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
