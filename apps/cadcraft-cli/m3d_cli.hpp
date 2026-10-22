// SPDX-License-Identifier: Apache-2.0
// The 3D commands of cadcraft-cli (W2: m3d, SolidOps, ModelCraft), kept apart from main.cpp.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace cc::cli {

/// Runs a 3D command when args[1] names one (`manifold <cases.json>`); nullopt when it is not one of them.
[[nodiscard]] std::optional<int> run_m3d(const std::vector<std::string_view>& args);

}  // namespace cc::cli
