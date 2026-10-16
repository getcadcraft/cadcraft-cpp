// SPDX-License-Identifier: Apache-2.0
// The draw-list compiler on hostile models: bytes → read_dxf_model (the easiest way to reach odd models: any group can
// hold anything) → compile_drawing → JSON. Must never crash, hang or trip a sanitizer.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/drawing/compile.hpp"
#include "cc/dxf/read.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  const auto model = cc::dxf::read_dxf_model(cc::dxf::decode_text(bytes));
  if (model) {
    std::string out;
    cc::drawing::CompileOptions opt;
    opt.max_coords = 1'000'000;  // small caps: the fuzzer explores the logic, not the size
    opt.max_visits = 200'000;
    cc::drawing::write_json(cc::drawing::compile_drawing(*model, opt), out);
  }
  return 0;
}
