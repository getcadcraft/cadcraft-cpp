// SPDX-License-Identifier: Apache-2.0
// A SolidWorks sketch export (swsketch.js readSwExport, loadSwSketch) and the macro written back from what it opened
// (docToSwb): any text gives a drawing or a reason — never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/doc/doc.hpp"
#include "cc/doc/swsketch.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 16)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;
  const std::string text(cc::base::as_text(std::as_bytes(std::span(data, size))));
  const cc::doc::swsketch::Export e = cc::doc::swsketch::read_export_text(text, kWords);
  if (!e.ok) {
    return 0;
  }
  const auto j = cc::base::parse_json(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text);
  if (!j) {
    return 0;
  }
  cc::doc::Doc d;
  const cc::doc::swsketch::Loaded l = cc::doc::swsketch::load_sketch(d, *j, e.sketches.back(), kWords);
  if (l.ok) {
    (void)cc::doc::swsketch::to_swb(d, "fuzz", "2026-10-20", kWords);
  }
  return 0;
}
