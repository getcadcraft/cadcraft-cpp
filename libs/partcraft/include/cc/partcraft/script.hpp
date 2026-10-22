// SPDX-License-Identifier: Apache-2.0
// cc::partcraft — the call scripts the PartCraft parity harness runs (tools/parity/part.mjs): {calls: [{op, args}]}
// → one result per call, {ok: value} or {err: message}, the same op names and argument shapes as the JS driver
// (tools/parity/part/driver.mjs), so `cadcraft-cli part <script.json>` answers what the frozen 0.5 modules answer.
// An op this build does not have answers {err: "unknown op <name>"}, as the driver does.
#pragma once

#include "cc/base/value.hpp"

namespace cc::partcraft {

/// Runs every call of `script` in order; never throws (a call that throws gives {err: what()}).
[[nodiscard]] base::Value run_script(const base::Value& script);

}  // namespace cc::partcraft
