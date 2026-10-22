// SPDX-License-Identifier: Apache-2.0
// lib/skp vff.js: SketchUp 2021+ files (the header, then a ZIP archive), ported. Format knowledge in cc-skp from
// OpenSKP (MIT, see NOTICE), checked against the corpus; this follows cc-skp's JavaScript.
//
// model.dat records: tag (2 bytes, read big-endian so 0xC409 is the bytes C4 09), length (u32 LE), payload. Only the
// containers on known paths are entered, so a crafted file cannot make the reader recurse (vff.js's header lists the
// paths). Ids are little-endian unsigned integers as long as their record (1–6 bytes).
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"
#include "common.hpp"
#include "readers.hpp"
#include "zip.hpp"

namespace cc::skp::detail {

namespace {

// Record tags (bytes as written in the file); compared with the u16 each record starts with, hence a plain enum.
enum Tag : std::uint16_t {  // NOLINT(cppcoreguidelines-use-enum-class)
  FILE_ = 0xf401,
  MATS = 0xf701,
  MATS2 = 0xd430,
  MATS3 = 0xd530,
  MAT = 0xc832,
  MAT_NAME = 0xcc32,
  LAYERS = 0xf801,
  LAYERS2 = 0x983a,
  LAYERS3 = 0x993a,
  LAYER = 0x8c3c,
  LAYER_NAME = 0x8d3c,
  LAYER_HIDDEN = 0x8e3c,
  LAYER_MAT = 0x8f3c,
  DEFS = 0xf901,
  DEFS2 = 0x7017,
  DEFS3 = 0x7117,
  DEF = 0x7c15,
  DEF_GUID = 0x7d15,
  DEF_NAME = 0x7e15,
  DEF_KIND = 0x8315,
  ROOT = 0xf601,
  ENTS = 0x8813,
  SCENES = 0x0702,
  SCENES2 = 0x6d60,
  SCENES3 = 0x6d61,
  SCENE = 0x7148,
  INFO = 0xd007,
  IDBOX = 0xdc05,
  ID = 0xde05,
  ATTRS = 0xdd05,
  MAT_REF = 0xd107,
  LAYER_REF = 0xd207,
  FLAGS = 0xd307,
  VERTS = 0x8913,
  VERT = 0xc409,
  XYZ = 0xc509,
  EDGES = 0x8a13,
  EDGE = 0xb80b,
  EDGE_A = 0xb90b,
  EDGE_B = 0xba0b,
  FACES = 0x8b13,
  FACE = 0xac0d,
  NORMAL = 0xad0d,
  LOOPS = 0xae0d,
  LOOP = 0x9411,
  COEDGES = 0x9511,
  COEDGE = 0xa00f,
  CO_EDGE = 0xa10f,
  CO_DIR = 0xa20f,
  BACK_MAT = 0xaf0d,
  COMPS = 0x8c13,
  GROUPS = 0x8d13,
  GROUP = 0x4c1d,
  IMAGES = 0x9013,
  IMAGE = 0x401f,
  INST = 0x6419,
  INST_NAME = 0x6519,
  INST_M = 0x6619,
  INST_DEF = 0x6719,
  CURVES = 0x9713,
  CURVES2 = 0x9613,
  GUIDE_LINES = 0x9113,
  GUIDE_POINTS = 0x9213,
  ANNOTATIONS = 0x9913,
};
constexpr std::uint8_t kFlagHidden = 0x01;
constexpr std::uint8_t kFlagSoft = 0x08;
constexpr std::uint8_t kFlagSmooth = 0x10;
constexpr double kNewestKnown = 26;

bool quiet(std::uint16_t t) {
  return t == INFO || t == 0x8e13 || t == 0x9b13 || t == 0x9e13 || t == 0x9f13 || t == 0xa013 || t == CURVES ||
         t == CURVES2;
}

std::string tag_hex(std::uint16_t t) {
  static constexpr std::string_view kDigits = "0123456789ABCDEF";
  std::string s(4, '0');
  for (int i = 3; i >= 0; --i) {
    s[static_cast<std::size_t>(i)] = kDigits[t & 15U];
    t = static_cast<std::uint16_t>(t >> 4U);
  }
  return s;
}

using Range = std::pair<std::size_t, std::size_t>;

/// vff.js utf8(): UTF-8 when it is (TextDecoder fatal), else Latin-1; NULs removed, then trimmed.
std::string text_of(std::span<const std::uint8_t> raw) {
  std::string s;
  if (utf8_valid(raw)) {
    std::size_t at = 0;
    if (raw.size() >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
      at = 3;
    }
    s.assign(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.end());
  } else {
    s = latin1(raw);
  }
  std::erase(s, '\0');
  return std::string(base::js_trim(s));
}

/// Model.dat walker: bounds-checked record access.
class Tlv {
 public:
  Tlv(std::span<const std::uint8_t> b, const i18n::Catalog* cat) : b_(b), cat_(cat) {}

  /// fn(tag, start, end) → false stops. Strict: a record running past e = damaged.
  template <class F>
  void each(std::size_t s, std::size_t e, const F& fn) const {
    std::size_t p = s;
    while (p < e) {
      if (e - p < 6) {
        damaged(cat_, "ccskp.damaged.record_header");
      }
      const std::uint32_t n = u32(p + 2);
      if (n > e - p - 6) {
        damaged(cat_, "ccskp.damaged.record_past");
      }
      if (!fn(tag(p), p + 6, p + 6 + n)) {
        return;
      }
      p += 6 + n;
    }
  }
  /// Lenient walk (optional data): stops quietly at the first record that does not fit.
  template <class F>
  void each_loose(std::size_t s, std::size_t e, const F& fn) const {
    std::size_t p = s;
    while (p + 6 <= e) {
      const std::uint32_t n = u32(p + 2);
      if (n > e - p - 6) {
        return;
      }
      if (!fn(tag(p), p + 6, p + 6 + n)) {
        return;
      }
      p += 6 + n;
    }
  }
  [[nodiscard]] std::optional<Range> find(std::size_t s, std::size_t e, std::uint16_t want, bool loose = false) const {
    std::optional<Range> hit;
    const auto take = [&](std::uint16_t t, std::size_t a, std::size_t z) {
      if (t == want) {
        hit = Range{a, z};
        return false;
      }
      return true;
    };
    if (loose) {
      each_loose(s, e, take);
    } else {
      each(s, e, take);
    }
    return hit;
  }
  /// Little-endian unsigned id of 1–6 bytes.
  [[nodiscard]] std::uint64_t uint(std::size_t s, std::size_t e) const {
    const std::size_t n = e - s;
    if (n < 1 || n > 6) {
      damaged(cat_, "ccskp.damaged.id_size");
    }
    std::uint64_t v = 0;
    for (std::size_t i = n; i-- > 0;) {
      v = v * 256U + b_[s + i];
    }
    return v;
  }
  [[nodiscard]] double f64(std::size_t p) const { return Reader::f64_at(b_, p); }
  [[nodiscard]] std::string text(std::size_t s, std::size_t e) const { return text_of(b_.subspan(s, e - s)); }
  /// An entity's own id: DC05 > DE05 inside [s, e).
  [[nodiscard]] std::optional<std::uint64_t> id_of(std::size_t s, std::size_t e) const {
    const auto box = find(s, e, IDBOX);
    if (!box) {
      return std::nullopt;
    }
    const auto id = find(box->first, box->second, ID, true);
    if (!id) {
      return std::nullopt;
    }
    return uint(id->first, id->second);
  }
  [[nodiscard]] std::uint8_t at(std::size_t p) const { return b_[p]; }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const { return b_; }

 private:
  [[nodiscard]] std::uint16_t tag(std::size_t p) const { return static_cast<std::uint16_t>((b_[p] << 8U) | b_[p + 1]); }
  [[nodiscard]] std::uint32_t u32(std::size_t p) const {
    return static_cast<std::uint32_t>(b_[p]) | (static_cast<std::uint32_t>(b_[p + 1]) << 8U) |
           (static_cast<std::uint32_t>(b_[p + 2]) << 16U) | (static_cast<std::uint32_t>(b_[p + 3]) << 24U);
  }
  std::span<const std::uint8_t> b_;
  const i18n::Catalog* cat_;
};

std::optional<std::string> guid_text(std::span<const std::uint8_t> b) {
  if (b.size() != 16) {
    return std::nullopt;
  }
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string h;
  for (const std::uint8_t c : b) {
    h += kDigits[c >> 4U];
    h += kDigits[c & 15U];
  }
  return h.substr(0, 8) + '-' + h.substr(8, 4) + '-' + h.substr(12, 4) + '-' + h.substr(16, 4) + '-' + h.substr(20);
}

// ------------------------------------------------------------------------------------------------ material.xml
bool word_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/// unxml: &lt; &gt; &amp; &apos; &quot; &#123; &#x1F; (a code point past 0x10FFFF stays as written).
std::string unxml(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    const std::size_t semi = s.find(';', i + 1);
    bool done = false;
    if (semi != std::string_view::npos) {
      const std::string_view k = s.substr(i + 1, semi - i - 1);
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kNamed = {
          {{"lt", "<"}, {"gt", ">"}, {"amp", "&"}, {"apos", "'"}, {"quot", "\""}}};
      for (const auto& [name, rep] : kNamed) {
        if (k == name) {
          out += rep;
          done = true;
        }
      }
      if (!done && k.size() >= 2 && k[0] == '#') {
        const bool hex = k[1] == 'x';
        const std::string_view digits = k.substr(hex ? 2 : 1);
        const bool ok = !digits.empty() && std::ranges::all_of(digits, [hex](char c) {
          return (c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        });
        if (ok) {
          double cp = 0;  // parseInt: exact while small, then too big either way
          for (const char c : digits) {
            const int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
            cp = cp * (hex ? 16 : 10) + d;
          }
          if (cp >= 0 && cp <= 0x10ffff) {
            base::append_code_point(out, static_cast<std::uint32_t>(cp));
          } else {
            out += s.substr(i, semi - i + 1);
          }
          done = true;
        }
      }
    }
    if (done) {
      i = semi + 1;
    } else {
      out += s[i++];
    }
  }
  return out;
}

/// The attributes of the first <prefix:name …> element (attribute values entity-decoded), as vff.js xmlElement.
std::optional<std::map<std::string, std::string>> xml_element(std::string_view text, std::string_view name) {
  for (std::size_t lt = text.find('<'); lt != std::string_view::npos; lt = text.find('<', lt + 1)) {
    // (?:[A-Za-z0-9_]+:)? then the name, then \b, then [^>]*>
    std::vector<std::size_t> starts;
    std::size_t run = lt + 1;
    while (run < text.size() && word_char(text[run])) {
      ++run;
    }
    if (run > lt + 1 && run < text.size() && text[run] == ':') {
      starts.push_back(run + 1);
    }
    starts.push_back(lt + 1);
    for (const std::size_t at : starts) {
      if (text.substr(at, name.size()) != name) {
        continue;
      }
      const std::size_t after = at + name.size();
      if (after < text.size() && word_char(text[after])) {
        continue;
      }
      const std::size_t gt = text.find('>', after);
      if (gt == std::string_view::npos) {
        continue;
      }
      const std::string_view body = text.substr(after, gt - after);
      std::map<std::string, std::string> attrs;
      // /([A-Za-z_][\w:.-]*)\s*=\s*(?:"([^"]*)"|'([^']*)')/g
      std::size_t p = 0;
      while (p < body.size()) {
        const char c0 = body[p];
        if ((c0 < 'A' || c0 > 'Z') && (c0 < 'a' || c0 > 'z') && c0 != '_') {
          ++p;
          continue;
        }
        std::size_t q = p + 1;
        while (q < body.size() && (word_char(body[q]) || body[q] == ':' || body[q] == '.' || body[q] == '-')) {
          ++q;
        }
        std::size_t r = q;
        while (r < body.size()) {
          const std::size_t w = base::js_space_at(body, r);
          if (w == 0) {
            break;
          }
          r += w;
        }
        if (r >= body.size() || body[r] != '=') {
          ++p;
          continue;
        }
        ++r;
        while (r < body.size()) {
          const std::size_t w = base::js_space_at(body, r);
          if (w == 0) {
            break;
          }
          r += w;
        }
        if (r >= body.size() || (body[r] != '"' && body[r] != '\'')) {
          ++p;
          continue;
        }
        const char quote = body[r];
        const std::size_t close = body.find(quote, r + 1);
        if (close == std::string_view::npos) {
          ++p;
          continue;
        }
        attrs[std::string(body.substr(p, q - p))] = unxml(body.substr(r + 1, close - r - 1));
        p = close + 1;
      }
      return attrs;
    }
  }
  return std::nullopt;
}

struct XmlTexture {
  std::string file;
  std::optional<double> width;
  std::optional<double> height;
  std::string path;
};
struct XmlMaterial {
  std::optional<std::string> name;
  std::optional<Rgb> color;
  double alpha = 1;
  std::optional<XmlTexture> texture;
  std::string folder;
};

const std::string* attr(const std::map<std::string, std::string>& m, const char* k) {
  const auto it = m.find(k);
  return it == m.end() ? nullptr : &it->second;
}

std::optional<XmlMaterial> read_material_xml(std::string_view text) {
  const auto mat = xml_element(text, "material");
  if (!mat) {
    return std::nullopt;
  }
  const auto int_of = [](const std::string* v) -> std::optional<int> {
    if (v == nullptr) {
      return std::nullopt;
    }
    const double n = base::js_parse_int(*v);
    if (!std::isfinite(n)) {
      return std::nullopt;
    }
    return static_cast<int>(std::clamp(n, 0.0, 255.0));
  };
  const auto r = int_of(attr(*mat, "colorRed"));
  const auto g = int_of(attr(*mat, "colorGreen"));
  const auto b = int_of(attr(*mat, "colorBlue"));
  XmlMaterial out;
  if (const std::string* nm = attr(*mat, "name")) {
    out.name = *nm;
  }
  if (r && g && b) {
    out.color = Rgb{*r, *g, *b};
  }
  if (const std::string* u = attr(*mat, "useTrans"); u != nullptr && *u == "1") {
    const std::string* tr = attr(*mat, "trans");
    const double t = tr == nullptr ? std::numeric_limits<double>::quiet_NaN() : base::js_parse_float(*tr);
    if (std::isfinite(t)) {
      out.alpha = std::clamp(1 - t, 0.0, 1.0);
    }
  }
  const auto tex = xml_element(text, "texture");
  const auto img = xml_element(text, "image");
  const auto num = [](const std::string* v) -> std::optional<double> {
    if (v == nullptr) {
      return std::nullopt;
    }
    const double n = base::js_parse_float(*v);
    if (std::isfinite(n) && n > 0 && n < 1e9) {
      return n;
    }
    return std::nullopt;
  };
  if (tex) {
    XmlTexture t;
    if (const std::string* f = attr(*tex, "textureFilename")) {
      t.file = *f;
    }
    t.width = num(attr(*tex, "xScale"));
    t.height = num(attr(*tex, "yScale"));
    if (img) {
      const std::string* p = attr(*img, "path");
      const std::string* fn = attr(*img, "file_name");
      if (p != nullptr && !p->empty()) {
        t.path = *p;
      } else if (fn != nullptr) {
        t.path = *fn;
      }
    }
    out.texture = std::move(t);
  }
  return out;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) {
      return false;
    }
  }
  return true;
}

/// /\/thumbnail\.[a-z]+$/i
bool thumbnail_name(std::string_view n) {
  const std::size_t dot = n.rfind('.');
  if (dot == std::string_view::npos || dot + 1 >= n.size()) {
    return false;
  }
  for (std::size_t i = dot + 1; i < n.size(); ++i) {
    if (!std::isalpha(static_cast<unsigned char>(n[i]))) {
      return false;
    }
  }
  const std::string_view head = n.substr(0, dot);
  return ends_with_ci(head, "/thumbnail");
}

std::string sniff_mime(std::span<const std::uint8_t> b, std::string_view name) {
  const auto at = [&b](std::size_t i) { return b[i]; };
  if (b.size() >= 8 && at(0) == 0x89 && at(1) == 0x50 && at(2) == 0x4e && at(3) == 0x47) {
    return "image/png";
  }
  if (b.size() >= 3 && at(0) == 0xff && at(1) == 0xd8 && at(2) == 0xff) {
    return "image/jpeg";
  }
  if (b.size() >= 6 && at(0) == 0x47 && at(1) == 0x49 && at(2) == 0x46) {
    return "image/gif";
  }
  if (b.size() >= 2 && at(0) == 0x42 && at(1) == 0x4d) {
    return "image/bmp";
  }
  if (b.size() >= 4 &&
      ((at(0) == 0x49 && at(1) == 0x49 && at(2) == 0x2a) || (at(0) == 0x4d && at(1) == 0x4d && at(3) == 0x2a))) {
    return "image/tiff";
  }
  if (b.size() >= 12 && at(8) == 0x57 && at(9) == 0x45 && at(10) == 0x42 && at(11) == 0x50) {
    return "image/webp";
  }
  if (ends_with_ci(name, ".png")) {
    return "image/png";
  }
  if (ends_with_ci(name, ".jpg") || ends_with_ci(name, ".jpeg")) {
    return "image/jpeg";
  }
  return "application/octet-stream";
}

/// Normalise a path inside the archive; nullopt when it climbs out.
std::optional<std::string> join_path(std::string_view folder, std::string_view rel) {
  std::vector<std::string> parts;
  const auto split = [&parts](std::string_view s) {
    std::size_t a = 0;
    for (;;) {
      const std::size_t b = s.find('/', a);
      parts.emplace_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
      if (b == std::string_view::npos) {
        break;
      }
      a = b + 1;
    }
  };
  if (!folder.empty()) {
    split(folder);
  }
  std::string r(rel);
  std::ranges::replace(r, '\\', '/');
  split(r);
  std::vector<std::string> out;
  for (const std::string& p : parts) {
    if (p.empty() || p == ".") {
      continue;
    }
    if (p == "..") {
      if (out.empty()) {
        return std::nullopt;
      }
      out.pop_back();
    } else {
      out.push_back(p);
    }
  }
  std::string s;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (i != 0) {
      s += '/';
    }
    s += out[i];
  }
  return s;
}

/// The last part of a path split on / and \ (JS split(/[\\/]/).pop()).
std::string_view base_name(std::string_view s) {
  const std::size_t at = s.find_last_of("/\\");
  return at == std::string_view::npos ? s : s.substr(at + 1);
}

// ------------------------------------------------------------------------------------------------ units
std::optional<std::string> read_units(std::span<const std::uint8_t> meta) {
  const auto u32 = [&meta](std::size_t p) {
    return static_cast<std::uint32_t>(meta[p]) | (static_cast<std::uint32_t>(meta[p + 1]) << 8U) |
           (static_cast<std::uint32_t>(meta[p + 2]) << 16U) | (static_cast<std::uint32_t>(meta[p + 3]) << 24U);
  };
  std::size_t s = 0;
  std::size_t e = meta.size();
  for (int depth = 0; depth < 2;) {
    std::size_t p = s;
    bool entered = false;
    while (p + 6 <= e) {
      const auto tag = static_cast<std::uint16_t>((meta[p] << 8U) | meta[p + 1]);
      const std::uint32_t n = u32(p + 2);
      if (n > e - p - 6) {
        return std::nullopt;
      }
      if (tag == 0x6400 && depth == 0) {
        s = p + 6;
        e = p + 6 + n;
        depth = 1;
        entered = true;
        break;
      }
      if (tag == 0x6d00 && depth == 1) {
        return text_of(meta.subspan(p + 6, n));
      }
      p += 6 + n;
    }
    if (!entered) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> unit_word(std::string_view w) {
  const auto starts_ci = [&w](std::string_view p) {
    if (w.size() < p.size()) {
      return false;
    }
    return std::ranges::equal(w.substr(0, p.size()), p, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
    });
  };
  if (starts_ci("inch")) {
    return "in";
  }
  if (starts_ci("feet") || starts_ci("foot")) {
    return "ft";
  }
  if (starts_ci("milli")) {
    return "mm";
  }
  if (starts_ci("centi")) {
    return "cm";
  }
  if (starts_ci("meter") || starts_ci("metre")) {
    return "m";
  }
  return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ geometry
P3 newell(const std::vector<P3>& loop) {
  double nx = 0;
  double ny = 0;
  double nz = 0;
  const std::size_t n = loop.size();
  for (std::size_t i = 0; i < n; ++i) {
    const P3& a = loop[i];
    const P3& b = loop[(i + 1) % n];
    nx += (a[1] - b[1]) * (a[2] + b[2]);
    ny += (a[2] - b[2]) * (a[0] + b[0]);
    nz += (a[0] - b[0]) * (a[1] + b[1]);
  }
  return {nx, ny, nz};
}

P2 face_uv(const P3& p, const P3& n, const std::optional<std::array<double, 9>>& place, double tw, double th) {
  P3 xr;
  P3 yr;
  const double cl = base::js_hypot(n[0], n[1]);
  if (cl < 1e-9) {
    xr = {1, 0, 0};
    yr = {0, n[2] >= 0 ? 1.0 : -1.0, 0};
  } else {
    xr = {-n[1] / cl, n[0] / cl, 0};
    yr = {n[1] * xr[2] - n[2] * xr[1], n[2] * xr[0] - n[0] * xr[2], n[0] * xr[1] - n[1] * xr[0]};
  }
  const double px = p[0] * xr[0] + p[1] * xr[1] + p[2] * xr[2];
  const double py = p[0] * yr[0] + p[1] * yr[1] + p[2] * yr[2];
  if (!place) {
    return {px / tw, py / th};
  }
  const auto& [a, b, c, d, e, f, g, h, i] = *place;
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (!(std::fabs(det) > 1e-12)) {
    return {px / tw, py / th};
  }
  const double k = 1 / det;
  const std::array<double, 9> inv = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
                                     (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
                                     (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
  const double u = px * inv[0] + py * inv[3] + inv[6];
  const double v = px * inv[1] + py * inv[4] + inv[7];
  double q = px * inv[2] + py * inv[5] + inv[8];
  if (std::fabs(q) < 1e-12) {
    q = 1;
  }
  return {u / q / tw, v / q / th};
}

// ------------------------------------------------------------------------------------------------ the reader
struct RawLayer {
  Layer layer;
  std::string mat_name;  // its "Layer_<name>" material, for the colour
};

struct Info {
  std::optional<std::uint64_t> id;
  std::optional<std::uint32_t> mat;
  std::optional<std::uint32_t> layer = 0;
  std::uint8_t flags = 0;
  std::optional<Range> box;
};

struct RawInst {
  std::uint64_t def_id = 0;
  Instance inst;
};

struct PendingFace {
  Face face;
  P3 n{};
  std::optional<std::array<double, 9>> place;
};

struct Entities {
  std::vector<PendingFace> faces;
  std::vector<Edge> edges;
  std::vector<std::array<P3, 2>> soft;
  std::vector<RawInst> insts;
};

struct Ctx {
  const Tlv* tlv;
  const i18n::Catalog* cat;
  Notes* notes;
  std::map<std::uint64_t, std::uint32_t> mat_ids;
  std::map<std::uint64_t, std::uint32_t> layer_ids;
  std::uint64_t vertices = 0;
};

Info read_info(Ctx& ctx, std::size_t s, std::size_t e) {
  const Tlv& tlv = *ctx.tlv;
  Info out;
  tlv.each(s, e, [&](std::uint16_t t, std::size_t a, std::size_t z) {
    if (t == IDBOX) {
      if (const auto id = tlv.find(a, z, ID, true)) {
        out.id = tlv.uint(id->first, id->second);
      }
      out.box = Range{a, z};
    } else if (t == MAT_REF && z > a) {
      const auto it = ctx.mat_ids.find(tlv.uint(a, z));
      if (it == ctx.mat_ids.end()) {
        ctx.notes->add("ccskp.note.material_ref");
      } else {
        out.mat = it->second;
      }
    } else if (t == LAYER_REF && z > a) {
      const auto it = ctx.layer_ids.find(tlv.uint(a, z));
      if (it == ctx.layer_ids.end()) {
        ctx.notes->add("ccskp.note.tag_ref");
        out.layer = std::nullopt;
      } else {
        out.layer = it->second;
      }
    } else if (t == FLAGS && z > a) {
      out.flags = tlv.at(a);
    }
    return true;
  });
  return out;
}

/// The front texture placement of a face: DC05 > DD05 > B136 > B236 > 1027 > 1127 > 1327 > 1527 (9 × f64).
std::optional<std::array<double, 9>> read_placement(const Ctx& ctx, Range box) {
  std::optional<Range> r = box;
  for (const std::uint16_t tag :
       {std::uint16_t{ATTRS}, std::uint16_t{0xb136}, std::uint16_t{0xb236}, std::uint16_t{0x1027},
        std::uint16_t{0x1127}, std::uint16_t{0x1327}, std::uint16_t{0x1527}}) {
    r = ctx.tlv->find(r->first, r->second, tag, true);
    if (!r) {
      return std::nullopt;
    }
  }
  if (r->second - r->first != 72) {
    return std::nullopt;
  }
  std::array<double, 9> m{};
  for (std::size_t i = 0; i < 9; ++i) {
    const double v = ctx.tlv->f64(r->first + i * 8);
    if (!std::isfinite(v)) {
      return std::nullopt;
    }
    m[i] = v;
  }
  return m;
}

/// An insertion-ordered map of ids (a JS Map: a repeated key keeps its first place, takes the new value).
template <class V>
class IdMap {
 public:
  void set(std::uint64_t k, V v) {
    const auto [it, fresh] = index_.try_emplace(k, items_.size());
    if (fresh) {
      items_.emplace_back(k, std::move(v));
    } else {
      items_[it->second].second = std::move(v);
    }
  }
  [[nodiscard]] const V* get(std::uint64_t k) const {
    const auto it = index_.find(k);
    return it == index_.end() ? nullptr : &items_[it->second].second;
  }
  [[nodiscard]] const std::vector<std::pair<std::uint64_t, V>>& items() const { return items_; }

 private:
  std::unordered_map<std::uint64_t, std::size_t> index_;
  std::vector<std::pair<std::uint64_t, V>> items_;
};

struct RawEdge {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::optional<std::uint32_t> layer;
  std::uint8_t flags = 0;
};
struct RawFace {
  std::optional<Info> info;
  std::optional<P3> normal;
  std::vector<std::vector<std::uint64_t>> loops;  // edge id, direction (0 along, 1 against), …
  std::optional<std::uint32_t> back;
};

Entities read_entities(Ctx& ctx, std::size_t s, std::size_t e) {
  const Tlv& tlv = *ctx.tlv;
  const i18n::Catalog* cat = ctx.cat;
  IdMap<P3> verts;
  IdMap<RawEdge> edges;
  std::vector<RawFace> raw_faces;
  Entities out;
  std::vector<std::pair<std::uint16_t, std::uint64_t>> unknown;  // insertion-ordered

  const auto instance = [&](std::size_t a, std::size_t z) {
    std::optional<Info> info;
    std::string name;
    std::optional<std::array<double, 13>> m13;
    std::optional<std::uint64_t> def_id;
    tlv.each(a, z, [&](std::uint16_t t, std::size_t p, std::size_t q) {
      if (t == INFO) {
        info = read_info(ctx, p, q);
      } else if (t == INST_NAME) {
        name = tlv.text(p, q);
      } else if (t == INST_M) {
        if (q - p < 104) {
          damaged(cat, "ccskp.damaged.placement_short");
        }
        std::array<double, 13> m{};
        for (std::size_t i = 0; i < 13; ++i) {
          m[i] = tlv.f64(p + i * 8);
        }
        m13 = m;
      } else if (t == INST_DEF && q > p) {
        def_id = tlv.uint(p, q);
      }
      return true;
    });
    if (!def_id) {
      ctx.notes->add("ccskp.note.inst_no_def");
      return;
    }
    RawInst ri;
    ri.def_id = *def_id;
    ri.inst.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (m13) {
      const auto& x = *m13;
      const double w = x[12];
      if (!std::isfinite(w) || std::fabs(w) <= 1e-12) {
        damaged(cat, "ccskp.damaged.placement_scale");
      }
      const double k = 1 / w;
      for (std::size_t i = 0; i < 12; ++i) {
        if (!ok_num(x[i] * k)) {
          damaged(cat, "ccskp.damaged.placement_numbers");
        }
      }
      ri.inst.m = {x[0] * k, x[3] * k, x[6] * k, 0, x[1] * k, x[4] * k,  x[7] * k,  0,
                   x[2] * k, x[5] * k, x[8] * k, 0, x[9] * k, x[10] * k, x[11] * k, 1};
    } else {
      ctx.notes->add("ccskp.note.inst_no_place");
    }
    ri.inst.name = std::move(name);
    ri.inst.layer = info ? info->layer : std::optional<std::uint32_t>(0);
    ri.inst.hidden = info && (info->flags & kFlagHidden) != 0;
    if (info) {
      ri.inst.material = info->mat;
    }
    out.insts.push_back(std::move(ri));
  };

  tlv.each(s, e, [&](std::uint16_t t, std::size_t a, std::size_t z) {
    if (t == VERTS) {
      tlv.each(a, z, [&](std::uint16_t t2, std::size_t p, std::size_t q) {
        if (t2 != VERT) {
          return true;
        }
        const auto id = tlv.id_of(p, q);
        const auto xyz = tlv.find(p, q, XYZ);
        if (!id || !xyz || xyz->second - xyz->first < 24) {
          damaged(cat, "ccskp.damaged.vertex_position");
        }
        const P3 v{tlv.f64(xyz->first), tlv.f64(xyz->first + 8), tlv.f64(xyz->first + 16)};
        if (!ok_num(v[0]) || !ok_num(v[1]) || !ok_num(v[2])) {
          damaged(cat, "ccskp.damaged.vertex_coords");
        }
        if (++ctx.vertices > Limits::vertices) {
          fail("too_big", msg(cat, "ccskp.too_big.points", {{"max", grouped(Limits::vertices)}}));
        }
        verts.set(*id, v);
        return true;
      });
    } else if (t == EDGES) {
      tlv.each(a, z, [&](std::uint16_t t2, std::size_t p, std::size_t q) {
        if (t2 != EDGE) {
          return true;
        }
        std::optional<Info> info;
        std::optional<std::uint64_t> va;
        std::optional<std::uint64_t> vb;
        tlv.each(p, q, [&](std::uint16_t t3, std::size_t x, std::size_t y) {
          if (t3 == INFO) {
            info = read_info(ctx, x, y);
          } else if (t3 == EDGE_A && y > x) {
            va = tlv.uint(x, y);
          } else if (t3 == EDGE_B && y > x) {
            vb = tlv.uint(x, y);
          }
          return true;
        });
        if (!info || !info->id || !va || !vb) {
          ctx.notes->add("ccskp.note.edge_no_ends");
          return true;
        }
        edges.set(*info->id, RawEdge{*va, *vb, info->layer, info->flags});
        return true;
      });
    } else if (t == FACES) {
      tlv.each(a, z, [&](std::uint16_t t2, std::size_t p, std::size_t q) {
        if (t2 != FACE) {
          return true;
        }
        RawFace face;
        tlv.each(p, q, [&](std::uint16_t t3, std::size_t x, std::size_t y) {
          if (t3 == INFO) {
            face.info = read_info(ctx, x, y);
          } else if (t3 == NORMAL && y - x >= 24) {
            face.normal = P3{tlv.f64(x), tlv.f64(x + 8), tlv.f64(x + 16)};
          } else if (t3 == BACK_MAT && y > x) {
            const auto it = ctx.mat_ids.find(tlv.uint(x, y));
            if (it == ctx.mat_ids.end()) {
              ctx.notes->add("ccskp.note.material_ref");
            } else {
              face.back = it->second;
            }
          } else if (t3 == LOOPS) {
            tlv.each(x, y, [&](std::uint16_t t4, std::size_t u, std::size_t w) {
              if (t4 != LOOP) {
                return true;
              }
              std::vector<std::uint64_t> co;
              tlv.each(u, w, [&](std::uint16_t t5, std::size_t u2, std::size_t w2) {
                if (t5 != COEDGES) {
                  return true;
                }
                tlv.each(u2, w2, [&](std::uint16_t t6, std::size_t u3, std::size_t w3) {
                  if (t6 != COEDGE) {
                    return true;
                  }
                  std::optional<std::uint64_t> edge;
                  std::optional<bool> rev;
                  tlv.each(u3, w3, [&](std::uint16_t t7, std::size_t u4, std::size_t w4) {
                    if (t7 == CO_EDGE && w4 > u4) {
                      edge = tlv.uint(u4, w4);
                    } else if (t7 == CO_DIR && w4 > u4) {
                      rev = tlv.uint(u4, w4) != 0;
                    }
                    return true;
                  });
                  if (!edge || !rev) {
                    damaged(cat, "ccskp.damaged.face_edge");
                  }
                  co.push_back(*edge);
                  co.push_back(*rev ? 1 : 0);
                  return true;
                });
                return true;
              });
              face.loops.push_back(std::move(co));
              return true;
            });
          }
          return true;
        });
        raw_faces.push_back(std::move(face));
        return true;
      });
    } else if (t == COMPS) {
      tlv.each(a, z, [&](std::uint16_t t2, std::size_t p, std::size_t q) {
        if (t2 == INST) {
          instance(p, q);
        }
        return true;
      });
    } else if (t == GROUPS || t == IMAGES) {
      const std::uint16_t inner = t == GROUPS ? GROUP : IMAGE;
      tlv.each(a, z, [&](std::uint16_t t2, std::size_t p, std::size_t q) {
        if (t2 == inner) {
          tlv.each(p, q, [&](std::uint16_t t3, std::size_t x, std::size_t y) {
            if (t3 == INST) {
              instance(x, y);
            }
            return true;
          });
        }
        return true;
      });
    } else if (t == GUIDE_LINES || t == GUIDE_POINTS || t == ANNOTATIONS) {
      std::uint64_t n = 0;
      tlv.each_loose(a, z, [&n](std::uint16_t, std::size_t, std::size_t) {
        ++n;
        return true;
      });
      std::string_view key = "ccskp.note.annotations";
      if (t == GUIDE_LINES) {
        key = "ccskp.note.guide_lines";
      } else if (t == GUIDE_POINTS) {
        key = "ccskp.note.guide_points";
      }
      ctx.notes->add(key, n);
    } else if (!quiet(t)) {
      const auto it = std::ranges::find_if(unknown, [t](const auto& u) { return u.first == t; });
      if (it == unknown.end()) {
        unknown.emplace_back(t, 1);
      } else {
        ++it->second;
      }
    }
    return true;
  });
  for (const auto& [t, n] : unknown) {
    ctx.notes->add("ccskp.note.other_lists", n, {{"record", tag_hex(t)}});
  }

  // Faces: coedges → vertex loops. A coedge runs from its edge's start (A20F 0) or end (1).
  std::unordered_map<std::uint64_t, bool> used;
  std::uint64_t broken = 0;
  std::uint64_t flipped = 0;
  for (RawFace& f : raw_faces) {
    std::vector<std::vector<P3>> loops;
    bool ok = !f.loops.empty();
    for (const auto& co : f.loops) {
      std::vector<P3> loop;
      std::optional<std::uint64_t> prev_end;
      for (std::size_t i = 0; i < co.size() && ok; i += 2) {
        const RawEdge* ed = edges.get(co[i]);
        if (ed == nullptr) {
          ok = false;
          break;
        }
        const bool against = i + 1 < co.size() && co[i + 1] != 0;
        const std::uint64_t from = against ? ed->b : ed->a;
        const std::uint64_t to = against ? ed->a : ed->b;
        if (prev_end && *prev_end != from) {
          ok = false;
          break;
        }
        const P3* v = verts.get(from);
        if (v == nullptr) {
          ok = false;
          break;
        }
        loop.push_back(*v);
        prev_end = to;
      }
      if (!ok) {
        break;
      }
      if (loop.size() < 3) {
        ok = false;
        break;
      }
      const RawEdge* first = edges.get(co[0]);
      const std::uint64_t start = co[1] != 0 ? first->b : first->a;
      if (prev_end != start) {
        ok = false;
        break;
      }
      loops.push_back(std::move(loop));
    }
    if (!ok) {
      ++broken;
      continue;
    }
    for (const auto& co : f.loops) {
      for (std::size_t i = 0; i < co.size(); i += 2) {
        used[co[i]] = true;
      }
    }
    // The outer loop is the largest; front = counter-clockwise about the normal.
    const bool stored = f.normal && std::isfinite((*f.normal)[0] + (*f.normal)[1] + (*f.normal)[2]);
    const P3 n = stored ? *f.normal : newell(loops[0]);
    double nl = base::js_hypot(n[0], n[1], n[2]);
    if (!(nl != 0) || std::isnan(nl)) {
      nl = 1;  // `|| 1`: 0 and NaN
    }
    const P3 unit{n[0] / nl, n[1] / nl, n[2] / nl};
    const auto area = [&unit](const std::vector<P3>& l) {
      const P3 w = newell(l);
      return (w[0] * unit[0] + w[1] * unit[1] + w[2] * unit[2]) / 2;
    };
    std::size_t outer = 0;
    for (std::size_t i = 1; i < loops.size(); ++i) {
      if (std::fabs(area(loops[i])) > std::fabs(area(loops[outer]))) {
        outer = i;
      }
    }
    if (outer != 0) {
      auto o = std::move(loops[outer]);
      loops.erase(loops.begin() + static_cast<std::ptrdiff_t>(outer));
      loops.insert(loops.begin(), std::move(o));
    }
    if (area(loops[0]) < 0) {
      for (auto& l : loops) {
        std::ranges::reverse(l);
      }
      ++flipped;
    }
    for (std::size_t i = 1; i < loops.size(); ++i) {
      if (area(loops[i]) > 0) {
        std::ranges::reverse(loops[i]);
      }
    }
    const Info info = f.info ? *f.info : Info{};
    PendingFace pf;
    pf.face.loops = std::move(loops);
    pf.face.front = info.mat;
    pf.face.back = f.back;
    pf.face.layer = info.layer;
    pf.face.hidden = (info.flags & kFlagHidden) != 0;
    pf.n = unit;
    if (info.box) {
      pf.place = read_placement(ctx, *info.box);
    }
    out.faces.push_back(std::move(pf));
  }
  if (broken != 0) {
    ctx.notes->add("ccskp.note.face_open", broken);
  }
  if (flipped != 0) {
    ctx.notes->add("ccskp.note.face_turned", flipped);
  }

  std::uint64_t missing = 0;
  for (const auto& [id, ed] : edges.items()) {
    const P3* a = verts.get(ed.a);
    const P3* b = verts.get(ed.b);
    if (a == nullptr || b == nullptr) {
      ++missing;
      continue;
    }
    if (used.contains(id)) {
      if ((ed.flags & (kFlagSoft | kFlagSmooth)) != 0) {
        out.soft.push_back({*a, *b});
      }
    } else {
      out.edges.push_back(Edge{*a, *b, ed.layer, (ed.flags & kFlagSoft) != 0, (ed.flags & kFlagSmooth) != 0,
                               (ed.flags & kFlagHidden) != 0});
    }
  }
  if (missing != 0) {
    ctx.notes->add("ccskp.note.edge_missing_end", missing);
  }
  return out;
}

/// Instances must not contain themselves; nesting ≤ Limits::depth. Iterative.
void check_nesting(const std::vector<Definition>& defs, const i18n::Catalog* cat) {
  std::vector<int> depth(defs.size(), -1);  // -1 unknown, -2 on the stack
  for (std::size_t start = 0; start < defs.size(); ++start) {
    if (depth[start] >= 0) {
      continue;
    }
    std::vector<std::pair<std::size_t, std::size_t>> stack{{start, 0}};
    depth[start] = -2;
    while (!stack.empty()) {
      auto& top = stack.back();
      const std::size_t d = top.first;
      const auto& insts = defs[d].insts;
      if (top.second < insts.size()) {
        const std::size_t c = insts[top.second].def;
        ++top.second;
        if (depth[c] == -2) {
          damaged(cat, "ccskp.damaged.self_nested");
        }
        if (depth[c] == -1) {
          if (stack.size() >= static_cast<std::size_t>(Limits::depth)) {
            fail("too_big", msg(cat, "ccskp.too_big.nesting", {{"levels", std::to_string(Limits::depth)}}));
          }
          depth[c] = -2;
          stack.emplace_back(c, 0);
        }
      } else {
        int m = 0;
        for (const Instance& inst : insts) {
          m = std::max(m, depth[inst.def] + 1);
        }
        if (m > Limits::depth) {
          fail("too_big", msg(cat, "ccskp.too_big.nesting", {{"levels", std::to_string(Limits::depth)}}));
        }
        depth[d] = m;
        stack.pop_back();
      }
    }
  }
}

struct ModelDat {
  std::vector<std::string> materials;  // names, id order
  std::vector<RawLayer> layers;
  std::vector<Definition> defs;
  std::vector<std::vector<PendingFace>> faces;  // per def, until the UVs are known
};

ModelDat read_model_dat(std::span<const std::uint8_t> md, Notes& notes, const ReadOptions& opts,
                        const i18n::Catalog* cat) {
  const Tlv tlv(md, cat);
  Range top{0, md.size()};
  std::vector<std::tuple<std::uint16_t, std::size_t, std::size_t>> only;
  tlv.each(0, md.size(), [&only](std::uint16_t t, std::size_t s, std::size_t e) {
    only.emplace_back(t, s, e);
    return true;
  });
  if (only.size() == 1 && std::get<0>(only[0]) == FILE_) {
    top = {std::get<1>(only[0]), std::get<2>(only[0])};
  }

  Ctx ctx{&tlv, cat, &notes, {}, {}, 0};
  ModelDat out;
  std::vector<Range> def_records;
  std::optional<Range> root_record;
  std::uint64_t scenes = 0;

  tlv.each(top.first, top.second, [&](std::uint16_t t, std::size_t s, std::size_t e) {
    if (t == MATS) {
      const auto a = tlv.find(s, e, MATS2);
      const auto b = a ? tlv.find(a->first, a->second, MATS3) : std::nullopt;
      if (b) {
        tlv.each(b->first, b->second, [&](std::uint16_t t2, std::size_t s2, std::size_t e2) {
          if (t2 != MAT) {
            return true;
          }
          const auto id = tlv.id_of(s2, e2);
          const auto nm = tlv.find(s2, e2, MAT_NAME);
          if (!id) {
            notes.add("ccskp.note.material_no_id");
            return true;
          }
          ctx.mat_ids[*id] = static_cast<std::uint32_t>(out.materials.size());
          out.materials.push_back(nm ? tlv.text(nm->first, nm->second) : std::string());
          return true;
        });
      }
    } else if (t == LAYERS) {
      const auto a = tlv.find(s, e, LAYERS2);
      const auto b = a ? tlv.find(a->first, a->second, LAYERS3) : std::nullopt;
      if (b) {
        tlv.each(b->first, b->second, [&](std::uint16_t t2, std::size_t s2, std::size_t e2) {
          if (t2 != LAYER) {
            return true;
          }
          const auto id = tlv.id_of(s2, e2);
          const auto nm = tlv.find(s2, e2, LAYER_NAME);
          const auto hid = tlv.find(s2, e2, LAYER_HIDDEN);
          const auto lm = tlv.find(s2, e2, LAYER_MAT);
          std::string mat_name;
          if (lm) {
            const auto c = tlv.find(lm->first, lm->second, MAT, true);
            const auto cn = c ? tlv.find(c->first, c->second, MAT_NAME, true) : std::nullopt;
            if (cn) {
              mat_name = tlv.text(cn->first, cn->second);
            }
          }
          if (!id) {
            notes.add("ccskp.note.tag_no_id");
            return true;
          }
          ctx.layer_ids[*id] = static_cast<std::uint32_t>(out.layers.size());
          RawLayer l;
          l.layer.name = nm ? tlv.text(nm->first, nm->second) : std::string();
          l.layer.visible = !hid || hid->second <= hid->first || tlv.at(hid->first) != 1;
          l.mat_name = std::move(mat_name);
          out.layers.push_back(std::move(l));
          return true;
        });
      }
    } else if (t == DEFS) {
      const auto a = tlv.find(s, e, DEFS2);
      const auto b = a ? tlv.find(a->first, a->second, DEFS3) : std::nullopt;
      if (b) {
        tlv.each(b->first, b->second, [&](std::uint16_t t2, std::size_t s2, std::size_t e2) {
          if (t2 == DEF) {
            def_records.emplace_back(s2, e2);
          }
          return true;
        });
      }
    } else if (t == ROOT) {
      root_record = tlv.find(s, e, ENTS);
    } else if (t == SCENES) {
      const auto a = tlv.find(s, e, SCENES2, true);
      const auto b = a ? tlv.find(a->first, a->second, SCENES3, true) : std::nullopt;
      if (b) {
        tlv.each_loose(b->first, b->second, [&scenes](std::uint16_t t2, std::size_t, std::size_t) {
          if (t2 == SCENE) {
            ++scenes;
          }
          return true;
        });
      }
    }
    return true;
  });
  if (!root_record) {
    damaged(cat, "ccskp.damaged.no_geometry");
  }
  notes.add("ccskp.note.scenes", scenes);

  // Tag 0 = Layer0 ("Untagged"), always first.
  const auto l0 = std::ranges::find_if(out.layers, [](const RawLayer& l) { return l.layer.name == "Layer0"; });
  if (l0 == out.layers.end()) {
    RawLayer l;
    l.layer.name = "Layer0";
    l.mat_name = "Layer_Layer0";
    out.layers.insert(out.layers.begin(), std::move(l));
    for (auto& [k, v] : ctx.layer_ids) {
      ++v;
    }
  } else if (l0 != out.layers.begin()) {
    const auto at = static_cast<std::uint32_t>(l0 - out.layers.begin());
    RawLayer l = std::move(*l0);
    out.layers.erase(l0);
    out.layers.insert(out.layers.begin(), std::move(l));
    for (auto& [k, v] : ctx.layer_ids) {
      if (v == at) {
        v = 0;
      } else if (v < at) {
        ++v;
      }
    }
  }

  // Definitions: index 0 = the model, then each 7C15 in file order.
  std::map<std::uint64_t, std::uint32_t> def_index;
  std::vector<std::vector<RawInst>> pending;
  Definition root;
  root.name = "Model";
  root.kind = DefKind::model;
  {
    Entities got = read_entities(ctx, root_record->first, root_record->second);
    root.edges = std::move(got.edges);
    root.soft = std::move(got.soft);
    out.faces.push_back(std::move(got.faces));
    pending.push_back(std::move(got.insts));
  }
  out.defs.push_back(std::move(root));
  for (std::size_t i = 0; i < def_records.size(); ++i) {
    if (i % 64 == 0) {
      if (opts.on_progress) {
        opts.on_progress(Progress{"read", i, def_records.size()});
      }
      if (opts.cancelled && opts.cancelled()) {
        fail("aborted", "aborted");
      }
    }
    const auto [s, e] = def_records[i];
    std::string name;
    std::optional<std::string> guid;
    DefKind kind = DefKind::component;
    std::optional<Range> ents;
    tlv.each(s, e, [&](std::uint16_t t, std::size_t a, std::size_t z) {
      if (t == ENTS) {
        ents = Range{a, z};
      } else if (t == DEF_NAME) {
        name = tlv.text(a, z);
      } else if (t == DEF_GUID) {
        guid = guid_text(md.subspan(a, z - a));
      } else if (t == DEF_KIND && z > a) {
        const std::uint64_t k = tlv.uint(a, std::min(z, a + 4));
        kind = DefKind::component;
        if (k == 1) {
          kind = DefKind::group;
        } else if (k == 2) {
          kind = DefKind::image;
        }
      }
      return true;
    });
    if (!ents) {
      notes.add("ccskp.note.def_no_geometry");
      continue;
    }
    const auto info = tlv.find(ents->first, ents->second, INFO);
    const auto id = info ? tlv.id_of(info->first, info->second) : std::nullopt;
    if (!id) {
      notes.add("ccskp.note.def_no_id");
      continue;
    }
    if (def_index.contains(*id)) {
      damaged(cat, "ccskp.damaged.def_id_twice");
    }
    Definition def;
    def.name = std::move(name);
    def.kind = kind;
    def.guid = std::move(guid);
    def_index[*id] = static_cast<std::uint32_t>(out.defs.size());
    Entities got = read_entities(ctx, ents->first, ents->second);
    def.edges = std::move(got.edges);
    def.soft = std::move(got.soft);
    out.faces.push_back(std::move(got.faces));
    pending.push_back(std::move(got.insts));
    out.defs.push_back(std::move(def));
  }
  if (opts.on_progress) {
    opts.on_progress(Progress{"read", def_records.size(), def_records.size()});
  }

  // Instances point at definition ids; resolve them now every definition is known.
  for (std::size_t d = 0; d < pending.size(); ++d) {
    for (RawInst& ri : pending[d]) {
      const auto it = def_index.find(ri.def_id);
      if (it == def_index.end()) {
        notes.add("ccskp.note.inst_missing_def");
        continue;
      }
      ri.inst.def = it->second;
      out.defs[d].insts.push_back(std::move(ri.inst));
    }
  }
  check_nesting(out.defs, cat);
  return out;
}

}  // namespace

Model read_vff(std::span<const std::uint8_t> bytes, const Sniff& header, const ReadOptions& opts) {
  const i18n::Catalog* cat = opts.catalog;
  const auto progress = [&opts](std::string_view stage, std::uint64_t done, std::uint64_t total) {
    if (opts.on_progress) {
      opts.on_progress(Progress{stage, done, total});
    }
  };
  const auto check_abort = [&opts] {
    if (opts.cancelled && opts.cancelled()) {
      fail("aborted", "aborted");
    }
  };

  // The ZIP starts at the first local header after the SketchUp header records.
  std::optional<std::size_t> zip_at;
  if (bytes.size() >= 4) {
    const std::size_t end = std::min(bytes.size() - 4, header.body_start + 4096);
    for (std::size_t i = header.body_start; i <= end; ++i) {
      if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b && bytes[i + 2] == 3 && bytes[i + 3] == 4) {
        zip_at = i;
        break;
      }
    }
  }
  if (!zip_at) {
    damaged(cat, "ccskp.damaged.no_archive");
  }
  const Zip zip(bytes, *zip_at, cat);
  std::uint64_t used = 0;
  const auto model_name = std::ranges::find_if(
      zip.names(), [](const std::string& n) { return n.size() == 9 && ends_with_ci(n, "model.dat"); });
  if (model_name == zip.names().end()) {
    damaged(cat, "ccskp.damaged.no_model_dat");
  }
  progress("unzip", 0, 1);
  check_abort();
  const Bytes md = zip.read(*model_name, used);
  progress("unzip", 1, 1);
  check_abort();

  Notes notes;
  ModelDat model;
  try {
    model = read_model_dat(md, notes, opts, cat);
  } catch (const Fail& f) {
    // a file newer than any we know that we cannot follow is "too new", not "damaged"
    if (f.error.code == "damaged" && header.major > kNewestKnown) {
      fail("too_new",
           msg(cat, "ccskp.too_new", {{"version", header.version}, {"newest", "20" + num_text(kNewestKnown)}}));
    }
    throw;
  }

  // Materials: the model's list joined to materials/*/material.xml by name, else by folder.
  std::map<std::string, std::size_t> xml_by_name;
  std::map<std::string, std::size_t> xml_by_folder;
  std::vector<XmlMaterial> xmls;
  for (const std::string& name : zip.names()) {
    // /^materials\/([^/]+)\/material\.xml$/i
    if (name.size() < 10 + 13) {
      continue;
    }
    if (!ends_with_ci(name.substr(0, 10), "materials/") || !ends_with_ci(name, "/material.xml")) {
      continue;
    }
    const std::string folder = name.substr(10, name.size() - 10 - 13);
    if (folder.empty() || folder.contains('/')) {
      continue;
    }
    std::optional<XmlMaterial> info;
    try {
      const Bytes raw = zip.read(name, used);
      info = read_material_xml(utf8_lossy(raw));
    } catch (const Fail& f) {
      if (f.error.code == "too_big") {
        throw;
      }
    }
    if (!info) {
      notes.add("ccskp.note.material_xml");
      continue;
    }
    info->folder = "materials/" + folder;
    xmls.push_back(std::move(*info));
    const std::size_t at = xmls.size() - 1;
    if (xmls[at].name && !xmls[at].name->empty()) {
      xml_by_name[*xmls[at].name] = at;
    }
    xml_by_folder[folder] = at;
  }
  const auto by_name = [&](const std::string& n) -> const XmlMaterial* {
    const auto it = xml_by_name.find(n);
    return it == xml_by_name.end() ? nullptr : &xmls[it->second];
  };
  Model m;
  m.vff = true;
  m.version = header.version;
  m.major = header.major;
  for (const std::string& mat_name : model.materials) {
    const XmlMaterial* x = by_name(mat_name);
    if (x == nullptr) {
      const auto it = xml_by_folder.find(mat_name);
      if (it != xml_by_folder.end()) {
        x = &xmls[it->second];
      }
    }
    Material out;
    out.name = x != nullptr && x->name && !x->name->empty() ? *x->name : mat_name;
    if (x != nullptr) {
      out.color = x->color;
      out.alpha = x->alpha;
    }
    if (x == nullptr) {
      notes.add("ccskp.note.material_no_xml");
    }
    if (x != nullptr && x->texture) {
      const XmlTexture& t = *x->texture;
      std::vector<std::optional<std::string>> want;
      if (!t.path.empty()) {
        want.push_back(join_path(x->folder, t.path));
      }
      if (!t.file.empty()) {
        want.push_back(join_path(x->folder, base_name(t.file)));
      }
      std::optional<std::string> entry;
      for (const auto& w : want) {
        if (w && !w->empty() && zip.has(*w)) {
          entry = *w;
          break;
        }
      }
      if (!entry) {
        const std::string prefix = x->folder + "/";
        for (const std::string& n : zip.names()) {
          if (n.starts_with(prefix) && !ends_with_ci(n, ".xml") && !thumbnail_name(n)) {
            entry = n;
            break;
          }
        }
      }
      if (!entry) {
        notes.add("ccskp.note.texture_missing");
      } else if (!t.width || !t.height) {
        notes.add("ccskp.note.texture_no_size");
      } else {
        Texture tex;
        tex.bytes = zip.read(*entry, used);
        tex.name = entry->substr(entry->rfind('/') == std::string::npos ? 0 : entry->rfind('/') + 1);
        tex.mime = sniff_mime(tex.bytes, *entry);
        tex.width = *t.width;
        tex.height = *t.height;
        out.texture = std::move(tex);
      }
    }
    m.materials.push_back(std::move(out));
  }

  // Tags: colour from their "Layer_<name>" material.xml.
  for (RawLayer& l : model.layers) {
    const XmlMaterial* x = l.mat_name.empty() ? nullptr : by_name(l.mat_name);
    l.layer.color = x != nullptr ? x->color : std::nullopt;
    m.layers.push_back(std::move(l.layer));
  }

  // UVs for textured faces (front side), now the tile sizes are known.
  for (std::size_t d = 0; d < model.defs.size(); ++d) {
    for (PendingFace& pf : model.faces[d]) {
      const Texture* tex = pf.face.front && *pf.face.front < m.materials.size() && m.materials[*pf.face.front].texture
                               ? &*m.materials[*pf.face.front].texture
                               : nullptr;
      if (tex != nullptr) {
        std::vector<std::vector<P2>> uv;
        uv.reserve(pf.face.loops.size());
        for (const auto& loop : pf.face.loops) {
          std::vector<P2> l;
          l.reserve(loop.size());
          for (const P3& p : loop) {
            l.push_back(face_uv(p, pf.n, pf.place, tex->width, tex->height));
          }
          uv.push_back(std::move(l));
        }
        pf.face.uv = std::move(uv);
      }
      model.defs[d].faces.push_back(std::move(pf.face));
    }
  }
  m.defs = std::move(model.defs);

  // Thumbnail and units.
  for (const char* n : {"meta/model_thumbnail.png", "meta/preview_thumbnail.png"}) {
    if (zip.has(n)) {
      Thumbnail th;
      th.bytes = zip.read(n, used);
      th.mime = sniff_mime(th.bytes, n);
      m.thumbnail = std::move(th);
      break;
    }
  }
  if (zip.has("meta/meta.dat")) {
    const Bytes meta = zip.read("meta/meta.dat", used);
    if (const auto word = read_units(meta); word && !word->empty()) {
      if (const auto unit = unit_word(*word)) {
        m.length_unit = *unit;
      } else {
        notes.add("ccskp.note.unit_unknown", 1, {{"unit", *word}});
      }
    }
  }
  std::uint64_t styles = 0;
  for (const std::string& n : zip.names()) {
    // /^styles\/[^/]+\/style\.xml$/i
    if (n.size() < 7 + 10 || !ends_with_ci(n.substr(0, 7), "styles/") || !ends_with_ci(n, "/style.xml")) {
      continue;
    }
    const std::string_view mid = std::string_view(n).substr(7, n.size() - 7 - 10);
    if (!mid.empty() && !mid.contains('/')) {
      ++styles;
    }
  }
  notes.add("ccskp.note.styles", styles);
  m.notes = notes.list(cat);
  return m;
}

}  // namespace cc::skp::detail
