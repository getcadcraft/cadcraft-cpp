// SPDX-License-Identifier: Apache-2.0
// cc::ui — L6. Draws render::Scene through WebGPU (Dawn natively, the browser's WebGPU on the web): lines as instanced
// segment quads, fills through an even-odd stencil (every ring fanned, then the fill's box covered where the stencil is
// odd), text as instanced quads over the signed-distance-field atlas. 4× multisampled. One render pass; the caller
// adds ImGui after the scene in the same pass.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include "cc/image/decode.hpp"
#include "cc/render/scene.hpp"
#include "cc/text/font.hpp"

namespace cc::ui {

inline constexpr wgpu::TextureFormat kDepthStencil = wgpu::TextureFormat::Depth24PlusStencil8;
inline constexpr std::uint32_t kSamples = 4;

class SceneRenderer {
 public:
  SceneRenderer(wgpu::Device device, wgpu::TextureFormat color);

  /// A picture's pixels by its file name (as the drawing names it), or nothing while the drawing does not have it.
  using PictureSource = std::function<std::optional<image::Rgba>(const std::string& file)>;
  /// Uploads a scene's geometry (replacing the last one). The scene must outlive its use in draw(). Pictures are
  /// asked of `pictures` once each and kept as textures until forget_pictures().
  void upload(const render::Scene& scene, const PictureSource& pictures = {});
  /// Drops the pictures' textures (the drawing's files changed).
  void forget_pictures();
  /// A picture under the drawing (a point cloud's plan: cloud-cad.js draw), in device px with its top left at `at`;
  /// nullptr: none. Drawn first in each frame, unclipped by the drawing's fills.
  void set_underlay(const image::Rgba* img, std::array<float, 2> at);
  /// Uploads the glyph atlas when its generation changed.
  void upload_atlas(const text::Atlas& atlas);
  /// Records the draws into `pass` (a pass on a `kSamples`-sampled target of `width` × `height` device px, with a
  /// `kDepthStencil` attachment), clipped to `canvas` (device px: x, y, w, h; w = 0 → the whole target).
  void draw(const wgpu::RenderPassEncoder& pass, std::span<const render::DrawBatch> draws, std::uint32_t width,
            std::uint32_t height, std::array<float, 4> canvas = {});

  /// True once the pipelines exist (on the web they are compiled in the background, started by the constructor);
  /// draw() draws nothing before.
  [[nodiscard]] bool ready() const noexcept {
    return pipes_->segments && pipes_->stencil && pipes_->cover && pipes_->text && pipes_->picture;
  }
  /// The draws of the last frame, for the status bar and the benchmark.
  [[nodiscard]] std::size_t last_draw_calls() const noexcept { return draw_calls_; }

 private:
  struct Ranges {
    std::uint32_t seg_first = 0, seg_count = 0;      // segment instances
    std::uint32_t glyph_first = 0, glyph_count = 0;  // glyph instances
    std::uint32_t fill_first = 0, fill_count = 0;    // into fills_
    std::uint32_t pic_first = 0;                     // bitmap: 6 vertices in the picture buffer
    std::size_t pic = kNoPicture;                    // bitmap: into pics_ (kNoPicture: its file is missing)
  };
  static constexpr std::size_t kNoPicture = static_cast<std::size_t>(-1);
  struct PicTex {
    std::string file;
    wgpu::Texture tex;  // null: the file could not be had
    wgpu::BindGroup bind;
  };
  struct FillRange {
    std::uint32_t fan_first = 0, fan_count = 0;  // vertices in the fan buffer
    std::uint32_t cover_first = 0;               // 6 vertices in the cover buffer
  };

  wgpu::Device device_;
  wgpu::Queue queue_;
  struct Pipelines {
    wgpu::RenderPipeline segments, stencil, cover, text, picture, underlay;
  };
  // filled when the device has compiled them (on the web asynchronously: a callback may outlive this renderer, so it
  // holds its own reference)
  std::shared_ptr<Pipelines> pipes_ = std::make_shared<Pipelines>();
  wgpu::BindGroupLayout layout_;
  wgpu::Buffer uniforms_;
  std::size_t uniform_slots_ = 0;
  wgpu::Buffer seg_buf_, fan_buf_, cover_buf_, glyph_buf_, pic_buf_;
  std::vector<PicTex> pics_;
  wgpu::Texture under_tex_;
  wgpu::BindGroup under_bind_;
  wgpu::Buffer under_buf_;
  std::uint32_t under_w_ = 0, under_h_ = 0;
  bool under_on_ = false;
  std::uint32_t max_texture_ = 8192;
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
  [[nodiscard]] wgpu::BindGroup bind_for(const wgpu::Texture& tex) const;
  std::size_t picture_index(const std::string& file, const PictureSource& pictures);
};

}  // namespace cc::ui
