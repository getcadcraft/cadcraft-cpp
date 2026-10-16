// SPDX-License-Identifier: Apache-2.0
// Ported from cc-dwg tools/dwg-test/test.mjs (sections bits, crc, read): the ODA spec v5.4.1 worked examples
// (ch. 2), the CRC tables and check value (2.14), and "readDwg never crashes".
#include "bits.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/dwg/read.hpp"
#include "container.hpp"

namespace {

std::vector<std::uint8_t> bits_to_bytes(std::string_view s) {
  std::vector<std::uint8_t> b((s.size() + 7) / 8, 0);
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '1') {
      b[i >> 3U] = static_cast<std::uint8_t>(b[i >> 3U] | (0x80U >> (i & 7U)));
    }
  }
  return b;
}
std::string bit_str(std::span<const std::uint8_t> b, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += ((b[i >> 3U] >> (7U - (i & 7U))) & 1U) != 0 ? '1' : '0';
  }
  return s;
}

}  // namespace

TEST_CASE("bit codes: the worked examples of the spec, chapter 2") {
  const cc::dwg::Codepage cp = cc::dwg::codepage_for(30);
  {
    const auto b = bits_to_bytes("0000000001000000011011010000111110");
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.BS() == 257);
    CHECK(r.BS() == 0);
    CHECK(r.BS() == 256);
    CHECK(r.BS() == 15);
    CHECK(r.BS() == 0);
  }
  {
    const auto b = bits_to_bytes("000000000100000001000000000000000010010000111110");
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.BL() == 257);
    CHECK(r.BL() == 0);
    CHECK(r.BL() == 15);
    CHECK(r.BL() == 0);
  }
  {
    const auto d = std::bit_cast<std::array<std::uint8_t, 8>>(2.5);
    const auto b = bits_to_bytes(
        "0110"
        "00" +
        bit_str(d, 64));
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.BD() == 1.0);
    CHECK(r.BD() == 0.0);
    CHECK(r.BD() == 2.5);
  }
  {
    const auto b = bits_to_bytes(
        "0"
        "10"
        "110"
        "111");
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.B3() == 0);
    CHECK(r.B3() == 2);
    CHECK(r.B3() == 6);
    CHECK(r.B3() == 7);
  }
  {
    const auto b = bits_to_bytes("010100100000010111100111");  // handle 5.2.05E7 (2.13)
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.H(0) == 0x5E7);
    CHECK(r.hcode == 5);
  }
  {
    const auto b = bits_to_bytes(
        "01000000"
        "11100000");  // the 2.14.1 object "01 00000011 100000"
    cc::dwg::Bits r(b, 15, &cp);
    CHECK(r.BS() == 3);
  }
  {
    // A bad BITLONG code 11 is an error, not a value.
    const auto b = bits_to_bytes("11");
    cc::dwg::Bits r(b, 15, &cp);
    CHECK_THROWS_AS(r.BL(), cc::dwg::DecodeError);
  }
}

TEST_CASE("modular chars and shorts (2.6, 2.7)") {
  auto mc = [](std::vector<std::uint8_t> v) {
    std::size_t p = 0;
    const double r = cc::dwg::read_mc(v, p);
    return std::pair{r, p};
  };
  CHECK(mc({0x82, 0x24}) == std::pair{4610.0, std::size_t{2}});
  CHECK(mc({233, 151, 230, 53}) == std::pair{112823273.0, std::size_t{4}});
  CHECK(mc({0x85, 0x4B}) == std::pair{-1413.0, std::size_t{2}});
  std::size_t p = 0;
  const std::vector<std::uint8_t> ms{49, 244, 141, 0};
  CHECK(cc::dwg::read_ms(ms, p) == 4650033.0);
  std::size_t q = 0;
  const std::vector<std::uint8_t> cut{0x82};
  CHECK_THROWS_AS(cc::dwg::read_mc(cut, q), cc::dwg::DecodeError);
}

TEST_CASE("CRCs (2.14): the table, the CRC-32 check value") {
  const std::array<std::uint8_t, 1> one{1};
  CHECK(cc::dwg::crc8(0, one) == 0xC0C1);  // table[1] = 0xC0C1
  const std::array<std::uint8_t, 1> ff{0xFF};
  CHECK(cc::dwg::crc8(0, ff) == 0x4040);  // table[255] = 0x4040
  const std::string_view check = "123456789";
  std::vector<std::uint8_t> b(check.begin(), check.end());
  CHECK(cc::dwg::crc32(b) == 0xCBF43926U);
}

TEST_CASE("read_dwg never crashes on damaged or foreign input") {
  auto read = [](std::span<const std::uint8_t> b) { return cc::dwg::read_dwg(std::as_bytes(b)); };
  const std::string_view fake = "AC1027fake";
  std::vector<std::uint8_t> a(fake.begin(), fake.end());
  CHECK(!read(a)["report"]["notes"].as_array().empty());
  std::vector<std::uint8_t> r12(4096, 0);
  std::memcpy(r12.data(), "AC1009", 6);
  const cc::base::Value m12 = read(r12);
  CHECK(m12["report"]["error"].as_string().starts_with("DWG version AC1009 is not supported"));
  const std::string_view text = "hello world, this is not a drawing";
  std::vector<std::uint8_t> t(text.begin(), text.end());
  CHECK(read(t)["report"]["error"].as_string() == "too short to be a DWG file (34 bytes)");
  std::vector<std::uint8_t> big(200, 'x');
  CHECK(read(big)["report"]["error"].as_string() == "not a DWG file (it starts \"xxxxxx\")");
  const std::filesystem::path f = std::filesystem::path(CC_CORPUS_DIR) / "fixtures/dwg/kitchen-equipment.dwg";
  if (std::filesystem::exists(f)) {
    const auto bytes = cc::base::read_file(f, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    for (const std::size_t n : {std::size_t{100}, std::size_t{4096}, bytes->size() / 2, bytes->size() - 1}) {
      const cc::base::Value m = cc::dwg::read_dwg(std::span(*bytes).first(n));
      CHECK(m.is_object());
      if (n <= 4096) {
        CHECK(!m["report"]["notes"].as_array().empty());  // a cut-off file says so
      }
    }
  }
}
