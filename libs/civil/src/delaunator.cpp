// SPDX-License-Identifier: Apache-2.0
// Delaunator 5.1.0 (index.js), ported step by step (THIRD_PARTY.md). Its notice:
//
//   ISC License
//
//   Copyright (c) 2026, Mapbox
//
//   Permission to use, copy, modify, and/or distribute this software for any purpose
//   with or without fee is hereby granted, provided that the above copyright notice
//   and this permission notice appear in all copies.
//
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
//   REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
//   FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
//   INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
//   OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
//   TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
//   THIS SOFTWARE.
//
// Spec gap (hostile input only): where the JS indexes a typed array at NaN or -1 (a point exactly on the seed
// circle's centre gives a NaN hash key; a hash with no live slot) it goes on with `undefined`; here a NaN key leaves
// the hash as it was (a JS typed-array write at NaN does nothing) and such a point is skipped.
#include "cc/civil/delaunator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "cc/civil/predicates.hpp"

namespace cc::civil {
namespace {

constexpr double kEps = 2.220446049250313e-16;  // Math.pow(2, -52)
constexpr std::size_t kEdgeStack = 512;
constexpr double kInf = std::numeric_limits<double>::infinity();

double dist(double ax, double ay, double bx, double by) {
  const double dx = ax - bx;
  const double dy = ay - by;
  return dx * dx + dy * dy;
}
bool in_circle(double ax, double ay, double bx, double by, double cx, double cy, double px, double py) {
  const double dx = ax - px;
  const double dy = ay - py;
  const double ex = bx - px;
  const double ey = by - py;
  const double fx = cx - px;
  const double fy = cy - py;
  const double ap = dx * dx + dy * dy;
  const double bp = ex * ex + ey * ey;
  const double cp = fx * fx + fy * fy;
  return dx * (ey * cp - bp * fy) - dy * (ex * cp - bp * fx) + ap * (ex * fy - ey * fx) < 0;
}
double circumradius(double ax, double ay, double bx, double by, double cx, double cy) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double ex = cx - ax;
  const double ey = cy - ay;
  const double bl = dx * dx + dy * dy;
  const double cl = ex * ex + ey * ey;
  const double d = 0.5 / (dx * ey - dy * ex);
  const double x = (ey * bl - dy * cl) * d;
  const double y = (dx * cl - ex * bl) * d;
  return x * x + y * y;
}
std::pair<double, double> circumcenter(double ax, double ay, double bx, double by, double cx, double cy) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double ex = cx - ax;
  const double ey = cy - ay;
  const double bl = dx * dx + dy * dy;
  const double cl = ex * ex + ey * ey;
  const double d = 0.5 / (dx * ey - dy * ex);
  return {ax + (ey * bl - dy * cl) * d, ay + (dx * cl - ex * bl) * d};
}
double pseudo_angle(double dx, double dy) {
  const double p = dx / (std::abs(dx) + std::abs(dy));
  return (dy > 0 ? 3 - p : 1 + p) / 4;
}

// NOLINTNEXTLINE(misc-no-recursion) — depth-bounded: past 2000 levels it sorts by insertion
void quicksort(std::vector<std::uint32_t>& ids, const std::vector<double>& dists, std::ptrdiff_t left,
               std::ptrdiff_t right, int depth) {
  auto id = [&](std::ptrdiff_t i) -> std::uint32_t& { return ids[static_cast<std::size_t>(i)]; };
  auto d = [&](std::ptrdiff_t i) { return dists[id(i)]; };
  // the JS recursion is bounded by its own stack; a degenerate split goes deep, so past a depth fall back to the
  // insertion sort, which gives the same order (both sort by distance; only equal distances could differ)
  if (right - left <= 20 || depth > 2000) {
    for (std::ptrdiff_t i = left + 1; i <= right; ++i) {
      const std::uint32_t temp = id(i);
      const double temp_dist = dists[temp];
      std::ptrdiff_t j = i - 1;
      while (j >= left && d(j) > temp_dist) {
        id(j + 1) = id(j);
        --j;
      }
      id(j + 1) = temp;
    }
    return;
  }
  const std::ptrdiff_t median = (left + right) >> 1;
  std::ptrdiff_t i = left + 1;
  std::ptrdiff_t j = right;
  std::swap(id(median), id(i));
  if (d(left) > d(right)) {
    std::swap(id(left), id(right));
  }
  if (d(i) > d(right)) {
    std::swap(id(i), id(right));
  }
  if (d(left) > d(i)) {
    std::swap(id(left), id(i));
  }
  const std::uint32_t temp = id(i);
  const double temp_dist = dists[temp];
  while (true) {
    do {
      ++i;
    } while (i < right && d(i) < temp_dist);  // the bounds never stop a sound sort (median of three is a sentinel)
    do {
      --j;
    } while (j > left && d(j) > temp_dist);
    if (j < i) {
      break;
    }
    std::swap(id(i), id(j));
  }
  id(left + 1) = id(j);
  id(j) = temp;
  if (right - i + 1 >= j - left) {
    quicksort(ids, dists, i, right, depth + 1);
    quicksort(ids, dists, left, j - 1, depth + 1);
  } else {
    quicksort(ids, dists, left, j - 1, depth + 1);
    quicksort(ids, dists, i, right, depth + 1);
  }
}

class Builder {
 public:
  explicit Builder(std::span<const double> coords)
      : c_(coords),
        n_(coords.size() / 2),
        hash_size_(static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n_))))) {
    const std::size_t max_triangles = n_ > 2 ? 2 * n_ - 5 : 0;
    tri_.assign(max_triangles * 3, 0);
    he_.assign(max_triangles * 3, 0);
    hull_prev_.assign(n_, 0);
    hull_next_.assign(n_, 0);
    hull_tri_.assign(n_, 0);
    hull_hash_.assign(hash_size_, -1);
    ids_.assign(n_, 0);
    dists_.assign(n_, 0);
  }

  Delaunay run();

 private:
  std::span<const double> c_;
  std::size_t n_;
  std::vector<std::int32_t> tri_, he_;
  std::size_t hash_size_ = 0;
  std::vector<std::uint32_t> hull_prev_, hull_next_, hull_tri_;
  std::vector<std::int32_t> hull_hash_;
  std::vector<std::uint32_t> ids_;
  std::vector<double> dists_;
  std::size_t len_ = 0;
  double cx_ = 0, cy_ = 0;
  std::uint32_t hull_start_ = 0;
  std::array<std::uint32_t, kEdgeStack> stack_{};

  [[nodiscard]] double x(std::size_t i) const { return c_[2 * i]; }
  [[nodiscard]] double y(std::size_t i) const { return c_[2 * i + 1]; }
  /// The hash key, or -1 for NaN (Math.floor(NaN) % size is NaN in the JS).
  [[nodiscard]] std::ptrdiff_t hash_key(double px, double py) const {
    const double k = std::floor(pseudo_angle(px - cx_, py - cy_) * static_cast<double>(hash_size_));
    if (!std::isfinite(k)) {
      return -1;
    }
    return static_cast<std::ptrdiff_t>(std::fmod(k, static_cast<double>(hash_size_)));
  }
  void set_hash(double px, double py, std::uint32_t v) {
    const std::ptrdiff_t k = hash_key(px, py);
    if (k >= 0 && std::cmp_less(k, hash_size_)) {
      hull_hash_[static_cast<std::size_t>(k)] = static_cast<std::int32_t>(v);
    }
  }
  void link(std::size_t a, std::int32_t b) {
    he_[a] = b;
    if (b != -1) {
      he_[static_cast<std::size_t>(b)] = static_cast<std::int32_t>(a);
    }
  }
  std::size_t add_triangle(std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::int32_t a, std::int32_t b,
                           std::int32_t c) {
    const std::size_t t = len_;
    if (t + 3 > tri_.size()) {  // never for sound input (2n − 5 triangles at most)
      tri_.resize(t + 3, 0);
      he_.resize(t + 3, 0);
    }
    tri_[t] = static_cast<std::int32_t>(i0);
    tri_[t + 1] = static_cast<std::int32_t>(i1);
    tri_[t + 2] = static_cast<std::int32_t>(i2);
    link(t, a);
    link(t + 1, b);
    link(t + 2, c);
    len_ += 3;
    return t;
  }
  std::uint32_t legalize(std::size_t a);
};

std::uint32_t Builder::legalize(std::size_t a) {
  std::size_t i = 0;
  std::size_t ar = 0;
  while (true) {
    const std::int32_t b = he_[a];
    const std::size_t a0 = a - a % 3;
    ar = a0 + (a + 2) % 3;
    if (b == -1) {
      if (i == 0) {
        break;
      }
      a = stack_[--i];
      continue;
    }
    const auto bu = static_cast<std::size_t>(b);
    const std::size_t b0 = bu - bu % 3;
    const std::size_t al = a0 + (a + 1) % 3;
    const std::size_t bl = b0 + (bu + 2) % 3;
    const auto p0 = static_cast<std::size_t>(tri_[ar]);
    const auto pr = static_cast<std::size_t>(tri_[a]);
    const auto pl = static_cast<std::size_t>(tri_[al]);
    const auto pb = static_cast<std::size_t>(tri_[bl]);  // the JS's p1
    const bool illegal = in_circle(x(p0), y(p0), x(pr), y(pr), x(pl), y(pl), x(pb), y(pb));
    if (illegal) {
      tri_[a] = static_cast<std::int32_t>(pb);
      tri_[bu] = static_cast<std::int32_t>(p0);
      const std::int32_t hbl = he_[bl];
      if (hbl == -1) {
        std::uint32_t e = hull_start_;
        do {
          if (hull_tri_[e] == bl) {
            hull_tri_[e] = static_cast<std::uint32_t>(a);
            break;
          }
          e = hull_prev_[e];
        } while (e != hull_start_);
      }
      link(a, hbl);
      link(bu, he_[ar]);
      link(ar, static_cast<std::int32_t>(bl));
      const std::size_t br = b0 + (bu + 1) % 3;
      if (i < kEdgeStack) {
        stack_[i++] = static_cast<std::uint32_t>(br);
      }
    } else {
      if (i == 0) {
        break;
      }
      a = stack_[--i];
    }
  }
  return static_cast<std::uint32_t>(ar);
}

Delaunay Builder::run() {
  Delaunay out;
  double min_x = kInf;
  double min_y = kInf;
  double max_x = -kInf;
  double max_y = -kInf;
  for (std::size_t i = 0; i < n_; ++i) {
    const double px = x(i);
    const double py = y(i);
    min_x = std::min(min_x, px);  // (x < min ? x : min: NaN never wins, as in the JS)
    min_y = std::min(min_y, py);
    max_x = std::max(max_x, px);
    max_y = std::max(max_y, py);
    ids_[i] = static_cast<std::uint32_t>(i);
  }
  const double cx = (min_x + max_x) / 2;
  const double cy = (min_y + max_y) / 2;
  std::uint32_t i0 = 0;
  std::uint32_t i1 = 0;
  std::uint32_t i2 = 0;
  {
    double min_dist = kInf;
    for (std::size_t i = 0; i < n_; ++i) {
      const double d = dist(cx, cy, x(i), y(i));
      if (d < min_dist) {
        i0 = static_cast<std::uint32_t>(i);
        min_dist = d;
      }
    }
  }
  const double i0x = n_ ? x(i0) : std::nan("");
  const double i0y = n_ ? y(i0) : std::nan("");
  {
    double min_dist = kInf;
    for (std::size_t i = 0; i < n_; ++i) {
      if (i == i0) {
        continue;
      }
      const double d = dist(i0x, i0y, x(i), y(i));
      if (d < min_dist && d > 0) {
        i1 = static_cast<std::uint32_t>(i);
        min_dist = d;
      }
    }
  }
  double i1x = n_ ? x(i1) : std::nan("");
  double i1y = n_ ? y(i1) : std::nan("");
  double min_radius = kInf;
  for (std::size_t i = 0; i < n_; ++i) {
    if (i == i0 || i == i1) {
      continue;
    }
    const double r = circumradius(i0x, i0y, i1x, i1y, x(i), y(i));
    if (r < min_radius) {
      i2 = static_cast<std::uint32_t>(i);
      min_radius = r;
    }
  }
  double i2x = n_ ? x(i2) : std::nan("");
  double i2y = n_ ? y(i2) : std::nan("");

  if (min_radius == kInf) {
    for (std::size_t i = 0; i < n_; ++i) {
      const double dx = x(i) - c_[0];
      dists_[i] = (dx != 0 && !std::isnan(dx)) ? dx : (y(i) - c_[1]);
    }
    if (n_ > 0) {
      quicksort(ids_, dists_, 0, static_cast<std::ptrdiff_t>(n_) - 1, 0);
    }
    double d0 = -kInf;
    for (std::size_t i = 0; i < n_; ++i) {
      const std::uint32_t id = ids_[i];
      const double d = dists_[id];
      if (d > d0) {
        out.hull.push_back(static_cast<std::int32_t>(id));
        d0 = d;
      }
    }
    return out;
  }

  if (orient2d(i0x, i0y, i1x, i1y, i2x, i2y) < 0) {
    std::swap(i1, i2);
    std::swap(i1x, i2x);
    std::swap(i1y, i2y);
  }
  const auto [ccx, ccy] = circumcenter(i0x, i0y, i1x, i1y, i2x, i2y);
  cx_ = ccx;
  cy_ = ccy;
  for (std::size_t i = 0; i < n_; ++i) {
    dists_[i] = dist(x(i), y(i), ccx, ccy);
  }
  quicksort(ids_, dists_, 0, static_cast<std::ptrdiff_t>(n_) - 1, 0);

  hull_start_ = i0;
  std::size_t hull_size = 3;
  hull_next_[i0] = hull_prev_[i2] = i1;
  hull_next_[i1] = hull_prev_[i0] = i2;
  hull_next_[i2] = hull_prev_[i1] = i0;
  hull_tri_[i0] = 0;
  hull_tri_[i1] = 1;
  hull_tri_[i2] = 2;
  set_hash(i0x, i0y, i0);
  set_hash(i1x, i1y, i1);
  set_hash(i2x, i2y, i2);
  len_ = 0;
  (void)add_triangle(i0, i1, i2, -1, -1, -1);

  double xp = 0;
  double yp = 0;
  for (std::size_t k = 0; k < n_; ++k) {
    const std::uint32_t i = ids_[k];
    const double px = x(i);
    const double py = y(i);
    if (k > 0 && std::abs(px - xp) <= kEps && std::abs(py - yp) <= kEps) {
      continue;
    }
    xp = px;
    yp = py;
    if (i == i0 || i == i1 || i == i2) {
      continue;
    }
    // a visible edge on the hull through the hash
    std::int64_t start = -1;
    const std::ptrdiff_t key = hash_key(px, py);
    if (key < 0) {
      continue;  // spec gap (top of this file)
    }
    for (std::size_t j = 0; j < hash_size_; ++j) {
      start = hull_hash_[(static_cast<std::size_t>(key) + j) % hash_size_];
      if (start != -1 && std::cmp_not_equal(start, hull_next_[static_cast<std::size_t>(start)])) {
        break;
      }
    }
    if (start < 0) {
      continue;  // spec gap (top of this file)
    }
    std::uint32_t st = hull_prev_[static_cast<std::size_t>(start)];
    std::int64_t e = st;
    std::uint32_t q = 0;
    while (true) {
      q = hull_next_[static_cast<std::size_t>(e)];
      if (!(orient2d(px, py, x(static_cast<std::size_t>(e)), y(static_cast<std::size_t>(e)), x(q), y(q)) >= 0)) {
        break;
      }
      e = q;
      if (std::cmp_equal(e, st)) {
        e = -1;
        break;
      }
    }
    if (e == -1) {
      continue;
    }
    auto eu = static_cast<std::uint32_t>(e);
    std::size_t t = add_triangle(eu, i, hull_next_[eu], -1, -1, static_cast<std::int32_t>(hull_tri_[eu]));
    hull_tri_[i] = legalize(t + 2);
    hull_tri_[eu] = static_cast<std::uint32_t>(t);
    ++hull_size;

    std::uint32_t nn = hull_next_[eu];
    while (true) {
      q = hull_next_[nn];
      if (!(orient2d(px, py, x(nn), y(nn), x(q), y(q)) < 0)) {
        break;
      }
      t = add_triangle(nn, i, q, static_cast<std::int32_t>(hull_tri_[i]), -1, static_cast<std::int32_t>(hull_tri_[nn]));
      hull_tri_[i] = legalize(t + 2);
      hull_next_[nn] = nn;
      --hull_size;
      nn = q;
    }
    if (eu == st) {
      while (true) {
        q = hull_prev_[eu];
        if (!(orient2d(px, py, x(q), y(q), x(eu), y(eu)) < 0)) {
          break;
        }
        t = add_triangle(q, i, eu, -1, static_cast<std::int32_t>(hull_tri_[eu]),
                         static_cast<std::int32_t>(hull_tri_[q]));
        (void)legalize(t + 2);
        hull_tri_[q] = static_cast<std::uint32_t>(t);
        hull_next_[eu] = eu;
        --hull_size;
        eu = q;
      }
    }
    hull_start_ = hull_prev_[i] = eu;
    hull_next_[eu] = hull_prev_[nn] = i;
    hull_next_[i] = nn;
    set_hash(px, py, i);
    set_hash(x(eu), y(eu), eu);
  }

  out.hull.reserve(hull_size);
  std::uint32_t e = hull_start_;
  for (std::size_t i = 0; i < hull_size; ++i) {
    out.hull.push_back(static_cast<std::int32_t>(e));
    e = hull_next_[e];
  }
  tri_.resize(len_);
  he_.resize(len_);
  out.triangles = std::move(tri_);
  out.halfedges = std::move(he_);
  return out;
}

}  // namespace

Delaunay delaunator(std::span<const double> coords) {
  Builder b(coords);
  return b.run();
}

}  // namespace cc::civil
