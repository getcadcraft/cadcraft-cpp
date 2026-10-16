// SPDX-License-Identifier: Apache-2.0
#include "cc/ui/gpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "cc/base/error.hpp"
#include "cc/base/file.hpp"
#include "cc/ui/scene_renderer.hpp"

namespace cc::ui {

namespace {

std::string text_of(wgpu::StringView s) {
  return s.data != nullptr ? std::string(std::string_view(
                                 s.data, s.length == WGPU_STRLEN ? std::string_view(s.data).size() : s.length))
                           : std::string();
}

wgpu::BackendType backend_of(std::string_view name) {
  if (name == "vulkan") {
    return wgpu::BackendType::Vulkan;
  }
  if (name == "d3d11") {
    return wgpu::BackendType::D3D11;
  }
  if (name == "metal") {
    return wgpu::BackendType::Metal;
  }
  if (name == "null") {
    return wgpu::BackendType::Null;
  }
  return wgpu::BackendType::Undefined;
}

std::string name_of(wgpu::BackendType b) {
  switch (b) {
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::D3D11: return "D3D11";
    case wgpu::BackendType::D3D12: return "D3D12";
    case wgpu::BackendType::Metal: return "Metal";
    case wgpu::BackendType::Null: return "null";
    case wgpu::BackendType::WebGPU: return "WebGPU";
    default: return "other";
  }
}

}  // namespace

base::Result<Gpu> create_gpu(GpuOptions options) {
  if (const auto env = base::env_var("APP_GPU_BACKEND"); env && !env->empty()) {
    options.backend = *env;
  }
  Gpu g;
  static constexpr wgpu::InstanceFeatureName kTimedWait = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor idesc;
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = &kTimedWait;
  g.instance = wgpu::CreateInstance(&idesc);
  if (!g.instance) {
    return base::fail(base::Errc::unsupported, "WebGPU is not available");
  }
  if (options.make_surface) {
    g.surface = options.make_surface(g.instance);
    if (!g.surface) {
      return base::fail(base::Errc::unsupported, "could not make a WebGPU surface for the window");
    }
  }
  wgpu::RequestAdapterOptions ao;
  ao.backendType = backend_of(options.backend);
  ao.powerPreference = wgpu::PowerPreference::HighPerformance;
  ao.compatibleSurface = g.surface;
  std::string why;
  wgpu::Future af = g.instance.RequestAdapter(
      &ao, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
        if (status == wgpu::RequestAdapterStatus::Success) {
          g.adapter = std::move(adapter);
        } else {
          why = text_of(message);
        }
      });
  if (g.instance.WaitAny(af, UINT64_MAX) != wgpu::WaitStatus::Success || !g.adapter) {
    return base::fail(base::Errc::unsupported, "no WebGPU adapter" + (why.empty() ? std::string() : ": " + why));
  }
  wgpu::AdapterInfo info;
  if (g.adapter.GetInfo(&info)) {
    g.adapter_name = text_of(info.device);
    g.backend = name_of(info.backendType);
  }
  wgpu::Limits limits;
  if (g.adapter.GetLimits(&limits)) {
    g.max_texture = std::min<std::uint32_t>(limits.maxTextureDimension2D, 16384);
  }
  wgpu::DeviceDescriptor dd;
  dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
    std::println(stderr, "cadcraft: GPU error: {}", text_of(message));
  });
  dd.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
                           [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                             if (reason != wgpu::DeviceLostReason::Destroyed) {
                               std::println(stderr, "cadcraft: GPU device lost: {}", text_of(message));
                             }
                           });
  wgpu::Future df =
      g.adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly,
                              [&](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                if (status == wgpu::RequestDeviceStatus::Success) {
                                  g.device = std::move(device);
                                } else {
                                  why = text_of(message);
                                }
                              });
  if (g.instance.WaitAny(df, UINT64_MAX) != wgpu::WaitStatus::Success || !g.device) {
    return base::fail(base::Errc::unsupported, "no WebGPU device" + (why.empty() ? std::string() : ": " + why));
  }
  g.queue = g.device.GetQueue();
  return g;
}

void create_gpu_async(const GpuOptions& options, std::function<void(base::Result<Gpu>)> done) {
  struct Pending {
    Gpu g;
    std::function<void(base::Result<Gpu>)> done;
  };
  auto p = std::make_shared<Pending>();
  p->done = std::move(done);
  p->g.instance = wgpu::CreateInstance(nullptr);
  if (!p->g.instance) {
    p->done(base::fail(base::Errc::unsupported, "WebGPU is not available"));
    return;
  }
  if (options.make_surface) {
    p->g.surface = options.make_surface(p->g.instance);
  }
  wgpu::RequestAdapterOptions ao;
  ao.powerPreference = wgpu::PowerPreference::HighPerformance;
  ao.compatibleSurface = p->g.surface;
  p->g.instance.RequestAdapter(
      &ao, wgpu::CallbackMode::AllowSpontaneous,
      [p](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
        if (status != wgpu::RequestAdapterStatus::Success) {
          p->done(base::fail(base::Errc::unsupported, "no WebGPU adapter: " + text_of(message)));
          return;
        }
        p->g.adapter = std::move(adapter);
        wgpu::AdapterInfo info;
        if (p->g.adapter.GetInfo(&info)) {
          p->g.adapter_name = text_of(info.device);
          p->g.backend = name_of(info.backendType);
        }
        wgpu::Limits limits;
        if (p->g.adapter.GetLimits(&limits)) {
          p->g.max_texture = std::min<std::uint32_t>(limits.maxTextureDimension2D, 16384);
        }
        wgpu::DeviceDescriptor dd;
        dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m) {
          std::println(stderr, "cadcraft: GPU error: {}", text_of(m));
        });
        p->g.adapter.RequestDevice(&dd, wgpu::CallbackMode::AllowSpontaneous,
                                   [p](wgpu::RequestDeviceStatus st, wgpu::Device device, wgpu::StringView m) {
                                     if (st != wgpu::RequestDeviceStatus::Success) {
                                       p->done(base::fail(base::Errc::unsupported, "no WebGPU device: " + text_of(m)));
                                       return;
                                     }
                                     p->g.device = std::move(device);
                                     p->g.queue = p->g.device.GetQueue();
                                     p->done(std::move(p->g));
                                   });
      });
}

base::Result<Picture> render_offscreen(const Gpu& gpu, SceneRenderer& renderer,
                                       std::span<const render::DrawBatch> draws, std::uint32_t width,
                                       std::uint32_t height, render::Rgba background) {
  if (width == 0 || height == 0 || width > gpu.max_texture || height > gpu.max_texture) {
    return base::fail(base::Errc::too_big, "picture size outside the GPU's limits");
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto texture = [&](wgpu::TextureFormat f, std::uint32_t samples, wgpu::TextureUsage usage) {
    wgpu::TextureDescriptor td;
    td.size = {width, height, 1};
    td.format = f;
    td.sampleCount = samples;
    td.usage = usage;
    return gpu.device.CreateTexture(&td);
  };
  const wgpu::Texture msaa = texture(kOffscreenFormat, kSamples, wgpu::TextureUsage::RenderAttachment);
  const wgpu::Texture resolved =
      texture(kOffscreenFormat, 1, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);
  const wgpu::Texture depth = texture(kDepthStencil, kSamples, wgpu::TextureUsage::RenderAttachment);

  wgpu::RenderPassColorAttachment ca;
  ca.view = msaa.CreateView();
  ca.resolveTarget = resolved.CreateView();
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Discard;
  ca.clearValue = {static_cast<double>(background.r), static_cast<double>(background.g),
                   static_cast<double>(background.b), 1.0};
  wgpu::RenderPassDepthStencilAttachment dsa;
  dsa.view = depth.CreateView();
  dsa.depthLoadOp = wgpu::LoadOp::Clear;
  dsa.depthStoreOp = wgpu::StoreOp::Discard;
  dsa.depthClearValue = 1.0F;
  dsa.stencilLoadOp = wgpu::LoadOp::Clear;
  dsa.stencilStoreOp = wgpu::StoreOp::Discard;
  dsa.stencilClearValue = 0;
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &ca;
  rp.depthStencilAttachment = &dsa;

  const std::uint32_t row = (width * 4 + 255) & ~255U;  // bytesPerRow must be a multiple of 256
  wgpu::BufferDescriptor bd;
  bd.size = static_cast<std::uint64_t>(row) * height;
  bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  const wgpu::Buffer readback = gpu.device.CreateBuffer(&bd);

  const wgpu::CommandEncoder enc = gpu.device.CreateCommandEncoder();
  {
    const wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    renderer.draw(pass, draws, width, height);
    pass.End();
  }
  wgpu::TexelCopyTextureInfo src;
  src.texture = resolved;
  wgpu::TexelCopyBufferInfo dst;
  dst.buffer = readback;
  dst.layout.bytesPerRow = row;
  dst.layout.rowsPerImage = height;
  const wgpu::Extent3D size{width, height, 1};
  enc.CopyTextureToBuffer(&src, &dst, &size);
  const wgpu::CommandBuffer cmd = enc.Finish();
  gpu.queue.Submit(1, &cmd);

  const auto bytes = static_cast<std::size_t>(bd.size);  // fits: the size is checked against the device's limits
  bool mapped = false;
  wgpu::Future mf = readback.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) { mapped = status == wgpu::MapAsyncStatus::Success; });
  if (gpu.instance.WaitAny(mf, UINT64_MAX) != wgpu::WaitStatus::Success || !mapped) {
    return base::fail(base::Errc::io, "could not read the picture back from the GPU");
  }
  Picture pic;
  pic.width = width;
  pic.height = height;
  pic.gpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const auto* base = static_cast<const std::uint8_t*>(readback.GetConstMappedRange(0, bytes));
  if (base == nullptr) {
    return base::fail(base::Errc::io, "the GPU gave no pixels");
  }
  const std::span<const std::uint8_t> all(base, bytes);
  pic.rgba.reserve(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto line = all.subspan(static_cast<std::size_t>(y) * row, static_cast<std::size_t>(width) * 4);
    pic.rgba.insert(pic.rgba.end(), line.begin(), line.end());
  }
  readback.Unmap();
  return pic;
}

}  // namespace cc::ui
