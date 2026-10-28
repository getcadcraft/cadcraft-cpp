// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::configs — PartCraft 0.5's configs.js (P18): configurations, the design-table rows a part keeps
// (file version 3): {rows: [{name, values: {<parameter>: number}}], active: <row name> | null}. A row holds values for
// the part's driving parameters; applying it sets them (their expr becomes the number) and marks it active.
// JS-shaped values (base::Value) in and out, as the part file holds them; a JS throw is an Error here.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/params.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft::configs {

inline constexpr std::size_t kMaxRows = 500;
inline constexpr std::size_t kNameMax = 60;

/// cleanName: control characters out, trimmed, 60 UTF-16 units at most.
[[nodiscard]] std::string clean_name(const base::Value& s);
/// readConfigs(raw, params): untrusted file data → clean configs, or null when there are no rows.
[[nodiscard]] base::Result<base::Value> read_configs(const base::Value& raw, const base::Value& params,
                                                     const i18n::Catalog& words);
/// rowFromCurrent(params, name): {name, values} of every driving parameter's current value.
[[nodiscard]] base::Value row_from_current(const base::Value& params, const base::Value& name,
                                           const i18n::Catalog& words);
/// saveRow(cfg, row): add, or replace the row of that name (any case).
[[nodiscard]] base::Result<base::Value> save_row(const base::Value& cfg, const base::Value& row,
                                                 const i18n::Catalog& words);
/// applyRow(params, row): the parameters with the row's values (an error for a parameter the part lacks).
[[nodiscard]] base::Result<base::Value> apply_row(base::Value params, const base::Value& row,
                                                  const i18n::Catalog& words);
/// rowMatches(params, row): do the current values equal the row's?
[[nodiscard]] bool row_matches(const base::Value& params, const base::Value& row, const i18n::Catalog& words);
[[nodiscard]] base::Result<base::Value> rename_row(const base::Value& cfg, const base::Value& from,
                                                   const base::Value& to, const i18n::Catalog& words);
/// deleteRow: null when the last row goes.
[[nodiscard]] base::Value delete_row(const base::Value& cfg, const base::Value& name);
/// followParams(cfg, before, after): renamed parameters carry their values, deleted ones drop out.
[[nodiscard]] base::Value follow_params(const base::Value& cfg, const base::Value& before, const base::Value& after);
/// rowsFromTable(csv, params): a CSV design table (params.js readDesignTable) → rows.
[[nodiscard]] base::Result<base::Value> rows_from_table(std::string_view csv, const base::Value& params,
                                                        const i18n::Catalog& words);
/// configsCsv(cfg): the rows as CSV (CRLF; a cell a spreadsheet would run as a formula gets a leading ').
[[nodiscard]] std::string configs_csv(const base::Value& cfg);

}  // namespace cc::partcraft::configs
