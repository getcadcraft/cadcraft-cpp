// SPDX-License-Identifier: Apache-2.0
// plot.js (CadCraft 0.5) in C++: sheets, the display list, the PDF writer. Byte for byte the JS's file when the page
// content is left uncompressed (tools/parity/plot.mjs); compressed, the same content goes through zlib.
#include "cc/plot/plot.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/docview/walk.hpp"
#include "cc/image/picture.hpp"
#include "cc/image/png.hpp"
#include "cc/sheets/sheets.hpp"

namespace cc::plot {

namespace {

using base::Value;
using cad::Mat2;
using cad::P2;

constexpr double kMargin = 5;  // mm round a model plot (most printers cannot print to the edge)

double num_of(const Value& e, std::string_view k) {
  return base::js_to_number(e.get(k));
}
/// `v || d` for numbers
double or_num(const Value& v, double d) {
  const double x = base::js_to_number(v);
  return v.truthy() && !std::isnan(x) ? x : d;
}

// ---- geom.js

Mat2 m_mul(const Mat2& m, const Mat2& n) {
  return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],        m[0] * n[2] + m[2] * n[3],
          m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
P2 m_apply(const Mat2& m, P2 p) {
  return {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]};
}
/// viewportMatrix(vp): paper ← model through a viewport (its twist too)
Mat2 viewport_matrix(const Value& vp) {
  const double s = num_of(vp, "s");
  const double x = num_of(vp, "x");
  const double y = num_of(vp, "y");
  const double cx = num_of(vp, "cx");
  const double cy = num_of(vp, "cy");
  const Value tw = vp.get("tw");
  if (!tw.truthy()) {
    return {s, 0, 0, s, x - cx * s, y - cy * s};
  }
  const double c = base::js_cos(base::js_to_number(tw)) * s;
  const double n = base::js_sin(base::js_to_number(tw)) * s;
  return {c, n, -n, c, x - (c * cx - n * cy), y - (n * cx + c * cy)};
}
std::vector<P2> viewport_corners(const Value& e) {
  const double hw = num_of(e, "w") / 2;
  const double hh = num_of(e, "h") / 2;
  const double x = num_of(e, "x");
  const double y = num_of(e, "y");
  return {{x - hw, y - hh}, {x + hw, y - hh}, {x + hw, y + hh}, {x - hw, y + hh}};
}
/// clipInfinite(p, d, rect, ray): the parameter range of a line inside a rectangle, or nothing
std::optional<std::pair<double, double>> clip_infinite(P2 p, P2 d, const cad::Bounds& rect, bool ray) {
  double t0 = ray ? 0 : -std::numeric_limits<double>::infinity();
  double t1 = std::numeric_limits<double>::infinity();
  const std::array<std::array<double, 4>, 2> axes{
      {{d.x, rect.min_x, rect.max_x, p.x}, {d.y, rect.min_y, rect.max_y, p.y}}};
  for (const auto& [q, lo, hi, v] : axes) {
    if (std::abs(q) < 1e-15) {
      if (v < lo || v > hi) {
        return std::nullopt;
      }
      continue;
    }
    double a = (lo - v) / q;
    double b = (hi - v) / q;
    if (a > b) {
      std::swap(a, b);
    }
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1) {
      return std::nullopt;
    }
  }
  if (std::isfinite(t0) && std::isfinite(t1)) {
    return std::pair{t0, t1};
  }
  return std::nullopt;
}
/// walk.js frozenSet(vp): the layers frozen in a viewport, upper case (VP.3 overrides are not ported)
std::set<std::string> frozen_set(const Value& vp) {
  std::set<std::string> out;
  const Value f = vp.get("frz");
  if (f.is_array()) {
    for (const Value& n : f.as_array()) {
      if (n.is_string() && !n.as_string().empty()) {
        out.insert(base::js_to_upper(n.as_string()));
      }
    }
  }
  return out;
}

// ---- numbers and colours as the PDF writes them

/// num(v): 4 decimals, trailing zeros dropped, tiny values as 0 — appended to `out`. JS's toFixed(4) is exact and
/// rounds a tie up (away from zero); std::to_chars is exact too and rounds a tie to even. At 4 decimals a double is an
/// exact tie only when v × 32 is an odd integer (v = odd / 32): those take the JS port.
/// The text of num(v) in `b`; its length.
std::size_t fmt_num(std::array<char, 64>& b, double v) {
  if (!std::isfinite(v)) {
    b[0] = '0';
    return 1;
  }
  if (std::abs(v) < 5e-5) {
    v = 0;
  }
  std::size_t n = 0;
  const double t = v * 32;
  if (std::abs(v) >= 1e15 || (t == std::floor(t) && std::fmod(std::abs(t), 2) == 1)) {
    const std::string slow = base::js_to_fixed(v, 4);
    n = std::min(slow.size(), b.size());
    std::copy_n(slow.begin(), n, b.begin());
  } else {
    const auto r = std::to_chars(b.data(), std::to_address(b.end()), v + 0.0, std::chars_format::fixed, 4);
    n = static_cast<std::size_t>(std::distance(b.data(), r.ptr));
  }
  if (std::string_view(b.data(), n).contains('.')) {
    while (n > 0 && b.at(n - 1) == '0') {
      --n;
    }
    if (n > 0 && b.at(n - 1) == '.') {
      --n;
    }
  }
  return n;
}
void put_num(std::string& out, double v) {
  std::array<char, 64> b{};
  out.append(b.data(), fmt_num(b, v));
}
std::string num(double v) {
  std::string s;
  put_num(s, v);
  return s;
}
int hex2(std::string_view h, std::size_t i) {
  int v = 0;
  for (std::size_t k = i; k < i + 2 && k < h.size(); ++k) {
    const char ch = h[k];
    int d = 0;
    if (ch >= '0' && ch <= '9') {
      d = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      d = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      d = ch - 'A' + 10;
    } else {
      return k == i ? -1 : v;  // parseInt stops at the first non-digit (NaN when there is none)
    }
    v = v * 16 + d;
  }
  return v;
}
/// rgb(hex): "r g b" in 0–1
std::string rgb(std::string_view hex) {
  std::string h;
  if (hex.size() == 4) {
    for (std::size_t i = 1; i < 4; ++i) {
      h += hex[i];
      h += hex[i];
    }
  } else {
    h = std::string(hex.substr(1, 6));
  }
  std::string out;
  for (const std::size_t i : {0U, 2U, 4U}) {
    const int v = hex2(h, i);
    out += (out.empty() ? "" : " ") + num(v < 0 ? std::nan("") : v / 255.0);
  }
  return out;
}
/// mix(a, b, t): a toward b
std::string mix(std::string_view a, std::string_view b, double t) {
  std::string out = "#";
  for (const std::size_t i : {1U, 3U, 5U}) {
    const double v = base::js_round(hex2(a, i) * (1 - t) + hex2(b, i) * t);
    static constexpr std::string_view kHex = "0123456789abcdef";
    const auto n = static_cast<int>(v);
    std::string s;
    for (int x = n; x > 0 || s.empty(); x /= 16) {
      s.insert(s.begin(), kHex[static_cast<std::size_t>(x % 16)]);
    }
    if (s.size() < 2) {
      s.insert(s.begin(), '0');
    }
    out += s;
  }
  return out;
}

// ---- font.js / Windows-1252

/// CP1252: the code points Windows-1252 puts at 0x80–0x9F
std::optional<std::uint32_t> cp1252(std::uint32_t cp) {
  static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 27> kMap{
      {{0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
       {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c}, {0x017D, 0x8e}, {0x2018, 0x91},
       {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
       {0x2122, 0x99}, {0x0161, 0x9a}, {0x203A, 0x9b}, {0x0153, 0x9c}, {0x017E, 0x9e}, {0x0178, 0x9f}}};
  for (const auto& [from, to] : kMap) {
    if (from == cp) {
      return to;
    }
  }
  return std::nullopt;
}
/// SUBST: drawn as a close Windows-1252 character (⌀ as Ø, the true minus as a hyphen …)
std::uint32_t subst(std::uint32_t cp) {
  switch (cp) {
    case 0x2300: return 0xD8;
    case 0x2212: return '-';
    case 0x2032: return '\'';
    case 0x2033: return '"';
    case '\t': return ' ';
    default: return cp;
  }
}

// ---- the page

/// pdf content operators (plot.js Ops)
class Ops {
 public:
  std::vector<std::uint8_t> buf;  // the content stream, one operator line after another
  std::vector<double> alphas;

  void reset() {
    sc_.reset();
    fc_.reset();
    ga_ = 1;
  }
  void raw(std::string_view s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back('\n');
  }
  void put(double v) {
    std::array<char, 64> b{};
    const std::size_t n = fmt_num(b, v);
    buf.insert(buf.end(), b.begin(), std::next(b.begin(), static_cast<std::ptrdiff_t>(n)));
  }
  /// "x y op": a point and its operator (m, l), without temporaries
  void at(double x, double y, std::string_view op) {
    put(x);
    buf.push_back(' ');
    put(y);
    buf.push_back(' ');
    buf.insert(buf.end(), op.begin(), op.end());
    buf.push_back('\n');
  }
  void alpha(double a) {
    a = std::isfinite(a) ? std::max(0.0, std::min(1.0, base::js_round(a * 1000) / 1000)) : 1;
    if (a == ga_) {
      return;
    }
    ga_ = a;
    auto it = std::ranges::find(alphas, a);
    std::size_t k = static_cast<std::size_t>(it - alphas.begin());
    if (it == alphas.end()) {
      k = alphas.size();
      alphas.push_back(a);
    }
    raw("/GA" + std::to_string(k) + " gs");
  }
  void stroke_color(const std::string& hex) {
    if (sc_ != hex) {
      sc_ = hex;
      raw(rgb(hex) + " RG");
    }
  }
  void fill_color(const std::string& hex) {
    if (fc_ != hex) {
      fc_ = hex;
      raw(rgb(hex) + " rg");
    }
  }
  void poly(const std::vector<P2>& pts) {
    raw(num(pts[0].x) + " " + num(pts[0].y) + " m");
    for (std::size_t i = 1; i < pts.size(); ++i) {
      raw(num(pts[i].x) + " " + num(pts[i].y) + " l");
    }
    raw("h");
  }
  void path(const std::vector<cad::Prim>& ps) {
    double lx = std::numeric_limits<double>::infinity();
    double ly = std::numeric_limits<double>::infinity();
    for (const cad::Prim& q : ps) {
      if (!q.arc) {
        if (std::abs(q.x1 - lx) > 1e-9 || std::abs(q.y1 - ly) > 1e-9) {
          at(q.x1, q.y1, "m");
        }
        at(q.x2, q.y2, "l");
        lx = q.x2;
        ly = q.y2;
      } else {
        const double from = q.rev ? q.a1 : q.a0;
        const double to = q.rev ? q.a0 : q.a1;
        const double sx = q.cx + q.r * base::js_cos(from);
        const double sy = q.cy + q.r * base::js_sin(from);
        if (std::abs(sx - lx) > 1e-9 || std::abs(sy - ly) > 1e-9) {
          at(sx, sy, "m");
        }
        for (const auto& c : arc_beziers(q.cx, q.cy, q.r, from, to)) {
          for (const double v : c) {
            put(v);
            buf.push_back(' ');
          }
          buf.push_back('c');
          buf.push_back('\n');
        }
        lx = q.cx + q.r * base::js_cos(to);
        ly = q.cy + q.r * base::js_sin(to);
      }
    }
  }
  /// One text entity (mm), Helvetica; how many characters printed as "?"
  std::size_t text(const Value& e, const std::string& color) {
    const cad::TextLayout lay = cad::text_layout(e);
    const double fs = num_of(e, "h") * cad::kFontEm;
    const double wf = or_num(e.get("wf"), 1);
    const double rot = or_num(e.get("rot"), 0);
    const double c = base::js_cos(rot);
    const double s = base::js_sin(rot);
    std::size_t missing = 0;
    fill_color(color);
    const Value ha = e.get("ha");
    const Value obl = e.get("obl");
    const double k = obl.truthy() ? fs * std::tan(base::js_to_number(obl)) : 0;
    for (std::size_t i = 0; i < lay.lines.size(); ++i) {
      const std::string& line = lay.lines[i];
      if (line.empty()) {
        continue;
      }
      const WinAnsi enc = win_ansi(line);
      missing += enc.missing;
      const double w = enc.width / 1000 * fs * wf;
      double lx = 0;
      if (ha == Value("center")) {
        lx = -w / 2;
      } else if (ha == Value("right")) {
        lx = -w;
      }
      const double ly = lay.baselines[i];
      const double x = num_of(e, "x") + lx * c - ly * s;
      const double y = num_of(e, "y") + lx * s + ly * c;
      raw("BT /F1 1 Tf " + num(fs * wf * c) + " " + num(fs * wf * s) + " " + num(k * c - fs * s) + " " +
          num(k * s + fs * c) + " " + num(x) + " " + num(y) + " Tm (" + enc.str + ") Tj ET");
    }
    return missing;
  }

 private:
  std::optional<std::string> sc_, fc_;
  double ga_ = 1;
};

/// A stroke's width on paper (mm): its lineweight, 0 as the thinnest line (0.05 mm), the default as the plot's pen.
double pen_for(double lw, double pen) {
  return lw >= 0 ? std::max(lw, 0.05) : pen;
}

struct Page {
  double w = 0, h = 0;
  std::vector<std::uint8_t> content;
  bool flate = false;
  std::size_t missing = 0, texts = 0;
  std::vector<double> alphas;
  std::string name;
  std::vector<std::size_t> xobjects;  // the images it uses, in first-use order
  std::size_t pictures = 0, shrunk = 0, kept = 0;
};
/// An image in the file (a picture used on several pages is stored once)
struct FileImage {
  image::PdfImage img;
  const doc::PictureFile* rec = nullptr;
  double max_dpi = 0;
};

/// toFixed(6) of a picture's matrix (not finite: 0)
std::string fixed6(double x) {
  return std::isfinite(x) ? base::js_to_fixed(x, 6) : std::string("0");
}

Page page_of(const DisplayList& list, double pen, const PdfOptions& o, std::vector<FileImage>& images) {
  Ops out;
  std::size_t missing = 0;
  std::size_t n_texts = 0;
  std::size_t n_pics = 0;
  std::size_t shrunk = 0;
  std::size_t kept = 0;
  std::vector<std::size_t> xobjects;
  const std::string k = base::js_to_fixed(72 / 25.4, 9);
  out.raw(k + " 0 0 " + k + " 0 0 cm");  // everything after this is in mm
  for (const ListView& v : list.views) {
    out.raw("q");
    out.reset();
    if (v.clip) {
      out.poly(*v.clip);
      out.raw("W n");
    }
    for (const docview::PictureOut& p : v.pictures) {
      const double mm_w = base::js_hypot(p.ux, p.uy) * p.w;
      const double mm_h = base::js_hypot(p.vx, p.vy) * p.h;
      const doc::PictureFile* rec = nullptr;
      for (const auto& [file, r] : list.picture_files) {
        if (file == p.file) {
          rec = r;
        }
      }
      const auto found =
          std::ranges::find_if(images, [&](const FileImage& im) { return im.rec == rec && im.max_dpi == o.max_dpi; });
      std::size_t im = static_cast<std::size_t>(found - images.begin());
      if (found == images.end()) {
        auto img = rec != nullptr ? image::pdf_image(rec->mime, rec->bytes, o.max_dpi, mm_w, mm_h) : std::nullopt;
        if (!img) {
          continue;
        }
        shrunk += img->shrunk ? 1U : 0U;
        kept += img->kept ? 1U : 0U;
        im = images.size();
        images.push_back({.img = std::move(*img), .rec = rec, .max_dpi = o.max_dpi});
      }
      if (std::ranges::find(xobjects, im) == xobjects.end()) {
        xobjects.push_back(im);
      }
      out.raw("q");
      if (p.clip.size() >= 6) {
        out.raw(num(p.clip[0]) + " " + num(p.clip[1]) + " m");
        for (std::size_t i = 2; i + 1 < p.clip.size(); i += 2) {
          out.raw(num(p.clip[i]) + " " + num(p.clip[i + 1]) + " l");
        }
        out.raw("h W n");
      }
      out.alpha(p.alpha);
      out.raw(fixed6(p.ux * p.w) + " " + fixed6(p.uy * p.w) + " " + fixed6(p.vx * p.h) + " " + fixed6(p.vy * p.h) +
              " " + fixed6(p.x) + " " + fixed6(p.y) + " cm /Im" + std::to_string(im) + " Do");
      out.raw("Q");
      out.alpha(1);
      ++n_pics;
    }
    out.raw("1 J 1 j " + num(pen) + " w");
    for (const Fill& f : v.fills) {
      out.alpha(f.alpha);
      out.fill_color(f.color);
      for (const std::vector<double>& l : f.loops) {
        if (l.size() < 6) {
          continue;
        }
        out.at(l[0], l[1], "m");
        for (std::size_t i = 2; i + 1 < l.size(); i += 2) {
          out.at(l[i], l[i + 1], "l");
        }
        out.raw("h");
      }
      out.raw("f*");
    }
    double width = pen;
    for (const Stroke& s : v.strokes) {
      const double w = pen_for(s.lw, pen);
      if (w != width) {
        out.raw(num(w) + " w");
        width = w;
      }
      out.alpha(s.alpha);
      out.stroke_color(s.color);
      out.path(s.prims);
      out.raw("S");
    }
    if (width != pen) {
      out.raw(num(pen) + " w");
    }
    out.alpha(1);
    for (const Mask& mk : v.masks) {  // over the linework, under the text (WL.8)
      const std::vector<double>& l = mk.loop;
      if (l.size() < 6) {
        continue;
      }
      out.fill_color(mk.color);
      out.at(l[0], l[1], "m");
      for (std::size_t i = 2; i + 1 < l.size(); i += 2) {
        out.at(l[i], l[i + 1], "l");
      }
      out.raw("h f");
    }
    for (const Point& p : v.points) {
      out.stroke_color(p.color);
      out.raw(num(p.x) + " " + num(p.y) + " m " + num(p.x) + " " + num(p.y) + " l S");
    }
    for (const Text& t : v.texts) {
      out.alpha(t.alpha);
      missing += out.text(t.e, t.color);
      ++n_texts;
    }
    out.raw("Q");
  }
  Page p{.w = list.w,
         .h = list.h,
         .content = {},
         .flate = false,
         .missing = missing,
         .texts = n_texts,
         .alphas = out.alphas,
         .name = {},
         .xobjects = std::move(xobjects),
         .pictures = n_pics,
         .shrunk = shrunk,
         .kept = kept};
  if (o.compress) {
    p.content = o.compress(out.buf);
    p.flate = !p.content.empty();
  }
  if (!p.flate) {
    p.content = std::move(out.buf);
  }
  return p;
}

/// "D:YYYYMMDDHHMMSSZ" of a time (UTC)
std::string pdf_date(double ms) {
  const auto t = static_cast<std::int64_t>(std::floor(ms / 1000));
  const std::int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  const std::int64_t secs = t - days * 86400;
  const std::string ymd = sheets::iso_date(static_cast<double>(days) * 86'400'000);  // YYYY-MM-DD
  const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  std::string d = "D:";
  for (const char ch : ymd) {
    if (ch != '-') {
      d += ch;
    }
  }
  return d + two(secs / 3600) + two(secs / 60 % 60) + two(secs % 60) + "Z";
}

/// assemble(pages, title): a PDF 1.4 file; with more than one page, a bookmark per page
std::vector<std::uint8_t> assemble(const std::vector<Page>& pages, const std::string& title, double date_ms,
                                   const std::vector<FileImage>& images) {
  std::vector<std::uint8_t> out;
  std::vector<std::size_t> offsets;
  const auto push = [&out](std::string_view s) { out.insert(out.end(), s.begin(), s.end()); };
  const auto obj = [&](std::size_t n, const std::vector<std::string_view>& body,
                       const std::vector<std::uint8_t>* bytes = nullptr, std::string_view tail = {}) {
    if (offsets.size() <= n) {
      offsets.resize(n + 1, 0);
    }
    offsets[n] = out.size();
    push(std::to_string(n) + " 0 obj\n");
    for (const std::string_view b : body) {
      push(b);
    }
    if (bytes != nullptr) {
      out.insert(out.end(), bytes->begin(), bytes->end());
      push(tail);
    }
    push("\nendobj\n");
  };
  const auto pt = [](double mm) { return num(mm * 72 / 25.4); };
  const std::string date = pdf_date(date_ms);
  const auto pdf_text = [](std::string_view s) { return "(" + win_ansi(s).str + ")"; };

  std::size_t room = 1U << 16U;
  for (const Page& p : pages) {
    room += p.content.size() + 1024;
  }
  for (const FileImage& im : images) {
    room += im.img.data.size() + (im.img.smask ? im.img.smask->size() : 0) + 1024;
  }
  out.reserve(room);
  push("%PDF-1.4\n");
  out.insert(out.end(), {0x25, 0xe2, 0xe3, 0xcf, 0xd3, 0x0a});  // a binary comment: "this file has 8-bit data"

  // 1 catalog, 2 pages, 3 font, 4 info, 5 outlines (more than one page), then per page: the page and its content
  const std::size_t n = pages.size();
  const bool outlines = n > 1;
  const std::size_t first = 6;
  const auto page_no = [&](std::size_t i) { return first + 2 * i; };
  const auto item_no = [&](std::size_t i) { return first + 2 * n + i; };
  // pictures (0.5 WL.13): after everything else, each image followed by its soft mask when it has one
  const std::size_t img_base = outlines ? item_no(n) : page_no(n);
  std::vector<std::size_t> img_no;
  {
    std::size_t k = img_base;
    for (const FileImage& im : images) {
      img_no.push_back(k);
      k += im.img.smask ? 2U : 1U;
    }
  }
  std::size_t total = img_base;
  if (!images.empty()) {
    total = img_no.back() + (images.back().img.smask ? 2 : 1);
  }
  const auto count = [](double v) {
    std::string s;
    base::append_number(s, v);
    return s;
  };
  obj(1, {"<< /Type /Catalog /Pages 2 0 R" + std::string(outlines ? " /Outlines 5 0 R /PageMode /UseOutlines" : "") +
          " >>"});
  std::string kids;
  for (std::size_t i = 0; i < n; ++i) {
    kids += (i ? " " : "") + std::to_string(page_no(i)) + " 0 R";
  }
  obj(2, {"<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(n) + " >>"});
  obj(3, {"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"});
  obj(4,
      {"<< /Title " + pdf_text(title) + " /Producer (CadCraft) /Creator (CadCraft) /CreationDate (" + date + ") >>"});
  obj(5, {outlines ? "<< /Type /Outlines /First " + std::to_string(item_no(0)) + " 0 R /Last " +
                         std::to_string(item_no(n - 1)) + " 0 R /Count " + std::to_string(n) + " >>"
                   : std::string("<< >>")});
  for (std::size_t i = 0; i < n; ++i) {
    const Page& p = pages[i];
    std::string gs;
    if (!p.alphas.empty()) {
      gs = " /ExtGState << ";
      for (std::size_t k = 0; k < p.alphas.size(); ++k) {
        gs += (k ? " " : "") + std::string("/GA") + std::to_string(k) + " << /Type /ExtGState /CA " + num(p.alphas[k]) +
              " /ca " + num(p.alphas[k]) + " >>";
      }
      gs += " >>";
    }
    std::string xo;
    if (!p.xobjects.empty()) {
      xo = " /XObject << ";
      for (std::size_t j = 0; j < p.xobjects.size(); ++j) {
        xo += (j ? " " : "") + std::string("/Im") + std::to_string(p.xobjects[j]) + " " +
              std::to_string(img_no[p.xobjects[j]]) + " 0 R";
      }
      xo += " >>";
    }
    std::string page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + pt(p.w) + " " + pt(p.h) +
                       "] /Resources << /Font << /F1 3 0 R >>";
    page += gs;
    page += xo;
    page += " >> /Contents " + std::to_string(page_no(i) + 1) + " 0 R >>";
    obj(page_no(i), {page});
    obj(page_no(i) + 1,
        {"<< /Length " + std::to_string(p.content.size()) + (p.flate ? " /Filter /FlateDecode" : "") + " >>\nstream\n"},
        &p.content, "\nendstream");
  }
  if (outlines) {
    for (std::size_t i = 0; i < n; ++i) {
      obj(item_no(i), {"<< /Title " + pdf_text(pages[i].name) + " /Parent 5 0 R" +
                       (i ? " /Prev " + std::to_string(item_no(i - 1)) + " 0 R" : std::string()) +
                       (i < n - 1 ? " /Next " + std::to_string(item_no(i + 1)) + " 0 R" : std::string()) + " /Dest [" +
                       std::to_string(page_no(i)) + " 0 R /Fit] >>"});
    }
  }
  for (std::size_t i = 0; i < images.size(); ++i) {
    const image::PdfImage& im = images[i].img;
    const std::size_t no = img_no[i];
    obj(no,
        {"<< /Type /XObject /Subtype /Image /Width " + count(im.w) + " /Height " + count(im.h) + " /ColorSpace /" +
         (im.colors == 1 ? "DeviceGray" : "DeviceRGB") + " /BitsPerComponent 8 /Filter /" + im.filter +
         (im.smask ? " /SMask " + std::to_string(no + 1) + " 0 R" : std::string()) + " /Length " +
         std::to_string(im.data.size()) + " >>\nstream\n"},
        &im.data, "\nendstream");
    if (im.smask) {
      obj(no + 1,
          {"<< /Type /XObject /Subtype /Image /Width " + count(im.w) + " /Height " + count(im.h) +
           " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length " +
           std::to_string(im.smask->size()) + " >>\nstream\n"},
          &*im.smask, "\nendstream");
    }
  }
  const std::size_t xref = out.size();
  std::string x = "xref\n0 " + std::to_string(total) + "\n0000000000 65535 f \n";
  for (std::size_t i = 1; i < total; ++i) {
    std::string o = std::to_string(i < offsets.size() ? offsets[i] : 0);
    x += std::string(o.size() < 10 ? 10 - o.size() : 0, '0') + o + " 00000 n \n";
  }
  push(x);
  push("trailer\n<< /Size " + std::to_string(total) + " /Root 1 0 R /Info 4 0 R >>\nstartxref\n" +
       std::to_string(xref) + "\n%%EOF\n");
  return out;
}

}  // namespace

std::vector<std::array<double, 6>> arc_beziers(double cx, double cy, double r, double a, double b) {
  const double sweep = b - a;
  const double n = std::max(1.0, std::ceil(std::abs(sweep) / (std::numbers::pi / 2) - 1e-9));
  const double d = sweep / n;
  const double k = 4.0 / 3 * std::tan(d / 4);
  std::vector<std::array<double, 6>> out;
  for (int i = 0; i < static_cast<int>(n); ++i) {
    const double t0 = a + d * i;
    const double t1 = t0 + d;
    const double c0 = base::js_cos(t0);
    const double s0 = base::js_sin(t0);
    const double c1 = base::js_cos(t1);
    const double s1 = base::js_sin(t1);
    out.push_back({cx + r * (c0 - k * s0), cy + r * (s0 + k * c0), cx + r * (c1 + k * s1), cy + r * (s1 - k * c1),
                   cx + r * c1, cy + r * s1});
  }
  return out;
}

WinAnsi win_ansi(std::string_view s) {
  WinAnsi r;
  for (std::size_t i = 0; i < s.size();) {
    const std::uint32_t ch = subst(cad::next_char(s, i));
    std::uint32_t code = ch;
    if (const auto c = cp1252(ch)) {
      code = *c;
    } else if (code > 0xff || (code >= 0x7f && code < 0xa0) || code < 0x20) {
      code = 0x3f;
      ++r.missing;
      r.width += 556;
    }
    if (code == 0x28 || code == 0x29 || code == 0x5c) {
      r.str += '\\';
      r.str += static_cast<char>(code);
    } else if (code >= 0x20 && code < 0x7f) {
      r.str += static_cast<char>(code);
    } else {
      std::string oct;
      for (std::uint32_t v = code; v > 0 || oct.empty(); v /= 8) {
        oct.insert(oct.begin(), static_cast<char>('0' + v % 8));
      }
      r.str += "\\" + std::string(oct.size() < 3 ? 3 - oct.size() : 0, '0') + oct;
    }
    if (code != 0x3f || ch == '?') {
      r.width += cad::char_width(ch);
    }
  }
  return r;
}

Sheet layout_sheet(const doc::Doc& doc, const doc::Layout& layout) {
  const Value& p = layout.paper;
  const double k = sheets::unit_mm(base::js_string(p.get("unit"))).value_or(std::nan(""));
  const Mat2 to_mm{k, 0, 0, k, -num_of(p, "x0") * k, -num_of(p, "y0") * k};
  Sheet sheet{.w = num_of(p, "w") * k, .h = num_of(p, "h") * k, .views = {}, .name = layout.name, .scale = {}};
  std::vector<const Value*> model;
  doc.model()->each([&](const doc::Id&, const Value& e) { model.push_back(&e); });
  std::vector<const Value*> paper;
  layout.entities->each([&](const doc::Id&, const Value& e) {
    paper.push_back(&e);
    if (!(e.get("type") == Value("viewport")) || e.get("on") == Value(false)) {
      return;  // a viewport turned off plots its frame only (P.7)
    }
    std::vector<P2> clip;
    for (const P2 q : viewport_corners(e)) {
      clip.push_back(m_apply(to_mm, q));
    }
    // the layers frozen in this viewport are left out of its view (VP.1)
    sheet.views.push_back(
        {.clip = std::move(clip), .m = m_mul(to_mm, viewport_matrix(e)), .entities = model, .frozen = frozen_set(e)});
  });
  sheet.views.push_back({.clip = std::nullopt, .m = to_mm, .entities = std::move(paper), .frozen = {}});
  return sheet;
}

Sheet model_sheet(const doc::Doc& doc, const cad::Bounds& area, std::string_view paper, bool portrait,
                  std::optional<double> n) {
  const Value pp = sheets::make_paper(paper, portrait);
  const double k = sheets::unit_mm(base::js_string(pp.get("unit"))).value_or(std::nan(""));
  const double big_w = num_of(pp, "w") * k;
  const double big_h = num_of(pp, "h") * k;
  const double aw = std::max(area.max_x - area.min_x, 1e-9);
  const double ah = std::max(area.max_y - area.min_y, 1e-9);
  const double s_fit = std::min((big_w - 2 * kMargin) / aw, (big_h - 2 * kMargin) / ah);
  const double s = n && *n != 0 ? sheets::s_of(doc, "mm", *n) : s_fit;
  const double cx = (area.min_x + area.max_x) / 2;
  const double cy = (area.min_y + area.max_y) / 2;
  const Mat2 m{s, 0, 0, s, big_w / 2 - cx * s, big_h / 2 - cy * s};
  std::vector<P2> clip{
      {kMargin, kMargin}, {big_w - kMargin, kMargin}, {big_w - kMargin, big_h - kMargin}, {kMargin, big_h - kMargin}};
  std::vector<const Value*> model;
  doc.model()->each([&](const doc::Id&, const Value& e) { model.push_back(&e); });
  const Value nm = doc.meta.get("name");
  const double nn = sheets::n_of(doc, "mm", s);
  const auto fmt4 = [](double v) {  // Number(v.toPrecision(4)).toString()
    std::string out;
    base::append_number(out, base::js_number(base::js_to_exponential(v, 3)));
    return out;
  };
  std::string label;
  if (n && *n != 0) {
    label = sheets::scale_label(doc, "mm", s);
  } else {
    label = (nn >= 1 ? "1:" + fmt4(nn) : fmt4(1 / nn) + ":1") + " (fitted)";
  }
  Sheet sheet{.w = big_w,
              .h = big_h,
              .views = {},
              .name = nm.truthy() ? base::js_string(nm) : std::string("Drawing1"),
              .scale = ScaleInfo{.s = s, .n = nn, .label = std::move(label), .fits = s <= s_fit * (1 + 1e-9)}};
  sheet.views.push_back({.clip = std::move(clip), .m = m, .entities = std::move(model), .frozen = {}});
  return sheet;
}

DisplayList display_list(const doc::Doc& doc, const Sheet& sheet, bool mono) {
  const auto ink = [mono](const std::string& c) { return mono || c == "ink" ? std::string("#000000") : c; };
  DisplayList list{.w = sheet.w, .h = sheet.h, .name = sheet.name, .views = {}, .picture_files = {}};
  for (const View& v : sheet.views) {
    ListView lv;
    lv.clip = v.clip;
    // construction lines (0.5 DR.7) are clipped to what this view prints: its clip, else the sheet
    std::optional<cad::Bounds> area;
    if (v.clip) {
      cad::Bounds b;
      for (const P2 q : *v.clip) {
        b.min_x = std::min(b.min_x, q.x);
        b.min_y = std::min(b.min_y, q.y);
        b.max_x = std::max(b.max_x, q.x);
        b.max_y = std::max(b.max_y, q.y);
      }
      area = b;
    } else if (sheet.w > 0 && sheet.h > 0) {
      area = cad::Bounds{.min_x = 0, .min_y = 0, .max_x = sheet.w, .max_y = sheet.h};
    }
    const auto alpha_of = [](const docview::Opts& o) {
      return o.alpha && std::isfinite(*o.alpha) ? std::max(0.0, std::min(1.0, *o.alpha)) : 1.0;
    };
    std::map<std::tuple<std::string, double, double>, std::size_t> index;
    const auto stroke_list = [&](const std::string& color, double lw, double a) -> std::vector<cad::Prim>& {
      const double l = lw >= 0 ? lw + 0.0 : -3;  // +0.0: -0 groups with 0, as the JS's string key does
      const auto key = std::tuple{color, l, a};
      auto it = index.find(key);
      if (it == index.end()) {
        it = index.emplace(key, lv.strokes.size()).first;
        lv.strokes.push_back({.color = color, .lw = l, .alpha = a, .prims = {}});
      }
      return lv.strokes[it->second].prims;
    };
    docview::Out out;
    out.stroke = [&](const std::string& color, const std::vector<cad::Prim>& ps, double lw, const docview::Opts& o) {
      std::vector<cad::Prim>& l = stroke_list(ink(color), lw, alpha_of(o));
      l.insert(l.end(), ps.begin(), ps.end());
    };
    out.fill = [&](const std::string& color, const std::vector<std::vector<double>>& loops, bool solid,
                   const docview::Opts& o) {
      const double a = alpha_of(o);
      if (solid) {
        lv.fills.push_back(
            {.color = o.wipe ? std::string("#ffffff") : ink(color), .loops = loops, .alpha = o.wipe ? 1.0 : a});
        return;
      }
      // a patterned hatch plots as a light tone of its colour, with its outline
      lv.fills.push_back({.color = mix(ink(color), "#ffffff", 0.82), .loops = loops, .alpha = a});
      std::vector<cad::Prim>& l2 = stroke_list(ink(color), -3, a);
      for (const std::vector<double>& l : loops) {
        for (std::size_t i = 0; i + 3 < l.size(); i += 2) {
          cad::Prim q;
          q.x1 = l[i];
          q.y1 = l[i + 1];
          q.x2 = l[i + 2];
          q.y2 = l[i + 3];
          l2.push_back(q);
        }
      }
    };
    out.text = [&](const std::string& color, const Value& e, const docview::Opts& o) {
      lv.texts.push_back({.color = ink(color), .e = e, .alpha = alpha_of(o)});
    };
    // a text's background mask (WL.8): the paper's white, or its own colour on a colour plot
    out.mask = [&](const std::string& color, const std::vector<double>& loop, bool wipe) {
      lv.masks.push_back({.color = wipe || mono ? std::string("#ffffff") : ink(color), .loop = loop});
    };
    out.point = [&](const std::string& color, double x, double y) {
      lv.points.push_back({.color = ink(color), .x = x, .y = y});
    };
    out.infinite = [&](const std::string& color, const Value& e, double lw, const docview::Opts& o) {
      if (!area) {
        return;
      }
      const P2 p{num_of(e, "x"), num_of(e, "y")};
      const P2 d{num_of(e, "dx"), num_of(e, "dy")};
      const auto tt = clip_infinite(p, d, *area, e.get("type") == Value("ray"));
      if (tt && tt->second > tt->first) {
        cad::Prim q;
        q.x1 = p.x + d.x * tt->first;
        q.y1 = p.y + d.y * tt->first;
        q.x2 = p.x + d.x * tt->second;
        q.y2 = p.y + d.y * tt->second;
        stroke_list(ink(color), lw, alpha_of(o)).push_back(q);
      }
    };
    // 0.5 WL.13/14: only pictures whose file CadCraft has
    out.picture = [&](const docview::PictureOut& p) {
      const doc::PictureFile* rec = doc::find_picture(doc, p.file);
      if (rec == nullptr) {
        return;
      }
      lv.pictures.push_back(p);
      for (auto& [file, r] : list.picture_files) {
        if (file == p.file) {
          r = rec;
          return;
        }
      }
      list.picture_files.emplace_back(p.file, rec);
    };
    docview::walk(doc, v.entities, out, {.m = v.m, .plot = true, .frozen = v.frozen});
    list.views.push_back(std::move(lv));
  }
  return list;
}

PdfResult to_pdf(const DisplayList& list, const PdfOptions& o) {
  std::vector<Page> pages;
  std::vector<FileImage> images;
  pages.push_back(page_of(list, o.pen, o, images));
  const Page& page = pages.front();
  PdfResult r{.bytes = {}, .missing = page.missing, .texts = page.texts, .pages = 1, .compressed = page.flate};
  r.pictures = page.pictures;
  r.shrunk = page.shrunk;
  r.kept = page.kept;
  r.bytes = assemble(pages, o.title ? *o.title : list.name, o.date_ms, images);
  return r;
}

PdfResult to_pdf_pages(const std::vector<DisplayList>& lists, const PdfOptions& o,
                       const std::vector<std::string>& names) {
  std::vector<Page> pages;
  std::vector<FileImage> images;
  PdfResult r;
  for (std::size_t i = 0; i < lists.size(); ++i) {
    Page p = page_of(lists[i], o.pen, o, images);
    if (i < names.size() && !names[i].empty()) {
      p.name = names[i];
    } else {
      p.name = lists[i].name.empty() ? "Page " + std::to_string(i + 1) : lists[i].name;
    }
    r.missing += p.missing;
    r.texts += p.texts;
    r.compressed = p.flate;
    pages.push_back(std::move(p));
  }
  r.bytes = assemble(pages, o.title ? *o.title : std::string("Drawing"), o.date_ms, images);
  r.pages = pages.size();
  return r;
}

std::string pdf_canonical(std::span<const std::uint8_t> pdf) {
  // tools/parity/commands.mjs pdfCanonical: /<<([^<>]*)>>\nstream\n/ → the stream's bytes, inflated when Flate
  const std::string text(pdf.begin(), pdf.end());  // the bytes as Latin-1
  const std::string_view s(text);
  std::string out;
  std::size_t at = 0;
  const std::size_t xr = s.rfind("\nxref\n");  // the table (not startxref's)
  const std::size_t end = xr == std::string_view::npos ? s.size() : xr + 1;
  while (at < end) {
    const std::size_t st = s.find(">>\nstream\n", at);
    if (st == std::string_view::npos || st >= end) {
      break;
    }
    const std::size_t open = s.rfind("<<", st);
    if (open == std::string_view::npos || open < at ||
        s.substr(open + 2, st - open - 2).find_first_of("<>") != std::string_view::npos) {
      out += s.substr(at, st + 10 - at);
      at = st + 10;
      continue;
    }
    std::string_view dict = s.substr(open + 2, st - open - 2);
    const std::size_t lp = dict.find("/Length ");
    std::size_t len = 0;
    if (lp != std::string_view::npos) {
      std::size_t j = lp + 8;
      while (j < dict.size() && dict[j] >= '0' && dict[j] <= '9') {
        len = len * 10 + static_cast<std::size_t>(dict[j] - '0');
        ++j;
      }
    }
    const std::size_t body = st + 10;
    if (body + len > s.size()) {
      break;
    }
    std::string d(dict);
    if (lp != std::string_view::npos) {
      std::size_t j = lp + 8;
      while (j < d.size() && d[j] >= '0' && d[j] <= '9') {
        ++j;
      }
      d.replace(lp, j - lp, "/Length *");
    }
    out += s.substr(at, open - at);
    out += "<<" + d + ">>\nstream\n";
    const auto bytes = pdf.subspan(body, len);
    if (d.contains("/Filter /FlateDecode")) {
      const auto inflated = image::zlib_inflate(bytes, std::size_t{1} << 30U);
      out += inflated ? "#inflated " + base::base64(*inflated) : std::string("#bad-flate");
    } else {
      out += "#raw " + base::base64(bytes);
    }
    at = body + len;
  }
  out += s.substr(at, end > at ? end - at : 0);
  // Latin-1 → UTF-8, as the harness's latin1 string is written to JSON
  std::string utf8;
  utf8.reserve(out.size());
  for (const char c : out) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x80) {
      utf8 += c;
    } else {
      utf8 += static_cast<char>(0xC0U | (u >> 6U));
      utf8 += static_cast<char>(0x80U | (u & 0x3FU));
    }
  }
  return utf8;
}

void script_pdf(const Value& plot, const doc::Doc& doc, double date_ms, Value& result) {
  const auto ext = extents_of(doc);
  if (!ext) {
    return;
  }
  const Value mono = plot.get("mono");
  const Value dpi = plot.get("maxDpi");
  PdfOptions o;
  o.date_ms = date_ms;
  o.compress = [](std::span<const std::uint8_t> b) { return image::zlib_compress(b); };
  o.max_dpi = dpi.is_number() && dpi.as_number() > 0 ? dpi.as_number() : 0;
  const PdfResult r = to_pdf(display_list(doc, model_sheet(doc, *ext), mono.is_bool() ? mono.as_bool() : true), o);
  result.set("pdf", pdf_canonical(r.bytes));
  result.set("pdfReport", base::make_object({{"pictures", static_cast<double>(r.pictures)},
                                             {"shrunk", static_cast<double>(r.shrunk)},
                                             {"kept", static_cast<double>(r.kept)}}));
}

std::optional<cad::Bounds> extents_of(const doc::Doc& doc) {
  cad::Bounds b;
  std::vector<const Value*> model;
  doc.model()->each([&](const doc::Id&, const Value& e) { model.push_back(&e); });
  const DisplayList list = display_list(
      doc,
      Sheet{
          .w = 0, .h = 0, .views = {{.clip = {}, .m = {}, .entities = model, .frozen = {}}}, .name = {}, .scale = {}});
  const ListView& v = list.views.front();
  const auto grow = [&b](double x, double y) {
    b.min_x = std::min(b.min_x, x);
    b.min_y = std::min(b.min_y, y);
    b.max_x = std::max(b.max_x, x);
    b.max_y = std::max(b.max_y, y);
  };
  for (const Stroke& s : v.strokes) {
    for (const cad::Prim& q : s.prims) {
      cad::grow_prim(q, b);
    }
  }
  for (const Fill& f : v.fills) {
    for (const std::vector<double>& l : f.loops) {
      for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
        grow(l[i], l[i + 1]);
      }
    }
  }
  for (const Text& t : v.texts) {
    for (const P2 p : cad::text_corners(t.e)) {
      grow(p.x, p.y);
    }
  }
  for (const Point& p : v.points) {
    grow(p.x, p.y);
  }
  return b.valid() ? std::optional(b) : std::nullopt;
}

}  // namespace cc::plot
