// SPDX-License-Identifier: Apache-2.0
// cc::i18n — L1. CadCraft 0.5's words: the lang/<locale>.json catalogs, keys unchanged (CPP-AGENTS.md section 15),
// looked up as the JS i18n.js t() does: "{name}" fills ("{{" and "}}" for braces; an unknown fill stays as written),
// plural forms {"=0", "one", "other"} chosen by `n`, and the key itself when a catalog lacks it.
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::i18n {

class Catalog {
 public:
  Catalog() = default;
  /// A catalog file's text (JSON: key → string or plural forms). `fallback` (English) answers the keys it lacks.
  [[nodiscard]] static base::Result<Catalog> parse(std::string locale, std::string_view json);

  [[nodiscard]] const std::string& locale() const noexcept { return locale_; }
  [[nodiscard]] bool has(std::string_view key) const;

  /// The words for `key` with its fills; `n` picks the plural form (and fills "{n}" when not given in `fills`).
  [[nodiscard]] std::string t(std::string_view key,
                              std::initializer_list<std::pair<std::string_view, std::string>> fills = {}) const;
  [[nodiscard]] std::string tn(std::string_view key, double n,
                               std::initializer_list<std::pair<std::string_view, std::string>> fills = {}) const;

  void set_fallback(const Catalog* english) noexcept { fallback_ = english; }

 private:
  std::string locale_;
  base::Value strings_;
  const Catalog* fallback_ = nullptr;
  [[nodiscard]] const base::Value* find(std::string_view key) const;
  [[nodiscard]] std::string text(std::string_view key, const double* n,
                                 std::initializer_list<std::pair<std::string_view, std::string>> fills) const;
};

/// i18n.js fill(): "{name}" → its value; "{{" → "{"; "}}" → "}".
[[nodiscard]] std::string fill(std::string_view text,
                               std::initializer_list<std::pair<std::string_view, std::string>> fills);

}  // namespace cc::i18n
