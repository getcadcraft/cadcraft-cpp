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
#include "cc/i18n/i18n.hpp"
#include "cc/m3d/model.hpp"
#include "cc/m3d/script.hpp"
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
  if (cmd == "m3d-script" && args.size() == 3) {  // tools/parity/m3d.mjs: one JSON line per script
    const auto file = read_json(std::filesystem::path(args[2]));
    if (!file) {
      return 1;
    }
    const auto words = read_json(std::filesystem::path(CC_ASSETS_DIR) / "lang/en.json");
    std::optional<i18n::Catalog> catalog;
    if (words) {
      if (auto c = i18n::Catalog::parse("en", base::to_json(*words))) {
        catalog = std::move(*c);
      }
    }
    m3d::set_words(catalog ? &*catalog : nullptr);
    const base::Value scripts = file->get("scripts");
    if (scripts.is_array()) {
      for (const base::Value& s : scripts.as_array()) {
        std::println("{}", base::js_stringify(m3d::run_script(s)));  // NaN as null, as JSON.stringify writes it
      }
    }
    m3d::set_words(nullptr);
    return 0;
  }
  return std::nullopt;
}

}  // namespace cc::cli
