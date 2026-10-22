// SPDX-License-Identifier: Apache-2.0
// libs/io-skp against cc-skp's own test files (tests/golden/skp/: the hand-made files of CadCraft 0.5's
// tools/skp-test/test.mjs, made by tools/gen/skp-fixtures.mjs with the frozen readSkp's answer to each): the same
// model field for field, or the same refusal (code and message). Then every cut of those files is read without
// trouble. The corpus check (27 files: SketchUp 3 … 2026) is the parity kit's (`cadcraft-cli parity skp`).
#include "cc/skp/skp.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "golden.hpp"

using cc::base::Value;

namespace {

std::filesystem::path fixtures() {
  return CC_SKP_FIXTURES;
}

std::vector<std::uint8_t> bytes_of(const std::filesystem::path& p) {
  const auto b = cc::base::read_file(p, std::uint64_t{1} << 26U);
  REQUIRE(b.has_value());
  std::vector<std::uint8_t> out(b->size());
  for (std::size_t i = 0; i < b->size(); ++i) {
    out[i] = static_cast<std::uint8_t>((*b)[i]);
  }
  return out;
}

Value json_of(const std::filesystem::path& p) {
  const auto b = cc::base::read_file(p, std::uint64_t{1} << 26U);
  REQUIRE(b.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*b));
  REQUIRE(v.has_value());
  return *v;
}

}  // namespace

TEST_CASE("io-skp.golden: cc-skp's hand-made files read as the 0.5 readSkp reads them") {
  const Value index = json_of(fixtures() / "index.json");
  std::size_t n = 0;
  for (const Value& name : index.as_array()) {
    INFO(name.as_string());
    const auto bytes = bytes_of(fixtures() / (name.as_string() + ".skp"));
    const Value want = json_of(fixtures() / (name.as_string() + ".json"));
    const auto got = cc::skp::read_skp(bytes);
    if (want.has("error")) {
      REQUIRE_FALSE(got.has_value());
      CHECK(got.error().code == want["error"]["code"].as_string());
      CHECK(got.error().message == want["error"]["message"].as_string());
    } else {
      REQUIRE(got.has_value());
      // through JSON, as the parity kit sees it: bytes as {"$base64": …}
      const auto back = cc::base::parse_json(cc::base::to_json(cc::skp::to_value(*got)));
      REQUIRE(back.has_value());
      std::vector<std::string> diffs;
      cc::test::compare(want["value"], *back, "", diffs);
      for (const std::string& d : diffs) {
        UNSCOPED_INFO(d);
      }
      CHECK(diffs.empty());
    }
    ++n;
  }
  CHECK(n == 31);
}

TEST_CASE("io-skp: the known model's geometry (test.mjs units)") {
  const auto bytes = bytes_of(fixtures() / "known.skp");
  const auto skp = cc::skp::read_skp(bytes);
  REQUIRE(skp.has_value());
  CHECK(skp->vff);
  CHECK(skp->major == 25);
  REQUIRE(skp->defs.size() == 2);
  CHECK(skp->defs[1].guid == "1bf0cca1-cf72-5a4a-86d6-a42d71d5b3a4");
  const cc::skp::Face& f = skp->defs[0].faces.at(0);
  REQUIRE(f.loops.size() == 2);
  CHECK(f.loops[1][1] == cc::skp::P3{4, 6, 0});  // the hole runs the other way
  const cc::skp::Instance& inst = skp->defs[0].insts.at(0);
  // rows (0,-1,0) (1,0,0) (0,0,1), t = (50,0,0), w = 0.5: (1,0,0) → (100,2,0)
  const auto& m = inst.m;
  CHECK(m[0] * 1 + m[12] == 100);
  CHECK(m[1] * 1 + m[13] == 2);
  CHECK(skp->length_unit == "mm");
  CHECK(skp->notes.empty());
  std::size_t progress = 0;
  cc::skp::ReadOptions opts;
  opts.on_progress = [&progress](const cc::skp::Progress&) { ++progress; };
  CHECK(cc::skp::read_skp(bytes, opts).has_value());
  CHECK(progress > 0);
  cc::skp::ReadOptions stop;
  stop.cancelled = [] { return true; };
  const auto aborted = cc::skp::read_skp(bytes, stop);
  REQUIRE_FALSE(aborted.has_value());
  CHECK(aborted.error().code == "aborted");
}

TEST_CASE("io-skp: every cut of the hand-made files is a model or a refusal") {
  for (const auto& entry : std::filesystem::directory_iterator(fixtures())) {
    if (entry.path().extension() != ".skp") {
      continue;
    }
    const auto bytes = bytes_of(entry.path());
    for (std::size_t cut = 0; cut < bytes.size(); cut += 1 + bytes.size() / 97) {
      const std::span<const std::uint8_t> part(bytes.data(), cut);
      const auto r = cc::skp::read_skp(part);
      if (!r) {
        CHECK_FALSE(r.error().code.empty());
      }
    }
    // flipped bytes: still a model or a refusal
    for (std::size_t at = 0; at < bytes.size(); at += 1 + bytes.size() / 61) {
      auto copy = bytes;
      copy[at] ^= 0xff;
      const auto r = cc::skp::read_skp(copy);
      if (!r) {
        CHECK_FALSE(r.error().code.empty());
      }
    }
  }
}
