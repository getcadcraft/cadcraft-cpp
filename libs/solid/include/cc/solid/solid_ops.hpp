// SPDX-License-Identifier: Apache-2.0
// cc::solid — L2. `SolidOps`: the mesh booleans of the CadCraft apps, on Manifold 3.5.4 (cmake/Manifold.cmake), the
// library CadCraft 0.5 runs as WebAssembly (manifold-3d 3.5.4). Built to compute what the WebAssembly computes (same
// commit, sequential, no FMA; tools/parity/manifold.mjs compares every float bit for bit), so a port of a 0.5 file that
// calls `M.Manifold…` calls the same operation here and gets the same mesh.
//
// The JS API and this one, side by side (manifold-3d's bindings.js):
//   new M.Mesh({numProp, vertProperties, triVerts}); mesh.merge()   Mesh{…}; merge(mesh)
//   new M.Manifold(mesh)  (throws on a bad status)                   Solid::of_mesh(mesh) → Result (the status' name)
//   M.Manifold.union([a, b…]) / intersection([…])                    union_of(span) / intersection_of(span)
//   a.add(b) / a.subtract(b) / a.intersect(b)                        a.add(b) / a.subtract(b) / a.intersect(b)
//   a.trimByPlane([x, y, z], off) / a.splitByPlane(…)                a.trim_by_plane(n, off) / a.split_by_plane(n, off)
//   a.translate / a.transform(m4 column-major) / a.getMesh()         translate / transform / mesh()
//   M.Manifold.cube / cylinder / sphere                              Solid::cube / cylinder / sphere
//   a.isEmpty() / a.volume() / a.status() / a.numTri()               is_empty / volume / status / num_tri
// Anything else is reachable through `raw()` (the manifold::Manifold); manifold throws only on allocation failure,
// which every call here turns into an Error. Nothing from a file reaches Manifold without of_mesh's checks.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "manifold/manifold.h"

namespace cc::solid {

/// manifold-3d's Mesh with the fields the apps fill: positions (numProp floats each, x y z first) and triangles.
struct Mesh {
  std::uint32_t num_prop = 3;
  std::vector<float> vert_properties;
  std::vector<std::uint32_t> tri_verts;
  std::vector<std::uint32_t> merge_from_vert, merge_to_vert;
  std::vector<std::uint32_t> run_index, run_original_id, face_id;
  std::vector<float> run_transform;

  [[nodiscard]] std::size_t num_vert() const { return num_prop == 0 ? 0 : vert_properties.size() / num_prop; }
  [[nodiscard]] std::size_t num_tri() const { return tri_verts.size() / 3; }
};

/// mesh.merge(): finds the open edges' coincident vertices (MeshGL::Merge). → whether it changed anything.
bool merge(Mesh& mesh);

/// The JS name of a manifold status ("NoError", "NotManifold", …: bindings.cpp's enum).
[[nodiscard]] std::string status_name(manifold::Manifold::Error e);

class Solid {
 public:
  Solid() = default;
  explicit Solid(manifold::Manifold m) : m_(std::move(m)) {}

  /// new M.Manifold(mesh): refused (the status' name as the message) when the mesh is not a closed manifold.
  [[nodiscard]] static base::Result<Solid> of_mesh(const Mesh& mesh);

  /// M.Manifold.cube(size, center) · cylinder(h, r1, r2 = -1, segments = 0, center) · sphere(r, segments = 0)
  [[nodiscard]] static base::Result<Solid> cube(std::array<double, 3> size, bool center);
  [[nodiscard]] static base::Result<Solid> cylinder(double height, double r_low, double r_high, int segments,
                                                    bool center);
  [[nodiscard]] static base::Result<Solid> sphere(double r, int segments);

  [[nodiscard]] base::Result<Solid> add(const Solid& o) const;
  [[nodiscard]] base::Result<Solid> subtract(const Solid& o) const;
  [[nodiscard]] base::Result<Solid> intersect(const Solid& o) const;
  [[nodiscard]] base::Result<Solid> trim_by_plane(std::array<double, 3> normal, double offset) const;
  [[nodiscard]] base::Result<std::pair<Solid, Solid>> split_by_plane(std::array<double, 3> normal, double offset) const;
  [[nodiscard]] base::Result<Solid> translate(std::array<double, 3> v) const;
  /// m: 16 numbers, column-major (the last row is ignored, as bindings.js does).
  [[nodiscard]] base::Result<Solid> transform(std::span<const double, 16> m) const;
  /// getMesh(normalIdx = -1)
  [[nodiscard]] base::Result<Mesh> mesh(int normal_idx = -1) const;

  [[nodiscard]] bool is_empty() const { return m_.IsEmpty(); }
  [[nodiscard]] double volume() const { return m_.Volume(); }
  [[nodiscard]] std::size_t num_tri() const { return m_.NumTri(); }
  [[nodiscard]] std::size_t num_vert() const { return m_.NumVert(); }
  [[nodiscard]] std::string status() const { return status_name(m_.Status()); }
  [[nodiscard]] const manifold::Manifold& raw() const noexcept { return m_; }

 private:
  manifold::Manifold m_;
};

/// M.Manifold.union(list) / M.Manifold.intersection(list) (BatchBoolean).
[[nodiscard]] base::Result<Solid> union_of(std::span<const Solid> list);
[[nodiscard]] base::Result<Solid> intersection_of(std::span<const Solid> list);

}  // namespace cc::solid
