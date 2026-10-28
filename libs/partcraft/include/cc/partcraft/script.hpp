// SPDX-License-Identifier: Apache-2.0
// cc::partcraft — the call scripts the PartCraft parity harness runs (tools/parity/part.mjs): {calls: [{op, args}]}
// → one result per call, {ok: value} or {err: message}, the same op names and argument shapes as the JS driver
// (tools/parity/part/driver.mjs), so `cadcraft-cli part <script.json>` answers what the frozen 0.5 modules answer.
// An op this build does not have answers {err: "unknown op <name>"}, as the driver does.
#pragma once

#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::partcraft {

/// Runs every call of `script` in order; never throws (a call that throws gives {err: what()}).
/// `words`: PartCraft's catalog (assets/lang/partcraft/en.json for the parity runs).
[[nodiscard]] base::Value run_script(const base::Value& script, const i18n::Catalog& words);

}  // namespace cc::partcraft
