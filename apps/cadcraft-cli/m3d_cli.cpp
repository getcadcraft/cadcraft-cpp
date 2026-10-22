// SPDX-License-Identifier: Apache-2.0
#include "m3d_cli.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/solid/cases.hpp"

namespace cc::cli {

namespace {

constexpr std::uint64_t kMaxInput = std::uint64_t{1} << 30U;

std::optional<base::Value> read_json(const std::filesystem::path& p) {
  const auto bytes = base::read_file(p, kMaxInput);
  if (!bytes) {
    std::println(stderr, "cadcraft-cli: {}", bytes.error().message);
    return std::nullopt;
  }
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());  // NOLINT
  auto v = base::parse_json(text);
  if (!v) {
    std::println(stderr, "cadcraft-cli: {}", v.error().message);
    return std::nullopt;
  }
  return std::move(*v);
}

}  // namespace

std::optional<int> run_m3d(const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return std::nullopt;
  }
  const std::string_view cmd = args[1];
  if (cmd == "manifold" && args.size() == 3) {  // tools/parity/manifold.mjs
    const auto cases = read_json(std::filesystem::path(args[2]));
    if (!cases) {
      return 1;
    }
    std::println("{}", base::to_json(solid::run_cases(*cases)));
    return 0;
  }
  return std::nullopt;
}

}  // namespace cc::cli
