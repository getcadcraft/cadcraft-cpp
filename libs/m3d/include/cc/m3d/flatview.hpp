// SPDX-License-Identifier: Apache-2.0
// flatview.js (CadCraft 0.4 C18): flat 2D views of the 3D model — every hard edge of every solid (and loose edges); a
// soft edge only where it is the outline seen from that side. Hidden-line removal: each edge is sampled against the
// model's triangles seen from the view (a 2D grid finds the triangles under a point), and where it changes between in
// front and behind, the change is found by halving to a millionth of the view. Pieces on one line become one line; a
// hidden piece where a visible one is drawn goes.
#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "cc/m3d/model.hpp"

namespace cc::m3d::flat {

/// A view's frame: u to the right on paper, v up on paper, w toward the viewer (u × v = w)
struct Frame {
  Vec3 u, v, w;
};
/// VIEWS: top, bottom, front, back, left, right, iso (south-east); nullptr for another name
[[nodiscard]] const Frame* view_frame(std::string_view view);

struct Projected {
  std::vector<std::array<double, 4>> visible, hidden;  // u1, v1, u2, v2 (meters)
  std::optional<std::array<double, 4>> box;            // u0, v0, u1, v1; nothing: no edges
};
/// projectModel(model, view, { hidden })
[[nodiscard]] Projected project_model(const Model& model, std::string_view view, bool hidden);

/// parseScale: '1:50' → 0.02, '2:1' → 2, '1/20' → 0.05, '0.5' → 0.5; nothing when it is not a scale
[[nodiscard]] std::optional<double> parse_scale(std::string_view s);

}  // namespace cc::m3d::flat
