// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::sketch — PartCraft 0.5's sketch.js and solver.js: the constrained 2D sketch. A sketch is plain JSON
// data, kept here as the JS-shaped value it saves as ({seq, entities, constraints}, the JS's key order), and edited in
// place:
//
//   point  {id, type:'point', x, y, construction?}         line   {id, type:'line', p1, p2, construction?}
//   circle {id, type:'circle', c, r, construction?}        arc    {id, type:'arc', c, s, e, construction?}
//   spline {id, type:'spline', kind:'fit'|'control', pts, closed?, a0?, a1?, knots?, construction?}
//   constraint {id, type, refs, value?, sign?, kind?, of?, at?, off?}
//
// The origin is the virtual point "O". Every relation becomes residual equations (equations()); solve_sketch runs the
// damped least-norm Newton of solver.js on them and analyze() the rank, redundancy and degrees of freedom. A JS throw
// is an Error here, with the JS's words (its own messages, and V8's TypeError words where 0.5 trips on odd input).
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::sketch {

inline constexpr std::string_view kOrigin = "O";

[[nodiscard]] base::Value new_sketch();
/// byId(sk, id): the entity (a copy), the origin's {id, type, x, y, origin}, or null.
[[nodiscard]] base::Value by_id(const base::Value& sk, const base::Value& id);
/// pointsOf(e): the point ids an entity is made of.
[[nodiscard]] std::vector<base::Value> points_of(const base::Value& e);
/// endsOf(e)
[[nodiscard]] std::vector<base::Value> ends_of(const base::Value& e);

// ---- building (each mutates `sk`)
[[nodiscard]] std::string add_point(base::Value& sk, double x, double y, const base::Value& opts);
/// a, b: [x, y] (a new point) or a point id (a new point joined to it by a coincident relation).
[[nodiscard]] base::Result<base::Value> add_line(base::Value& sk, const base::Value& a, const base::Value& b,
                                                 const base::Value& opts, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> add_circle(base::Value& sk, const base::Value& c, const base::Value& r,
                                                   const base::Value& opts, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> add_arc(base::Value& sk, const base::Value& c, const base::Value& s,
                                                const base::Value& e, const base::Value& opts,
                                                const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> add_rect(base::Value& sk, const base::Value& a, const base::Value& b,
                                                 const base::Value& opts, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> add_spline(base::Value& sk, const base::Value& pts, const base::Value& opts,
                                                   const i18n::Catalog& words);
/// matchSig(sk, type, refs): {sig, refs} (refs reordered P < L < R < S), or null.
[[nodiscard]] base::Result<base::Value> match_sig(const base::Value& sk, const base::Value& type,
                                                  const base::Value& refs);
[[nodiscard]] base::Result<base::Value> applicable(const base::Value& sk, const base::Value& refs);
/// addConstraint(sk, type, refs, value, extra): the constraint added (value undefined = the current measurement).
[[nodiscard]] base::Result<base::Value> add_constraint(base::Value& sk, const base::Value& type,
                                                       const base::Value& refs, const base::Value& value,
                                                       const base::Value& extra, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> measure(const base::Value& sk, const base::Value& type,
                                                const base::Value& refs);
/// describe(c): "Distance 50", "Parallel" (the type itself for an unknown one).
[[nodiscard]] base::Value describe(const base::Value& c, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> remove_entities(base::Value& sk, const base::Value& ids);
void remove_constraints(base::Value& sk, const base::Value& ids);
[[nodiscard]] base::Result<base::Value> fillet_corner(base::Value& sk, const base::Value& point_id, double r,
                                                      const i18n::Catalog& words);
[[nodiscard]] double extent(const base::Value& sk);
[[nodiscard]] base::Result<base::Value> bounds(const base::Value& sk, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> loose_points(const base::Value& sk);
[[nodiscard]] base::Result<base::Value> end_meet(const base::Value& sk, const base::Value& a, const base::Value& b);
[[nodiscard]] base::Result<base::Value> tess_arc(const base::Value& sk, const base::Value& arc, double seg_full);
[[nodiscard]] base::Result<base::Value> tess_spline(const base::Value& sk, const base::Value& e, double per_span,
                                                    const i18n::Catalog& words);
/// profile(sk, segFull): {loops, curves} or {error, at?}.
[[nodiscard]] base::Result<base::Value> profile(const base::Value& sk, double seg_full, const i18n::Catalog& words);
[[nodiscard]] bool inside_loops(const base::Value& loops, std::array<double, 2> p);
[[nodiscard]] double loop_area(const base::Value& loop);
/// readSketch(raw): a clean copy of untrusted sketch JSON.
[[nodiscard]] base::Result<base::Value> read_sketch(const base::Value& raw, const i18n::Catalog& words);

// ---- equations and the solver
struct Vars {
  std::vector<std::pair<std::string, std::vector<std::size_t>>> map;  // id (or "<spline>:a0") → indices into x
  std::vector<double> x;
  [[nodiscard]] const std::vector<std::size_t>* find(std::string_view id) const;
};
/// buildVars(sk, pinned): every point's x, y (unless pinned), then circles' r and fit splines' end angles.
[[nodiscard]] Vars build_vars(const base::Value& sk, const std::vector<std::string>& pinned = {});
/// The residuals [cid, f] at the sketch as stored (the parity ops' sketch.residuals).
[[nodiscard]] base::Result<std::vector<std::pair<std::string, double>>> residuals(const base::Value& sk,
                                                                                  const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> analyze(const base::Value& sk, std::optional<double> tol_scale,
                                                const i18n::Catalog& words);
/// solveSketch(sk, {pin, pinRadius}): solves in place → {ok, dragged, status, dof, conflicts, redundant, determined,
/// iterations}.
[[nodiscard]] base::Result<base::Value> solve_sketch(
    base::Value& sk, const std::vector<std::pair<std::string, std::array<double, 2>>>& pin,
    const std::vector<std::pair<std::string, double>>& pin_radius, const i18n::Catalog& words);

}  // namespace cc::partcraft::sketch
