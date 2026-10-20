// SPDX-License-Identifier: Apache-2.0
// dxf.js writeDxf (CadCraft 0.5) in C++: a CadCraft drawing → ASCII DXF R12 (AC1009). One class whose members are the
// JS function's closure variables, called in the JS's order (the order fixes the R12 names given out, the anonymous
// block numbers and the counts' key order).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/files.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/m3d/drawing3d.hpp"
#include "cc/save/dxf.hpp"
#include "dxf_common.hpp"

namespace cc::save {

using base::Array;
using base::Value;
using detail::f12;

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = kPi * 2;
constexpr double kEps = 1e-9;

double num(const Value& v) {
  return base::js_to_number(v);
}
bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
Value or_(const Value& v, const Value& d) {
  return v.truthy() ? v : d;
}
Value nn(const Value& v, const Value& d) {
  return nullish(v) ? d : v;
}
std::string str(const Value& v) {
  return base::js_string(v);
}
int i32(const Value& v) {
  return base::js_to_int32(num(v));
}
double deg(double r) {
  return (r * 180) / kPi;
}
bool is_num(const Value& v, double n) {
  return v.is_number() && v.as_number() == n;
}
const Array& arr(const Value& v) {
  static const Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}
// of a temporary it would dangle: hold the value first, or read the member in place with at()
const Array& arr(const Value&& v) = delete;
/// o[k] in place (a plain object's member; undefined otherwise, a compact point included)
const Value& at(const Value& o, std::string_view k) {
  static const Value kUndefined;
  if (!o.is_object() || o.is_point()) {
    return kUndefined;
  }
  const Value* v = o.find(k);
  return v != nullptr ? *v : kUndefined;
}
const Array& arr(const Value& o, std::string_view k) {
  return arr(at(o, k));
}
std::vector<const base::Member*> entries(const Value& o) {
  if (!o.is_object() || o.is_point()) {
    return {};
  }
  return base::js_members(o.as_object());
}
/// encodeText(String(s))
std::string encode(const Value& s) {
  return detail::encode_r12(str(s));
}
/// geom.js normAngle: into [0, 2π)
double norm_angle(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}
std::pair<double, double> arc_range(double a0, double a1) {
  a0 = norm_angle(a0);
  a1 = norm_angle(a1);
  if (a1 <= a0 + kEps) {
    a1 += kTau;
  }
  return {a0, a1};
}
struct Pt {
  double x = 0, y = 0;
};
/// geom.js ellipsePoints(e) (72 steps a turn)
std::vector<Pt> ellipse_points(const Value& e) {
  const double mx = num(e.get("mx"));
  const double my = num(e.get("my"));
  const double ma = base::js_hypot(mx, my);
  const double mi = ma * num(e.get("ratio"));
  const double rot = base::js_atan2(my, mx);
  const double et0 = num(e.get("t0"));
  const double et1 = num(e.get("t1"));
  const auto [t0, t1] = et1 - et0 >= kTau - kEps ? std::pair<double, double>{0, kTau} : arc_range(et0, et1);
  const double steps = std::max(8.0, std::ceil(72 * (t1 - t0) / kTau));
  const double c = base::js_cos(rot);
  const double s = base::js_sin(rot);
  const double cx = num(e.get("cx"));
  const double cy = num(e.get("cy"));
  std::vector<Pt> pts;
  if (!(steps >= 0) || steps > 1e6) {
    return pts;
  }
  const auto n = static_cast<std::int64_t>(steps);
  for (std::int64_t i = 0; i <= n; ++i) {
    const double t = t0 + (t1 - t0) * static_cast<double>(i) / steps;
    const double x = ma * base::js_cos(t);
    const double y = mi * base::js_sin(t);
    pts.push_back({cx + x * c - y * s, cy + x * s + y * c});
  }
  return pts;
}
/// geom.js clipInfinite(p, d, rect, ray): [t0, t1] inside the rectangle, or nothing (Liang–Barsky)
std::optional<std::pair<double, double>> clip_infinite(Pt p, Pt d, const cad::Bounds& r, bool ray) {
  double t0 = ray ? 0 : -std::numeric_limits<double>::infinity();
  double t1 = std::numeric_limits<double>::infinity();
  const std::array<std::array<double, 4>, 2> axes = {{{d.x, r.min_x, r.max_x, p.x}, {d.y, r.min_y, r.max_y, p.y}}};
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
    t0 = std::isnan(a) ? a : std::max(t0, a);  // Math.max / Math.min: NaN wins
    t1 = std::isnan(b) ? b : std::min(t1, b);
    if (t0 > t1) {
      return std::nullopt;
    }
  }
  if (std::isfinite(t0) && std::isfinite(t1)) {
    return std::pair{t0, t1};
  }
  return std::nullopt;
}
using Mat = std::array<double, 6>;
Mat m_mul(const Mat& m, const Mat& n) {
  return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],        m[0] * n[2] + m[2] * n[3],
          m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
Pt m_apply(const Mat& m, Pt p) {
  return {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]};
}
/// ha 'center' → 1, 'right' → 2, else 0 (TEXT / ATTRIB group 72)
int h_code(const Value& ha) {
  if (ha == Value("center")) {
    return 1;
  }
  return ha == Value("right") ? 2 : 0;
}
bool th_type(const Value& t) {
  if (!t.is_string()) {
    return false;
  }
  const std::string& s = t.as_string();
  return s == "line" || s == "circle" || s == "arc" || s == "poly" || s == "point" || s == "text";
}

std::vector<const Value*> ptrs(const Array& a) {
  std::vector<const Value*> out;
  out.reserve(a.size());
  for (const Value& v : a) {
    out.push_back(&v);
  }
  return out;
}

struct VtxIn {
  Value x, y, b, sw, ew;
};
struct Opt {
  bool top = false;
  bool paper = false;
};
struct DimBlock {
  std::string name;
  const Array* own = nullptr;  // the dimension's own block's entities, or
  Array made;                  // its geometry
  [[nodiscard]] const Array& ents() const { return own != nullptr ? *own : made; }
};

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) — a writer lives for one write_dxf call
class R12 {
 public:
  R12(const doc::Doc& d, const i18n::Catalog& w) : doc_(d), words_(w) {}
  DxfSaveResult run();

 private:
  const doc::Doc& doc_;
  const i18n::Catalog& words_;
  detail::DxfLines L_;
  Value counts_ = Value::object();
  std::optional<cad::Bounds> extents_;
  cad::Bounds ext_;
  std::unordered_set<std::string> used_;
  std::unordered_map<std::string, std::string> name_map_;
  int anon_ = 0;
  std::unordered_map<std::string, std::string> block_names_;
  std::unordered_map<const Value*, std::string> dim_name_;
  std::vector<DimBlock> dim_blocks_;
  std::vector<std::pair<std::string, Value>> lt_used_;  // R12 name → pattern (a Map)
  std::vector<std::string> no_plot_;
  cad::BlockLookup blocks_ = [this](const std::string& n) { return doc_.blocks().get(n); };

  void w(int code, const char* v) { L_.w(code, std::string_view(v)); }
  template <class V>
  void w(int code, const V& v) {
    L_.w(code, v);
  }
  void wf(int code, const Value& v) { L_.w(code, f12(v)); }
  void wf(int code, double v) { L_.w(code, f12(v)); }
  /// zf(v): f(v) when finite, else '0.0'
  void wz(int code, const Value& v) { L_.w(code, finite(v) ? f12(v) : std::string("0.0")); }
  void xy(int c, const Value& p) {
    wf(c, p.get("x"));
    wf(c + 10, p.get("y"));
    w(c + 20, "0.0");
  }
  void xy(int c, Pt p) {
    wf(c, p.x);
    wf(c + 10, p.y);
    w(c + 20, "0.0");
  }
  void note(const std::string& k, double n = 1) {
    const Value have = counts_.get(k);
    counts_.set(k, (have.truthy() ? num(have) : 0) + n);
  }
  [[nodiscard]] double count(std::string_view k) const {
    const Value v = counts_.get(k);
    return v.truthy() ? num(v) : 0;
  }

  std::string r12_name(const std::string& name);
  std::string anon_name(const std::string& prefix) {
    std::string n;
    do {
      n = prefix + std::to_string(++anon_);
    } while (used_.contains(n));
    used_.insert(n);
    return n;
  }
  std::string layer_name(const Value& n) { return r12_name(str(n)); }
  cad::Bounds drawing_extents();
  std::optional<std::string> lt_name(const Value& n);
  Value color_code(const Value& c) {
    if (nullish(c)) {
      return {nullptr};
    }
    if (is_num(c, 0)) {
      return 0;
    }
    if (c.is_string()) {
      note("truecolor");
      return cad::nearest_aci(c.as_string());
    }
    return c;
  }

  void layout_xdata(const doc::Layout& l);
  std::vector<std::string> frz_names(const Value& e);
  void viewport(const Value& e, int id, const std::function<void()>& extra);
  void head(std::string_view type, const Value& e, const Opt& o);
  void xd(const Value& e);
  std::string ref_;  // the CCREF key of the top-level entity being written (o.ref)
  void grow(double x, double y, const Opt& o) {
    if (!o.top) {
      return;
    }
    ext_.min_x = std::min(ext_.min_x, x);
    ext_.min_y = std::min(ext_.min_y, y);
    ext_.max_x = std::max(ext_.max_x, x);
    ext_.max_y = std::max(ext_.max_y, y);
  }
  void polyline(const Value& e, const std::vector<VtxIn>& pts, bool closed, const Opt& o,
                const std::function<void()>& xd_fn);
  void text_out(const Value& e, const std::string& s, double x, double y, const Opt& o, bool baseline);
  void attribute(std::string_view type, const Value& t, const Opt& o, const Value* d);
  std::string attr_value(const Value& s);
  void dimension(const Value& e, const Opt& o);
  void entity(const Value& e, const Opt& o);
  void block(const std::string& name, const Value& base_pt, const std::vector<const Value*>& ents,
             const std::function<void()>& extra, const Value* attdefs = nullptr,
             const std::optional<std::string>& xref_path = std::nullopt);
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

std::string R12::r12_name(const std::string& name) {
  if (const auto it = name_map_.find(name); it != name_map_.end()) {
    return it->second;
  }
  // a linked drawing's "Plan|Walls" → PLAN$0$WALLS, as a bind names it
  std::string n;
  for (const std::uint16_t u : base::to_utf16(base::js_to_upper(name))) {
    if (n.size() >= 31) {
      break;
    }
    if (u == '|') {
      n += "$0$";
    } else if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '$' || u == '_' || u == '-') {
      n += static_cast<char>(u);
    } else {
      n += '_';
    }
  }
  if (n.size() > 31) {
    n.resize(31);
  }
  if (n.empty()) {
    n = "UNNAMED";
  }
  const std::string stem = n.substr(0, 27);
  int k = 1;
  while (used_.contains(n)) {
    n = stem + "_" + std::to_string(k++);
  }
  used_.insert(n);
  name_map_.emplace(name, n);
  if (n != name) {
    note("renamed");
  }
  return n;
}

cad::Bounds R12::drawing_extents() {
  if (extents_) {
    return *extents_;
  }
  cad::Bounds b;
  doc_.model()->each([&](const doc::Id&, const Value& x) {
    const cad::Bounds q = cad::entity_bounds(x, blocks_);
    if (q.valid()) {
      b.min_x = std::min(b.min_x, q.min_x);
      b.min_y = std::min(b.min_y, q.min_y);
      b.max_x = std::max(b.max_x, q.max_x);
      b.max_y = std::max(b.max_y, q.max_y);
    }
  });
  if (!b.valid() || (b.max_x <= b.min_x && b.max_y <= b.min_y)) {
    b = {.min_x = -100, .min_y = -100, .max_x = 100, .max_y = 100};
  }
  extents_ = b;
  return b;
}

std::optional<std::string> R12::lt_name(const Value& n) {
  if (!n.truthy()) {
    return std::nullopt;
  }
  const std::string s = str(n);
  const std::string low = base::js_to_lower(s);
  if (low == "bylayer") {
    return std::nullopt;
  }
  if (low == "byblock") {
    return "BYBLOCK";
  }
  if (low == "continuous") {
    return "CONTINUOUS";
  }
  Value pat = detail::pattern_of(doc_, s);
  if (!pat.truthy()) {
    return "CONTINUOUS";
  }
  // its own table: names need not differ from layers'
  std::string r;
  for (const std::uint16_t u : base::to_utf16(base::js_to_upper(s))) {
    if (r.size() >= 31) {
      break;
    }
    r += (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '$' || u == '_' || u == '-' ? static_cast<char>(u)
                                                                                              : '_';
  }
  for (auto& [k, v] : lt_used_) {
    if (k == r) {
      v = std::move(pat);
      return r;
    }
  }
  lt_used_.emplace_back(r, std::move(pat));
  return r;
}

void R12::layout_xdata(const doc::Layout& l) {
  const Value& p = l.paper;
  w(1001, "CADCRAFT");
  w(1000, "LAYOUT");
  w(1000, std::string_view(detail::encode_r12(l.name)).substr(0, 255));
  w(1000, p.get("unit"));
  w(1000, or_(p.get("key"), ""));
  for (const char* k : {"x0", "y0", "w", "h"}) {
    w(1040, f12(p.get(k)));
  }
  // R12 layers cannot say "do not plot": CadCraft keeps the list here
  if (!no_plot_.empty()) {
    w(1000, "NOPLOT");
    for (const std::string& n : no_plot_) {
      w(1000, n);
    }
  }
}

std::vector<std::string> R12::frz_names(const Value& e) {
  std::unordered_set<std::string> have;
  doc_.layers().each([&](const std::string& n, const Value&) { have.insert(base::js_to_upper(n)); });
  std::vector<std::string> out;
  for (const Value& n : arr(e, "frz")) {
    if (n.is_string() && have.contains(base::js_to_upper(n.as_string()))) {
      std::string x = layer_name(n);
      if (std::ranges::find(out, x) == out.end()) {
        out.push_back(std::move(x));
      }
    }
  }
  return out;
}

void R12::viewport(const Value& e, int id, const std::function<void()>& extra) {
  w(0, "VIEWPORT");
  w(8, layer_name(or_(e.get("layer"), "0")));
  if (const Value c = color_code(e.get("color")); !c.is_null()) {
    w(62, c);
  }
  w(67, 1);
  wf(10, e.get("x"));
  wf(20, e.get("y"));
  w(30, "0.0");
  wf(40, e.get("w"));
  wf(41, e.get("h"));
  w(68, e.get("on") == Value(false) ? 0 : id);  // 68 = 0: off (P.7)
  w(69, id);
  // The view, as R12 keeps it (ACAD MVIEW xdata: target, direction, twist, height, centre, …)
  w(1001, "ACAD");
  w(1000, "MVIEW");
  w(1002, "{");
  w(1070, 16);
  w(1010, "0.0");
  w(1020, "0.0");
  w(1030, "0.0");
  w(1010, "0.0");
  w(1020, "0.0");
  w(1030, "1.0");
  const Value tw = e.get("tw");
  wf(1040, finite(tw) ? tw : Value(0));
  wf(1040, num(e.get("h")) / num(e.get("s")));  // twist (radians, DR.8), height, centre
  wf(1040, e.get("cx"));
  wf(1040, e.get("cy"));
  w(1040, "50.0");
  w(1040, "0.0");
  w(1040, "0.0");
  for (const int v : {0, 100, 1, 3, 0, 0, 0, 0}) {
    w(1070, v);
  }
  for (const char* v : {"0.0", "0.0", "0.0", "1.0", "1.0", "1.0", "1.0"}) {
    w(1040, v);
  }
  w(1070, 0);
  w(1002, "{");
  for (const std::string& n : frz_names(e)) {
    w(1003, n);
  }
  w(1002, "}");
  w(1002, "}");  // the layers frozen in it (VP.1)
  if (extra) {
    extra();
  } else if (e.get("locked").truthy()) {  // display locked (P.7): R12 has no flag for it
    w(1001, "CADCRAFT");
    w(1000, "CCVPLOCK");
  }
}

void R12::head(std::string_view type, const Value& e, const Opt& o) {
  w(0, type);
  w(8, layer_name(or_(e.get("layer"), "0")));
  if (const auto lt = lt_name(e.get("lt"))) {
    w(6, *lt);
  }
  if (const Value c = color_code(e.get("color")); !c.is_null()) {
    w(62, c);
  }
  if (o.paper) {
    w(67, 1);
  }
  if (const Value th = e.get("th"); th.truthy() && th_type(e.get("type")) && type != "VERTEX" && type != "SEQEND") {
    wf(39, th);  // thickness (P.5)
  }
  if (const Value tr = e.get("tr"); finite(tr) || tr == Value("ByBlock")) {
    note("tr");
  }
}

void R12::xd(const Value& e) {
  const Value x = cad::props_xdata(e, true);
  std::vector<std::string> cc;
  if (!ref_.empty()) {
    cc.push_back("CCREF:" + ref_);
  }
  for (const Value& it : arr(x, "CADCRAFT")) {
    const Array& p = arr(it);
    cc.push_back(p.size() > 1 ? str(p[1]) : std::string("undefined"));
  }
  if (!cc.empty()) {
    w(1001, "CADCRAFT");
    for (const std::string& v : cc) {
      w(1000, detail::encode_r12(v));
    }
  }
  for (const base::Member* m : entries(x)) {
    if (m->key == "CADCRAFT") {
      continue;
    }
    w(1001, base::js_to_upper(m->key));
    for (const Value& it : arr(m->value)) {
      const Array& p = arr(it);
      if (p.size() < 2) {
        continue;
      }
      const int code = i32(p[0]);
      if (code == 1000) {
        w(code, encode(p[1]));
      } else if (code == 1040) {
        wf(code, num(p[1]));
      } else {
        w(code, p[1]);
      }
    }
  }
  if (const Value mat = e.get("mat"); mat.is_string() && !mat.as_string().empty()) {
    note("material");
  }
}

void R12::polyline(const Value& e, const std::vector<VtxIn>& pts, bool closed, const Opt& o,
                   const std::function<void()>& xd_fn) {
  // elevation (P.3) in the polyline's own point; widths (P.7): the default pair here, a vertex's own on it
  const bool poly = e.get("type") == Value("poly");
  const Value ew = e.get("w");
  const double gw = poly && finite(ew) && ew.as_number() > 0 ? ew.as_number() : 0;
  bool each = false;
  if (poly) {
    for (const VtxIn& p : pts) {
      each = each || finite(p.sw) || finite(p.ew);
    }
  }
  head("POLYLINE", e, o);
  w(66, 1);
  w(10, "0.0");
  w(20, "0.0");
  if (poly) {
    wz(30, e.get("z"));
  } else {
    w(30, "0.0");
  }
  w(70, closed ? 1 : 0);
  if (gw != 0) {
    wf(40, gw);
    wf(41, gw);
  }
  if (xd_fn) {
    xd_fn();
  }
  const std::string lay = layer_name(or_(e.get("layer"), "0"));
  for (const VtxIn& p : pts) {
    w(0, "VERTEX");
    w(8, lay);
    if (o.paper) {
      w(67, 1);
    }
    wf(10, p.x);
    wf(20, p.y);
    if (poly) {
      wz(30, e.get("z"));
    } else {
      w(30, "0.0");
    }
    if (each) {
      wf(40, finite(p.sw) ? num(p.sw) : gw);
      wf(41, finite(p.ew) ? num(p.ew) : gw);
    }
    if (p.b.truthy()) {
      wf(42, p.b);
    }
  }
  w(0, "SEQEND");
  w(8, lay);
  if (o.paper) {
    w(67, 1);
  }
}

void R12::text_out(const Value& e, const std::string& s, double x, double y, const Opt& o, bool baseline) {
  head("TEXT", e, o);
  wf(10, x);
  wf(20, y);
  wz(30, e.get("z"));
  wf(40, e.get("h"));
  w(1, detail::encode_r12(s));
  if (const Value r = e.get("rot"); r.truthy()) {
    wf(50, deg(num(r)));
  }
  if (const Value wfv = e.get("wf"); wfv.truthy() && !is_num(wfv, 1)) {
    wf(41, wfv);
  }
  if (const Value ob = e.get("obl"); ob.truthy()) {
    wf(51, deg(num(ob)));  // oblique angle (P.7)
  }
  if (const Value st = e.get("style"); st.truthy() && !detail::is_standard(st)) {
    w(7, r12_name(str(st)));
  }
  const Value ha = e.get("ha");
  const Value va = baseline ? Value("baseline") : e.get("va");
  const int h72 = h_code(ha);
  int v73 = 0;
  if (va == Value("middle")) {
    v73 = 2;
  } else if (va == Value("top")) {
    v73 = 3;
  } else if (va == Value("bottom")) {
    v73 = 1;
  }
  if (h72 != 0) {
    w(72, h72);
  }
  if (h72 != 0 || v73 != 0) {
    wf(11, x);
    wf(21, y);
    wz(31, e.get("z"));
  }
  if (v73 != 0) {
    w(73, v73);
  }
  xd(e);
}

void R12::attribute(std::string_view type, const Value& t, const Opt& o, const Value* d) {
  head(type, t, o);
  wf(10, t.get("x"));
  wf(20, t.get("y"));
  w(30, "0.0");
  wf(40, t.get("h"));
  w(1, attr_value(t.get("s")));
  if (d != nullptr) {
    w(3, attr_value(or_(d->get("prompt"), "")));
  }
  w(2, encode(or_(t.get("tag"), "TAG")));
  w(70, (t.get("invisible").truthy() ? 1 : 0) | (d != nullptr && d->get("constant").truthy() ? 2 : 0) |
            (d != nullptr && d->get("preset").truthy() ? 8 : 0));
  if (const Value r = t.get("rot"); r.truthy()) {
    wf(50, deg(num(r)));
  }
  if (const Value wfv = t.get("wf"); wfv.truthy() && !is_num(wfv, 1)) {
    wf(41, wfv);
  }
  const Value ha = t.get("ha");
  const Value va = t.get("va");
  const int h72 = h_code(ha);
  int v74 = 0;
  if (va == Value("middle")) {
    v74 = 2;
  } else if (va == Value("top")) {
    v74 = 3;
  } else if (va == Value("bottom")) {
    v74 = 1;
  }
  if (h72 != 0) {
    w(72, h72);
  }
  if (h72 != 0 || v74 != 0) {
    wf(11, t.get("x"));
    wf(21, t.get("y"));
    w(31, "0.0");
  }
  if (v74 != 0) {
    w(74, v74);
  }
}

/// R12 strings stop at 255 characters.
std::string R12::attr_value(const Value& s) {
  std::string e = detail::encode_r12(cad::one_line(nullish(s) ? std::string() : str(s)));
  if (e.size() <= 255) {
    return e;
  }
  note("longattr");
  e.resize(255);
  // .replace(/\\U\+[0-9A-F]{0,3}$|\\U?$|%%?$/, ''): the leftmost place where an alternative runs to the end
  const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); };
  for (std::size_t i = e.size() >= 6 ? e.size() - 6 : 0; i < e.size(); ++i) {
    const std::string_view r = std::string_view(e).substr(i);
    bool m = false;
    if (r.starts_with("\\U+") && r.size() <= 6) {
      m = std::ranges::all_of(r.substr(3), hex);
    }
    m = m || r == "\\" || r == "\\U" || r == "%" || r == "%%";
    if (m) {
      e.resize(i);
      break;
    }
  }
  return e;
}

void R12::dimension(const Value& e, const Opt& o) {
  const auto it = dim_name_.find(&e);
  if (it == dim_name_.end()) {
    return;
  }
  head("DIMENSION", e, o);
  w(2, it->second);
  const Value tp = cad::dim_text_point(e);
  const Value ds = cad::dim_style(e);
  const std::string kind = str(e.get("kind"));
  const Value p1 = e.get("p1");
  const Value p2 = e.get("p2");
  const Value c = e.get("c");
  const Value loc = e.get("loc");
  const auto X = [](const Value& p) { return num(p.get("x")); };
  const auto Y = [](const Value& p) { return num(p.get("y")); };
  const Value text = e.get("text");
  if (kind == "linear" || kind == "aligned") {
    const double r = kind == "aligned" ? base::js_atan2(Y(p2) - Y(p1), X(p2) - X(p1)) : num(or_(e.get("rot"), 0));
    const Pt u = {base::js_cos(r), base::js_sin(r)};
    const double d2 = (X(p2) - X(loc)) * u.x + (Y(p2) - Y(loc)) * u.y;
    xy(10, Pt{X(loc) + u.x * d2, Y(loc) + u.y * d2});
    xy(11, tp);
    w(70, kind == "aligned" ? 1 : 0);
    if (text.truthy()) {
      w(1, encode(text));
    }
    xy(13, p1);
    xy(14, p2);
    if (kind == "linear") {
      wf(50, deg(norm_angle(num(or_(e.get("rot"), 0)))));
    }
  } else if (kind == "radius" || kind == "diameter") {
    const double r = base::js_hypot(X(p1) - X(c), Y(p1) - Y(c));
    const double dl = base::js_hypot(X(loc) - X(c), Y(loc) - Y(c));
    const double a =
        dl > 1e-12 ? base::js_atan2(Y(loc) - Y(c), X(loc) - X(c)) : base::js_atan2(Y(p1) - Y(c), X(p1) - X(c));
    const Pt near = {X(c) + r * base::js_cos(a), Y(c) + r * base::js_sin(a)};
    const Pt far = {X(c) - r * base::js_cos(a), Y(c) - r * base::js_sin(a)};
    if (kind == "radius") {
      xy(10, c);
    } else {
      xy(10, far);
    }
    xy(11, tp);
    w(70, kind == "radius" ? 4 : 3);
    if (text.truthy()) {
      w(1, encode(text));
    }
    xy(15, near);
    w(40, "0.0");
  } else if (kind == "angular") {
    xy(10, loc);
    xy(11, tp);
    w(70, 5);
    if (text.truthy()) {
      w(1, encode(text));
    }
    xy(13, p1);
    xy(14, p2);
    xy(15, c);
  }
  const Value dsn = e.get("dimstyle");
  w(3, dsn.truthy() && !detail::is_standard(dsn) ? r12_name(str(dsn)) : std::string("STANDARD"));
  // this dimension's own settings, as AutoCAD stores per-dimension overrides: every variable R12 has (dwg-props.js; its
  // DIMUNIT carries CadCraft's feet and inches and fractions as the unstacked kinds 6, 7)
  w(1001, "ACAD");
  for (const Value& itv : cad::dstyle_items(ds, true)) {
    const Array& p = itv.as_array();
    const int code = i32(p.at(0));
    if (code == 1040) {
      wf(code, num(p.at(1)));
    } else if (code == 1000) {
      w(code, encode(p.at(1)));
    } else {
      w(code, p.at(1));
    }
  }
  // CadCraft's decimals and length format, the text rotation (no R12 field) and what no R12 variable holds (CCDIMST)
  w(1001, "CADCRAFT");
  w(1000, "DIM");
  w(1070, i32(ds.get("dec")));
  w(1070, i32(ds.get("adec")));
  w(1070, i32(ds.get("zin")));
  w(1070, or_(ds.get("lunit"), 2));
  if (const Value tr = e.get("trot"); tr.truthy()) {
    w(1000, "CCTROT:" + str(tr));
  }
  for (const Value& itv : cad::cc_dim_items(ds, true)) {
    w(1000, encode(itv.as_array().at(1)));
  }
  note("dims");
}

// The JS's switch. Recursive once at most: a multileader's parts are never multileaders.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size,misc-no-recursion)
void R12::entity(const Value& e, const Opt& o) {
  const std::string type = str(e.get("type"));
  const auto N = [&](std::string_view k) { return num(e.get(k)); };
  if (type == "xline" || type == "ray") {
    const auto tt = clip_infinite({N("x"), N("y")}, {N("dx"), N("dy")}, drawing_extents(), type == "ray");
    note("infinite");
    if (!tt || !(tt->second > tt->first)) {
      return;
    }
    const Pt a = {N("x") + N("dx") * tt->first, N("y") + N("dy") * tt->first};
    const Pt b = {N("x") + N("dx") * tt->second, N("y") + N("dy") * tt->second};
    head("LINE", e, o);
    wf(10, a.x);
    wf(20, a.y);
    w(30, "0.0");
    wf(11, b.x);
    wf(21, b.y);
    w(31, "0.0");
    grow(a.x, a.y, o);
    grow(b.x, b.y, o);
    xd(e);
  } else if (type == "line") {
    head("LINE", e, o);
    wf(10, e.get("x1"));
    wf(20, e.get("y1"));
    wz(30, e.get("z1"));
    wf(11, e.get("x2"));
    wf(21, e.get("y2"));
    wz(31, e.get("z2"));
    grow(N("x1"), N("y1"), o);
    grow(N("x2"), N("y2"), o);
    xd(e);
  } else if (type == "point") {
    head("POINT", e, o);
    wf(10, e.get("x"));
    wf(20, e.get("y"));
    wz(30, e.get("z"));
    grow(N("x"), N("y"), o);
    xd(e);
  } else if (type == "circle" || type == "arc") {
    head(type == "circle" ? "CIRCLE" : "ARC", e, o);
    wf(10, e.get("cx"));
    wf(20, e.get("cy"));
    wz(30, e.get("z"));
    wf(40, e.get("r"));
    if (type == "arc") {
      wf(50, deg(norm_angle(N("a0"))));
      wf(51, deg(norm_angle(N("a1"))));
    }
    grow(N("cx") - N("r"), N("cy") - N("r"), o);
    grow(N("cx") + N("r"), N("cy") + N("r"), o);
    xd(e);
  } else if (type == "poly") {
    std::vector<VtxIn> pts;
    for (const Value& p : arr(e, "pts")) {
      pts.push_back({p.get("x"), p.get("y"), p.get("b"), p.get("sw"), p.get("ew")});
    }
    polyline(e, pts, e.get("closed").truthy(), o, [&] { xd(e); });
    for (const VtxIn& p : pts) {
      grow(num(p.x), num(p.y), o);
    }
  } else if (type == "ellipse") {
    note("ellipse");
    std::vector<VtxIn> pts;
    for (const Pt& p : ellipse_points(e)) {
      pts.push_back({p.x, p.y, 0, Value(), Value()});
    }
    polyline(e, pts, N("t1") - N("t0") >= kTau - 1e-9, o, nullptr);
  } else if (type == "hatch") {
    if (e.get("wipe").truthy()) {
      return;  // a dimension's text background: DIMTFILL says it
    }
    std::vector<std::vector<VtxIn>> rings;
    for (const Value& l : arr(e, "loops")) {
      const Array& fl = arr(l);
      std::vector<VtxIn> pts;
      for (std::size_t k = 0; k + 1 < fl.size(); k += 2) {
        pts.push_back({fl[k], fl[k + 1], 0, Value(), Value()});
      }
      rings.push_back(std::move(pts));
    }
    // A filled triangle or quadrilateral (arrowheads, imported SOLIDs) is an R12 SOLID; anything else keeps its
    // outline.
    if (rings.size() == 1 && e.get("solid").truthy()) {
      std::vector<VtxIn> q = rings[0];
      if (q.size() > 1 && base::js_hypot(num(q[0].x) - num(q.back().x), num(q[0].y) - num(q.back().y)) < 1e-12) {
        q.pop_back();
      }
      if (q.size() >= 3 && q.size() <= 4) {
        head("SOLID", e, o);
        const VtxIn& d = q.size() == 4 ? q[3] : q[2];
        for (const auto& [c, p] : {std::pair<int, const VtxIn&>{10, q[0]}, {11, q[1]}, {12, d}, {13, q[2]}}) {
          wf(c, p.x);  // SOLID corners run 1, 2, 4, 3
          wf(c + 10, p.y);
          w(c + 20, "0.0");
        }
        return;
      }
    }
    note("hatch");
    for (const auto& pts : rings) {
      polyline(e, pts, true, o, nullptr);
    }
  } else if (type == "text") {
    if (e.get("bgm").truthy()) {
      note("textmask");  // R12 has no background mask (WL.8)
    }
    const cad::TextLayout lay = cad::text_layout(e);
    if (lay.lines.size() == 1) {
      text_out(e, str(e.get("s")), N("x"), N("y"), o, false);
      grow(N("x"), N("y"), o);
      return;
    }
    note("mtext");
    const double rot = num(or_(e.get("rot"), 0));
    const Mat m = m_mul({1, 0, 0, 1, N("x"), N("y")},
                        {base::js_cos(rot), base::js_sin(rot), -base::js_sin(rot), base::js_cos(rot), 0, 0});
    const Value ha = e.get("ha");
    double ax = lay.box_x;
    if (ha == Value("center")) {
      ax = lay.box_x + lay.box_w / 2;
    } else if (ha == Value("right")) {
      ax = lay.box_x + lay.box_w;
    }
    for (std::size_t k = 0; k < lay.lines.size(); ++k) {
      const Pt p = m_apply(m, {ax, lay.baselines.at(k)});
      text_out(e, lay.lines[k], p.x, p.y, o, true);
      grow(p.x, p.y, o);
    }
  } else if (type == "insert") {
    const Value bn = e.get("block");
    const Value* blk = doc_.blocks().get(str(bn));
    const Value attrs = e.get("attrs");
    Array atts;
    if (blk != nullptr && attrs.truthy() && !entries(attrs).empty()) {
      atts = cad::world_attribs(e, *blk);
    }
    head("INSERT", e, o);
    if (!atts.empty()) {
      w(66, 1);
    }
    const auto bit = block_names_.find(str(bn));
    w(2, bit != block_names_.end() && !bit->second.empty() ? bit->second : r12_name(str(bn)));
    wf(10, e.get("x"));
    wf(20, e.get("y"));
    wz(30, e.get("z"));
    if (const Value sx = e.get("sx"); !is_num(nn(sx, 1), 1)) {
      wf(41, sx);
    }
    if (const Value sy = e.get("sy"); !is_num(nn(sy, 1), 1)) {
      wf(42, sy);
    }
    if (const Value sz = e.get("sz"); finite(sz) && sz.truthy() && !is_num(sz, 1)) {
      wf(43, sz);
    }
    if (const Value r = e.get("rot"); r.truthy()) {
      wf(50, deg(num(r)));
    }
    grow(N("x"), N("y"), o);
    xd(e);
    if (!atts.empty()) {
      for (const Value& t : atts) {
        attribute("ATTRIB", t, o, nullptr);
      }
      w(0, "SEQEND");
      w(8, layer_name(or_(e.get("layer"), "0")));
      if (o.paper) {
        w(67, 1);
      }
    }
  } else if (type == "dim") {
    dimension(e, o);
  } else if (type == "viewport") {
    // a viewport outside the first layout's paper space: kept as its frame, with its view in CadCraft's xdata
    const double hw = N("w") / 2;
    const double hh = N("h") / 2;
    const double x = N("x");
    const double y = N("y");
    const std::vector<VtxIn> pts = {{x - hw, y - hh, 0, Value(), Value()},
                                    {x + hw, y - hh, 0, Value(), Value()},
                                    {x + hw, y + hh, 0, Value(), Value()},
                                    {x - hw, y + hh, 0, Value(), Value()}};
    polyline(e, pts, true, o, [&] {
      w(1001, "CADCRAFT");
      w(1000, "VIEWPORT");
      wf(1040, e.get("cx"));
      wf(1040, e.get("cy"));
      wf(1040, e.get("s"));
      if (const auto fz = frz_names(e); !fz.empty()) {  // the layers frozen in it (VP.1)
        w(1000, "FRZ");
        for (const std::string& n : fz) {
          w(1000, n);
        }
      }
    });
  } else if (type == "mleader") {
    // R12 has no leader object: the lines, arrowhead and text it draws, on its layer and colour
    note("mleader");
    for (Value part : cad::leader_geometry(e)) {
      part.set("layer", e.get("layer"));
      part.set("color", nn(e.get("color"), Value(nullptr)));
      entity(part, o);
    }
  } else {
    note("other");
  }
}

void R12::block(const std::string& name, const Value& base_pt, const std::vector<const Value*>& ents,
                const std::function<void()>& extra, const Value* attdefs, const std::optional<std::string>& xref_path) {
  w(0, "BLOCK");
  w(8, "0");
  w(2, name);
  const bool with_defs = attdefs != nullptr && !arr(*attdefs).empty();
  w(70, (name.starts_with('*') ? 1 : 0) | (with_defs ? 2 : 0) | (xref_path ? 4 : 0));
  wf(10, base_pt.get("x"));
  wf(20, base_pt.get("y"));
  w(30, "0.0");
  w(3, name);
  if (xref_path) {  // a linked drawing CadCraft did not load: still a link (xref.js)
    w(1, std::string_view(detail::encode_r12(*xref_path)).substr(0, 255));
  }
  if (extra) {
    extra();
  }
  for (const Value* e : ents) {
    entity(*e, {});
  }
  if (with_defs) {
    for (const Value& d : arr(*attdefs)) {
      Value t = base::make_object({{"type", "text"}});
      if (d.is_object() && !d.is_point()) {
        for (const base::Member* m : base::js_members(d.as_object())) {
          t.set(m->key, m->value);
        }
      }
      t.set("s", or_(d.get("def"), ""));
      attribute("ATTDEF", t, {}, &d);
    }
  }
  w(0, "ENDBLK");
  w(8, "0");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — writeDxf's body, in order
DxfSaveResult R12::run() {
  // constraints + parameters: CCREF xdata on entities, block CADCRAFT_PARAMETRIC (gcons.js)
  const auto pf = doc::gcons::for_file(doc_);
  doc_.layers().each([&](const std::string&, const Value& l) { layer_name(l.get("name")); });

  // Which blocks to write: every named block; anonymous ones unless only a dimension draws them
  // (each dimension writes its geometry as its own *D block below).
  std::vector<const Value*> everything;
  doc_.model()->each([&](const doc::Id&, const Value& e) { everything.push_back(&e); });
  for (const doc::Layout& l : doc_.layouts()) {
    l.entities->each([&](const doc::Id&, const Value& e) { everything.push_back(&e); });
  }
  doc_.blocks().each([&](const std::string&, const Value& b) {
    for (const Value& e : arr(b["entities"])) {
      everything.push_back(&e);
    }
  });
  std::unordered_set<std::string> insert_used;
  for (const Value* e : everything) {
    if (e->get("type") == Value("insert")) {
      insert_used.insert(str(e->get("block")));
    }
  }
  doc_.blocks().each([&](const std::string&, const Value& b) {
    const std::string n = str(b.get("name"));
    if (n.starts_with('*') && !insert_used.contains(n)) {
      return;
    }
    std::string out = n.starts_with('*') ? anon_name("*U") : r12_name(n);
    block_names_.insert_or_assign(n, std::move(out));
  });
  const auto collect_dims = [&](const Value& e) {
    if (e.get("type") != Value("dim")) {
      return;
    }
    const Value bn = e.get("block");
    const Value* own = bn.truthy() ? doc_.blocks().get(str(bn)) : nullptr;
    std::string name = anon_name("*D");
    dim_name_.insert_or_assign(&e, name);
    DimBlock d{.name = std::move(name), .own = nullptr, .made = {}};
    if (own != nullptr) {
      d.own = &arr((*own)["entities"]);
    } else {
      d.made = cad::dim_geometry(e);
    }
    dim_blocks_.push_back(std::move(d));
  };
  doc_.model()->each([&](const doc::Id&, const Value& e) { collect_dims(e); });
  for (const doc::Layout& l : doc_.layouts()) {
    l.entities->each([&](const doc::Id&, const Value& e) { collect_dims(e); });
  }
  doc_.blocks().each([&](const std::string&, const Value& b) {
    if (block_names_.contains(str(b.get("name")))) {
      for (const Value& e : arr(b["entities"])) {
        collect_dims(e);
      }
    }
  });
  std::vector<std::pair<const doc::Layout*, std::string>> extra_layouts;
  for (std::size_t k = 1; k < doc_.layouts().size(); ++k) {
    extra_layouts.emplace_back(&doc_.layouts()[k], "CCLAYOUT_" + std::to_string(k + 1));
  }
  for (const auto& [l, name] : extra_layouts) {
    used_.insert(name);
  }
  if (pf) {
    used_.insert(std::string(doc::gcons::kParamBlock));
  }
  const Value units_v = doc_.meta.get("units");
  // the 3D model (model3d.js): 3DFACEs on layer CC-3D + its exact copy in block CADCRAFT_3D
  const auto m3 = m3d::for_file3d(doc_.model3d, units_v.truthy() ? str(units_v) : std::string());
  const std::string m3_layer(m3d::kM3dLayer);
  if (m3) {
    used_.insert(std::string(m3d::kM3dBlock));
    if (!doc_.layers().has(m3_layer)) {
      layer_name(m3_layer);
    }
  }
  doc_.layers().each([&](const std::string&, const Value& l) {
    if (l.get("plot") == Value(false)) {
      no_plot_.push_back(layer_name(l.get("name")));
    }
  });
  Value st = cad::dim_defaults();
  if (const Value cd = doc_.current.get("dim"); cd.is_object() && !cd.is_point()) {
    for (const base::Member* m : base::js_members(cd.as_object())) {
      st.set(m->key, m->value);
    }
  }
  const doc::Layout* first = doc_.layouts().empty() ? nullptr : doc_.layouts().data();

  // HEADER
  w(0, "SECTION");
  w(2, "HEADER");
  w(9, "$ACADVER");
  w(1, "AC1009");
  w(9, "$DWGCODEPAGE");
  w(3, "ANSI_1252");
  w(9, "$INSBASE");
  w(10, "0.0");
  w(20, "0.0");
  w(30, "0.0");
  w(9, "$TEXTSIZE");
  wf(40, doc_.current.get("textHeight"));
  if (const Value lts = doc_.meta.get("ltscale"); num(lts) > 0 && !is_num(lts, 1)) {
    w(9, "$LTSCALE");
    wf(40, lts);
  }
  const int uc = cad::unit_code(units_v.is_string() ? units_v.as_string() : std::string());
  if (uc != 0) {
    w(9, "$INSUNITS");
    w(70, uc);
  }
  if (doc_.ucs.truthy()) {  // the current UCS (DR.8)
    const double a = num(or_(doc_.ucs.get("ang"), 0));
    w(9, "$UCSORG");
    wf(10, doc_.ucs.get("x"));
    wf(20, doc_.ucs.get("y"));
    w(30, "0.0");
    w(9, "$UCSXDIR");
    wf(10, base::js_cos(a));
    wf(20, base::js_sin(a));
    w(30, "0.0");
    w(9, "$UCSYDIR");
    wf(10, -base::js_sin(a));
    wf(20, base::js_cos(a));
    w(30, "0.0");
  }
  if (const Value lu = doc_.meta.get("lunits"); num(lu) >= 1 && num(lu) <= 5) {
    w(9, "$LUNITS");
    w(70, i32(lu));
    if (const Value lp = doc_.meta.get("luprec"); finite(lp)) {
      w(9, "$LUPREC");
      w(70, i32(lp));
    }
  }
  for (const auto& [name, key] : {std::pair<std::string_view, std::string_view>{"$DIMSCALE", "scale"},
                                  {"$DIMASZ", "asz"},
                                  {"$DIMEXO", "exo"},
                                  {"$DIMDLI", "dli"},
                                  {"$DIMEXE", "exe"},
                                  {"$DIMTXT", "txt"},
                                  {"$DIMGAP", "gap"}}) {
    w(9, name);
    wf(40, st.get(key));
  }
  w(9, "$DIMTAD");
  w(70, i32(st.get("tad")));
  w(9, "$DIMZIN");
  w(70, i32(st.get("zin")));
  w(9, "$TILEMODE");
  w(70, 1);
  if (first != nullptr) {
    const Value& p = first->paper;
    w(9, "$PLIMMIN");
    wf(10, p.get("x0"));
    wf(20, p.get("y0"));
    w(9, "$PLIMMAX");
    wf(10, num(p.get("x0")) + num(p.get("w")));
    wf(20, num(p.get("y0")) + num(p.get("h")));
  }
  w(0, "ENDSEC");

  // TABLES
  w(0, "SECTION");
  w(2, "TABLES");
  // the split screen (doc.vports, dwg-vports.js): one *ACTIVE per pane, the current one first; one pane writes no table
  if (doc_.vports.truthy() || doc_.view_twist != 0) {  // (one pane with a turned view too, DR.8)
    cad::Bounds b;
    doc_.model()->each([&](const doc::Id&, const Value& e) {
      const cad::Bounds eb = cad::entity_bounds(e, blocks_);
      if (eb.valid()) {
        b.min_x = std::min(b.min_x, eb.min_x);
        b.min_y = std::min(b.min_y, eb.min_y);
        b.max_x = std::max(b.max_x, eb.max_x);
        b.max_y = std::max(b.max_y, eb.max_y);
      }
    });
    Value fb = base::make_object({{"cx", 0}, {"cy", 0}, {"h", 10}});
    if (b.valid()) {
      const double h = std::max({b.max_y - b.min_y, (b.max_x - b.min_x) / 1.6, 1e-6}) * 1.05;
      fb = base::make_object({{"cx", (b.min_x + b.max_x) / 2}, {"cy", (b.min_y + b.max_y) / 2}, {"h", h}});
    }
    const Array recs = cad::vport_records(doc_.vports, fb, doc_.view_twist);
    w(0, "TABLE");
    w(2, "VPORT");
    w(70, static_cast<double>(recs.size()));
    for (const Value& r : recs) {
      w(0, "VPORT");
      w(2, "*ACTIVE");
      w(70, 0);
      wf(10, r.get("x0"));
      wf(20, r.get("y0"));
      wf(11, r.get("x1"));
      wf(21, r.get("y1"));
      wf(12, r.get("cx"));
      wf(22, r.get("cy"));
      for (const auto& [c, v] : {std::pair<int, std::string_view>{13, "0.0"},
                                 {23, "0.0"},
                                 {14, "1.0"},
                                 {24, "1.0"},
                                 {15, "0.0"},
                                 {25, "0.0"},
                                 {16, "0.0"},
                                 {26, "0.0"},
                                 {36, "1.0"},
                                 {17, "0.0"},
                                 {27, "0.0"},
                                 {37, "0.0"}}) {
        w(c, v);
      }
      wf(40, r.get("h"));
      wf(41, r.get("aspect"));
      w(42, "50.0");
      w(43, "0.0");
      w(44, "0.0");
      w(50, "0.0");
      wf(51, deg(num(or_(r.get("tw"), 0))));
      for (const auto& [c, v] :
           {std::pair<int, int>{71, 0}, {72, 100}, {73, 1}, {74, 3}, {75, 0}, {76, 0}, {77, 0}, {78, 0}}) {
        w(c, v);
      }
      if (const Value sh = r.get("sheet"); sh.truthy()) {
        w(1001, "CADCRAFT");
        w(1000, detail::encode_r12("PANESHEET:" + detail::utf16_prefix(str(sh), 245)));
      }
    }
    w(0, "ENDTAB");
  }
  // linetypes (0.4 C8): every one a layer or an object uses, with its dashes in drawing units
  std::unordered_map<std::string, std::string> layer_lt;
  doc_.layers().each([&](const std::string&, const Value& l) {
    layer_lt.insert_or_assign(str(l.get("name")), lt_name(l.get("linetype")).value_or("CONTINUOUS"));
  });
  for (const Value* e : everything) {
    (void)lt_name(e->get("lt"));
    if (const Value lw = e->get("lw"); !lw.is_undefined() && !is_num(lw, -3)) {
      note("lw");
    }
    if (const Value lts = e->get("lts"); num(lts) > 0 && !is_num(lts, 1)) {
      note("lts");
    }
  }
  doc_.layers().each([&](const std::string&, const Value& l) {
    if (num(l.get("lineweight")) >= 0) {
      note("lw");
    }
  });
  w(0, "TABLE");
  w(2, "LTYPE");
  w(70, static_cast<double>(1 + lt_used_.size()));
  w(0, "LTYPE");
  w(2, "CONTINUOUS");
  w(70, 0);
  w(3, words_.t("dxf.solid_line"));
  w(72, 65);
  w(73, 0);
  w(40, "0.0");
  for (const auto& [n, pat] : lt_used_) {
    w(0, "LTYPE");
    w(2, n);
    w(70, 0);
    const Value* own = doc_.linetypes.get(n);
    w(3, own != nullptr ? or_(own->get("description"), n) : Value(n));
    w(72, 65);
    w(73, static_cast<double>(arr(pat).size()));
    double sum = 0;
    for (const Value& v : arr(pat)) {
      sum += std::abs(num(v));
    }
    wf(40, sum);
    for (const Value& v : arr(pat)) {
      wf(49, v);
    }
  }
  w(0, "ENDTAB");
  const bool add3d_layer = m3 && !doc_.layers().has(m3_layer);
  w(0, "TABLE");
  w(2, "LAYER");
  w(70, static_cast<double>(doc_.layers().size() + (add3d_layer ? 1 : 0)));
  doc_.layers().each([&](const std::string&, const Value& l) {
    const Value col = l.get("color");
    Value c;
    if (col.is_string()) {
      note("truecolor");
      c = cad::nearest_aci(col.as_string());
    } else {
      c = or_(col, 7);
    }
    if (!l.get("on").truthy()) {
      c = -std::abs(num(c));
    }
    w(0, "LAYER");
    w(2, layer_name(l.get("name")));
    w(70, (l.get("frozen").truthy() ? 1 : 0) | (l.get("newVpFrozen").truthy() ? 2 : 0) |
              (l.get("locked").truthy() ? 4 : 0));
    w(62, c);
    const auto it = layer_lt.find(str(l.get("name")));
    w(6, it != layer_lt.end() ? it->second : std::string("undefined"));
    if (l.get("plot") == Value(false)) {
      note("noplot");
    }
  });
  if (add3d_layer) {
    w(0, "LAYER");
    w(2, layer_name(m3_layer));
    w(70, 0);
    w(62, 8);
    w(6, "CONTINUOUS");
  }
  w(0, "ENDTAB");
  // text styles (P.7): STANDARD, every one the drawing keeps and every name a text uses
  std::vector<std::pair<std::string, Value>> text_style_out;
  const auto ts_get = [&](const std::string& k) -> Value* {
    for (auto& [kk, v] : text_style_out) {
      if (kk == k) {
        return &v;
      }
    }
    return nullptr;
  };
  doc_.text_styles.each([&](const std::string& n, const Value& v) {
    if (!detail::is_standard(Value(n))) {
      const std::string k = r12_name(n);
      if (Value* have = ts_get(k)) {
        *have = v;
      } else {
        text_style_out.emplace_back(k, v);
      }
    }
  });
  for (const Value* e : everything) {
    if (e->get("type") == Value("text")) {
      const Value s = e->get("style");
      if (s.truthy() && !detail::is_standard(s) && ts_get(r12_name(str(s))) == nullptr) {
        text_style_out.emplace_back(r12_name(str(s)),
                                    base::make_object({{"font", "txt"}, {"h", 0}, {"wf", 1}, {"obl", 0}}));
      }
    }
  }
  const Value text_h = doc_.current.get("textHeight");
  w(0, "TABLE");
  w(2, "STYLE");
  w(70, static_cast<double>(1 + text_style_out.size()));
  w(0, "STYLE");
  w(2, "STANDARD");
  w(70, 0);
  w(40, "0.0");
  w(41, "1.0");
  w(50, "0.0");
  w(71, 0);
  wf(42, text_h);
  w(3, "txt");
  w(4, "");
  for (const auto& [n, v] : text_style_out) {
    w(0, "STYLE");
    w(2, n);
    w(70, 0);
    wf(40, or_(v.get("h"), 0));
    wf(41, or_(v.get("wf"), 1));
    wf(50, deg(num(or_(v.get("obl"), 0))));
    w(71, 0);
    wf(42, text_h);
    w(3, encode(or_(v.get("font"), "txt")));
    w(4, "");
  }
  w(0, "ENDTAB");
  // named UCSs (DR.8)
  std::vector<const Value*> ucs_named;
  for (const Value& u : arr(doc_.ucs_named)) {
    if (u.truthy() && u.get("name").truthy()) {
      ucs_named.push_back(&u);
    }
  }
  if (!ucs_named.empty()) {
    w(0, "TABLE");
    w(2, "UCS");
    w(70, static_cast<double>(ucs_named.size()));
    for (const Value* u : ucs_named) {
      const double a = num(or_(u->get("ang"), 0));
      w(0, "UCS");
      w(2, layer_name(u->get("name")));
      w(70, 0);
      wf(10, u->get("x"));
      wf(20, u->get("y"));
      w(30, "0.0");
      wf(11, base::js_cos(a));
      wf(21, base::js_sin(a));
      w(31, "0.0");
      wf(12, -base::js_sin(a));
      wf(22, base::js_cos(a));
      w(32, "0.0");
    }
    w(0, "ENDTAB");
  }
  // named views (a sheet's one as a paper-space view: R12 does not say which sheet)
  std::vector<const Value*> views;
  for (const Value& v : arr(doc_.views)) {
    if (v.truthy() && v.get("name").truthy() && num(v.get("w")) > 0 && num(v.get("h")) > 0) {
      views.push_back(&v);
    }
  }
  if (!views.empty()) {
    w(0, "TABLE");
    w(2, "VIEW");
    w(70, static_cast<double>(views.size()));
    std::unordered_set<std::string> vn;
    for (const Value* v : views) {
      std::string n = r12_name(str(v->get("name")));
      int k = 2;
      while (vn.contains(n)) {
        n = r12_name(str(v->get("name"))).substr(0, 28) + "_" + std::to_string(k++);
      }
      vn.insert(n);
      if (v->get("layout").truthy()) {
        note("paperView");
      }
      w(0, "VIEW");
      w(2, n);
      w(70, v->get("layout").truthy() ? 1 : 0);
      wf(40, v->get("h"));
      wf(10, v->get("cx"));
      wf(20, v->get("cy"));
      wf(41, v->get("w"));
      for (const auto& [c, s] : {std::pair<int, std::string_view>{11, "0.0"},
                                 {21, "0.0"},
                                 {31, "1.0"},
                                 {12, "0.0"},
                                 {22, "0.0"},
                                 {32, "0.0"},
                                 {42, "50.0"},
                                 {43, "0.0"},
                                 {44, "0.0"},
                                 {50, "0.0"}}) {
        w(c, s);
      }
      w(71, 0);
    }
    w(0, "ENDTAB");
  }
  std::vector<std::string> app_names = {"ACAD", "CADCRAFT"};
  for (const Value* e : everything) {
    const Value x = cad::props_xdata(*e, true);
    for (const base::Member* m : entries(x)) {
      std::string a = base::js_to_upper(m->key);
      if (std::ranges::find(app_names, a) == app_names.end()) {
        app_names.push_back(std::move(a));
      }
    }
  }
  w(0, "TABLE");
  w(2, "APPID");
  w(70, static_cast<double>(app_names.size()));
  for (const std::string& a : app_names) {
    w(0, "APPID");
    w(2, a);
    w(70, 0);
  }
  w(0, "ENDTAB");
  // dimension styles (P.6): STANDARD (the drawing's current settings) and every named one, with R12's variables
  std::vector<std::pair<std::string, Value>> dim_style_out = {{"STANDARD", st}};
  const auto ds_has = [&](const std::string& k) {
    return std::ranges::any_of(dim_style_out, [&](const auto& x) { return x.first == k; });
  };
  const auto ds_set = [&](const std::string& k, const Value& v) {
    for (auto& [kk, vv] : dim_style_out) {
      if (kk == k) {
        vv = v;
        return;
      }
    }
    dim_style_out.emplace_back(k, v);
  };
  doc_.dim_styles.each([&](const std::string& n, const Value& s2) {
    if (!detail::is_standard(Value(n))) {
      ds_set(r12_name(n), s2);
    }
  });
  for (const Value* e : everything) {
    if (e->get("type") == Value("dim")) {
      const Value d = e->get("dimstyle");
      if (d.truthy() && !detail::is_standard(d) && !ds_has(r12_name(str(d)))) {
        ds_set(r12_name(str(d)), st);
      }
    }
  }
  w(0, "TABLE");
  w(2, "DIMSTYLE");
  w(70, static_cast<double>(dim_style_out.size()));
  static constexpr std::array<int, 14> kReal = {40, 41, 42, 43, 44, 45, 47, 48, 140, 142, 143, 144, 146, 147};
  for (const auto& [n, s2] : dim_style_out) {
    w(0, "DIMSTYLE");
    w(2, n);
    w(70, 0);
    for (const Value& pv : cad::dimstyle_pairs(s2, true)) {
      const Array& p = pv.as_array();
      const int code = i32(p.at(0));
      const Value& v = p.at(1);
      if (v.is_string()) {
        w(code, detail::encode_r12(v.as_string()));
      } else if (v.is_number() && std::isfinite(v.as_number()) && std::trunc(v.as_number()) == v.as_number() &&
                 std::ranges::find(kReal, code) == kReal.end()) {
        w(code, v);
      } else {
        wf(code, num(v));
      }
    }
    wf(141, num(or_(nn(s2.get("asz"), st.get("asz")), 2.5)) / 2);
    const Array cc =
        cad::cc_dim_items(s2, true);  // what R12 has no variable for (decimals, arrowheads, …), for CadCraft
    if (!cc.empty()) {
      w(1001, "CADCRAFT");
      for (const Value& itv : cc) {
        w(1000, encode(itv.as_array().at(1)));
      }
    }
  }
  w(0, "ENDTAB");
  w(0, "ENDSEC");

  // BLOCKS
  w(0, "SECTION");
  w(2, "BLOCKS");
  doc_.blocks().each([&](const std::string&, const Value& b) {
    const std::string n = str(b.get("name"));
    const auto it = block_names_.find(n);
    if (it == block_names_.end()) {
      return;
    }
    const Array& ents = arr(b["entities"]);
    std::optional<std::string> link;
    if (cad::is_xref_root(b) && ents.empty()) {
      link = str(or_(b.get("xref").get("path"), n));
      note("xrefLink");
    } else if (cad::is_xref_root(b)) {
      note("xrefBound");
    }
    const Value* defs = b.find("attdefs");
    block(it->second, b.get("base"), ptrs(ents), nullptr, defs, link);
  });
  for (const DimBlock& d : dim_blocks_) {
    block(d.name, base::pt2(0, 0), ptrs(d.ents()), nullptr);
  }
  if (pf) {
    block(std::string(doc::gcons::kParamBlock), base::pt2(0, 0), {}, [&] {
      for (const std::string& t : pf->points) {
        w(0, "POINT");
        w(8, "0");
        w(10, "0.0");
        w(20, "0.0");
        w(30, "0.0");
        w(1001, "CADCRAFT");
        w(1000, t);
      }
    });
  }
  if (m3) {
    block(std::string(m3d::kM3dBlock), base::pt2(0, 0), {}, [&] {
      for (const std::string& t : m3->points) {
        w(0, "POINT");
        w(8, "0");
        w(10, "0.0");
        w(20, "0.0");
        w(30, "0.0");
        w(1001, "CADCRAFT");
        w(1000, t);
      }
    });
  }
  for (const auto& [l, name] : extra_layouts) {
    std::vector<const Value*> ents;
    l->entities->each([&](const doc::Id&, const Value& e) { ents.push_back(&e); });
    const doc::Layout& lr = *l;
    block(name, base::pt2(0, 0), ents, [&] {
      w(0, "POINT");
      w(8, "0");
      wf(10, lr.paper.get("x0"));
      wf(20, lr.paper.get("y0"));
      w(30, "0.0");
      layout_xdata(lr);
    });
  }
  w(0, "ENDSEC");

  // ENTITIES: the model, then the first layout in paper space
  w(0, "SECTION");
  w(2, "ENTITIES");
  doc_.model()->each([&](const doc::Id& id, const Value& e) {
    ref_.clear();  // entity(e, { top: true, ref: pf?.refs.get(e.id) })
    if (pf) {
      for (const auto& [rid, key] : pf->refs) {
        if (rid == static_cast<double>(id)) {
          ref_ = key;
        }
      }
    }
    entity(e, {.top = true});
  });
  ref_.clear();
  if (m3) {
    const std::string m3l = layer_name(m3_layer);
    for (const auto& fc : m3->faces) {
      w(0, "3DFACE");
      w(8, m3l);
      for (int k = 0; k < 4; ++k) {
        const auto& p = fc.pts.at(static_cast<std::size_t>(k));
        wf(10 + k, p[0]);
        wf(20 + k, p[1]);
        wf(30 + k, p[2]);
      }
      if (fc.invis != 0) {
        w(70, fc.invis);
      }
    }
  }
  if (first != nullptr) {
    const Value& p = first->paper;
    const double x0 = num(p.get("x0"));
    const double y0 = num(p.get("y0"));
    const double pw = num(p.get("w"));
    const double ph = num(p.get("h"));
    const Value vp = base::make_object({{"layer", "0"},
                                        {"color", nullptr},
                                        {"x", x0 + pw / 2},
                                        {"y", y0 + ph / 2},
                                        {"w", pw},
                                        {"h", ph},
                                        {"cx", x0 + pw / 2},
                                        {"cy", y0 + ph / 2},
                                        {"s", 1}});
    viewport(vp, 1, [&] { layout_xdata(*first); });
    int id = 2;
    first->entities->each([&](const doc::Id&, const Value& e) {
      if (e.get("type") == Value("viewport")) {
        viewport(e, id++, nullptr);
      } else {
        entity(e, {.paper = true});
      }
    });
  }
  w(0, "ENDSEC");
  w(0, "EOF");

  Array notes;
  const auto tn = [&](std::string_view key, std::string_view count_key) {
    if (const double n = count(count_key); n != 0) {
      notes.emplace_back(words_.tn(key, n));
    }
  };
  tn("dxf.save.renamed", "renamed");
  tn("dxf.save.infinite", "infinite");
  {
    double n = 0;  // WL.16: R12 has no clips
    const auto look = [&](const doc::Id&, const Value& e) {
      if (e.get("type") == Value("insert") && arr(at(e, "clip"), "pts").size() >= 3) {
        ++n;
      }
    };
    doc_.model()->each(look);
    for (const doc::Layout& l : doc_.layouts()) {
      l.entities->each(look);
    }
    if (n != 0) {
      notes.emplace_back(words_.tn("dxf.save.xclip", n));
    }
  }
  if (const double n = detail::viewport_overrides(doc_); n != 0) {
    notes.emplace_back(words_.tn("dxf.save.vpovr", n));  // VP.3: DWG only
  }
  if (const double n = detail::picture_refs(doc_); n != 0) {
    notes.emplace_back(words_.tn("dxf.save.pictures", n));  // R12 has no pictures: their frames (0.5 WL.14)
  }
  if (count("truecolor") != 0) {
    notes.emplace_back(words_.t("dxf.true_colours_were_saved"));
  }
  tn("dxf.save.hatch", "hatch");
  tn("dxf.save.ellipse", "ellipse");
  if (count("mtext") != 0) {
    notes.emplace_back(words_.t("dxf.multi_line_text_was"));
  }
  tn("dxf.save.textmask", "textmask");
  tn("dxf.save.longattr", "longattr");
  if (const double n = count("noplot"); n != 0) {
    std::string layers;
    for (std::size_t i = 0; i < no_plot_.size(); ++i) {
      layers += (i != 0 ? ", " : "") + no_plot_[i];
    }
    notes.emplace_back(
        words_.tn(first != nullptr ? "dxf.save.noplot" : "dxf.save.noplot_nolayout", n, {{"layers", layers}}));
  }
  if (!extra_layouts.empty()) {
    std::string lays;
    std::string blks;
    for (std::size_t i = 0; i < extra_layouts.size(); ++i) {
      lays += (i != 0 ? ", \"" : "\"") + extra_layouts[i].first->name + "\"";
      blks += (i != 0 ? ", " : "") + extra_layouts[i].second;
    }
    notes.emplace_back(words_.tn("dxf.save.layouts", static_cast<double>(extra_layouts.size()),
                                 {{"first", first->name}, {"layouts", lays}, {"blocks", blks}}));
  }
  tn("dxf.save.mleader", "mleader");
  tn("dwg.save.xref.bound", "xrefBound");
  tn("dwg.save.xref.link", "xrefLink");
  if (count("lw") != 0) {
    notes.emplace_back(words_.t("dxf.save.lw"));
  }
  tn("dxf.save.lts", "lts");
  tn("dxf.save.paperview", "paperView");
  tn("dwg.save.other", "other");
  if (count("tr") != 0) {
    notes.emplace_back(words_.t("dxf.save.tr"));
  }
  if (count("material") != 0) {
    notes.emplace_back(words_.t("dwg.save.material"));
  }
  if (m3) {
    counts_.set("face3d", static_cast<double>(m3->faces.size()));
    notes.emplace_back(words_.tn("dxf.save.model3d", static_cast<double>(m3->faces.size()),
                                 {{"layer", m3_layer}, {"block", std::string(m3d::kM3dBlock)}}));
  }
  DxfSaveResult res;
  res.text = L_.take();
  res.report = base::make_object(
      {{"notes", std::move(notes)},
       {"counts", counts_},
       {"extents",
        base::make_object({{"minX", ext_.min_x}, {"minY", ext_.min_y}, {"maxX", ext_.max_x}, {"maxY", ext_.max_y}})}});
  return res;
}

}  // namespace

DxfSaveResult write_dxf(const doc::Doc& doc, const i18n::Catalog& words) {
  R12 x(doc, words);
  return x.run();
}

}  // namespace cc::save
