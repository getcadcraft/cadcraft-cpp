// SPDX-License-Identifier: Apache-2.0
// cc::m3d io — L3. ModelCraft's file formats other than its own (io.js of CadCraft 0.5, m3d's copy in every 3D app):
// the exports, byte for byte the JS's (tools/parity/m3d-io.mjs), and the imports (M6.4).
//
//   export_glb   glTF 2.0 binary: one mesh per (definition, placement colour), instances as nodes, a root node that
//   turns
//                Z up into Y up
//   export_obj   Wavefront OBJ, Y up, world coordinates, n-gons (holed faces as triangles), loose edges as `l`;
//                with an MTL name, `usemtl` per colour; export_mtl writes that file
//   export_stl   binary STL in millimeters, Z up
//   export_dxf   DXF R12 in the model's display unit: faces as 3DFACE triangles, loose edges as LINEs, groups and
//                components as blocks + INSERTs; to_dwg_model the same as a cc-dwg writer model (write_dwg)
//   export_dae   Collada 1.4.1, Z up, meters
//   export_ifc   IFC4: one IfcBuildingElementProxy per top-level placement (+ "Loose geometry"), one storey per entry
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"

namespace cc::m3d::io {

[[nodiscard]] std::vector<std::uint8_t> export_glb(const Model& model);
/// `mtl`: the materials file's name (exportOBJ(model, {mtl})); nothing: no materials.
[[nodiscard]] std::string export_obj(const Model& model, std::optional<std::string_view> mtl = std::nullopt);
[[nodiscard]] std::string export_mtl(const Model& model);
[[nodiscard]] std::vector<std::uint8_t> export_stl(const Model& model);

/// The neutral CAD scene's report: {faces, triangles, lines, blocks, inserts, flattened, unit}.
struct CadReport {
  std::size_t faces = 0, triangles = 0, lines = 0, blocks = 0, inserts = 0, flattened = 0;
  std::string unit;
};
struct DxfExport {
  std::string text;
  CadReport report;
};
/// exportDXF: refused (the m3d.io words) past 2,000,000 triangles, lines and inserts.
[[nodiscard]] base::Result<DxfExport> export_dxf(const Model& model);
struct DwgExport {
  base::Value dwg;  // cc-dwg's model (docs/DWG-LIB-SPEC.md) for cc::dwg::write_dwg
  CadReport report;
};
[[nodiscard]] base::Result<DwgExport> to_dwg_model(const Model& model);
/// The nearest of AutoCAD's 255 colours to paint `hex` (7 when it is not a colour).
[[nodiscard]] int nearest_aci(std::string_view hex);
struct Insert {
  Vec3 ins{}, scale{};
  double rot = 0;
  Vec3 n{};
};
/// insertOf(m, k): a placement as an INSERT (OCS insertion point, scale, rotation, extrusion), or nothing when it is
/// not a turn + scale.
[[nodiscard]] std::optional<Insert> insert_of(const Mat4& m, double k);

/// exportDAE: `date_iso` is Date#toISOString() without the milliseconds ("2026-10-16T00:00:00Z").
[[nodiscard]] std::string export_dae(const Model& model, std::string_view name, std::string_view date_iso);
/// exportIFC: `timestamp` is toISOString().slice(0, 19) ("2026-10-16T00:00:00").
[[nodiscard]] std::string export_ifc(const Model& model, std::string_view name, std::string_view timestamp);
/// Every export of the model a model script ends with (tools/parity/m3d-io.mjs, the golden test): the script is run
/// (cc::m3d::run_script), its last model read back (Model::load), and exported in each format with a fixed date:
/// {name, glb, stl (base64), obj, objMtl ("model.mtl"), mtl, dxf | dxfErr, dwg | dwgErr, dae, ifc}.
[[nodiscard]] base::Value export_all(const base::Value& script);

/// A deterministic IFC GlobalId (22 characters) from a string (ifcGuid).
[[nodiscard]] std::string ifc_guid(std::string_view seed);
/// An ISO-10303-21 string literal (stepString).
[[nodiscard]] std::string step_string(std::string_view s);

}  // namespace cc::m3d::io
