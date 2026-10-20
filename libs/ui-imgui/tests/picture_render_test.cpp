// SPDX-License-Identifier: Apache-2.0
// Pictures on screen (0.5 drawPictures): a drawing with a picture whose file it has draws that file's pixels where the
// picture lies, clipped to its frame — through the app's own pipeline (walk → drawing → scene → WebGPU), offscreen.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/cad/pictures.hpp"
#include "cc/doc/doc.hpp"
#include "cc/doc/pictures.hpp"
#include "cc/docview/drawing.hpp"
#include "cc/image/decode.hpp"
#include "cc/image/png.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"
#include "cc/text/font.hpp"
#include "cc/ui/gpu.hpp"
#include "cc/ui/scene_renderer.hpp"

namespace {

/// A 4 × 2 picture: the top row red, the bottom row blue (opaque).
std::vector<std::uint8_t> two_rows_png() {
  std::vector<std::uint8_t> rgba;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      rgba.insert(rgba.end(),
                  {static_cast<std::uint8_t>(y == 0 ? 255 : 0), 0, static_cast<std::uint8_t>(y == 0 ? 0 : 255), 255});
    }
  }
  const auto png = cc::image::encode_png(rgba, 4, 2);
  std::vector<std::uint8_t> out;
  for (const std::byte b : png.value_or(std::vector<std::byte>{})) {
    out.push_back(std::to_integer<std::uint8_t>(b));
  }
  return out;
}

}  // namespace

// Under the sanitizers the GPU driver (not instrumented, and leaking at exit) is left out: Dawn's null adapter runs the
// same path (decode, textures, the draws) without pixels to check.
#if defined(CC_SANITIZED)
constexpr bool kPixels = false;
#else
constexpr bool kPixels = true;
#endif

TEST_CASE("pictures: a picture draws its file's pixels, clipped to its frame (drawPictures)") {
  auto gpu = cc::ui::create_gpu({.backend = kPixels ? std::string() : std::string("null"), .make_surface = {}});
  if (!gpu) {
    SKIP("no GPU adapter: " + gpu.error().message);
  }
  auto font_bytes = cc::base::read_file(std::filesystem::path(CC_ASSETS_DIR) / "fonts/archivo/Archivo-VF.ttf",
                                        std::uint64_t{1} << 24U);
  REQUIRE(font_bytes);
  auto font = cc::text::Font::load(std::move(*font_bytes), {.width = 75, .texture_limit = 4096});
  REQUIRE(font);

  cc::doc::Doc d;
  const std::vector<std::uint8_t> png = two_rows_png();
  REQUIRE(cc::doc::add_picture(d, "rows.png", png));
  // the picture 40 × 20 units at (0, 0); a second one clipped to its left half at (100, 0)
  const cc::base::Value pic = cc::base::make_object(
      {{"file", "rows.png"}, {"x", 0}, {"y", 0}, {"ux", 10}, {"uy", 0}, {"vx", 0}, {"vy", 10}, {"w", 4}, {"h", 2}});
  cc::base::Array clip;
  for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{20.0, 0.0}, std::pair{20.0, 20.0}, std::pair{0.0, 20.0}}) {
    clip.push_back(cc::base::make_object({{"x", x}, {"y", y}}));
  }
  const cc::base::Value clip_v(std::move(clip));
  d.change("PICTURES", [&] {
    (void)d.define_block(cc::cad::picture_block("*I1", pic, nullptr));
    (void)d.define_block(cc::cad::picture_block("*I2", pic, &clip_v));
    for (const auto& [name, x] : {std::pair{"*I1", 0.0}, std::pair{"*I2", 100.0}}) {
      (void)d.add(cc::base::make_object(
          {{"type", "insert"}, {"layer", "0"}, {"block", name}, {"x", x}, {"y", 0}, {"sx", 1}, {"sy", 1}, {"rot", 0}}));
    }
  });
  const cc::drawing::Drawing drawing = cc::docview::to_drawing(d);
  REQUIRE_FALSE(drawing.spaces.empty());
  CHECK(drawing.spaces.front().bitmaps.size() == 2);
  const cc::render::Scene scene = cc::render::build_scene(drawing, **font);
  cc::ui::SceneRenderer renderer(gpu->device, cc::ui::kOffscreenFormat);
  renderer.upload(scene, [&d](const std::string& file) -> std::optional<cc::image::Rgba> {
    const cc::doc::PictureFile* rec = cc::doc::find_picture(d, file);
    return rec != nullptr ? cc::image::decode_picture(rec->bytes) : std::nullopt;
  });
  renderer.upload_atlas((*font)->atlas());
  // 1 px per unit, the world's (−10, −10) at the top-left corner: y up becomes y down
  const cc::render::View v{.cx = 70, .cy = 10, .zoom = 1, .width = 160, .height = 40, .dpr = 1};
  cc::render::FrameOptions fo;
  const auto draws = cc::render::frame_batches(scene, drawing, 0, v, fo);
  auto shot =
      cc::ui::render_offscreen(*gpu, renderer, draws, 160, 40, cc::render::Rgba{.r = 1, .g = 1, .b = 1, .a = 1});
  REQUIRE(shot);
  if (!kPixels || gpu->backend == "null") {
    if (kPixels) {
      WARN("no adapter that draws (" << gpu->adapter_name << "): the pixels are not checked");
    }
    return;
  }
  const auto px = [&shot](int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * shot->width + static_cast<std::size_t>(x)) * 4;
    return std::array<int, 3>{shot->rgba[i], shot->rgba[i + 1], shot->rgba[i + 2]};
  };
  // world (wx, wy) → pixel (wx − 70 + 80, 20 − (wy − 10))
  const auto at = [&px](double wx, double wy) { return px(static_cast<int>(wx + 10), static_cast<int>(30 - wy)); };
  const auto reddish = [](std::array<int, 3> c) { return c[0] > 200 && c[1] < 60 && c[2] < 60; };
  const auto bluish = [](std::array<int, 3> c) { return c[2] > 200 && c[0] < 60 && c[1] < 60; };
  CHECK(reddish(at(20, 15)));         // the top row
  CHECK(bluish(at(20, 5)));           // the bottom row
  CHECK(reddish(at(110, 15)));        // the clipped one: its left half
  CHECK_FALSE(reddish(at(130, 15)));  // its right half is clipped away
  CHECK_FALSE(bluish(at(130, 5)));
}
