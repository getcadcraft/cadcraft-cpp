// SPDX-License-Identifier: Apache-2.0
#include "cc/base/checked.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("checked arithmetic refuses to wrap") {
  constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
  CHECK(cc::base::checked_add<std::uint32_t>(1, 2).value() == 3);
  CHECK_FALSE(cc::base::checked_add<std::uint32_t>(kMax, 1).has_value());
  CHECK(cc::base::checked_mul<std::uint64_t>(0, 7).value() == 0);
  CHECK_FALSE(cc::base::checked_mul<std::uint32_t>(65536, 65536).has_value());
}

TEST_CASE("narrow and to_int keep values that fit and refuse the rest") {
  CHECK(cc::base::narrow<std::uint8_t>(255).value() == 255);
  CHECK_FALSE(cc::base::narrow<std::uint8_t>(256).has_value());
  CHECK_FALSE(cc::base::narrow<std::uint32_t>(-1).has_value());
  CHECK(cc::base::to_int<std::int32_t>(-3.9).value() == -3);
  CHECK_FALSE(cc::base::to_int<std::int32_t>(std::nan("")).has_value());
  CHECK_FALSE(cc::base::to_int<std::int32_t>(2147483648.0).has_value());
  CHECK(cc::base::to_int<std::int32_t>(2147483647.0).value() == 2147483647);
  CHECK_FALSE(cc::base::to_int<std::int64_t>(9.3e18).has_value());
  CHECK_FALSE(cc::base::to_int<std::uint8_t>(-1.0).has_value());
}
