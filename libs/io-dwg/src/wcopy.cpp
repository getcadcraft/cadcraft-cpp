// SPDX-License-Identifier: Apache-2.0
// nongraphic-write.js (the source drawing's non-graphical objects copied into the written file), the writing half of
// passthrough.js (raw objects and entities, proxies of their own class in another version) and mleader-write.js
// (MULTILEADER, MLEADERSTYLE) in C++. The "Spec gap:" notes are in the JS files.
#include <algorithm>
#include <cmath>
#include <deque>
#include <regex>
#include <set>

#include "bits.hpp"
#include "cc/base/js.hpp"
#include "container.hpp"
#include "messages.hpp"
#include "wwriter.hpp"

namespace cc::dwg::w {

using base::make_object;

namespace {

bool is_str(const Value& v, std::string_view s) {
  return v.is_string() && v.as_string() == s;
}
std::string str_of(const Value& v) {
  if (nullish(v)) {
    return {};
  }
  return v.is_string() ? v.as_string() : base::js_string(v);
}
std::string type_of(const Value& o) {
  const Value& t = ref(o, "type");
  return t.is_string() ? t.as_string() : base::js_string(t);
}
int vnum(std::string_view v) {
  return version_num(v);
}
Value orv(const Value& a, const Value& b) {
  return a.truthy() ? a : b;
}

// ---------------------------------------------------------------- passthrough.js (writing)

struct ConvPlan {
  bool native = false;
  ByteBuf bits;
  std::uint64_t nd = 0;
  std::vector<U16> strings;
  double fmt_ver = 0, fmt_maint = 0;
  ClassDef cls;
};

struct RawAux {
  std::optional<ClassDef> cls;
  const Value* raw = nullptr;
  Value gfx;  // MULTILEADER: the entity's proxy graphics (base64) instead of raw.gfx
  bool is_entity = false;
  double type_num = 0;
  std::function<double(const Value&)> map;
  /// passthrough.js proxyClass: the class of a proxy's class id, as its RAW:<dxf> key and entry
  std::function<std::optional<std::pair<std::string, ClassDef>>(double)> proxy_class;
  std::shared_ptr<ConvPlan> conv;
};

int dhl(std::string_view v) {
  if (v == "AC1015") {
    return 23;
  }
  if (v == "AC1018") {
    return 25;
  }
  if (v == "AC1021") {
    return 27;
  }
  if (v == "AC1024") {
    return 29;
  }
  if (v == "AC1027") {
    return 31;
  }
  if (v == "AC1032") {
    return 33;
  }
  return 0;
}
bool dhl_ver(double f) {
  return f == 23 || f == 25 || f == 27 || f == 29 || f == 31 || f == 33;
}

bool raw_usable(const Value& raw, std::string_view version, std::string_view type = {}) {
  if (!raw.truthy()) {
    return false;
  }
  const double v = vnum(version);
  const double rv = num(ref(raw, "ver"), -1);
  if (rv == v) {
    return true;
  }
  using G = std::vector<std::vector<double>>;
  static const G two = {{15, 18}, {21, 24, 27, 32}};
  static const std::unordered_map<std::string_view, G> groups = {
      {"MATERIAL", two},
      {"DIMASSOC", two},
      {"ACDB_ALDIMOBJECTCONTEXTDATA_CLASS", two},
      {"ACDB_MTEXTOBJECTCONTEXTDATA_CLASS", two},
      {"RAPIDRTRENDERSETTINGS", two},
      {"SUN", G{{21, 24, 27, 32}}},
      {"VISUALSTYLE", G{{27, 32}}},
      {"ACDB_MLEADEROBJECTCONTEXTDATA_CLASS", G{{24, 27, 32}}},
      {"ACDBDETAILVIEWSTYLE", G{{21, 24, 27}}},
      {"ACDBSECTIONVIEWSTYLE", G{{21, 24, 27}}},
  };
  if (type.empty()) {
    return false;
  }
  const auto it = groups.find(type);
  if (it == groups.end()) {
    return false;
  }
  return std::ranges::any_of(it->second, [&](const std::vector<double>& g) {
    return std::ranges::find(g, rv) != g.end() && std::ranges::find(g, v) != g.end();
  });
}

struct Strings {
  std::uint64_t nd = 0;
  std::vector<U16> strings;
};
std::optional<Strings> strings_of(const ByteBuf& buf, std::uint64_t n) {
  if (n == 0) {
    return std::nullopt;
  }
  Bits t(buf, 32, nullptr, n - 1, n);
  if (t.B() == 0) {
    return Strings{n - 1, {}};
  }
  if (n < 17) {
    return std::nullopt;
  }
  std::uint64_t p = n - 1 - 16;
  t.set_pos(p);
  t.set_end(p + 16);
  std::uint64_t size = t.RSu();
  if ((size & 0x8000U) != 0) {
    if (p < 16) {
      return std::nullopt;
    }
    p -= 16;
    t.set_pos(p);
    t.set_end(p + 16);
    const std::uint64_t hi = t.RSu();
    size = (size & 0x7FFFU) | (hi << 15U);
  }
  if (size > p) {
    return std::nullopt;
  }
  const std::uint64_t start = p - size;
  Bits s(buf, 32, nullptr, start, p);
  Strings out{start, {}};
  while (s.left() >= 2) {
    out.strings.push_back(s.TU16());
  }
  return out;
}

struct ClassData {
  ByteBuf bits;
  std::uint64_t nd = 0;
  std::vector<U16> strings;
  double fmt_ver = 0, fmt_maint = 0;
  double class_id = -1;
};
std::optional<ClassData> class_data(const Value& raw, bool is_proxy, std::string_view src_version, double src_maint) {
  ByteBuf buf = unb64(str_of(ref(raw, "data")));
  const auto n = static_cast<std::uint64_t>(std::max(0.0, num(ref(raw, "n"))));
  const int v = static_cast<int>(num(ref(raw, "ver")));
  try {
    if (!is_proxy) {
      if (v < 21) {
        return std::nullopt;
      }
      auto t = strings_of(buf, n);
      if (!t) {
        return std::nullopt;
      }
      return ClassData{std::move(buf), t->nd, std::move(t->strings), static_cast<double>(dhl(src_version)),
                       src_maint,      -1};
    }
    Bits r(buf, v, nullptr, 0, n);
    const double class_id = r.BL();
    double fv = 0;
    double fm = 0;
    if (v == 18) {
      (void)r.T();
      const auto f = static_cast<std::uint32_t>(r.BL());
      fv = f & 0xFFFFU;
      fm = f >> 16U;
    } else if (v >= 32) {
      fv = r.BL();
      fm = r.BL();
    } else {
      const auto f = static_cast<std::uint32_t>(r.BL());
      fv = f & 0xFFFFU;
      fm = f >> 16U;
    }
    (void)r.B();
    const std::uint64_t pos = r.pos();
    ByteBuf body = slice_bits(buf, pos, n);
    const std::uint64_t nb = n > pos ? n - pos : 0;
    auto t = strings_of(body, nb);
    if (!t) {
      return std::nullopt;
    }
    std::vector<U16> strings = std::move(t->strings);
    if (v >= 21 && !strings.empty() && strings[0].size() >= 3 && strings[0][0] == 'c' && strings[0][1] == 'n' &&
        strings[0][2] == ':') {
      strings.erase(strings.begin());
    }
    return ClassData{std::move(body), t->nd, std::move(strings), fv, fm, class_id};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::shared_ptr<ConvPlan> convert_plan(const Value& raw, bool is_proxy,
                                       const std::function<std::optional<ClassDef>(double)>& cls_for,
                                       std::string_view src_version, double src_maint, std::string_view version) {
  if (!raw.truthy()) {
    return nullptr;
  }
  auto data = class_data(raw, is_proxy, src_version, src_maint);
  if (!data || data->fmt_ver == 0 || !dhl_ver(data->fmt_ver)) {
    return nullptr;
  }
  auto cls = cls_for(data->class_id);
  if (!cls) {
    return nullptr;
  }
  auto p = std::make_shared<ConvPlan>();
  p->native = data->fmt_ver == dhl(version) && vnum(version) >= 21;
  p->bits = std::move(data->bits);
  p->nd = data->nd;
  p->strings = std::move(data->strings);
  p->fmt_ver = data->fmt_ver;
  p->fmt_maint = data->fmt_maint;
  p->cls = *cls;
  return p;
}

ByteBuf frame_object(ObjWriter& w, std::uint64_t endbit) {
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

double register_raw(Ctx& ctx, const ClassDef& c) {
  const std::string k = "RAW:" + c.dxf;
  if (!ctx.classed.contains(k)) {
    ctx.classed[k] = c;
  }
  return ctx.class_num(k);
}

double raw_type(const RawAux& a, Ctx& ctx) {
  if (a.type_num < 500) {
    return a.type_num;
  }
  return register_raw(ctx, a.cls ? *a.cls : ClassDef{});
}

void map_refs(ObjWriter& w, const RawAux& a) {
  for (const Value& it : arr(*a.raw, "h")) {
    const Array& p = it.as_array();
    const double code = base::js_to_number(p[0]);
    const Value& hex = p.size() > 1 ? p[1] : undef();
    if (!hex.truthy() || is_str(hex, "0")) {
      w.hnd.H(code, 0);
      continue;
    }
    w.hnd.H(code >= 6 ? kSoftPtr : code, a.map ? a.map(hex) : 0);
  }
}

}  // namespace

/// passthrough.js rawEntityCommon: the common entity data with the source's proxy graphics.
void raw_entity_common(ObjWriter& w, const Value& r, const Value& gfx_b64) {
  const ByteBuf g = gfx_b64.truthy() ? unb64(str_of(gfx_b64)) : ByteBuf{};
  if (!g.empty()) {
    w.B(true);
    if (w.r2010) {
      w.BLL(static_cast<double>(g.size()));
    } else {
      w.RL(static_cast<double>(g.size()));
    }
    for (const std::uint8_t b : g) {
      w.RC(b);
    }
  } else {
    w.B(false);
  }
  const double mode = nnum(ref(r, "mode"), 2);
  w.BB(mode);
  const Array& reactors = arr(r, "reactors");
  w.BL(static_cast<double>(reactors.size()));
  const bool xdic = tr(r, "xdic");
  if (w.r2004) {
    w.B(!xdic);
  }
  if (w.r2013) {
    w.B(false);
  }
  const bool has_prev = !ref(r, "prev").is_undefined();
  if (!w.r2004) {
    w.B(!has_prev);
  }
  w.ENC(ref(r, "color"), w.r2004 ? ref(r, "transparency") : undef());
  w.BD(nnum(ref(r, "ltscale"), 1));
  const double ltf = nnum(get(ref(r, "ltype"), "flag"), 0);
  w.BB(ltf);
  const double psf = nnum(get(ref(r, "plotstyle"), "flag"), 0);
  w.BB(psf);
  const double mf = nnum(get(ref(r, "material"), "flag"), 0);
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
  w.H(5, ref(r, "layer"));
  if (ltf == 3) {
    w.H(5, get(ref(r, "ltype"), "h"));
  }
  if (w.r2007 && mf == 3) {
    w.H(5, get(ref(r, "material"), "h"));
  }
  if (psf == 3) {
    w.H(5, get(ref(r, "plotstyle"), "h"));
  }
}

namespace {

std::optional<ByteBuf> encode_converted(Rec& rec, Ctx& ctx, const RawAux& a) {
  const ConvPlan& cv = *a.conv;
  const int v = vnum(ctx.version);
  ObjWriter w(ctx.version);
  const double cnum = register_raw(ctx, a.cls ? *a.cls : cv.cls);
  const double proxy_type = a.is_entity ? 0x1F2 : 0x1F3;
  begin(w, cv.native ? cnum : proxy_type, rec.v);
  if (a.is_entity) {
    raw_entity_common(w, rec.v, ref(*a.raw, "gfx"));
  } else {
    object_common(w, rec.v);
  }
  const ClassDef& cls = a.cls ? *a.cls : cv.cls;
  if (cv.native) {
    append_bits(w, cv.bits, cv.nd);
    for (const U16& s : cv.strings) {
      w.TV16(s);
    }
    const std::uint64_t endbit = w.end_data();
    map_refs(w, a);
    return frame_object(w, endbit);
  }
  const auto fmt = static_cast<std::uint32_t>((static_cast<std::uint32_t>(cv.fmt_maint) & 0xFFFFU) << 16U |
                                              static_cast<std::uint32_t>(cv.fmt_ver));
  w.BL(cnum);
  const std::string cn = "cn:" + cls.cpp;
  if (v == 18) {
    const auto u = base::to_utf16(cn);
    w.BS(static_cast<double>(u.size()));
    for (const std::uint16_t c : u) {
      w.RC(static_cast<double>(c & 0xFFU));
    }
  }
  if (v >= 32) {
    w.BL(cv.fmt_ver);
    w.BL(cv.fmt_maint);
  } else {
    w.BL(static_cast<double>(static_cast<std::int32_t>(fmt)));
  }
  w.B(false);
  append_bits(w, cv.bits, cv.nd);
  std::uint64_t endbit = 0;
  if (v < 21) {
    if (cv.strings.empty()) {
      w.B(false);
    } else {
      ObjWriter t("AC1021");
      for (const U16& s : cv.strings) {
        t.TU16(s);
      }
      append_bits(w, t.buf, t.bit);
      const std::uint64_t n = t.bit;
      if (n < 0x8000) {
        w.RS(static_cast<double>(n));
      } else {
        w.RS(static_cast<double>(n >> 15U));
        w.RS(static_cast<double>((n & 0x7FFFU) | 0x8000U));
      }
      w.B(true);
    }
    endbit = w.bit;
  } else {
    w.TV(Value(cn));
    for (const U16& s : cv.strings) {
      w.TV16(s);
    }
    endbit = w.end_data();
  }
  map_refs(w, a);
  return frame_object(w, endbit);
}

std::optional<ByteBuf> encode_raw(Rec& rec, Ctx& ctx) {
  const auto& a = *std::static_pointer_cast<RawAux>(rec.aux);
  if (a.conv) {
    return encode_converted(rec, ctx, a);
  }
  ObjWriter w(ctx.version);
  begin(w, raw_type(a, ctx), rec.v);
  if (a.is_entity) {
    raw_entity_common(w, rec.v, ref(*a.raw, "gfx"));
  } else {
    object_common(w, rec.v);
  }
  ByteBuf bits = unb64(str_of(ref(*a.raw, "data")));
  auto n = static_cast<std::uint64_t>(std::max(0.0, num(ref(*a.raw, "n"))));
  if ((a.type_num == 0x1F2 || a.type_num == 0x1F3) && a.proxy_class && vnum(ctx.version) >= 15) {
    try {
      Bits r(bits, vnum(ctx.version), nullptr, 0, n);
      const double id = r.BL();
      if (auto key = a.proxy_class(id)) {
        if (!ctx.classed.contains(key->first)) {
          ctx.classed[key->first] = key->second;
        }
        ObjWriter t(ctx.version);
        t.BL(ctx.class_num(key->first));
        const std::uint64_t pos = r.pos();
        append_bits(t, slice_bits(bits, pos, n), n > pos ? n - pos : 0);
        bits = t.buf;
        n = t.bit;
      }
    } catch (const std::exception&) {
      bits = unb64(str_of(ref(*a.raw, "data")));  // a short proxy: copied as it is
    }
  }
  append_bits(w, bits, n);
  const std::uint64_t endbit = w.bit;
  map_refs(w, a);
  return frame_object(w, endbit);
}

// ---------------------------------------------------------------- mleader-write.js

/// TXT: up to R2004 a multileader's strings have no closing NUL
void txt(ObjWriter& w, const Value& s) {
  if (w.r2007) {
    w.TV(s);
    return;
  }
  const ByteBuf b = encode1252(str_of(s));
  w.BS(static_cast<double>(b.size()));
  for (const std::uint8_t c : b) {
    w.RC(c);
  }
}
Value col(const Value& c, double d) {
  if (nullish(c)) {
    return make_object({{"index", d}});
  }
  if (c.is_number()) {
    return make_object({{"index", c}});
  }
  return c;
}
/// pt(w, p, d): P(p, d) as BD3
void pt(ObjWriter& w, const Value& p, double dx = 0, double dy = 0, double dz = 0) {
  if (p.truthy()) {
    w.BD3(numk(p, "x"), numk(p, "y"), numk(p, "z"));
  } else {
    w.BD3(dx, dy, dz);
  }
}
double nnb(const Value& o, std::string_view k, double d) {
  return nnum(ref(o, k), d);
}

void write_context(ObjWriter& w, const Value& c, MlRefs& R, int v) {
  const Array& roots = arr(c, "roots");
  w.BL(static_cast<double>(roots.size()));
  for (std::size_t ri = 0; ri < roots.size(); ++ri) {
    const Value& root = roots[ri];
    w.B(!is_false(ref(root, "contentValid")));
    w.B(!is_false(ref(root, "unknown")));
    pt(w, ref(root, "connection"));
    pt(w, ref(root, "direction"), 1, 0, 0);
    const Array& br = arr(root, "breaks");
    w.BL(static_cast<double>(br.size()));
    for (const Value& b : br) {
      pt(w, b.as_array()[0]);
      pt(w, b.as_array()[1]);
    }
    w.BL(nnb(root, "index", static_cast<double>(ri)));
    w.BD(numk(root, "landingDistance", 0.36));
    const Array& lines = arr(root, "lines");
    w.BL(static_cast<double>(lines.size()));
    for (std::size_t li = 0; li < lines.size(); ++li) {
      const Value& ln = lines[li];
      const Array& ps = arr(ln, "points");
      w.BL(static_cast<double>(ps.size()));
      for (const Value& p : ps) {
        pt(w, p);
      }
      const double bc = ivk(ln, "breakInfoCount");
      w.BL(bc);
      if (bc != 0) {
        w.BL(ivk(ln, "segmentIndex"));
        const Array& b = arr(ln, "breaks");
        w.BL(static_cast<double>(b.size()));
        for (const Value& x : b) {
          pt(w, x.as_array()[0]);
          pt(w, x.as_array()[1]);
        }
      }
      w.BL(nnb(ln, "index", static_cast<double>(li)));
      if (v >= 24) {
        w.BS(nnb(ln, "leaderType", 1));
        w.CMC(col(ref(ln, "color"), 0));
        w.H(kHardPtr, R.h(ref(ln, "linetypeHandle")));
        w.BL(nnb(ln, "lineweight", -2));
        w.BD(numk(ln, "arrowSize", 0));
        w.H(kHardPtr, R.h(ref(ln, "arrowheadHandle")));
        w.BL(ivk(ln, "overrideFlags"));
      }
    }
    if (v >= 24) {
      w.BS(ivk(root, "attachDirection"));
    }
  }
  w.BD(numk(c, "scale", 1));
  pt(w, ref(c, "contentBase"));
  w.BD(numk(c, "textHeight", 0.18));
  w.BD(numk(c, "arrowSize", 0.18));
  w.BD(numk(c, "landingGap", 0.09));
  w.BS(nnb(c, "leftAttach", 1));
  w.BS(nnb(c, "rightAttach", 1));
  w.BS(ivk(c, "textAlign"));
  w.BS(ivk(c, "attachType"));
  const Value& t = !is_false(ref(c, "hasText")) && ref(c, "text").truthy() ? ref(c, "text") : undef();
  w.B(t.truthy());
  if (t.truthy()) {
    txt(w, Value(str_of(ref(t, "label"))));
    pt(w, ref(t, "normal"), 0, 0, 1);
    w.H(kHardPtr, R.text_style());
    pt(w, ref(t, "location"));
    pt(w, ref(t, "direction"), 1, 0, 0);
    w.BD(numk(t, "rotation"));
    w.BD(numk(t, "width"));
    w.BD(numk(t, "height"));
    w.BD(numk(t, "lineSpacing", 1));
    w.BS(nnb(t, "lineSpacingStyle", 1));
    w.CMC(col(ref(t, "color"), 256));
    w.BS(nnb(t, "alignment", 1));
    w.BS(nnb(t, "flow", 1));
    w.CMC(col(ref(t, "bgColor"), 7));
    w.BD(numk(t, "bgScale", 1.5));
    w.BL(ivk(t, "bgTransparency"));
    w.B(tr(t, "bgFill"));
    w.B(tr(t, "bgMask"));
    w.BS(ivk(t, "columnType"));
    w.B(tr(t, "autoHeight"));
    w.BD(numk(t, "columnWidth"));
    w.BD(numk(t, "columnGutter"));
    w.B(tr(t, "columnFlowReversed"));
    const Array& cs = arr(t, "columnSizes");
    w.BL(static_cast<double>(cs.size()));
    for (const Value& x : cs) {
      w.BD(num(x));
    }
    w.B(tr(t, "wordBreak"));
    w.B(tr(t, "unknown"));
  } else {
    const Value& b = !is_false(ref(c, "hasBlock")) && ref(c, "block").truthy() ? ref(c, "block") : undef();
    w.B(b.truthy());
    if (b.truthy()) {
      w.H(kSoftPtr, R.content_block());
      pt(w, ref(b, "normal"), 0, 0, 1);
      pt(w, ref(b, "location"));
      pt(w, ref(b, "scale"), 1, 1, 1);
      w.BD(numk(b, "rotation"));
      w.CMC(col(ref(b, "color"), 0));
      const Value& m = ref(b, "matrix");
      if (m.is_array() && m.as_array().size() == 16) {
        for (const Value& x : m.as_array()) {
          w.BD(num(x));
        }
      } else {
        const Value& sv = ref(b, "scale");
        const double sx = sv.truthy() ? numk(sv, "x") : 1;
        const double sy = sv.truthy() ? numk(sv, "y") : 1;
        const double sz = sv.truthy() ? numk(sv, "z") : 1;
        const double r = numk(b, "rotation");
        const Value& lv = ref(b, "location");
        const double lx = lv.truthy() ? numk(lv, "x") : 0;
        const double ly = lv.truthy() ? numk(lv, "y") : 0;
        const double lz = lv.truthy() ? numk(lv, "z") : 0;
        const double cs = std::cos(r);
        const double sn = std::sin(r);
        for (const double x :
             {cs * sx, -sn * sy, 0.0, lx, sn * sx, cs * sy, 0.0, ly, 0.0, 0.0, sz, lz, 0.0, 0.0, 0.0, 1.0}) {
          w.BD(x);
        }
      }
    }
  }
  pt(w, ref(c, "basePoint"));
  pt(w, ref(c, "baseDirection"), 1, 0, 0);
  pt(w, ref(c, "baseVertical"), 0, 1, 0);
  w.B(tr(c, "normalReversed"));
  if (v >= 24) {
    w.BS(nnb(c, "topAttach", 9));
    w.BS(nnb(c, "bottomAttach", 9));
  }
}

std::optional<ByteBuf> encode_mleader(Rec& rec, Ctx& ctx) {
  const Value& e = **std::static_pointer_cast<const Value*>(rec.aux);
  MlRefs& R = *rec.ml;
  const int v = vnum(ctx.version);
  ObjWriter w(ctx.version);
  begin(w, ctx.class_num("MULTILEADER"), rec.v);
  raw_entity_common(w, rec.v, ref(e, "proxyGraphics"));
  if (v >= 24) {
    w.BS(nnb(e, "version", 2));
  }
  write_context(w, ref(e, "context"), R, v);
  w.H(kHardPtr, R.style());
  w.BL(nnb(e, "overrideFlags", 0));
  w.BS(nnb(e, "leaderType", 1));
  w.CMC(col(ref(e, "leaderColor"), 0));
  w.H(kHardPtr, R.leader_ltype());
  w.BL(nnb(e, "leaderLineweight", -2));
  w.B(!is_false(ref(e, "landingEnabled")));
  w.B(!is_false(ref(e, "doglegEnabled")));
  w.BD(numk(e, "landingDistance", 0.36));
  w.H(kHardPtr, R.arrowhead());
  w.BD(numk(e, "arrowSize", 0.18));
  {
    const Value& ct = ref(e, "contentType");
    const Value& cx = ref(e, "context");
    double content = 0;
    if (!nullish(ct)) {
      content = base::js_to_number(ct);
    } else if (tr(cx, "hasText")) {
      content = 2;
    } else if (tr(cx, "hasBlock")) {
      content = 1;
    }
    w.BS(content);
  }
  w.H(kHardPtr, R.style_text());
  w.BS(nnb(e, "textLeftAttach", 1));
  w.BS(nnb(e, "textRightAttach", 1));
  w.BS(nnb(e, "textAngleType", 1));
  w.BS(nnb(e, "unknown175", 0));
  w.CMC(col(ref(e, "textColor"), 0));
  w.B(tr(e, "textFrame"));
  w.H(kHardPtr, R.style_block());
  w.CMC(col(ref(e, "blockColor"), 0));
  pt(w, ref(e, "blockScale"), 1, 1, 1);
  w.BD(numk(e, "blockRotation"));
  w.BS(nnb(e, "blockAttach", 0));
  w.B(tr(e, "annotative"));
  if (v < 24) {
    const Array& ah = arr(e, "arrowheads");
    w.BL(static_cast<double>(ah.size()));
    for (const Value& a : ah) {
      w.B(tr(a, "isDefault"));
      w.H(kHardPtr, R.h(ref(a, "handle")));
    }
  }
  std::vector<const Value*> bl;
  for (const Value& l : arr(e, "blockLabels")) {
    if (R.h(ref(l, "attdefHandle")) != 0) {
      bl.push_back(&l);
    }
  }
  w.BL(static_cast<double>(bl.size()));
  for (const Value* l : bl) {
    w.H(kSoftPtr, R.h(ref(*l, "attdefHandle")));
    txt(w, Value(str_of(ref(*l, "text"))));
    w.BS(ivk(*l, "index"));
    w.BD(numk(*l, "width"));
  }
  w.B(tr(e, "textDirectionNegative"));
  w.BS(nnb(e, "ipeAlign", 0));
  w.BS(nnb(e, "justification", 1));
  w.BD(numk(e, "scale", 1));
  if (v >= 24) {
    w.BS(nnb(e, "attachDirection", 0));
    w.BS(nnb(e, "textTopAttach", 9));
    w.BS(nnb(e, "textBottomAttach", 9));
  }
  if (v >= 27) {
    w.B(tr(e, "extendToText"));
  }
  return finish(w);
}

}  // namespace

/// MLEADERSTYLE from the model's fields (mleader-write.js encodeMleaderStyle).
std::optional<ByteBuf> encode_mleader_style(const Value& rv, const Value& o,
                                            const std::function<double(const Value&)>& rf, Ctx& ctx) {
  const int v = vnum(ctx.version);
  ObjWriter w(ctx.version);
  begin(w, ctx.class_num("MLEADERSTYLE"), rv);
  object_common(w, rv);
  const bool new_format = v >= 24 || tr(rv, "newFormat");
  if (v >= 24) {
    w.BS(nnb(o, "styleVersion", 2));
  }
  w.BS(nnb(o, "contentType", 2));
  w.BS(nnb(o, "drawMLeaderOrder", 1));
  w.BS(ivk(o, "drawLeaderOrder"));
  w.BL(nnb(o, "maxPoints", 2));
  w.BD(numk(o, "firstAngle"));
  w.BD(numk(o, "secondAngle"));
  w.BS(nnb(o, "leaderType", 1));
  w.CMC(col(ref(o, "leaderColor"), 0));
  w.H(kHardPtr, rf(ref(o, "leaderLinetype")));
  w.BL(nnb(o, "leaderLineweight", -2));
  w.B(!is_false(ref(o, "landingEnabled")));
  w.BD(numk(o, "landingGap", 0.09));
  w.B(!is_false(ref(o, "doglegEnabled")));
  w.BD(numk(o, "landingDistance", 0.36));
  w.TV(nullish(ref(o, "description")) ? Value("") : ref(o, "description"));
  w.H(kHardPtr, rf(ref(o, "arrowhead")));
  w.BD(numk(o, "arrowSize", 0.18));
  w.TV(nullish(ref(o, "textDefault")) ? Value("") : ref(o, "textDefault"));
  w.H(kHardPtr, rf(ref(o, "textStyle")));
  w.BS(nnb(o, "leftAttach", 1));
  w.BS(nnb(o, "rightAttach", 1));
  if (new_format) {
    w.BS(nnb(o, "textAngleType", 1));
  }
  w.BS(ivk(o, "textAlign"));
  w.CMC(col(ref(o, "textColor"), 0));
  w.BD(numk(o, "textHeight", 0.18));
  w.B(tr(o, "textFrame"));
  if (new_format) {
    w.B(tr(o, "alwaysAlignLeft"));
  }
  w.BD(numk(o, "alignSpace", 4));
  w.H(kHardPtr, rf(ref(o, "block")));
  w.CMC(col(ref(o, "blockColor"), 0));
  pt(w, ref(o, "blockScale"), 1, 1, 1);
  w.B(!is_false(ref(o, "blockScaleEnabled")));
  w.BD(numk(o, "blockRotation"));
  w.B(!is_false(ref(o, "blockRotationEnabled")));
  w.BS(ivk(o, "blockConnection"));
  w.BD(numk(o, "scale", 1));
  w.B(tr(o, "changed"));
  w.B(tr(o, "annotative"));
  w.BD(numk(o, "breakSize", 0.125));
  if (v >= 24) {
    w.BS(ivk(o, "attachDirection"));
    w.BS(nnb(o, "topAttach", 9));
    w.BS(nnb(o, "bottomAttach", 9));
  }
  if (v >= 27) {
    w.B(tr(o, "unknown298"));
  }
  return finish(w);
}

// ---------------------------------------------------------------- nongraphic-write.js

struct CopyWriter::Impl {
  Writer& W;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives inside its Writer
  explicit Impl(Writer& w) : W(w) {}
  std::unordered_map<double, const Value*> classes;  // model class number → class
  double src_maint = 0;
  bool old = false;
  Value mleader_eed;
  std::unordered_map<const Value*, Value> eed_cache;
  struct Notes {
    Value raw_entities = Value::object();
    Value dropped_entities = Value::object();
    Value proxied = Value::object();
  } notes;
  std::deque<Value> synth;  // srcThings the copier makes up (block records, control objects)

  [[nodiscard]] std::optional<ClassDef> cls_of(double n) const {
    const auto it = classes.find(n);
    if (it == classes.end()) {
      return std::nullopt;
    }
    const Value& c = *it->second;
    ClassDef d;
    d.dxf = str_of(ref(c, "dxfName"));
    d.cpp = str_of(ref(c, "cppName"));
    d.app = str_of(ref(c, "appName"));
    d.entity = tr(c, "isEntity");
    d.proxy = static_cast<int>(ivk(c, "proxyFlags"));
    return d;
  }
  [[nodiscard]] std::optional<std::pair<std::string, ClassDef>> proxy_class(double id) const {
    auto c = cls_of(id);
    if (!c) {
      return std::nullopt;
    }
    return std::pair{"RAW:" + c->dxf, *c};
  }
  static bool never_proxy(const std::string& dxf) {
    static const std::regex re(
        R"(^(MATERIAL|VISUALSTYLE|ACDBDETAILVIEWSTYLE|ACDBSECTIONVIEWSTYLE|SUN|DIMASSOC|RAPIDRTRENDERSETTINGS|RENDER\w*|ACDB_\w*OBJECTCONTEXTDATA_CLASS|TABLESTYLE|CELLSTYLEMAP|TABLECONTENT|TABLEGEOMETRY|DATATABLE|FIELD|FIELDLIST|SCALE|MLEADERSTYLE|LAYOUT|PLOTSETTINGS|\w*VARIABLES|IMAGEDEF\w*|DICTIONARY\w*|XRECORD|GROUP|ACDBPLACEHOLDER|SORTENTSTABLE|SPATIAL_\w+|LAYER_INDEX|IDBUFFER|DBCOLOR|GEODATA|LIGHTLIST|MULTILEADER|ACAD_TABLE|LIGHT|REGION|3DSOLID|BODY|MLINESTYLE)$)");
    return std::regex_match(dxf, re);
  }
  std::shared_ptr<ConvPlan> plan_for(const Value& raw, bool is_proxy, double type_num) {
    return convert_plan(
        raw, is_proxy,
        [&](double id) -> std::optional<ClassDef> {
          auto c = cls_of(is_proxy ? id : type_num);
          if (c && !never_proxy(c->dxf)) {
            return c;
          }
          return std::nullopt;
        },
        str_of(ref(W.model, "version")), src_maint, W.version);
  }
  [[nodiscard]] double map(const Value& hex) const { return W.src(hex); }
};

CopyWriter::CopyWriter(Writer& w) : impl(std::make_unique<Impl>(w)) {
  Impl& I = *impl;
  const Value& model = w.model;
  for (const Value& c : arr(model, "classes")) {
    I.classes[num(ref(c, "num"), std::nan(""))] = &c;
  }
  I.src_maint = num(ref(model, "maintenance"));
  if (I.src_maint == 0) {
    I.src_maint = is_str(ref(model, "version"), "AC1032") ? 29 : 0;
  }
  I.old = vnum(w.version) < 24;
  bool has_ml =
      std::ranges::any_of(arr(model, "objects"), [](const Value& o) { return is_str(ref(o, "type"), "MLEADERSTYLE"); });
  if (!has_ml) {
    for (const Value& b : arr(model, "blocks")) {
      for (const Value& e : arr(b, "entities")) {
        if (is_str(ref(e, "type"), "MULTILEADER") || is_str(ref(e, "type"), "MLEADER")) {
          has_ml = true;
        }
      }
    }
  }
  if (has_ml && I.old) {
    I.mleader_eed = w.eed_of(make_object({{"ACAD_MLEADERVER", base::array_of(Array{1070, 2})}}));
  }
  for (const Value& o : arr(model, "objects")) {
    Value x = ref(o, "xdata");
    if (is_str(ref(o, "type"), "MLEADERSTYLE") && !raw_usable(ref(o, "raw"), w.version)) {
      x = x.truthy() && x.is_object() ? x : Value::object();
      if (I.old) {
        x.set("ACAD_MLEADERVER", base::array_of(Array{1070, 2}));
      } else {
        x.erase("ACAD_MLEADERVER");
      }
    }
    Value e = w.eed_of(x);
    if (e.truthy()) {
      I.eed_cache[&o] = std::move(e);
    }
  }
  const Value& T = ref(model, "tables");
  if (T.is_object()) {
    for (const base::Member* m : base::js_members(T.as_object())) {
      if (!m->value.is_array()) {
        continue;
      }
      for (const Value& t : m->value.as_array()) {
        if (ref(t, "xdata").truthy()) {
          Value e = w.eed_of(ref(t, "xdata"));
          if (e.truthy()) {
            I.eed_cache[&t] = std::move(e);
          }
        }
      }
    }
  }
}

CopyWriter::~CopyWriter() = default;

std::optional<std::variant<std::vector<RecPtr>, std::string>> CopyWriter::convert(const Value& e,
                                                                                  const std::string& type,
                                                                                  double owner) const {
  Impl& I = *impl;
  Writer& W = I.W;
  if (type == "MULTILEADER" || type == "MLEADER") {
    const Value& ctx = ref(e, "context");
    if (!ctx.truthy() || !ref(ctx, "roots").is_array()) {
      return std::string("MULTILEADER without its leaders and content");
    }
    auto rec = std::make_shared<Rec>();
    rec->v = W.common(e, "MULTILEADER", owner);
    rec->aux = std::make_shared<const Value*>(&e);
    auto R = std::make_shared<MlRefs>();
    Writer* Wp = &W;
    R->h = [Wp](const Value& hex) { return hex.truthy() ? Wp->src(hex) : 0.0; };
    const auto block_h = [Wp](const Value& hex, const Value& name) {
      BlockW const* B = nullptr;  // looked up now, as the JS does (block records get their handles later)
      if (name.truthy()) {
        const auto it = Wp->block_by_name.find(upper(name));
        B = it == Wp->block_by_name.end() ? nullptr : it->second;
      }
      return [Wp, hex, B]() -> double {
        const double h = hex.truthy() ? Wp->src(hex) : 0.0;
        if (h != 0) {
          return h;
        }
        return B != nullptr ? B->hdr : 0.0;
      };
    };
    const auto style_text = [Wp](const Value& hex, const Value& name) -> double {
      const double h = hex.truthy() ? Wp->src(hex) : 0.0;
      if (h != 0) {
        return h;
      }
      return name.truthy() ? Wp->style_ref(name) : 0.0;
    };
    const Value e_style = ref(e, "styleHandle");
    R->style = [Wp, e_style] { return e_style.truthy() ? Wp->src(e_style) : 0.0; };
    const Value llh = ref(e, "leaderLinetypeHandle");
    const Value lln = ref(e, "leaderLinetype");
    R->leader_ltype = [Wp, llh, lln] {
      const double h = llh.truthy() ? Wp->src(llh) : 0.0;
      if (h != 0) {
        return h;
      }
      return lln.truthy() ? num(get(Wp->ltype_ref(lln), "h")) : 0.0;
    };
    R->arrowhead = block_h(ref(e, "arrowheadHandle"), ref(e, "arrowheadName"));
    const Value sth = ref(e, "styleTextStyleHandle");
    R->style_text = [style_text, sth] { return style_text(sth, Value()); };
    R->style_block = block_h(ref(e, "styleBlockHandle"), Value());
    const Value tsh = get(ref(ctx, "text"), "styleHandle");
    const Value tsn = ref(e, "textStyle");
    R->text_style = [style_text, tsh, tsn] { return style_text(tsh, tsn); };
    const Value& blk = ref(ctx, "block");
    const Value bn = nullish(get(blk, "name")) ? ref(e, "contentBlock") : get(blk, "name");
    R->content_block = block_h(get(blk, "blockHandle"), bn);
    rec->ml = R;
    rec->v.set("mleaderStyleSrc", ref(e, "styleHandle"));
    rec->encode = encode_mleader;
    return std::vector<RecPtr>{rec};
  }
  if ((type == "UNKNOWN" || type == "ACAD_PROXY_ENTITY") && ref(e, "raw").truthy()) {
    std::string name = type;
    if (type == "UNKNOWN") {
      name = ref(e, "dxfName").truthy() ? str_of(ref(e, "dxfName")) : "UNKNOWN";
    }
    const double type_num = type == "ACAD_PROXY_ENTITY" ? 0x1F2 : num(ref(e, "typeNum"), std::nan(""));
    const std::optional<ClassDef> cls = type_num >= 500 ? I.cls_of(type_num) : std::nullopt;
    const Value& raw = ref(e, "raw");
    if (!raw_usable(raw, W.version) || (type_num >= 500 && !cls)) {
      auto conv = I.plan_for(raw, type == "ACAD_PROXY_ENTITY", type_num);
      if (conv) {
        I.notes.proxied.set(name, num(ref(I.notes.proxied, name)) + 1);
        auto rec = std::make_shared<Rec>();
        rec->v = W.common(e, name, owner);
        auto a = std::make_shared<RawAux>();
        a->type_num = type_num;
        a->cls = conv->cls;
        a->conv = conv;
        a->raw = &raw;
        a->is_entity = true;
        Impl const* ip = &I;
        a->map = [ip](const Value& h) { return ip->map(h); };
        rec->aux = a;
        rec->encode = encode_raw;
        return std::vector<RecPtr>{rec};
      }
      I.notes.dropped_entities.set(name, num(ref(I.notes.dropped_entities, name)) + 1);
      return name + " (kept only when the drawing is written in its own version)";
    }
    I.notes.raw_entities.set(name, num(ref(I.notes.raw_entities, name)) + 1);
    auto rec = std::make_shared<Rec>();
    rec->v = W.common(e, name, owner);
    auto a = std::make_shared<RawAux>();
    a->type_num = type_num;
    a->cls = cls;
    a->raw = &raw;
    a->is_entity = true;
    Impl const* ip = &I;
    a->map = [ip](const Value& h) { return ip->map(h); };
    a->proxy_class = [ip](double id) { return ip->proxy_class(id); };
    rec->aux = a;
    rec->encode = encode_raw;
    return std::vector<RecPtr>{rec};
  }
  return std::nullopt;
}

namespace {

struct CopyAux {
  const Value* src = nullptr;
  std::function<double(const Value&)> ref;
  std::function<bool(const Value&)> keep;
  Array list;
  double owner_block = 0;
};

bool field_encoder(std::string_view t) {
  static const std::set<std::string_view> k = {
      "GROUP",           "DICTIONARYVAR",    "SCALE",           "IDBUFFER",     "FIELDLIST",
      "LAYER_INDEX",     "SPATIAL_FILTER",   "SORTENTSTABLE",   "PLOTSETTINGS", "MLEADERSTYLE",
      "RASTERVARIABLES", "WIPEOUTVARIABLES", "ACDBPLACEHOLDER", "FIELD"};
  return k.contains(t);
}
std::string field_type(const Value& o) {
  const std::string t = type_of(o);
  return t == "ACDBDICTIONARYWDFLT" ? std::string("DICTIONARYWDFLT") : t;
}

}  // namespace

namespace {

std::optional<ByteBuf> encode_field_copy(Rec& rec, Ctx& ctx) {
  const auto& a = *std::static_pointer_cast<CopyAux>(rec.aux);
  const Value& o = *a.src;
  const std::string t = field_type(o);
  ObjWriter w(ctx.version);
  if (t == "MLEADERSTYLE") {
    return encode_mleader_style(rec.v, o, a.ref, ctx);
  }
  if (t == "ACDBPLACEHOLDER") {
    begin(w, ctx.class_num("PLACEHOLDER"), rec.v);
    object_common(w, rec.v);
    return finish(w);
  }
  begin(w, t == "GROUP" ? 72 : ctx.class_num(t), rec.v);
  object_common(w, rec.v);
  if (t == "GROUP") {
    w.TV(nullish(ref(o, "name")) ? Value("") : ref(o, "name"));
    w.BS(nnum(ref(o, "unnamed"), 1));
    w.BS(nnum(ref(o, "selectable"), 1));
    w.BL(static_cast<double>(a.list.size()));
    for (const Value& h : a.list) {
      w.H(kHardPtr, h);
    }
  } else if (t == "DICTIONARYVAR") {
    w.RC(ivk(o, "intval"));
    w.TV(Value(str_of(ref(o, "value"))));
  } else if (t == "SCALE") {
    w.BS(0);
    w.TV(nullish(ref(o, "name")) ? Value("") : ref(o, "name"));
    w.BD(numk(o, "paperUnits", 1));
    w.BD(numk(o, "drawingUnits", 1));
    w.B(tr(o, "unitScale"));
  } else if (t == "IDBUFFER") {
    w.RC(0);
    w.BL(static_cast<double>(a.list.size()));
    for (const Value& h : a.list) {
      w.H(kSoftPtr, h);
    }
  } else if (t == "FIELDLIST") {
    w.BL(static_cast<double>(a.list.size()));
    w.B(tr(o, "unknown"));
    for (const Value& h : a.list) {
      w.H(kHardOwn, h);
    }
  } else if (t == "LAYER_INDEX") {
    w.BL(0);
    w.BL(0);
    w.BL(static_cast<double>(a.list.size()));
    for (const Value& e : a.list) {
      w.BL(ivk(e, "index"));
      w.TV(nullish(ref(e, "name")) ? Value("") : ref(e, "name"));
    }
    for (const Value& e : a.list) {
      w.H(kHardPtr, ref(e, "h"));
    }
  } else if (t == "SPATIAL_FILTER") {
    const Array& pts = arr(o, "points");
    w.BS(static_cast<double>(pts.size()));
    for (const Value& p : pts) {
      w.RD2(numk(p, "x"), numk(p, "y"));
    }
    const Value& e = ref(o, "extrusion");
    if (e.truthy()) {
      w.BD3(numk(e, "x"), numk(e, "y"), numk(e, "z", 1));
    } else {
      w.BD3(0, 0, 1);
    }
    const Value& g = ref(o, "origin");
    w.BD3(numk(g, "x"), numk(g, "y"), numk(g, "z"));
    w.BS(ivk(o, "displayBoundary"));
    for (const char* k : {"frontClip", "backClip"}) {
      if (!nullish(ref(o, k))) {
        w.BS(1);
        w.BD(numk(o, k));
      } else {
        w.BS(0);
      }
    }
    for (const char* k : {"inverseBlockTransform", "clipTransform"}) {
      const Value& m = ref(o, k);
      for (std::size_t i = 0; i < 12; ++i) {
        const Value& x = m.is_array() && i < m.as_array().size() ? m.as_array()[i] : undef();
        w.BD(num(x, i % 4 == 0 ? 1 : 0));
      }
    }
  } else if (t == "SORTENTSTABLE") {
    w.BL(static_cast<double>(a.list.size()));
    for (const Value& p : a.list) {
      w.Hd(0, p.as_array()[1]);
    }
    w.H(kSoftPtr, a.owner_block);
    for (const Value& p : a.list) {
      w.H(kSoftPtr, p.as_array()[0]);
    }
  } else if (t == "PLOTSETTINGS") {
    const Value& p = orv(ref(o, "plot"), undef());
    const auto tv = [&](std::string_view k, const char* d) {
      const Value v = get(p, k);
      w.TV(nullish(v) ? Value(d) : v);
    };
    tv("pageSetupName", "");
    tv("printer", "none_device");
    w.BS(nnum(get(p, "flags"), 688));
    const Value m = get(p, "margins");
    for (const char* k : {"left", "bottom", "right", "top"}) {
      w.BD(numk(m, k, 6.35));
    }
    w.BD(numk(p, "paperWidth", 215.9));
    w.BD(numk(p, "paperHeight", 279.4));
    tv("paperSize", "");
    w.BD2(numk(get(p, "origin"), "x"), numk(get(p, "origin"), "y"));
    w.BS(iv(get(p, "paperUnits")));
    w.BS(iv(get(p, "rotation")));
    w.BS(iv(get(p, "plotType")));
    w.BD2(numk(get(p, "windowMin"), "x"), numk(get(p, "windowMin"), "y"));
    w.BD2(numk(get(p, "windowMax"), "x"), numk(get(p, "windowMax"), "y"));
    if (!w.r2004) {
      const Value vn = get(p, "viewName");
      w.T8(nullish(vn) ? Value("") : vn);
    }
    w.BD(numk(p, "realWorldUnits", 1));
    w.BD(numk(p, "drawingUnits", 1));
    tv("styleSheet", "");
    w.BS(iv(get(p, "scaleType")));
    w.BD(numk(p, "scaleFactor", 1));
    w.BD2(numk(get(p, "imageOrigin"), "x"), numk(get(p, "imageOrigin"), "y"));
    if (w.r2004) {
      w.BS(iv(get(p, "shadePlotMode")));
      w.BS(nnum(get(p, "shadePlotRes"), 2));
      w.BS(nnum(get(p, "shadePlotDpi"), 300));
      w.H(kHardPtr, 0);
    }
    if (w.r2007) {
      w.H(kSoftPtr, 0);
    }
  } else if (t == "RASTERVARIABLES") {
    w.BL(ivk(o, "classVersion"));
    w.BS(nnum(ref(o, "displayFrame"), 1));
    w.BS(nnum(ref(o, "displayQuality"), 1));
    w.BS(ivk(o, "units"));
  } else if (t == "WIPEOUTVARIABLES") {
    w.BS(nnum(ref(o, "displayFrame"), 1));
  } else if (t == "FIELD") {
    Array kids;
    for (const Value& h : arr(o, "children")) {
      if (const double n = a.ref(h); n != 0) {
        kids.emplace_back(n);
      }
    }
    Array objs;
    for (const Value& h : arr(o, "objects")) {
      if (const double n = a.ref(h); n != 0) {
        objs.emplace_back(n);
      }
    }
    write_field(w, o, kids, objs, a.ref);
  }
  return finish(w);
}

/// XRECORD (20.4.104) with handle values and object ids mapped (nongraphic-write.js encodeXrecord).
std::optional<ByteBuf> encode_xrecord_copy(Rec& rec, Ctx& ctx) {
  const auto& a = *std::static_pointer_cast<CopyAux>(rec.aux);
  const Value& o = *a.src;
  const auto is_h = [](double c) {
    return (c >= 320 && c <= 369) || (c >= 390 && c <= 399) || c == 480 || c == 481 || c == 1005;
  };
  Array data;
  for (const Value& x : arr(o, "data")) {
    if (a.keep && !a.keep(x)) {
      continue;
    }
    const Array& p = x.as_array();
    const double c = base::js_to_number(p[0]);
    if (is_h(c)) {
      data.emplace_back(Array{p[0], hex_upper(a.ref(p.size() > 1 ? p[1] : undef()))});
    } else {
      data.push_back(x);
    }
  }
  ObjWriter w(ctx.version);
  begin(w, ctx.class_num("XRECORD"), rec.v);
  object_common(w, rec.v);
  ByteBuf b = xrecord_bytes(Value(std::move(data)), w.r2007);
  // rawXrecordBytes: the data bytes as the reader kept them raw (BL count, then the bytes), or nothing
  const auto raw_bytes = [](const Value& raw) -> std::optional<ByteBuf> {
    if (!raw.truthy()) {
      return std::nullopt;
    }
    try {
      const ByteBuf u = unb64(str_of(ref(raw, "data")));
      const auto rn = static_cast<std::uint64_t>(std::max(0.0, num(ref(raw, "n"))));
      Bits r(u, static_cast<int>(num(ref(raw, "ver"))), nullptr, 0, rn);
      const std::int32_t n = r.BL();
      if (n < 0 || static_cast<std::uint64_t>(n) * 8 > rn) {
        return std::nullopt;
      }
      return r.bytes(static_cast<std::size_t>(n));
    } catch (const std::exception&) {
      return std::nullopt;  // not framed: the decoded data stands
    }
  };
  const Value& raw = ref(o, "raw");
  if (auto rb = raw_bytes(raw); rb && !a.keep && (num(ref(raw, "ver")) >= 21) == w.r2007 && rb->size() != b.size()) {
    b = std::move(*rb);
  }
  w.BL(static_cast<double>(b.size()));
  for (const std::uint8_t x : b) {
    w.RC(x);
  }
  if (vnum(ctx.version) >= 15) {
    w.BS(nnum(ref(o, "cloning"), 1));
  }
  for (const Value& h : arr(o, "objids")) {
    w.H(kSoftPtr, a.ref(h));
  }
  return finish(w);
}

/// ensureMleaderStyle: multileaders whose style is not written get a Standard one.
void ensure_mleader_style(std::vector<RecPtr>& recs, Writer& W, const Value& mleader_eed) {
  std::vector<RecPtr> mls;
  for (const RecPtr& r : recs) {
    if (is_str(ref(r->v, "t"), "MULTILEADER") && r->ml && r->ml->style() == 0) {
      mls.push_back(r);
    }
  }
  if (mls.empty()) {
    return;
  }
  const double nod_h = W.H["nod"];
  RecPtr nod;
  for (const RecPtr& r : recs) {
    if (num(ref(r->v, "h")) == nod_h && is_str(ref(r->v, "t"), "DICTIONARY")) {
      nod = r;
      break;
    }
  }
  RecPtr dict;
  if (nod) {
    for (const Value& it : arr(nod->v, "items")) {
      if (is_str(it.as_array()[0], "ACAD_MLEADERSTYLE")) {
        const double h = num(it.as_array()[1]);
        for (const RecPtr& r : recs) {
          if (num(ref(r->v, "h")) == h) {
            dict = r;
            break;
          }
        }
        break;
      }
    }
  }
  const double style_h = W.next();
  if (!dict) {
    dict = std::make_shared<Rec>();
    dict->v = make_object({{"t", "DICTIONARY"},
                           {"h", W.next()},
                           {"owner", nod_h},
                           {"reactors", Array{nod_h}},
                           {"items", Array{}},
                           {"cloning", 1}});
    recs.push_back(dict);
    if (nod) {
      Array& items = nod->v.find("items")->as_array();
      items.emplace_back(Array{"ACAD_MLEADERSTYLE", ref(dict->v, "h")});
      std::ranges::stable_sort(items, [](const Value& a, const Value& b) {
        return js_less(a.as_array()[0].as_string(), b.as_array()[0].as_string());
      });
    }
  }
  std::string name = "Standard";
  int k = 2;
  Array& items = dict->v.find("items")->as_array();
  while (std::ranges::any_of(items, [&](const Value& x) { return is_str(x.as_array()[0], name); })) {
    name = "Standard_" + std::to_string(k++);
  }
  items.emplace_back(Array{name, style_h});
  auto rec = std::make_shared<Rec>();
  rec->v = make_object({{"t", "MLEADERSTYLE"},
                        {"h", style_h},
                        {"owner", ref(dict->v, "h")},
                        {"reactors", Array{ref(dict->v, "h")}},
                        {"eed", mleader_eed},
                        {"newFormat", true}});
  auto src =
      std::make_shared<Value>(make_object({{"description", name}, {"textStyle", "*TS"}, {"leaderLinetype", "*LT"}}));
  const double ts = W.style_ref(Value("Standard"));
  const double lt = W.H["ltByBlock"];
  rec->aux = src;
  rec->encode = [src, ts, lt](Rec& r, Ctx& ctx) {
    return encode_mleader_style(
        r.v, *src,
        [ts, lt](const Value& key) {
          if (is_str(key, "*TS")) {
            return ts;
          }
          if (is_str(key, "*LT")) {
            return lt;
          }
          return 0.0;
        },
        ctx);
  };
  recs.push_back(rec);
  for (const RecPtr& r : mls) {
    r->ml->style = [style_h] { return style_h; };
  }
}

std::string list_of(const Value& counts) {
  std::string s;
  for (const base::Member* m : base::js_members(counts.as_object())) {
    if (!(num(m->value) > 0)) {
      continue;
    }
    if (!s.empty()) {
      s += ", ";
    }
    s += base::js_string(m->value) + " " + m->key;
  }
  return s;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-size): nongraphic-write.js X.finish in one piece, as the JS reads
void CopyWriter::finish(std::vector<RecPtr>& recs, const std::vector<std::pair<Value, double>>& layouts) {
  Impl& I = *impl;
  Writer& W = I.W;
  const Value& model = W.model;
  const Array& objs = arr(model, "objects");
  const auto written = [&](const Value& hex) { return W.src(hex); };
  for (const auto& [sh, h] : layouts) {
    if (sh.truthy() && h != 0 && !W.has_src(hex_key(sh))) {
      W.map_src(sh, h);
    }
  }
  std::unordered_map<std::string, const Value*> src_ent;
  for (const Value& b : arr(model, "blocks")) {
    for (const Value& e : arr(b, "entities")) {
      const Key k = hex_key(ref(e, "handle"));
      src_ent[k ? *k : std::string("\x01null")] = &e;
    }
  }
  std::unordered_map<double, std::string> back;
  for (const auto& [k, v] : W.src_order) {
    back[v] = k;
  }
  for (const RecPtr& r : recs) {
    const Value& t = ref(r->v, "t");
    if (!(is_str(t, "INSERT") || is_str(t, "MINSERT")) || arr(r->v, "attribs").empty()) {
      continue;
    }
    const auto bk = back.find(num(ref(r->v, "h")));
    const Value* e = nullptr;
    if (bk != back.end()) {
      const auto it = src_ent.find(bk->second);
      e = it == src_ent.end() ? nullptr : it->second;
    }
    std::vector<const Value*> atts;
    if (e != nullptr) {
      for (const Value& a : arr(*e, "attribs")) {
        const Value ins = ref(a, "insert");
        if (ins.truthy() && get(ins, "x").is_number() && std::isfinite(get(ins, "x").as_number()) &&
            get(ins, "y").is_number() && std::isfinite(get(ins, "y").as_number()) &&
            base::js_to_number(ref(a, "height")) > 0 && tr(a, "tag")) {
          atts.push_back(&a);
        }
      }
    }
    const Array& ra = arr(r->v, "attribs");
    if (atts.size() == ra.size()) {
      for (std::size_t i = 0; i < atts.size(); ++i) {
        if (!W.has_src(hex_key(ref(*atts[i], "handle")))) {
          W.map_src(ref(*atts[i], "handle"), num(ra[i]));
        }
      }
    }
  }
  const Value* act = nullptr;
  for (const Value& v : arr(ref(model, "tables"), "vports")) {
    if (base::js_to_lower(str_of(orv(ref(v, "name"), Value("")))) == "*active") {
      act = &v;
      break;
    }
  }
  if (act != nullptr && W.h("vport") != 0 && !W.has_src(hex_key(ref(*act, "handle")))) {
    W.map_src(ref(*act, "handle"), W.h("vport"));
  }
  const auto nk = [](const Key& k) { return k ? *k : std::string("\x01null"); };
  std::unordered_map<std::string, const Value*> by_h;
  for (const Value& o : objs) {
    by_h[nk(hex_key(ref(o, "handle")))] = &o;
  }
  const auto byh = [&](const Value& hex) -> const Value* {
    const auto it = by_h.find(nk(hex_key(hex)));
    return it == by_h.end() ? nullptr : it->second;
  };
  std::unordered_map<std::string, std::vector<const Value*>> kids;
  for (const Value& o : objs) {
    const Key k = hex_key(ref(o, "owner"));
    if (k && !k->empty()) {
      kids[*k].push_back(&o);
    }
  }
  const auto same_ver = [&](const Value& o) { return raw_usable(ref(o, "raw"), W.version, type_of(o)); };
  std::unordered_map<const Value*, std::shared_ptr<ConvPlan>> conv_cache;
  const auto class_num_of = [&](const Value& o) -> double {
    for (const Value& c : arr(model, "classes")) {
      if (ref(c, "dxfName") == ref(o, "type")) {
        return num(ref(c, "num"), std::nan(""));
      }
    }
    return std::nan("");
  };
  const auto conv_of = [&](const Value& o) -> std::shared_ptr<ConvPlan> {
    if (!(tr(o, "undecoded") || is_str(ref(o, "type"), "ACAD_PROXY_OBJECT")) || !ref(o, "raw").truthy()) {
      return nullptr;
    }
    if (const auto it = conv_cache.find(&o); it != conv_cache.end()) {
      return it->second;
    }
    const Value& tn = ref(o, "typeNum");
    auto p = I.plan_for(ref(o, "raw"), is_str(ref(o, "type"), "ACAD_PROXY_OBJECT"),
                        nullish(tn) ? class_num_of(o) : num(tn, std::nan("")));
    conv_cache[&o] = p;
    return p;
  };
  static const std::set<std::string> writer_types = {"LAYOUT", "MLINESTYLE", "IMAGEDEF", "IMAGEDEF_REACTOR",
                                                     "VPENT_HEADER"};
  static const std::set<std::string> always_fields = {"DICTIONARY", "ACDBDICTIONARYWDFLT", "DICTIONARYWDFLT",
                                                      "GROUP",      "SORTENTSTABLE",       "XRECORD",
                                                      "IDBUFFER",   "LAYER_INDEX",         "FIELDLIST"};
  const auto can_write = [&](const Value* o) {
    if (o == nullptr) {
      return false;
    }
    const std::string t = type_of(*o);
    if (writer_types.contains(t) || t.ends_with("_CONTROL")) {
      return false;
    }
    if (always_fields.contains(t)) {
      return !tr(*o, "damaged");
    }
    if (same_ver(*o)) {
      return true;
    }
    if (field_encoder(field_type(*o)) && !tr(*o, "undecoded") && !tr(*o, "damaged")) {
      return true;
    }
    return conv_of(*o) != nullptr;
  };
  std::set<std::string> ml_handles;
  for (const auto& [k, e] : src_ent) {
    if (is_str(ref(*e, "type"), "MULTILEADER")) {
      ml_handles.insert(k);
    }
  }
  const auto stale_round_trip = [&](const Value& o) {
    if (vnum(W.version) < 24) {
      return false;
    }
    const Value* d = is_str(ref(o, "type"), "XRECORD") ? byh(ref(o, "owner")) : &o;
    if (d == nullptr || !is_str(ref(*d, "type"), "DICTIONARY") || !ml_handles.contains(nk(hex_key(ref(*d, "owner"))))) {
      return false;
    }
    const Value& en = ref(*d, "entries");
    if (d == &o) {
      if (!en.is_object() || en.as_object().empty()) {
        return false;
      }
      return std::ranges::all_of(en.as_object(), [](const base::Member& m) { return m.key == "ACAD_XREC_ROUNDTRIP"; });
    }
    return hex_key(get(en, "ACAD_XREC_ROUNDTRIP")) == hex_key(ref(o, "handle"));
  };
  const Value& hdr = ref(model, "header");
  const Value* nod_src0 = byh(ref(hdr, "DICTIONARY_NAMED_OBJECTS"));
  if (nod_src0 == nullptr) {
    for (const Value& o : objs) {
      const Key k = hex_key(ref(o, "owner"));
      if (is_str(ref(o, "type"), "DICTIONARY") && (!k || k->empty())) {
        nod_src0 = &o;
        break;
      }
    }
  }
  const Key recompose_h = nod_src0 != nullptr ? hex_key(get(ref(*nod_src0, "entries"), "ACDB_RECOMPOSE_DATA")) : Key();
  const auto recompose_keep = [&](const Value& v) {
    std::string t;
    if (const Value* o = byh(v); o != nullptr && ref(*o, "type").truthy()) {
      t = type_of(*o);
    } else if (const auto it = src_ent.find(nk(hex_key(v))); it != src_ent.end() && ref(*it->second, "type").truthy()) {
      t = type_of(*it->second);
    }
    return !t.empty() && t != "TABLESTYLE" && t != "ACAD_TABLE";
  };
  struct PlanE {
    const Value* o;
    std::variant<double, std::string> owner;
    double h = 0;
  };
  std::unordered_map<std::string, PlanE> plan;
  std::vector<std::string> plan_order;
  Value dropped = Value::object();
  std::set<std::string> dropped_h;
  std::deque<std::string> queue;
  const auto drop = [&](const std::string& k, const Value& o) {
    dropped_h.insert(k);
    const std::string t = type_of(o);
    dropped.set(t, num(ref(dropped, t)) + 1);
  };
  const auto want = [&](const Value& hex, std::variant<double, std::string> owner) {
    const Key kk = hex_key(hex);
    if (!kk || kk->empty() || plan.contains(*kk) || W.src_map.contains(*kk)) {
      return;
    }
    const std::string& k = *kk;
    const auto it = by_h.find(k);
    if (it == by_h.end()) {
      return;
    }
    const Value& o = *it->second;
    if (dropped_h.contains(k)) {
      return;
    }
    if (stale_round_trip(o) ||
        (recompose_h && k == *recompose_h && !std::ranges::any_of(arr(o, "data"), [&](const Value& x) {
           const Array& p = x.as_array();
           return is(p[0], 330) && recompose_keep(p.size() > 1 ? p[1] : undef());
         }))) {
      drop(k, o);
      return;
    }
    if (W.TBL->skip(o) || !can_write(&o)) {
      drop(k, o);
      return;
    }
    plan.emplace(k, PlanE{&o, std::move(owner)});
    plan_order.push_back(k);
    queue.push_back(k);
  };
  const Value* nod_src = nod_src0;
  std::vector<std::pair<std::string, std::string>> nod_add;
  std::vector<std::pair<std::string, std::string>> group_add;
  std::vector<std::pair<std::string, std::string>> plot_add;
  std::set<std::string> writer_nod;
  for (const RecPtr& r : recs) {
    if (num(ref(r->v, "h")) == W.h("nod") && is_str(ref(r->v, "t"), "DICTIONARY")) {
      for (const Value& it : arr(r->v, "items")) {
        writer_nod.insert(it.as_array()[0].as_string());
      }
      break;
    }
  }
  static const std::set<std::string> writer_nod_names = {"ACAD_GROUP",         "ACAD_LAYOUT",     "ACAD_MLINESTYLE",
                                                         "ACAD_PLOTSTYLENAME", "ACAD_IMAGE_DICT", "ACAD_IMAGE_VARS",
                                                         "ACAD_WIPEOUT_VARS"};
  if (nod_src != nullptr && ref(*nod_src, "entries").is_object()) {
    for (const base::Member* m : base::js_members(ref(*nod_src, "entries").as_object())) {
      const std::string& name = m->key;
      const Key hk = hex_key(m->value);
      if (name.empty() || !hk || hk->empty()) {
        continue;
      }
      if (name == "ACAD_GROUP") {
        const Value* gd = byh(m->value);
        if (gd != nullptr && ref(*gd, "entries").is_object()) {
          for (const base::Member* g : base::js_members(ref(*gd, "entries").as_object())) {
            const Key gk = hex_key(g->value);
            if (!g->key.empty() && gk && !gk->empty()) {
              want(g->value, W.h("groupDict"));
              group_add.emplace_back(g->key, *gk);
            }
          }
        }
        continue;
      }
      if (name == "ACAD_PLOTSTYLENAME") {
        const Value* pd = byh(m->value);
        if (pd != nullptr && ref(*pd, "entries").is_object()) {
          for (const base::Member* p : base::js_members(ref(*pd, "entries").as_object())) {
            const Key pk = hex_key(p->value);
            if (p->key.empty() || !pk || pk->empty()) {
              continue;
            }
            if (base::js_to_lower(p->key) == "normal") {
              if (!W.has_src(pk)) {
                W.map_src(p->value, W.h("plotStyleNormal"));
              }
              continue;
            }
            want(p->value, W.h("plotStyleDict"));
            plot_add.emplace_back(p->key, *pk);
          }
        }
        continue;
      }
      if (writer_nod_names.contains(name) && (!name.ends_with("_VARS") || writer_nod.contains(name))) {
        continue;
      }
      want(m->value, W.h("nod"));
      nod_add.emplace_back(name, *hk);
    }
  }
  // extension dictionaries of everything written that the model describes
  std::vector<const Value*> src_things;
  const Value& T = ref(model, "tables");
  if (T.is_object()) {
    for (const base::Member* m : base::js_members(T.as_object())) {
      if (m->value.is_array()) {
        for (const Value& t : m->value.as_array()) {
          src_things.push_back(&t);
        }
      }
    }
  }
  for (const Value& b : arr(model, "blocks")) {
    Value ext;
    for (const Value& r : arr(T, "blockRecords")) {
      if (hex_key(ref(r, "handle")) == hex_key(ref(b, "record"))) {
        ext = ref(r, "extDict");
        break;
      }
    }
    src_things.push_back(&I.synth.emplace_back(make_object({{"handle", ref(b, "record")}, {"extDict", ext}})));
    for (const Value& e : arr(b, "entities")) {
      src_things.push_back(&e);
      for (const Value& a : arr(e, "attribs")) {
        src_things.push_back(&a);
      }
    }
  }
  for (const Value& L : arr(model, "layouts")) {
    src_things.push_back(&L);
  }
  static const std::array<std::pair<const char*, const char*>, 10> ctl = {{
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
  }};
  for (const auto& [hv, key] : ctl) {
    const Key k = hex_key(ref(hdr, hv));
    const double hk = W.h(key);
    if (!k || k->empty() || hk == 0 ||
        std::ranges::none_of(recs, [&](const RecPtr& r) { return num(ref(r->v, "h")) == hk; })) {
      continue;
    }
    if (!W.src_map.contains(*k)) {
      W.map_src_key(*k, hk);
    }
    const Value* xd = nullptr;
    if (const auto it = kids.find(*k); it != kids.end()) {
      for (const Value* o : it->second) {
        if (type_of(*o).contains("DICTIONARY")) {
          xd = o;
          break;
        }
      }
    }
    if (xd != nullptr) {
      src_things.push_back(&I.synth.emplace_back(make_object({{"handle", *k}, {"extDict", ref(*xd, "handle")}})));
    }
  }
  std::unordered_map<double, const Value*> thing_by_new;
  for (const Value* s : src_things) {
    const double nh = W.src(ref(*s, "handle"));
    if (nh == 0) {
      continue;
    }
    if (!thing_by_new.contains(nh) || tr(*s, "extDict")) {
      thing_by_new[nh] = s;
    }
    if (tr(*s, "extDict")) {
      want(ref(*s, "extDict"), nh);
    }
  }
  for (const Value& o : objs) {
    const Key ow = hex_key(ref(o, "owner"));
    if (ow && !ow->empty() && W.src_map.contains(*ow)) {
      want(ref(o, "handle"), W.src_map.at(*ow));
    }
  }
  for (const Value& o : objs) {
    const double nh = W.src(ref(o, "handle"));
    if (nh != 0 && tr(o, "extDict") && !thing_by_new.contains(nh)) {
      thing_by_new[nh] = &o;
      want(ref(o, "extDict"), nh);
    }
  }
  while (!queue.empty()) {
    const std::string k = queue.front();
    queue.pop_front();
    const Value& o = *plan.at(k).o;
    if (tr(o, "extDict")) {
      want(ref(o, "extDict"), k);
    }
    const Value& en = ref(o, "entries");
    if (en.is_object() || en.is_array()) {
      const auto visit = [&](const Value& v) {
        const Value* c = byh(v);
        if (c != nullptr && hex_key(ref(*c, "owner")) == Key(k)) {
          want(v, k);
        }
      };
      if (en.is_array()) {
        for (const Value& v : en.as_array()) {
          visit(v);
        }
      } else {
        for (const base::Member* m : base::js_members(en.as_object())) {
          visit(m->value);
        }
      }
    }
    if (const auto it = kids.find(k); it != kids.end()) {
      for (const Value* c : it->second) {
        want(ref(*c, "handle"), k);
      }
    }
  }
  for (const Value& o : objs) {
    const Key k0 = hex_key(ref(o, "handle"));
    const std::string s0 = nk(k0);
    if (plan.contains(s0) || dropped_h.contains(s0) || (k0 && W.src_map.contains(*k0))) {
      continue;
    }
    Key h = hex_key(ref(o, "owner"));
    int n = 0;
    while (h && !h->empty() && !dropped_h.contains(*h) && by_h.contains(*h) && !plan.contains(*h) && n++ < 64) {
      h = hex_key(ref(*by_h.at(*h), "owner"));
    }
    if (h && !h->empty() && dropped_h.contains(*h)) {
      drop(s0, o);
    }
  }
  // handles
  std::vector<std::string> keys = plan_order;
  const auto hexv = [](const std::string& s) {
    double v = 0;
    for (const char c : s) {
      int d = -1;
      if (c >= '0' && c <= '9') {
        d = c - '0';
      } else if (c >= 'A' && c <= 'F') {
        d = c - 'A' + 10;
      } else if (c >= 'a' && c <= 'f') {
        d = c - 'a' + 10;
      }
      if (d < 0) {
        break;
      }
      v = v * 16 + d;
    }
    return v;
  };
  std::ranges::stable_sort(keys, [&](const std::string& a, const std::string& b) { return hexv(a) < hexv(b); });
  for (const std::string& k : keys) {
    PlanE& p = plan.at(k);
    p.h = W.next();
    W.map_src_key(k, p.h);
  }
  const auto refs = [&](const Value& list) {
    Array out;
    if (list.is_array()) {
      for (const Value& x : list.as_array()) {
        if (const double n = written(x); n != 0) {
          out.emplace_back(n);
        }
      }
    }
    return out;
  };
  double raw_n = 0;
  double fields_n = 0;
  Value by_type = Value::object();
  Impl const* ip = &I;
  const auto ref_fn = [ip](const Value& hex) { return ip->W.src(hex); };
  for (const std::string& k : keys) {
    PlanE& p = plan.at(k);
    const Value& o = *p.o;
    const std::string ot = type_of(o);
    auto rec = std::make_shared<Rec>();
    const double owner = std::holds_alternative<double>(p.owner) ? std::get<double>(p.owner)
                                                                 : W.src(Value(std::get<std::string>(p.owner)));
    Array reactors = refs(ref(o, "reactors"));
    rec->v = make_object({{"t", "COPY:" + ot}, {"h", p.h}, {"owner", owner}});
    if (!reactors.empty()) {
      rec->v.set("reactors", std::move(reactors));
    }
    if (tr(o, "extDict")) {
      const double x = written(ref(o, "extDict"));
      rec->v.set("xdic", x != 0 ? Value(x) : Value());
    }
    if (const auto it = I.eed_cache.find(&o); it != I.eed_cache.end()) {
      rec->v.set("eed", it->second);
    }
    rec->v.set("newFormat", true);
    auto aux = std::make_shared<CopyAux>();
    aux->src = &o;
    aux->ref = ref_fn;
    by_type.set(ot, num(ref(by_type, ot)) + 1);
    if (ot == "DICTIONARY" || ot == "ACDBDICTIONARYWDFLT" || ot == "DICTIONARYWDFLT") {
      const bool plain = ot == "DICTIONARY";
      rec->v.set("t", plain ? "DICTIONARY" : "DICTIONARYWDFLT");
      Array items;
      if (const Value& en = ref(o, "entries"); en.is_object()) {
        for (const base::Member* m : base::js_members(en.as_object())) {
          if (const double n = written(m->value); !m->key.empty() && n != 0) {
            items.emplace_back(Array{m->key, n});
          }
        }
      }
      rec->v.set("items", std::move(items));
      rec->v.set("hardOwner", tr(o, "hardOwner"));
      rec->v.set("cloning", nnum(ref(o, "cloning"), 1));
      if (!plain) {
        rec->v.set("defaultEntry", written(ref(o, "defaultEntry")));
      }
      fields_n += 1;
    } else if (ot == "XRECORD") {
      rec->encode = encode_xrecord_copy;
      fields_n += 1;
      if (recompose_h && k == *recompose_h) {
        // recomposeKeep reads the source model only: settled now for this xrecord's entries
        auto ok = std::make_shared<std::set<std::string>>();
        for (const Value& x : arr(o, "data")) {
          const Array& pp = x.as_array();
          if (is(pp[0], 330) && pp.size() > 1 && recompose_keep(pp[1])) {
            ok->insert(nk(hex_key(pp[1])));
          }
        }
        Writer const* Wp = &W;
        aux->keep = [Wp, ok, nk](const Value& x) {
          const Array& pp = x.as_array();
          if (!is(pp[0], 330)) {
            return true;
          }
          const Value& v = pp.size() > 1 ? pp[1] : undef();
          return ok->contains(nk(hex_key(v))) && Wp->src(v) != 0;
        };
      }
    } else if (!same_ver(o) && !always_fields.contains(ot) && (!field_encoder(field_type(o)) || tr(o, "undecoded")) &&
               conv_of(o) != nullptr) {
      auto conv = conv_of(o);
      auto a = std::make_shared<RawAux>();
      a->conv = conv;
      a->cls = conv->cls;
      a->raw = &ref(o, "raw");
      a->map = ref_fn;
      a->is_entity = false;
      rec->aux = a;
      rec->encode = encode_raw;
      I.notes.proxied.set(ot, num(ref(I.notes.proxied, ot)) + 1);
      raw_n += 1;
      recs.push_back(rec);
      continue;
    } else if (always_fields.contains(ot) || !same_ver(o)) {
      const std::string ft = field_type(o);
      rec->encode = encode_field_copy;
      if (ft == "GROUP") {
        aux->list = refs(ref(o, "entities"));
      } else if (ft == "IDBUFFER") {
        aux->list = refs(ref(o, "ids"));
      } else if (ft == "FIELDLIST") {
        aux->list = refs(ref(o, "fields"));
      } else if (ft == "LAYER_INDEX") {
        for (const Value& e : arr(o, "entries")) {
          const double h = written(ref(e, "handle"));
          if (h != 0) {
            Value x = e;
            x.set("h", h);
            aux->list.push_back(std::move(x));
          }
        }
      } else if (ft == "SORTENTSTABLE") {
        const Key block_rec = hex_key(ref(o, "ownerBlock"));
        aux->owner_block = block_rec ? W.src(Value(*block_rec)) : 0;
        const Value* b = nullptr;
        for (const Value& x : arr(model, "blocks")) {
          if (hex_key(ref(x, "record")) == block_rec) {
            b = &x;
            break;
          }
        }
        std::unordered_map<std::string, double> sort_of;
        const Array& ents = arr(o, "entities");
        const Array& sh = arr(o, "sortHandles");
        for (std::size_t i = 0; i < ents.size(); ++i) {
          const Value& sv = i < sh.size() && sh[i].truthy() ? sh[i] : ents[i];
          sort_of[nk(hex_key(ents[i]))] = hexv(str_of(sv));
        }
        struct E {
          double src, key, h;
        };
        std::vector<E> es;
        if (b != nullptr) {
          for (const Value& e : arr(*b, "entities")) {
            const double s = hexv(str_of(ref(e, "handle")));
            const auto it = sort_of.find(nk(hex_key(ref(e, "handle"))));
            const double h = written(ref(e, "handle"));
            if (h != 0) {
              es.push_back({s, it != sort_of.end() ? it->second : s, h});
            }
          }
        }
        std::ranges::stable_sort(es,
                                 [](const E& a, const E& c) { return a.key != c.key ? a.key < c.key : a.src < c.src; });
        std::vector<double> new_hs;
        new_hs.reserve(es.size());
        for (const E& x : es) {
          new_hs.push_back(x.h);
        }
        std::ranges::stable_sort(new_hs);
        for (std::size_t i = 0; i < es.size(); ++i) {
          aux->list.emplace_back(Array{es[i].h, new_hs[i]});
        }
      }
      fields_n += 1;
    } else {
      auto a = std::make_shared<RawAux>();
      a->raw = &ref(o, "raw");
      a->map = ref_fn;
      a->is_entity = false;
      const Value& tn = ref(o, "typeNum");
      if (ot == "ACAD_PROXY_OBJECT") {
        a->type_num = 0x1F3;
      } else {
        a->type_num = nullish(tn) ? class_num_of(o) : num(tn, std::nan(""));
      }
      a->cls = a->type_num >= 500 ? I.cls_of(a->type_num) : std::nullopt;
      a->proxy_class = [ip](double id) { return ip->proxy_class(id); };
      if (std::isnan(a->type_num) || (a->type_num >= 500 && !a->cls)) {
        by_type.set(ot, num(ref(by_type, ot)) - 1);
        dropped.set(ot, num(ref(dropped, ot)) + 1);
        continue;
      }
      rec->aux = a;
      rec->encode = encode_raw;
      raw_n += 1;
      recs.push_back(rec);
      continue;
    }
    rec->aux = aux;
    recs.push_back(rec);
  }
  // the *Active viewport owns its SUN
  if (act != nullptr && W.h("vport") != 0) {
    const Value* sun = nullptr;
    for (const Value& o : objs) {
      if (is_str(ref(o, "type"), "SUN") && hex_key(ref(o, "owner")) == hex_key(ref(*act, "handle"))) {
        sun = &o;
        break;
      }
    }
    RecPtr vr;
    for (const RecPtr& r : recs) {
      if (num(ref(r->v, "h")) == W.h("vport") && is_str(ref(r->v, "t"), "VPORT")) {
        vr = r;
        break;
      }
    }
    if (sun != nullptr && vr && written(ref(*sun, "handle")) != 0) {
      vr->v.set("sun", written(ref(*sun, "handle")));
    }
  }
  ensure_mleader_style(recs, W, I.mleader_eed);
  // extension dictionaries and reactors of written entities, table entries, block records and layouts
  double linked = 0;
  const auto eed_type = [](const std::string& t) {
    return t == "LAYER" || t == "STYLE" || t == "LTYPE" || t == "DIMSTYLE" || t == "BLOCK_HEADER" || t == "VPORT" ||
           t == "VIEW";
  };
  for (const RecPtr& r : recs) {
    const auto it = thing_by_new.find(num(ref(r->v, "h")));
    if (it == thing_by_new.end()) {
      continue;
    }
    const std::string t = ref(r->v, "t").is_string() ? ref(r->v, "t").as_string() : std::string();
    if (t.starts_with("COPY:")) {
      continue;
    }
    const Value& s = *it->second;
    if (tr(s, "extDict") && written(ref(s, "extDict")) != 0 && !tr(r->v, "xdic")) {
      r->v.set("xdic", written(ref(s, "extDict")));
      linked += 1;
    }
    if (!tr(r->v, "eed") && I.eed_cache.contains(&s) && eed_type(t)) {
      r->v.set("eed", I.eed_cache.at(&s));
    }
    Array rs;
    const Array& have = arr(r->v, "reactors");
    for (const Value& x : refs(ref(s, "reactors"))) {
      if (std::ranges::find(have, x) == have.end()) {
        rs.push_back(x);
      }
    }
    if (!rs.empty()) {
      Array all = have;
      for (Value& x : rs) {
        all.push_back(std::move(x));
      }
      r->v.set("reactors", std::move(all));
    }
  }
  const auto find_rec = [&](double h, std::string_view t) -> RecPtr {
    for (const RecPtr& r : recs) {
      if (num(ref(r->v, "h")) == h && is_str(ref(r->v, "t"), t)) {
        return r;
      }
    }
    return nullptr;
  };
  if (RecPtr nod = find_rec(W.h("nod"), "DICTIONARY")) {
    Array& items = nod->v.find("items")->as_array();
    std::set<std::string> have;
    for (const Value& x : items) {
      have.insert(x.as_array()[0].as_string());
    }
    for (const auto& [name, k] : nod_add) {
      const double n = written(Value(k));
      if (n != 0 && !have.contains(name)) {
        items.emplace_back(Array{name, n});
      }
    }
    std::ranges::stable_sort(items, [](const Value& a, const Value& b) {
      return js_less(a.as_array()[0].as_string(), b.as_array()[0].as_string());
    });
  }
  if (RecPtr pd = find_rec(W.h("plotStyleDict"), "DICTIONARYWDFLT")) {
    Array& items = pd->v.find("items")->as_array();
    for (const auto& [name, k] : plot_add) {
      const double n = written(Value(k));
      if (n != 0 && std::ranges::none_of(items, [&](const Value& x) { return is_str(x.as_array()[0], name); })) {
        items.emplace_back(Array{name, n});
      }
    }
  }
  for (const RecPtr& r : recs) {
    if (!is_str(ref(r->v, "t"), "LAYER")) {
      continue;
    }
    const auto it = thing_by_new.find(num(ref(r->v, "h")));
    if (it == thing_by_new.end()) {
      continue;
    }
    const Value& sl = *it->second;
    if (tr(sl, "plotStyle") && written(ref(sl, "plotStyle")) != 0) {
      r->v.set("plotstyle", written(ref(sl, "plotStyle")));
    }
    if (tr(sl, "material") && written(ref(sl, "material")) != 0) {
      r->v.set("material", written(ref(sl, "material")));
    }
  }
  if (RecPtr gd = find_rec(W.h("groupDict"), "DICTIONARY")) {
    Array& items = gd->v.find("items")->as_array();
    for (const auto& [name, k] : group_add) {
      if (const double n = written(Value(k)); n != 0) {
        items.emplace_back(Array{name, n});
      }
    }
  }
  for (const auto& [hv, key] :
       std::array<std::pair<const char*, const char*>, 5>{{{"DICTIONARY_MATERIALS", "materialsDict"},
                                                           {"DICTIONARY_COLORS", "colorsDict"},
                                                           {"DICTIONARY_VISUALSTYLE", "visualStyleDict"},
                                                           {"DICTIONARY_PLOTSETTINGS", "plotSettingsDict"},
                                                           {"CMATERIAL", "cmaterial"}}}) {
    if (const double n = written(ref(hdr, hv)); n != 0) {
      header_handles[key] = n;
    }
  }
  // report
  const auto note = [&](std::string s) { W.report.find("notes")->push(Value(std::move(s))); };
  const double total = raw_n + fields_n;
  if (total != 0) {
    note(msg("ccdwg.write.nongraphic", {{"n", total}, {"raw", raw_n}, {"fields", fields_n}}));
  }
  if (const std::string list = list_of(dropped); !list.empty()) {
    note(msg("ccdwg.write.not_copied", {{"list", list}}));
  }
  for (const base::Member* m : base::js_members(I.notes.dropped_entities.as_object())) {
    note(msg("ccdwg.write.entities_dropped", {{"n", m->value}, {"type", m->key}}));
  }
  if (!I.notes.proxied.as_object().empty()) {
    note(msg("ccdwg.write.proxied", {{"list", list_of(I.notes.proxied)}}));
  }
  if (!I.notes.raw_entities.as_object().empty()) {
    note(msg("ccdwg.write.raw", {{"list", list_of(I.notes.raw_entities)}}));
  }
  W.report.set("copied", make_object({{"objects", by_type},
                                      {"dropped", dropped},
                                      {"proxied", I.notes.proxied},
                                      {"rawEntities", I.notes.raw_entities},
                                      {"droppedEntities", I.notes.dropped_entities},
                                      {"linkedExtDicts", linked}}));
}

}  // namespace cc::dwg::w
