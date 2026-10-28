// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::part — PartCraft 0.5's part.js: the part document, an ordered feature history saved as JSON
// (<name>.partcraft.json, format 'partcraft', versions 1–3). Geometry is never stored: the kernel rebuilds it from the
// history. The part is the JS-shaped value it saves as (base::Value, the JS's key order), edited in place:
//
//   {format, version, name, units:'mm', resolution, seq, rollback, features:[{id, type, name, suppressed?, …}],
//    drawing?, material?, studies?, params?, configs?, bodyInfo?}
//
// readPart checks an untrusted file feature by feature (sketch.js, gdt.js, design-features.js, bodies.js, surface.js,
// sheetmetal.js, rib.js, threads.js, params-part.js, configs.js and bom.js readers), the JS's words on refusal.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::part {

inline constexpr std::string_view kFormat = "partcraft";
inline constexpr double kVersion = 3;

[[nodiscard]] base::Value new_part(const base::Value& name);
/// featureById: a pointer into the part (nullptr when absent).
[[nodiscard]] base::Value* feature_by_id(base::Value& part, const base::Value& id);
/// builtCount: how many features are built (the rollback bar sits after this many).
[[nodiscard]] double built_count(const base::Value& part);
/// nameFeatures(name): the feature ids a face name depends on.
[[nodiscard]] std::vector<std::string> name_features(const base::Value& name);
/// addFeature(part, type, props): the new feature, inserted at the rollback bar.
[[nodiscard]] base::Result<base::Value> add_feature(base::Value& part, const base::Value& type,
                                                    const base::Value& props);
/// dependsOn(f): the feature ids this feature needs.
[[nodiscard]] base::Result<std::vector<base::Value>> depends_on(const base::Value& f);
[[nodiscard]] base::Result<base::Value> dependents(const base::Value& part, const base::Value& id);
/// canMove(part, id, to) → {ok, reason?}
[[nodiscard]] base::Result<base::Value> can_move(const base::Value& part, const base::Value& id, double to,
                                                 const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> move_feature(base::Value& part, const base::Value& id, double to,
                                                     const i18n::Catalog& words);
/// deleteFeatures(part, ids): removes them and everything that depends on them → the names removed.
[[nodiscard]] base::Result<base::Value> delete_features(base::Value& part, const base::Value& ids);
/// fileVersion(part): 3 when a version-2 reader would drop or refuse something, else 2.
[[nodiscard]] double file_version(const base::Value& part);
/// toJSON(part): the file's text (JSON.stringify(…, null, 1)).
[[nodiscard]] std::string to_json(const base::Value& part);
/// readPart(raw): a clean part from an untrusted file (its value or its text), or the reason.
[[nodiscard]] base::Result<base::Value> read_part(const base::Value& raw, const i18n::Catalog& words);
/// readDrawing(d): the drawing settings (section, detail, notes, sheet, title, holeTable, revisions).
[[nodiscard]] base::Result<base::Value> read_drawing(const base::Value& d, const i18n::Catalog& words);

}  // namespace cc::partcraft::part
