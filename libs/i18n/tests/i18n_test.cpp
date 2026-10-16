// SPDX-License-Identifier: Apache-2.0
#include "cc/i18n/i18n.hpp"

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/reader.hpp"

TEST_CASE("fills and plurals as i18n.js t()") {
  auto c = cc::i18n::Catalog::parse(
      "en", R"({"a":"Sheet {name}","b":{"=0":"none","one":"{n} corner","other":"{n} corners"},"c":"{{x}} {y}"})");
  REQUIRE(c.has_value());
  CHECK(c->t("a", {{"name", "A-101"}}) == "Sheet A-101");
  CHECK(c->tn("b", 0) == "none");
  CHECK(c->tn("b", 1) == "1 corner");
  CHECK(c->tn("b", 3) == "3 corners");
  CHECK(c->t("c") == "{x} {y}");  // braces escaped, an unknown fill as written
  CHECK(c->t("missing.key") == "missing.key");
}

TEST_CASE("the shipped catalogs read, with the keys the viewer uses, and Spanish falls back to English") {
  auto read = [](const char* name) {
    auto b = cc::base::read_file(std::string(CC_ASSETS_DIR) + "/lang/" + name, std::uint64_t{1} << 24U);
    REQUIRE(b.has_value());
    return std::string(cc::base::as_text(*b));
  };
  auto en = cc::i18n::Catalog::parse("en", read("en.json"));
  auto es = cc::i18n::Catalog::parse("es-MX", read("es-MX.json"));
  REQUIRE(en.has_value());
  REQUIRE(es.has_value());
  es->set_fallback(&*en);
  for (const char* k : {"ui.open", "ui.layers", "ui.zoom_everything", "app.tab.model", "ui.open_dwg_dxf_ctrl"}) {
    CHECK(en->has(k));
    CHECK(es->t(k) != k);
  }
  CHECK(en->t("ui.layers") == "Layers");
}
