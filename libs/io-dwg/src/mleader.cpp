// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg mleader-read.js — MULTILEADER (MLEADER) and MLEADERSTYLE into the model, every field.
// Written from the ODA "Open Design Specification for .dwg files" v5.4.1: 20.4.48 (MLEADER), 20.4.86
// (MLeaderAnnotContext), 20.4.87 (MLEADERSTYLE), 20.1 (the R2007+ string stream), 2.13 (handles).
//
//   read_mleader(rec, ctx, res)       → model fields     read.cpp, for a decoded MULTILEADER record
//   read_mleader_style(rec, ctx, res) → model fields     read.cpp, for MLEADERSTYLE
// The record is decoded again from its own bits (objects.cpp leaves data_start/endbit/h_pos/buf), so every field —
// including the ones objects.cpp skips — reaches the model, with handle references as hex strings. The model's
// field list is documented in mleader-read.js.
//
// Spec gap: the spec puts the "Common AcDbAnnotScaleObjectContextData data" (20.4.71) before the leader roots;
// inside MLEADER the context starts directly with the root count — found by builder R's reader on AutoCAD/ODA
// files (objects-read.js) and confirmed here against the ODA File Converter's DXF of every corpus multileader.
// Spec gap: the arrowhead list is present up to R2007 only, the block labels in every version (as 20.4.48's
// indentation suggests) — the corpus multileaders of all six versions decode to the end of their data that way.
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "cc/base/checked.hpp"
#include "parts.hpp"

namespace cc::dwg {

namespace {

/// A data reader, its string-stream reader (R2007+) and a handle reader positioned at the record's own data.
/// Non-copyable: `r.str` points at `s`.
struct Readers {
  Bits r;
  Bits s;
  Bits h;
  Readers(const Rec& rec, const Ctx& ctx)
      : r(rec.buf, ctx.ver, &ctx.cp, rec.data_start, rec.endbit), h(rec.buf, ctx.ver, &ctx.cp, rec.h_pos, rec.obj_end) {
    if (ctx.ver >= 21) {
      const std::uint64_t obj_start = rec.data_start;  // the string stream lies inside [dataStart, endbit)
      s = Bits(rec.buf, ctx.ver, &ctx.cp, obj_start, rec.endbit);
      if (const auto ss = string_stream_start(rec.buf, rec.endbit, obj_start)) {
        s.set_pos(ss->start);
        s.set_end(ss->end);
        r.set_end(ss->start);
      } else {
        s.set_pos(rec.endbit);
        s.set_end(rec.endbit);
      }
      r.str = &s;
    }
  }
  Readers(const Readers&) = delete;
  Readers& operator=(const Readers&) = delete;
  Readers(Readers&&) = delete;
  Readers& operator=(Readers&&) = delete;
  ~Readers() = default;
};

/// hx(h.H(rec.handle))
Value href(Bits& h, const Rec& rec) {
  return {handle_hex(h.H(rec.handle))};
}

Value bool_v(std::uint32_t b) {
  return {b != 0};
}

/// JS nameOf(hex): the named record's name for a non-empty hex handle (undefined otherwise). Null names stay null.
Value name_of_hex(const Resolver& res, const Value& hex) {
  if (!hex.is_string() || hex.as_string().empty()) {
    return {};
  }
  const auto h = static_cast<std::int64_t>(std::stoull(hex.as_string(), nullptr, 16));
  auto n = res.name_of(h);
  return n ? *n : Value();
}

/// a ?? b
Value nullish_or(const Value& a, const Value& b) {
  return a.is_undefined() || a.is_null() ? b : a;
}

Value bd3_pair(Bits& r) {
  Value pair = Value::array();
  pair.push(r.BD3());
  pair.push(r.BD3());
  return pair;
}

/// 20.4.86 MLeaderAnnotContext as embedded in MLEADER.
Value context(Bits& r, Bits& h, const Rec& rec, int ver) {
  Value c = Value::object();
  Value roots = Value::array();
  const std::int64_t nr = fits(r, r.BL(), 8);
  for (std::int64_t i = 0; i < nr; i++) {
    Value root = Value::object();
    root.set("contentValid", bool_v(r.B()));
    root.set("unknown", bool_v(r.B()));
    root.set("connection", r.BD3());
    root.set("direction", r.BD3());
    Value breaks = Value::array();
    const std::int64_t nb = fits(r, r.BL(), 12);
    for (std::int64_t k = 0; k < nb; k++) {
      breaks.push(bd3_pair(r));
    }
    root.set("breaks", std::move(breaks));
    root.set("lines", Value::array());  // the JS object literal's place for `lines`
    Value lines = Value::array();
    root.set("index", r.BL());
    root.set("landingDistance", r.BD());
    const std::int64_t nl = fits(r, r.BL(), 4);
    for (std::int64_t k = 0; k < nl; k++) {
      Value line = Value::object();
      Value points = Value::array();
      const std::int64_t np = fits(r, r.BL(), 6);
      for (std::int64_t j = 0; j < np; j++) {
        points.push(r.BD3());
      }
      line.set("points", std::move(points));
      const std::int32_t bic = r.BL();
      line.set("breakInfoCount", bic);
      if (bic != 0) {
        line.set("segmentIndex", r.BL());
        const std::int64_t ns = fits(r, r.BL(), 12);
        Value lb = Value::array();
        for (std::int64_t j = 0; j < ns; j++) {
          lb.push(bd3_pair(r));
        }
        line.set("breaks", std::move(lb));
      }
      line.set("index", r.BL());
      if (ver >= 24) {
        line.set("leaderType", r.BS());
        line.set("color", r.CMC());
        line.set("linetypeHandle", href(h, rec));
        line.set("lineweight", r.BL());
        line.set("arrowSize", r.BD());
        line.set("arrowheadHandle", href(h, rec));
        line.set("overrideFlags", r.BL());
      }
      lines.push(std::move(line));
    }
    root.set("lines", std::move(lines));
    if (ver >= 24) {
      root.set("attachDirection", r.BS());
    }
    roots.push(std::move(root));
  }
  c.set("roots", std::move(roots));
  c.set("scale", r.BD());
  c.set("contentBase", r.BD3());
  c.set("textHeight", r.BD());
  c.set("arrowSize", r.BD());
  c.set("landingGap", r.BD());
  c.set("leftAttach", r.BS());
  c.set("rightAttach", r.BS());
  c.set("textAlign", r.BS());
  c.set("attachType", r.BS());
  const bool has_text = r.B() != 0;
  c.set("hasText", has_text);
  if (has_text) {
    Value t = Value::object();
    t.set("label", r.TV());
    t.set("normal", r.BD3());
    t.set("styleHandle", href(h, rec));
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
    const std::int64_t ncs = fits(r, r.BL(), 2);
    Value sizes = Value::array();
    for (std::int64_t i = 0; i < ncs; i++) {
      sizes.push(r.BD());
    }
    t.set("columnSizes", std::move(sizes));
    t.set("wordBreak", bool_v(r.B()));
    t.set("unknown", bool_v(r.B()));
    c.set("text", std::move(t));
    c.set("hasBlock", false);
  } else {
    const bool has_block = r.B() != 0;
    c.set("hasBlock", has_block);
    if (has_block) {
      Value b = Value::object();
      b.set("blockHandle", href(h, rec));
      b.set("normal", r.BD3());
      b.set("location", r.BD3());
      b.set("scale", r.BD3());
      b.set("rotation", r.BD());
      b.set("color", r.CMC());
      Value matrix = Value::array();
      for (int i = 0; i < 16; i++) {
        matrix.push(r.BD());
      }
      b.set("matrix", std::move(matrix));
      c.set("block", std::move(b));
    }
  }
  c.set("basePoint", r.BD3());
  c.set("baseDirection", r.BD3());
  c.set("baseVertical", r.BD3());
  c.set("normalReversed", bool_v(r.B()));
  if (ver >= 24) {
    c.set("topAttach", r.BS());
    c.set("bottomAttach", r.BS());
  }
  return c;
}

}  // namespace

std::optional<Value> read_mleader(const Rec& rec, const Ctx& ctx, const Resolver& res) {
  if (rec.buf.empty()) {
    return std::nullopt;
  }
  Readers rd(rec, ctx);
  Bits& r = rd.r;
  Bits& h = rd.h;
  const int ver = ctx.ver;
  Value f = Value::object();
  if (ver >= 24) {
    f.set("version", r.BS());
  }
  f.set("context", context(r, h, rec, ver));
  f.set("styleHandle", href(h, rec));
  f.set("overrideFlags", r.BL());
  f.set("leaderType", r.BS());
  f.set("leaderColor", r.CMC());
  f.set("leaderLinetypeHandle", href(h, rec));
  f.set("leaderLineweight", r.BL());
  f.set("landingEnabled", bool_v(r.B()));
  f.set("doglegEnabled", bool_v(r.B()));
  f.set("landingDistance", r.BD());
  f.set("arrowheadHandle", href(h, rec));
  f.set("arrowSize", r.BD());
  f.set("contentType", r.BS());
  f.set("styleTextStyleHandle", href(h, rec));
  f.set("textLeftAttach", r.BS());
  f.set("textRightAttach", r.BS());
  f.set("textAngleType", r.BS());
  f.set("unknown175", r.BS());
  f.set("textColor", r.CMC());
  f.set("textFrame", bool_v(r.B()));
  f.set("styleBlockHandle", href(h, rec));
  f.set("blockColor", r.CMC());
  f.set("blockScale", r.BD3());
  f.set("blockRotation", r.BD());
  f.set("blockAttach", r.BS());
  f.set("annotative", bool_v(r.B()));
  if (ver < 24) {
    const std::int64_t na = fits(r, r.BL(), 2);
    Value arrows = Value::array();
    for (std::int64_t i = 0; i < na; i++) {
      Value a = Value::object();
      a.set("isDefault", bool_v(r.B()));
      a.set("handle", href(h, rec));
      arrows.push(std::move(a));
    }
    f.set("arrowheads", std::move(arrows));
  }
  const std::int64_t nl = fits(r, r.BL(), 4);
  Value labels = Value::array();
  for (std::int64_t i = 0; i < nl; i++) {
    Value l = Value::object();
    l.set("attdefHandle", href(h, rec));
    l.set("text", r.TV());
    l.set("index", r.BS());
    l.set("width", r.BD());
    labels.push(std::move(l));
  }
  f.set("blockLabels", std::move(labels));
  f.set("textDirectionNegative", bool_v(r.B()));
  f.set("ipeAlign", r.BS());
  f.set("justification", r.BS());
  f.set("scale", r.BD());
  if (ver >= 24) {
    f.set("attachDirection", r.BS());
    f.set("textTopAttach", r.BS());
    f.set("textBottomAttach", r.BS());
  }
  if (ver >= 27) {
    f.set("extendToText", bool_v(r.B()));
  }

  // names, for programs that look things up by name
  const Value style_handle = f["styleHandle"];
  f.set("style", nullish_or(name_of_hex(res, style_handle), style_handle));
  {
    Value* cx = f.find("context");
    Value* block = cx != nullptr ? cx->find("block") : nullptr;
    if (block != nullptr && block->truthy()) {
      const Value bh = (*block)["blockHandle"];
      Value name = name_of_hex(res, bh);
      block->set("name", name);
      f.set("contentBlock", nullish_or(name, bh));  // `block` is not used after this
    }
  }
  {
    const Value& text = f["context"]["text"];
    if (text.truthy()) {
      const Value sh = text["styleHandle"];
      f.set("textStyle", name_of_hex(res, sh));
    }
  }
  const Value lt = f["leaderLinetypeHandle"];
  f.set("leaderLinetype", name_of_hex(res, lt));
  const Value ah = f["arrowheadHandle"];
  f.set("arrowheadName", name_of_hex(res, ah));
  // the proxy graphics (ch. 29) programs without multileaders draw; kept for the writer
  if (rec.gfx_pos && rec.graphics > 0 && std::isfinite(rec.graphics)) {
    const std::uint64_t from = *rec.gfx_pos;
    const auto len = base::clamp_cast<std::uint64_t>(rec.graphics * 8);
    f.set("proxyGraphics", base::base64(slice_bits_of(rec.buf, from, from + len)));
  }
  return f;
}

/// 20.4.87 MLEADERSTYLE. IsNewFormat: R2010+, or the ACAD_MLEADERVER extended data (the only EED it carries).
std::optional<Value> read_mleader_style(const Rec& rec, const Ctx& ctx, const Resolver& /*res*/) {
  if (rec.buf.empty()) {
    return std::nullopt;
  }
  Readers rd(rec, ctx);
  Bits& r = rd.r;
  Bits& h = rd.h;
  const int ver = ctx.ver;
  Value f = Value::object();
  if (ver >= 24) {
    f.set("styleVersion", r.BS());
  }
  f.set("contentType", r.BS());
  f.set("drawMLeaderOrder", r.BS());
  f.set("drawLeaderOrder", r.BS());
  f.set("maxPoints", r.BL());
  f.set("firstAngle", r.BD());
  f.set("secondAngle", r.BD());
  f.set("leaderType", r.BS());
  f.set("leaderColor", r.CMC());
  f.set("leaderLinetype", href(h, rec));
  f.set("leaderLineweight", r.BL());
  f.set("landingEnabled", bool_v(r.B()));
  f.set("landingGap", r.BD());
  f.set("doglegEnabled", bool_v(r.B()));
  f.set("landingDistance", r.BD());
  f.set("description", r.TV());
  f.set("arrowhead", href(h, rec));
  f.set("arrowSize", r.BD());
  f.set("textDefault", r.TV());
  f.set("textStyle", href(h, rec));
  f.set("leftAttach", r.BS());
  f.set("rightAttach", r.BS());
  const bool new_format = ver >= 24 || rec.has_eed;
  if (new_format) {
    f.set("textAngleType", r.BS());
  }
  f.set("textAlign", r.BS());
  f.set("textColor", r.CMC());
  f.set("textHeight", r.BD());
  f.set("textFrame", bool_v(r.B()));
  if (new_format) {
    f.set("alwaysAlignLeft", bool_v(r.B()));
  }
  f.set("alignSpace", r.BD());
  f.set("block", href(h, rec));
  f.set("blockColor", r.CMC());
  f.set("blockScale", r.BD3());
  f.set("blockScaleEnabled", bool_v(r.B()));
  f.set("blockRotation", r.BD());
  f.set("blockRotationEnabled", bool_v(r.B()));
  f.set("blockConnection", r.BS());
  f.set("scale", r.BD());
  f.set("changed", bool_v(r.B()));
  f.set("annotative", bool_v(r.B()));
  f.set("breakSize", r.BD());
  if (ver >= 24) {
    f.set("attachDirection", r.BS());
    f.set("topAttach", r.BS());
    f.set("bottomAttach", r.BS());
  }
  // Spec gap: R2013+ styles have one more bit after the bottom attachment — the ODA File Converter writes it as DXF
  // 298 (0 in every corpus style); found by the one bit left over in the R2013/R2018 corpus styles.
  if (ver >= 27) {
    f.set("unknown298", bool_v(r.B()));
  }
  return f;
}

}  // namespace cc::dwg
