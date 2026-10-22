// SPDX-License-Identifier: Apache-2.0
// lasso-core.js, ported line by line.
#include "cc/appkit/lasso.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cc/base/js_math.hpp"

namespace cc::appkit {
namespace {

double dist(LPt a, LPt b) {
  return base::js_hypot(a.x - b.x, a.y - b.y);
}
int orient(double px, double py, double qx, double qy, double rx, double ry) {
  const double v = (qx - px) * (ry - py) - (qy - py) * (rx - px);
  if (std::abs(v) < 1e-12) {
    return 0;
  }
  return v > 0 ? 1 : -1;  // NaN → -1, as the JS's ternary gives
}
bool on(double px, double py, double qx, double qy, double rx, double ry) {
  return std::min(px, qx) - 1e-12 <= rx && rx <= std::max(px, qx) + 1e-12 && std::min(py, qy) - 1e-12 <= ry &&
         ry <= std::max(py, qy) + 1e-12;
}

}  // namespace

std::string lasso_mode(LPt start, LPt cur, double min_px) {
  const double dx = cur.x - start.x;
  if (std::abs(dx) < min_px) {
    return {};
  }
  return dx > 0 ? "window" : "crossing";
}

std::string next_mode(std::string_view mode) {
  if (mode == "window") {
    return "crossing";
  }
  if (mode == "crossing") {
    return "fence";
  }
  return "window";  // fence, or not a mode (indexOf -1 → 0)
}

std::vector<LPt> thin(const std::vector<LPt>& pts, double min_step) {
  std::vector<LPt> out;
  for (const LPt& p : pts) {
    if (out.empty() || dist(out.back(), p) >= min_step) {
      out.push_back(p);
    }
  }
  if (pts.size() > 1 && !out.empty() && dist(out.back(), pts.back()) > 1e-12) {
    out.push_back(pts.back());
  }
  return out;
}

bool point_in_poly(LPt p, const std::vector<LPt>& pts) {
  bool inside = false;
  const std::size_t n = pts.size();
  for (std::size_t i = 0, j = n == 0 ? 0 : n - 1; i < n; j = i++) {
    const LPt a = pts[i];
    const LPt b = pts[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < ((b.x - a.x) * (p.y - a.y)) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

bool segs_cross(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
  const int o1 = orient(ax, ay, bx, by, cx, cy);
  const int o2 = orient(ax, ay, bx, by, dx, dy);
  const int o3 = orient(cx, cy, dx, dy, ax, ay);
  const int o4 = orient(cx, cy, dx, dy, bx, by);
  if (o1 != o2 && o3 != o4) {
    return true;
  }
  return (o1 == 0 && on(ax, ay, bx, by, cx, cy)) || (o2 == 0 && on(ax, ay, bx, by, dx, dy)) ||
         (o3 == 0 && on(cx, cy, dx, dy, ax, ay)) || (o4 == 0 && on(cx, cy, dx, dy, bx, by));
}

std::vector<std::size_t> lasso_select(const std::vector<LassoItem>& items, const std::vector<LPt>& pts,
                                      std::string_view mode) {
  std::vector<std::size_t> out;
  if (pts.size() < 2) {
    return out;
  }
  std::vector<LSeg> path;
  const std::size_t n = mode == "fence" ? pts.size() - 1 : pts.size();
  for (std::size_t i = 0; i < n; ++i) {
    const LPt a = pts[i];
    const LPt b = pts[(i + 1) % pts.size()];
    if (dist(a, b) > 1e-12) {
      path.push_back({a.x, a.y, b.x, b.y});
    }
  }
  if (path.empty() || (mode != "fence" && pts.size() < 3)) {
    return out;
  }
  constexpr double kInf = std::numeric_limits<double>::infinity();
  double lx0 = kInf;
  double ly0 = kInf;
  double lx1 = -kInf;
  double ly1 = -kInf;
  auto jmin = [](double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::min(a, b); };
  auto jmax = [](double a, double b) { return (std::isnan(a) || std::isnan(b)) ? std::nan("") : std::max(a, b); };
  for (const LPt& p : pts) {
    lx0 = jmin(lx0, p.x);
    ly0 = jmin(ly0, p.y);
    lx1 = jmax(lx1, p.x);
    ly1 = jmax(ly1, p.y);
  }
  auto crosses = [&](const std::vector<LSeg>& ss) {
    return std::ranges::any_of(ss, [&](const LSeg& s) {
      return std::ranges::any_of(
          path, [&](const LSeg& q) { return segs_cross(s[0], s[1], s[2], s[3], q[0], q[1], q[2], q[3]); });
    });
  };
  for (std::size_t k = 0; k < items.size(); ++k) {
    const LassoItem& it = items[k];
    if (!it.box) {
      continue;
    }
    const LBox b = *it.box;
    const bool touches_box = b.max_x >= lx0 && b.min_x <= lx1 && b.max_y >= ly0 && b.min_y <= ly1;  // NaN: false
    if (!touches_box) {
      continue;
    }
    const std::vector<LSeg>& ss = it.segs;
    if (mode == "fence") {
      if (crosses(ss)) {
        out.push_back(k);
      }
      continue;
    }
    if (mode == "window") {
      const bool in_box = b.min_x >= lx0 && b.max_x <= lx1 && b.min_y >= ly0 && b.max_y <= ly1;
      if (!in_box) {
        continue;
      }
      if (!ss.empty() && !crosses(ss) &&
          std::ranges::all_of(ss, [&](const LSeg& s) { return point_in_poly({s[0], s[1]}, pts); })) {
        out.push_back(k);
      }
      continue;
    }
    if (crosses(ss) || std::ranges::any_of(ss, [&](const LSeg& s) { return point_in_poly({s[0], s[1]}, pts); }) ||
        (it.filled && std::ranges::any_of(pts, [&](const LPt& q) {
           return q.x >= b.min_x && q.x <= b.max_x && q.y >= b.min_y && q.y <= b.max_y;
         }))) {
      out.push_back(k);
    }
  }
  return out;
}

}  // namespace cc::appkit
