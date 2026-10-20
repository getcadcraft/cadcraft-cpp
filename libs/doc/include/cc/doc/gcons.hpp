// SPDX-License-Identifier: Apache-2.0
// cc::doc::gcons — gcons.js (CadCraft 0.5): 2D constraints on a drawing. They live in the undoable document beside
// the parameter table: doc.constraints = [{ id: "k3", type, refs: [{ e: id, k }], …type fields }], doc.params =
// { vars, links } (cad::params; a dimensional constraint is a parameter of its own name, linked as "c:<id>").
// Both are replaced, never edited in place (Doc::set_parametric inside a change: one undo step with the geometry).
//
// Geometric: coincident (PP PL PR), collinear (LL), concentric (RR), fix (P, E), parallel, perpendicular (LL),
// horizontal, vertical (L, PP), tangent (LR RR), symmetric (PPL LLL RRL), equal (LL RR). Dimensional: hdist, vdist
// (PP), dist (PP PL LL), radius, diameter (R), angle (LL R) — each with name, value, off.
//
// Moving constrained geometry goes through Doc::change, which calls after_edit(): what moved is pinned, the rest
// follows; failing that the pins come off; failing that the edit is refused (rolled back) and Doc::note says why.
#pragma once

#include <array>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/params.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::doc::gcons {

struct TypeInfo {
  std::string_view type;
  std::string_view label_key;
  std::array<std::string_view, 3> sigs;  // the reference kinds it takes ("PPL" …); unused slots empty
  std::string_view glyph;                // geometric
  std::string_view prefix;               // dimensional: d, rad, dia, ang
  std::string_view kind;                 // dimensional: length | angle
  bool dimensional = false;
};
/// TYPES (GEOMETRIC then DIMENSIONAL, in the JS's order); nullptr for an unknown type
[[nodiscard]] const TypeInfo* type_info(std::string_view type);
[[nodiscard]] std::span<const TypeInfo> types();
/// T.sigs.includes(sig)
[[nodiscard]] bool has_sig(const TypeInfo& t, std::string_view sig);
[[nodiscard]] bool is_dim(const base::Value& c);
[[nodiscard]] std::string target_key(const base::Value& c);  // "c:<id>"
inline constexpr std::size_t kMaxConstraints = 20000;
inline constexpr std::size_t kMaxPayload = 8'000'000;
inline constexpr std::string_view kParamBlock = "CADCRAFT_PARAMETRIC";

// ---- references
struct PointRef {
  std::string k;
  double x = 0, y = 0, d = 0;
};
/// constraintPoints(e): where a constraint can grab an object
[[nodiscard]] std::vector<PointRef> constraint_points(const base::Value& e);
/// nearestPointRef(e, p)
[[nodiscard]] std::optional<PointRef> nearest_point_ref(const base::Value& e, double px, double py);
/// curveRef(e, p): "L", "R" or the polyline segment "s<i>" nearest p
[[nodiscard]] std::optional<std::string> curve_ref(const base::Value& e, double px, double py);
/// signature(doc, refs): "PPL" …, or nothing when one does not resolve
[[nodiscard]] std::optional<std::string> signature(const Doc& doc, const base::Value& refs);
[[nodiscard]] std::string describe_ref(const Doc& doc, const base::Value& r, const i18n::Catalog& words);
[[nodiscard]] std::string describe_constraint(const Doc& doc, const base::Value& c, const i18n::Catalog& words);

// ---- making
[[nodiscard]] std::string next_id(const base::Value& constraints);
[[nodiscard]] std::string next_name(const cad::params::Params& p, std::string_view prefix,
                                    const std::set<std::string>& taken = {});
struct MakeOpts {
  std::optional<std::string> id, name;
  std::optional<std::pair<double, double>> loc;
  std::optional<double> value;
  bool model_space = false;  // opts.space === 'model'
};
/// makeConstraint(doc, type, refs, opts) → the constraint, or the reason
[[nodiscard]] std::pair<std::optional<base::Value>, std::string> make_constraint(const Doc& doc, std::string_view type,
                                                                                 const base::Value& refs,
                                                                                 const MakeOpts& o,
                                                                                 const i18n::Catalog& words);
/// measure(doc, c): what a dimensional constraint measures now (degrees for angles)
[[nodiscard]] double measure(const Doc& doc, const base::Value& c);
[[nodiscard]] std::string label_of(const base::Value& c, const cad::params::Params* p, const i18n::Catalog& words);
[[nodiscard]] std::string fmt_value(const base::Value& c, double v);
/// dimEntityFor(doc, c, st): a dimension entity drawing a dimensional constraint where it is now, or nothing
[[nodiscard]] std::optional<base::Value> dim_entity_for(const Doc& doc, const base::Value& c, const base::Value* st,
                                                        const i18n::Catalog& words);

// ---- solving
struct Solved {
  bool ok = false;
  std::vector<std::pair<double, base::Value>> updates;
  std::vector<std::string> conflicts;
  bool freed = false;
};
/// solveConstraints(doc, constraints, { pinned, only, strict, get })
[[nodiscard]] Solved solve_constraints(const Doc& doc, const base::Array& constraints,
                                       const std::set<std::string>* pinned, const std::set<double>* only, bool strict,
                                       const std::vector<std::pair<double, base::Value>>* overlay = nullptr);
struct Status {
  double count = 0, dof = 0, objects = 0;
  std::vector<std::string> conflicts, redundant;
  std::vector<double> determined;
};
[[nodiscard]] Status status(const Doc& doc);
[[nodiscard]] std::string status_text(const Doc& doc, const i18n::Catalog& words);
/// dropDimParams(params, dropped): the parameters of dimensional constraints going away, written into their users
[[nodiscard]] cad::params::Params drop_dim_params(const cad::params::Params& p, const base::Array& dropped,
                                                  const i18n::Catalog& words);
[[nodiscard]] bool fits(const Doc& doc, const base::Value& c);
/// afterEdit(doc, ops): nothing, or the reason the change is refused (Doc::change rolls it back)
[[nodiscard]] std::optional<std::string> after_edit(Doc& doc, const std::vector<Op>& ops, const i18n::Catalog& words);

// ---- the clipboard (clipboard.js) and ckeep.js
/// clipConstraints(doc, index): the constraints among copied objects, refs as indexes into the copy (index: id → i)
[[nodiscard]] base::Array clip_constraints(const Doc& doc, const std::vector<std::pair<double, double>>& index);
/// pasteConstraints(doc, list, ids, dx, dy): constraints for the new entities (ids[i] = clip entity i's new id), moved;
/// dimensional ones get fresh names. Inside the paste's change. → how many were added
double paste_constraints(Doc& doc, const base::Array& list, const std::vector<std::optional<double>>& ids, double dx,
                         double dy, const i18n::Catalog& words);

// ---- files
struct ForFile {
  std::vector<std::pair<double, std::string>> refs;  // entity id → key ("1", "2" …)
  std::vector<std::string> points;                   // "CCPARAM:<n>:<text>" strings, one per point
  std::string text;
};
/// forFile(doc): the parametric data for a file, or nothing
[[nodiscard]] std::optional<ForFile> for_file(const Doc& doc);
/// refKeyIn(strings): the CCREF key of an entity's CADCRAFT strings
[[nodiscard]] std::optional<std::string> ref_key_in(const base::Value& strings);
struct Resolved {
  base::Value constraints = base::Value::array();
  cad::params::Params params;
  std::vector<std::string> notes;
};
/// resolveParametric(raw, refMap): untrusted parametric data + the file's entities (key → { id, entity })
[[nodiscard]] Resolved resolve_parametric(
    const base::Value& raw, const std::vector<std::pair<std::string, std::pair<double, base::Value>>>& refs,
    const i18n::Catalog& words);

}  // namespace cc::doc::gcons
