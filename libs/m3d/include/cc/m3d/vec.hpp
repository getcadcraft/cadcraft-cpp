// SPDX-License-Identifier: Apache-2.0
// cc::m3d — vec.js of CadCraft 0.5 (ModelCraft's m3d kernel, byte-identical in every app): 3D vectors as [x, y, z],
// 4×4 matrices as 16 numbers in column-major order (three.js / glTF; m[12..14] is the translation), with the JS's
// arithmetic (Math.hypot / sin / cos as V8 computes them: base/js_math.hpp), so results match the JS bit for bit.
// Model space is Z up, X = red, Y = green, Z = blue; lengths are meters.
#pragma once

#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "cc/base/js_math.hpp"

namespace cc::m3d {

using Vec3 = std::array<double, 3>;
using Vec2 = std::array<double, 2>;
using Mat4 = std::array<double, 16>;

inline constexpr double kEps = 1e-6;  // EPS: the length tolerance, 1 µm
inline constexpr double kEps2 = kEps * kEps;
inline constexpr double kPi = std::numbers::pi;  // Math.PI

[[nodiscard]] constexpr Vec3 add(const Vec3& a, const Vec3& b) noexcept {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
[[nodiscard]] constexpr Vec3 sub(const Vec3& a, const Vec3& b) noexcept {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
[[nodiscard]] constexpr Vec3 mul(const Vec3& a, double s) noexcept {
  return {a[0] * s, a[1] * s, a[2] * s};
}
[[nodiscard]] constexpr double dot(const Vec3& a, const Vec3& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] inline double len(const Vec3& a) noexcept {
  return base::js_hypot(a[0], a[1], a[2]);
}
[[nodiscard]] inline double dist(const Vec3& a, const Vec3& b) noexcept {
  return base::js_hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
[[nodiscard]] constexpr Vec3 lerp(const Vec3& a, const Vec3& b, double t) noexcept {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
[[nodiscard]] constexpr Vec3 neg(const Vec3& a) noexcept {
  return {-a[0], -a[1], -a[2]};
}
[[nodiscard]] inline bool near(const Vec3& a, const Vec3& b, double tol = kEps) noexcept {
  return dist(a, b) <= tol;
}
[[nodiscard]] inline Vec3 norm(const Vec3& a) noexcept {
  const double l = len(a);
  return l > 1e-15 ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 0};
}
/// Any unit vector perpendicular to n.
[[nodiscard]] inline Vec3 perp(const Vec3& n) noexcept {
  const Vec3 a = std::abs(n[0]) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  return norm(cross(n, a));
}
[[nodiscard]] inline bool parallel(const Vec3& a, const Vec3& b, double tol = 1e-9) noexcept {
  return len(cross(norm(a), norm(b))) <= tol;
}

/// Newell normal of a polygon: unit, CCW = right-hand rule; [0, 0, 0] if degenerate.
[[nodiscard]] Vec3 newell(std::span<const Vec3> pts) noexcept;
/// The polygon's area (half the Newell vector's length).
[[nodiscard]] double poly_area3(std::span<const Vec3> pts) noexcept;

/// A plane n·p = d, n canonical (its first non-zero component positive).
struct Plane {
  Vec3 n{};
  double d = 0;
};
[[nodiscard]] Plane plane_from(const Vec3& n, const Vec3& p) noexcept;
[[nodiscard]] inline bool on_plane(const Plane& pl, const Vec3& p, double tol = kEps) noexcept {
  return std::abs(dot(pl.n, p) - pl.d) <= tol;
}
[[nodiscard]] bool same_plane(const Plane& a, const Plane& b) noexcept;

/// A 2D frame on a plane with normal n: u × v = n (horizontal planes: u = +X; others: u horizontal, v up the plane).
struct Frame {
  Vec3 u{}, v{}, n{};
};
[[nodiscard]] Frame frame(const Vec3& n) noexcept;
[[nodiscard]] constexpr Vec2 to2(const Frame& f, const Vec3& p) noexcept {
  return {dot(p, f.u), dot(p, f.v)};
}

struct RaySeg {
  double t = 0, s = 0;
  Vec3 on_ray{}, on_seg{};
};
/// Closest points between the line o + t·d and the segment a–b.
[[nodiscard]] RaySeg ray_to_segment(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b) noexcept;
struct OnLine {
  double s = 0;
  Vec3 p{};
};
/// Closest point to p on the infinite line a + s·u.
[[nodiscard]] OnLine closest_on_line(const Vec3& p, const Vec3& a, const Vec3& u) noexcept;
struct LineLine {
  double t = 0, s = 0;
  Vec3 p1{}, p2{};
};
/// Closest points between two infinite lines; nothing when parallel.
[[nodiscard]] std::optional<LineLine> line_line(const Vec3& o1, const Vec3& d1, const Vec3& o2,
                                                const Vec3& d2) noexcept;
struct RayHit {
  double t = 0;
  Vec3 p{};
};
[[nodiscard]] std::optional<RayHit> ray_plane(const Vec3& o, const Vec3& d, const Plane& pl) noexcept;
/// Möller–Trumbore, both sides: t > 0, or nothing.
[[nodiscard]] std::optional<double> ray_tri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b,
                                            const Vec3& c) noexcept;

// ---- matrices (column-major)
[[nodiscard]] constexpr Mat4 ident() noexcept {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
[[nodiscard]] Mat4 mmul(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] constexpr Mat4 translation(const Vec3& t) noexcept {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1};
}
[[nodiscard]] Mat4 scaling(double s, const Vec3& about = {0, 0, 0}) noexcept;
/// Rotation by `angle` (radians, right-hand) about the axis through `about`.
[[nodiscard]] Mat4 rotation(const Vec3& axis, double angle, const Vec3& about = {0, 0, 0}) noexcept;
[[nodiscard]] Vec3 apply(const Mat4& m, const Vec3& p) noexcept;
/// Direction (no translation).
[[nodiscard]] constexpr Vec3 apply_dir(const Mat4& m, const Vec3& v) noexcept {
  return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
          m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}
/// Normal transform for an affine m (inverse transpose of its 3×3), normalised.
[[nodiscard]] Vec3 apply_normal(const Mat4& m, const Vec3& n) noexcept;
[[nodiscard]] constexpr double det3(const Mat4& m) noexcept {
  return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}
/// The inverse; the identity when singular.
[[nodiscard]] Mat4 invert(const Mat4& m) noexcept;
[[nodiscard]] bool is_ident(const Mat4& m) noexcept;

/// A bounding box {min, max}.
struct Box {
  Vec3 min{}, max{};
};
/// bboxAdd(bb, p): grows `bb` (or starts it) to hold p.
void bbox_add(std::optional<Box>& bb, const Vec3& p) noexcept;

}  // namespace cc::m3d
