// SPDX-License-Identifier: Apache-2.0
// lib/skp util.js and messages.js, ported. See common.hpp.
#include "common.hpp"

#include <algorithm>
#include <array>
#include <bit>

#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/i18n/i18n.hpp"

namespace cc::skp::detail {

namespace {

struct Message {
  std::string_view key;
  bool plural;
  std::string_view one;
  std::string_view other;
};
#include "messages.inc"

const Message* find_message(std::string_view key) {
  for (const Message& m : kMessages) {
    if (m.key == key) {
      return &m;
    }
  }
  return nullptr;
}

template <class F>
std::string format(std::string_view key, const F& lookup, const i18n::Catalog* cat, bool has_n, std::string_view n) {
  (void)cat;
  const Message* m = find_message(key);
  if (m == nullptr) {
    return std::string(key);
  }
  std::string_view s = m->other;
  if (m->plural) {
    s = (has_n && n == "1") ? m->one : m->other;
  }
  // FILL = /\{\{|\}\}|\{([A-Za-z_][A-Za-z0-9_]*)\}/g
  std::string out;
  out.reserve(s.size() + 16);
  for (std::size_t i = 0; i < s.size();) {
    if (s.substr(i, 2) == "{{") {
      out += '{';
      i += 2;
      continue;
    }
    if (s.substr(i, 2) == "}}") {
      out += '}';
      i += 2;
      continue;
    }
    if (s[i] == '{') {
      std::size_t j = i + 1;
      const auto word = [](char c, bool first) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (!first && c >= '0' && c <= '9');
      };
      if (j < s.size() && word(s[j], true)) {
        ++j;
        while (j < s.size() && word(s[j], false)) {
          ++j;
        }
        if (j < s.size() && s[j] == '}') {
          const std::string_view name = s.substr(i + 1, j - i - 1);
          if (const std::string* v = lookup(name)) {
            out += *v;
          } else {
            out += s.substr(i, j - i + 1);
          }
          i = j + 1;
          continue;
        }
      }
    }
    out += s[i];
    ++i;
  }
  return out;
}

}  // namespace

std::string msg(const i18n::Catalog* cat, std::string_view key, Fills fills) {
  std::string_view n;
  bool has_n = false;
  for (const auto& [k, v] : fills) {
    if (k == "n") {
      n = v;
      has_n = true;
    }
  }
  if (cat != nullptr && cat->has(key)) {
    return has_n ? cat->tn(key, base::js_number(n), fills) : cat->t(key, fills);
  }
  const auto lookup = [&fills](std::string_view name) -> const std::string* {
    for (const auto& [k, v] : fills) {
      if (k == name) {
        return &v;
      }
    }
    return nullptr;
  };
  return format(key, lookup, cat, has_n, n);
}

std::string msg_v(const i18n::Catalog* cat, std::string_view key,
                  const std::vector<std::pair<std::string, std::string>>& fills) {
  std::string_view n;
  bool has_n = false;
  for (const auto& [k, v] : fills) {
    if (k == "n") {
      n = v;
      has_n = true;
    }
  }
  if (cat != nullptr && cat->has(key)) {
    // the catalog takes an initializer list: at most the fills cc-skp's notes use (n and one more)
    const std::string* other_name = nullptr;
    const std::string* other_value = nullptr;
    for (const auto& [k, v] : fills) {
      if (k != "n") {
        other_name = &k;
        other_value = &v;
      }
    }
    if (other_name != nullptr) {
      return has_n ? cat->tn(key, base::js_number(n), {{*other_name, *other_value}, {"n", std::string(n)}})
                   : cat->t(key, {{*other_name, *other_value}});
    }
    return has_n ? cat->tn(key, base::js_number(n), {{"n", std::string(n)}}) : cat->t(key);
  }
  const auto lookup = [&fills](std::string_view name) -> const std::string* {
    for (const auto& [k, v] : fills) {
      if (k == name) {
        return &v;
      }
    }
    return nullptr;
  };
  return format(key, lookup, cat, has_n, n);
}

void fail(std::string code, std::string message) {
  throw Fail{Error{std::move(code), std::move(message)}};
}

void damaged(const i18n::Catalog* cat, std::string_view key, Fills fills) {
  fail("damaged", msg(cat, "ccskp.damaged", {{"reason", msg(cat, key, fills)}}));
}

std::string num_text(double x) {
  std::string s;
  base::append_number(s, x);
  return s;
}

std::string grouped(std::uint64_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------- Reader
double Reader::f64_at(std::span<const std::uint8_t> b, std::size_t p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[p + i]) << (8U * i);
  }
  return std::bit_cast<double>(v);
}

std::string Reader::utf16(std::size_t n) {
  need(n * 2);
  std::vector<std::uint16_t> units(n);
  for (std::size_t i = 0; i < n; ++i) {
    units[i] = static_cast<std::uint16_t>(b_[pos + i * 2] | (b_[pos + i * 2 + 1] << 8U));
  }
  pos += n * 2;
  std::string s;
  s.reserve(n);
  base::append_utf16(s, units);
  return s;
}

std::string Reader::ascii(std::size_t n) {
  return latin1(take(n));
}

// ---------------------------------------------------------------------------------------------------- Notes
void Notes::add(std::string_view key, std::uint64_t n, std::vector<std::pair<std::string, std::string>> fills) {
  if (n == 0) {
    return;
  }
  for (Entry& e : entries_) {
    if (e.key == key && e.fills == fills) {
      e.n += n;
      return;
    }
  }
  entries_.push_back(Entry{std::string(key), std::move(fills), n});
}

std::vector<std::string> Notes::list(const i18n::Catalog* cat) const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const Entry& e : entries_) {
    auto fills = e.fills;  // {...e.fills, n: e.n}
    fills.emplace_back("n", std::to_string(e.n));
    out.push_back(msg_v(cat, e.key, fills));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------- text
namespace {

/// The length of the well-formed UTF-8 sequence at b[i], or 0 (and `sub` = the ill-formed subpart's length, ≥ 1).
std::size_t utf8_seq(std::span<const std::uint8_t> b, std::size_t i, std::size_t& sub) noexcept {
  const std::uint8_t c = b[i];
  sub = 1;
  if (c < 0x80) {
    return 1;
  }
  std::size_t need = 0;
  std::uint8_t lo = 0x80;
  std::uint8_t hi = 0xbf;
  if (c >= 0xc2 && c <= 0xdf) {
    need = 1;
  } else if (c >= 0xe0 && c <= 0xef) {
    need = 2;
    if (c == 0xe0) {
      lo = 0xa0;
    }
    if (c == 0xed) {
      hi = 0x9f;
    }
  } else if (c >= 0xf0 && c <= 0xf4) {
    need = 3;
    if (c == 0xf0) {
      lo = 0x90;
    }
    if (c == 0xf4) {
      hi = 0x8f;
    }
  } else {
    return 0;
  }
  std::size_t j = 1;
  for (; j <= need; ++j) {
    if (i + j >= b.size()) {
      break;
    }
    const std::uint8_t x = b[i + j];
    if (x < (j == 1 ? lo : 0x80) || x > (j == 1 ? hi : 0xbf)) {
      break;
    }
  }
  if (j <= need) {
    sub = j;
    return 0;
  }
  return need + 1;
}

}  // namespace

bool utf8_valid(std::span<const std::uint8_t> b) noexcept {
  std::size_t sub = 0;
  for (std::size_t i = 0; i < b.size();) {
    const std::size_t n = utf8_seq(b, i, sub);
    if (n == 0) {
      return false;
    }
    i += n;
  }
  return true;
}

std::string utf8_lossy(std::span<const std::uint8_t> b) {
  std::string out;
  out.reserve(b.size());
  std::size_t i = 0;
  if (b.size() >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
    i = 3;
  }
  std::size_t sub = 0;
  while (i < b.size()) {
    const std::size_t n = utf8_seq(b, i, sub);
    if (n == 0) {
      out += "\xef\xbf\xbd";
      i += sub;
      continue;
    }
    for (std::size_t k = 0; k < n; ++k) {
      out += static_cast<char>(b[i + k]);
    }
    i += n;
  }
  return out;
}

std::string latin1(std::span<const std::uint8_t> b) {
  std::string s;
  s.reserve(b.size());
  for (const std::uint8_t c : b) {
    base::append_code_point(s, c);
  }
  return s;
}

}  // namespace cc::skp::detail
