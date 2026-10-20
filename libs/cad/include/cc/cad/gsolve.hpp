// SPDX-License-Identifier: Apache-2.0
// cc::cad::gsolve — gsolve.js (CadCraft 0.5): the 2D constraint solver for CadCraft's own entities, which keep their
// coordinates in place (a line is x1 y1 x2 y2). Variables are every field of every entity a constraint touches (line
// x1 y1 x2 y2 · circle cx cy r · arc cx cy r a0 a1 · point x y · polyline vertex "<i>x" "<i>y"); a pinned field
// ("<id>|<field>") is held. Damped Newton with the least-norm step dx = Jᵀ (J Jᵀ + μI)⁻¹ (−r), a halving line search
// and Levenberg–Marquardt growth of μ; Jacobian rows by central differences over each equation's own variables.
// analyze(): Gram–Schmidt over the rows in the order the constraints were made → rank, redundant constraints,
// degrees of freedom, the entities fully determined.
//
// References: { e: entity id, k } with k = line p1 p2 mid L · circle c R · arc c s e mid R · point p ·
// poly v<i> (vertex) s<i> (segment) · * (the whole entity). Every order (variables, equations, sums) is the JS's,
// so results match it to the last bit.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::cad::gsolve {

/// The entity with this id (nullptr: none).
using Getter = std::function<const base::Value*(double id)>;

inline constexpr std::array<std::string_view, 5> kSolvable{"line", "circle", "arc", "point", "poly"};
[[nodiscard]] bool solvable(const base::Value& e);

/// fieldsOf(e)
[[nodiscard]] std::vector<std::string> fields_of(const base::Value& e);
/// getField(e, f) (NaN when absent, as `undefined` becomes in arithmetic)
[[nodiscard]] double get_field(const base::Value& e, std::string_view f);
/// withFields(e, vals): a copy with new field values; radii stay positive, arc angles in [0, 2π).
[[nodiscard]] base::Value with_fields(const base::Value& e, const std::vector<std::pair<std::string, double>>& vals);
/// segCount(e): a polyline's segments (a closed one wraps round)
[[nodiscard]] double seg_count(const base::Value& e);
/// refKind(e, k) → 'P' | 'L' | 'R' | 'E', or nothing
[[nodiscard]] std::optional<char> ref_kind(const base::Value* e, const base::Value& k);

struct RefPoint {
  double x = 0, y = 0;
};
struct RefLine {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};
struct RefCircle {
  double cx = 0, cy = 0, r = 0;
};
/// refPoint(e, k): where a reference is now (a line's middle, a circle's centre for L / R)
[[nodiscard]] std::optional<RefPoint> ref_point(const base::Value& e, const base::Value& k);
[[nodiscard]] RefLine ref_line(const base::Value& e, const base::Value& k);
[[nodiscard]] RefCircle ref_circle(const base::Value& e, const base::Value& k);

struct SolveResult {
  bool ok = false;
  std::vector<std::pair<double, base::Value>> updates;  // id → the entity moved (in the order met)
  double iterations = 0;
  double residual = 0;
  std::vector<std::string> worst;  // constraint ids still far from met
};
/// solveSystem(constraints, get, { pinned, maxIter }) — nothing is changed; moved entities come back as copies.
[[nodiscard]] SolveResult solve_system(const base::Array& constraints, const Getter& get,
                                       const std::set<std::string>* pinned = nullptr, int max_iter = 100);
/// residualOk(constraints, get): every equation met at the current geometry
[[nodiscard]] bool residual_ok(const base::Array& constraints, const Getter& get);

struct Analysis {
  double dof = 0, rank = 0, vars = 0;
  std::vector<std::string> redundant, conflicts;
  std::vector<double> determined;  // entity ids, in the order met
};
[[nodiscard]] Analysis analyze(const base::Array& constraints, const Getter& get);

/// components(constraints): groups that share entities, in the order of their first constraint
[[nodiscard]] std::vector<base::Array> components(const base::Array& constraints);

/// The id of a reference's entity (refs[i].e)
[[nodiscard]] double ref_id(const base::Value& ref);

}  // namespace cc::cad::gsolve
