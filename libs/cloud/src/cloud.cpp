// SPDX-License-Identifier: Apache-2.0
// pointcloud.js and las.js (CadCraft 0.5), step for step: see cloud.hpp.
#include "cc/cloud/cloud.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <lazperf/readers.hpp>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"

namespace cc::cloud {

namespace {

using base::Errc;
using base::fail;

constexpr std::array<std::uint32_t, 11> kMinLen{20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
/// COLOR_AT: where a format's red, green, blue start (-1: no colour)
int color_at(int fmt) {
  switch (fmt) {
    case 2: return 20;
    case 3:
    case 5: return 28;
    case 7:
    case 8:
    case 10: return 30;
    default: return -1;
  }
}

/// A little-endian value at `off`, or 0 when it is not in `b` (callers check sizes first)
template <class T>
T le(Bytes b, std::uint64_t off) {
  base::ByteReader r(b);
  if (off > b.size() || !r.seek(static_cast<std::size_t>(off))) {
    return T{};
  }
  if constexpr (std::is_same_v<T, std::uint8_t>) {
    return r.read_u8().value_or(0);
  } else if constexpr (std::is_same_v<T, std::uint16_t>) {
    return r.read_u16_le().value_or(0);
  } else if constexpr (std::is_same_v<T, std::uint32_t>) {
    return r.read_u32_le().value_or(0);
  } else if constexpr (std::is_same_v<T, std::uint64_t>) {
    return r.read_u64_le().value_or(0);
  } else if constexpr (std::is_same_v<T, std::int32_t>) {
    return r.read_i32_le().value_or(0);
  } else {
    return r.read_f64_le().value_or(0.0);
  }
}

/// `file.slice(a, b)`: clamped to the file
Bytes slice(Bytes f, std::uint64_t a, std::uint64_t b) {
  const std::uint64_t n = f.size();
  a = std::min(a, n);
  b = std::clamp(b, a, n);
  return f.subspan(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
}

/// las.js ascii(): printable ASCII up to the first NUL, others as spaces, trimmed
std::string ascii(Bytes b, std::uint64_t o, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    const auto c = le<std::uint8_t>(b, o + i);
    if (c == 0) {
      break;
    }
    s += c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
  }
  return std::string(base::js_trim(s));
}

/// toLocaleString('en-US') of a whole number
std::string grouped(double v) {
  std::string n;
  base::append_number(n, std::abs(v));
  std::string out = v < 0 ? "-" : "";
  for (std::size_t i = 0; i < n.size(); ++i) {
    if (i > 0 && (n.size() - i) % 3 == 0) {
      out += ',';
    }
    out += n[i];
  }
  return out;
}
std::string num(double v) {
  std::string s;
  base::append_number(s, v);
  return s;
}

/// new TextDecoder().decode(bytes): UTF-8, the BOM dropped, every ill-formed part one U+FFFD (WHATWG)
std::string utf8_lossy(Bytes b) {
  std::string out;
  out.reserve(b.size());
  std::size_t i = 0;
  if (b.size() >= 3 && std::to_integer<unsigned>(b[0]) == 0xEF && std::to_integer<unsigned>(b[1]) == 0xBB &&
      std::to_integer<unsigned>(b[2]) == 0xBF) {
    i = 3;
  }
  const auto at = [&b](std::size_t k) { return std::to_integer<unsigned>(b[k]); };
  while (i < b.size()) {
    const unsigned c = at(i);
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    std::size_t need = 0;
    unsigned lo = 0x80;
    unsigned hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      lo = c == 0xE0 ? 0xA0 : 0x80;
      hi = c == 0xED ? 0x9F : 0xBF;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      lo = c == 0xF0 ? 0x90 : 0x80;
      hi = c == 0xF4 ? 0x8F : 0xBF;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    std::size_t k = 1;
    for (; k <= need; ++k) {
      if (i + k >= b.size()) {
        break;
      }
      const unsigned d = at(i + k);
      const unsigned l = k == 1 ? lo : 0x80;
      const unsigned h = k == 1 ? hi : 0xBF;
      if (d < l || d > h) {
        break;
      }
    }
    if (k <= need) {  // ill-formed: the maximal part read so far becomes one replacement
      out += "\xEF\xBF\xBD";
      i += k;
      continue;
    }
    for (std::size_t m = 0; m <= need; ++m) {
      out += static_cast<char>(at(i + m));
    }
    i += need + 1;
  }
  return out;
}

/// Each line of `text` as `split(/\r?\n/)` gives them
std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      std::size_t end = i;
      if (end > start && text[end - 1] == '\r') {
        --end;
      }
      out.push_back(text.substr(start, end - start));
      start = i + 1;
    }
  }
  out.push_back(text.substr(start));
  return out;
}

/// numbers(line): `line.trim().split(/[\s,;]+/)`, every part a finite number, else nothing
std::optional<std::vector<double>> numbers(std::string_view line) {
  const std::string_view s = base::js_trim(line);
  std::vector<double> out;
  std::size_t i = 0;
  while (i <= s.size()) {
    std::size_t j = i;
    while (j < s.size() && s[j] != ',' && s[j] != ';' && base::js_space_at(s, j) == 0) {
      ++j;
    }
    const std::string_view part = s.substr(i, j - i);
    if (!part.empty()) {
      const double v = base::js_number(part);
      if (!std::isfinite(v)) {
        return std::nullopt;
      }
      out.push_back(v);
    }
    if (j >= s.size()) {
      break;
    }
    // the separator run
    while (j < s.size()) {
      if (s[j] == ',' || s[j] == ';') {
        ++j;
      } else if (const std::size_t w = base::js_space_at(s, j); w > 0) {
        j += w;
      } else {
        break;
      }
    }
    i = j;
  }
  return out;
}

bool is_integer(double v) {
  return std::isfinite(v) && std::floor(v) == v;
}

/// Keeps at most `cap` points evenly: when full, every other kept point goes and the keep rate halves
struct Keeper {
  std::size_t cap = 1;
  double rate = 1, acc = 0;
  std::size_t n = 0;
  std::vector<double> x, y, z;
  std::vector<std::uint16_t> rgb, inten;
  std::vector<std::uint8_t> cls;
  bool has_rgb = false, has_inten = false, has_cls = false;

  Keeper(double cap_in, double rate_in, bool want_rgb, bool want_inten, bool want_cls)
      : cap(static_cast<std::size_t>(std::max(1.0, std::floor(cap_in)))),
        rate(std::min(1.0, rate_in)),
        x(cap),
        y(cap),
        z(cap),
        has_rgb(want_rgb),
        has_inten(want_inten),
        has_cls(want_cls) {
    if (has_rgb) {
      rgb.resize(cap * 3);
    }
    if (has_inten) {
      inten.resize(cap);
    }
    if (has_cls) {
      cls.resize(cap);
    }
  }
  bool want() {
    acc += rate;
    if (acc < 1) {
      return false;
    }
    acc -= 1;
    if (n == cap) {
      halve();
    }
    return true;
  }
  void halve() {
    std::size_t k = 0;
    for (std::size_t j = 0; j < n; j += 2, ++k) {
      x[k] = x[j];
      y[k] = y[j];
      z[k] = z[j];
      if (has_rgb) {
        rgb[k * 3] = rgb[j * 3];
        rgb[k * 3 + 1] = rgb[j * 3 + 1];
        rgb[k * 3 + 2] = rgb[j * 3 + 2];
      }
      if (has_inten) {
        inten[k] = inten[j];
      }
      if (has_cls) {
        cls[k] = cls[j];
      }
    }
    n = k;
    rate /= 2;
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — x, y, z, r, g, b as the JS passes them
  void put(double px, double py, double pz, std::uint16_t r, std::uint16_t g, std::uint16_t b, std::uint16_t i,
           std::uint8_t c) {
    const std::size_t k = n++;
    x[k] = px;
    y[k] = py;
    z[k] = pz;
    if (has_rgb) {
      rgb[k * 3] = r;
      rgb[k * 3 + 1] = g;
      rgb[k * 3 + 2] = b;
    }
    if (has_inten) {
      inten[k] = i;
    }
    if (has_cls) {
      cls[k] = c;
    }
  }
};

struct Bounds {
  V3 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::infinity()};
  V3 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity()};
  void add(double x, double y, double z) {
    min[0] = std::min(min[0], x);
    max[0] = std::max(max[0], x);
    min[1] = std::min(min[1], y);
    max[1] = std::max(max[1], y);
    min[2] = std::min(min[2], z);
    max[2] = std::max(max[2], z);
  }
};

std::uint16_t clamp16(double v) {
  return static_cast<std::uint16_t>(std::clamp(base::js_round(v), 0.0, 65535.0));
}

/// recordDecoder(fmt, L, scale, offset): one point record at `o` into the bounds, the class counts and the keeper
struct RecordDecoder {
  V3 scale, offset;
  bool ext;
  int ca;
  bool color_ok;
  explicit RecordDecoder(const LasHeader& head)
      : scale(head.scale),
        offset(head.offset),
        ext(head.format >= 6),
        ca(color_at(head.format)),
        color_ok(ca >= 0 && static_cast<std::uint32_t>(ca) + 6 <= head.record_length) {}
  void operator()(Bytes dv, std::uint64_t o, Keeper& keep, Bounds& box,
                  std::map<int, std::uint64_t>& class_counts) const {
    const double x = le<std::int32_t>(dv, o) * scale[0] + offset[0];
    const double y = le<std::int32_t>(dv, o + 4) * scale[1] + offset[1];
    const double z = le<std::int32_t>(dv, o + 8) * scale[2] + offset[2];
    const std::uint8_t c = ext ? le<std::uint8_t>(dv, o + 16) : (le<std::uint8_t>(dv, o + 15) & 31U);
    box.add(x, y, z);
    ++class_counts[c];
    if (!keep.want()) {
      return;
    }
    const auto inten = le<std::uint16_t>(dv, o + 12);
    if (color_ok) {
      const auto cu = static_cast<std::uint64_t>(ca);
      keep.put(x, y, z, le<std::uint16_t>(dv, o + cu), le<std::uint16_t>(dv, o + cu + 2),
               le<std::uint16_t>(dv, o + cu + 4), inten, c);
    } else {
      keep.put(x, y, z, 0, 0, 0, inten, c);
    }
  }
};

/// LAZ: the file handed to laz-perf (a copy: its reader takes a writable buffer), decoded point by point; a point that
/// will not decode ends the read with a note, as 0.5's does
base::Result<std::uint64_t> scan_laz(Bytes file, const LasHeader& h, Keeper& keep, Bounds& box,
                                     std::map<int, std::uint64_t>& class_counts, std::vector<std::string>& notes,
                                     const i18n::Catalog& words) {
  std::vector<char> buf(file.size());
  std::ranges::transform(file, buf.begin(), [](std::byte b) { return static_cast<char>(b); });
  std::unique_ptr<lazperf::reader::mem_file> laz;
  try {
    laz = std::make_unique<lazperf::reader::mem_file>(buf.data(), buf.size());
  } catch (...) {  // NOLINT(bugprone-empty-catch) — laz-perf throws for a file it cannot open: said below
    laz.reset();
  }
  if (!laz) {
    return fail(Errc::invalid, words.t("las.bad.laz_open"));
  }
  // getCount() is laz-perf's 32-bit count
  const auto n = std::min<std::uint64_t>(static_cast<std::uint32_t>(laz->pointCount()), h.count);
  const std::uint32_t L = laz->header().point_record_length;
  if (L < 20) {
    return fail(Errc::invalid, words.t("las.bad.laz_len"));
  }
  std::vector<char> pt(L);
  std::vector<std::byte> rec(L);
  const RecordDecoder dec(h);
  std::uint64_t read = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    bool ok = true;
    try {
      laz->readPoint(pt.data());
    } catch (...) {  // NOLINT(bugprone-empty-catch) — a damaged point: said as a note
      ok = false;
    }
    if (!ok) {
      notes.push_back(words.t("las.laz.stopped",
                              {{"read", grouped(static_cast<double>(i))}, {"n", grouped(static_cast<double>(n))}}));
      break;
    }
    std::ranges::transform(pt, rec.begin(), [](char ch) { return static_cast<std::byte>(ch); });
    dec(rec, 0, keep, box, class_counts);
    ++read;
  }
  return read;
}

std::uint64_t scan_las(Bytes file, const Info& info, Keeper& keep, Bounds& box,
                       std::map<int, std::uint64_t>& class_counts) {
  if (!info.las) {
    return 0;
  }
  const LasHeader& h = *info.las;
  const std::uint32_t L = h.record_length;
  const RecordDecoder dec(h);
  const std::uint64_t per = std::max<std::uint64_t>(1, std::uint64_t{4} * 1048576 / L);
  const auto total = static_cast<std::uint64_t>(info.count);
  std::uint64_t read = 0;
  for (std::uint64_t i = 0; i < total; i += per) {
    const std::uint64_t n = std::min(per, total - i);
    const std::uint64_t start = h.offset_to_points + i * L;
    const Bytes dv = slice(file, start, start + n * L);
    const std::uint64_t m = dv.size() / L;
    for (std::uint64_t k = 0; k < m; ++k) {
      dec(dv, k * L, keep, box, class_counts);
    }
    read += m;
    if (m < n) {
      break;
    }
  }
  return read;
}

std::uint64_t scan_text(Bytes file, const Info& info, Keeper& keep, Bounds& box, std::vector<std::string>& notes,
                        const i18n::Catalog& words) {
  const Columns cols = info.columns.value_or(Columns{});
  const std::string text = utf8_lossy(file);
  std::uint64_t read = 0;
  std::uint64_t skipped = 0;
  bool first = true;
  std::vector<std::string_view> ls = lines_of(text);
  if (!ls.empty() && ls.back().empty()) {
    ls.pop_back();  // `if (rest) line(rest)`
  }
  for (const std::string_view s : ls) {
    if (first) {
      first = false;
      const auto v = numbers(s);
      if (v && v->size() == 1 && is_integer(v->front())) {
        continue;  // the PTS count line
      }
    }
    const auto v = numbers(s);
    if (!v || v->size() < 3) {
      if (!base::js_blank(s)) {
        ++skipped;
      }
      continue;
    }
    const double x = (*v)[0];
    const double y = (*v)[1];
    const double z = (*v)[2];
    box.add(x, y, z);
    ++read;
    if (!keep.want()) {
      continue;
    }
    const auto at = [&v](int k) { return (*v)[static_cast<std::size_t>(k)]; };
    const std::uint16_t i =
        cols.i >= 0 && v->size() > static_cast<std::size_t>(cols.i) ? clamp16(at(cols.i)) : std::uint16_t{0};
    if (cols.rgb >= 0 && v->size() > static_cast<std::size_t>(cols.rgb) + 2) {
      keep.put(x, y, z, clamp16(at(cols.rgb)), clamp16(at(cols.rgb + 1)), clamp16(at(cols.rgb + 2)), i, 0);
    } else {
      keep.put(x, y, z, 0, 0, 0, i, 0);
    }
  }
  if (skipped > 0) {
    notes.push_back(
        words.tn("pc.text.skipped", static_cast<double>(skipped), {{"count", grouped(static_cast<double>(skipped))}}));
  }
  return read;
}

base::Result<Info> text_info(std::string_view name, Bytes file, const i18n::Catalog& words) {
  const std::uint64_t size = file.size();
  if (size > kMaxTextBytes) {
    return fail(Errc::too_big, words.t("pc.too_big", {{"name", std::string(name)},
                                                      {"mb", num(base::js_round(static_cast<double>(size) / 1048576))},
                                                      {"max", num(static_cast<double>(kMaxTextBytes) / 1048576)}}));
  }
  const std::string text = utf8_lossy(slice(file, 0, 65536));
  std::vector<std::string_view> lines = lines_of(text);
  if (size > 65536) {
    lines.pop_back();  // the last line may be cut
  }
  std::size_t first = 0;
  std::optional<double> declared;
  const auto one = numbers(lines.empty() ? std::string_view() : lines.front());
  if (one && one->size() == 1 && is_integer(one->front())) {
    declared = one->front();
    first = 1;
  }
  double rows = 0;
  double bytes = 0;
  std::vector<std::pair<std::size_t, double>> counts;  // a Map: first seen first
  for (std::size_t i = first; i < lines.size(); ++i) {
    const auto v = numbers(lines[i]);
    if (!v || v->size() < 3) {
      continue;
    }
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto it = std::ranges::find_if(counts, [&v](const auto& c) { return c.first == v->size(); });
    if (it == counts.end()) {
      counts.emplace_back(v->size(), 0);
      it = std::prev(counts.end());
    }
    it->second += 1;
    rows += 1;
    bytes += static_cast<double>(base::utf16_length(lines[i])) + 1;
  }
  if (rows == 0) {
    return fail(Errc::invalid, words.t("pc.text.none", {{"name", std::string(name)}}));
  }
  std::ranges::stable_sort(counts, [](const auto& a, const auto& b) { return a.second > b.second; });
  Info info;
  info.kind = "xyz";
  info.name = std::string(name);
  info.size = size;
  info.columns = text_columns(static_cast<int>(counts.front().first));
  info.estimate = !declared.has_value();
  info.count =
      declared ? *declared : std::max(rows, base::js_round(static_cast<double>(size) / std::max(8.0, bytes / rows)));
  info.color = info.columns->rgb >= 0;
  info.intensity = info.columns->i >= 0;
  info.classes = false;
  info.crs_note = words.t("las.crs.none");
  return info;
}

double quantile(const auto& arr, std::size_t step, std::size_t offset, double frac, std::size_t n) {
  std::vector<double> pick;
  const std::size_t every = std::max<std::size_t>(1, n / 20000);
  for (std::size_t k = 0; k < n; k += every) {
    pick.push_back(static_cast<double>(arr[k * step + offset]));
  }
  std::ranges::sort(pick);
  const double at = std::floor(frac * static_cast<double>(pick.size() - 1));
  return pick[static_cast<std::size_t>(std::min(static_cast<double>(pick.size() - 1), std::max(0.0, at)))];
}

constexpr std::array<std::array<double, 3>, 5> kRamp{
    {{0.19, 0.29, 0.75}, {0.15, 0.62, 0.85}, {0.2, 0.7, 0.35}, {0.95, 0.8, 0.2}, {0.85, 0.25, 0.15}}};
void ramp_at(double f, std::vector<std::uint8_t>& out, std::size_t k) {
  const double cl = std::isnan(f) ? 0 : std::max(0.0, std::min(1.0, f));
  const double x = cl * static_cast<double>(kRamp.size() - 1);
  const double i = std::min(static_cast<double>(kRamp.size() - 2), std::floor(x));
  const double u = x - i;
  const auto& a = kRamp.at(static_cast<std::size_t>(i));
  const auto& b = kRamp.at(static_cast<std::size_t>(i) + 1);
  for (std::size_t c = 0; c < 3; ++c) {
    out[k + c] = static_cast<std::uint8_t>(base::js_round((a.at(c) + (b.at(c) - a.at(c)) * u) * 255));
  }
}

std::array<std::uint8_t, 3> class_color(int c) {
  static constexpr std::array<std::array<std::uint8_t, 3>, 19> kColors{{{150, 150, 150},
                                                                        {170, 170, 170},
                                                                        {160, 120, 70},
                                                                        {140, 200, 110},
                                                                        {70, 160, 60},
                                                                        {30, 110, 40},
                                                                        {215, 70, 50},
                                                                        {110, 110, 110},
                                                                        {240, 200, 40},
                                                                        {50, 110, 220},
                                                                        {120, 80, 160},
                                                                        {90, 90, 100},
                                                                        {200, 200, 120},
                                                                        {230, 150, 40},
                                                                        {240, 120, 20},
                                                                        {180, 60, 160},
                                                                        {220, 160, 60},
                                                                        {140, 140, 190},
                                                                        {80, 80, 80}}};
  if (c >= 0 && std::cmp_less(c, kColors.size())) {
    return kColors.at(static_cast<std::size_t>(c));
  }
  return {200, 200, 200};
}

}  // namespace

base::Result<LasHeader> read_las_header(Bytes file, const i18n::Catalog& words) {
  const std::uint64_t size = file.size();
  const auto bad = [&words](std::string_view key,
                            std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    return fail(Errc::invalid, words.t(key, f));
  };
  if (size < 227) {
    return bad("las.bad.short");
  }
  const Bytes dv = slice(file, 0, 375);
  if (ascii(dv, 0, 4) != "LASF") {
    return fail(Errc::invalid, words.t("las.not_las"));
  }
  const auto major = le<std::uint8_t>(dv, 24);
  const auto minor = le<std::uint8_t>(dv, 25);
  if (major != 1 || minor > 4) {
    return bad("las.bad.version", {{"version", std::to_string(major) + "." + std::to_string(minor)}});
  }
  LasHeader h;
  h.header_size = le<std::uint16_t>(dv, 94);
  h.offset_to_points = le<std::uint32_t>(dv, 96);
  const auto n_vlr = le<std::uint32_t>(dv, 100);
  const auto fmt_byte = le<std::uint8_t>(dv, 104);
  h.format = static_cast<int>(fmt_byte & 0x3FU);
  h.compressed = (fmt_byte & 0xC0U) != 0;
  h.record_length = le<std::uint16_t>(dv, 105);
  if (h.header_size < 227 || h.header_size > size) {
    return bad("las.bad.header_size");
  }
  if (h.offset_to_points < h.header_size || h.offset_to_points > size) {
    return bad("las.bad.offset");
  }
  if (h.format > 10) {
    return bad("las.bad.format", {{"format", std::to_string(h.format)}});
  }
  if (h.record_length < kMinLen.at(static_cast<std::size_t>(h.format))) {
    return bad("las.bad.record_len", {{"len", std::to_string(h.record_length)}, {"format", std::to_string(h.format)}});
  }
  if (n_vlr > 100000) {
    return bad("las.bad.vlr_count", {{"n", std::to_string(n_vlr)}});
  }
  h.count = le<std::uint32_t>(dv, 107);
  h.scale = {le<double>(dv, 131), le<double>(dv, 139), le<double>(dv, 147)};
  h.offset = {le<double>(dv, 155), le<double>(dv, 163), le<double>(dv, 171)};
  h.max = {le<double>(dv, 179), le<double>(dv, 195), le<double>(dv, 211)};
  h.min = {le<double>(dv, 187), le<double>(dv, 203), le<double>(dv, 219)};
  const auto finite_all = [](const V3& v) { return std::ranges::all_of(v, [](double d) { return std::isfinite(d); }); };
  if (!finite_all(h.scale) || !finite_all(h.offset) ||
      std::ranges::any_of(h.scale, [](double s) { return s == 0 || std::abs(s) > 1e6; })) {
    return bad("las.bad.scale");
  }
  std::uint64_t evlr_start = 0;
  std::uint32_t n_evlr = 0;
  if (minor >= 4 && h.header_size >= 375) {
    evlr_start = le<std::uint64_t>(dv, 235);
    n_evlr = le<std::uint32_t>(dv, 243);
    if (const auto c64 = le<std::uint64_t>(dv, 247); c64 > 0) {
      h.count = c64;
    }
  }
  h.version = std::to_string(major) + "." + std::to_string(minor);
  // VLRs (54-byte headers)
  const Bytes vb = slice(file, h.header_size, h.offset_to_points);
  std::uint64_t o = h.header_size;
  for (std::uint32_t i = 0; i < n_vlr; ++i) {
    const std::uint64_t r = o - h.header_size;
    if (r + 54 > vb.size()) {
      h.notes.push_back(words.tn("las.vlr.past", static_cast<double>(n_vlr - i)));
      break;
    }
    const auto len = le<std::uint16_t>(vb, r + 20);
    if (r + 54 + len > vb.size()) {
      h.notes.push_back(words.t("las.vlr.one_past"));
      break;
    }
    h.vlrs.push_back(Vlr{.user = ascii(vb, r + 2, 16),
                         .desc = ascii(vb, r + 22, 32),
                         .id = le<std::uint16_t>(vb, r + 18),
                         .offset = o + 54,
                         .length = len,
                         .ext = false});
    o += 54 + std::uint64_t{len};
  }
  if (n_evlr != 0 && evlr_start != 0) {
    if (n_evlr > 100000 || evlr_start < h.offset_to_points || evlr_start > size) {
      h.notes.push_back(words.t("las.evlr.outside"));
    } else {
      std::uint64_t e = evlr_start;
      for (std::uint32_t i = 0; i < n_evlr && e + 60 <= size; ++i) {
        const Bytes eb = slice(file, e, e + 60);
        const auto len = le<std::uint64_t>(eb, 20);
        if (len > size || e + 60 + len > size) {
          h.notes.push_back(words.t("las.evlr.past"));
          break;
        }
        h.evlrs.push_back(Vlr{.user = ascii(eb, 2, 16),
                              .desc = ascii(eb, 22, 32),
                              .id = le<std::uint16_t>(eb, 18),
                              .offset = e + 60,
                              .length = len,
                              .ext = true});
        e += 60 + len;
      }
    }
  }
  // points the file really holds
  const std::uint64_t end =
      evlr_start != 0 && evlr_start > h.offset_to_points && evlr_start <= size ? evlr_start : size;
  h.available = h.count;
  if (!h.compressed) {
    h.available = (end - h.offset_to_points) / h.record_length;
    if (h.available < h.count) {
      h.notes.push_back(words.t("las.short", {{"available", grouped(static_cast<double>(h.available))},
                                              {"count", grouped(static_cast<double>(h.count))}}));
    } else {
      h.available = h.count;
    }
  } else if (std::ranges::none_of(h.vlrs, [](const Vlr& v) { return v.user == "laszip encoded" && v.id == 22204; })) {
    return bad("las.bad.no_laszip");
  }
  return h;
}

Columns text_columns(int n) {
  if (n >= 7) {
    return {.n = n, .i = 3, .rgb = 4};  // x y z i r g b (PTS)
  }
  if (n == 6) {
    return {.n = n, .i = -1, .rgb = 3};  // x y z r g b
  }
  if (n >= 4) {
    return {.n = n, .i = 3, .rgb = -1};  // x y z i
  }
  return {.n = n, .i = -1, .rgb = -1};
}

std::string sniff(std::string_view name, Bytes head) {
  if (head.size() >= 4 && std::to_integer<unsigned>(head[0]) == 0x4C && std::to_integer<unsigned>(head[1]) == 0x41 &&
      std::to_integer<unsigned>(head[2]) == 0x53 && std::to_integer<unsigned>(head[3]) == 0x46) {
    return "las";  // LASF
  }
  const std::size_t dot = name.rfind('.');
  const std::string ext = base::js_to_lower(dot == std::string_view::npos ? name : name.substr(dot + 1));
  if (ext == "las" || ext == "laz") {
    return "las";
  }
  if (ext == "xyz" || ext == "pts") {
    return "xyz";
  }
  return {};
}

base::Result<Info> read_info(std::string_view name, Bytes file, const i18n::Catalog& words) {
  const std::string kind = sniff(name, slice(file, 0, 4));
  if (kind.empty()) {
    return fail(Errc::unsupported, words.t("pc.not_cloud", {{"name", std::string(name)}}));
  }
  if (kind != "las") {
    return text_info(name, file, words);
  }
  const std::uint64_t size = file.size();
  const auto mb = [size] { return num(base::js_round(static_cast<double>(size) / 1048576)); };
  if (size > kMaxFileBytes) {
    return fail(Errc::too_big, words.t("pc.too_big", {{"name", std::string(name)},
                                                      {"mb", mb()},
                                                      {"max", num(static_cast<double>(kMaxFileBytes) / 1048576)}}));
  }
  auto h = read_las_header(file, words);
  if (!h) {
    return std::unexpected(h.error());
  }
  if (h->compressed && size > std::uint64_t{1024} * 1048576) {
    return fail(Errc::too_big, words.t("pc.laz_too_big", {{"name", std::string(name)}, {"mb", mb()}}));
  }
  Info info;
  info.kind = h->compressed ? "laz" : "las";
  info.name = std::string(name);
  info.size = size;
  info.count = static_cast<double>(h->compressed ? h->count : h->available);
  info.estimate = false;
  info.min = h->min;
  info.max = h->max;
  info.color = color_at(h->format) >= 0;
  info.intensity = true;
  info.classes = true;
  info.notes = h->notes;
  info.version = h->version;
  info.format = h->format;
  info.las = std::move(*h);
  return info;
}

base::Result<Cloud> load_cloud(std::string_view name, Bytes file, const Info& info, double budget,
                               const i18n::Catalog& words) {
  budget = std::max(1000.0, std::min(kMaxBudget, std::floor(budget)));
  const double rate = info.count > 0 ? budget / info.count : 1;
  Keeper keep(budget, rate, info.color, info.intensity, info.classes);
  Bounds box;
  std::map<int, std::uint64_t> class_counts;
  std::vector<std::string> notes = info.notes;
  std::uint64_t read = 0;
  if (info.kind == "xyz") {
    read = scan_text(file, info, keep, box, notes, words);
  } else if (!info.las) {
    return fail(Errc::invalid, words.t("las.bad.laz_open"));
  } else if (info.las->compressed) {
    auto r = scan_laz(file, *info.las, keep, box, class_counts, notes, words);
    if (!r) {
      return std::unexpected(r.error());
    }
    read = *r;
  } else {
    read = scan_las(file, info, keep, box, class_counts);
  }
  if (keep.n == 0) {
    return fail(Errc::invalid, words.t("pc.empty", {{"name", std::string(name)}}));
  }
  // the origin: the low corner of what was read, rounded to a whole unit, so the float32 offsets stay small
  Cloud c;
  c.name = std::string(name);
  c.kind = info.kind;
  c.read = read;
  c.n = keep.n;
  for (std::size_t a = 0; a < 3; ++a) {
    c.origin.at(a) = std::floor(box.min.at(a));
  }
  c.pos.resize(c.n * 3);
  for (std::size_t k = 0; k < c.n; ++k) {
    c.pos[k * 3] = static_cast<float>(keep.x[k] - c.origin[0]);
    c.pos[k * 3 + 1] = static_cast<float>(keep.y[k] - c.origin[1]);
    c.pos[k * 3 + 2] = static_cast<float>(keep.z[k] - c.origin[2]);
  }
  if (keep.has_rgb) {
    std::uint16_t hi = 0;
    for (std::size_t k = 0; k < c.n * 3; ++k) {
      hi = std::max(hi, keep.rgb[k]);
    }
    if (hi == 0) {
      notes.push_back(words.t("pc.no_color"));
    } else {
      const unsigned shift = hi > 255 ? 8U : 0U;
      std::vector<std::uint8_t> rgb(c.n * 3);
      for (std::size_t k = 0; k < c.n * 3; ++k) {
        rgb[k] = static_cast<std::uint8_t>(static_cast<unsigned>(keep.rgb[k]) >> shift);
      }
      c.rgb = std::move(rgb);
    }
  }
  if (static_cast<double>(read) < info.count && info.kind != "xyz") {
    notes.push_back(
        words.t("pc.read_short", {{"read", grouped(static_cast<double>(read))}, {"count", grouped(info.count)}}));
  }
  if (keep.has_inten) {
    c.inten = std::vector<std::uint16_t>(keep.inten.begin(), keep.inten.begin() + static_cast<std::ptrdiff_t>(c.n));
  }
  if (keep.has_cls) {
    c.cls = std::vector<std::uint8_t>(keep.cls.begin(), keep.cls.begin() + static_cast<std::ptrdiff_t>(c.n));
  }
  c.min = box.min;
  c.max = box.max;
  c.class_counts = std::move(class_counts);
  c.unit = info.unit;
  c.z_unit = info.z_unit ? info.z_unit : info.unit;
  c.notes = std::move(notes);
  return c;
}

std::vector<std::uint8_t> colorize(const Cloud& c, std::string_view mode, std::array<std::uint8_t, 3> color) {
  const std::size_t n = c.n;
  std::vector<std::uint8_t> out(n * 3);
  if (mode == "rgb" && c.rgb) {
    return *c.rgb;
  }
  if (mode == "single") {
    for (std::size_t k = 0; k < n; ++k) {
      std::ranges::copy(color, out.begin() + static_cast<std::ptrdiff_t>(k * 3));
    }
    return out;
  }
  if (mode == "class" && c.cls) {
    for (std::size_t k = 0; k < n; ++k) {
      std::ranges::copy(class_color((*c.cls)[k]), out.begin() + static_cast<std::ptrdiff_t>(k * 3));
    }
    return out;
  }
  if (mode == "intensity" && c.inten) {
    const double lo = quantile(*c.inten, 1, 0, 0.02, n);
    double hi = quantile(*c.inten, 1, 0, 0.98, n);
    if (hi == 0) {
      hi = 1;
    }
    for (std::size_t k = 0; k < n; ++k) {
      const double f = std::max(0.0, std::min(1.0, ((*c.inten)[k] - lo) / std::max(1.0, hi - lo)));
      const auto g = static_cast<std::uint8_t>(base::js_round(40 + 215 * f));
      out[k * 3] = g;
      out[k * 3 + 1] = g;
      out[k * 3 + 2] = g;
    }
    return out;
  }
  // height (also rgb without colours, class without classes, intensity without intensity)
  const double lo = quantile(c.pos, 3, 2, 0.01, n);
  const double hi = quantile(c.pos, 3, 2, 0.99, n);
  double span = hi - lo;
  if (span == 0 || std::isnan(span)) {
    span = 1;
  }
  for (std::size_t k = 0; k < n; ++k) {
    ramp_at((static_cast<double>(c.pos[k * 3 + 2]) - lo) / span, out, k * 3);
  }
  return out;
}

std::vector<std::string> color_modes(const Cloud& c) {
  std::vector<std::string> out;
  if (c.rgb) {
    out.emplace_back("rgb");
  }
  out.emplace_back("height");
  if (c.inten) {
    out.emplace_back("intensity");
  }
  if (c.cls && c.class_counts.size() > 1) {
    out.emplace_back("class");
  }
  out.emplace_back("single");
  return out;
}

std::vector<std::uint32_t> slice_of(const Cloud& c, char axis, double at, double thick) {
  std::size_t a = 2;
  if (axis == 'x') {
    a = 0;
  } else if (axis == 'y') {
    a = 1;
  }
  const double centre = at - c.origin.at(a);
  const double h = std::abs(thick) / 2;
  std::vector<std::uint32_t> out;
  for (std::size_t k = 0; k < c.n; ++k) {
    if (std::abs(static_cast<double>(c.pos[k * 3 + a]) - centre) <= h) {
      out.push_back(static_cast<std::uint32_t>(k));
    }
  }
  return out;
}

V3 point_at(const Cloud& c, std::size_t i) {
  return {static_cast<double>(c.pos[i * 3]) + c.origin[0], static_cast<double>(c.pos[i * 3 + 1]) + c.origin[1],
          static_cast<double>(c.pos[i * 3 + 2]) + c.origin[2]};
}

PlanGrid::PlanGrid(const Cloud& c, const std::vector<std::uint32_t>* indices, double cell) : c_(&c) {
  std::vector<std::uint32_t> all;
  if (indices == nullptr) {
    all.resize(c.n);
    for (std::size_t k = 0; k < c.n; ++k) {
      all[k] = static_cast<std::uint32_t>(k);
    }
  }
  const std::vector<std::uint32_t>& idx = indices != nullptr ? *indices : all;
  const double span = std::max({c.max[0] - c.min[0], c.max[1] - c.min[1], 1e-9});
  cell_ = cell > 0 ? cell : std::max(span / 1024, span / std::sqrt(std::max(1.0, static_cast<double>(idx.size()))) * 2);
  for (const std::uint32_t k : idx) {
    const double key = std::floor(static_cast<double>(c.pos[std::size_t{k} * 3]) / cell_) * 1048576 +
                       std::floor(static_cast<double>(c.pos[std::size_t{k} * 3 + 1]) / cell_);
    map_[key].push_back(k);
  }
}

std::int64_t PlanGrid::nearest(double x, double y, double r) const {
  const Cloud& c = *c_;
  const double lx = x - c.origin[0];
  const double ly = y - c.origin[1];
  const double i0 = std::floor((lx - r) / cell_);
  const double i1 = std::floor((lx + r) / cell_);
  const double j0 = std::floor((ly - r) / cell_);
  const double j1 = std::floor((ly + r) / cell_);
  if ((i1 - i0 + 1) * (j1 - j0 + 1) > 40000) {
    return -1;  // a radius far bigger than the points: nothing sensible to snap to
  }
  std::int64_t best = -1;
  double bd = r * r;
  // at most 40 000 cells (a NaN bound: none); counted, so a cell index past 2^53 cannot stall the loop
  const auto ni = i1 >= i0 ? static_cast<std::int64_t>(i1 - i0 + 1) : std::int64_t{0};
  const auto nj = j1 >= j0 ? static_cast<std::int64_t>(j1 - j0 + 1) : std::int64_t{0};
  for (std::int64_t a = 0; a < ni; ++a) {
    const double i = i0 + static_cast<double>(a);
    for (std::int64_t b = 0; b < nj; ++b) {
      const double j = j0 + static_cast<double>(b);
      const auto it = map_.find(i * 1048576 + j);
      if (it == map_.end()) {
        continue;
      }
      for (const std::uint32_t k : it->second) {
        const double dx = static_cast<double>(c.pos[std::size_t{k} * 3]) - lx;
        const double dy = static_cast<double>(c.pos[std::size_t{k} * 3 + 1]) - ly;
        const double d = dx * dx + dy * dy;
        if (d <= bd) {
          bd = d;
          best = k;
        }
      }
    }
  }
  return best;
}

std::optional<std::string> class_name(int c, const i18n::Catalog& words) {
  if (c < 0 || c > 18) {
    return std::nullopt;
  }
  return words.t("las.class." + std::to_string(c));
}

Summary summary(const Cloud& c, const i18n::Catalog& words) {
  Summary s;
  s.read = c.read;
  s.kept = c.n;
  for (std::size_t a = 0; a < 3; ++a) {
    s.size.at(a) = c.max.at(a) - c.min.at(a);
  }
  for (const auto& [cls, n] : c.class_counts) {
    s.classes.push_back(
        {.c = cls,
         .name = class_name(cls, words).value_or(words.t("pc.class.other", {{"c", std::to_string(cls)}})),
         .n = n});
  }
  return s;
}

}  // namespace cc::cloud
