// SPDX-License-Identifier: Apache-2.0
#include "cc/base/file.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

TEST_CASE("an atomic write replaces the file and a capped read refuses large files") {
  const fs::path dir = fs::temp_directory_path() / ("cc-base-test-" + std::to_string(std::random_device{}()));
  fs::create_directories(dir);
  const fs::path f = dir / "a.bin";
  const std::array<std::byte, 3> one{std::byte{1}, std::byte{2}, std::byte{3}};
  REQUIRE(cc::base::write_file_atomic(f, one).has_value());
  const std::array<std::byte, 2> two{std::byte{9}, std::byte{8}};
  REQUIRE(cc::base::write_file_atomic(f, two).has_value());
  const auto back = cc::base::read_file(f, 16);
  REQUIRE(back.has_value());
  CHECK(back->size() == 2);
  CHECK(back->at(0) == std::byte{9});
  const auto capped = cc::base::read_file(f, 1);
  REQUIRE_FALSE(capped.has_value());
  CHECK(capped.error().code == cc::base::Errc::too_big);
  CHECK_FALSE(cc::base::read_file(dir / "missing", 16).has_value());
  std::size_t entries = 0;
  for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) {
    ++entries;
  }
  CHECK(entries == 1);  // no temporary file left behind
  fs::remove_all(dir);
}
