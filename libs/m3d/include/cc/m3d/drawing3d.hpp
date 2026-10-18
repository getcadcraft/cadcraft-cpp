// SPDX-License-Identifier: Apache-2.0
// cc::m3d — CadCraft 0.5's model3d.js: the 3D model a drawing carries. A file's 3D faces come in as one group of
// loose faces (fromFile); saving writes every face as a 3DFACE and CadCraft's own copy of the model as CADCRAFT_3D
// point strings (forFile3d).
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"

namespace cc::m3d {

/// mPerUnit(units): meters per drawing unit (the DXF $INSUNITS names; unitless = millimeters).
[[nodiscard]] double m_per_unit(std::string_view units);
/// displayUnit(units): ModelCraft's display unit for a drawing unit.
[[nodiscard]] std::string display_unit(std::string_view units);
/// cleanView(v): a saved 3D view made safe ({ eye, target, persp, fov, style? }) or null.
[[nodiscard]] base::Value clean_view(const base::Value& v);

/// Model3D.fromFile(in3d, units): the model reset to the file's faces (in3d = split3d's { text, faces, view }).
/// → how many loose faces came in, and how many were left out (over 200,000). CadCraft's own copy (in3d.text) is not
/// read back yet (docs/gaps.md): its faces come in as loose faces.
struct FromFile {
  std::size_t loose = 0, left_out = 0;
};
FromFile from_file(Model& m, const base::Value& in3d, std::string_view units, std::string_view faces_name);

/// forFile3d(doc): every face as a 3DFACE (drawing units; triangles of non-convex or holed faces with their inner
/// edges invisible), and the model's JSON as "CC3D:<n>:<230 characters>" strings (+ "CCCAM:<view>"); nullopt when
/// the model is empty.
struct Face3d {
  std::array<Vec3, 4> pts{};
  int invis = 0;
};
struct File3d {
  std::vector<Face3d> faces;
  std::vector<std::string> points;
  std::string text;
};
[[nodiscard]] std::optional<File3d> for_file3d(const Model& m, std::string_view units);

inline constexpr std::string_view kM3dLayer = "CC-3D";
inline constexpr std::string_view kM3dBlock = "CADCRAFT_3D";

}  // namespace cc::m3d
