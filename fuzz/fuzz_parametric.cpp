// SPDX-License-Identifier: Apache-2.0
// A file's constraints and parameters (gcons.js resolveParametric, params.js readParams, the expression parser
// under them): any text gives clean constraints or notes — never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 18)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;
  const std::string text(cc::base::as_text(std::as_bytes(std::span(data, size))));
  // three objects the payload may refer to by key
  std::vector<std::pair<std::string, std::pair<double, cc::base::Value>>> refs;
  refs.emplace_back(
      "1", std::pair{1.0, cc::base::make_object({{"type", "line"}, {"x1", 0}, {"y1", 0}, {"x2", 10}, {"y2", 0}})});
  refs.emplace_back("2", std::pair{2.0, cc::base::make_object({{"type", "circle"}, {"cx", 0}, {"cy", 0}, {"r", 5}})});
  cc::base::Array pts;
  pts.push_back(cc::base::make_object({{"x", 0}, {"y", 0}, {"b", 0.5}}));
  pts.push_back(cc::base::make_object({{"x", 5}, {"y", 5}}));
  refs.emplace_back(
      "3", std::pair{3.0, cc::base::make_object({{"type", "poly"}, {"pts", cc::base::Value(std::move(pts))}})});
  (void)cc::doc::gcons::resolve_parametric(cc::base::Value(text), refs, kWords);
  return 0;
}
