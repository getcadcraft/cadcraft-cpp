// SPDX-License-Identifier: Apache-2.0
// cc::m3d — L2. The start of the ModelCraft kernel port (CadCraft 0.5 m3d/model.js, vec.js, earcut.js): definitions
// with vertices, edges, faces and instances, the model that holds them, its native JSON (toJSON), triangulation
// (faceTris) and meshDef (a group from loose polygons). Enough for CadCraft's 3D model of a drawing (model3d.js): a
// file's 3D faces in, the faces and CadCraft's own copy out when saving. Editing, push/pull, guides, dimensions,
// scenes and reading a saved model back come with M5 (docs/gaps.md).
//
// Units: meters, Z up; matrices are 16 numbers, column-major (vec.js).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::m3d {

using Vec3 = std::array<double, 3>;
using Mat4 = std::array<double, 16>;

inline constexpr double kEps = 1e-6;  // vec.js EPS: 1 µm
inline constexpr double kEps2 = kEps * kEps;

/// vec.js ident()
[[nodiscard]] constexpr Mat4 ident() noexcept {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
/// vec.js mmul(a, b)
[[nodiscard]] Mat4 mmul(const Mat4& a, const Mat4& b) noexcept;
/// vec.js apply(m, p)
[[nodiscard]] Vec3 apply(const Mat4& m, const Vec3& p) noexcept;
/// vec.js newell(pts): the unit Newell normal ([0, 0, 0] when degenerate).
[[nodiscard]] Vec3 newell(const std::vector<Vec3>& pts) noexcept;
/// vec.js polyArea3(pts): the polygon's area.
[[nodiscard]] double poly_area3(const std::vector<Vec3>& pts) noexcept;

/// A JS Map keyed by integer ids: insertion order, lookup by id.
template <class T>
class IdMap {
 public:
  [[nodiscard]] const T* get(std::int64_t id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &items_[it->second].second;
  }
  [[nodiscard]] T* get(std::int64_t id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &items_[it->second].second;
  }
  void set(std::int64_t id, T v) {
    if (T* have = get(id)) {
      *have = std::move(v);
      return;
    }
    index_.emplace(id, items_.size());
    items_.emplace_back(id, std::move(v));
  }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] const std::vector<std::pair<std::int64_t, T>>& items() const noexcept { return items_; }
  [[nodiscard]] std::vector<std::pair<std::int64_t, T>>& items() noexcept { return items_; }

 private:
  std::vector<std::pair<std::int64_t, T>> items_;
  std::unordered_map<std::int64_t, std::size_t> index_;
};

struct Edge {
  std::int64_t a = 0, b = 0;
  bool soft = false;
  std::string curve;
  bool hidden = false;
};
struct Face {
  std::vector<std::vector<std::int64_t>> loops;  // [0] outer (CCW from the front), the rest holes
  std::string color;
  bool hidden = false;
};
struct Inst {
  std::string def;
  Mat4 m = ident();
};

/// model.js Def: one definition (the root, a group or a component).
struct Def {
  std::string id, kind, name;
  IdMap<Vec3> verts;
  IdMap<Edge> edges;
  IdMap<Face> faces;
  IdMap<Inst> insts;
  std::int64_t next = 1;
  std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> edge_index;  // def._ek

  std::int64_t v_add(const Vec3& p);
  /// eAdd(def, a, b): the edge between a and b (made when missing); 0 when a == b.
  std::int64_t e_add(std::int64_t a, std::int64_t b);
  /// fAdd(def, loops, color): the face and its edges.
  std::int64_t f_add(std::vector<std::vector<std::int64_t>> loops, const std::string& color = {});
  // The vertex `v`; a face naming a missing vertex is a model bug (std::logic_error).
  [[nodiscard]] const Vec3& vert(std::int64_t v) const;
  [[nodiscard]] std::vector<Vec3> points(const std::vector<std::int64_t>& loop) const;
};

/// faceNormal(def, f)
[[nodiscard]] Vec3 face_normal(const Def& d, const Face& f);
/// faceTris(def, f): triangles as vertex-id triples, counter-clockwise from the front.
[[nodiscard]] std::vector<std::array<std::int64_t, 3>> face_tris(const Def& d, const Face& f);

/// model.js Model (the subset above). Definitions in insertion order; the root is "root".
class Model {
 public:
  Model() { reset(); }
  Model(const Model& o);
  Model& operator=(const Model& o);
  Model(Model&&) noexcept = default;
  Model& operator=(Model&&) noexcept = default;
  ~Model() = default;
  void reset();
  [[nodiscard]] Def& root() { return *defs_.front().second; }
  [[nodiscard]] const Def& root() const { return *defs_.front().second; }
  [[nodiscard]] const Def* def(std::string_view id) const;
  Def& new_def(std::string_view kind, std::string_view name);
  std::int64_t place(Def& in, const std::string& def_id, const Mat4& m);
  /// walk(cb): the root and every placed definition below it, with its world matrix (32 levels at most).
  void walk(const std::function<void(const Def&, const Mat4&)>& cb) const;
  /// model.js toJSON() (nothing but definitions here: guides, dimensions and scenes are empty).
  [[nodiscard]] base::Value to_json() const;
  /// The model has nothing in it (Model3D.empty).
  [[nodiscard]] bool empty() const;

  std::string units = "ft-in";
  std::int64_t next_id = 1;
  base::Value saved_view;  // the 3D view a file brought (model3d.js savedView), or null

 private:
  std::vector<std::pair<std::string, std::unique_ptr<Def>>> defs_;
  [[nodiscard]] std::string unique_name(std::string_view base) const;
};

/// meshDef(model, polys, name): a group of faces from polygons (meters), vertices welded at 1e-5 m.
Def& mesh_def(Model& model, const std::vector<std::vector<Vec3>>& polys, std::string_view name);

}  // namespace cc::m3d
