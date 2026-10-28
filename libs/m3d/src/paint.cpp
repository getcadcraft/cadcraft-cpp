// SPDX-License-Identifier: Apache-2.0
// model.js: paint (colours, library materials, imported image textures), offset, the solid checker.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"
#include "geom_internal.hpp"

namespace cc::m3d {

using namespace detail;  // NOLINT(google-build-using-namespace): the kernel's own helpers

namespace {

bool hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_digit(char c) {
  return c >= '0' && c <= '9';
}
std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

/// NUM = -?\d+(?:\.\d+)?(?:e[-+]?\d+)?  (case-insensitive) matched at s[i…]; → the end, or npos.
std::size_t match_num(std::string_view s, std::size_t i) {
  if (i < s.size() && s[i] == '-') {
    ++i;
  }
  const std::size_t d0 = i;
  while (i < s.size() && is_digit(s[i])) {
    ++i;
  }
  if (i == d0) {
    return std::string_view::npos;
  }
  if (i + 1 < s.size() && s[i] == '.' && is_digit(s[i + 1])) {
    ++i;
    while (i < s.size() && is_digit(s[i])) {
      ++i;
    }
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    std::size_t j = i + 1;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) {
      ++j;
    }
    const std::size_t e0 = j;
    while (j < s.size() && is_digit(s[j])) {
      ++j;
    }
    if (j > e0) {
      i = j;
    }
  }
  return i;
}

std::function<std::optional<std::string>(std::string_view)>& material_colour() {
  static std::function<std::optional<std::string>(std::string_view)> fn;
  return fn;
}

}  // namespace

bool is_color(std::string_view c) {
  return c.size() == 7 && c[0] == '#' && std::ranges::all_of(c.substr(1), hex_digit);
}

bool is_material_paint(std::string_view c) {
  // /^mat:[a-z0-9][a-z0-9-]{0,39}$/
  if (c.size() < 5 || c.size() > 44 || !c.starts_with("mat:")) {
    return false;
  }
  const auto ok = [](char ch, bool first) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (!first && ch == '-');
  };
  for (std::size_t i = 4; i < c.size(); ++i) {
    if (!ok(c[i], i == 4)) {
      return false;
    }
  }
  return true;
}

bool is_image_paint(std::string_view c) {
  // c.length <= 200 && /^img:[a-z0-9]{1,24}\|(?:NUM,){7}NUM\|(?:#[0-9a-f]{6})?$/i
  if (c.size() > 200 || c.size() < 4 || lower(c.substr(0, 4)) != "img:") {
    return false;
  }
  std::size_t i = 4;
  const std::size_t id0 = i;
  while (i < c.size() && (is_digit(c[i]) || (c[i] >= 'a' && c[i] <= 'z') || (c[i] >= 'A' && c[i] <= 'Z'))) {
    ++i;
  }
  if (i == id0 || i - id0 > 24 || i >= c.size() || c[i] != '|') {
    return false;
  }
  ++i;
  for (int k = 0; k < 8; ++k) {
    const std::size_t e = match_num(c, i);
    if (e == std::string_view::npos) {
      return false;
    }
    i = e;
    if (k < 7) {
      if (i >= c.size() || c[i] != ',') {
        return false;
      }
      ++i;
    }
  }
  if (i >= c.size() || c[i] != '|') {
    return false;
  }
  ++i;
  return i == c.size() || is_color(c.substr(i));
}

bool is_paint(std::string_view c) {
  return is_color(c) || is_material_paint(c) || is_image_paint(c);
}

void set_material_colors(std::function<std::optional<std::string>(std::string_view)> fn) {
  material_colour() = std::move(fn);
}

std::string image_id(std::string_view c) {
  const auto bar = c.find('|');
  return std::string(c.substr(4, bar == std::string_view::npos ? std::string_view::npos : bar - 4));
}

std::string paint_hex(std::string_view c) {
  if (is_material_paint(c)) {
    if (material_colour()) {
      if (auto got = material_colour()(c.substr(4)); got && !got->empty()) {
        return *got;
      }
    }
    return "#c8c4bc";
  }
  if (is_image_paint(c)) {
    const std::string tail(c.substr(c.rfind('|') + 1));
    return tail.empty() ? std::string("#c8c4bc") : tail;
  }
  return std::string(c);
}

std::string to_base64(std::span<const std::uint8_t> bytes) {
  return base::base64(bytes);
}

namespace {

/// r7: Number(x.toPrecision(7)), -0 → 0, as a JS number string (join(',')).
std::string r7(double x) {
  if (!std::isfinite(x)) {
    std::string s;
    base::append_number(s, x);
    return s;
  }
  if (x == 0) {
    return "0";
  }
  // toPrecision(7): 7 significant digits, an exact half rounded away from zero (as toExponential does)
  const std::string e = base::js_to_exponential(x, 6);
  double v = std::strtod(e.c_str(), nullptr);
  if (v == 0) {
    v = 0;  // -0 → 0
  }
  std::string s;
  base::append_number(s, v);
  return s;
}

}  // namespace

std::string image_paint(std::string_view id, std::span<const double, 4> U, std::span<const double, 4> V,
                        std::string_view avg) {
  std::string s = "img:" + std::string(id) + "|";
  for (std::size_t i = 0; i < 8; ++i) {
    if (i != 0) {
      s += ',';
    }
    s += r7(i < 4 ? U[i] : V[i - 4]);
  }
  s += '|';
  if (is_color(avg)) {
    s += lower(avg);
  }
  return s;
}

Vec2 image_uv(std::string_view c, const Vec3& p) {
  const auto a = c.find('|');
  const auto b = c.find('|', a + 1);
  const std::string_view nums = c.substr(a + 1, b - a - 1);
  std::array<double, 8> k{};
  std::size_t i = 0;
  std::size_t at = 0;
  while (i < 8 && at <= nums.size()) {
    const auto comma = nums.find(',', at);
    k.at(i++) = base::js_number(nums.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
    if (comma == std::string_view::npos) {
      break;
    }
    at = comma + 1;
  }
  return {k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + k[3], k[4] * p[0] + k[5] * p[1] + k[6] * p[2] + k[7]};
}

std::optional<std::string> image_paint_fit(std::string_view id, std::span<const Vec3> pts, std::span<const Vec2> uvs,
                                           const Vec3& n, std::string_view avg) {
  for (std::size_t i = 0; i + 2 < pts.size(); ++i) {
    for (std::size_t j = i + 1; j + 1 < pts.size(); ++j) {
      for (std::size_t k = j + 1; k < pts.size(); ++k) {
        const std::array<std::array<double, 4>, 4> A = {{{pts[i][0], pts[i][1], pts[i][2], 1},
                                                         {pts[j][0], pts[j][1], pts[j][2], 1},
                                                         {pts[k][0], pts[k][1], pts[k][2], 1},
                                                         {n[0], n[1], n[2], 0}}};
        const auto solve = [&](std::array<double, 4> b) -> std::optional<std::array<double, 4>> {
          std::array<std::array<double, 5>, 4> M{};
          for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t q = 0; q < 4; ++q) {
              M.at(r).at(q) = A.at(r).at(q);
            }
            M.at(r)[4] = b.at(r);
          }
          for (std::size_t c = 0; c < 4; ++c) {
            std::size_t piv = c;
            for (std::size_t r = c + 1; r < 4; ++r) {
              if (std::abs(M.at(r).at(c)) > std::abs(M.at(piv).at(c))) {
                piv = r;
              }
            }
            if (std::abs(M.at(piv).at(c)) < 1e-12) {
              return std::nullopt;
            }
            std::swap(M.at(c), M.at(piv));
            for (std::size_t r = 0; r < 4; ++r) {
              if (r != c) {
                const double f = M.at(r).at(c) / M.at(c).at(c);
                for (std::size_t q = c; q < 5; ++q) {
                  M.at(r).at(q) -= f * M.at(c).at(q);
                }
              }
            }
          }
          std::array<double, 4> out{};
          for (std::size_t q = 0; q < 4; ++q) {
            out.at(q) = M.at(q)[4] / M.at(q).at(q);
          }
          return out;
        };
        const auto U = solve({uvs[i][0], uvs[j][0], uvs[k][0], 0});
        const auto V = solve({uvs[i][1], uvs[j][1], uvs[k][1], 0});
        if (U && V && std::ranges::all_of(*U, [](double x) { return std::isfinite(x); }) &&
            std::ranges::all_of(*V, [](double x) { return std::isfinite(x); })) {
          return image_paint(id, *U, *V, avg);
        }
      }
    }
  }
  return std::nullopt;
}

base::Result<void> paint_faces(Def& d, std::span<const Id> fids, std::optional<std::string_view> c) {
  if (c && !is_paint(*c)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.that_is_not_a_colour"));
  }
  for (const Id id : fids) {
    if (Face* f = d.faces.get(id)) {
      f->color = c && !c->empty() ? base::js_to_lower(*c) : std::string();
    }
  }
  return {};
}

base::Result<void> paint_instance(Def& d, Id iid, std::optional<std::string_view> c) {
  if (c && !is_paint(*c)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.that_is_not_a_colour"));
  }
  if (Inst* i = d.insts.get(iid)) {
    i->color = c && !c->empty() ? base::js_to_lower(*c) : std::string();
  }
  return {};
}

// ---------------------------------------------------------------- offset

std::optional<std::vector<Vec2>> offset2(std::span<const Vec2> P, double dd, bool closed) {
  const std::size_t n = P.size();
  if (n < 2) {
    return std::nullopt;
  }
  struct Seg {
    Vec2 a, u;
  };
  std::vector<Seg> segs;
  for (std::size_t i = 0; i < (closed ? n : n - 1); ++i) {
    const Vec2& a = P[i];
    const Vec2& b = P[(i + 1) % n];
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const double L = base::js_hypot(dx, dy);
    if (!(L > 1e-12)) {
      return std::nullopt;
    }
    const double nx = -dy / L;
    const double ny = dx / L;
    segs.push_back({{a[0] + nx * dd, a[1] + ny * dd}, {dx / L, dy / L}});
  }
  const auto meet = [](const Seg& s, const Seg& t) -> Vec2 {
    const double den = s.u[0] * t.u[1] - s.u[1] * t.u[0];
    if (std::abs(den) < 1e-9) {
      return t.a;
    }
    const double k = ((t.a[0] - s.a[0]) * t.u[1] - (t.a[1] - s.a[1]) * t.u[0]) / den;
    return {s.a[0] + s.u[0] * k, s.a[1] + s.u[1] * k};
  };
  std::vector<Vec2> Q;
  for (std::size_t i = 0; i < n; ++i) {
    if (!closed && i == 0) {
      Q.push_back(segs[0].a);
      continue;
    }
    if (!closed && i == n - 1) {
      const Seg& s = segs[n - 2];
      const double L = base::js_hypot(P[n - 1][0] - P[n - 2][0], P[n - 1][1] - P[n - 2][1]);
      Q.push_back({s.a[0] + s.u[0] * L, s.a[1] + s.u[1] * L});
      continue;
    }
    Q.push_back(meet(segs[(i - 1 + segs.size()) % segs.size()], segs[i]));
  }
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const Vec2& a = Q[i];
    const Vec2& b = Q[(i + 1) % n];
    if ((b[0] - a[0]) * segs[i].u[0] + (b[1] - a[1]) * segs[i].u[1] <= 1e-9) {
      return std::nullopt;
    }
  }
  return Q;
}

namespace {

bool segs_cross2(const Vec2& A, const Vec2& B, const Vec2& C, const Vec2& D) {
  const auto o = [](const Vec2& p, const Vec2& q, const Vec2& r) {
    return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]);
  };
  const double d1 = o(C, D, A);
  const double d2 = o(C, D, B);
  const double d3 = o(A, B, C);
  const double d4 = o(A, B, D);
  return ((d1 > 1e-12 && d2 < -1e-12) || (d1 < -1e-12 && d2 > 1e-12)) &&
         ((d3 > 1e-12 && d4 < -1e-12) || (d3 < -1e-12 && d4 > 1e-12));
}

bool loops_cross(const std::vector<std::vector<Vec2>>& loops) {
  struct S {
    std::size_t li, i, n;
    Vec2 a, b;
  };
  std::vector<S> segs;
  for (std::size_t li = 0; li < loops.size(); ++li) {
    const auto& L = loops[li];
    for (std::size_t i = 0; i < L.size(); ++i) {
      segs.push_back({li, i, L.size(), L[i], L[(i + 1) % L.size()]});
    }
  }
  for (std::size_t i = 0; i < segs.size(); ++i) {
    for (std::size_t j = i + 1; j < segs.size(); ++j) {
      const S& s = segs[i];
      const S& t = segs[j];
      const std::size_t gap = s.i > t.i ? s.i - t.i : t.i - s.i;
      if (s.li == t.li && (gap <= 1 || gap == s.n - 1)) {
        continue;
      }
      if (segs_cross2(s.a, s.b, t.a, t.b)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

base::Result<std::vector<std::vector<Vec3>>> offset_face_pts(const Def& d, Id fid, double dist) {
  const Face* f = d.faces.get(fid);
  if (f == nullptr) {
    return base::fail(base::Errc::invalid, tr("m3d.model.there_is_no_such_face"));
  }
  const Vec3 n = face_normal(d, *f);
  const Frame fr = frame(n);
  const Vec3 o = d.vert(f->loops[0][0]);
  const double h = dot(o, n);
  const auto l2 = loops2(d, *f, fr);
  std::vector<std::vector<Vec2>> out2;
  for (const auto& L : l2) {
    auto Q = offset2(L, dist, true);
    if (!Q) {
      return base::fail(base::Errc::invalid, tr("m3d.model.that_is_further_than_this_face"));
    }
    out2.push_back(std::move(*Q));
  }
  if (loops_cross(out2)) {
    return base::fail(base::Errc::invalid, tr("m3d.model.that_is_further_than_this_face_2"));
  }
  std::vector<std::vector<Vec3>> out;
  for (const auto& Q : out2) {
    std::vector<Vec3> P;
    P.reserve(Q.size());
    for (const Vec2& q : Q) {
      P.push_back(add(add(mul(fr.u, q[0]), mul(fr.v, q[1])), mul(n, h)));
    }
    out.push_back(std::move(P));
  }
  return out;
}

base::Result<std::optional<Id>> offset_face(Def& d, Id fid, double dist) {
  const auto pts = offset_face_pts(d, fid, dist);
  if (!pts) {
    return std::unexpected(pts.error());
  }
  const Face f = *d.faces.get(fid);
  const Vec3 n = face_normal(d, f);
  std::vector<std::vector<std::string>> curves_of;
  for (const Loop& l : f.loops) {
    std::vector<std::string> cs;
    for (std::size_t i = 0; i < l.size(); ++i) {
      const auto e = d.edge_between(l[i], l[(i + 1) % l.size()]);
      cs.push_back(e ? d.edges.get(*e)->curve : std::string());
    }
    curves_of.push_back(std::move(cs));
  }
  for (const auto& P : *pts) {
    (void)add_polygon(d, P, n);
  }
  for (std::size_t li = 0; li < pts->size(); ++li) {
    const auto& P = (*pts)[li];
    const auto& cs = curves_of[li];
    const std::size_t m = P.size();
    if (std::ranges::none_of(cs, [](const std::string& c) { return !c.empty(); })) {
      continue;
    }
    const auto kind = [](const std::string& c) { return c.substr(0, c.find(':')); };
    if (std::ranges::all_of(cs, [&](const std::string& c) { return !c.empty() && c == cs[0]; })) {
      (void)tag_curve(d, P, kind(cs[0]), true);
      continue;
    }
    std::size_t start = 0;
    bool found = false;
    for (std::size_t i = 0; i < m; ++i) {
      if (cs[i] != cs[(i + m - 1) % m]) {
        start = i;
        found = true;
        break;
      }
    }
    if (!found) {
      start = 0;
    }
    for (std::size_t k = 0; k < m;) {
      const std::size_t i = (start + k) % m;
      const std::string& c = cs[i];
      std::size_t j = k;
      while (j < m && cs[(start + j) % m] == c) {
        ++j;
      }
      if (!c.empty()) {
        std::vector<Vec3> run;
        run.reserve(j - k + 1);
        for (std::size_t q = 0; q < j - k + 1; ++q) {
          run.push_back(P[(start + k + q) % m]);
        }
        (void)tag_curve(d, run, kind(c));
      }
      k = j;
    }
  }
  if (dist > 0) {
    const auto& first = (*pts)[0];
    for (const auto& [gid, g] : d.faces) {
      if (g.loops[0].size() == first.size() && std::ranges::all_of(g.loops[0], [&](Id v) {
            return std::ranges::any_of(first, [&](const Vec3& p) { return m3d::dist(p, d.vert(v)) <= kEps; });
          })) {
        return std::optional<Id>(gid);
      }
    }
  }
  return std::optional<Id>();
}

base::Result<OffsetChain> offset_chain_pts(const Def& d, std::span<const Id> eids, double dist, const Vec3& n) {
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
  for (const auto& [v, l] : adj) {
    if (l.size() > 2) {
      return base::fail(base::Errc::invalid, tr("m3d.model.offset_works_on_one_chain_of"));
    }
  }
  std::vector<Id> ends;
  for (const auto& [v, l] : adj) {
    if (l.size() == 1) {
      ends.push_back(v);
    }
  }
  OffsetChain out;
  out.closed = ends.empty();
  std::optional<Id> cur;
  if (!ends.empty()) {
    cur = ends[0];
  } else if (!adj.empty()) {
    cur = adj.keys()[0];
  }
  std::optional<Id> prev;
  while (cur && std::ranges::find(out.chain, *cur) == out.chain.end()) {
    out.chain.push_back(*cur);
    std::optional<Id> nx;
    for (const Id x : *adj.get(*cur)) {
      if ((!prev || x != *prev) && std::ranges::find(out.chain, x) == out.chain.end()) {
        nx = x;
        break;
      }
    }
    prev = cur;
    cur = nx;
  }
  if (out.chain.size() != adj.size()) {
    return base::fail(base::Errc::invalid, tr("m3d.model.offset_works_on_one_connected_chain"));
  }
  const std::vector<Vec3> P3 = d.points(out.chain);
  const Frame fr = frame(norm(n));
  // an empty chain: the JS reads P3[0] (undefined) and throws a TypeError; here it is one more refusal
  if (P3.empty()) {
    return base::fail(base::Errc::invalid, tr("m3d.model.offset_works_on_one_connected_chain"));
  }
  const double h = dot(P3[0], fr.n);
  for (const Vec3& p : P3) {
    if (std::abs(dot(p, fr.n) - h) > std::max(kEps, 1e-7 * m3d::dist(p, P3[0]))) {
      return base::fail(base::Errc::invalid, tr("m3d.model.these_edges_are_not_all_in"));
    }
  }
  std::vector<Vec2> p2;
  p2.reserve(P3.size());
  for (const Vec3& p : P3) {
    p2.push_back(to2(fr, p));
  }
  const auto Q = offset2(p2, dist, out.closed);
  if (!Q || (out.closed && loops_cross({*Q}))) {
    return base::fail(base::Errc::invalid, tr("m3d.model.that_is_further_than_these_edges"));
  }
  for (const Vec2& q : *Q) {
    out.pts.push_back(add(add(mul(fr.u, q[0]), mul(fr.v, q[1])), mul(fr.n, h)));
  }
  return out;
}

// ---------------------------------------------------------------- solids

SolidProblems solid_problems(const Def& d) {
  const auto ef = edge_faces(d);
  const auto count = [&](Id a, Id b) {
    const auto it = ef.find(edge_key(a, b));
    return it == ef.end() ? std::size_t{0} : it->second.size();
  };
  SolidProblems out;
  for (const auto& [eid, e] : d.edges) {
    const std::size_t n = count(e.a, e.b);
    if (n == 0) {
      out.stray.push_back(eid);
    } else if (n == 1) {
      out.holes.push_back(eid);
    } else if (n > 2) {
      out.extra.push_back(eid);
    }
  }
  OrderedSet<Id> inner;
  for (const auto& [fid, f] : d.faces) {
    const auto segs = loop_segs(f);
    if (std::ranges::all_of(segs, [&](const auto& s) { return count(s.first, s.second) > 2; })) {
      inner.add(fid);
    }
  }
  const auto runs = [&](Id fid, Id a, Id b) {
    const auto segs = loop_segs(*d.faces.get(fid));
    return std::ranges::any_of(segs, [&](const auto& s) { return s.first == a && s.second == b; });
  };
  std::unordered_map<Id, int> sign;
  for (const auto& [start, sf] : d.faces) {
    if (sign.contains(start) || inner.has(start)) {
      continue;
    }
    sign[start] = 1;
    std::vector<Id> piece = {start};
    std::vector<Id> stack = {start};
    bool open = false;
    while (!stack.empty()) {
      const Id f = stack.back();
      stack.pop_back();
      for (const auto& [a, b] : loop_segs(*d.faces.get(f))) {
        std::vector<Id> fs;
        if (const auto it = ef.find(edge_key(a, b)); it != ef.end()) {
          for (const Id g : it->second) {
            if (!inner.has(g)) {
              fs.push_back(g);
            }
          }
        }
        if (fs.size() != 2) {
          open = true;
          continue;
        }
        const Id g = fs[0] == f ? fs[1] : fs[0];
        const int s = runs(g, a, b) ? -sign.at(f) : sign.at(f);
        if (!sign.contains(g)) {
          sign[g] = s;
          piece.push_back(g);
          stack.push_back(g);
        }
      }
    }
    std::vector<Id> plus;
    std::vector<Id> minus;
    for (const Id f : piece) {
      (sign.at(f) > 0 ? plus : minus).push_back(f);
    }
    bool turn_minus = minus.size() <= plus.size();
    if (!open) {
      double vol = 0;
      const int keep = turn_minus ? 1 : -1;
      for (const Id f : piece) {
        double v = 0;
        for (const auto& [a, b, c] : face_tris(d, *d.faces.get(f))) {
          v += dot(d.vert(a), cross(d.vert(b), d.vert(c))) / 6;
        }
        vol += sign.at(f) == keep ? v : -v;
      }
      if (vol < 0) {
        turn_minus = !turn_minus;
      }
    }
    const std::vector<Id>& turn = turn_minus ? minus : plus;
    out.flipped.insert(out.flipped.end(), turn.begin(), turn.end());
  }
  out.inner = inner.values();
  return out;
}

FixSolid fix_solid(Def& d) {
  const SolidProblems p = solid_problems(d);
  for (const Id eid : p.stray) {
    e_remove(d, eid);
  }
  for (const Id fid : p.inner) {
    d.faces.erase(fid);
  }
  reverse_faces(d, p.flipped);
  OrderedMap<Id, std::vector<Id>> rim;
  for (const Id eid : solid_problems(d).holes) {
    const Edge& e = *d.edges.get(eid);
    for (const auto& [x, y] : {std::pair{e.a, e.b}, std::pair{e.b, e.a}}) {
      std::vector<Id>* l = rim.get(x);
      if (l == nullptr) {
        l = &rim.set(x, {});
      }
      l->push_back(y);
    }
  }
  std::unordered_set<Id> seen;
  std::size_t holes = 0;
  for (const Id v0 : rim.keys()) {
    if (seen.contains(v0)) {
      continue;
    }
    std::vector<Id> loop;
    std::vector<Id> stack = {v0};
    seen.insert(v0);
    while (!stack.empty()) {
      const Id v = stack.back();
      stack.pop_back();
      loop.push_back(v);
      for (const Id w : *rim.get(v)) {
        if (!seen.contains(w)) {
          seen.insert(w);
          stack.push_back(w);
        }
      }
    }
    if (loop.size() < 3) {
      continue;
    }
    const std::vector<Vec3> P = d.points(loop);
    Vec3 n{0, 0, 0};
    for (std::size_t i = 1; i + 1 < P.size(); ++i) {
      const Vec3 c = cross(sub(P[i], P[0]), sub(P[i + 1], P[0]));
      n = dot(c, n) < 0 ? sub(n, c) : add(n, c);
    }
    if (len(n) < 1e-12) {
      continue;
    }
    n = norm(n);
    if (std::ranges::any_of(P, [&](const Vec3& q) { return std::abs(dot(n, sub(q, P[0]))) > kEps; })) {
      continue;
    }
    const std::size_t before = d.faces.size();
    const std::unordered_set<Id> set(loop.begin(), loop.end());
    OrderedSet<Id> rim_edges;
    for (const auto& [id, e] : d.edges) {
      if (set.contains(e.a) && set.contains(e.b)) {
        rim_edges.add(id);
      }
    }
    (void)reface(d, plane_from(n, P[0]), rim_edges);
    if (d.faces.size() > before) {
      ++holes;
    }
  }
  remove_loose_verts(d);
  reverse_faces(d, solid_problems(d).flipped);
  return {p.stray.size(), p.inner.size(), p.flipped.size(), holes};
}

SolidInfo solid_info(const Def& d, Kernel k) {
  const auto ef = edge_faces(d);
  bool closed = !d.faces.empty();
  for (const auto& [key, fs] : ef) {
    if (fs.size() != 2) {
      closed = false;
      break;
    }
  }
  double vol = 0;
  double area = 0;
  for (const auto& [fid, f] : d.faces) {
    area += face_area(d, f);
    for (const auto& [a, b, c] : face_tris(d, f, k)) {
      vol += dot(d.vert(a), cross(d.vert(b), d.vert(c))) / 6;
    }
  }
  return {closed,        closed ? std::optional<double>(vol) : std::nullopt, area, d.faces.size(), d.edges.size(),
          d.verts.size()};
}

}  // namespace cc::m3d
