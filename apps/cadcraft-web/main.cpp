// SPDX-License-Identifier: Apache-2.0
// cadcraft-web — the viewer in the browser (Emscripten, the browser's WebGPU through emdawnwebgpu): a test version of
// CadCraft in C++. The page (index.html) has no script of its own beyond Emscripten's generated file (CSP: script-src
// 'self' 'wasm-unsafe-eval'); the little JavaScript a browser needs (file picker, drops, ?sample, ?bench, the
// "needs WebGPU" screen) is EM_JS glue here, compiled into that file.
//   ?sample        open the bundled sample drawing
//   ?bench         …and time 240 frames while zooming; the numbers appear on the page and in the console
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include "cc/ui/gpu.hpp"
#include "cc/ui/host.hpp"
#include "cc/ui/viewer.hpp"

// clang-format off
EM_JS(int, cc_web_has_webgpu, (), { return navigator.gpu ? 1 : 0; });
EM_JS(void, cc_web_show, (const char* id), {
  const e = document.getElementById(UTF8ToString(id));
  if (e) e.hidden = false;
});
EM_JS(int, cc_web_query, (const char* name), { return new URLSearchParams(location.search).has(UTF8ToString(name)) ? 1 : 0; });
EM_JS(void, cc_web_install, (), {
  const send = async (file) => {
    const b = new Uint8Array(await file.arrayBuffer());
    const p = _malloc(b.length), n = stringToNewUTF8(file.name);
    HEAPU8.set(b, p);
    _cc_web_open(p, b.length, n);
    _free(p); _free(n);
  };
  const input = document.createElement("input");
  input.type = "file"; input.accept = ".dwg,.dxf"; input.hidden = true; input.id = "cc-file";
  input.addEventListener("change", () => { if (input.files[0]) send(input.files[0]); input.value = ""; });
  document.body.appendChild(input);
  const c = document.getElementById("canvas");
  c.addEventListener("dragover", (e) => e.preventDefault());
  c.addEventListener("drop", (e) => { e.preventDefault(); const f = e.dataTransfer.files[0]; if (f) send(f); });
});
EM_JS(void, cc_web_pick, (), { document.getElementById("cc-file").click(); });
EM_JS(void, cc_web_fetch_sample, (const char* url), {
  const u = UTF8ToString(url);
  fetch(u).then((r) => r.arrayBuffer()).then((a) => {
    const b = new Uint8Array(a), p = _malloc(b.length), n = stringToNewUTF8(u.split("/").pop());
    HEAPU8.set(b, p);
    _cc_web_open(p, b.length, n);
    _free(p); _free(n);
  });
});
EM_JS(double, cc_web_now, (), { return performance.now(); });
EM_JS(void, cc_web_report, (const char* text), {
  const t = UTF8ToString(text);
  console.log("cadcraft bench " + t);
  const e = document.getElementById("bench");
  if (e) { e.textContent = t; e.hidden = false; }
});
// clang-format on

namespace {

struct App {
  SDL_Window* window = nullptr;
  std::unique_ptr<cc::ui::Gpu> gpu;
  std::unique_ptr<cc::ui::Host> host;
  bool bench = false;
  int bench_frame = -1;
  double last = 0;
  std::vector<double> frame_ms;   // between frames (what the user sees)
  std::vector<double> cpu_ms;     // inside a frame: input, draw list, encode, submit
  std::vector<double> open_runs;  // open → first frame, ms
};
App* g_app = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — the browser's loop has no context

std::string median(std::vector<double> v) {
  if (v.empty()) {
    return "0";
  }
  std::ranges::sort(v);
  return std::to_string(v[v.size() / 2]);
}

std::string join(const std::vector<double>& v) {
  std::string s;
  for (std::size_t i = 0; i < v.size(); ++i) {
    s += (i != 0 ? "," : "") + std::to_string(v[i]);
  }
  return s;
}

void loop() {
  App& a = *g_app;
  if (!a.host) {
    return;  // the device is still on its way
  }
  const double t0 = cc_web_now();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    (void)a.host->event(e);
  }
  if (a.bench && a.bench_frame >= 0 && a.bench_frame < 240) {  // zoom in and out about the centre, a frame each
    SDL_Event w{};
    w.type = SDL_EVENT_MOUSE_WHEEL;
    float ww = 0;
    float wh = 0;
    int iw = 0;
    int ih = 0;
    SDL_GetWindowSize(a.window, &iw, &ih);
    ww = static_cast<float>(iw);
    wh = static_cast<float>(ih);
    w.wheel.mouse_x = ww / 2 + 120;
    w.wheel.mouse_y = wh / 2;
    w.wheel.y = (a.bench_frame / 30) % 2 == 0 ? 1.0F : -1.0F;
    (void)a.host->event(w);
  }
  a.host->frame();
  const double t1 = cc_web_now();
  if (a.bench && a.bench_frame >= 0) {
    if (a.bench_frame > 0) {
      a.frame_ms.push_back(t0 - a.last);
      a.cpu_ms.push_back(t1 - t0);
    }
    a.last = t0;
    if (++a.bench_frame == 241) {
      const auto s = a.host->viewer().stats();
      const std::string r = "{\"frames\":240,\"frame_ms_median\":" + median(a.frame_ms) +
                            ",\"cpu_ms_median\":" + median(a.cpu_ms) + ",\"open_to_first_frame_ms\":[" +
                            join(a.open_runs) + "]" + ",\"draws\":" + std::to_string(s.draws) + ",\"adapter\":\"" +
                            a.gpu->adapter_name + "\"}";
      cc_web_report(r.c_str());
    }
  }
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void cc_web_open(const std::uint8_t* data, int size, const char* name) {
  if (g_app == nullptr || !g_app->host || data == nullptr || size <= 0) {
    return;
  }
  const std::span<const std::uint8_t> in(data, static_cast<std::size_t>(size));
  std::vector<std::byte> bytes(in.size());
  std::ranges::transform(in, bytes.begin(), [](std::uint8_t b) { return static_cast<std::byte>(b); });
  const std::string file = name != nullptr ? name : "drawing";
  if (!g_app->bench) {
    g_app->host->viewer().inbox().post({.name = file, .path = {}, .bytes = std::move(bytes)});
    return;
  }
  // open → first frame, six times (the first is cold): the same measurement the JS bench page makes
  for (int run = 0; run < 6; ++run) {
    const double t0 = cc_web_now();
    g_app->host->viewer().inbox().post({.name = file, .path = {}, .bytes = bytes});
    g_app->host->frame();
    g_app->open_runs.push_back(cc_web_now() - t0);
  }
  g_app->bench_frame = 0;
}

int main(int /*argc*/, char** /*argv*/) {
  static App app;
  g_app = &app;
  if (cc_web_has_webgpu() == 0) {
    cc_web_show("no-webgpu");
    return 0;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "cadcraft: SDL: %s\n", SDL_GetError());  // NOLINT(cppcoreguidelines-pro-type-vararg)
    return 1;
  }
  app.window =
      SDL_CreateWindow("CadCraft (C++ test version)", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  app.bench = cc_web_query("bench") != 0;
  const bool sample = app.bench || cc_web_query("sample") != 0;
  cc_web_install();
  cc::ui::create_gpu_async(
      {.backend = {},
       .make_surface = [](const wgpu::Instance& i) { return cc::ui::make_window_surface(i, g_app->window); }},
      [sample](cc::base::Result<cc::ui::Gpu> gpu) {
        if (!gpu) {
          std::fprintf(stderr, "cadcraft: %s\n",
                       gpu.error().message.c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg)
          cc_web_show("no-webgpu");
          return;
        }
        g_app->gpu = std::make_unique<cc::ui::Gpu>(std::move(*gpu));
        cc::ui::Services services;
        services.pick_open = [] { cc_web_pick(); };
        g_app->host = std::make_unique<cc::ui::Host>(g_app->window, *g_app->gpu, services, "/assets", "en");
        if (sample) {
          cc_web_fetch_sample("samples/kitchen-equipment.dxf");
        }
      });
  emscripten_set_main_loop(loop, 0, false);
  return 0;
}
