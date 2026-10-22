// SPDX-License-Identifier: Apache-2.0
// The part call scripts against what the frozen CadCraft 0.5 modules answered (tests/golden/js-0.5-part.tar.gz, made
// by tools/parity/part.mjs --golden-out): every script replayed through run_script, each call compared with the kit's
// canon at the DWG tolerance (abs 1e-9 + rel 1e-12).
#include "golden.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/partcraft/script.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

Value read_json(const fs::path& p) {
  const auto bytes = cc::base::read_file(p, std::uint64_t{1} << 30U);
  REQUIRE(bytes.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*bytes));
  REQUIRE(v.has_value());
  return std::move(*v);
}

}  // namespace

TEST_CASE("golden: the part call scripts answer what CadCraft 0.5's modules answered") {
  const fs::path dir = fs::path(CC_GOLDEN_DIR) / "part";
  if (!fs::exists(dir)) {
    SKIP("references missing (tests/golden/js-0.5-part.tar.gz)");
  }
  std::size_t calls = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const Value list = read_json(entry.path());
    for (const Value& item : list.as_array()) {
      const Value want = item.get("js");
      // our answer through JSON, so NaN and ±Infinity take the canonical "$NaN" / "$Inf" form as the reference does
      auto got = cc::base::parse_json(cc::base::to_json(cc::partcraft::run_script(item.get("script"))));
      REQUIRE(got.has_value());
      std::vector<std::string> diffs;
      cc::test::compare(want, *got, entry.path().filename().string(), diffs);
      const std::string first = diffs.empty() ? std::string() : diffs.front();
      INFO(first);
      CHECK(diffs.empty());
      calls += want.as_array().size();
    }
  }
  CHECK(calls > 0);
}
