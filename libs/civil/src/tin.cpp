// SPDX-License-Identifier: Apache-2.0
// tin.js (CivilCraft 0.5), ported function by function. Iteration orders follow the JS (Map insertion order, stable
// sorts), so every vertex, triangle and contour comes out in the same order.
#include "cc/civil/tin.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>

#include "cc/appkit/jsops.hpp"
#include "cc/base/js_math.hpp"
#include "cc/civil/delaunator.hpp"
#include "cc/civil/predicates.hpp"

namespace cc::civil {
namespace {

using appkit::js_round;
using appkit::num_str;
constexpr double kInf = std::numeric_limits<double>::infinity();

std::size_t nxt(std::size_t e) {
  return e % 3 == 2 ? e - 2 : e + 1;
}
std::size_t prv(std::size_t e) {
  return e % 3 == 0 ? e + 2 : e - 1;
}
double hyp(double a, double b) {
  return base::js_hypot(a, b);
}
double jmin(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b);
}
double jmax(double a, double b) {
  return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b);
}

/// A rounded coordinate as a JS map key: String(Math.round(v)) — -0 and 0 are one key, NaN one of its own.
struct KeyD {
  double v;
  friend bool operator<(KeyD a, KeyD b) {
    const bool an = std::isnan(a.v);
    const bool bn = std::isnan(b.v);
    if (an || bn) {
      return !an && bn;
    }
    return a.v < b.v;
  }
};
using VKey = std::pair<KeyD, KeyD>;
VKey key_of(double x, double y) {
  double kx = js_round(x / kSnap);
  double ky = js_round(y / kSnap);
  if (kx == 0) {
    kx = 0;
  }
  if (ky == 0) {
    ky = 0;
  }
  return {KeyD{kx}, KeyD{ky}};
}

std::string tr(const i18n::Catalog& w, const char* key) {
  return w.t(key);
}
std::string trn(const i18n::Catalog& w, const char* key, double n) {
  return w.tn(key, n, {{"n", num_str(n)}});
}

// ---------------------------------------------------------------------------------------------- segment crossings

struct Hit {
  double t, u;
};
std::optional<Hit> seg_hit(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
  const double rx = bx - ax;
  const double ry = by - ay;
  const double sx = dx - cx;
  const double sy = dy - cy;
  const double den = rx * sy - ry * sx;
  if (std::abs(den) < 1e-12) {
    return std::nullopt;
  }
  const double t = ((cx - ax) * sy - (cy - ay) * sx) / den;
  const double u = ((cx - ax) * ry - (cy - ay) * rx) / den;
  constexpr double kEps = 1e-9;
  if (t <= kEps || t >= 1 - kEps || u <= kEps || u >= 1 - kEps) {
    return std::nullopt;
  }
  return Hit{t, u};  // (NaN passes the test above, as in the JS)
}

struct Seg {
  std::int32_t a, b;
  bool brk;
};

struct Verts {
  std::vector<double> X, Y, Z;
  std::map<VKey, std::int32_t> key;
};

std::vector<std::pair<std::int32_t, std::int32_t>> split_crossings(Verts& v, const std::vector<Seg>& segs,
                                                                   TinReport& report) {
  const std::size_t n = segs.size();
  struct Cut {
    double t;
    std::int32_t v;
  };
  std::vector<std::vector<Cut>> cuts(n);
  auto X = [&](std::int32_t i) { return v.X[static_cast<std::size_t>(i)]; };
  auto Y = [&](std::int32_t i) { return v.Y[static_cast<std::size_t>(i)]; };
  auto Z = [&](std::int32_t i) { return v.Z[static_cast<std::size_t>(i)]; };
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  auto min_x = [&](std::size_t i) { return jmin(X(segs[i].a), X(segs[i].b)); };
  appkit::js_sort(order, [&](std::size_t i, std::size_t j) { return min_x(i) - min_x(j); });
  std::vector<std::size_t> active;
  for (const std::size_t i : order) {
    const std::int32_t a = segs[i].a;
    const std::int32_t b = segs[i].b;
    const double mx = jmin(X(a), X(b));
    for (std::size_t k = active.size(); k-- > 0;) {
      const Seg& s = segs[active[k]];
      if (jmax(X(s.a), X(s.b)) < mx - kSnap) {
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(k));
      }
    }
    for (const std::size_t j : active) {
      const std::int32_t c = segs[j].a;
      const std::int32_t d = segs[j].b;
      if (a == c || a == d || b == c || b == d) {
        continue;
      }
      const auto hit = seg_hit(X(a), Y(a), X(b), Y(b), X(c), Y(c), X(d), Y(d));
      if (!hit) {
        continue;
      }
      const double x = X(a) + (X(b) - X(a)) * hit->t;
      const double y = Y(a) + (Y(b) - Y(a)) * hit->t;
      const double zi = Z(a) + (Z(b) - Z(a)) * hit->t;
      const double zj = Z(c) + (Z(d) - Z(c)) * hit->u;
      const VKey k = key_of(x, y);
      const bool bi = segs[i].brk;
      const bool bj = segs[j].brk;
      double z = zi;  // a breakline gives the crossing its elevation (the first given wins); a boundary does not
      if (bi && bj) {
        z = i < j ? zi : zj;
      } else if (!bi && bj) {
        z = zj;
      }
      std::int32_t vv = 0;
      if (const auto it = v.key.find(k); it != v.key.end()) {
        vv = it->second;
      } else {
        vv = static_cast<std::int32_t>(v.X.size());
        v.key.emplace(k, vv);
        v.X.push_back(x);
        v.Y.push_back(y);
        v.Z.push_back(z);
      }
      if (bi && bj && std::abs(zi - zj) > 0.01) {
        report.z_conflicts++;
      }
      report.crossings++;
      cuts[i].push_back({hit->t, vv});
      cuts[j].push_back({hit->u, vv});
    }
    active.push_back(i);
  }
  std::vector<std::pair<std::int32_t, std::int32_t>> out;
  for (std::size_t i = 0; i < n; ++i) {
    auto& c = cuts[i];
    appkit::js_sort(c, [](const Cut& p, const Cut& q) { return p.t - q.t; });
    std::int32_t from = segs[i].a;
    for (const Cut& cut : c) {
      if (cut.v != from) {
        out.emplace_back(from, cut.v);
      }
      from = cut.v;
    }
    if (from != segs[i].b) {
      out.emplace_back(from, segs[i].b);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------- the mesh

class Mesh {
 public:
  Mesh(std::span<const double> x, std::span<const double> y, std::vector<std::int32_t> tri,
       std::vector<std::int32_t> he)
      : X(x), Y(y), tri(std::move(tri)), he(std::move(he)), con(this->he.size(), 0), vmap(x.size(), -1) {
    for (std::size_t e = 0; e < this->tri.size(); ++e) {
      vmap[at(e)] = static_cast<std::int32_t>(e);
    }
  }
  std::span<const double> X;  // the vertices outlive the mesh
  std::span<const double> Y;
  std::vector<std::int32_t> tri, he;
  std::vector<std::uint8_t> con;
  std::vector<std::int32_t> vmap;

  [[nodiscard]] std::size_t at(std::size_t e) const { return static_cast<std::size_t>(tri[e]); }
  [[nodiscard]] double orient(std::size_t a, std::size_t b, std::size_t c) const {
    return -orient2d(X[a], Y[a], X[b], Y[b], X[c], Y[c]);
  }
  void mark(std::size_t e) {
    con[e] = 1;
    const std::int32_t o = he[e];
    if (o >= 0) {
      con[static_cast<std::size_t>(o)] = 1;
    }
  }

  [[nodiscard]] std::vector<std::size_t> around(std::size_t v) const {
    std::vector<std::size_t> out;
    const std::int32_t s = vmap[v];
    if (s < 0) {
      return out;
    }
    const auto start = static_cast<std::size_t>(s);
    std::size_t e = start;
    bool open = false;
    std::size_t guard = 0;
    do {
      out.push_back(e);
      const std::int32_t o = he[prv(e)];
      if (o < 0) {
        open = true;
        break;
      }
      e = static_cast<std::size_t>(o);
    } while (e != start && ++guard < he.size() + 3);
    if (open) {
      e = start;
      for (guard = 0; guard < he.size() + 3; ++guard) {
        const std::int32_t o = he[e];
        if (o < 0) {
          break;
        }
        e = nxt(static_cast<std::size_t>(o));
        if (e == start) {
          break;
        }
        out.push_back(e);
      }
    }
    return out;
  }

  struct Walk {
    bool ok = false;
    std::optional<std::size_t> edge;
    std::vector<std::size_t> crossed;
    std::optional<std::pair<std::size_t, std::size_t>> rest;
  };

  [[nodiscard]] int side(std::size_t p, std::size_t q, std::size_t v) const {
    const double o = orient(p, q, v);
    if (o == 0) {
      return 0;
    }
    const double L = hyp(X[q] - X[p], Y[q] - Y[p]);
    if (std::abs(o) / L < 1e-7 * (1 + L)) {
      return 0;
    }
    return o > 0 ? 1 : -1;
  }
  [[nodiscard]] bool ahead(std::size_t p, std::size_t q, std::size_t v) const {
    const double dx = X[q] - X[p];
    const double dy = Y[q] - Y[p];
    const double t = ((X[v] - X[p]) * dx + (Y[v] - Y[p]) * dy) / (dx * dx + dy * dy);
    return t > 0 && t < 1;
  }

  [[nodiscard]] Walk walk(std::size_t p, std::size_t q) const {
    Walk r;
    std::optional<std::size_t> first;
    for (const std::size_t e : around(p)) {
      const std::size_t b = at(nxt(e));
      const std::size_t c = at(prv(e));
      if (b == q) {
        r.ok = true;
        r.edge = e;
        return r;
      }
      if (c == q) {
        r.ok = true;
        r.edge = prv(e);
        return r;
      }
      const int ob = side(p, q, b);
      const int oc = side(p, q, c);
      if (ob == 0 && ahead(p, q, b)) {
        r.ok = true;
        r.edge = e;
        r.rest = std::pair{b, q};
        return r;
      }
      if (oc == 0 && ahead(p, q, c)) {
        r.ok = true;
        r.edge = prv(e);
        r.rest = std::pair{c, q};
        return r;
      }
      if (ob < 0 && oc > 0) {
        first = nxt(e);
        break;
      }
    }
    if (!first) {
      return r;
    }
    std::size_t e = *first;
    for (int guard = 0; guard < 1000000; ++guard) {
      if (con[e] != 0) {
        return Walk{};
      }
      r.crossed.push_back(e);
      const std::int32_t o = he[e];
      if (o < 0) {
        return Walk{};
      }
      const auto ou = static_cast<std::size_t>(o);
      const std::size_t v = at(prv(ou));
      if (v == q) {
        r.ok = true;
        return r;
      }
      const int ov = side(p, q, v);
      if (ov == 0) {
        r.ok = true;
        r.rest = std::pair{v, q};
        return r;
      }
      e = ov > 0 ? nxt(ou) : prv(ou);
    }
    return Walk{};
  }

  bool constrain(std::size_t a0, std::size_t b0) {
    std::vector<std::pair<std::size_t, std::size_t>> todo{{a0, b0}};
    int guard = 0;
    while (!todo.empty()) {
      if (++guard > 100000) {
        return false;
      }
      const auto [p, q] = todo.back();
      todo.pop_back();
      if (p == q) {
        continue;
      }
      if (vmap[p] < 0 || vmap[q] < 0) {
        return false;
      }
      const Walk r = walk(p, q);
      if (!r.ok) {
        return false;
      }
      if (r.edge) {
        mark(*r.edge);
        if (r.rest) {
          todo.push_back(*r.rest);
        }
        continue;
      }
      if (!flip_out(p, q, r.crossed)) {
        return false;
      }
      if (r.rest) {
        todo.push_back(*r.rest);
      }
    }
    return true;
  }

  [[nodiscard]] bool crosses(std::size_t p, std::size_t q, std::size_t u, std::size_t v) const {
    const double a = orient(p, q, u);
    const double b = orient(p, q, v);
    const double c = orient(u, v, p);
    const double d = orient(u, v, q);
    return ((a > 0 && b < 0) || (a < 0 && b > 0)) && ((c > 0 && d < 0) || (c < 0 && d > 0));
  }
  [[nodiscard]] bool convex(std::size_t e) const {
    const std::int32_t o = he[e];
    if (o < 0) {
      return false;
    }
    const std::size_t a = at(e);
    const std::size_t b = at(nxt(e));
    const std::size_t c = at(prv(e));
    const std::size_t d = at(prv(static_cast<std::size_t>(o)));
    const double s1 = orient(c, d, a);
    const double s2 = orient(c, d, b);
    return (s1 > 0 && s2 < 0) || (s1 < 0 && s2 > 0);
  }
  [[nodiscard]] bool illegal(std::size_t e) const {
    const std::int32_t o = he[e];
    if (o < 0) {
      return false;
    }
    const std::size_t a = at(e);
    const std::size_t b = at(nxt(e));
    const std::size_t c = at(prv(e));
    const std::size_t d = at(prv(static_cast<std::size_t>(o)));
    return incircle(X[a], Y[a], X[b], Y[b], X[c], Y[c], X[d], Y[d]) > 0 && convex(e);
  }

  /// Flips the diagonal e; returns the new diagonal's half-edge in e's old triangle.
  std::size_t flip(std::size_t e) {
    const auto o = static_cast<std::size_t>(he[e]);
    const std::size_t bot = prv(e);
    const std::size_t lft = nxt(e);
    const std::size_t top = prv(o);
    const std::size_t rgt = nxt(o);
    const std::int32_t adj_bot = he[bot];
    const std::int32_t adj_top = he[top];
    const std::uint8_t c_bot = con[bot];
    const std::uint8_t c_top = con[top];
    tri[e] = tri[top];
    he[e] = adj_top;
    if (adj_top >= 0) {
      he[static_cast<std::size_t>(adj_top)] = static_cast<std::int32_t>(e);
    }
    con[e] = c_top;
    tri[o] = tri[bot];
    he[o] = adj_bot;
    if (adj_bot >= 0) {
      he[static_cast<std::size_t>(adj_bot)] = static_cast<std::int32_t>(o);
    }
    con[o] = c_bot;
    he[bot] = static_cast<std::int32_t>(top);
    he[top] = static_cast<std::int32_t>(bot);
    con[bot] = 0;
    con[top] = 0;
    vmap[at(e)] = static_cast<std::int32_t>(e);
    vmap[at(lft)] = static_cast<std::int32_t>(lft);
    vmap[at(o)] = static_cast<std::int32_t>(o);
    vmap[at(rgt)] = static_cast<std::int32_t>(rgt);
    return bot;
  }

  [[nodiscard]] std::int64_t find_edge(std::size_t u, std::size_t v) const {
    for (const std::size_t e : around(u)) {
      if (at(nxt(e)) == v) {
        return static_cast<std::int64_t>(e);
      }
      if (at(prv(e)) == v) {
        return static_cast<std::int64_t>(prv(e));
      }
    }
    return -1;
  }

  using Pair = std::pair<std::size_t, std::size_t>;
  void push_edge(std::vector<Pair>& st, std::size_t f) const { st.emplace_back(at(f), at(nxt(f))); }

  void lawson(std::vector<Pair>& stack, std::size_t limit) {
    std::size_t n = 0;
    while (!stack.empty() && n++ < limit) {
      const auto [u, v] = stack.back();
      stack.pop_back();
      const std::int64_t e = find_edge(u, v);
      if (e < 0) {
        continue;
      }
      const auto eu = static_cast<std::size_t>(e);
      if (con[eu] != 0 || he[eu] < 0 || !illegal(eu)) {
        continue;
      }
      const std::size_t ne = flip(eu);
      const auto o = static_cast<std::size_t>(he[ne]);
      for (const std::size_t f : {nxt(ne), prv(ne), nxt(o), prv(o)}) {
        push_edge(stack, f);
      }
    }
  }

  bool flip_out(std::size_t p, std::size_t /*q0*/, const std::vector<std::size_t>& crossed) {
    std::size_t q = 0;
    {
      const std::size_t last = crossed.back();
      const auto o = static_cast<std::size_t>(he[last]);
      q = at(prv(o));
    }
    std::deque<std::size_t> queue(crossed.begin(), crossed.end());
    std::vector<std::size_t> made;
    std::size_t guard = 0;
    const std::size_t m = crossed.size() + 10;
    while (!queue.empty()) {
      if (++guard > 50 * m * m) {
        return false;
      }
      const std::size_t e = queue.front();
      queue.pop_front();
      if (!convex(e)) {
        queue.push_back(e);
        continue;
      }
      const auto o = static_cast<std::size_t>(he[e]);
      const std::size_t bot = prv(e);
      const std::size_t top = prv(o);
      for (std::size_t& x : queue) {
        if (x == bot) {
          x = o;
        } else if (x == top) {
          x = e;
        }
      }
      const std::size_t ne = flip(e);
      const std::size_t u = at(ne);
      const std::size_t v = at(nxt(ne));
      if (u != p && u != q && v != p && v != q && crosses(p, q, u, v)) {
        queue.push_back(ne);
      } else {
        made.push_back(ne);
      }
    }
    bool found = false;
    for (const std::size_t e : around(p)) {
      if (at(nxt(e)) == q) {
        mark(e);
        found = true;
        break;
      }
      if (at(prv(e)) == q) {
        mark(prv(e));
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
    std::vector<Pair> stack;
    for (const std::size_t e : made) {
      const std::int32_t o = he[e];
      for (const std::size_t f : {e, nxt(e), prv(e)}) {
        push_edge(stack, f);
      }
      if (o >= 0) {
        const auto ou = static_cast<std::size_t>(o);
        for (const std::size_t f : {nxt(ou), prv(ou)}) {
          push_edge(stack, f);
        }
      }
    }
    lawson(stack, 200000);
    return true;
  }

  void legalize_all() {
    std::vector<Pair> stack;
    for (std::size_t e = 0; e < tri.size(); ++e) {
      if (std::cmp_greater(he[e], e)) {
        push_edge(stack, e);
      }
    }
    lawson(stack, 50 * tri.size());
  }

  void peel(std::vector<std::uint8_t>& keep, double max_edge) const {
    auto len = [&](std::size_t e) { return hyp(X[at(nxt(e))] - X[at(e)], Y[at(nxt(e))] - Y[at(e)]); };
    auto outer = [&](std::size_t e) {
      const std::int32_t o = he[e];
      return o < 0 || keep[static_cast<std::size_t>(o) / 3] == 0;
    };
    std::vector<std::size_t> stack;
    for (std::size_t e = 0; e < tri.size(); ++e) {
      if (keep[e / 3] != 0 && outer(e)) {
        stack.push_back(e);
      }
    }
    while (!stack.empty()) {
      const std::size_t e = stack.back();
      stack.pop_back();
      const std::size_t t = e / 3;
      if (keep[t] == 0 || con[e] != 0 || !outer(e) || len(e) <= max_edge) {
        continue;
      }
      keep[t] = 0;
      for (const std::size_t f : {3 * t, 3 * t + 1, 3 * t + 2}) {
        const std::int32_t o = he[f];
        if (o >= 0 && keep[static_cast<std::size_t>(o) / 3] != 0) {
          stack.push_back(static_cast<std::size_t>(o));
        }
      }
    }
  }

  [[nodiscard]] std::vector<std::pair<std::int32_t, std::int32_t>> constrained_pairs() const {
    std::vector<std::pair<std::int32_t, std::int32_t>> out;
    for (std::size_t e = 0; e < tri.size(); ++e) {
      if (con[e] != 0 && (he[e] < 0 || std::cmp_less(e, he[e]))) {
        out.emplace_back(tri[e], tri[nxt(e)]);
      }
    }
    return out;
  }
};

TinReport fresh_report() {
  return {};
}

Built triangulate(const BuildInput& in, const std::optional<std::vector<P3>>& boundary, double max_edge,
                  TinReport report, const i18n::Catalog& w) {
  double ox = kInf;
  double oy = kInf;
  for (const TinPoint& p : in.points) {
    if (p.z && std::isfinite(*p.z)) {
      ox = std::min(ox, p.x);
      oy = std::min(oy, p.y);
    }
  }
  for (const Breakline& b : in.breaklines) {
    for (const P3& p : b.pts) {
      ox = std::min(ox, p.x);  // (p.x < ox ? p.x : ox, as the JS's if)
      oy = std::min(oy, p.y);
    }
  }
  if (!std::isfinite(ox)) {
    report.errors.push_back(tr(w, "tin.err.noz"));
    return {nullptr, std::move(report)};
  }
  ox = std::floor(ox);
  oy = std::floor(oy);

  Verts v;
  auto add_v = [&](double x, double y, double z, bool hard) -> std::int32_t {
    const double lx = x - ox;
    const double ly = y - oy;
    const VKey k = key_of(lx, ly);
    if (const auto it = v.key.find(k); it != v.key.end()) {
      const auto at = static_cast<std::size_t>(it->second);
      if (std::abs(v.Z[at] - z) > 1e-3) {
        report.z_conflicts++;
        if (hard) {
          v.Z[at] = z;
        }
      } else if (!hard) {
        report.merged++;
      }
      return it->second;
    }
    const auto id = static_cast<std::int32_t>(v.X.size());
    v.key.emplace(k, id);
    v.X.push_back(lx);
    v.Y.push_back(ly);
    v.Z.push_back(z);
    return id;
  };
  for (const TinPoint& p : in.points) {
    if (!p.z || !std::isfinite(*p.z)) {
      report.skipped_no_z++;
      continue;
    }
    (void)add_v(p.x, p.y, *p.z, false);
  }
  std::vector<Seg> segs;
  for (const Breakline& b : in.breaklines) {
    for (std::size_t i = 0; i + 1 < b.pts.size(); ++i) {
      const std::int32_t a = add_v(b.pts[i].x, b.pts[i].y, b.pts[i].z, true);
      const std::int32_t c = add_v(b.pts[i + 1].x, b.pts[i + 1].y, b.pts[i + 1].z, true);
      if (a != c) {
        segs.push_back({a, c, true});
      }
    }
  }
  const std::size_t n_break = segs.size();
  if (boundary) {
    const double before = report.merged;
    std::vector<std::int32_t> ids;
    ids.reserve(boundary->size());
    for (const P3& p : *boundary) {
      ids.push_back(add_v(p.x, p.y, p.z, false));
    }
    report.merged = before;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const std::int32_t a = ids[i];
      const std::int32_t c = ids[(i + 1) % ids.size()];
      if (a != c) {
        segs.push_back({a, c, false});
      }
    }
  }
  const auto pieces = split_crossings(v, segs, report);

  if (v.X.size() < 3) {
    report.errors.push_back(tr(w, "tin.err.three"));
    return {nullptr, std::move(report)};
  }
  std::vector<double> coords(v.X.size() * 2);
  for (std::size_t i = 0; i < v.X.size(); ++i) {
    coords[2 * i] = v.X[i];
    coords[2 * i + 1] = -v.Y[i];
  }
  Delaunay del = delaunator(coords);
  if (del.triangles.empty()) {
    report.errors.push_back(tr(w, "tin.err.line"));
    return {nullptr, std::move(report)};
  }
  Mesh mesh(v.X, v.Y, std::move(del.triangles), std::move(del.halfedges));
  for (const auto& [a, b] : pieces) {
    if (!mesh.constrain(static_cast<std::size_t>(a), static_cast<std::size_t>(b))) {
      report.constraint_failures++;
    }
  }
  if (!pieces.empty()) {
    mesh.legalize_all();
  }
  if (report.constraint_failures != 0) {
    report.notes.push_back(trn(w, "tin.constraint", report.constraint_failures));
  }
  std::vector<std::uint8_t> keep(mesh.tri.size() / 3, 1);
  if (boundary) {
    std::vector<P2> poly;
    poly.reserve(boundary->size());
    for (const P3& p : *boundary) {
      poly.push_back({p.x - ox, p.y - oy});
    }
    for (std::size_t t = 0; t < keep.size(); ++t) {
      const std::size_t a = mesh.at(3 * t);
      const std::size_t b = mesh.at(3 * t + 1);
      const std::size_t c = mesh.at(3 * t + 2);
      if (!point_in_poly((v.X[a] + v.X[b] + v.X[c]) / 3, (v.Y[a] + v.Y[b] + v.Y[c]) / 3, poly)) {
        keep[t] = 0;
      }
    }
  }
  if (max_edge > 0) {
    mesh.peel(keep, max_edge);
  }
  std::vector<std::int32_t> tris;
  for (std::size_t t = 0; t < keep.size(); ++t) {
    if (keep[t] != 0) {
      tris.push_back(mesh.tri[3 * t]);
      tris.push_back(mesh.tri[3 * t + 1]);
      tris.push_back(mesh.tri[3 * t + 2]);
    }
  }
  if (tris.empty()) {
    report.errors.push_back(tr(w, "tin.err.inside"));
    return {nullptr, std::move(report)};
  }
  if (report.merged != 0) {
    report.notes.push_back(trn(w, "tin.merged", report.merged));
  }
  if (report.z_conflicts != 0) {
    report.notes.push_back(trn(w, "tin.zconflict", report.z_conflicts));
  }
  if (report.skipped_no_z != 0) {
    report.notes.push_back(trn(w, "tin.noz", report.skipped_no_z));
  }
  report.breakline_segments = static_cast<double>(n_break);
  auto constrained = mesh.constrained_pairs();
  auto s = std::make_shared<Surface>(std::move(v.X), std::move(v.Y), std::move(v.Z), std::move(tris), ox, oy);
  s->constrained_edges = std::move(constrained);
  return {std::move(s), std::move(report)};
}

std::vector<P2> ccw_copy(const std::vector<P2>& pts) {
  std::vector<P2> out = pts;
  if (is_clockwise(pts)) {
    std::ranges::reverse(out);
  }
  return out;
}

double extent(const std::vector<P2>& pts) {
  double a = kInf;
  double b = kInf;
  double c = -kInf;
  double d = -kInf;
  for (const P2& p : pts) {
    a = jmin(a, p.x);
    b = jmin(b, p.y);
    c = jmax(c, p.x);
    d = jmax(d, p.y);
  }
  return jmax(c - a, d - b);
}
/// clampI; nullopt for NaN (the JS then loops over no cells, or reads an undefined cell: nothing).
std::optional<std::size_t> clamp_i(double v, std::size_t n) {
  const double i = std::floor(v);
  if (std::isnan(i)) {
    return std::nullopt;
  }
  if (i < 0) {
    return 0;
  }
  if (i >= static_cast<double>(n)) {
    return n - 1;
  }
  return static_cast<std::size_t>(i);
}
double band_index(const std::vector<double>& levels, double z) {
  std::size_t i = 0;  // i − 1 is the band
  while (i < levels.size() && z >= levels[i]) {
    ++i;
  }
  return static_cast<double>(i) - 1;
}
using V3 = std::array<double, 3>;
std::vector<V3> clip_z(const std::vector<V3>& poly, double L, double sign) {
  std::vector<V3> out;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const V3& p = poly[i];
    const V3& q = poly[(i + 1) % poly.size()];
    const double fp = sign * (p[2] - L);
    const double fq = sign * (q[2] - L);
    if (fp >= 0) {
      out.push_back(p);
    }
    if ((fp >= 0) != (fq >= 0)) {
      const double t = fp / (fp - fq);
      out.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, L});
    }
  }
  return out;
}

// contours: a crossing on edge (lo, hi), and chain()
struct CHit {
  std::uint64_t k;
  double x, y;
};
struct Line {
  std::vector<CHit> pts;
  bool closed;
};
std::vector<Line> chain(const std::vector<std::array<CHit, 2>>& segs) {
  // `at`: a Map in insertion order
  std::vector<std::uint64_t> order;
  std::map<std::uint64_t, std::vector<std::size_t>> at;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    for (const CHit& h : segs[i]) {
      auto it = at.find(h.k);
      if (it != at.end()) {
        it->second.push_back(i);
      } else {
        at.emplace(h.k, std::vector<std::size_t>{i});
        order.push_back(h.k);
      }
    }
  }
  std::vector<std::uint8_t> used(segs.size(), 0);
  std::vector<Line> out;
  auto follow = [&](std::size_t i, std::uint64_t from_key) {
    std::vector<CHit> pts;
    std::size_t cur = i;
    std::uint64_t k = from_key;
    for (std::size_t guard = 0; guard <= segs.size(); ++guard) {
      used[cur] = 1;
      const auto& s = segs[cur];
      const CHit next_h = s[0].k == k ? s[1] : s[0];
      pts.push_back(next_h);
      const auto it = at.find(next_h.k);
      std::optional<std::size_t> n;
      if (it != at.end()) {
        for (const std::size_t j : it->second) {
          if (used[j] == 0) {
            n = j;
            break;
          }
        }
      }
      if (!n) {
        break;
      }
      cur = *n;
      k = next_h.k;
    }
    return pts;
  };
  for (const std::uint64_t k : order) {
    const auto& list = at.at(k);
    if (list.size() != 1 || used[list[0]] != 0) {
      continue;
    }
    const auto& s = segs[list[0]];
    const CHit start_h = s[0].k == k ? s[0] : s[1];
    std::vector<CHit> pts{start_h};
    auto r = follow(list[0], k);
    pts.insert(pts.end(), r.begin(), r.end());
    out.push_back({std::move(pts), false});
  }
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (used[i] != 0) {
      continue;
    }
    const auto& s = segs[i];
    std::vector<CHit> pts{s[0]};
    auto r = follow(i, s[0].k);
    pts.insert(pts.end(), r.begin(), r.end());
    out.push_back({std::move(pts), true});
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- building

Built build_surface(const BuildInput& in, const i18n::Catalog& w) {
  TinReport report = fresh_report();
  std::optional<std::vector<P3>> clipped;
  if (in.boundary && in.boundary->size() >= 3) {
    BuildInput first_in{in.points, in.breaklines, std::nullopt, 0};
    Built first = triangulate(first_in, std::nullopt, 0, fresh_report(), w);
    if (!first.surface) {
      report.errors.insert(report.errors.end(), first.report.errors.begin(), first.report.errors.end());
      return {nullptr, std::move(report)};
    }
    const std::vector<P2> hull = first.surface->hull();
    const std::vector<P2> sub = ccw_copy(*in.boundary);
    const std::vector<Clipped> c = clip_convex(sub, hull);
    if (c.size() < 3) {
      report.errors.push_back(tr(w, "tin.err.bndy"));
      return {nullptr, std::move(report)};
    }
    const auto& bnd = *in.boundary;
    bool differs = c.size() != bnd.size();
    for (std::size_t i = 0; !differs && i < c.size(); ++i) {
      if (hyp(c[i].x - bnd[i].x, c[i].y - bnd[i].y) > 1e-9) {
        differs = true;
      }
    }
    if (differs) {
      if (std::ranges::any_of(bnd, [&](const P2& p) { return !point_in_poly(p.x, p.y, hull); })) {
        report.notes.push_back(tr(w, "tin.bndy.cut"));
      }
    }
    std::vector<P3> withz;
    for (const Clipped& p : c) {
      const auto z = first.surface->elevation_at(p.x, p.y, 1e-6);
      if (!z) {
        report.errors.push_back(tr(w, "tin.err.bndyz"));
        return {nullptr, std::move(report)};
      }
      withz.push_back({p.x, p.y, *z});
    }
    clipped = std::move(withz);
  }
  return triangulate(in, clipped, in.max_edge, std::move(report), w);
}

Built surface_from_faces(const FaceInput& in, const i18n::Catalog& w) {
  TinReport report;
  double ox = kInf;
  double oy = kInf;
  for (const auto& p : in.pts) {
    if (p) {
      ox = jmin(ox, p->x);
      oy = jmin(oy, p->y);
    }
  }
  if (!std::isfinite(ox)) {
    report.errors.push_back(tr(w, "tin.err.nopts"));
    return {nullptr, std::move(report)};
  }
  ox = std::floor(ox);
  oy = std::floor(oy);
  const std::size_t n = in.pts.size();
  std::vector<double> X(n, 0);
  std::vector<double> Y(n, 0);
  std::vector<double> Z(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    if (const auto& p = in.pts[i]; p) {
      X[i] = p->x - ox;
      Y[i] = p->y - oy;
      Z[i] = p->z;
    }
  }
  std::vector<std::int32_t> tris;
  double bad = 0;
  for (const auto& f : in.faces) {
    // f.some(bad id) || f[0] === f[1] || …: a face shorter than three has undefined members (bad ids too)
    bool fail = false;
    for (const double i : f) {
      const bool whole = std::isfinite(i) && std::floor(i) == i;
      if (!whole || i < 0 || i >= static_cast<double>(n) || !in.pts[static_cast<std::size_t>(i)]) {
        fail = true;
      }
    }
    // f[k] undefined for a short face: undefined === undefined fails it, except [a, b] (a ≠ b), which the JS keeps
    // with vertex 0 third (X[undefined] gives a NaN orientation, Int32Array stores undefined as 0)
    if (!fail && (f.size() < 2 || (f.size() >= 3 && (f[0] == f[1] || f[1] == f[2] || f[0] == f[2])) ||
                  (f.size() == 2 && f[0] == f[1]))) {
      fail = true;
    }
    if (fail) {
      bad++;
      continue;
    }
    if (f.size() == 2) {
      tris.push_back(static_cast<std::int32_t>(f[0]));
      tris.push_back(static_cast<std::int32_t>(f[1]));
      tris.push_back(0);
      continue;
    }
    auto a = static_cast<std::size_t>(f[0]);
    auto b = static_cast<std::size_t>(f[1]);
    auto c = static_cast<std::size_t>(f[2]);
    const double o = (X[b] - X[a]) * (Y[c] - Y[a]) - (Y[b] - Y[a]) * (X[c] - X[a]);
    if (o == 0) {
      bad++;
      continue;
    }
    if (o < 0) {
      std::swap(b, c);
    }
    tris.push_back(static_cast<std::int32_t>(a));
    tris.push_back(static_cast<std::int32_t>(b));
    tris.push_back(static_cast<std::int32_t>(c));
  }
  if (bad != 0) {
    report.notes.push_back(trn(w, "tin.badfaces", bad));
  }
  if (tris.empty()) {
    report.errors.push_back(tr(w, "tin.err.nofaces"));
    return {nullptr, std::move(report)};
  }
  return {std::make_shared<Surface>(std::move(X), std::move(Y), std::move(Z), std::move(tris), ox, oy),
          std::move(report)};
}

// ---------------------------------------------------------------------------------------------- the surface

Surface::Surface(std::vector<double> x, std::vector<double> y, std::vector<double> z, std::vector<std::int32_t> tris,
                 double ox, double oy)
    : x_(std::move(x)), y_(std::move(y)), z_(std::move(z)), tris_(std::move(tris)), ox_(ox), oy_(oy) {}

std::vector<std::uint8_t> Surface::used_vertices() const {
  std::vector<std::uint8_t> used(x_.size(), 0);
  for (const std::int32_t i : tris_) {
    used[static_cast<std::size_t>(i)] = 1;
  }
  return used;
}

const SurfaceStats& Surface::stats() const {
  if (stats_) {
    return *stats_;
  }
  const auto used = used_vertices();
  double n = 0;
  double min_z = kInf;
  double max_z = -kInf;
  double sum = 0;
  double min_x = kInf;
  double min_y = kInf;
  double max_x = -kInf;
  double max_y = -kInf;
  for (std::size_t i = 0; i < x_.size(); ++i) {
    if (used[i] == 0) {
      continue;
    }
    n++;
    sum += z_[i];
    min_z = std::min(min_z, z_[i]);  // (std::min keeps the first for NaN, as the JS's ifs)
    max_z = std::max(max_z, z_[i]);
    min_x = std::min(min_x, x_[i]);
    max_x = std::max(max_x, x_[i]);
    min_y = std::min(min_y, y_[i]);
    max_y = std::max(max_y, y_[i]);
  }
  double a2 = 0;
  double a3 = 0;
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    const auto a = static_cast<std::size_t>(tris_[t]);
    const auto b = static_cast<std::size_t>(tris_[t + 1]);
    const auto c = static_cast<std::size_t>(tris_[t + 2]);
    const double ux = x_[b] - x_[a];
    const double uy = y_[b] - y_[a];
    const double uz = z_[b] - z_[a];
    const double vx = x_[c] - x_[a];
    const double vy = y_[c] - y_[a];
    const double vz = z_[c] - z_[a];
    const double cx = uy * vz - uz * vy;
    const double cy = uz * vx - ux * vz;
    const double cz = ux * vy - uy * vx;
    a2 += std::abs(cz) / 2;
    a3 += base::js_hypot(cx, cy, cz) / 2;
  }
  stats_ = SurfaceStats{n,           static_cast<double>(triangle_count()),
                        min_z,       max_z,
                        sum / n,     a2,
                        a3,          min_x + ox_,
                        min_y + oy_, max_x + ox_,
                        max_y + oy_};
  return *stats_;
}

std::vector<P2> Surface::hull() const {
  const auto used = used_vertices();
  std::vector<P2> pts;
  for (std::size_t i = 0; i < x_.size(); ++i) {
    if (used[i] != 0) {
      pts.push_back({x_[i] + ox_, y_[i] + oy_});
    }
  }
  std::vector<P2> out;
  for (const std::size_t i : convex_hull(pts)) {
    out.push_back(pts[i]);
  }
  return out;
}

const Surface::Grid& Surface::grid() const {
  if (grid_) {
    return *grid_;
  }
  auto g = std::make_unique<Grid>();
  const SurfaceStats& s = stats();
  g->bminx = s.min_x - ox_;
  g->bminy = s.min_y - oy_;
  g->bmaxx = s.max_x - ox_;
  g->bmaxy = s.max_y - oy_;
  const auto nt = static_cast<double>(triangle_count());
  const double n = std::max(1.0, js_round(std::sqrt(nt / 2)));
  const double w = std::max(g->bmaxx - g->bminx, 1e-9);
  const double h = std::max(g->bmaxy - g->bminy, 1e-9);
  double nx = std::max(1.0, js_round(n * std::sqrt(w / h)));
  double ny = std::max(1.0, js_round(n * std::sqrt(h / w)));
  // hostile input only: non-finite bounds, or a sliver so thin that one side of its grid rounds to a single cell and
  // the other runs to millions (nx · ny is about n² otherwise): capped at 64 n² + 4096 cells (docs/gaps.md)
  if (!std::isfinite(nx)) {
    nx = 1;
  }
  if (!std::isfinite(ny)) {
    ny = 1;
  }
  const double max_cells = 64 * n * n + 4096;
  if (nx * ny > max_cells) {
    if (nx >= ny) {
      nx = std::max(1.0, std::floor(max_cells / ny));
    } else {
      ny = std::max(1.0, std::floor(max_cells / nx));
    }
  }
  g->nx = static_cast<std::size_t>(nx);
  g->ny = static_cast<std::size_t>(ny);
  g->cw = w / nx;
  g->ch = h / ny;
  const std::size_t cells = g->nx * g->ny;
  std::vector<std::int32_t> counts(cells + 1, 0);
  auto each_cell = [&](std::size_t t, auto&& f) {
    const auto a = static_cast<std::size_t>(tris_[3 * t]);
    const auto c = static_cast<std::size_t>(tris_[3 * t + 1]);
    const auto d = static_cast<std::size_t>(tris_[3 * t + 2]);
    const double x0 = jmin(jmin(x_[a], x_[c]), x_[d]);
    const double x1 = jmax(jmax(x_[a], x_[c]), x_[d]);
    const double y0 = jmin(jmin(y_[a], y_[c]), y_[d]);
    const double y1 = jmax(jmax(y_[a], y_[c]), y_[d]);
    const auto i0 = clamp_i((x0 - g->bminx) / g->cw, g->nx);
    const auto i1 = clamp_i((x1 - g->bminx) / g->cw, g->nx);
    const auto j0 = clamp_i((y0 - g->bminy) / g->ch, g->ny);
    const auto j1 = clamp_i((y1 - g->bminy) / g->ch, g->ny);
    if (!i0 || !i1 || !j0 || !j1) {
      return;
    }
    for (std::size_t j = *j0; j <= *j1; ++j) {
      for (std::size_t i = *i0; i <= *i1; ++i) {
        f(j * g->nx + i);
      }
    }
  };
  const std::size_t ntri = triangle_count();
  for (std::size_t t = 0; t < ntri; ++t) {
    each_cell(t, [&](std::size_t k) { counts[k + 1]++; });
  }
  for (std::size_t k = 1; k < counts.size(); ++k) {
    counts[k] += counts[k - 1];
  }
  std::vector<std::int32_t> items(static_cast<std::size_t>(counts.back()), 0);
  std::vector<std::int32_t> fill(counts.begin(), counts.end() - 1);
  for (std::size_t t = 0; t < ntri; ++t) {
    each_cell(t, [&](std::size_t k) { items[static_cast<std::size_t>(fill[k]++)] = static_cast<std::int32_t>(t); });
  }
  g->start = std::move(counts);
  g->items = std::move(items);
  grid_ = std::move(g);
  return *grid_;
}

std::optional<Located> Surface::locate(double lx, double ly, double tol) const {
  const Grid& g = grid();
  if (lx < g.bminx - tol || lx > g.bmaxx + tol || ly < g.bminy - tol || ly > g.bmaxy + tol) {
    return std::nullopt;
  }
  const auto ci = clamp_i((lx - g.bminx) / g.cw, g.nx);
  const auto cj = clamp_i((ly - g.bminy) / g.ch, g.ny);
  if (!ci || !cj) {
    return std::nullopt;  // the JS reads the grid at NaN: no items
  }
  const std::size_t k = *cj * g.nx + *ci;
  std::optional<Located> best;
  double best_min = -kInf;
  for (auto s = g.start[k]; s < g.start[k + 1]; ++s) {
    const auto t = static_cast<std::size_t>(g.items[static_cast<std::size_t>(s)]);
    const auto a = static_cast<std::size_t>(tris_[3 * t]);
    const auto b = static_cast<std::size_t>(tris_[3 * t + 1]);
    const auto c = static_cast<std::size_t>(tris_[3 * t + 2]);
    const double d = (y_[b] - y_[c]) * (x_[a] - x_[c]) + (x_[c] - x_[b]) * (y_[a] - y_[c]);
    if (d == 0) {
      continue;
    }
    const double w1 = ((y_[b] - y_[c]) * (lx - x_[c]) + (x_[c] - x_[b]) * (ly - y_[c])) / d;
    const double w2 = ((y_[c] - y_[a]) * (lx - x_[c]) + (x_[a] - x_[c]) * (ly - y_[c])) / d;
    const double w3 = 1 - w1 - w2;
    const double m = jmin(jmin(w1, w2), w3);
    if (m >= 0) {
      return Located{t, {w1, w2, w3}};
    }
    if (m > best_min) {
      best_min = m;
      best = Located{t, {w1, w2, w3}};
    }
  }
  if (best && tol > 0) {
    const std::size_t t = best->t;
    const auto a = static_cast<std::size_t>(tris_[3 * t]);
    const auto b = static_cast<std::size_t>(tris_[3 * t + 1]);
    const auto c = static_cast<std::size_t>(tris_[3 * t + 2]);
    const double scale = jmax(jmax(hyp(x_[b] - x_[a], y_[b] - y_[a]), hyp(x_[c] - x_[b], y_[c] - y_[b])),
                              hyp(x_[a] - x_[c], y_[a] - y_[c]));
    if (-best_min * scale <= tol) {
      std::array<double, 3> w{};
      for (std::size_t q = 0; q < 3; ++q) {
        w[q] = jmax(0, best->w[q]);
      }
      const double s = w[0] + w[1] + w[2];
      return Located{t, {w[0] / s, w[1] / s, w[2] / s}};
    }
  }
  return std::nullopt;
}

std::optional<double> Surface::elevation_at(double x, double y, double tol) const {
  const auto r = locate(x - ox_, y - oy_, tol);
  if (!r) {
    return std::nullopt;
  }
  const std::size_t t = r->t;
  return r->w[0] * z_[static_cast<std::size_t>(tris_[3 * t])] +
         r->w[1] * z_[static_cast<std::size_t>(tris_[3 * t + 1])] +
         r->w[2] * z_[static_cast<std::size_t>(tris_[3 * t + 2])];
}

Surface::SlopeOf Surface::slope_of(std::size_t t) const {
  const auto a = static_cast<std::size_t>(tris_[3 * t]);
  const auto b = static_cast<std::size_t>(tris_[3 * t + 1]);
  const auto c = static_cast<std::size_t>(tris_[3 * t + 2]);
  const double ux = x_[b] - x_[a];
  const double uy = y_[b] - y_[a];
  const double uz = z_[b] - z_[a];
  const double vx = x_[c] - x_[a];
  const double vy = y_[c] - y_[a];
  const double vz = z_[c] - z_[a];
  const double nx = uy * vz - uz * vy;
  const double ny = uz * vx - ux * vz;
  const double nz = ux * vy - uy * vx;
  const double gx = -nx / nz;
  const double gy = -ny / nz;
  return {hyp(gx, gy), base::js_atan2(-gy, -gx), gx, gy};
}

std::vector<Slope> Surface::slopes() const {
  std::vector<Slope> out;
  const std::size_t nt = triangle_count();
  out.reserve(nt);
  for (std::size_t t = 0; t < nt; ++t) {
    const auto a = static_cast<std::size_t>(tris_[3 * t]);
    const auto b = static_cast<std::size_t>(tris_[3 * t + 1]);
    const auto c = static_cast<std::size_t>(tris_[3 * t + 2]);
    const SlopeOf s = slope_of(t);
    out.push_back({(x_[a] + x_[b] + x_[c]) / 3 + ox_, (y_[a] + y_[b] + y_[c]) / 3 + oy_, s.slope, s.dir,
                   std::sqrt(std::abs((x_[b] - x_[a]) * (y_[c] - y_[a]) - (y_[b] - y_[a]) * (x_[c] - x_[a])) / 2)});
  }
  return out;
}

std::optional<std::vector<Contour>> Surface::contours(double interval, double base, double max_levels) const {
  std::vector<Contour> out;
  if (!(interval > 0)) {
    return out;
  }
  const SurfaceStats& s = stats();
  const double k0 = std::ceil((s.min_z - base) / interval - 1e-9);
  const double k1 = std::floor((s.max_z - base) / interval + 1e-9);
  if (k1 - k0 > max_levels) {
    return std::nullopt;
  }
  if (!(k1 - k0 <= max_levels)) {  // NaN: the JS loop `k <= k1` never runs
    return out;
  }
  const double eps = interval * 1e-7;
  const double tiny = 1e-6 * hyp(s.max_x - s.min_x, s.max_y - s.min_y);
  for (std::int64_t step = 0; k0 + static_cast<double>(step) <= k1; ++step) {
    const double k = k0 + static_cast<double>(step);  // whole numbers, exact (k1 − k0 ≤ maxLevels)
    const double L = base + k * interval;
    auto zv = [&](std::size_t i) { return std::abs(z_[i] - L) < eps ? L + eps : z_[i]; };
    std::vector<std::array<CHit, 2>> segs;
    auto cross = [&](std::size_t i, std::size_t j) -> std::optional<CHit> {
      const double zi = zv(i);
      const double zj = zv(j);
      if ((zi > L) == (zj > L)) {
        return std::nullopt;
      }
      const double t = (L - zi) / (zj - zi);
      const std::uint64_t key = i < j ? (std::uint64_t{i} << 32U) | j : (std::uint64_t{j} << 32U) | i;
      return CHit{key, x_[i] + (x_[j] - x_[i]) * t, y_[i] + (y_[j] - y_[i]) * t};
    };
    for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
      const auto a = static_cast<std::size_t>(tris_[t]);
      const auto b = static_cast<std::size_t>(tris_[t + 1]);
      const auto c = static_cast<std::size_t>(tris_[t + 2]);
      const double za = zv(a);
      const double zb = zv(b);
      const double zc = zv(c);
      if ((za > L && zb > L && zc > L) || (za < L && zb < L && zc < L)) {
        continue;
      }
      std::vector<CHit> hits;
      for (const auto& h : {cross(a, b), cross(b, c), cross(c, a)}) {
        if (h) {
          hits.push_back(*h);
        }
      }
      if (hits.size() == 2) {
        segs.push_back({hits[0], hits[1]});
      }
    }
    for (const Line& line : chain(segs)) {
      std::vector<P2> pts;
      pts.reserve(line.pts.size());
      for (const CHit& h : line.pts) {
        pts.push_back({h.x, h.y});
      }
      if (extent(pts) > tiny) {
        for (P2& p : pts) {
          p.x += ox_;
          p.y += oy_;
        }
        out.push_back({L, std::move(pts), line.closed});
      }
    }
  }
  return out;
}

std::vector<Band> Surface::bands(const std::vector<double>& levels) const {
  std::vector<Band> out;
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    std::vector<V3> v;
    for (std::size_t q = 0; q < 3; ++q) {
      const auto i = static_cast<std::size_t>(tris_[t + q]);
      v.push_back({x_[i] + ox_, y_[i] + oy_, z_[i]});
    }
    const double zmin = jmin(jmin(v[0][2], v[1][2]), v[2][2]);
    const double zmax = jmax(jmax(v[0][2], v[1][2]), v[2][2]);
    const double b0 = band_index(levels, zmin);
    const double bj = band_index(levels, zmax);
    auto flat = [](const std::vector<V3>& p) {
      std::vector<double> l;
      for (const V3& q : p) {
        l.push_back(q[0]);
        l.push_back(q[1]);
      }
      return l;
    };
    if (b0 == bj) {
      out.push_back({b0, flat(v)});
      continue;
    }
    for (std::int64_t q = 0; b0 + static_cast<double>(q) <= bj; ++q) {
      const double bi = b0 + static_cast<double>(q);
      std::vector<V3> poly = v;
      if (bi >= 0) {
        poly = clip_z(poly, levels[static_cast<std::size_t>(bi)], 1);
      }
      if (bi + 1 < static_cast<double>(levels.size())) {
        poly = clip_z(poly, levels[static_cast<std::size_t>(bi + 1)], -1);
      }
      if (poly.size() >= 3) {
        out.push_back({bi, flat(poly)});
      }
    }
  }
  return out;
}

std::vector<std::array<double, 9>> Surface::triangles() const {
  std::vector<std::array<double, 9>> out;
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    std::array<double, 9> r{};
    for (std::size_t q = 0; q < 3; ++q) {
      const auto i = static_cast<std::size_t>(tris_[t + q]);
      r[3 * q] = x_[i] + ox_;
      r[3 * q + 1] = y_[i] + oy_;
      r[3 * q + 2] = z_[i];
    }
    out.push_back(r);
  }
  return out;
}

std::vector<std::int32_t> Surface::edges() const {
  std::set<std::uint64_t> seen;
  std::vector<std::int32_t> out;
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::int32_t a = tris_[t + k];
      const std::int32_t b = tris_[t + (k + 1) % 3];
      const auto lo = static_cast<std::uint64_t>(std::min(a, b));
      const auto hi = static_cast<std::uint64_t>(std::max(a, b));
      if (!seen.insert((lo << 32U) | hi).second) {
        continue;
      }
      out.push_back(a);
      out.push_back(b);
    }
  }
  return out;
}

std::vector<std::vector<P2>> Surface::outline() const {
  // count: a Map of directed edges in first-seen order
  std::set<std::uint64_t> count;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
  auto key = [](std::uint32_t a, std::uint32_t b) { return (std::uint64_t{a} << 32U) | b; };
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    for (std::size_t k = 0; k < 3; ++k) {
      const auto a = static_cast<std::uint32_t>(tris_[t + k]);
      const auto b = static_cast<std::uint32_t>(tris_[t + (k + 1) % 3]);
      if (count.insert(key(a, b)).second) {
        order.emplace_back(a, b);
      }
    }
  }
  // nextOf: a Map vertex → next vertex; a later set keeps the key's place and changes its value
  std::vector<std::pair<std::uint32_t, std::uint32_t>> next_of;
  std::map<std::uint32_t, std::size_t> where;
  for (const auto& [a, b] : order) {
    if (count.contains(key(b, a))) {
      continue;
    }
    if (const auto it = where.find(a); it != where.end()) {
      next_of[it->second].second = b;
    } else {
      where.emplace(a, next_of.size());
      next_of.emplace_back(a, b);
    }
  }
  std::vector<std::uint8_t> gone(next_of.size(), 0);
  std::size_t first = 0;
  std::size_t left = next_of.size();
  std::vector<std::vector<P2>> loops;
  while (left > 0) {
    while (gone[first] != 0) {
      ++first;
    }
    std::vector<P2> loop;
    std::uint32_t v = next_of[first].first;
    for (std::size_t g = 0; g < 10000000; ++g) {
      const auto it = where.find(v);
      if (it == where.end() || gone[it->second] != 0) {
        break;
      }
      loop.push_back({x_[v] + ox_, y_[v] + oy_});
      const std::uint32_t n = next_of[it->second].second;
      gone[it->second] = 1;
      --left;
      v = n;
    }
    if (loop.size() >= 3) {
      loops.push_back(std::move(loop));
    }
  }
  return loops;
}

Surface::Faces Surface::to_faces() const {
  const auto used = used_vertices();
  std::vector<std::int32_t> map(x_.size(), -1);
  Faces f;
  for (std::size_t i = 0; i < x_.size(); ++i) {
    if (used[i] != 0) {
      map[i] = static_cast<std::int32_t>(f.pts.size());
      f.pts.push_back({x_[i] + ox_, y_[i] + oy_, z_[i]});
    }
  }
  for (std::size_t t = 0; t + 2 < tris_.size(); t += 3) {
    f.faces.push_back({map[static_cast<std::size_t>(tris_[t])], map[static_cast<std::size_t>(tris_[t + 1])],
                       map[static_cast<std::size_t>(tris_[t + 2])]});
  }
  return f;
}

// ---------------------------------------------------------------------------------------------- plane helpers

std::vector<std::size_t> convex_hull(const std::vector<P2>& pts) {
  std::vector<std::size_t> p(pts.size());
  for (std::size_t i = 0; i < p.size(); ++i) {
    p[i] = i;
  }
  appkit::js_sort(p, [&](std::size_t a, std::size_t b) {
    const double d = pts[a].x - pts[b].x;
    return (d != 0 && !std::isnan(d)) ? d : pts[a].y - pts[b].y;
  });
  if (p.size() < 3) {
    return p;
  }
  auto cr = [&](std::size_t o, std::size_t a, std::size_t b) {
    return (pts[a].x - pts[o].x) * (pts[b].y - pts[o].y) - (pts[a].y - pts[o].y) * (pts[b].x - pts[o].x);
  };
  std::vector<std::size_t> lo;
  std::vector<std::size_t> hi;
  for (const std::size_t q : p) {
    while (lo.size() >= 2 && cr(lo[lo.size() - 2], lo.back(), q) <= 0) {
      lo.pop_back();
    }
    lo.push_back(q);
  }
  for (std::size_t i = p.size(); i-- > 0;) {
    const std::size_t q = p[i];
    while (hi.size() >= 2 && cr(hi[hi.size() - 2], hi.back(), q) <= 0) {
      hi.pop_back();
    }
    hi.push_back(q);
  }
  lo.pop_back();
  hi.pop_back();
  lo.insert(lo.end(), hi.begin(), hi.end());
  return lo;
}

double poly_area(const std::vector<P2>& pts) {
  double a = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const P2& p = pts[i];
    const P2& q = pts[(i + 1) % pts.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return a / 2;
}

bool is_clockwise(const std::vector<P2>& pts) {
  return poly_area(pts) < 0;
}

bool point_in_poly(double x, double y, const std::vector<P2>& pts) {
  bool inside = false;
  const std::size_t n = pts.size();
  for (std::size_t i = 0, j = n == 0 ? 0 : n - 1; i < n; j = i++) {
    const P2& a = pts[i];
    const P2& b = pts[j];
    if ((a.y > y) != (b.y > y) && x < ((b.x - a.x) * (y - a.y)) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

std::vector<Clipped> clip_convex(const std::vector<P2>& subject, const std::vector<P2>& clip) {
  std::vector<Clipped> out;
  out.reserve(subject.size());
  for (std::size_t i = 0; i < subject.size(); ++i) {
    out.push_back({subject[i].x, subject[i].y, static_cast<std::ptrdiff_t>(i)});
  }
  for (std::size_t i = 0; i < clip.size() && !out.empty(); ++i) {
    const P2 a = clip[i];
    const P2 b = clip[(i + 1) % clip.size()];
    auto inside = [&](const Clipped& p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) >= -1e-9; };
    const std::vector<Clipped> inp = std::move(out);
    out.clear();
    for (std::size_t k = 0; k < inp.size(); ++k) {
      const Clipped& p = inp[k];
      const Clipped& q = inp[(k + 1) % inp.size()];
      const bool ip = inside(p);
      const bool iq = inside(q);
      if (ip) {
        out.push_back(p);
      }
      if (ip != iq) {
        const double dx = q.x - p.x;
        const double dy = q.y - p.y;
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double t = ((a.x - p.x) * ey - (a.y - p.y) * ex) / (dx * ey - dy * ex);
        out.push_back({p.x + dx * t, p.y + dy * t, -1});
      }
    }
  }
  std::vector<Clipped> kept;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const Clipped& p = out[i];
    const Clipped& q = out[(i + 1) % out.size()];
    if (hyp(p.x - q.x, p.y - q.y) > 1e-9) {
      kept.push_back(p);
    }
  }
  return kept;
}

}  // namespace cc::civil
