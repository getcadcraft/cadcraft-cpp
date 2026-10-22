// SPDX-License-Identifier: Apache-2.0
// cc-skp's reader (libs/io-skp) on hostile files: a model or a refusal, never a crash, a hang or a sanitizer report;
// whatever reads also turns into the JS model object.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/skp/skp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto r = cc::skp::read_skp(std::span<const std::uint8_t>(data, size));
  if (r) {
    (void)cc::skp::to_value(*r);
  }
  return 0;
}
