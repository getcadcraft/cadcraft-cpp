// SPDX-License-Identifier: Apache-2.0
// model.js, definitions: vertices, edges, faces, re-facing, clean-up, push/pull — in the JS's order of operations and
// iteration (a face re-added after it was deleted goes to the end of the table, as in a JS Map).
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/m3d/model.hpp"
#include "earcut.hpp"
#include "geom_internal.hpp"

namespace cc::m3d {

// ---------------------------------------------------------------- Def

std::unordered_map<std::uint64_t, Id>& Def::ek() const {
  if (!ek_) {
    ek_.emplace();
    for (const auto& [eid, e] : edges) {
      ek_->emplace(edge_key(e.a, e.b), eid);
    }
  }
  return *ek_;
}

std::optional<Id> Def::edge_between(Id a, Id b) const {
  const auto& k = ek();
  const auto it = k.find(edge_key(a, b));
  return it == k.end() ? std::nullopt : std::optional<Id>(it->second);
}

const Vec3& Def::vert(Id v) const {
  const Vec3* p = verts.get(v);
  if (p == nullptr) {
    throw std::logic_error("m3d: a face or edge names a missing vertex");
  }
  return *p;
}

std::vector<Vec3> Def::points(const Loop& loop) const {
  std::vector<Vec3> out;
  out.reserve(loop.size());
  for (const Id v : loop) {
    out.push_back(vert(v));
  }
  return out;
}

void Def::compact() {
  verts.compact();
  edges.compact();
  faces.compact();
  insts.compact();
  splines.compact();
}

Id v_add(Def& d, const Vec3& p) {
  const Id id = d.next++;
  d.verts.set(id, p);
  return id;
}

std::optional<Id> find_vert(const Def& d, const Vec3& p, double tol) {
  for (const auto& [id, q] : d.verts) {
    if (near(p, q, tol)) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<Id> e_add(Def& d, Id a, Id b, bool soft, std::string_view curve, bool hidden) {
  if (a == b) {
    return std::nullopt;
  }
  const std::uint64_t k = edge_key(a, b);
  auto& index = d.ek();
  if (const auto it = index.find(k); it != index.end()) {
    return it->second;
  }
  const Id id = d.next++;
  d.edges.set(id, Edge{.a = a, .b = b, .soft = soft, .curve = std::string(curve), .hidden = hidden});
  index.emplace(k, id);
  return id;
}

void e_remove(Def& d, Id id) {
  const Edge* e = d.edges.get(id);
  if (e == nullptr) {
    return;
  }
  const std::uint64_t k = edge_key(e->a, e->b);
  d.edges.erase(id);
  d.ek().erase(k);
}

Id f_add(Def& d, std::vector<Loop> loops, std::string_view color, std::optional<Id> id, bool hidden) {
  const Id fid = id ? *id : d.next++;
  for (const Loop& l : loops) {
    for (std::size_t i = 0; i < l.size(); ++i) {
      (void)e_add(d, l[i], l[(i + 1) % l.size()]);
    }
  }
  d.faces.set(fid, Face{.loops = std::move(loops), .color = std::string(color), .hidden = hidden});
  return fid;
}

std::vector<std::vector<Vec3>> f_pts(const Def& d, const Face& f) {
  std::vector<std::vector<Vec3>> out;
  out.reserve(f.loops.size());
  for (const Loop& l : f.loops) {
    out.push_back(d.points(l));
  }
  return out;
}

Vec3 face_normal(const Def& d, const Face& f) {
  return newell(d.points(f.loops.at(0)));
}

Plane face_plane(const Def& d, const Face& f) {
  const Vec3 n = face_normal(d, f);
  return {n, dot(n, d.vert(f.loops.at(0).at(0)))};
}

double face_area(const Def& d, const Face& f) {
  double a = poly_area3(d.points(f.loops.at(0)));
  for (std::size_t i = 1; i < f.loops.size(); ++i) {
    a -= poly_area3(d.points(f.loops[i]));
  }
  return a;
}

namespace detail {

std::vector<std::pair<Id, Id>> loop_segs(const Face& f) {
  std::vector<std::pair<Id, Id>> out;
  for (const Loop& l : f.loops) {
    for (std::size_t i = 0; i < l.size(); ++i) {
      out.emplace_back(l[i], l[(i + 1) % l.size()]);
    }
  }
  return out;
}

std::unordered_map<Id, OrderedSet<Id>> vert_faces(const Def& d) {
  std::unordered_map<Id, OrderedSet<Id>> m;
  for (const auto& [fid, f] : d.faces) {
    for (const Loop& l : f.loops) {
      for (const Id v : l) {
        m[v].add(fid);
      }
    }
  }
  return m;
}

double area2(std::span<const Vec2> poly) {
  double s = 0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const Vec2& p = poly[i];
    const Vec2& q = poly[(i + 1) % poly.size()];
    s += p[0] * q[1] - q[0] * p[1];
  }
  return s / 2;
}

bool in_poly(const Vec2& pt, std::span<const Vec2> poly) {
  bool inside = false;
  const std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2& a = poly[i];
    const Vec2& b = poly[j];
    if ((a[1] > pt[1]) != (b[1] > pt[1]) && pt[0] < (b[0] - a[0]) * (pt[1] - a[1]) / (b[1] - a[1]) + a[0]) {
      inside = !inside;
    }
  }
  return inside;
}

bool in_region(const Vec2& pt, const std::vector<std::vector<Vec2>>& loops) {
  if (!in_poly(pt, loops.at(0))) {
    return false;
  }
  for (std::size_t i = 1; i < loops.size(); ++i) {
    if (in_poly(pt, loops[i])) {
      return false;
    }
  }
  return true;
}

std::optional<Vec2> sample_point(const std::vector<std::vector<Vec2>>& loops) {
  std::vector<double> flat;
  std::vector<std::size_t> holes;
  std::vector<Vec2> pts;
  for (std::size_t i = 0; i < loops.size(); ++i) {
    if (i != 0) {
      holes.push_back(pts.size());
    }
    for (const Vec2& p : loops[i]) {
      flat.push_back(p[0]);
      flat.push_back(p[1]);
      pts.push_back(p);
    }
  }
  const auto t = earcut(flat, holes);
  Vec2 best{};
  double ba = -1;
  for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
    const Vec2& a = pts.at(t[i]);
    const Vec2& b = pts.at(t[i + 1]);
    const Vec2& c = pts.at(t[i + 2]);
    const double ar = std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
    if (ar > ba) {
      ba = ar;
      best = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3};
    }
  }
  return ba > 1e-14 ? std::optional<Vec2>(best) : std::nullopt;
}

std::vector<std::vector<Vec2>> loops2(const Def& d, const Face& f, const Frame& fr) {
  std::vector<std::vector<Vec2>> out;
  out.reserve(f.loops.size());
  for (const Loop& l : f.loops) {
    std::vector<Vec2> p;
    p.reserve(l.size());
    for (const Id v : l) {
      p.push_back(to2(fr, d.vert(v)));
    }
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace detail

using namespace detail;  // NOLINT(google-build-using-namespace): this file's own helpers

std::unordered_map<std::uint64_t, std::vector<Id>> edge_faces(const Def& d) {
  std::unordered_map<std::uint64_t, std::vector<Id>> m;
  for (const auto& [fid, f] : d.faces) {
    for (const Loop& l : f.loops) {
      for (std::size_t i = 0; i < l.size(); ++i) {
        m[edge_key(l[i], l[(i + 1) % l.size()])].push_back(fid);
      }
    }
  }
  return m;
}

std::unordered_map<Id, std::vector<Id>> vert_edges(const Def& d) {
  std::unordered_map<Id, std::vector<Id>> m;
  for (const auto& [id, e] : d.edges) {
    m[e.a].push_back(id);
    m[e.b].push_back(id);
  }
  return m;
}

std::vector<std::array<Id, 3>> face_tris(const Def& d, const Face& f, Kernel k) {
  const Vec3 n = face_normal(d, f);
  if (len(n) == 0) {
    return {};
  }
  if (k == Kernel::modelcraft && f.loops.size() == 1 && (f.loops[0].size() == 3 || f.loops[0].size() == 4)) {
    const Loop& l = f.loops[0];
    if (l.size() == 3) {
      return {{l[0], l[1], l[2]}};
    }
    const std::vector<Vec3> P = d.points(l);
    const auto up = [&](std::size_t a, std::size_t b, std::size_t c) {
      return dot(cross(sub(P[b], P[a]), sub(P[c], P[a])), n) > 0;
    };
    if (up(0, 1, 2) && up(0, 2, 3)) {
      return {{l[0], l[1], l[2]}, {l[0], l[2], l[3]}};
    }
    if (up(1, 2, 3) && up(1, 3, 0)) {
      return {{l[1], l[2], l[3]}, {l[1], l[3], l[0]}};
    }
  }
  const Frame fr = frame(n);
  std::vector<double> flat;
  std::vector<Id> ids;
  std::vector<std::size_t> holes;
  for (std::size_t i = 0; i < f.loops.size(); ++i) {
    if (i != 0) {
      holes.push_back(ids.size());
    }
    for (const Id v : f.loops[i]) {
      const Vec2 p = to2(fr, d.vert(v));
      flat.push_back(p[0]);
      flat.push_back(p[1]);
      ids.push_back(v);
    }
  }
  const auto t = earcut(flat, holes);
  std::vector<std::array<Id, 3>> out;
  for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
    std::array<Id, 3> tri = {ids.at(t[i]), ids.at(t[i + 1]), ids.at(t[i + 2])};
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

// ---------------------------------------------------------------- edges and vertices

void split_edge(Def& d, Id eid, Id v) {
  const Edge* ep = d.edges.get(eid);
  if (ep == nullptr || ep->a == v || ep->b == v) {
    return;
  }
  const Edge e = *ep;
  e_remove(d, eid);
  (void)e_add(d, e.a, v, e.soft, e.curve, e.hidden);
  (void)e_add(d, v, e.b, e.soft, e.curve, e.hidden);
  for (auto& [fid, f] : d.faces) {
    for (Loop& l : f.loops) {
      for (std::size_t i = 0; i < l.size(); ++i) {
        const Id p = l[i];
        const Id q = l[(i + 1) % l.size()];
        if ((p == e.a && q == e.b) || (p == e.b && q == e.a)) {
          l.insert(l.begin() + static_cast<std::ptrdiff_t>(i + 1), v);
          break;
        }
      }
    }
  }
}

Id vertex_at(Def& d, const Vec3& p) {
  if (const auto have = find_vert(d, p)) {
    return *have;
  }
  const Id id = v_add(d, p);
  std::vector<std::pair<Id, Edge>> edges;
  for (const auto& [eid, e] : d.edges) {
    edges.emplace_back(eid, e);
  }
  for (const auto& [eid, e] : edges) {
    const Vec3 A = d.vert(e.a);
    const Vec3 B = d.vert(e.b);
    const Vec3 u = sub(B, A);
    const double L2 = dot(u, u);
    const double s = dot(sub(p, A), u) / L2;
    if (s <= 0 || s >= 1) {  // NaN (a zero-length edge) goes on, and fails the distance test
      continue;
    }
    if (dist(add(A, mul(u, s)), p) <= kEps) {
      split_edge(d, eid, id);
      break;
    }
  }
  return id;
}

namespace {

struct SegSeg {
  double s = 0, t = 0;
  Vec3 p{};
};

/// Proper crossing of segments A + s·u and C + t·w (both strictly inside), coplanar within EPS.
std::optional<SegSeg> seg_seg(const Vec3& A, const Vec3& u, const Vec3& C, const Vec3& w) {
  const Vec3 n = cross(u, w);
  const double nn = dot(n, n);
  if (nn < 1e-18 * dot(u, u) * dot(w, w)) {
    return std::nullopt;
  }
  const Vec3 AC = sub(C, A);
  const double s = dot(cross(AC, w), n) / nn;
  const double t = dot(cross(AC, u), n) / nn;
  const double tolS = kEps / std::sqrt(dot(u, u));
  const double tolT = kEps / std::sqrt(dot(w, w));
  if (s <= tolS || s >= 1 - tolS || t <= tolT || t >= 1 - tolT) {
    return std::nullopt;
  }
  const Vec3 p1 = add(A, mul(u, s));
  const Vec3 p2 = add(C, mul(w, t));
  if (dist(p1, p2) > kEps) {
    return std::nullopt;
  }
  return SegSeg{s, t, p1};
}

}  // namespace

std::vector<Plane> detail::candidate_planes(const Def& d, std::span<const Id> eids) {
  std::vector<Plane> planes;
  const auto put = [&](const Plane& pl) {
    if (std::ranges::none_of(planes, [&](const Plane& q) { return same_plane(q, pl); })) {
      planes.push_back(pl);
    }
  };
  const auto ve = vert_edges(d);
  for (const Id eid : eids) {
    const Edge* e = d.edges.get(eid);
    if (e == nullptr) {
      continue;
    }
    const Vec3 A = d.vert(e->a);
    const Vec3 B = d.vert(e->b);
    const Vec3 u = sub(B, A);
    for (const auto& [fid, f] : d.faces) {
      const Plane pl = face_plane(d, f);
      if (len(pl.n) != 0 && on_plane(pl, A) && on_plane(pl, B)) {
        put(plane_from(pl.n, A));
      }
    }
    for (const Id end : {e->a, e->b}) {
      const auto it = ve.find(end);
      if (it == ve.end()) {
        continue;
      }
      for (const Id id2 : it->second) {
        if (id2 == eid) {
          continue;
        }
        const Edge& e2 = *d.edges.get(id2);
        const Vec3 C = d.vert(e2.a == end ? e2.b : e2.a);
        const Vec3 n = cross(u, sub(C, A));
        if (len(n) > 1e-9 * len(u) * dist(C, A)) {
          put(plane_from(n, A));
        }
      }
    }
  }
  return planes;
}

std::vector<Id> add_edge(Def& d, const Vec3& p, const Vec3& q, const AddEdgeOpts& o) {
  if (dist(p, q) <= kEps) {
    return {};
  }
  const Id a = vertex_at(d, p);
  const Id b = vertex_at(d, q);
  if (a == b) {
    return {};
  }
  const Vec3 A = d.vert(a);
  const Vec3 B = d.vert(b);
  const Vec3 u = sub(B, A);
  const double L2 = dot(u, u);
  struct Stop {
    double s;
    Id v;
  };
  std::vector<Stop> stops = {{0, a}, {1, b}};
  for (const auto& [vid, P] : d.verts) {
    if (vid == a || vid == b) {
      continue;
    }
    const double s = dot(sub(P, A), u) / L2;
    if (s <= 0 || s >= 1) {
      continue;
    }
    if (dist(add(A, mul(u, s)), P) <= kEps) {
      stops.push_back({s, vid});
    }
  }
  std::vector<std::pair<Id, Edge>> edges;
  for (const auto& [eid, e] : d.edges) {
    edges.emplace_back(eid, e);
  }
  for (const auto& [eid, e] : edges) {
    if (e.a == a || e.a == b || e.b == a || e.b == b) {
      continue;
    }
    if (std::ranges::any_of(stops, [&](const Stop& st) { return st.v == e.a || st.v == e.b; })) {
      continue;
    }
    const Vec3 C = d.vert(e.a);
    const Vec3 D = d.vert(e.b);
    const Vec3 w = sub(D, C);
    const auto r = seg_seg(A, u, C, w);
    if (!r) {
      continue;
    }
    const Id v = v_add(d, r->p);
    split_edge(d, eid, v);
    stops.push_back({r->s, v});
  }
  std::ranges::stable_sort(stops, [](const Stop& x, const Stop& y) { return x.s < y.s; });
  std::vector<Id> ids;
  for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
    if (stops[i].v == stops[i + 1].v) {
      continue;
    }
    if (const auto id = e_add(d, stops[i].v, stops[i + 1].v)) {
      ids.push_back(*id);
    }
  }
  if (o.faces && !ids.empty()) {
    const OrderedSet<Id> fresh = OrderedSet<Id>::of(ids);
    for (const Plane& pl : candidate_planes(d, ids)) {
      (void)reface(d, pl, fresh, o.hint);
    }
  }
  return ids;
}

// ---------------------------------------------------------------- re-facing

namespace {

void prune_dangling(const Def& d, OrderedSet<Id>& live) {
  bool changed = true;
  while (changed) {
    changed = false;
    std::unordered_map<Id, int> deg;
    for (const Id id : live) {
      const Edge& e = *d.edges.get(id);
      ++deg[e.a];
      ++deg[e.b];
    }
    for (const Id id : live.values()) {
      const Edge& e = *d.edges.get(id);
      if (deg[e.a] < 2 || deg[e.b] < 2) {
        live.erase(id);
        changed = true;
      }
    }
  }
}

struct Half {
  Id from, to, e;
  double ang;
};
struct Cycle {
  std::vector<Id> verts, edges;
  std::vector<Vec2> poly;
  double area = 0;
};

struct PairHash {
  std::size_t operator()(const std::pair<Id, Id>& p) const noexcept {
    return std::hash<Id>()(p.first) * 1000003U ^ std::hash<Id>()(p.second);
  }
};

/// walkCycles: faces of a planar graph; each cycle keeps its region on the left (CCW positive area).
std::vector<Cycle> walk_cycles(const Def& d, const OrderedSet<Id>& live, const std::unordered_map<Id, Vec2>& P2) {
  OrderedMap<Id, std::vector<Half>> out;
  for (const Id id : live) {
    const Edge& e = *d.edges.get(id);
    for (const auto& [u, v] : {std::pair{e.a, e.b}, std::pair{e.b, e.a}}) {
      const Vec2& p = P2.at(u);
      const Vec2& q = P2.at(v);
      std::vector<Half>* l = out.get(u);
      if (l == nullptr) {
        l = &out.set(u, {});
      }
      l->push_back({u, v, id, base::js_atan2(q[1] - p[1], q[0] - p[0])});
    }
  }
  for (auto& [v, l] : out) {
    std::ranges::stable_sort(l, [](const Half& x, const Half& y) { return x.ang < y.ang; });
  }
  std::unordered_set<std::pair<Id, Id>, PairHash> seen;
  std::vector<Cycle> cycles;
  for (const auto& [v0, l0] : out) {
    for (const Half& h0 : l0) {
      if (seen.contains({h0.from, h0.to})) {
        continue;
      }
      Cycle c;
      Half h = h0;
      int guard = 0;
      while (!seen.contains({h.from, h.to}) && guard++ < 100000) {
        seen.insert({h.from, h.to});
        c.verts.push_back(h.from);
        c.edges.push_back(h.e);
        const std::vector<Half>& list = *out.get(h.to);
        std::size_t i = 0;
        bool found = false;
        for (; i < list.size(); ++i) {
          if (list[i].to == h.from) {
            found = true;
            break;
          }
        }
        // list[(i - 1 + length) % length]; i = -1 when missing (never: every half has its twin)
        const std::size_t n = list.size();
        const std::size_t k = found ? (i + n - 1) % n : (n - 2 + n) % n;
        h = list[k];
      }
      for (const Id v : c.verts) {
        c.poly.push_back(P2.at(v));
      }
      c.area = area2(c.poly);
      cycles.push_back(std::move(c));
    }
  }
  return cycles;
}

}  // namespace

bool reface(Def& d, const Plane& plane, const OrderedSet<Id>& new_edges, std::optional<Vec3> hint) {
  const Frame fr = frame(plane.n);
  OrderedSet<Id> onP;
  for (const auto& [id, p] : d.verts) {
    if (on_plane(plane, p)) {
      onP.add(id);
    }
  }
  struct Old {
    Id fid;
    Face f;
    double s;
    std::vector<std::vector<Vec2>> poly;
  };
  std::vector<Old> old;
  for (const auto& [fid, f] : d.faces) {
    const bool all = std::ranges::all_of(
        f.loops, [&](const Loop& l) { return std::ranges::all_of(l, [&](Id v) { return onP.has(v); }); });
    if (!all) {
      continue;
    }
    const Vec3 n = face_normal(d, f);
    if (len(n) == 0 || std::abs(dot(n, plane.n)) < 1 - 1e-6) {
      continue;
    }
    const double dn = dot(n, plane.n);
    old.push_back({fid, f, dn > 0 ? 1.0 : -1.0, loops2(d, f, fr)});  // |dn| is near 1 here
  }
  std::vector<Id> E;
  for (const auto& [eid, e] : d.edges) {
    if (onP.has(e.a) && onP.has(e.b)) {
      E.push_back(eid);
    }
  }
  if (old.empty() && std::ranges::none_of(E, [&](Id id) { return new_edges.has(id); })) {
    return false;
  }
  std::unordered_map<Id, Vec2> P2;
  for (const Id v : onP) {
    P2.emplace(v, to2(fr, d.vert(v)));
  }

  OrderedSet<Id> live = OrderedSet<Id>::of(E);
  std::vector<Cycle> cycles;
  for (int guard = 0; guard < 200; ++guard) {
    prune_dangling(d, live);
    cycles = walk_cycles(d, live, P2);
    OrderedSet<Id> bridges;
    for (const Cycle& c : cycles) {
      std::unordered_set<Id> seen;
      for (const Id e : c.edges) {
        if (seen.contains(e)) {
          bridges.add(e);
        }
        seen.insert(e);
      }
    }
    if (bridges.empty()) {
      break;
    }
    for (const Id b : bridges) {
      live.erase(b);
    }
  }
  std::vector<std::size_t> pos;
  std::vector<std::size_t> negs;
  for (std::size_t i = 0; i < cycles.size(); ++i) {
    if (cycles[i].area > kEps2) {
      pos.push_back(i);
    } else if (cycles[i].area < -kEps2) {
      negs.push_back(i);
    }
  }
  std::unordered_map<std::size_t, std::vector<std::size_t>> holes_of;
  for (const std::size_t c : pos) {
    holes_of[c];
  }
  for (const std::size_t h : negs) {
    std::optional<std::size_t> best;
    for (const std::size_t c : pos) {
      const std::unordered_set<Id> vs(cycles[c].verts.begin(), cycles[c].verts.end());
      const auto probe = std::ranges::find_if(cycles[h].verts, [&](Id v) { return !vs.contains(v); });
      if (probe == cycles[h].verts.end() || !in_poly(P2.at(*probe), cycles[c].poly)) {
        continue;
      }
      if (!best || cycles[c].area < cycles[*best].area) {
        best = c;
      }
    }
    if (best) {
      holes_of[*best].push_back(h);
    }
  }
  struct Made {
    std::optional<Id> fid;
    std::vector<Loop> loops;
    std::string color;
    bool hidden;
  };
  std::vector<Made> made;
  std::unordered_set<Id> reused;
  for (const std::size_t c : pos) {
    const std::vector<std::size_t>& holes = holes_of[c];
    std::vector<std::vector<Vec2>> region = {cycles[c].poly};
    for (const std::size_t h : holes) {
      region.push_back(cycles[h].poly);
    }
    const auto pt = sample_point(region);
    if (!pt) {
      continue;
    }
    const Old* inherit = nullptr;
    double sgn = 0;
    bool mixed = false;
    for (const Old& o : old) {
      if (!in_region(*pt, o.poly)) {
        continue;
      }
      if (inherit == nullptr) {
        inherit = &o;
      }
      if (sgn != 0 && sgn != o.s) {
        mixed = true;
      }
      sgn = o.s;
    }
    if (mixed) {
      continue;
    }
    if (inherit == nullptr) {
      if (std::ranges::none_of(cycles[c].edges, [&](Id e) { return new_edges.has(e); })) {
        continue;
      }
      sgn = hint && dot(*hint, plane.n) < 0 ? -1 : 1;
    }
    std::vector<Loop> loops = {cycles[c].verts};
    for (const std::size_t h : holes) {
      loops.push_back(cycles[h].verts);
    }
    if (sgn < 0) {
      for (Loop& l : loops) {
        std::ranges::reverse(l);
      }
    }
    std::optional<Id> fid;
    if (inherit != nullptr && !reused.contains(inherit->fid)) {
      fid = inherit->fid;
      reused.insert(inherit->fid);
    }
    made.push_back({fid, std::move(loops), inherit != nullptr ? inherit->f.color : std::string(),
                    inherit != nullptr && inherit->f.hidden});
  }
  for (const Old& o : old) {
    d.faces.erase(o.fid);
  }
  for (Made& m : made) {
    (void)f_add(d, std::move(m.loops), m.color, m.fid, m.hidden);
  }
  return true;
}

// ---------------------------------------------------------------- clean-up

void prune_splines(Def& d) {
  if (d.splines.empty()) {
    return;
  }
  std::unordered_set<std::string> live;
  for (const auto& [id, e] : d.edges) {
    if (!e.curve.empty()) {
      live.insert(e.curve);
    }
  }
  for (const std::string& cid : d.splines.keys()) {
    if (!live.contains(cid)) {
      d.splines.erase(cid);
    }
  }
}

bool cleanup(Def& d) {
  prune_splines(d);
  std::vector<Id> ids = d.verts.keys();
  std::ranges::stable_sort(ids, [&](Id a, Id b) { return d.vert(a)[0] < d.vert(b)[0]; });
  std::unordered_map<Id, Id> to;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (to.contains(ids[i])) {
      continue;
    }
    const Vec3 p = d.vert(ids[i]);
    for (std::size_t j = i + 1; j < ids.size(); ++j) {
      const Vec3& q = d.vert(ids[j]);
      if (q[0] - p[0] > kEps) {
        break;
      }
      if (!to.contains(ids[j]) && near(p, q)) {
        to.emplace(ids[j], ids[i]);
      }
    }
  }
  bool changed = !to.empty();
  if (!to.empty()) {
    const auto m = [&](Id v) {
      const auto it = to.find(v);
      return it == to.end() ? v : it->second;
    };
    std::vector<std::pair<Id, Edge>> edges;
    for (const auto& [id, e] : d.edges) {
      edges.emplace_back(id, e);
    }
    d.edges.clear();
    d.reset_index();
    for (auto& [id, e] : edges) {
      const Id a = m(e.a);
      const Id b = m(e.b);
      if (a == b || d.edge_between(a, b)) {
        continue;
      }
      e.a = a;
      e.b = b;
      d.edges.set(id, e);
      d.ek().emplace(edge_key(a, b), id);
    }
    for (auto& [fid, f] : d.faces) {
      for (Loop& l : f.loops) {
        for (Id& v : l) {
          v = m(v);
        }
      }
    }
    for (const Id v : ids) {  // `for (const v of to.keys())`: the order does not matter for a delete
      if (to.contains(v)) {
        d.verts.erase(v);
      }
    }
  }
  for (const Id fid : d.faces.keys()) {
    Face& f = *d.faces.get(fid);
    std::vector<std::optional<Loop>> loops;
    for (std::size_t i = 0; i < f.loops.size(); ++i) {
      const Loop& l = f.loops[i];
      Loop c;
      for (std::size_t k = 0; k < l.size(); ++k) {
        if (l[k] != l[(k + 1) % l.size()]) {
          c.push_back(l[k]);
        }
      }
      if (c.size() >= 3 && poly_area3(d.points(c)) > kEps2) {
        loops.emplace_back(std::move(c));
      } else if (i == 0) {
        loops.emplace_back(std::nullopt);
      }
    }
    if (loops.empty() || !loops[0]) {
      d.faces.erase(fid);
      changed = true;
      continue;
    }
    bool differs = loops.size() != f.loops.size();
    for (std::size_t i = 0; !differs && i < loops.size(); ++i) {
      differs = loops[i]->size() != f.loops[i].size();
    }
    if (differs) {
      std::vector<Loop> nl;
      nl.reserve(loops.size());
      for (auto& l : loops) {
        nl.push_back(std::move(l.value()));
      }
      f.loops = std::move(nl);
      changed = true;
    }
    const std::vector<Loop> fl = f.loops;  // e_add may grow the edge table, not the faces
    for (const Loop& l : fl) {
      for (std::size_t i = 0; i < l.size(); ++i) {
        (void)e_add(d, l[i], l[(i + 1) % l.size()]);
      }
    }
  }
  remove_loose_verts(d);
  return changed;
}

void remove_loose_verts(Def& d) {
  std::unordered_set<Id> used;
  for (const auto& [id, e] : d.edges) {
    used.insert(e.a);
    used.insert(e.b);
  }
  for (const auto& [fid, f] : d.faces) {
    for (const Loop& l : f.loops) {
      used.insert(l.begin(), l.end());
    }
  }
  for (const Id v : d.verts.keys()) {
    if (!used.contains(v)) {
      d.verts.erase(v);
    }
  }
  prune_splines(d);
}

void heal_collinear(Def& d, std::span<const Id> vids) {
  for (const Id v : vids) {
    if (!d.verts.has(v)) {
      continue;
    }
    const auto vem = vert_edges(d);
    const auto it = vem.find(v);
    if (it == vem.end() || it->second.size() != 2) {
      continue;
    }
    const std::vector<Id> ve = it->second;
    const Edge e1 = *d.edges.get(ve[0]);
    const Edge e2 = *d.edges.get(ve[1]);
    const Id a = e1.a == v ? e1.b : e1.a;
    const Id b = e2.a == v ? e2.b : e2.a;
    if (a == b) {
      continue;
    }
    const Vec3 V = d.vert(v);
    const Vec3 A = d.vert(a);
    const Vec3 B = d.vert(b);
    const Vec3 u1 = norm(sub(A, V));
    const Vec3 u2 = norm(sub(B, V));
    if (dot(u1, u2) > -1 + 1e-9) {
      continue;
    }
    if (d.edge_between(a, b)) {
      continue;
    }
    const auto ef = edge_faces(d);
    const auto faces_of = [&](Id x, Id y) {
      const auto f = ef.find(edge_key(x, y));
      std::vector<Id> out = f == ef.end() ? std::vector<Id>{} : f->second;
      std::ranges::sort(out);
      return out;
    };
    if (faces_of(v, a) != faces_of(v, b)) {
      continue;
    }
    for (auto& [fid, f] : d.faces) {
      for (Loop& l : f.loops) {
        const auto at = std::ranges::find(l, v);
        if (at != l.end()) {
          l.erase(at);
        }
      }
    }
    e_remove(d, ve[0]);
    e_remove(d, ve[1]);
    (void)e_add(d, a, b, e1.soft && e2.soft, e1.curve == e2.curve ? e1.curve : std::string(), e1.hidden && e2.hidden);
    d.verts.erase(v);
  }
}

int soften_edges(Def& d, std::span<const Id> eids, double deg) {
  const auto ef = edge_faces(d);
  const double c0 = base::js_cos((std::max(0.0, std::min(180.0, deg)) * kPi) / 180);
  int n = 0;
  for (const Id eid : eids) {
    Edge* e = d.edges.get(eid);
    if (e == nullptr) {
      continue;
    }
    const auto it = ef.find(edge_key(e->a, e->b));
    if (it == ef.end() || it->second.size() != 2) {
      continue;
    }
    const double c = dot(face_normal(d, *d.faces.get(it->second[0])), face_normal(d, *d.faces.get(it->second[1])));
    const bool soft = deg > 0 && c >= c0 - 1e-12;
    if (e->soft != soft) {
      e->soft = soft;
      ++n;
    }
  }
  return n;
}

std::map<std::pair<Id, Id>, Vec3> smooth_normals(const Def& d) {
  std::map<std::pair<Id, Id>, Vec3> out;
  bool any_soft = false;
  for (const auto& [eid, e] : d.edges) {
    any_soft = any_soft || e.soft;
  }
  if (!any_soft) {
    return out;
  }
  const auto ef = edge_faces(d);
  std::unordered_map<Id, Id> up;
  const auto find = [&](Id x) {
    while (up.at(x) != x) {
      up[x] = up.at(up.at(x));
      x = up.at(x);
    }
    return x;
  };
  for (const auto& [fid, f] : d.faces) {
    up[fid] = fid;
  }
  OrderedSet<Id> joined;
  for (const auto& [eid, e] : d.edges) {
    if (!e.soft) {
      continue;
    }
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it == ef.end()) {
      continue;
    }
    const auto& fs = it->second;
    for (std::size_t i = 1; i < fs.size(); ++i) {
      up[find(fs[i])] = find(fs[0]);
      joined.add(fs[0]);
      joined.add(fs[i]);
    }
  }
  std::map<std::pair<Id, Id>, Vec3> acc;
  for (const Id fid : joined) {
    const Face& f = *d.faces.get(fid);
    const Vec3 n = face_normal(d, f);
    double a = poly_area3(d.points(f.loops[0]));
    if (a == 0 || std::isnan(a)) {
      a = 1e-12;
    }
    const Id r = find(fid);
    for (const Loop& loop : f.loops) {
      for (const Id v : loop) {
        Vec3& s = acc[{r, v}];
        s = add(s, mul(n, a));
      }
    }
  }
  for (const Id fid : joined) {
    const Face& f = *d.faces.get(fid);
    const Id r = find(fid);
    const Vec3 fn = face_normal(d, f);
    for (const Loop& loop : f.loops) {
      for (const Id v : loop) {
        const Vec3& s = acc.at({r, v});
        const double L = len(s);
        out[{fid, v}] = L > 1e-12 ? mul(s, 1 / L) : fn;
      }
    }
  }
  return out;
}

void merge_coplanar(Def& d, std::span<const Id> eids) {
  for (const Id eid : eids) {
    const Edge* ep = d.edges.get(eid);
    if (ep == nullptr) {
      continue;
    }
    const Edge e = *ep;
    const auto ef = edge_faces(d);
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it == ef.end() || it->second.size() != 2) {
      continue;
    }
    const Plane p1 = face_plane(d, *d.faces.get(it->second[0]));
    const Plane p2 = face_plane(d, *d.faces.get(it->second[1]));
    if (dot(p1.n, p2.n) < 1 - 1e-9 || std::abs(p1.d - p2.d) > kEps) {
      continue;
    }
    e_remove(d, eid);
    (void)reface(d, plane_from(p1.n, d.vert(e.a)));
    remove_loose_verts(d);
    const std::array<Id, 2> ends = {e.a, e.b};
    heal_collinear(d, ends);
  }
}

// ---------------------------------------------------------------- push/pull

namespace {

bool backed(const Def& d, Id fid, const std::unordered_map<std::uint64_t, std::vector<Id>>& ef) {
  return std::ranges::all_of(loop_segs(*d.faces.get(fid)), [&](const std::pair<Id, Id>& s) {
    const auto it = ef.find(edge_key(s.first, s.second));
    return it != ef.end() && it->second.size() >= 2;
  });
}

}  // namespace

double depth_behind(const Def& d, Id fid) {
  const Face& f = *d.faces.get(fid);
  const Vec3 n = face_normal(d, f);
  const Frame fr = frame(n);
  const auto pt2 = sample_point(loops2(d, f, fr));
  if (!pt2) {
    return std::numeric_limits<double>::infinity();
  }
  const Vec3 P0 = d.vert(f.loops[0][0]);
  const Vec3 o = add(add(mul(fr.u, (*pt2)[0]), mul(fr.v, (*pt2)[1])), mul(n, dot(n, P0)));
  const Vec3 dir = mul(n, -1);
  double best = std::numeric_limits<double>::infinity();
  for (const auto& [gid, g] : d.faces) {
    if (gid == fid) {
      continue;
    }
    for (const auto& [a, b, c] : face_tris(d, g)) {
      const auto t = ray_tri(o, dir, d.vert(a), d.vert(b), d.vert(c));
      if (t && *t > kEps && *t < best) {
        best = *t;
      }
    }
  }
  return best;
}

PushPullResult push_pull(Def& d, Id fid, double distance) {
  const Face* fp = d.faces.get(fid);
  if (fp == nullptr || !(std::abs(distance) > kEps)) {
    return {};
  }
  const Vec3 n = face_normal(d, *fp);
  if (len(n) == 0) {
    return {};
  }
  const auto ef = edge_faces(d);
  OrderedSet<Id> vids;
  for (const Loop& l : fp->loops) {
    for (const Id v : l) {
      vids.add(v);
    }
  }
  const auto segs = loop_segs(*fp);
  const auto contains = [&](Id g) { return std::abs(dot(face_normal(d, *d.faces.get(g)), n)) < 1e-6; };

  bool move = std::ranges::all_of(segs, [&](const std::pair<Id, Id>& s) {
    const auto it = ef.find(edge_key(s.first, s.second));
    std::vector<Id> others;
    if (it != ef.end()) {
      for (const Id x : it->second) {
        if (x != fid) {
          others.push_back(x);
        }
      }
    }
    return !others.empty() && std::ranges::all_of(others, contains);
  });
  if (move) {
    const auto vf = vert_faces(d);
    for (const Id v : vids) {
      const auto it = vf.find(v);
      if (it == vf.end()) {
        continue;
      }
      for (const Id g : it->second) {
        if (g != fid && !contains(g)) {
          move = false;
        }
      }
    }
  }
  if (move) {
    double limit = std::numeric_limits<double>::infinity();
    const auto ve = vert_edges(d);
    for (const Id v : vids) {
      const auto it = ve.find(v);
      if (it == ve.end()) {
        continue;
      }
      for (const Id eid : it->second) {
        const Edge& e = *d.edges.get(eid);
        const Id w = e.a == v ? e.b : e.a;
        if (vids.has(w)) {
          continue;
        }
        const double along = dot(sub(d.vert(w), d.vert(v)), n);
        if (along < -kEps) {
          limit = std::min(limit, -along);
        }
      }
    }
    if (distance < 0 && -distance >= limit - kEps) {
      distance = -limit;
    }
    for (const Id v : vids) {
      d.verts.set(v, add(d.vert(v), mul(n, distance)));
    }
    const Vec3 anchor = d.vert(fp->loops[0][0]);
    (void)cleanup(d);
    (void)reface(d, plane_from(n, anchor));
    (void)cleanup(d);
    return {.ok = true,
            .mode = "move",
            .distance = distance,
            .face = d.faces.has(fid) ? std::optional<Id>(fid) : std::nullopt};
  }

  const bool solid = backed(d, fid, ef);
  if (solid && distance < 0) {
    const double t = depth_behind(d, fid);
    if (-distance > t - kEps) {
      distance = -t;
    }
  }
  std::unordered_map<Id, Id> map;
  for (const Id v : vids) {
    map.emplace(v, v_add(d, add(d.vert(v), mul(n, distance))));
  }
  const std::vector<Loop> orig = d.faces.get(fid)->loops;
  const std::string color = d.faces.get(fid)->color;
  std::vector<Loop> top;
  for (const Loop& l : orig) {
    Loop t;
    for (const Id v : l) {
      t.push_back(map.at(v));
    }
    top.push_back(std::move(t));
  }
  const bool lone = !solid;
  const bool flip = lone && distance < 0;
  if (lone && distance > 0) {
    std::vector<Loop> rev = orig;
    for (Loop& l : rev) {
      std::ranges::reverse(l);
    }
    d.faces.get(fid)->loops = std::move(rev);
  } else if (!lone) {
    d.faces.erase(fid);
  }
  if (flip) {
    for (Loop& l : top) {
      std::ranges::reverse(l);
    }
  }
  const Id top_id = f_add(d, std::move(top), color);
  std::vector<Id> side_edges;
  for (const Loop& l : orig) {
    for (std::size_t i = 0; i < l.size(); ++i) {
      const Id a = l[i];
      const Id b = l[(i + 1) % l.size()];
      const Id a2 = map.at(a);
      const Id b2 = map.at(b);
      Loop quad = {a, b, b2, a2};
      if (flip) {
        std::ranges::reverse(quad);
      }
      (void)f_add(d, {std::move(quad)}, color);
      if (const auto e = d.edge_between(a, a2)) {
        side_edges.push_back(*e);
      }
      if (const auto e = d.edge_between(a, b)) {
        side_edges.push_back(*e);
      }
    }
  }
  (void)cleanup(d);
  if (const Face* tf = d.faces.get(top_id)) {
    (void)reface(d, plane_from(n, d.vert(tf->loops[0][0])));
  }
  (void)cleanup(d);
  merge_coplanar(d, side_edges);
  const auto ef2 = edge_faces(d);
  const double c25 = base::js_cos(25 * kPi / 180);
  for (const Loop& l : orig) {
    for (const Id a : l) {
      const auto eid = d.edge_between(a, map.at(a));
      if (!eid) {
        continue;
      }
      Edge& e = *d.edges.get(*eid);
      const auto it = ef2.find(edge_key(e.a, e.b));
      if (it != ef2.end() && it->second.size() == 2 &&
          dot(face_normal(d, *d.faces.get(it->second[0])), face_normal(d, *d.faces.get(it->second[1]))) > c25) {
        e.soft = true;
      }
    }
  }
  return {.ok = true,
          .mode = "extrude",
          .distance = distance,
          .face = d.faces.has(top_id) ? std::optional<Id>(top_id) : std::nullopt};
}

}  // namespace cc::m3d
