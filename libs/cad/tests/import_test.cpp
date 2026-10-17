// SPDX-License-Identifier: Apache-2.0
// cc::cad: CadCraft's drawing from the readers' models, against the 0.5 JS importDwg (tests/golden/README.md), and
// the JS details the port leans on.
#include "cc/cad/import.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/dwg/read.hpp"
#include "cc/i18n/i18n.hpp"
#include "golden.hpp"
#include "props.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

cc::i18n::Catalog english() {
  const auto bytes = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
  REQUIRE(bytes.has_value());
  auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*bytes));
  REQUIRE(c.has_value());
  return std::move(*c);
}

}  // namespace

TEST_CASE("the drawing matches the JS importDwg on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "import";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const cc::i18n::Catalog words = english();
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();  // kitchen-equipment.dwg.json
    const std::string dwg = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / dwg;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / dwg;
    }
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    const Value db = cc::dwg::read_dwg(*bytes);
    const Value got = cc::cad::import_dwg(db, cc::base::as_text(std::span<const std::byte>(*bytes).first(6)), words);
    std::vector<std::string> diffs;
    cc::test::compare(*want, got, "", diffs);
    INFO(dwg << ": " << (diffs.empty() ? std::string() : diffs.front()));
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked >= 4);
}

TEST_CASE("MTEXT codes come out as dxf.js mtextPlain gives them") {
  using cc::cad::detail::mtext_plain;
  CHECK(mtext_plain("\\fArial|b0|i0;Hello\\PWorld") == "Hello\nWorld");
  CHECK(mtext_plain("{\\C1;red} text") == "red text");
  CHECK(mtext_plain("1\\S2/3;\" wide") == "12/3\" wide");
  CHECK(mtext_plain("50%%d and %%c10 %%p0.5") == "50° and ⌀10 ±0.5");
  CHECK(mtext_plain("a\\\\b {braces}") == "a\\b braces");
  CHECK(mtext_plain("  \\LUnderlined\\l  ") == "Underlined");
  CHECK(mtext_plain("\\U+00E9t\\U+00E9") == "été");
  CHECK(mtext_plain("no semicolon \\H2.5x") == "no semicolon \\H2.5x");  // the JS regex needs its ';'
  CHECK(cc::cad::detail::plain("%%uAB%%u %%k") == "AB %%k");             // plain() keeps %%k (dxf.js drops it)
}

TEST_CASE("dimension variables read as dwg-props.js reads them") {
  Value st = cc::cad::detail::dim_defaults();
  const Value vars = cc::base::make_object({{"DIMSCALE", 48},
                                            {"DIMCLRD", 256},
                                            {"DIMLWD", 35},
                                            {"DIMDSEP", 44},
                                            {"DIMPOST", "pre<>suf"},
                                            {"DIMTOL", 1},
                                            {"DIMTP", 0.1},
                                            {"DIMTM", 0.1},
                                            {"DIMGAP", -0.5},
                                            {"DIMTIH", 1},
                                            {"DIMTOH", 0}});
  cc::cad::detail::st_from_vars(vars, st);
  CHECK(st["scale"] == Value(48));
  CHECK(st["clrd"].is_null());  // 256 = by layer
  CHECK(st["lwd"] == Value(0.35));
  CHECK(st["dsep"] == Value(","));
  CHECK(st["pre"] == Value("pre"));
  CHECK(st["suf"] == Value("suf"));
  CHECK(st["tol"] == Value("sym"));  // DIMTOL with equal plus and minus
  CHECK(st["gap"] == Value(0.5));    // a negative gap is turned round (basic only without DIMTOL)
  CHECK(st["tih"] == Value(false));  // DIMTIH && DIMTOH
}

TEST_CASE("an empty or foreign drawing model gives an empty drawing, not a failure") {
  const cc::i18n::Catalog words = english();
  const Value d = cc::cad::import_dwg(Value::object(), "AC1015", words);
  CHECK(d["entities"].as_array().empty());
  CHECK(d["meta"]["release"] == Value("2000"));
  const Value hostile = cc::base::make_object(
      {{"blocks", cc::base::Array{cc::base::make_object(
                      {{"name", "*Model_Space"},
                       {"entities", cc::base::Array{cc::base::make_object({{"type", "MINSERT"},
                                                                           {"block", "B"},
                                                                           {"insert", cc::base::pt3(0, 0, 0)},
                                                                           {"rows", 1e9},
                                                                           {"cols", 1e9}})}}})}}});
  const Value h = cc::cad::import_dwg(hostile, "AC1015", words, {.minsert_copies = 50});
  CHECK(h["entities"].as_array().size() == 50);  // the copies are capped (the JS has no limit: docs/gaps.md)
}
