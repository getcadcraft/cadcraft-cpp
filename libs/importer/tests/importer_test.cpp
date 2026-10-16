// SPDX-License-Identifier: Apache-2.0
#include "cc/importer/importer.hpp"

#include <array>
#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

TEST_CASE("a crashing importer costs a message, not the caller") {
  const std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto r = cc::importer::run_helper(CC_CRASHING_IMPORTER, "segv", bytes);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.contains("SIGSEGV"));
}

TEST_CASE("a hanging importer is stopped at the time limit") {
  cc::importer::Limits l;
  l.max_seconds = 1;
  const std::array<std::byte, 1> bytes{std::byte{0}};
  const auto r = cc::importer::run_helper(CC_CRASHING_IMPORTER, "hang", bytes, l);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.contains("longer than 1 s"));
}

TEST_CASE("an importer that answers garbage is refused") {
  const std::array<std::byte, 1> bytes{std::byte{0}};
  const auto r = cc::importer::run_helper(CC_CRASHING_IMPORTER, "garbage", bytes);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.contains("JSON"));
}

TEST_CASE("the real importer reads hostile bytes inside its sandbox and answers a model") {
  std::array<std::byte, 300> junk{};
  for (std::size_t i = 0; i < junk.size(); ++i) {
    junk.at(i) = std::byte{static_cast<unsigned char>(i * 13U)};
  }
  const auto r = cc::importer::run_helper(CC_IMPORTER, "dwg", junk);
  INFO((r ? std::string() : r.error().message));
  REQUIRE(r.has_value());
  CHECK((*r)["report"]["error"].as_string().starts_with("not a DWG file"));
  const auto other = cc::importer::run_helper(CC_IMPORTER, "skp", junk);
  REQUIRE_FALSE(other.has_value());
  CHECK(other.error().code == cc::base::Errc::unsupported);
}

#endif

#if defined(__linux__) && defined(CC_CORPUS_DIR)
#include <cstdlib>
#include <filesystem>

#include "cc/base/file.hpp"
#include "cc/dwg/read.hpp"

TEST_CASE("a drawing read in the helper equals the drawing read in-process") {
  const std::filesystem::path f = std::filesystem::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dwg";
  if (!std::filesystem::exists(f)) {
    if (cc::base::env_var("CC_REQUIRE_CORPUS")) {
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("public fixtures not fetched (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  const auto bytes = cc::base::read_file(f, std::uint64_t{1} << 30U);
  REQUIRE(bytes.has_value());
  auto isolated = cc::importer::run_helper(CC_IMPORTER, "dwg", *bytes);
  INFO((isolated ? std::string() : isolated.error().message));
  REQUIRE(isolated.has_value());
  cc::base::Value direct = cc::dwg::read_dwg(*bytes);
  isolated->find("report")->erase("ms");
  direct.find("report")->erase("ms");
  CHECK(cc::base::to_json(*isolated) == cc::base::to_json(direct));
}
#endif
