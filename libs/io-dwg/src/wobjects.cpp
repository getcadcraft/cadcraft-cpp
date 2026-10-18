// SPDX-License-Identifier: Apache-2.0
// objects-write.js in C++ (see wobjects.hpp). The encoders keep the JS order of fields; the "Spec gap:" notes are in
// the JS file.
#include "wobjects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "container.hpp"
#include "record.hpp"

namespace cc::dwg::w {

namespace {

using base::make_object;
constexpr double kPi = std::numbers::pi;

const Value& color_index(double i) {
  static const Value c250 = make_object({{"index", 250}});
  static const Value c256 = make_object({{"index", 256}});
  static const Value c7 = make_object({{"index", 7}});
  if (i == 250) {
    return c250;
  }
  if (i == 7) {
    return c7;
  }
  return c256;
}

double rad(const Value& a) {
  constexpr double t = 2 * kPi;
  double v = base::js_to_number(a);
  if (v != v) {
    v = 0;
  }
  double r = v == 0 ? 0 : std::fmod(v, t);
  if (r < 0) {
    r += t;
  }
  return r;
}

bool default_extrusion(const Value& e) {
  return nullish(e) || (numk(e, "x") == 0 && numk(e, "y") == 0 && numk(e, "z", 1) == 1);
}
void BEx(ObjWriter& w, const Value& e) {
  if (default_extrusion(e)) {
    w.BE();
  } else {
    w.BE(numk(e, "x"), numk(e, "y"), numk(e, "z", 1));
  }
}
void ext3(ObjWriter& w, const Value& e) {
  if (e.truthy()) {
    w.BD3(numk(e, "x"), numk(e, "y"), numk(e, "z", 1));
  } else {
    w.BD3(0, 0, 1);
  }
}
/// w.P3(P(p)): P's finite numbers, then P3's `|| 0` (a -0 becomes 0)
void P3n(ObjWriter& w, const D3& p) {
  const auto z = [](double v) { return v == 0 ? 0.0 : v; };
  w.BD3(z(p.x), z(p.y), z(p.z));
}
const Value& unit3(int which) {
  static const Value x = make_object({{"x", 1}, {"y", 0}, {"z", 0}});
  static const Value y = make_object({{"x", 0}, {"y", 1}, {"z", 0}});
  static const Value z = make_object({{"x", 0}, {"y", 0}, {"z", 1}});
  static const Value one = make_object({{"x", 1}, {"y", 1}, {"z", 1}});
  switch (which) {
    case 0: return x;
    case 1: return y;
    case 2: return z;
    default: return one;
  }
}
/// a || b
Value orv(const Value& a, const Value& b) {
  return a.truthy() ? a : b;
}
double flag_of(const Value& r, std::string_view k) {
  return nnum(get(ref(r, k), "flag"), 0);
}

// ---------------------------------------------------------------- common parts

void control(ObjWriter& w, const Value& r) {
  object_common(w, r);
  const Value& cnt = ref(r, "count");
  w.BL(nullish(cnt) ? static_cast<double>(arr(r, "entries").size()) : base::js_to_number(cnt));
  if (ref(r, "t").as_string() == "DIMSTYLE_CONTROL") {
    w.RC(0);
  }
  for (const Value& h : arr(r, "entries")) {
    w.H(kSoftOwn, h);
  }
  for (const Value& h : arr(r, "hardEntries")) {
    w.H(kHardOwn, h);
  }
}

void block_header(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"), tr(r, "xdep") ? 16 : 0);
  const bool xref = tr(r, "isXref");
  const bool overlaid = tr(r, "overlaid");
  w.B(tr(r, "anonymous"));
  w.B(tr(r, "hasAttrs"));
  w.B(xref);
  w.B(overlaid);
  w.B(false);
  const Array& owned = arr(r, "owned");
  if (w.r2004 && !xref && !overlaid) {
    w.BL(static_cast<double>(owned.size()));
  }
  w.P3(ref(r, "base"));
  w.TV(orv(ref(r, "xrefPath"), Value("")));
  const Array& inserts = arr(r, "inserts");
  for (std::size_t i = 0; i < inserts.size(); ++i) {
    w.RC(1);
  }
  w.RC(0);
  w.TV(orv(ref(r, "description"), Value("")));
  const Value& pv = ref(r, "preview");
  if (pv.is_bytes()) {
    w.BL(static_cast<double>(pv.as_bytes().size()));
    for (const auto b : pv.as_bytes()) {
      w.RC8(static_cast<std::uint8_t>(b));
    }
  } else {
    w.BL(0);
  }
  if (w.r2007) {
    w.BS(nnum(ref(r, "insertUnits"), 0));
    w.B(!is_false(ref(r, "explodable")));
    w.RC(is_false(ref(r, "scalable")) ? 0 : 1);
  }
  w.H(kHardPtr, 0);
  w.H(kHardOwn, ref(r, "block"));
  if (!w.r2004) {
    if (!xref && !overlaid) {
      w.H(kSoftPtr, owned.empty() ? Value(0) : owned.front());
      w.H(kSoftPtr, owned.empty() ? Value(0) : owned.back());
    }
  } else if (!xref && !overlaid) {
    for (const Value& h : owned) {
      w.H(kHardOwn, h);
    }
  }
  w.H(kHardOwn, ref(r, "endblk"));
  for (const Value& h : inserts) {
    w.H(kSoftPtr, h);
  }
  w.H(kHardPtr, ref(r, "layout"));
}

void layer(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"), tr(r, "xdep") ? 16 : 0);
  const double lw = nnum(ref(r, "lwIndex"), 31);
  const int lwi = base::js_to_int32(lw);
  w.BS(static_cast<double>((tr(r, "frozen") ? 1 : 0) | (tr(r, "off") ? 2 : 0) | (tr(r, "frozenNew") ? 4 : 0) |
                           (tr(r, "locked") ? 8 : 0) | (is_false(ref(r, "plot")) ? 0 : 16) | ((lwi & 31) << 5)));
  const Value& col = ref(r, "color");
  if (col.truthy() && get(col, "rgb").is_undefined()) {
    w.CMC(make_object({{"index", std::abs(nnum(get(col, "index"), 7))}}));
  } else {
    w.CMC(col);
  }
  w.H(kHardPtr, ref(r, "xrefBlock"));
  w.H(kHardPtr, ref(r, "plotstyle"));
  if (w.r2007) {
    w.H(kHardPtr, ref(r, "material"));
  }
  w.H(kHardPtr, ref(r, "ltype"));
  if (w.r2013) {
    w.H(kHardPtr, 0);
  }
}

void style(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  w.B(tr(r, "vertical"));
  w.B(tr(r, "shapeFile"));
  w.BD(numk(r, "height"));
  w.BD(numk(r, "widthFactor", 1));
  w.BD(numk(r, "oblique"));
  w.RC(ivk(r, "generation"));
  w.BD(numk(r, "lastHeight", 0.2));
  const Value& font = ref(r, "font");
  w.TV(nullish(font) ? Value("txt") : font);
  const Value& big = ref(r, "bigFont");
  w.TV(nullish(big) ? Value("") : big);
  w.H(kHardPtr, 0);
}

void ltype(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  w.TV(orv(ref(r, "description"), Value("")));
  const Array& els = arr(r, "elements");
  double sum = 0;
  for (const Value& e : els) {
    sum += std::abs(numk(e, "length"));
  }
  w.BD(numk(r, "patternLength", sum));
  w.RC(65);
  w.RC(static_cast<double>(els.size()));
  bool text_area = false;
  for (const Value& e : els) {
    w.BD(numk(e, "length"));
    w.BS(ivk(e, "shape"));
    const Value off = get(ref(e, "offset"), "x");
    w.RD(num(nullish(off) ? ref(e, "offsetX") : off));
    const Value offy = get(ref(e, "offset"), "y");
    w.RD(num(nullish(offy) ? ref(e, "offsetY") : offy));
    w.BD(numk(e, "scale", 1));
    w.BD(numk(e, "rotation"));
    w.BS(ivk(e, "flags"));
    if ((base::js_to_int32(ivk(e, "flags")) & 2) != 0) {
      text_area = true;
    }
  }
  if (!w.r2007) {
    for (int i = 0; i < 256; ++i) {
      w.RC(std::uint8_t{0});
    }
  } else if (text_area) {
    for (int i = 0; i < 512; ++i) {
      w.RC(std::uint8_t{0});
    }
  }
  w.H(kHardPtr, 0);
  for (const Value& e : els) {
    w.H(kHardPtr, ref(e, "styleH"));
  }
}

void ucs(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  const D3 o = P(ref(r, "origin"));
  const D3 x = P(ref(r, "xAxis"));
  const D3 y = P(ref(r, "yAxis"));
  w.BD3(o.x, o.y, o.z);
  w.BD3(x.x, x.y, x.z);
  w.BD3(y.x, y.y, y.z);
  w.BD(numk(r, "elevation"));
  w.BS(0);
  w.BS(0);
  w.H(kHardPtr, 0);
  w.H(kHardPtr, 0);
  w.H(kHardPtr, 0);
}

void vport(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  const double vh = numk(r, "viewHeight", 1);
  const double asp = numk(r, "aspect", 1.5);
  w.BD(vh);
  w.BD(vh * asp);
  w.RD2(numk(ref(r, "center"), "x"), numk(ref(r, "center"), "y"));
  w.P3(ref(r, "target"));
  ext3(w, orv(ref(r, "direction"), unit3(2)));
  w.BD(numk(r, "twist"));
  w.BD(numk(r, "lens", 50));
  w.BD(0);
  w.BD(0);
  w.bits(nnum(ref(r, "viewMode"), 0), 4);
  w.RC(0);
  if (w.r2007) {
    w.B(true);
    w.RC(1);
    w.BD(0);
    w.BD(0);
    w.CMC(color_index(250));
  }
  w.RD2(numk(ref(r, "lowerLeft"), "x"), numk(ref(r, "lowerLeft"), "y"));
  w.RD2(numk(ref(r, "upperRight"), "x", 1), numk(ref(r, "upperRight"), "y", 1));
  w.B(false);
  w.BS(1000);
  w.B(true);
  w.bits(3, 2);
  w.B(false);
  w.RD2(numk(ref(r, "grid"), "x", 10), numk(ref(r, "grid"), "y", 10));
  w.B(false);
  w.B(false);
  w.BS(0);
  w.BD(0);
  w.RD2(0, 0);
  w.RD2(numk(ref(r, "snap"), "x", 10), numk(ref(r, "snap"), "y", 10));
  w.B(false);
  w.B(true);
  w.BD3(0, 0, 0);
  w.BD3(1, 0, 0);
  w.BD3(0, 1, 0);
  w.BD(0);
  w.BS(0);
  if (w.r2007) {
    w.BS(7);
    w.BS(5);
  }
  w.H(kHardPtr, 0);
  if (w.r2007) {
    w.H(kSoftPtr, 0);
    w.H(kHardPtr, 0);
    w.H(kHardOwn, ref(r, "sun"));
  }
  w.H(kHardPtr, 0);
  w.H(kHardPtr, 0);
}

void view(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"), base::js_to_int32(ivk(r, "flags")) & 64);
  w.BD(numk(r, "height", 1));
  w.BD(numk(r, "width", 1));
  w.RD2(numk(ref(r, "center"), "x"), numk(ref(r, "center"), "y"));
  P3n(w, P(ref(r, "target")));
  ext3(w, orv(ref(r, "direction"), unit3(2)));
  w.BD(numk(r, "twist"));
  w.BD(numk(r, "lensLength", 50));
  w.BD(numk(r, "frontClip"));
  w.BD(numk(r, "backClip"));
  w.bits(static_cast<double>(base::js_to_int32(ivk(r, "viewMode")) & 15), 4);
  w.RC(ivk(r, "renderMode"));
  if (w.r2007) {
    w.B(!is_false(ref(r, "useDefaultLights")));
    w.RC(nnum(ref(r, "defaultLightingType"), 1));
    w.BD(numk(r, "brightness"));
    w.BD(numk(r, "contrast"));
    w.CMC(orv(ref(r, "ambientColor"), color_index(250)));
  }
  w.B(tr(r, "paperSpace"));
  const bool assoc = tr(r, "associatedUcs");
  w.B(assoc);
  if (assoc) {
    P3n(w, P(ref(r, "ucsOrigin")));
    if (tr(r, "ucsXAxis")) {
      P3n(w, P(ref(r, "ucsXAxis")));
    } else {
      w.P3(unit3(0));
    }
    if (tr(r, "ucsYAxis")) {
      P3n(w, P(ref(r, "ucsYAxis")));
    } else {
      w.P3(unit3(1));
    }
    w.BD(numk(r, "ucsElevation"));
    w.BS(ivk(r, "ucsOrthoType"));
  }
  if (w.r2007) {
    w.B(tr(r, "cameraPlottable"));
  }
  w.H(kHardPtr, 0);
  if (w.r2007) {
    w.H(kSoftPtr, 0);
    w.H(kHardPtr, 0);
    w.H(kHardOwn, 0);
  }
  if (assoc) {
    w.H(kHardPtr, 0);
    w.H(kHardPtr, 0);
  }
  if (w.r2007) {
    w.H(kSoftPtr, 0);
  }
}

void appid(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  w.RC(0);
  w.H(kHardPtr, 0);
}

const Value& dim_defaults_js() {
  static const Value d = make_object(
      {{"DIMPOST", ""},   {"DIMAPOST", ""}, {"DIMSCALE", 1},        {"DIMASZ", 0.18},       {"DIMEXO", 0.0625},
       {"DIMDLI", 0.38},  {"DIMEXE", 0.18}, {"DIMRND", 0},          {"DIMDLE", 0},          {"DIMTP", 0},
       {"DIMTM", 0},      {"DIMFXL", 1},    {"DIMJOGANG", kPi / 4}, {"DIMTFILL", 0},        {"DIMTFILLCLR", 0},
       {"DIMTOL", 0},     {"DIMLIM", 0},    {"DIMTIH", 1},          {"DIMTOH", 1},          {"DIMSE1", 0},
       {"DIMSE2", 0},     {"DIMTAD", 0},    {"DIMZIN", 0},          {"DIMAZIN", 0},         {"DIMARCSYM", 0},
       {"DIMTXT", 0.18},  {"DIMCEN", 0.09}, {"DIMTSZ", 0},          {"DIMALTF", 25.4},      {"DIMLFAC", 1},
       {"DIMTVP", 0},     {"DIMTFAC", 1},   {"DIMGAP", 0.09},       {"DIMALTRND", 0},       {"DIMALT", 0},
       {"DIMALTD", 2},    {"DIMTOFL", 0},   {"DIMSAH", 0},          {"DIMTIX", 0},          {"DIMSOXD", 0},
       {"DIMCLRD", 0},    {"DIMCLRE", 0},   {"DIMCLRT", 0},         {"DIMADEC", 0},         {"DIMDEC", 4},
       {"DIMTDEC", 4},    {"DIMALTU", 2},   {"DIMALTTD", 2},        {"DIMAUNIT", 0},        {"DIMFRAC", 0},
       {"DIMLUNIT", 2},   {"DIMDSEP", 46},  {"DIMTMOVE", 0},        {"DIMJUST", 0},         {"DIMSD1", 0},
       {"DIMSD2", 0},     {"DIMTOLJ", 1},   {"DIMTZIN", 0},         {"DIMALTZ", 0},         {"DIMALTTZ", 0},
       {"DIMUPT", 0},     {"DIMATFIT", 3},  {"DIMFXLON", 0},        {"DIMTXTDIRECTION", 0}, {"DIMALTMZF", 100},
       {"DIMALTMZS", ""}, {"DIMMZF", 100},  {"DIMMZS", ""},         {"DIMLWD", -2},         {"DIMLWE", -2}});
  return d;
}

void dim_vars(ObjWriter& w, const Value& V, bool in_header) {
  const Value& D = dim_defaults_js();
  const auto g = [&](std::string_view k) -> const Value& {
    const Value& v = ref(V, k);
    return v.is_undefined() ? ref(D, k) : v;
  };
  const auto clr = [&](std::string_view k) {
    const Value& c = g(k);
    return c.is_number() ? make_object({{"index", c}}) : c;
  };
  const auto bd = [&](std::string_view k) { w.BD(num(g(k))); };
  const auto b = [&](std::string_view k) { w.B(g(k).truthy()); };
  const auto bs = [&](std::string_view k) { w.BS(iv(g(k))); };
  if (!in_header) {
    w.TV(g("DIMPOST"));
    w.TV(g("DIMAPOST"));
  }
  for (const char* k : {"DIMSCALE", "DIMASZ", "DIMEXO", "DIMDLI", "DIMEXE", "DIMRND", "DIMDLE", "DIMTP", "DIMTM"}) {
    bd(k);
  }
  if (w.r2007) {
    bd("DIMFXL");
    bd("DIMJOGANG");
    bs("DIMTFILL");
    w.CMC(clr("DIMTFILLCLR"));
  }
  for (const char* k : {"DIMTOL", "DIMLIM", "DIMTIH", "DIMTOH", "DIMSE1", "DIMSE2"}) {
    b(k);
  }
  bs("DIMTAD");
  bs("DIMZIN");
  bs("DIMAZIN");
  if (w.r2007) {
    bs("DIMARCSYM");
  }
  for (const char* k :
       {"DIMTXT", "DIMCEN", "DIMTSZ", "DIMALTF", "DIMLFAC", "DIMTVP", "DIMTFAC", "DIMGAP", "DIMALTRND"}) {
    bd(k);
  }
  b("DIMALT");
  bs("DIMALTD");
  for (const char* k : {"DIMTOFL", "DIMSAH", "DIMTIX", "DIMSOXD"}) {
    b(k);
  }
  for (const char* k : {"DIMCLRD", "DIMCLRE", "DIMCLRT"}) {
    w.CMC(clr(k));
  }
  for (const char* k : {"DIMADEC", "DIMDEC", "DIMTDEC", "DIMALTU", "DIMALTTD", "DIMAUNIT", "DIMFRAC", "DIMLUNIT",
                        "DIMDSEP", "DIMTMOVE", "DIMJUST"}) {
    bs(k);
  }
  b("DIMSD1");
  b("DIMSD2");
  for (const char* k : {"DIMTOLJ", "DIMTZIN", "DIMALTZ", "DIMALTTZ"}) {
    bs(k);
  }
  b("DIMUPT");
  bs("DIMATFIT");
  if (w.r2007) {
    b("DIMFXLON");
  }
  if (w.r2010) {
    b("DIMTXTDIRECTION");
    bd("DIMALTMZF");
    w.TV(g("DIMALTMZS"));
    bd("DIMMZF");
    w.TV(g("DIMMZS"));
  }
}

void dimstyle(ObjWriter& w, const Value& r) {
  object_common(w, r);
  table_entry(w, ref(r, "name"));
  const Value& V = orv(ref(r, "vars"), undef());
  dim_vars(w, V, false);
  w.BS(nnum(ref(V, "DIMLWD"), -2));
  w.BS(nnum(ref(V, "DIMLWE"), -2));
  w.B(false);
  w.H(kHardPtr, 0);
  w.H(kHardPtr, ref(r, "txsty"));
  w.H(kHardPtr, ref(r, "ldrblk"));
  w.H(kHardPtr, ref(r, "blk"));
  w.H(kHardPtr, ref(r, "blk1"));
  w.H(kHardPtr, ref(r, "blk2"));
  if (w.r2007) {
    w.H(kHardPtr, ref(r, "dimltype"));
    w.H(kHardPtr, ref(r, "ltex1"));
    w.H(kHardPtr, ref(r, "ltex2"));
  }
}

void dictionary(ObjWriter& w, const Value& r) {
  object_common(w, r);
  const Array& items = arr(r, "items");
  w.BL(static_cast<double>(items.size()));
  w.BS(nnum(ref(r, "cloning"), 1));
  const bool hard = tr(r, "hardOwner");
  w.RC(hard ? 1 : 0);
  for (const Value& it : items) {
    w.TV(it.as_array()[0]);
  }
  for (const Value& it : items) {
    w.H(hard ? kHardOwn : kSoftOwn, it.as_array()[1]);
  }
}

void xrecord(ObjWriter& w, const Value& r) {
  object_common(w, r);
  const ByteBuf b = xrecord_bytes(ref(r, "data"), w.r2007);
  w.BL(static_cast<double>(b.size()));
  for (const std::uint8_t x : b) {
    w.RC(x);
  }
  w.BS(nnum(ref(r, "cloning"), 1));
  for (const Value& h : arr(r, "objids")) {
    w.H(kSoftPtr, h);
  }
}

void mlinestyle(ObjWriter& w, const Value& r) {
  object_common(w, r);
  w.TV(orv(ref(r, "name"), Value("Standard")));
  w.TV(orv(ref(r, "description"), Value("")));
  w.BS(0);
  w.CMC(color_index(256));
  w.BD(kPi / 2);
  w.BD(kPi / 2);
  w.RC(2);
  for (const double off : {0.5, -0.5}) {
    w.BD(off);
    w.CMC(color_index(256));
    if (w.r2018) {
      w.H(kHardPtr, ref(r, "ltByLayer"));
    } else {
      w.BS(32767);
    }
  }
}

void layout(ObjWriter& w, const Value& r) {
  object_common(w, r);
  const Value& p = orv(ref(r, "paper"), undef());
  const bool tab = tr(r, "tab");
  w.TV(orv(get(p, "pageSetup"), Value("")));
  w.TV(orv(get(p, "printer"), Value("none_device")));
  w.BS(nnum(ref(r, "plotFlags"), tab ? 688 : 1712));
  for (const char* m : {"left", "bottom", "right", "top"}) {
    w.BD(numk(p, m, 6.35));
  }
  w.BD(numk(p, "w", 215.9));
  w.BD(numk(p, "h", 279.4));
  w.TV(orv(get(p, "name"), Value("Letter_(8.50_x_11.00_Inches)")));
  w.BD2(0, 0);
  w.BS(nnum(get(p, "units"), tab ? 1 : 0));
  w.BS(nnum(get(p, "rotation"), 1));
  w.BS(nnum(get(p, "plotType"), tab ? 5 : 0));
  w.BD2(0, 0);
  w.BD2(0, 0);
  if (!w.r2004) {
    w.T8(Value(""));
  }
  w.BD(1);
  w.BD(numk(p, "drawingUnits", 1));
  w.TV(orv(get(p, "styleSheet"), Value("")));
  w.BS(nnum(get(p, "scaleType"), tab ? 16 : 0));
  w.BD(numk(p, "scale", 1));
  w.BD2(0, 0);
  if (w.r2004) {
    w.BS(0);
    w.BS(2);
    w.BS(300);
  }
  w.TV(ref(r, "name"));
  w.BL(ivk(r, "tab"));
  w.BS(nnum(ref(r, "flags"), 1));
  w.BD3(0, 0, 0);
  const Value lmin = get(ref(r, "limits"), "min");
  const Value lmax = get(ref(r, "limits"), "max");
  w.RD2(numk(lmin, "x"), numk(lmin, "y"));
  w.RD2(numk(lmax, "x", 12), numk(lmax, "y", 9));
  w.P3(ref(r, "insbase"));
  w.BD3(1, 0, 0);
  w.BD3(0, 1, 0);
  w.BD(0);
  w.BS(0);
  const Value& e = ref(r, "extents");
  if (e.truthy()) {
    w.P3(get(e, "min"));
    w.P3(get(e, "max"));
  } else {
    w.BD3(1e20, 1e20, 1e20);
    w.BD3(-1e20, -1e20, -1e20);
  }
  const Array& vps = arr(r, "viewports");
  if (w.r2004) {
    w.BL(static_cast<double>(vps.size()));
    w.H(kHardPtr, 0);
  }
  if (w.r2007) {
    w.H(kSoftPtr, 0);
  }
  w.H(kSoftPtr, ref(r, "block"));
  w.H(kSoftPtr, ref(r, "lastViewport"));
  w.H(kHardPtr, 0);
  w.H(kHardPtr, 0);
  if (w.r2004) {
    for (const Value& h : vps) {
      w.H(kSoftPtr, h);
    }
  }
}

// ---------------------------------------------------------------- entities

void line(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  const D3 a = P(ref(r, "start"));
  const D3 b = P(ref(r, "end"));
  const bool flat = a.z == 0 && b.z == 0;
  w.B(flat);
  w.RD(a.x);
  w.DD(b.x, a.x);
  w.RD(a.y);
  w.DD(b.y, a.y);
  if (!flat) {
    w.RD(a.z);
    w.DD(b.z, a.z);
  }
  w.BT(numk(r, "thickness"));
  BEx(w, ref(r, "extrusion"));
}

void corners(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  w.BT(numk(r, "thickness"));
  const Array& pts = arr(r, "points");
  const Value& el = ref(r, "elevation");
  if (nullish(el)) {
    w.BD(num(pts.empty() ? undef() : get(pts[0], "z")));
  } else {
    w.BD(num(el));
  }
  for (std::size_t i = 0; i < 4; ++i) {
    const Value& p = i < pts.size() && pts[i].truthy() ? pts[i] : pts.back();
    w.RD2(numk(p, "x"), numk(p, "y"));
  }
  BEx(w, ref(r, "extrusion"));
}

void face3d(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  const Array& pts = arr(r, "points");
  std::array<D3, 4> p{};
  for (std::size_t i = 0; i < 4; ++i) {
    p[i] = P(i < pts.size() && pts[i].truthy() ? pts[i] : pts.back());
  }
  const double flags = ivk(r, "invisibleEdges");
  const bool zz = std::ranges::all_of(p, [](const D3& q) { return q.z == 0; });
  w.B(flags == 0);
  w.B(zz);
  w.RD(p[0].x);
  w.RD(p[0].y);
  if (!zz) {
    w.RD(p[0].z);
  }
  for (std::size_t i = 1; i < 4; ++i) {
    w.DD(p[i].x, p[i - 1].x);
    w.DD(p[i].y, p[i - 1].y);
    w.DD(p[i].z, p[i - 1].z);
  }
  if (flags != 0) {
    w.BS(flags);
  }
}

void text_data(ObjWriter& w, const Value& r) {
  const D3 ins = P(ref(r, "insert"));
  const bool has_al = ref(r, "align").truthy();
  const D3 al = has_al ? P(ref(r, "align")) : D3{};
  const double ha = ivk(r, "hAlign");
  const double va = ivk(r, "vAlign");
  const bool has_align = has_al && (ha != 0 || va != 0);
  const double elev = ins.z;
  const double rot = numk(r, "rotation");
  const double wf = numk(r, "widthFactor", 1);
  const double obl = numk(r, "oblique");
  const double gen = ivk(r, "generation");
  w.RC(static_cast<double>((elev != 0 ? 0 : 1) | (has_align ? 0 : 2) | (obl != 0 ? 0 : 4) | (rot != 0 ? 0 : 8) |
                           (wf != 1 ? 0 : 0x10) | (gen != 0 ? 0 : 0x20) | (ha != 0 ? 0 : 0x40) | (va != 0 ? 0 : 0x80)));
  if (elev != 0) {
    w.RD(elev);
  }
  w.RD2(ins.x, ins.y);
  if (has_align) {
    w.DD(al.x, ins.x);
    w.DD(al.y, ins.y);
  }
  BEx(w, ref(r, "extrusion"));
  w.BT(numk(r, "thickness"));
  if (obl != 0) {
    w.RD(obl);
  }
  if (rot != 0) {
    w.RD(rot);
  }
  w.RD(numk(r, "height", 0.2));
  if (wf != 1) {
    w.RD(wf);
  }
  const Value& v = ref(r, "value");
  w.TV(nullish(v) ? Value("") : v);
  if (gen != 0) {
    w.BS(gen);
  }
  if (ha != 0) {
    w.BS(ha);
  }
  if (va != 0) {
    w.BS(va);
  }
}

void attrib_tail(ObjWriter& w, const Value& r) {
  if (w.r2010) {
    w.RC(0);
  }
  if (w.r2018) {
    w.RC(1);
  }
  w.TV(ref(r, "tag"));
  w.BS(0);
  w.RC(ivk(r, "flags"));
  if (w.r2007) {
    w.B(tr(r, "lockPosition"));
  }
}

void mtext(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  const D3 ins = P(ref(r, "insert"));
  D3 xa;
  if (ref(r, "xAxis").truthy()) {
    xa = P(ref(r, "xAxis"));
  } else {
    xa = {cc::base::js_cos(numk(r, "rotation")), cc::base::js_sin(numk(r, "rotation")), 0};
  }
  w.BD3(ins.x, ins.y, ins.z);
  ext3(w, ref(r, "extrusion"));
  w.BD3(xa.x, xa.y, xa.z);
  w.BD(numk(r, "width"));
  if (w.r2007) {
    w.BD(numk(r, "rectHeight"));
  }
  w.BD(numk(r, "height", 0.2));
  const auto or1 = [&](std::string_view k, double d) {
    const Value& v = ref(r, k);
    return v.truthy() ? base::js_to_number(v) : d;
  };
  w.BS(or1("attach", 1));
  w.BS(or1("direction", 1));
  w.BD(numk(r, "extHeight"));
  w.BD(numk(r, "extWidth"));
  const Value& v = ref(r, "value");
  w.TV(nullish(v) ? Value("") : v);
  w.H(kHardPtr, ref(r, "style"));
  w.BS(or1("lineSpacingStyle", 1));
  w.BD(numk(r, "lineSpacing", 1));
  w.B(false);
  if (w.r2004) {
    const int bg = base::js_to_int32(ivk(r, "backgroundFlags"));
    w.BL(bg);
    if ((bg & 1) != 0 || (w.r2018 && (bg & 0x10) != 0)) {
      w.BD(numk(r, "backgroundScale", 1.5));
      w.CMC(orv(ref(r, "backgroundColor"), color_index(7)));
      w.BL(ivk(r, "backgroundTransparency"));
    }
  }
  if (w.r2018) {
    w.B(true);
    w.BS(4);
    w.B(true);
    w.H(kHardPtr, 0);
    w.BL(or1("attach", 1));
    w.BD3(xa.x, xa.y, xa.z);
    w.BD3(ins.x, ins.y, ins.z);
    w.BD(numk(r, "width"));
    w.BD(numk(r, "rectHeight"));
    w.BD(numk(r, "extWidth"));
    w.BD(numk(r, "extHeight"));
    w.BS(0);
  }
}

void insert_scale(ObjWriter& w, const Value& s) {
  const double sx = numk(s, "x", 1);
  const double sy = numk(s, "y", 1);
  const double sz = numk(s, "z", 1);
  if (sx == 1 && sy == 1 && sz == 1) {
    w.BB(3);
  } else if (sx == sy && sx == sz) {
    w.BB(2);
    w.RD(sx);
  } else if (sx == 1) {
    w.BB(1);
    w.DD(sy, 1);
    w.DD(sz, 1);
  } else {
    w.BB(0);
    w.RD(sx);
    w.DD(sy, sx);
    w.DD(sz, sx);
  }
}
void insert_tail(ObjWriter& w, const Value& r) {
  const Array& at = arr(r, "attribs");
  if (!at.empty()) {
    if (!w.r2004) {
      w.H(kSoftPtr, at.front());
      w.H(kSoftPtr, at.back());
    } else {
      for (const Value& h : at) {
        w.H(kHardOwn, h);
      }
    }
    w.H(kHardOwn, ref(r, "seqend"));
  }
}
void insert_head(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  w.P3(ref(r, "insert"));
  insert_scale(w, ref(r, "scale"));
  w.BD(numk(r, "rotation"));
  ext3(w, ref(r, "extrusion"));
  const std::size_t na = arr(r, "attribs").size();
  w.B(na != 0);
  if (w.r2004 && na != 0) {
    w.BL(static_cast<double>(na));
  }
}

void poly_owned(ObjWriter& w, const Value& r) {
  const Array& v = arr(r, "vertices");
  if (w.r2004) {
    for (const Value& h : v) {
      w.H(kHardOwn, h);
    }
  } else {
    w.H(kSoftPtr, v.empty() ? Value(0) : v.front());
    w.H(kSoftPtr, v.empty() ? Value(0) : v.back());
  }
  w.H(kHardOwn, ref(r, "seqend"));
}

void lwpolyline(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  const Array& v = arr(r, "vertices");
  const bool bulges = std::ranges::any_of(v, [](const Value& p) { return numk(p, "bulge") != 0; });
  const bool widths = !tr(r, "constantWidth") && std::ranges::any_of(v, [](const Value& p) {
    return numk(p, "startWidth") != 0 || numk(p, "endWidth") != 0;
  });
  const bool ids = w.r2010 && std::ranges::any_of(v, [](const Value& p) { return tr(p, "id"); });
  const double elev = numk(r, "elevation");
  const double th = numk(r, "thickness");
  const double cw = numk(r, "constantWidth");
  const bool ex = !default_extrusion(ref(r, "extrusion"));
  const int flags = (ex ? 1 : 0) | (th != 0 ? 2 : 0) | (cw != 0 ? 4 : 0) | (elev != 0 ? 8 : 0) | (bulges ? 16 : 0) |
                    (widths ? 32 : 0) | (tr(r, "plinegen") ? 256 : 0) | (tr(r, "closed") ? 512 : 0) | (ids ? 1024 : 0);
  w.BS(flags);
  if (cw != 0) {
    w.BD(cw);
  }
  if (elev != 0) {
    w.BD(elev);
  }
  if (th != 0) {
    w.BD(th);
  }
  if (ex) {
    const Value& e = ref(r, "extrusion");
    w.BD3(numk(e, "x"), numk(e, "y"), numk(e, "z", 1));
  }
  const auto n = static_cast<double>(v.size());
  w.BL(n);
  if (bulges) {
    w.BL(n);
  }
  if (ids) {
    w.BL(n);
  }
  if (widths) {
    w.BL(n);
  }
  w.RD2(numk(v[0], "x"), numk(v[0], "y"));
  for (std::size_t k = 1; k < v.size(); ++k) {
    w.DD(numk(v[k], "x"), numk(v[k - 1], "x"));
    w.DD(numk(v[k], "y"), numk(v[k - 1], "y"));
  }
  if (bulges) {
    for (const Value& p : v) {
      w.BD(numk(p, "bulge"));
    }
  }
  if (ids) {
    for (const Value& p : v) {
      w.BL(ivk(p, "id"));
    }
  }
  if (widths) {
    for (const Value& p : v) {
      w.BD(numk(p, "startWidth"));
      w.BD(numk(p, "endWidth"));
    }
  }
}

bool weight_not_one(const Value& wv) {
  return !wv.is_undefined() && !is(wv, 1);
}

void spline(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  const Array& fit = arr(r, "fit");
  const Array& ctrl = arr(r, "control");
  const Array& knots = arr(r, "knots");
  const bool use_fit = fit.size() >= 2 && ctrl.empty();
  const int fl = base::js_to_int32(ivk(r, "flags"));
  const bool closed = (fl & 1) != 0;
  if (w.r2013) {
    w.BL(1);
    const int closed_bit = closed ? 4 : 0;
    w.BL(use_fit ? (9 | closed_bit) : closed_bit);
    const Value& kp = ref(r, "knotParam");
    const double k = base::js_to_number(kp);
    const double kparam = !nullish(kp) && k >= 0 && k <= 2 ? k : 0;
    w.BL(use_fit ? kparam : 15);
  } else {
    w.BL(use_fit ? 2 : 1);
  }
  const Value& deg = ref(r, "degree");
  w.BL(deg.truthy() ? base::js_to_number(deg) : 3);
  const Array& weights_arr = arr(r, "weights");
  const bool weights = std::ranges::any_of(ctrl, [](const Value& p) { return weight_not_one(get(p, "w")); }) ||
                       std::ranges::any_of(weights_arr, [](const Value& x) { return !is(x, 1); });
  if (use_fit) {
    w.BD(numk(r, "fitTolerance"));
    w.P3(ref(r, "startTangent"));
    w.P3(ref(r, "endTangent"));
    w.BL(static_cast<double>(fit.size()));
    for (const Value& p : fit) {
      w.P3(p);
    }
  } else {
    w.B(weights);
    w.B(closed);
    w.B((fl & 2) != 0);
    w.BD(numk(r, "knotTolerance", 1e-10));
    w.BD(numk(r, "controlTolerance", 1e-10));
    w.BL(static_cast<double>(knots.size()));
    w.BL(static_cast<double>(ctrl.size()));
    w.B(weights);
    for (const Value& k : knots) {
      w.BD(num(k));
    }
    for (std::size_t i = 0; i < ctrl.size(); ++i) {
      w.P3(ctrl[i]);
      if (weights) {
        const Value pw = get(ctrl[i], "w");
        if (nullish(pw)) {
          w.BD(num(i < weights_arr.size() ? weights_arr[i] : undef(), 1));
        } else {
          w.BD(num(pw, 1));
        }
      }
    }
  }
}

void hatch(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  if (w.r2004) {
    const Value& g = ref(r, "gradient");
    w.BL(g.truthy() ? 1 : 0);
    w.BL(0);
    w.BD(numk(g, "angle"));
    w.BD(numk(g, "shift"));
    w.BL(tr(g, "single") ? 1 : 0);
    w.BD(numk(g, "tint"));
    const Array& cols = arr(g, "colors");
    w.BL(static_cast<double>(cols.size()));
    for (const Value& c : cols) {
      w.BD(numk(c, "value"));
      w.BS(0);
      const int rgb = base::js_to_int32(numk(c, "rgb")) & 0xFFFFFF;
      w.BL(static_cast<double>(base::js_to_int32(static_cast<double>(0xC2000000U | static_cast<std::uint32_t>(rgb)))));
      w.RC(0);
    }
    w.TV(orv(get(g, "name"), Value("")));
  }
  w.BD(numk(r, "elevation"));
  ext3(w, ref(r, "extrusion"));
  const bool solid = tr(r, "solid");
  w.TV(orv(ref(r, "pattern"), Value(solid ? "SOLID" : "ANSI31")));
  w.B(solid);
  w.B(false);
  const Array& loops = arr(r, "loops");
  w.BL(static_cast<double>(loops.size()));
  bool pixel = false;
  for (const Value& l : loops) {
    const bool poly = tr(l, "polyline");
    const int flag = (base::js_to_int32(ivk(l, "flags")) & ~2 & ~0x200) | (poly ? 2 : 0);
    if ((flag & 4) != 0) {
      pixel = true;
    }
    w.BL(flag);
    if (!poly) {
      const Array& edges = arr(l, "edges");
      w.BL(static_cast<double>(edges.size()));
      for (const Value& e : edges) {
        const Value& ty = ref(e, "type");
        Value t = ty;
        if (ty.is_string()) {
          const std::string& s = ty.as_string();
          if (s == "line") {
            t = 1;
          } else if (s == "arc") {
            t = 2;
          } else if (s == "ellipse") {
            t = 3;
          } else if (s == "spline") {
            t = 4;
          }
        }
        w.RC(base::js_to_number(t));
        if (is(t, 1)) {
          w.RD2(numk(ref(e, "start"), "x"), numk(ref(e, "start"), "y"));
          w.RD2(numk(ref(e, "end"), "x"), numk(ref(e, "end"), "y"));
        } else if (is(t, 2)) {
          w.RD2(numk(ref(e, "center"), "x"), numk(ref(e, "center"), "y"));
          w.BD(numk(e, "radius"));
          w.BD(numk(e, "startAngle"));
          w.BD(numk(e, "endAngle"));
          w.B(!is_false(ref(e, "ccw")));
        } else if (is(t, 3)) {
          w.RD2(numk(ref(e, "center"), "x"), numk(ref(e, "center"), "y"));
          w.RD2(numk(ref(e, "majorAxis"), "x"), numk(ref(e, "majorAxis"), "y"));
          w.BD(numk(e, "ratio", 1));
          w.BD(numk(e, "startAngle"));
          w.BD(numk(e, "endAngle"));
          w.B(!is_false(ref(e, "ccw")));
        } else if (is(t, 4)) {
          const Array& ctl = arr(e, "control");
          const Array& kn = arr(e, "knots");
          const bool rational = std::ranges::any_of(ctl, [](const Value& p) { return weight_not_one(get(p, "w")); });
          const Value& deg = ref(e, "degree");
          w.BL(deg.truthy() ? base::js_to_number(deg) : 3);
          w.B(rational);
          w.B(tr(e, "periodic"));
          w.BL(static_cast<double>(kn.size()));
          w.BL(static_cast<double>(ctl.size()));
          for (const Value& k : kn) {
            w.BD(num(k));
          }
          for (const Value& p : ctl) {
            w.RD2(numk(p, "x"), numk(p, "y"));
            if (rational) {
              w.BD(numk(p, "w", 1));
            }
          }
          if (w.r2010) {
            const Array& fit = arr(e, "fit");
            w.BL(static_cast<double>(fit.size()));
            if (!fit.empty()) {
              for (const Value& p : fit) {
                w.RD2(numk(p, "x"), numk(p, "y"));
              }
              w.RD2(numk(ref(e, "startTangent"), "x"), numk(ref(e, "startTangent"), "y"));
              w.RD2(numk(ref(e, "endTangent"), "x"), numk(ref(e, "endTangent"), "y"));
            }
          }
        }
      }
    } else {
      const Value& pl = ref(l, "polyline");
      const Array& pv = arr(pl, "vertices");
      const bool has_b = std::ranges::any_of(pv, [](const Value& p) { return numk(p, "bulge") != 0; });
      w.B(has_b);
      w.B(!is_false(ref(pl, "closed")));
      w.BL(static_cast<double>(pv.size()));
      for (const Value& p : pv) {
        w.RD2(numk(p, "x"), numk(p, "y"));
        if (has_b) {
          w.BD(numk(p, "bulge"));
        }
      }
    }
    w.BL(0);
  }
  w.BS(ivk(r, "style"));
  w.BS(nnum(ref(r, "patternType"), 1));
  if (!solid) {
    w.BD(numk(r, "angle"));
    w.BD(numk(r, "scale", 1));
    w.B(tr(r, "double"));
    const Array& lines = arr(r, "lines");
    w.BS(static_cast<double>(lines.size()));
    for (const Value& ln : lines) {
      w.BD(numk(ln, "angle"));
      w.BD2(numk(ref(ln, "base"), "x"), numk(ref(ln, "base"), "y"));
      w.BD2(numk(ref(ln, "offset"), "x"), numk(ref(ln, "offset"), "y"));
      const Array& dashes = arr(ln, "dashes");
      w.BS(static_cast<double>(dashes.size()));
      for (const Value& d : dashes) {
        w.BD(num(d));
      }
    }
  }
  if (pixel) {
    w.BD(numk(r, "pixelSize", 1));
  }
  const Array& seeds = arr(r, "seeds");
  w.BL(static_cast<double>(seeds.size()));
  for (const Value& s : seeds) {
    w.RD2(numk(s, "x"), numk(s, "y"));
  }
}

void dim_common(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  if (w.r2010) {
    w.RC(0);
  }
  ext3(w, ref(r, "extrusion"));
  const Value& tm = ref(r, "textMid");
  w.RD2(numk(tm, "x"), numk(tm, "y"));
  const Value& el = ref(r, "elevation");
  w.BD(num(nullish(el) ? get(tm, "z") : el));
  const Value& f1 = ref(r, "flags1");
  if (nullish(f1)) {
    w.RC(tr(r, "textMidUser") ? 0 : 1);
  } else {
    w.RC(base::js_to_number(f1));
  }
  const Value& text = ref(r, "text");
  w.TV(nullish(text) ? Value("") : text);
  w.BD(numk(r, "textRotation"));
  w.BD(numk(r, "horizDir"));
  w.P3(orv(ref(r, "insScale"), unit3(3)));
  w.BD(numk(r, "insRotation"));
  const auto or1 = [&](std::string_view k, double d) {
    const Value& v = ref(r, k);
    return v.truthy() ? base::js_to_number(v) : d;
  };
  w.BS(or1("attach", 5));
  w.BS(or1("lineSpacingStyle", 1));
  w.BD(numk(r, "lineSpacing", 1));
  w.BD(numk(r, "measurement"));
  if (w.r2007) {
    w.B(false);
    w.B(tr(r, "flipArrow1"));
    w.B(tr(r, "flipArrow2"));
  }
  const Value& ins = ref(r, "insert");
  w.RD2(numk(ins, "x"), numk(ins, "y"));
}
void dim_handles(ObjWriter& w, const Value& r) {
  w.H(kHardPtr, ref(r, "dimstyle"));
  w.H(kHardPtr, ref(r, "block"));
}

void image_entity(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  w.BL(ivk(r, "classVersion"));
  w.P3(ref(r, "insert"));
  w.P3(ref(r, "u"));
  w.P3(ref(r, "v"));
  w.RD2(numk(ref(r, "size"), "x", 1), numk(ref(r, "size"), "y", 1));
  w.BS(nnum(ref(r, "displayFlags"), 7));
  w.B(tr(r, "clipping"));
  w.RC(nnum(ref(r, "brightness"), 50));
  w.RC(nnum(ref(r, "contrast"), 50));
  w.RC(ivk(r, "fade"));
  if (w.r2010) {
    w.B(tr(r, "clipMode"));
  }
  const Array& clip = arr(r, "clip");
  const int type = is(ref(r, "clipType"), 1) && clip.size() >= 2 ? 1 : 2;
  w.BS(type);
  if (type == 1) {
    w.RD2(numk(clip[0], "x"), numk(clip[0], "y"));
    w.RD2(numk(clip[1], "x"), numk(clip[1], "y"));
  } else {
    w.BL(static_cast<double>(clip.size()));
    for (const Value& p : clip) {
      w.RD2(numk(p, "x"), numk(p, "y"));
    }
  }
  w.H(kHardPtr, ref(r, "def"));
  w.H(kHardOwn, ref(r, "defReactor"));
}

void viewport(ObjWriter& w, const Value& r) {
  entity_common(w, r);
  w.P3(ref(r, "center"));
  w.BD(numk(r, "width", 1));
  w.BD(numk(r, "height", 1));
  w.P3(ref(r, "viewTarget"));
  ext3(w, orv(ref(r, "viewDirection"), unit3(2)));
  w.BD(numk(r, "twist"));
  w.BD(numk(r, "viewHeight", numk(r, "height", 1)));
  w.BD(numk(r, "lens", 50));
  w.BD(0);
  w.BD(0);
  w.BD(0);
  w.RD2(numk(ref(r, "viewCenter"), "x"), numk(ref(r, "viewCenter"), "y"));
  w.RD2(0, 0);
  w.RD2(10, 10);
  w.RD2(10, 10);
  w.BS(1000);
  if (w.r2007) {
    w.BS(5);
  }
  const Array& frozen = arr(r, "frozenLayers");
  w.BL(static_cast<double>(frozen.size()));
  w.BL(nnum(ref(r, "status"), 0x8060));
  w.TV(Value(""));
  w.RC(0);
  w.B(false);
  w.B(false);
  w.BD3(0, 0, 0);
  w.BD3(1, 0, 0);
  w.BD3(0, 1, 0);
  w.BD(0);
  w.BS(0);
  if (w.r2004) {
    w.BS(0);
  }
  if (w.r2007) {
    w.B(true);
    w.RC(1);
    w.BD(0);
    w.BD(0);
    w.CMC(color_index(250));
  }
  for (const Value& h : frozen) {
    w.H(w.r2004 ? kSoftPtr : kHardPtr, h);
  }
  w.H(kSoftPtr, 0);
  if (!w.r2004) {
    w.H(kHardPtr, 0);
  }
  w.H(kHardPtr, 0);
  w.H(kHardPtr, 0);
  if (w.r2007) {
    w.H(kSoftPtr, 0);
    w.H(kHardPtr, 0);
    w.H(kSoftPtr, 0);
    w.H(kHardOwn, 0);
  }
}

using CEncFn = void (*)(ObjWriter&, const Value&);

std::unordered_map<std::string, EncFn>& extra_encoders() {
  static std::unordered_map<std::string, EncFn> m;
  return m;
}

const std::unordered_map<std::string_view, CEncFn>& encoders() {
  static const std::unordered_map<std::string_view, CEncFn> m = [] {
    std::unordered_map<std::string_view, CEncFn> e;
    for (const char* k : {"BLOCK_CONTROL", "LAYER_CONTROL", "STYLE_CONTROL", "LTYPE_CONTROL", "VIEW_CONTROL",
                          "UCS_CONTROL", "VPORT_CONTROL", "APPID_CONTROL", "DIMSTYLE_CONTROL", "VPENT_CONTROL"}) {
      e[k] = control;
    }
    e["BLOCK_HEADER"] = block_header;
    e["LAYER"] = layer;
    e["STYLE"] = style;
    e["LTYPE"] = ltype;
    e["UCS"] = ucs;
    e["VPORT"] = vport;
    e["VIEW"] = view;
    e["APPID"] = appid;
    e["DIMSTYLE"] = dimstyle;
    e["DICTIONARY"] = dictionary;
    e["DICTIONARYWDFLT"] = [](ObjWriter& w, const Value& r) {
      dictionary(w, r);
      w.H(kHardPtr, ref(r, "defaultEntry"));
    };
    e["PLACEHOLDER"] = [](ObjWriter& w, const Value& r) { object_common(w, r); };
    e["XRECORD"] = xrecord;
    e["MLINESTYLE"] = mlinestyle;
    e["LAYOUT"] = layout;
    e["BLOCK"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.TV(ref(r, "name"));
    };
    e["ENDBLK"] = [](ObjWriter& w, const Value& r) { entity_common(w, r); };
    e["SEQEND"] = [](ObjWriter& w, const Value& r) { entity_common(w, r); };
    e["LINE"] = line;
    e["POINT"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "position"));
      w.BT(numk(r, "thickness"));
      BEx(w, ref(r, "extrusion"));
      w.BD(numk(r, "xAxisAngle"));
    };
    e["CIRCLE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "center"));
      w.BD(numk(r, "radius"));
      w.BT(numk(r, "thickness"));
      BEx(w, ref(r, "extrusion"));
    };
    e["ARC"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "center"));
      w.BD(numk(r, "radius"));
      w.BT(numk(r, "thickness"));
      BEx(w, ref(r, "extrusion"));
      w.BD(rad(ref(r, "startAngle")));
      w.BD(rad(ref(r, "endAngle")));
    };
    e["ELLIPSE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "center"));
      w.P3(ref(r, "majorAxis"));
      ext3(w, ref(r, "extrusion"));
      w.BD(numk(r, "ratio", 1));
      w.BD(numk(r, "startParam"));
      w.BD(numk(r, "endParam", 2 * kPi));
    };
    e["RAY"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "start"));
      w.P3(ref(r, "direction"));
    };
    e["XLINE"] = e["RAY"];
    e["SOLID"] = corners;
    e["TRACE"] = corners;
    e["3DFACE"] = face3d;
    e["TEXT"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      text_data(w, r);
      w.H(kHardPtr, ref(r, "style"));
    };
    e["ATTRIB"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      text_data(w, r);
      attrib_tail(w, r);
      w.H(kHardPtr, ref(r, "style"));
    };
    e["ATTDEF"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      text_data(w, r);
      attrib_tail(w, r);
      if (w.r2010) {
        w.RC(0);
      }
      const Value& p = ref(r, "prompt");
      w.TV(nullish(p) ? Value("") : p);
      w.H(kHardPtr, ref(r, "style"));
    };
    e["MTEXT"] = mtext;
    e["INSERT"] = [](ObjWriter& w, const Value& r) {
      insert_head(w, r);
      w.H(kHardPtr, ref(r, "block"));
      insert_tail(w, r);
    };
    e["MINSERT"] = [](ObjWriter& w, const Value& r) {
      insert_head(w, r);
      const Value& c = ref(r, "cols");
      const Value& rw = ref(r, "rows");
      w.BS(c.truthy() ? base::js_to_number(c) : 1);
      w.BS(rw.truthy() ? base::js_to_number(rw) : 1);
      w.BD(numk(r, "colSpacing"));
      w.BD(numk(r, "rowSpacing"));
      w.H(kHardPtr, ref(r, "block"));
      insert_tail(w, r);
    };
    e["POLYLINE_2D"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.BS(ivk(r, "flags"));
      w.BS(ivk(r, "curveType"));
      w.BD(numk(r, "startWidth"));
      w.BD(numk(r, "endWidth"));
      w.BT(numk(r, "thickness"));
      w.BD(numk(r, "elevation"));
      BEx(w, ref(r, "extrusion"));
      if (w.r2004) {
        w.BL(static_cast<double>(arr(r, "vertices").size()));
      }
      poly_owned(w, r);
    };
    e["POLYLINE_3D"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.RC(ivk(r, "splineFlags"));
      w.RC(tr(r, "closed") ? 1 : 0);
      if (w.r2004) {
        w.BL(static_cast<double>(arr(r, "vertices").size()));
      }
      poly_owned(w, r);
    };
    e["POLYLINE_PFACE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.BS(ivk(r, "numVerts"));
      w.BS(ivk(r, "numFaces"));
      if (w.r2004) {
        w.BL(static_cast<double>(arr(r, "vertices").size()));
      }
      poly_owned(w, r);
    };
    e["POLYLINE_MESH"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      for (const char* k : {"flags", "curveType", "m", "n", "mDensity", "nDensity"}) {
        w.BS(ivk(r, k));
      }
      if (w.r2004) {
        w.BL(static_cast<double>(arr(r, "vertices").size()));
      }
      poly_owned(w, r);
    };
    e["VERTEX_2D"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.RC(ivk(r, "flags"));
      const Value& pt = ref(r, "point");
      w.P3(make_object({{"x", get(pt, "x")}, {"y", get(pt, "y")}, {"z", 0}}));
      const double sw = numk(r, "startWidth");
      const double ew = numk(r, "endWidth", sw);
      if (sw != 0 && sw == ew) {
        w.BD(-std::abs(sw));
      } else {
        w.BD(sw);
        w.BD(ew);
      }
      w.BD(numk(r, "bulge"));
      if (w.r2010) {
        w.BL(ivk(r, "id"));
      }
      w.BD(numk(r, "tangent"));
    };
    e["VERTEX_3D"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.RC(ivk(r, "flags"));
      w.P3(ref(r, "point"));
    };
    e["VERTEX_MESH"] = e["VERTEX_3D"];
    e["VERTEX_PFACE"] = e["VERTEX_3D"];
    e["VERTEX_PFACE_FACE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      const Array& ix = arr(r, "indices");
      for (std::size_t i = 0; i < 4; ++i) {
        w.BS(i < ix.size() ? iv(ix[i]) : 0);
      }
    };
    e["LWPOLYLINE"] = lwpolyline;
    e["SPLINE"] = spline;
    e["HATCH"] = hatch;
    e["DIMENSION_ORDINATE"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "defPoint"));
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      w.RC(tr(r, "ordinateX") ? 1 : 0);
      dim_handles(w, r);
    };
    e["DIMENSION_LINEAR"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      w.P3(ref(r, "defPoint"));
      w.BD(numk(r, "obliqueAngle"));
      w.BD(numk(r, "angle"));
      dim_handles(w, r);
    };
    e["DIMENSION_ALIGNED"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      w.P3(ref(r, "defPoint"));
      w.BD(numk(r, "obliqueAngle"));
      dim_handles(w, r);
    };
    e["DIMENSION_ANG3PT"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "defPoint"));
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      w.P3(ref(r, "center"));
      dim_handles(w, r);
    };
    e["DIMENSION_ANG2LN"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.RD2(numk(ref(r, "arcPoint"), "x"), numk(ref(r, "arcPoint"), "y"));
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      const Value& xe = ref(r, "xline1End");
      w.P3(nullish(xe) ? ref(r, "center") : xe);
      w.P3(ref(r, "defPoint"));
      dim_handles(w, r);
    };
    e["DIMENSION_RADIUS"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "defPoint"));
      w.P3(ref(r, "chordPoint"));
      w.BD(numk(r, "leaderLen"));
      dim_handles(w, r);
    };
    e["DIMENSION_DIAMETER"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "chordPoint"));
      w.P3(ref(r, "defPoint"));
      w.BD(numk(r, "leaderLen"));
      dim_handles(w, r);
    };
    e["ARC_DIMENSION"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "defPoint"));
      w.P3(ref(r, "xline1"));
      w.P3(ref(r, "xline2"));
      w.P3(ref(r, "center"));
      w.B(tr(r, "isPartial"));
      w.BD(numk(r, "arcStart"));
      w.BD(numk(r, "arcEnd"));
      w.B(tr(r, "hasLeader"));
      w.P3(ref(r, "leader1"));
      w.P3(ref(r, "leader2"));
      dim_handles(w, r);
    };
    e["LARGE_RADIAL_DIMENSION"] = [](ObjWriter& w, const Value& r) {
      dim_common(w, r);
      w.P3(ref(r, "defPoint"));
      w.P3(ref(r, "chordPoint"));
      w.BD(0);
      w.P3(ref(r, "overrideCenter"));
      w.P3(ref(r, "jogPoint"));
      dim_handles(w, r);
    };
    e["IMAGE"] = image_entity;
    e["WIPEOUT"] = image_entity;
    e["IMAGEDEF"] = [](ObjWriter& w, const Value& r) {
      object_common(w, r);
      w.BL(ivk(r, "classVersion"));
      w.RD2(numk(ref(r, "size"), "x", 1), numk(ref(r, "size"), "y", 1));
      w.TV(orv(ref(r, "fileName"), Value("")));
      w.B(!is_false(ref(r, "loaded")));
      w.RC(ivk(r, "resUnits"));
      w.RD2(numk(ref(r, "pixelSize"), "x", 1), numk(ref(r, "pixelSize"), "y", 1));
    };
    e["IMAGEDEF_REACTOR"] = [](ObjWriter& w, const Value& r) {
      object_common(w, r);
      w.BL(nnum(ref(r, "classVersion"), 2));
    };
    e["RASTERVARIABLES"] = [](ObjWriter& w, const Value& r) {
      object_common(w, r);
      w.BL(0);
      w.BS(nnum(ref(r, "frame"), 1));
      w.BS(nnum(ref(r, "quality"), 1));
      w.BS(ivk(r, "units"));
    };
    e["WIPEOUTVARIABLES"] = [](ObjWriter& w, const Value& r) {
      object_common(w, r);
      w.BS(nnum(ref(r, "frame"), 1));
    };
    e["LEADER"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.B(false);
      w.BS(nnum(ref(r, "annotType"), 3));
      w.BS(ivk(r, "pathType"));
      const Array& v = arr(r, "vertices");
      w.BL(static_cast<double>(v.size()));
      for (const Value& p : v) {
        w.P3(p);
      }
      w.P3(orv(ref(r, "origin"), v.empty() ? undef() : v[0]));
      ext3(w, ref(r, "extrusion"));
      w.P3(orv(ref(r, "xDirection"), unit3(0)));
      w.P3(ref(r, "blockOffset"));
      w.P3(ref(r, "endPointProj"));
      if (!w.r2010) {
        w.BD(numk(r, "boxHeight"));
        w.BD(numk(r, "boxWidth"));
      }
      w.B(tr(r, "hookLineOnX"));
      w.B(!is_false(ref(r, "arrowhead")));
      w.BS(0);
      w.B(false);
      w.B(false);
      w.H(kHardPtr, ref(r, "annotation"));
      w.H(kHardPtr, ref(r, "dimstyle"));
    };
    e["TOLERANCE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "insert"));
      w.P3(orv(ref(r, "xDirection"), unit3(0)));
      ext3(w, ref(r, "extrusion"));
      const Value& t = ref(r, "text");
      w.TV(nullish(t) ? Value("") : t);
      w.H(kHardPtr, ref(r, "dimstyle"));
    };
    e["MLINE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.BD(numk(r, "scale", 1));
      w.RC(ivk(r, "justification"));
      const Array& vs = arr(r, "vertices");
      w.P3(orv(ref(r, "base"), vs.empty() ? undef() : ref(vs[0], "point")));
      ext3(w, ref(r, "extrusion"));
      w.BS(tr(r, "closed") ? 3 : 1);
      const double nl = base::js_to_number(ref(r, "lines"));
      w.RC(nl);
      w.BS(static_cast<double>(vs.size()));
      for (const Value& v : vs) {
        w.P3(ref(v, "point"));
        w.P3(ref(v, "direction"));
        w.P3(ref(v, "miter"));
        const Array& ls = arr(v, "lines");
        for (std::size_t k = 0; static_cast<double>(k) < nl; ++k) {
          const Value& l = k < ls.size() ? ls[k] : undef();
          const Array& seg = arr(l, "segments");
          w.BS(static_cast<double>(seg.size()));
          for (const Value& x : seg) {
            w.BD(num(x));
          }
          const Array& fill = arr(l, "fill");
          w.BS(static_cast<double>(fill.size()));
          for (const Value& x : fill) {
            w.BD(num(x));
          }
        }
      }
      w.H(kHardPtr, ref(r, "style"));
    };
    e["SHAPE"] = [](ObjWriter& w, const Value& r) {
      entity_common(w, r);
      w.P3(ref(r, "insert"));
      w.BD(numk(r, "scale", 1));
      w.BD(numk(r, "rotation"));
      w.BD(numk(r, "widthFactor", 1));
      w.BD(numk(r, "oblique"));
      w.BD(numk(r, "thickness"));
      w.BS(ivk(r, "shapeNumber"));
      ext3(w, ref(r, "extrusion"));
      w.H(kHardPtr, ref(r, "style"));
    };
    e["VIEWPORT"] = viewport;
    return e;
  }();
  return m;
}

const std::unordered_map<std::string_view, double>& type_numbers() {
  static const std::unordered_map<std::string_view, double> t = {
      {"TEXT", 1},
      {"ATTRIB", 2},
      {"ATTDEF", 3},
      {"BLOCK", 4},
      {"ENDBLK", 5},
      {"SEQEND", 6},
      {"INSERT", 7},
      {"MINSERT", 8},
      {"VERTEX_2D", 10},
      {"VERTEX_3D", 11},
      {"VERTEX_MESH", 12},
      {"VERTEX_PFACE", 13},
      {"VERTEX_PFACE_FACE", 14},
      {"POLYLINE_2D", 15},
      {"POLYLINE_3D", 16},
      {"ARC", 17},
      {"CIRCLE", 18},
      {"LINE", 19},
      {"DIMENSION_ORDINATE", 20},
      {"DIMENSION_LINEAR", 21},
      {"DIMENSION_ALIGNED", 22},
      {"DIMENSION_ANG3PT", 23},
      {"DIMENSION_ANG2LN", 24},
      {"DIMENSION_RADIUS", 25},
      {"DIMENSION_DIAMETER", 26},
      {"POINT", 27},
      {"3DFACE", 28},
      {"POLYLINE_PFACE", 29},
      {"POLYLINE_MESH", 30},
      {"SOLID", 31},
      {"TRACE", 32},
      {"SHAPE", 33},
      {"VIEWPORT", 34},
      {"ELLIPSE", 35},
      {"SPLINE", 36},
      {"REGION", 37},
      {"3DSOLID", 38},
      {"BODY", 39},
      {"RAY", 40},
      {"XLINE", 41},
      {"DICTIONARY", 42},
      {"MTEXT", 44},
      {"LEADER", 45},
      {"TOLERANCE", 46},
      {"MLINE", 47},
      {"BLOCK_CONTROL", 48},
      {"BLOCK_HEADER", 49},
      {"LAYER_CONTROL", 50},
      {"LAYER", 51},
      {"STYLE_CONTROL", 52},
      {"STYLE", 53},
      {"LTYPE_CONTROL", 56},
      {"LTYPE", 57},
      {"VIEW_CONTROL", 60},
      {"VIEW", 61},
      {"UCS_CONTROL", 62},
      {"UCS", 63},
      {"VPORT_CONTROL", 64},
      {"VPORT", 65},
      {"APPID_CONTROL", 66},
      {"APPID", 67},
      {"DIMSTYLE_CONTROL", 68},
      {"DIMSTYLE", 69},
      {"VPENT_CONTROL", 70},
      {"VPENT", 71},
      {"GROUP", 72},
      {"MLINESTYLE", 73},
      {"LWPOLYLINE", 77},
      {"HATCH", 78},
      {"XRECORD", 79},
      {"PLACEHOLDER", 80},
      {"LAYOUT", 82},
  };
  return t;
}

}  // namespace

// ---------------------------------------------------------------- classes, ObjWriter, framing

std::map<std::string, ClassDef, std::less<>> base_classed() {
  return {
      {"LAYOUT", {"LAYOUT", "AcDbLayout", "ObjectDBX Classes", false, 0}},
      {"DICTIONARYWDFLT", {"ACDBDICTIONARYWDFLT", "AcDbDictionaryWithDefault", "ObjectDBX Classes", false, 0}},
      {"PLACEHOLDER", {"ACDBPLACEHOLDER", "AcDbPlaceHolder", "ObjectDBX Classes", false, 0}},
      {"XRECORD", {"XRECORD", "AcDbXrecord", "ObjectDBX Classes", false, 0}},
      {"ARC_DIMENSION", {"ARC_DIMENSION", "AcDbArcDimension", "ObjectDBX Classes", true, 0x401}},
      {"LARGE_RADIAL_DIMENSION",
       {"LARGE_RADIAL_DIMENSION", "AcDbRadialDimensionLarge", "ObjectDBX Classes", true, 0x401}},
      {"IMAGE", {"IMAGE", "AcDbRasterImage", "ISM", true, 127}},
      {"IMAGEDEF", {"IMAGEDEF", "AcDbRasterImageDef", "ISM", false, 0}},
      {"IMAGEDEF_REACTOR", {"IMAGEDEF_REACTOR", "AcDbRasterImageDefReactor", "ISM", false, 1}},
      {"RASTERVARIABLES", {"RASTERVARIABLES", "AcDbRasterVariables", "ISM", false, 0}},
      {"WIPEOUT", {"WIPEOUT", "AcDbWipeout", "WipeOut", true, 127}},
      {"WIPEOUTVARIABLES", {"WIPEOUTVARIABLES", "AcDbWipeoutVariables", "WipeOut", false, 0}},
  };
}

double Ctx::class_num(std::string_view t) {
  auto it = class_nums.find(t);
  if (it == class_nums.end()) {
    ClassEntry c;
    if (const auto d = classed.find(t); d != classed.end()) {
      c.def = d->second;
    }
    c.num = 500 + static_cast<double>(classes.size());
    classes.push_back(c);
    it = class_nums.emplace(std::string(t), c.num).first;
  }
  for (ClassEntry& c : classes) {
    if (c.num == it->second) {
      c.count += 1;
      break;
    }
  }
  return it->second;
}

ObjWriter::ObjWriter(std::string_view v, bool inl) : BitWriter(v, 256), hnd(v, 64), strs(v, 64), inline_handles(inl) {
  str = &strs;
}

void ObjWriter::H(double code, const Value& v) {
  if (inline_handles) {
    BitWriter::H(code, v);
  } else {
    hnd.H(code, v);
  }
}
void ObjWriter::H(double code, double v) {
  if (inline_handles) {
    BitWriter::H(code, v);
  } else {
    hnd.H(code, v);
  }
}

std::uint64_t ObjWriter::end_data() {
  if (r2007) {
    if (str_used) {
      append(strs);
      const std::uint64_t n = strs.bit;
      if (n < 0x8000) {
        RS(static_cast<double>(n));
      } else {
        RS(static_cast<double>(n >> 15U));
        RS(static_cast<double>((n & 0x7FFFU) | 0x8000U));
      }
      B(true);
    } else {
      B(false);
    }
  }
  return bit;
}

ByteBuf eed_bytes(const Value& items, const BitWriter& w) {
  ByteWriter o(64);
  if (!items.is_array()) {
    return o.buf;
  }
  for (const Value& it : items.as_array()) {
    const Array& pair = it.as_array();
    const double code = base::js_to_number(pair[0]);
    const Value& v = pair.size() > 1 ? pair[1] : undef();
    if (code == 1000) {
      o.RC(0);
      if (w.r2007) {
        auto u = base::to_utf16(clean_text_of(v));
        if (u.size() > 255) {
          u.resize(255);
        }
        o.RS(static_cast<double>(u.size()));
        for (const std::uint16_t c : u) {
          o.RS(c);
        }
      } else {
        ByteBuf b = encode1252(str_or_empty(v));
        if (b.size() > 255) {
          b.resize(255);
        }
        o.RC(static_cast<double>(b.size()));
        o.RC(0);
        o.RC(30);
        o.raw(b);
      }
    } else if (code == 1002) {
      o.RC(2);
      o.RC(v.is_string() && v.as_string() == "}" ? 1 : 0);
    } else if (code == 1003 || code == 1005) {
      o.RC(code - 1000);
      std::uint64_t x = 0;
      if (v.is_number()) {
        x = static_cast<std::uint64_t>(static_cast<std::int64_t>(v.as_number()));
      } else {
        const auto hb = handle_bytes(v);
        for (const std::uint8_t b : hb) {
          x = (x << 8U) | b;
        }
      }
      for (int i = 7; i >= 0; --i) {
        o.RC(static_cast<double>((x >> (8U * static_cast<unsigned>(i))) & 0xFFU));
      }
    } else if (code == 1004) {
      ByteBuf b;
      if (v.is_bytes()) {
        for (const auto c : v.as_bytes()) {
          b.push_back(static_cast<std::uint8_t>(c));
        }
      } else if (v.is_string()) {
        const std::string& s = v.as_string();
        const auto hex = [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        const auto val = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        for (std::size_t i = 0; i + 1 < s.size();) {
          if (hex(s[i]) && hex(s[i + 1])) {
            b.push_back(static_cast<std::uint8_t>(val(s[i]) * 16 + val(s[i + 1])));
            i += 2;
          } else {
            ++i;
          }
        }
      }
      o.RC(4);
      o.RC(static_cast<double>(std::min<std::size_t>(255, b.size())));
      o.raw(std::span<const std::uint8_t>(b).first(std::min<std::size_t>(255, b.size())));
    } else if (code >= 1010 && code <= 1013) {
      o.RC(code - 1000);
      const auto c = [&](std::string_view k, std::size_t i) {
        Value x = get(v, k);
        if (nullish(x) && v.is_array() && i < v.as_array().size()) {
          x = v.as_array()[i];
        }
        return nullish(x) ? 0.0 : base::js_to_number(x);
      };
      o.RD(c("x", 0));
      o.RD(c("y", 1));
      o.RD(c("z", 2));
    } else if (code >= 1040 && code <= 1042) {
      o.RC(code - 1000);
      const double d = base::js_to_number(v);
      o.RD(d == d && d != 0 ? d : 0);  // +v || 0: NaN and -0 → 0
    } else if (code == 1070) {
      o.RC(70);
      o.RS(iv(v));
    } else if (code == 1071) {
      o.RC(71);
      o.RL(iv(v));
    }
  }
  return o.buf;
}

void begin(ObjWriter& w, double type, const Value& rec) {
  w.OT(type);
  if (!w.r2010) {
    w.size_at = w.bit;
    w.RL(0);
  }
  w.Hd(0, ref(rec, "h"));
  for (const Value& e : arr(rec, "eed")) {
    const ByteBuf b = eed_bytes(ref(e, "items"), w);
    if (b.empty()) {
      continue;
    }
    w.BS(static_cast<double>(b.size()));
    w.Hd(5, ref(e, "app"));
    for (const std::uint8_t x : b) {
      w.RC(x);
    }
  }
  w.BS(0);
}

ByteBuf finish(ObjWriter& w) {
  const std::uint64_t endbit = w.end_data();
  if (!w.r2010) {
    w.patch_RL(w.size_at, static_cast<double>(endbit));
  }
  w.append(w.hnd);
  const ByteBuf data = w.bytes();
  ByteWriter out(data.size() + 8);
  out.MS(static_cast<double>(data.size()));
  if (w.r2010) {
    out.UMC(static_cast<double>(data.size() * 8 - endbit));
  }
  out.raw(data);
  const std::uint32_t crc = crc8(0xC0C1, out.buf);
  out.RC(crc & 0xFFU);
  out.RC(crc >> 8U);
  return out.buf;
}

void entity_common(ObjWriter& w, const Value& r) {
  w.B(false);
  const double mode = nnum(ref(r, "mode"), 2);
  w.BB(mode);
  const Array& reactors = arr(r, "reactors");
  w.BL(static_cast<double>(reactors.size()));
  const bool xdic = tr(r, "xdic");
  if (w.r2004) {
    w.B(!xdic);
  }
  if (w.r2013) {
    w.B(tr(r, "ds"));
  }
  const bool has_prev = !ref(r, "prev").is_undefined();
  if (!w.r2004) {
    w.B(!has_prev);
  }
  w.ENC(ref(r, "color"), w.r2004 ? ref(r, "transparency") : undef());
  w.BD(nnum(ref(r, "ltscale"), 1));
  const double ltf = flag_of(r, "ltype");
  w.BB(ltf);
  const double psf = flag_of(r, "plotstyle");
  w.BB(psf);
  const double mf = flag_of(r, "material");
  if (w.r2007) {
    w.BB(mf);
    w.RC(nnum(ref(r, "shadow"), 0));
  }
  if (w.r2010) {
    w.B(false);
    w.B(false);
    w.B(false);
  }
  w.BS(tr(r, "invisible") ? 1 : 0);
  w.RC(nnum(ref(r, "lw"), 29));
  if (mode == 0) {
    w.H(kSoftPtr, ref(r, "owner"));
  }
  for (const Value& x : reactors) {
    w.H(kSoftPtr, x);
  }
  if (!w.r2004 || xdic) {
    w.H(kHardOwn, ref(r, "xdic"));
  }
  if (!w.r2004 && has_prev) {
    w.H(kSoftPtr, ref(r, "prev"));
    w.H(kSoftPtr, ref(r, "next"));
  }
  w.H(kHardPtr, ref(r, "layer"));
  if (ltf == 3) {
    w.H(kHardPtr, get(ref(r, "ltype"), "h"));
  }
  if (w.r2007 && mf == 3) {
    w.H(kHardPtr, get(ref(r, "material"), "h"));
  }
  if (psf == 3) {
    w.H(kHardPtr, get(ref(r, "plotstyle"), "h"));
  }
}

void object_common(ObjWriter& w, const Value& r) {
  const Array& reactors = arr(r, "reactors");
  w.BL(static_cast<double>(reactors.size()));
  const bool xdic = tr(r, "xdic");
  if (w.r2004) {
    w.B(!xdic);
  }
  if (w.r2013) {
    w.B(false);
  }
  w.H(nnum(ref(r, "ownerCode"), kSoftPtr), ref(r, "owner"));
  for (const Value& x : reactors) {
    w.H(kSoftPtr, x);
  }
  if (!w.r2004 || xdic) {
    w.H(kHardOwn, ref(r, "xdic"));
  }
}

void table_entry(ObjWriter& w, const Value& name, int flags) {
  w.TV(name);
  if (!w.r2007) {
    w.B((flags & 64) != 0);
    w.BS(0);
    w.B((flags & 16) != 0);
    return;
  }
  w.B(true);
  w.B((flags & 16) != 0);
}

ByteBuf xrecord_bytes(const Value& items, bool r2007) {
  ByteWriter o(256);
  if (!items.is_array()) {
    return o.buf;
  }
  for (const Value& it : items.as_array()) {
    const Array& pair = it.as_array();
    const double code = base::js_to_number(pair[0]);
    const Value& v = pair.size() > 1 ? pair[1] : undef();
    const std::string_view t = group_type(static_cast<int>(code));
    if (t.empty()) {
      continue;
    }
    o.RS(code);
    const auto plus_or0 = [](const Value& x) {
      const double d = base::js_to_number(x);
      return d == d && d != 0 ? d : 0.0;  // +x || 0: NaN and -0 → 0
    };
    if (t == "str") {
      if (r2007) {
        const auto u = base::to_utf16(clean_text_of(v));
        o.RS(static_cast<double>(u.size()));
        for (const std::uint16_t c : u) {
          o.RS(c);
        }
      } else {
        const ByteBuf b = encode1252(str_or_empty(v));
        o.RS(static_cast<double>(b.size()));
        o.RC(30);
        o.raw(b);
      }
    } else if (t == "pt3") {
      o.RD(plus_or0(get(v, "x")));
      o.RD(plus_or0(get(v, "y")));
      o.RD(plus_or0(get(v, "z")));
    } else if (t == "pt2") {
      o.RD(plus_or0(get(v, "x")));
      o.RD(plus_or0(get(v, "y")));
    } else if (t == "dbl") {
      o.RD(plus_or0(v));
    } else if (t == "i8" || t == "bool") {
      o.RC(iv(v));
    } else if (t == "i16") {
      o.RS(iv(v));
    } else if (t == "i32") {
      o.RL(iv(v));
    } else if (t == "i64") {
      o.RLL(std::trunc(plus_or0(v)));
    } else if (t == "hnd") {
      std::string s = v.truthy() ? str_or_empty(v) : std::string("0");
      if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
      }
      std::uint64_t x = 0;
      for (const char c : s) {
        int d = 0;
        if (c >= '0' && c <= '9') {
          d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
          d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          d = c - 'A' + 10;
        }
        x = (x << 4U) | static_cast<unsigned>(d);
      }
      o.RLL(x);
    } else if (t == "bin") {
      const std::string hex = v.truthy() ? str_or_empty(v) : std::string();
      const std::size_t n = std::min<std::size_t>(255, hex.size() >> 1U);
      o.RC(static_cast<double>(n));
      for (std::size_t i = 0; i < n; ++i) {
        o.RC(parse_hex2(hex, i * 2));
      }
    }
  }
  return o.buf;
}

double fixed_type(std::string_view t) {
  const auto& m = type_numbers();
  const auto it = m.find(t);
  return it == m.end() ? -1 : it->second;
}

void register_encoder(std::string_view t, EncFn f) {
  extra_encoders()[std::string(t)] = f;
}

std::optional<ByteBuf> encode_object(Rec& rec, Ctx& ctx) {
  if (rec.encode) {
    return rec.encode(rec, ctx);
  }
  const Value& tv = ref(rec.v, "t");
  if (!tv.is_string()) {
    return std::nullopt;
  }
  const std::string t = tv.as_string();
  const auto& enc = encoders();
  const auto it = enc.find(t);
  EncFn extra = nullptr;
  if (it == enc.end()) {
    const auto x = extra_encoders().find(t);
    if (x == extra_encoders().end()) {
      return std::nullopt;
    }
    extra = x->second;
  }
  double type = -1;
  if (ctx.classed.contains(t)) {
    type = ctx.class_num(t);
  } else {
    type = fixed_type(t);
  }
  if (type < 0) {
    return std::nullopt;
  }
  ObjWriter w(ctx.version);
  if (extra != nullptr) {
    // these encoders settle fields (owner, reactors, …) before the common part, so begin() follows them in spirit:
    // the JS calls begin() first, which only reads h and eed — unchanged by them
    begin(w, type, rec.v);
    extra(w, rec.v);
    return finish(w);
  }
  begin(w, type, rec.v);
  it->second(w, rec.v);
  return finish(w);
}

ByteBuf encode_classes(Ctx& ctx) {
  ObjWriter w(ctx.version);
  double max_num = 499;
  for (const ClassEntry& c : ctx.classes) {
    max_num = std::max(max_num, c.num);
  }
  if (w.r2007) {
    w.size_at = w.bit;
    w.RL(0);
  }
  if (w.r2004) {
    w.BS(max_num);
    w.RC(0);
    w.RC(0);
    w.B(true);
  }
  for (const ClassEntry& c : ctx.classes) {
    w.BS(c.num);
    w.BS(c.def.proxy);
    w.TV(Value(c.def.app));
    w.TV(Value(c.def.cpp));
    w.TV(Value(c.def.dxf));
    w.B(false);
    w.BS(c.def.entity ? 0x1F2 : 0x1F3);
    if (w.r2004) {
      w.BL(c.count);
      w.BS(ctx.dwg_version);
      w.BS(ctx.maint);
      w.BL(0);
      w.BL(0);
    }
  }
  const std::uint64_t endbit = w.end_data();
  if (w.r2007) {
    w.patch_RL(w.size_at, static_cast<double>(endbit));
    w.append(w.hnd);
  }
  return w.bytes();
}

JulianPair julian(double t) {
  const double days = std::floor(t / 86400000);
  return {days + 2440588, t - days * 86400000};
}

// ---------------------------------------------------------------- header variables (ch. 9)

namespace {

struct Def {
  int kind = 0;  // 0 none, 1 number, 2 string
  double n = 0;
  std::string_view s;
};
struct HRow {
  std::string_view name, type;
  int from, to;
  Def def;
};
constexpr auto kHeaderRows = std::to_array<HRow>({
#include "wheader_table.inc"
});

const Value& header_defaults() {
  static const Value d = make_object({
      {"UNKNOWN_9", 24},
      {"MENU", "acad"},
      {"DIMASO", 1},
      {"DIMSHO", 1},
      {"REGENMODE", 1},
      {"FILLMODE", 1},
      {"PSLTSCALE", 1},
      {"USRTIMER", 1},
      {"WORLDVIEW", 1},
      {"TILEMODE", 1},
      {"VISRETAIN", 1},
      {"PROXYGRAPHICS", 1},
      {"TREEDEPTH", 3020},
      {"LUNITS", 2},
      {"LUPREC", 4},
      {"ATTMODE", 1},
      {"SPLINESEGS", 8},
      {"SURFU", 6},
      {"SURFV", 6},
      {"SURFTYPE", 6},
      {"SURFTAB1", 6},
      {"SURFTAB2", 6},
      {"SPLINETYPE", 6},
      {"SHADEDGE", 3},
      {"SHADEDIF", 70},
      {"MAXACTVP", 64},
      {"ISOLINES", 4},
      {"TEXTQLTY", 50},
      {"LTSCALE", 1},
      {"TEXTSIZE", 0.2},
      {"TRACEWID", 0.05},
      {"SKETCHINC", 0.1},
      {"FACETRES", 0.5},
      {"CMLSCALE", 1},
      {"CELTSCALE", 1},
      {"PUCSXDIR", make_object({{"x", 1}, {"y", 0}, {"z", 0}})},
      {"PUCSYDIR", make_object({{"x", 0}, {"y", 1}, {"z", 0}})},
      {"UCSXDIR", make_object({{"x", 1}, {"y", 0}, {"z", 0}})},
      {"UCSYDIR", make_object({{"x", 0}, {"y", 1}, {"z", 0}})},
      {"PLIMMIN", make_object({{"x", 0}, {"y", 0}})},
      {"PLIMMAX", make_object({{"x", 12}, {"y", 9}})},
      {"LIMMIN", make_object({{"x", 0}, {"y", 0}})},
      {"LIMMAX", make_object({{"x", 12}, {"y", 9}})},
      {"TSTACKALIGN", 1},
      {"TSTACKSIZE", 70},
      {"SORTENTS", 127},
      {"HIDETEXT", 1},
      {"DIMASSOC", 2},
      {"OBSCUREDCOLOR", 257},
      {"INTERSECTIONCOLOR", 257},
      {"STEPSPERSEC", 2},
      {"STEPSIZE", 6},
      {"3DDWFPREC", 2},
      {"LENSLENGTH", 50},
      {"SHOWHIST", 1},
      {"PSOLWIDTH", 0.25},
      {"PSOLHEIGHT", 4},
      {"LOFTANG1", kPi / 2},
      {"LOFTANG2", kPi / 2},
      {"LOFTPARAM", 7},
      {"LOFTNORMALS", 1},
      {"LATITUDE", 37.795},
      {"LONGITUDE", -122.394},
      {"TIMEZONE", -8000},
      {"LIGHTGLYPHDISPLAY", 1},
      {"TILEMODELIGHTSYNCH", 1},
      {"DWFFRAME", 2},
      {"INTERFERECOLOR", make_object({{"index", 1}})},
      {"CECOLOR", make_object({{"index", 256}})},
      {"DIMLWD", -2},
      {"DIMLWE", -2},
      {"PSTYLEMODE", 1},
  });
  return d;
}

std::string_view header_handle_key(std::string_view n) {
  static const std::unordered_map<std::string_view, std::string_view> m = {
      {"CLAYER", "clayer"},
      {"TEXTSTYLE", "textstyle"},
      {"CELTYPE", "celtype"},
      {"CMATERIAL", "cmaterial"},
      {"DIMSTYLE", "dimstyleCur"},
      {"CMLSTYLE", "cmlstyle"},
      {"DIMTXSTY", "textstyle"},
      {"BLOCK_CONTROL_OBJECT", "blockCtl"},
      {"LAYER_CONTROL_OBJECT", "layerCtl"},
      {"STYLE_CONTROL_OBJECT", "styleCtl"},
      {"LINETYPE_CONTROL_OBJECT", "ltypeCtl"},
      {"VIEW_CONTROL_OBJECT", "viewCtl"},
      {"UCS_CONTROL_OBJECT", "ucsCtl"},
      {"VPORT_CONTROL_OBJECT", "vportCtl"},
      {"APPID_CONTROL_OBJECT", "appidCtl"},
      {"DIMSTYLE_CONTROL_OBJECT", "dimstyleCtl"},
      {"VPORT_ENTITY_HEADER_CONTROL_OBJECT", "vpentCtl"},
      {"DICTIONARY_ACAD_GROUP", "groupDict"},
      {"DICTIONARY_ACAD_MLINESTYLE", "mlsDict"},
      {"DICTIONARY_NAMED_OBJECTS", "nod"},
      {"DICTIONARY_LAYOUTS", "layoutDict"},
      {"DICTIONARY_PLOTSETTINGS", "plotSettingsDict"},
      {"DICTIONARY_PLOTSTYLES", "plotStyleDict"},
      {"DICTIONARY_MATERIALS", "materialsDict"},
      {"DICTIONARY_COLORS", "colorsDict"},
      {"DICTIONARY_VISUALSTYLE", "visualStyleDict"},
      {"BLOCK_RECORD_PAPER_SPACE", "psBlockHdr"},
      {"BLOCK_RECORD_MODEL_SPACE", "msBlockHdr"},
      {"LTYPE_BYLAYER", "ltByLayer"},
      {"LTYPE_BYBLOCK", "ltByBlock"},
      {"LTYPE_CONTINUOUS", "ltContinuous"},
  };
  const auto it = m.find(n);
  return it == m.end() ? std::string_view() : it->second;
}

bool hard_owned(std::string_view n) {
  static constexpr std::array<std::string_view, 11> k = {
      "BLOCK_CONTROL_OBJECT",    "LAYER_CONTROL_OBJECT",
      "STYLE_CONTROL_OBJECT",    "LINETYPE_CONTROL_OBJECT",
      "VIEW_CONTROL_OBJECT",     "UCS_CONTROL_OBJECT",
      "VPORT_CONTROL_OBJECT",    "APPID_CONTROL_OBJECT",
      "DIMSTYLE_CONTROL_OBJECT", "VPORT_ENTITY_HEADER_CONTROL_OBJECT",
      "DICTIONARY_NAMED_OBJECTS"};
  return std::ranges::find(k, n) != k.end();
}

}  // namespace

ByteBuf encode_header(const std::map<std::string, double, std::less<>>& H, const Value& hv, Ctx& ctx) {
  const int ver = version_num(ctx.version);
  ObjWriter w(ctx.version, !at_least(ctx.version, "AC1021"));
  if (w.r2007) {
    w.size_at = w.bit;
    w.RL(0);
  }
  const Value no_ext = make_object({{"x", 1e20}, {"y", 1e20}, {"z", 1e20}});
  const Value no_ext_max = make_object({{"x", -1e20}, {"y", -1e20}, {"z", -1e20}});
  const JulianPair jc = julian(ctx.created_ms);
  const Value td = make_object({{"day", jc.day}, {"ms", jc.ms}});
  Value computed = make_object({{"UNKNOWN_5", ctx.metric ? "m" : ""},
                                {"EXTMIN", orv(get(ctx.extents, "min"), no_ext)},
                                {"EXTMAX", orv(get(ctx.extents, "max"), no_ext_max)},
                                {"PEXTMIN", no_ext},
                                {"PEXTMAX", no_ext_max},
                                {"TDCREATE", td},
                                {"TDUPDATE", td},
                                {"HANDSEED", ctx.handseed}});
  computed.set("FINGERPRINTGUID", ctx.guid());
  computed.set("VERSIONGUID", ctx.guid());
  const Value& D = dim_defaults_js();
  const Value& HD = header_defaults();
  const auto val = [&](std::string_view name, const Value& def) -> Value {
    if (const Value* c = computed.find(name)) {
      return *c;
    }
    if (name.starts_with("DIM") && ref(hv, name).is_undefined() && !ref(D, name).is_undefined()) {
      return ref(D, name);
    }
    const Value& h = ref(hv, name);
    if (!nullish(h)) {
      return h;
    }
    const Value& d = ref(HD, name);
    if (!nullish(d)) {
      return d;
    }
    return def;
  };
  for (const HRow& e : kHeaderRows) {
    if (ver < e.from || ver > e.to) {
      continue;
    }
    Value def;
    if (e.def.kind == 1) {
      def = e.def.n;
    } else if (e.def.kind == 2) {
      def = Value(e.def.s);
    }
    const std::string_view name = e.name;
    const Value v = val(name, def);
    const std::string_view t = e.type;
    if (t == "B") {
      w.B(v.truthy());
    } else if (t == "BS") {
      w.BS(iv(v));
    } else if (t == "BL") {
      if (name == "FLAGS") {
        w.BL(29 | 0x200 | 0x800 | (val("PSTYLEMODE", 1).truthy() ? 0x2000 : 0));
      } else {
        w.BL(iv(v));
      }
    } else if (t == "BLL") {
      w.BLL(v.truthy() ? base::js_to_number(v) : 0);
    } else if (t == "BD") {
      w.BD(num(Value(base::js_to_number(v)), 0));
    } else if (t == "RC") {
      w.RC(iv(v));
    } else if (t == "RS") {
      w.RS(iv(v));
    } else if (t == "RL") {
      w.RL(iv(v));
    } else if (t == "TV") {
      w.TV(nullish(v) ? Value("") : v);
    } else if (t == "T") {
      w.T8(nullish(v) ? Value("") : v);
    } else if (t == "CMC") {
      if (v.is_number()) {
        w.CMC(make_object({{"index", v}}));
      } else {
        w.CMC(v.truthy() ? v : color_index(256));
      }
    } else if (t == "3BD") {
      w.BD3(numk(v, "x"), numk(v, "y"), numk(v, "z"));
    } else if (t == "2RD") {
      w.RD2(numk(v, "x"), numk(v, "y"));
    } else if (t == "TD") {
      w.BL(nnum(get(v, "day"), std::nan("")));
      w.BL(nnum(get(v, "ms"), std::nan("")));
    } else if (t == "TS") {
      w.BL(0);
      w.BL(0);
    } else if (t == "HS") {
      w.Hd(0, v);
    } else if (t == "H") {
      if (name == "CPSNID" && base::js_to_int32(base::js_to_number(val("CEPSNTYPE", 0))) != 3) {
        continue;
      }
      double h = 0;
      if (const std::string_view key = header_handle_key(name); !key.empty()) {
        if (const auto it = H.find(key); it != H.end()) {
          h = it->second;
        }
      }
      w.H(hard_owned(name) ? kHardOwn : kHardPtr, h);
    } else {
      throw std::logic_error("header type " + std::string(t));
    }
  }
  const std::uint64_t endbit = w.end_data();
  if (w.r2007) {
    w.patch_RL(w.size_at, static_cast<double>(endbit));
    w.append(w.hnd);
  }
  return w.bytes();
}

}  // namespace cc::dwg::w
