// SPDX-License-Identifier: Apache-2.0
// The GPU-ready form of a compiled drawing: per space and batch, float geometry relative to an origin near the batch's
// data (a double), so a drawing in survey coordinates (x = 500 000 m) stays steady at any zoom: the view transform is
// composed with the origin in double and only then rounded to float (AGENTS.md section 12, large coordinates).
//
//   Scene scene = build_scene(drawing, font);   // once per drawing (and when the font atlas must be rebuilt)
//   frame_batches(scene, drawing, space, view)  // per frame: what to draw, with each batch's float transform
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cc/drawing/compile.hpp"
#include "cc/render/view.hpp"
#include "cc/text/font.hpp"

namespace cc::render {

inline constexpr std::size_t kNoLayer = static_cast<std::size_t>(-1);

/// One glyph to draw: its text's anchor (origin-relative world units), the text's height (world), rotation (world
/// radians), mirror flag, and the glyph quad in em units after alignment, with its atlas rectangle.
struct GlyphInstance {
  float ax = 0, ay = 0;
  float h = 0;
  float rot = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  float mirror = 0;  // 1 = mirrored about the anchor
  float pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(GlyphInstance) == 16 * sizeof(float));

/// A fill: its rings as triangle fans (for an even-odd stencil: every ring fanned from its first point) and the
/// bounding box the cover pass draws.
struct FillGeometry {
  std::uint32_t first = 0;     // first vertex in SceneBatch::fan (x, y pairs)
  std::uint32_t count = 0;     // vertices
  std::array<float, 4> box{};  // min x, min y, max x, max y (origin-relative)
  bool pattern = false;        // pattern hatch: drawn at 18 % alpha
};

enum class BatchKind : std::uint8_t {
  drawing,     // Space::batches[batch]
  picture,     // a DWG viewer's raster image frame (its file is not loaded: docs/gaps.md)
  bitmap,      // a document's picture: its frame into the stencil (`fills`), then `quad` textured with `bitmap`
  sheet_fill,  // a layout's paper
  sheet_edge,  // its outline
};

struct SceneBatch {
  BatchKind kind = BatchKind::drawing;
  std::size_t batch = 0;         // index into Space::batches (drawing batches)
  std::size_t layer = kNoLayer;  // index into Drawing::layers (for the on/off and plot checks)
  double ox = 0, oy = 0;         // origin (world)
  std::vector<float> segments;   // x0, y0, x1, y1 per straight segment of every polyline
  std::vector<float> fan;        // fill triangles, x, y per vertex
  std::vector<FillGeometry> fills;
  std::vector<GlyphInstance> glyphs;
  std::string bitmap;       // bitmap: the picture's file
  std::vector<float> quad;  // bitmap: 6 vertices × (x, y origin-relative, u, v)
  float alpha = 1;          // bitmap
};

struct SceneSpace {
  std::vector<SceneBatch> batches;
  std::size_t segments = 0;  // totals, for the status bar and the benchmark
  std::size_t triangles = 0;
  std::size_t glyphs = 0;
};

struct Scene {
  std::vector<SceneSpace> spaces;  // parallel to Drawing::spaces
};

/// Every space's geometry. The font lays out the texts (and grows its atlas as it meets new glyphs).
[[nodiscard]] Scene build_scene(const drawing::Drawing& d, text::Font& font);

/// One draw this frame: a batch's fills and lines, or (`text`) its texts — dwg-view.js draws every batch's geometry,
/// then all their texts, per group (each viewport's model, then the space itself).
struct DrawBatch {
  const SceneBatch* geometry = nullptr;
  bool text = false;
  Rgba color;
  Affine to_device{};          // origin-relative world → device px (y down), double; cast to float at upload
  double css_px_per_unit = 1;  // CSS px per world unit (text size and culling, as the JS measures them)
  double dpr = 1;
  double line_px = 1;              // line width in device px
  bool clip = false;               // inside a viewport: draw only within `scissor`
  std::array<float, 4> scissor{};  // device px: x, y, w, h
};

struct FrameOptions {
  Theme theme = Theme::model();
  bool print = false;
  const std::vector<bool>* hidden = nullptr;  // per Drawing::layers index: true = off (may be null)
};

/// What to draw for space `s` in view `v`: the sheet's viewports first (the model through each, clipped), then the
/// space's own batches — dwg-view.js draw() order. Hidden layers and, in print view, layers that never print, are left
/// out.
[[nodiscard]] std::vector<DrawBatch> frame_batches(const Scene& scene, const drawing::Drawing& d, std::size_t s,
                                                   const View& v, const FrameOptions& o);

/// Text smaller than this many device px, or larger than 4000, is not drawn (dwg-view.js).
inline constexpr double kMinTextPx = 2;
inline constexpr double kMaxTextPx = 4000;
/// The view draws text at 1.1 × its height (the font size of dwg-view.js).
inline constexpr double kTextScale = 1.1;

}  // namespace cc::render
