// SPDX-License-Identifier: Apache-2.0
// libs/cloud on hand-made files: text clouds (PTS count line, separators, skipped lines, columns), the LAS header's
// refusals, a tiny LAS 1.2 file read back, the keeper thinning evenly. Parity with the JS on real files is
// tools/parity/cloud.mjs (the public room fixtures).
#include "cc/cloud/cloud.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/i18n/i18n.hpp"

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
  std::vector<std::byte> b(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    b[i] = static_cast<std::byte>(s[i]);
  }
  return b;
}
void put(std::vector<std::byte>& b, std::size_t at, std::uint64_t v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    b.at(at + i) = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
  }
}
void put_f64(std::vector<std::byte>& b, std::size_t at, double d) {
  put(b, at, std::bit_cast<std::uint64_t>(d), 8);
}

}  // namespace

TEST_CASE("cloud: text clouds: a PTS count line, mixed separators, skipped lines, colours and intensity") {
  const cc::i18n::Catalog words;
  const auto b = bytes_of("3\n1,2,3,10,255,0,0\n4;5;6;20;0;255;0\nnot a point\n7 8 9 30 0 0 255\n");
  const auto info = cc::cloud::read_info("a.pts", b, words);
  REQUIRE(info);
  CHECK(info->kind == "xyz");
  CHECK(info->count == 3);
  CHECK_FALSE(info->estimate);
  CHECK((info->columns && info->columns->rgb == 4));
  const auto c = cc::cloud::load_cloud("a.pts", b, *info, 2e6, words);
  REQUIRE(c);
  CHECK(c->n == 3);
  CHECK(c->origin == cc::cloud::V3{1, 2, 3});
  CHECK((c->rgb && c->rgb->at(3 * 1 + 1) == 255));
  CHECK((c->inten && c->inten->at(2) == 30));
  REQUIRE(c->notes.size() == 1);  // the line that was not numbers
  CHECK(cc::cloud::slice_of(*c, 'z', 6, 1).size() == 1);
  CHECK(cc::cloud::PlanGrid(*c, nullptr).nearest(4.1, 5.1, 0.5) == 1);
}

TEST_CASE("cloud: not a cloud, an empty text cloud, LAS headers refused") {
  const cc::i18n::Catalog words;
  CHECK_FALSE(cc::cloud::read_info("a.txt", bytes_of("1 2 3"), words));
  CHECK_FALSE(cc::cloud::read_info("a.xyz", bytes_of("hello\nworld\n"), words));
  std::vector<std::byte> las(227);
  put(las, 0, 0x4653414CU, 4);                          // LASF
  CHECK_FALSE(cc::cloud::read_las_header(las, words));  // version 0.0
  las[24] = std::byte{1};
  las[25] = std::byte{2};
  CHECK_FALSE(cc::cloud::read_las_header(las, words));  // header size 0
}

TEST_CASE("cloud: a LAS 1.2 format 0 file read back, thinned evenly at a small budget") {
  const cc::i18n::Catalog words;
  constexpr std::size_t n = 3000;
  std::vector<std::byte> f(227 + n * 20);
  put(f, 0, 0x4653414CU, 4);
  f[24] = std::byte{1};
  f[25] = std::byte{2};
  put(f, 94, 227, 2);
  put(f, 96, 227, 4);
  put(f, 105, 20, 2);
  put(f, 107, n, 4);
  for (const std::size_t at : {131U, 139U, 147U}) {
    put_f64(f, at, 0.01);
  }
  for (std::size_t i = 0; i < n; ++i) {
    put(f, 227 + i * 20, i, 4);
    put(f, 227 + i * 20 + 15, i % 3, 1);
  }
  const auto info = cc::cloud::read_info("a.las", f, words);
  REQUIRE(info);
  CHECK(info->kind == "las");
  CHECK(info->count == n);
  const auto c = cc::cloud::load_cloud("a.las", f, *info, 1000, words);
  REQUIRE(c);
  CHECK(c->read == n);
  CHECK(c->n <= 1000);
  CHECK(c->class_counts.at(2) == n / 3);
  CHECK(cc::cloud::summary(*c, words).classes.size() == 3);
}

namespace {

std::vector<std::byte> read_seed(const char* name) {
  auto b = cc::base::read_file(std::filesystem::path(CC_CLOUD_SEEDS) / name, std::uint64_t{1} << 20U);
  return b ? std::move(*b) : std::vector<std::byte>{};
}

/// A LAZ opened by laz-perf: a cloud or a reason, never a trap (the hardened library) or a sanitizer report
void open_laz(const std::vector<std::byte>& f, const cc::i18n::Catalog& words) {
  const auto info = cc::cloud::read_info("t.laz", f, words);
  if (info) {
    const auto c = cc::cloud::load_cloud("t.laz", f, *info, 1000, words);
    CHECK((c || !c.error().message.empty()));  // a cloud, or a reason
  }
}

}  // namespace

TEST_CASE("cloud: LAZ through laz-perf, and damaged LAZ files refused or read, never a crash") {
  const cc::i18n::Catalog words;
  for (const char* name : {"tiny-f3.laz", "tiny-f7.laz"}) {
    const std::vector<std::byte> f = read_seed(name);
    REQUIRE(f.size() > 400);
    const auto info = cc::cloud::read_info(name, f, words);
    REQUIRE(info);
    CHECK(info->kind == "laz");
    const auto c = cc::cloud::load_cloud(name, f, *info, 2e6, words);
    REQUIRE(c);
    CHECK(c->n == 300);
    // the chunk table offset (the first 8 bytes of the points): before the file, past its end (fuzz, 2026-10-21)
    const auto point_offset = static_cast<std::size_t>(std::to_integer<unsigned>(f[96])) |
                              (static_cast<std::size_t>(std::to_integer<unsigned>(f[97])) << 8U);
    for (const std::uint64_t at : {~std::uint64_t{4}, std::uint64_t{1} << 40U, std::uint64_t{0}}) {
      std::vector<std::byte> g = f;
      put(g, point_offset, at, 8);
      open_laz(g, words);
    }
    // cut short anywhere, and bits flipped past the header (a fixed sequence)
    for (std::size_t cut = 300; cut < f.size(); cut += 97) {
      open_laz(std::vector<std::byte>(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(cut)), words);
    }
    std::uint32_t seed = 7;
    const auto next = [&seed] {
      seed = seed * 1103515245U + 12345U;
      return seed >> 8U;
    };
    for (int k = 0; k < 300; ++k) {
      std::vector<std::byte> g = f;
      for (int i = 0; i < 1 + k % 20; ++i) {
        g[point_offset + next() % (g.size() - point_offset)] = static_cast<std::byte>(next() & 0xFFU);
      }
      open_laz(g, words);
    }
  }
}

TEST_CASE("cloud: a damaged LAZ whose symbol is outside laz-perf's model table stops with a note") {
  // fuzz, 2026-10-21: decodeSymbol indexed the model's decoder table with a value past its interval, a heap read out of
  // bounds under ASan (point format 7+: the point14 field, the RGB field). cmake/LazPerf.cmake patch 8 throws there.
  const cc::i18n::Catalog words;
  const std::string stopped = words.t("las.laz.stopped", {{"read", "{read}"}, {"n", "{n}"}});
  const std::string lead = stopped.substr(0, stopped.find('{'));
  REQUIRE(!lead.empty());
  for (const char* name : {"regress-cb45fe695372", "regress-1f5fe2ccd8a4"}) {
    INFO(name);
    const std::vector<std::byte> f = read_seed(name);
    REQUIRE(f.size() > 400);
    const auto info = cc::cloud::read_info("f.las", f, words);
    REQUIRE(info);
    CHECK(info->kind == "laz");
    const auto c = cc::cloud::load_cloud("f.las", f, *info, 1000, words);
    REQUIRE(c);
    CHECK(std::ranges::any_of(c->notes, [&](const std::string& n) { return n.starts_with(lead); }));
  }
}
