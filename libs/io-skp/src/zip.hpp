// SPDX-License-Identifier: Apache-2.0
// lib/skp zip.js: the smallest ZIP reader cc-skp needs (central directory; stored and deflate; CRC-32 checked;
// ZIP64, encrypted and multi-part archives refused; entry and total sizes capped). Internal to libs/io-skp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace cc::skp::detail {

class Zip {
 public:
  /// openZip(bytes, start): the archive whose first local header is at `start`.
  Zip(std::span<const std::uint8_t> bytes, std::size_t start, const i18n::Catalog* cat);

  [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }
  [[nodiscard]] bool has(std::string_view name) const;
  /// Unpacks one entry; `used` = bytes inflated so far for this file (the total cap holds across entries).
  [[nodiscard]] Bytes read(std::string_view name, std::uint64_t& used) const;

 private:
  struct Entry {
    std::string name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t csize = 0;
    std::uint32_t usize = 0;
    std::uint64_t local = 0;
  };
  [[nodiscard]] const Entry* find(std::string_view name) const;

  std::span<const std::uint8_t> b_;
  std::size_t start_;
  const i18n::Catalog* cat_;
  std::vector<Entry> entries_;  // directory order, first of each name
  std::vector<std::string> names_;
};

/// CRC-32 (IEEE).
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace cc::skp::detail
