// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's tin.js: TIN surfaces — a constrained Delaunay triangulation (Delaunator, then breaklines forced in
// by Sloan's flips and Delaunay restored round them), elevations, contours, bands, slopes, outlines. Coordinates are
// moved to a whole-unit local origin before triangulating, so projected survey coordinates (state plane, UTM) keep
// their precision. Same vertices, triangles and numbers as the JS.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/i18n/i18n.hpp"

namespace cc::civil {

inline constexpr double kSnap = 1e-4;

/// A point given to buildSurface: z absent (or not a finite number) = skipped.
struct TinPoint {
  double x = 0, y = 0;
  std::optional<double> z;
};
struct P3 {
  double x = 0, y = 0, z = 0;
};
struct P2 {
  double x = 0, y = 0;
};

struct TinReport {
  std::vector<std::string> notes, errors;
  double skipped_no_z = 0, merged = 0, crossings = 0, z_conflicts = 0, constraint_failures = 0;
  std::optional<double> breakline_segments;  // set on success
};

struct SurfaceStats {
  double points = 0, triangles = 0, min_z = 0, max_z = 0, mean_z = 0, area2d = 0, area3d = 0;
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;  // bounds, project coordinates
};

struct Contour {
  double z = 0;
  std::vector<P2> pts;
  bool closed = false;
};
struct Band {
  double band = 0;
  std::vector<double> loop;  // x, y, x, y …
};
struct Slope {
  double x = 0, y = 0, slope = 0, dir = 0, size = 0;
};
struct Located {
  std::size_t t = 0;
  std::array<double, 3> w{};
};

class Surface {
 public:
  Surface(std::vector<double> x, std::vector<double> y, std::vector<double> z, std::vector<std::int32_t> tris,
          double ox, double oy);

  [[nodiscard]] const std::vector<double>& X() const noexcept { return x_; }
  [[nodiscard]] const std::vector<double>& Y() const noexcept { return y_; }
  [[nodiscard]] const std::vector<double>& Z() const noexcept { return z_; }
  [[nodiscard]] const std::vector<std::int32_t>& tris() const noexcept { return tris_; }
  [[nodiscard]] double ox() const noexcept { return ox_; }
  [[nodiscard]] double oy() const noexcept { return oy_; }
  [[nodiscard]] std::size_t triangle_count() const noexcept { return tris_.size() / 3; }
  std::vector<std::pair<std::int32_t, std::int32_t>> constrained_edges;

  [[nodiscard]] std::vector<std::uint8_t> used_vertices() const;
  [[nodiscard]] const SurfaceStats& stats() const;
  /// The convex hull of the used vertices, counter-clockwise, project coordinates.
  [[nodiscard]] std::vector<P2> hull() const;
  [[nodiscard]] std::optional<Located> locate(double lx, double ly, double tol = 1e-9) const;
  [[nodiscard]] std::optional<double> elevation_at(double x, double y, double tol = 1e-9) const;
  struct SlopeOf {
    double slope, dir, gx, gy;
  };
  [[nodiscard]] SlopeOf slope_of(std::size_t t) const;
  [[nodiscard]] std::vector<Slope> slopes() const;
  /// nullopt = more than max_levels levels (the JS returns null); empty when interval is not > 0.
  [[nodiscard]] std::optional<std::vector<Contour>> contours(double interval, double base = 0,
                                                             double max_levels = 2000) const;
  [[nodiscard]] std::vector<Band> bands(const std::vector<double>& levels) const;
  [[nodiscard]] std::vector<std::array<double, 9>> triangles() const;
  [[nodiscard]] std::vector<std::int32_t> edges() const;
  [[nodiscard]] std::vector<std::vector<P2>> outline() const;
  struct Faces {
    std::vector<P3> pts;
    std::vector<std::array<std::int32_t, 3>> faces;
  };
  [[nodiscard]] Faces to_faces() const;

 private:
  struct Grid {
    double bminx = 0, bminy = 0, bmaxx = 0, bmaxy = 0;
    std::size_t nx = 1, ny = 1;
    double cw = 1, ch = 1;
    std::vector<std::int32_t> start, items;
  };
  [[nodiscard]] const Grid& grid() const;
  std::vector<double> x_, y_, z_;
  std::vector<std::int32_t> tris_;
  double ox_ = 0, oy_ = 0;
  mutable std::optional<SurfaceStats> stats_;
  mutable std::unique_ptr<Grid> grid_;
};

struct Breakline {
  std::vector<P3> pts;
};
struct BuildInput {
  std::vector<TinPoint> points;
  std::vector<Breakline> breaklines;
  std::optional<std::vector<P2>> boundary;
  double max_edge = 0;
};
struct Built {
  std::shared_ptr<Surface> surface;  // null when the surface cannot be built (report.errors says why)
  TinReport report;
};

/// buildSurface({ points, breaklines, boundary, maxEdge })
[[nodiscard]] Built build_surface(const BuildInput& in, const i18n::Catalog& words);

/// surfaceFromFaces({ pts, faces }): a face is kept when its three ids are whole numbers of present points.
struct FaceInput {
  std::vector<std::optional<P3>> pts;
  std::vector<std::vector<double>> faces;
};
[[nodiscard]] Built surface_from_faces(const FaceInput& in, const i18n::Catalog& words);

// plane helpers (exported for grading and the UI); results index the input
[[nodiscard]] std::vector<std::size_t> convex_hull(const std::vector<P2>& pts);
[[nodiscard]] double poly_area(const std::vector<P2>& pts);
/// ccw(pts): the input order, reversed when clockwise.
[[nodiscard]] bool is_clockwise(const std::vector<P2>& pts);
[[nodiscard]] bool point_in_poly(double x, double y, const std::vector<P2>& pts);
/// clipConvex(subject, clip): each result point is a subject point (src = its index) or a new crossing (src = -1).
struct Clipped {
  double x = 0, y = 0;
  std::ptrdiff_t src = -1;
};
[[nodiscard]] std::vector<Clipped> clip_convex(const std::vector<P2>& subject, const std::vector<P2>& clip);

}  // namespace cc::civil
