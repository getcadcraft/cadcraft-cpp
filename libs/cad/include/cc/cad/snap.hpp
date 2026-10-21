// SPDX-License-Identifier: Apache-2.0
// cc::cad — object snaps and grips (CadCraft 0.5 geom.js snapPoints / grips / gripMove, view.js snap).
//
//   auto s = best_snap(nearby, cursor, 10 / zoom, {"end", "mid", "cen", …}, blocks, base);  // s->kind, s->x, s->y
//   for (const Grip& g : grips(e)) …;  e2 = grip_move(e, g.key, to);
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/geom.hpp"

namespace cc::cad {

/// end, mid, cen, quad, int, perp, tan, node, ins, near
using SnapKinds = std::set<std::string>;

struct Snap {
  std::string kind;
  double x = 0, y = 0;
  std::optional<std::array<double, 3>> defer;  // a tangent with nothing to measure from yet: the circle (cx, cy, r)
  std::optional<int> pr;                       // its priority when not its kind's (a point cloud's point: 3)
};

/// (best_snap: `extra` is view.js snapExtra — a point cloud's nearest point — asked when Node is on.)
/// geom.js snapPoints(e, q, r, kinds, blocks, base): the snap points of one object within r of q.
[[nodiscard]] std::vector<Snap> snap_points(const base::Value& e, P2 q, double r, const SnapKinds& kinds,
                                            const BlockLookup& blocks, std::optional<P2> base);
/// view.js snap(p, { kinds, base }) over the objects near p: the best by priority (end, int, node first; nearest last)
/// then distance, intersections among the first 60 included.
[[nodiscard]] std::optional<Snap> best_snap(const std::vector<const base::Value*>& nearby, P2 p, double r,
                                            const SnapKinds& kinds, const BlockLookup& blocks, std::optional<P2> base,
                                            const std::function<std::optional<Snap>(P2, double)>& extra = {});
/// geom.js tangentFrom(p, circle): the points where lines from p touch the circle.
[[nodiscard]] std::vector<P2> tangent_from(P2 p, double cx, double cy, double r);

struct Grip {
  double x = 0, y = 0;
  std::string key;
};
/// geom.js grips(e): the points a selected object can be pulled by.
[[nodiscard]] std::vector<Grip> grips(const base::Value& e);
/// geom.js gripMove(e, key, to): the object with that grip moved to `to`.
[[nodiscard]] base::Value grip_move(const base::Value& e, const std::string& key, P2 to);

}  // namespace cc::cad
