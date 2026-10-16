// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg objects-read.js: decode one object of AcDb:AcDbObjects into a record (ODA spec v5.4.1: 20.1 common
// object format and the R2007+ string stream, 20.2/20.4.1/20.4.2 common entity data, 20.3 type numbers, 20.4.3–
// 20.4.104 the objects, 28 extended data, 2.11 colours). The "Spec gap:" notes of the JS are kept where the code
// deals with them. Field names, order and JS types (number vs boolean) follow the JS exactly: the parity gate
// compares them.
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cc/base/checked.hpp"
#include "container.hpp"
#include "messages.hpp"
#include "record.hpp"

namespace cc::dwg {

using base::Array;
using base::make_object;

std::string handle_hex(std::int64_t h) {
  if (h == 0) {
    return {};
  }
  if (h < 0) {
    return "-" + base::hex_upper(static_cast<std::uint64_t>(-h));
  }
  return base::hex_upper(static_cast<std::uint64_t>(h));
}

std::string hex_bytes(const std::vector<std::uint8_t>& b) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string s;
  s.reserve(b.size() * 2);
  for (const std::uint8_t x : b) {
    s += kHex[x >> 4U];
    s += kHex[x & 15U];
  }
  return s;
}

// ---------------------------------------------------------------- Rec helpers
void Rec::set_h(std::string_view name, std::int64_t v) {
  for (HandleRef& r : h) {
    if (r.name == name) {
      r.is_list = false;
      r.one = v;
      r.list.clear();
      return;
    }
  }
  h.push_back(HandleRef{std::string(name), false, v, {}});
}
void Rec::set_hl(std::string_view name, std::vector<std::int64_t> v) {
  for (HandleRef& r : h) {
    if (r.name == name) {
      r.is_list = true;
      r.one = 0;
      r.list = std::move(v);
      return;
    }
  }
  h.push_back(HandleRef{std::string(name), true, 0, std::move(v)});
}
const HandleRef* Rec::href(std::string_view name) const {
  for (const HandleRef& r : h) {
    if (r.name == name) {
      return &r;
    }
  }
  return nullptr;
}
std::int64_t Rec::ref(std::string_view name) const {
  const HandleRef* r = href(name);
  return r != nullptr && !r->is_list ? r->one : 0;
}
const std::vector<std::int64_t>& Rec::refs(std::string_view name) const {
  static const std::vector<std::int64_t> kEmpty;
  const HandleRef* r = href(name);
  return r != nullptr && r->is_list ? r->list : kEmpty;
}

// ---------------------------------------------------------------- helpers
std::vector<std::int64_t> hrefs(Bits& h, std::int64_t ref, std::int64_t n) {
  if (n < 0 || n * 8 > h.left()) {
    throw DecodeError(msg("ccdwg.err.handle_count", {{"n", static_cast<double>(n)}}));
  }
  std::vector<std::int64_t> a(static_cast<std::size_t>(n));
  for (auto& x : a) {
    x = h.H(ref);
  }
  return a;
}

std::int64_t fits(const Bits& r, std::int64_t n, std::int64_t min_bits) {
  if (n < 0 || n * min_bits > r.left()) {
    throw DecodeError(msg("ccdwg.err.count_large", {{"n", static_cast<double>(n)}}));
  }
  return n;
}

namespace {

/// Handles until the stream is exhausted (bytes left < 1).
std::vector<std::int64_t> rest_handles(Bits& h, std::int64_t ref) {
  std::vector<std::int64_t> a;
  while (h.left() >= 8 && a.size() < 1000000) {
    try {
      a.push_back(h.H(ref));
    } catch (const std::exception&) {
      break;
    }
  }
  return a;
}

Value bool_v(std::uint32_t b) {
  return {b != 0};
}

/// JS v.toString(16).toUpperCase() for a non-negative integer-valued double of at most 2^64 (8 bytes read as a
/// JS number: the sum rounds to 2^64 when every byte is 0xFF).
std::string js_hex(double v) {
  if (v >= 18446744073709551616.0) {
    return "10000000000000000";
  }
  return base::hex_upper(base::clamp_cast<std::uint64_t>(v));
}

}  // namespace

std::optional<StreamRange> string_stream_start(Bytes buf, std::uint64_t endbit, std::uint64_t min_bit) {
  if (endbit < 1) {
    return std::nullopt;
  }
  Bits t(buf, 15, nullptr, endbit - 1, endbit);
  if (t.B() == 0) {
    return std::nullopt;
  }
  if (endbit < 17 + min_bit) {
    return std::nullopt;
  }
  std::uint64_t p = endbit - 1 - 16;
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
  if (size > p || p - size < min_bit) {
    return std::nullopt;
  }
  return StreamRange{p - size, p};
}

std::string_view group_type(int c) {
  if (c >= 0 && c <= 9) {
    return "str";
  }
  if (c >= 10 && c <= 39) {
    return "pt3";
  }
  if (c >= 40 && c <= 59) {
    return "dbl";
  }
  if (c >= 60 && c <= 79) {
    return "i16";
  }
  if (c >= 90 && c <= 99) {
    return "i32";
  }
  if (c == 100 || c == 101 || c == 102) {
    return "str";
  }
  if (c == 105) {
    return "hnd";
  }
  if (c >= 110 && c <= 149) {
    return c <= 139 ? "pt3" : "dbl";
  }
  if (c >= 160 && c <= 169) {
    return "i64";
  }
  if (c >= 170 && c <= 179) {
    return "i16";
  }
  if (c >= 210 && c <= 239) {
    return "pt3";
  }
  if (c >= 270 && c <= 279) {
    return "i16";
  }
  if (c >= 280 && c <= 289) {
    return "i8";
  }
  if (c >= 290 && c <= 299) {
    return "bool";
  }
  if (c >= 300 && c <= 309) {
    return "str";
  }
  if (c >= 310 && c <= 319) {
    return "bin";
  }
  if (c >= 320 && c <= 369) {
    return "hnd";
  }
  if (c >= 370 && c <= 389) {
    return "i16";
  }
  if (c >= 390 && c <= 399) {
    return "hnd";
  }
  if (c >= 400 && c <= 409) {
    return "i16";
  }
  if (c >= 410 && c <= 419) {
    return "str";
  }
  if (c >= 420 && c <= 429) {
    return "i32";
  }
  if (c >= 430 && c <= 439) {
    return "str";
  }
  if (c >= 440 && c <= 459) {
    return "i32";
  }
  if (c >= 460 && c <= 469) {
    return "dbl";
  }
  if (c >= 470 && c <= 479) {
    return "str";
  }
  if (c >= 480 && c <= 481) {
    return "hnd";
  }
  if (c == 999) {
    return "str";
  }
  if (c >= 1000 && c <= 1009) {
    return "str";
  }
  if (c >= 1010 && c <= 1059) {
    return c <= 1039 ? "pt3" : "dbl";
  }
  if (c >= 1060 && c <= 1070) {
    return "i16";
  }
  if (c == 1071) {
    return "i32";
  }
  return "";
}

namespace {

// ---------------------------------------------------------------- 20.3 fixed types
std::string_view fixed_type(int t) {
  switch (t) {
    case 1: return "TEXT";
    case 2: return "ATTRIB";
    case 3: return "ATTDEF";
    case 4: return "BLOCK";
    case 5: return "ENDBLK";
    case 6: return "SEQEND";
    case 7: return "INSERT";
    case 8: return "MINSERT";
    case 10: return "VERTEX_2D";
    case 11: return "VERTEX_3D";
    case 12: return "VERTEX_MESH";
    case 13: return "VERTEX_PFACE";
    case 14: return "VERTEX_PFACE_FACE";
    case 15: return "POLYLINE_2D";
    case 16: return "POLYLINE_3D";
    case 17: return "ARC";
    case 18: return "CIRCLE";
    case 19: return "LINE";
    case 20: return "DIMENSION_ORDINATE";
    case 21: return "DIMENSION_LINEAR";
    case 22: return "DIMENSION_ALIGNED";
    case 23: return "DIMENSION_ANG3PT";
    case 24: return "DIMENSION_ANG2LN";
    case 25: return "DIMENSION_RADIUS";
    case 26: return "DIMENSION_DIAMETER";
    case 27: return "POINT";
    case 28: return "3DFACE";
    case 29: return "POLYLINE_PFACE";
    case 30: return "POLYLINE_MESH";
    case 31: return "SOLID";
    case 32: return "TRACE";
    case 33: return "SHAPE";
    case 34: return "VIEWPORT";
    case 35: return "ELLIPSE";
    case 36: return "SPLINE";
    case 37: return "REGION";
    case 38: return "3DSOLID";
    case 39: return "BODY";
    case 40: return "RAY";
    case 41: return "XLINE";
    case 42: return "DICTIONARY";
    case 43: return "OLEFRAME";
    case 44: return "MTEXT";
    case 45: return "LEADER";
    case 46: return "TOLERANCE";
    case 47: return "MLINE";
    case 48: return "BLOCK_CONTROL";
    case 49: return "BLOCK_HEADER";
    case 50: return "LAYER_CONTROL";
    case 51: return "LAYER";
    case 52: return "STYLE_CONTROL";
    case 53: return "STYLE";
    case 56: return "LTYPE_CONTROL";
    case 57: return "LTYPE";
    case 60: return "VIEW_CONTROL";
    case 61: return "VIEW";
    case 62: return "UCS_CONTROL";
    case 63: return "UCS";
    case 64: return "VPORT_CONTROL";
    case 65: return "VPORT";
    case 66: return "APPID_CONTROL";
    case 67: return "APPID";
    case 68: return "DIMSTYLE_CONTROL";
    case 69: return "DIMSTYLE";
    case 70: return "VPENT_CONTROL";
    case 71: return "VPENT_HEADER";
    case 72: return "GROUP";
    case 73: return "MLINESTYLE";
    case 74: return "OLE2FRAME";
    case 75: return "DUMMY";
    case 76: return "LONG_TRANSACTION";
    case 77: return "LWPOLYLINE";
    case 78: return "HATCH";
    case 79: return "XRECORD";
    case 80: return "ACDBPLACEHOLDER";
    case 81: return "VBA_PROJECT";
    case 82: return "LAYOUT";
    case 0x1F2: return "ACAD_PROXY_ENTITY";
    case 0x1F3: return "ACAD_PROXY_OBJECT";
    default: return {};
  }
}

bool entity_fixed(int t) {
  return (t >= 1 && t <= 8) || (t >= 10 && t <= 41) || (t >= 43 && t <= 47) || t == 74 || t == 77 || t == 78 ||
         t == 0x1F2;
}

// ---------------------------------------------------------------- 28 EED
void read_eed(Bits& r, Rec& rec, const Ctx& ctx) {
  int n = 0;
  for (;;) {
    const std::int32_t size = r.BS();
    if (size == 0) {
      break;
    }
    Eed e;
    e.app = r.H();
    const std::uint64_t end = r.pos() + static_cast<std::uint64_t>(static_cast<std::int64_t>(size) * 8);
    try {
      while (r.pos() < end) {
        const std::uint32_t code = r.RC();
        Array item;
        switch (code) {
          case 0: {
            if (ctx.ver >= 21) {
              const std::uint32_t len = r.RSu();
              U16 s(len);
              for (auto& c : s) {
                c = static_cast<std::uint16_t>(r.RSu());
              }
              item = {1000, utf8(s)};
            } else {
              const std::uint32_t len = r.RC();
              (void)r.RSu();
              item = {1000, decode_text(ctx.cp, r.bytes(len))};
            }
            break;
          }
          case 2: item = {1002, r.RC() != 0 ? "}" : "{"}; break;
          case 3:
          case 5: {
            const std::vector<std::uint8_t> b = r.bytes(8);
            double v = 0;
            for (const std::uint8_t x : b) {
              v = v * 256 + x;
            }
            item = {static_cast<int>(1000 + code), js_hex(v)};
            break;
          }
          case 4: {
            const std::uint32_t len = r.RC();
            item = {1004, hex_bytes(r.bytes(len))};
            break;
          }
          case 10:
          case 11:
          case 12:
          case 13:
          case 14:
          case 15: item = {static_cast<int>(1000 + code), r.RD3()}; break;
          case 40:
          case 41:
          case 42: item = {static_cast<int>(1000 + code), r.RD()}; break;
          case 70: item = {1070, r.RS()}; break;
          case 71: item = {1071, r.RLs()}; break;
          default: throw DecodeError(msg("ccdwg.err.eed_code", {{"code", code}}));
        }
        e.items.emplace_back(std::move(item));
      }
    } catch (const std::exception& err) {
      rec.set("_eedError", error_text(err));
    }
    r.set_pos(end);
    rec.eed.push_back(std::move(e));
    rec.has_eed = true;
    if (++n > 10000) {
      break;
    }
  }
}

// ---------------------------------------------------------------- 20.2 / 20.4.1 common entity data
void common_entity(Bits& r, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  if (r.B() != 0) {  // graphic present
    const double n = ver >= 24 ? r.BLL() : static_cast<double>(r.RL());
    rec.gfx_pos = r.pos();
    r.set_pos(r.pos() + base::clamp_cast<std::uint64_t>(n * 8));
    rec.graphics = n;
  }
  if (ver < 15) {
    rec.r14bits = r.RL();
  }
  rec.entmode = static_cast<int>(r.BB());
  rec.nreact = r.BL();
  rec.no_xdic = ver >= 18 ? r.B() : 0;
  if (ver >= 27) {
    rec.ds = r.B();
  }
  if (ver < 15) {
    rec.bylayerlt = r.B();
  }
  // Spec gap: R2004+ files have no Nolinks bit.
  rec.nolinks = ver <= 15 ? r.B() : 1;
  Enc c = r.ENC();
  rec.color = std::move(c.color);
  if (c.has_transparency) {
    rec.transparency = c.transparency;
  }
  rec.color_book = c.has_book;
  rec.ltscale = r.BD();
  if (ver >= 15) {
    rec.ltflags = r.BB();
    rec.psflags = r.BB();
  }
  if (ver >= 21) {
    rec.matflags = r.BB();
    (void)r.RC();  // shadow
  }
  if (ver >= 24) {
    rec.vs_full = r.B();
    rec.vs_face = r.B();
    rec.vs_edge = r.B();
  }
  rec.invisible = (r.BS() & 1) != 0;
  if (ver >= 15) {
    rec.lw = r.RC();
  }
}

void common_entity_handles(Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  const std::int64_t ref = rec.handle;
  if (rec.entmode == 0) {
    rec.set_h("owner", h.H(ref));
  }
  if (rec.nreact != 0) {
    rec.set_hl("reactors", hrefs(h, ref, rec.nreact));
  }
  if (rec.no_xdic == 0) {
    rec.set_h("xdic", h.H(ref));
  }
  if (ver < 15) {
    rec.set_h("layer", h.H(ref));
    if (rec.bylayerlt == 0) {
      rec.set_h("ltype", h.H(ref));
    }
  }
  if (ver <= 15 && rec.nolinks == 0) {
    rec.set_h("prev", h.H(ref));
    rec.set_h("next", h.H(ref));
  }
  if (ver >= 18 && rec.color_book) {
    rec.set_h("colorBook", h.H(ref));
  }
  if (ver >= 15) {
    rec.set_h("layer", h.H(ref));
    if (rec.ltflags == 3U) {
      rec.set_h("ltype", h.H(ref));
    }
    if (ver >= 21 && rec.matflags == 3) {
      rec.set_h("material", h.H(ref));
    }
    if (rec.psflags == 3) {
      rec.set_h("plotstyle", h.H(ref));
    }
    if (ver >= 24) {
      if (rec.vs_full != 0) {
        rec.set_h("vsFull", h.H(ref));
      }
      if (rec.vs_face != 0) {
        rec.set_h("vsFace", h.H(ref));
      }
      if (rec.vs_edge != 0) {
        rec.set_h("vsEdge", h.H(ref));
      }
    }
  }
}

// ---------------------------------------------------------------- 20.1 common object data
void common_object(Bits& r, Rec& rec, const Ctx& ctx) {
  if (ctx.ver < 15) {
    rec.r14bits = r.RL();
  }
  rec.nreact = r.BL();
  rec.no_xdic = ctx.ver >= 18 ? r.B() : 0;
  if (ctx.ver >= 27) {
    rec.ds = r.B();
  }
}

void common_object_handles(Bits& h, Rec& rec) {
  rec.set_h("owner", h.H(rec.handle));
  if (rec.nreact != 0) {
    rec.set_hl("reactors", hrefs(h, rec.handle, rec.nreact));
  }
  if (rec.no_xdic == 0) {
    rec.set_h("xdic", h.H(rec.handle));
  }
}

void table_entry(Bits& r, Rec& rec, const Ctx& ctx) {
  rec.set("name", r.TV());
  rec.set("_flag64", r.B());
  // Spec gap: R2007+ table entries have no "xrefindex+1 BS".
  if (ctx.ver < 21) {
    rec.set("_xrefIndex", r.BS() - 1);
  }
  rec.set("_xdep", r.B());
}
void table_entry_handles(Bits& h, Rec& rec) {
  rec.set_h("xrefBlock", h.H(rec.handle));
}

// ---------------------------------------------------------------- type decoders
using Decoders = std::unordered_map<std::string_view, Decoder>;

void mtext_data(Bits& r, Bits* h, Rec* rec, Value& m, const Ctx& ctx);

// ---- 20.4.3 TEXT (and ATTRIB/ATTDEF text part)
void text_data(Bits& r, Rec& rec, const Ctx& ctx) {
  if (ctx.ver < 15) {
    const double elev = r.BD();
    rec.set("elevation", elev);
    const Value a = r.RD2();
    const Value b = r.RD2();
    rec.set("insert", base::pt3(a["x"].as_number(), a["y"].as_number(), elev));
    rec.set("align", base::pt3(b["x"].as_number(), b["y"].as_number(), elev));
    rec.set("extrusion", r.BD3());
    rec.set("thickness", r.BD());
    rec.set("oblique", r.BD());
    rec.set("rotation", r.BD());
    rec.set("height", r.BD());
    rec.set("widthFactor", r.BD());
    rec.set("value", r.TV());
    rec.set("generation", r.BS());
    rec.set("hAlign", r.BS());
    rec.set("vAlign", r.BS());
    return;
  }
  const std::uint32_t f = r.RC();
  const double elev = (f & 1U) != 0 ? 0 : r.RD();
  rec.set("elevation", elev);
  const Value ins = r.RD2();
  const double ix = ins["x"].as_number();
  const double iy = ins["y"].as_number();
  rec.set("insert", base::pt3(ix, iy, elev));
  if ((f & 2U) != 0) {
    rec.set("align", base::pt3(ix, iy, elev));
  } else {
    const double ax = r.DD(ix);
    const double ay = r.DD(iy);
    rec.set("align", base::pt3(ax, ay, elev));
  }
  rec.set("_alignGiven", (f & 2U) == 0);
  rec.set("extrusion", r.BE());
  rec.set("thickness", r.BT());
  rec.set("oblique", (f & 4U) != 0 ? 0.0 : r.RD());
  rec.set("rotation", (f & 8U) != 0 ? 0.0 : r.RD());
  rec.set("height", r.RD());
  rec.set("widthFactor", (f & 0x10U) != 0 ? 1.0 : r.RD());
  rec.set("value", r.TV());
  rec.set("generation", (f & 0x20U) != 0 ? 0 : r.BS());
  rec.set("hAlign", (f & 0x40U) != 0 ? 0 : r.BS());
  rec.set("vAlign", (f & 0x80U) != 0 ? 0 : r.BS());
}

void dec_text(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  text_data(r, rec, ctx);
  rec.set_h("style", h.H(rec.handle));
}

/// R2018 multi-line ATTRIB/ATTDEF: the embedded MTEXT from the entmode on (Spec gap: see objects-read.js).
Value embedded_mtext(Bits& r, const Ctx& ctx) {
  const int ver = ctx.ver;
  Value m = Value::object();
  (void)r.BB();
  (void)r.BL();
  (void)r.B();
  if (ver >= 27) {
    (void)r.B();
  }
  m.set("color", r.ENC().color);
  m.set("ltscale", r.BD());
  (void)r.BB();
  (void)r.BB();
  if (ver >= 21) {
    (void)r.BB();
    (void)r.RC();
  }
  if (ver >= 24) {
    (void)r.B();
    (void)r.B();
    (void)r.B();
  }
  (void)r.BS();
  (void)r.RC();
  mtext_data(r, nullptr, nullptr, m, ctx);
  return m;
}

void attrib_data(Bits& r, Rec& rec, const Ctx& ctx) {
  text_data(r, rec, ctx);
  if (ctx.ver >= 24) {
    rec.set("_attVersion", r.RC());
  }
  std::uint32_t type = 1;
  if (ctx.ver >= 32) {
    type = r.RC();
  }
  rec.set("attType", type);
  if (type > 1) {
    rec.set("mtext", embedded_mtext(r, ctx));
    const std::int32_t ann = r.BS();
    if (ann > 0) {
      (void)r.bytes(static_cast<std::size_t>(ann));
      rec.set("_annApp", true);
      (void)r.BS();
    }
    rec.set("tag", r.TV());
    (void)r.BS();
    rec.set("flags", r.RC());
    rec.set("lockPosition", bool_v(r.B()));
  } else {
    rec.set("tag", r.TV());
    rec.set("fieldLength", r.BS());
    rec.set("flags", r.RC());
    if (ctx.ver >= 21) {
      rec.set("lockPosition", bool_v(r.B()));
    }
  }
}

void dec_attrib(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  attrib_data(r, rec, ctx);
  rec.set_h("style", h.H(rec.handle));
  if (rec.get("_annApp").truthy()) {
    rec.set_h("annApp", h.H(rec.handle));
  }
}
void dec_attdef(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  attrib_data(r, rec, ctx);
  if (ctx.ver >= 24) {
    rec.set("_attdefVersion", r.RC());
  }
  rec.set("prompt", r.TV());
  rec.set_h("style", h.H(rec.handle));
  if (rec.get("_annApp").truthy()) {
    rec.set_h("annApp", h.H(rec.handle));
  }
}

// ---- 20.4.6/7/8
void dec_block(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("name", r.TV());
}
void dec_none(Bits& /*r*/, Bits& /*h*/, Rec& /*rec*/, const Ctx& /*ctx*/) {}

// ---- 20.4.9/10 INSERT, MINSERT
Value read_scale(Bits& r, const Ctx& ctx) {
  if (ctx.ver < 15) {
    const double x = r.BD();
    const double y = r.BD();
    return base::pt3(x, y, r.BD());
  }
  const std::uint32_t f = r.BB();
  if (f == 3) {
    return base::pt3(1, 1, 1);
  }
  if (f == 1) {
    const double y = r.DD(1);
    return base::pt3(1, y, r.DD(1));
  }
  if (f == 2) {
    const double x = r.RD();
    return base::pt3(x, x, x);
  }
  const double x = r.RD();
  const double y = r.DD(x);
  return base::pt3(x, y, r.DD(x));
}

void insert_data(Bits& r, Bits& h, Rec& rec, const Ctx& ctx, bool minsert) {
  rec.set("insert", r.BD3());
  rec.set("scale", read_scale(r, ctx));
  rec.set("rotation", r.BD());
  rec.set("extrusion", r.BD3());
  const std::uint32_t has_attribs = r.B();
  rec.set("_hasAttribs", has_attribs);
  std::int32_t owned = 0;
  // Spec gap: the owned count is present only when the has-ATTRIBs bit is set.
  if (ctx.ver >= 18 && has_attribs != 0) {
    owned = r.BL();
  }
  if (minsert) {
    rec.set("cols", r.BS());
    rec.set("rows", r.BS());
    rec.set("colSpacing", r.BD());
    rec.set("rowSpacing", r.BD());
  }
  rec.set_h("block", h.H(rec.handle));
  if (has_attribs != 0) {
    if (ctx.ver < 18) {
      rec.set_h("firstAttrib", h.H(rec.handle));
      rec.set_h("lastAttrib", h.H(rec.handle));
    } else {
      rec.set_hl("attribs", hrefs(h, rec.handle, owned));
    }
    rec.set_h("seqend", h.H(rec.handle));
  }
}
void dec_insert(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  insert_data(r, h, rec, ctx, false);
}
void dec_minsert(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  insert_data(r, h, rec, ctx, true);
}

// ---- 20.4.11–15 VERTEX
void dec_vertex2d(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  rec.set("flags", r.RC());
  rec.set("point", r.BD3());
  const double sw = r.BD();
  if (sw < 0) {
    rec.set("startWidth", -sw);
    rec.set("endWidth", -sw);
  } else {
    rec.set("startWidth", sw);
    rec.set("endWidth", r.BD());
  }
  rec.set("bulge", r.BD());
  if (ctx.ver >= 24) {
    rec.set("id", r.BL());
  }
  rec.set("tangent", r.BD());
}
void dec_vertex3d(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("flags", r.RC());
  rec.set("point", r.BD3());
}
void dec_pface_face(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  Array a;
  for (int i = 0; i < 4; ++i) {
    a.emplace_back(r.BS());
  }
  rec.set("indices", std::move(a));
}

// ---- 20.4.16/17/33/34 POLYLINE
void poly_owned(Bits& h, Rec& rec, const Ctx& ctx, std::int32_t owned) {
  if (ctx.ver < 18) {
    rec.set_h("firstVertex", h.H(rec.handle));
    rec.set_h("lastVertex", h.H(rec.handle));
  } else {
    rec.set_hl("vertices", hrefs(h, rec.handle, owned));
  }
  rec.set_h("seqend", h.H(rec.handle));
}
void dec_polyline2d(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("flags", r.BS());
  rec.set("curveType", r.BS());
  rec.set("startWidth", r.BD());
  rec.set("endWidth", r.BD());
  rec.set("thickness", r.BT());
  rec.set("elevation", r.BD());
  rec.set("extrusion", r.BE());
  const std::int32_t owned = ctx.ver >= 18 ? r.BL() : 0;
  poly_owned(h, rec, ctx, owned);
}
void dec_polyline3d(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("_splineFlags", r.RC());
  rec.set("_closedFlags", r.RC());
  const std::int32_t owned = ctx.ver >= 18 ? r.BL() : 0;
  poly_owned(h, rec, ctx, owned);
}
void dec_polyline_pface(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("numVertices", r.BS());
  rec.set("numFaces", r.BS());
  const std::int32_t owned = ctx.ver >= 18 ? r.BL() : 0;
  poly_owned(h, rec, ctx, owned);
}
void dec_polyline_mesh(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("flags", r.BS());
  rec.set("curveType", r.BS());
  rec.set("m", r.BS());
  rec.set("n", r.BS());
  rec.set("mDensity", r.BS());
  rec.set("nDensity", r.BS());
  const std::int32_t owned = ctx.ver >= 18 ? r.BL() : 0;
  poly_owned(h, rec, ctx, owned);
}

// ---- 20.4.18/20/21 ARC CIRCLE LINE
void dec_arc(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("center", r.BD3());
  rec.set("radius", r.BD());
  rec.set("thickness", r.BT());
  rec.set("extrusion", r.BE());
  rec.set("startAngle", r.BD());
  rec.set("endAngle", r.BD());
}
void dec_circle(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("center", r.BD3());
  rec.set("radius", r.BD());
  rec.set("thickness", r.BT());
  rec.set("extrusion", r.BE());
}
void dec_line(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  if (ctx.ver < 15) {
    rec.set("start", r.BD3());
    rec.set("end", r.BD3());
  } else {
    const std::uint32_t zzero = r.B();
    const double sx = r.RD();
    const double ex = r.DD(sx);
    const double sy = r.RD();
    const double ey = r.DD(sy);
    double sz = 0;
    double ez = 0;
    if (zzero == 0) {
      sz = r.RD();
      ez = r.DD(sz);
    }
    rec.set("start", base::pt3(sx, sy, sz));
    rec.set("end", base::pt3(ex, ey, ez));
  }
  rec.set("thickness", r.BT());
  rec.set("extrusion", r.BE());
}

// ---- 20.4.22–30 DIMENSION
void dim_common(Bits& r, Rec& rec, const Ctx& ctx) {
  if (ctx.ver >= 24) {
    rec.set("_dimVersion", r.RC());
  }
  rec.set("extrusion", r.BD3());
  const Value tm = r.RD2();
  const double elev = r.BD();
  rec.set("elevation", elev);
  rec.set("textMid", base::pt3(tm["x"].as_number(), tm["y"].as_number(), elev));
  rec.set("_flags1", r.RC());
  rec.set("text", r.TV());
  rec.set("textRotation", r.BD());
  rec.set("horizDir", r.BD());
  rec.set("insScale", r.BD3());
  rec.set("insRotation", r.BD());
  if (ctx.ver >= 15) {
    rec.set("attach", r.BS());
    rec.set("lineSpacingStyle", r.BS());
    rec.set("lineSpacing", r.BD());
    rec.set("measurement", r.BD());
  }
  if (ctx.ver >= 21) {
    rec.set("_dimUnknown", r.B());
    rec.set("flipArrow1", r.B());
    rec.set("flipArrow2", r.B());
  }
  const Value p12 = r.RD2();
  rec.set("insert", base::pt3(p12["x"].as_number(), p12["y"].as_number(), elev));
}
void dim_handles(Bits& h, Rec& rec) {
  rec.set_h("dimstyle", h.H(rec.handle));
  rec.set_h("block", h.H(rec.handle));
}
void dec_dim_ordinate(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 6);
  rec.set("defPoint", r.BD3());
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("_flags2", r.RC());
  dim_handles(h, rec);
}
void dec_dim_linear(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 0);
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("defPoint", r.BD3());
  rec.set("obliqueAngle", r.BD());
  rec.set("angle", r.BD());
  dim_handles(h, rec);
}
void dec_dim_aligned(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 1);
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("defPoint", r.BD3());
  rec.set("obliqueAngle", r.BD());
  dim_handles(h, rec);
}
void dec_dim_ang3pt(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 5);
  rec.set("defPoint", r.BD3());
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("center", r.BD3());
  dim_handles(h, rec);
}
void dec_dim_ang2ln(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 2);
  const Value p16 = r.RD2();
  rec.set("arcPoint", base::pt3(p16["x"].as_number(), p16["y"].as_number(), rec.num("elevation")));
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("center", r.BD3());
  rec.set("defPoint", r.BD3());
  dim_handles(h, rec);
}
void dec_dim_radius(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 4);
  rec.set("defPoint", r.BD3());
  rec.set("center", r.BD3());
  rec.set("leaderLen", r.BD());
  dim_handles(h, rec);
}
void dec_dim_diameter(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 3);
  rec.set("center", r.BD3());
  rec.set("defPoint", r.BD3());
  rec.set("leaderLen", r.BD());
  dim_handles(h, rec);
}
void dec_arc_dimension(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 8);
  rec.set("defPoint", r.BD3());
  rec.set("xline1", r.BD3());
  rec.set("xline2", r.BD3());
  rec.set("center", r.BD3());
  rec.set("isPartial", bool_v(r.B()));
  rec.set("arcStart", r.BD());
  rec.set("arcEnd", r.BD());
  rec.set("hasLeader", bool_v(r.B()));
  rec.set("leader1", r.BD3());
  rec.set("leader2", r.BD3());
  dim_handles(h, rec);
}
void dec_large_radial(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dim_common(r, rec, ctx);
  rec.set("dimType", 9);
  rec.set("defPoint", r.BD3());
  rec.set("chordPoint", r.BD3());
  (void)r.BD();
  rec.set("overrideCenter", r.BD3());
  rec.set("jogPoint", r.BD3());
  dim_handles(h, rec);
}

// ---- 20.4.31/32/35/36 POINT 3DFACE SOLID TRACE SHAPE
void dec_point(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("position", r.BD3());
  rec.set("thickness", r.BT());
  rec.set("extrusion", r.BE());
  rec.set("xAxisAngle", r.BD());
}
void dec_3dface(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  if (ctx.ver < 15) {
    Array pts;
    for (int i = 0; i < 4; ++i) {
      pts.push_back(r.BD3());
    }
    rec.set("points", std::move(pts));
    rec.set("invisibleEdges", r.BS());
    return;
  }
  const std::uint32_t no_flags = r.B();
  const std::uint32_t zzero = r.B();
  const double x = r.RD();
  const double y = r.RD();
  double z = 0;
  if (zzero == 0) {
    z = r.RD();
  }
  Value p1 = base::pt3(x, y, z);
  Value p2 = r.DD3(p1);
  Value p3 = r.DD3(p2);
  Value p4 = r.DD3(p3);
  rec.set("points", Array{std::move(p1), std::move(p2), std::move(p3), std::move(p4)});
  rec.set("invisibleEdges", no_flags != 0 ? 0 : r.BS());
}
void dec_solid(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("thickness", r.BT());
  const double elev = r.BD();
  rec.set("elevation", elev);
  Array pts;
  for (int i = 0; i < 4; ++i) {
    const Value p = r.RD2();
    pts.push_back(base::pt3(p["x"].as_number(), p["y"].as_number(), elev));
  }
  rec.set("points", std::move(pts));
  rec.set("extrusion", r.BE());
}
void dec_shape(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("insert", r.BD3());
  rec.set("scale", r.BD());
  rec.set("rotation", r.BD());
  rec.set("widthFactor", r.BD());
  rec.set("oblique", r.BD());
  rec.set("thickness", r.BD());
  rec.set("shapeNumber", r.BS());
  rec.set("extrusion", r.BD3());
  rec.set_h("style", h.H(rec.handle));
}

// ---- 20.4.38 VIEWPORT
void dec_viewport(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  rec.set("center", r.BD3());
  rec.set("width", r.BD());
  rec.set("height", r.BD());
  std::int32_t frozen = 0;
  if (ver >= 15) {
    rec.set("viewTarget", r.BD3());
    rec.set("viewDirection", r.BD3());
    rec.set("twist", r.BD());
    rec.set("viewHeight", r.BD());
    rec.set("lensLength", r.BD());
    rec.set("frontClip", r.BD());
    rec.set("backClip", r.BD());
    rec.set("snapAngle", r.BD());
    rec.set("viewCenter", r.RD2());
    rec.set("snapBase", r.RD2());
    rec.set("snapSpacing", r.RD2());
    rec.set("gridSpacing", r.RD2());
    rec.set("circleZoom", r.BS());
    if (ver >= 21) {
      rec.set("gridMajor", r.BS());
    }
    frozen = r.BL();
    rec.set("status", r.BL());
    rec.set("styleSheet", r.TV());
    rec.set("renderMode", r.RC());
    rec.set("ucsAtOrigin", bool_v(r.B()));
    rec.set("ucsPerViewport", bool_v(r.B()));
    rec.set("ucsOrigin", r.BD3());
    rec.set("ucsXAxis", r.BD3());
    rec.set("ucsYAxis", r.BD3());
    rec.set("ucsElevation", r.BD());
    rec.set("ucsOrthoType", r.BS());
    if (ver >= 18) {
      rec.set("shadePlotMode", r.BS());
    }
    if (ver >= 21) {
      rec.set("useDefaultLights", bool_v(r.B()));
      rec.set("defaultLightingType", r.RC());
      rec.set("brightness", r.BD());
      rec.set("contrast", r.BD());
      rec.set("ambientColor", r.CMC());
    }
  }
  if (ver < 15) {
    rec.set_h("vpentHeader", h.H(rec.handle));
  }
  if (ver >= 15) {
    rec.set_hl("frozenLayers", hrefs(h, rec.handle, frozen));
    rec.set_h("clipBoundary", h.H(rec.handle));
    if (ver == 15) {
      rec.set_h("vpentHeader", h.H(rec.handle));
    }
    rec.set_h("namedUcs", h.H(rec.handle));
    rec.set_h("baseUcs", h.H(rec.handle));
  }
  if (ver >= 21) {
    rec.set_h("background", h.H(rec.handle));
    rec.set_h("visualStyle", h.H(rec.handle));
    rec.set_h("shadePlot", h.H(rec.handle));
    rec.set_h("sun", h.H(rec.handle));
  }
}

// ---- 20.4.39/40 ELLIPSE SPLINE
void dec_ellipse(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("center", r.BD3());
  rec.set("majorAxis", r.BD3());
  rec.set("extrusion", r.BD3());
  rec.set("ratio", r.BD());
  rec.set("startParam", r.BD());
  rec.set("endParam", r.BD());
}
void dec_spline(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  std::int32_t scenario = r.BL();
  if (ctx.ver >= 27) {
    const std::int32_t f1 = r.BL();
    const std::int32_t kp = r.BL();
    rec.set("splineFlags1", f1);
    rec.set("knotParam", kp);
    // Spec gap: from R2013 the stored scenario is superseded.
    scenario = (f1 & 1) != 0 && kp != 15 ? 2 : 1;
  }
  rec.set("degree", r.BL());
  std::int64_t nknots = 0;
  std::int64_t nctrl = 0;
  std::int64_t nfit = 0;
  bool weighted = false;
  rec.set("flags", 0);
  if ((scenario & 2) != 0) {
    rec.set("fitTolerance", r.BD());
    rec.set("startTangent", r.BD3());
    rec.set("endTangent", r.BD3());
    nfit = r.BL();
  }
  if ((scenario & 1) != 0) {
    rec.set("rational", bool_v(r.B()));
    rec.set("closed", bool_v(r.B()));
    rec.set("periodic", bool_v(r.B()));
    rec.set("knotTolerance", r.BD());
    rec.set("controlTolerance", r.BD());
    nknots = r.BL();
    nctrl = r.BL();
    weighted = r.B() != 0;
  }
  rec.set("scenario", scenario);
  fits(r, nknots, 2);
  fits(r, nctrl, 6);
  fits(r, nfit, 6);
  Array knots;
  knots.reserve(static_cast<std::size_t>(nknots));
  for (std::int64_t i = 0; i < nknots; ++i) {
    knots.emplace_back(r.BD());
  }
  rec.set("knots", std::move(knots));
  Array control;
  Array weights;
  for (std::int64_t i = 0; i < nctrl; ++i) {
    control.push_back(r.BD3());
    if (weighted) {
      weights.emplace_back(r.BD());
    }
  }
  rec.set("control", std::move(control));
  if (weighted) {
    // JS: rec.weights is assigned (as an array) after control was assigned, so it follows it.
    rec.set("weights", std::move(weights));
  }
  Array fit;
  for (std::int64_t i = 0; i < nfit; ++i) {
    fit.push_back(r.BD3());
  }
  rec.set("fit", std::move(fit));
}

// ---- 20.4.42/43 RAY XLINE
void dec_ray(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("start", r.BD3());
  rec.set("direction", r.BD3());
}

// ---- 20.4.44/45 DICTIONARY
void dictionary(Bits& r, Bits& h, Rec& rec, const Ctx& ctx, bool wdflt) {
  const std::int32_t n = r.BL();
  if (n < 0 || n > 10000000) {
    throw DecodeError(msg("ccdwg.err.dictionary_size", {{"n", n}}));
  }
  if (ctx.ver == 14) {
    (void)r.RC();
  }
  if (ctx.ver >= 15) {
    rec.set("cloning", r.BS());
    rec.set("hardOwner", r.RC());
  }
  // JS: rec.names = new Array(n) — assigned before it is filled, so the key exists even when a read fails.
  rec.set("names", Array{});
  Array names;
  names.reserve(static_cast<std::size_t>(std::min(n, 100000)));
  try {
    for (std::int32_t i = 0; i < n; ++i) {
      names.emplace_back(r.TV());
    }
  } catch (...) {
    Array partial = std::move(names);
    partial.resize(static_cast<std::size_t>(n));
    rec.set("names", std::move(partial));
    throw;
  }
  rec.set("names", std::move(names));
  rec.set_hl("items", hrefs(h, rec.handle, n));
  if (wdflt) {
    rec.set_h("defaultEntry", h.H(rec.handle));
  }
}
void dec_dictionary(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dictionary(r, h, rec, ctx, false);
}
void dec_dictionary_wdflt(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  dictionary(r, h, rec, ctx, true);
}

// ---- 20.4.46 MTEXT
void mtext_data(Bits& r, Bits* h, Rec* rec, Value& m, const Ctx& ctx) {
  const int ver = ctx.ver;
  m.set("insert", r.BD3());
  m.set("extrusion", r.BD3());
  m.set("xAxis", r.BD3());
  m.set("width", r.BD());
  if (ver >= 21) {
    m.set("rectHeight", r.BD());
  }
  m.set("height", r.BD());
  m.set("attach", r.BS());
  m.set("direction", r.BS());
  m.set("_extH", r.BD());
  m.set("_extW", r.BD());
  m.set("value", r.TV());
  if (h != nullptr && rec != nullptr) {
    rec->set_h("style", h->H(rec->handle));
  }
  if (ver >= 15) {
    m.set("lineSpacingStyle", r.BS());
    m.set("lineSpacing", r.BD());
    (void)r.B();
  }
  if (ver >= 18) {
    const std::int32_t bg = r.BL();
    m.set("backgroundFlags", bg);
    if ((bg & 1) != 0 || (ver >= 32 && (bg & 0x10) != 0)) {
      // Spec gap: the background scale is a BD.
      m.set("backgroundScale", r.BD());
      m.set("backgroundColor", r.CMC());
      m.set("backgroundTransparency", r.BL());
    }
  }
  if (ver >= 32) {
    const std::uint32_t not_annotative = r.B();
    if (not_annotative != 0) {
      (void)r.BS();
      (void)r.B();
      if (h != nullptr && rec != nullptr) {
        rec->set_h("mtextApp", h->H(rec->handle));
      }
      (void)r.BL();
      (void)r.BD3();
      (void)r.BD3();
      (void)r.BD();
      (void)r.BD();
      (void)r.BD();
      (void)r.BD();
      const std::int32_t col_type = r.BS();
      if (col_type != 0) {
        Value& cols = m.set("columns", make_object({{"type", col_type}}));
        const std::int32_t count = r.BL();
        cols.set("count", count);
        cols.set("width", r.BD());
        cols.set("gutter", r.BD());
        const bool auto_height = r.B() != 0;
        cols.set("autoHeight", auto_height);
        cols.set("flowReversed", bool_v(r.B()));
        if (!auto_height && col_type == 2) {
          Array hs;
          for (std::int32_t i = 0; i < count; ++i) {
            hs.emplace_back(r.BD());
          }
          // m.set may have moved `cols`; look it up again before writing.
          m.find("columns")->set("heights", std::move(hs));
        }
      }
    }
  }
}
void dec_mtext(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  mtext_data(r, &h, &rec, rec.f, ctx);
}

// ---- 20.4.47 LEADER
void dec_leader(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  (void)r.B();
  rec.set("annotType", r.BS());
  rec.set("pathType", r.BS());
  const std::int64_t n = fits(r, r.BL(), 6);
  rec.set("vertices", Array{});
  Array vs;
  for (std::int64_t i = 0; i < n; ++i) {
    vs.push_back(r.BD3());
  }
  rec.set("vertices", std::move(vs));
  rec.set("origin", r.BD3());
  rec.set("extrusion", r.BD3());
  rec.set("xDirection", r.BD3());
  rec.set("blockOffset", r.BD3());
  if (ver >= 14) {
    rec.set("endPointProj", r.BD3());
  }
  if (ver < 15) {
    rec.set("dimgap", r.BD());
  }
  // Spec gap: R2010+ files have no box height/width here.
  if (ver < 24) {
    rec.set("boxHeight", r.BD());
    rec.set("boxWidth", r.BD());
  }
  rec.set("hookLineOnX", bool_v(r.B()));
  rec.set("arrowhead", bool_v(r.B()));
  if (ver < 15) {
    (void)r.BS();
    (void)r.BD();
    (void)r.B();
    (void)r.B();
    (void)r.BS();
    (void)r.BS();
    (void)r.B();
    (void)r.B();
  } else {
    (void)r.BS();
    (void)r.B();
    (void)r.B();
  }
  rec.set_h("annotation", h.H(rec.handle));
  rec.set_h("dimstyle", h.H(rec.handle));
}

// ---- 20.4.49 TOLERANCE, 20.4.50 MLINE
void dec_tolerance(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  if (ctx.ver < 15) {
    (void)r.BS();
    rec.set("height", r.BD());
    rec.set("dimgap", r.BD());
  }
  rec.set("insert", r.BD3());
  rec.set("xDirection", r.BD3());
  rec.set("extrusion", r.BD3());
  rec.set("text", r.TV());
  rec.set_h("dimstyle", h.H(rec.handle));
}
void dec_mline(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("scale", r.BD());
  rec.set("justification", r.RC());
  rec.set("base", r.BD3());
  rec.set("extrusion", r.BD3());
  rec.set("closed", r.BS() == 3);
  const std::uint32_t lines = r.RC();
  const std::int32_t n = r.BS();
  rec.set("vertices", Array{});
  Value* vs = rec.f.find("vertices");
  for (std::int32_t i = 0; i < n; ++i) {
    Value v = Value::object();
    v.set("point", r.BD3());
    v.set("direction", r.BD3());
    v.set("miter", r.BD3());
    Array ls;
    for (std::uint32_t k = 0; k < lines; ++k) {
      Array seg;
      const std::int32_t ns = r.BS();
      for (std::int32_t j = 0; j < ns; ++j) {
        seg.emplace_back(r.BD());
      }
      Array fill;
      const std::int32_t nf = r.BS();
      for (std::int32_t j = 0; j < nf; ++j) {
        fill.emplace_back(r.BD());
      }
      ls.push_back(make_object({{"segments", std::move(seg)}, {"fill", std::move(fill)}}));
    }
    v.set("lines", std::move(ls));
    vs->push(std::move(v));
  }
  rec.set_h("style", h.H(rec.handle));
}

// ---- 20.4.51–70 tables
void dec_control(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("count", r.BL());
  // Spec gap: R2000+ DIMSTYLE controls carry an RC after the entry count.
  if (rec.type == "DIMSTYLE_CONTROL" && ctx.ver >= 15) {
    rec.set("_extra", r.RC());
  }
  rec.set_hl("entries", rest_handles(h, rec.handle));
}

void dec_block_header(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  table_entry(r, rec, ctx);
  rec.set("anonymous", bool_v(r.B()));
  rec.set("hasAttribs", bool_v(r.B()));
  const bool is_xref = r.B() != 0;
  rec.set("isXref", is_xref);
  const bool is_overlaid = r.B() != 0;
  rec.set("isOverlaid", is_overlaid);
  if (ver >= 15) {
    rec.set("loaded", bool_v(r.B()));
  }
  std::int32_t owned = 0;
  // Spec gap: xref and overlay block records have no owned count.
  if (ver >= 18 && !is_xref && !is_overlaid) {
    owned = r.BL();
  }
  rec.set("base", r.BD3());
  rec.set("xrefPath", r.TV());
  std::int64_t inserts = 0;
  if (ver >= 15) {
    while (r.RC() != 0) {
      ++inserts;
    }
    rec.set("description", r.TV());
    const std::int32_t n = r.BL();
    if (n > 0) {
      rec.set("preview", base::ByteVec(r.bytes(static_cast<std::size_t>(n))));
    }
  }
  if (ver >= 21) {
    rec.set("insertUnits", r.BS());
    rec.set("explodable", bool_v(r.B()));
    rec.set("scalable", r.RC());
  }
  table_entry_handles(h, rec);
  rec.set_h("block", h.H(rec.handle));
  if (ver < 18) {
    if (!is_xref && !is_overlaid) {
      rec.set_h("firstEntity", h.H(rec.handle));
      rec.set_h("lastEntity", h.H(rec.handle));
    }
  } else {
    rec.set_hl("entities", hrefs(h, rec.handle, owned));
  }
  rec.set_h("endblk", h.H(rec.handle));
  if (ver >= 15) {
    rec.set_hl("inserts", hrefs(h, rec.handle, inserts));
    rec.set_h("layout", h.H(rec.handle));
  }
}

void dec_layer(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  if (ctx.ver < 15) {
    // Spec gap: the second bit is set when the layer is OFF.
    rec.set("frozen", bool_v(r.B()));
    rec.set("on", r.B() == 0);
    rec.set("frozenNew", bool_v(r.B()));
    rec.set("locked", bool_v(r.B()));
    rec.set("plot", true);
    rec.set("lineweight", -3);
  } else {
    const auto v = static_cast<std::uint32_t>(r.BS());
    rec.set("_values", static_cast<std::int32_t>(v));
    rec.set("frozen", (v & 1U) != 0);
    rec.set("on", (v & 2U) == 0);
    rec.set("frozenNew", (v & 4U) != 0);
    rec.set("locked", (v & 8U) != 0);
    rec.set("plot", (v & 16U) != 0);
    rec.set("_lwIndex", (v & 0x3E0U) >> 5U);
  }
  rec.set("color", r.CMC());
  table_entry_handles(h, rec);
  if (ctx.ver >= 15) {
    rec.set_h("plotstyle", h.H(rec.handle));
  }
  if (ctx.ver >= 21) {
    rec.set_h("material", h.H(rec.handle));
  }
  rec.set_h("ltype", h.H(rec.handle));
  // Spec gap: only R2013+ files have the trailing NULL handle.
  if (ctx.ver >= 27) {
    rec.set_h("unknown", h.H(rec.handle));
  }
}

void dec_style(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  rec.set("vertical", bool_v(r.B()));
  rec.set("isShapeFile", bool_v(r.B()));
  rec.set("height", r.BD());
  rec.set("widthFactor", r.BD());
  rec.set("oblique", r.BD());
  rec.set("generation", r.RC());
  rec.set("lastHeight", r.BD());
  rec.set("font", r.TV());
  rec.set("bigFont", r.TV());
  table_entry_handles(h, rec);
}

void dec_ltype(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  rec.set("description", r.TV());
  rec.set("patternLength", r.BD());
  rec.set("alignment", r.RC());
  const std::uint32_t n = r.RC();
  rec.set("elements", Array{});
  bool any_text = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    Value e = Value::object();
    e.set("length", r.BD());
    e.set("shape", r.BS());
    const double ox = r.RD();
    e.set("offset", base::pt2(ox, r.RD()));
    e.set("scale", r.BD());
    e.set("rotation", r.BD());
    const std::int32_t flags = r.BS();
    e.set("flags", flags);
    if ((flags & 2) != 0) {
      any_text = true;
    }
    rec.f.find("elements")->push(std::move(e));
  }
  std::vector<std::uint8_t> area;
  bool has_area = false;
  if (ctx.ver <= 18) {
    area = r.bytes(256);
    has_area = true;
  } else if (any_text) {
    area = r.bytes(512);
    has_area = true;
  }
  if (has_area) {
    for (Value& e : rec.f.find("elements")->as_array()) {
      const auto flags = base::clamp_cast<std::int32_t>(e["flags"].num_or(0));
      if ((flags & 2) == 0) {
        continue;
      }
      const auto shape = base::clamp_cast<std::int64_t>(e["shape"].num_or(0));
      U16 s;
      if (ctx.ver <= 18) {
        // text dashes keep their string in the area at the 75-group index
        for (std::int64_t k = shape;
             k >= 0 && static_cast<std::size_t>(k) < area.size() && area[static_cast<std::size_t>(k)] != 0; ++k) {
          s.push_back(area[static_cast<std::size_t>(k)]);
        }
      } else {
        for (std::int64_t k = shape * 2; k >= 0 && static_cast<std::size_t>(k) + 1 < area.size(); k += 2) {
          const auto c = static_cast<std::uint16_t>(area[static_cast<std::size_t>(k)] |
                                                    (area[static_cast<std::size_t>(k) + 1] << 8U));
          if (c == 0) {
            break;
          }
          s.push_back(c);
        }
      }
      e.set("text", utf8(s));
    }
  }
  table_entry_handles(h, rec);
  rec.set_hl("shapes", hrefs(h, rec.handle, n));
}

void dec_view(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  table_entry(r, rec, ctx);
  rec.set("height", r.BD());
  rec.set("width", r.BD());
  rec.set("center", r.RD2());
  rec.set("target", r.BD3());
  rec.set("direction", r.BD3());
  rec.set("twist", r.BD());
  rec.set("lensLength", r.BD());
  rec.set("frontClip", r.BD());
  rec.set("backClip", r.BD());
  rec.set("viewMode", r.bits(4));
  if (ver >= 15) {
    rec.set("renderMode", r.RC());
  }
  if (ver >= 21) {
    rec.set("useDefaultLights", bool_v(r.B()));
    rec.set("defaultLightingType", r.RC());
    rec.set("brightness", r.BD());
    rec.set("contrast", r.BD());
    rec.set("ambientColor", r.CMC());
  }
  rec.set("paperSpace", bool_v(r.B()));
  bool assoc = false;
  if (ver >= 15) {
    assoc = r.B() != 0;
    rec.set("associatedUcs", assoc);
    if (assoc) {
      rec.set("ucsOrigin", r.BD3());
      rec.set("ucsXAxis", r.BD3());
      rec.set("ucsYAxis", r.BD3());
      rec.set("ucsElevation", r.BD());
      rec.set("ucsOrthoType", r.BS());
    }
  }
  if (ver >= 21) {
    rec.set("cameraPlottable", bool_v(r.B()));
  }
  table_entry_handles(h, rec);
  if (ver >= 21) {
    rec.set_h("background", h.H(rec.handle));
    rec.set_h("visualStyle", h.H(rec.handle));
    rec.set_h("sun", h.H(rec.handle));
  }
  if (ver >= 15 && assoc) {
    rec.set_h("baseUcs", h.H(rec.handle));
    rec.set_h("namedUcs", h.H(rec.handle));
  }
  if (ver >= 21) {
    rec.set_h("liveSection", h.H(rec.handle));
  }
}

void dec_ucs(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  rec.set("origin", r.BD3());
  rec.set("xAxis", r.BD3());
  rec.set("yAxis", r.BD3());
  if (ctx.ver >= 15) {
    rec.set("elevation", r.BD());
    rec.set("orthoViewType", r.BS());
    rec.set("orthoType", r.BS());
  }
  table_entry_handles(h, rec);
}

void dec_vport(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  table_entry(r, rec, ctx);
  rec.set("viewHeight", r.BD());
  rec.set("aspectRatio", r.BD());
  rec.set("viewCenter", r.RD2());
  rec.set("viewTarget", r.BD3());
  rec.set("viewDirection", r.BD3());
  rec.set("twist", r.BD());
  rec.set("lensLength", r.BD());
  rec.set("frontClip", r.BD());
  rec.set("backClip", r.BD());
  rec.set("viewMode", r.bits(4));
  if (ver >= 15) {
    rec.set("renderMode", r.RC());
  }
  if (ver >= 21) {
    rec.set("useDefaultLights", bool_v(r.B()));
    rec.set("defaultLightingType", r.RC());
    rec.set("brightness", r.BD());
    rec.set("contrast", r.BD());
    rec.set("ambientColor", r.CMC());
  }
  rec.set("lowerLeft", r.RD2());
  rec.set("upperRight", r.RD2());
  rec.set("ucsFollow", bool_v(r.B()));
  rec.set("circleZoom", r.BS());
  rec.set("fastZoom", bool_v(r.B()));
  rec.set("ucsIcon", r.bits(2));
  rec.set("gridOn", bool_v(r.B()));
  rec.set("gridSpacing", r.RD2());
  rec.set("snapOn", bool_v(r.B()));
  rec.set("snapStyle", bool_v(r.B()));
  rec.set("snapIsoPair", r.BS());
  rec.set("snapRotation", r.BD());
  rec.set("snapBase", r.RD2());
  rec.set("snapSpacing", r.RD2());
  if (ver >= 15) {
    (void)r.B();
    rec.set("ucsPerViewport", bool_v(r.B()));
    rec.set("ucsOrigin", r.BD3());
    rec.set("ucsXAxis", r.BD3());
    rec.set("ucsYAxis", r.BD3());
    rec.set("ucsElevation", r.BD());
    rec.set("ucsOrthoType", r.BS());
  }
  if (ver >= 21) {
    rec.set("gridFlags", r.BS());
    rec.set("gridMajor", r.BS());
  }
  table_entry_handles(h, rec);
  if (ver >= 21) {
    rec.set_h("background", h.H(rec.handle));
    rec.set_h("visualStyle", h.H(rec.handle));
    rec.set_h("sun", h.H(rec.handle));
  }
  if (ver >= 15) {
    rec.set_h("namedUcs", h.H(rec.handle));
    rec.set_h("baseUcs", h.H(rec.handle));
  }
}

void dec_appid(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  rec.set("unknown", r.RC());
  table_entry_handles(h, rec);
}

// 20.4.68 DIMSTYLE — values kept by DXF variable name
struct DimVar {
  std::string_view name;
  std::string_view type;
  int from;
};
constexpr auto kDimR2000 = std::to_array<DimVar>({
    {"DIMPOST", "TV", 0},    {"DIMAPOST", "TV", 0},   {"DIMSCALE", "BD", 0},      {"DIMASZ", "BD", 0},
    {"DIMEXO", "BD", 0},     {"DIMDLI", "BD", 0},     {"DIMEXE", "BD", 0},        {"DIMRND", "BD", 0},
    {"DIMDLE", "BD", 0},     {"DIMTP", "BD", 0},      {"DIMTM", "BD", 0},         {"DIMFXL", "BD", 21},
    {"DIMJOGANG", "BD", 21}, {"DIMTFILL", "BS", 21},  {"DIMTFILLCLR", "CMC", 21}, {"DIMTOL", "B", 0},
    {"DIMLIM", "B", 0},      {"DIMTIH", "B", 0},      {"DIMTOH", "B", 0},         {"DIMSE1", "B", 0},
    {"DIMSE2", "B", 0},      {"DIMTAD", "BS", 0},     {"DIMZIN", "BS", 0},        {"DIMAZIN", "BS", 0},
    {"DIMARCSYM", "BS", 21}, {"DIMTXT", "BD", 0},     {"DIMCEN", "BD", 0},        {"DIMTSZ", "BD", 0},
    {"DIMALTF", "BD", 0},    {"DIMLFAC", "BD", 0},    {"DIMTVP", "BD", 0},        {"DIMTFAC", "BD", 0},
    {"DIMGAP", "BD", 0},     {"DIMALTRND", "BD", 0},  {"DIMALT", "B", 0},         {"DIMALTD", "BS", 0},
    {"DIMTOFL", "B", 0},     {"DIMSAH", "B", 0},      {"DIMTIX", "B", 0},         {"DIMSOXD", "B", 0},
    {"DIMCLRD", "CMC", 0},   {"DIMCLRE", "CMC", 0},   {"DIMCLRT", "CMC", 0},      {"DIMADEC", "BS", 0},
    {"DIMDEC", "BS", 0},     {"DIMTDEC", "BS", 0},    {"DIMALTU", "BS", 0},       {"DIMALTTD", "BS", 0},
    {"DIMAUNIT", "BS", 0},   {"DIMFRAC", "BS", 0},    {"DIMLUNIT", "BS", 0},      {"DIMDSEP", "BS", 0},
    {"DIMTMOVE", "BS", 0},   {"DIMJUST", "BS", 0},    {"DIMSD1", "B", 0},         {"DIMSD2", "B", 0},
    {"DIMTOLJ", "BS", 0},    {"DIMTZIN", "BS", 0},    {"DIMALTZ", "BS", 0},       {"DIMALTTZ", "BS", 0},
    {"DIMUPT", "B", 0},      {"DIMATFIT", "BS", 0},   {"DIMFXLON", "B", 21},      {"DIMTXTDIRECTION", "B", 24},
    {"DIMALTMZF", "BD", 24}, {"DIMALTMZS", "TV", 24}, {"DIMMZF", "BD", 24},       {"DIMMZS", "TV", 24},
    {"DIMLWD", "BS", 0},     {"DIMLWE", "BS", 0},
});
constexpr auto kDimR13 = std::to_array<DimVar>({
    {"DIMTOL", "B", 0},      {"DIMLIM", "B", 0},       {"DIMTIH", "B", 0},       {"DIMTOH", "B", 0},
    {"DIMSE1", "B", 0},      {"DIMSE2", "B", 0},       {"DIMALT", "B", 0},       {"DIMTOFL", "B", 0},
    {"DIMSAH", "B", 0},      {"DIMTIX", "B", 0},       {"DIMSOXD", "B", 0},      {"DIMALTD", "RC", 0},
    {"DIMZIN", "RC", 0},     {"DIMSD1", "B", 0},       {"DIMSD2", "B", 0},       {"DIMTOLJ", "RC", 0},
    {"DIMJUST", "RC", 0},    {"DIMFIT", "RC", 0},      {"DIMUPT", "B", 0},       {"DIMTZIN", "RC", 0},
    {"DIMALTZ", "RC", 0},    {"DIMALTTZ", "RC", 0},    {"DIMTAD", "RC", 0},      {"DIMUNIT", "BS", 0},
    {"DIMAUNIT", "BS", 0},   {"DIMDEC", "BS", 0},      {"DIMTDEC", "BS", 0},     {"DIMALTU", "BS", 0},
    {"DIMALTTD", "BS", 0},   {"DIMSCALE", "BD", 0},    {"DIMASZ", "BD", 0},      {"DIMEXO", "BD", 0},
    {"DIMDLI", "BD", 0},     {"DIMEXE", "BD", 0},      {"DIMRND", "BD", 0},      {"DIMDLE", "BD", 0},
    {"DIMTP", "BD", 0},      {"DIMTM", "BD", 0},       {"DIMTXT", "BD", 0},      {"DIMCEN", "BD", 0},
    {"DIMTSZ", "BD", 0},     {"DIMALTF", "BD", 0},     {"DIMLFAC", "BD", 0},     {"DIMTVP", "BD", 0},
    {"DIMTFAC", "BD", 0},    {"DIMGAP", "BD", 0},      {"DIMPOST", "T", 0},      {"DIMAPOST", "T", 0},
    {"DIMBLK_NAME", "T", 0}, {"DIMBLK1_NAME", "T", 0}, {"DIMBLK2_NAME", "T", 0}, {"DIMCLRD", "BS", 0},
    {"DIMCLRE", "BS", 0},    {"DIMCLRT", "BS", 0},
});

Value read_typed(Bits& r, std::string_view t) {
  if (t == "B") {
    return r.B();
  }
  if (t == "BS") {
    return r.BS();
  }
  if (t == "BD") {
    return r.BD();
  }
  if (t == "RC") {
    return r.RC();
  }
  if (t == "TV") {
    return r.TV();
  }
  if (t == "T") {
    return r.T();
  }
  if (t == "CMC") {
    return r.CMC();
  }
  throw DecodeError(std::string(t));
}

void dec_dimstyle(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  Value vars = Value::object();
  const std::span<const DimVar> list =
      ctx.ver < 15 ? std::span<const DimVar>(kDimR13) : std::span<const DimVar>(kDimR2000);
  try {
    for (const DimVar& d : list) {
      if (d.from != 0 && ctx.ver < d.from) {
        continue;
      }
      vars.set(d.name, read_typed(r, d.type));
    }
  } catch (...) {
    // JS: `vars` is only attached after the loop, so a failed read leaves rec.vars unset.
    throw;
  }
  rec.set("_unknownBit", r.B());
  rec.set("vars", std::move(vars));
  table_entry_handles(h, rec);
  rec.set_h("DIMTXSTY", h.H(rec.handle));
  if (ctx.ver >= 15) {
    rec.set_h("DIMLDRBLK", h.H(rec.handle));
    rec.set_h("DIMBLK", h.H(rec.handle));
    rec.set_h("DIMBLK1", h.H(rec.handle));
    rec.set_h("DIMBLK2", h.H(rec.handle));
  }
  if (ctx.ver >= 21) {
    rec.set_h("DIMLTYPE", h.H(rec.handle));
    rec.set_h("DIMLTEX1", h.H(rec.handle));
    rec.set_h("DIMLTEX2", h.H(rec.handle));
  }
}

void dec_vpent_header(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  table_entry(r, rec, ctx);
  rec.set("flag1", r.B());
  table_entry_handles(h, rec);
  rec.set_h("viewport", h.H(rec.handle));
  rec.set_h("prevHeader", h.H(rec.handle));
}

// ---- 20.4.72/73 GROUP MLINESTYLE
void dec_group(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("name", r.TV());
  rec.set("unnamed", r.BS());
  rec.set("selectable", r.BS());
  const std::int32_t n = r.BL();
  rec.set_hl("entities", hrefs(h, rec.handle, n));
}
void dec_mlinestyle(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("name", r.TV());
  rec.set("description", r.TV());
  rec.set("flags", r.BS());
  rec.set("fillColor", r.CMC());
  rec.set("startAngle", r.BD());
  rec.set("endAngle", r.BD());
  const std::uint32_t n = r.RC();
  rec.set("elements", Array{});
  for (std::uint32_t i = 0; i < n; ++i) {
    Value e = Value::object();
    e.set("offset", r.BD());
    e.set("color", r.CMC());
    if (ctx.ver < 32) {
      e.set("linetypeIndex", r.BS());
    }
    rec.f.find("elements")->push(std::move(e));
  }
  if (ctx.ver >= 32) {
    for (Value& e : rec.f.find("elements")->as_array()) {
      e.set("_ltype", h.H(rec.handle));
    }
  }
}

// ---- 20.4.74 DICTIONARYVAR
void dec_dictionaryvar(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("intval", r.RC());
  rec.set("value", r.TV());
}

// ---- 20.4.75 HATCH
void dec_hatch(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  if (ver >= 18) {
    const std::int32_t is_grad = r.BL();
    (void)r.BL();
    const double angle = r.BD();
    const double shift = r.BD();
    const std::int32_t single = r.BL();
    const double tint = r.BD();
    const std::int32_t nc = r.BL();
    Array colors;
    for (std::int32_t i = 0; i < nc; ++i) {
      const double value = r.BD();
      (void)r.BS();
      const auto rgb = static_cast<std::uint32_t>(r.BL());
      (void)r.RC();
      colors.push_back(make_object({{"value", value}, {"rgb", rgb & 0xFFFFFFU}}));
    }
    std::string name = r.TV();
    if (is_grad != 0) {
      rec.set("gradient", make_object({{"name", std::move(name)},
                                       {"angle", angle},
                                       {"shift", shift},
                                       {"singleColor", single},
                                       {"tint", tint},
                                       {"colors", std::move(colors)}}));
    }
  }
  rec.set("elevation", r.BD());
  rec.set("extrusion", r.BD3());
  rec.set("pattern", r.TV());
  const bool solid = r.B() != 0;
  rec.set("solid", solid);
  rec.set("associative", bool_v(r.B()));
  const std::int32_t np = r.BL();
  rec.set("loops", Array{});
  std::int64_t total_bound = 0;
  bool any_pixel = false;
  for (std::int32_t i = 0; i < np; ++i) {
    const std::int32_t flags = r.BL();
    Value loop = make_object({{"flags", flags}});
    if ((flags & 4) != 0) {
      any_pixel = true;
    }
    // The loop joins rec.loops only once it is complete (JS pushes after reading it).
    if ((flags & 2) == 0) {
      const std::int32_t ns = r.BL();
      loop.set("edges", Array{});
      for (std::int32_t k = 0; k < ns; ++k) {
        const std::uint32_t t = r.RC();
        Value e;
        if (t == 1) {
          Value s = r.RD2();
          e = make_object({{"type", "line"}, {"start", std::move(s)}, {"end", r.RD2()}});
        } else if (t == 2) {
          e = make_object({{"type", "arc"}});
          e.set("center", r.RD2());
          e.set("radius", r.BD());
          e.set("startAngle", r.BD());
          e.set("endAngle", r.BD());
          e.set("ccw", bool_v(r.B()));
        } else if (t == 3) {
          e = make_object({{"type", "ellipse"}});
          e.set("center", r.RD2());
          e.set("majorAxis", r.RD2());
          e.set("ratio", r.BD());
          e.set("startAngle", r.BD());
          e.set("endAngle", r.BD());
          e.set("ccw", bool_v(r.B()));
        } else if (t == 4) {
          e = make_object({{"type", "spline"}});
          e.set("degree", r.BL());
          const bool rational = r.B() != 0;
          e.set("rational", rational);
          e.set("periodic", bool_v(r.B()));
          const std::int32_t nk = r.BL();
          const std::int32_t ncp = r.BL();
          Array knots;
          for (std::int32_t j = 0; j < nk; ++j) {
            knots.emplace_back(r.BD());
          }
          e.set("knots", std::move(knots));
          Array control;
          Array weights;
          for (std::int32_t j = 0; j < ncp; ++j) {
            control.push_back(r.RD2());
            if (rational) {
              weights.emplace_back(r.BD());
            }
          }
          e.set("control", std::move(control));
          if (rational) {
            e.set("weights", std::move(weights));
          }
          if (ver >= 24) {
            const std::int32_t nf = r.BL();
            if (nf > 0) {
              Array fit;
              for (std::int32_t j = 0; j < nf; ++j) {
                fit.push_back(r.RD2());
              }
              e.set("fit", std::move(fit));
              e.set("startTangent", r.RD2());
              e.set("endTangent", r.RD2());
            }
          }
        } else {
          throw DecodeError(msg("ccdwg.err.hatch_edge", {{"type", t}}));
        }
        loop.find("edges")->push(std::move(e));
      }
    } else {
      const std::uint32_t bulges = r.B();
      const std::uint32_t closed = r.B();
      const std::int32_t n = r.BL();
      Array vs;
      for (std::int32_t k = 0; k < n; ++k) {
        const Value p = r.RD2();
        const double b = bulges != 0 ? r.BD() : 0;
        vs.push_back(make_object({{"x", p["x"]}, {"y", p["y"]}, {"bulge", b}}));
      }
      loop.set("polyline",
               make_object({{"closed", closed != 0}, {"hasBulges", bulges != 0}, {"vertices", std::move(vs)}}));
    }
    const std::int32_t nb = r.BL();
    loop.set("_nbound", nb);
    total_bound += nb;
    rec.f.find("loops")->push(std::move(loop));
  }
  rec.set("style", r.BS());
  rec.set("patternType", r.BS());
  if (!solid) {
    rec.set("angle", r.BD());
    rec.set("scale", r.BD());
    rec.set("double", bool_v(r.B()));
    const std::int32_t nd = r.BS();
    rec.set("patternLines", Array{});
    for (std::int32_t i = 0; i < nd; ++i) {
      Value l = Value::object();
      l.set("angle", r.BD());
      l.set("base", r.BD2());
      l.set("offset", r.BD2());
      Array dashes;
      const std::int32_t n = r.BS();
      for (std::int32_t k = 0; k < n; ++k) {
        dashes.emplace_back(r.BD());
      }
      l.set("dashes", std::move(dashes));
      rec.f.find("patternLines")->push(std::move(l));
    }
  }
  if (any_pixel) {
    rec.set("pixelSize", r.BD());
  }
  const std::int32_t ns = r.BL();
  rec.set("seeds", Array{});
  for (std::int32_t i = 0; i < ns; ++i) {
    rec.f.find("seeds")->push(r.RD2());
  }
  std::vector<std::int64_t> boundary = hrefs(h, rec.handle, total_bound);
  std::size_t bi = 0;
  for (Value& l : rec.f.find("loops")->as_array()) {
    const auto n = base::clamp_cast<std::size_t>(l["_nbound"].num_or(0));
    Array part;
    for (std::size_t k = 0; k < n && bi + k < boundary.size(); ++k) {
      part.emplace_back(static_cast<double>(boundary[bi + k]));
    }
    bi += n;
    l.set("_boundary", std::move(part));
  }
  rec.set_hl("boundary", std::move(boundary));
}

// ---- 20.4.79–84
void dec_idbuffer(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  (void)r.RC();
  const std::int32_t n = r.BL();
  rec.set_hl("ids", hrefs(h, rec.handle, n));
}
void dec_image(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("classVersion", r.BL());
  rec.set("insert", r.BD3());
  rec.set("u", r.BD3());
  rec.set("v", r.BD3());
  rec.set("size", r.RD2());
  rec.set("displayFlags", r.BS());
  rec.set("clipping", bool_v(r.B()));
  rec.set("brightness", r.RC());
  rec.set("contrast", r.RC());
  rec.set("fade", r.RC());
  if (ctx.ver >= 24) {
    rec.set("clipMode", r.B());
  }
  const std::int32_t clip_type = r.BS();
  rec.set("clipType", clip_type);
  if (clip_type == 1) {
    Value a = r.RD2();
    rec.set("clip", Array{std::move(a), r.RD2()});
  } else {
    const std::int32_t n = r.BL();
    rec.set("clip", Array{});
    for (std::int32_t i = 0; i < n; ++i) {
      rec.f.find("clip")->push(r.RD2());
    }
  }
  rec.set_h("def", h.H(rec.handle));
  rec.set_h("defReactor", h.H(rec.handle));
}
void dec_imagedef(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("classVersion", r.BL());
  rec.set("size", r.RD2());
  rec.set("fileName", r.TV());
  rec.set("loaded", bool_v(r.B()));
  rec.set("resUnits", r.RC());
  rec.set("pixelSize", r.RD2());
}
void dec_imagedef_reactor(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("classVersion", r.BL());
}
void dec_layer_index(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  (void)r.BL();
  (void)r.BL();
  const std::int32_t n = r.BL();
  rec.set("entries", Array{});
  for (std::int32_t i = 0; i < n; ++i) {
    const std::int32_t index = r.BL();
    rec.f.find("entries")->push(make_object({{"index", index}, {"name", r.TV()}}));
  }
  rec.set_hl("entries", hrefs(h, rec.handle, n));
}

void plot_settings(Bits& r, Value& ps, int ver) {
  ps.set("pageSetupName", r.TV());
  ps.set("printer", r.TV());
  ps.set("flags", r.BS());
  Value m = Value::object();
  m.set("left", r.BD());
  m.set("bottom", r.BD());
  m.set("right", r.BD());
  m.set("top", r.BD());
  ps.set("margins", std::move(m));
  ps.set("paperWidth", r.BD());
  ps.set("paperHeight", r.BD());
  ps.set("paperSize", r.TV());
  ps.set("origin", r.BD2());
  ps.set("paperUnits", r.BS());
  ps.set("rotation", r.BS());
  ps.set("plotType", r.BS());
  ps.set("windowMin", r.BD2());
  ps.set("windowMax", r.BD2());
  if (ver <= 15) {
    ps.set("viewName", r.T());
  }
  ps.set("realWorldUnits", r.BD());
  ps.set("drawingUnits", r.BD());
  ps.set("styleSheet", r.TV());
  ps.set("scaleType", r.BS());
  ps.set("scaleFactor", r.BD());
  ps.set("imageOrigin", r.BD2());
  if (ver >= 18) {
    ps.set("shadePlotMode", r.BS());
    ps.set("shadePlotRes", r.BS());
    ps.set("shadePlotDpi", r.BS());
  }
}

void dec_layout(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  rec.set("plot", Value::object());
  plot_settings(r, *rec.f.find("plot"), ver);
  rec.set("name", r.TV());
  rec.set("tabOrder", r.BL());
  rec.set("flags", r.BS());
  rec.set("ucsOrigin", r.BD3());
  rec.set("limMin", r.RD2());
  rec.set("limMax", r.RD2());
  rec.set("insBase", r.BD3());
  rec.set("ucsXAxis", r.BD3());
  rec.set("ucsYAxis", r.BD3());
  rec.set("elevation", r.BD());
  rec.set("orthoType", r.BS());
  rec.set("extMin", r.BD3());
  rec.set("extMax", r.BD3());
  std::int32_t nvp = 0;
  if (ver >= 18) {
    nvp = r.BL();  // Spec gap: a BL, not an RL
    rec.set_h("plotView", h.H(rec.handle));
  }
  if (ver >= 21) {
    rec.set_h("visualStyle", h.H(rec.handle));
  }
  rec.set_h("block", h.H(rec.handle));
  rec.set_h("lastViewport", h.H(rec.handle));
  rec.set_h("baseUcs", h.H(rec.handle));
  rec.set_h("namedUcs", h.H(rec.handle));
  if (ver >= 18) {
    rec.set_hl("viewports", hrefs(h, rec.handle, nvp));
  }
}
void dec_plotsettings(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("plot", Value::object());
  plot_settings(r, *rec.f.find("plot"), ctx.ver);
  if (ctx.ver >= 18) {
    rec.set_h("plotView", h.H(rec.handle));
  }
}

// ---- 20.4.85 LWPOLYLINE
void dec_lwpolyline(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  const std::int32_t fs = r.BS();
  const auto f = static_cast<std::uint32_t>(fs);
  rec.set("flags", fs);
  rec.set("constantWidth", (f & 4U) != 0 ? r.BD() : 0.0);
  rec.set("elevation", (f & 8U) != 0 ? r.BD() : 0.0);
  rec.set("thickness", (f & 2U) != 0 ? r.BD() : 0.0);
  rec.set("extrusion", (f & 1U) != 0 ? r.BD3() : base::pt3(0, 0, 1));
  const std::int64_t n = fits(r, r.BL(), 2);
  const std::int64_t nb = (f & 16U) != 0 ? fits(r, r.BL(), 2) : 0;
  const std::int64_t nid = ver >= 24 && (f & 1024U) != 0 ? fits(r, r.BL(), 2) : 0;
  const std::int64_t nw = (f & 32U) != 0 ? fits(r, r.BL(), 4) : 0;
  rec.set("closed", (f & 512U) != 0);
  rec.set("plinegen", (f & 256U) != 0);
  rec.set("vertices", Array(static_cast<std::size_t>(n)));
  Array& vs = rec.f.find("vertices")->as_array();
  auto vertex = [](double x, double y) {
    return make_object({{"x", x}, {"y", y}, {"bulge", 0}, {"startWidth", 0}, {"endWidth", 0}});
  };
  if (n > 0) {
    if (ver < 15) {
      for (std::int64_t i = 0; i < n; ++i) {
        const Value p = r.RD2();
        vs[static_cast<std::size_t>(i)] = vertex(p["x"].as_number(), p["y"].as_number());
      }
    } else {
      double x = r.RD();
      double y = r.RD();
      vs[0] = vertex(x, y);
      for (std::int64_t i = 1; i < n; ++i) {
        x = r.DD(x);
        y = r.DD(y);
        vs[static_cast<std::size_t>(i)] = vertex(x, y);
      }
    }
  }
  for (std::int64_t i = 0; i < nb; ++i) {
    const double b = r.BD();
    if (i < n && vs[static_cast<std::size_t>(i)].is_object()) {
      vs[static_cast<std::size_t>(i)].set("bulge", b);
    }
  }
  for (std::int64_t i = 0; i < nid; ++i) {
    const std::int32_t id = r.BL();
    if (i < n && vs[static_cast<std::size_t>(i)].is_object()) {
      vs[static_cast<std::size_t>(i)].set("id", id);
    }
  }
  for (std::int64_t i = 0; i < nw; ++i) {
    const double s = r.BD();
    const double e = r.BD();
    if (i < n && vs[static_cast<std::size_t>(i)].is_object()) {
      vs[static_cast<std::size_t>(i)].set("startWidth", s);
      vs[static_cast<std::size_t>(i)].set("endWidth", e);
    }
  }
}

// ---- 20.4.88 OLE2FRAME
void dec_ole2frame(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  rec.set("flags", r.BS());
  if (ctx.ver >= 15) {
    rec.set("mode", r.BS());
  }
  const std::int32_t len = r.BL();
  rec.set("dataLength", len);
  r.set_pos(r.pos() + static_cast<std::uint64_t>(static_cast<std::int64_t>(len) * 8));
  if (ctx.ver >= 15) {
    (void)r.RC();
  }
}

// ---- 20.4.90 PROXY
void dec_proxy(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& ctx) {
  if (ctx.ver >= 15) {
    rec.set("classId", r.BL());
    if (ctx.ver >= 32) {
      rec.set("proxyVersion", r.BL());
      rec.set("proxyMaint", r.BL());
    } else {
      rec.set("drawingFormat", r.BL());
    }
    rec.set("originalFormat", r.B());
  }
}

// ---- 20.4.91–95
void dec_rastervariables(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("classVersion", r.BL());
  rec.set("displayFrame", r.BS());
  rec.set("displayQuality", r.BS());
  rec.set("units", r.BS());
}
void dec_scale(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  (void)r.BS();
  rec.set("name", r.TV());
  rec.set("paperUnits", r.BD());
  rec.set("drawingUnits", r.BD());
  rec.set("unitScale", bool_v(r.B()));
}
void dec_sortentstable(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  const std::int32_t n = r.BL();
  rec.set("_sort", Array{});
  for (std::int32_t i = 0; i < n; ++i) {
    rec.f.find("_sort")->push(static_cast<double>(r.H()));
  }
  rec.set_h("ownerBlock", h.H(rec.handle));
  rec.set_hl("ents", hrefs(h, rec.handle, n));
}
void dec_spatial_filter(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  const std::int32_t n = r.BS();
  rec.set("points", Array{});
  for (std::int32_t i = 0; i < n; ++i) {
    rec.f.find("points")->push(r.RD2());
  }
  rec.set("extrusion", r.BD3());
  rec.set("origin", r.BD3());
  rec.set("displayBoundary", r.BS());
  rec.set("frontClip", r.BS() != 0 ? Value(r.BD()) : Value(nullptr));
  rec.set("backClip", r.BS() != 0 ? Value(r.BD()) : Value(nullptr));
  rec.set("inverseBlockTransform", Array{});
  for (int i = 0; i < 12; ++i) {
    rec.f.find("inverseBlockTransform")->push(r.BD());
  }
  rec.set("clipTransform", Array{});
  for (int i = 0; i < 12; ++i) {
    rec.f.find("clipTransform")->push(r.BD());
  }
}

// ---- LIGHT. Spec gap: layout found by experiment (objects-read.js).
void dec_light(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("lightVersion", r.BL());
  rec.set("name", r.TV());
  rec.set("lightType", r.BS());
  rec.set("status", bool_v(r.B()));
  rec.set("lightColor", r.CMC());
  rec.set("plotGlyph", bool_v(r.B()));
  rec.set("intensity", r.BD());
  rec.set("position", r.BD3());
  rec.set("target", r.BD3());
  rec.set("attenuationType", r.BS());
  rec.set("useAttenuationLimits", bool_v(r.B()));
  rec.set("attenuationStart", r.BD());
  rec.set("attenuationEnd", r.BD());
  rec.set("hotspot", r.BD());
  rec.set("falloff", r.BD());
  rec.set("castShadows", bool_v(r.B()));
  rec.set("shadowType", r.BS());
  rec.set("shadowMapSize", r.BS());
  rec.set("shadowMapSoftness", r.RC());
}

void dec_wipeoutvariables(Bits& r, Bits& /*h*/, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("displayFrame", r.BS());
}

// ---- 20.4.104 XRECORD
Value xrecord_value(Bits& r, int code, const Ctx& ctx) {
  const std::string_view t = group_type(code);
  if (t == "str") {
    if (ctx.ver >= 21) {
      const std::uint32_t n = r.RSu();
      U16 s(n);
      for (auto& c : s) {
        c = static_cast<std::uint16_t>(r.RSu());
      }
      return utf8(s);
    }
    const std::uint32_t n = r.RSu();
    (void)r.RC();
    return decode_text(ctx.cp, r.bytes(n));
  }
  if (t == "pt3") {
    return r.RD3();
  }
  if (t == "pt2") {
    return r.RD2();
  }
  if (t == "dbl") {
    return r.RD();
  }
  if (t == "i8") {
    return r.RC();
  }
  if (t == "i16") {
    return r.RS();
  }
  if (t == "i32") {
    return r.RLs();
  }
  if (t == "i64") {
    return r.RLL();
  }
  if (t == "hnd") {
    const std::vector<std::uint8_t> b = r.bytes(8);
    double v = 0;
    for (std::size_t i = 8; i-- > 0;) {
      v = v * 256 + b[i];
    }
    return js_hex(v);
  }
  if (t == "bin") {
    const std::uint32_t n = r.RC();
    return hex_bytes(r.bytes(n));
  }
  if (t == "bool") {
    return r.RC();
  }
  throw DecodeError(msg("ccdwg.err.xrecord_code", {{"code", code}}));
}

void dec_xrecord(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const std::int32_t n = r.BL();
  const std::uint64_t end = r.pos() + static_cast<std::uint64_t>(static_cast<std::int64_t>(n) * 8);
  rec.set("data", Array{});
  Value* data = rec.f.find("data");
  try {
    while (r.pos() < end) {
      const std::int32_t code = r.RS();
      Value v = xrecord_value(r, code, ctx);
      data->push(Array{code, std::move(v)});
    }
  } catch (const std::exception& e) {
    rec.set("_xrecError", error_text(e));
  }
  r.set_pos(end);
  if (ctx.ver >= 15) {
    rec.set("cloning", r.BS());
  }
  rec.set_hl("objids", rest_handles(h, rec.handle));
}

// ---- 20.4.48/86 MULTILEADER
void annot_context(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  rec.set("context", make_object({{"roots", Array{}}}));
  Value* c = rec.f.find("context");
  const std::int32_t nr = r.BL();
  for (std::int32_t i = 0; i < nr; ++i) {
    Value root = Value::object();
    root.set("contentValid", bool_v(r.B()));
    root.set("unknown", bool_v(r.B()));
    root.set("connection", r.BD3());
    root.set("direction", r.BD3());
    root.set("breaks", Array{});
    root.set("lines", Array{});
    const std::int32_t nb = r.BL();
    for (std::int32_t k = 0; k < nb; ++k) {
      Value a = r.BD3();
      root.find("breaks")->push(Array{std::move(a), r.BD3()});
    }
    root.set("index", r.BL());
    root.set("landingDistance", r.BD());
    const std::int32_t nl = r.BL();
    for (std::int32_t k = 0; k < nl; ++k) {
      Value line = make_object({{"points", Array{}}});
      const std::int32_t np = r.BL();
      for (std::int32_t j = 0; j < np; ++j) {
        line.find("points")->push(r.BD3());
      }
      const std::int32_t bic = r.BL();
      line.set("breakInfoCount", bic);
      if (bic != 0) {
        line.set("segmentIndex", r.BL());
        const std::int32_t ns = r.BL();
        line.set("breaks", Array{});
        for (std::int32_t j = 0; j < ns; ++j) {
          Value a = r.BD3();
          line.find("breaks")->push(Array{std::move(a), r.BD3()});
        }
      }
      line.set("index", r.BL());
      if (ver >= 24) {
        line.set("leaderType", r.BS());
        line.set("color", r.CMC());
        line.set("_ltype", static_cast<double>(h.H(rec.handle)));
        line.set("lineweight", r.BL());
        line.set("arrowSize", r.BD());
        line.set("_arrow", static_cast<double>(h.H(rec.handle)));
        line.set("overrideFlags", r.BL());
      }
      root.find("lines")->push(std::move(line));
    }
    if (ver >= 24) {
      root.set("attachDirection", r.BS());
    }
    c->find("roots")->push(std::move(root));
  }
  c->set("scale", r.BD());
  c->set("contentBase", r.BD3());
  c->set("textHeight", r.BD());
  c->set("arrowSize", r.BD());
  c->set("landingGap", r.BD());
  c->set("leftAttach", r.BS());
  c->set("rightAttach", r.BS());
  c->set("textAlign", r.BS());
  c->set("attachType", r.BS());
  const bool has_text = r.B() != 0;
  c->set("hasText", has_text);
  if (has_text) {
    Value& t = c->set("text", Value::object());
    t.set("label", r.TV());
    t.set("normal", r.BD3());
    t.set("_style", static_cast<double>(h.H(rec.handle)));
    t.set("location", r.BD3());
    t.set("direction", r.BD3());
    t.set("rotation", r.BD());
    t.set("width", r.BD());
    t.set("height", r.BD());
    t.set("lineSpacing", r.BD());
    t.set("lineSpacingStyle", r.BS());
    t.set("color", r.CMC());
    t.set("alignment", r.BS());
    t.set("flow", r.BS());
    t.set("bgColor", r.CMC());
    t.set("bgScale", r.BD());
    t.set("bgTransparency", r.BL());
    t.set("bgFill", bool_v(r.B()));
    t.set("bgMask", bool_v(r.B()));
    t.set("columnType", r.BS());
    t.set("autoHeight", bool_v(r.B()));
    t.set("columnWidth", r.BD());
    t.set("columnGutter", r.BD());
    t.set("columnFlowReversed", bool_v(r.B()));
    const std::int32_t ncs = r.BL();
    Array sizes;
    for (std::int32_t i = 0; i < ncs; ++i) {
      sizes.emplace_back(r.BD());
    }
    t.set("columnSizes", std::move(sizes));
    t.set("wordBreak", bool_v(r.B()));
    (void)r.B();
  } else {
    const bool has_block = r.B() != 0;
    c->set("hasBlock", has_block);
    if (has_block) {
      Value& b = c->set("block", Value::object());
      b.set("_block", static_cast<double>(h.H(rec.handle)));
      b.set("normal", r.BD3());
      b.set("location", r.BD3());
      b.set("scale", r.BD3());
      b.set("rotation", r.BD());
      b.set("color", r.CMC());
      Array m;
      for (int i = 0; i < 16; ++i) {
        m.emplace_back(r.BD());
      }
      b.set("matrix", std::move(m));
    }
  }
  c->set("basePoint", r.BD3());
  c->set("baseDirection", r.BD3());
  c->set("baseVertical", r.BD3());
  c->set("normalReversed", bool_v(r.B()));
  if (ver >= 24) {
    c->set("topAttach", r.BS());
    c->set("bottomAttach", r.BS());
  }
}

void dec_multileader(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  if (ver >= 24) {
    rec.set("mlVersion", r.BS());
  }
  annot_context(r, h, rec, ctx);
  rec.set_h("style", h.H(rec.handle));
  rec.set("overrideFlags", r.BL());
  rec.set("leaderType", r.BS());
  rec.set("leaderColor", r.CMC());
  rec.set_h("leaderLtype", h.H(rec.handle));
  rec.set("leaderLineweight", r.BL());
  rec.set("landingEnabled", bool_v(r.B()));
  rec.set("doglegEnabled", bool_v(r.B()));
  rec.set("landingDistance", r.BD());
  rec.set_h("arrowhead", h.H(rec.handle));
  rec.set("arrowSize", r.BD());
  rec.set("contentType", r.BS());
  rec.set_h("textStyle", h.H(rec.handle));
  rec.set("textLeftAttach", r.BS());
  rec.set("textRightAttach", r.BS());
  rec.set("textAngleType", r.BS());
  (void)r.BS();
  rec.set("textColor", r.CMC());
  rec.set("textFrame", bool_v(r.B()));
  rec.set_h("blockStyle", h.H(rec.handle));
  rec.set("blockColor", r.CMC());
  rec.set("blockScale", r.BD3());
  rec.set("blockRotation", r.BD());
  rec.set("blockAttach", r.BS());
  rec.set("annotative", bool_v(r.B()));
  if (ver < 24) {
    const std::int32_t na = r.BL();
    rec.set("arrowheads", Array{});
    for (std::int32_t i = 0; i < na; ++i) {
      const bool is_default = r.B() != 0;
      rec.f.find("arrowheads")
          ->push(make_object({{"isDefault", is_default}, {"_h", static_cast<double>(h.H(rec.handle))}}));
    }
  }
  const std::int32_t nl = r.BL();
  rec.set("blockLabels", Array{});
  for (std::int32_t i = 0; i < nl; ++i) {
    const auto attdef = static_cast<double>(h.H(rec.handle));
    std::string text = r.TV();
    const std::int32_t index = r.BS();
    rec.f.find("blockLabels")
        ->push(make_object({{"_attdef", attdef}, {"text", std::move(text)}, {"index", index}, {"width", r.BD()}}));
  }
  rec.set("textDirectionNegative", bool_v(r.B()));
  rec.set("ipeAlign", r.BS());
  rec.set("justification", r.BS());
  rec.set("scale", r.BD());
  if (ver >= 24) {
    rec.set("attachDirection", r.BS());
    rec.set("textTopAttach", r.BS());
    rec.set("textBottomAttach", r.BS());
  }
  if (ver >= 27) {
    rec.set("extendToText", bool_v(r.B()));
  }
}

void dec_mleaderstyle(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  if (ver >= 24) {
    rec.set("styleVersion", r.BS());
  }
  rec.set("contentType", r.BS());
  rec.set("drawMLeaderOrder", r.BS());
  rec.set("drawLeaderOrder", r.BS());
  rec.set("maxPoints", r.BL());
  rec.set("firstAngle", r.BD());
  rec.set("secondAngle", r.BD());
  rec.set("leaderType", r.BS());
  rec.set("leaderColor", r.CMC());
  rec.set_h("leaderLtype", h.H(rec.handle));
  rec.set("leaderLineweight", r.BL());
  rec.set("landingEnabled", bool_v(r.B()));
  rec.set("landingGap", r.BD());
  rec.set("doglegEnabled", bool_v(r.B()));
  rec.set("landingDistance", r.BD());
  rec.set("description", r.TV());
  rec.set_h("arrowhead", h.H(rec.handle));
  rec.set("arrowSize", r.BD());
  rec.set("textDefault", r.TV());
  rec.set_h("textStyle", h.H(rec.handle));
  rec.set("leftAttach", r.BS());
  rec.set("rightAttach", r.BS());
  // 20.4.87: "IsNewFormat" = R2010+, or extended data of ACAD_MLEADERVER
  const bool new_format = ver >= 24 || rec.has_eed;
  if (new_format) {
    rec.set("textAngleType", r.BS());
  }
  rec.set("textAlign", r.BS());
  rec.set("textColor", r.CMC());
  rec.set("textHeight", r.BD());
  rec.set("textFrame", bool_v(r.B()));
  if (new_format) {
    rec.set("alwaysAlignLeft", bool_v(r.B()));
  }
  rec.set("alignSpace", r.BD());
  rec.set_h("block", h.H(rec.handle));
  rec.set("blockColor", r.CMC());
  rec.set("blockScale", r.BD3());
  rec.set("blockScaleEnabled", bool_v(r.B()));
  rec.set("blockRotation", r.BD());
  rec.set("blockRotationEnabled", bool_v(r.B()));
  rec.set("blockConnection", r.BS());
  rec.set("scale", r.BD());
  rec.set("changed", bool_v(r.B()));
  rec.set("annotative", bool_v(r.B()));
  rec.set("breakSize", r.BD());
  if (ver >= 24) {
    rec.set("attachDirection", r.BS());
    rec.set("topAttach", r.BS());
    rec.set("bottomAttach", r.BS());
  }
}

const Decoders& decoders() {
  static const Decoders kD = [] {
    Decoders d;
    d["TEXT"] = dec_text;
    d["ATTRIB"] = dec_attrib;
    d["ATTDEF"] = dec_attdef;
    d["BLOCK"] = dec_block;
    d["ENDBLK"] = dec_none;
    d["SEQEND"] = dec_none;
    d["INSERT"] = dec_insert;
    d["MINSERT"] = dec_minsert;
    d["VERTEX_2D"] = dec_vertex2d;
    d["VERTEX_3D"] = dec_vertex3d;
    d["VERTEX_MESH"] = dec_vertex3d;
    d["VERTEX_PFACE"] = dec_vertex3d;
    d["VERTEX_PFACE_FACE"] = dec_pface_face;
    d["POLYLINE_2D"] = dec_polyline2d;
    d["POLYLINE_3D"] = dec_polyline3d;
    d["POLYLINE_PFACE"] = dec_polyline_pface;
    d["POLYLINE_MESH"] = dec_polyline_mesh;
    d["ARC"] = dec_arc;
    d["CIRCLE"] = dec_circle;
    d["LINE"] = dec_line;
    d["DIMENSION_ORDINATE"] = dec_dim_ordinate;
    d["DIMENSION_LINEAR"] = dec_dim_linear;
    d["DIMENSION_ALIGNED"] = dec_dim_aligned;
    d["DIMENSION_ANG3PT"] = dec_dim_ang3pt;
    d["DIMENSION_ANG2LN"] = dec_dim_ang2ln;
    d["DIMENSION_RADIUS"] = dec_dim_radius;
    d["DIMENSION_DIAMETER"] = dec_dim_diameter;
    d["ARC_DIMENSION"] = dec_arc_dimension;
    d["LARGE_RADIAL_DIMENSION"] = dec_large_radial;
    d["POINT"] = dec_point;
    d["3DFACE"] = dec_3dface;
    d["SOLID"] = dec_solid;
    d["TRACE"] = dec_solid;
    d["SHAPE"] = dec_shape;
    d["VIEWPORT"] = dec_viewport;
    d["ELLIPSE"] = dec_ellipse;
    d["SPLINE"] = dec_spline;
    d["REGION"] = decode_acis_entity;
    d["3DSOLID"] = decode_acis_entity;
    d["BODY"] = decode_acis_entity;
    for (const std::string_view s : {"SURFACE", "PLANESURFACE", "EXTRUDEDSURFACE", "LOFTEDSURFACE", "REVOLVEDSURFACE",
                                     "SWEPTSURFACE", "NURBSURFACE"}) {
      d[s] = decode_surface;
    }
    d["RAY"] = dec_ray;
    d["XLINE"] = dec_ray;
    d["DICTIONARY"] = dec_dictionary;
    d["DICTIONARYWDFLT"] = dec_dictionary_wdflt;
    d["ACDBDICTIONARYWDFLT"] = dec_dictionary_wdflt;
    d["MTEXT"] = dec_mtext;
    d["LEADER"] = dec_leader;
    d["TOLERANCE"] = dec_tolerance;
    d["MLINE"] = dec_mline;
    for (const std::string_view s :
         {"BLOCK_CONTROL", "LAYER_CONTROL", "STYLE_CONTROL", "LTYPE_CONTROL", "VIEW_CONTROL", "UCS_CONTROL",
          "VPORT_CONTROL", "APPID_CONTROL", "DIMSTYLE_CONTROL", "VPENT_CONTROL"}) {
      d[s] = dec_control;
    }
    d["BLOCK_HEADER"] = dec_block_header;
    d["LAYER"] = dec_layer;
    d["STYLE"] = dec_style;
    d["LTYPE"] = dec_ltype;
    d["VIEW"] = dec_view;
    d["UCS"] = dec_ucs;
    d["VPORT"] = dec_vport;
    d["APPID"] = dec_appid;
    d["DIMSTYLE"] = dec_dimstyle;
    d["VPENT_HEADER"] = dec_vpent_header;
    d["GROUP"] = dec_group;
    d["MLINESTYLE"] = dec_mlinestyle;
    d["DICTIONARYVAR"] = dec_dictionaryvar;
    d["HATCH"] = dec_hatch;
    d["IDBUFFER"] = dec_idbuffer;
    d["IMAGE"] = dec_image;
    d["WIPEOUT"] = dec_image;
    d["IMAGEDEF"] = dec_imagedef;
    d["IMAGEDEF_REACTOR"] = dec_imagedef_reactor;
    d["LAYER_INDEX"] = dec_layer_index;
    d["LAYOUT"] = dec_layout;
    d["PLOTSETTINGS"] = dec_plotsettings;
    d["LWPOLYLINE"] = dec_lwpolyline;
    d["OLE2FRAME"] = dec_ole2frame;
    d["OLEFRAME"] = dec_ole2frame;
    d["ACAD_PROXY_ENTITY"] = dec_proxy;
    d["ACAD_PROXY_OBJECT"] = dec_proxy;
    d["RASTERVARIABLES"] = dec_rastervariables;
    d["SCALE"] = dec_scale;
    d["SORTENTSTABLE"] = dec_sortentstable;
    d["SPATIAL_FILTER"] = dec_spatial_filter;
    d["LIGHT"] = dec_light;
    d["ACDBPLACEHOLDER"] = dec_none;
    d["WIPEOUTVARIABLES"] = dec_wipeoutvariables;
    d["XRECORD"] = dec_xrecord;
    d["MULTILEADER"] = dec_multileader;
    d["MLEADER"] = dec_multileader;
    d["MLEADERSTYLE"] = dec_mleaderstyle;
    return d;
  }();
  return kD;
}

Decoder find_decoder(std::string_view name) {
  // table-read.js replaces the ACAD_TABLE / FIELDLIST decoders and adds the table objects (applied last in the JS).
  if (Decoder t = table_decoder(name); t != nullptr) {
    return t;
  }
  const Decoders& d = decoders();
  const auto it = d.find(name);
  return it == d.end() ? nullptr : it->second;
}

}  // namespace

// ---------------------------------------------------------------- framing (20.1)
Rec decode_object(Bytes buf, std::size_t offset, const Ctx& ctx) {
  const int ver = ctx.ver;
  if (offset + 2 > buf.size()) {
    throw DecodeError(msg("ccdwg.err.offset_outside"));
  }
  std::size_t p = offset;
  const double size_d = read_ms(buf, p);
  double hsize_d = 0;
  if (ver >= 24) {
    hsize_d = read_mcu(buf, p);
  }
  if (!(size_d <= static_cast<double>(buf.size() - p))) {
    throw DecodeError(msg("ccdwg.err.object_past_end", {{"n", size_d}}));
  }
  const auto size = static_cast<std::size_t>(size_d);
  const std::uint64_t hsize = base::clamp_cast<std::uint64_t>(hsize_d);
  if (p + size > buf.size()) {
    throw DecodeError(msg("ccdwg.err.object_past_end", {{"n", static_cast<double>(size)}}));
  }
  const std::uint64_t obj_start = static_cast<std::uint64_t>(p) * 8;
  const std::uint64_t obj_end = (p + size) * 8;
  Bits r(buf, ver, &ctx.cp, obj_start, obj_end);
  const std::int32_t type_num = r.OT();
  std::optional<std::int64_t> endbit;
  if (ver >= 24) {
    endbit = hsize > obj_end ? std::int64_t{-1} : static_cast<std::int64_t>(obj_end - hsize);
  } else if (ver >= 15) {
    endbit = static_cast<std::int64_t>(obj_start) + r.RL();
  }
  if (endbit && (std::cmp_less(*endbit, obj_start) || std::cmp_greater(*endbit, obj_end))) {
    throw DecodeError(msg("ccdwg.err.size_fields"));
  }
  Rec rec;
  rec.handle = r.H();
  rec.type_num = type_num;
  rec.size = size;
  // 20.1: the CRC (seed 0xC0C1) covers the size fields and the data
  if (p + size + 2 <= buf.size()) {
    const std::uint32_t want = buf[p + size] | (static_cast<std::uint32_t>(buf[p + size + 1]) << 8U);
    if (crc8(0xC0C1, buf.subspan(offset, p + size - offset)) != want) {
      rec.crc_bad = true;
    }
  }
  read_eed(r, rec, ctx);

  std::string name(fixed_type(type_num));
  if (type_num >= 500) {
    const auto it = ctx.classes.find(type_num);
    if (it != ctx.classes.end()) {
      name = it->second.dxf_name;
      rec.is_entity = it->second.is_entity;
      rec.class_name = it->second.cpp_name;
    } else {
      name = "CLASS_" + std::to_string(type_num);
      rec.is_entity = false;
      rec.class_name = std::string();
    }
  } else {
    rec.is_entity = entity_fixed(type_num);
  }
  rec.type = !name.empty() ? name : "TYPE_" + std::to_string(type_num);

  if (ver < 15) {
    endbit.reset();  // R13/R14: size in bits comes later
  }
  if (rec.is_entity) {
    common_entity(r, rec, ctx);
  } else {
    common_object(r, rec, ctx);
  }
  if (ver < 15) {
    endbit = static_cast<std::int64_t>(obj_start) + rec.r14bits;
    if (std::cmp_less(*endbit, obj_start) || std::cmp_greater(*endbit, obj_end)) {
      throw DecodeError(msg("ccdwg.err.size_fields"));
    }
  }
  const auto eb = static_cast<std::uint64_t>(endbit.value_or(static_cast<std::int64_t>(obj_end)));

  // R2007+: the string stream sits just before the end of the data part
  std::uint64_t data_end = eb;
  Bits s;
  if (ver >= 21) {
    s = Bits(buf, ver, &ctx.cp, obj_start, eb);
    if (const auto ss = string_stream_start(buf, eb, obj_start)) {
      s.set_pos(ss->start);
      s.set_end(ss->end);
      data_end = ss->start;
    } else {
      s.set_pos(eb);
      s.set_end(eb);
    }
    r.str = &s;
  }
  r.set_end(std::min(obj_end, data_end));
  Bits h(buf, ver, &ctx.cp, eb, obj_end);
  rec.data_end = data_end;

  try {
    if (rec.is_entity) {
      common_entity_handles(h, rec, ctx);
    } else {
      common_object_handles(h, rec);
    }
  } catch (const std::exception& e) {
    rec.error = "handles: " + error_text(e);
  }

  rec.data_start = r.pos();
  rec.h_pos = h.pos();
  rec.endbit = eb;
  rec.obj_end = obj_end;
  rec.buf = buf;
  static const std::unordered_map<std::string_view, std::string_view> kAlias = {
      {"LWPOLYLINE", "LWPOLYLINE"}, {"HATCH", "HATCH"},           {"XRECORD", "XRECORD"},
      {"LAYOUT", "LAYOUT"},         {"OLE2FRAME", "OLE2FRAME"},   {"ACDBPLACEHOLDER", "ACDBPLACEHOLDER"},
      {"GROUP", "GROUP"},           {"MLINESTYLE", "MLINESTYLE"}, {"DICTIONARYWDFLT", "DICTIONARYWDFLT"}};
  const auto al = kAlias.find(name);
  const Decoder dec = find_decoder(al != kAlias.end() ? al->second : std::string_view(name));
  if (dec != nullptr) {
    try {
      dec(r, h, rec, ctx);
    } catch (const std::exception& e) {
      rec.error = (rec.error ? *rec.error + "; " : std::string()) + error_text(e);
    }
  } else {
    rec.unknown = true;
  }
  rec.data_left = static_cast<std::int64_t>(r.end()) - static_cast<std::int64_t>(r.pos());
  r.str = nullptr;
  return rec;
}

}  // namespace cc::dwg
