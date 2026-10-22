// SPDX-License-Identifier: Apache-2.0
// cc::lisp — a LISP parity session (tests/lisp/*.json): what `cadcraft-cli lisp` runs and tools/parity/lisp.mjs runs
// through CadCraft 0.5's createLisp, so the two can be compared step by step.
//
//   {"name": "…", "steps": [{"eval": "(+ 1 2)"}, {"put": {"name": "lib.lsp", "text": "…"}},
//                           {"load": {"name": "a.lsp", "text": "…"}}, {"call": "NAME"}]}
//
// eval = Interp::eval_text; put = a file in the session's memory; load = put + load_text (as session.js loads a file);
// call = a (defun c:NAME …) routine. Each step gives {ok, value (prin1 text) | error, cancel, out: [[text, piece]…]};
// the session ends with the routines' commands and the names (defun c:…) reported, in order. Each session starts
// with fresh process counters (and, for replaying a fuzz input, may set "max_steps" / "max_string" as fuzz_lisp does)
// (selection set numbers, temp file names), as the runner gives each fresh JS modules.
#pragma once

#include "cc/base/value.hpp"

namespace cc::i18n {
class Catalog;
}

namespace cc::lisp {

[[nodiscard]] base::Value run_session(const base::Value& session, const i18n::Catalog* words);

}  // namespace cc::lisp
