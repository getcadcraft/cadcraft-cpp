// SPDX-License-Identifier: Apache-2.0
// hatch.js's boundaries (CadCraft 0.5) in C++: loopsOf (a closed object as a flat loop) and boundaryAt (the closed area
// round a point, with its islands), with the JS's order of operations, grid hash and tolerances.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/edit.hpp"
#include "prims.hpp"
#include "util.hpp"

namespace cc::cad {

using detail::g;
using detail::num;
using detail::Value;

namespace {

constexpr double kTau = std::numbers::pi * 2;
constexpr double kEps = 1e-9;
constexpr std::size_t kMaxBoundarySegs = 80000;

/// hatch.js arcPoints(p, rev): points along an arc primitive as a flat list ending at its end
std::vector<double> arc_points(double cx, double cy, double r, double a0, double a1, bool rev) {
  const double sweep = a1 - a0;
  const int n = std::max(4, static_cast<int>(std::ceil(std::abs(sweep) / (kTau / 128))));
  std::vector<double> flat;
  flat.reserve(static_cast<std::size_t>(n + 1) * 2);
  for (int i = 0; i <= n; ++i) {
    const double a = rev ? a1 - sweep * i / n : a0 + sweep * i / n;
    flat.push_back(cx + r * base::js_cos(a));
    flat.push_back(cy + r * base::js_sin(a));
  }
  return flat;
}
base::Array to_array(const std::vector<double>& v) {
  base::Array a;
  a.reserve(v.size());
  for (const double d : v) {
    a.emplace_back(d);
  }
  return a;
}

struct Seg {
  double x1, y1, x2, y2;
};

/// hatch.js crossSplit(s, t, ss, ts, tol): where two pieces cross or touch, as parameters on each
void cross_split(const Seg& s, const Seg& t, std::vector<double>& ss, std::vector<double>& ts, double tol) {
  const double ax = s.x1;
  const double ay = s.y1;
  const double cx = t.x1;
  const double cy = t.y1;
  const double rx = s.x2 - ax;
  const double ry = s.y2 - ay;
  const double qx = t.x2 - cx;
  const double qy = t.y2 - cy;
  const double den = rx * qy - ry * qx;
  const double lr = base::js_hypot(rx, ry);
  const double lq = base::js_hypot(qx, qy);
  const auto on_seg = [tol](double px, double py, double x1, double y1, double vx, double vy,
                            double l) -> std::optional<double> {
    const double u = ((px - x1) * vx + (py - y1) * vy) / (l * l);
    const double d = std::abs((px - x1) * vy - (py - y1) * vx) / l;
    if (d <= tol && u > -tol / l && u < 1 + tol / l) {
      return std::max(0.0, std::min(1.0, u));
    }
    return std::nullopt;
  };
  if (std::abs(den) <= 1e-12 * lr * lq) {
    // parallel: when they lie on one line, each splits the other at its ends
    for (const auto& [px, py] : {std::pair{cx, cy}, std::pair{t.x2, t.y2}}) {
      if (const auto u = on_seg(px, py, ax, ay, rx, ry, lr)) {
        ss.push_back(*u);
      }
    }
    for (const auto& [px, py] : {std::pair{ax, ay}, std::pair{s.x2, s.y2}}) {
      if (const auto u = on_seg(px, py, cx, cy, qx, qy, lq)) {
        ts.push_back(*u);
      }
    }
    return;
  }
  const double u = ((cx - ax) * qy - (cy - ay) * qx) / den;
  const double v = ((cx - ax) * ry - (cy - ay) * rx) / den;
  const double eu = tol / lr;
  const double ev = tol / lq;
  if (u < -eu || u > 1 + eu || v < -ev || v > 1 + ev) {
    return;
  }
  ss.push_back(std::max(0.0, std::min(1.0, u)));
  ts.push_back(std::max(0.0, std::min(1.0, v)));
}

struct Vert {
  double x, y;
  std::vector<std::size_t> out;  // half-edges leaving it
  std::size_t i;
};
struct Half {
  std::size_t from, to;
  double ang;
  std::size_t twin;
  bool used = false;
};

bool in_ring(P2 q, const std::vector<std::size_t>& ring, const std::vector<Vert>& verts) {
  bool inside = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const Vert& a = verts[ring[i]];
    const Vert& b = verts[ring[j]];
    if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

}  // namespace

std::optional<base::Array> loops_of(const Value& e) {
  const std::string t = detail::str(g(e, "type"));
  if (t == "circle") {
    // array_of, not Array{Array}: MSVC takes a braced list of one Array as a copy of it
    return base::array_of(to_array(arc_points(num(g(e, "cx")), num(g(e, "cy")), num(g(e, "r")), 0, kTau, false)));
  }
  if (t == "poly" && g(e, "closed").truthy() && detail::len(g(e, "pts")) >= 2) {
    std::vector<Prim> ps;
    detail::poly_prims(e, ps);
    std::vector<double> flat;
    for (const Prim& p : ps) {
      if (!p.arc) {
        flat.push_back(p.x1);
        flat.push_back(p.y1);
        continue;
      }
      std::vector<double> pts = arc_points(p.cx, p.cy, p.r, p.a0, p.a1, p.rev);
      pts.resize(pts.size() - 2);
      flat.insert(flat.end(), pts.begin(), pts.end());
    }
    if (flat.size() < 6) {
      return std::nullopt;
    }
    return base::array_of(to_array(flat));
  }
  if (t == "ellipse" && num(g(e, "t1")) - num(g(e, "t0")) >= kTau - 1e-9) {
    const std::vector<P2> pts = ellipse_points(e, 144);
    std::vector<double> flat;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
      flat.push_back(pts[i].x);
      flat.push_back(pts[i].y);
    }
    return base::array_of(to_array(flat));
  }
  return std::nullopt;
}

Boundary boundary_at(const std::vector<const Value*>& entities, P2 pt, const BlockLookup& blocks,
                     const std::optional<Bounds>& within) {
  // 1. every boundary-making object as straight pieces
  std::vector<Seg> segs;
  const auto add = [&segs](double x1, double y1, double x2, double y2) {
    if (std::abs(x1 - x2) > kEps || std::abs(y1 - y2) > kEps) {
      segs.push_back({x1, y1, x2, y2});
    }
  };
  for (const Value* e : entities) {
    const std::string t = detail::str(g(*e, "type"));
    if (t == "text" || t == "point" || t == "hatch" || t == "dim" || t == "mleader" || t == "viewport") {
      continue;
    }
    if (within) {
      const Bounds b = entity_bounds(*e, blocks);
      if (!b.valid() || b.max_x < within->min_x || b.min_x > within->max_x || b.max_y < within->min_y ||
          b.min_y > within->max_y) {
        continue;
      }
    }
    for (const Prim& p : entity_prims(*e, blocks)) {
      if (p.box) {
        continue;
      }
      if (!p.arc) {
        add(p.x1, p.y1, p.x2, p.y2);
      } else {
        const std::vector<double> f = arc_points(p.cx, p.cy, p.r, p.a0, p.a1, false);
        for (std::size_t i = 0; i + 3 < f.size(); i += 2) {
          add(f[i], f[i + 1], f[i + 2], f[i + 3]);
        }
      }
    }
    if (segs.size() > kMaxBoundarySegs) {
      return {.error = "large", .loops = {}};
    }
  }
  if (segs.empty()) {
    return {.error = "none", .loops = {}};
  }
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = min_x;
  double max_x = -min_x;
  double max_y = -min_x;
  for (const Seg& s : segs) {
    min_x = std::min({min_x, s.x1, s.x2});
    max_x = std::max({max_x, s.x1, s.x2});
    min_y = std::min({min_y, s.y1, s.y2});
    max_y = std::max({max_y, s.y1, s.y2});
  }
  const double tol = std::max({max_x - min_x, max_y - min_y, 1.0}) * 1e-9;

  // 2. split every piece where others cross or touch it (a grid keeps this near linear)
  const std::size_t n_segs = segs.size();
  std::vector<std::vector<double>> splits(n_segs, std::vector<double>{0, 1});
  const double cs =
      std::max({max_x - min_x, max_y - min_y, 1e-9}) / std::max(1.0, std::sqrt(static_cast<double>(n_segs)));
  // the JS key: i * 73856093 ^ j * 19349663 (both sides ToInt32): cells that share a key share a list
  std::vector<std::int32_t> order;
  std::unordered_map<std::int32_t, std::vector<std::size_t>> cells;
  for (std::size_t idx = 0; idx < n_segs; ++idx) {
    const Seg& s = segs[idx];
    const double i0 = std::floor((std::min(s.x1, s.x2) - min_x) / cs);
    const double i1 = std::floor((std::max(s.x1, s.x2) - min_x) / cs);
    const double j0 = std::floor((std::min(s.y1, s.y2) - min_y) / cs);
    const double j1 = std::floor((std::max(s.y1, s.y2) - min_y) / cs);
    // cells run 0 … √n (cs is the extent over √n); a NaN or infinite piece takes none, as the JS loop does
    if (!std::isfinite(i0) || !std::isfinite(i1) || !std::isfinite(j0) || !std::isfinite(j1)) {
      continue;
    }
    for (auto i = static_cast<std::int64_t>(i0); i <= static_cast<std::int64_t>(i1); ++i) {
      for (auto j = static_cast<std::int64_t>(j0); j <= static_cast<std::int64_t>(j1); ++j) {
        const std::int32_t k =
            base::js_to_int32(static_cast<double>(i) * 73856093) ^ base::js_to_int32(static_cast<double>(j) * 19349663);
        auto [it, fresh] = cells.try_emplace(k);
        if (fresh) {
          order.push_back(k);
        }
        it->second.push_back(idx);
      }
    }
  }
  std::unordered_set<std::uint64_t> done;
  for (const std::int32_t k : order) {
    const std::vector<std::size_t>& list = cells[k];
    for (std::size_t a = 0; a < list.size(); ++a) {
      for (std::size_t b = a + 1; b < list.size(); ++b) {
        const std::size_t i = list[a];
        const std::size_t j = list[b];
        const std::uint64_t pk =
            i < j ? (static_cast<std::uint64_t>(i) * n_segs) + j : (static_cast<std::uint64_t>(j) * n_segs) + i;
        if (!done.insert(pk).second) {
          continue;
        }
        cross_split(segs[i], segs[j], splits[i], splits[j], tol);
      }
    }
  }

  // 3. a planar graph: vertices merged within the tolerance, edges once each
  std::map<std::pair<double, double>, std::vector<std::size_t>> vid;
  std::vector<Vert> verts;
  const double snap = tol * 1000;
  const auto vertex = [&](double x, double y) {
    const double gx = base::js_round(x / snap);
    const double gy = base::js_round(y / snap);
    for (int i = -1; i <= 1; ++i) {
      for (int j = -1; j <= 1; ++j) {
        const auto it = vid.find({gx + i, gy + j});
        if (it == vid.end()) {
          continue;
        }
        for (const std::size_t v : it->second) {
          if (std::abs(verts[v].x - x) <= snap && std::abs(verts[v].y - y) <= snap) {
            return v;
          }
        }
      }
    }
    const std::size_t v = verts.size();
    verts.push_back({x, y, {}, v});
    vid[{gx, gy}].push_back(v);
    return v;
  };
  std::set<std::pair<std::size_t, std::size_t>> edge_set;
  std::vector<std::pair<std::size_t, std::size_t>> edges;
  for (std::size_t i = 0; i < n_segs; ++i) {
    const Seg& s = segs[i];
    std::vector<double> ts = splits[i];
    std::ranges::sort(ts);
    const auto dup = std::ranges::unique(ts);
    ts.erase(dup.begin(), dup.end());
    std::size_t prev = vertex(s.x1 + (s.x2 - s.x1) * ts[0], s.y1 + (s.y2 - s.y1) * ts[0]);
    for (std::size_t k = 1; k < ts.size(); ++k) {
      const std::size_t v = vertex(s.x1 + (s.x2 - s.x1) * ts[k], s.y1 + (s.y2 - s.y1) * ts[k]);
      if (v != prev && edge_set.insert(prev < v ? std::pair{prev, v} : std::pair{v, prev}).second) {
        edges.emplace_back(prev, v);
      }
      prev = v;
    }
  }
  // drop dangling pieces: they bound nothing
  std::vector<int> deg(verts.size(), 0);
  for (const auto& [a, b] : edges) {
    ++deg[a];
    ++deg[b];
  }
  std::vector<bool> alive(edges.size(), true);
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t i = 0; i < edges.size(); ++i) {
      const auto [a, b] = edges[i];
      if (alive[i] && (deg[a] < 2 || deg[b] < 2)) {
        alive[i] = false;
        --deg[a];
        --deg[b];
        changed = true;
      }
    }
  }
  std::vector<Half> half;
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (!alive[i]) {
      continue;
    }
    const auto [a, b] = edges[i];
    const Vert& va = verts[a];
    const Vert& vb = verts[b];
    const std::size_t h1 = half.size();
    half.push_back({a, b, base::js_atan2(vb.y - va.y, vb.x - va.x), h1 + 1});
    half.push_back({b, a, base::js_atan2(va.y - vb.y, va.x - vb.x), h1});
    verts[a].out.push_back(h1);
    verts[b].out.push_back(h1 + 1);
  }
  if (half.empty()) {
    return {.error = "open", .loops = {}};
  }
  for (Vert& v : verts) {
    std::ranges::stable_sort(v.out, [&half](std::size_t p, std::size_t q) { return half[p].ang < half[q].ang; });
  }

  // 4. every face: from a half-edge u→v, turn to the edge leaving v just clockwise of v→u (face on the left)
  const auto next = [&](std::size_t h) {
    const std::vector<std::size_t>& out = verts[half[h].to].out;
    const auto it = std::ranges::find(out, half[h].twin);
    const auto i = static_cast<std::size_t>(it - out.begin());
    return out[(i + out.size() - 1) % out.size()];
  };
  struct Face {
    std::vector<std::size_t> ring;
    double area = 0;
    int comp = -1;
  };
  std::vector<Face> faces;
  for (std::size_t h0 = 0; h0 < half.size(); ++h0) {
    if (half[h0].used) {
      continue;
    }
    Face f;
    std::size_t h = h0;
    std::size_t guard = 0;
    do {
      half[h].used = true;
      f.ring.push_back(half[h].from);
      h = next(h);
    } while (h != h0 && ++guard < half.size() + 2);
    double area = 0;
    for (std::size_t i = 0; i < f.ring.size(); ++i) {
      const Vert& p = verts[f.ring[i]];
      const Vert& q = verts[f.ring[(i + 1) % f.ring.size()]];
      area += p.x * q.y - q.x * p.y;
    }
    f.area = area / 2;
    faces.push_back(std::move(f));
  }
  // components (connected pieces of the graph), for islands
  std::vector<int> comp(verts.size(), -1);
  int n_comp = 0;
  for (std::size_t i = 0; i < verts.size(); ++i) {
    if (comp[i] >= 0 || verts[i].out.empty()) {
      continue;
    }
    std::vector<std::size_t> stack{i};
    comp[i] = n_comp;
    while (!stack.empty()) {
      const std::size_t u = stack.back();
      stack.pop_back();
      for (const std::size_t h : verts[u].out) {
        if (comp[half[h].to] < 0) {
          comp[half[h].to] = n_comp;
          stack.push_back(half[h].to);
        }
      }
    }
    ++n_comp;
  }
  for (Face& f : faces) {
    f.comp = comp[f.ring[0]];
  }

  // 5. the smallest bounded face round the point
  const Face* outer = nullptr;
  for (const Face& f : faces) {
    if (f.area > 0 && (outer == nullptr || f.area < outer->area) && in_ring(pt, f.ring, verts)) {
      outer = &f;
    }
  }
  if (outer == nullptr) {
    return {.error = "open", .loops = {}};
  }
  const auto flat_of = [&verts](const std::vector<std::size_t>& ring) {
    base::Array a;
    a.reserve(ring.size() * 2);
    for (const std::size_t v : ring) {
      a.emplace_back(verts[v].x);
      a.emplace_back(verts[v].y);
    }
    return a;
  };
  // islands: the outline of every other piece of the graph that lies inside it (its outside face, clockwise)
  Boundary out;
  out.loops.emplace_back(flat_of(outer->ring));
  for (const Face& f : faces) {
    if (f.area >= 0 || f.comp == outer->comp) {
      continue;
    }
    const Vert& first = verts[f.ring[0]];
    if (in_ring({first.x, first.y}, outer->ring, verts) && !in_ring(pt, f.ring, verts)) {
      out.loops.emplace_back(flat_of(f.ring));
    }
  }
  return out;
}

}  // namespace cc::cad
