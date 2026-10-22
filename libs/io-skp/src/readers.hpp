// SPDX-License-Identifier: Apache-2.0
// The two containers' readers (vff.js readVff, legacy.js readLegacy). Internal to libs/io-skp. They raise Fail.
#pragma once

#include <cstdint>
#include <span>

#include "cc/skp/skp.hpp"

namespace cc::skp::detail {

/// SketchUp 2021+: the model without counts (read.cpp adds them and the header fields).
[[nodiscard]] Model read_vff(std::span<const std::uint8_t> bytes, const Sniff& header, const ReadOptions& opts);
/// SketchUp 3 … 2020.
[[nodiscard]] Model read_legacy(std::span<const std::uint8_t> bytes, const Sniff& header, const ReadOptions& opts);

}  // namespace cc::skp::detail
