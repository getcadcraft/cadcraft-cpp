// SPDX-License-Identifier: Apache-2.0
// pictures.js (CadCraft 0.5) in C++: jpegInfo, decodePng, shrink, pdfImage — the same pixels and the same bytes.
#include "cc/image/picture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "cc/base/checked.hpp"
#include "cc/image/png.hpp"

namespace cc::image {

namespace {

constexpr double kMaxPixels = 100e6;
/// How much more than the rows need an IDAT stream may inflate to (0.5 takes any; past this the file is refused).
constexpr std::size_t kSlack = std::size_t{64} * 1024 * 1024;

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t i) {
  return (static_cast<std::uint32_t>(b[i]) << 24U) | (static_cast<std::uint32_t>(b[i + 1]) << 16U) |
         (static_cast<std::uint32_t>(b[i + 2]) << 8U) | static_cast<std::uint32_t>(b[i + 3]);
}

/// DecompressionStream('deflate'): a zlib stream to its end (cut short or damaged: nothing), at most `cap` bytes.
std::optional<std::vector<std::uint8_t>> inflate_all(std::span<const std::uint8_t> in, std::size_t cap) {
  z_stream z{};
  if (inflateInit(&z) != Z_OK) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> out;
  std::array<std::uint8_t, 65536> buf{};
  std::size_t at = 0;
  int rc = Z_OK;
  while (rc == Z_OK) {
    const std::size_t chunk = std::min<std::size_t>(in.size() - at, std::size_t{1} << 30U);
    z.next_in = const_cast<Bytef*>(in.subspan(at).data());  // NOLINT(cppcoreguidelines-pro-type-const-cast) zlib's API
    z.avail_in = static_cast<uInt>(chunk);
    z.next_out = buf.data();
    z.avail_out = static_cast<uInt>(buf.size());
    rc = inflate(&z, Z_NO_FLUSH);
    at += chunk - z.avail_in;
    const std::size_t got = buf.size() - z.avail_out;
    if (out.size() + got > cap) {
      rc = Z_MEM_ERROR;
      break;
    }
    const auto chunk_out = std::span(buf).first(got);
    out.insert(out.end(), chunk_out.begin(), chunk_out.end());
    if (rc == Z_BUF_ERROR && got > 0) {
      rc = Z_OK;  // made progress; go on
    }
  }
  inflateEnd(&z);
  if (rc != Z_STREAM_END) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> zlib_inflate(std::span<const std::uint8_t> in, std::size_t cap) {
  return inflate_all(in, cap);
}

std::optional<JpegInfo> jpeg_info(std::span<const std::uint8_t> b) {
  std::size_t i = 2;
  while (i + 9 < b.size()) {
    if (b[i] != 0xff) {
      return std::nullopt;
    }
    const std::uint8_t m = b[i + 1];
    if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) {
      i += 2;
      continue;
    }
    const std::size_t len = (static_cast<std::size_t>(b[i + 2]) << 8U) | b[i + 3];
    if (m >= 0xc0 && m <= 0xc2) {
      return JpegInfo{.w = static_cast<double>((static_cast<unsigned>(b[i + 7]) << 8U) | b[i + 8]),
                      .h = static_cast<double>((static_cast<unsigned>(b[i + 5]) << 8U) | b[i + 6]),
                      .components = static_cast<double>(b[i + 9])};
    }
    if (m == 0xda || len < 2) {
      return std::nullopt;
    }
    i += 2 + len;
  }
  return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — decodePng, step for step
std::optional<Decoded> decode_png(std::span<const std::uint8_t> b, double max_pixels) {
  std::size_t i = 8;
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  int depth = -1;  // absent: refused below, as `undefined` is in 0.5
  int ct = -1;
  int inter = 0;
  std::span<const std::uint8_t> pal;
  std::optional<std::span<const std::uint8_t>> trns;
  std::vector<std::uint8_t> idat;
  while (i + 12 <= b.size()) {
    const std::size_t len = be32(b, i);
    const std::array<char, 4> type{static_cast<char>(b[i + 4]), static_cast<char>(b[i + 5]),
                                   static_cast<char>(b[i + 6]), static_cast<char>(b[i + 7])};
    if (i + 12 + len > b.size()) {
      return std::nullopt;
    }
    const std::span<const std::uint8_t> d = b.subspan(i + 8, len);
    const std::string_view t(type.data(), type.size());
    if (t == "IHDR") {
      if (i + 16 > b.size()) {
        return std::nullopt;
      }
      w = be32(b, i + 8);
      h = be32(b, i + 12);
      depth = d.size() > 8 ? d[8] : -1;
      ct = d.size() > 9 ? d[9] : -1;
      inter = d.size() > 12 ? d[12] : 0;
    } else if (t == "PLTE") {
      pal = d;
    } else if (t == "tRNS") {
      trns = d;
    } else if (t == "IDAT") {
      if (idat.size() + d.size() > (std::size_t{1} << 31U)) {
        return std::nullopt;
      }
      idat.insert(idat.end(), d.begin(), d.end());
    } else if (t == "IEND") {
      break;
    }
    i += 12 + len;
  }
  const bool ok_depth = depth == 8 || (depth == 16 && ct != 3);
  if (w == 0 || h == 0 || static_cast<double>(w) * static_cast<double>(h) > std::min(max_pixels, kMaxPixels) ||
      inter != 0 || !ok_depth || (ct != 0 && ct != 2 && ct != 3 && ct != 4 && ct != 6)) {
    return std::nullopt;
  }
  std::size_t chans = 1;  // { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ct]
  if (ct == 2) {
    chans = 3;
  } else if (ct == 4) {
    chans = 2;
  } else if (ct == 6) {
    chans = 4;
  }
  const std::size_t step = depth == 16 ? 2 : 1;
  const std::size_t bpp = chans * step;
  const std::size_t stride = std::size_t{w} * bpp;
  const std::size_t need = std::size_t{h} * (stride + 1);
  const auto raw = inflate_all(idat, need + kSlack);
  if (!raw || raw->size() < need) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> px(std::size_t{h} * stride);
  for (std::size_t y = 0; y < h; ++y) {
    const std::uint8_t f = (*raw)[y * (stride + 1)];
    if (f > 4) {
      return std::nullopt;
    }
    const std::size_t src = y * (stride + 1) + 1;
    const std::size_t row = y * stride;
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = x >= bpp ? px[row + x - bpp] : 0;
      const int u = y > 0 ? px[row - stride + x] : 0;
      const int c = y > 0 && x >= bpp ? px[row - stride + x - bpp] : 0;
      int v = (*raw)[src + x];
      if (f == 1) {
        v += a;
      } else if (f == 2) {
        v += u;
      } else if (f == 3) {
        v += (a + u) >> 1;
      } else if (f == 4) {
        const int p = a + u - c;
        const int pa = std::abs(p - a);
        const int pb = std::abs(p - u);
        const int pc = std::abs(p - c);
        int pred = c;  // Paeth
        if (pa <= pb && pa <= pc) {
          pred = a;
        } else if (pb <= pc) {
          pred = u;
        }
        v += pred;
      }
      px[row + x] = static_cast<std::uint8_t>(v & 0xff);
    }
  }
  const std::size_t n = std::size_t{w} * h;
  Decoded out{.w = w, .h = h, .rgb = std::vector<std::uint8_t>(n * 3), .alpha = std::nullopt};
  if (ct == 4 || ct == 6 || trns) {
    out.alpha = std::vector<std::uint8_t>(n, 255);
  }
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t o = k * bpp;
    const auto s = [&](std::size_t j) { return px[o + j * step]; };
    if (ct == 3) {
      const std::size_t ix = px[o];
      for (std::size_t j = 0; j < 3; ++j) {
        out.rgb[k * 3 + j] = ix * 3 + j < pal.size() ? pal[ix * 3 + j] : 0;
      }
      if (out.alpha) {
        (*out.alpha)[k] = trns && ix < trns->size() ? (*trns)[ix] : 255;
      }
    } else if (ct == 0 || ct == 4) {
      out.rgb[k * 3] = out.rgb[k * 3 + 1] = out.rgb[k * 3 + 2] = s(0);
      if (ct == 4) {
        (*out.alpha)[k] = s(1);
      }
    } else {
      out.rgb[k * 3] = s(0);
      out.rgb[k * 3 + 1] = s(1);
      out.rgb[k * 3 + 2] = s(2);
      if (ct == 6) {
        (*out.alpha)[k] = s(3);
      }
    }
  }
  if (out.alpha && std::ranges::all_of(*out.alpha, [](std::uint8_t v) { return v == 255; })) {
    out.alpha.reset();
  }
  return out;
}

Decoded shrink(const Decoded& img, std::uint32_t w2, std::uint32_t h2) {
  const std::uint64_t w = img.w;
  const std::uint64_t h = img.h;
  Decoded out{.w = w2,
              .h = h2,
              .rgb = std::vector<std::uint8_t>(std::size_t{w2} * h2 * 3),
              .alpha = img.alpha ? std::optional(std::vector<std::uint8_t>(std::size_t{w2} * h2)) : std::nullopt};
  for (std::uint64_t y = 0; y < h2; ++y) {
    for (std::uint64_t x = 0; x < w2; ++x) {
      const std::uint64_t x0 = x * w / w2;
      const std::uint64_t x1 = std::max(x0 + 1, (x + 1) * w / w2);
      const std::uint64_t y0 = y * h / h2;
      const std::uint64_t y1 = std::max(y0 + 1, (y + 1) * h / h2);
      std::uint64_t r = 0;
      std::uint64_t g = 0;
      std::uint64_t bl = 0;
      std::uint64_t a = 0;
      std::uint64_t n = 0;
      for (std::uint64_t yy = y0; yy < y1; ++yy) {
        for (std::uint64_t xx = x0; xx < x1; ++xx) {
          const auto k = static_cast<std::size_t>(yy * w + xx);  // < w × h ≤ 100 Mpixels
          r += img.rgb[k * 3];
          g += img.rgb[k * 3 + 1];
          bl += img.rgb[k * 3 + 2];
          if (img.alpha) {
            a += (*img.alpha)[k];
          }
          ++n;
        }
      }
      const auto o = static_cast<std::size_t>(y * w2 + x);  // r / n in a Uint8Array: the whole part
      out.rgb[o * 3] = static_cast<std::uint8_t>(r / n);
      out.rgb[o * 3 + 1] = static_cast<std::uint8_t>(g / n);
      out.rgb[o * 3 + 2] = static_cast<std::uint8_t>(bl / n);
      if (out.alpha) {
        (*out.alpha)[o] = static_cast<std::uint8_t>(a / n);
      }
    }
  }
  return out;
}

std::optional<PdfImage> pdf_image(std::string_view mime, std::span<const std::uint8_t> bytes, double max_dpi,
                                  double mm_w, double mm_h) {
  if (mime == "image/jpeg") {
    const auto s = jpeg_info(bytes);
    if (!s || (s->components != 1 && s->components != 3)) {  // CMYK JPEGs need a colour profile: left out
      return std::nullopt;
    }
    const bool dense = max_dpi > 0 && mm_w > 0 && s->w / (mm_w / 25.4) > max_dpi * 1.01;
    return PdfImage{.w = s->w,
                    .h = s->h,
                    .filter = "DCTDecode",
                    .data = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                    .colors = s->components,
                    .smask = std::nullopt,
                    .shrunk = false,
                    .kept = dense};
  }
  auto img = decode_png(bytes);
  if (!img) {
    return std::nullopt;
  }
  bool shrunk = false;
  if (max_dpi > 0 && mm_w > 0 && mm_h > 0) {
    const double w2 = std::max(1.0, base::js_round(max_dpi * mm_w / 25.4));
    const double h2 = std::max(1.0, base::js_round(max_dpi * mm_h / 25.4));
    if (w2 < img->w && h2 < img->h) {
      img = shrink(*img, static_cast<std::uint32_t>(w2), static_cast<std::uint32_t>(h2));
      shrunk = true;
    }
  }
  PdfImage out{.w = static_cast<double>(img->w),
               .h = static_cast<double>(img->h),
               .filter = "FlateDecode",
               .data = zlib_compress(img->rgb),
               .colors = 3,
               .smask = std::nullopt,
               .shrunk = shrunk,
               .kept = false};
  if (img->alpha) {
    out.smask = zlib_compress(*img->alpha);
  }
  return out;
}

}  // namespace cc::image
