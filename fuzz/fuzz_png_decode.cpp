// SPDX-License-Identifier: Apache-2.0
// A picture file printed to PDF (pictures.js decodePng, shrink, pdfImage): any bytes, as a PNG or a JPEG, give an
// image or nothing — never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/image/picture.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 20)) {
    return 0;
  }
  const std::span<const std::uint8_t> bytes(data, size);
  // at most 4 Mpixels here (a fuzzer's memory); the app's cap is 100 Mpixels
  if (const auto img = cc::image::decode_png(bytes, double{1U << 22U})) {
    (void)cc::image::shrink(*img, 1 + img->w / 3, 1 + img->h / 3);
    (void)cc::image::pdf_image("image/png", bytes, 72, 10, 10);
  }
  (void)cc::image::pdf_image("image/jpeg", bytes, 72, 10, 10);
  return 0;
}
