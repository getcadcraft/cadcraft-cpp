// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/drawing/compile.hpp"
#include "cc/dxf/read.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"
#include "cc/text/font.hpp"

using Catch::Approx;
using namespace cc;

namespace {
std::string hex(const render::Rgba& c) {
  auto b = [](float v) { return static_cast<unsigned>(std::lround(v * 255)); };
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s = "#";
  for (const unsigned v : {b(c.r), b(c.g), b(c.b)}) {
    s += kHex[v >> 4U];
    s += kHex[v & 15U];
  }
  return s;
}
std::unique_ptr<text::Font> font() {
  auto bytes = base::read_file(CC_FONT, std::uint64_t{1} << 24U);
  REQUIRE(bytes.has_value());
  auto f = text::Font::load(std::move(*bytes), {.width = 75});
  REQUIRE(f.has_value());
  return std::move(*f);
}
}  // namespace

TEST_CASE("colours follow dwg-view.js: ink, and pale or dark colours adjusted for the background") {
  // expected values from the JS rules run in Node
  CHECK(hex(render::draw_color("#ffff00", render::Theme::light(), false)) == "#8c8c00");
  CHECK(hex(render::draw_color("#0000ff", render::Theme::model(), false)) == "#7373ff");
  CHECK(hex(render::draw_color("#ff0000", render::Theme::model(), false)) == "#ff0000");
  CHECK(hex(render::draw_color("#c0c0c0", render::Theme::light(), false)) == "#c0c0c0");
  CHECK(hex(render::draw_color("ink", render::Theme::model(), false)) == "#e8eef4");
  CHECK(hex(render::draw_color("#ff0000", render::Theme::print(false), true)) == "#000000");
  CHECK(render::Theme::model().dark);
  CHECK_FALSE(render::Theme::light().dark);
}

TEST_CASE("fit, zoom about the cursor and pan keep the drawing where the JS view puts it") {
  drawing::Space s;
  s.kind = "model";
  s.bounds = {0, 0, 100, 50};
  render::View v{.width = 800, .height = 600, .dpr = 2};
  v = render::fit(s, v);
  CHECK(v.cx == 50);
  CHECK(v.cy == 25);
  CHECK(v.zoom == Approx(8 * 0.92));
  const auto before = render::to_world(v, 100, 100);
  v = render::zoom_at(v, 100, 100, 1.7);
  const auto after = render::to_world(v, 100, 100);
  CHECK(after[0] == Approx(before[0]));
  CHECK(after[1] == Approx(before[1]));
  const auto m = render::world_to_device(v);
  CHECK(m[0] * v.cx + m[4] == Approx(400 * 2));  // the centre lands mid-canvas, in device px
  CHECK(m[3] * v.cy + m[5] == Approx(300 * 2));
  v = render::pan_by(v, 10, 0);
  CHECK(render::to_world(v, 110, 100)[0] == Approx(after[0]));
}

TEST_CASE("survey coordinates stay steady: the floats are relative to each batch's origin") {
  drawing::Drawing d;
  drawing::Space s;
  s.kind = "model";
  drawing::Batch b;
  b.layer = "0";
  b.color = "ink";
  b.lines.push_back({500000.0, 4000000.0, 500000.001, 4000000.001});  // 1 mm in survey metres
  s.batches.push_back(b);
  s.bounds = {500000.0, 4000000.0, 500000.001, 4000000.001};
  d.spaces.push_back(s);
  d.layers.push_back({"0", "ink", true, 0.25, true});
  auto f = font();
  const render::Scene scene = render::build_scene(d, *f);
  const auto& sb = scene.spaces[0].batches[0];
  REQUIRE(sb.segments.size() == 4);
  CHECK(sb.segments[2] == Approx(0.001F).epsilon(1e-4));  // full float precision near the origin
  render::View v{.width = 800, .height = 600};
  v = render::fit(s, v);
  const auto draws = render::frame_batches(scene, d, 0, v, {});
  REQUIRE(draws.size() == 1);
  const auto& m = draws[0].to_device;  // origin-relative → device: the end point lands where it should
  const double x1 = m[0] * static_cast<double>(sb.segments[2]) + m[2] * static_cast<double>(sb.segments[3]) + m[4];
  const auto dev = render::world_to_device(v);
  CHECK(x1 == Approx(dev[0] * 500000.001 + dev[4]).margin(1e-3));
}

TEST_CASE("the kitchen fixture becomes a scene; hidden layers and print view drop batches") {
  const std::filesystem::path f = std::filesystem::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dxf";
  if (!std::filesystem::exists(f)) {
    SKIP("public fixtures not fetched (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const auto bytes = base::read_file(f, std::uint64_t{1} << 28U);
  REQUIRE(bytes.has_value());
  const auto model = dxf::read_dxf_model(dxf::decode_text(*bytes));
  REQUIRE(model.has_value());
  const drawing::Drawing d = drawing::compile_drawing(*model);
  auto fnt = font();
  const render::Scene scene = render::build_scene(d, *fnt);
  REQUIRE(scene.spaces.size() == d.spaces.size());
  CHECK(scene.spaces[0].segments > 1000);
  CHECK(scene.spaces[0].glyphs > 10);
  render::View v{.width = 1200, .height = 800};
  v = render::fit(d.spaces[0], v);
  const auto all = render::frame_batches(scene, d, 0, v, {});
  std::vector<bool> hidden(d.layers.size(), true);
  const auto none = render::frame_batches(scene, d, 0, v, {.hidden = &hidden});
  CHECK(all.size() > 1);
  CHECK(none.empty());
}
