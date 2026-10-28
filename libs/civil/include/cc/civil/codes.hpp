// SPDX-License-Identifier: Apache-2.0
// CivilCraft 0.5's desckeys.js (description keys and line codes), clip.js (copying and pasting survey points) and
// point-edit.js (checking one point's edit). Inputs are untrusted JSON (the project file, the clipboard).
#pragma once

#include <string>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::civil {

// ---- desckeys.js
[[nodiscard]] base::Value default_line_codes();
[[nodiscard]] base::Value clean_desc_keys(const base::Value& list);
[[nodiscard]] base::Value clean_line_codes(const base::Value& o);
/// parseDesc(desc, lineCodes) → { code, ctrl, rest }; lc undefined → the defaults
[[nodiscard]] base::Value parse_desc(const base::Value& desc, const base::Value& lc);
/// The group of each point (grouper(keys)), and the index of the key a code matches (compileKeys), or −1.
[[nodiscard]] std::string group_of(const base::Value& keys, const base::Value& point);
[[nodiscard]] long key_index(const base::Value& keys, std::string_view code);
/// figures(points, { keys, lineCodes, only }); only = nullptr: every group
[[nodiscard]] base::Value figures(const base::Value& points, const base::Value& keys, const base::Value& line_codes,
                                  const std::vector<std::string>* only);
[[nodiscard]] base::Value key_stats(const base::Value& points, const base::Value& keys);

// ---- clip.js
[[nodiscard]] std::string points_text(const base::Value& points, std::string_view order);
/// makeClip(points, { units, order, source }) with `at` given (the JS's Date.now())
[[nodiscard]] base::Value make_clip(const base::Value& points, const base::Value& opts, double at);
[[nodiscard]] base::Value read_clip(const base::Value& obj);
[[nodiscard]] base::Value text_points(const base::Value& text, std::string_view order, const i18n::Catalog& words);
[[nodiscard]] base::Value choose_paste(const base::Value& own, const base::Value& system_text, std::string_view order,
                                       const i18n::Catalog& words);
[[nodiscard]] base::Value renumber(const base::Value& incoming, const base::Value& existing, const base::Value& opts);
[[nodiscard]] std::string renumber_text(const base::Value& changed, const base::Value& kept,
                                        const i18n::Catalog& words);
[[nodiscard]] base::Value move_by(const base::Value& points, const base::Value& dx, const base::Value& dy);
[[nodiscard]] base::Value centre(const base::Value& points);

// ---- point-edit.js
/// checkPointEdit(points, n, patch)
[[nodiscard]] base::Value check_point_edit(const base::Value& points, const base::Value& n, const base::Value& patch,
                                           const i18n::Catalog& words);

}  // namespace cc::civil
