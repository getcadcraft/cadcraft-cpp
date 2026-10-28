// SPDX-License-Identifier: Apache-2.0
#include "cc/base/random.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("base.random: bytes from the OS, never the same twice") {
  std::array<std::uint8_t, 64> a{};
  std::array<std::uint8_t, 64> b{};
  cc::base::os_random(a);
  cc::base::os_random(b);
  CHECK(std::ranges::any_of(a, [](std::uint8_t x) { return x != 0; }));
  CHECK(a != b);
  std::array<std::uint8_t, 1000> big{};
  cc::base::os_random(big);  // more than getentropy's 256 per call
  CHECK(std::ranges::count(big, std::uint8_t{0}) < 50);
}
