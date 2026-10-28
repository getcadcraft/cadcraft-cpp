// SPDX-License-Identifier: Apache-2.0
// vec.js: PartCraft's 3D vectors, plane frames and 4x4 column-major matrices, computed as the JS computes them (the
// same operation order; Math.hypot / sin / cos through base's fdlibm ports), so a frame or a matrix here is the JS's
// bit for bit.
#pragma once

#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"

namespace cc::partcraft::vec {

using V3 = std::array<double, 3>;
using M4 = std::array<double, 16>;

[[nodiscard]] inline V3 add(const V3& a, const V3& b) noexcept {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
[[nodiscard]] inline V3 sub(const V3& a, const V3& b) noexcept {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
[[nodiscard]] inline V3 mul(const V3& a, double s) noexcept {
  return {a[0] * s, a[1] * s, a[2] * s};
}
[[nodiscard]] inline double dot(const V3& a, const V3& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
[[nodiscard]] inline V3 cross(const V3& a, const V3& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] inline double len(const V3& a) noexcept {
  return base::js_hypot(a[0], a[1], a[2]);
}
/// norm: a zero (or NaN) length divides by 1, as `len(a) || 1`.
[[nodiscard]] inline V3 norm(const V3& a) noexcept {
  double l = len(a);
  if (l == 0 || std::isnan(l)) {
    l = 1;
  }
  return {a[0] / l, a[1] / l, a[2] / l};
}

/// A plane frame {o, x, y, n}.
struct Frame {
  V3 o{}, x{}, y{}, n{};
};

[[nodiscard]] inline Frame base_plane(std::string_view name) {
  if (name == "top") {
    return {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
  }
  if (name == "right") {
    return {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
  }
  return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
}

/// frameFor(n, p).
[[nodiscard]] inline Frame frame_for(const V3& n0, const V3& p) {
  const V3 n = norm(n0);
  V3 x{};
  if (std::abs(n[1]) > 0.9) {
    x = norm(sub({1, 0, 0}, mul(n, n[0])));
  } else {
    x = norm(cross({0, 1, 0}, n));
  }
  const V3 y = cross(n, x);
  const double d = dot(p, n);
  return {mul(n, d), x, y, n};
}

[[nodiscard]] inline V3 to_world(const Frame& f, double u, double v) noexcept {
  return {f.o[0] + f.x[0] * u + f.y[0] * v, f.o[1] + f.x[1] * u + f.y[1] * v, f.o[2] + f.x[2] * u + f.y[2] * v};
}
[[nodiscard]] inline std::array<double, 2> to_plane(const Frame& f, const V3& p) noexcept {
  const V3 d = sub(p, f.o);
  return {dot(d, f.x), dot(d, f.y)};
}
[[nodiscard]] inline Frame flip_frame(const Frame& f) noexcept {
  return {f.o, mul(f.x, -1), f.y, mul(f.n, -1)};
}

[[nodiscard]] inline M4 basis_matrix(const V3& a, const V3& b, const V3& c, const V3& o) noexcept {
  return {a[0], a[1], a[2], 0, b[0], b[1], b[2], 0, c[0], c[1], c[2], 0, o[0], o[1], o[2], 1};
}
[[nodiscard]] inline M4 frame_matrix(const Frame& f) noexcept {
  return basis_matrix(f.x, f.y, f.n, f.o);
}
[[nodiscard]] inline M4 translation(const V3& t) noexcept {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1};
}

/// rotationAbout(p, d, deg): right-hand rule about the axis through p.
[[nodiscard]] inline M4 rotation_about(const V3& p, const V3& d, double deg) {
  const V3 u = norm(d);
  const double x = u[0];
  const double y = u[1];
  const double z = u[2];
  const double a = deg * std::numbers::pi / 180;
  const double c = base::js_cos(a);
  const double s = base::js_sin(a);
  const double t = 1 - c;
  const std::array<double, 9> r{t * x * x + c,     t * x * y + s * z, t * x * z - s * y,
                                t * x * y - s * z, t * y * y + c,     t * y * z + s * x,
                                t * x * z + s * y, t * y * z - s * x, t * z * z + c};
  const V3 rp{r[0] * p[0] + r[3] * p[1] + r[6] * p[2], r[1] * p[0] + r[4] * p[1] + r[7] * p[2],
              r[2] * p[0] + r[5] * p[1] + r[8] * p[2]};
  return {r[0], r[1], r[2], 0, r[3], r[4], r[5], 0, r[6], r[7], r[8], 0, p[0] - rp[0], p[1] - rp[1], p[2] - rp[2], 1};
}

/// reflection(o, n): in the plane through o with normal n.
[[nodiscard]] inline M4 reflection(const V3& o, const V3& n0) {
  const V3 n = norm(n0);
  const double k = 2 * dot(o, n);
  const auto m = [&](std::size_t i, std::size_t j) { return (i == j ? 1 : 0) - 2 * n.at(i) * n.at(j); };
  return {m(0, 0), m(1, 0), m(2, 0), 0, m(0, 1),  m(1, 1),  m(2, 1),  0,
          m(0, 2), m(1, 2), m(2, 2), 0, k * n[0], k * n[1], k * n[2], 1};
}

[[nodiscard]] inline V3 apply_mat(const M4& m, const V3& p) noexcept {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
[[nodiscard]] inline V3 apply_dir(const M4& m, const V3& d) noexcept {
  return {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
          m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
}
[[nodiscard]] inline M4 mat_mul(const M4& a, const M4& b) {
  M4 r{};
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t ri = 0; ri < 4; ++ri) {
      double s = 0;
      for (std::size_t k = 0; k < 4; ++k) {
        s += a.at(k * 4 + ri) * b.at(c * 4 + k);
      }
      r.at(c * 4 + ri) = s;
    }
  }
  return r;
}

/// solveDense(A, b): Gaussian elimination with partial pivoting; nullopt when singular. The rows are A's with b's entry
/// after each (`[...row, b[i]]`); reading past a row's end is NaN, as `undefined` is in the JS's arithmetic. The caller
/// gives A at least b.size() rows.
[[nodiscard]] inline std::optional<std::vector<double>> solve_dense(const std::vector<std::vector<double>>& A,
                                                                    const std::vector<double>& b) {
  const std::size_t n = b.size();
  std::vector<std::vector<double>> M;
  M.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::vector<double> row = A.at(i);
    row.push_back(b[i]);
    if (row.size() < n + 1) {
      row.resize(n + 1, std::nan(""));
    }
    M.push_back(std::move(row));
  }
  for (std::size_t c = 0; c < n; ++c) {
    std::size_t p = c;
    for (std::size_t r = c + 1; r < n; ++r) {
      if (std::abs(M[r][c]) > std::abs(M[p][c])) {
        p = r;
      }
    }
    if (std::abs(M[p][c]) < 1e-300) {
      return std::nullopt;
    }
    std::swap(M[c], M[p]);
    const double piv = M[c][c];
    for (std::size_t r = c + 1; r < n; ++r) {
      const double f = M[r][c] / piv;
      if (f == 0) {
        continue;
      }
      for (std::size_t k = c; k <= n; ++k) {
        M[r][k] -= f * M[c][k];
      }
    }
  }
  std::vector<double> x(n, 0.0);
  for (std::size_t r = n; r-- > 0;) {
    double s = M[r][n];
    for (std::size_t k = r + 1; k < n; ++k) {
      s -= M[r][k] * x[k];
    }
    x[r] = s / M[r][r];
  }
  return x;
}

/// fmt(n, dp): up to dp decimals, no trailing zeros; '' when not finite.
[[nodiscard]] inline std::string fmt(double n, int dp = 4) {
  if (!std::isfinite(n)) {
    return "";
  }
  const double a = std::abs(n) < 1e-9 ? 0 : n;
  std::string out;
  base::append_number(out, base::js_to_number(base::Value(base::js_to_fixed(a, dp))));
  return out;
}

// ---- values

/// A JS [x, y, z] (each entry through Number(); missing entries NaN).
[[nodiscard]] inline V3 v3(const base::Value& v) {
  V3 r{std::nan(""), std::nan(""), std::nan("")};
  if (v.is_array()) {
    const auto& a = v.as_array();
    for (std::size_t i = 0; i < 3 && i < a.size(); ++i) {
      r.at(i) = base::js_to_number(a[i]);
    }
  }
  return r;
}
[[nodiscard]] inline base::Value value(const V3& p) {
  return base::Array{p[0], p[1], p[2]};
}
[[nodiscard]] inline base::Value value(const M4& m) {
  base::Array a;
  a.reserve(16);
  for (const double x : m) {
    a.emplace_back(x);
  }
  return a;
}
[[nodiscard]] inline M4 m4(const base::Value& v) {
  M4 m{};
  m.fill(std::nan(""));
  if (v.is_array()) {
    const auto& a = v.as_array();
    for (std::size_t i = 0; i < 16 && i < a.size(); ++i) {
      m.at(i) = base::js_to_number(a[i]);
    }
  }
  return m;
}
[[nodiscard]] inline Frame frame(const base::Value& v) {
  return {v3(v.get("o")), v3(v.get("x")), v3(v.get("y")), v3(v.get("n"))};
}
[[nodiscard]] inline base::Value value(const Frame& f) {
  return base::make_object({{"o", value(f.o)}, {"x", value(f.x)}, {"y", value(f.y)}, {"n", value(f.n)}});
}

}  // namespace cc::partcraft::vec
