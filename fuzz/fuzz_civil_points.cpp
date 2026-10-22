// SPDX-License-Identifier: Apache-2.0
// A survey point file (CivilCraft's points.js parsePoints) in every column order: any bytes give points and reasons —
// never a crash, a hang or a sanitizer report; the groups and the PNEZD writer run on what comes back.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cc/base/reader.hpp"
#include "cc/civil/points.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 20)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;
  const std::string_view text = cc::base::as_text(std::as_bytes(std::span(data, size)));
  for (const char* fmt : {"auto", "PENZD", "ENZ"}) {
    const auto r = cc::civil::parse_points(text, fmt, "PNEZD", kWords);
    (void)cc::civil::point_groups(r.points);
    (void)cc::civil::write_points(r.points, 3);
    if (!r.points.empty()) {
      (void)cc::civil::group_points(r.points, cc::civil::group_key(r.points[0].d));
      (void)cc::civil::lasso_points(r.points, {{0, 0}, {1e6, 0}, {1e6, 1e7}}, "crossing", 1);
    }
  }
  (void)cc::civil::parse_points(text, "auto", "PENZD", kWords);
  return 0;
}
