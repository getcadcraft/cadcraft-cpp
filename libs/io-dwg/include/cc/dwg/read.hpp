// SPDX-License-Identifier: Apache-2.0
// cc::dwg — L3. Our own DWG reader: the C++ port of cc-dwg's readDwg (lib/dwg in the CadCraft 0.5 JavaScript build),
// written from the ODA "Open Design Specification for .dwg files" v5.4.1 with the JS reader's spec-gap findings.
//
//   read_dwg(bytes) → the drawing model, field for field the JS readDwg model (docs/DWG-LIB-SPEC.md "Model"):
//     { version, maintenance, codepage, header, classes, tables, blocks, layouts, objects, report }
//   It never throws on a damaged object: the object is noted in report.notes and left out (or kept as UNKNOWN).
//   A file it cannot open at all gives a model with report.error set.
#pragma once

#include <cstddef>
#include <span>

#include "cc/base/value.hpp"

namespace cc::dwg {

struct ReadOptions {
  /// Largest file accepted (bytes); larger input gives report.error.
  std::size_t max_bytes = std::size_t{1} << 31U;
  /// Threads that decode the objects (they are independent; the model is the same whatever the count). 0 = the
  /// machine's cores, up to 8; 1 = this thread only (the sandboxed import helper may not start threads).
  unsigned threads = 0;
};

[[nodiscard]] base::Value read_dwg(std::span<const std::byte> input, const ReadOptions& opts = {});

}  // namespace cc::dwg
