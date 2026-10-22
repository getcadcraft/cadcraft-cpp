// SPDX-License-Identifier: Apache-2.0
// Robust geometric predicates as CivilCraft 0.5's tin.js and Delaunator call them (robust-predicates 3.0.3, a port
// of Shewchuk's adaptive predicates). orient2d gives the same value as the JS, bit for bit: tin.js reads its magnitude
// (Mesh.side), so the adaptive stages are followed as written. incircle's sign is exact (only its sign is read).
#pragma once

namespace cc::civil {

/// robust-predicates orient2d(ax, ay, bx, by, cx, cy): > 0 when a, b, c turn clockwise (y down), < 0 counter-clockwise.
[[nodiscard]] double orient2d(double ax, double ay, double bx, double by, double cx, double cy) noexcept;
/// robust-predicates incircle: > 0 when d lies inside the circle through a, b, c; the sign is exact. A non-finite
/// input gives NaN (the JS gives NaN or another non-positive value then).
[[nodiscard]] double incircle(double ax, double ay, double bx, double by, double cx, double cy, double dx,
                              double dy) noexcept;

}  // namespace cc::civil
