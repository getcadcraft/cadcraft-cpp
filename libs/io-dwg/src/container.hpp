// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg container-read.js, r2007.js, lz-r2004.js and the reading half of crc.js: find the named sections
// of a DWG file (ODA spec v5.4.1 ch. 3 R13–R15, ch. 4 R2004 family, ch. 5 R2007, ch. 6–8 R2010–R2018, 2.14 CRCs).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bits.hpp"

namespace cc::dwg {

/// The file cannot be read at all (UnsupportedDwg in the JS): its message becomes the report's error.
class Unsupported : public DecodeError {
 public:
  using DecodeError::DecodeError;
};

struct Section {
  std::vector<std::uint8_t> owned;  // decompressed data, or empty when `view` points into the file
  Bytes view;
};

struct Container {
  std::string version;  // "AC1015"
  int ver = 0;          // 12 14 15 18 21 24 27 32
  int maintenance = 0;
  int codepage_num = 0;
  std::vector<std::string> notes;
  std::map<std::string, Section> sections;
  [[nodiscard]] const Section* section(const std::string& name) const {
    auto it = sections.find(name);
    return it == sections.end() ? nullptr : &it->second;
  }
};

constexpr std::array<std::uint8_t, 16> kSentinelHeader = {0xCF, 0x7B, 0x1F, 0x23, 0xFD, 0xDE, 0x38, 0xA9,
                                                          0x5F, 0x7C, 0x68, 0xB8, 0x4E, 0x6D, 0x33, 0x5F};
constexpr std::array<std::uint8_t, 16> kSentinelClasses = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
                                                           0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A};

/// Throws Unsupported (not a DWG of a supported family) or DecodeError.
[[nodiscard]] Container read_container(Bytes file);

/// 2.14.1 the 16-bit table CRC (seed 0xC0C1 for objects and sections).
[[nodiscard]] std::uint32_t crc8(std::uint32_t seed, Bytes b);
/// 2.14.2 CRC-32.
[[nodiscard]] std::uint32_t crc32(Bytes b, std::uint32_t seed = 0);
/// 4.2 the R2004 page checksum.
[[nodiscard]] std::uint32_t page_checksum(std::uint32_t seed, Bytes b);
/// 4.7 R2004-family LZ77.
[[nodiscard]] std::vector<std::uint8_t> decompress_r2004(Bytes src, std::size_t out_size);
/// 5.10 R2007 LZ77.
[[nodiscard]] std::vector<std::uint8_t> decompress_r21(Bytes src, std::size_t out_size);

/// VERSION_NUM of header-vars.js (0 when unknown).
[[nodiscard]] int version_num(std::string_view v);

}  // namespace cc::dwg
