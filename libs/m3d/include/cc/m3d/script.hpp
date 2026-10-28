// SPDX-License-Identifier: Apache-2.0
// Model scripts (tools/parity/m3d.mjs): a ModelCraft model edited by kernel calls, step by step, for parity with the
// 0.5 kernel and for the golden tests. A script is {name, units?, steps: [{op, …}]}; the ops and selectors are listed
// at the top of tools/parity/m3d.mjs.
#pragma once

#include <cstddef>

#include "cc/base/value.hpp"

namespace cc::m3d {

/// → {name, steps: [{op, r | err, model}], undo, redo}. `max_elements` (vertices + edges + faces + placements over
/// every definition) ends the script when an edit grows the model past it or leaves a point that is not a number (the
/// fuzz target's budget; 0: no limit).
[[nodiscard]] base::Value run_script(const base::Value& script, std::size_t max_elements = 0);

}  // namespace cc::m3d
