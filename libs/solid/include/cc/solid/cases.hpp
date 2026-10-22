// SPDX-License-Identifier: Apache-2.0
// The parity cases of tools/parity/manifold.mjs, run through SolidOps: a case is a stack program of steps
//   {mesh: {vp, tv}, merge}                 a Mesh (3 floats per vertex), merged when asked, made a solid; a refused
//                                           mesh ends the case with {error: <status name>}
//   {cube: [x, y, z], center} · {cylinder: [h, r1, r2, segments, center]} · {sphere: [r, segments]}
//   {translate: [x, y, z]} · {transform: [16 numbers]}           on the top solid
//   {op: "add" | "subtract" | "intersect"}  the two top solids → one
//   {union: n} · {intersection: n}          the n top solids → one (BatchBoolean)
//   {trim: [nx, ny, nz, offset]} · {split: […]}  the top solid → its part (trim) or its two parts (split)
// and its result is {out: [{status, empty, volume (the double's bits, hex), numProp, vp (each float's bits, hex,
// joined), tv (indices joined by commas)}…]} — what the script prints for the wasm.
#pragma once

#include "cc/base/value.hpp"

namespace cc::solid {

/// {cases: [{steps…}…]} → {results: [result…]}; a malformed step gives {bad: "<why>"} for its case.
[[nodiscard]] base::Value run_cases(const base::Value& cases);
/// One case's result.
[[nodiscard]] base::Value run_case(const base::Value& steps);

}  // namespace cc::solid
