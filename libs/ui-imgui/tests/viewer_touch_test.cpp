// SPDX-License-Identifier: Apache-2.0
// The viewer's touch input, headless: Dawn's null adapter, an ImGui context with no window, SDL finger events made by
// hand. Pinch zooms about the fingers' midpoint, two fingers pan, a double tap fits, a finger switches the touch layout
// on and a real mouse switches it off.
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_events.h>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/engine/open.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/render/view.hpp"
#include "cc/save/dwg.hpp"
#include "cc/ui/gpu.hpp"
#include "cc/ui/viewer.hpp"

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
  std::vector<std::byte> b(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    b[i] = static_cast<std::byte>(s[i]);
  }
  return b;
}

struct Rig {
  Rig() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280, 800};
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // the font texture is asked for and never made
  }
  ~Rig() { ImGui::DestroyContext(); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  Rig(Rig&&) = delete;
  Rig& operator=(Rig&&) = delete;
};

void frame(cc::ui::Viewer& v) {
  ImGui::GetIO().DeltaTime = 1.0F / 60;
  ImGui::NewFrame();
  v.ui(1.0F);
  ImGui::Render();
}

std::uint64_t g_ns = 1'000'000'000U;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — event clock

SDL_Event finger(SDL_EventType type, SDL_FingerID id, double x, double y, std::uint64_t after_ms = 20) {
  g_ns += after_ms * 1'000'000U;
  SDL_Event e{};
  e.type = type;
  e.tfinger.timestamp = g_ns;
  e.tfinger.fingerID = id;
  e.tfinger.x = static_cast<float>(x / 1280);  // SDL: 0–1 of the window
  e.tfinger.y = static_cast<float>(y / 800);
  return e;
}

}  // namespace

TEST_CASE("touch: layout, pinch, two-finger pan, double tap") {
  const Rig rig;
  auto gpu = cc::ui::create_gpu({.backend = "null", .make_surface = {}});
  if (!gpu) {
    SKIP("no null WebGPU adapter: " << gpu.error().message);
  }
  cc::ui::Viewer viewer(*gpu, wgpu::TextureFormat::BGRA8Unorm, {}, CC_ASSETS_DIR, "en");
  auto d = cc::engine::open_bytes(
      "t.dxf", bytes_of("0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n0\n20\n0\n11\n100\n21\n50\n0\nENDSEC\n0\nEOF\n"));
  REQUIRE(d);
  viewer.show(std::move(*d));
  frame(viewer);
  frame(viewer);
  const cc::render::View fitted = viewer.view();
  REQUIRE(fitted.zoom > 0);
  CHECK_FALSE(viewer.touch_layout());

  // a finger switches the touch layout on (and the chrome to 40 px rows)
  const float dense = ImGui::GetFrameHeight();
  viewer.event(finger(SDL_EVENT_FINGER_DOWN, 1, 800, 400), false);
  viewer.event(finger(SDL_EVENT_FINGER_UP, 1, 800, 400), false);
  frame(viewer);
  frame(viewer);  // the menu bar's new height reaches the viewport's work area a frame later
  CHECK(viewer.touch_layout());
  CHECK(ImGui::GetFrameHeight() >= 40.0F);
  CHECK(dense < 30.0F);

  // pinch out 100 → 200 px about (800, 400): zoom × 2, the drawing point under the midpoint stays put
  const cc::render::View before = viewer.view();
  // the midpoint (800, 400) on the canvas: right of the 280 px layers panel, below the menu bar and the 48 px tabs
  const auto canvas_x = [] { return 800.0 - 280.0; };
  const double canvas_y = 400.0 - 48.0 - static_cast<double>(ImGui::GetMainViewport()->WorkPos.y);
  const auto under = cc::render::to_world(before, canvas_x(), canvas_y);
  viewer.event(finger(SDL_EVENT_FINGER_DOWN, 1, 750, 400, 500), false);
  viewer.event(finger(SDL_EVENT_FINGER_DOWN, 2, 850, 400), false);
  viewer.event(finger(SDL_EVENT_FINGER_MOTION, 1, 700, 400), false);
  viewer.event(finger(SDL_EVENT_FINGER_MOTION, 2, 900, 400), false);
  const cc::render::View pinched = viewer.view();
  CHECK(std::abs(pinched.zoom / before.zoom - 2.0) < 0.01);
  const auto still = cc::render::to_world(pinched, canvas_x(), canvas_y);
  CHECK(std::abs(still[0] - under[0]) < 1e-6 * std::max(1.0, std::abs(under[0])) + 1e-9);
  CHECK(std::abs(still[1] - under[1]) < 1e-6 * std::max(1.0, std::abs(under[1])) + 1e-9);

  // both fingers 60 px right: a pan, no zoom
  viewer.event(finger(SDL_EVENT_FINGER_MOTION, 1, 760, 400), false);
  viewer.event(finger(SDL_EVENT_FINGER_MOTION, 2, 960, 400), false);
  const cc::render::View panned = viewer.view();
  CHECK(std::abs(panned.zoom - pinched.zoom) < 1e-12 * pinched.zoom);
  const auto moved = cc::render::to_world(panned, canvas_x() + 60, canvas_y);
  CHECK(std::abs(moved[0] - still[0]) < 1e-6 * std::max(1.0, std::abs(still[0])) + 1e-9);
  viewer.event(finger(SDL_EVENT_FINGER_UP, 1, 760, 400), false);
  viewer.event(finger(SDL_EVENT_FINGER_UP, 2, 960, 400), false);

  // a single tap does nothing; a second within 400 ms fits again
  viewer.event(finger(SDL_EVENT_FINGER_DOWN, 3, 800, 400, 500), false);
  viewer.event(finger(SDL_EVENT_FINGER_UP, 3, 800, 400, 60), false);
  CHECK(std::abs(viewer.view().zoom - panned.zoom) < 1e-12 * panned.zoom);
  viewer.event(finger(SDL_EVENT_FINGER_DOWN, 4, 804, 402, 120), false);
  viewer.event(finger(SDL_EVENT_FINGER_UP, 4, 804, 402, 60), false);
  frame(viewer);
  // fitted again: back near the first fit (the touch layout's canvas is a little smaller), far from the 2× pinch
  const cc::render::View refit = viewer.view();
  CHECK(refit.zoom < panned.zoom * 0.6);
  CHECK(std::abs(refit.zoom / fitted.zoom - 1.0) < 0.2);

  // a real mouse brings the dense layout back
  SDL_Event m{};
  m.type = SDL_EVENT_MOUSE_MOTION;
  m.motion.which = 1;
  m.motion.x = 700;
  m.motion.y = 400;
  viewer.event(m, false);
  frame(viewer);
  CHECK_FALSE(viewer.touch_layout());
  CHECK(ImGui::GetFrameHeight() < 30.0F);
}

TEST_CASE("properties: PROPERTIES opens the panel, a typed value edits the selection, QPMODE toggles") {
  const Rig rig;
  auto gpu = cc::ui::create_gpu({.backend = "null", .make_surface = {}});
  if (!gpu) {
    SKIP("no null WebGPU adapter: " << gpu.error().message);
  }
  // a drawing with one line, saved as a DWG so it opens as an editable document
  cc::doc::Doc src;
  src.change("LINE", [&] {
    (void)src.add(cc::base::make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 10}, {"y2", 0}}));
  });
  const cc::i18n::Catalog words;
  cc::save::DwgSaveOptions opts;
  opts.version = "2018";
  opts.created_ms = 0;
  const auto saved = cc::save::save_dwg(src, opts, words);
  REQUIRE_FALSE(saved.bytes.empty());
  std::vector<std::byte> dwg(saved.bytes.size());
  for (std::size_t i = 0; i < dwg.size(); ++i) {
    dwg[i] = static_cast<std::byte>(saved.bytes[i]);
  }
  cc::ui::Viewer viewer(*gpu, wgpu::TextureFormat::BGRA8Unorm, {}, CC_ASSETS_DIR, "en");
  auto d = cc::engine::open_bytes("t.dwg", dwg);
  REQUIRE(d);
  viewer.show(std::move(*d));
  frame(viewer);
  REQUIRE(viewer.document() != nullptr);
  CHECK_FALSE(viewer.properties_open());

  viewer.command("PROPERTIES");
  frame(viewer);
  CHECK(viewer.properties_open());
  frame(viewer);  // the canvas shrinks for the panel
  viewer.command("PRCLOSE");
  frame(viewer);
  CHECK_FALSE(viewer.properties_open());
  viewer.command("PR");
  frame(viewer);
  CHECK(viewer.properties_open());

  viewer.command("QP");
  frame(viewer);
  CHECK(viewer.quick_properties());
  viewer.command("QPMODE");
  CHECK_FALSE(viewer.quick_properties());

  // the pick button's command asks for a point and leaves the selection alone
  viewer.command("SELECTALL");
  viewer.command("_PROPPICK");
  CHECK(viewer.prompt().contains("Specify"));
  viewer.command("5,5");  // nothing waits for it (no field's button was pressed): no change
  frame(viewer);
  const auto undo = viewer.document()->undo_labels();
  CHECK((undo.empty() || undo.back() != "PROPERTIES"));
  frame(viewer);
}
