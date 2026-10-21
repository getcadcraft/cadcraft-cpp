// SPDX-License-Identifier: Apache-2.0
// cloud-cad.js CloudAttach (CadCraft 0.5 SG.1): the point cloud under a window's drawing. It is not part of the
// drawing: not saved in the DWG / DXF and untouched by undo; one cloud per window. Where it sits: its file's own
// coordinates, or its lower-left corner at a point; drawing units per file unit from both units when both are known,
// else 1. A building scan (taller than 2 m, under 200 m across) starts as a plan cut at 1.2 m, 0.1 m thick.
//
// The drawing's units come from the caller (its meta.units and metres per unit: m3d m_per_unit), as this library sits
// below the document.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/cloud/cloud.hpp"

namespace cc::cloud {

/// UNIT_M: metres in one unit of a cloud's coordinate system (m, ft, ftUS)
[[nodiscard]] std::optional<double> unit_m(std::string_view unit);

/// The drawing a cloud sits under: its meta.units ("" when none) and metres per drawing unit
struct DrawingUnits {
  std::string units;
  double m_per_unit = 0.001;
};

struct Bbox {
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};

class Attached {
 public:
  struct Slice {
    bool on = false;
    std::optional<double> h, thick;  // drawing units; nothing = the defaults (1.2 m, 0.1 m)
  };
  struct SliceSpec {
    double h = 0, thick = 0;
  };

  [[nodiscard]] bool has() const noexcept { return cloud.has_value(); }
  void attach(Cloud c, std::optional<std::array<double, 2>> at);
  void detach();
  /// a setting changed: the version moves on (caches follow it)
  void changed();
  /// colours of the colour mode (`single` in `color_one`)
  void recolor(std::array<std::uint8_t, 3> color_one = {200, 200, 200});

  [[nodiscard]] double scale(const DrawingUnits& u) const;
  [[nodiscard]] double z_scale(const DrawingUnits& u) const;
  /// a file point (absolute, file units) → the drawing
  [[nodiscard]] V3 to_drawing(double x, double y, double z, const DrawingUnits& u) const;
  [[nodiscard]] std::array<double, 2> to_file(double x, double y, const DrawingUnits& u) const;
  [[nodiscard]] V3 drawing_point(std::size_t i, const DrawingUnits& u) const;
  [[nodiscard]] std::optional<Bbox> bbox(const DrawingUnits& u) const;
  [[nodiscard]] SliceSpec slice_spec(const DrawingUnits& u) const;
  /// the indices drawn: the slice's points, or nullptr (every point)
  [[nodiscard]] const std::vector<std::uint32_t>* shown(const DrawingUnits& u);
  /// the cloud point nearest p within r (drawing units), in the drawing; nothing when hidden or none near
  [[nodiscard]] std::optional<std::array<double, 2>> snap(double px, double py, double r, const DrawingUnits& u);

  std::optional<Cloud> cloud;
  std::string color = "rgb";
  int size = 2;
  bool visible = true;
  Slice slice;
  std::uint64_t version = 0;
  std::optional<std::array<double, 2>> insert;  // nothing: the file's own coordinates
  std::vector<std::uint8_t> colors;

 private:
  std::string shown_key_;
  std::vector<std::uint32_t> shown_;
  std::string grid_of_;
  std::optional<PlanGrid> grid_;
};

}  // namespace cc::cloud
