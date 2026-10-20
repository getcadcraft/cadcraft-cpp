// SPDX-License-Identifier: Apache-2.0
// cc::doc::ckeep — ckeep.js (CadCraft 0.5): constraints that come through MIRROR and TRIM. Both run inside the
// command's Doc::change, so the geometry and its constraints are one undo step.
//
//   mirror_constraints(doc, pairs)   pairs (source id, copy id): the constraints among the sources, copied onto the
//                                    copies, signs and sides worked out again on the mirrored geometry
//   resign_among(doc, ids)           after an in-place change (MIRROR erasing its source): signs and sides again
//   trim_constraints(doc, old, pieces)  each constraint on the object moves to the piece that still has what it holds;
//                                    ones that held the part that went, or a length the cut changed, are dropped
#pragma once

#include <set>
#include <utility>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::doc::ckeep {

[[nodiscard]] double mirror_constraints(Doc& doc, const std::vector<std::pair<double, double>>& pairs,
                                        const i18n::Catalog& words);
[[nodiscard]] double resign_among(Doc& doc, const std::set<double>& ids, const i18n::Catalog& words);

struct Piece {
  double id = 0;
  double t0 = 0, t1 = 1;  // where it lies along the original (line / arc 0 … 1)
};
struct Kept {
  double kept = 0, dropped = 0;
};
[[nodiscard]] Kept trim_constraints(Doc& doc, const base::Value& old_e, const std::vector<Piece>& pieces,
                                    const i18n::Catalog& words);

}  // namespace cc::doc::ckeep
