// SPDX-License-Identifier: Apache-2.0
// Delaunator 5.1.0 (ISC, Mapbox; THIRD_PARTY.md) as CivilCraft 0.5 vendors it: the plain Delaunay
// triangulation tin.js starts from. Same triangles, half-edges and hull, in the same order, as the JS.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace cc::civil {

struct Delaunay {
  std::vector<std::int32_t> triangles;  // three vertex ids per triangle, counter-clockwise (y down)
  std::vector<std::int32_t> halfedges;  // the twin of each half-edge, -1 on the hull
  std::vector<std::int32_t> hull;       // hull vertex ids
};

/// `new Delaunator(coords)`: coords = [x0, y0, x1, y1, …]. Points within 2^-52 of the one before (in distance order)
/// are skipped, as the JS does.
[[nodiscard]] Delaunay delaunator(std::span<const double> coords);

}  // namespace cc::civil
