// SPDX-License-Identifier: Apache-2.0
// The add-on host (libs/addons) on hostile JSON: a manifest (check_manifest for two apps), add-on data kept on records
// (clean_addon_data), a served package (read_package_json, then verify_package) and the stored on/off prefs (a Loader
// started over them). Never a crash; whatever comes back stringifies.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cc/addons/loader.hpp"
#include "cc/addons/trust.hpp"
#include "cc/base/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data),
                              size);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  auto v = cc::base::parse_json(text, cc::base::JsonLimits{std::size_t{1} << 20U, 64, std::size_t{1} << 20U});
  if (!v) {
    return 0;
  }
  for (const char* api : {"1.0", "1.4"}) {
    const auto r = cc::addons::check_manifest(*v, "cadcraft", api);
    (void)cc::base::js_stringify(r.manifest);
  }
  (void)cc::base::js_stringify(cc::addons::clean_addon_data(*v));
  const auto pkg = cc::addons::read_package_json(*v);
  if (pkg.ok && pkg.pkg) {
    (void)cc::addons::verify_package(*pkg.pkg);
  }
  cc::addons::MemoryKeyValue kv;
  kv.items["cc-addons:cadcraft"] = std::string(text);
  cc::addons::Builtin b;
  b.key = "x";
  b.manifest = *v;
  cc::addons::LoaderOptions o;
  o.app = "cadcraft";
  o.api_version = "1.0";
  o.builtins.push_back(std::move(b));
  o.storage = &kv;
  cc::addons::Loader loader(std::move(o));
  for (const auto& row : loader.start()) {
    (void)loader.set_enabled(row.id, false);
    (void)cc::base::js_stringify(row.to_value());
  }
  return 0;
}
