// SPDX-License-Identifier: Apache-2.0
// The DWG writer: byte for byte the JS writer's output for the public fixtures in every version
// (tests/golden/README.md), its files read back by our own reader, and its parts — the R2004 and R2007 compressors
// against the reader's decompressors, the bit writer against the bit reader.
#include "cc/dwg/write.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "bits.hpp"
#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/dwg/read.hpp"
#include "container.hpp"
#include "golden.hpp"
#include "wbits.hpp"
#include "wcontainer.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

/// The parity harness's random bytes (tools/parity/dwg-write.mjs): s = s * 1103515245 + 12345, byte = s >> 24.
cc::dwg::WriteOptions seeded(const std::string& version, double created) {
  cc::dwg::WriteOptions o;
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

std::map<std::string, std::size_t> entity_types(const Value& model) {
  std::map<std::string, std::size_t> n;
  for (const Value& b : model["blocks"].as_array()) {
    if (!b["entities"].is_array()) {
      continue;
    }
    for (const Value& e : b["entities"].as_array()) {
      n[e["type"].is_string() ? e["type"].as_string() : std::string("?")] += 1;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("the writer matches the JS writer byte for byte on the public fixtures, in all six versions") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dwg-write/writes.json";
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
  std::map<std::string, Value> models;
  int checked = 0;
  for (const Value& w : (*want)["writes"].as_array()) {
    const std::string name = w["name"].as_string();
    const std::string version = w["version"].as_string();
    if (!models.contains(name)) {
      fs::path src = corpus / "fixtures/dwg" / name;
      if (!fs::exists(src)) {
        src = corpus / "fixtures/electrical" / name;
      }
      REQUIRE(fs::exists(src));
      const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
      REQUIRE(bytes.has_value());
      models[name] = cc::dwg::read_dwg(*bytes);
    }
    const cc::dwg::WriteResult r = cc::dwg::write_dwg(models[name], seeded(version, created));
    INFO(name << " " << version);
    CHECK(static_cast<double>(r.bytes.size()) == w["size"].as_number());
    CHECK(static_cast<double>(cc::dwg::crc32(r.bytes)) == w["crc32"].as_number());
    std::vector<std::string> diffs;
    cc::test::compare(w["report"], r.report, "report", diffs);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 24);
}

TEST_CASE("a written drawing reads back with the same entities") {
  const fs::path src = fs::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dwg";
  if (!fs::exists(src)) {
    SKIP("public fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
  REQUIRE(bytes.has_value());
  const Value model = cc::dwg::read_dwg(*bytes);
  const auto before = entity_types(model);
  for (const char* v : {"AC1015", "AC1018", "AC1021", "AC1024", "AC1027", "AC1032"}) {
    const cc::dwg::WriteResult r = cc::dwg::write_dwg(model, seeded(v, 0));
    const Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)));
    INFO(v);
    CHECK(back["version"] == Value(v));
    CHECK(!back["report"]["error"].truthy());
    CHECK(entity_types(back) == before);
  }
}

TEST_CASE("the writer refuses a version it does not write") {
  CHECK_THROWS_AS(cc::dwg::write_dwg(Value::object(), seeded("AC1012", 0)), std::invalid_argument);
  CHECK(cc::dwg::dwg_version_code("2004") == "AC1018");
  CHECK(cc::dwg::dwg_version_code("R2018") == "AC1032");
  CHECK(cc::dwg::dwg_version_code("2005").empty());
}

TEST_CASE("an empty model writes a drawing every version reads back") {
  for (const char* v : {"AC1015", "AC1018", "AC1021", "AC1024", "AC1027", "AC1032"}) {
    const cc::dwg::WriteResult r = cc::dwg::write_dwg(Value::object(), seeded(v, 0));
    const Value back = cc::dwg::read_dwg(std::as_bytes(std::span(r.bytes)));
    INFO(v);
    CHECK(!back["report"]["error"].truthy());
    CHECK(back["blocks"].is_array());
  }
}

TEST_CASE("the R2004 and R2007 compressors round-trip through the reader's decompressors") {
  std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed): a fixed test set
  std::vector<std::vector<std::uint8_t>> inputs = {{}, {1}, {1, 2, 3}, std::vector<std::uint8_t>(0x7400, 0)};
  for (int k = 0; k < 20; ++k) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>(rng() % 6000));
    const unsigned alphabet = 1 + (rng() % 8);
    for (std::uint8_t& x : v) {
      x = static_cast<std::uint8_t>(rng() % alphabet);  // small alphabets give long matches
    }
    inputs.push_back(std::move(v));
  }
  for (const auto& in : inputs) {
    const auto z4 = cc::dwg::w::compress_r2004(in);
    auto out4 = cc::dwg::decompress_r2004(z4, std::max<std::size_t>(in.size(), 4));
    out4.resize(in.size());
    CHECK(out4 == in);
    if (!in.empty()) {
      const auto z7 = cc::dwg::w::compress_r2007(in);
      const auto out7 = cc::dwg::decompress_r21(z7, in.size());
      CHECK(out7 == in);
    }
  }
}

TEST_CASE("the bit writer and the bit reader agree") {
  cc::dwg::w::BitWriter w("AC1018");
  w.B(true);
  w.BS(0);
  w.BS(256);
  w.BS(17);
  w.BS(-2);
  w.BL(0);
  w.BL(255);
  w.BL(70000);
  w.BD(0);
  w.BD(1);
  w.BD(-0.0);
  w.BD(3.25);
  w.RS(0x1234);
  w.RL(0xDEADBEEF);
  w.TV(Value("Ä \\U"));
  const auto bytes = w.bytes();
  cc::dwg::Bits r(bytes, 18, nullptr);
  CHECK(r.B() == 1);
  CHECK(r.BS() == 0);
  CHECK(r.BS() == 256);
  CHECK(r.BS() == 17);
  CHECK(r.BS() == -2);
  CHECK(r.BL() == 0);
  CHECK(r.BL() == 255);
  CHECK(r.BL() == 70000);
  CHECK(r.BD() == 0);
  CHECK(r.BD() == 1);
  CHECK(std::signbit(r.BD()));
  CHECK(r.BD() == 3.25);
  CHECK(r.RSu() == 0x1234);
  CHECK(r.RL() == 0xDEADBEEF);
}
