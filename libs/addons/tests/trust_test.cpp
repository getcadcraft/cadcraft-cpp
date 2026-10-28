// SPDX-License-Identifier: Apache-2.0
// libs/addons against CadCraft 0.5's addons/trust.js: tests/golden/js-0.5-addon-trust.json holds test packages signed
// with a test key and what the frozen trust.js answered for each (readPackageJson, packageDigest, verifyPackage), and
// canonical() of a few values (tools/gen/addon-trust.mjs). Every answer here must be the same.
#include "cc/addons/trust.hpp"

#include <map>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"

using cc::base::Value;

TEST_CASE("addons.golden: package signatures as the 0.5 trust.js decides them") {
  const auto b = cc::base::read_file(CC_ADDON_GOLDEN, std::uint64_t{1} << 24U);
  REQUIRE(b.has_value());
  const auto g = cc::base::parse_json(cc::base::as_text(*b));
  REQUIRE(g.has_value());
  std::map<std::string, std::string, std::less<>> keys;
  Value kv = (*g)["keys"];
  for (const auto& m : kv.as_object()) {
    keys[m.key] = m.value.as_string();
  }
  std::size_t n = 0;
  for (const Value& c : (*g)["cases"].as_array()) {
    INFO(c["name"].as_string());
    const auto read = cc::addons::read_package_json(c["served"]);
    CHECK(read.ok == c["read"]["ok"].as_bool());
    if (!read.ok) {
      CHECK(read.reason == c["read"]["reason"].as_string());
      continue;
    }
    REQUIRE(read.pkg.has_value());
    if (!read.pkg) {
      continue;
    }
    const auto& pkg = *read.pkg;
    const auto digest = cc::addons::package_digest(pkg);
    if (c["digest"].is_null()) {
      CHECK_FALSE(digest.has_value());
    } else {
      CHECK(digest == std::optional<std::string>(c["digest"].as_string()));
    }
    const auto v = cc::addons::verify_package(pkg, keys);
    CHECK(v.ok == c["verdict"]["ok"].as_bool());
    CHECK(v.reason == c["verdict"]["reason"].as_string());
    ++n;
  }
  CHECK(n >= 10);
  for (const Value& c : (*g)["canonical"].as_array()) {
    CHECK(cc::addons::canonical(c["value"]) == c["text"].as_string());
  }
  // the production key is pinned, and a test key's package is not accepted by it
  CHECK(cc::addons::trusted_keys().contains("cc-addons-2026-1"));
  const auto good = cc::addons::read_package_json((*g)["cases"].as_array()[0]["served"]);
  REQUIRE(good.pkg.has_value());
  if (good.pkg) {
    CHECK(cc::addons::verify_package(*good.pkg).reason == "key");
  }
}

TEST_CASE("addons: paths and base64") {
  CHECK(cc::addons::good_path("a/b.js"));
  CHECK(cc::addons::good_path("_x"));
  CHECK_FALSE(cc::addons::good_path("../x"));
  CHECK_FALSE(cc::addons::good_path("a//b"));
  CHECK_FALSE(cc::addons::good_path("a/./b"));
  CHECK_FALSE(cc::addons::good_path(".hidden"));
  CHECK_FALSE(cc::addons::good_path(std::string(202, 'a')));
  CHECK(cc::addons::from_base64("aGk=") == cc::addons::Bytes{'h', 'i'});
  CHECK(cc::addons::from_base64("aGk") == cc::addons::Bytes{'h', 'i'});
  CHECK_FALSE(cc::addons::from_base64("a").has_value());
  CHECK_FALSE(cc::addons::from_base64("a-b_").has_value());
}
