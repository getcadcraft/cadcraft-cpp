// SPDX-License-Identifier: Apache-2.0
// cc::render — L3. The 2D view of a compiled drawing, as BimCraft 0.5's dwg-view.js draws it, without a GPU: colours,
// the view transform, and the scene (render/scene.hpp) the GPU layer uploads. Everything here is a pure function of
// (drawing, view, options), so tests and headless export use it as the app does.
#pragma once

#include <array>
#include <string_view>

#include "cc/drawing/compile.hpp"

namespace cc::render {

using drawing::Affine;

struct Rgba {
  float r = 0, g = 0, b = 0, a = 1;
};

/// The site's colour tokens (assets/css/tokens.css in CadCraft 0.5): paper (light) and model (dark).
struct Theme {
  Rgba sheet;  // --sheet: the background
  Rgba paper;  // --sheet-2: a layout's paper
  Rgba ink;    // --ink: ACI 7 and the UI's text
  Rgba line;   // --line: paper edge, picture frames
  bool dark = false;
  [[nodiscard]] static Theme model();            // dark screen (the default)
  [[nodiscard]] static Theme light();            // paper
  [[nodiscard]] static Theme print(bool sheet);  // print view: white paper, black ink, a grey desk round sheets
};

/// A batch colour ("ink" or "#rrggbb") as the view draws it: ink → the theme's ink; pale colours darkened on a light
/// background, dark ones lightened on a dark one (dwg-view.js color()); print view: everything in ink.
[[nodiscard]] Rgba draw_color(std::string_view color, const Theme& theme, bool print);
/// "#rrggbb" → colour (grey for anything else, as the JS does).
[[nodiscard]] Rgba parse_hex(std::string_view hex);

/// The view: model point (cx, cy) at the centre, `zoom` CSS px per drawing unit, the canvas in CSS px, `dpr` device px
/// per CSS px.
struct View {
  double cx = 0;
  double cy = 0;
  double zoom = 1;
  double width = 1;   // CSS px
  double height = 1;  // CSS px
  double dpr = 1;
};

/// Zoom to the drawing (model) or the sheet (layout): dwg-view.js fit().
[[nodiscard]] View fit(const drawing::Space& space, View v);
/// Zoom by `factor` keeping the drawing point under (px, py) (CSS px from the top left) still; zoom stays 1e-9…1e9.
[[nodiscard]] View zoom_at(View v, double px, double py, double factor);
/// Pan by a pointer move of (dx, dy) CSS px.
[[nodiscard]] View pan_by(View v, double dx, double dy);
/// CSS px → drawing coordinates.
[[nodiscard]] std::array<double, 2> to_world(const View& v, double px, double py);
/// Drawing coordinates → device px (y down): the canvas transform of dwg-view.js draw().
[[nodiscard]] Affine world_to_device(const View& v);
/// A viewport's model → paper map: about the view centre, scaled to the viewport, twisted, moved to its centre.
[[nodiscard]] Affine viewport_matrix(const drawing::Viewport& vp);
/// p ∘ q (apply q first), as the JS mul().
[[nodiscard]] Affine mul(const Affine& p, const Affine& q);
/// Millimetres per paper unit of a sheet (inches unless the compiler found millimetres).
[[nodiscard]] double mm_per_unit(const drawing::Space& sheet);

}  // namespace cc::render
