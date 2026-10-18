// SPDX-License-Identifier: Apache-2.0
// m3d/model.js and vec.js from CadCraft 0.5: the parts cc::m3d has so far (model.hpp), with the JS's arithmetic.
#include "cc/m3d/model.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>

#include "cc/base/checked.hpp"
#include "cc/base/js_math.hpp"
#include "earcut.hpp"

namespace cc::m3d {

namespace {

double dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 sub(const Vec3& a, const Vec3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double len(const Vec3& a) {
  return base::js_hypot(a[0], a[1], a[2]);
}
double dist(const Vec3& a, const Vec3& b) {
  return base::js_hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
Vec3 norm(const Vec3& a) {
  const double l = len(a);
  return l > 1e-15 ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 0};
}
Vec3 newell_raw(const std::vector<Vec3>& pts) {
  double x = 0;
  double y = 0;
  double z = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Vec3& p = pts[i];
    const Vec3& q = pts[(i + 1) % pts.size()];
    x += (p[1] - q[1]) * (p[2] + q[2]);
    y += (p[2] - q[2]) * (p[0] + q[0]);
    z += (p[0] - q[0]) * (p[1] + q[1]);
  }
  return {x, y, z};
}

struct Frame {
  Vec3 u, v, n;
};
Frame frame(const Vec3& n) {
  const Vec3 u = std::abs(n[2]) > 0.999 ? Vec3{1, 0, 0} : norm(cross({0, 0, 1}, n));
  return {u, cross(n, u), n};
}

std::pair<std::int64_t, std::int64_t> edge_key(std::int64_t a, std::int64_t b) {
  return a < b ? std::pair{a, b} : std::pair{b, a};
}

}  // namespace

Mat4 mmul(const Mat4& a, const Mat4& b) noexcept {
  Mat4 r{};
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t rr = 0; rr < 4; ++rr) {
      double s = 0;
      for (std::size_t k = 0; k < 4; ++k) {
        s += a[k * 4 + rr] * b[c * 4 + k];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index) — < 16
      }
      r[c * 4 + rr] = s;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
  }
  return r;
}

Vec3 apply(const Mat4& m, const Vec3& p) noexcept {
  const double x = p[0];
  const double y = p[1];
  const double z = p[2];
  double w = m[3] * x + m[7] * y + m[11] * z + m[15];
  if (w == 0 || std::isnan(w)) {  // `|| 1`
    w = 1;
  }
  return {(m[0] * x + m[4] * y + m[8] * z + m[12]) / w, (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
          (m[2] * x + m[6] * y + m[10] * z + m[14]) / w};
}

Vec3 newell(const std::vector<Vec3>& pts) noexcept {
  return norm(newell_raw(pts));
}

double poly_area3(const std::vector<Vec3>& pts) noexcept {
  const Vec3 v = newell_raw(pts);
  return base::js_hypot(v[0], v[1], v[2]) / 2;
}

// ---------------------------------------------------------------- Def

std::int64_t Def::v_add(const Vec3& p) {
  const std::int64_t vid = next++;
  verts.set(vid, p);
  return vid;
}

std::int64_t Def::e_add(std::int64_t a, std::int64_t b) {
  if (a == b) {
    return 0;
  }
  const auto k = edge_key(a, b);
  if (const auto it = edge_index.find(k); it != edge_index.end()) {
    return it->second;
  }
  const std::int64_t eid = next++;
  edges.set(eid, Edge{.a = a, .b = b, .soft = false, .curve = {}, .hidden = false});
  edge_index.emplace(k, eid);
  return eid;
}

std::int64_t Def::f_add(std::vector<std::vector<std::int64_t>> loops, const std::string& color) {
  const std::int64_t fid = next++;
  for (const auto& l : loops) {
    for (std::size_t i = 0; i < l.size(); ++i) {
      (void)e_add(l[i], l[(i + 1) % l.size()]);
    }
  }
  faces.set(fid, Face{.loops = std::move(loops), .color = color, .hidden = false});
  return fid;
}

const Vec3& Def::vert(std::int64_t v) const {
  const Vec3* p = verts.get(v);
  if (p == nullptr) {
    throw std::logic_error("m3d: a face names a missing vertex");
  }
  return *p;
}

std::vector<Vec3> Def::points(const std::vector<std::int64_t>& loop) const {
  std::vector<Vec3> out;
  out.reserve(loop.size());
  for (const std::int64_t v : loop) {
    out.push_back(vert(v));
  }
  return out;
}

Vec3 face_normal(const Def& d, const Face& f) {
  return newell(d.points(f.loops.at(0)));
}

std::vector<std::array<std::int64_t, 3>> face_tris(const Def& d, const Face& f) {
  const Vec3 n = face_normal(d, f);
  if (len(n) == 0) {
    return {};
  }
  const Frame fr = frame(n);
  std::vector<double> flat;
  std::vector<std::int64_t> ids;
  std::vector<std::size_t> holes;
  for (std::size_t i = 0; i < f.loops.size(); ++i) {
    if (i != 0) {
      holes.push_back(ids.size());
    }
    for (const std::int64_t v : f.loops[i]) {
      const Vec3& p = d.vert(v);
      flat.push_back(dot(p, fr.u));
      flat.push_back(dot(p, fr.v));
      ids.push_back(v);
    }
  }
  const auto t = detail::earcut(flat, holes);
  std::vector<std::array<std::int64_t, 3>> out;
  for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
    std::array<std::int64_t, 3> tri = {ids.at(t[i]), ids.at(t[i + 1]), ids.at(t[i + 2])};
    const Vec3& a = d.vert(tri[0]);
    const Vec3& b = d.vert(tri[1]);
    const Vec3& c = d.vert(tri[2]);
    if (dot(cross(sub(b, a), sub(c, a)), n) < 0) {
      std::swap(tri[1], tri[2]);
    }
    out.push_back(tri);
  }
  return out;
}

// ---------------------------------------------------------------- Model

Model::Model(const Model& o) : units(o.units), next_id(o.next_id), saved_view(o.saved_view) {
  defs_.reserve(o.defs_.size());
  for (const auto& [k, d] : o.defs_) {
    defs_.emplace_back(k, std::make_unique<Def>(*d));
  }
}

Model& Model::operator=(const Model& o) {
  if (this != &o) {
    Model copy(o);
    *this = std::move(copy);
  }
  return *this;
}

void Model::reset() {
  defs_.clear();
  auto root = std::make_unique<Def>();
  root->id = "root";
  root->kind = "root";
  root->name = "Model";
  defs_.emplace_back("root", std::move(root));
  units = "ft-in";
  next_id = 1;
  saved_view = base::Value(nullptr);
}

const Def* Model::def(std::string_view id) const {
  for (const auto& [k, d] : defs_) {
    if (k == id) {
      return d.get();
    }
  }
  return nullptr;
}

std::string Model::unique_name(std::string_view base_name) const {
  std::set<std::string> names;
  for (const auto& [k, d] : defs_) {
    names.insert(d->name);
  }
  for (int i = 1;; ++i) {
    std::string n = std::string(base_name) + "#" + std::to_string(i);
    if (!names.contains(n)) {
      return n;
    }
  }
}

Def& Model::new_def(std::string_view kind, std::string_view name) {
  auto d = std::make_unique<Def>();
  d->id = "d" + std::to_string(next_id++);
  d->kind = std::string(kind);
  d->name = !name.empty() ? std::string(name) : unique_name(kind == "component" ? "Component" : "Group");
  Def& ref = *d;
  defs_.emplace_back(ref.id, std::move(d));
  return ref;
}

std::int64_t Model::place(Def& in, const std::string& def_id, const Mat4& m) {
  const std::int64_t iid = in.next++;
  in.insts.set(iid, Inst{.def = def_id, .m = m});
  return iid;
}

void Model::walk(const std::function<void(const Def&, const Mat4&)>& cb) const {
  // NOLINTNEXTLINE(misc-no-recursion) — 32 levels at most, as model.js
  const std::function<void(const Def&, const Mat4&, int)> go = [&](const Def& d, const Mat4& m, int depth) {
    if (depth > 32) {
      return;
    }
    cb(d, m);
    for (const auto& [iid, inst] : d.insts.items()) {
      if (const Def* c = def(inst.def)) {
        go(*c, mmul(m, inst.m), depth + 1);
      }
    }
  };
  go(root(), ident(), 0);
}

bool Model::empty() const {
  const Def& r = root();
  return r.faces.empty() && r.edges.empty() && r.insts.empty();
}

base::Value Model::to_json() const {
  using base::Array;
  using base::Value;
  Array defs;
  for (const auto& [k, d] : defs_) {
    Array verts;
    for (const auto& [id, p] : d->verts.items()) {
      verts.emplace_back(Array{Value(id), p[0], p[1], p[2]});
    }
    Array edges;
    for (const auto& [id, e] : d->edges.items()) {
      Array row = {Value(id), Value(e.a), Value(e.b)};
      if (e.hidden) {
        row.emplace_back(e.soft ? 1 : 0);
        row.emplace_back(e.curve.empty() ? Value(0) : Value(e.curve));
        row.emplace_back(1);
      } else if (!e.curve.empty()) {
        row.emplace_back(e.soft ? 1 : 0);
        row.emplace_back(e.curve);
      } else if (e.soft) {
        row.emplace_back(1);
      }
      edges.emplace_back(std::move(row));
    }
    Array faces;
    for (const auto& [id, f] : d->faces.items()) {
      Array loops;
      for (const auto& l : f.loops) {
        Array lv;
        for (const std::int64_t v : l) {
          lv.emplace_back(v);
        }
        loops.emplace_back(std::move(lv));
      }
      Value o = base::make_object({{"id", Value(id)}, {"loops", std::move(loops)}});
      if (!f.color.empty()) {
        o.set("color", f.color);
      }
      if (f.hidden) {
        o.set("hidden", true);
      }
      faces.push_back(std::move(o));
    }
    Array insts;
    for (const auto& [id, inst] : d->insts.items()) {
      Array m;
      for (const double x : inst.m) {
        m.emplace_back(x);
      }
      insts.push_back(base::make_object({{"id", Value(id)}, {"def", inst.def}, {"m", std::move(m)}}));
    }
    defs.push_back(base::make_object({{"id", d->id},
                                      {"kind", d->kind},
                                      {"name", d->name},
                                      {"next", Value(d->next)},
                                      {"verts", std::move(verts)},
                                      {"edges", std::move(edges)},
                                      {"faces", std::move(faces)},
                                      {"insts", std::move(insts)}}));
  }
  return base::make_object({{"format", "modelcraft"},
                            {"version", 1},
                            {"units", units},
                            {"nextId", Value(next_id)},
                            {"defs", std::move(defs)},
                            {"guides", Array{}},
                            {"dims", Array{}},
                            {"scenes", Array{}}});
}

// ---------------------------------------------------------------- meshDef

Def& mesh_def(Model& model, const std::vector<std::vector<Vec3>>& polys, std::string_view name) {
  Def& d = model.new_def("group", name);
  std::map<Vec3, std::int64_t> grid;  // Math.round(x * 1e5) per axis
  const auto vid = [&](const Vec3& p) {
    Vec3 k{};
    for (std::size_t i = 0; i < 3; ++i) {
      const double r = base::js_round(p.at(i) * 1e5);
      k.at(i) = r == 0 ? 0.0 : r;  // String(-0) is "0"
    }
    const auto it = grid.find(k);
    if (it != grid.end()) {
      return it->second;
    }
    const std::int64_t id = d.v_add(p);
    grid.emplace(k, id);
    return id;
  };
  const auto ring = [&](const std::vector<Vec3>& pts) -> std::optional<std::vector<std::int64_t>> {
    std::vector<std::int64_t> loop;
    for (const Vec3& p : pts) {
      const std::int64_t v = vid(p);
      if (loop.empty() || loop.back() != v) {
        loop.push_back(v);
      }
    }
    while (loop.size() > 1 && loop.front() == loop.back()) {
      loop.pop_back();
    }
    if (loop.size() < 3 || std::set<std::int64_t>(loop.begin(), loop.end()).size() != loop.size()) {
      return std::nullopt;
    }
    if (poly_area3(d.points(loop)) <= kEps2) {
      return std::nullopt;
    }
    return loop;
  };
  for (const auto& poly : polys) {
    const auto outer = ring(poly);
    if (!outer) {
      continue;
    }
    const std::vector<Vec3> P = d.points(*outer);
    const Vec3 n = newell(P);
    double size = 0;
    for (const Vec3& p : P) {
      size = std::max(size, dist(p, P[0]));
    }
    const double tol = std::max(kEps, size * 1e-6);
    const bool bent = std::ranges::any_of(P, [&](const Vec3& p) { return std::abs(dot(n, sub(p, P[0]))) > tol; });
    if (bent) {  // not flat: a fan of triangles
      for (std::size_t i = 1; i + 1 < outer->size(); ++i) {
        std::vector<std::int64_t> t = {(*outer)[0], (*outer)[i], (*outer)[i + 1]};
        if (poly_area3(d.points(t)) > kEps2) {
          (void)d.f_add({std::move(t)});
        }
      }
      continue;
    }
    (void)d.f_add({*outer});
  }
  // edges between two faces facing the same way are soft
  std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::int64_t>> ef;
  for (const auto& [fid, f] : d.faces.items()) {
    for (const auto& l : f.loops) {
      for (std::size_t i = 0; i < l.size(); ++i) {
        ef[edge_key(l[i], l[(i + 1) % l.size()])].push_back(fid);
      }
    }
  }
  for (auto& [eid, e] : d.edges.items()) {
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it == ef.end() || it->second.size() != 2) {
      continue;
    }
    const Face* f0 = d.faces.get(it->second[0]);
    const Face* f1 = d.faces.get(it->second[1]);
    if (dot(face_normal(d, *f0), face_normal(d, *f1)) > 1 - 1e-6) {
      e.soft = true;
    }
  }
  return d;
}

}  // namespace cc::m3d
