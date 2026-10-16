// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg solid-read.js, surface-read.js, surface-layout.js (reading), acis.js (reading) and acds.js (reading):
// REGION / 3DSOLID / BODY and the SURFACE family (ODA spec v5.4.1 20.4.41; the surface classes are not in the spec,
// their layout was found by experiment in the JS), and the R2013+ data storage section (ch. 24).
#include <algorithm>
#include <array>
#include <map>
#include <string_view>

#include "messages.hpp"
#include "parts.hpp"
#include "record.hpp"

namespace cc::dwg {

using base::Array;
using base::make_object;

namespace {

// ---------------------------------------------------------------- acis.js
std::string latin1_string(std::span<const std::uint8_t> b) {
  std::string s;
  s.reserve(b.size());
  for (const std::uint8_t c : b) {
    base::append_code_point(s, c);
  }
  return s;
}

std::string sat_decrypt(const std::vector<std::uint8_t>& b) {
  std::vector<std::uint8_t> o(b.size());
  for (std::size_t i = 0; i < b.size(); ++i) {
    const std::uint8_t c = b[i];
    if (c >= 0x21 && c <= 0x7E) {
      o[i] = static_cast<std::uint8_t>(0x9F - c);
    } else {
      o[i] = c == 9 ? 32 : c;
    }
  }
  return latin1_string(o);
}

constexpr std::array<std::string_view, 4> kMarks = {std::string_view("End\x0E\x02of\x0E\x04"
                                                                     "ACIS\x0D\x04"
                                                                     "data"),
                                                    std::string_view("End\x0E\x02of\x0E\x03"
                                                                     "ASM\x0D\x04"
                                                                     "data"),
                                                    std::string_view("End-of-ACIS-data"),
                                                    std::string_view("End-of-ASM-data")};

std::int64_t find_acis_end(std::span<const std::uint8_t> b) {
  std::int64_t best = -1;
  for (const std::string_view m : kMarks) {
    const auto hit =
        std::ranges::search(b, m, [](std::uint8_t x, char y) { return x == static_cast<std::uint8_t>(y); });
    if (hit.empty()) {
      continue;
    }
    const auto end = static_cast<std::int64_t>(hit.end() - b.begin());
    if (best < 0 || end < best) {
      best = end;
    }
  }
  return best;
}

std::string head15(std::span<const std::uint8_t> b) {
  std::string s;
  for (std::size_t i = 0; i < 15 && i < b.size(); ++i) {
    s += static_cast<char>(b[i]);
  }
  return s;
}
bool is_sab(std::span<const std::uint8_t> b) {
  const std::string h = head15(b);
  return h == "ACIS BinaryFile" || h == "ASM BinaryFile4";
}
Value sab_version(std::span<const std::uint8_t> b) {
  const std::string h = head15(b);
  if ((h == "ACIS BinaryFile" && b.size() >= 19) || (h == "ASM BinaryFile4" && b.size() >= 23)) {
    return static_cast<double>(static_cast<std::uint32_t>(b[15]) | (static_cast<std::uint32_t>(b[16]) << 8U) |
                               (static_cast<std::uint32_t>(b[17]) << 16U) | (static_cast<std::uint32_t>(b[18]) << 24U));
  }
  return {};
}
/// The first number of a SAT header (acis.js acisInfo('sat')).
Value sat_version(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' ||
                             text[i] == '\f' || text[i] == '\v')) {
    ++i;
  }
  double v = 0;
  bool any = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    v = v * 10 + (text[i] - '0');
    any = true;
    ++i;
  }
  return any ? Value(v) : Value();
}

Value acis_value(std::span<const std::uint8_t> b) {
  if (is_sab(b)) {
    return make_object({{"format", "sab"}, {"version", sab_version(b)}, {"data", base::base64(b)}});
  }
  std::string t = latin1_string(b);
  Value v = sat_version(t);
  return make_object({{"format", "sat"}, {"version", std::move(v)}, {"data", std::move(t)}});
}

// ---------------------------------------------------------------- 20.4.41
struct AcisBlock {
  bool empty = true;
  std::uint32_t unknown = 0;
  int dwg_version = 0;
  Value acis;
};

AcisBlock acis_block(Bits& r, bool with_empty_bit) {
  AcisBlock out;
  if (with_empty_bit && r.B() != 0) {
    return out;
  }
  out.empty = false;
  out.unknown = r.B();
  const std::int32_t version = r.BS();
  if (version == 1) {
    std::vector<std::uint8_t> all;
    for (int guard = 0; guard < 1000000; ++guard) {
      const std::int32_t n = r.BL();
      if (n == 0) {
        break;
      }
      if (static_cast<std::int64_t>(n) * 8 > r.left()) {
        throw DecodeError(msg("ccdwg.err.acis_block", {{"n", n}}));
      }
      const std::vector<std::uint8_t> part = r.bytes(static_cast<std::size_t>(n));
      all.insert(all.end(), part.begin(), part.end());
    }
    std::string text = sat_decrypt(all);
    Value v = sat_version(text);
    out.dwg_version = 1;
    out.acis = make_object({{"format", "sat"}, {"version", std::move(v)}, {"data", std::move(text)}});
    return out;
  }
  if (version == 2) {
    const std::int64_t left = std::max<std::int64_t>(0, r.left() >> 3);
    const std::uint64_t save = r.pos();
    const std::vector<std::uint8_t> all = r.bytes(static_cast<std::size_t>(left));
    const std::int64_t n = find_acis_end(all);
    if (n < 0) {
      throw DecodeError(msg("ccdwg.err.acis_no_end"));
    }
    r.set_pos(save + static_cast<std::uint64_t>(n) * 8);
    out.dwg_version = 2;
    out.acis = acis_value(std::span(all).first(static_cast<std::size_t>(n)));
    return out;
  }
  throw DecodeError(msg("ccdwg.err.acis_version", {{"version", version}}));
}

Value wire(Bits& r) {
  Value w = Value::object();
  w.set("type", r.RC());
  w.set("selectionMarker", r.BL());
  w.set("color", r.BS());
  w.set("acisIndex", r.BL());
  const std::int32_t n = r.BL();
  if (static_cast<std::int64_t>(n) * 6 > r.left()) {
    throw DecodeError(msg("ccdwg.err.wire_large", {{"n", n}}));
  }
  Array pts;
  for (std::int32_t i = 0; i < n; ++i) {
    pts.push_back(r.BD3());
  }
  w.set("points", std::move(pts));
  if (r.B() != 0) {
    Value t = Value::object();
    t.set("xAxis", r.BD3());
    t.set("yAxis", r.BD3());
    t.set("zAxis", r.BD3());
    t.set("translation", r.BD3());
    t.set("scale", r.BD());
    t.set("hasRotation", r.B() != 0);
    t.set("hasReflection", r.B() != 0);
    t.set("hasShear", r.B() != 0);
    w.set("transform", std::move(t));
  }
  return w;
}

Array wires(Bits& r) {
  const std::int32_t n = r.BL();
  if (static_cast<std::int64_t>(n) * 10 > r.left()) {
    throw DecodeError(msg("ccdwg.err.wires_large", {{"n", n}}));
  }
  Array a;
  for (std::int32_t i = 0; i < n; ++i) {
    a.push_back(wire(r));
  }
  return a;
}

std::string hex_pad(std::uint64_t v, int n) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s(static_cast<std::size_t>(n), '0');
  for (int i = n - 1; i >= 0; --i) {
    s[static_cast<std::size_t>(i)] = kHex[v & 15U];
    v >>= 4U;
  }
  return s;
}

std::string guid_string(std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::span<const std::uint8_t> d4) {
  std::string b;
  for (const std::uint8_t x : d4) {
    b += hex_pad(x, 2);
  }
  return "{" + hex_pad(d1, 8) + "-" + hex_pad(d2, 4) + "-" + hex_pad(d3, 4) + "-" + b.substr(0, 4) + "-" + b.substr(4) +
         "}";
}

}  // namespace

void decode_acis_entity(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const int ver = ctx.ver;
  // Spec gap (solid-read.js): R2013+ with the data-storage flag keep the ACIS data in AcDb:AcDsPrototype_1b.
  if (ver >= 27 && rec.ds != 0) {
    rec.set("_acisInDataStorage", true);
  } else {
    AcisBlock a = acis_block(r, true);
    if (!a.empty) {
      rec.set("acis", std::move(a.acis));
      rec.set("_acisUnknownBit", a.unknown);
      rec.set("_acisDwgVersion", a.dwg_version);
    } else {
      rec.set("acisEmpty", true);
    }
  }
  if (r.B() != 0) {
    rec.set("wireframe", Value::object());
    {
      Value* wf = rec.f.find("wireframe");
      if (r.B() != 0) {
        wf->set("point", r.BD3());
      }
      wf->set("isolines", r.BL());
      const bool present = r.B() != 0;
      wf->set("isolinesPresent", present);
      if (present) {
        Array w = wires(r);
        rec.f.find("wireframe")->set("wires", std::move(w));
      }
    }
    const std::int32_t ns = r.BL();
    if (static_cast<std::int64_t>(ns) * 10 > r.left()) {
      throw DecodeError(msg("ccdwg.err.silhouettes_large", {{"n", ns}}));
    }
    rec.set("silhouettes", Array{});
    for (std::int32_t i = 0; i < ns; ++i) {
      Value sil = Value::object();
      sil.set("viewportId", ver >= 24 ? r.BLL() : static_cast<double>(r.BL()));
      sil.set("target", r.BD3());
      sil.set("direction", r.BD3());
      sil.set("up", r.BD3());
      sil.set("perspective", r.B() != 0);
      const bool wp = r.B() != 0;
      sil.set("wiresPresent", wp);
      if (wp) {
        sil.set("wires", wires(r));
      }
      rec.f.find("silhouettes")->push(std::move(sil));
    }
    if (r.B() == 0) {
      AcisBlock a = acis_block(r, false);
      rec.set("acisExtra", std::move(a.acis));
    }
  }
  if (ver >= 21) {
    rec.set("_unknownBL", r.BL());
  }
  if (ver >= 27) {
    rec.set("hasRevisionGuid", r.B() != 0);
    const auto d1 = static_cast<std::uint32_t>(r.BL());
    const std::uint32_t d2 = static_cast<std::uint32_t>(r.BS()) & 0xFFFFU;
    const std::uint32_t d3 = static_cast<std::uint32_t>(r.BS()) & 0xFFFFU;
    const std::vector<std::uint8_t> d4 = r.bytes(8);
    rec.set("revisionGuid", guid_string(d1, d2, d3, d4));
    rec.set("_unknownBL2", r.BL());
  }
  if (ver >= 21 && rec.type == "3DSOLID") {
    rec.set_h("history", h.H(rec.handle));
  }
}

// ---------------------------------------------------------------- surfaces
namespace {

struct SurfField {
  std::string_view name;
  std::string_view op;
};
constexpr auto kSweep = std::to_array<SurfField>({
    {"draftAngle", "BD"},
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
    {"pathEntityTransform", "M"},
});

std::vector<SurfField> layout_of(std::string_view type) {
  std::vector<SurfField> v;
  if (type == "EXTRUDEDSURFACE") {
    v.assign(kSweep.begin(), kSweep.end());
    v.push_back({"sweepVector", "BD3"});
    v.push_back({"matrix", "M"});
    v.push_back({"profileEntity", "ENT"});
  } else if (type == "SWEPTSURFACE") {
    v.assign(kSweep.begin(), kSweep.end());
    v.push_back({"sweepEntityMatrix", "M"});
    v.push_back({"pathEntityMatrix", "M"});
    v.push_back({"sweepEntity", "ENT"});
    v.push_back({"pathEntity", "ENT"});
  } else if (type == "REVOLVEDSURFACE") {
    v = {{"draftAngle", "BD"},
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
  } else if (type == "LOFTEDSURFACE") {
    v = {{"matrix", "M"},
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
  } else if (type == "NURBSURFACE") {
    v = {{"nurbs170", "BS"}, {"nurbs290", "B"},  {"nurbs10", "BD3"},
         {"nurbs11", "BD3"}, {"nurbs12", "BD3"}, {"nurbs13", "BD3"}};
  }
  return v;
}

Value read_field(Bits& r, std::string_view op) {
  if (op == "BD") {
    return r.BD();
  }
  if (op == "BS") {
    return r.BS();
  }
  if (op == "BL") {
    return r.BL();
  }
  if (op == "B") {
    return r.B() != 0;
  }
  if (op == "BD3") {
    return r.BD3();
  }
  if (op == "M") {
    Array m;
    for (int i = 0; i < 16; ++i) {
      m.emplace_back(r.BD());
    }
    return m;
  }
  throw DecodeError(msg("ccdwg.err.surface_code", {{"code", std::string(op)}}));
}

/// Bits [from, to) as a byte array starting at bit 0 (bits-util.js sliceBits).
std::vector<std::uint8_t> slice_bits(Bytes buf, std::uint64_t from, std::uint64_t to) {
  const std::uint64_t n = to > from ? to - from : 0;
  std::vector<std::uint8_t> out(static_cast<std::size_t>((n + 7) >> 3U));
  const auto sh = static_cast<unsigned>(from & 7U);
  const std::uint64_t b0 = from >> 3U;
  auto at = [&](std::uint64_t i) -> std::uint32_t { return i < buf.size() ? buf[static_cast<std::size_t>(i)] : 0U; };
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = sh == 0 ? static_cast<std::uint8_t>(at(b0 + i))
                     : static_cast<std::uint8_t>(((at(b0 + i) << sh) | (at(b0 + i + 1) >> (8U - sh))) & 0xFFU);
  }
  if ((n & 7U) != 0 && !out.empty()) {
    out.back() = static_cast<std::uint8_t>(out.back() & ((0xFFU << (8U - (n & 7U))) & 0xFFU));
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> slice_bits_of(Bytes buf, std::uint64_t from, std::uint64_t to) {
  return slice_bits(buf, from, to);
}

void decode_surface(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  decode_acis_entity(r, h, rec, ctx);
  rec.set("uIsolines", r.BS());
  rec.set("vIsolines", r.BS());
  for (const SurfField& fd : layout_of(rec.type)) {
    if (fd.op != "ENT" && fd.op != "ENTB") {
      rec.set(fd.name, read_field(r, fd.op));
      continue;
    }
    const Value value = fd.op == "ENT" ? Value(r.BL()) : Value(r.B() != 0);
    if (!value.truthy()) {
      continue;
    }
    // embedded data of unknown encoding: keep the rest of the entity's data as it is and stop
    const std::uint64_t n = r.left() > 0 ? static_cast<std::uint64_t>(r.left()) : 0;
    const std::vector<std::uint8_t> bits = slice_bits(r.buf(), r.pos(), r.pos() + n);
    rec.set(
        fd.name,
        make_object(
            {{"value", value},
             {"raw", make_object({{"ver", ctx.ver}, {"n", static_cast<double>(n)}, {"data", base::base64(bits)}})}}));
    r.set_pos(r.end());
    break;
  }
}

// ---------------------------------------------------------------- acds.js (reading)
namespace {

constexpr std::uint32_t kSig = 0x6472616A;
constexpr std::uint32_t kSegSig = 0xD5AC;
constexpr std::uint32_t kBlobRef = 0xBB106BB1;

[[noreturn]] void dv_range() {
  throw DecodeError("Offset is outside the bounds of the DataView");
}
std::uint32_t le32(Bytes b, std::int64_t at) {
  if (at < 0 || static_cast<std::uint64_t>(at) + 4 > b.size()) {
    dv_range();
  }
  const auto a = static_cast<std::size_t>(at);
  return static_cast<std::uint32_t>(b[a]) | (static_cast<std::uint32_t>(b[a + 1]) << 8U) |
         (static_cast<std::uint32_t>(b[a + 2]) << 16U) | (static_cast<std::uint32_t>(b[a + 3]) << 24U);
}
std::uint32_t le16(Bytes b, std::int64_t at) {
  if (at < 0 || static_cast<std::uint64_t>(at) + 2 > b.size()) {
    dv_range();
  }
  const auto a = static_cast<std::size_t>(at);
  return static_cast<std::uint32_t>(b[a]) | (static_cast<std::uint32_t>(b[a + 1]) << 8U);
}
std::int32_t sle32(Bytes b, std::int64_t at) {
  return static_cast<std::int32_t>(le32(b, at));
}
double le64(Bytes b, std::int64_t at) {
  return static_cast<double>(static_cast<std::uint64_t>(le32(b, at)) |
                             (static_cast<std::uint64_t>(le32(b, at + 4)) << 32U));
}

struct Seg {
  std::int64_t off = 0;
  std::string name;
  std::int32_t obj_align = 0;
  std::int64_t data = 0;
};

struct Acds {
  std::map<std::int64_t, std::vector<std::uint8_t>> records;
  std::vector<std::string> notes;
};

Acds read_acds(Bytes sec) {
  Acds out;
  try {
    if (sec.size() < 0x40) {
      out.notes.push_back(msg("ccdwg.read.acds_empty"));
      return out;
    }
    if (le32(sec, 0) != kSig) {
      out.notes.push_back(msg("ccdwg.read.acds_no_sig"));
      return out;
    }
    const std::int32_t seg_idx_offset = sle32(sec, 24);
    const std::int32_t seg_count = sle32(sec, 32);
    const std::int32_t dat_idx = sle32(sec, 40);
    auto seg_at = [&](std::int64_t off) -> Seg {
      if (off < 0 || static_cast<std::uint64_t>(off) + 0x30 > sec.size() || le16(sec, off) != kSegSig) {
        throw DecodeError(msg("ccdwg.err.acds_no_segment", {{"offset", static_cast<double>(off)}}));
      }
      Seg s;
      s.off = off;
      for (int i = 0; i < 6; ++i) {
        s.name += static_cast<char>(sec[static_cast<std::size_t>(off) + 2 + static_cast<std::size_t>(i)]);
      }
      s.obj_align = sle32(sec, off + 36);
      s.data = off + 0x30;
      return s;
    };
    const Seg si = seg_at(seg_idx_offset);
    if (si.name != "segidx") {
      throw DecodeError(msg("ccdwg.err.acds_index", {{"name", si.name}}));
    }
    struct SegEntry {
      double offset;
      std::uint32_t size;
    };
    std::vector<SegEntry> segs;
    for (std::int64_t i = 0, p = si.data; i < seg_count && static_cast<std::uint64_t>(p) + 12 <= sec.size();
         ++i, p += 12) {
      segs.push_back(SegEntry{le64(sec, p), le32(sec, p + 8)});
    }
    auto seg = [&](std::int64_t i) -> Seg {
      if (i < 0 || static_cast<std::size_t>(i) >= segs.size() || segs[static_cast<std::size_t>(i)].offset == 0) {
        throw DecodeError(msg("ccdwg.err.acds_segment", {{"n", static_cast<double>(i)}}));
      }
      return seg_at(static_cast<std::int64_t>(segs[static_cast<std::size_t>(i)].offset));
    };
    const Seg di = seg(dat_idx);
    const std::int32_t n = sle32(sec, di.data);
    struct Entry {
      std::uint32_t seg;
      std::uint32_t local;
    };
    std::vector<Entry> entries;
    for (std::int64_t i = 0, p = di.data + 8; i < n && static_cast<std::uint64_t>(p) + 12 <= sec.size(); ++i, p += 12) {
      const Entry e{le32(sec, p), le32(sec, p + 4)};
      if (e.seg != 0) {
        entries.push_back(e);
      }
    }
    // group by segment, first-seen order (a JS Map)
    std::vector<std::pair<std::uint32_t, std::vector<Entry>>> by_seg;
    for (const Entry& e : entries) {
      auto it = std::ranges::find_if(by_seg, [&](const auto& x) { return x.first == e.seg; });
      if (it == by_seg.end()) {
        by_seg.push_back({e.seg, {e}});
      } else {
        it->second.push_back(e);
      }
    }
    for (const auto& [si2, list] : by_seg) {
      const Seg s = seg(si2);
      if (s.name != "_data_") {
        out.notes.push_back(msg("ccdwg.read.acds_entry", {{"name", s.name}}));
        continue;
      }
      const std::int64_t seg_end = s.off + segs[si2].size;
      struct Head {
        double handle;
        std::uint32_t local;
      };
      std::vector<Head> heads;
      for (const Entry& e : list) {
        const std::int64_t p = s.data + e.local;
        heads.push_back(Head{le64(sec, p + 8), le32(sec, p + 16)});
      }
      const std::int64_t start = s.off + (static_cast<std::int64_t>(s.obj_align) << 4);
      std::vector<std::int64_t> starts;
      starts.reserve(heads.size());
      for (const Head& hd : heads) {
        starts.push_back(start + hd.local);
      }
      std::ranges::sort(starts);
      for (const Head& hd : heads) {
        const std::int64_t at = start + hd.local;
        std::int64_t next_at = seg_end;
        for (const std::int64_t x : starts) {
          if (x > at) {
            next_at = x;
            break;
          }
        }
        const std::int64_t max = next_at - at;
        const std::uint32_t size = le32(sec, at);
        const auto handle = static_cast<std::int64_t>(hd.handle);
        if (size == kBlobRef) {
          // 24.2.2.3.1 a blob reference: the pages live in blob01 segments
          const std::int64_t p = at + 4;
          const double total = le64(sec, p);
          const std::uint32_t pages = le32(sec, p + 8);
          std::vector<std::uint8_t> blob(static_cast<std::size_t>(total));
          std::int64_t q = p + 32;
          for (std::uint32_t k = 0; k < pages; ++k, q += 8) {
            const Seg bs = seg(le32(sec, q));
            const double bstart = le64(sec, bs.data + 8);
            const double bsize = le64(sec, bs.data + 24);
            const auto from = static_cast<std::size_t>(bs.data + 32);
            const auto cnt = static_cast<std::size_t>(std::min(bsize, total - bstart));
            const std::size_t avail = from < sec.size() ? std::min(cnt, sec.size() - from) : 0;
            const auto dst = static_cast<std::size_t>(bstart);
            if (dst + avail <= blob.size()) {
              std::copy_n(sec.begin() + static_cast<std::ptrdiff_t>(from), avail,
                          blob.begin() + static_cast<std::ptrdiff_t>(dst));
            }
          }
          out.records[handle] = std::move(blob);
        } else if (static_cast<std::int64_t>(size) + 4 <= max) {
          const auto from = static_cast<std::size_t>(at + 4);
          const std::size_t cnt = std::min<std::size_t>(size, sec.size() - std::min(sec.size(), from));
          out.records[handle] = std::vector<std::uint8_t>(sec.begin() + static_cast<std::ptrdiff_t>(from),
                                                          sec.begin() + static_cast<std::ptrdiff_t>(from + cnt));
        } else {
          out.notes.push_back(
              msg("ccdwg.read.acds_record_large", {{"handle", base::hex_upper(static_cast<std::uint64_t>(handle))}}));
        }
      }
    }
  } catch (const std::exception& e) {
    out.notes.push_back(msg("ccdwg.read.acds_partial", {{"reason", error_text(e)}}));
  }
  return out;
}

}  // namespace

void attach_data_storage(Records& records, const Bytes* sec, std::vector<std::string>& notes) {
  if (sec == nullptr) {
    int n = 0;
    for (const Rec& r : records.all()) {
      if (r.get("_acisInDataStorage").truthy()) {
        ++n;
      }
    }
    if (n != 0) {
      notes.push_back(msg("ccdwg.read.acis_no_storage", {{"n", n}}));
    }
    return;
  }
  Acds ds = read_acds(*sec);
  for (std::string& n : ds.notes) {
    notes.push_back(std::move(n));
  }
  int missing = 0;
  for (Rec& r : records.all()) {
    if (!r.get("_acisInDataStorage").truthy()) {
      continue;
    }
    const auto it = ds.records.find(r.handle);
    if (it == ds.records.end()) {
      ++missing;
      r.set("acisEmpty", true);
      continue;
    }
    r.set("acis", acis_value(it->second));
  }
  if (missing != 0) {
    notes.push_back(msg("ccdwg.read.acis_no_record", {{"n", missing}}));
  }
}

// ---------------------------------------------------------------- model fields (read.js typeFields)
namespace {
/// ACAD_OBJECT_CHANGE_GUID extended data: [1070 flag][1071][1070][1070][1070 × 8].
bool guid_from_xdata(const base::Array& items, bool& flag, std::string& guid) {
  if (items.size() < 12) {
    return false;
  }
  auto val = [&](std::size_t i) { return items[i].as_array().at(1).num_or(0); };
  std::array<std::uint8_t, 8> d4{};
  for (std::size_t i = 0; i < 8; ++i) {
    d4[i] = static_cast<std::uint8_t>(static_cast<std::int64_t>(val(4 + i)) & 0xFF);
  }
  flag = val(0) != 0;
  guid = guid_string(static_cast<std::uint32_t>(static_cast<std::int64_t>(val(1))),
                     static_cast<std::uint32_t>(static_cast<std::int64_t>(val(2))) & 0xFFFFU,
                     static_cast<std::uint32_t>(static_cast<std::int64_t>(val(3))) & 0xFFFFU, d4);
  return true;
}
}  // namespace

void solid_fields(const Rec& rec, Value& f, const Resolver& res) {
  if (const std::int64_t hh = rec.ref("history"); hh != 0) {
    f.set("history", handle_hex(hh));
  }
  if (!rec.f.has("revisionGuid") && rec.has_eed) {
    for (const Eed& e : rec.eed) {
      if (res.app_name(e.app) == "ACAD_OBJECT_CHANGE_GUID") {
        bool flag = false;
        std::string guid;
        if (guid_from_xdata(e.items, flag, guid)) {
          f.set("hasRevisionGuid", flag);
          f.set("revisionGuid", guid);
        }
        break;
      }
    }
  }
}

void surface_fields(const Rec& rec, Value& f, const Resolver& res) {
  solid_fields(rec, f, res);
  const Rec* dict = rec.ref("xdic") != 0 ? res.get(rec.ref("xdic")) : nullptr;
  if (rec.type != "SURFACE" || dict == nullptr || dict->type != "DICTIONARY" || !dict->get("names").is_array()) {
    return;
  }
  const Array& names = dict->get("names").as_array();
  const Rec* xr = nullptr;
  for (std::size_t k = 0; k < names.size(); ++k) {
    if (names[k].is_string() && names[k].as_string() == "ACAD_XREC_ROUNDTRIP") {
      const auto& items = dict->refs("items");
      xr = k < items.size() ? res.get(items[k]) : nullptr;
      break;
    }
  }
  if (xr == nullptr || xr->type != "XRECORD" || !xr->get("data").is_array()) {
    return;
  }
  const Array& data = xr->get("data").as_array();
  for (std::size_t i = 0; i < data.size(); ++i) {
    const Array& g = data[i].as_array();
    if (g.at(0).num_or(-1) == 102 && g.at(1).is_string() && g.at(1).as_string() == "AcadModlrDecomposedType") {
      if (i + 1 < data.size() && data[i + 1].as_array().at(0).num_or(-1) == 90) {
        const Value& v = data[i + 1].as_array().at(1);
        if (v.num_or(-1) == 1) {
          f.set("savedDownFrom", "NURBSURFACE");
        } else {
          f.set("decomposedType", v);
        }
      }
      return;
    }
  }
}

}  // namespace cc::dwg
