// SPDX-License-Identifier: Apache-2.0
// The 2D viewer: CadCraft's drawing view (BimCraft 0.5's dwg-view.js behaviour) in Dear ImGui chrome — a menu bar,
// the space tabs (Model and each layout), the layers panel, a status bar, and the canvas: drag to pan, the wheel zooms
// about the cursor, a double-click fits. The same code runs on desktop and on the web; the host (apps/cadcraft,
// apps/cadcraft-web) owns the window, the surface and the loop, and hands files in through the inbox.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <webgpu/webgpu_cpp.h>

#include "cc/engine/open.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"
#include "cc/text/font.hpp"
#include "cc/ui/gpu.hpp"
#include "cc/ui/scene_renderer.hpp"

namespace cc::ui {

/// Platform services the viewer asks for (AGENTS.md section 7): never called directly, always through the host.
struct Services {
  std::function<void()> pick_open;  // show the platform's file picker; the choice arrives through the inbox
};

/// Files handed to the viewer from another thread (a picker callback, a drop): taken on the UI thread.
class Inbox {
 public:
  struct Item {
    std::string name;
    std::string path;              // desktop: a path to open
    std::vector<std::byte> bytes;  // web: the file's bytes
  };
  void post(Item item);
  [[nodiscard]] std::vector<Item> take();

 private:
  std::mutex m_;  // lock order: alone (never held while calling out)
  std::vector<Item> items_;
};

class Viewer {
 public:
  /// `assets`: the folder with fonts/ and lang/. `locale`: "en" or "es-MX".
  Viewer(const Gpu& gpu, wgpu::TextureFormat target, Services services, const std::string& assets,
         const std::string& locale);
  ~Viewer();
  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;
  Viewer(Viewer&&) = delete;
  Viewer& operator=(Viewer&&) = delete;

  [[nodiscard]] Inbox& inbox() noexcept { return inbox_; }
  /// Shows a drawing already opened (the CLI's --open, a test).
  void show(std::unique_ptr<engine::OpenDrawing> d);
  /// Canvas input (after ImGui saw the event): pan, zoom, fit.
  void event(const SDL_Event& e, bool imgui_wants_mouse);
  /// The ImGui widgets of one frame (between ImGui::NewFrame and ImGui::Render). `dpr`: device px per window point.
  void ui(float dpr);
  /// The drawing into `pass` (target `width` × `height` device px); ImGui is drawn after it by the host.
  void render(const wgpu::RenderPassEncoder& pass, std::uint32_t width, std::uint32_t height);
  /// True while something on screen still has to change (the host may sleep otherwise).
  [[nodiscard]] bool busy() const noexcept { return redraw_ > 0; }
  /// The last frame's numbers, for the status bar and ?bench.
  struct Stats {
    double frame_ms = 0;
    std::size_t draws = 0;
  };
  [[nodiscard]] Stats stats() const noexcept { return stats_; }
  [[nodiscard]] const i18n::Catalog& words() const noexcept { return *words_; }
  /// The canvas colour (the host clears to it).
  [[nodiscard]] render::Rgba background() const;

 private:
  void take_inbox();
  void open_item(Inbox::Item item);
  void set_space(std::size_t s);
  void apply_theme() const;
  void layers_panel();
  void status_bar();

  Services services_;
  Inbox inbox_;
  std::unique_ptr<i18n::Catalog> english_;
  std::unique_ptr<i18n::Catalog> words_;
  std::unique_ptr<text::Font> font_;
  SceneRenderer renderer_;
  std::unique_ptr<engine::OpenDrawing> doc_;
  render::Scene scene_;
  std::size_t space_ = 0;
  std::vector<render::View> views_;  // one per space (each keeps its own zoom and pan)
  std::vector<bool> hidden_;         // per layer
  bool dark_ = true;
  bool print_ = false;
  std::string message_;  // the last error, shown in the status bar
  // the canvas, in window points
  double cx_ = 0, cy_ = 0, cw_ = 1, ch_ = 1;
  double dpr_ = 1;
  bool dragging_ = false;
  double last_x_ = 0, last_y_ = 0;
  double mouse_x_ = -1, mouse_y_ = -1;
  int redraw_ = 2;
  bool fit_pending_ = false;  // fit every space once the canvas has its size
  Stats stats_;
  std::uint64_t frame_start_ = 0;
};

}  // namespace cc::ui
