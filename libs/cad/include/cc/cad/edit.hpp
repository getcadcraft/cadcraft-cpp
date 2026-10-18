// SPDX-License-Identifier: Apache-2.0
// cc::cad — the editing geometry of CadCraft 0.5's geom.js that the commands use: primitives and their intersections,
// OFFSET (offsetEntity), TRIM in pieces (trimSplit → pieceAt / pieceEntity / trimResult, pathCrossings), EXTEND
// (extendEntity, extensionPart), the window test (inWindow). Ported with the JS's arithmetic and tolerances.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/geom.hpp"

namespace cc::cad {

/// geom.js's primitive: a segment (L: x1, y1 → x2, y2) or a counter-clockwise arc (A: cx, cy, r, a0 → a1, a1 > a0).
/// `rev`: an arc that runs clockwise along its polyline (a negative bulge); `full`: a whole circle.
struct Prim {
  bool arc = false;
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  double cx = 0, cy = 0, r = 0, a0 = 0, a1 = 0;
  bool full = false;
  bool rev = false;
  bool box = false;  // a text box, an arrowhead: for picking and bounds, not for snaps
};

/// geom.js prims(e, blocks): an entity's primitives in world coordinates.
[[nodiscard]] std::vector<Prim> entity_prims(const base::Value& e, const BlockLookup& blocks);
/// geom.js txPrim over a list: primitives through a matrix (an arc under a shear or stretch as short lines).
[[nodiscard]] std::vector<Prim> tx_prims(const std::vector<Prim>& ps, const Mat2& m);
/// geom.js insertMatrix(e, blk): a reference's block → world matrix.
[[nodiscard]] Mat2 insert_matrix(const base::Value& e, const base::Value& blk);
/// geom.js ellipsePoints(e, steps): points along an ellipse (or its arc).
[[nodiscard]] std::vector<P2> ellipse_points(const base::Value& e, double steps = 72);
/// geom.js primParam(p, q, clamp): the parameter (0 … 1) of the point on p nearest q.
[[nodiscard]] double prim_param(const Prim& p, P2 q, bool clamp = true);
/// geom.js distToPrim(p, q)
[[nodiscard]] double dist_to_prim(const Prim& p, P2 q);
/// geom.js intersect(p, q, ext1, ext2): the points on both (ext: as an unbounded line / whole circle).
[[nodiscard]] std::vector<P2> intersect(const Prim& p, const Prim& q, bool ext1 = false, bool ext2 = false);

/// geom.js offsetEntity(e, d, side): a parallel copy d away on the side of `side`, or nothing.
[[nodiscard]] std::optional<base::Value> offset_entity(const base::Value& e, double d, P2 side);

/// One piece of a trimSplit, in its parameter (poly: segment index + local t; circle: fraction of a turn).
struct TrimPiece {
  double t0 = 0, t1 = 0;
  bool whole = false;
};
struct TrimSplit {
  base::Value e;
  std::vector<Prim> own;
  std::vector<TrimPiece> pieces;
  double max_t = 1;
  bool closed = false;
};
/// geom.js trimSplit(e, cutters, blocks, { edgeExtend }): e split where the cutting edges cross it, or nothing for
/// objects TRIM does not cut.
[[nodiscard]] std::optional<TrimSplit> trim_split(const base::Value& e, const std::vector<const base::Value*>& cutters,
                                                  const BlockLookup& blocks, bool edge_extend = false);
/// geom.js pieceAt(split, q): the piece holding the point nearest q.
[[nodiscard]] std::size_t piece_at(const TrimSplit& split, P2 q);
/// geom.js pieceEntity(split, i): one piece as an object of its own (null when there is none).
[[nodiscard]] base::Value piece_entity(const TrimSplit& split, std::size_t i);
/// geom.js trimResult(split, gone): what stays, the kept runs joined (empty: nothing left); nothing when none goes.
[[nodiscard]] std::optional<base::Array> trim_result(const TrimSplit& split, const std::set<std::size_t>& gone);
/// geom.js subEntity(e, t0, t1): the piece of a line / arc / open polyline / construction line between t0 < t1.
[[nodiscard]] base::Value sub_entity(const base::Value& e, double t0, double t1);
/// geom.js pathCrossings(e, pts, blocks): where an open run of points crosses e.
[[nodiscard]] std::vector<P2> path_crossings(const base::Value& e, const std::vector<P2>& pts,
                                             const BlockLookup& blocks);

/// geom.js extendEntity(e, boundaries, pick, blocks, { edgeExtend }): the end nearest `pick` taken to the nearest
/// boundary, or nothing.
[[nodiscard]] std::optional<base::Value> extend_entity(const base::Value& e,
                                                       const std::vector<const base::Value*>& boundaries, P2 pick,
                                                       const BlockLookup& blocks, bool edge_extend = false);
/// geom.js extensionPart(old, nu): what an extend adds (for the preview), or null.
[[nodiscard]] base::Value extension_part(const base::Value& old, const base::Value& nu);

struct Rect2 {
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};
/// geom.js inWindow(e, w, crossing, blocks): inside (window) or inside or touching (crossing).
[[nodiscard]] bool in_window(const base::Value& e, const Rect2& w, bool crossing, const BlockLookup& blocks);

// ---- reshape.cpp: BREAK, LENGTHEN, JOIN, STRETCH, EXPLODE

/// geom.js paramOn(e, q): where q is along e (line / arc 0 … 1, polyline segment index + 0 … 1).
[[nodiscard]] double param_on(const base::Value& e, P2 q);
/// geom.js breakEntity(e, p1, p2): the pieces left, or error "type" / "circle".
struct BreakResult {
  std::string error;
  base::Array pieces;
};
[[nodiscard]] BreakResult break_entity(const base::Value& e, P2 p1, P2 p2);
/// geom.js curveLength(e): a line's, arc's or polyline's length (nothing for others).
[[nodiscard]] std::optional<double> curve_length(const base::Value& e);
/// geom.js lengthenEntity(e, pick, { delta, dAngle }): e with the end nearest pick moved, or error "type" / "short" /
/// "full".
struct LengthenResult {
  std::string error;
  base::Value e;
};
[[nodiscard]] LengthenResult lengthen_entity(const base::Value& e, P2 pick, double delta,
                                             std::optional<double> d_angle = std::nullopt);
/// geom.js joinEntities(ents, tol): { results: [{ from: ids, to }], left: ids }.
struct JoinPart {
  base::Array from;
  base::Value to;
};
struct JoinResult {
  std::vector<JoinPart> results;
  base::Array left;
};
[[nodiscard]] JoinResult join_entities(const std::vector<base::Value>& ents, std::optional<double> tol = std::nullopt);
/// geom.js stretchEntity(e, inside, dx, dy): what is in the window moves; nothing when nothing of e moves.
[[nodiscard]] std::optional<base::Value> stretch_entity(const base::Value& e, const std::function<bool(P2)>& inside,
                                                        double dx, double dy);
/// geom.js explodeEntity(e, blocks): a polyline's lines and arcs, a leader's parts, a reference's or dimension's
/// objects; nothing for other objects (or a reference to a missing block).
[[nodiscard]] std::optional<base::Array> explode_entity(const base::Value& e, const BlockLookup& blocks);

// ---- corners.cpp: FILLET and CHAMFER

/// geom.js cornerTarget(e, pick): what a pick on e is for a corner (seg: the polyline segment nearest the pick).
struct CornerTarget {
  base::Value e;
  std::optional<std::size_t> seg;
  Prim prim;
  P2 pick;
};
[[nodiscard]] std::optional<CornerTarget> corner_target(const base::Value& e, P2 pick);
/// A corner: { ok, edits (entities with their ids), add (new entities) } or an error code (type, same, parallel,
/// collinear, large, none, middle, angle); semicircle: FILLET joined two parallel lines.
struct CornerResult {
  bool ok = false;
  std::string error;
  base::Array edits;
  base::Array add;
  bool semicircle = false;
};
/// geom.js filletPair(A, B, r, { trim, current })
[[nodiscard]] CornerResult fillet_pair(const CornerTarget& a, const CornerTarget& b, double r, bool trim,
                                       const base::Value& current);
/// A chamfer's settings: method D (d1 from the first piece, d2 from the second) or A (len along the first at ang).
struct ChamferSpec {
  std::string method = "D";
  double d1 = 0;
  std::optional<double> d2;
  double len = 0;
  double ang = 0;
};
/// geom.js chamferPair(A, B, spec, { trim, current })
[[nodiscard]] CornerResult chamfer_pair(const CornerTarget& a, const CornerTarget& b, const ChamferSpec& spec,
                                        bool trim, const base::Value& current);
/// geom.js polyCorners(e, kind, spec): every corner of a polyline filleted (radius) or chamfered (spec).
struct PolyCornersResult {
  base::Value e;
  std::size_t done = 0, short_ = 0, arcs = 0, removed = 0;
};
[[nodiscard]] PolyCornersResult poly_corners(const base::Value& e, bool fillet, double radius, const ChamferSpec& spec);

}  // namespace cc::cad
