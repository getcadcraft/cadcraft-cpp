// SPDX-License-Identifier: Apache-2.0
// CadCraft's own DXF reader (dxf.js readDxf → cc::cad::read_dxf) on text from strangers: a drawing or a reason, never
// a crash, a hang or a sanitizer report — the drawing model reader for multileaders, pictures and wipeouts included.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/base/reader.hpp"
#include "cc/cad/read_dxf.hpp"
#include "cc/dxf/read.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 20U)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;  // no catalog: the words are their keys
  const auto text = cc::dxf::decode_text(std::as_bytes(std::span(data, size)));
  (void)cc::cad::read_dxf(text, kWords, [](std::string_view t) { return cc::dxf::read_dxf_model(t); });
  return 0;
}
