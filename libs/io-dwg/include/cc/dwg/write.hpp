// SPDX-License-Identifier: Apache-2.0
// cc::dwg — our own DWG writer: the C++ port of cc-dwg's writeDwg (lib/dwg/write.js and its modules in the CadCraft 0.5
// JavaScript build), written from the ODA "Open Design Specification for .dwg files" v5.4.1 with the JS writer's
// spec-gap findings. Byte for byte the JS writer's output (tools/parity/dwg-write.mjs).
//
//   write_dwg(model, { version }) → { bytes, report }
//     model: the version-neutral drawing model read_dwg makes (docs/DWG-LIB-SPEC.md "Model"), or one a program builds.
//     version: AC1015 (R2000), AC1018 (R2004), AC1021 (R2007), AC1024 (R2010), AC1027 (R2013), AC1032 (R2018); also
//       "R2004" / "2004"; empty = the model's own, else AC1015.
//   Throws std::invalid_argument for a version it does not write.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::dwg {

struct WriteOptions {
  std::string version;
  /// The file's creation / update time (ms since 1970, UTC); now when not given.
  std::optional<double> created_ms;
  /// Random bytes for the GUIDs the file carries (FINGERPRINTGUID, VERSIONGUID, new surface GUIDs); the OS's when
  /// empty. A fixed source makes the output reproducible (the parity harness gives the JS writer the same one).
  std::function<void(std::span<std::uint8_t>)> random;
};

struct WriteResult {
  std::vector<std::uint8_t> bytes;
  base::Value report;
};

[[nodiscard]] WriteResult write_dwg(const base::Value& model, const WriteOptions& opts = {});

/// The version code write_dwg writes for a version name ("2004", "R2004", "AC1018"), or "" when it writes none.
[[nodiscard]] std::string dwg_version_code(std::string_view v);

}  // namespace cc::dwg
