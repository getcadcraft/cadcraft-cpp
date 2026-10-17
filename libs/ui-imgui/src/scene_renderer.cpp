// SPDX-License-Identifier: Apache-2.0
#include "cc/ui/scene_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace cc::ui {

namespace {

constexpr std::uint64_t kSlot = 256;  // minUniformBufferOffsetAlignment

constexpr std::string_view kWgsl = R"(
struct U { m: vec4f, t: vec4f, color: vec4f, p: vec4f };
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

fn to_device(p: vec2f) -> vec2f {
  return vec2f(u.m.x * p.x + u.m.z * p.y + u.t.x, u.m.y * p.x + u.m.w * p.y + u.t.y);
}
fn to_clip(d: vec2f) -> vec4f {
  return vec4f(d.x / u.t.z * 2.0 - 1.0, 1.0 - d.y / u.t.w * 2.0, 0.0, 1.0);
}
const kCorner = array<vec2f, 6>(vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
                                vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));

// ---- lines: a segment per instance, a quad of the line's width (+1 px for the edge's antialiasing), butt caps
struct SegOut { @builtin(position) pos: vec4f, @location(0) d: f32 };
@vertex fn seg_vs(@builtin(vertex_index) vi: u32, @location(0) seg: vec4f) -> SegOut {
  let a = to_device(seg.xy);
  let b = to_device(seg.zw);
  var dir = b - a;
  let len = length(dir);
  dir = select(vec2f(1.0, 0.0), dir / len, len > 1e-6);
  let n = vec2f(-dir.y, dir.x);
  let half = u.p.x * 0.5 + 0.5;
  let c = kCorner[vi];
  let side = c.y * 2.0 - 1.0;
  let p = select(a, b, c.x > 0.5) + n * side * half;
  var o: SegOut;
  o.pos = to_clip(p);
  o.d = side * half;
  return o;
}
@fragment fn seg_fs(i: SegOut) -> @location(0) vec4f {
  let k = clamp(u.p.x * 0.5 + 0.5 - abs(i.d), 0.0, 1.0);
  return vec4f(u.color.rgb, u.color.a * k);
}

// ---- fills: the rings' fans into the stencil (invert), then the fill's box where the stencil is odd
@vertex fn fan_vs(@location(0) p: vec2f) -> @builtin(position) vec4f {
  return to_clip(to_device(p));
}
@fragment fn fan_fs() -> @location(0) vec4f {
  return vec4f(0.0);
}
struct CoverOut { @builtin(position) pos: vec4f, @location(0) alpha: f32 };
@vertex fn cover_vs(@location(0) p: vec3f) -> CoverOut {
  var o: CoverOut;
  o.pos = to_clip(to_device(p.xy));
  o.alpha = p.z;
  return o;
}
@fragment fn cover_fs(i: CoverOut) -> @location(0) vec4f {
  return vec4f(u.color.rgb, u.color.a * i.alpha);
}

// ---- text: a glyph per instance, placed and turned as dwg-view.js places text (screen-space size, skipped < 2 px)
struct GlyphIn {
  @location(0) a: vec4f,   // anchor x, y (origin-relative), height (world), rotation
  @location(1) q: vec4f,   // em quad x0, y0, x1, y1 (y up)
  @location(2) uv: vec4f,  // u0, v0 (top), u1, v1 (bottom)
  @location(3) e: vec4f,   // mirror
};
struct TextOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) px_per_em: f32 };
@vertex fn text_vs(@builtin(vertex_index) vi: u32, g: GlyphIn) -> TextOut {
  var o: TextOut;
  let css = g.a.z * u.p.y;
  if (!(css >= 2.0 && css <= 4000.0)) {
    o.pos = vec4f(2.0, 2.0, 0.0, 1.0);
    o.uv = vec2f(0.0);
    o.px_per_em = 1.0;
    return o;
  }
  let size = css * u.p.z * 1.1;
  let anchor = to_device(g.a.xy);
  let cr = cos(g.a.w);
  let sr = sin(g.a.w);
  // the text's baseline on screen (dwg-view.js turns the canvas by atan2 of it): its unit vector is the cos/sin of
  // the turn, with no atan2 (a software atan2 got −0 wrong: text upside down in the browser)
  let base = vec2f(u.m.x * cr + u.m.z * sr, u.m.y * cr + u.m.w * sr);
  let blen = length(base);
  let dir = select(vec2f(1.0, 0.0), base / blen, blen > 1e-12);
  let c = kCorner[vi];
  let em = vec2f(select(g.q.x, g.q.z, c.x > 0.5), select(g.q.y, g.q.w, c.y > 0.5));
  var local = vec2f(em.x, -em.y) * size;
  if (g.e.x > 0.5) {
    local.x = -local.x;
  }
  let rc = dir.x;
  let rs = dir.y;
  o.pos = to_clip(anchor + vec2f(local.x * rc - local.y * rs, local.x * rs + local.y * rc));
  o.uv = vec2f(select(g.uv.x, g.uv.z, c.x > 0.5), select(g.uv.w, g.uv.y, c.y > 0.5));
  o.px_per_em = size;
  return o;
}
// coverage of one SDF sample: the distance in device px (u.p.w = the atlas's 0→1 span in em) through a 1 px ramp
fn cover(d: f32, px_per_em: f32, bias: f32) -> f32 {
  return clamp((d - 0.5 + bias) * u.p.w * px_per_em + 0.5, 0.0, 1.0);
}
@fragment fn text_fs(i: TextOut) -> @location(0) vec4f {
  // four samples over the pixel's footprint (area coverage, as a rasteriser gives); below 12 px a little stem
  // darkening so thin strokes stay readable, as browsers' hinting keeps them
  let dx = dpdx(i.uv) * 0.25;
  let dy = dpdy(i.uv) * 0.25;
  let bias = clamp((12.0 - i.px_per_em) / 12.0, 0.0, 1.0) * 0.03;
  let k = 0.25 * (cover(textureSample(atlas, samp, i.uv - dx - dy).r, i.px_per_em, bias) +
                  cover(textureSample(atlas, samp, i.uv + dx - dy).r, i.px_per_em, bias) +
                  cover(textureSample(atlas, samp, i.uv - dx + dy).r, i.px_per_em, bias) +
                  cover(textureSample(atlas, samp, i.uv + dx + dy).r, i.px_per_em, bias));
  return vec4f(u.color.rgb, u.color.a * k);
}
)";

struct Uniform {
  std::array<float, 4> m;
  std::array<float, 4> t;
  std::array<float, 4> color;
  std::array<float, 4> p;
};
static_assert(sizeof(Uniform) == 64);

wgpu::Buffer make_buffer(const wgpu::Device& d, std::uint64_t size, wgpu::BufferUsage usage) {
  wgpu::BufferDescriptor bd;
  bd.size = std::max<std::uint64_t>(16, (size + 3) & ~std::uint64_t{3});
  bd.usage = usage | wgpu::BufferUsage::CopyDst;
  return d.CreateBuffer(&bd);
}

template <class T>
wgpu::Buffer upload_vector(const wgpu::Device& d, const wgpu::Queue& q, const std::vector<T>& v,
                           wgpu::BufferUsage usage) {
  const std::size_t bytes = v.size() * sizeof(T);
  wgpu::Buffer b = make_buffer(d, bytes, usage);
  if (bytes > 0) {
    q.WriteBuffer(b, 0, v.data(), bytes);
  }
  return b;
}

wgpu::DepthStencilState depth_stencil(wgpu::CompareFunction compare, wgpu::StencilOperation pass) {
  wgpu::DepthStencilState ds;
  ds.format = kDepthStencil;
  ds.depthWriteEnabled = wgpu::OptionalBool::False;
  ds.depthCompare = wgpu::CompareFunction::Always;
  ds.stencilFront = {.compare = compare,
                     .failOp = wgpu::StencilOperation::Keep,
                     .depthFailOp = wgpu::StencilOperation::Keep,
                     .passOp = pass};
  ds.stencilBack = ds.stencilFront;
  return ds;
}

}  // namespace

SceneRenderer::SceneRenderer(wgpu::Device device, wgpu::TextureFormat color) : device_(std::move(device)) {
  queue_ = device_.GetQueue();
  wgpu::ShaderSourceWGSL src;
  src.code = kWgsl;
  wgpu::ShaderModuleDescriptor smd;
  smd.nextInChain = &src;
  const wgpu::ShaderModule shader = device_.CreateShaderModule(&smd);

  std::array<wgpu::BindGroupLayoutEntry, 3> entries{};
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.hasDynamicOffset = true;
  entries[0].buffer.minBindingSize = sizeof(Uniform);
  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Fragment;
  entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
  wgpu::BindGroupLayoutDescriptor bld;
  bld.entryCount = entries.size();
  bld.entries = entries.data();
  layout_ = device_.CreateBindGroupLayout(&bld);
  wgpu::PipelineLayoutDescriptor pld;
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &layout_;
  const wgpu::PipelineLayout pl = device_.CreatePipelineLayout(&pld);

  wgpu::BlendState blend;
  blend.color = {.operation = wgpu::BlendOperation::Add,
                 .srcFactor = wgpu::BlendFactor::SrcAlpha,
                 .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha};
  blend.alpha = {.operation = wgpu::BlendOperation::Add,
                 .srcFactor = wgpu::BlendFactor::One,
                 .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha};

  // the pipelines start compiling now; on the web in the background (the first frame that needs them waits for
  // nothing: draw() is empty until they are there), natively at once (Dawn compiles them here)
  auto pipeline = [&](wgpu::RenderPipeline Pipelines::* slot, const char* vs, const char* fs,
                      const wgpu::VertexBufferLayout& vbl, wgpu::DepthStencilState ds, bool write_color) {
    wgpu::ColorTargetState target;
    target.format = color;
    target.blend = &blend;
    target.writeMask = write_color ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None;
    wgpu::FragmentState frag;
    frag.module = shader;
    frag.entryPoint = fs;
    frag.targetCount = 1;
    frag.targets = &target;
    wgpu::RenderPipelineDescriptor rpd;
    rpd.layout = pl;
    rpd.vertex.module = shader;
    rpd.vertex.entryPoint = vs;
    rpd.vertex.bufferCount = 1;
    rpd.vertex.buffers = &vbl;
    rpd.fragment = &frag;
    rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    rpd.primitive.cullMode = wgpu::CullMode::None;
    rpd.depthStencil = &ds;
    rpd.multisample.count = kSamples;
#if defined(__EMSCRIPTEN__)
    (void)device_.CreateRenderPipelineAsync(
        &rpd, wgpu::CallbackMode::AllowSpontaneous,
        [pipes = pipes_, slot](wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline p, wgpu::StringView) {
          if (status == wgpu::CreatePipelineAsyncStatus::Success) {
            (*pipes).*slot = std::move(p);
          }
        });
#else
    (*pipes_).*slot = device_.CreateRenderPipeline(&rpd);
#endif
  };

  const std::array<wgpu::VertexAttribute, 1> seg_attr{
      {{.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 0}}};
  const wgpu::VertexBufferLayout seg_l{.stepMode = wgpu::VertexStepMode::Instance,
                                       .arrayStride = 16,
                                       .attributeCount = 1,
                                       .attributes = seg_attr.data()};
  const std::array<wgpu::VertexAttribute, 1> fan_attr{
      {{.format = wgpu::VertexFormat::Float32x2, .offset = 0, .shaderLocation = 0}}};
  const wgpu::VertexBufferLayout fan_l{
      .stepMode = wgpu::VertexStepMode::Vertex, .arrayStride = 8, .attributeCount = 1, .attributes = fan_attr.data()};
  const std::array<wgpu::VertexAttribute, 1> cover_attr{
      {{.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0}}};
  const wgpu::VertexBufferLayout cover_l{.stepMode = wgpu::VertexStepMode::Vertex,
                                         .arrayStride = 12,
                                         .attributeCount = 1,
                                         .attributes = cover_attr.data()};
  const std::array<wgpu::VertexAttribute, 4> glyph_attr{
      {{.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 0},
       {.format = wgpu::VertexFormat::Float32x4, .offset = 16, .shaderLocation = 1},
       {.format = wgpu::VertexFormat::Float32x4, .offset = 32, .shaderLocation = 2},
       {.format = wgpu::VertexFormat::Float32x4, .offset = 48, .shaderLocation = 3}}};
  const wgpu::VertexBufferLayout glyph_l{.stepMode = wgpu::VertexStepMode::Instance,
                                         .arrayStride = sizeof(render::GlyphInstance),
                                         .attributeCount = 4,
                                         .attributes = glyph_attr.data()};

  using C = wgpu::CompareFunction;
  using S = wgpu::StencilOperation;
  pipeline(&Pipelines::segments, "seg_vs", "seg_fs", seg_l, depth_stencil(C::Always, S::Keep), true);
  pipeline(&Pipelines::stencil, "fan_vs", "fan_fs", fan_l, depth_stencil(C::Always, S::Invert), false);
  pipeline(&Pipelines::cover, "cover_vs", "cover_fs", cover_l, depth_stencil(C::NotEqual, S::Zero), true);
  pipeline(&Pipelines::text, "text_vs", "text_fs", glyph_l, depth_stencil(C::Always, S::Keep), true);

  wgpu::SamplerDescriptor sd;
  sd.magFilter = wgpu::FilterMode::Linear;
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.addressModeU = wgpu::AddressMode::ClampToEdge;
  sd.addressModeV = wgpu::AddressMode::ClampToEdge;
  sampler_ = device_.CreateSampler(&sd);
  upload_atlas(text::Atlas{.width = 1, .height = 1, .texels = {0}, .spread_em = 0, .generation = 0xFFFFFFFEU});
  ensure_uniforms(64);
}

void SceneRenderer::ensure_uniforms(std::size_t slots) {
  if (slots <= uniform_slots_ && uniforms_) {
    return;
  }
  uniform_slots_ = std::max<std::size_t>(slots, uniform_slots_ * 2);
  uniforms_ = make_buffer(device_, uniform_slots_ * kSlot, wgpu::BufferUsage::Uniform);
  rebuild_bind_group();
}

void SceneRenderer::rebuild_bind_group() {
  if (!uniforms_ || !atlas_tex_) {
    return;
  }
  std::array<wgpu::BindGroupEntry, 3> e{};
  e[0].binding = 0;
  e[0].buffer = uniforms_;
  e[0].size = sizeof(Uniform);
  e[1].binding = 1;
  e[1].textureView = atlas_tex_.CreateView();
  e[2].binding = 2;
  e[2].sampler = sampler_;
  wgpu::BindGroupDescriptor bgd;
  bgd.layout = layout_;
  bgd.entryCount = e.size();
  bgd.entries = e.data();
  bind_ = device_.CreateBindGroup(&bgd);
}

void SceneRenderer::upload_atlas(const text::Atlas& atlas) {
  if (atlas.generation == atlas_generation_ || atlas.width <= 0 || atlas.height <= 0) {
    return;
  }
  if (atlas.width != atlas_w_ || atlas.height != atlas_h_ || !atlas_tex_) {
    wgpu::TextureDescriptor td;
    td.size = {static_cast<std::uint32_t>(atlas.width), static_cast<std::uint32_t>(atlas.height), 1};
    td.format = wgpu::TextureFormat::R8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    atlas_tex_ = device_.CreateTexture(&td);
    atlas_w_ = atlas.width;
    atlas_h_ = atlas.height;
    rebuild_bind_group();
  }
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = atlas_tex_;
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = static_cast<std::uint32_t>(atlas.width);
  layout.rowsPerImage = static_cast<std::uint32_t>(atlas.height);
  const wgpu::Extent3D size{static_cast<std::uint32_t>(atlas.width), static_cast<std::uint32_t>(atlas.height), 1};
  queue_.WriteTexture(&dst, atlas.texels.data(), atlas.texels.size(), &layout, &size);
  atlas_generation_ = atlas.generation;
  spread_em_ = atlas.spread_em;
}

void SceneRenderer::upload(const render::Scene& scene) {
  std::vector<float> segs;
  std::vector<float> fan;
  std::vector<float> covers;
  std::vector<render::GlyphInstance> glyphs;
  ranges_.clear();
  fills_.clear();
  for (const render::SceneSpace& sp : scene.spaces) {
    for (const render::SceneBatch& sb : sp.batches) {
      Ranges r;
      r.seg_first = static_cast<std::uint32_t>(segs.size() / 4);
      r.seg_count = static_cast<std::uint32_t>(sb.segments.size() / 4);
      segs.insert(segs.end(), sb.segments.begin(), sb.segments.end());
      r.glyph_first = static_cast<std::uint32_t>(glyphs.size());
      r.glyph_count = static_cast<std::uint32_t>(sb.glyphs.size());
      glyphs.insert(glyphs.end(), sb.glyphs.begin(), sb.glyphs.end());
      r.fill_first = static_cast<std::uint32_t>(fills_.size());
      const auto fan_base = static_cast<std::uint32_t>(fan.size() / 2);
      fan.insert(fan.end(), sb.fan.begin(), sb.fan.end());
      for (const render::FillGeometry& f : sb.fills) {
        FillRange fr{.fan_first = fan_base + f.first,
                     .fan_count = f.count,
                     .cover_first = static_cast<std::uint32_t>(covers.size() / 3)};
        const float a = f.pattern ? 0.18F : 1.0F;
        const auto [x0, y0, x1, y1] = f.box;
        for (const auto& [x, y] :
             std::array<std::array<float, 2>, 6>{{{x0, y0}, {x1, y0}, {x0, y1}, {x0, y1}, {x1, y0}, {x1, y1}}}) {
          covers.insert(covers.end(), {x, y, a});
        }
        fills_.push_back(fr);
      }
      r.fill_count = static_cast<std::uint32_t>(fills_.size()) - r.fill_first;
      ranges_.emplace(&sb, r);
    }
  }
  seg_buf_ = upload_vector(device_, queue_, segs, wgpu::BufferUsage::Vertex);
  fan_buf_ = upload_vector(device_, queue_, fan, wgpu::BufferUsage::Vertex);
  cover_buf_ = upload_vector(device_, queue_, covers, wgpu::BufferUsage::Vertex);
  glyph_buf_ = upload_vector(device_, queue_, glyphs, wgpu::BufferUsage::Vertex);
}

void SceneRenderer::draw(const wgpu::RenderPassEncoder& pass, std::span<const render::DrawBatch> draws,
                         std::uint32_t width, std::uint32_t height, std::array<float, 4> canvas) {
  draw_calls_ = 0;
  if (draws.empty() || width == 0 || height == 0 || !ready()) {
    return;
  }
  ensure_uniforms(draws.size());
  std::vector<std::byte> data(draws.size() * kSlot);
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const render::DrawBatch& db = draws[i];
    const auto& m = db.to_device;
    Uniform u{};
    u.m = {static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]), static_cast<float>(m[3])};
    u.t = {static_cast<float>(m[4]), static_cast<float>(m[5]), static_cast<float>(width), static_cast<float>(height)};
    u.color = {db.color.r, db.color.g, db.color.b, db.color.a};
    u.p = {static_cast<float>(db.line_px), static_cast<float>(db.css_px_per_unit), static_cast<float>(db.dpr),
           spread_em_};
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Uniform)>>(u);
    std::ranges::copy(bytes, data.begin() + static_cast<std::ptrdiff_t>(i * kSlot));
  }
  queue_.WriteBuffer(uniforms_, 0, data.data(), data.size());
  pass.SetStencilReference(0);
  if (canvas[2] <= 0 || canvas[3] <= 0) {
    canvas = {0, 0, static_cast<float>(width), static_cast<float>(height)};
  }
  // the scissor in whole pixels: `r` ∩ the canvas ∩ the target; false when nothing is left
  auto scissor = [&](std::array<float, 4> r, std::array<std::uint32_t, 4>& out) {
    const float x0 = std::clamp(std::max(r[0], canvas[0]), 0.0F, static_cast<float>(width));
    const float y0 = std::clamp(std::max(r[1], canvas[1]), 0.0F, static_cast<float>(height));
    const float x1 = std::clamp(std::min(r[0] + r[2], canvas[0] + canvas[2]), 0.0F, static_cast<float>(width));
    const float y1 = std::clamp(std::min(r[1] + r[3], canvas[1] + canvas[3]), 0.0F, static_cast<float>(height));
    const auto sx = static_cast<std::uint32_t>(std::floor(x0));
    const auto sy = static_cast<std::uint32_t>(std::floor(y0));
    const auto ex = static_cast<std::uint32_t>(std::ceil(x1));
    const auto ey = static_cast<std::uint32_t>(std::ceil(y1));
    out = {sx, sy, ex > sx ? ex - sx : 0, ey > sy ? ey - sy : 0};
    return out[2] > 0 && out[3] > 0;
  };
  std::array<std::uint32_t, 4> full{};
  if (!scissor(canvas, full)) {
    return;
  }
  std::array<std::uint32_t, 4> current{};
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const render::DrawBatch& db = draws[i];
    const auto it = ranges_.find(db.geometry);
    if (it == ranges_.end()) {
      continue;
    }
    const Ranges& r = it->second;
    std::array<std::uint32_t, 4> want = full;
    if (db.clip && !scissor(db.scissor, want)) {
      continue;  // the viewport is off screen
    }
    if (want != current) {
      pass.SetScissorRect(want[0], want[1], want[2], want[3]);
      current = want;
    }
    const std::array<std::uint32_t, 1> offset{static_cast<std::uint32_t>(i * kSlot)};
    if (db.text) {
      if (r.glyph_count > 0) {
        pass.SetPipeline(pipes_->text);
        pass.SetBindGroup(0, bind_, offset.size(), offset.data());
        pass.SetVertexBuffer(0, glyph_buf_);
        pass.Draw(6, r.glyph_count, 0, r.glyph_first);
        ++draw_calls_;
      }
      continue;
    }
    for (std::uint32_t f = 0; f < r.fill_count; ++f) {
      const FillRange& fr = fills_[r.fill_first + f];
      pass.SetPipeline(pipes_->stencil);
      pass.SetBindGroup(0, bind_, offset.size(), offset.data());
      pass.SetVertexBuffer(0, fan_buf_);
      pass.Draw(fr.fan_count, 1, fr.fan_first, 0);
      pass.SetPipeline(pipes_->cover);
      pass.SetBindGroup(0, bind_, offset.size(), offset.data());
      pass.SetVertexBuffer(0, cover_buf_);
      pass.Draw(6, 1, fr.cover_first, 0);
      draw_calls_ += 2;
    }
    if (r.seg_count > 0) {
      pass.SetPipeline(pipes_->segments);
      pass.SetBindGroup(0, bind_, offset.size(), offset.data());
      pass.SetVertexBuffer(0, seg_buf_);
      pass.Draw(6, r.seg_count, 0, r.seg_first);
      ++draw_calls_;
    }
  }
}

}  // namespace cc::ui
