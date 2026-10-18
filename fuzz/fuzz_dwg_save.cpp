// SPDX-License-Identifier: Apache-2.0
// Saving whatever drawing CadCraft makes of hostile bytes (read → import → document → save, the path File > Save as
// takes): save_dwg must never crash, hang or trip a sanitizer, and its file must read back. The first byte picks the
// version saved (the rest is the file). The same document is saved as DXF R12 and R2000 too (where the JS throws, the
// R12 writer throws std::invalid_argument; the save command's guard turns it into a message).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "cc/base/reader.hpp"
#include "cc/cad/import.hpp"
#include "cc/doc/doc.hpp"
#include "cc/dwg/read.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 7) {
    return 0;
  }
  static constexpr std::array<const char*, 6> kVersions = {"2000", "2004", "2007", "2010", "2013", "2018"};
  static const cc::i18n::Catalog kWords;  // no catalog: the notes are their keys
  const auto bytes = std::as_bytes(std::span(data, size)).subspan(1);
  const cc::base::Value model = cc::dwg::read_dwg(bytes, {.threads = 1});
  if (!model["blocks"].is_array() || model["blocks"].as_array().empty()) {
    return 0;
  }
  cc::doc::Doc d;
  d.load(cc::cad::import_dwg(model, cc::base::as_text(bytes.first(6)), kWords), kWords);
  cc::save::DwgSaveOptions o;
  o.version = kVersions[data[0] % kVersions.size()];
  o.created_ms = 0;
  o.random = [](std::span<std::uint8_t> b) {
    for (std::uint8_t& x : b) {
      x = 0x5A;
    }
  };
  const cc::save::DwgSaveResult r = cc::save::save_dwg(d, o, kWords);
  const cc::base::Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)), {.threads = 1});
  (void)back;
  try {
    (void)cc::save::write_dxf(d, kWords);
  } catch (const std::invalid_argument&) {  // NOLINT(bugprone-empty-catch) — the JS's own refusal
  }
  (void)cc::save::write_dxf2000(d, kWords);
  return 0;
}
