// SPDX-License-Identifier: Apache-2.0
// cc::doc::params_doc — params-doc.js (CadCraft 0.5): every parameter and constraint edit as one undo step. Each works
// on a draft (copies of the constraints and the parameters), resolves it forward (params::apply_params), solves the
// touched geometry (the first thing a dimension measures from held when it can be) and only then writes it all in one
// Doc::change: a solve that fails changes nothing and says why. Targets: each dimensional constraint, "c:<id>".
#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/params.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::doc::params_doc {

struct Result {
  bool ok = false;
  std::string error;
  std::string message;                 // e.g. "width is now 60"
  std::vector<std::string> choices;    // several driving parameters behind a value
  std::vector<std::string> conflicts;  // constraint ids
  std::vector<std::string> redundant;
  std::optional<base::Value> c;  // add_constraint: the constraint as stored
  double removed = 0;            // remove_constraints
  bool used = false;             // remove_parameter: refused because something uses it
};

struct CommitOpts {
  std::optional<std::set<double>> touch;
  std::vector<std::string> added;
  std::optional<std::set<std::string>> pinned;
};
/// commit(doc, label, constraints, params, { touch, added, pinned })
[[nodiscard]] Result commit(Doc& doc, const std::string& label, const base::Array& constraints,
                            const cad::params::Params& params, const CommitOpts& o, const i18n::Catalog& words);

struct DimInput {
  std::optional<std::string> name;
  std::string expr;
};
/// parseDimInput("d2=width/2") → { name: d2, expr: width/2 }; "40" → { expr: 40 }
[[nodiscard]] DimInput parse_dim_input(std::string_view text);

struct AddOpts {
  std::optional<std::pair<double, double>> loc;
  std::optional<std::string> expr, name;
  bool pin_first = true;
  std::optional<double> pin_entity;
};
[[nodiscard]] Result add_constraint(Doc& doc, std::string_view type, const base::Value& refs, const AddOpts& o,
                                    const i18n::Catalog& words);
[[nodiscard]] Result set_dim_input(Doc& doc, std::string_view cid, std::string_view text, const i18n::Catalog& words);
[[nodiscard]] Result drive_constraint(Doc& doc, std::string_view cid, double value,
                                      const std::optional<std::string>& prefer, const i18n::Catalog& words);
[[nodiscard]] Result set_parameter(Doc& doc, std::string_view name, std::string_view expr,
                                   const std::optional<std::string>& note, const std::optional<std::string>& rename,
                                   const i18n::Catalog& words);
[[nodiscard]] Result drive_parameter(Doc& doc, std::string_view name, double value,
                                     const std::optional<std::string>& prefer, const i18n::Catalog& words);
[[nodiscard]] Result remove_parameter(Doc& doc, std::string_view name, bool inline_value, const i18n::Catalog& words);
[[nodiscard]] Result remove_constraints(Doc& doc, const std::vector<std::string>& ids, const std::string& label,
                                        const i18n::Catalog& words);
[[nodiscard]] Result solve_all(Doc& doc, const std::string& label, const i18n::Catalog& words);

struct Row {
  std::string name, expr, note;
  std::optional<double> value;
  std::optional<std::string> error;
  std::string kind;  // "dim" | "user"
  std::optional<base::Value> constraint;
  std::string type;
  bool driving = false;
  std::vector<std::string> used_by;
  std::optional<std::string> label, shown;
};
struct Described {
  std::vector<Row> rows;
  std::string status;
};
/// describe(doc): rows for the Parameters panel and -PARAMETERS
[[nodiscard]] Described describe(const Doc& doc, const i18n::Catalog& words);

}  // namespace cc::doc::params_doc
