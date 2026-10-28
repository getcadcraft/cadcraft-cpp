// SPDX-License-Identifier: Apache-2.0
// faces.js (planar faces of a triangle mesh and their boundary loops) and meshfaces.js (the mesh kernel's named faces
// and edges: each triangle matched to a surface of the tool that made it, brep.js's face and edge tables).
// Positions are float32, as the kernel's Float32Array; the arithmetic is the JS's, in its order.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/partcraft/vec.hpp"

namespace cc::partcraft::meshfaces {

/// { positions (x y z per vertex), indices (3 per triangle), triFace? }.
struct Mesh {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  std::optional<std::vector<std::int32_t>> tri_face;
};

/// The driver's meshOf: Float32Array / Uint32Array / Int32Array.from of a {positions, indices, triFace?} value.
/// Throws ops-style (std::runtime_error) on a triangle count that is not whole, as `new Int32Array(n / 3)` does.
[[nodiscard]] Mesh mesh_of(const base::Value& m);
[[nodiscard]] base::Value mesh_value(const Mesh& m);

// ---- faces.js

struct Group {
  std::vector<std::size_t> tris;
  vec::V3 n{};
  double d = 0;
  std::optional<std::int32_t> face;  // faceGroups: the B-rep face (undefined past triFace's end)
  bool has_face = false;
};
struct Groups {
  std::vector<std::int32_t> face_of;
  std::vector<Group> groups;
};
[[nodiscard]] Groups face_groups(const Mesh& mesh);
[[nodiscard]] Groups planar_groups_mesh(const Mesh& mesh, double ang_tol = 1e-5);
[[nodiscard]] Groups planar_groups(const Mesh& mesh, double ang_tol = 1e-5);
[[nodiscard]] std::vector<std::vector<std::uint32_t>> boundary_loops(const Mesh& mesh,
                                                                     const std::vector<std::size_t>& tris);
using Loop2 = std::vector<std::array<double, 2>>;
struct Loops2D {
  vec::Frame frame;
  std::vector<Loop2> loops;
  double area = 0;
};
[[nodiscard]] Loops2D face_loops_2d(const Mesh& mesh, const Group& group);
[[nodiscard]] Loop2 drop_collinear(const Loop2& loop, double sin_tol = 1e-6);
[[nodiscard]] base::Value groups_value(const Groups& g, bool face_first);

// ---- meshfaces.js: surfaces are JS-shaped values ({name, type, …}), as the kernel's tools carry them

[[nodiscard]] base::Value plane(const std::string& name, const vec::V3& n, const vec::V3& o);
[[nodiscard]] base::Value cylinder(const std::string& name, const vec::V3& o, const vec::V3& d, double r);
[[nodiscard]] base::Value cone(const std::string& name, const vec::V3& apex, const vec::V3& d, double k);
[[nodiscard]] base::Value sphere(const std::string& name, const vec::V3& c, double r);
[[nodiscard]] base::Value torus(const std::string& name, const vec::V3& c, const vec::V3& d, double R, double r);
[[nodiscard]] base::Value other(const std::string& name);
[[nodiscard]] base::Value thread(const std::string& name, const vec::V3& o, const vec::V3& d, double r0, double r1,
                                 double t0, double t1);
[[nodiscard]] base::Value within(const base::Value& s, const vec::V3& o, const vec::V3& d, double r);
[[nodiscard]] base::Value span(const base::Value& s, const vec::V3& a, const vec::V3& b);

/// moveSurfs(surfs, M4, suffix): null for null.
[[nodiscard]] base::Value move_surfs(const base::Value& surfs, const vec::M4& m, const std::string& suffix);

struct Named {
  std::vector<std::int32_t> tri_face;
  base::Value faces;  // array of face records
  base::Value edges;  // array of edge records
  std::vector<float> edge_segs;
  std::vector<std::int32_t> seg_edge;
};
/// namedFaces(mesh, triTool, toolSurfs, {segments}). triTool past its end is "no tool" (undefined in the JS).
[[nodiscard]] Named named_faces(const Mesh& mesh, const std::vector<std::int32_t>& tri_tool,
                                const base::Value& tool_surfs, double segments = 64);
[[nodiscard]] base::Value named_value(const Named& n);

}  // namespace cc::partcraft::meshfaces
