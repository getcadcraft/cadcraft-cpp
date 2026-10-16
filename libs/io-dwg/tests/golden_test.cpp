// SPDX-License-Identifier: Apache-2.0
// The reader against the JS reader's output for the public fixtures (tests/golden/README.md).
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/dwg/read.hpp"

namespace fs = std::filesystem;
using cc::base::Value;

namespace {

// The kit's canon(): undefined dropped, NaN/Inf as "$NaN"/"$Inf"/"$-Inf", -0 → 0; then its diff with the DWG tolerance.
// NOLINTNEXTLINE(misc-no-recursion) — bounded by the model's depth
void compare(const Value& want, const Value& got, const std::string& path, std::vector<std::string>& out) {
  if (out.size() >= 20) {
    return;
  }
  if (got.is_number() && std::isnan(got.as_number()) && want.is_string() && want.as_string() == "$NaN") {
    return;
  }
  if (got.is_number() && std::isinf(got.as_number()) && want.is_string() &&
      want.as_string() == (got.as_number() > 0 ? "$Inf" : "$-Inf")) {
    return;
  }
  if (want.is_number() && got.is_number()) {
    const double a = want.as_number();
    const double b = got.as_number();
    if (a != b && !(std::abs(a - b) <= 1e-9 + 1e-12 * std::max(std::abs(a), std::abs(b)))) {
      out.push_back(path + ": " + std::to_string(a) + " vs " + std::to_string(b));
    }
    return;
  }
  if (want.kind() != got.kind()) {
    if (want.is_null() && got.is_undefined()) {
      return;  // an undefined array item is null in JSON
    }
    out.push_back(path + ": type differs");
    return;
  }
  if (want.is_array()) {
    const auto& a = want.as_array();
    const auto& b = got.as_array();
    if (a.size() != b.size()) {
      out.push_back(path + ".length: " + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    }
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
      compare(a[i], b[i], path + "[" + std::to_string(i) + "]", out);
    }
    return;
  }
  if (want.is_object()) {
    for (const auto& m : want.as_object()) {
      const Value* g = got.find(m.key);
      if (g == nullptr || g->is_undefined()) {
        out.push_back(path + "." + m.key + ": missing");
        continue;
      }
      compare(m.value, *g, path + "." + m.key, out);
    }
    for (const auto& m : got.as_object()) {
      if (!m.value.is_undefined() && !want.has(m.key)) {
        out.push_back(path + "." + m.key + ": extra");
      }
    }
    return;
  }
  if (!(want == got)) {
    out.push_back(path + ": value differs");
  }
}

}  // namespace

TEST_CASE("the reader matches the JS reader on the public fixtures") {
  const fs::path golden = fs::path(CC_GOLDEN_DIR) / "dwg";
  const fs::path corpus(CC_CORPUS_DIR);
  if (!fs::exists(golden) || !fs::exists(corpus / "fixtures")) {
    if (std::getenv("CC_REQUIRE_CORPUS") != nullptr) {  // NOLINT(concurrency-mt-unsafe)
      FAIL("CC_REQUIRE_CORPUS is set but the public fixtures are missing");
    }
    SKIP("references or fixtures missing (cmake -DDEST=build/corpus -P tools/corpus.cmake)");
  }
  int checked = 0;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const std::string name = entry.path().filename().string();  // kitchen-equipment.dwg.json
    const std::string dwg = name.substr(0, name.size() - 5);
    fs::path src = corpus / "fixtures/dwg" / dwg;
    if (!fs::exists(src)) {
      src = corpus / "fixtures/electrical" / dwg;
    }
    REQUIRE(fs::exists(src));
    const auto ref_bytes = cc::base::read_file(entry.path(), std::uint64_t{1} << 30U);
    REQUIRE(ref_bytes.has_value());
    const auto want = cc::base::parse_json(cc::base::as_text(*ref_bytes));
    REQUIRE(want.has_value());
    const auto bytes = cc::base::read_file(src, std::uint64_t{1} << 30U);
    REQUIRE(bytes.has_value());
    Value got = cc::dwg::read_dwg(*bytes);
    got.find("report")->erase("ms");
    std::vector<std::string> diffs;
    compare(*want, got, dwg, diffs);
    INFO(dwg);
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d);
    }
    CHECK(diffs.empty());
    ++checked;
  }
  CHECK(checked == 4);
}
