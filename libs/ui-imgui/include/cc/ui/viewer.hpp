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
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <webgpu/webgpu_cpp.h>

#include "cc/base/value.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/snap.hpp"
#include "cc/cmd/engine.hpp"
#include "cc/cmd/props.hpp"
#include "cc/doc/doc.hpp"
#include "cc/drawing/compile.hpp"
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
  /// Desktop: the save dialog for a file `name` of kind `ext` ("dwg" / "dxf"); the path chosen arrives through the
  /// inbox as a save target. Unset on the web (the file downloads).
  std::function<void(const std::string& name, const std::string& ext)> pick_save;
  /// Web: the bytes handed to the browser as a download named `name`.
  std::function<void(const std::string& name, std::span<const std::byte> bytes)> download;
};

/// Files handed to the viewer from another thread (a picker callback, a drop): taken on the UI thread.
class Inbox {
 public:
  struct Item {
    std::string name;
    std::string path;              // desktop: a path to open
    std::vector<std::byte> bytes;  // web: the file's bytes
    bool save_target = false;      // a path chosen in the save dialog, for the save waiting on it
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
  ~Viewer();  // NOLINT(performance-trivially-destructible) — the engine before the document it edits
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
  /// True once the GPU pipelines exist (the web compiles them in the background); before, frames show no drawing.
  [[nodiscard]] bool ready() const noexcept { return renderer_.ready(); }
  /// True while something on screen still has to change (the host may sleep otherwise).
  [[nodiscard]] bool busy() const noexcept { return redraw_ > 0; }
  /// The last frame's numbers, for the status bar and ?bench.
  struct Stats {
    double frame_ms = 0;
    std::size_t draws = 0;
    engine::OpenTimes open;  // the last drawing opened: read, parse, compile
    double scene_ms = 0;     // its scene (geometry, text layout, new glyphs)
    double upload_ms = 0;    // its buffers to the GPU
  };
  [[nodiscard]] Stats stats() const noexcept { return stats_; }
  [[nodiscard]] const i18n::Catalog& words() const noexcept { return *words_; }
  /// The current space's view (tests, the status bar); a default view with no drawing open.
  [[nodiscard]] render::View view() const { return space_ < views_.size() ? views_[space_] : render::View{}; }
  /// True while the touch layout (40 px hit areas) is on: from the first finger until a real mouse moves.
  [[nodiscard]] bool touch_layout() const noexcept { return touch_; }
  /// The command line (tests, scripts): a typed line, as Enter sends it.
  void command(std::string_view line);
  /// The CadCraft document a DWG opens as (null with nothing open, or a DXF: shown, not edited yet).
  [[nodiscard]] const doc::Doc* document() const noexcept { return cad_.get(); }
  /// The prompt the command line shows ("" between commands).
  [[nodiscard]] const std::string& prompt() const noexcept { return prompt_; }
  /// The space shown: 0 = the model, n = the n-th sheet (its tab).
  [[nodiscard]] std::size_t space_index() const noexcept { return space_; }
  /// True while the Properties panel is open (PROPERTIES / PROPERTIESCLOSE, its close button).
  [[nodiscard]] bool properties_open() const noexcept { return props_open_; }
  /// True while Quick Properties is on (QPMODE).
  [[nodiscard]] bool quick_properties() const noexcept { return qp_on_; }
  /// The canvas colour (the host clears to it).
  [[nodiscard]] render::Rgba background() const;

 private:
  class CmdHost;
  void take_inbox();
  void rebuild();      // the document changed: its draw lists and scene again
  void after_input();  // after the command line took input
  void command_line();
  void overlay();
  void canvas_click(double x, double y, bool right);
  // saving (cadcraft.js saveFile / saveAs)
  [[nodiscard]] std::string save_format() const;
  [[nodiscard]] std::string dwg_version() const;
  [[nodiscard]] std::string format_label(const std::string& format) const;
  void save(std::string format, bool as);
  void save_to(const std::string& path);
  void save_as_dialog();
  /// PLOT (plot.js): the open sheet at 1:1, or the model at its extents on the default paper, as a PDF
  void plot_now();
  /// NEW (cadcraft.js newDrawing): a blank drawing named Drawing1 in place of this one.
  void new_drawing();
  void new_dialog();
  /// The command line on the document (show() and NEW), and what is drawn from it.
  void adopt_engine();
  /// The drawing on screen: its scene, layers, fitted views (show() and NEW).
  void present();
  [[nodiscard]] std::size_t doc_space_index() const;
  void canvas_window(double x0, double y0, double x1, double y1);
  [[nodiscard]] bool wants_point() const;
  [[nodiscard]] bool can_select() const;
  /// The point a click or a move stands for: an object snap, else ortho from the base point, else the cursor.
  [[nodiscard]] std::array<double, 2> point_for(std::array<double, 2> raw);
  [[nodiscard]] std::vector<const base::Value*> near_cursor(std::array<double, 2> p, double r);
  [[nodiscard]] std::array<double, 2> world_at(double x, double y) const;
  void open_item(Inbox::Item item);
  void set_space(std::size_t s);
  void apply_theme() const;
  void layers_panel();
  void editable_layers();
  void status_bar();
  // the Properties panel and Quick Properties (props_panel.cpp)
  void props_panel();
  void quick_props();
  [[nodiscard]] std::vector<doc::Id> props_selection() const;
  [[nodiscard]] cmd::props::Ctx props_ctx(const std::vector<doc::Id>& ids) const;
  [[nodiscard]] std::string prop_show(const cmd::props::Def& d, double v) const;
  void prop_apply(const std::string& group, const std::vector<doc::Id>& ids, const std::string& key,
                  const base::Value& raw);
  void prop_field(const cmd::props::Def& d, const cmd::props::Ctx& ctx, const std::string& group,
                  const std::vector<doc::Id>& ids, const std::string& uid);
  void prop_rows(const std::vector<cmd::props::Def>& defs, const cmd::props::Ctx& ctx, const std::string& group,
                 const std::vector<doc::Id>& ids, const std::string& prefix, bool pick);
  void props_picked(double x, double y);
  void finger(const SDL_Event& e);
  void set_touch(bool on);
  // the chrome's sizes (window points): larger in the touch layout
  [[nodiscard]] float layers_width() const noexcept { return touch_ ? 280.0F : 240.0F; }
  [[nodiscard]] float tabs_height() const noexcept { return touch_ ? 48.0F : 30.0F; }
  [[nodiscard]] float status_height() const noexcept { return touch_ ? 32.0F : 24.0F; }
  [[nodiscard]] float command_height() const noexcept { return touch_ ? 120.0F : 72.0F; }
  [[nodiscard]] float props_width() const noexcept {
    if (!props_open_) {
      return 0.0F;
    }
    return touch_ ? 340.0F : 300.0F;
  }
  /// The width between the layers panel and the Properties panel (the tabs, the canvas, the command line).
  [[nodiscard]] float middle_width(float total) const noexcept { return total - layers_width() - props_width(); }

  Services services_;
  Inbox inbox_;
  std::unique_ptr<i18n::Catalog> english_;
  std::unique_ptr<i18n::Catalog> words_;
  std::unique_ptr<text::Font> font_;
  SceneRenderer renderer_;
  std::unique_ptr<engine::OpenDrawing> doc_;
  drawing::Drawing drawing_;  // what is drawn: the document's (a DWG), else the file's as compiled (a DXF)
  std::unique_ptr<doc::Doc> cad_;
  std::unique_ptr<CmdHost> cmd_host_;
  std::unique_ptr<cmd::Engine> engine_;
  std::uint64_t drawn_version_ = 0;
  std::array<char, 512> line_{};
  std::vector<std::string> printed_;  // the command line's history (last 200)
  std::string prompt_;
  base::Array preview_;  // the rubber band the waiting command draws
  cad::SnapKinds kinds_ = {"end", "mid", "cen", "quad", "int", "perp", "node", "ins"};
  bool osnap_ = true;                                // F3
  bool ortho_ = false;                               // F8
  std::optional<cad::Snap> snap_;                    // the snap the cursor is on
  std::unordered_map<doc::Id, cad::Bounds> bounds_;  // per object, for the snaps' nearby test (the drawn version)
  struct GripEdit {
    doc::Id id = 0;
    std::string key;
    double x = 0, y = 0;
  };
  std::optional<GripEdit> grip_;            // a grip picked: the next click moves it there
  std::string source_path_;                 // the file the drawing came from (desktop), for Save in place
  std::string last_format_, last_version_;  // the format and DWG version chosen last (Save as)
  struct PendingSave {
    std::string format, name;
    std::vector<std::byte> bytes;
    base::Array notes;
    std::uint64_t version = 0;
  };
  std::optional<PendingSave> pending_save_;  // a save waiting for the dialog's path
  bool save_as_open_ = false;
  std::optional<std::pair<std::string, bool>> save_request_;  // SAVE / DXFOUT (format, false) or SAVEAS (…, true)
  std::optional<render::View> prev_view_;                     // ZOOM P
  bool plot_request_ = false;
  cmd::Clipboard clipboard_;                        // COPYCLIP / PASTECLIP: kept when another drawing opens
  bool new_request_ = false, new_confirm_ = false;  // NEW: a blank drawing (asked first when this one has changes) //
                                                    // PLOT asked: the PDF is made once the command ends
  std::optional<std::size_t> tab_select_;           // a tab the document opened (LAYOUT, VPVIEW): selected next frame
  std::vector<bool> need_fit_;                      // spaces whose view is fitted on the next frame (new sheets)
  int save_as_format_ = 0, save_as_version_ = 5;
  bool press_ = false;  // a button down on the canvas; `moved_` once it went beyond a click
  bool moved_ = false;
  int press_button_ = 0;
  double press_x_ = 0, press_y_ = 0;
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
  // touch: the layout with 40 px hit areas while fingers are in use; the fingers down (window points) for pinch / pan
  bool touch_ = false;
  struct Finger {
    SDL_FingerID id = 0;
    double x = 0, y = 0;
  };
  std::vector<Finger> fingers_;
  // a tap: one finger down and up again in place, quickly; two in a row = fit (double-click's touch twin)
  struct Tap {
    std::uint64_t down_ns = 0, up_ns = 0;
    double x = 0, y = 0;
    bool alone = false;  // no second finger joined
  };
  Tap tap_, last_tap_;
  // the Properties panel: ids only (looked up again on every edit), the group shown, the field being typed in
  bool props_open_ = false, qp_on_ = false;
  std::string props_sig_, props_group_, qp_for_, qp_dismissed_;
  float qp_x_ = 0, qp_y_ = 0;
  int props_vertex_ = 0;
  std::optional<doc::Id> props_vertex_of_;
  std::string prop_active_;  // the ImGui id of the field being edited
  std::array<char, 2048> prop_buf_{};
  bool prop_focus_ = false;
  struct PropPick {  // a point's pick button: _PROPPICK asks for the point, then both axes change in one step
    std::string group, name, label;
    std::vector<doc::Id> ids;
  };
  std::optional<PropPick> prop_pick_;
  Stats stats_;
  std::uint64_t frame_start_ = 0;
};

}  // namespace cc::ui
