// SPDX-License-Identifier: Apache-2.0
// flatview.js, step for step: see flatview.hpp.
#include "cc/m3d/flatview.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::m3d::flat {

namespace {

constexpr double kS2 = 0.70710678118654757;  // Math.SQRT1_2
// NOLINTNEXTLINE(modernize-use-std-numbers) — 1 / Math.sqrt(3) as the JS computes it, not the rounded constant
const double kS3 = 1 / std::sqrt(3.0);
const double kS6 = 1 / std::sqrt(6.0);

double dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

using Tri = std::array<Vec3, 3>;
using Seg = std::array<Vec3, 2>;

/// Every edge and triangle of the model in the view's frame (u, v, depth toward the viewer)
// NOLINTNEXTLINE(readability-function-cognitive-complexity) — collect(), step for step
void collect(const Model& model, const Frame& V, std::vector<Tri>& tris, std::vector<Seg>& edges) {
  const Def* root = &model.root();
  model.walk([&](const Def& def, const Mat4& m) {
    if (&def == root && def.faces.empty() && def.edges.empty()) {
      return;
    }
    std::unordered_map<std::int64_t, Vec3> P;
    const auto at = [&](std::int64_t vid) {
      if (const auto it = P.find(vid); it != P.end()) {
        return it->second;
      }
      const Vec3 p = apply(m, def.vert(vid));
      const Vec3 q{dot(p, V.u), dot(p, V.v), dot(p, V.w)};
      P.emplace(vid, q);
      return q;
    };
    std::unordered_map<std::int64_t, double> facing;  // face id → its normal · w (toward the viewer > 0)
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::int64_t>> by_edge;
    for (const auto& [fid, f] : def.faces.items()) {
      for (const auto& tr : face_tris(def, f)) {
        tris.push_back({at(tr[0]), at(tr[1]), at(tr[2])});
      }
      const Vec3 n = face_normal(def, f);
      const Vec3 o = apply(m, {0, 0, 0});
      const Vec3 e = apply(m, n);
      facing[fid] = dot({e[0] - o[0], e[1] - o[1], e[2] - o[2]}, V.w);
      for (const auto& l : f.loops) {
        for (std::size_t i = 0; i < l.size(); ++i) {
          const std::int64_t a = l[i];
          const std::int64_t b = l[(i + 1) % l.size()];
          by_edge[{std::min(a, b), std::max(a, b)}].push_back(fid);
        }
      }
    }
    for (const auto& [eid, e] : def.edges.items()) {
      if (e.soft) {
        const auto it = by_edge.find({std::min(e.a, e.b), std::max(e.a, e.b)});
        if (it != by_edge.end() && it->second.size() == 2) {
          const double a = facing[it->second[0]];
          const double b = facing[it->second[1]];
          if (a * b > 0 || (std::abs(a) < 1e-9 && std::abs(b) < 1e-9)) {
            continue;  // not the outline from here
          }
        }
      }
      edges.push_back({at(e.a), at(e.b)});
    }
  });
}

std::size_t cell(double x, double x0, double w, int G) {
  const double c = std::floor((x - x0) / w);
  if (!(c >= 0)) {
    return 0;  // below the box, or not a number
  }
  return static_cast<std::size_t>(std::min(static_cast<double>(G - 1), c));
}

/// merge(): segments grouped by the line they lie on, spans merged; insertion order kept (a JS Map)
struct Line {
  double dx = 0, dy = 0, off = 0;
  std::vector<std::array<double, 2>> spans;
};
struct Lines {
  std::vector<std::pair<std::string, Line>> items;
  std::unordered_map<std::string, std::size_t> index;
};

Lines merge(const std::vector<std::array<double, 4>>& segs, double tol) {
  Lines lines;
  for (const auto& [x1, y1, x2, y2] : segs) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    const double L = base::js_hypot(dx, dy);
    if (L < tol) {
      continue;
    }
    dx /= L;
    dy /= L;
    if (dx < -1e-12 || (std::abs(dx) <= 1e-12 && dy < 0)) {
      dx = -dx;
      dy = -dy;
    }
    const double off = x1 * -dy + y1 * dx;  // distance of the line from the origin
    std::string k;
    base::append_number(k, base::js_round(base::js_atan2(dy, dx) * 1e6));
    k += ':';
    base::append_number(k, base::js_round(off / (tol * 20)));
    auto it = lines.index.find(k);
    if (it == lines.index.end()) {
      it = lines.index.emplace(k, lines.items.size()).first;
      lines.items.emplace_back(k, Line{.dx = dx, .dy = dy, .off = off, .spans = {}});
    }
    Line& ln = lines.items[it->second].second;
    const double a = x1 * dx + y1 * dy;
    const double b = x2 * dx + y2 * dy;
    ln.spans.push_back({std::min(a, b), std::max(a, b)});
  }
  for (auto& [k, ln] : lines.items) {
    std::ranges::stable_sort(ln.spans, [](const auto& p, const auto& q) { return p[0] < q[0]; });
    std::vector<std::array<double, 2>> m;
    for (const auto& sp : ln.spans) {
      if (!m.empty() && sp[0] <= m.back()[1] + tol) {
        m.back()[1] = std::max(m.back()[1], sp[1]);
      } else {
        m.push_back(sp);
      }
    }
    ln.spans = std::move(m);
  }
  return lines;
}

/// flatten(): merged lines → segments; `minus` (merged visible lines) takes away what it covers on the same line
std::vector<std::array<double, 4>> flatten(const Lines& lines, const Lines* minus) {
  std::vector<std::array<double, 4>> out;
  for (const auto& [k, ln] : lines.items) {
    std::vector<std::array<double, 2>> spans = ln.spans;
    if (minus != nullptr) {
      if (const auto it = minus->index.find(k); it != minus->index.end()) {
        const auto& cut = minus->items[it->second].second.spans;
        std::vector<std::array<double, 2>> res;
        for (const auto& [a, b] : spans) {
          std::vector<std::array<double, 2>> parts{{a, b}};
          for (const auto& [c, d] : cut) {
            std::vector<std::array<double, 2>> next;
            for (const auto& [p, q] : parts) {
              if (d <= p || c >= q) {
                next.push_back({p, q});
                continue;
              }
              if (c > p) {
                next.push_back({p, c});
              }
              if (d < q) {
                next.push_back({d, q});
              }
            }
            parts = std::move(next);
          }
          res.insert(res.end(), parts.begin(), parts.end());
        }
        spans = std::move(res);
      }
    }
    for (const auto& [a, b] : spans) {
      if (b - a <= 0) {
        continue;
      }
      const double px = -ln.dy * ln.off;
      const double py = ln.dx * ln.off;
      out.push_back({px + ln.dx * a, py + ln.dy * a, px + ln.dx * b, py + ln.dy * b});
    }
  }
  return out;
}

struct GridTri {
  Vec3 a, b, c;
  double area = 0;
};

}  // namespace

const Frame* view_frame(std::string_view view) {
  static const std::array<std::pair<std::string_view, Frame>, 7> kViews{{
      {"top", {.u = {1, 0, 0}, .v = {0, 1, 0}, .w = {0, 0, 1}}},
      {"bottom", {.u = {1, 0, 0}, .v = {0, -1, 0}, .w = {0, 0, -1}}},
      {"front", {.u = {1, 0, 0}, .v = {0, 0, 1}, .w = {0, -1, 0}}},
      {"back", {.u = {-1, 0, 0}, .v = {0, 0, 1}, .w = {0, 1, 0}}},
      {"right", {.u = {0, 1, 0}, .v = {0, 0, 1}, .w = {1, 0, 0}}},
      {"left", {.u = {0, -1, 0}, .v = {0, 0, 1}, .w = {-1, 0, 0}}},
      {"iso", {.u = {kS2, kS2, 0}, .v = {-kS6, kS6, 2 * kS6}, .w = {kS3, -kS3, kS3}}},
  }};
  for (const auto& [name, f] : kViews) {
    if (name == view) {
      return &f;
    }
  }
  return nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — projectModel(), step for step
Projected project_model(const Model& model, std::string_view view, bool hidden) {
  const Frame* vf = view_frame(view);
  if (vf == nullptr) {
    vf = view_frame("front");
  }
  Projected out;
  if (vf == nullptr) {
    return out;
  }
  const Frame& V = *vf;
  std::vector<Tri> tris;
  std::vector<Seg> edges;
  collect(model, V, tris, edges);
  if (edges.empty()) {
    return out;
  }
  constexpr double kInf = std::numeric_limits<double>::infinity();
  double u0 = kInf;
  double v0 = kInf;
  double u1 = -kInf;
  double v1 = -kInf;
  const auto grow = [&](const Vec3& p) {
    u0 = std::min(u0, p[0]);
    u1 = std::max(u1, p[0]);
    v0 = std::min(v0, p[1]);
    v1 = std::max(v1, p[1]);
  };
  for (const auto& [a, b] : edges) {
    grow(a);
    grow(b);
  }
  for (const Tri& tr : tris) {
    for (const Vec3& p : tr) {
      grow(p);
    }
  }
  out.box = std::array<double, 4>{u0, v0, u1, v1};
  double diag = base::js_hypot(u1 - u0, v1 - v0);
  if (diag == 0 || std::isnan(diag)) {
    diag = 1;
  }
  const double tol = diag * 1e-6;

  // triangles seen from the front (or edge-on ones dropped), in a grid of cells over the box
  constexpr int G = 96;
  double cw = (u1 - u0) / G;
  if (cw == 0 || std::isnan(cw)) {
    cw = 1;
  }
  double ch = (v1 - v0) / G;
  if (ch == 0 || std::isnan(ch)) {
    ch = 1;
  }
  std::vector<std::vector<std::uint32_t>> grid(std::size_t{G} * G);
  std::vector<GridTri> T;
  for (const auto& [a, b, c] : tris) {
    const double area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (std::abs(area) < diag * diag * 1e-12) {
      continue;
    }
    const auto i = static_cast<std::uint32_t>(T.size());
    T.push_back({.a = a, .b = b, .c = c, .area = area});
    const std::size_t cx0 = cell(std::min({a[0], b[0], c[0]}), u0, cw, G);
    const std::size_t cx1 = cell(std::max({a[0], b[0], c[0]}), u0, cw, G);
    const std::size_t cy0 = cell(std::min({a[1], b[1], c[1]}), v0, ch, G);
    const std::size_t cy1 = cell(std::max({a[1], b[1], c[1]}), v0, ch, G);
    for (std::size_t y = cy0; y <= cy1; ++y) {
      for (std::size_t x = cx0; x <= cx1; ++x) {
        grid[y * G + x].push_back(i);
      }
    }
  }
  // is the point (u, v) at depth d behind some triangle?
  const auto behind = [&](double u, double v, double d) {
    for (const std::uint32_t i : grid[cell(v, v0, ch, G) * G + cell(u, u0, cw, G)]) {
      const GridTri& t = T[i];
      const double l1 = ((t.b[0] - u) * (t.c[1] - v) - (t.c[0] - u) * (t.b[1] - v)) / t.area;
      const double l2 = ((t.c[0] - u) * (t.a[1] - v) - (t.a[0] - u) * (t.c[1] - v)) / t.area;
      const double l3 = 1 - l1 - l2;
      if (l1 < 1e-9 || l2 < 1e-9 || l3 < 1e-9) {
        continue;
      }
      if (l1 * t.a[2] + l2 * t.b[2] + l3 * t.c[2] > d + tol) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::array<double, 4>> vis_raw;
  std::vector<std::array<double, 4>> hid_raw;
  for (const auto& [a, b] : edges) {
    const double L = base::js_hypot(b[0] - a[0], b[1] - a[1]);
    if (L < tol) {
      continue;  // an edge pointing at the viewer
    }
    const auto pt = [&](double s) -> Vec3 {
      return {a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s, a[2] + (b[2] - a[2]) * s};
    };
    const auto hid = [&](double s) {
      const Vec3 p = pt(s);
      return behind(p[0], p[1], p[2]);
    };
    const auto n = static_cast<int>(std::max(1.0, std::min(400.0, std::ceil(L / (diag / 300)))));
    struct Run {
      double r0, r1;
      bool h;
    };
    std::vector<Run> runs;
    double s0 = 0;
    bool prev = hid(0.5 / n);
    for (int i = 1; i < n; ++i) {
      const bool cur = hid((i + 0.5) / n);
      if (cur == prev) {
        continue;
      }
      double lo = (i - 0.5) / n;  // the change lies between the two samples
      double hi = (i + 0.5) / n;
      while ((hi - lo) * L > tol) {
        const double mid = (lo + hi) / 2;
        if (hid(mid) == prev) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      runs.push_back({s0, (lo + hi) / 2, prev});
      s0 = (lo + hi) / 2;
      prev = cur;
    }
    runs.push_back({s0, 1, prev});
    for (const Run& r : runs) {
      if ((r.r1 - r.r0) * L < tol * 10) {
        continue;
      }
      const Vec3 p = pt(r.r0);
      const Vec3 q = pt(r.r1);
      (r.h ? hid_raw : vis_raw).push_back({p[0], p[1], q[0], q[1]});
    }
  }
  // pieces on one line (a box's back edges behind its front ones, a circle seen edge-on) become one line; a hidden
  // piece where a visible one is drawn goes
  const Lines vis = merge(vis_raw, tol);
  out.visible = flatten(vis, nullptr);
  if (hidden) {
    out.hidden = flatten(merge(hid_raw, tol), &vis);
  }
  return out;
}

std::optional<double> parse_scale(std::string_view s) {
  // /^\s*(\d+(?:\.\d+)?)\s*[:/]\s*(\d+(?:\.\d+)?)\s*$/ else Number(s.trim())
  const std::string_view t = base::js_trim(s);
  const auto number = [](std::string_view x, std::size_t& i) -> std::optional<double> {
    const std::size_t start = i;
    while (i < x.size() && x[i] >= '0' && x[i] <= '9') {
      ++i;
    }
    if (i == start) {
      return std::nullopt;
    }
    if (i < x.size() && x[i] == '.') {
      const std::size_t dot_at = i++;
      const std::size_t frac = i;
      while (i < x.size() && x[i] >= '0' && x[i] <= '9') {
        ++i;
      }
      if (i == frac) {
        i = dot_at;  // "\d+" then a lone "." does not match the decimal part
      }
    }
    return base::js_number(x.substr(start, i - start));
  };
  double v = std::numeric_limits<double>::quiet_NaN();
  std::size_t i = 0;
  bool ratio = false;
  if (const auto a = number(t, i)) {
    std::size_t j = i;
    while (j < t.size() && base::js_space_at(t, j) > 0) {
      j += base::js_space_at(t, j);
    }
    if (j < t.size() && (t[j] == ':' || t[j] == '/')) {
      ++j;
      while (j < t.size() && base::js_space_at(t, j) > 0) {
        j += base::js_space_at(t, j);
      }
      std::size_t k = j;
      if (const auto b = number(t, k); b && k == t.size()) {
        v = *a / *b;
        ratio = true;
      }
    }
  }
  if (!ratio) {
    v = base::js_number(t);
  }
  if (std::isfinite(v) && v > 0 && v < 1e6) {
    return v;
  }
  return std::nullopt;
}

}  // namespace cc::m3d::flat
