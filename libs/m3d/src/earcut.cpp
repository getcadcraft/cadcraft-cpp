// SPDX-License-Identifier: Apache-2.0 AND ISC
// earcut: polygon-with-holes triangulation, ported from CadCraft 0.5's m3d/earcut.js, which follows mapbox/earcut
// closely (ISC licence, © Mapbox; the notice is kept below as it asks). The linked list is a pool of nodes addressed
// by index (no pointers); the z-order hash is left out, as in the JS.
//
// ISC License — Copyright (c) 2016, Mapbox.
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
// granted, provided that the above copyright notice and this permission notice appear in all copies. THE SOFTWARE IS
// PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
// DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#include "earcut.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cc::m3d::detail {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
constexpr int kMaxDepth = 4096;  // earcutLinked ↔ splitEarcut recursion; the JS has no cap (a guard for hostile faces)

struct Node {
  std::size_t i = 0;  // vertex index
  double x = 0, y = 0;
  std::size_t prev = kNone, next = kNone;
  bool steiner = false;
};

class Earcut {
 public:
  std::vector<std::size_t> run(std::span<const double> data, std::span<const std::size_t> holes) {
    const bool has_holes = !holes.empty();
    const std::size_t outer_len = has_holes ? std::min(holes[0] * 2, data.size()) : data.size();
    std::size_t outer = linked_list(data, 0, outer_len, true);
    if (outer == kNone || n(outer).next == n(outer).prev) {
      return std::move(tris_);
    }
    if (has_holes) {
      outer = eliminate_holes(data, holes, outer);
    }
    earcut_linked(outer, 0, 0);
    return std::move(tris_);
  }

 private:
  std::vector<Node> pool_;
  std::vector<std::size_t> tris_;

  Node& n(std::size_t k) { return pool_.at(k); }
  [[nodiscard]] const Node& n(std::size_t k) const { return pool_.at(k); }
  [[nodiscard]] std::size_t nx(std::size_t k) const { return n(k).next; }
  [[nodiscard]] std::size_t pv(std::size_t k) const { return n(k).prev; }

  [[nodiscard]] double area(std::size_t p, std::size_t q, std::size_t r) const {
    const Node& P = n(p);
    const Node& Q = n(q);
    const Node& R = n(r);
    return (Q.y - P.y) * (R.x - Q.x) - (Q.x - P.x) * (R.y - Q.y);
  }
  [[nodiscard]] bool equals(std::size_t a, std::size_t b) const { return n(a).x == n(b).x && n(a).y == n(b).y; }

  std::size_t create_node(std::size_t i, double x, double y) {
    pool_.push_back(Node{.i = i, .x = x, .y = y, .prev = kNone, .next = kNone, .steiner = false});
    return pool_.size() - 1;
  }
  std::size_t insert_node(std::size_t i, double x, double y, std::size_t last) {
    const std::size_t p = create_node(i, x, y);
    if (last == kNone) {
      n(p).prev = p;
      n(p).next = p;
    } else {
      n(p).next = nx(last);
      n(p).prev = last;
      n(nx(last)).prev = p;
      n(last).next = p;
    }
    return p;
  }
  void remove_node(std::size_t p) {
    n(nx(p)).prev = pv(p);
    n(pv(p)).next = nx(p);
  }

  static double signed_area(std::span<const double> d, std::size_t start, std::size_t end) {
    double sum = 0;
    for (std::size_t i = start, j = end - 2; i < end; i += 2) {
      sum += (d[j] - d[i]) * (d[i + 1] + d[j + 1]);
      j = i;
    }
    return sum;
  }

  std::size_t linked_list(std::span<const double> d, std::size_t start, std::size_t end, bool clockwise) {
    std::size_t last = kNone;
    end = start + (end - start) / 2 * 2;  // whole points
    if (end <= start) {
      return kNone;
    }
    if (clockwise == (signed_area(d, start, end) > 0)) {
      for (std::size_t i = start; i < end; i += 2) {
        last = insert_node(i / 2, d[i], d[i + 1], last);
      }
    } else {
      for (std::size_t i = end; i >= start + 2; i -= 2) {
        last = insert_node((i - 2) / 2, d[i - 2], d[i - 1], last);
      }
    }
    if (last != kNone && equals(last, nx(last))) {
      remove_node(last);
      last = nx(last);
    }
    return last;
  }

  std::size_t filter_points(std::size_t start, std::size_t end = kNone) {
    if (start == kNone) {
      return start;
    }
    if (end == kNone) {
      end = start;
    }
    std::size_t p = start;
    bool again = false;
    do {
      again = false;
      if (!n(p).steiner && (equals(p, nx(p)) || area(pv(p), p, nx(p)) == 0)) {
        remove_node(p);
        p = end = pv(p);
        if (p == nx(p)) {
          break;
        }
        again = true;
      } else {
        p = nx(p);
      }
    } while (again || p != end);
    return end;
  }

  // NOLINTNEXTLINE(misc-no-recursion) — mapbox's passes; depth capped
  void earcut_linked(std::size_t ear, int pass, int depth) {
    if (ear == kNone || depth > kMaxDepth) {
      return;
    }
    std::size_t stop = ear;
    while (pv(ear) != nx(ear)) {
      const std::size_t prev = pv(ear);
      const std::size_t next = nx(ear);
      if (is_ear(ear)) {
        tris_.push_back(n(prev).i);
        tris_.push_back(n(ear).i);
        tris_.push_back(n(next).i);
        remove_node(ear);
        ear = nx(next);
        stop = nx(next);
        continue;
      }
      ear = next;
      if (ear == stop) {
        if (pass == 0) {
          earcut_linked(filter_points(ear), 1, depth + 1);
        } else if (pass == 1) {
          ear = cure_local_intersections(filter_points(ear));
          earcut_linked(ear, 2, depth + 1);
        } else if (pass == 2) {
          split_earcut(ear, depth + 1);
        }
        break;
      }
    }
  }

  static bool point_in_triangle(double ax, double ay, double bx, double by, double cx, double cy, double px,
                                double py) {
    return (cx - px) * (ay - py) >= (ax - px) * (cy - py) && (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
           (bx - px) * (cy - py) >= (cx - px) * (by - py);
  }

  [[nodiscard]] bool is_ear(std::size_t ear) const {
    const std::size_t a = pv(ear);
    const std::size_t b = ear;
    const std::size_t c = nx(ear);
    if (area(a, b, c) >= 0) {
      return false;
    }
    const double ax = n(a).x;
    const double bx = n(b).x;
    const double cx = n(c).x;
    const double ay = n(a).y;
    const double by = n(b).y;
    const double cy = n(c).y;
    const double x0 = std::min({ax, bx, cx});
    const double y0 = std::min({ay, by, cy});
    const double x1 = std::max({ax, bx, cx});
    const double y1 = std::max({ay, by, cy});
    std::size_t p = nx(c);
    while (p != a) {
      const double px = n(p).x;
      const double py = n(p).y;
      if (px >= x0 && px <= x1 && py >= y0 && py <= y1 && (ax != px || ay != py) &&
          point_in_triangle(ax, ay, bx, by, cx, cy, px, py) && area(pv(p), p, nx(p)) >= 0) {
        return false;
      }
      p = nx(p);
    }
    return true;
  }

  static int sign(double v) {
    if (v > 0) {
      return 1;
    }
    return v < 0 ? -1 : 0;
  }
  [[nodiscard]] bool on_segment(std::size_t p, std::size_t q, std::size_t r) const {
    return n(q).x <= std::max(n(p).x, n(r).x) && n(q).x >= std::min(n(p).x, n(r).x) &&
           n(q).y <= std::max(n(p).y, n(r).y) && n(q).y >= std::min(n(p).y, n(r).y);
  }
  [[nodiscard]] bool intersects(std::size_t p1, std::size_t q1, std::size_t p2, std::size_t q2) const {
    const int o1 = sign(area(p1, q1, p2));
    const int o2 = sign(area(p1, q1, q2));
    const int o3 = sign(area(p2, q2, p1));
    const int o4 = sign(area(p2, q2, q1));
    if (o1 != o2 && o3 != o4) {
      return true;
    }
    return (o1 == 0 && on_segment(p1, p2, q1)) || (o2 == 0 && on_segment(p1, q2, q1)) ||
           (o3 == 0 && on_segment(p2, p1, q2)) || (o4 == 0 && on_segment(p2, q1, q2));
  }
  [[nodiscard]] bool locally_inside(std::size_t a, std::size_t b) const {
    return area(pv(a), a, nx(a)) < 0 ? area(a, b, nx(a)) >= 0 && area(a, pv(a), b) >= 0
                                     : area(a, b, pv(a)) < 0 || area(a, nx(a), b) < 0;
  }

  std::size_t cure_local_intersections(std::size_t start) {
    std::size_t p = start;
    do {
      const std::size_t a = pv(p);
      const std::size_t b = nx(nx(p));
      if (!equals(a, b) && intersects(a, p, nx(p), b) && locally_inside(a, b) && locally_inside(b, a)) {
        tris_.push_back(n(a).i);
        tris_.push_back(n(p).i);
        tris_.push_back(n(b).i);
        remove_node(p);
        remove_node(nx(p));
        p = start = b;
      }
      p = nx(p);
    } while (p != start);
    return filter_points(p);
  }

  [[nodiscard]] bool intersects_polygon(std::size_t a, std::size_t b) const {
    std::size_t p = a;
    do {
      if (n(p).i != n(a).i && n(nx(p)).i != n(a).i && n(p).i != n(b).i && n(nx(p)).i != n(b).i &&
          intersects(p, nx(p), a, b)) {
        return true;
      }
      p = nx(p);
    } while (p != a);
    return false;
  }
  [[nodiscard]] bool middle_inside(std::size_t a, std::size_t b) const {
    std::size_t p = a;
    bool inside = false;
    const double px = (n(a).x + n(b).x) / 2;
    const double py = (n(a).y + n(b).y) / 2;
    do {
      const Node& P = n(p);
      const Node& Q = n(nx(p));
      if (((P.y > py) != (Q.y > py)) && Q.y != P.y && (px < (Q.x - P.x) * (py - P.y) / (Q.y - P.y) + P.x)) {
        inside = !inside;
      }
      p = nx(p);
    } while (p != a);
    return inside;
  }
  [[nodiscard]] bool is_valid_diagonal(std::size_t a, std::size_t b) const {
    return n(nx(a)).i != n(b).i && n(pv(a)).i != n(b).i && !intersects_polygon(a, b) &&
           ((locally_inside(a, b) && locally_inside(b, a) && middle_inside(a, b) &&
             (area(pv(a), a, pv(b)) != 0 || area(a, pv(b), b) != 0)) ||
            (equals(a, b) && area(pv(a), a, nx(a)) > 0 && area(pv(b), b, nx(b)) > 0));
  }

  std::size_t split_polygon(std::size_t a, std::size_t b) {
    const std::size_t a2 = create_node(n(a).i, n(a).x, n(a).y);
    const std::size_t b2 = create_node(n(b).i, n(b).x, n(b).y);
    const std::size_t an = nx(a);
    const std::size_t bp = pv(b);
    n(a).next = b;
    n(b).prev = a;
    n(a2).next = an;
    n(an).prev = a2;
    n(b2).next = a2;
    n(a2).prev = b2;
    n(bp).next = b2;
    n(b2).prev = bp;
    return b2;
  }

  // NOLINTNEXTLINE(misc-no-recursion) — mapbox's split; depth capped
  void split_earcut(std::size_t start, int depth) {
    std::size_t a = start;
    do {
      std::size_t b = nx(nx(a));
      while (b != pv(a)) {
        if (n(a).i != n(b).i && is_valid_diagonal(a, b)) {
          std::size_t c = split_polygon(a, b);
          a = filter_points(a, nx(a));
          c = filter_points(c, nx(c));
          earcut_linked(a, 0, depth);
          earcut_linked(c, 0, depth);
          return;
        }
        b = nx(b);
      }
      a = nx(a);
    } while (a != start);
  }

  [[nodiscard]] std::size_t get_leftmost(std::size_t start) const {
    std::size_t p = start;
    std::size_t left = start;
    do {
      if (n(p).x < n(left).x || (n(p).x == n(left).x && n(p).y < n(left).y)) {
        left = p;
      }
      p = nx(p);
    } while (p != start);
    return left;
  }

  [[nodiscard]] bool sector_contains_sector(std::size_t m, std::size_t p) const {
    return area(pv(m), m, pv(p)) < 0 && area(nx(p), m, nx(m)) < 0;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity) — mapbox's findHoleBridge, as is
  [[nodiscard]] std::size_t find_hole_bridge(std::size_t hole, std::size_t outer) const {
    std::size_t p = outer;
    const double hx = n(hole).x;
    const double hy = n(hole).y;
    double qx = -std::numeric_limits<double>::infinity();
    std::size_t m = kNone;
    if (equals(hole, p)) {
      return p;
    }
    do {
      if (equals(hole, nx(p))) {
        return nx(p);
      }
      const Node& P = n(p);
      const Node& Q = n(nx(p));
      if (hy <= P.y && hy >= Q.y && Q.y != P.y) {
        const double x = P.x + (hy - P.y) * (Q.x - P.x) / (Q.y - P.y);
        if (x <= hx && x > qx) {
          qx = x;
          m = P.x < Q.x ? p : nx(p);
          if (x == hx) {
            return m;
          }
        }
      }
      p = nx(p);
    } while (p != outer);
    if (m == kNone) {
      return kNone;
    }
    const std::size_t stop = m;
    const double mx = n(m).x;
    const double my = n(m).y;
    double tan_min = std::numeric_limits<double>::infinity();
    p = m;
    do {
      const double px = n(p).x;
      const double py = n(p).y;
      if (hx >= px && px >= mx && hx != px &&
          point_in_triangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, px, py)) {
        const double tan = std::abs(hy - py) / (hx - px);
        if (locally_inside(p, hole) &&
            (tan < tan_min || (tan == tan_min && (px > n(m).x || (px == n(m).x && sector_contains_sector(m, p)))))) {
          m = p;
          tan_min = tan;
        }
      }
      p = nx(p);
    } while (p != stop);
    return m;
  }

  std::size_t eliminate_hole(std::size_t hole, std::size_t outer) {
    const std::size_t bridge = find_hole_bridge(hole, outer);
    if (bridge == kNone) {
      return outer;
    }
    const std::size_t rev = split_polygon(bridge, hole);
    filter_points(rev, nx(rev));
    return filter_points(bridge, nx(bridge));
  }

  std::size_t eliminate_holes(std::span<const double> d, std::span<const std::size_t> holes, std::size_t outer) {
    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < holes.size(); ++i) {
      const std::size_t start = std::min(holes[i] * 2, d.size());
      const std::size_t end = i + 1 < holes.size() ? std::min(holes[i + 1] * 2, d.size()) : d.size();
      if (end <= start) {
        continue;
      }
      const std::size_t list = linked_list(d, start, end, false);
      if (list == kNone) {
        continue;
      }
      if (list == nx(list)) {
        n(list).steiner = true;
      }
      queue.push_back(get_leftmost(list));
    }
    std::ranges::stable_sort(queue, [&](std::size_t a, std::size_t b) {
      const double dx = n(a).x - n(b).x;
      const double c = dx != 0 && !std::isnan(dx) ? dx : n(a).y - n(b).y;
      return c < 0;
    });
    for (const std::size_t h : queue) {
      outer = eliminate_hole(h, outer);
    }
    return outer;
  }
};

}  // namespace

std::vector<std::size_t> earcut(std::span<const double> data, std::span<const std::size_t> holes) {
  return Earcut().run(data, holes);
}

}  // namespace cc::m3d::detail
