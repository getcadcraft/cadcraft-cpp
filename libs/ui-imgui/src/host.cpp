// SPDX-License-Identifier: Apache-2.0
#include "cc/ui/host.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_wgpu.h>
#include <webgpu/webgpu_cpp.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif

#include "cc/base/file.hpp"

namespace cc::ui {

wgpu::Surface make_window_surface(const wgpu::Instance& instance, SDL_Window* window) {
#if defined(__EMSCRIPTEN__)
  (void)window;
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
  canvas.selector = "#canvas";
  wgpu::SurfaceDescriptor sd;
  sd.nextInChain = &canvas;
  return instance.CreateSurface(&sd);
#else
  // SDL3 has no WebGPU surface of its own: the native handles, through ImGui's helper (the one platform-specific spot;
  // the handles are opaque pointers / numbers SDL gives out, passed on unchanged)
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  ImGui_ImplWGPU_CreateSurfaceInfo info{};
  info.Instance = instance.Get();
#if defined(SDL_PLATFORM_MACOS)
  info.System = "cocoa";
  info.RawWindow = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
  if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
    info.System = "wayland";
    info.RawDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    info.RawSurface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  } else {
    info.System = "x11";
    // an X11 Window is a number; the helper takes it in a pointer-sized slot
    info.RawWindow =
        reinterpret_cast<void*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
            static_cast<std::uintptr_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
    info.RawDisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  }
#elif defined(SDL_PLATFORM_WIN32)
  info.System = "win32";
  info.RawWindow = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  info.RawInstance = ::GetModuleHandleW(nullptr);
#endif
  WGPUSurface s = ImGui_ImplWGPU_CreateWGPUSurfaceHelper(&info);
  return s != nullptr ? wgpu::Surface::Acquire(s) : wgpu::Surface();
#endif
}

Host::Host(SDL_Window* window, Gpu& gpu, Services services, const std::string& assets, const std::string& locale)
    : window_(window), gpu_(&gpu) {
  wgpu::SurfaceCapabilities caps;
  gpu.surface.GetCapabilities(gpu.adapter, &caps);
  // a non-sRGB format: the canvas the 0.5 view draws on blends in the same (gamma) space
  const std::span<const wgpu::TextureFormat> formats(caps.formats, caps.formatCount);
  format_ = formats.empty() ? wgpu::TextureFormat::BGRA8Unorm : formats.front();
  for (const wgpu::TextureFormat f : formats) {
    if (f == wgpu::TextureFormat::BGRA8Unorm || f == wgpu::TextureFormat::RGBA8Unorm) {
      format_ = f;
      break;
    }
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // layouts are ours (AGENTS.md section 7), not imgui.ini
  auto font = base::read_file(assets + "/fonts/archivo/Archivo-VF.ttf", std::uint64_t{1} << 24U);
  if (font) {
    ui_font_ = std::move(*font);
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(ui_font_.data(), static_cast<int>(ui_font_.size()), 15.0F, &cfg);
  }
  ImGui_ImplSDL3_InitForOther(window);
  ImGui_ImplWGPU_InitInfo init;
  init.Device = gpu.device.Get();
  init.NumFramesInFlight = 3;
  init.RenderTargetFormat = static_cast<WGPUTextureFormat>(format_);
  init.DepthStencilFormat = static_cast<WGPUTextureFormat>(kDepthStencil);
  init.PipelineMultisampleState.count = kSamples;
  ImGui_ImplWGPU_Init(&init);
  viewer_ = std::make_unique<Viewer>(gpu, format_, std::move(services), assets, locale);
}

Host::~Host() {
  viewer_.reset();
  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

bool Host::event(const SDL_Event& e) {
  ImGui_ImplSDL3_ProcessEvent(&e);
  if (e.type == SDL_EVENT_QUIT ||
      (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window_))) {
    return false;
  }
  if (e.type == SDL_EVENT_DROP_FILE && e.drop.data != nullptr) {
    const std::string path = e.drop.data;
    viewer_->inbox().post(
        {.name = path.substr(path.find_last_of("/\\") + 1), .path = path, .bytes = {}, .save_target = false});
  }
  viewer_->event(e, ImGui::GetIO().WantCaptureMouse);
  return true;
}

void Host::configure(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
  wgpu::SurfaceConfiguration sc;
  sc.device = gpu_->device;
  sc.format = format_;
  sc.usage = wgpu::TextureUsage::RenderAttachment;
  sc.width = width;
  sc.height = height;
  sc.presentMode = wgpu::PresentMode::Fifo;
  sc.alphaMode = wgpu::CompositeAlphaMode::Auto;
  gpu_->surface.Configure(&sc);
  auto texture = [&](wgpu::TextureFormat f) {
    wgpu::TextureDescriptor td;
    td.size = {width, height, 1};
    td.format = f;
    td.sampleCount = kSamples;
    td.usage = wgpu::TextureUsage::RenderAttachment;
    return gpu_->device.CreateTexture(&td);
  };
  msaa_ = texture(format_);
  depth_ = texture(kDepthStencil);
}

void Host::frame() {
  int w = 0;
  int h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  if (w <= 0 || h <= 0) {
    return;
  }
  if (std::cmp_not_equal(w, width_) || std::cmp_not_equal(h, height_)) {
    configure(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  }
  wgpu::SurfaceTexture st;
  gpu_->surface.GetCurrentTexture(&st);
  if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    configure(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));  // lost or outdated: try again next frame
    return;
  }
  ImGui_ImplWGPU_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  viewer_->ui(SDL_GetWindowPixelDensity(window_));
  ImGui::Render();

  const render::Rgba bg = viewer_->background();
  wgpu::RenderPassColorAttachment ca;
  ca.view = msaa_.CreateView();
  ca.resolveTarget = st.texture.CreateView();
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Discard;
  ca.clearValue = {static_cast<double>(bg.r), static_cast<double>(bg.g), static_cast<double>(bg.b), 1.0};
  wgpu::RenderPassDepthStencilAttachment dsa;
  dsa.view = depth_.CreateView();
  dsa.depthLoadOp = wgpu::LoadOp::Clear;
  dsa.depthStoreOp = wgpu::StoreOp::Discard;
  dsa.depthClearValue = 1.0F;
  dsa.stencilLoadOp = wgpu::LoadOp::Clear;
  dsa.stencilStoreOp = wgpu::StoreOp::Discard;
  wgpu::RenderPassDescriptor rp;
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &ca;
  rp.depthStencilAttachment = &dsa;
  const wgpu::CommandEncoder enc = gpu_->device.CreateCommandEncoder();
  {
    const wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rp);
    viewer_->render(pass, width_, height_);
    pass.SetScissorRect(0, 0, width_, height_);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
    pass.End();
  }
  const wgpu::CommandBuffer cmd = enc.Finish();
  gpu_->queue.Submit(1, &cmd);
#if !defined(__EMSCRIPTEN__)
  gpu_->surface.Present();
  gpu_->instance.ProcessEvents();
#endif
}

}  // namespace cc::ui
