// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg container-write.js, lz-r2004-write.js and r2007-write.js: lay the named sections out as a DWG file —
// R13–R15 (AC1015, ch. 3, 26), the R2004 family (AC1018, AC1024, AC1027, AC1032: ch. 4, 6, 7, 8, LZ77) and R2007
// (AC1021, ch. 5: Reed-Solomon, the CRC random encoder, 64-bit CRCs). Every "Spec gap:" note is in the JS files.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "wbits.hpp"

namespace cc::dwg::w {

struct VersionInfo {
  int code = 0;
  int maint = 0;
};
/// VERSION_INFO (ch. 27): the aux / file header version numbers and the maintenance release written.
[[nodiscard]] VersionInfo version_info(std::string_view version);

struct JulianDate {
  double day = 0;
  double ms = 0;
};

struct Parts {
  ByteBuf header, classes;
  struct Object {
    double handle = 0;
    ByteBuf data;
  };
  std::vector<Object> objects;
  bool measurement = false;
  JulianDate created, updated;
  double handseed = 0;
  std::vector<double> keys;
  std::optional<ByteBuf> acds;  // R2013+ AcDb:AcDsPrototype_1b
  std::uint64_t random_seed = 0x5A17C0DEULL;
};

[[nodiscard]] ByteBuf layout_r2000(const Parts& p);
[[nodiscard]] ByteBuf layout_r2004(const Parts& p, std::string_view version);
[[nodiscard]] ByteBuf layout_r2007(const Parts& p);

/// lz-r2004-write.js compressR2004.
[[nodiscard]] ByteBuf compress_r2004(std::span<const std::uint8_t> src);
/// r2007-write.js compressR2007.
[[nodiscard]] ByteBuf compress_r2007(std::span<const std::uint8_t> src);

}  // namespace cc::dwg::w
