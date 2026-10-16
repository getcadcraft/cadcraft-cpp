// SPDX-License-Identifier: Apache-2.0
// cadcraft-shot — a drawing to a PNG without a window, through the app's own pipelines (AGENTS.md section 9: UI tests,
// headless export), and the frame-time benchmark.
//
//   cadcraft-shot <file.dwg|dxf> --out shot.png [--space N] [--size 1600x1000] [--dpr 1] [--theme model|light|print]
//                 [--zoom F] [--center X,Y] [--bench N]
//
// --bench N: N frames after the first; prints {"open_ms","first_frame_ms","frame_ms":[…],…} as JSON. The GPU is the
// adapter Dawn picks (APP_GPU_BACKEND=vulkan|null to choose); on a server that is a CPU rasteriser, so frame times
// there say nothing about a real GPU (the web build's ?bench measures in the browser).
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/value.hpp"
#include "cc/engine/open.hpp"
#include "cc/image/png.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"
#include "cc/text/font.hpp"
#include "cc/ui/gpu.hpp"
#include "cc/ui/scene_renderer.hpp"

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

bool number(std::string_view s, double& out) {
  const std::span<const char> txt(s);
  const auto [p, ec] = std::from_chars(txt.data(), std::to_address(txt.end()), out);
  return ec == std::errc{} && p == std::to_address(txt.end()) && std::isfinite(out);
}

std::filesystem::path font_path() {
  if (const auto env = cc::base::env_var("APP_ASSETS_DIR"); env && !env->empty()) {
    return std::filesystem::path(*env) / "fonts/archivo/Archivo-VF.ttf";
  }
  return std::filesystem::path(CC_ASSETS_DIR) / "fonts/archivo/Archivo-VF.ttf";
}

int run(const std::vector<std::string_view>& args) {
  std::string file;
  std::string out;
  std::size_t space = 0;
  double w = 1600;
  double h = 1000;
  double dpr = 1;
  double zoom = 1;
  int bench = 0;
  double center_x = std::numeric_limits<double>::quiet_NaN();
  double center_y = std::numeric_limits<double>::quiet_NaN();
  std::string theme = "model";
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const bool has = i + 1 < args.size();
    double v = 0;
    if (a == "--out" && has) {
      out = std::string(args[++i]);
    } else if (a == "--space" && has && number(args[++i], v) && v >= 0) {
      space = static_cast<std::size_t>(v);
    } else if (a == "--size" && has) {
      const std::string_view s = args[++i];
      const auto x = s.find('x');
      if (x == std::string_view::npos || !number(s.substr(0, x), w) || !number(s.substr(x + 1), h) || w < 1 || h < 1 ||
          w > 16384 || h > 16384) {
        std::println(stderr, "cadcraft-shot: --size WxH");
        return 2;
      }
    } else if (a == "--dpr" && has && number(args[++i], v) && v > 0 && v <= 4) {
      dpr = v;
    } else if (a == "--zoom" && has && number(args[++i], v) && v > 0) {
      zoom = v;
    } else if (a == "--center" && has) {
      const std::string_view c = args[++i];
      const auto comma = c.find(',');
      if (comma == std::string_view::npos || !number(c.substr(0, comma), center_x) ||
          !number(c.substr(comma + 1), center_y)) {
        std::println(stderr, "cadcraft-shot: --center X,Y (drawing units)");
        return 2;
      }
    } else if (a == "--bench" && has && number(args[++i], v) && v >= 1 && v <= 10000) {
      bench = static_cast<int>(v);
    } else if (a == "--theme" && has) {
      theme = std::string(args[++i]);
    } else if (!a.starts_with("--") && file.empty()) {
      file = std::string(a);
    } else {
      std::println(stderr,
                   "usage: cadcraft-shot <file> --out <png> [--space N] [--size WxH] [--dpr D] [--theme "
                   "model|light|print] [--zoom F] [--bench N]");
      return 2;
    }
  }
  if (file.empty() || (out.empty() && bench == 0)) {
    std::println(stderr, "usage: cadcraft-shot <file> --out <png> [--bench N]");
    return 2;
  }
  const auto t_open = Clock::now();
  auto opened = cc::engine::open_file(file);
  if (!opened) {
    std::println(stderr, "cadcraft-shot: {}", opened.error().message);
    return 1;
  }
  const double open_ms = ms_since(t_open);
  const cc::drawing::Drawing& d = (*opened)->drawing;
  if (space >= d.spaces.size()) {
    std::println(stderr, "cadcraft-shot: the drawing has {} spaces", d.spaces.size());
    return 1;
  }
  auto gpu = cc::ui::create_gpu({});
  if (!gpu) {
    std::println(stderr, "cadcraft-shot: {}", gpu.error().message);
    return 1;
  }
  const auto t_first = Clock::now();
  auto font_bytes = cc::base::read_file(font_path(), std::uint64_t{1} << 24U);
  if (!font_bytes) {
    std::println(stderr, "cadcraft-shot: the font: {}", font_bytes.error().message);
    return 1;
  }
  auto font =
      cc::text::Font::load(std::move(*font_bytes), {.width = 75, .texture_limit = static_cast<int>(gpu->max_texture)});
  if (!font) {
    std::println(stderr, "cadcraft-shot: {}", font.error().message);
    return 1;
  }
  const auto t_scene = Clock::now();
  const cc::render::Scene scene = cc::render::build_scene(d, **font);
  const double scene_ms = ms_since(t_scene);
  cc::ui::SceneRenderer renderer(gpu->device, cc::ui::kOffscreenFormat);
  renderer.upload(scene);
  renderer.upload_atlas((*font)->atlas());
  cc::render::View v{.width = w, .height = h, .dpr = dpr};
  v = cc::render::fit(d.spaces[space], v);
  v.zoom *= zoom;
  if (std::isfinite(center_x)) {
    v.cx = center_x;
    v.cy = center_y;
  }
  cc::render::FrameOptions fo;
  const bool print = theme == "print";
  if (print) {
    fo.theme = cc::render::Theme::print(d.spaces[space].kind == "paper");
  } else if (theme == "light") {
    fo.theme = cc::render::Theme::light();
  }
  fo.print = print;
  std::vector<bool> hidden(d.layers.size());
  for (std::size_t i = 0; i < d.layers.size(); ++i) {
    hidden[i] = !d.layers[i].on;
  }
  fo.hidden = &hidden;
  const auto pw = static_cast<std::uint32_t>(std::lround(w * dpr));
  const auto ph = static_cast<std::uint32_t>(std::lround(h * dpr));
  const auto draws = cc::render::frame_batches(scene, d, space, v, fo);
  auto pic = cc::ui::render_offscreen(*gpu, renderer, draws, pw, ph, fo.theme.sheet);
  if (!pic) {
    std::println(stderr, "cadcraft-shot: {}", pic.error().message);
    return 1;
  }
  const double first_ms = ms_since(t_first);
  if (!out.empty()) {
    auto png = cc::image::encode_png(pic->rgba, pic->width, pic->height);
    if (!png) {
      std::println(stderr, "cadcraft-shot: {}", png.error().message);
      return 1;
    }
    if (auto w_ok = cc::base::write_file_atomic(out, *png); !w_ok) {
      std::println(stderr, "cadcraft-shot: {}", w_ok.error().message);
      return 1;
    }
  }
  if (bench > 0) {
    cc::base::Array frames;
    cc::base::Array encode;
    for (int i = 0; i < bench; ++i) {
      // each frame zooms a little about the centre, as a user would: the draw list is rebuilt every frame
      const auto t = Clock::now();
      v = cc::render::zoom_at(v, w / 2, h / 2, i % 2 == 0 ? 1.05 : 1 / 1.05);
      const auto fd = cc::render::frame_batches(scene, d, space, v, fo);
      const double cpu = ms_since(t);
      auto p = cc::ui::render_offscreen(*gpu, renderer, fd, pw, ph, fo.theme.sheet);
      if (!p) {
        std::println(stderr, "cadcraft-shot: {}", p.error().message);
        return 1;
      }
      frames.emplace_back(std::round((cpu + p->gpu_ms) * 1000) / 1000);
      encode.emplace_back(std::round(cpu * 1000) / 1000);
    }
    const auto& sp = scene.spaces[space];
    const cc::base::Value o = cc::base::make_object({
        {"file", std::filesystem::path(file).filename().string()},
        {"size", std::to_string(pw) + "x" + std::to_string(ph)},
        {"adapter", gpu->adapter_name + " (" + gpu->backend + ")"},
        {"open_ms", std::round(open_ms * 1000) / 1000},
        {"parse_ms", std::round((*opened)->times.parse_ms * 1000) / 1000},
        {"compile_ms", std::round((*opened)->times.compile_ms * 1000) / 1000},
        {"scene_ms", std::round(scene_ms * 1000) / 1000},
        {"first_frame_ms", std::round((open_ms + first_ms) * 1000) / 1000},
        {"frame_ms", std::move(frames)},
        {"frame_cpu_ms", std::move(encode)},
        {"segments", static_cast<double>(sp.segments)},
        {"triangles", static_cast<double>(sp.triangles)},
        {"glyphs", static_cast<double>(sp.glyphs)},
        {"draw_calls", static_cast<double>(renderer.last_draw_calls())},
    });
    std::println("{}", cc::base::to_json(o));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    std::vector<std::string_view> args(raw.begin(), raw.end());
    return run(args);
  } catch (const std::exception& e) {
    (void)std::fputs("cadcraft-shot: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputc('\n', stderr);
    return 1;
  } catch (...) {
    (void)std::fputs("cadcraft-shot: unexpected error\n", stderr);
    return 1;
  }
}
