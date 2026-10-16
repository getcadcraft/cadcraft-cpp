// SPDX-License-Identifier: Apache-2.0
// The WebGPU device: chosen before any window exists (AGENTS.md section 10, GPU start-up fallback), and an offscreen
// target for headless pictures and benchmarks (the same pipelines as the window: what the tests see is what users see).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "cc/base/error.hpp"
#include "cc/render/scene.hpp"
#include "cc/render/view.hpp"

namespace cc::ui {

class SceneRenderer;

struct Gpu {
  wgpu::Instance instance;
  wgpu::Surface surface;  // the window's, when there is one
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  std::string adapter_name;
  std::string backend;
  std::uint32_t max_texture = 8192;
};

struct GpuOptions {
  /// "vulkan", "d3d11", "metal", "null" (no output: tests of the pipelines), or empty for the platform's best;
  /// APP_GPU_BACKEND overrides.
  std::string backend;
  /// Makes the window's surface from the instance (before the adapter is chosen, so it can present to it).
  std::function<wgpu::Surface(const wgpu::Instance&)> make_surface;
};

/// Instance, adapter and device, synchronously (native builds).
[[nodiscard]] base::Result<Gpu> create_gpu(GpuOptions options);

/// The same in the browser, where the adapter and the device arrive later: `done` runs once, from the event loop.
void create_gpu_async(const GpuOptions& options, std::function<void(base::Result<Gpu>)> done);

/// A frame drawn without a window: RGBA rows, top to bottom.
struct Picture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
  double gpu_ms = 0;  // encode + submit + wait for the GPU (the frame time a window would see, minus presenting)
};

/// Draws `draws` at width × height device px on `background`, through `renderer`, and reads the pixels back.
[[nodiscard]] base::Result<Picture> render_offscreen(const Gpu& gpu, SceneRenderer& renderer,
                                                     std::span<const render::DrawBatch> draws, std::uint32_t width,
                                                     std::uint32_t height, render::Rgba background);

/// The colour format the offscreen target and the renderer use.
inline constexpr wgpu::TextureFormat kOffscreenFormat = wgpu::TextureFormat::RGBA8Unorm;

}  // namespace cc::ui
