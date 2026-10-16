// SPDX-License-Identifier: Apache-2.0
#include "cc/base/reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

template <std::size_t N>
std::array<std::byte, N> bytes(const std::array<std::uint8_t, N>& in) {
  std::array<std::byte, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out.at(i) = std::byte{in.at(i)};
  }
  return out;
}

}  // namespace

TEST_CASE("ByteReader reads little-endian fields and refuses to read past the end") {
  const auto data = bytes<std::size_t{11}>({0x01, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xFF, 0xFF, 0xAA, 0xBB});
  cc::base::ByteReader r{std::span(data)};
  CHECK(r.read_u8().value() == 0x01);
  CHECK(r.read_u16_le().value() == 0x1234);
  CHECK(r.read_u32_le().value() == 0x12345678U);
  CHECK(r.read_i16_le().value() == -1);
  CHECK(r.remaining() == 2);
  const auto big = r.read_u32_le();
  REQUIRE_FALSE(big.has_value());
  CHECK(big.error().code == cc::base::Errc::out_of_range);
  CHECK(r.pos() == 9);  // a failed read does not move the cursor
  CHECK(r.bytes(2).value().size() == 2);
  CHECK_FALSE(r.skip(1).has_value());
  CHECK_FALSE(r.seek(12).has_value());
  CHECK(r.seek(11).has_value());
}

TEST_CASE("BitReader reads bits MSB first, bytes at any position, and throws past the end") {
  const auto data = bytes<std::size_t{3}>({0b1011'0001, 0b0100'0000, 0xFF});
  cc::base::BitReader r{std::span(data)};
  CHECK(r.bit() == 1);
  CHECK(r.bits2() == 1);  // 01
  CHECK(r.bits(3) == 0b100);
  CHECK(r.rc() == 0b0101'0000);  // bits 6..13: 01 + 010000
  CHECK(r.pos() == 14);
  CHECK(r.bits_left() == 10);
  std::vector<std::uint8_t> out;
  CHECK_THROWS_AS(r.raw(2, out), cc::base::ReadOverrun);
  r.set_pos(16);
  r.raw(1, out);
  CHECK(out.at(0) == 0xFF);
  CHECK_THROWS_AS(r.bit(), cc::base::ReadOverrun);
}

TEST_CASE("BitReader never reads beyond its span even when asked to") {
  const auto data = bytes<std::size_t{1}>({0xAA});
  cc::base::BitReader r{std::span(data), 0, 1000};
  CHECK(r.end() == 8);
  CHECK(r.rc() == 0xAA);
  CHECK_THROWS_AS(r.bits2(), cc::base::ReadOverrun);
}
