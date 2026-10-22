// SPDX-License-Identifier: Apache-2.0
// wild.js: AutoLISP's wildcard patterns (wcmatch; ssget filters). The JS turns each comma-separated part into a
// RegExp; this matches the same strings by hand, over UTF-16 units as the RegExp does (no Unicode flag), and refuses
// the same patterns with V8's words: a [z-a] class is "Invalid regular expression: /^(?:[z-a])$/: Range out of order
// in character class", the source shown being the one the JS builds.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/lisp/lisp.hpp"
#include "internal.hpp"

namespace cc::lisp {

namespace detail {

namespace {

bool line_terminator(char16_t c) {
  return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029;
}

/// RegExp's Canonicalize for /i without /u: the upper case of a unit when it is one unit, not ASCII from non-ASCII.
char16_t canonical(char16_t c) {
  if (c < 128) {
    return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - 32) : c;
  }
  const std::u16string up = u16(base::js_to_upper(u8(std::u16string(1, c))));
  if (up.size() != 1 || up[0] < 128) {
    return c;
  }
  return up[0];
}

bool element_matches(const GlobEl& e, char16_t c, bool icase) {
  switch (e.k) {
    case GlobEl::K::lit: return icase ? canonical(c) == canonical(e.c) : c == e.c;
    case GlobEl::K::any:
    case GlobEl::K::star: return !e.nolt || !line_terminator(c);
    case GlobEl::K::cls: {
      bool in = false;
      for (const auto& [lo, hi] : e.ranges) {
        if (c >= lo && c <= hi) {
          in = true;
          break;
        }
      }
      return in != e.inv;
    }
  }
  return false;
}

/// escRe: a character as the JS escapes it in the RegExp source
void esc_re(std::u16string& src, char16_t c) {
  if (std::u16string_view(u".*+?^${}()|[]\\/-").contains(c)) {
    src += u'\\';
  }
  src += c;
}

struct Part {
  bool neg = false;
  std::vector<GlobEl> els;
};

GlobEl cls(std::vector<std::pair<char16_t, char16_t>> r, bool inv) {
  return GlobEl{.k = GlobEl::K::cls, .c = 0, .ranges = std::move(r), .inv = inv, .nolt = false};
}

/// One part ("~" first = not) → elements, and the RegExp source the JS gives it (for its error message).
Part part_of(std::u16string_view s) {
  Part p;
  if (!s.empty() && s[0] == u'~') {
    p.neg = true;
    s.remove_prefix(1);
  }
  std::u16string src;
  const auto lit = [&](char16_t c) {
    esc_re(src, c);
    p.els.push_back(GlobEl{.k = GlobEl::K::lit, .c = c, .ranges = {}, .inv = false, .nolt = false});
  };
  std::vector<std::pair<char16_t, char16_t>> bad_range;
  bool bad = false;
  for (std::size_t k = 0; k < s.size(); ++k) {
    const char16_t c = s[k];
    if (c == u'`' && k + 1 < s.size()) {
      lit(s[++k]);
      continue;
    }
    if (c == u'#') {
      src += u"[0-9]";
      p.els.push_back(cls({{u'0', u'9'}}, false));
    } else if (c == u'@') {
      src += u"[A-Za-z]";
      p.els.push_back(cls({{u'A', u'Z'}, {u'a', u'z'}}, false));
    } else if (c == u'.') {
      src += u"[^A-Za-z0-9]";
      p.els.push_back(cls({{u'A', u'Z'}, {u'a', u'z'}, {u'0', u'9'}}, true));
    } else if (c == u'*') {
      src += u"[\\s\\S]*";
      p.els.push_back(GlobEl{.k = GlobEl::K::star, .c = 0, .ranges = {}, .inv = false, .nolt = false});
    } else if (c == u'?') {
      src += u"[\\s\\S]";
      p.els.push_back(GlobEl{.k = GlobEl::K::any, .c = 0, .ranges = {}, .inv = false, .nolt = false});
    } else if (c == u'[') {
      const std::size_t end = k + 2 <= s.size() ? s.find(u']', k + 2) : std::u16string_view::npos;
      if (end == std::u16string_view::npos) {
        src += u"\\[";
        p.els.push_back(GlobEl{.k = GlobEl::K::lit, .c = u'[', .ranges = {}, .inv = false, .nolt = false});
        continue;
      }
      std::u16string_view set = s.substr(k + 1, end - k - 1);
      bool inv = false;
      if (!set.empty() && set[0] == u'~') {
        inv = true;
        set.remove_prefix(1);
      }
      src += u'[';
      if (inv) {
        src += u'^';
      }
      for (const char16_t x : set) {
        if (x == u'\\' || x == u']' || x == u'^') {
          src += u'\\';
        }
        src += x;
      }
      src += u']';
      // ClassRanges, left to right: a - b is a range when an atom follows the dash
      std::vector<std::pair<char16_t, char16_t>> ranges;
      for (std::size_t i = 0; i < set.size();) {
        if (i + 2 < set.size() && set[i + 1] == u'-') {
          if (set[i] > set[i + 2]) {
            bad = true;
          }
          ranges.emplace_back(set[i], set[i + 2]);
          i += 3;
        } else {
          ranges.emplace_back(set[i], set[i]);
          ++i;
        }
      }
      p.els.push_back(cls(std::move(ranges), inv));
      k = end;
    } else {
      lit(c);
    }
  }
  if (bad) {
    throw LispError("Invalid regular expression: /^(?:" + u8(src) + ")$/: Range out of order in character class");
  }
  return p;
}

}  // namespace

bool glob_match(const std::vector<GlobEl>& p, std::u16string_view s, bool icase) {
  if (std::ranges::none_of(p, [](const GlobEl& e) { return e.k == GlobEl::K::star && e.nolt; })) {
    // every star takes any run: the last star seen absorbs what a mismatch needs (linear on most patterns)
    std::size_t pi = 0;
    std::size_t si = 0;
    std::size_t star = p.size();
    std::size_t mark = 0;
    while (si < s.size()) {
      if (pi < p.size() && p[pi].k != GlobEl::K::star && element_matches(p[pi], s[si], icase)) {
        ++pi;
        ++si;
      } else if (pi < p.size() && p[pi].k == GlobEl::K::star) {
        star = pi++;
        mark = si;
      } else if (star != p.size()) {
        pi = star + 1;
        si = ++mark;
      } else {
        return false;
      }
    }
    while (pi < p.size() && p[pi].k == GlobEl::K::star) {
      ++pi;
    }
    return pi == p.size();
  }
  // a star that stops at a line terminator: one row per element over the text's units: row[j] = the elements so far
  // match s[0, j)
  std::vector<char> row(s.size() + 1, 0);
  std::vector<char> next(s.size() + 1, 0);
  row[0] = 1;
  for (const GlobEl& e : p) {
    if (e.k == GlobEl::K::star) {
      next[0] = row[0];
      for (std::size_t j = 1; j <= s.size(); ++j) {
        next[j] = static_cast<char>(row[j] != 0 || (next[j - 1] != 0 && element_matches(e, s[j - 1], icase)));
      }
    } else {
      next[0] = 0;
      for (std::size_t j = 1; j <= s.size(); ++j) {
        next[j] = static_cast<char>(row[j - 1] != 0 && element_matches(e, s[j - 1], icase));
      }
    }
    std::swap(row, next);
  }
  return row[s.size()] != 0;
}

bool wcmatch_or_throw(std::string_view s, std::string_view pattern, Interp* L) {
  const std::u16string p = u16(pattern);
  std::vector<std::u16string> parts;
  std::u16string cur;
  for (std::size_t i = 0; i < p.size();) {
    const char16_t c = p[i];
    if (c == u'`' && i + 1 < p.size()) {
      cur += c;
      cur += p[i + 1];
      i += 2;
      continue;
    }
    if (c == u',') {
      parts.push_back(std::move(cur));
      cur.clear();
      ++i;
      continue;
    }
    cur += c;
    ++i;
  }
  parts.push_back(std::move(cur));
  std::vector<Part> alts;
  alts.reserve(parts.size());
  for (const std::u16string& part : parts) {
    alts.push_back(part_of(part));  // every part's RegExp is made before any is tried
  }
  const std::u16string text = u16(s);
  if (L != nullptr) {  // the matching's worst case, as steps (Options::max_steps)
    L->charge((text.size() + 1) * (p.size() + 1) / 64);
  }
  return std::ranges::any_of(alts, [&](const Part& a) { return glob_match(a.els, text, false) != a.neg; });
}

}  // namespace detail

base::Result<bool> wcmatch(std::string_view s, std::string_view pattern) {
  try {
    return detail::wcmatch_or_throw(s, pattern, nullptr);
  } catch (const LispError& e) {
    return base::fail(base::Errc::invalid, e.message());
  }
}

}  // namespace cc::lisp
