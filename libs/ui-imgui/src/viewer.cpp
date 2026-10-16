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

constexpr float kLayersWidth = 240;
constexpr float kStatusHeight = 24;
constexpr float kTabsHeight = 30;

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
  st.FramePadding = {6, 3};
  st.ItemSpacing = {6, 4};
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
  if (!doc_ || space_ >= views_.size()) {
    return;
  }
  render::View& v = views_[space_];
  auto inside = [&](double x, double y) { return x >= cx_ && y >= cy_ && x < cx_ + cw_ && y < cy_ + ch_; };
  switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
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

void Viewer::layers_panel() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float top = vp->WorkPos.y;
  ImGui::SetNextWindowPos({vp->WorkPos.x, top});
  ImGui::SetNextWindowSize({kLayersWidth, vp->WorkSize.y - kStatusHeight});
  ImGui::Begin(words_->t("ui.layers").c_str(), nullptr,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (doc_) {
    if (ImGui::SmallButton(words_->t("input.cmd.layers.on").c_str())) {
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
  ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kStatusHeight});
  ImGui::SetNextWindowSize({vp->WorkSize.x, kStatusHeight});
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
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_O) && services_.pick_open) {
    services_.pick_open();
  }
  layers_panel();
  // the space tabs, above the canvas
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x + kLayersWidth, vp->WorkPos.y});
  ImGui::SetNextWindowSize({vp->WorkSize.x - kLayersWidth, kTabsHeight});
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
  cx_ = static_cast<double>(vp->WorkPos.x + kLayersWidth);
  cy_ = static_cast<double>(vp->WorkPos.y + kTabsHeight);
  cw_ = std::max(1.0, static_cast<double>(vp->WorkSize.x - kLayersWidth));
  ch_ = std::max(1.0, static_cast<double>(vp->WorkSize.y - kTabsHeight - kStatusHeight));
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
