// SPDX-License-Identifier: Apache-2.0
// cc::save — L4. A CadCraft drawing → DWG: the C++ port of CadCraft 0.5's dwg-save.js, at parity with it (the same
// bytes for the same document, date and random bytes; tools/parity/dwg-save.mjs). The document becomes cc-dwg's
// writer model (layers, linetypes, text and dimension styles, blocks, model and sheets, views, UCS, split screen,
// dynamic blocks, pictures, clipped references, the 3D model) and cc::dwg::write_dwg writes it.
//
//   auto r = cc::save::save_dwg(doc, {.version = "2018"}, words);   // r.bytes; r.report: version, notes, counts, top
//
// Not ported yet (docs/gaps.md): the constraints payload (CADCRAFT_PARAMETRIC; the fork does not keep constraints),
// a visibility parameter written again after its states were edited (no block editor yet).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::save {

/// The versions Save as offers, newest first: { key '2018', code 'AC1032' } … { '2000', 'AC1015' }.
struct DwgVersion {
  std::string_view key, code;
};
[[nodiscard]] std::span<const DwgVersion> dwg_versions() noexcept;
/// dwgVersionKey(v): '2018', 'AC1032' or 'R2018' → '2018'; empty when unknown.
[[nodiscard]] std::string dwg_version_key(std::string_view v);

struct DwgSaveOptions {
  std::string version = "2000";
  std::optional<double> created_ms;                     // the file's dates (ms since 1970); now when absent
  std::function<void(std::span<std::uint8_t>)> random;  // the GUIDs' bytes; the system's when empty
  bool model_only = false;       // return the writer model instead of writing it (DXF 2000 writes it next)
  bool explode_leaders = false;  // multileaders as their parts (R2000 DXF has no MULTILEADER)
};

struct DwgSaveResult {
  std::vector<std::uint8_t> bytes;
  base::Value report;  // { version, notes, counts, top, … the writer's }
  base::Value model;   // the writer model (model_only, and for the parity check)
  base::Value counts;
};

/// saveDwg(doc, { version, created }). Throws std::invalid_argument for a model the writer refuses.
[[nodiscard]] DwgSaveResult save_dwg(const doc::Doc& doc, const DwgSaveOptions& opts, const i18n::Catalog& words);

}  // namespace cc::save
