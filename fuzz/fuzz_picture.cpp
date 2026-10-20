// SPDX-License-Identifier: Apache-2.0
// A picture file someone gives CadCraft (pictures.js sniffPicture, jpegInfo, addPicture): any bytes are kept as a
// PNG or JPEG of a bounded size, or refused — never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/doc/doc.hpp"
#include "cc/doc/pictures.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 20)) {
    return 0;
  }
  const std::span<const std::uint8_t> bytes(data, size);
  (void)cc::doc::sniff_picture(bytes);
  (void)cc::doc::jpeg_info(bytes);
  cc::doc::Doc d;
  (void)cc::doc::add_picture(d, "C:\\in\\picture.png", bytes);
  (void)cc::doc::find_picture(d, "picture");
  return 0;
}
