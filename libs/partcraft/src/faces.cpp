// SPDX-License-Identifier: Apache-2.0
// faces.js: planar faces of a triangle mesh and their boundary loops, and the driver's meshOf.
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js.hpp"
#include "cc/partcraft/meshfaces.hpp"

namespace cc::partcraft::meshfaces {
namespace {

using base::Value;
using vec::V3;
namespace V = vec;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// ToUint32 / ToInt32 of a JS number (what Uint32Array.from / Int32Array.from store).
std::uint32_t to_uint32(double x) {
  if (!std::isfinite(x)) {
    return 0;
  }
  const double t = std::trunc(x);
  const double m = std::fmod(t, 4294967296.0);
  return static_cast<std::uint32_t>(m < 0 ? m + 4294967296.0 : m);
}
std::int32_t to_int32(double x) {
  return static_cast<std::int32_t>(to_uint32(x));
}

/// pos[i] (undefined, so NaN, past the end).
double at(const std::vector<float>& pos, double i) {
  if (!(i >= 0) || i >= static_cast<double>(pos.size())) {
    return kNaN;
  }
  return static_cast<double>(pos[static_cast<std::size_t>(i)]);
}

struct TriN {
  V3 n{}, a{};
  double area2 = 0;
};
TriN tri_normal(const Mesh& m, std::size_t t) {
  const auto p = [&](std::size_t k) {
    const double v = static_cast<double>(m.indices[t * 3 + k]) * 3;
    return V3{at(m.positions, v), at(m.positions, v + 1), at(m.positions, v + 2)};
  };
  const V3 a = p(0);
  const V3 b = p(1);
  const V3 c = p(2);
  const V3 n = V::cross(V::sub(b, a), V::sub(c, a));
  const double l = V::len(n);
  return {l > 0 ? V::mul(n, 1 / l) : V3{0, 0, 0}, a, l};
}

std::size_t tri_count(const Mesh& m) {
  return m.indices.size() / 3;
}

double js_max(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a > b ? a : b;
}

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
  return a < b ? (std::uint64_t{a} << 32) | b : (std::uint64_t{b} << 32) | a;
}

}  // namespace

Mesh mesh_of(const Value& m) {
  Mesh out;
  const Value pos = m.get("positions");
  const Value idx = m.get("indices");
  if (pos.is_array()) {
    out.positions.reserve(pos.as_array().size());
    for (const Value& x : pos.as_array()) {
      out.positions.push_back(static_cast<float>(base::js_to_number(x)));
    }
  }
  if (idx.is_array()) {
    out.indices.reserve(idx.as_array().size());
    for (const Value& x : idx.as_array()) {
      out.indices.push_back(to_uint32(base::js_to_number(x)));
    }
  }
  const Value tf = m.get("triFace");
  if (tf.truthy()) {
    std::vector<std::int32_t> f;
    if (tf.is_array()) {
      for (const Value& x : tf.as_array()) {
        f.push_back(to_int32(base::js_to_number(x)));
      }
    }
    out.tri_face = std::move(f);
  }
  if (out.indices.size() % 3 != 0) {
    std::string n;
    base::append_number(n, static_cast<double>(out.indices.size()) / 3);
    throw std::runtime_error("Invalid typed array length: " + n);
  }
  return out;
}

Value mesh_value(const Mesh& m) {
  base::Array p;
  p.reserve(m.positions.size());
  for (const float x : m.positions) {
    p.emplace_back(static_cast<double>(x));
  }
  base::Array i;
  i.reserve(m.indices.size());
  for (const std::uint32_t x : m.indices) {
    i.emplace_back(static_cast<double>(x));
  }
  Value out = base::make_object({{"positions", std::move(p)}, {"indices", std::move(i)}});
  if (m.tri_face) {
    base::Array f;
    for (const std::int32_t x : *m.tri_face) {
      f.emplace_back(static_cast<double>(x));
    }
    out.set("triFace", std::move(f));
  }
  return out;
}

Groups face_groups(const Mesh& mesh) {
  const std::size_t nt = tri_count(mesh);
  Groups out;
  out.face_of.assign(nt, 0);
  // the key: the triangle's face, or "undefined" past triFace's end
  std::unordered_map<std::int64_t, std::size_t> by_face;
  constexpr std::int64_t kUndef = std::int64_t{1} << 40;
  static const std::vector<std::int32_t> kNoFaces;
  const auto& tf = mesh.tri_face ? *mesh.tri_face : kNoFaces;
  for (std::size_t t = 0; t < nt; ++t) {
    const bool has = t < tf.size();
    const std::int64_t key = has ? tf[t] : kUndef;
    auto it = by_face.find(key);
    std::size_t g = 0;
    if (it == by_face.end()) {
      const TriN q = tri_normal(mesh, t);
      g = out.groups.size();
      by_face.emplace(key, g);
      Group gr;
      gr.n = q.n;
      gr.d = V::dot(q.n, q.a);
      gr.has_face = true;
      if (has) {
        gr.face = tf[t];
      }
      out.groups.push_back(std::move(gr));
    } else {
      g = it->second;
    }
    out.groups[g].tris.push_back(t);
    out.face_of[t] = static_cast<std::int32_t>(g);
  }
  return out;
}

Groups planar_groups_mesh(const Mesh& mesh, double ang_tol) {
  const std::size_t nt = tri_count(mesh);
  double size = 1;
  for (const float x : mesh.positions) {
    size = js_max(size, std::abs(static_cast<double>(x)));
  }
  const double d_tol = 1e-5 * size;
  std::vector<std::pair<V3, double>> info;
  info.reserve(nt);
  for (std::size_t t = 0; t < nt; ++t) {
    const TriN q = tri_normal(mesh, t);
    info.emplace_back(q.n, V::dot(q.n, q.a));
  }
  std::vector<std::size_t> parent(nt);
  for (std::size_t i = 0; i < nt; ++i) {
    parent[i] = i;
  }
  const auto find = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  std::unordered_map<std::uint64_t, std::size_t> edges;
  for (std::size_t t = 0; t < nt; ++t) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t a = mesh.indices[t * 3 + k];
      const std::uint32_t b = mesh.indices[t * 3 + (k + 1) % 3];
      const auto [it, fresh] = edges.emplace(edge_key(a, b), t);
      if (fresh) {
        continue;
      }
      const auto& A = info[t];
      const auto& B = info[it->second];
      if (V::dot(A.first, B.first) > 1 - ang_tol && std::abs(A.second - B.second) < d_tol) {
        parent[find(t)] = find(it->second);
      }
    }
  }
  Groups out;
  out.face_of.assign(nt, 0);
  std::unordered_map<std::size_t, std::size_t> map;
  for (std::size_t t = 0; t < nt; ++t) {
    const std::size_t r = find(t);
    auto it = map.find(r);
    std::size_t g = 0;
    if (it == map.end()) {
      g = out.groups.size();
      map.emplace(r, g);
      Group gr;
      gr.n = info[t].first;
      gr.d = info[t].second;
      out.groups.push_back(std::move(gr));
    } else {
      g = it->second;
    }
    out.groups[g].tris.push_back(t);
    out.face_of[t] = static_cast<std::int32_t>(g);
  }
  return out;
}

Groups planar_groups(const Mesh& mesh, double ang_tol) {
  if (mesh.tri_face) {
    return face_groups(mesh);
  }
  return planar_groups_mesh(mesh, ang_tol);
}

std::vector<std::vector<std::uint32_t>> boundary_loops(const Mesh& mesh, const std::vector<std::size_t>& tris) {
  // dir: a Map in insertion order, with deletions (a re-added key goes to the end)
  struct Entry {
    std::uint32_t a, b;
    bool alive;
  };
  std::vector<Entry> entries;
  std::unordered_map<std::uint64_t, std::size_t> dir;
  const auto key = [](std::uint32_t a, std::uint32_t b) { return (std::uint64_t{a} << 32) | b; };
  for (const std::size_t t : tris) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t a = mesh.indices[t * 3 + k];
      const std::uint32_t b = mesh.indices[t * 3 + (k + 1) % 3];
      if (auto it = dir.find(key(b, a)); it != dir.end()) {
        entries[it->second].alive = false;
        dir.erase(it);
      } else if (!dir.contains(key(a, b))) {
        dir.emplace(key(a, b), entries.size());
        entries.push_back({a, b, true});
      }
    }
  }
  std::vector<std::uint32_t> order;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> next;
  for (const Entry& e : entries) {
    if (!e.alive) {
      continue;
    }
    auto [it, fresh] = next.try_emplace(e.a);
    if (fresh) {
      order.push_back(e.a);
    }
    it->second.push_back(e.b);
  }
  std::vector<std::vector<std::uint32_t>> loops;
  for (const std::uint32_t start : order) {
    while (!next[start].empty()) {
      std::vector<std::uint32_t> loop{start};
      std::uint32_t cur = next[start].back();
      next[start].pop_back();
      int guard = 0;
      while (cur != start && guard++ < 1000000) {
        loop.push_back(cur);
        auto it = next.find(cur);
        if (it == next.end() || it->second.empty()) {
          break;
        }
        cur = it->second.back();
        it->second.pop_back();
      }
      if (loop.size() >= 3) {
        loops.push_back(std::move(loop));
      }
    }
  }
  return loops;
}

Loops2D face_loops_2d(const Mesh& mesh, const Group& group) {
  Loops2D out;
  out.frame = V::frame_for(group.n, V::mul(group.n, group.d));
  std::vector<Loop2> loops;
  for (const auto& l : boundary_loops(mesh, group.tris)) {
    Loop2 q;
    q.reserve(l.size());
    for (const std::uint32_t v : l) {
      const double i = static_cast<double>(v) * 3;
      q.push_back(
          V::to_plane(out.frame, {at(mesh.positions, i), at(mesh.positions, i + 1), at(mesh.positions, i + 2)}));
    }
    loops.push_back(std::move(q));
  }
  const auto area = [](const Loop2& l) {
    double s = 0;
    for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) {
      s += l[j][0] * l[i][1] - l[i][0] * l[j][1];
    }
    return s / 2;
  };
  appkit::js_sort(loops, [&](const Loop2& a, const Loop2& b) { return std::abs(area(b)) - std::abs(area(a)); });
  for (const Loop2& l : loops) {
    out.area += area(l);
  }
  for (const Loop2& l : loops) {
    out.loops.push_back(drop_collinear(l));
  }
  return out;
}

Loop2 drop_collinear(const Loop2& loop, double sin_tol) {
  if (loop.size() <= 3) {
    return loop;
  }
  Loop2 out;
  const std::size_t n = loop.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto& a = loop[(i + n - 1) % n];
    const auto& b = loop[i];
    const auto& c = loop[(i + 1) % n];
    const std::array<double, 2> u{b[0] - a[0], b[1] - a[1]};
    const std::array<double, 2> v{c[0] - b[0], c[1] - b[1]};
    const double l = base::js_hypot(u[0], u[1]) * base::js_hypot(v[0], v[1]);
    const bool straight = l > 0 && std::abs(u[0] * v[1] - u[1] * v[0]) / l < sin_tol && u[0] * v[0] + u[1] * v[1] > 0;
    if (!straight) {
      out.push_back(b);
    }
  }
  return out.size() >= 3 ? out : loop;
}

Value groups_value(const Groups& g, bool face_first) {
  base::Array groups;
  for (const Group& gr : g.groups) {
    base::Array tris;
    tris.reserve(gr.tris.size());
    for (const std::size_t t : gr.tris) {
      tris.emplace_back(static_cast<double>(t));
    }
    Value o = base::make_object({{"tris", std::move(tris)}, {"n", V::value(gr.n)}, {"d", gr.d}});
    if (const auto& f = gr.face) {
      o.set("face", static_cast<double>(*f));
    }
    groups.emplace_back(std::move(o));
  }
  base::Array face_of;
  face_of.reserve(g.face_of.size());
  for (const std::int32_t f : g.face_of) {
    face_of.emplace_back(static_cast<double>(f));
  }
  if (face_first) {
    return base::make_object({{"faceOf", std::move(face_of)}, {"groups", std::move(groups)}});
  }
  return base::make_object({{"groups", std::move(groups)}, {"faceOf", std::move(face_of)}});
}

}  // namespace cc::partcraft::meshfaces
