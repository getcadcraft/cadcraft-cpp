// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg dynblock-read.js — dynamic blocks (0.5 DB.2): what a definition's parameters are and what each
// reference is set to. Not in the ODA spec: every layout here is a `Spec gap:` found by experiment on AutoCAD's own
// files (docs/DWG-DYNAMIC-BLOCKS.md in the site repo).
//
// read_dynamic(model) runs after the model is built (read.cpp) and works on the model only — the objects kept raw
// (passthrough.cpp `raw`), the dictionaries, XRECORDs, extension dictionaries and xdata. It adds, validated (file data
// is untrusted: counts, sizes and handles checked):
//
//   block.dynamic = { graph, params: [{ node, kind, handle, name, label?, description?, point?, grip?, all?, states?
//   }],
//                     actions: [{ node, kind, handle, name }] }                        on a definition
//   block.dynamicOf = definition block record (handle)                                   on a reference's copy
//   insert.dyn = { def, values: [{ node, kind, xrecord, data, …decoded }], state? }      on a reference
//
// Model containers are never held by reference across an edit: definitions are kept by block index and every write
// looks its target up again.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bits.hpp"
#include "parts.hpp"
#include "record.hpp"

namespace cc::dwg {

namespace {

using base::Array;
using base::make_object;

constexpr std::int64_t kLimitNodes = 10000;
constexpr std::int64_t kLimitList = 100000;
constexpr std::int64_t kLimitStates = 1000;
constexpr std::size_t kLimitName = 255;

/// Thrown where the JS throws inside a try (a layout that does not fit).
class DynError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- text helpers

/// UTF-8 (WTF-8: lone surrogates as 3-byte forms) → UTF-16 units, as a JS string holds it.
U16 to_u16(std::string_view s) {
  U16 out;
  out.reserve(s.size());
  std::size_t i = 0;
  const auto at = [&](std::size_t k) -> std::uint32_t { return static_cast<std::uint8_t>(s[k]); };
  while (i < s.size()) {
    const std::uint32_t c = at(i);
    std::uint32_t cp = 0xFFFD;
    std::size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0U) == 0xC0U && i + 1 < s.size()) {
      cp = ((c & 0x1FU) << 6U) | (at(i + 1) & 0x3FU);
      len = 2;
    } else if ((c & 0xF0U) == 0xE0U && i + 2 < s.size()) {
      cp = ((c & 0x0FU) << 12U) | ((at(i + 1) & 0x3FU) << 6U) | (at(i + 2) & 0x3FU);
      len = 3;
    } else if ((c & 0xF8U) == 0xF0U && i + 3 < s.size()) {
      cp = ((c & 0x07U) << 18U) | ((at(i + 1) & 0x3FU) << 12U) | ((at(i + 2) & 0x3FU) << 6U) | (at(i + 3) & 0x3FU);
      len = 4;
    }
    if (cp >= 0x10000U && cp <= 0x10FFFFU) {
      const std::uint32_t v = cp - 0x10000U;
      out.push_back(static_cast<std::uint16_t>(0xD800U + (v >> 10U)));
      out.push_back(static_cast<std::uint16_t>(0xDC00U + (v & 0x3FFU)));
    } else {
      out.push_back(static_cast<std::uint16_t>(cp > 0xFFFFU ? 0xFFFDU : cp));
    }
    i += len;
  }
  return out;
}

/// cleanName: control characters dropped, at most 255 UTF-16 units.
std::string clean_name(const U16& s) {
  U16 out;
  out.reserve(s.size());
  for (const std::uint16_t u : s) {
    if (u >= 0x20) {
      out.push_back(u);
    }
  }
  if (out.size() > kLimitName) {
    out.resize(kLimitName);
  }
  return utf8(out);
}

/// String(x) for the values a handle can be (JS String conversion of the common kinds). Recursion only into arrays
/// of the file's own values, whose depth the reader bounds.
// NOLINTNEXTLINE(misc-no-recursion)
std::string js_string(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::undefined: return "undefined";
    case Value::Kind::null: return "null";
    case Value::Kind::boolean: return v.as_bool() ? "true" : "false";
    case Value::Kind::number: {
      std::string s;
      base::append_number(s, v.as_number());
      return s;
    }
    case Value::Kind::string: return v.as_string();
    case Value::Kind::array: {
      std::string s;
      bool first = true;
      for (const Value& x : v.as_array()) {
        if (!first) {
          s += ',';
        }
        first = false;
        if (!x.is_undefined() && !x.is_null()) {
          s += js_string(x);
        }
      }
      return s;
    }
    case Value::Kind::object: return "[object Object]";
    case Value::Kind::bytes: return "";
  }
  return "";
}

/// hexKey: upper case, leading zeros dropped (one digit kept); "" for undefined/null.
std::string hex_key(const Value& h) {
  if (h.is_undefined() || h.is_null()) {
    return {};
  }
  std::string s = base::to_upper(js_string(h));
  std::size_t z = 0;
  while (z + 1 < s.size() && s[z] == '0') {
    ++z;
  }
  return s.substr(z);
}

/// x[k] for an array element (undefined when x is not an array or k is outside it).
const Value& item(const Value& x, std::size_t k) {
  static const Value kUndef;
  if (!x.is_array() || k >= x.as_array().size()) {
    return kUndef;
  }
  return x.as_array()[k];
}

/// o.key (undefined when o is not an object or has no such member).
const Value& member(const Value& o, std::string_view key) {
  static const Value kUndef;
  if (!o.is_object()) {
    return kUndef;
  }
  const Value* v = o.find(key);
  return v != nullptr ? *v : kUndef;
}

bool is_code(const Value& pair, double code) {
  const Value& c = item(pair, 0);
  return c.is_number() && c.as_number() == code;
}

/// JS Number(string) for the keys a dictionary can have; NaN when it is not a number.
double js_number(std::string_view s) {
  const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
  while (!s.empty() && ws(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && ws(s.back())) {
    s.remove_suffix(1);
  }
  if (s.empty()) {
    return 0;
  }
  const double nan = std::nan("");
  if (s.size() > 2 && s[0] == '0' &&
      (s[1] == 'x' || s[1] == 'X' || s[1] == 'o' || s[1] == 'O' || s[1] == 'b' || s[1] == 'B')) {
    const char k = static_cast<char>(s[1] | 0x20);
    double base = 2;
    if (k == 'x') {
      base = 16;
    } else if (k == 'o') {
      base = 8;
    }
    double v = 0;
    for (const char c : s.substr(2)) {
      int d = -1;
      if (c >= '0' && c <= '9') {
        d = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        d = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        d = c - 'A' + 10;
      }
      if (d < 0 || d >= static_cast<int>(base)) {
        return nan;
      }
      v = v * base + d;
    }
    return v;
  }
  std::size_t i = 0;
  if (s[i] == '+' || s[i] == '-') {
    ++i;
  }
  const std::string_view body = s.substr(i);
  if (body == "Infinity") {
    return s[0] == '-' ? -HUGE_VAL : HUGE_VAL;
  }
  std::size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    ++digits;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      ++digits;
    }
  }
  if (digits == 0) {
    return nan;
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    std::size_t ed = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      ++ed;
    }
    if (ed == 0) {
      return nan;
    }
  }
  if (i != s.size()) {
    return nan;
  }
  return std::strtod(std::string(s).c_str(), nullptr);
}

bool is_integer(double d) {
  return std::isfinite(d) && std::trunc(d) == d;
}

/// A canonical array index ("0".."4294967294"): JS enumerates such keys first, in numeric order.
std::optional<std::uint64_t> array_index(std::string_view k) {
  if (k.empty() || k.size() > 10 || (k.size() > 1 && k[0] == '0')) {
    return std::nullopt;
  }
  std::uint64_t v = 0;
  for (const char c : k) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (v > 4294967294ULL) {
    return std::nullopt;
  }
  return v;
}

/// Math.hypot(a, b) as V8 computes it (normalised Kahan sum), so distances match the reference to the bit.
double js_hypot(double a, double b) {
  const double x = std::fabs(a);
  const double y = std::fabs(b);
  if (std::isinf(x) || std::isinf(y)) {
    return HUGE_VAL;
  }
  if (std::isnan(x) || std::isnan(y)) {
    return std::nan("");
  }
  const double mx = x > y ? x : y;
  if (mx == 0) {
    return 0;
  }
  double sum = 0;
  double compensation = 0;
  for (const double v : {x, y}) {
    const double n = v / mx;
    const double summand = n * n - compensation;
    const double preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }
  return std::sqrt(sum) * mx;
}

// ---------------------------------------------------------------- the raw reader

/// base64 → bytes (lenient like Buffer.from(s, 'base64'): other characters skipped, stops at '=').
std::vector<std::uint8_t> unb64(std::string_view s) {
  std::vector<std::uint8_t> out;
  out.reserve(s.size() / 4 * 3 + 3);
  std::uint32_t acc = 0;
  unsigned bits = 0;
  for (const char c : s) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') {
      v = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      v = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      v = c - '0' + 52;
    } else if (c == '+' || c == '-') {
      v = 62;
    } else if (c == '/' || c == '_') {
      v = 63;
    } else if (c == '=') {
      break;
    }
    if (v < 0) {
      continue;
    }
    acc = ((acc << 6U) | static_cast<std::uint32_t>(v)) & 0xFFFFFFU;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFU));
    }
  }
  return out;
}

/// A reader on a raw object's data bits; R2007+ strings come from the string stream at the end (ODA spec 20.1).
/// Owns the decoded bytes; `r.str` points at `s`, so it is neither copied nor moved.
class RawReader {
 public:
  explicit RawReader(const Value& raw) : cp_(codepage_for(30)) {
    if (!raw.is_object()) {
      throw DynError("no raw data");
    }
    const Value& data = member(raw, "data");
    buf_ = unb64(data.is_string() ? std::string_view(data.as_string()) : std::string_view{});
    const Value& vv = member(raw, "ver");
    const double vd = vv.num_or(0);
    // o.version || 15
    const int ver = std::isfinite(vd) && vd != 0 && std::fabs(vd) < 1e6 ? static_cast<int>(vd) : 15;
    const Value& nv = member(raw, "n");
    std::uint64_t n = static_cast<std::uint64_t>(buf_.size()) * 8U;
    if (nv.is_number()) {
      const double nd = nv.as_number();
      if (!std::isfinite(nd) || nd < 0 || nd > 9e15) {
        throw DynError("raw size");
      }
      n = static_cast<std::uint64_t>(nd);
    }
    const Bytes b(buf_);
    r = Bits(b, ver, &cp_, 0, n);
    if (ver >= 21) {
      s_ = Bits(b, ver, &cp_, n, n);
      if (const auto ss = string_stream_start(b, n, 0)) {
        s_.set_pos(ss->start);
        s_.set_end(ss->end);
        r.set_end(ss->start);
      } else {
        // r.end = raw.n - 1 (a negative end in the JS leaves nothing to read)
        r.set_end(n >= 1 ? n - 1 : 0);
      }
      r.str = &s_;
    }
  }
  RawReader(const RawReader&) = delete;
  RawReader& operator=(const RawReader&) = delete;
  RawReader(RawReader&&) = delete;
  RawReader& operator=(RawReader&&) = delete;
  ~RawReader() = default;

  Bits r;

 private:
  std::vector<std::uint8_t> buf_;
  Codepage cp_;
  Bits s_;
};

/// TV as the JS BitReader reads it here: TU (string stream) from R2007, else T decoded as Latin-1.
U16 tv16(Bits& r) {
  if (r.version() >= 21) {
    Bits& s = r.str != nullptr ? *r.str : r;
    U16 u = s.TU16();
    if (s.version() < 32) {
      unescape_u(u);
    }
    return u;
  }
  const std::int32_t n = r.BS();
  if (n <= 0) {
    return {};
  }
  const std::vector<std::uint8_t> b = r.bytes(static_cast<std::size_t>(n));
  std::size_t e = b.size();
  while (e > 0 && b[e - 1] == 0) {
    --e;
  }
  U16 u;
  u.reserve(e);
  for (std::size_t i = 0; i < e; ++i) {
    u.push_back(b[i]);
  }
  return u;
}

std::int64_t count(std::int64_t n, std::int64_t max) {
  if (n < 0 || n > max) {
    throw DynError("count " + std::to_string(n));
  }
  return n;
}

/// Spec gap: the element header every dynamic-block object starts with — BL −1, BL release (27 = the AutoCAD 2007
/// format), BL maintenance, BL a 32-bit value (meaning unknown), and its name as the first string.
std::string header(Bits& r) {
  r.BL();
  r.BL();
  r.BL();
  r.BL();
  return clean_name(tv16(r));
}

/// Spec gap: an element's name (the first string) without decoding the rest.
std::string element_name(const Value& o) {
  try {
    RawReader rr(member(o, "raw"));
    return header(rr.r);
  } catch (const std::exception&) {
    return {};
  }
}

struct Node {
  std::int32_t node = 0;
  std::string handle;
};

/// Spec gap: ACAD_EVALUATION_GRAPH — BL, BL, BL node count, then per node BL index into the graph's handle list, BL
/// flags, BL node id, BL ×4 edge indices; the edges follow (not read). Same bits in AutoCAD 2000–2018.
std::vector<Node> graph_nodes(const Value& o) {
  std::vector<Node> out;
  const Value& raw = member(o, "raw");
  if (!raw.truthy()) {
    return out;
  }
  RawReader rr(raw);
  Bits& r = rr.r;
  const Value& refs = member(raw, "h");
  r.BL();
  r.BL();
  const std::int64_t n = count(r.BL(), kLimitNodes);
  for (std::int64_t i = 0; i < n; ++i) {
    const std::int32_t idx = r.BL();
    r.BL();
    const std::int32_t node = r.BL();
    r.BL();
    r.BL();
    r.BL();
    r.BL();
    static const Value kUndef;
    const Value& h = idx >= 0 ? item(item(refs, static_cast<std::size_t>(idx)), 1) : kUndef;
    if (node >= 0 && h.truthy()) {
      out.push_back({node, hex_key(h)});
    }
  }
  return out;
}

/// Spec gap: BLOCKVISIBILITYPARAMETER — header · B B (show properties, chain actions) · BS ×4 · BD3 grip point · BS BS
/// · BL node id of its grip · TV label · TV description · B B · BL n + n handles (every entity of the block) · BL
/// states, each: TV name · BL a + a handles (entities shown) · BL b + b handles (parameters and grips shown). Exact (no
/// bit, handle or string left) on every visibility parameter of the samples.
struct State {
  std::string name;
  std::vector<std::string> entities;
  std::vector<std::string> elements;
};
struct Visibility {
  std::string name;
  std::string label;
  std::string description;
  Value point;
  std::int32_t grip = 0;
  std::vector<std::string> all;
  std::vector<State> states;
};

Visibility visibility_of(const Value& o) {
  const Value& raw = member(o, "raw");
  RawReader rr(raw);
  Bits& r = rr.r;
  std::vector<std::string> refs;
  if (const Value& h = member(raw, "h"); h.truthy()) {
    if (!h.is_array()) {
      throw DynError("handles");
    }
    for (const Value& x : h.as_array()) {
      refs.push_back(hex_key(item(x, 1)));
    }
  }
  std::size_t k = 0;
  const auto take = [&](std::int64_t n) {
    const auto un = static_cast<std::size_t>(n);
    if (k + un > refs.size()) {
      throw DynError("handles");
    }
    std::vector<std::string> l(refs.begin() + static_cast<std::ptrdiff_t>(k),
                               refs.begin() + static_cast<std::ptrdiff_t>(k + un));
    k += un;
    return l;
  };
  Visibility v;
  v.name = header(r);
  r.B();
  r.B();
  r.BS();
  r.BS();
  r.BS();
  r.BS();
  v.point = r.BD3();
  r.BS();
  r.BS();
  v.grip = r.BL();
  v.label = clean_name(tv16(r));
  v.description = clean_name(tv16(r));
  r.B();
  r.B();
  v.all = take(count(r.BL(), kLimitList));
  const std::int64_t ns = count(r.BL(), kLimitStates);
  for (std::int64_t i = 0; i < ns; ++i) {
    State st;
    st.name = clean_name(tv16(r));
    st.entities = take(count(r.BL(), kLimitList));
    st.elements = take(count(r.BL(), kLimitList));
    v.states.push_back(std::move(st));
  }
  if (r.left() != 0 || k != refs.size()) {
    throw DynError("layout");
  }
  if (!std::isfinite(v.point["x"].num_or(0)) || !std::isfinite(v.point["y"].num_or(0)) ||
      !std::isfinite(v.point["z"].num_or(0))) {
    throw DynError("point");
  }
  return v;
}

Value hex_array(const std::vector<std::string>& l) {
  Array a;
  a.reserve(l.size());
  for (const std::string& s : l) {
    a.emplace_back(s);
  }
  return a;
}

/// A finite 2D point of an XRECORD group (p && Number.isFinite(p.x) && Number.isFinite(p.y)).
bool usable_point(const Value& p) {
  if (!p.truthy() || !p.is_object()) {
    return false;
  }
  const Value& x = member(p, "x");
  const Value& y = member(p, "y");
  return x.is_number() && std::isfinite(x.as_number()) && y.is_number() && std::isfinite(y.as_number());
}

/// A reference's values (an XRECORD's groups) for a parameter kind → the decoded fields. Unknown shapes stay raw.
Value value_of(std::string_view kind, const Value& data) {
  static const Array kEmpty;
  const Array& list = data.is_array() ? data.as_array() : kEmpty;
  // later AutoCAD writes a header (1071, 1071, 70, 70) before the values (docs/DWG-DYNAMIC-BLOCKS.md)
  const bool head = list.size() >= 4 && is_code(list[0], 1071) && is_code(list[1], 1071) && is_code(list[2], 70) &&
                    is_code(list[3], 70);
  const std::size_t from = head ? 4 : 0;
  std::vector<const Value*> pts;
  for (std::size_t i = from; i < list.size(); ++i) {
    if (is_code(list[i], 10) && usable_point(item(list[i], 1))) {
      pts.push_back(&item(list[i], 1));
    }
  }
  const auto find = [&](double code) -> const Value* {
    for (std::size_t i = from; i < list.size(); ++i) {
      if (is_code(list[i], code)) {
        return &list[i];
      }
    }
    return nullptr;
  };
  Value out = Value::object();
  if (kind == "visibility") {
    const Value* s = find(1);
    if (s != nullptr && item(*s, 1).is_string()) {
      out.set("state", clean_name(to_u16(item(*s, 1).as_string())));
    }
    if (!pts.empty()) {
      out.set("point", *pts[0]);
    }
  } else if (kind == "linear" || kind == "alignment") {
    if (pts.size() >= 2) {
      out.set("p1", *pts[0]);
      out.set("p2", *pts[1]);
      const double dx = member(*pts[1], "x").as_number() - member(*pts[0], "x").as_number();
      const double dy = member(*pts[1], "y").as_number() - member(*pts[0], "y").as_number();
      if (kind == "linear") {
        out.set("distance", js_hypot(dx, dy));
      } else {
        out.set("angle", std::atan2(dy, dx));
      }
    }
  } else if (kind == "point" || kind == "basepoint") {
    if (!pts.empty()) {
      out.set("point", *pts[0]);
    }
  } else if (kind == "flip") {
    if (pts.size() >= 2) {
      out.set("p1", *pts[0]);
      out.set("p2", *pts[1]);
    }
    if (const Value* f = find(70)) {
      const Value& fv = item(*f, 1);
      out.set("flipped", fv.is_number() && fv.as_number() == 1);
    }
  }
  return out;
}

std::string_view param_kind(std::string_view t) {
  static const std::map<std::string_view, std::string_view> kParams = {
      {"BLOCKVISIBILITYPARAMETER", "visibility"},
      {"BLOCKLINEARPARAMETER", "linear"},
      {"BLOCKPOLARPARAMETER", "polar"},
      {"BLOCKXYPARAMETER", "xy"},
      {"BLOCKROTATIONPARAMETER", "rotation"},
      {"BLOCKALIGNMENTPARAMETER", "alignment"},
      {"BLOCKFLIPPARAMETER", "flip"},
      {"BLOCKPOINTPARAMETER", "point"},
      {"BLOCKBASEPOINTPARAMETER", "basepoint"},
      {"BLOCKLOOKUPPARAMETER", "lookup"},
  };
  const auto it = kParams.find(t);
  return it == kParams.end() ? "other" : it->second;
}
std::string_view action_kind(std::string_view t) {
  static const std::map<std::string_view, std::string_view> kActions = {
      {"BLOCKMOVEACTION", "move"},
      {"BLOCKSTRETCHACTION", "stretch"},
      {"BLOCKSCALEACTION", "scale"},
      {"BLOCKROTATEACTION", "rotate"},
      {"BLOCKFLIPACTION", "flip"},
      {"BLOCKARRAYACTION", "array"},
      {"BLOCKPOLARSTRETCHACTION", "polarstretch"},
      {"BLOCKLOOKUPACTION", "lookup"},
  };
  const auto it = kActions.find(t);
  return it == kActions.end() ? "other" : it->second;
}
/// /^BLOCK.*<suffix>$/
bool block_type(const Value& t, std::string_view suffix) {
  const std::string s = js_string(t);
  return s.size() >= 5 + suffix.size() && s.starts_with("BLOCK") && s.ends_with(suffix);
}

/// What the reference pass needs of a definition (copied out of the model, so nothing is held across an edit).
struct Param {
  double node = 0;
  std::string kind;
  bool has_states = false;  // a decoded visibility parameter
  std::vector<State> states;
};
struct Def {
  std::size_t block = 0;
  std::vector<Param> params;
};

/// The visibility state a block shows: its entities line up with the definition's (a copy holds them all, in order,
/// the hidden ones flagged invisible); the state whose list is exactly the shown ones. → name | nullopt
std::optional<std::string> state_shown(const Param& vis, const Value* def, const Value* blk) {
  if (blk == nullptr || def == nullptr) {
    return std::nullopt;
  }
  static const Array kEmpty;
  const Value& be = member(*blk, "entities");
  const Value& de = member(*def, "entities");
  const Array& bl = be.is_array() ? be.as_array() : kEmpty;
  const Array& dl = de.is_array() ? de.as_array() : kEmpty;
  if (bl.size() != dl.size()) {
    return std::nullopt;
  }
  std::set<std::string> shown;
  for (std::size_t i = 0; i < bl.size(); ++i) {
    if (!member(bl[i], "invisible").truthy()) {
      shown.insert(hex_key(member(dl[i], "handle")));
    }
  }
  for (const State& s : vis.states) {
    if (s.entities.size() != shown.size()) {
      continue;
    }
    bool all = true;
    for (const std::string& h : s.entities) {
      if (!shown.contains(hex_key(Value(h)))) {
        all = false;
        break;
      }
    }
    if (all) {
      return s.name;
    }
  }
  return std::nullopt;
}

}  // namespace

/// Add block.dynamic, block.dynamicOf and insert.dyn to a model (see the header). What fails is left out.
void read_dynamic(Value& model) {
  if (!model.is_object()) {
    return;
  }
  static const Value kEmptyObject = Value::object();
  // objects by handle (the last one of a handle wins, as Map.set does)
  std::unordered_map<std::string, const Value*> objs;
  if (const Value& ol = member(model, "objects"); ol.is_array()) {
    for (const Value& o : ol.as_array()) {
      objs[hex_key(member(o, "handle"))] = &o;
    }
  }
  const auto obj = [&](const std::string& k) -> const Value* {
    const auto it = objs.find(k);
    return it == objs.end() ? nullptr : it->second;
  };
  const auto dict = [&](const Value& h) -> const Value& {
    const Value* o = obj(hex_key(h));
    if (o != nullptr) {
      const Value& e = member(*o, "entries");
      if (e.truthy() && (e.is_object() || e.is_array())) {
        return e.is_object() ? e : kEmptyObject;
      }
    }
    return kEmptyObject;
  };
  std::unordered_map<std::string, const Value*> recs;
  if (const Value& bl = member(member(model, "tables"), "blockRecords"); bl.is_array()) {
    for (const Value& r : bl.as_array()) {
      recs[hex_key(member(r, "handle"))] = &r;
    }
  }

  const auto blocks_len = [&]() -> std::size_t {
    const Value& b = member(model, "blocks");
    return b.is_array() ? b.as_array().size() : 0;
  };
  const auto block_at = [&](std::size_t i) -> Value& { return model.find("blocks")->as_array()[i]; };

  std::map<std::string, Def> defs;  // definition record → what the references need
  double n = 0;
  double failed = 0;
  for (std::size_t bi = 0; bi < blocks_len(); ++bi) {
    const std::string rec_key = hex_key(member(block_at(bi), "record"));
    const auto rit = recs.find(rec_key);
    if (rit == recs.end()) {
      continue;
    }
    const Value& rr = *rit->second;
    const Value& btag = member(member(rr, "xdata"), "AcDbBlockRepBTag");
    std::optional<std::string> dyn_of;
    if (btag.is_array()) {
      for (const Value& t : btag.as_array()) {
        if (is_code(t, 1005)) {
          const std::string tk = hex_key(item(t, 1));
          if (recs.contains(tk)) {
            dyn_of = tk;
          }
          break;
        }
      }
    }
    if (dyn_of) {
      block_at(bi).set("dynamicOf", *dyn_of);
    }
    const Value* g = nullptr;
    if (member(rr, "extDict").truthy()) {
      g = obj(hex_key(member(dict(member(rr, "extDict")), "ACAD_ENHANCEDBLOCK")));
    }
    if (g == nullptr || !member(*g, "type").is_string() || member(*g, "type").as_string() != "ACAD_EVALUATION_GRAPH" ||
        !member(*g, "raw").truthy()) {
      continue;
    }
    try {
      Array params;
      Array actions;
      Def def;
      def.block = bi;
      for (const Node& nd : graph_nodes(*g)) {
        const Value* o = obj(nd.handle);
        if (o == nullptr) {
          continue;
        }
        const Value& type = member(*o, "type");
        if (block_type(type, "PARAMETER")) {
          Param pi;
          pi.node = nd.node;
          pi.kind = std::string(type.is_string() ? param_kind(type.as_string()) : "other");
          Value p =
              make_object({{"node", nd.node}, {"kind", pi.kind}, {"handle", nd.handle}, {"name", element_name(*o)}});
          if (pi.kind == "visibility") {
            try {
              Visibility v = visibility_of(*o);
              p.set("name", v.name);
              p.set("label", v.label);
              p.set("description", v.description);
              p.set("point", v.point);
              p.set("grip", v.grip);
              p.set("all", hex_array(v.all));
              Array states;
              for (const State& s : v.states) {
                states.push_back(make_object(
                    {{"name", s.name}, {"entities", hex_array(s.entities)}, {"elements", hex_array(s.elements)}}));
              }
              p.set("states", std::move(states));
              pi.has_states = true;
              pi.states = std::move(v.states);
            } catch (const std::exception&) {
              p.set("undecoded", true);
            }
          }
          params.push_back(std::move(p));
          def.params.push_back(std::move(pi));
        } else if (block_type(type, "ACTION")) {
          actions.push_back(make_object({{"node", nd.node},
                                         {"kind", type.is_string() ? action_kind(type.as_string()) : "other"},
                                         {"handle", nd.handle},
                                         {"name", element_name(*o)}}));
        }
      }
      Value dyn = make_object(
          {{"graph", hex_key(member(*g, "handle"))}, {"params", std::move(params)}, {"actions", std::move(actions)}});
      block_at(bi).set("dynamic", std::move(dyn));
      defs[rec_key] = std::move(def);
      n += 1;
    } catch (const std::exception&) {
      failed += 1;
    }
  }

  // references: INSERT xdict → AcDbBlockRepresentation → AcDbRepData (→ definition) + AppDataCache →
  // ACAD_ENHANCEDBLOCKDATA
  const auto block_named = [&](const Value& name) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < blocks_len(); ++i) {
      if (member(block_at(i), "name") == name) {
        return i;
      }
    }
    return std::nullopt;
  };
  double refs = 0;
  for (std::size_t bi = 0; bi < blocks_len(); ++bi) {
    const auto ents_len = [&]() -> std::size_t {
      const Value& e = member(block_at(bi), "entities");
      return e.is_array() ? e.as_array().size() : 0;
    };
    for (std::size_t ei = 0; ei < ents_len(); ++ei) {
      Value dyn;
      {
        const Value& e = member(block_at(bi), "entities").as_array()[ei];
        const Value& type = member(e, "type");
        if (!type.is_string() || type.as_string() != "INSERT") {
          continue;
        }
        const Value& rep = member(e, "extDict").truthy()
                               ? dict(member(dict(member(e, "extDict")), "AcDbBlockRepresentation"))
                               : kEmptyObject;
        const Value* rd = obj(hex_key(member(rep, "AcDbRepData")));
        std::string def;
        if (rd != nullptr) {
          const Value& raw = member(*rd, "raw");
          const Value& h = member(raw, "h");
          if (raw.truthy() && h.truthy() && item(h, 0).truthy()) {
            def = hex_key(item(item(h, 0), 1));
          }
        }
        if (!defs.contains(def)) {  // a reference straight to the definition (no representation data)
          const auto own = block_named(member(e, "block"));
          const std::string ok = own ? hex_key(member(block_at(*own), "record")) : std::string();
          def = own && defs.contains(ok) ? ok : std::string();
        }
        const auto dit = defs.find(def);
        if (dit == defs.end()) {
          continue;
        }
        const Def& d = dit->second;
        // byNode: the last parameter of a node wins
        std::map<double, const Param*> by_node;
        for (const Param& p : d.params) {
          by_node[p.node] = &p;
        }
        // Object.entries order: array-index keys in numeric order, then the others in insertion order
        const Value& cache = dict(member(dict(member(rep, "AppDataCache")), "ACAD_ENHANCEDBLOCKDATA"));
        std::vector<const base::Member*> entries;
        std::vector<std::pair<std::uint64_t, const base::Member*>> indexed;
        for (const base::Member& m : cache.as_object()) {
          if (const auto ix = array_index(m.key)) {
            indexed.emplace_back(*ix, &m);
          } else {
            entries.push_back(&m);
          }
        }
        std::ranges::stable_sort(indexed, [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<const base::Member*> ordered;
        ordered.reserve(indexed.size() + entries.size());
        for (const auto& [ix, m] : indexed) {
          ordered.push_back(m);
        }
        ordered.insert(ordered.end(), entries.begin(), entries.end());

        Array values;
        struct Seen {
          double node;
          std::optional<std::string> state;
        };
        std::vector<Seen> seen;
        for (const base::Member* m : ordered) {
          const double node = js_number(m->key);
          const Value* x = obj(hex_key(m->value));
          if (!is_integer(node) || x == nullptr || !member(*x, "type").is_string() ||
              member(*x, "type").as_string() != "XRECORD") {
            continue;
          }
          const auto pit = by_node.find(node == 0 ? 0.0 : node);
          if (pit == by_node.end()) {
            continue;  // grips and actions keep values too: left as they are in the file
          }
          const Param& p = *pit->second;
          const Value& data = member(*x, "data");
          Value v = make_object({{"node", node},
                                 {"kind", p.kind},
                                 {"xrecord", hex_key(m->value)},
                                 {"data", data.truthy() ? data : Value(Array{})}});
          Value dec = value_of(p.kind, data);
          std::optional<std::string> st;
          for (base::Member& f : dec.as_object()) {
            if (f.key == "state") {
              st = f.value.as_string();
            }
            v.set(f.key, std::move(f.value));
          }
          seen.push_back({node, st});
          values.push_back(std::move(v));
        }
        const Param* vis = nullptr;
        for (const Param& p : d.params) {
          if (p.kind == "visibility" && p.has_states) {
            vis = &p;
            break;
          }
        }
        const Seen* vv = nullptr;
        if (vis != nullptr) {
          for (const Seen& s : seen) {
            if (s.node == vis->node) {
              vv = &s;
              break;
            }
          }
        }
        std::optional<std::string> shown;
        if (vis != nullptr && (vv == nullptr || !vv->state)) {
          const auto own = block_named(member(e, "block"));
          shown = state_shown(*vis, &block_at(d.block), own ? &block_at(*own) : nullptr);
        }
        dyn = make_object({{"def", def}, {"values", std::move(values)}});
        if (vv != nullptr && vv->state) {
          dyn.set("state", *vv->state);
        } else if (shown && !shown->empty()) {
          dyn.set("state", *shown);
        }
      }
      // looked up again: nothing above is held across this edit
      model.find("blocks")->as_array()[bi].find("entities")->as_array()[ei].set("dyn", std::move(dyn));
      refs += 1;
    }
  }
  Value* report = model.find("report");
  if (report != nullptr && report->truthy() && report->is_object() && (n != 0 || failed != 0)) {
    report->set("dynamicBlocks", n);
    report->set("dynamicReferences", refs);
    if (failed != 0) {
      report->set("dynamicFailed", failed);
    }
  }
}

}  // namespace cc::dwg
