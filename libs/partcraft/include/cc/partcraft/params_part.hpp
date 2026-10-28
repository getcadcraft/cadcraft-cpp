// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::pp — PartCraft 0.5's params-part.js: the parametric engine's adapter for a part. Every number an
// equation may drive is a target ("<featureId>.<field>" from PARAM_FIELDS, "<sketchFeatureId>.<constraintId>" for a
// sketch dimension); after an apply, every sketch whose dimension changed is solved again, and one that cannot solve
// gets its old dimensions back. Also the rollback helpers the panel and the CLI share.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/cad/params.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::pp {

struct Target {
  std::string key, label, kind;
  std::optional<double> min, max;
  bool integer = false;
  std::string feature, field, constraint, dim;  // field: the member of *slot that holds the number
  base::Value* slot = nullptr;                  // the feature, or the sketch constraint
};
/// targets(part): every drivable number, in the JS's order (pointers into `part`).
[[nodiscard]] std::vector<Target> targets(base::Value& part, const i18n::Catalog& words);
/// What the parity driver shows of a target: {label, kind, min?, max?, integer, feature, field|constraint+dim, value}.
[[nodiscard]] base::Value target_value(const Target& t);
/// fieldInUse(f, field): does the field matter in the feature's current mode?
[[nodiscard]] bool field_in_use(const base::Value& f, const base::Value& field);
[[nodiscard]] std::pair<std::string, std::string> split_key(std::string_view key);

/// The engine's adapter over one part (it holds the part and a journal of the values an apply replaced).
class PartAdapter {
 public:
  PartAdapter(base::Value& part, const i18n::Catalog& words);
  [[nodiscard]] const cad::params::Adapter& adapter() const { return adapter_; }

 private:
  base::Value& part_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): an adapter lives for one apply
  const i18n::Catalog& words_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  std::vector<std::pair<std::string, double>> journal_;
  std::vector<Target> targets_;
  cad::params::Adapter adapter_;
  std::vector<std::pair<std::string, std::string>> after_apply(const std::vector<std::string>& changed);
};

/// resolveTarget(part, text): the key named the way people type it ("Extrude1.depth", "Sketch1.D3"), or nothing.
[[nodiscard]] std::optional<std::string> resolve_target(base::Value& part, const base::Value& text,
                                                        const i18n::Catalog& words);
/// targetName(part, key): "Extrude1.depth" style (the key itself when it is no target).
[[nodiscard]] std::string target_name(base::Value& part, std::string_view key, const i18n::Catalog& words);
/// readPartParams(raw, part): a file's params checked against the part's targets; null when empty.
[[nodiscard]] base::Result<base::Value> read_part_params(const base::Value& raw, base::Value& part,
                                                         const i18n::Catalog& words);
/// applyPartParams(part, {overrides}): {ok, values, errors:[{where, message}], changed}.
[[nodiscard]] base::Value apply_part_params(base::Value& part,
                                            const std::vector<std::pair<std::string, double>>* overrides,
                                            const i18n::Catalog& words);
/// pruneDeleted(before, after): the links whose targets are gone leave `after` → their labels from before.
[[nodiscard]] base::Value prune_deleted(base::Value& before, base::Value& after, const i18n::Catalog& words);
/// paramsReport(part): {vars:[{name, expr, note, value, error}], links:[{…describeLinks, name}]}.
[[nodiscard]] base::Value params_report(base::Value& part, const i18n::Catalog& words);
[[nodiscard]] base::Value step_rollback(base::Value& part, double delta, const i18n::Catalog& words);
[[nodiscard]] base::Value roll_to(base::Value& part, const base::Value& id, const i18n::Catalog& words);
[[nodiscard]] std::string rollback_words(const base::Value& part, const i18n::Catalog& words);

}  // namespace cc::partcraft::pp
