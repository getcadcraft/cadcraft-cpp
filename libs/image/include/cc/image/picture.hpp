// SPDX-License-Identifier: Apache-2.0
// cc::image — pictures.js's file side for print (CadCraft 0.5 WL.13): a JPEG's frame header, 0.5's own PNG decoder
// (decodePng: 8-bit grey, grey + alpha, RGB, RGBA and palette + tRNS, 16-bit taken to 8; interlaced files refused),
// the box filter that shrinks a picture to a print density, and a picture as a PDF image (pdfImage). File data is
// untrusted: every read is bounds-checked, sizes are capped (100 Mpixels) and a damaged file gives nothing.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cc::image {

struct JpegInfo {
  double w = 0, h = 0, components = 0;
};
/// jpegInfo(bytes): the frame header (SOF0–SOF2).
[[nodiscard]] std::optional<JpegInfo> jpeg_info(std::span<const std::uint8_t> bytes);

struct Decoded {
  std::uint32_t w = 0, h = 0;
  std::vector<std::uint8_t> rgb;                   // w × h × 3
  std::optional<std::vector<std::uint8_t>> alpha;  // w × h, absent when every pixel is opaque
};
/// A zlib stream inflated to its end (cut short, damaged or over `cap` bytes: nothing) —
/// DecompressionStream('deflate').
[[nodiscard]] std::optional<std::vector<std::uint8_t>> zlib_inflate(std::span<const std::uint8_t> in, std::size_t cap);
/// decodePng(bytes): the picture's pixels, or nothing (not a PNG 0.5 reads, damaged, over `max_pixels`).
[[nodiscard]] std::optional<Decoded> decode_png(std::span<const std::uint8_t> bytes, double max_pixels = 100e6);
/// shrink(img, w2, h2): box-filtered down to w2 × h2.
[[nodiscard]] Decoded shrink(const Decoded& img, std::uint32_t w2, std::uint32_t h2);

struct PdfImage {
  double w = 0, h = 0;
  std::string filter;  // "DCTDecode" (a JPEG as it is) | "FlateDecode"
  std::vector<std::uint8_t> data;
  double colors = 3;
  std::optional<std::vector<std::uint8_t>> smask;  // the transparency, deflated
  bool shrunk = false, kept = false;
};
/// pdfImage(rec, { maxDpi, mmW, mmH }): a JPEG goes in as it is (grey or RGB; CMYK is left out), a PNG decoded and
/// deflated with its transparency as a soft mask — shrunk to `max_dpi` on paper when printed denser (a JPEG is kept,
/// said in `kept`).
[[nodiscard]] std::optional<PdfImage> pdf_image(std::string_view mime, std::span<const std::uint8_t> bytes,
                                                double max_dpi = 0, double mm_w = 0, double mm_h = 0);

}  // namespace cc::image
