// SPDX-License-Identifier: Apache-2.0
// cc::cmd — the Properties panel's model (CadCraft 0.5 propdefs.js and propexpr.js): what each kind of object shows,
// the value a group of objects shares, a typed value read and set on every object of the group in one undo step. The
// viewer draws the panel from it; scripts and tests drive it the same way.
//
//   const auto groups = props::groups_of(ents);              // "All (12)", "Line (8)", …
//   for (const props::Def& d : props::defs_for(key, ents, ctx)) props::value_of(d, ents, ctx) …
//   auto v = read_value("12'6\"", "len", read_ctx, words);    // v.ok, v.value | v.error
//   props::apply_value(doc, def, ents, v.value, ctx);         // one undo step ("PROPERTIES")
//
// Not ported yet (docs/gaps.md): dynamic-block parameters (Custom), sections added by add-ons.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cmd {

// ---- propexpr.js

/// What a typed value is read against (props.js readCtx).
struct ReadCtx {
  base::Value lunits = 2;  // the drawing's length format code (doc.meta.lunits)
  std::string units;       // doc.meta.units
  std::string paper;       // 'mm' | 'in' on a sheet (not for model values), else empty
  bool gt0 = false;
  std::optional<double> min, max;
  std::map<std::string, double> vars;  // names an expression may use
};
struct ReadResult {
  bool ok = false;
  double value = 0;
  std::string error;  // already in the reader's language
};
/// propexpr.js readValue(raw, kind, ctx): kind 'len' (drawing units), 'ang' (degrees), 'num', 'int'.
[[nodiscard]] ReadResult read_value(std::string_view raw, std::string_view kind, const ReadCtx& ctx,
                                    const i18n::Catalog& words);
/// propexpr.js editText(v): the full-precision text a field shows while it is edited.
[[nodiscard]] std::string edit_text(double v);
/// propexpr.js mmPerUnit(ctx): millimetres in one drawing unit (or the sheet's paper unit), or nothing.
[[nodiscard]] std::optional<double> mm_per_unit(const ReadCtx& ctx);

// ---- linetypes.js

/// LINEWEIGHTS: the lineweights a person can choose, in mm.
inline constexpr std::array<double, 24> kLineweights{0,   0.05, 0.09, 0.13, 0.15, 0.18, 0.2, 0.25,
                                                     0.3, 0.35, 0.4,  0.5,  0.53, 0.6,  0.7, 0.8,
                                                     0.9, 1,    1.06, 1.2,  1.4,  1.58, 2,   2.11};
/// linetypeNames(doc): Continuous, the drawing's own, the standard ones.
[[nodiscard]] std::vector<std::string> linetype_names(const doc::Doc& d);
/// ltLabel(name) / lwLabel(mm): what a linetype or a lineweight is called.
[[nodiscard]] std::string lt_label(const i18n::Catalog& words, const base::Value& n);
[[nodiscard]] std::string lw_label(const i18n::Catalog& words, const base::Value& w);

namespace props {

/// propdefs.js SECTIONS, in panel order.
inline constexpr std::array<std::string_view, 11> kSections{"general", "geometry", "custom", "text",    "lines", "fit",
                                                            "units",   "alt",      "tol",    "pattern", "misc"};

/// What the panel and the descriptors are given (propdefs.js ctx).
struct Ctx {
  const doc::Doc* doc = nullptr;
  const i18n::Catalog* words = nullptr;
  std::vector<base::Value> ents;  // the group's objects
  int vertex = 0;                 // the polyline vertex the panel shows
};

/// A descriptor: `get` gives nothing for "varies"; `set` gives the changed entity, or an error (already translated).
struct SetResult {
  base::Value e;
  std::string error;
};
struct Def {
  std::string key, sec, kind, label, title;
  std::function<std::optional<base::Value>(const base::Value& e, const Ctx& c)> get;
  std::function<SetResult(const base::Value& e, const base::Value& v, const Ctx& c)> set;  // empty: read-only
  std::function<bool(const base::Value& e, const Ctx& c)> when;                            // empty: always
  bool one = false;                                                                        // only for a single object
  std::string pt_name, pt_axis;  // one axis of a point (a pick button in the panel)
  bool gt0 = false;
  std::optional<double> min, max;
  std::function<std::vector<std::pair<base::Value, std::string>>(const Ctx& c)> choices;
  std::vector<std::string> types;     // general fields only for these types (empty: every type)
  bool paper = false, model = false;  // viewport: a sheet length / a model length
  bool none = false;                  // a colour that may be "none"
};

struct Group {
  std::string key;  // 'all' or the type
  std::size_t n = 0;
  std::vector<doc::Id> ids;
  std::string label;
};
/// propdefs.js groupsOf(ents): "All" first when there is more than one type, then each type.
[[nodiscard]] std::vector<Group> groups_of(const std::vector<base::Value>& ents, const i18n::Catalog& words);
/// propdefs.js defsFor(key, ents, ctx): the descriptors that group shows, in section order.
[[nodiscard]] std::vector<Def> defs_for(const std::string& key, const Ctx& ctx);
/// propdefs.js valueOf(def, ents, ctx): the value every object shares, or nothing ("varies").
[[nodiscard]] std::optional<base::Value> value_of(const Def& def, const Ctx& ctx);

struct Applied {
  bool ok = false;
  std::size_t changed = 0, locked = 0;
  std::string error;
};
/// propdefs.js applyValue(doc, def, ents, v, ctx): every object of the group in one undo step; objects on locked
/// layers stay as they are (counted); an error from any object refuses the whole change.
Applied apply_value(doc::Doc& doc, const Def& def, const base::Value& v, const Ctx& ctx);

/// propdefs.js measure(e): length, area, perimeter where they mean something.
struct Measure {
  std::optional<double> length, area, perimeter;
};
[[nodiscard]] Measure measure(const base::Value& e);

}  // namespace props

}  // namespace cc::cmd
