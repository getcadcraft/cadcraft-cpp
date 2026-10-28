// SPDX-License-Identifier: Apache-2.0
// vec.js, with the JS's arithmetic in the JS's order.
#include "cc/m3d/vec.hpp"

#include <cmath>
#include <cstddef>

namespace cc::m3d {

namespace {

Vec3 newell_raw(std::span<const Vec3> pts) noexcept {
  double x = 0;
  double y = 0;
  double z = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Vec3& p = pts[i];
    const Vec3& q = pts[(i + 1) % pts.size()];
    x += (p[1] - q[1]) * (p[2] + q[2]);
    y += (p[2] - q[2]) * (p[0] + q[0]);
    z += (p[0] - q[0]) * (p[1] + q[1]);
  }
  return {x, y, z};
}

double js_sign(double x) noexcept {
  if (std::isnan(x)) {
    return x;
  }
  if (x > 0) {
    return 1.0;
  }
  return x < 0 ? -1.0 : x;
}

}  // namespace

Vec3 newell(std::span<const Vec3> pts) noexcept {
  return norm(newell_raw(pts));
}

double poly_area3(std::span<const Vec3> pts) noexcept {
  const Vec3 v = newell_raw(pts);
  return base::js_hypot(v[0], v[1], v[2]) / 2;
}

Plane plane_from(const Vec3& n, const Vec3& p) noexcept {
  Vec3 m = norm(n);
  std::size_t k = 2;
  if (std::abs(m[0]) > 1e-9) {
    k = 0;
  } else if (std::abs(m[1]) > 1e-9) {
    k = 1;
  }
  if (m[k] < 0) {
    m = neg(m);
  }
  return {m, dot(m, p)};
}

bool same_plane(const Plane& a, const Plane& b) noexcept {
  return std::abs(dot(a.n, b.n)) > 1 - 1e-9 && std::abs(a.d * js_sign(dot(a.n, b.n)) - b.d) <= kEps;
}

Frame frame(const Vec3& n) noexcept {
  const Vec3 u = std::abs(n[2]) > 0.999 ? Vec3{1, 0, 0} : norm(cross({0, 0, 1}, n));
  return {u, cross(n, u), n};
}

RaySeg ray_to_segment(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b) noexcept {
  const Vec3 u = sub(b, a);
  const Vec3 w = sub(o, a);
  const double A = dot(d, d);
  const double B = dot(d, u);
  const double C = dot(u, u);
  const double D = dot(d, w);
  const double E = dot(u, w);
  const double den = A * C - B * B;
  double t = 0;
  double s = 0;
  if (den < 1e-14) {
    s = 0;
    t = -D / A;
  } else {
    t = (B * E - C * D) / den;
    s = (A * E - B * D) / den;
  }
  if (s < 0) {
    s = 0;
    t = -D / A;
  } else if (s > 1) {
    s = 1;
    t = (B - D) / A;
  }
  return {t, s, add(o, mul(d, t)), add(a, mul(u, s))};
}

OnLine closest_on_line(const Vec3& p, const Vec3& a, const Vec3& u) noexcept {
  const double s = dot(sub(p, a), u) / dot(u, u);
  return {s, add(a, mul(u, s))};
}

std::optional<LineLine> line_line(const Vec3& o1, const Vec3& d1, const Vec3& o2, const Vec3& d2) noexcept {
  const Vec3 w = sub(o1, o2);
  const double A = dot(d1, d1);
  const double B = dot(d1, d2);
  const double C = dot(d2, d2);
  const double D = dot(d1, w);
  const double E = dot(d2, w);
  const double den = A * C - B * B;
  if (std::abs(den) < 1e-14 * A * C) {
    return std::nullopt;
  }
  const double t = (B * E - C * D) / den;
  const double s = (A * E - B * D) / den;
  return LineLine{t, s, add(o1, mul(d1, t)), add(o2, mul(d2, s))};
}

std::optional<RayHit> ray_plane(const Vec3& o, const Vec3& d, const Plane& pl) noexcept {
  const double den = dot(pl.n, d);
  if (std::abs(den) < 1e-12) {
    return std::nullopt;
  }
  const double t = (pl.d - dot(pl.n, o)) / den;
  return RayHit{t, add(o, mul(d, t))};
}

std::optional<double> ray_tri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) noexcept {
  const Vec3 e1 = sub(b, a);
  const Vec3 e2 = sub(c, a);
  const Vec3 p = cross(d, e2);
  const double det = dot(e1, p);
  if (std::abs(det) < 1e-14) {
    return std::nullopt;
  }
  const double inv = 1 / det;
  const Vec3 s = sub(o, a);
  const double u = dot(s, p) * inv;
  if (u < -1e-9 || u > 1 + 1e-9) {
    return std::nullopt;
  }
  const Vec3 q = cross(s, e1);
  const double v = dot(d, q) * inv;
  if (v < -1e-9 || u + v > 1 + 1e-9) {
    return std::nullopt;
  }
  const double t = dot(e2, q) * inv;
  return t > 0 ? std::optional<double>(t) : std::nullopt;
}

Mat4 mmul(const Mat4& a, const Mat4& b) noexcept {
  Mat4 r{};
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t rr = 0; rr < 4; ++rr) {
      double s = 0;
      for (std::size_t k = 0; k < 4; ++k) {
        s += a[k * 4 + rr] * b[c * 4 + k];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): < 16
      }
      r[c * 4 + rr] = s;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
  }
  return r;
}

Mat4 scaling(double s, const Vec3& about) noexcept {
  const Mat4 S = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
  return mmul(translation(about), mmul(S, translation(neg(about))));
}

Mat4 rotation(const Vec3& axis, double angle, const Vec3& about) noexcept {
  const Vec3 n = norm(axis);
  const double x = n[0];
  const double y = n[1];
  const double z = n[2];
  const double c = base::js_cos(angle);
  const double s = base::js_sin(angle);
  const double t = 1 - c;
  const Mat4 R = {t * x * x + c,
                  t * x * y + s * z,
                  t * x * z - s * y,
                  0,
                  t * x * y - s * z,
                  t * y * y + c,
                  t * y * z + s * x,
                  0,
                  t * x * z + s * y,
                  t * y * z - s * x,
                  t * z * z + c,
                  0,
                  0,
                  0,
                  0,
                  1};
  return mmul(translation(about), mmul(R, translation(neg(about))));
}

Vec3 apply(const Mat4& m, const Vec3& p) noexcept {
  const double x = p[0];
  const double y = p[1];
  const double z = p[2];
  double w = m[3] * x + m[7] * y + m[11] * z + m[15];
  if (w == 0 || std::isnan(w)) {  // `|| 1`
    w = 1;
  }
  return {(m[0] * x + m[4] * y + m[8] * z + m[12]) / w, (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
          (m[2] * x + m[6] * y + m[10] * z + m[14]) / w};
}

Vec3 apply_normal(const Mat4& m, const Vec3& n) noexcept {
  const Mat4 inv = invert(m);
  return norm({inv[0] * n[0] + inv[1] * n[1] + inv[2] * n[2], inv[4] * n[0] + inv[5] * n[1] + inv[6] * n[2],
               inv[8] * n[0] + inv[9] * n[1] + inv[10] * n[2]});
}

Mat4 invert(const Mat4& m) noexcept {
  const Mat4& a = m;
  const auto [a00, a01, a02, a03, a10, a11, a12, a13, a20, a21, a22, a23, a30, a31, a32, a33] = a;
  const double b00 = a00 * a11 - a01 * a10;
  const double b01 = a00 * a12 - a02 * a10;
  const double b02 = a00 * a13 - a03 * a10;
  const double b03 = a01 * a12 - a02 * a11;
  const double b04 = a01 * a13 - a03 * a11;
  const double b05 = a02 * a13 - a03 * a12;
  const double b06 = a20 * a31 - a21 * a30;
  const double b07 = a20 * a32 - a22 * a30;
  const double b08 = a20 * a33 - a23 * a30;
  const double b09 = a21 * a32 - a22 * a31;
  const double b10 = a21 * a33 - a23 * a31;
  const double b11 = a22 * a33 - a23 * a32;
  double det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (det == 0 || std::isnan(det)) {  // `if (!det)`
    return ident();
  }
  det = 1 / det;
  return {(a11 * b11 - a12 * b10 + a13 * b09) * det, (a02 * b10 - a01 * b11 - a03 * b09) * det,
          (a31 * b05 - a32 * b04 + a33 * b03) * det, (a22 * b04 - a21 * b05 - a23 * b03) * det,
          (a12 * b08 - a10 * b11 - a13 * b07) * det, (a00 * b11 - a02 * b08 + a03 * b07) * det,
          (a32 * b02 - a30 * b05 - a33 * b01) * det, (a20 * b05 - a22 * b02 + a23 * b01) * det,
          (a10 * b10 - a11 * b08 + a13 * b06) * det, (a01 * b08 - a00 * b10 - a03 * b06) * det,
          (a30 * b04 - a31 * b02 + a33 * b00) * det, (a21 * b02 - a20 * b04 - a23 * b00) * det,
          (a11 * b07 - a10 * b09 - a12 * b06) * det, (a00 * b09 - a01 * b07 + a02 * b06) * det,
          (a31 * b01 - a30 * b03 - a32 * b00) * det, (a20 * b03 - a21 * b01 + a22 * b00) * det};
}

bool is_ident(const Mat4& m) noexcept {
  const Mat4 I = ident();
  for (std::size_t i = 0; i < 16; ++i) {
    if (!(std::abs(m[i] - I[i]) < 1e-12)) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      return false;
    }
  }
  return true;
}

void bbox_add(std::optional<Box>& bb, const Vec3& p) noexcept {
  if (!bb) {
    bb = Box{p, p};
    return;
  }
  for (std::size_t k = 0; k < 3; ++k) {
    bb->min[k] = std::min(bb->min[k], p[k]);
    bb->max[k] = std::max(bb->max[k], p[k]);
  }
}

}  // namespace cc::m3d
