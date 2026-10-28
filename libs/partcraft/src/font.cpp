// SPDX-License-Identifier: Apache-2.0
// font.js (PartCraft 0.5): Helvetica metrics.
#include "cc/partcraft/font.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace cc::partcraft::font {
namespace {

#include "nfd_base.inc"

// Helvetica advance widths for the printable ASCII range
constexpr std::array<std::uint16_t, 95> kHelv = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333,  278, 278, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667,
    611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667,  611, 722, 667, 944, 667,
    667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278,  556, 556, 222, 222, 500,
    222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500,  500, 334, 260, 334, 584,
};
// Windows-1252 0x80–0x9F: the Unicode character each code stands for
constexpr std::array<std::array<std::uint32_t, 2>, 27> kCp1252 = {{
    {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
    {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c}, {0x017D, 0x8e}, {0x2018, 0x91},
    {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
    {0x2122, 0x99}, {0x0161, 0x9a}, {0x203A, 0x9b}, {0x0153, 0x9c}, {0x017E, 0x9e}, {0x0178, 0x9f},
}};
// widths above 0x7F (by Windows-1252 code) that differ from their base letter
constexpr std::array<std::array<std::uint16_t, 2>, 46> kWide = {{
    {0x80, 556}, {0x85, 1000}, {0x89, 1000}, {0x8c, 1000}, {0x91, 222}, {0x92, 222}, {0x93, 333}, {0x94, 333},
    {0x95, 350}, {0x96, 556},  {0x97, 1000}, {0x99, 1000}, {0x9c, 944}, {0xa0, 278}, {0xa1, 333}, {0xa6, 260},
    {0xa7, 556}, {0xa9, 737},  {0xaa, 370},  {0xab, 556},  {0xac, 584}, {0xad, 333}, {0xae, 737}, {0xb0, 400},
    {0xb1, 584}, {0xb2, 333},  {0xb3, 333},  {0xb4, 333},  {0xb5, 556}, {0xb6, 537}, {0xb7, 278}, {0xb8, 333},
    {0xb9, 333}, {0xba, 365},  {0xbb, 556},  {0xbc, 834},  {0xbd, 834}, {0xbe, 834}, {0xbf, 611}, {0xc6, 1000},
    {0xd7, 584}, {0xd8, 778},  {0xdf, 611},  {0xe6, 889},  {0xf7, 584}, {0xf8, 611},
}};

std::uint16_t wide(std::uint32_t code) {
  for (const auto& [c, w] : kWide) {
    if (c == code) {
      return w;
    }
  }
  return 0;
}

std::uint32_t subst(std::uint32_t cp) {
  switch (cp) {
    case 0x2300: return 0xD8;  // ⌀ → Ø
    case 0x2212: return '-';   // − → -
    case 0x2032: return '\'';  // ′
    case 0x2033: return '"';   // ″
    case '\t': return ' ';
    default: return cp;
  }
}

}  // namespace

double char_advance(std::uint32_t cp) {
  const std::uint32_t code = subst(cp);
  if (code >= 32 && code < 127) {
    return kHelv[code - 32];
  }
  std::uint16_t w = 0;
  // NOLINTNEXTLINE(readability-qualified-auto) — a pointer natively, a class under Emscripten
  const auto in1252 = std::ranges::find_if(kCp1252, [code](const auto& r) { return r[0] == code; });
  if (in1252 != kCp1252.end()) {
    w = wide((*in1252)[1]);
  } else {
    w = wide(code);
  }
  if (w != 0) {
    return w;
  }
  // NOLINTNEXTLINE(readability-qualified-auto) — a pointer natively, a class under Emscripten
  const auto it = std::ranges::lower_bound(kNfdBase, code, {}, [](const auto& r) { return r[0]; });
  if (it != kNfdBase.end() && (*it)[0] == code) {
    return kHelv[(*it)[1] - 32];
  }
  return code >= 0x2e80 ? 1000 : 556;
}

double advance(std::string_view s) {
  double w = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = c;
    std::size_t len = 1;
    if (c >= 0xF0 && i + 3 < s.size()) {
      cp = ((c & 0x07U) << 18U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12U) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6U) | (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
      len = 4;
    } else if (c >= 0xE0 && i + 2 < s.size()) {
      cp = ((c & 0x0FU) << 12U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
      len = 3;
    } else if (c >= 0xC0 && i + 1 < s.size()) {
      cp = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
      len = 2;
    }
    w += char_advance(cp);
    i += len;
  }
  return w;
}

}  // namespace cc::partcraft::font
