// SPDX-License-Identifier: Apache-2.0
// solid-write.js (REGION / 3DSOLID / BODY with their ACIS data, LIGHT), surface-write.js (the SURFACE family), the
// writing half of acis.js and acds.js (the R2013+ data storage section) in C++. The "Spec gap:" notes are in the JS.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <regex>

#include "cc/base/js.hpp"
#include "container.hpp"
#include "messages.hpp"
#include "wwriter.hpp"

namespace cc::dwg::w {

using base::make_object;

namespace {

constexpr const char* kGuid0 = "{00000000-0000-0000-0000-000000000000}";

bool is_str(const Value& v, std::string_view s) {
  return v.is_string() && v.as_string() == s;
}
std::string str_of(const Value& v) {
  if (nullish(v)) {
    return {};
  }
  return v.is_string() ? v.as_string() : base::js_string(v);
}
double solid_type(std::string_view t) {
  if (t == "REGION") {
    return 37;
  }
  if (t == "3DSOLID") {
    return 38;
  }
  if (t == "BODY") {
    return 39;
  }
  return -1;
}

// ---------------------------------------------------------------- acis.js

/// latin1: each UTF-16 unit & 0xFF
ByteBuf latin1(const std::string& s) {
  ByteBuf b;
  for (const std::uint16_t c : base::to_utf16(s)) {
    b.push_back(static_cast<std::uint8_t>(c & 0xFFU));
  }
  return b;
}
ByteBuf sat_encrypt(const std::string& s) {
  ByteBuf b = latin1(s);
  for (std::uint8_t& c : b) {
    if (c >= 0x21 && c <= 0x7E) {
      c = static_cast<std::uint8_t>(0x9F - c);
    }
  }
  return b;
}
struct Acis {
  bool sab = false;
  std::string text;
  ByteBuf bytes;
};
bool js_blank(const std::string& s) {
  return base::js_trim(s).empty();
}
std::optional<Acis> norm_acis(const Value& a) {
  if (!a.truthy()) {
    return std::nullopt;
  }
  if (a.is_string()) {
    if (js_blank(a.as_string())) {
      return std::nullopt;
    }
    return Acis{false, a.as_string(), {}};
  }
  if (is_str(ref(a, "format"), "sab")) {
    const Value& d = ref(a, "data");
    ByteBuf bytes;
    if (d.is_bytes()) {
      for (const auto b : d.as_bytes()) {
        bytes.push_back(static_cast<std::uint8_t>(b));
      }
    } else {
      bytes = unb64(d.truthy() ? str_of(d) : std::string());
    }
    if (bytes.empty()) {
      return std::nullopt;
    }
    return Acis{true, {}, std::move(bytes)};
  }
  const std::string text = str_of(ref(a, "data"));
  if (js_blank(text)) {
    return std::nullopt;
  }
  return Acis{false, text, {}};
}

struct Guid {
  double d1 = 0, d2 = 0, d3 = 0;
  std::array<std::uint8_t, 8> d4{};
};
Guid parse_guid(const Value& s) {
  static const std::regex re(R"(^\{?([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})\}?$)",
                             std::regex::icase);
  const std::string str = s.truthy() ? str_of(s) : std::string();
  std::smatch m;
  Guid g;
  if (!std::regex_match(str, m, re)) {
    return g;
  }
  const auto hex = [](const std::string& h) { return static_cast<double>(std::stoull(h, nullptr, 16)); };
  const std::string tail = m[4].str() + m[5].str();
  for (std::size_t i = 0; i < 8; ++i) {
    g.d4[i] = static_cast<std::uint8_t>(std::stoul(tail.substr(i * 2, 2), nullptr, 16));
  }
  g.d1 = hex(m[1].str());
  g.d2 = hex(m[2].str());
  g.d3 = hex(m[3].str());
  return g;
}

void write_acis_block(ObjWriter& w, const std::optional<Acis>& a) {
  if (!a) {
    w.B(true);
    return;
  }
  w.B(false);
  if (!a->sab) {
    w.B(true);
    w.BS(1);
    const ByteBuf b = sat_encrypt(a->text);
    for (std::size_t i = 0; i < b.size(); i += 4096) {
      const std::size_t n = std::min<std::size_t>(4096, b.size() - i);
      w.BL(static_cast<double>(n));
      for (std::size_t k = 0; k < n; ++k) {
        w.RC(b[i + k]);
      }
    }
    w.BL(0);
  } else {
    w.B(false);
    w.BS(2);
    for (const std::uint8_t x : a->bytes) {
      w.RC(x);
    }
  }
}

void write_wire(ObjWriter& w, const Value& x) {
  w.RC(ivk(x, "type"));
  w.BL(ivk(x, "selectionMarker"));
  w.BS(ivk(x, "color"));
  w.BL(ivk(x, "acisIndex"));
  const Array& pts = arr(x, "points");
  w.BL(static_cast<double>(pts.size()));
  for (const Value& p : pts) {
    w.P3(p);
  }
  const Value& t = ref(x, "transform");
  w.B(t.truthy());
  if (t.truthy()) {
    w.P3(ref(t, "xAxis"));
    w.P3(ref(t, "yAxis"));
    w.P3(ref(t, "zAxis"));
    w.P3(ref(t, "translation"));
    w.BD(numk(t, "scale", 1));
    w.B(tr(t, "hasRotation"));
    w.B(tr(t, "hasReflection"));
    w.B(tr(t, "hasShear"));
  }
}
void write_wires(ObjWriter& w, const Array& list) {
  w.BL(static_cast<double>(list.size()));
  for (const Value& x : list) {
    write_wire(w, x);
  }
}
void write_wireframe(ObjWriter& w, const Value& rec) {
  const Value& wf = ref(rec, "wireframe");
  if (!wf.truthy()) {
    w.B(false);
    return;
  }
  w.B(true);
  w.B(tr(wf, "point"));
  if (tr(wf, "point")) {
    w.P3(ref(wf, "point"));
  }
  w.BL(nnum(ref(wf, "isolines"), 4));
  const Array& wires = arr(wf, "wires");
  const Value& ip = ref(wf, "isolinesPresent");
  const bool present = nullish(ip) ? !wires.empty() : ip.truthy();
  w.B(present);
  if (present) {
    write_wires(w, wires);
  }
  const Array& sils = arr(rec, "silhouettes");
  w.BL(static_cast<double>(sils.size()));
  for (const Value& s : sils) {
    const Value& vid = ref(s, "viewportId");
    const double v = vid.truthy() ? base::js_to_number(vid) : 0;
    if (w.r2010) {
      w.BLL(v);
    } else {
      w.BL(v);
    }
    w.P3(ref(s, "target"));
    w.P3(ref(s, "direction"));
    w.P3(ref(s, "up"));
    w.B(tr(s, "perspective"));
    const Array& sw = arr(s, "wires");
    const Value& wp = ref(s, "wiresPresent");
    const bool sp = nullish(wp) ? !sw.empty() : wp.truthy();
    w.B(sp);
    if (sp) {
      write_wires(w, sw);
    }
  }
  w.B(true);
}

void write_guid_part(ObjWriter& w, const Value& r) {
  const Guid g = parse_guid(ref(r, "revisionGuid"));
  w.B(tr(r, "hasRevisionGuid"));
  w.BL(g.d1);
  w.BS(g.d2);
  w.BS(g.d3);
  for (const std::uint8_t x : g.d4) {
    w.RC(x);
  }
  w.BL(0);
}

struct AcisAux {
  std::optional<Acis> acis;
  std::string t;  // the type at encode time (BODY until a surface renames it)
};

std::optional<ByteBuf> encode_acis_entity(Rec& rec, Ctx& ctx) {
  const auto& a = *std::static_pointer_cast<AcisAux>(rec.aux);
  const std::string t = ref(rec.v, "t").as_string();
  ObjWriter w(ctx.version);
  begin(w, solid_type(t), rec.v);
  entity_common(w, rec.v);
  if (!(w.r2013 && tr(rec.v, "ds"))) {
    write_acis_block(w, a.acis);
  }
  write_wireframe(w, rec.v);
  if (w.r2007) {
    w.BL(0);
  }
  if (w.r2013) {
    write_guid_part(w, rec.v);
  }
  if (w.r2007 && t == "3DSOLID") {
    w.H(kSoftOwn, ref(rec.v, "history"));
  }
  return finish(w);
}

std::optional<ByteBuf> encode_light(Rec& rec, Ctx& ctx) {
  const Value& r = rec.v;
  ObjWriter w(ctx.version);
  begin(w, ctx.class_num("LIGHT"), r);
  entity_common(w, r);
  w.BL(nnum(ref(r, "lightVersion"), 1));
  w.TV(nullish(ref(r, "name")) ? Value("") : ref(r, "name"));
  w.BS(nnum(ref(r, "lightType"), 2));
  w.B(tr(r, "status"));
  w.CMC(ref(r, "lightColor").truthy() ? ref(r, "lightColor") : make_object({{"index", 7}}));
  w.B(tr(r, "plotGlyph"));
  w.BD(numk(r, "intensity", 1));
  w.P3(ref(r, "position"));
  w.P3(ref(r, "target"));
  w.BS(ivk(r, "attenuationType"));
  w.B(tr(r, "useAttenuationLimits"));
  w.BD(numk(r, "attenuationStart"));
  w.BD(numk(r, "attenuationEnd", 10));
  w.BD(numk(r, "hotspot", std::numbers::pi / 4));
  w.BD(numk(r, "falloff", std::numbers::pi / 4 + 0.0872664626));
  w.B(tr(r, "castShadows"));
  w.BS(ivk(r, "shadowType"));
  w.BS(nnum(ref(r, "shadowMapSize"), 256));
  w.RC(nnum(ref(r, "shadowMapSoftness"), 1));
  return finish(w);
}

// ---------------------------------------------------------------- surface-layout.js / surface-write.js

struct Field {
  const char* name;
  const char* op;
};
const std::vector<Field>& sweep_options() {
  static const std::vector<Field> f = {{"draftAngle", "BD"},
                                       {"startDraftDistance", "BD"},
                                       {"endDraftDistance", "BD"},
                                       {"twistAngle", "BD"},
                                       {"scaleFactor", "BD"},
                                       {"alignAngle", "BD"},
                                       {"solid", "B"},
                                       {"alignment", "BS"},
                                       {"miterOption", "BS"},
                                       {"alignStart", "B"},
                                       {"bank", "B"},
                                       {"basePointSet", "B"},
                                       {"twistRefVector", "BD3"},
                                       {"sweepTransformComputed", "B"},
                                       {"pathTransformComputed", "B"},
                                       {"sweepEntityTransform", "M"},
                                       {"pathEntityTransform", "M"}};
  return f;
}
const std::vector<Field>* surface_layout(std::string_view t) {
  static const std::vector<Field> none;
  static const std::vector<Field> extruded = [] {
    std::vector<Field> f = sweep_options();
    f.push_back({"sweepVector", "BD3"});
    f.push_back({"matrix", "M"});
    f.push_back({"profileEntity", "ENT"});
    return f;
  }();
  static const std::vector<Field> swept = [] {
    std::vector<Field> f = sweep_options();
    f.push_back({"sweepEntityMatrix", "M"});
    f.push_back({"pathEntityMatrix", "M"});
    f.push_back({"sweepEntity", "ENT"});
    f.push_back({"pathEntity", "ENT"});
    return f;
  }();
  static const std::vector<Field> revolved = {{"draftAngle", "BD"},
                                              {"startDraftDistance", "BD"},
                                              {"endDraftDistance", "BD"},
                                              {"twistAngle", "BD"},
                                              {"solid", "B"},
                                              {"closeToAxis", "B"},
                                              {"axisPoint", "BD3"},
                                              {"axisVector", "BD3"},
                                              {"revolveAngle", "BD"},
                                              {"startAngle", "BD"},
                                              {"matrix", "M"},
                                              {"revolveEntity", "ENT"}};
  static const std::vector<Field> lofted = {{"matrix", "M"},
                                            {"crossSections", "ENT"},
                                            {"guides", "ENT"},
                                            {"pathEntity", "ENTB"},
                                            {"startDraftAngle", "BD"},
                                            {"endDraftAngle", "BD"},
                                            {"startDraftMagnitude", "BD"},
                                            {"endDraftMagnitude", "BD"},
                                            {"arcLengthParameterization", "B"},
                                            {"noTwist", "B"},
                                            {"alignDirection", "B"},
                                            {"simpleSurfaces", "B"},
                                            {"solid", "B"},
                                            {"closedSurfaces", "B"},
                                            {"ruledSurface", "B"},
                                            {"virtualGuide", "B"},
                                            {"normal", "BS"}};
  static const std::vector<Field> nurbs = {{"nurbs170", "BS"}, {"nurbs290", "B"},  {"nurbs10", "BD3"},
                                           {"nurbs11", "BD3"}, {"nurbs12", "BD3"}, {"nurbs13", "BD3"}};
  if (t == "SURFACE" || t == "PLANESURFACE") {
    return &none;
  }
  if (t == "EXTRUDEDSURFACE") {
    return &extruded;
  }
  if (t == "SWEPTSURFACE") {
    return &swept;
  }
  if (t == "REVOLVEDSURFACE") {
    return &revolved;
  }
  if (t == "LOFTEDSURFACE") {
    return &lofted;
  }
  if (t == "NURBSURFACE") {
    return &nurbs;
  }
  return nullptr;
}

Value surface_default(std::string_view name) {
  if (name == "scaleFactor") {
    return 1;
  }
  if (name == "startDraftAngle") {
    return std::numbers::pi / 2;
  }
  if (name == "noTwist" || name == "alignDirection" || name == "simpleSurfaces") {
    return true;
  }
  return {};
}

void write_surface_field(ObjWriter& w, std::string_view op, const Value& v, std::string_view name) {
  const Value d = surface_default(name);
  if (op == "BD") {
    w.BD(num(v, nullish(d) ? 0 : base::js_to_number(d)));
  } else if (op == "BS") {
    w.BS(num(v, nullish(d) ? 0 : base::js_to_number(d)));
  } else if (op == "BL") {
    w.BL(num(v, 0));
  } else if (op == "B") {
    w.B((nullish(v) ? d : v).truthy());
  } else if (op == "BD3") {
    const auto z = [](double x) { return x == 0 ? 0.0 : x; };
    w.BD3(z(numk(v, "x")), z(numk(v, "y")), z(numk(v, "z")));
  } else if (op == "M") {
    const bool ok = v.is_array() && v.as_array().size() == 16 && std::ranges::all_of(v.as_array(), [](const Value& x) {
                      return x.is_number() && std::isfinite(x.as_number());
                    });
    if (ok) {
      for (const Value& x : v.as_array()) {
        w.BD(x.as_number());
      }
    } else {
      for (std::size_t i = 0; i < 16; ++i) {
        w.BD(i % 5 == 0 ? 1 : 0);
      }
    }
  }
}

std::optional<ByteBuf> encode_surface(Rec& rec, Ctx& ctx) {
  const auto& a = *std::static_pointer_cast<AcisAux>(rec.aux);
  const std::string t = ref(rec.v, "t").as_string();
  ObjWriter w(ctx.version);
  begin(w, ctx.class_num(t), rec.v);
  entity_common(w, rec.v);
  if (!(w.r2013 && tr(rec.v, "ds"))) {
    write_acis_block(w, a.acis);
  }
  write_wireframe(w, rec.v);
  if (w.r2007) {
    w.BL(0);
  }
  if (w.r2013) {
    write_guid_part(w, rec.v);
  }
  w.BS(numk(rec.v, "uIsolines", 6));
  w.BS(numk(rec.v, "vIsolines", 6));
  const Value& f = ref(rec.v, "f");
  if (const auto* layout = surface_layout(t)) {
    for (const Field& fd : *layout) {
      const std::string_view op = fd.op;
      if (op != "ENT" && op != "ENTB") {
        write_surface_field(w, op, ref(f, fd.name), fd.name);
        continue;
      }
      const Value& ent = ref(f, fd.name);
      const Value& raw = ref(ent, "raw");
      if (ent.truthy() && tr(ent, "value") && raw.truthy() && is(ref(raw, "ver"), version_num(ctx.version))) {
        if (op == "ENT") {
          w.BL(base::js_to_number(ref(ent, "value")));
        } else {
          w.B(true);
        }
        append_bits(w, unb64(str_of(ref(raw, "data"))), static_cast<std::uint64_t>(std::max(0.0, num(ref(raw, "n")))));
        break;
      }
      if (op == "ENT") {
        w.BL(0);
      } else {
        w.B(false);
      }
    }
  }
  return finish(w);
}

// ---------------------------------------------------------------- acds.js (writer)

struct Bytes {
  ByteBuf b;
  void u8(std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v & 0xFFU)); }
  void u16(std::uint32_t v) {
    u8(v);
    u8(v >> 8U);
  }
  void u32(std::uint32_t v) {
    for (unsigned k = 0; k < 4; ++k) {
      u8(v >> (8U * k));
    }
  }
  void u64(std::uint64_t v) {
    for (unsigned k = 0; k < 8; ++k) {
      b.push_back(static_cast<std::uint8_t>((v >> (8U * k)) & 0xFFU));
    }
  }
  void raw(std::span<const std::uint8_t> a) { b.insert(b.end(), a.begin(), a.end()); }
  void str0(std::string_view s) {
    for (const char c : s) {
      u8(static_cast<unsigned char>(c));
    }
    u8(0);
  }
  void pad(std::size_t mult, std::uint8_t byte) {
    while (b.size() % mult != 0) {
      b.push_back(byte);
    }
  }
  [[nodiscard]] std::size_t p() const { return b.size(); }
};

constexpr std::uint32_t kSig = 0x6472616A;
constexpr std::uint32_t kSegSig = 0xD5AC;
constexpr std::uint32_t kBlobRef = 0xBB106BB1;
constexpr std::uint8_t kPad = 0x70;
constexpr std::size_t kBlobMax = 0x40000;
constexpr std::size_t kPage = 0xFFFB0;

struct SegOpts {
  std::uint32_t unknown1 = 0, sys_align = 0, obj_align = 0;
  std::optional<std::uint32_t> size_field;
};
ByteBuf segment(std::string_view name, std::uint32_t index, std::span<const std::uint8_t> data, const SegOpts& o = {}) {
  const auto size = static_cast<std::uint32_t>((0x30 + data.size() + 0x3F) & ~std::size_t{0x3F});
  Bytes s;
  s.u16(kSegSig);
  for (std::size_t i = 0; i < 6; ++i) {
    s.u8(i < name.size() ? static_cast<unsigned char>(name[i]) : 0U);
  }
  s.u32(index);
  s.u32(o.unknown1);
  s.u32(o.size_field ? *o.size_field : size);
  s.u32(0);
  s.u32(1);
  s.u32(0);
  s.u32(o.sys_align);
  s.u32(o.obj_align);
  for (int i = 0; i < 8; ++i) {
    s.u8(0x55);
  }
  s.raw(data);
  s.pad(0x40, kPad);
  return s.b;
}

constexpr std::array<std::string_view, 6> kSchemaNames = {
    "AcDb_Thumbnail_Schema", "AcDb3DSolid_ASM_Data",          "AcDbDs::TreatedAsObjectDataSchema",
    "AcDbDs::LegacySchema",  "AcDbDs::IndexedPropertySchema", "AcDbDs::HandleAttributeSchema"};
constexpr std::array<std::string_view, 7> kPropNames = {
    "AcDbDs::ID",     "Thumbnail_Data",         "ASM_Data", "AcDbDs::TreatedAsObjectData", "AcDbDs::Legacy",
    "AcDs:Indexable", "AcDbDs::HandleAttribute"};

struct SchemaData {
  ByteBuf data;
  std::vector<std::uint32_t> offsets;
  std::size_t names_at = 0;
};
SchemaData schema_data() {
  Bytes o;
  for (int s = 0; s < 2; ++s) {
    for (const std::uint32_t f : {1U, 1U, 1U, 0U}) {
      o.u32(8);
      o.u32(f);
    }
  }
  SchemaData sd;
  const auto prop = [&](std::uint32_t flags, std::uint32_t name_idx, std::uint32_t type,
                        const std::vector<std::uint64_t>& values, std::uint32_t extra) {
    o.u32(flags);
    o.u32(name_idx);
    if ((flags & 2U) == 0) {
      o.u32(type);
    }
    if (flags == 1 || flags == 8) {
      o.u32(extra);
    }
    o.u16(static_cast<std::uint32_t>(values.size()));
    for (const std::uint64_t v : values) {
      if (type == 10) {
        o.u64(v);
      } else if (type == 7) {
        o.u8(static_cast<std::uint32_t>(v));
      }
    }
  };
  struct S {
    std::vector<std::uint64_t> idx, ids;
    std::uint32_t data_name;
  };
  for (const S& s : {S{{4, 5}, {6, 7}, 1}, S{{0, 1}, {2, 3}, 2}}) {
    sd.offsets.push_back(static_cast<std::uint32_t>(o.p()));
    o.u16(static_cast<std::uint32_t>(s.idx.size()));
    for (const std::uint64_t v : s.idx) {
      o.u64(v);
    }
    o.u16(2);
    prop(0, 0, 10, s.ids, 0);
    prop(0, s.data_name, 15, {}, 0);
  }
  for (const std::uint32_t n : {3U, 4U, 5U}) {
    sd.offsets.push_back(static_cast<std::uint32_t>(o.p()));
    o.u16(0);
    o.u16(1);
    prop(0, n, 1, {}, 0);
  }
  sd.offsets.push_back(static_cast<std::uint32_t>(o.p()));
  o.u16(0);
  o.u16(1);
  prop(8, 6, 7, {0}, 1);
  o.pad(16, 0x73);
  sd.names_at = o.p();
  o.u32(static_cast<std::uint32_t>(kPropNames.size()));
  for (const std::string_view n : kPropNames) {
    o.str0(n);
  }
  sd.data = std::move(o.b);
  return sd;
}

std::pair<ByteBuf, std::size_t> schema_index(std::uint32_t dat_seg, const std::vector<std::uint32_t>& offsets) {
  Bytes o;
  o.u32(static_cast<std::uint32_t>(offsets.size()));
  o.u32(0);
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    o.u32(static_cast<std::uint32_t>(i));
    o.u32(dat_seg);
    o.u32(offsets[i]);
  }
  o.u64(0x0AF10C);
  o.u32(8);
  o.u32(0);
  for (std::uint32_t k = 0; k < 8; ++k) {
    o.u32(dat_seg);
    o.u32(k * 8);
    o.u32(2 + (k & 3U));
  }
  o.pad(16, kPad);
  const std::size_t names_at = o.p();
  o.u32(static_cast<std::uint32_t>(kSchemaNames.size()));
  for (const std::string_view n : kSchemaNames) {
    o.str0(n);
  }
  return {std::move(o.b), names_at};
}

struct AcdsRec {
  double handle;
  ByteBuf data;
};

std::optional<ByteBuf> write_acds(std::vector<AcdsRec> records) {
  if (records.empty()) {
    return std::nullopt;
  }
  std::ranges::stable_sort(records, [](const AcdsRec& a, const AcdsRec& b) { return a.handle < b.handle; });
  struct Seg {
    std::uint32_t index;
    ByteBuf bytes;
  };
  std::vector<Seg> segs;
  std::uint32_t next_index = 2;
  struct PageRef {
    std::uint32_t index;
    std::size_t size;
  };
  std::vector<std::vector<PageRef>> blob_refs(records.size());
  for (std::size_t ri = 0; ri < records.size(); ++ri) {
    const AcdsRec& r = records[ri];
    if (r.data.size() <= kBlobMax) {
      continue;
    }
    const std::size_t n = std::max<std::size_t>(1, r.data.size() / kPage);
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t start = k * kPage;
      const std::size_t size = k == n - 1 ? r.data.size() - start : kPage;
      Bytes h;
      h.u64(r.data.size());
      h.u64(start);
      h.u32(static_cast<std::uint32_t>(k));
      h.u32(static_cast<std::uint32_t>(n));
      h.u64(size);
      h.raw(std::span<const std::uint8_t>(r.data).subspan(start, size));
      const std::uint32_t index = next_index++;
      SegOpts so;
      so.unknown1 = 1;
      so.size_field = 0x30;
      so.obj_align = static_cast<std::uint32_t>(k);
      segs.push_back({index, segment("blob01", index, h.b, so)});
      blob_refs[ri].push_back({index, size});
    }
  }
  const std::uint32_t data_index = next_index++;
  Bytes head;
  Bytes body;
  for (std::size_t ri = 0; ri < records.size(); ++ri) {
    const AcdsRec& r = records[ri];
    head.u32(20);
    head.u32(1);
    head.u64(static_cast<std::uint64_t>(r.handle));
    head.u32(static_cast<std::uint32_t>(body.p()));
    const auto& pages = blob_refs[ri];
    if (!pages.empty()) {
      body.u32(kBlobRef);
      body.u64(r.data.size());
      body.u32(static_cast<std::uint32_t>(pages.size()));
      body.u32(static_cast<std::uint32_t>(32 + pages.size() * 8));
      body.u32(static_cast<std::uint32_t>(kPage));
      body.u32(static_cast<std::uint32_t>(pages.back().size));
      body.u32(0);
      body.u32(0);
      for (const PageRef& p : pages) {
        body.u32(p.index);
        body.u32(static_cast<std::uint32_t>(p.size));
      }
    } else {
      body.u32(static_cast<std::uint32_t>(r.data.size()));
      body.raw(r.data);
    }
  }
  const std::size_t data_start = (0x30 + head.p() + 15) & ~std::size_t{15};
  Bytes dd;
  dd.raw(head.b);
  while (dd.p() < data_start - 0x30) {
    dd.u8(0x62);
  }
  dd.raw(body.b);
  {
    SegOpts so;
    so.obj_align = static_cast<std::uint32_t>(data_start >> 4U);
    segs.push_back({data_index, segment("_data_", data_index, dd.b, so)});
  }
  const std::uint32_t empty_index = next_index++;
  {
    const ByteBuf e(16, 0x62);
    SegOpts so;
    so.obj_align = 4;
    segs.push_back({empty_index, segment("_data_", empty_index, e, so)});
  }
  const std::uint32_t dat_idx = next_index++;
  Bytes di;
  di.u32(static_cast<std::uint32_t>(records.size()));
  di.u32(0);
  for (std::size_t i = 0; i < records.size(); ++i) {
    di.u32(data_index);
    di.u32(static_cast<std::uint32_t>(i * 20));
    di.u32(1);
  }
  segs.push_back({dat_idx, segment("datidx", dat_idx, di.b)});
  const std::uint32_t sch_dat = next_index++;
  const std::uint32_t sch_idx = next_index++;
  const SchemaData sd = schema_data();
  {
    SegOpts so;
    so.sys_align = static_cast<std::uint32_t>((0x30 + sd.names_at) >> 4U);
    segs.push_back({sch_dat, segment("schdat", sch_dat, sd.data, so)});
  }
  {
    const auto [si, names_at] = schema_index(sch_dat, sd.offsets);
    SegOpts so;
    so.sys_align = static_cast<std::uint32_t>((0x30 + names_at) >> 4U);
    segs.push_back({sch_idx, segment("schidx", sch_idx, si, so)});
  }
  const std::uint32_t search_idx = next_index++;
  Bytes se;
  se.u32(2);
  se.u32(1);
  se.u64(records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    se.u64(i);
  }
  se.u32(1);
  se.u32(0);
  se.u32(static_cast<std::uint32_t>(records.size()));
  for (std::size_t i = 0; i < records.size(); ++i) {
    se.u64(static_cast<std::uint64_t>(records[i].handle));
    se.u64(1);
    se.u64(i);
  }
  se.u32(0);
  se.u64(0);
  se.u32(1);
  se.u32(0);
  se.u32(0);
  segs.push_back({search_idx, segment("search", search_idx, se.b)});
  const std::uint32_t count = next_index;
  std::size_t pos = 0x80;
  std::vector<std::optional<std::pair<std::size_t, std::size_t>>> where(count);
  for (const Seg& s : segs) {
    where[s.index] = std::pair{pos, s.bytes.size()};
    pos += s.bytes.size();
  }
  const std::size_t seg_idx_off = pos;
  const std::size_t si_size = (0x30 + count * 12 + 0x3F) & ~std::size_t{0x3F};
  where[1] = std::pair{seg_idx_off, si_size};
  Bytes sx;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto wv = where[i].value_or(std::pair<std::size_t, std::size_t>{0, 0});
    sx.u64(wv.first);
    sx.u32(static_cast<std::uint32_t>(wv.second));
  }
  const ByteBuf seg_idx_bytes = segment("segidx", 1, sx.b);
  const std::size_t file_size = seg_idx_off + seg_idx_bytes.size();
  Bytes out;
  out.u32(kSig);
  out.u32(0x80);
  out.u32(2);
  out.u32(2);
  out.u32(0);
  out.u32(1);
  out.u32(static_cast<std::uint32_t>(seg_idx_off));
  out.u32(0);
  out.u32(count);
  out.u32(sch_idx);
  out.u32(dat_idx);
  out.u32(search_idx);
  out.u32(0);
  out.u32(static_cast<std::uint32_t>(file_size));
  while (out.p() < 0x80) {
    out.u8(0);
  }
  for (const Seg& s : segs) {
    out.raw(s.bytes);
  }
  out.raw(seg_idx_bytes);
  return std::move(out.b);
}

std::string lower_hex(std::uint64_t v, int width) {
  return hex_digits(v, false, width);
}

}  // namespace

// ---------------------------------------------------------------- solidWriter + surfaceWriter

struct SolidWriter::Impl {
  Writer& W;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives inside its Writer
  explicit Impl(Writer& w) : W(w) {}
  int ver = 0;
  std::vector<AcdsRec> acds;
  std::vector<RecPtr> recs;
  struct Notes {
    double sat_in_r2013 = 0, sab_in_r2000 = 0, empty = 0;
  } notes;
  struct SNotes {
    double down = 0, up = 0, embedded = 0;
  } snotes;
  std::vector<std::pair<RecPtr, const Value*>> downs, ups;
};

SolidWriter::SolidWriter(Writer& w) : impl(std::make_unique<Impl>(w)) {
  impl->ver = version_num(w.version);
}
SolidWriter::~SolidWriter() = default;

std::optional<std::vector<RecPtr>> SolidWriter::convert_solid(
    const Value& e, const std::string& type, double owner, const std::function<RecPtr(const std::string&)>& c) const {
  Impl& I = *impl;
  Writer& W = I.W;
  if (type == "LIGHT") {
    Value lc = ref(e, "lightColor").truthy() ? ref(e, "lightColor") : make_object({{"index", 7}});
    if (I.ver < 18 && get(lc, "index").is_undefined() && !get(lc, "rgb").is_undefined()) {
      lc = make_object({{"index", aci_from_rgb(base::js_to_number(get(lc, "rgb")))}});
    }
    RecPtr r = c("LIGHT");
    Value& v = r->v;
    v.set("lightVersion", ref(e, "lightVersion"));
    v.set("name", Value(str_of(ref(e, "name"))));
    v.set("lightType", nullish(ref(e, "lightType")) ? Value(2) : ref(e, "lightType"));
    v.set("status", !is_false(ref(e, "status")));
    v.set("lightColor", std::move(lc));
    v.set("plotGlyph", tr(e, "plotGlyph"));
    v.set("intensity", ref(e, "intensity"));
    v.set("position",
          base::pt3(numk(ref(e, "position"), "x"), numk(ref(e, "position"), "y"), numk(ref(e, "position"), "z")));
    v.set("target", base::pt3(numk(ref(e, "target"), "x"), numk(ref(e, "target"), "y"), numk(ref(e, "target"), "z")));
    for (const char* k : {"attenuationType", "attenuationStart", "attenuationEnd", "hotspot", "falloff", "shadowType",
                          "shadowMapSize", "shadowMapSoftness"}) {
      v.set(k, ref(e, k));
    }
    v.set("useAttenuationLimits", tr(e, "useAttenuationLimits"));
    v.set("castShadows", tr(e, "castShadows"));
    r->encode = encode_light;
    return std::vector<RecPtr>{r};
  }
  if (solid_type(type) < 0) {
    return std::nullopt;
  }
  auto a = norm_acis(ref(e, "acis"));
  if (!a) {
    I.notes.empty += 1;
  }
  RecPtr r;
  const Value& xdata = ref(e, "xdata");
  const Value xd = get(xdata, "ACAD_OBJECT_CHANGE_GUID");
  const Value& rg = ref(e, "revisionGuid");
  if (I.ver >= 27 && xd.truthy()) {
    Value x = xdata;
    x.erase("ACAD_OBJECT_CHANGE_GUID");
    Value ee = e;
    ee.set("xdata", std::move(x));
    r = std::make_shared<Rec>();
    r->v = W.common(ee, type, owner);
  } else if (I.ver >= 18 && I.ver < 27 && !xd.truthy() && tr(e, "hasRevisionGuid") && rg.truthy() &&
             !is_str(rg, kGuid0)) {
    const Guid g = parse_guid(rg);
    const auto s16 = [](double v) {
      return static_cast<double>(static_cast<std::int16_t>(static_cast<std::uint16_t>(base::js_to_int32(v) & 0xFFFF)));
    };
    Array items{Array{1070, 1}, Array{1071, static_cast<double>(base::js_to_int32(g.d1))}, Array{1070, s16(g.d2)},
                Array{1070, s16(g.d3)}};
    for (const std::uint8_t v : g.d4) {
      items.emplace_back(Array{1070, static_cast<double>(v)});
    }
    Value x = xdata.is_object() ? xdata : Value::object();
    x.set("ACAD_OBJECT_CHANGE_GUID", std::move(items));
    Value ee = e;
    ee.set("xdata", std::move(x));
    r = std::make_shared<Rec>();
    r->v = W.common(ee, type, owner);
  } else {
    r = c(type);
  }
  bool has_guid = tr(e, "hasRevisionGuid");
  Value guid = rg;
  if (guid.is_undefined() && xd.truthy() && xd.is_array()) {
    Array v;
    for (const Value& x : xd.as_array()) {
      v.push_back(x.is_array() && x.as_array().size() > 1 ? x.as_array()[1] : Value());
    }
    if (v.size() >= 12) {
      has_guid = v[0].truthy();
      const auto n = [&](std::size_t i) { return base::js_to_number(v[i]); };
      std::string s = "{" + lower_hex(base::js_to_uint32(n(1)), 8) + "-" +
                      lower_hex(static_cast<std::uint32_t>(base::js_to_int32(n(2)) & 0xFFFF), 4) + "-" +
                      lower_hex(static_cast<std::uint32_t>(base::js_to_int32(n(3)) & 0xFFFF), 4) + "-";
      for (std::size_t i = 4; i < 6; ++i) {
        s += lower_hex(static_cast<std::uint32_t>(base::js_to_int32(n(i)) & 0xFF), 2);
      }
      s += "-";
      for (std::size_t i = 6; i < 12; ++i) {
        s += lower_hex(static_cast<std::uint32_t>(base::js_to_int32(n(i)) & 0xFF), 2);
      }
      guid = s + "}";
    }
  }
  r->v.set("wireframe", ref(e, "wireframe"));
  r->v.set("silhouettes", ref(e, "silhouettes"));
  r->v.set("hasRevisionGuid", has_guid && guid.truthy());
  r->v.set("revisionGuid", guid.truthy() ? guid : Value(kGuid0));
  if (type == "3DSOLID") {
    r->v.set("historySrc", ref(e, "history"));
  }
  if (a && I.ver >= 27) {
    r->v.set("ds", true);
    if (a->sab) {
      I.acds.push_back({num(ref(r->v, "h")), a->bytes});
    } else {
      I.acds.push_back({num(ref(r->v, "h")), latin1(a->text)});
      I.notes.sat_in_r2013 += 1;
    }
  }
  if (a && I.ver < 18 && a->sab) {
    I.notes.sab_in_r2000 += 1;
  }
  auto aux = std::make_shared<AcisAux>();
  aux->acis = std::move(a);
  r->aux = aux;
  r->encode = encode_acis_entity;
  I.recs.push_back(r);
  return std::vector<RecPtr>{r};
}

std::optional<std::vector<RecPtr>> SolidWriter::convert_surface(const Value& e, const std::string& type,
                                                                double owner) const {
  Impl& I = *impl;
  Writer& W = I.W;
  if (surface_layout(type) == nullptr) {
    return std::nullopt;
  }
  std::string t = type;
  const bool down = t == "NURBSURFACE" && I.ver < 27;
  const bool up = t == "SURFACE" && I.ver >= 27 && is_str(ref(e, "savedDownFrom"), "NURBSURFACE");
  if (down) {
    t = "SURFACE";
  }
  if (up) {
    t = "NURBSURFACE";
  }
  Value src = e;
  const Value& xdata = ref(e, "xdata");
  const Value xd = get(xdata, "ACAD_OBJECT_CHANGE_GUID");
  static const std::regex zero8(R"(^\{?0{8}-)");
  if (down) {
    Value x = xdata.is_object() ? xdata : Value::object();
    x.erase("ACAD_OBJECT_CHANGE_GUID");
    src.set("xdata", std::move(x));
    src.set("hasRevisionGuid", false);
    src.set("revisionGuid", Value());
  } else if (I.ver < 18 && !xd.truthy() && tr(e, "hasRevisionGuid") && tr(e, "revisionGuid") &&
             !std::regex_search(str_of(ref(e, "revisionGuid")), zero8)) {
    const Guid g = parse_guid(ref(e, "revisionGuid"));
    const auto s16 = [](double v) {
      return static_cast<double>(static_cast<std::int16_t>(static_cast<std::uint16_t>(base::js_to_int32(v) & 0xFFFF)));
    };
    Array items{Array{1070, 1}, Array{1071, static_cast<double>(base::js_to_int32(g.d1))}, Array{1070, s16(g.d2)},
                Array{1070, s16(g.d3)}};
    for (const std::uint8_t v : g.d4) {
      items.emplace_back(Array{1070, static_cast<double>(v)});
    }
    Value x = xdata.is_object() ? xdata : Value::object();
    x.set("ACAD_OBJECT_CHANGE_GUID", std::move(items));
    src.set("xdata", std::move(x));
  }
  auto rs = convert_solid(src, "BODY", owner, [&](const std::string& tt) {
    auto rr = std::make_shared<Rec>();
    rr->v = W.common(src, tt, owner);
    return rr;
  });
  if (!rs || rs->empty()) {
    return std::nullopt;
  }
  RecPtr r = (*rs)[0];
  Value f = Value::object();
  for (const Field& fd : *surface_layout(t)) {
    f.set(fd.name, ref(e, fd.name));
    const std::string_view op = fd.op;
    const Value& ent = ref(e, fd.name);
    if ((op == "ENT" || op == "ENTB") && ent.truthy() && tr(ent, "value") &&
        !(tr(ent, "raw") && is(ref(ref(ent, "raw"), "ver"), I.ver))) {
      I.snotes.embedded += 1;
    }
  }
  r->v.set("t", t);
  r->encode = encode_surface;
  r->v.set("f", std::move(f));
  r->v.set("uIsolines", down ? Value(6) : ref(e, "uIsolines"));
  r->v.set("vIsolines", down ? Value(6) : ref(e, "vIsolines"));
  if (up) {
    static const std::regex zero(R"(^\{?0{8}-0{4}-0{4}-0{4}-0{12}\}?$)");
    if (!tr(r->v, "hasRevisionGuid") || std::regex_match(str_of(ref(r->v, "revisionGuid")), zero)) {
      std::array<std::uint8_t, 16> b{};
      W.random_bytes(b);
      std::string h;
      for (const std::uint8_t x : b) {
        h += lower_hex(x, 2);
      }
      r->v.set("revisionGuid", "{" + h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
                                   h.substr(16, 4) + "-" + h.substr(20) + "}");
    }
    r->v.set("hasRevisionGuid", true);
    I.snotes.up += 1;
    I.ups.emplace_back(r, &e);
  }
  if (down) {
    I.snotes.down += 1;
    I.downs.emplace_back(r, &e);
  }
  return rs;
}

void SolidWriter::finish() const {
  Impl& I = *impl;
  for (const RecPtr& r : I.recs) {
    if (tr(r->v, "historySrc")) {
      r->v.set("history", I.W.src(ref(r->v, "historySrc")));
    }
  }
  const auto note = [&](std::string s) { I.W.report.find("notes")->push(Value(std::move(s))); };
  if (I.notes.empty != 0) {
    note(msg("ccdwg.write.acis_empty", {{"n", I.notes.empty}}));
  }
  if (I.notes.sat_in_r2013 != 0) {
    note(msg("ccdwg.write.acis_sat", {{"n", I.notes.sat_in_r2013}}));
  }
  if (I.notes.sab_in_r2000 != 0) {
    note(msg("ccdwg.write.acis_sab", {{"n", I.notes.sab_in_r2000}}));
  }
}

void SolidWriter::finish_surfaces(std::vector<RecPtr>& recs) const {
  Impl& I = *impl;
  Writer& W = I.W;
  std::unordered_map<double, RecPtr> by_h;
  for (const RecPtr& x : recs) {
    by_h.emplace(num(ref(x->v, "h")), x);
  }
  const auto by_name = [](Array& items) {
    std::ranges::stable_sort(items, [](const Value& a, const Value& b) {
      return js_less(a.as_array()[0].as_string(), b.as_array()[0].as_string());
    });
  };
  for (const auto& [r, e] : I.downs) {
    RecPtr dict;
    if (tr(r->v, "xdic")) {
      if (const auto it = by_h.find(num(ref(r->v, "xdic"))); it != by_h.end()) {
        dict = it->second;
      }
    }
    const double xh = W.next();
    if (!dict || !is_str(ref(dict->v, "t"), "DICTIONARY")) {
      dict = std::make_shared<Rec>();
      const double rh = num(ref(r->v, "h"));
      dict->v = make_object({{"t", "DICTIONARY"},
                             {"h", W.next()},
                             {"owner", rh},
                             {"reactors", Array{rh}},
                             {"items", Array{}},
                             {"hardOwner", true},
                             {"cloning", 1}});
      recs.push_back(dict);
      r->v.set("xdic", ref(dict->v, "h"));
    }
    Array items;
    for (const Value& it : arr(dict->v, "items")) {
      if (!is_str(it.as_array()[0], "ACAD_XREC_ROUNDTRIP")) {
        items.push_back(it);
      }
    }
    items.emplace_back(Array{"ACAD_XREC_ROUNDTRIP", xh});
    by_name(items);
    dict->v.set("items", std::move(items));
    auto x = std::make_shared<Rec>();
    x->v = make_object({{"t", "XRECORD"},
                        {"h", xh},
                        {"owner", ref(dict->v, "h")},
                        {"reactors", Array{ref(dict->v, "h")}},
                        {"data", Array{Array{102, "AcadNurbSurfaceData"}, Array{90, 0}, Array{310, ""},
                                       Array{102, "AcadModlrDecomposedType"}, Array{90, 1}}},
                        {"cloning", 1}});
    recs.push_back(x);
  }
  for (const auto& [r, e] : I.ups) {
    const Value* sd = nullptr;
    for (const Value& o : arr(W.model, "objects")) {
      if (hex_key(ref(o, "handle")) == hex_key(ref(*e, "extDict"))) {
        sd = &o;
        break;
      }
    }
    const Value xs = sd != nullptr ? get(ref(*sd, "entries"), "ACAD_XREC_ROUNDTRIP") : Value();
    const double xh = xs.truthy() ? W.src(xs) : 0;
    RecPtr dict;
    if (tr(r->v, "xdic")) {
      if (const auto it = by_h.find(num(ref(r->v, "xdic"))); it != by_h.end()) {
        dict = it->second;
      }
    }
    if (xh != 0) {
      if (const auto it = std::ranges::find_if(recs, [&](const RecPtr& x) { return num(ref(x->v, "h")) == xh; });
          it != recs.end()) {
        recs.erase(it);
      }
    }
    if (dict && ref(dict->v, "items").truthy()) {
      Array items;
      for (const Value& it : arr(dict->v, "items")) {
        if (!is_str(it.as_array()[0], "ACAD_XREC_ROUNDTRIP") && !(num(it.as_array()[1]) == xh)) {
          items.push_back(it);
        }
      }
      const bool empty = items.empty();
      dict->v.set("items", std::move(items));
      if (empty) {
        if (const auto it = std::ranges::find(recs, dict); it != recs.end()) {
          recs.erase(it);
        }
        r->v.erase("xdic");
      }
    }
  }
  const auto note = [&](std::string s) { W.report.find("notes")->push(Value(std::move(s))); };
  if (I.snotes.down != 0) {
    note(msg("ccdwg.write.nurbs_down", {{"n", I.snotes.down}, {"version", W.version}}));
  }
  if (I.snotes.up != 0) {
    note(msg("ccdwg.write.nurbs_up", {{"n", I.snotes.up}}));
  }
  if (I.snotes.embedded != 0) {
    note(msg("ccdwg.write.embedded", {{"n", I.snotes.embedded}}));
  }
}

std::optional<ByteBuf> SolidWriter::data_storage() const {
  if (impl->ver < 27) {
    return std::nullopt;
  }
  return write_acds(impl->acds);
}

}  // namespace cc::dwg::w
