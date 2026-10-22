// SPDX-License-Identifier: Apache-2.0
// points.js (CivilCraft 0.5), ported function by function.
#include "cc/civil/points.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "cc/appkit/jsops.hpp"
#include "cc/appkit/lasso.hpp"
#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/civil/units.hpp"

namespace cc::civil {
namespace {

constexpr std::array<std::string_view, 8> kFormats{"PNEZD", "PENZD", "PNEZ", "PENZ", "NEZD", "ENZD", "NEZ", "ENZ"};
constexpr std::size_t kMaxBad = 200;

bool is_format(std::string_view f) {
  return std::ranges::find(kFormats, f) != kFormats.end();
}

/// s.slice(0, n) in UTF-16 units (a pair cut in two leaves its first half, as in JS).
std::string slice16(std::string_view s, std::size_t n) {
  if (s.size() <= n) {
    return std::string(s);  // n UTF-8 bytes hold at most n units
  }
  const auto u = base::to_utf16(s);
  if (u.size() <= n) {
    return std::string(s);
  }
  std::string out;
  base::append_utf16(out, std::span(u).first(n));
  return out;
}

/// split on runs of JS white space (s.split(/\s+/)); s is trimmed by the callers, so no empty ends unless s is empty.
std::vector<std::string> split_ws(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t n = base::js_space_at(s, i);
    if (n != 0) {
      out.push_back(std::move(cur));
      cur.clear();
      i += n;
      while (i < s.size()) {
        const std::size_t m = base::js_space_at(s, i);
        if (m == 0) {
          break;
        }
        i += m;
      }
      continue;
    }
    cur.push_back(s[i]);
    ++i;
  }
  out.push_back(std::move(cur));
  return out;
}

std::string clean_text(std::string_view s) {
  std::string t;
  t.reserve(s.size());
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    t.push_back(u < 0x20 || u == 0x7F ? ' ' : c);
  }
  return slice16(base::js_trim(t), 200);
}

/// toLowerCase and drop [\s._()-]
std::string header_key(std::string_view c) {
  const std::string lower = base::js_to_lower(c);
  std::string k;
  for (std::size_t i = 0; i < lower.size();) {
    const std::size_t n = base::js_space_at(lower, i);
    if (n != 0) {
      i += n;
      continue;
    }
    const char ch = lower[i];
    if (ch != '.' && ch != '_' && ch != '(' && ch != ')' && ch != '-') {
      k.push_back(ch);
    }
    ++i;
  }
  return k;
}

bool one_of(std::string_view k, std::initializer_list<std::string_view> words) {
  return std::ranges::find(words, k) != words.end();
}

char header_letter(std::string_view cell) {
  const std::string k = header_key(cell);
  if (one_of(k, {"pt", "pnt", "point", "pointno", "pointnumber", "number", "no", "id", "name", "pt#", "point#", "#"})) {
    return 'P';
  }
  if (one_of(k, {"n", "north", "northing", "y", "ycoord", "lat", "latitude"})) {
    return 'N';
  }
  if (one_of(k, {"e", "east", "easting", "x", "xcoord", "lon", "long", "longitude"})) {
    return 'E';
  }
  if (one_of(k, {"z", "elev", "elevation", "el", "height", "h", "zcoord", "rl"})) {
    return 'Z';
  }
  // /…/i on the lower-cased key (toLowerCase can make letters /i would fold differently only outside ASCII words)
  if (one_of(k,
             {"d", "desc", "description", "code", "rawdescription", "fulldescription", "note", "notes", "feature"})) {
    return 'D';
  }
  return 0;
}

std::optional<std::string> format_from_header(const std::vector<std::string>& cells) {
  std::string f;
  bool any_null = false;
  bool has_n = false;
  bool has_e = false;
  for (const std::string& c : cells) {
    const char l = header_letter(c);
    if (l == 0) {
      any_null = true;
    } else {
      f.push_back(l);
      has_n = has_n || l == 'N';
      has_e = has_e || l == 'E';
    }
  }
  if (!has_n || !has_e || any_null) {
    return std::nullopt;
  }
  return is_format(f) ? std::optional(f) : std::nullopt;
}

std::string detect_delimiter(const std::vector<std::string_view>& lines) {
  const auto need = static_cast<std::size_t>(std::ceil(static_cast<double>(lines.size()) * 0.6));
  auto score = [&](char c) {
    return static_cast<std::size_t>(std::ranges::count_if(lines, [&](std::string_view l) { return l.contains(c); }));
  };
  if (score(',') >= need) {
    return ",";
  }
  if (score('\t') >= need) {
    return "\t";
  }
  if (score(';') >= need) {
    return ";";
  }
  return "ws";
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::vector<std::string> split_row(std::string_view s, std::string_view delim) {
  if (delim == "ws") {
    return split_ws(base::js_trim(s));
  }
  std::vector<std::string> out;
  std::string cur;
  bool q = false;
  const char d = delim.empty() ? '\0' : delim[0];
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char ch = s[i];
    if (q) {
      if (ch == '"') {
        if (i + 1 < s.size() && s[i + 1] == '"') {
          cur.push_back('"');
          ++i;
        } else {
          q = false;
        }
      } else {
        cur.push_back(ch);
      }
    } else if (ch == '"' && base::js_blank(cur)) {
      q = true;
      cur.clear();
    } else if (ch == d && delim.size() == 1) {
      out.emplace_back(base::js_trim(cur));
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  out.emplace_back(base::js_trim(cur));
  return out;
}

ParsedPoints parse_points(std::string_view text, std::string_view format, std::string_view prefer,
                          const i18n::Catalog& words) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  struct Row {
    double line;
    std::string_view text;
  };
  std::vector<Row> rows;
  {
    std::size_t i = 0;
    double line = 0;
    while (true) {
      std::size_t j = i;
      while (j < text.size() && text[j] != '\r' && text[j] != '\n') {
        ++j;
      }
      const std::string_view l = text.substr(i, j - i);
      ++line;
      const std::string_view t = base::js_trim(l);
      if (!(t.empty() || t.starts_with('#') || t.starts_with(';') || t.starts_with("//"))) {
        rows.push_back({line, l});
      }
      if (j >= text.size()) {
        break;
      }
      i = j + ((text[j] == '\r' && j + 1 < text.size() && text[j + 1] == '\n') ? 2 : 1);
    }
  }
  ParsedPoints out;
  if (rows.empty()) {
    return out;
  }
  std::vector<std::string_view> head;
  for (std::size_t i = 0; i < rows.size() && i < 20; ++i) {
    head.push_back(rows[i].text);
  }
  const std::string delim = detect_delimiter(head);
  out.delimiter = delim;
  auto split = [&](std::string_view s) { return split_row(s, delim); };

  std::size_t start = 0;
  std::optional<std::string> fmt;
  const std::vector<std::string> first = split(rows[0].text);
  const auto numeric = std::ranges::count_if(first, [](const std::string& c) { return is_num(c); });
  if (numeric < 2) {
    out.header = first;
    start = 1;
    fmt = format_from_header(first);
    if (fmt) {
      out.format_from = "header";
    }
    bool lat = false;
    bool lon = false;
    for (const std::string& c : first) {
      const std::string k = header_key(c);
      lat = lat || k == "lat" || k == "latitude";
      lon = lon || k == "lon" || k == "long" || k == "longitude";
    }
    if (fmt && lat && lon) {
      out.latlon = true;
    }
  }
  if (format != "auto" && is_format(format)) {
    fmt = std::string(format);
    out.format_from = "chosen";
  }
  if (!fmt) {
    std::vector<std::vector<std::string>> sample;
    for (std::size_t k = start; k < rows.size() && k < start + 20; ++k) {
      sample.push_back(split(rows[k].text));
    }
    // mode of the column counts: the first count seen most often
    std::vector<std::pair<std::size_t, std::size_t>> m;
    for (const auto& c : sample) {
      auto it = std::ranges::find_if(m, [&](const auto& e) { return e.first == c.size(); });
      if (it != m.end()) {
        it->second++;
      } else {
        m.emplace_back(c.size(), 1);
      }
    }
    std::size_t best = 0;
    std::size_t n = 0;
    for (const auto& [v, c] : m) {
      if (c > best) {
        best = c;
        n = v;
      }
    }
    if (n >= 5) {
      fmt = "PNEZD";
    } else if (n == 4) {
      const bool pnez = std::ranges::all_of(sample, [](const auto& c) {
        return all_digits(c[0]) && is_num(c.size() > 3 ? std::string_view(c[3]) : std::string_view());
      });
      fmt = pnez ? "PNEZ" : "NEZD";
    } else {
      fmt = "NEZ";
    }
    if (prefer == "PENZD") {
      if (*fmt == "PNEZD") {
        fmt = "PENZD";
      } else if (*fmt == "PNEZ") {
        fmt = "PENZ";
      } else if (*fmt == "NEZD") {
        fmt = "ENZD";
      } else {
        fmt = "ENZ";
      }
    }
    out.format_from = "columns";
  }
  out.format = fmt;
  const std::string& cols = *fmt;
  auto idx = [&](char c) -> std::ptrdiff_t {
    const auto p = cols.find(c);
    return p == std::string::npos ? -1 : static_cast<std::ptrdiff_t>(p);
  };
  const std::ptrdiff_t i_p = idx('P');
  const std::ptrdiff_t i_n = idx('N');
  const std::ptrdiff_t i_e = idx('E');
  const std::ptrdiff_t i_z = idx('Z');
  const std::ptrdiff_t i_d = idx('D');
  const auto ncols = static_cast<std::ptrdiff_t>(cols.size());
  std::set<std::string> seen;
  double auto_n = 0;
  auto bad = [&](const Row& r, std::string why) {
    out.bad_count++;
    if (out.bad.size() < kMaxBad) {
      out.bad.push_back({r.line, slice16(r.text, 160), std::move(why)});
    }
  };
  for (std::size_t k = start; k < rows.size(); ++k) {
    const Row& r = rows[k];
    std::vector<std::string> c = split(r.text);
    if (i_d == ncols - 1 && std::cmp_greater(c.size(), ncols)) {
      const std::string glue = delim == "," ? "," : " ";
      std::string tail;
      for (auto q = static_cast<std::size_t>(i_d); q < c.size(); ++q) {
        if (std::cmp_greater(q, i_d)) {
          tail += glue;
        }
        tail += c[q];
      }
      c.resize(static_cast<std::size_t>(i_d));
      c.push_back(std::move(tail));
    }
    if (static_cast<std::ptrdiff_t>(c.size()) < ncols - (i_d >= 0 ? 1 : 0)) {
      bad(r, words.tn("pts.bad.cols", static_cast<double>(c.size()),
                      {{"n", appkit::num_str(static_cast<double>(c.size()))},
                       {"want", appkit::num_str(static_cast<double>(cols.size()))},
                       {"format", cols}}));
      continue;
    }
    auto cell = [&](std::ptrdiff_t i) -> const std::string* {
      return i >= 0 && static_cast<std::size_t>(i) < c.size() ? &c[static_cast<std::size_t>(i)] : nullptr;
    };
    const std::string* N = cell(i_n);
    const std::string* E = cell(i_e);
    const std::string* Z = cell(i_z);
    if (N == nullptr || E == nullptr || !is_num(*N) || !is_num(*E)) {
      bad(r, words.t("pts.bad.ne"));
      continue;
    }
    if (Z != nullptr && !Z->empty() && !is_num(*Z)) {
      bad(r, words.t("pts.bad.z"));
      continue;
    }
    if (out.latlon && (std::abs(base::js_number(*N)) > 90 || std::abs(base::js_number(*E)) > 180)) {
      bad(r, words.t("pts.bad.latlon"));
      continue;
    }
    std::string n;
    if (i_p >= 0) {
      const std::string* P = cell(i_p);
      n = P != nullptr ? std::string(base::js_trim(*P)) : std::string();
      if (n.empty()) {
        bad(r, words.t("pts.bad.nonum"));
        continue;
      }
    } else {
      do {
        n = appkit::num_str(++auto_n);
      } while (seen.contains(n));
    }
    if (seen.contains(n)) {
      out.duplicates++;
      bad(r, words.t("pts.bad.twice", {{"num", n}}));
      continue;
    }
    seen.insert(n);
    std::optional<double> z;
    if (Z != nullptr && !Z->empty()) {
      z = base::js_number(*Z);
    }
    const std::string* D = cell(i_d);
    out.points.push_back({n, base::js_number(*E), base::js_number(*N), z,
                          i_d >= 0 ? clean_text(D != nullptr ? std::string_view(*D) : std::string_view()) : ""});
  }
  return out;
}

std::string group_key(std::string_view desc) {
  const std::string_view t = base::js_trim(desc);
  std::size_t e = 0;
  while (e < t.size() && base::js_space_at(t, e) == 0) {
    ++e;
  }
  const std::string_view w = t.substr(0, e);
  return w.empty() ? std::string("(no description)") : base::js_to_upper(w);
}

std::vector<GroupCount> point_groups(const std::vector<SurveyPoint>& points) {
  std::vector<GroupCount> out;
  std::map<std::string, std::size_t> at;
  for (const SurveyPoint& p : points) {
    const std::string k = group_key(p.d);
    if (const auto it = at.find(k); it != at.end()) {
      out[it->second].count++;
    } else {
      at.emplace(k, out.size());
      out.push_back({k, 1});
    }
  }
  appkit::js_sort(out, [](const GroupCount& a, const GroupCount& b) {
    return static_cast<double>(base::js_locale_compare(a.key, b.key));
  });
  return out;
}

double by_number(const SurveyPoint& a, const SurveyPoint& b) {
  const double na = base::js_number(a.n);
  const double nb = base::js_number(b.n);
  if (std::isfinite(na) && std::isfinite(nb)) {
    return na - nb;
  }
  return static_cast<double>(base::js_locale_compare_numeric(a.n, b.n));
}

std::vector<std::size_t> group_points(const std::vector<SurveyPoint>& points, std::string_view key) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (group_key(points[i].d) == key) {
      out.push_back(i);
    }
  }
  appkit::js_sort(out, [&](std::size_t a, std::size_t b) { return by_number(points[a], points[b]); });
  return out;
}

std::string write_points(const std::vector<SurveyPoint>& points, double dec) {
  auto q = [](const std::string& s) {
    if (s.find_first_of("\",") == std::string::npos) {
      return s;
    }
    std::string o = "\"";
    for (const char c : s) {
      if (c == '"') {
        o += "\"\"";
      } else {
        o.push_back(c);
      }
    }
    return o + "\"";
  };
  const int d = static_cast<int>(std::clamp(std::isnan(dec) ? 0.0 : std::trunc(dec), 0.0, 100.0));
  std::string out;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const SurveyPoint& p = points[i];
    if (i > 0) {
      out += "\r\n";
    }
    out += q(p.n) + "," + base::js_to_fixed(p.y, d) + "," + base::js_to_fixed(p.x, d) + "," +
           (p.z ? base::js_to_fixed(*p.z, d) : std::string()) + "," + q(p.d);
  }
  return out + "\r\n";
}

std::vector<std::size_t> lasso_points(const std::vector<SurveyPoint>& points, const std::vector<XY>& path,
                                      std::string_view mode, double r) {
  std::vector<appkit::LassoItem> items;
  items.reserve(points.size());
  for (const SurveyPoint& p : points) {
    appkit::LassoItem it;
    it.box = appkit::LBox{p.x - r, p.y - r, p.x + r, p.y + r};
    it.segs = {{p.x - r, p.y, p.x + r, p.y}, {p.x, p.y - r, p.x, p.y + r}};
    items.push_back(std::move(it));
  }
  std::vector<appkit::LPt> pts;
  pts.reserve(path.size());
  for (const XY& q : path) {
    pts.push_back({q.x, q.y});
  }
  return appkit::lasso_select(items, pts, mode);
}

}  // namespace cc::civil
