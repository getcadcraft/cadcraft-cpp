// SPDX-License-Identifier: Apache-2.0
// What a window needs around the viewer, shared by the desktop app and the web build: Dear ImGui (SDL3 + WebGPU
// backends, the Archivo UI font), the surface, the 4× multisampled targets sized to the window, and one frame.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <webgpu/webgpu_cpp.h>

#include "cc/ui/gpu.hpp"
#include "cc/ui/viewer.hpp"

namespace cc::ui {

/// The window's WebGPU surface (desktop: from SDL's native handles; web: the page's canvas).
[[nodiscard]] wgpu::Surface make_window_surface(const wgpu::Instance& instance, SDL_Window* window);

class Host {
 public:
  Host(SDL_Window* window, Gpu& gpu, Services services, const std::string& assets, const std::string& locale);
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  Host(Host&&) = delete;
  Host& operator=(Host&&) = delete;

  /// One SDL event for ImGui and the viewer. False when the app should quit.
  bool event(const SDL_Event& e);
  /// Draws and presents a frame (ImGui widgets, the drawing, ImGui on top).
  void frame();
  [[nodiscard]] Viewer& viewer() noexcept { return *viewer_; }

 private:
  void configure(std::uint32_t width, std::uint32_t height);

  SDL_Window* window_;
  Gpu* gpu_;
  wgpu::TextureFormat format_ = wgpu::TextureFormat::Undefined;
  std::uint32_t width_ = 0, height_ = 0;
  wgpu::Texture msaa_, depth_;
  std::vector<std::byte> ui_font_;  // ImGui reads the font from these bytes for as long as it runs
  std::unique_ptr<Viewer> viewer_;
};

}  // namespace cc::ui
