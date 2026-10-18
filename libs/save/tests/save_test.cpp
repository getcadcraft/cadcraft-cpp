// SPDX-License-Identifier: Apache-2.0
// Saving a drawing as DWG (the dwg-save.js port): the public fixtures opened as CadCraft opens them and saved in all
// six versions give the JS saveDwg's bytes (size + CRC-32) and report (tests/golden/README.md); what is saved reads
// back; a drawing made here (no file behind it) saves and reads back with what was drawn.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/cad/import.hpp"
#include "cc/doc/doc.hpp"
#include "cc/dwg/read.hpp"
#include "cc/i18n/i18n.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"
#include "golden.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

const cc::i18n::Catalog& words() {
  static const cc::i18n::Catalog kWords = [] {
    const auto text = cc::base::read_file(fs::path(CC_ASSETS_DIR) / "lang/en.json", std::uint64_t{1} << 26U);
    REQUIRE(text.has_value());
    auto c = cc::i18n::Catalog::parse("en", cc::base::as_text(*text));
    REQUIRE(c.has_value());
    return std::move(*c);
  }();
  return kWords;
}

/// The harness's random bytes (tools/parity/dwg-save.mjs): s = s * 1103515245 + 12345, byte = s >> 24, s = 1.
cc::save::DwgSaveOptions seeded(const std::string& version, double created) {
  cc::save::DwgSaveOptions o;
  o.version = version;
  o.created_ms = created;
  auto s = std::make_shared<std::uint32_t>(1);
  o.random = [s](std::span<std::uint8_t> b) {
    for (std::uint8_t& x : b) {
      *s = *s * 1103515245U + 12345U;
      x = static_cast<std::uint8_t>(*s >> 24U);
    }
  };
  return o;
}

/// CRC-32 (IEEE, reflected; cc-dwg crc.js crc32)
std::uint32_t crc32(std::span<const std::uint8_t> b) {
  std::uint32_t c = 0xFFFFFFFFU;
  for (const std::uint8_t x : b) {
    c ^= x;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0 ? (c >> 1U) ^ 0xEDB88320U : c >> 1U;
    }
  }
  return ~c;
}

std::unique_ptr<cc::doc::Doc> open(const fs::path& file) {
  const auto bytes = cc::base::read_file(file, std::uint64_t{1} << 30U);
  REQUIRE(bytes.has_value());
  const Value model = cc::dwg::read_dwg(*bytes);
  const auto head = std::span<const std::byte>(*bytes).first(6);
  auto d = std::make_unique<cc::doc::Doc>();
  d->load(cc::cad::import_dwg(model, cc::base::as_text(head), words()), words());
  return d;
}

}  // namespace

TEST_CASE("golden: saving the public fixtures gives the JS saveDwg's bytes and report in all six versions") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dwg-save/saves.json";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const auto ref_bytes = cc::base::read_file(golden, std::uint64_t{1} << 30U);
  REQUIRE(ref_bytes.has_value());
  const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
  REQUIRE(want.has_value());
  const double created = (*want)["created"].as_number();
  std::map<std::string, std::unique_ptr<cc::doc::Doc>> docs;
  int checked = 0;
  for (const Value& w : (*want)["saves"].as_array()) {
    const std::string name = w["name"].as_string();
    const std::string version = w["version"].as_string();
    if (!docs.contains(name)) {
      fs::path src = corpus / "fixtures/dwg" / name;
      if (!fs::exists(src)) {
        src = corpus / "fixtures/electrical" / name;
      }
      REQUIRE(fs::exists(src));
      docs[name] = open(src);
    }
    const cc::save::DwgSaveResult r = cc::save::save_dwg(*docs[name], seeded(version, created), words());
    INFO(name << " " << version);
    CHECK(static_cast<double>(r.bytes.size()) == w["size"].as_number());
    CHECK(static_cast<double>(crc32(r.bytes)) == w["crc32"].as_number());
    std::vector<std::string> diffs;
    cc::test::compare(w["report"], r.report, "report", diffs);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    // and the file reads back
    const Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)));
    CHECK(!back["report"]["error"].truthy());
    ++checked;
  }
  CHECK(checked == 24);
}

TEST_CASE(
    "golden: saving the public fixtures as DXF R12 and R2000 gives the JS writeDxf / writeDxf2000 text and report") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dxf-save/saves.json";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const auto ref_bytes = cc::base::read_file(golden, std::uint64_t{1} << 30U);
  REQUIRE(ref_bytes.has_value());
  const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
  REQUIRE(want.has_value());
  std::map<std::string, std::unique_ptr<cc::doc::Doc>> docs;
  int checked = 0;
  for (const Value& w : (*want)["saves"].as_array()) {
    const std::string name = w["name"].as_string();
    const std::string format = w["format"].as_string();
    if (!docs.contains(name)) {
      fs::path src = corpus / "fixtures/dwg" / name;
      if (!fs::exists(src)) {
        src = corpus / "fixtures/electrical" / name;
      }
      REQUIRE(fs::exists(src));
      docs[name] = open(src);
    }
    const cc::save::DxfSaveResult r =
        format == "2000" ? cc::save::write_dxf2000(*docs[name], words()) : cc::save::write_dxf(*docs[name], words());
    INFO(name << " " << format);
    std::vector<std::uint8_t> u8;
    u8.reserve(r.text.size());
    for (const char c : r.text) {
      u8.push_back(static_cast<std::uint8_t>(c));
    }
    CHECK(static_cast<double>(r.text.size()) == w["size"].as_number());
    CHECK(static_cast<double>(crc32(u8)) == w["crc32"].as_number());
    std::vector<std::string> diffs;
    cc::test::compare(w["report"], r.report, "report", diffs);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 8);
}

TEST_CASE("a drawing made here saves in every version and reads back with what was drawn") {
  cc::doc::Doc d;
  Value drawing = cc::base::make_object({{"entities", cc::base::Array{}}});
  d.load(drawing, words());
  d.change("DRAW", [&] {
    (void)d.add(cc::base::make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 100}, {"y2", 50}}));
    (void)d.add(cc::base::make_object({{"type", "circle"}, {"cx", 10}, {"cy", 10}, {"r", 5}, {"color", 1}}));
    (void)d.add(cc::base::make_object(
        {{"type", "text"}, {"x", 5}, {"y", 5}, {"h", 2.5}, {"s", "Hello"}, {"ha", "left"}, {"va", "baseline"}}));
    (void)d.add(cc::base::make_object({{"type", "dim"},
                                       {"kind", "linear"},
                                       {"p1", cc::base::pt2(0, 0)},
                                       {"p2", cc::base::pt2(100, 0)},
                                       {"loc", cc::base::pt2(0, -10)},
                                       {"rot", 0},
                                       {"text", ""}}));
  });
  for (const auto& v : cc::save::dwg_versions()) {
    const auto r = cc::save::save_dwg(d, seeded(std::string(v.key), 0), words());
    INFO(v.key);
    REQUIRE(!r.bytes.empty());
    const Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)));
    CHECK(back["version"] == Value(std::string(v.code)));
    CHECK(!back["report"]["error"].truthy());
    CHECK(r.report["top"]["LINE"] == Value(1));
    CHECK(r.report["top"]["CIRCLE"] == Value(1));
    CHECK(r.report["top"]["TEXT"] == Value(1));
    CHECK(r.report["top"]["DIMENSION"] == Value(1));
  }
}

TEST_CASE("version keys") {
  CHECK(cc::save::dwg_version_key("2018") == "2018");
  CHECK(cc::save::dwg_version_key("AC1027") == "2013");
  CHECK(cc::save::dwg_version_key("r2004") == "2004");
  CHECK(cc::save::dwg_version_key("2005").empty());
  CHECK(cc::save::dwg_versions().size() == 6);
}
