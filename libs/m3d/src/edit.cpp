// SPDX-License-Identifier: Apache-2.0
// model.js, definitions: erasing, moving, copying, shapes, curves, smooth curves, hide, flip, follow me.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/m3d/model.hpp"
#include "geom_internal.hpp"

namespace cc::m3d {

using namespace detail;  // NOLINT(google-build-using-namespace): the kernel's own helpers

// ---------------------------------------------------------------- erasing, moving, copying

void erase_edges(Def& d, std::span<const Id> eids) {
  std::vector<Id> ends;
  for (const Id eid : eids) {
    const Edge* ep = d.edges.get(eid);
    if (ep == nullptr) {
      continue;
    }
    const Edge e = *ep;
    ends.push_back(e.a);
    ends.push_back(e.b);
    const auto ef = edge_faces(d);
    const auto it = ef.find(edge_key(e.a, e.b));
    const std::vector<Id> fs = it == ef.end() ? std::vector<Id>{} : it->second;
    if (fs.size() == 2) {
      const Plane p1 = face_plane(d, *d.faces.get(fs[0]));
      const Plane p2 = face_plane(d, *d.faces.get(fs[1]));
      if (dot(p1.n, p2.n) > 1 - 1e-9 && std::abs(p1.d - p2.d) <= kEps) {
        e_remove(d, eid);
        (void)reface(d, plane_from(p1.n, d.vert(e.a)));
        continue;
      }
    }
    for (const Id fid : fs) {
      d.faces.erase(fid);
    }
    e_remove(d, eid);
  }
  remove_loose_verts(d);
  heal_collinear(d, ends);
}

OrderedSet<Id> sel_verts(const Def& d, const Sel& sel) {
  OrderedSet<Id> vs;
  for (const Item& s : sel) {
    if (s.t == 'e') {
      if (const Edge* e = d.edges.get(s.id)) {
        vs.add(e->a);
        vs.add(e->b);
      }
    } else if (s.t == 'f') {
      if (const Face* f = d.faces.get(s.id)) {
        for (const Loop& l : f->loops) {
          for (const Id v : l) {
            vs.add(v);
          }
        }
      }
    }
  }
  return vs;
}

void transform_sel(Def& d, const Sel& sel, const Mat4& m) {
  transform_verts(d, sel_verts(d, sel), m);
  for (const Item& s : sel) {
    if (s.t == 'i') {
      if (Inst* inst = d.insts.get(s.id)) {
        inst->m = mmul(m, inst->m);
      }
    }
  }
}

namespace {

bool planar(const Def& d, const Face& f) {
  const Plane pl = face_plane(d, f);
  if (len(pl.n) == 0) {
    return false;
  }
  return std::ranges::all_of(f.loops, [&](const Loop& l) {
    return std::ranges::all_of(l, [&](Id v) { return std::abs(dot(pl.n, d.vert(v)) - pl.d) <= kEps * 10; });
  });
}

}  // namespace

void transform_verts(Def& d, const OrderedSet<Id>& vs, const Mat4& m) {
  if (vs.empty()) {
    return;
  }
  const auto vf = vert_faces(d);
  OrderedSet<Id> touched;
  for (const Id v : vs) {
    if (const auto it = vf.find(v); it != vf.end()) {
      for (const Id f : it->second) {
        touched.add(f);
      }
    }
  }
  std::unordered_map<Id, std::vector<std::array<Id, 3>>> before;
  for (const Id fid : touched) {
    before.emplace(fid, face_tris(d, *d.faces.get(fid)));
  }
  for (const Id v : vs) {
    d.verts.set(v, m3d::apply(m, d.vert(v)));
  }
  if (det3(m) < 0) {
    for (auto& [fid, f] : d.faces) {
      const bool all = std::ranges::all_of(
          f.loops, [&](const Loop& l) { return std::ranges::all_of(l, [&](Id v) { return vs.has(v); }); });
      if (all) {
        for (Loop& l : f.loops) {
          std::ranges::reverse(l);
        }
      }
    }
  }
  for (const std::string& cid : d.splines.keys()) {
    std::size_t own = 0;
    std::size_t moved = 0;
    bool touches = false;
    for (const auto& [eid, e] : d.edges) {
      if (e.curve != cid) {
        continue;
      }
      ++own;
      const bool a = vs.has(e.a);
      const bool b = vs.has(e.b);
      moved += a && b ? 1 : 0;
      touches = touches || a || b;
    }
    if (own != 0 && moved == own) {
      Spline& sp = *d.splines.get(cid);
      for (Vec3& p : sp.pts) {
        p = m3d::apply(m, p);
      }
    } else if (moved != 0 || touches) {
      d.splines.erase(cid);
    }
  }
  for (const Id fid : touched) {
    const Face* fp = d.faces.get(fid);
    if (fp == nullptr || planar(d, *fp)) {
      continue;
    }
    const Face f = *fp;
    d.faces.erase(fid);
    for (const auto& tri : before.at(fid)) {
      (void)f_add(d, {Loop{tri[0], tri[1], tri[2]}}, f.color, std::nullopt, f.hidden);
    }
  }
  (void)cleanup(d);
}

base::Result<std::size_t> stretch(Def& d, const Vec3& dir, double cut, double delta, const StretchOpts& o) {
  const double tol = kEps * 10;
  OrderedSet<Id> vs;
  for (const auto& [id, p] : d.verts) {
    if ((o.only == nullptr || o.only->has(id)) && dot(p, dir) >= cut - tol) {
      vs.add(id);
    }
  }
  if (vs.empty() && d.insts.empty()) {
    return base::fail(base::Errc::invalid, tr("m3d.model.there_is_nothing_on_that_side"));
  }
  std::unordered_map<Id, Vec3> at;
  std::vector<std::pair<Id, Vec3>> at_order;
  for (const Id v : vs) {
    const Vec3 q = add(d.vert(v), mul(dir, delta));
    at.emplace(v, q);
    at_order.emplace_back(v, q);
  }
  const auto pos = [&](Id v) {
    const auto it = at.find(v);
    return it != at.end() ? it->second : d.vert(v);
  };
  for (const auto& [fid, f] : d.faces) {
    std::size_t total = 0;
    std::size_t n = 0;
    for (const Loop& l : f.loops) {
      for (const Id v : l) {
        ++total;
        n += vs.has(v) ? 1 : 0;
      }
    }
    if (n == 0 || n == total) {
      continue;
    }
    const Vec3 n0 = face_normal(d, f);
    std::vector<Vec3> moved;
    for (const Id v : f.loops[0]) {
      moved.push_back(pos(v));
    }
    const Vec3 n1 = newell(moved);
    if (len(n1) <= kEps2 || dot(norm(n1), n0) <= 0) {
      return base::fail(base::Errc::invalid, tr("m3d.model.a_face_would_shrink_to_nothing"));
    }
    const Vec3 k = norm(n1);
    const double c = dot(k, pos(f.loops[0][0]));
    for (const Loop& l : f.loops) {
      for (const Id v : l) {
        if (!(std::abs(dot(k, pos(v)) - c) <= tol)) {
          return base::fail(base::Errc::invalid, tr("m3d.model.a_face_would_bend_out_of"));
        }
      }
    }
  }
  std::optional<Box> bb;
  if (o.only != nullptr) {
    for (const Id v : *o.only) {
      bbox_add(bb, d.vert(v));
    }
  }
  for (const auto& [v, p] : at_order) {
    d.verts.set(v, p);
  }
  for (auto& [iid, inst] : d.insts) {
    const auto local = o.box_of ? o.box_of(inst.def) : std::nullopt;
    if (!local) {
      continue;
    }
    std::vector<Vec3> corners;
    corners.reserve(8);
    for (unsigned i = 0; i < 8; ++i) {
      corners.push_back(m3d::apply(
          inst.m, {(i & 1U) != 0 ? local->max[0] : local->min[0], (i & 2U) != 0 ? local->max[1] : local->min[1],
                   (i & 4U) != 0 ? local->max[2] : local->min[2]}));
    }
    if (std::ranges::any_of(corners, [&](const Vec3& p) { return dot(p, dir) < cut - tol; })) {
      continue;
    }
    std::optional<Box> ib;
    for (const Vec3& p : corners) {
      bbox_add(ib, p);
    }
    if (bb) {
      bool apart = false;
      for (std::size_t i = 0; i < 3; ++i) {
        apart = apart || ib->min.at(i) > bb->max.at(i) + tol || ib->max.at(i) < bb->min.at(i) - tol;
      }
      if (apart) {
        continue;
      }
    }
    inst.m = mmul(translation(mul(dir, delta)), inst.m);
  }
  (void)cleanup(d);
  return vs.size();
}

Sel copy_sel(Def& d, const Sel& sel) {
  std::unordered_map<Id, Id> vmap;
  const auto cv = [&](Id v) {
    const auto it = vmap.find(v);
    if (it != vmap.end()) {
      return it->second;
    }
    const Id n = v_add(d, d.vert(v));
    vmap.emplace(v, n);
    return n;
  };
  Sel out;
  CurveRemapper rc(d);
  std::vector<Face> faces;
  for (const Item& s : sel) {
    if (s.t == 'f') {
      if (const Face* f = d.faces.get(s.id)) {
        faces.push_back(*f);
      }
    }
  }
  for (const Face& f : faces) {
    std::vector<Loop> loops;
    for (const Loop& l : f.loops) {
      Loop c;
      for (const Id v : l) {
        c.push_back(cv(v));
      }
      loops.push_back(std::move(c));
    }
    out.push_back({'f', f_add(d, std::move(loops), f.color, std::nullopt, f.hidden)});
  }
  OrderedMap<std::string, Spline> spl;
  for (const Item& s : sel) {
    if (s.t != 'e') {
      continue;
    }
    const Edge* ep = d.edges.get(s.id);
    if (ep == nullptr) {
      continue;
    }
    const Edge e = *ep;
    const std::string nc = e.curve.empty() ? std::string() : rc(e.curve);
    const Id a = cv(e.a);
    const Id b = cv(e.b);
    const auto id = e_add(d, a, b, e.soft, nc, e.hidden);
    out.push_back({'e', id.value_or(0)});
    if (!e.curve.empty()) {
      if (const Spline* sp = d.splines.get(e.curve)) {
        spl.set(nc, *sp);
      }
    }
  }
  for (const auto& [nc, sp] : spl) {
    d.splines.set(nc, sp);
  }
  for (const Item& s : sel) {
    if (s.t != 'i') {
      continue;
    }
    const Inst* inst = d.insts.get(s.id);
    if (inst == nullptr) {
      continue;
    }
    const Inst copy = *inst;
    const Id id = d.next++;
    d.insts.set(id, copy);
    out.push_back({'i', id});
  }
  return out;
}

void erase_sel(Def& d, const Sel& sel) {
  for (const Item& s : sel) {
    if (s.t == 'f') {
      d.faces.erase(s.id);
    }
  }
  for (const Item& s : sel) {
    if (s.t == 'i') {
      d.insts.erase(s.id);
    }
  }
  std::vector<Id> eids;
  for (const Item& s : sel) {
    if (s.t == 'e') {
      eids.push_back(s.id);
    }
  }
  erase_edges(d, eids);
  remove_loose_verts(d);
}

void reverse_faces(Def& d, std::span<const Id> fids) {
  for (const Id fid : fids) {
    if (Face* f = d.faces.get(fid)) {
      for (Loop& l : f->loops) {
        std::ranges::reverse(l);
      }
    }
  }
}

Sel connected(const Def& d, const Sel& sel) {
  const OrderedSet<Id> vs = sel_verts(d, sel);
  const auto ve = vert_edges(d);
  const auto vf = vert_faces(d);
  std::vector<Id> stack = vs.values();
  std::unordered_set<Id> seen_v(stack.begin(), stack.end());
  OrderedSet<Id> eids;
  OrderedSet<Id> fids;
  while (!stack.empty()) {
    const Id v = stack.back();
    stack.pop_back();
    if (const auto it = ve.find(v); it != ve.end()) {
      for (const Id eid : it->second) {
        eids.add(eid);
        const Edge& e = *d.edges.get(eid);
        for (const Id w : {e.a, e.b}) {
          if (!seen_v.contains(w)) {
            seen_v.insert(w);
            stack.push_back(w);
          }
        }
      }
    }
    if (const auto it = vf.find(v); it != vf.end()) {
      for (const Id f : it->second) {
        fids.add(f);
      }
    }
  }
  Sel out;
  for (const Id id : fids) {
    out.push_back({'f', id});
  }
  for (const Id id : eids) {
    out.push_back({'e', id});
  }
  return out;
}

OrderedSet<Id> connected_verts(const Def& d, Id v) {
  const auto ve = vert_edges(d);
  OrderedSet<Id> seen{v};
  std::vector<Id> stack = {v};
  while (!stack.empty()) {
    const Id x = stack.back();
    stack.pop_back();
    const auto it = ve.find(x);
    if (it == ve.end()) {
      continue;
    }
    for (const Id eid : it->second) {
      const Edge& e = *d.edges.get(eid);
      for (const Id w : {e.a, e.b}) {
        if (!seen.has(w)) {
          seen.add(w);
          stack.push_back(w);
        }
      }
    }
  }
  return seen;
}

Sel face_with_edges(const Def& d, Id fid) {
  const Face* f = d.faces.get(fid);
  if (f == nullptr) {
    return {};
  }
  Sel out = {{'f', fid}};
  for (const auto& [a, b] : loop_segs(*f)) {
    if (const auto e = d.edge_between(a, b)) {
      out.push_back({'e', *e});
    }
  }
  return out;
}

// ---------------------------------------------------------------- shapes

std::vector<Id> add_polygon(Def& d, std::span<const Vec3> pts, std::optional<Vec3> hint) {
  std::vector<Id> ids;
  if (pts.empty()) {  // the JS reads pts[0] (undefined) and throws: nothing to draw (fuzz, 2026-10-21)
    return ids;
  }
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const auto e = add_edge(d, pts[i], pts[(i + 1) % pts.size()], {.faces = false, .hint = std::nullopt});
    ids.insert(ids.end(), e.begin(), e.end());
  }
  std::vector<Plane> planes = candidate_planes(d, ids);
  const Plane pl = plane_from(newell(pts), pts[0]);
  if (std::ranges::none_of(planes, [&](const Plane& q) { return same_plane(q, pl); })) {
    planes.insert(planes.begin(), pl);
  }
  const OrderedSet<Id> fresh = OrderedSet<Id>::of(ids);
  for (const Plane& p : planes) {
    (void)reface(d, p, fresh, hint);
  }
  return ids;
}

std::vector<Vec3> rect_pts(const Vec3& p, const Vec3& u, const Vec3& v, double w, double h) {
  return {p, add(p, mul(u, w)), add(add(p, mul(u, w)), mul(v, h)), add(p, mul(v, h))};
}

std::vector<Vec3> ngon_pts(const Vec3& c, const Vec3& n, const Vec3& u, double r, int sides) {
  const Vec3 v = cross(norm(n), u);
  std::vector<Vec3> out;
  for (int i = 0; i < sides; ++i) {
    const double a = (static_cast<double>(i) / sides) * kPi * 2;
    out.push_back(add(c, add(mul(u, r * base::js_cos(a)), mul(v, r * base::js_sin(a)))));
  }
  return out;
}

// ---------------------------------------------------------------- curves

bool is_curve_id(std::string_view s) {
  // /^(arc|circle|polygon|curve):\d{1,9}$/
  const auto colon = s.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string_view kind = s.substr(0, colon);
  const std::string_view num = s.substr(colon + 1);
  if (kind != "arc" && kind != "circle" && kind != "polygon" && kind != "curve") {
    return false;
  }
  return !num.empty() && num.size() <= 9 && std::ranges::all_of(num, [](char ch) { return ch >= '0' && ch <= '9'; });
}

std::string CurveRemapper::operator()(const std::string& c) {
  if (c.empty()) {
    return {};
  }
  const auto it = m_.find(c);
  if (it != m_.end()) {
    return it->second;
  }
  std::string n = c.substr(0, c.find(':')) + ":" + std::to_string(d_->next++);
  m_.emplace(c, n);
  return n;
}

std::vector<Vec3> arc_pts(const Vec3& c, const Vec3& n, const Vec3& u, double r, double sweep, int segs) {
  const Vec3 v = cross(norm(n), u);
  std::vector<Vec3> out;
  for (int i = 0; i <= segs; ++i) {
    const double a = (sweep * i) / segs;
    out.push_back(add(c, add(mul(u, r * base::js_cos(a)), mul(v, r * base::js_sin(a)))));
  }
  return out;
}

std::string tag_curve(Def& d, std::span<const Vec3> pts, std::string_view kind, bool closed) {
  std::string id = std::string(kind) + ":" + std::to_string(d.next++);
  if (pts.empty()) {
    return id;
  }
  std::vector<std::pair<Vec3, Vec3>> segs;
  for (std::size_t i = 0; i + 1 < pts.size() + (closed ? 1 : 0); ++i) {
    segs.emplace_back(pts[i], pts[(i + 1) % pts.size()]);
  }
  const auto on = [](const Vec3& p, const Vec3& A, const Vec3& B) {
    const Vec3 u = sub(B, A);
    const double L2 = dot(u, u);
    const double t = L2 != 0 ? dot(sub(p, A), u) / L2 : 0;
    return t >= -1e-9 && t <= 1 + 1e-9 && dist(add(A, mul(u, t)), p) <= kEps;
  };
  for (auto& [eid, e] : d.edges) {
    const Vec3& P = d.vert(e.a);
    const Vec3& Q = d.vert(e.b);
    if (std::ranges::any_of(segs,
                            [&](const auto& s) { return on(P, s.first, s.second) && on(Q, s.first, s.second); })) {
      e.curve = id;
    }
  }
  return id;
}

std::vector<Id> curve_edges(const Def& d, std::string_view cid) {
  std::vector<Id> out;
  for (const auto& [id, e] : d.edges) {
    if (e.curve == cid) {
      out.push_back(id);
    }
  }
  return out;
}

std::optional<CurveInfo> curve_info(const Def& d, std::string_view cid) {
  const std::vector<Id> eids = curve_edges(d, cid);
  if (eids.empty()) {
    return std::nullopt;
  }
  OrderedMap<Id, std::vector<Id>> adj;
  double length = 0;
  for (const Id id : eids) {
    const Edge& e = *d.edges.get(id);
    for (const auto& [x, y] : {std::pair{e.a, e.b}, std::pair{e.b, e.a}}) {
      std::vector<Id>* l = adj.get(x);
      if (l == nullptr) {
        l = &adj.set(x, {});
      }
      l->push_back(y);
    }
    length += dist(d.vert(e.a), d.vert(e.b));
  }
  std::vector<Id> ends;
  bool all2 = true;
  for (const auto& [v, l] : adj) {
    if (l.size() == 1) {
      ends.push_back(v);
    }
    all2 = all2 && l.size() == 2;
  }
  const bool closed = ends.empty() && all2;
  std::vector<Id> chain;
  std::optional<Id> cur = !ends.empty() ? std::optional<Id>(ends[0]) : std::optional<Id>(adj.keys()[0]);
  std::optional<Id> prev;
  while (cur && chain.size() <= adj.size()) {
    chain.push_back(*cur);
    const std::vector<Id>* l = adj.get(*cur);
    std::optional<Id> nx;
    if (l != nullptr) {
      for (const Id x : *l) {
        if ((!prev || x != *prev) && std::ranges::find(chain, x) == chain.end()) {
          nx = x;
          break;
        }
      }
    }
    prev = cur;
    cur = nx;
  }
  const std::vector<Vec3> P = d.points(chain);
  CurveInfo out;
  out.id = std::string(cid);
  out.kind = std::string(cid.substr(0, cid.find(':')));
  out.edges = eids;
  out.verts = chain;
  out.closed = closed;
  out.length = length;
  out.segs = eids.size();
  if (P.size() >= 3 && chain.size() == adj.size()) {
    const Vec3& a = P[0];
    const Vec3& b = P[P.size() / 3];
    const Vec3& c = P[(2 * P.size()) / 3];
    const Vec3 ab = sub(b, a);
    const Vec3 ac = sub(c, a);
    const Vec3 n = cross(ab, ac);
    const double nn = dot(n, n);
    if (nn > 1e-24) {
      const Vec3 cc = add(a, mul(add(mul(cross(n, ab), dot(ac, ac)), mul(cross(ac, n), dot(ab, ab))), 1 / (2 * nn)));
      const double r = dist(cc, a);
      const Vec3 nrm = norm(n);
      const double tol = std::max(kEps, r * 1e-7);
      if (std::ranges::all_of(P, [&](const Vec3& p) {
            return std::abs(dist(p, cc) - r) <= tol && std::abs(dot(sub(p, cc), nrm)) <= tol;
          })) {
        out.round = true;
        out.center = cc;
        out.normal = nrm;
        out.radius = r;
        const auto ef = edge_faces(d);
        out.flat = std::ranges::all_of(eids, [&](Id id) {
          const Edge& e = *d.edges.get(id);
          const auto it = ef.find(edge_key(e.a, e.b));
          if (it == ef.end()) {
            return true;
          }
          return std::ranges::all_of(
              it->second, [&](Id fid) { return std::abs(dot(face_normal(d, *d.faces.get(fid)), nrm)) > 1 - 1e-9; });
        });
        double sw = 0;
        for (std::size_t i = 0; i + 1 < P.size() + (closed ? 1 : 0); ++i) {
          const Vec3 u1 = norm(sub(P[i], cc));
          const Vec3 u2 = norm(sub(P[(i + 1) % P.size()], cc));
          sw += base::js_atan2(dot(cross(u1, u2), nrm), dot(u1, u2));
        }
        out.sweep = std::abs(sw);
      }
    }
  }
  return out;
}

base::Result<CurveInfo> set_curve_radius(Def& d, std::string_view cid, double r) {
  const auto ci = curve_info(d, cid);
  if (!ci || !ci->round) {
    return base::fail(base::Errc::invalid, tr("m3d.model.only_arcs_circles_and_polygons_have"));
  }
  if (!ci->flat) {
    return base::fail(base::Errc::invalid, tr("m3d.model.this_curve_bounds_a_face_that"));
  }
  if (!(r > kEps)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.type_a_radius_above_zero"));
  }
  const double k = r / ci->radius;
  const Vec3 c = must(ci->center);
  for (const Id v : ci->verts) {
    d.verts.set(v, add(c, mul(sub(d.vert(v), c), k)));
  }
  return must(curve_info(d, cid));
}

std::vector<Vec3> smooth_pts(std::span<const Vec3> P, bool closed, int segs) {
  const std::size_t n = P.size();
  if (n < 3) {
    return {P.begin(), P.end()};
  }
  const auto at = [&](std::ptrdiff_t i) -> Vec3 {
    const auto N = static_cast<std::ptrdiff_t>(n);
    if (closed) {
      return P[static_cast<std::size_t>(((i % N) + N) % N)];
    }
    if (i < 0) {
      return sub(mul(P[0], 2), P[1]);
    }
    if (i >= N) {
      return sub(mul(P[n - 1], 2), P[n - 2]);
    }
    return P[static_cast<std::size_t>(i)];
  };
  std::vector<Vec3> out;
  const std::size_t spans = closed ? n : n - 1;
  const auto lerp4 = [](const Vec3& a, const Vec3& b, double ta, double tb, double t) {
    return add(mul(a, (tb - t) / (tb - ta)), mul(b, (t - ta) / (tb - ta)));
  };
  for (std::size_t s = 0; s < spans; ++s) {
    const auto i = static_cast<std::ptrdiff_t>(s);
    const Vec3 p0 = at(i - 1);
    const Vec3 p1 = at(i);
    const Vec3 p2 = at(i + 1);
    const Vec3 p3 = at(i + 2);
    const auto tj = [](const Vec3& a, const Vec3& b) { return std::max(1e-9, std::sqrt(dist(a, b))); };
    const double t1 = tj(p0, p1);
    const double t2 = t1 + tj(p1, p2);
    const double t3 = t2 + tj(p2, p3);
    for (int k = 0; k < segs; ++k) {
      const double t = t1 + ((t2 - t1) * k) / segs;
      const Vec3 A1 = lerp4(p0, p1, 0, t1, t);
      const Vec3 A2 = lerp4(p1, p2, t1, t2, t);
      const Vec3 A3 = lerp4(p2, p3, t2, t3, t);
      const Vec3 B1 = lerp4(A1, A2, 0, t2, t);
      const Vec3 B2 = lerp4(A2, A3, t1, t3, t);
      out.push_back(lerp4(B1, B2, t1, t2, t));
    }
  }
  if (!closed) {
    out.push_back(P[n - 1]);
  }
  return out;
}

base::Result<std::string> add_spline(Def& d, std::span<const Vec3> pts, bool closed, int segs,
                                     std::optional<Vec3> hint) {
  if (pts.size() < 2) {
    return base::fail(base::Errc::invalid, tr("m3d.model.a_curve_needs_two_points_or"));
  }
  const std::vector<Vec3> S = smooth_pts(pts, closed, segs);
  for (std::size_t i = 0; i + 1 < S.size() + (closed ? 1 : 0); ++i) {
    (void)add_edge(d, S[i], S[(i + 1) % S.size()], {.faces = true, .hint = hint});
  }
  std::string id = tag_curve(d, S, "curve", closed);
  d.splines.set(id, Spline{.pts = {pts.begin(), pts.end()}, .closed = closed, .segs = segs});
  return id;
}

base::Result<std::string> edit_spline(Def& d, const std::string& cid, std::span<const Vec3> pts,
                                      std::optional<bool> closed, std::optional<int> segs) {
  const Spline* sp = d.splines.get(cid);
  if (sp == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.model.this_curve_can_no_longer_be"));
  }
  const bool c = closed.value_or(sp->closed);
  const int n = segs.value_or(sp->segs);
  const auto ef = edge_faces(d);
  const std::vector<Id> own = curve_edges(d, cid);
  for (const Id id : own) {
    const Edge& e = *d.edges.get(id);
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it != ef.end() && !it->second.empty()) {
      return base::fail(base::Errc::invalid, tr("m3d.model.this_curve_bounds_a_face_erase"));
    }
  }
  for (const Id id : own) {
    e_remove(d, id);
  }
  remove_loose_verts(d);
  const std::vector<Vec3> S = smooth_pts(pts, c, n);
  for (std::size_t i = 0; i + 1 < S.size() + (c ? 1 : 0); ++i) {
    (void)add_edge(d, S[i], S[(i + 1) % S.size()], {.faces = false, .hint = std::nullopt});
  }
  const std::string nid2 = tag_curve(d, S, "curve", c);
  for (auto& [eid, e] : d.edges) {
    if (e.curve == nid2) {
      e.curve = cid;
    }
  }
  d.splines.set(cid, Spline{.pts = {pts.begin(), pts.end()}, .closed = c, .segs = n});
  return cid;
}

// ---------------------------------------------------------------- hide, isolate, flip

void hide_sel(Def& d, const Sel& sel, bool on) {
  for (const Item& s : sel) {
    if (s.t == 'e') {
      if (Edge* e = d.edges.get(s.id)) {
        e->hidden = on;
      }
    } else if (s.t == 'f') {
      if (Face* f = d.faces.get(s.id)) {
        f->hidden = on;
      }
    } else if (s.t == 'i') {
      if (Inst* i = d.insts.get(s.id)) {
        i->hidden = on;
      }
    }
  }
}

int isolate_sel(Def& d, const Sel& sel) {
  std::unordered_set<std::string> keep;
  const auto k = [](char t, Id id) { return std::string(1, t) + std::to_string(id); };
  for (const Item& s : sel) {
    keep.insert(k(s.t, s.id));
  }
  for (const Item& s : sel) {
    if (s.t != 'f') {
      continue;
    }
    if (const Face* f = d.faces.get(s.id)) {
      for (const Loop& l : f->loops) {
        for (std::size_t i = 0; i < l.size(); ++i) {
          if (const auto id = d.edge_between(l[i], l[(i + 1) % l.size()])) {
            keep.insert(k('e', *id));
          }
        }
      }
    }
  }
  int n = 0;
  for (auto& [id, e] : d.edges) {
    if (!keep.contains(k('e', id)) && !e.hidden) {
      e.hidden = true;
      ++n;
    }
  }
  for (auto& [id, f] : d.faces) {
    if (!keep.contains(k('f', id)) && !f.hidden) {
      f.hidden = true;
      ++n;
    }
  }
  for (auto& [id, i] : d.insts) {
    if (!keep.contains(k('i', id)) && !i.hidden) {
      i.hidden = true;
      ++n;
    }
  }
  return n;
}

Mat4 mirror_matrix(const Vec3& c, const Vec3& n) {
  const Vec3 u = norm(n);
  const double x = u[0];
  const double y = u[1];
  const double z = u[2];
  const Mat4 L = {1 - 2 * x * x,
                  -2 * x * y,
                  -2 * x * z,
                  0,
                  -2 * x * y,
                  1 - 2 * y * y,
                  -2 * y * z,
                  0,
                  -2 * x * z,
                  -2 * y * z,
                  1 - 2 * z * z,
                  0,
                  0,
                  0,
                  0,
                  1};
  return mmul(translation(c), mmul(L, translation(mul(c, -1))));
}

Sel flip_sel(Def& d, const Sel& sel, const Vec3& c, const Vec3& n, bool copy) {
  Sel target = copy ? copy_sel(d, sel) : sel;
  transform_sel(d, target, mirror_matrix(c, n));
  return target;
}

// ---------------------------------------------------------------- follow me

base::Result<Chain> edge_chain(const Def& d, std::span<const Id> eids) {
  OrderedMap<Id, std::vector<Id>> adj;
  for (const Id id : eids) {
    const Edge* e = d.edges.get(id);
    if (e == nullptr) {
      continue;
    }
    for (const auto& [x, y] : {std::pair{e->a, e->b}, std::pair{e->b, e->a}}) {
      std::vector<Id>* l = adj.get(x);
      if (l == nullptr) {
        l = &adj.set(x, {});
      }
      l->push_back(y);
    }
  }
  if (adj.empty()) {
    return base::fail(base::Errc::invalid, tr("m3d.model.choose_a_path_one_or_more"));
  }
  std::vector<Id> ends;
  for (const auto& [v, l] : adj) {
    if (l.size() > 2) {
      return base::fail(base::Errc::invalid, tr("m3d.model.the_path_branches_select_one_chain"));
    }
  }
  for (const auto& [v, l] : adj) {
    if (l.size() == 1) {
      ends.push_back(v);
    }
  }
  Chain out;
  std::optional<Id> cur = !ends.empty() ? std::optional<Id>(ends[0]) : std::optional<Id>(adj.keys()[0]);
  std::optional<Id> prev;
  while (cur && std::ranges::find(out.verts, *cur) == out.verts.end()) {
    out.verts.push_back(*cur);
    std::optional<Id> nx;
    for (const Id x : *adj.get(*cur)) {
      if ((!prev || x != *prev) && std::ranges::find(out.verts, x) == out.verts.end()) {
        nx = x;
        break;
      }
    }
    prev = cur;
    cur = nx;
  }
  if (out.verts.size() != adj.size()) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_path_is_in_more_than"));
  }
  out.closed = ends.empty();
  return out;
}

std::vector<Id> chain_through(const Def& d, Id eid) {
  const auto ve = vert_edges(d);
  const Edge* e0 = d.edges.get(eid);
  if (e0 == nullptr) {
    return {};
  }
  OrderedSet<Id> out{eid};
  for (const Id start : {e0->a, e0->b}) {
    Id v = start;
    Id from = eid;
    for (int guard = 0; guard < 100000; ++guard) {
      std::vector<Id> es;
      if (const auto it = ve.find(v); it != ve.end()) {
        for (const Id x : it->second) {
          if (x != from) {
            es.push_back(x);
          }
        }
      }
      if (es.size() != 1 || out.has(es[0])) {
        break;
      }
      out.add(es[0]);
      const Edge& e = *d.edges.get(es[0]);
      v = e.a == v ? e.b : e.a;
      from = es[0];
    }
  }
  return out.values();
}

base::Result<FollowResult> follow_me(Def& d, Id fid, std::span<const Id> path_eids) {
  const Face* fp = d.faces.get(fid);
  if (fp == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.model.choose_the_face_to_sweep"));
  }
  const auto ch = edge_chain(d, path_eids);
  if (!ch) {
    return std::unexpected(ch.error());
  }
  const bool closed = ch->closed;
  std::vector<Vec3> P = d.points(ch->verts);
  const Face f = *fp;
  const Vec3 np = face_normal(d, f);
  const Vec3 p0 = d.vert(f.loops[0][0]);
  Vec3 cen{0, 0, 0};
  {
    const std::vector<Vec3> A = d.points(f.loops[0]);
    for (const Vec3& q : A) {
      cen = add(cen, q);
    }
    cen = mul(cen, 1.0 / static_cast<double>(A.size()));
  }
  if (!closed) {
    if (dist(P.back(), cen) < dist(P[0], cen)) {
      std::ranges::reverse(P);
    }
  } else {
    std::size_t k = 0;
    for (std::size_t i = 0; i < P.size(); ++i) {
      if (dist(P[i], cen) < dist(P[k], cen)) {
        k = i;
      }
    }
    std::ranges::rotate(P, P.begin() + static_cast<std::ptrdiff_t>(k));
  }
  const std::size_t nseg = closed ? P.size() : P.size() - 1;
  if (P.size() < 2 && !closed) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_path_needs_at_least_one"));
  }
  if (nseg < 1) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_path_needs_at_least_one"));
  }
  std::vector<Vec3> dirs;
  for (std::size_t i = 0; i < nseg; ++i) {
    const Vec3 dv = norm(sub(P[(i + 1) % P.size()], P[i]));
    if (len(dv) == 0) {
      return base::fail(base::Errc::invalid, tr("m3d.model.the_path_has_an_edge_of"));
    }
    dirs.push_back(dv);
  }
  if (std::abs(dot(dirs[0], np)) < 1e-3) {
    return base::fail(base::Errc::invalid, tr("m3d.model.the_face_lies_along_the_path"));
  }
  std::vector<std::vector<Vec3>> ring = f_pts(d, f);
  std::vector<std::vector<Loop>> rings = {f.loops};
  for (std::size_t k = 1; k <= nseg; ++k) {
    const Vec3& dk = dirs[k - 1];
    Vec3 n{};
    Vec3 o{};
    if (closed && k == nseg) {
      n = np;
      o = p0;
    } else if (!closed && k == nseg) {
      n = dk;
      o = P[k];
    } else {
      const Vec3& dn = dirs[k % nseg];
      if (dot(dk, dn) < -0.999) {
        return base::fail(base::Errc::invalid, tr("m3d.model.the_path_turns_straight_back_on"));
      }
      n = norm(add(dk, dn));
      o = P[k];
    }
    const double nd = dot(n, dk);
    if (std::abs(nd) < 1e-9) {
      return base::fail(base::Errc::invalid, tr("m3d.model.the_path_turns_too_sharply_here"));
    }
    for (auto& L : ring) {
      for (Vec3& q : L) {
        q = add(q, mul(dk, dot(n, sub(o, q)) / nd));
      }
    }
    if (closed && k == nseg) {
      rings.push_back(rings[0]);
    } else {
      std::vector<Loop> r;
      for (const auto& L : ring) {
        Loop l;
        for (const Vec3& q : L) {
          l.push_back(v_add(d, q));
        }
        r.push_back(std::move(l));
      }
      rings.push_back(std::move(r));
    }
  }
  std::vector<Id> added;
  for (std::size_t k = 0; k < nseg; ++k) {
    const auto& A = rings[k];
    const auto& B = rings[k + 1];
    for (std::size_t l = 0; l < A.size(); ++l) {
      const std::size_t m = A[l].size();
      for (std::size_t j = 0; j < m; ++j) {
        const Loop q = {A[l][j], A[l][(j + 1) % m], B[l][(j + 1) % m], B[l][j]};
        OrderedSet<Id> uniq;
        for (const Id v : q) {
          uniq.add(v);
        }
        if (uniq.size() < 3) {
          continue;
        }
        added.push_back(f_add(d, {uniq.size() == 4 ? q : uniq.values()}, f.color));
      }
    }
  }
  if (closed) {
    d.faces.erase(fid);
  } else {
    added.push_back(f_add(d, rings[nseg], f.color));
  }
  std::vector<Id> body;
  if (!closed) {
    body.push_back(fid);
  }
  body.insert(body.end(), added.begin(), added.end());
  double vol = 0;
  for (const Id id : body) {
    for (const auto& [a, b, c] : face_tris(d, *d.faces.get(id))) {
      vol += dot(d.vert(a), cross(d.vert(b), d.vert(c))) / 6;
    }
  }
  if (vol < 0) {
    reverse_faces(d, body);
  }
  const auto ef = edge_faces(d);
  const std::unordered_set<Id> mine(body.begin(), body.end());
  const double c25 = base::js_cos(25 * kPi / 180);
  for (auto& [eid, e] : d.edges) {
    const auto it = ef.find(edge_key(e.a, e.b));
    if (it == ef.end() || it->second.size() != 2) {
      continue;
    }
    const auto& fs = it->second;
    if (mine.contains(fs[0]) && mine.contains(fs[1]) &&
        dot(face_normal(d, *d.faces.get(fs[0])), face_normal(d, *d.faces.get(fs[1]))) > c25) {
      e.soft = true;
    }
  }
  return FollowResult{.faces = added, .closed = closed};
}

}  // namespace cc::m3d
