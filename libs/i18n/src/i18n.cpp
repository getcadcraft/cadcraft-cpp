// SPDX-License-Identifier: Apache-2.0
#include "cc/i18n/i18n.hpp"

#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "cc/base/error.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"

namespace cc::i18n {

namespace {

bool name_char(char c, bool first) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/// Intl.PluralRules for the catalogs CadCraft ships (en, es-MX): 1 is "one", everything else "other".
std::string_view plural_category(double n) {
  return n == 1 ? "one" : "other";
}

}  // namespace

std::string fill(std::string_view text, std::initializer_list<std::pair<std::string_view, std::string>> fills) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text.substr(i, 2) == "{{") {
      out += '{';
      i += 2;
      continue;
    }
    if (text.substr(i, 2) == "}}") {
      out += '}';
      i += 2;
      continue;
    }
    if (text[i] == '{' && i + 1 < text.size() && name_char(text[i + 1], true)) {
      std::size_t j = i + 1;
      while (j < text.size() && name_char(text[j], false)) {
        ++j;
      }
      if (j < text.size() && text[j] == '}') {
        const std::string_view name = text.substr(i + 1, j - i - 1);
        bool done = false;
        for (const auto& [k, v] : fills) {
          if (k == name) {
            out += v;
            done = true;
            break;
          }
        }
        if (!done) {
          out.append(text.substr(i, j - i + 1));  // an unknown fill stays as written
        }
        i = j + 1;
        continue;
      }
    }
    out += text[i];
    ++i;
  }
  return out;
}

base::Result<Catalog> Catalog::parse(std::string locale, std::string_view json) {
  auto v = base::parse_json(json);
  if (!v) {
    return std::unexpected(v.error());
  }
  if (!v->is_object()) {
    return base::fail(base::Errc::invalid, "a language catalog is a JSON object");
  }
  Catalog c;
  c.locale_ = std::move(locale);
  c.strings_ = std::move(*v);
  return c;
}

const base::Value* Catalog::find(std::string_view key) const {
  return strings_.is_object() ? strings_.find(key) : nullptr;
}

bool Catalog::has(std::string_view key) const {
  return find(key) != nullptr;
}

// NOLINTNEXTLINE(misc-no-recursion) — into the English fallback: one level
std::string Catalog::text(std::string_view key, const double* n,
                          std::initializer_list<std::pair<std::string_view, std::string>> fills) const {
  const base::Value* v = find(key);
  if (v == nullptr) {
    return fallback_ != nullptr ? fallback_->text(key, n, fills) : std::string(key);
  }
  std::string s;
  if (v->is_string()) {
    s = v->as_string();
  } else if (v->is_object()) {
    const base::Value* form = nullptr;
    if (n != nullptr && std::isfinite(*n)) {
      std::string exact = "=";
      base::append_number(exact, *n);
      form = v->find(exact);
      if (form == nullptr || !form->is_string()) {
        form = v->find(plural_category(*n));
      }
    }
    if (form == nullptr || !form->is_string()) {
      form = v->find("other");
    }
    s = form != nullptr && form->is_string() ? form->as_string() : std::string(key);
  } else {
    s = std::string(key);
  }
  return fill(s, fills);
}

std::string Catalog::t(std::string_view key,
                       std::initializer_list<std::pair<std::string_view, std::string>> fills) const {
  return text(key, nullptr, fills);
}

std::string Catalog::tn(std::string_view key, double n,
                        std::initializer_list<std::pair<std::string_view, std::string>> fills) const {
  std::string ns;
  base::append_number(ns, n);
  // "{n}" is filled with n unless the caller gave it
  bool given = false;
  for (const auto& [k, v] : fills) {
    given = given || k == "n";
  }
  if (given) {
    return text(key, &n, fills);
  }
  const std::string with_n = text(key, &n, fills);
  return fill(with_n, {{"n", ns}});
}

}  // namespace cc::i18n
