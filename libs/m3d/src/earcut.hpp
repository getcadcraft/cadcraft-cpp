// SPDX-License-Identifier: Apache-2.0
// earcut (earcut.cpp): polygon-with-holes triangulation as CadCraft 0.5's m3d/earcut.js does it.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace cc::m3d::detail {

/// earcut(flat, holeIndices) → vertex indices, three per triangle; flat = [x0, y0, x1, y1, …], holes = the index of
/// each hole's first vertex.
[[nodiscard]] std::vector<std::size_t> earcut(std::span<const double> data, std::span<const std::size_t> holes);

}  // namespace cc::m3d::detail
