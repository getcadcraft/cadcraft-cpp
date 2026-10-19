// SPDX-License-Identifier: Apache-2.0
// cc::cmd — a command script run headless on a fresh drawing:
//   { name, steps: [ "LINE", "0,0", { click: [x, y], pick?: id }, { cursor: [x, y] }, { esc: true },
//                    { window: [minX, minY, maxX, maxY], crossing?, ids: [...] }, { select: [ids] }, { shift: bool },
//                    { layer: name, patch?: {…}, current?: bool } (what the Layers panel does) ] }
// → { name, log: [[print|prompt, text]…], model: [[id, entity]…], blocks, layers: [[name, layer]…], meta, ucs,
//     ucsNamed, views, current, nextId, undo: [labels], redo: [labels] }
// (tools/parity/commands.mjs runs the same scripts through CadCraft 0.5's Engine and compares).
#pragma once

#include <cstddef>

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::cmd {

struct ScriptLimits {
  std::size_t max_steps = 100000;
};

[[nodiscard]] base::Value run_script(const base::Value& script, const i18n::Catalog& words,
                                     const ScriptLimits& limits = {});

}  // namespace cc::cmd
