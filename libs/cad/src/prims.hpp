// SPDX-License-Identifier: Apache-2.0
// cc::cad internals: geom.js's primitives (geom.cpp) for the editing geometry (edit.cpp).
#pragma once

#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "util.hpp"

namespace cc::cad::detail {

P2 m_apply(const Mat& m, double x, double y);
double dist(double ax, double ay, double bx, double by);
/// geom.js onSweep(a, a0, a1, tol)
bool on_sweep(double a, double a0, double a1, double tol = 1e-9);
/// geom.js arcRange(a0, a1): both into [0, 2π), a1 > a0
std::pair<double, double> arc_range(double a0, double a1);
P2 prim_point(const Prim& p, double t);
Prim line_prim(double x1, double y1, double x2, double y2);
/// geom.js bulgeArc(p, q, b)
Prim bulge_arc(double px, double py, double qx, double qy, double b);
/// geom.js polyPrims(e)
void poly_prims(const base::Value& e, std::vector<Prim>& out);
/// geom.js ellipsePoints(e, steps)
std::vector<P2> ellipse_points(const base::Value& e, double steps = 72);
/// geom.js prims(e, blocks, depth)
void prims(const base::Value& e, const BlockLookup& blocks, int depth, std::vector<Prim>& out);

}  // namespace cc::cad::detail
