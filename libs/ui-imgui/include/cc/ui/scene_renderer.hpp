// SPDX-License-Identifier: Apache-2.0
// cc::ui — L6. Draws render::Scene through WebGPU (Dawn natively, the browser's WebGPU on the web): lines as instanced
// segment quads, fills through an even-odd stencil (every ring fanned, then the fill's box covered where the stencil is
// odd), text as instanced quads over the signed-distance-field atlas. 4× multisampled. One render pass; the caller
// adds ImGui after the scene in the same pass.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include "cc/render/scene.hpp"
#include "cc/text/font.hpp"

namespace cc::ui {

inline constexpr wgpu::TextureFormat kDepthStencil = wgpu::TextureFormat::Depth24PlusStencil8;
inline constexpr std::uint32_t kSamples = 4;

class SceneRenderer {
 public:
  SceneRenderer(wgpu::Device device, wgpu::TextureFormat color);

  /// Uploads a scene's geometry (replacing the last one). The scene must outlive its use in draw().
  void upload(const render::Scene& scene);
  /// Uploads the glyph atlas when its generation changed.
  void upload_atlas(const text::Atlas& atlas);
  /// Records the draws into `pass` (a pass on a `kSamples`-sampled target of `width` × `height` device px, with a
  /// `kDepthStencil` attachment), clipped to `canvas` (device px: x, y, w, h; w = 0 → the whole target).
  void draw(const wgpu::RenderPassEncoder& pass, std::span<const render::DrawBatch> draws, std::uint32_t width,
            std::uint32_t height, std::array<float, 4> canvas = {});

  /// The draws of the last frame, for the status bar and the benchmark.
  [[nodiscard]] std::size_t last_draw_calls() const noexcept { return draw_calls_; }

 private:
  struct Ranges {
    std::uint32_t seg_first = 0, seg_count = 0;      // segment instances
    std::uint32_t glyph_first = 0, glyph_count = 0;  // glyph instances
    std::uint32_t fill_first = 0, fill_count = 0;    // into fills_
  };
  struct FillRange {
    std::uint32_t fan_first = 0, fan_count = 0;  // vertices in the fan buffer
    std::uint32_t cover_first = 0;               // 6 vertices in the cover buffer
  };

  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::RenderPipeline segments_, stencil_, cover_, text_;
  wgpu::BindGroupLayout layout_;
  wgpu::Buffer uniforms_;
  std::size_t uniform_slots_ = 0;
  wgpu::Buffer seg_buf_, fan_buf_, cover_buf_, glyph_buf_;
  wgpu::Texture atlas_tex_;
  wgpu::Sampler sampler_;
  wgpu::BindGroup bind_;
  std::uint32_t atlas_generation_ = 0xFFFFFFFFU;
  int atlas_w_ = 0, atlas_h_ = 0;
  float spread_em_ = 0;
  std::unordered_map<const render::SceneBatch*, Ranges> ranges_;
  std::vector<FillRange> fills_;
  std::size_t draw_calls_ = 0;

  void ensure_uniforms(std::size_t slots);
  void rebuild_bind_group();
};

}  // namespace cc::ui
