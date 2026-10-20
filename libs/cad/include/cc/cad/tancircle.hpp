// SPDX-License-Identifier: Apache-2.0
// cc::cad — tancircle.js (CadCraft 0.5 DR.1 / DR.2): circles tangent to picked objects.
//
//   auto a = cad::curve_at(e, pick, blocks);                 // the line or full circle under a pick
//   auto c = cad::circle_ttr(*a, pa, *b, pb, r);             // tangent to both, radius r, touching nearest the picks
//   auto c = cad::circle_ttt(*a, pa, *b, pb, *c3, pc);       // tangent to three (Apollonius)
#pragma once

#include <optional>

#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"

namespace cc::cad {

/// { k: 'L', x, y, dx, dy } (a unit direction) or { k: 'C', cx, cy, r }.
struct Curve {
  bool line = true;
  double x = 0, y = 0, dx = 0, dy = 0;
  double cx = 0, cy = 0, r = 0;
};
struct Circle {
  double cx = 0, cy = 0, r = 0;
};

/// curveAt(e, pick, blocks): the straight line or the whole circle under the pick (a polyline's segment, a block's
/// piece, an arc's circle), or nothing (text, points …).
[[nodiscard]] std::optional<Curve> curve_at(const base::Value& e, P2 pick, const BlockLookup& blocks);
/// touchPoint(o, circle): where the circle touches o.
[[nodiscard]] P2 touch_point(const Curve& o, const Circle& c);
/// circleTTR(a, pa, b, pb, r)
[[nodiscard]] std::optional<Circle> circle_ttr(const Curve& a, P2 pa, const Curve& b, P2 pb, double r);
/// circleTTT(a, pa, b, pb, c, pc)
[[nodiscard]] std::optional<Circle> circle_ttt(const Curve& a, P2 pa, const Curve& b, P2 pb, const Curve& c, P2 pc);

}  // namespace cc::cad
