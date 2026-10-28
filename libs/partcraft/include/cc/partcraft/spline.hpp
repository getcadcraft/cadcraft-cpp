// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::spline — PartCraft 0.5's sketch-spline.js: cubic B-spline math for sketch splines. A curve is a flat
// B-spline {deg, knots, poles, periodic?, n?, P?, tau?} over [knots[deg], knots[poles.length]] (de Boor; Cox–de Boor
// basis, The NURBS Book A2.2 / A2.3); a periodic curve also keeps its n distinct poles P and period knots tau.
// Fit splines pass through their points (chord-length parameters, natural or tangent ends, or periodic); control
// splines are clamped uniform (or given knots) or periodic uniform. Every function is total on its input: a curve whose
// arrays do not agree is refused (base::Error with the JS's TypeError words) where the JS throws.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::spline {

using P2 = std::array<double, 2>;

struct Curve {
  int deg = 3;
  std::vector<double> knots;
  std::vector<P2> poles;
  bool periodic = false;
  int n = 0;                // periodic: the distinct poles' count
  std::vector<P2> P;        // periodic: the distinct poles
  std::vector<double> tau;  // periodic: the period knots (n + 1)
};

[[nodiscard]] Curve from_value(const base::Value& v);
[[nodiscard]] base::Value to_value(const Curve& c);
[[nodiscard]] base::Value points_value(const std::vector<P2>& pts);
[[nodiscard]] std::vector<P2> points_of(const base::Value& v);

[[nodiscard]] std::array<double, 2> domain(const Curve& c);
[[nodiscard]] base::Result<P2> point_at(const Curve& c, double u);
[[nodiscard]] base::Result<std::vector<P2>> derivs_at(const Curve& c, double u, int n = 1);
[[nodiscard]] std::vector<std::array<double, 2>> spans(const Curve& c);
[[nodiscard]] Curve periodic_curve(const std::vector<P2>& P, const std::vector<double>& tau, int periods = 1);
[[nodiscard]] std::vector<double> clamped_uniform(int m, int p);
[[nodiscard]] Curve control_curve(const std::vector<P2>& P, bool closed,
                                  const std::optional<std::vector<double>>& knots);
/// knotProblem(U, m): the file reader's message for a bad stored knot vector, or nothing.
[[nodiscard]] std::optional<std::string> knot_problem(const base::Value& U, double m, const i18n::Catalog& words);
struct FitOptions {
  bool closed = false;
  std::optional<double> a0, a1;  // end tangent angles (radians)
};
[[nodiscard]] base::Result<Curve> interp_fit(const std::vector<P2>& Q, const FitOptions& o, const i18n::Catalog& words);
[[nodiscard]] base::Result<std::vector<P2>> tessellate(const Curve& c, double per_span = 16);
[[nodiscard]] base::Result<std::vector<P2>> tessellate_tol(const Curve& c, double tol);
[[nodiscard]] base::Result<double> area_term(const Curve& c);
[[nodiscard]] base::Result<double> arc_length(const Curve& c);
[[nodiscard]] base::Result<double> closest_param(const Curve& c, P2 p, double per_span = 12);
[[nodiscard]] base::Result<double> curvature_at(const Curve& c, double u);
[[nodiscard]] base::Result<Curve> insert_knot(const Curve& c, double u);
[[nodiscard]] base::Result<Curve> segment(const Curve& c, double ua, double ub);
[[nodiscard]] Curve reverse(const Curve& c);
[[nodiscard]] Curve normalize_knots(const Curve& c);
struct Offset {
  std::vector<P2> Q;
  std::optional<double> a0, a1;
  bool closed = false;
  double err = 0;
  Curve curve;
};
[[nodiscard]] base::Result<Offset> offset_fit(const Curve& c, double d, double tol, const i18n::Catalog& words);

}  // namespace cc::partcraft::spline
