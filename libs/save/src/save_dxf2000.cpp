// SPDX-License-Identifier: Apache-2.0
// dxf2000.js (CadCraft 0.5) in C++: the writer model DWG saving builds (save_dwg, model_only + explode_leaders) →
// ASCII DXF R2000, with every object's handle and owner. One class whose members are the JS function's closure
// variables, called in the JS's order (the order fixes the handles).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/value.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/save/dwg.hpp"
#include "cc/save/dxf.hpp"
#include "dxf_common.hpp"

namespace cc::save {

using base::Array;
using base::Value;
using detail::f2k;

namespace {

constexpr double kPi = std::numbers::pi;

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
/// Math.max(a, b, c): NaN when any is
double js_max3(double a, double b, double c) {
  if (std::isnan(a) || std::isnan(b) || std::isnan(c)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::max({a, b, c});
}
double deg(double r) {
  return (r * 180) / kPi;
}
/// enc(s): String(s ?? '')
std::string enc(const Value& v) {
  return detail::enc2000(nullish(v) ? std::string() : str(v));
}
std::string enc(const std::string& s) {
  return detail::enc2000(s);
}
/// s.replace(/\|/g, '$0$')
std::string bars(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '|') {
      out += "$0$";
    } else {
      out += c;
    }
  }
  return out;
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
/// Object.entries(o) of a plain object (JS property order).
std::vector<const base::Member*> entries(const Value& o) {
  if (!o.is_object() || o.is_point()) {
    return {};
  }
  return base::js_members(o.as_object());
}

// A JS Map with string keys: set keeps an existing key's place and replaces its value.
template <class T>
class StrMap {
 public:
  void set(const std::string& k, T v) {
    for (auto& [kk, vv] : items_) {
      if (kk == k) {
        vv = std::move(v);
        return;
      }
    }
    items_.emplace_back(k, std::move(v));
  }
  [[nodiscard]] T* get(const std::string& k) {
    for (auto& [kk, vv] : items_) {
      if (kk == k) {
        return &vv;
      }
    }
    return nullptr;
  }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] const std::vector<std::pair<std::string, T>>& items() const noexcept { return items_; }

 private:
  std::vector<std::pair<std::string, T>> items_;
};

struct Rec {
  std::string br, block, endblk, layout;
};
struct LayoutOut {
  std::string name;
  int tab = 0;
  const Value* space = nullptr;
  Value src;
  std::string h;
};
struct Clip {
  std::string own, xd, d2, sf;
  Value pts;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) — a writer lives for one write_dxf2000 call
class Dxf2000 {
 public:
  Dxf2000(const doc::Doc& d, const i18n::Catalog& w) : doc_(d), words_(w) {}
  DxfSaveResult run();

 private:
  const doc::Doc& doc_;
  const i18n::Catalog& words_;
  detail::DxfLines L_;
  std::uint64_t seed_ = 1;
  Value counts_ = Value::object();
  std::vector<std::string> lt_names_;                         // upper case
  std::vector<std::pair<std::string, std::string>> layer_h_;  // upper-case layer name → handle (a Map)
  std::vector<Clip> clips_;

  std::string h() { return base::hex_upper(seed_++); }
  void w(int code, const char* v) { L_.w(code, std::string_view(v)); }
  template <class V>
  void w(int code, const V& v) {
    L_.w(code, v);
  }
  void wf(int code, const Value& v) { L_.w(code, f2k(v)); }
  void wf(int code, double v) { L_.w(code, f2k(v)); }

  static std::string layer_name(const Value& n) { return bars(n.truthy() ? str(n) : std::string("0")); }
  static std::string block_name(const Value& n) { return bars(str(n)); }
  /// ltName(n): ByLayer → none; ByBlock, Continuous by their names; a linetype the file has; anything else none.
  [[nodiscard]] std::optional<std::string> lt_name(const Value& n) const {
    if (!n.truthy()) {
      return std::nullopt;
    }
    const std::string low = base::js_to_lower(str(n));
    if (low == "bylayer") {
      return std::nullopt;
    }
    if (low == "byblock") {
      return "ByBlock";
    }
    if (low == "continuous") {
      return "Continuous";
    }
    const std::string up = base::js_to_upper(str(n));
    for (const std::string& x : lt_names_) {
      if (x == up) {
        return str(n);
      }
    }
    return std::nullopt;
  }
  void P(int c, const Value& p) {
    wf(c, or_(p.get("x"), 0));
    wf(c + 10, or_(p.get("y"), 0));
    wf(c + 20, or_(p.get("z"), 0));
  }
  void table(std::string_view name, const std::string& th, std::size_t n, const std::function<void()>& body) {
    w(0, "TABLE");
    w(2, name);
    w(5, th);
    w(330, "0");
    w(100, "AcDbSymbolTable");
    w(70, static_cast<double>(n));
    body();
    w(0, "ENDTAB");
  }
  void entry(std::string_view type, const std::string& eh, const std::string& th, std::string_view sub,
             const std::string& name, int flags = 0) {
    w(0, type);
    w(type == "DIMSTYLE" ? 105 : 5, eh);
    w(330, th);
    w(100, "AcDbSymbolTableRecord");
    w(100, sub);
    w(2, name);
    w(70, flags);
  }
  void head(std::string_view type, const Value& e, const std::string& owner, bool paper,
            const Value* layer_over = nullptr);
  void th(const Value& e) {
    if (const Value t = e.get("thickness"); t.truthy()) {
      wf(39, t);
    }
  }
  void xdata(const Value& e);
  void text_body(const Value& e);
  void entity(const Value& e, const std::string& owner, bool paper);
  void dict(const std::string& dh, const std::string& owner,
            const std::vector<std::pair<std::string, std::string>>& items) {
    w(0, "DICTIONARY");
    w(5, dh);
    w(330, owner);
    w(100, "AcDbDictionary");
    w(281, 1);
    for (const auto& [k, v] : items) {
      w(3, k);
      w(350, v);
    }
  }
  void note_skipped() {
    const Value have = counts_.get("skipped");
    counts_.set("skipped", (have.truthy() ? num(have) : 0) + 1);
  }
  [[nodiscard]] double count(std::string_view k) const {
    const Value v = counts_.get(k);
    return v.truthy() ? num(v) : 0;
  }
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

void Dxf2000::head(std::string_view type, const Value& e, const std::string& owner, bool paper,
                   const Value* layer_over) {
  const std::string own = h();
  w(0, type);
  w(5, own);
  if (const Value& xc = at(e, "xclip"); xc.truthy()) {
    if (const Value pts = xc.get("pts"); pts.is_array() && pts.as_array().size() >= 3) {
      Clip c{.own = own, .xd = h(), .d2 = h(), .sf = h(), .pts = pts};
      w(102, "{ACAD_XDICTIONARY");
      w(360, c.xd);
      w(102, "}");
      clips_.push_back(std::move(c));
    }
  }
  w(330, owner);
  w(100, "AcDbEntity");
  if (paper) {
    w(67, 1);
  }
  w(8, enc(layer_name(layer_over != nullptr ? *layer_over : or_(e.get("layer"), "0"))));
  if (const auto lt = lt_name(e.get("linetype"))) {
    w(6, *lt);
  }
  const Value& c = at(e, "color");
  if (c.truthy()) {
    const Value idx = c.get("index");
    if (!idx.is_undefined() && !(idx == Value(256))) {
      w(62, idx);
    }
    if (const Value rgb = c.get("rgb"); !rgb.is_undefined()) {
      w(420, i32(rgb) & 0xffffff);
    }
  }
  if (const Value lw = e.get("lineweight"); finite(lw) && lw.as_number() != -1) {
    w(370, lw);
  }
  if (const Value lts = e.get("ltscale"); num(lts) > 0 && !(lts == Value(1))) {
    wf(48, lts);
  }
  if (const Value tr = e.get("transparency"); finite(tr)) {
    w(440, i32(tr));
  }
}

void Dxf2000::xdata(const Value& e) {
  const Value& xd = at(e, "xdata");
  for (const base::Member* m : entries(xd)) {
    w(1001, base::js_to_upper(m->key));
    for (const Value& it : arr(m->value)) {
      const Array& p = arr(it);
      if (p.size() < 2) {
        continue;
      }
      const int code = i32(p[0]);
      if (code == 1000) {
        w(code, enc(p[1]));
      } else if (code == 1040) {
        wf(code, num(p[1]));
      } else if (code == 1070) {
        w(code, i32(p[1]));
      } else {
        w(code, p[1]);
      }
    }
  }
}

void Dxf2000::text_body(const Value& e) {
  th(e);
  P(10, e.get("insert"));
  wf(40, e.get("height"));
  w(1, enc(e.get("value")));
  if (const Value r = e.get("rotation"); r.truthy()) {
    wf(50, deg(num(r)));
  }
  if (const Value wfv = e.get("widthFactor"); wfv.truthy() && !(wfv == Value(1))) {
    wf(41, wfv);
  }
  if (const Value o = e.get("oblique"); o.truthy()) {
    wf(51, deg(num(o)));
  }
  const Value style = e.get("style");
  w(7, style.truthy() ? enc(style) : std::string("Standard"));
  const Value ha = e.get("hAlign");
  if (ha.truthy()) {
    w(72, ha);
  }
  if (const Value al = e.get("align"); al.truthy() && (ha.truthy() || e.get("vAlign").truthy())) {
    P(11, al);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — the JS's switch
void Dxf2000::entity(const Value& e, const std::string& owner, bool paper) {
  const std::string type = str(e.get("type"));
  if (type == "LINE") {
    head("LINE", e, owner, paper);
    w(100, "AcDbLine");
    th(e);
    P(10, e.get("start"));
    P(11, e.get("end"));
  } else if (type == "CIRCLE") {
    head("CIRCLE", e, owner, paper);
    w(100, "AcDbCircle");
    th(e);
    P(10, e.get("center"));
    wf(40, e.get("radius"));
  } else if (type == "ARC") {
    head("ARC", e, owner, paper);
    w(100, "AcDbCircle");
    th(e);
    P(10, e.get("center"));
    wf(40, e.get("radius"));
    w(100, "AcDbArc");
    wf(50, deg(num(e.get("startAngle"))));
    wf(51, deg(num(e.get("endAngle"))));
  } else if (type == "POINT") {
    head("POINT", e, owner, paper);
    w(100, "AcDbPoint");
    P(10, e.get("position"));
    th(e);
  } else if (type == "XLINE" || type == "RAY") {
    head(type, e, owner, paper);
    w(100, type == "XLINE" ? "AcDbXline" : "AcDbRay");
    P(10, e.get("start"));
    P(11, e.get("direction"));
  } else if (type == "LWPOLYLINE") {
    const Value& vs = at(e, "vertices");
    bool each = false;
    for (const Value& v : arr(vs)) {
      each = each || v.get("startWidth").truthy() || v.get("endWidth").truthy();
    }
    head("LWPOLYLINE", e, owner, paper);
    w(100, "AcDbPolyline");
    w(90, static_cast<double>(arr(vs).size()));
    w(70, e.get("closed").truthy() ? 1 : 0);
    wf(43, each ? Value(0) : or_(e.get("constantWidth"), 0));
    if (const Value el = e.get("elevation"); el.truthy()) {
      wf(38, el);
    }
    th(e);
    for (const Value& v : arr(vs)) {
      wf(10, v.get("x"));
      wf(20, v.get("y"));
      if (each) {
        wf(40, or_(v.get("startWidth"), 0));
        wf(41, or_(v.get("endWidth"), 0));
      }
      if (const Value b = v.get("bulge"); b.truthy()) {
        wf(42, b);
      }
    }
  } else if (type == "ELLIPSE") {
    // DXF wants the ratio ≤ 1: a taller one turns its major axis a quarter turn (as cc-dwg's writer does)
    Value maj = e.get("majorAxis");
    Value ratio = e.get("ratio");
    double s0 = num(nn(e.get("startParam"), 0));
    double s1 = num(nn(e.get("endParam"), 2 * kPi));
    if (num(ratio) > 1) {
      const double r = num(ratio);
      maj = base::pt3(-num(maj.get("y")) * r, num(maj.get("x")) * r, 0);
      ratio = 1 / r;
      s0 -= kPi / 2;
      s1 -= kPi / 2;
    }
    head("ELLIPSE", e, owner, paper);
    w(100, "AcDbEllipse");
    P(10, e.get("center"));
    P(11, maj);
    wf(40, ratio);
    wf(41, s0);
    wf(42, s1);
  } else if (type == "TEXT") {
    head("TEXT", e, owner, paper);
    w(100, "AcDbText");
    text_body(e);
    w(100, "AcDbText");
    if (const Value va = e.get("vAlign"); va.truthy()) {
      w(73, va);
    }
  } else if (type == "MTEXT") {
    head("MTEXT", e, owner, paper);
    w(100, "AcDbMText");
    P(10, e.get("insert"));
    wf(40, e.get("height"));
    wf(41, or_(e.get("width"), 0));
    w(71, or_(e.get("attach"), 1));
    w(72, 1);
    const std::string v = enc(e.get("value"));
    for (std::size_t i = 0; i + 250 < v.size(); i += 250) {
      w(3, std::string_view(v).substr(i, 250));
    }
    w(1, std::string_view(v).substr(((v.empty() ? 0 : v.size() - 1) / 250) * 250));
    const Value style = e.get("style");
    w(7, style.truthy() ? enc(style) : std::string("Standard"));
    if (const Value xa = e.get("xAxis"); xa.truthy()) {
      P(11, xa);
    }
    wf(44, or_(e.get("lineSpacing"), 1));
    const Value bf = e.get("backgroundFlags");
    if ((i32(bf) & 1) !=
        0) {  // background mask (0.5 WL.8): flags, colour (index + true colour), border offset, transparency
      const Value bc = or_(e.get("backgroundColor"), base::make_object({{"index", 7}}));
      w(90, bf);
      w(63, nn(bc.get("index"), 7));
      if (const Value rgb = bc.get("rgb"); !rgb.is_undefined()) {
        w(421, i32(rgb) & 0xffffff);
      }
      wf(45, or_(e.get("backgroundScale"), 1.5));
      w(441, i32(e.get("backgroundTransparency")));
    }
  } else if (type == "ATTDEF") {
    head("ATTDEF", e, owner, paper);
    w(100, "AcDbText");
    text_body(e);
    w(100, "AcDbAttributeDefinition");
    w(3, enc(e.get("prompt")));
    w(2, enc(e.get("tag")));
    w(70, i32(e.get("flags")));
    if (const Value va = e.get("vAlign"); va.truthy()) {
      w(74, va);
    }
  } else if (type == "INSERT") {
    head("INSERT", e, owner, paper);
    w(100, "AcDbBlockReference");
    const Value& atts = at(e, "attribs");
    const bool any = !arr(atts).empty();
    if (any) {
      w(66, 1);
    }
    w(2, enc(block_name(e.get("block"))));
    P(10, e.get("insert"));
    const Value sc = e.get("scale");
    wf(41, nn(sc.get("x"), 1));
    wf(42, nn(sc.get("y"), 1));
    wf(43, nn(sc.get("z"), 1));
    if (const Value r = e.get("rotation"); r.truthy()) {
      wf(50, deg(num(r)));
    }
    xdata(e);
    if (any) {
      const std::string ih = base::hex_upper(seed_ - 1);
      const Value elayer = e.get("layer");
      for (const Value& a : arr(atts)) {
        const Value lay = or_(a.get("layer"), elayer);
        head("ATTRIB", a, ih, paper, &lay);
        w(100, "AcDbText");
        text_body(a);
        w(100, "AcDbAttribute");
        w(2, enc(a.get("tag")));
        w(70, i32(a.get("flags")));
        if (const Value va = a.get("vAlign"); va.truthy()) {
          w(74, va);
        }
      }
      head("SEQEND", base::make_object({{"layer", elayer}}), ih, paper);
    }
    return;
  } else if (type == "SOLID" || type == "3DFACE") {
    head(type, e, owner, paper);
    w(100, type == "SOLID" ? "AcDbTrace" : "AcDbFace");
    int i = 0;
    for (const Value& p : arr(e, "points")) {
      P(10 + i++, p);
    }
    if (const Value ie = e.get("invisibleEdges"); type == "3DFACE" && ie.truthy()) {
      w(70, ie);
    }
  } else if (type == "HATCH") {
    head("HATCH", e, owner, paper);
    w(100, "AcDbHatch");
    w(10, "0.0");
    w(20, "0.0");
    wf(30, or_(e.get("elevation"), 0));
    w(210, "0.0");
    w(220, "0.0");
    w(230, "1.0");
    w(2, enc(or_(e.get("pattern"), "SOLID")));
    const bool solid = e.get("solid").truthy();
    w(70, solid ? 1 : 0);
    w(71, 0);
    const Value& loops = at(e, "loops");
    w(91, static_cast<double>(arr(loops).size()));
    for (const Value& l : arr(loops)) {
      const Value& v = at(at(l, "polyline"), "vertices");
      bool bul = false;
      for (const Value& q : arr(v)) {
        bul = bul || q.get("bulge").truthy();
      }
      w(92, i32(l.get("flags")) | 2);
      w(72, bul ? 1 : 0);
      w(73, 1);
      w(93, static_cast<double>(arr(v).size()));
      for (const Value& q : arr(v)) {
        wf(10, q.get("x"));
        wf(20, q.get("y"));
        if (bul) {
          wf(42, or_(q.get("bulge"), 0));
        }
      }
      w(97, 0);
    }
    w(75, i32(e.get("style")));
    w(76, nn(e.get("patternType"), 1));
    if (!solid) {
      wf(52, deg(num(or_(e.get("angle"), 0))));
      wf(41, or_(e.get("scale"), 1));
      w(77, 0);
      const Value& lines = at(e, "lines");
      w(78, static_cast<double>(arr(lines).size()));
      for (const Value& ln : arr(lines)) {
        wf(53, deg(num(ln.get("angle"))));
        const Value bs = ln.get("base");
        const Value of = ln.get("offset");
        wf(43, bs.get("x"));
        wf(44, bs.get("y"));
        wf(45, of.get("x"));
        wf(46, of.get("y"));
        const Value& dashes = at(ln, "dashes");
        w(79, static_cast<double>(arr(dashes).size()));
        for (const Value& d : arr(dashes)) {
          wf(49, d);
        }
      }
    }
    w(98, 0);
  } else if (type == "DIMENSION") {
    head("DIMENSION", e, owner, paper);
    w(100, "AcDbDimension");
    w(2, enc(block_name(e.get("block"))));
    P(10, e.get("defPoint"));
    P(11, e.get("textMid"));
    const Value dt = e.get("dimType");
    w(70, nn(e.get("flags"), dt));
    if (const Value t = e.get("text"); t.truthy()) {
      w(1, enc(t));
    }
    if (const Value tr = e.get("textRotation"); tr.truthy()) {
      wf(53, deg(num(tr)));
    }
    const Value style = e.get("style");
    w(3, style.truthy() && !detail::is_standard(style) ? enc(style) : std::string("Standard"));
    const int k = i32(dt) & 7;
    if (k == 0 || k == 1) {
      w(100, "AcDbAlignedDimension");
      P(13, e.get("xline1"));
      P(14, e.get("xline2"));
      if (k == 0) {
        wf(50, deg(num(or_(e.get("angle"), 0))));
        w(100, "AcDbRotatedDimension");
      }
    } else if (k == 4) {
      w(100, "AcDbRadialDimension");
      P(15, e.get("center"));
      w(40, "0.0");
    } else if (k == 3) {
      w(100, "AcDbDiametricDimension");
      P(15, e.get("center"));
      w(40, "0.0");
    } else if (k == 5) {
      w(100, "AcDb3PointAngularDimension");
      P(13, e.get("xline1"));
      P(14, e.get("xline2"));
      P(15, e.get("center"));
    }
  } else if (type == "VIEWPORT") {
    const double st = static_cast<double>(static_cast<std::uint32_t>(i32(nn(e.get("status"), 32864))));
    const auto sti = static_cast<std::uint32_t>(st);
    head("VIEWPORT", e, owner, true);
    w(100, "AcDbViewport");
    P(10, e.get("center"));
    wf(40, e.get("width"));
    wf(41, e.get("height"));
    const Value id = e.get("id");
    int status = id == Value(1) ? 1 : 2;
    if ((sti & 131072U) != 0) {
      status = 0;
    }
    w(68, status);
    w(69, or_(id, 2));
    const Value vc = e.get("viewCenter");
    wf(12, or_(vc.get("x"), 0));
    wf(22, or_(vc.get("y"), 0));
    w(13, "0.0");
    w(23, "0.0");
    w(14, "10.0");
    w(24, "10.0");
    w(15, "10.0");
    w(25, "10.0");
    w(16, "0.0");
    w(26, "0.0");
    w(36, "1.0");
    w(17, "0.0");
    w(27, "0.0");
    w(37, "0.0");
    w(42, "50.0");
    w(43, "0.0");
    w(44, "0.0");
    wf(45, e.get("viewHeight"));
    w(50, "0.0");
    wf(51, deg(num(or_(e.get("twist"), 0))));  // its twist (DR.8)
    w(72, 1000);
    for (const Value& n : arr(e, "frozenLayers")) {  // frozen in this viewport (VP.1), where AutoCAD puts them
      const std::string up = base::js_to_upper(str(n));
      for (const auto& [k, lh] : layer_h_) {
        if (k == up) {
          w(331, lh);
          break;
        }
      }
    }
    w(90, st);
    w(1, "");
    w(281, 0);
    w(71, 1);
    w(74, 0);
    w(110, "0.0");
    w(120, "0.0");
    w(130, "0.0");
    w(111, "1.0");
    w(121, "0.0");
    w(131, "0.0");
    w(112, "0.0");
    w(122, "1.0");
    w(132, "0.0");
    w(79, 0);
    w(146, "0.0");
  } else {
    note_skipped();
    return;
  }
  xdata(e);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — writeDxf2000's body, in order
DxfSaveResult Dxf2000::run() {
  DwgSaveOptions so;
  so.model_only = true;
  so.explode_leaders = true;
  DwgSaveResult saved = save_dwg(doc_, so, words_);
  const Value& model = saved.model;
  for (const base::Member* m : entries(saved.counts)) {
    counts_.set(m->key, m->value);
  }

  // ---- handles fixed up front (owners must be known before what they own is written)
  const std::string Hvport_t = h();
  const std::string Hltype_t = h();
  const std::string Hlayer_t = h();
  const std::string Hstyle_t = h();
  const std::string Hview_t = h();
  const std::string Hucs_t = h();
  const std::string Happid_t = h();
  const std::string Hdim_t = h();
  const std::string Hbr_t = h();
  const std::string Hvport = h();
  const std::string Hby_block = h();
  const std::string Hby_layer = h();
  const std::string Hcont = h();
  const std::string Hstyle = h();
  const std::string Hdim = h();
  const std::string Hnod = h();
  const std::string Hgroups = h();
  const std::string Hlayouts_d = h();
  const std::string Hmls_d = h();
  const std::string Hmls = h();
  const Array& blocks = arr(model["blocks"]);
  const Value* ms = nullptr;
  std::vector<const Value*> spaces;
  std::vector<const Value*> plain;
  for (const Value& b : blocks) {
    if (ms == nullptr && b.get("name") == Value("*Model_Space")) {
      ms = &b;
    }
  }
  for (const Value& b : blocks) {
    const std::string n = str(b.get("name"));
    bool paper_space = n.starts_with("*Paper_Space");
    for (std::size_t i = 12; paper_space && i < n.size(); ++i) {
      paper_space = n[i] >= '0' && n[i] <= '9';
    }
    if (paper_space) {
      spaces.push_back(&b);
    }
  }
  for (const Value& b : blocks) {
    if (&b != ms && std::ranges::find(spaces, &b) == spaces.end()) {
      plain.push_back(&b);
    }
  }
  if (ms == nullptr) {
    throw std::invalid_argument("Cannot read properties of undefined (reading 'name')");
  }
  const Value own_space = base::make_object({{"name", "*Paper_Space"}, {"entities", Value::array()}});
  if (spaces.empty()) {
    spaces.push_back(&own_space);
  }
  std::vector<const Value*> order = {ms};
  order.insert(order.end(), spaces.begin(), spaces.end());
  order.insert(order.end(), plain.begin(), plain.end());
  StrMap<Rec> rec;
  for (const Value* b : order) {
    Rec r;
    r.br = h();
    r.block = h();
    r.endblk = h();
    rec.set(str(b->get("name")), std::move(r));
  }
  // layouts: Model + one per sheet; each sheet's paper space by its record key
  std::vector<LayoutOut> layouts;
  layouts.push_back({.name = "Model", .tab = 0, .space = ms, .src = Value::object(), .h = {}});
  {
    std::vector<const Value*> paper_layouts;
    for (const Value& l : arr(model["layouts"])) {
      if (base::js_to_lower(str(l.get("name"))) != "model") {
        paper_layouts.push_back(&l);
      }
    }
    for (std::size_t i = 0; i < paper_layouts.size(); ++i) {
      const Value& l = *paper_layouts[i];
      // spaceOf.get(l.block): the last space whose record is the same value
      const Value key = l.get("block");
      const Value* sp = nullptr;
      for (const Value* b : spaces) {
        if (b->get("record") == key) {
          sp = b;
        }
      }
      if (sp == nullptr) {
        sp = i < spaces.size() ? spaces[i] : spaces[0];
      }
      layouts.push_back({.name = str(l.get("name")), .tab = static_cast<int>(i + 1), .space = sp, .src = l, .h = {}});
    }
  }
  if (layouts.size() == 1) {
    layouts.push_back({.name = "Layout1", .tab = 1, .space = spaces[0], .src = Value::object(), .h = {}});
  }
  for (LayoutOut& L : layouts) {
    L.h = h();
    if (Rec* r = rec.get(str(L.space->get("name")))) {
      r->layout = L.h;
    }
  }

  // ---- names: tables are matched by name, without case
  const Value& tables = at(model, "tables");
  lt_names_ = {"BYLAYER", "BYBLOCK", "CONTINUOUS"};
  for (const Value& l : arr(tables, "linetypes")) {
    lt_names_.push_back(base::js_to_upper(str(l.get("name"))));
  }
  std::vector<std::string> apps = {"ACAD", "CADCRAFT"};
  const auto add_app = [&](const std::string& a) {
    if (std::ranges::find(apps, a) == apps.end()) {
      apps.push_back(a);
    }
  };
  const std::function<void(const Value&)> collect_apps = [&](const Value& ents) {
    for (const Value& e : arr(ents)) {
      for (const base::Member* m : entries(at(e, "xdata"))) {
        add_app(base::js_to_upper(m->key));
      }
      collect_apps(at(e, "attribs"));
    }
  };
  for (const Value& b : blocks) {
    collect_apps(at(b, "entities"));
  }

  // ---- HEADER
  const Value hd = or_(model.get("header"), Value::object());
  w(0, "SECTION");
  w(2, "HEADER");
  w(9, "$ACADVER");
  w(1, "AC1015");
  w(9, "$DWGCODEPAGE");
  w(3, "ANSI_1252");
  w(9, "$HANDSEED");
  const std::size_t handseed_at = L_.value_at();
  w(5, "FFFF");
  w(9, "$INSBASE");
  {
    const Value ib = hd.get("INSBASE");
    wf(10, or_(ib.get("x"), 0));
    wf(20, or_(ib.get("y"), 0));
    w(30, "0.0");
  }
  const Value emin = hd.get("EXTMIN");
  const Value emax = hd.get("EXTMAX");
  if (emin.truthy() && emax.truthy()) {
    w(9, "$EXTMIN");
    wf(10, emin.get("x"));
    wf(20, emin.get("y"));
    w(30, "0.0");
    w(9, "$EXTMAX");
    wf(10, emax.get("x"));
    wf(20, emax.get("y"));
    w(30, "0.0");
  }
  w(9, "$INSUNITS");
  w(70, i32(hd.get("INSUNITS")));
  w(9, "$MEASUREMENT");
  w(70, nn(hd.get("MEASUREMENT"), 1));
  w(9, "$LUNITS");
  w(70, or_(hd.get("LUNITS"), 2));
  if (const Value uo = hd.get("UCSORG"), ux = hd.get("UCSXDIR");
      uo.truthy() && ux.truthy()) {  // the current UCS (DR.8)
    const Value uy = or_(hd.get("UCSYDIR"), base::make_object({{"x", -num(ux.get("y"))}, {"y", ux.get("x")}}));
    for (const auto& [k, p] :
         {std::pair<std::string_view, const Value&>{"$UCSORG", uo}, {"$UCSXDIR", ux}, {"$UCSYDIR", uy}}) {
      w(9, k);
      wf(10, p.get("x"));
      wf(20, p.get("y"));
      w(30, "0.0");
    }
  }
  if (const Value lp = hd.get("LUPREC"); finite(lp)) {
    w(9, "$LUPREC");
    w(70, i32(lp));
  }
  w(9, "$LTSCALE");
  wf(40, or_(hd.get("LTSCALE"), 1));
  w(9, "$CLAYER");
  w(8, "0");
  w(9, "$TILEMODE");
  w(70, 1);
  w(0, "ENDSEC");
  w(0, "SECTION");
  w(2, "CLASSES");
  w(0, "ENDSEC");

  // ---- TABLES
  w(0, "SECTION");
  w(2, "TABLES");
  // *Active: one zoomed to the drawing, or the split screen (dwg-save.js activeVports: the current pane first)
  const Value e0 = or_(emin, base::pt2(0, 0));
  const Value e1 = or_(emax, base::pt2(12, 9));
  const double vh0 =
      js_max3(num(e1.get("y")) - num(e0.get("y")), (num(e1.get("x")) - num(e0.get("x"))) / 1.5, 1) * 1.05;
  const Value tv = tables.get("vports");
  const Array actives =
      arr(tv).empty()
          ? Array{base::make_object({{"lowerLeft", base::pt2(0, 0)}, {"upperRight", base::pt2(1, 1)}, {"aspect", 1.5}})}
          : arr(tv);
  table("VPORT", Hvport_t, actives.size(), [&] {
    for (std::size_t k = 0; k < actives.size(); ++k) {
      const Value& v = actives[k];
      entry("VPORT", k != 0 ? h() : Hvport, Hvport_t, "AcDbViewportTableRecord", "*Active");
      const Value c = or_(v.get("center"), base::make_object({{"x", (num(e0.get("x")) + num(e1.get("x"))) / 2},
                                                              {"y", (num(e0.get("y")) + num(e1.get("y"))) / 2}}));
      const Value ll = v.get("lowerLeft");
      const Value ur = v.get("upperRight");
      wf(10, ll.get("x"));
      wf(20, ll.get("y"));
      wf(11, ur.get("x"));
      wf(21, ur.get("y"));
      wf(12, c.get("x"));
      wf(22, c.get("y"));
      w(13, "0.0");
      w(23, "0.0");
      w(14, "1.0");
      w(24, "1.0");
      w(15, "1.0");
      w(25, "1.0");
      w(16, "0.0");
      w(26, "0.0");
      w(36, "1.0");
      w(17, "0.0");
      w(27, "0.0");
      w(37, "0.0");
      const Value vh = v.get("viewHeight");
      wf(40, num(vh) > 0 ? vh : Value(vh0));
      const Value asp = v.get("aspect");
      wf(41, num(asp) > 0 ? asp : Value(1.5));
      w(42, "50.0");
      w(43, "0.0");
      w(44, "0.0");
      w(50, "0.0");
      wf(51, deg(num(or_(v.get("twist"), 0))));  // view twist (DR.8)
      w(71, 0);
      w(72, 1000);
      w(73, 1);
      w(74, 3);
      w(75, 0);
      w(76, 0);
      w(77, 0);
      w(78, 0);
      for (const base::Member* m : entries(v.get("xdata"))) {
        w(1001, base::js_to_upper(m->key));
        for (const Value& it : arr(m->value)) {
          const Array& p = arr(it);
          if (p.size() >= 2) {
            const int code = i32(p[0]);
            if (code == 1000) {
              w(code, enc(p[1]));
            } else {
              w(code, p[1]);
            }
          }
        }
      }
    }
  });
  const Array& lts = arr(tables, "linetypes");
  table("LTYPE", Hltype_t, 3 + lts.size(), [&] {
    for (const auto& [eh, n, d] :
         {std::tuple<const std::string&, std::string_view, std::string_view>{Hby_block, "ByBlock", ""},
          {Hby_layer, "ByLayer", ""},
          {Hcont, "Continuous", "Solid line"}}) {
      entry("LTYPE", eh, Hltype_t, "AcDbLinetypeTableRecord", std::string(n));
      w(3, d);
      w(72, 65);
      w(73, 0);
      w(40, "0.0");
    }
    for (const Value& l : lts) {
      entry("LTYPE", h(), Hltype_t, "AcDbLinetypeTableRecord", str(l.get("name")));
      w(3, enc(or_(l.get("description"), l.get("name"))));
      w(72, 65);
      const Value els = l.get("elements");
      w(73, static_cast<double>(arr(els).size()));
      wf(40, l.get("patternLength"));
      for (const Value& x : arr(els)) {
        wf(49, x.get("length"));
        w(74, 0);
      }
    }
  });
  const Array& ls = arr(tables, "layers");
  bool has_zero = false;
  for (const Value& l : ls) {
    has_zero = has_zero || l.get("name") == Value("0");
  }
  table("LAYER", Hlayer_t, ls.size() + (has_zero ? 0 : 1), [&] {
    if (!has_zero) {
      entry("LAYER", h(), Hlayer_t, "AcDbLayerTableRecord", "0");
      w(62, 7);
      w(6, "Continuous");
      w(370, -3);
      w(390, "0");
    }
    for (const Value& l : ls) {
      const std::string lh = h();
      // VP.1: viewports name their frozen layers by handle (331)
      const std::string up = base::js_to_upper(str(l.get("name")));
      bool found = false;
      for (auto& [k, v] : layer_h_) {
        if (k == up) {
          v = lh;
          found = true;
        }
      }
      if (!found) {
        layer_h_.emplace_back(up, lh);
      }
      const int flags = (l.get("frozen").truthy() ? 1 : 0) | (l.get("frozenInNewViewports").truthy() ? 2 : 0) |
                        (l.get("locked").truthy() ? 4 : 0);
      entry("LAYER", lh, Hlayer_t, "AcDbLayerTableRecord", enc(layer_name(l.get("name"))), flags);
      const Value col = l.get("color");
      const Value ci = nn(col.get("index"), 7);
      if (l.get("off").truthy()) {
        w(62, -std::abs(num(or_(ci, 7))));
      } else {
        w(62, or_(ci, 7));
      }
      if (const Value rgb = col.get("rgb"); !rgb.is_undefined()) {
        w(420, i32(rgb) & 0xffffff);
      }
      w(6, lt_name(l.get("linetype")).value_or("Continuous"));
      if (l.get("plot") == Value(false)) {
        w(290, 0);
      }
      const Value lw = l.get("lineweight");
      w(370, finite(lw) ? lw : Value(-3));
      w(390, "0");
    }
  });
  // text styles (P.7): Standard and every one the drawing keeps or an object names
  std::vector<const Value*> styles_in;
  for (const Value& x : arr(tables, "styles")) {
    if (x.get("name").truthy() && !detail::is_standard(x.get("name"))) {
      styles_in.push_back(&x);
    }
  }
  StrMap<std::string> style_h;
  style_h.set("STANDARD", Hstyle);
  for (const Value* x : styles_in) {
    style_h.set(base::js_to_upper(str(x->get("name"))), h());
  }
  table("STYLE", Hstyle_t, 1 + styles_in.size(), [&] {
    entry("STYLE", Hstyle, Hstyle_t, "AcDbTextStyleTableRecord", "Standard");
    w(40, "0.0");
    w(41, "1.0");
    w(50, "0.0");
    w(71, 0);
    w(42, "2.5");
    w(3, "txt");
    w(4, "");
    for (const Value* x : styles_in) {
      entry("STYLE", *style_h.get(base::js_to_upper(str(x->get("name")))), Hstyle_t, "AcDbTextStyleTableRecord",
            enc(x->get("name")));
      wf(40, or_(x->get("height"), 0));
      wf(41, or_(x->get("widthFactor"), 1));
      wf(50, deg(num(or_(x->get("oblique"), 0))));
      w(71, 0);
      w(42, "2.5");
      w(3, enc(or_(x->get("font"), "txt")));
      w(4, "");
    }
  });
  const Array& views = arr(tables, "views");
  table("VIEW", Hview_t, views.size(), [&] {
    for (const Value& v : views) {
      entry("VIEW", h(), Hview_t, "AcDbViewTableRecord", enc(v.get("name")), v.get("paperSpace").truthy() ? 1 : 0);
      const Value c = v.get("center");
      wf(40, v.get("height"));
      wf(10, c.get("x"));
      wf(20, c.get("y"));
      wf(41, v.get("width"));
      w(11, "0.0");
      w(21, "0.0");
      w(31, "1.0");
      w(12, "0.0");
      w(22, "0.0");
      w(32, "0.0");
      w(42, "50.0");
      w(43, "0.0");
      w(44, "0.0");
      w(50, "0.0");
      w(71, 0);
      w(281, 0);
      w(72, 0);
    }
  });
  std::vector<const Value*> ucs_t;  // named UCSs (DR.8)
  for (const Value& u : arr(tables, "ucs")) {
    if (u.truthy() && u.get("name").truthy() && u.get("origin").truthy() && u.get("xAxis").truthy()) {
      ucs_t.push_back(&u);
    }
  }
  table("UCS", Hucs_t, ucs_t.size(), [&] {
    for (const Value* u : ucs_t) {
      entry("UCS", h(), Hucs_t, "AcDbUCSTableRecord", enc(u->get("name")));
      const Value xa = u->get("xAxis");
      const Value ya = or_(u->get("yAxis"), base::make_object({{"x", -num(xa.get("y"))}, {"y", xa.get("x")}}));
      const Value org = u->get("origin");
      for (const auto& [c, q] : {std::pair<int, const Value&>{10, org}, {11, xa}, {12, ya}}) {
        wf(c, or_(q.get("x"), 0));
        wf(c + 10, or_(q.get("y"), 0));
        w(c + 20, "0.0");
      }
      w(79, 0);
      w(146, "0.0");
    }
  });
  std::vector<std::pair<std::string, std::string>> app_h;
  app_h.reserve(apps.size());
  for (const std::string& a : apps) {
    app_h.emplace_back(a, h());
  }
  table("APPID", Happid_t, apps.size(), [&] {
    for (const auto& [a, ah] : app_h) {
      entry("APPID", ah, Happid_t, "AcDbRegAppTableRecord", a);
    }
  });
  // dimension styles (P.6): every variable, one for one (dwg-props.js)
  const Value tds = tables.get("dimstyles");
  const Array dims_in =
      tds.truthy() ? arr(tds) : Array{base::make_object({{"name", "Standard"}, {"vars", Value::object()}})};
  table("DIMSTYLE", Hdim_t, dims_in.size(), [&] {
    for (std::size_t k = 0; k < dims_in.size(); ++k) {
      const Value& d = dims_in[k];
      const bool std_ = detail::is_standard(d.get("name"));
      entry("DIMSTYLE", std_ && k == 0 ? Hdim : h(), Hdim_t, "AcDbDimStyleTableRecord",
            std_ ? std::string("Standard") : enc(d.get("name")));
      const Value vars = or_(d.get("vars"), Value::object());
      for (const Value& pv : cad::dimstyle_pairs(cad::st_from_vars(vars), false)) {
        const Array& p = pv.as_array();
        const int code = i32(p.at(0));
        const Value& v = p.at(1);
        if (v.is_string()) {
          w(code, enc(v));
        } else if (v.is_number() && std::isfinite(v.as_number()) && std::trunc(v.as_number()) == v.as_number()) {
          w(code, v);
        } else {
          wf(code, v);
        }
      }
      const std::string tx = base::js_to_upper(str(or_(vars.get("DIMTXSTY"), "Standard")));
      const std::string* sh = style_h.get(tx);
      w(340, sh != nullptr && !sh->empty() ? *sh : Hstyle);
    }
  });
  table("BLOCK_RECORD", Hbr_t, rec.size(), [&] {
    for (const Value* b : order) {
      const Rec& r = *rec.get(str(b->get("name")));
      entry("BLOCK_RECORD", r.br, Hbr_t, "AcDbBlockTableRecord", enc(block_name(b->get("name"))));
      w(340, r.layout.empty() ? std::string("0") : r.layout);
    }
  });
  w(0, "ENDSEC");

  // ---- BLOCKS: every record's BLOCK / ENDBLK; plain blocks and the other sheets hold their objects here
  w(0, "SECTION");
  w(2, "BLOCKS");
  const Value* active = spaces[0];
  for (const Value* b : order) {
    const Rec r = *rec.get(str(b->get("name")));
    const bool is_paper = std::ranges::find(spaces, b) != spaces.end();
    const std::string bn = str(b->get("name"));
    const std::string name = enc(block_name(b->get("name")));
    w(0, "BLOCK");
    w(5, r.block);
    w(330, r.br);
    w(100, "AcDbEntity");
    if (is_paper) {
      w(67, 1);
    }
    w(8, "0");
    w(100, "AcDbBlockBegin");
    w(2, name);
    const std::string bup = base::js_to_upper(bn);
    bool attdefs = false;
    for (const Value& x : arr(*b, "entities")) {
      attdefs = attdefs || x.get("type") == Value("ATTDEF");
    }
    const bool xref = (i32(b->get("flags")) & 4) != 0;
    w(70, (bn.starts_with('*') && bup != "*MODEL_SPACE" && bup != "*PAPER_SPACE" && !is_paper ? 1 : 0) |
              (attdefs ? 2 : 0) | (xref ? 4 : 0));
    P(10, b->get("base"));
    w(3, name);
    w(1, xref ? enc(or_(b->get("xrefPath"), "")) : std::string());
    if (b != ms && b != active && !xref) {
      for (const Value& e : arr((*b)["entities"])) {
        entity(e, r.br, is_paper);
      }
    }
    w(0, "ENDBLK");
    w(5, r.endblk);
    w(330, r.br);
    w(100, "AcDbEntity");
    if (is_paper) {
      w(67, 1);
    }
    w(8, "0");
    w(100, "AcDbBlockEnd");
  }
  w(0, "ENDSEC");
  // ---- ENTITIES: the model and the first sheet
  w(0, "SECTION");
  w(2, "ENTITIES");
  {
    const std::string msbr = rec.get(str(ms->get("name")))->br;
    for (const Value& e : arr((*ms)["entities"])) {
      entity(e, msbr, false);
    }
    const std::string abr = rec.get(str(active->get("name")))->br;
    for (const Value& e : arr((*active)["entities"])) {
      entity(e, abr, true);
    }
  }
  w(0, "ENDSEC");

  // ---- OBJECTS: the named-object dictionary, groups, layouts, the Standard multiline style
  w(0, "SECTION");
  w(2, "OBJECTS");
  dict(Hnod, "0", {{"ACAD_GROUP", Hgroups}, {"ACAD_LAYOUT", Hlayouts_d}, {"ACAD_MLINESTYLE", Hmls_d}});
  dict(Hgroups, Hnod, {});
  {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(layouts.size());
    for (const LayoutOut& L : layouts) {
      items.emplace_back(enc(L.name), L.h);
    }
    dict(Hlayouts_d, Hnod, items);
  }
  dict(Hmls_d, Hnod, {{"Standard", Hmls}});
  // clipped references (WL.16): xdict → ACAD_FILTER → SPATIAL → SPATIAL_FILTER (block coordinates, identity matrices)
  for (const Clip& c : clips_) {
    dict(c.xd, c.own, {{"ACAD_FILTER", c.d2}});
    dict(c.d2, c.xd, {{"SPATIAL", c.sf}});
    w(0, "SPATIAL_FILTER");
    w(5, c.sf);
    w(102, "{ACAD_REACTORS");
    w(330, c.d2);
    w(102, "}");
    w(330, c.d2);
    w(100, "AcDbFilter");
    w(100, "AcDbSpatialFilter");
    w(70, static_cast<double>(arr(c.pts).size()));
    for (const Value& p : arr(c.pts)) {
      wf(10, p.get("x"));
      wf(20, p.get("y"));
    }
    w(210, "0.0");
    w(220, "0.0");
    w(230, "1.0");
    w(11, "0.0");
    w(21, "0.0");
    w(31, "0.0");
    w(71, 1);
    w(72, 0);
    w(73, 0);
    for (int k = 0; k < 2; ++k) {
      for (int i = 0; i < 12; ++i) {
        w(40, i % 5 == 0 ? "1.0" : "0.0");  // 3 × 4 identity, twice
      }
    }
  }
  w(0, "MLINESTYLE");
  w(5, Hmls);
  w(330, Hmls_d);
  w(100, "AcDbMlineStyle");
  w(2, "Standard");
  w(70, 0);
  w(3, "");
  w(62, 256);
  w(51, "90.0");
  w(52, "90.0");
  w(71, 2);
  for (const double o : {0.5, -0.5}) {
    wf(49, o);
    w(62, 256);
    w(6, "BYLAYER");
  }
  for (const LayoutOut& L : layouts) {
    const Value p = or_(L.src.get("paper"), Value::object());
    const Value lim =
        or_(L.src.get("limits"), base::make_object({{"min", base::pt2(0, 0)}, {"max", base::pt2(12, 9)}}));
    const bool paper = L.tab > 0;
    w(0, "LAYOUT");
    w(5, L.h);
    w(102, "{ACAD_REACTORS");
    w(330, Hlayouts_d);
    w(102, "}");
    w(330, Hlayouts_d);
    w(100, "AcDbPlotSettings");
    w(1, "");
    w(2, "none_device");
    w(4, enc(or_(p.get("name"), "")));
    w(6, "");
    w(40, "0.0");
    w(41, "0.0");
    w(42, "0.0");
    w(43, "0.0");
    wf(44, or_(p.get("w"), 215.9));
    wf(45, or_(p.get("h"), 279.4));
    w(46, "0.0");
    w(47, "0.0");
    w(48, "0.0");
    w(49, "0.0");
    w(140, "0.0");
    w(141, "0.0");
    w(142, "1.0");
    w(143, "1.0");
    w(70, paper ? 688 : 1712);
    w(72, nn(p.get("units"), paper ? 1 : 0));
    w(73, 0);
    w(74, paper ? 5 : 0);
    w(7, "");
    w(75, paper ? 16 : 0);
    w(147, "1.0");
    w(148, "0.0");
    w(149, "0.0");
    w(100, "AcDbLayout");
    w(1, enc(L.name));
    w(70, 1);
    w(71, L.tab);
    const Value mn = lim.get("min");
    const Value mx = lim.get("max");
    wf(10, mn.get("x"));
    wf(20, mn.get("y"));
    wf(11, mx.get("x"));
    wf(21, mx.get("y"));
    w(12, "0.0");
    w(22, "0.0");
    w(32, "0.0");
    w(14, "0.0");
    w(24, "0.0");
    w(34, "0.0");
    w(15, "0.0");
    w(25, "0.0");
    w(35, "0.0");
    w(146, "0.0");
    w(13, "0.0");
    w(23, "0.0");
    w(33, "0.0");
    w(16, "1.0");
    w(26, "0.0");
    w(36, "0.0");
    w(17, "0.0");
    w(27, "1.0");
    w(37, "0.0");
    w(76, 0);
    w(330, rec.get(str(L.space->get("name")))->br);
  }
  w(0, "ENDSEC");
  w(0, "EOF");
  L_.patch(handseed_at, 4, base::hex_upper(seed_));

  Array notes;
  if (const double n = count("mleader"); n != 0) {
    notes.emplace_back(words_.tn("dxf2000.save.mleader", n));
  }
  if (const double n = detail::picture_refs(doc_); n != 0) {
    notes.emplace_back(words_.tn("dxf.save.pictures", n));
  }
  if (const double n = detail::viewport_overrides(doc_); n != 0) {
    notes.emplace_back(words_.tn("dxf.save.vpovr", n));
  }
  if (count("material") != 0) {
    notes.emplace_back(words_.t("dwg.save.material"));
  }
  if (count("tr2000") != 0) {
    notes.emplace_back(words_.t("dwg.save.tr2000"));
  }
  if (const double n = count("xrefBound"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.xref.bound", n));
  }
  if (const double n = count("xrefLink"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.xref.link", n));
  }
  if (const double n = count("skipped") + count("other"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.other", n));
  }
  DxfSaveResult res;
  res.text = L_.take();
  res.report = base::make_object({{"notes", std::move(notes)}, {"counts", counts_}});
  return res;
}

}  // namespace

DxfSaveResult write_dxf2000(const doc::Doc& doc, const i18n::Catalog& words) {
  Dxf2000 x(doc, words);
  return x.run();
}

}  // namespace cc::save
