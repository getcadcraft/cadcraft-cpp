// SPDX-License-Identifier: Apache-2.0
// cc::docview — xclip.js (CadCraft 0.5 WL.16): clipped block references. A reference's clip is
// `insert.clip = { pts: [{x, y}…] }`, a closed polygon in the block's own coordinates; walk() draws only what lies
// inside it (lines and arcs cut at the boundary, fills cut to it, texts and points kept when their insertion point is
// inside), so the screen, plots and extents all clip.
#pragma once

#include <optional>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"

namespace cc::docview {

/// insidePoly(p, poly): even–odd point in polygon.
[[nodiscard]] bool inside_poly(cad::P2 p, const std::vector<cad::P2>& poly);
/// clipLine(q, poly) / clipArc(q, poly): the pieces of a line / a counter-clockwise arc inside the polygon.
[[nodiscard]] std::vector<cad::Prim> clip_line(const cad::Prim& q, const std::vector<cad::P2>& poly);
[[nodiscard]] std::vector<cad::Prim> clip_arc(const cad::Prim& q, const std::vector<cad::P2>& poly);
/// clipRing(ring, poly): a fill ring [x, y, …] cut to the polygon (a concave clip keeps rings across it whole).
[[nodiscard]] std::optional<std::vector<double>> clip_ring(const std::vector<double>& l,
                                                           const std::vector<cad::P2>& poly);
/// clipPoly(ref): a usable clip polygon of a reference (block coordinates), or nothing.
[[nodiscard]] std::optional<std::vector<cad::P2>> clip_poly(const base::Value& ref);

}  // namespace cc::docview
