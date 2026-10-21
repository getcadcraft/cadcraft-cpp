// SPDX-License-Identifier: Apache-2.0
// cadcraft — the desktop app (a test version of CadCraft in C++): the 2D viewer in a window.
//   cadcraft [file.dwg|dxf] [--lang en|es-MX]
// APP_GPU_BACKEND=vulkan|d3d11|metal picks the WebGPU backend; APP_ASSETS_DIR the fonts and words.
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "cc/base/file.hpp"
#include "cc/ui/gpu.hpp"
#include "cc/ui/host.hpp"
#include "cc/ui/viewer.hpp"

namespace {

std::string assets_dir(const char* argv0) {
  if (const auto env = cc::base::env_var("APP_ASSETS_DIR"); env && !env->empty()) {
    return *env;
  }
  std::error_code ec;
  const auto beside = std::filesystem::canonical(argv0, ec).parent_path() / "assets";
  if (!ec && std::filesystem::exists(beside / "lang/en.json", ec)) {
    return beside.string();
  }
  return CC_ASSETS_DIR;  // the source tree (a build that is not installed)
}

std::string locale_from_env() {
  const auto lang = cc::base::env_var("LANG");
  return lang && lang->starts_with("es") ? "es-MX" : "en";
}

void SDLCALL save_picked(void* userdata, const char* const* files, int /*filter*/) {
  if (userdata == nullptr) {
    return;
  }
  // a cancelled dialog still answers: the save waiting on it then says "Not saved."
  const std::string path = files != nullptr && *files != nullptr ? std::string(*files) : std::string();
  static_cast<cc::ui::Inbox*>(userdata)->post(
      {.name = {}, .path = path, .bytes = {}, .save_target = true, .pictures = false, .files = {}});
}

void SDLCALL picked(void* userdata, const char* const* files, int /*filter*/) {
  if (files == nullptr || *files == nullptr || userdata == nullptr) {
    return;  // cancelled, or the dialog failed
  }
  const std::string path = *files;
  static_cast<cc::ui::Inbox*>(userdata)->post({.name = std::filesystem::path(path).filename().string(),
                                               .path = path,
                                               .bytes = {},
                                               .save_target = false,
                                               .pictures = false,
                                               .files = {}});
}

void SDLCALL pictures_picked(void* userdata, const char* const* files, int /*filter*/) {
  if (userdata == nullptr) {
    return;
  }
  // a cancelled (or failed) dialog still answers: the command waiting on it then goes on with nothing
  cc::ui::Inbox::Item item{.name = {}, .path = {}, .bytes = {}, .save_target = false, .pictures = true, .files = {}};
  for (const std::span<const char* const> list(files, files != nullptr ? 1024 : 0); const char* f : list) {
    if (f == nullptr) {
      break;
    }
    const std::string path = f;
    item.files.push_back({.name = std::filesystem::path(path).filename().string(),
                          .path = path,
                          .bytes = {},
                          .save_target = false,
                          .pictures = false,
                          .files = {}});
  }
  static_cast<cc::ui::Inbox*>(userdata)->post(std::move(item));
}

int run(std::span<char*> argv) {
  std::string open;
  std::string locale = locale_from_env();
  for (std::size_t i = 1; i < argv.size(); ++i) {
    const std::string_view a = argv[i];
    if (a == "--lang" && i + 1 < argv.size()) {
      locale = argv[++i];
    } else if (!a.starts_with("-")) {
      open = std::string(a);
    }
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::println(stderr, "cadcraft: SDL: {}", SDL_GetError());
    return 1;
  }
  SDL_Window* window =
      SDL_CreateWindow("CadCraft (C++ test version)", 1400, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window == nullptr) {
    std::println(stderr, "cadcraft: SDL window: {}", SDL_GetError());
    return 1;
  }
  auto gpu = cc::ui::create_gpu(
      {.backend = {}, .make_surface = [&](const wgpu::Instance& i) { return cc::ui::make_window_surface(i, window); }});
  if (!gpu) {
    std::println(stderr, "cadcraft: {}", gpu.error().message);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  int rc = 0;
  {
    cc::ui::Inbox* inbox = nullptr;
    static constexpr std::array<SDL_DialogFileFilter, 1> kFilters{
        {{"DWG, DXF, SolidWorks sketch export", "dwg;dxf;json"}}};
    cc::ui::Services services;
    services.pick_open = [&] { SDL_ShowOpenFileDialog(picked, inbox, window, kFilters.data(), 1, nullptr, false); };
    static constexpr std::array<SDL_DialogFileFilter, 1> kDwg{{{"DWG", "dwg"}}};
    static constexpr std::array<SDL_DialogFileFilter, 1> kDxf{{{"DXF", "dxf"}}};
    static constexpr std::array<SDL_DialogFileFilter, 1> kPdf{{{"PDF", "pdf"}}};
    static constexpr std::array<SDL_DialogFileFilter, 1> kSwb{{{"SolidWorks macro", "swb"}}};
    static constexpr std::array<SDL_DialogFileFilter, 1> kPictures{{{"PNG, JPEG", "png;jpg;jpeg"}}};
    static constexpr std::array<SDL_DialogFileFilter, 1> kClouds{
        {{"Point clouds (LAS, LAZ, XYZ, PTS)", "las;laz;xyz;pts"}}};
    services.pick_pictures = [&](bool multiple) {
      SDL_ShowOpenFileDialog(pictures_picked, inbox, window, kPictures.data(), 1, nullptr, multiple);
    };
    services.pick_cloud = [&] {  // the waiting command takes it as it takes pictures
      SDL_ShowOpenFileDialog(pictures_picked, inbox, window, kClouds.data(), 1, nullptr, false);
    };
    services.pick_save = [&](const std::string& name, const std::string& ext) {
      const SDL_DialogFileFilter* f = kDxf.data();
      if (ext == "dwg") {
        f = kDwg.data();
      } else if (ext == "pdf") {
        f = kPdf.data();
      } else if (ext == "swb") {
        f = kSwb.data();
      }
      SDL_ShowSaveFileDialog(save_picked, inbox, window, f, 1, name.c_str());
    };
    cc::ui::Host host(window, *gpu, services, assets_dir(argv[0]), locale);
    inbox = &host.viewer().inbox();
    if (!open.empty()) {
      inbox->post({.name = std::filesystem::path(open).filename().string(),
                   .path = open,
                   .bytes = {},
                   .save_target = false,
                   .pictures = false,
                   .files = {}});
    }
    bool running = true;
    while (running) {
      SDL_Event e;
      // render on demand: sleep until something happens (or half a second, for the status bar)
      if (!host.viewer().busy() && SDL_WaitEventTimeout(&e, 500)) {
        running = host.event(e);
      }
      while (running && SDL_PollEvent(&e)) {
        running = host.event(e);
      }
      if (running) {
        host.frame();
      }
    }
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    (void)std::fputs("cadcraft: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputc('\n', stderr);
    return 1;
  } catch (...) {
    (void)std::fputs("cadcraft: unexpected error\n", stderr);
    return 1;
  }
}
