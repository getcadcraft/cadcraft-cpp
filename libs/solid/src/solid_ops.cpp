// SPDX-License-Identifier: Apache-2.0
// SolidOps on Manifold 3.5.4, as manifold-3d's bindings (bindings.js, helpers.cpp) call it.
#include "cc/solid/solid_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace cc::solid {

namespace {

manifold::MeshGL to_gl(const Mesh& m) {
  manifold::MeshGL g;
  g.numProp = m.num_prop;
  g.vertProperties = m.vert_properties;
  g.triVerts = m.tri_verts;
  g.mergeFromVert = m.merge_from_vert;
  g.mergeToVert = m.merge_to_vert;
  g.runIndex = m.run_index;
  g.runOriginalID = m.run_original_id;
  g.faceID = m.face_id;
  g.runTransform = m.run_transform;
  return g;
}

Mesh from_gl(manifold::MeshGL g) {
  Mesh m;
  m.num_prop = g.numProp;
  m.vert_properties = std::move(g.vertProperties);
  m.tri_verts = std::move(g.triVerts);
  m.merge_from_vert = std::move(g.mergeFromVert);
  m.merge_to_vert = std::move(g.mergeToVert);
  m.run_index = std::move(g.runIndex);
  m.run_original_id = std::move(g.runOriginalID);
  m.face_id = std::move(g.faceID);
  m.run_transform = std::move(g.runTransform);
  return m;
}

// Manifold throws only when memory runs out (and on programmer errors); every call is wrapped.
template <class F>
auto guarded(const F& f) -> base::Result<decltype(f())> {
  try {
    return f();
  } catch (const std::bad_alloc&) {
    return base::fail(base::Errc::too_big, "solid: out of memory");
  } catch (const std::exception& e) {
    return base::fail(base::Errc::invalid, std::string("solid: ") + e.what());
  } catch (...) {
    return base::fail(base::Errc::invalid, "solid: failed");
  }
}

constexpr std::size_t kMaxNumbers = std::size_t{1} << 27U;  // floats or indices in one mesh

// What Manifold's constructor and Merge do not check before they index (the wasm reads arbitrary memory or traps
// there; hostile input only): a mesh with no positions, triangle or merge indices past the vertices, runs that do
// not cover the triangles. Merge leaves such a mesh as it is; the constructor refuses it with the status it would
// reach (as the Rust fork's port answers them).
bool indices_ok(const Mesh& m) {
  if (m.num_prop < 3 || m.vert_properties.size() > kMaxNumbers || m.tri_verts.size() > kMaxNumbers) {
    return false;
  }
  const std::size_t nv = m.vert_properties.size() / m.num_prop;
  const auto in = [nv](std::uint32_t v) { return v < nv; };
  return std::ranges::all_of(m.tri_verts, in) && m.merge_from_vert.size() == m.merge_to_vert.size() &&
         std::ranges::all_of(m.merge_from_vert, in) && std::ranges::all_of(m.merge_to_vert, in);
}
bool runs_ok(const Mesh& m) {
  std::vector<std::uint32_t> run = m.run_index;
  const auto end = static_cast<std::uint32_t>(m.tri_verts.size());
  if (run.empty()) {
    run = {0, end};
  } else if (run.size() == m.run_original_id.size() || run.size() == 1) {
    run.push_back(end);
  }
  const std::size_t runs = std::max<std::size_t>(1, m.run_original_id.size());
  return run.size() >= runs + 1 && run.front() == 0 && run[runs] == end && std::ranges::is_sorted(run) &&
         run.back() <= end;
}

// Manifold pairs a mesh's halfedges by sorting them and matching the i-th with the (i + n/2)-th (CreateHalfedges):
// where the edges do not pair up (an open or non-manifold mesh) that reads unrelated or missing halfedges, and its
// check then follows pairs out of bounds (fuzz, 2026-10-21). Such a mesh is not a manifold: refused before. Vertices
// are compared after the merge vectors, as Manifold compares them.
bool edges_pair_up(const Mesh& m) {
  const std::size_t nv = m.vert_properties.size() / m.num_prop;
  std::vector<std::uint32_t> to(nv);
  for (std::size_t i = 0; i < nv; ++i) {
    to[i] = static_cast<std::uint32_t>(i);
  }
  for (std::size_t i = 0; i < m.merge_from_vert.size(); ++i) {
    to[m.merge_from_vert[i]] = m.merge_to_vert[i];
  }
  std::vector<std::uint64_t> fwd;
  std::vector<std::uint64_t> back;
  fwd.reserve(m.tri_verts.size());
  back.reserve(m.tri_verts.size());
  for (std::size_t t = 0; t + 2 < m.tri_verts.size(); t += 3) {
    const std::uint32_t v0 = to[m.tri_verts[t]];
    const std::uint32_t v1 = to[m.tri_verts[t + 1]];
    const std::uint32_t v2 = to[m.tri_verts[t + 2]];
    if (v0 == v1 || v1 == v2 || v2 == v0) {
      continue;  // Manifold drops a triangle with two corners on one vertex before it pairs the edges
    }
    for (std::size_t i = 0; i < 3; ++i) {
      const std::uint64_t a = to[m.tri_verts[t + i]];
      const std::uint64_t b = to[m.tri_verts[t + ((i + 1) % 3)]];
      fwd.push_back((a << 32U) | b);
      back.push_back((b << 32U) | a);
    }
  }
  std::ranges::sort(fwd);
  std::ranges::sort(back);
  return fwd == back;
}

// Impl(const MeshGLP&)'s checks before it indexes the runs (impl.h): when they all pass, the mesh goes on.
bool passes_early_checks(const Mesh& m) {
  const std::size_t nv = m.vert_properties.size() / m.num_prop;
  const auto finite = [](float x) { return std::isfinite(x); };
  return nv >= 4 && m.num_tri() >= 4 && m.num_prop >= 3 && m.merge_from_vert.size() == m.merge_to_vert.size() &&
         (m.run_transform.empty() || 12 * m.run_original_id.size() == m.run_transform.size()) &&
         (m.run_original_id.empty() || m.run_index.empty() || m.run_original_id.size() + 1 == m.run_index.size() ||
          m.run_original_id.size() == m.run_index.size()) &&
         (m.face_id.empty() || m.face_id.size() == m.num_tri()) && std::ranges::all_of(m.vert_properties, finite) &&
         std::ranges::all_of(m.run_transform, finite) &&
         std::ranges::all_of(m.merge_from_vert, [nv](std::uint32_t v) { return v < nv; }) &&
         std::ranges::all_of(m.merge_to_vert, [nv](std::uint32_t v) { return v < nv; });
}

base::Result<Solid> checked(manifold::Manifold&& m) {
  if (m.Status() != manifold::Manifold::Error::NoError) {
    return base::fail(base::Errc::invalid, status_name(m.Status()));
  }
  return Solid(std::move(m));
}

}  // namespace

std::string status_name(manifold::Manifold::Error e) {
  using E = manifold::Manifold::Error;
  switch (e) {
    case E::NoError: return "NoError";
    case E::NonFiniteVertex: return "NonFiniteVertex";
    case E::NotManifold: return "NotManifold";
    case E::VertexOutOfBounds: return "VertexOutOfBounds";
    case E::PropertiesWrongLength: return "PropertiesWrongLength";
    case E::MissingPositionProperties: return "MissingPositionProperties";
    case E::MergeVectorsDifferentLengths: return "MergeVectorsDifferentLengths";
    case E::MergeIndexOutOfBounds: return "MergeIndexOutOfBounds";
    case E::TransformWrongLength: return "TransformWrongLength";
    case E::RunIndexWrongLength: return "RunIndexWrongLength";
    case E::FaceIDWrongLength: return "FaceIDWrongLength";
    case E::InvalidConstruction: return "InvalidConstruction";
    case E::ResultTooLarge: return "ResultTooLarge";
    case E::InvalidTangents: return "InvalidTangents";
    case E::Cancelled: return "Cancelled";
  }
  return "UnknownError";
}

bool merge(Mesh& mesh) {
  if (!indices_ok(mesh)) {
    return false;
  }
  auto r = guarded([&] {
    manifold::MeshGL g = to_gl(mesh);
    const bool changed = g.Merge();
    if (changed) {
      mesh = from_gl(std::move(g));
    }
    return changed;
  });
  return r.value_or(false);
}

base::Result<Solid> Solid::of_mesh(const Mesh& mesh) {
  if (mesh.num_prop == 0) {  // Manifold divides by it first
    return base::fail(base::Errc::invalid, "MissingPositionProperties");
  }
  if (mesh.vert_properties.size() > kMaxNumbers || mesh.tri_verts.size() > kMaxNumbers) {
    return base::fail(base::Errc::too_big, "ResultTooLarge");
  }
  // Manifold's own checks come first, in its order; only a mesh that passes them reaches the unchecked parts
  if (passes_early_checks(mesh)) {
    if (!runs_ok(mesh)) {
      return base::fail(base::Errc::invalid, "RunIndexWrongLength");
    }
    if (indices_ok(mesh) && !edges_pair_up(mesh)) {
      return base::fail(base::Errc::invalid, "NotManifold");
    }
  }
  auto r = guarded([&] { return manifold::Manifold(to_gl(mesh)); });
  if (!r) {
    return std::unexpected(r.error());
  }
  return checked(std::move(*r));
}

base::Result<Solid> Solid::cube(std::array<double, 3> size, bool center) {
  return guarded([&] { return Solid(manifold::Manifold::Cube(manifold::vec3(size[0], size[1], size[2]), center)); });
}
base::Result<Solid> Solid::cylinder(double height, double r_low, double r_high, int segments, bool center) {
  return guarded([&] { return Solid(manifold::Manifold::Cylinder(height, r_low, r_high, segments, center)); });
}
base::Result<Solid> Solid::sphere(double r, int segments) {
  return guarded([&] { return Solid(manifold::Manifold::Sphere(r, segments)); });
}

base::Result<Solid> Solid::add(const Solid& o) const {
  return guarded([&] { return Solid(m_ + o.m_); });
}
base::Result<Solid> Solid::subtract(const Solid& o) const {
  return guarded([&] { return Solid(m_ - o.m_); });
}
base::Result<Solid> Solid::intersect(const Solid& o) const {
  return guarded([&] { return Solid(m_ ^ o.m_); });
}
base::Result<Solid> Solid::trim_by_plane(std::array<double, 3> n, double offset) const {
  return guarded([&] { return Solid(m_.TrimByPlane(manifold::vec3(n[0], n[1], n[2]), offset)); });
}
base::Result<std::pair<Solid, Solid>> Solid::split_by_plane(std::array<double, 3> n, double offset) const {
  return guarded([&] {
    auto [a, b] = m_.SplitByPlane(manifold::vec3(n[0], n[1], n[2]), offset);
    return std::pair<Solid, Solid>(Solid(std::move(a)), Solid(std::move(b)));
  });
}
base::Result<Solid> Solid::translate(std::array<double, 3> v) const {
  return guarded([&] { return Solid(m_.Translate(manifold::vec3(v[0], v[1], v[2]))); });
}
base::Result<Solid> Solid::transform(std::span<const double, 16> m) const {
  return guarded([&] {
    manifold::mat3x4 t;
    for (std::size_t col = 0; col < 4; ++col) {
      for (std::size_t row = 0; row < 3; ++row) {
        t[static_cast<int>(col)][static_cast<int>(row)] = m[col * 4 + row];
      }
    }
    return Solid(m_.Transform(t));
  });
}
base::Result<Mesh> Solid::mesh(int normal_idx) const {
  return guarded([&] { return from_gl(m_.GetMeshGL(normal_idx)); });
}

base::Result<Solid> union_of(std::span<const Solid> list) {
  return guarded([&] {
    std::vector<manifold::Manifold> v;
    v.reserve(list.size());
    for (const Solid& s : list) {
      v.push_back(s.raw());
    }
    return Solid(manifold::Manifold::BatchBoolean(v, manifold::OpType::Add));
  });
}
base::Result<Solid> intersection_of(std::span<const Solid> list) {
  return guarded([&] {
    std::vector<manifold::Manifold> v;
    v.reserve(list.size());
    for (const Solid& s : list) {
      v.push_back(s.raw());
    }
    return Solid(manifold::Manifold::BatchBoolean(v, manifold::OpType::Intersect));
  });
}

}  // namespace cc::solid
