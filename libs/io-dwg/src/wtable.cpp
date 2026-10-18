// SPDX-License-Identifier: Apache-2.0
// table-write.js in C++: editable tables — ACAD_TABLE, TABLESTYLE and FIELD in every version R2000–R2018. The
// "Spec gap:" notes are in the JS file.
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include "cc/base/js.hpp"
#include "messages.hpp"
#include "record.hpp"
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
Value orv(const Value& a, const Value& b) {
  return a.truthy() ? a : b;
}
Value idx(const Value& o, std::size_t i) {  // o?.[i]
  if (o.is_array()) {
    return i < o.as_array().size() ? o.as_array()[i] : Value();
  }
  return get(o, std::to_string(i));
}
Value c_index(double i) {
  return make_object({{"index", i}});
}

/// Colours of table data: the R2004 CMC form in every version
void cmc(ObjWriter& w, const Value& c) {
  const Value& cc = c.truthy() ? c : undef();
  const Value col = cc.truthy() ? cc : c_index(256);
  if (w.r2004) {
    w.CMC(col);
    return;
  }
  w.r2004 = true;
  w.CMC(col);
  w.r2004 = false;
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

ByteBuf utf16z(const Value& s) {
  const auto u = base::to_utf16(str_of(s));
  ByteBuf b((u.size() + 1) * 2, 0);
  for (std::size_t i = 0; i < u.size(); ++i) {
    b[2 * i] = static_cast<std::uint8_t>(u[i] & 0xFFU);
    b[2 * i + 1] = static_cast<std::uint8_t>(u[i] >> 8U);
  }
  return b;
}
ByteBuf hex_bytes(const Value& h) {
  const std::string s = h.truthy() ? str_of(h) : std::string();
  ByteBuf b(s.size() >> 1U, 0);
  for (std::size_t i = 0; i < b.size(); ++i) {
    b[i] = static_cast<std::uint8_t>(static_cast<int>(parse_hex2(s, 2 * i)) & 0xFF);
  }
  return b;
}
void put_bytes(ObjWriter& w, const ByteBuf& b) {
  w.BL(static_cast<double>(b.size()));
  for (const std::uint8_t x : b) {
    w.RC(x);
  }
}

using RefFn = std::function<double(const Value&)>;

void write_value(ObjWriter& w, const Value& v0, const RefFn& rf, bool general_text = false) {
  const Value v = v0.truthy() ? v0 : make_object({{"flags", 3}, {"dataType", 0}});
  int type = static_cast<int>(ivk(v, "dataType"));
  const bool r2007 = w.r2007;
  const int flags = static_cast<int>(ivk(v, "flags"));
  if (!r2007 && (type == 512 || type == 128 || type == 256)) {
    type = tr(v, "text") ? 4 : 0;
  }
  if (r2007) {
    w.BL(flags);
  }
  w.BL(type);
  if (!r2007 || (flags & 1) == 0) {
    switch (type) {
      case 0:
      case 1: w.BL(static_cast<double>(base::js_to_int32(numk(v, "value")))); break;
      case 2: {
        const double d = base::js_to_number(ref(v, "value"));
        w.BD(std::isfinite(d) ? d : 0);
        break;
      }
      case 4: {
        const Value& val = ref(v, "value");
        Value s = val;
        if (val.is_undefined() || !is(ref(v, "dataType"), 4)) {
          s = nullish(ref(v, "text")) ? Value("") : ref(v, "text");
        }
        if (r2007) {
          put_bytes(w, utf16z(s));
        } else {
          w.TV(Value(str_of(s)));
        }
        break;
      }
      case 512:
        if (general_text && !tr(v, "bytes") && tr(v, "text")) {
          put_bytes(w, utf16z(ref(v, "text")));
          break;
        }
        [[fallthrough]];
      case 8:
      case 128:
      case 256: put_bytes(w, hex_bytes(ref(v, "bytes"))); break;
      case 16:
        w.BL(16);
        w.RD(numk(ref(v, "value"), "x"));
        w.RD(numk(ref(v, "value"), "y"));
        break;
      case 32:
        w.BL(24);
        w.RD(numk(ref(v, "value"), "x"));
        w.RD(numk(ref(v, "value"), "y"));
        w.RD(numk(ref(v, "value"), "z"));
        break;
      case 64: w.H(kSoftPtr, rf(ref(v, "id"))); break;
      default: w.BL(0);
    }
  }
  if (r2007) {
    w.BL(ivk(v, "unitType"));
    w.TV(nullish(ref(v, "format")) ? Value("") : ref(v, "format"));
    w.TV(nullish(ref(v, "text")) ? Value("") : ref(v, "text"));
  }
}

}  // namespace

void write_field(ObjWriter& w, const Value& f, const Array& kids, const Array& objs, const RefFn& rf) {
  const auto tv = [&](std::string_view k) { w.TV(nullish(ref(f, k)) ? Value("") : ref(f, k)); };
  tv("evaluatorId");
  tv("code");
  w.BL(static_cast<double>(kids.size()));
  for (const Value& h : kids) {
    w.H(kHardOwn, h);
  }
  w.BL(static_cast<double>(objs.size()));
  for (const Value& h : objs) {
    w.H(kSoftPtr, h);
  }
  if (!w.r2007) {
    const Value& fm = ref(f, "format");
    const Value vf = get(ref(f, "value"), "format");
    if (!nullish(fm)) {
      w.TV(fm);
    } else {
      w.TV(!nullish(vf) ? vf : Value(""));
    }
  }
  w.BL(nnum(ref(f, "evalOptions"), 63));
  w.BL(ivk(f, "filingOptions"));
  w.BL(nnum(ref(f, "state"), 13));
  w.BL(nnum(ref(f, "evalStatus"), 2));
  w.BL(ivk(f, "evalError"));
  tv("evalMessage");
  write_value(w, ref(f, "value"), rf);
  tv("valueString");
  const Value& vsl = ref(f, "valueStringLength");
  w.BL(!nullish(vsl) ? base::js_to_number(vsl)
                     : static_cast<double>(base::utf16_length(str_of(ref(f, "valueString")))));
  const Array& cv = arr(f, "childValues");
  w.BL(static_cast<double>(cv.size()));
  for (const Value& c : cv) {
    w.TV(nullish(ref(c, "key")) ? Value("") : ref(c, "key"));
    write_value(w, ref(c, "value"), rf);
  }
}

namespace {

constexpr std::array<std::pair<int, std::string_view>, 6> kEdges = {
    {{1, "top"}, {2, "right"}, {4, "bottom"}, {8, "left"}, {16, "insideV"}, {32, "insideH"}}};

struct StyleRefs {
  Writer* W = nullptr;
  [[nodiscard]] double style(const Value& name) const {
    return name.truthy() ? W->style_ref(name) : W->style_ref(Value("Standard"));
  }
  [[nodiscard]] double ltype(const Value& name) const { return num(get(W->ltype_ref(name), "h")); }
  [[nodiscard]] double src(const Value& hex) const { return W->src(hex); }
};

void write_content_format(ObjWriter& w, const Value& c, const StyleRefs& R) {
  w.BL(ivk(c, "overrides"));
  w.BL(ivk(c, "propertyFlags"));
  w.BL(nnum(ref(c, "dataType"), 512));
  w.BL(ivk(c, "unitType"));
  w.TV(nullish(ref(c, "format")) ? Value("") : ref(c, "format"));
  w.BD(numk(c, "rotation"));
  w.BD(numk(c, "blockScale", 1));
  w.BL(ref(c, "align").truthy() ? base::js_to_number(ref(c, "align")) : 1);
  cmc(w, ref(c, "color").truthy() ? ref(c, "color") : c_index(0));
  w.H(kHardPtr, ref(c, "textStyle").truthy() ? R.style(ref(c, "textStyle")) : 0);
  w.BD(numk(c, "textHeight", 0.18));
}

void write_cell_style(ObjWriter& w, const Value& s, const StyleRefs& R) {
  w.BL(nnum(get(s, "type"), 1));
  const bool has = s.truthy() && !ref(s, "overrides").is_undefined();
  w.BS(has ? 1 : 0);
  if (!has) {
    return;
  }
  w.BL(ivk(s, "overrides"));
  w.BL(ivk(s, "mergeFlags"));
  cmc(w, ref(s, "background").truthy() ? ref(s, "background") : c_index(257));
  w.BL(nnum(ref(s, "contentLayout"), 1));
  write_content_format(w, ref(s, "content").truthy() ? ref(s, "content") : Value::object(), R);
  if (ref(s, "margins").truthy()) {
    w.BS(1);
    const Value& m = ref(s, "margins");
    for (const char* k : {"vertical", "horizontal", "bottom", "right", "hSpacing", "vSpacing"}) {
      w.BD(numk(m, k));
    }
  } else {
    w.BS(0);
  }
  const Value& borders = ref(s, "borders");
  std::vector<std::pair<int, std::string_view>> bs;
  for (const auto& e : kEdges) {
    if (borders.truthy() && tr(borders, e.second)) {
      bs.push_back(e);
    }
  }
  w.BL(static_cast<double>(bs.size()));
  for (const auto& [e, k] : bs) {
    const Value& b = ref(borders, k);
    w.BL(e);
    w.BL(ivk(b, "overrides"));
    w.BL(nnum(ref(b, "type"), 1));
    cmc(w, ref(b, "color").truthy() ? ref(b, "color") : c_index(0));
    w.BL(static_cast<double>(base::js_to_int32(numk(b, "lineweight", -2))));
    w.H(kHardPtr, ref(b, "linetype").truthy() ? R.ltype(ref(b, "linetype")) : 0);
    w.BL(is_false(ref(b, "visible")) ? 1 : 0);
    w.BD(numk(b, "doubleSpacing", 0.045));
  }
}
void write_named_cell_style(ObjWriter& w, const Value& s, const StyleRefs& R) {
  write_cell_style(w, s, R);
  w.BL(ivk(s, "id"));
  w.BL(nnum(ref(s, "styleClass"), 2));
  w.TV(nullish(ref(s, "name")) ? Value("") : ref(s, "name"));
}

Value default_style_name_of(double id) {
  if (id == 1) {
    return "_TITLE";
  }
  if (id == 2) {
    return "_HEADER";
  }
  if (id == 3) {
    return "_DATA";
  }
  if (id == 4) {
    return "Table";
  }
  return {};
}

Value border_rec(const Value& color, double lineweight, bool visible) {
  return make_object({{"overrides", 0},
                      {"type", 1},
                      {"color", color},
                      {"lineweight", lineweight},
                      {"visible", visible},
                      {"doubleSpacing", 0.045}});
}

Value r24_cell_style(const Value& cs, const Value& st) {
  const Value& cell = ref(cs, "cell");
  if (cell.truthy() && tr(cell, "content")) {
    Value o = cell;
    o.set("id", ref(cs, "id"));
    o.set("name", ref(cs, "name"));
    o.set("styleClass", nullish(ref(cs, "styleClass")) ? ref(cell, "styleClass") : ref(cs, "styleClass"));
    return o;
  }
  Value borders = Value::object();
  for (const auto& [e, k] : kEdges) {
    const Value b = get(ref(cs, "borders"), k);
    borders.set(k, border_rec(get(b, "color").truthy() ? get(b, "color") : c_index(0), numk(b, "lineweight", -2),
                              !is_false(get(b, "visible"))));
  }
  const double hm = numk(st, "hMargin", 0.06);
  const double vm = numk(st, "vMargin", 0.06);
  const double id = num(ref(cs, "id"), std::nan(""));
  const Value& al = ref(cs, "align");
  Value content = make_object({{"overrides", 0},
                               {"propertyFlags", 0},
                               {"dataType", nullish(ref(cs, "dataType")) ? Value(512) : ref(cs, "dataType")},
                               {"unitType", ivk(cs, "unitType")},
                               {"format", nullish(ref(cs, "format")) ? Value("") : ref(cs, "format")},
                               {"rotation", numk(cs, "rotation")},
                               {"blockScale", 1},
                               {"align", al.truthy() ? al : Value(id == 3 ? 2 : 5)},
                               {"color", ref(cs, "color").truthy() ? ref(cs, "color") : c_index(0)},
                               {"textStyle", ref(cs, "textStyle")},
                               {"textHeight", numk(cs, "textHeight", 0.18)}});
  return make_object({{"type", 5},
                      {"overrides", 0},
                      {"mergeFlags", id == 1 ? 0x8000 : 0},
                      {"background", tr(cs, "backgroundEnabled") ? ref(cs, "background") : c_index(257)},
                      {"contentLayout", 1},
                      {"content", std::move(content)},
                      {"margins", make_object({{"vertical", vm},
                                               {"horizontal", hm},
                                               {"bottom", vm},
                                               {"right", hm},
                                               {"hSpacing", hm * 3},
                                               {"vSpacing", vm * 3}})},
                      {"borders", std::move(borders)},
                      {"id", ref(cs, "id")},
                      {"name", ref(cs, "name")},
                      {"styleClass", nullish(ref(cs, "styleClass")) ? Value(id == 3 ? 1 : 2) : ref(cs, "styleClass")}});
}

Value default_table_style() {
  const auto cs = [](double id, const char* name, double cls, double th, double al) {
    return make_object({{"id", id},
                        {"name", name},
                        {"styleClass", cls},
                        {"textStyle", "Standard"},
                        {"textHeight", th},
                        {"align", al},
                        {"color", c_index(0)},
                        {"background", c_index(257)},
                        {"backgroundEnabled", false}});
  };
  return make_object(
      {{"name", "Standard"},
       {"description", ""},
       {"flowDirection", 0},
       {"flags", 0},
       {"hMargin", 0.06},
       {"vMargin", 0.06},
       {"titleSuppressed", false},
       {"headerSuppressed", false},
       {"cellStyles", Array{cs(3, "_DATA", 1, 0.18, 2), cs(1, "_TITLE", 2, 0.25, 5), cs(2, "_HEADER", 2, 0.18, 5)}}});
}

/// ENC.TABLESTYLE (after the caller settled reactors and xdic)
void encode_table_style(ObjWriter& w, const Value& r, const Value& st, const StyleRefs& R) {
  object_common(w, r);
  const Array& cells = arr(st, "cellStyles");
  const auto by_id = [&](double id) -> Value {
    for (const Value& c : cells) {
      if (is(ref(c, "id"), id)) {
        return c;
      }
    }
    return make_object({{"id", id},
                        {"name", default_style_name_of(id)},
                        {"textHeight", id == 1 ? 0.25 : 0.18},
                        {"align", id == 3 ? 2 : 5}});
  };
  if (w.r2010) {
    w.RC(iv(get(ref(st, "unknown"), "rc")));
    w.TV(nullish(ref(st, "description")) ? Value("") : ref(st, "description"));
    w.BL(nnum(ref(st, "flags"), 8));
    w.BL(nnum(get(ref(st, "unknown"), "bl2"), 101));
    w.H(kHardOwn, 0);
    Value tcs = ref(st, "tableCellStyle");
    if (!tcs.truthy()) {
      const double vm = numk(st, "vMargin", 0.06);
      const double hm = numk(st, "hMargin", 0.06);
      Value borders = Value::object();
      for (const auto& [e, k] : kEdges) {
        borders.set(k, border_rec(c_index(0), -2, true));
      }
      tcs = make_object({{"type", 5},
                         {"overrides", 0},
                         {"mergeFlags", tr(st, "flowDirection") ? 0x10000 : 0},
                         {"background", c_index(257)},
                         {"contentLayout", 1},
                         {"content", make_object({{"overrides", 0},
                                                  {"propertyFlags", 0},
                                                  {"dataType", 4},
                                                  {"unitType", 0},
                                                  {"format", ""},
                                                  {"rotation", 0},
                                                  {"blockScale", 1},
                                                  {"align", 1},
                                                  {"color", c_index(0)},
                                                  {"textHeight", 0.18}})},
                         {"margins", make_object({{"vertical", vm},
                                                  {"horizontal", hm},
                                                  {"bottom", vm},
                                                  {"right", hm},
                                                  {"hSpacing", 0.18},
                                                  {"vSpacing", 0.18}})},
                         {"borders", std::move(borders)}});
    } else {
      tcs = Value(tcs);
    }
    tcs.set("id", 4);
    tcs.set("styleClass", 2);
    tcs.set("name", "Table");
    write_named_cell_style(w, tcs, R);
    std::vector<Value> list{r24_cell_style(by_id(1), st), r24_cell_style(by_id(2), st), r24_cell_style(by_id(3), st)};
    for (const Value& c : cells) {
      if (num(ref(c, "id")) > 4) {
        list.push_back(r24_cell_style(c, st));
      }
    }
    w.BL(static_cast<double>(list.size()));
    for (const Value& s : list) {
      w.BL(base::js_to_number(ref(s, "id")));
      write_named_cell_style(w, s, R);
    }
    return;
  }
  w.TV(nullish(ref(st, "description")) ? Value("") : ref(st, "description"));
  w.BS(ivk(st, "flowDirection"));
  w.BS(tr(st, "tableCellStyle") ? 0 : static_cast<double>(base::js_to_int32(ivk(st, "flags")) & 0x7FFF));
  w.BD(numk(st, "hMargin", 0.06));
  w.BD(numk(st, "vMargin", 0.06));
  w.B(tr(st, "titleSuppressed"));
  w.B(tr(st, "headerSuppressed"));
  for (const double id : {3.0, 1.0, 2.0}) {
    const Value cs = by_id(id);
    w.H(kHardPtr, R.style(ref(cs, "textStyle")));
    w.BD(numk(cs, "textHeight", 0.18));
    w.BS(ref(cs, "align").truthy() ? base::js_to_number(ref(cs, "align")) : 5);
    cmc(w, ref(cs, "color").truthy() ? ref(cs, "color") : c_index(0));
    cmc(w, ref(cs, "background").truthy() ? ref(cs, "background") : c_index(257));
    w.B(tr(cs, "backgroundEnabled"));
    for (const char* k : {"top", "insideH", "bottom", "left", "insideV", "right"}) {
      const Value b = get(ref(cs, "borders"), k);
      w.BS(numk(b, "lineweight", -2));
      w.B(!is_false(get(b, "visible")));
      cmc(w, get(b, "color").truthy() ? get(b, "color") : c_index(0));
    }
    if (w.r2007) {
      w.BL(nnum(ref(cs, "dataType"), 512));
      w.BL(ivk(cs, "unitType"));
      w.TV(nullish(ref(cs, "format")) ? Value("") : ref(cs, "format"));
    }
  }
}

// ---------------------------------------------------------------- ACAD_TABLE

constexpr std::array<std::pair<std::string_view, int>, 4> kSideBit = {
    {{"top", 0}, {"right", 1}, {"bottom", 2}, {"left", 3}}};
int side_bit(std::string_view s) {
  for (const auto& [k, b] : kSideBit) {
    if (k == s) {
      return b;
    }
  }
  return -1;
}

struct Edge {
  int flags = 0;
  int virt = 0;
  Value vprops = Value::object();
};

/// edgeMap: border overrides per cell edge, with the neighbour across each edge
struct EdgeMap {
  std::map<std::pair<long, long>, Edge> m;
  Edge& E(long i, long j) { return m[{i, j}]; }
};
EdgeMap edge_map(const Value& T) {
  EdgeMap em;
  const long nr = static_cast<long>(arr(T, "rows").size());
  const long nc = static_cast<long>(arr(T, "columns").size());
  const Array& cells = arr(T, "cells");
  for (long i = 0; i < nr; ++i) {
    const Value row = std::cmp_less(i, cells.size()) ? cells[static_cast<std::size_t>(i)] : Value();
    for (long j = 0; j < nc; ++j) {
      const Value c = idx(row, static_cast<std::size_t>(j));
      if (!c.truthy()) {
        continue;
      }
      Edge& e = em.E(i, j);
      e.flags |= static_cast<int>(iv(get(ref(c, "edges"), "flags")));
      e.virt |= static_cast<int>(iv(get(ref(c, "edges"), "virtual")));
      const Value borders = get(ref(c, "overrides"), "borders");
      if (!borders.is_object()) {
        continue;
      }
      for (const base::Member* mb : base::js_members(borders.as_object())) {
        const int k = side_bit(mb->key);
        if (k < 0 || !mb->value.truthy()) {
          continue;
        }
        Edge& e2 = em.E(i, j);
        e2.flags |= 1 << k;
        e2.virt &= ~(1 << k);
        std::string_view os;
        long di = 0;
        long dj = 0;
        if (mb->key == "top") {
          os = "bottom";
          di = -1;
        } else if (mb->key == "right") {
          os = "left";
          dj = 1;
        } else if (mb->key == "bottom") {
          os = "top";
          di = 1;
        } else {
          os = "right";
          dj = -1;
        }
        const long ni = i + di;
        const long nj = j + dj;
        if (ni < 0 || nj < 0 || ni >= nr || nj >= nc) {
          continue;
        }
        Edge& n = em.E(ni, nj);
        const int ok = side_bit(os);
        if (ok < 0) {
          continue;
        }
        n.flags |= 1 << ok;
        n.virt |= 1 << ok;
        n.vprops.set(os, mb->value);
      }
    }
  }
  return em;
}

struct TableRefs {
  StyleRefs S;
  double table_style = 0;
  std::unordered_map<std::string, double> style_ids;  // upper name → id
  [[nodiscard]] double style_id(const Value& name) const {
    const auto it = style_ids.find(upper(nullish(name) ? Value("") : name));
    return it == style_ids.end() ? 0 : it->second;
  }
  [[nodiscard]] double block(const Value& cell) const {
    const auto it = S.W->block_by_name.find(upper(nullish(ref(cell, "block")) ? Value("") : ref(cell, "block")));
    return it != S.W->block_by_name.end() ? it->second->hdr : S.W->src(ref(cell, "blockRecord"));
  }
  [[nodiscard]] static double field(const Value& cell) { return num(ref(cell, "__fieldH")); }
};

bool is_text(const Value& c) {
  return !is_str(ref(c, "type"), "block");
}

Value cell_value(const Value& c) {
  const Value& d = ref(c, "data");
  if (d.truthy() && (base::js_to_int32(base::js_to_number(ref(d, "flags"))) & 1) == 0 &&
      !ref(d, "dataType").is_undefined()) {
    Value v = d;
    const Value& cv = ref(c, "value");
    if (is(ref(v, "dataType"), 4)) {
      v.set("value", nullish(cv) ? ref(d, "value") : cv);
    } else if (is(ref(v, "dataType"), 512)) {
      v.set("text", nullish(cv) ? ref(d, "text") : cv);
    }
    return v;
  }
  const Value& cv = ref(c, "value");
  if (is_str(cv, "") || cv.is_undefined()) {
    return make_object({{"flags", 3}, {"dataType", 0}});
  }
  return make_object({{"flags", 0}, {"dataType", 4}, {"value", str_of(cv)}});
}

Value default_cell() {
  return make_object({{"type", "text"}, {"value", ""}});
}

Value cell_at(const Value& T, std::size_t i, std::size_t j) {
  const Value c = idx(idx(ref(T, "cells"), i), j);
  return c.truthy() ? c : default_cell();
}

void old_table(ObjWriter& w, const Value& r, const TableRefs& R) {
  const Value& T = ref(r, "table");
  const std::size_t nr = arr(T, "rows").size();
  const std::size_t nc = arr(T, "columns").size();
  const Value& tf = ref(T, "flags");
  w.BS(!nullish(tf)
           ? base::js_to_number(tf)
           : static_cast<double>(6 | (tr(T, "flowDirection") ? 0 : 0x10) | (tr(T, "titleSuppressed") ? 0x20 : 0)));
  w.P3(orv(ref(r, "horizDir"), make_object({{"x", 1}, {"y", 0}, {"z", 0}})));
  w.BL(static_cast<double>(nc));
  w.BL(static_cast<double>(nr));
  for (const Value& c : arr(T, "columns")) {
    w.BD(numk(c, "width", 1));
  }
  for (const Value& x : arr(T, "rows")) {
    w.BD(numk(x, "height", 0.25));
  }
  w.H(kHardPtr, ref(r, "styleH"));
  std::map<std::pair<double, double>, const Value*> merge_at;
  std::set<std::pair<double, double>> covered;
  for (const Value& m : arr(T, "merged")) {
    merge_at[{num(ref(m, "top")), num(ref(m, "left"))}] = &m;
  }
  for (const Value& m : arr(T, "merged")) {
    // The JS walks i from top to bottom (and j likewise) and marks "i,j": only whole numbers inside the grid are ever
    // looked up, so the walk is limited to those (same cells, no runaway loop on hostile values).
    const double top = num(ref(m, "top"));
    const double left = num(ref(m, "left"));
    if (std::trunc(top) != top || std::trunc(left) != left) {
      continue;  // a fractional start marks no grid cell
    }
    const double bottom = std::fmin(std::floor(num(ref(m, "bottom"))), static_cast<double>(nr));
    const double right = std::fmin(std::floor(num(ref(m, "right"))), static_cast<double>(nc));
    for (auto i = static_cast<long>(std::fmax(top, 0)); static_cast<double>(i) <= bottom; ++i) {
      for (auto j = static_cast<long>(std::fmax(left, 0)); static_cast<double>(j) <= right; ++j) {
        if (static_cast<double>(i) != top || static_cast<double>(j) != left) {
          covered.insert({static_cast<double>(i), static_cast<double>(j)});
        }
      }
    }
  }
  EdgeMap E = edge_map(T);
  for (std::size_t i = 0; i < nr; ++i) {
    for (std::size_t j = 0; j < nc; ++j) {
      const Value c = cell_at(T, i, j);
      const std::pair<double, double> key{static_cast<double>(i), static_cast<double>(j)};
      const auto mit = merge_at.find(key);
      const Value* m = mit == merge_at.end() ? nullptr : mit->second;
      Edge const& ed = E.E(static_cast<long>(i), static_cast<long>(j));
      const bool text = is_text(c);
      w.BS(text ? 1 : 2);
      w.RC(ed.flags);
      w.B(covered.contains(key) || tr(c, "covered"));
      w.B(tr(c, "autoFit"));
      w.BL(m != nullptr ? num(ref(*m, "right")) - num(ref(*m, "left")) + 1 : 1);
      w.BL(m != nullptr ? num(ref(*m, "bottom")) - num(ref(*m, "top")) + 1 : 1);
      w.BD(numk(c, "rotation"));
      const double field_h = text && tr(c, "field") ? TableRefs::field(c) : 0;
      if (text) {
        w.H(kHardPtr, field_h);
        if (field_h == 0 && !w.r2007) {
          w.TV(Value(str_of(ref(c, "value"))));
        }
      } else {
        w.H(kHardPtr, R.block(c));
        w.BD(numk(c, "blockScale", 1));
        std::vector<std::pair<const Value*, double>> at;
        for (const Value& a : arr(c, "attrs")) {
          if (const double h = R.S.src(ref(a, "attdef")); h != 0) {
            at.emplace_back(&a, h);
          }
        }
        w.B(!at.empty());
        if (!at.empty()) {
          w.BS(static_cast<double>(at.size()));
          for (const auto& [a, h] : at) {
            w.H(kSoftPtr, h);
            w.BS(ivk(*a, "index"));
            w.TV(Value(str_of(ref(*a, "value"))));
          }
        }
      }
      const Value ov = ref(c, "overrides").truthy() ? ref(c, "overrides") : Value::object();
      int fl = 0;
      if (!ref(ov, "align").is_undefined()) {
        fl |= 0x01;
      }
      if (!ref(ov, "fillNone").is_undefined()) {
        fl |= 0x02;
      }
      if (tr(ov, "background")) {
        fl |= 0x04;
      }
      if (tr(ov, "color")) {
        fl |= 0x08;
      }
      if (!ref(ov, "textStyle").is_undefined()) {
        fl |= 0x10;
      }
      if (!ref(ov, "textHeight").is_undefined()) {
        fl |= 0x20;
      }
      Value bo = ed.vprops;
      if (const Value& ob = ref(ov, "borders"); ob.is_object()) {
        for (const base::Member& mb : ob.as_object()) {
          bo.set(mb.key, mb.value);
        }
      }
      for (const auto& [side, k] : kSideBit) {
        const Value& b = ref(bo, side);
        if (!b.truthy()) {
          continue;
        }
        if (tr(b, "color")) {
          fl |= 0x40 << k;
        }
        if (!ref(b, "lineweight").is_undefined()) {
          fl |= 0x400 << k;
        }
        if (!ref(b, "visible").is_undefined()) {
          fl |= 0x4000 << k;
        }
      }
      const Value val = text && field_h == 0 ? cell_value(c) : make_object({{"flags", 3}, {"dataType", 0}});
      if (w.r2007 && text && field_h == 0 && (base::js_to_int32(base::js_to_number(ref(val, "flags"))) & 1) == 0) {
        fl |= 0x40000;
      }
      w.B(fl != 0 || ed.virt != 0);
      if (fl != 0 || ed.virt != 0) {
        w.BL(fl);
        w.RC(ed.virt);
        if ((fl & 0x01) != 0) {
          w.BS(ivk(ov, "align"));
        }
        if ((fl & 0x02) != 0) {
          w.B(tr(ov, "fillNone"));
        }
        if ((fl & 0x04) != 0) {
          cmc(w, ref(ov, "background"));
        }
        if ((fl & 0x08) != 0) {
          cmc(w, ref(ov, "color"));
        }
        if ((fl & 0x10) != 0) {
          w.H(kHardPtr, R.S.style(ref(ov, "textStyle")));
        }
        if ((fl & 0x20) != 0) {
          w.BD(numk(ov, "textHeight", 0.18));
        }
        for (const auto& [side, k] : kSideBit) {
          const Value& b = ref(bo, side);
          if (!b.truthy() || (ed.virt & (1 << k)) != 0) {
            continue;
          }
          if ((fl & (0x40 << k)) != 0) {
            cmc(w, ref(b, "color"));
          }
          if ((fl & (0x400 << k)) != 0) {
            w.BS(numk(b, "lineweight", -2));
          }
          if ((fl & (0x4000 << k)) != 0) {
            w.BS(is_false(ref(b, "visible")) ? 1 : 0);
          }
        }
      }
      if (w.r2007) {
        w.BL(0);
        write_value(w, val, [&](const Value& h) { return R.S.src(h); }, true);
      }
    }
  }
  const Value& o = ref(T, "overrides");
  const int f = o.truthy() ? base::js_to_int32(ivk(T, "overrideFlags")) : 0;
  w.B(f != 0);
  if (f != 0) {
    w.BL(f);
    if ((f & 1) != 0) {
      w.B(tr(o, "titleSuppressed"));
    }
    if ((f & 4) != 0) {
      w.BS(ivk(o, "flowDirection"));
    }
    if ((f & 8) != 0) {
      w.BD(numk(o, "hMargin"));
    }
    if ((f & 0x10) != 0) {
      w.BD(numk(o, "vMargin"));
    }
    const std::array<std::string, 3> rows = {"title", "header", "data"};
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x20 << i)) != 0) {
        cmc(w, ref(o, rows[static_cast<std::size_t>(i)] + "Color"));
      }
    }
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x100 << i)) != 0) {
        w.B(tr(o, rows[static_cast<std::size_t>(i)] + "FillNone"));
      }
    }
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x800 << i)) != 0) {
        cmc(w, ref(o, rows[static_cast<std::size_t>(i)] + "Fill"));
      }
    }
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x4000 << i)) != 0) {
        w.BS(ivk(o, rows[static_cast<std::size_t>(i)] + "Align"));
      }
    }
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x20000 << i)) != 0) {
        w.H(kHardPtr, R.S.style(ref(o, rows[static_cast<std::size_t>(i)] + "Style")));
      }
    }
    for (int i = 0; i < 3; ++i) {
      if ((f & (0x100000 << i)) != 0) {
        w.BD(numk(o, rows[static_cast<std::size_t>(i)] + "Height"));
      }
    }
  }
  for (int which = 0; which < 3; ++which) {
    static constexpr std::array<const char*, 3> kNames = {"borderColors", "borderLineweights", "borderVisibility"};
    const char* k = kNames[static_cast<std::size_t>(which)];
    const Value& b = ref(T, k);
    w.B(b.truthy());
    if (b.truthy()) {
      const int bf = base::js_to_int32(ivk(b, "flags"));
      w.BL(bf);
      for (int i = 0; i < 18; ++i) {
        if ((bf & (1 << i)) != 0) {
          const Value v = idx(b, static_cast<std::size_t>(i));
          if (which == 0) {
            cmc(w, v);
          } else if (which == 1) {
            w.BS(num(v, -2));
          } else {
            w.BS(iv(v));
          }
        }
      }
    }
  }
}

/// R24 cell style of a cell: its own (if the source had one) with the model's overrides applied
Value cell_style_for(const Value& c, const Value& vprops) {
  const Value ov0 = ref(c, "overrides").truthy() ? ref(c, "overrides") : Value::object();
  Value ov = ov0;
  if (!vprops.as_object().empty()) {
    Value b = vprops;
    if (const Value& ob = ref(ov0, "borders"); ob.is_object()) {
      for (const base::Member& m : ob.as_object()) {
        b.set(m.key, m.value);
      }
    }
    ov.set("borders", std::move(b));
  }
  const Value& cs = ref(c, "cellStyle");
  const bool has_base = cs.truthy() && !ref(cs, "overrides").is_undefined();
  const double rot = numk(c, "rotation");
  if (!has_base && ov.as_object().empty() && rot == 0) {
    return make_object({{"type", 1}});
  }
  Value s;
  if (has_base) {
    s = cs;
  } else {
    const Value& al = ref(c, "align");
    s = make_object({{"type", 1},
                     {"overrides", 0},
                     {"mergeFlags", 0},
                     {"background", c_index(257)},
                     {"contentLayout", 1},
                     {"content", make_object({{"overrides", 0},
                                              {"propertyFlags", 0},
                                              {"dataType", 512},
                                              {"unitType", 0},
                                              {"format", ""},
                                              {"rotation", 0},
                                              {"blockScale", 1},
                                              {"align", al.truthy() ? al : Value(1)},
                                              {"color", c_index(0)},
                                              {"textHeight", 0.18}})},
                     {"borders", Value::object()}});
  }
  const int s_over = base::js_to_int32(base::js_to_number(ref(s, "overrides")));
  int fl = s_over & ~(0x4 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200);
  if (!s.has("content")) {
    s.set("content", Value::object());
  }
  Value& ct = *s.find("content");
  if (!ref(ov, "align").is_undefined()) {
    fl |= 0x10;
    ct.set("align", ref(ov, "align"));
  }
  if (tr(ov, "color")) {
    fl |= 0x20;
    ct.set("color", ref(ov, "color"));
  }
  if (!ref(ov, "textStyle").is_undefined()) {
    fl |= 0x40;
    ct.set("textStyle", ref(ov, "textStyle"));
  }
  if (!ref(ov, "textHeight").is_undefined()) {
    fl |= 0x80;
    ct.set("textHeight", ref(ov, "textHeight"));
  }
  if (tr(ov, "background") || tr(ov, "fillNone")) {
    fl |= 0x200;
    s.set("background", tr(ov, "fillNone") ? c_index(257) : ref(ov, "background"));
  }
  Value& ct2 = *s.find("content");
  if (rot != 0) {
    fl |= 0x4;
    ct2.set("rotation", rot);
  } else if ((s_over & 0x4) == 0) {
    ct2.set("rotation", 0);
  }
  ct2.set("overrides", static_cast<double>(fl & 0x1FF));
  s.set("overrides", static_cast<double>(fl));
  if (const Value& ob = ref(ov, "borders"); ob.is_object()) {
    if (!s.has("borders") || !ref(s, "borders").is_object()) {
      s.set("borders", Value::object());
    }
    for (const base::Member* m : base::js_members(ob.as_object())) {
      Value& borders = *s.find("borders");
      if (!borders.has(m->key) || !ref(borders, m->key).truthy()) {
        borders.set(m->key, border_rec(c_index(0), -2, true));
      }
      Value& x = *borders.find(m->key);
      x.set("overrides", static_cast<double>(base::js_to_int32(ivk(x, "overrides")) | 26));
      const Value& b = m->value;
      if (tr(b, "color")) {
        x.set("color", ref(b, "color"));
      }
      if (!ref(b, "lineweight").is_undefined()) {
        x.set("lineweight", ref(b, "lineweight"));
      }
      if (!ref(b, "visible").is_undefined()) {
        x.set("visible", tr(b, "visible"));
      }
    }
  }
  return s;
}

void new_table(ObjWriter& w, const Value& r, const TableRefs& R) {
  const Value& T = ref(r, "table");
  const std::size_t nr = arr(T, "rows").size();
  const std::size_t nc = arr(T, "columns").size();
  EdgeMap E = edge_map(T);
  w.RC(0);
  w.H(kSoftPtr, 0);
  w.BL(0);
  if (w.r2013) {
    w.BL(0);
  } else {
    w.B(true);
  }
  w.TV(nullish(ref(T, "name")) ? Value("") : ref(T, "name"));
  w.TV(nullish(ref(T, "description")) ? Value("") : ref(T, "description"));
  w.BL(static_cast<double>(nc));
  for (const Value& col : arr(T, "columns")) {
    w.TV(nullish(ref(col, "name")) ? Value("") : ref(col, "name"));
    w.BL(0);
    w.BL(0);
    write_cell_style(w, ref(col, "cellStyle").truthy() ? ref(col, "cellStyle") : make_object({{"type", 3}}), R.S);
    w.BL(tr(col, "style") ? R.style_id(ref(col, "style")) : 0);
    w.BD(numk(col, "width", 1));
  }
  w.BL(static_cast<double>(nr));
  Array fields;
  const auto src = [&](const Value& h) { return R.S.src(h); };
  for (std::size_t i = 0; i < nr; ++i) {
    const Value& row = arr(T, "rows")[i];
    w.BL(static_cast<double>(nc));
    for (std::size_t j = 0; j < nc; ++j) {
      const Value c = cell_at(T, i, j);
      w.BL(ivk(c, "state"));
      w.TV(nullish(ref(c, "tooltip")) ? Value("") : ref(c, "tooltip"));
      w.BL(0);
      w.BL(0);
      w.BL(0);
      const bool text = is_text(c);
      const double field_h = text && tr(c, "field") ? TableRefs::field(c) : 0;
      const Value& al = ref(c, "align");
      Value cf = make_object({{"overrides", 0},
                              {"propertyFlags", 0},
                              {"dataType", 512},
                              {"unitType", 0},
                              {"format", ""},
                              {"rotation", numk(c, "rotation")},
                              {"blockScale", numk(c, "blockScale", 1)},
                              {"align", al.truthy() ? al : Value(1)},
                              {"color", c_index(0)},
                              {"textHeight", 0.18}});
      if (const Value& cfo = ref(c, "contentFormat"); cfo.is_object()) {
        for (const base::Member& m : cfo.as_object()) {
          cf.set(m.key, m.value);
        }
      }
      cf.set("rotation", numk(c, "rotation"));
      if (is_str(ref(c, "type"), "block")) {
        cf.set("blockScale", numk(c, "blockScale", 1));
      }
      if (num(ref(cf, "rotation")) != 0) {
        cf.set("overrides", static_cast<double>(base::js_to_int32(ivk(cf, "overrides")) | 4));
      }
      if (!text) {
        w.BL(1);
        w.BL(4);
        w.H(kHardPtr, R.block(c));
        std::vector<std::pair<const Value*, double>> at;
        for (const Value& a : arr(c, "attrs")) {
          if (const double h = src(ref(a, "attdef")); h != 0) {
            at.emplace_back(&a, h);
          }
        }
        w.BL(static_cast<double>(at.size()));
        for (const auto& [a, h] : at) {
          w.H(kSoftPtr, h);
          w.TV(Value(str_of(ref(*a, "value"))));
          w.BL(ivk(*a, "index"));
        }
        w.BS(1);
        write_content_format(w, cf, R.S);
      } else if (field_h != 0) {
        fields.emplace_back(field_h);
        w.BL(1);
        w.BL(2);
        w.H(kHardPtr, field_h);
        w.BL(0);
        w.BS(1);
        write_content_format(w, cf, R.S);
      } else {
        const Value v = cell_value(c);
        const bool has_cf = tr(c, "contentFormat");
        if ((base::js_to_int32(base::js_to_number(ref(v, "flags"))) & 1) != 0 && !has_cf) {
          w.BL(0);
        } else {
          if (!has_cf) {
            cf.set("overrides", 1);
            cf.set("dataType", ref(v, "dataType"));
          }
          w.BL(1);
          w.BL(1);
          write_value(w, v, src);
          w.BL(0);
          w.BS(1);
          write_content_format(w, cf, R.S);
        }
      }
      write_cell_style(w, cell_style_for(c, E.E(static_cast<long>(i), static_cast<long>(j)).vprops), R.S);
      w.BL(tr(c, "style") && !(ref(c, "style") == ref(row, "style")) ? R.style_id(ref(c, "style")) : 0);
      w.BL(0);
    }
    w.BL(0);
    w.BL(0);
    write_cell_style(w, ref(row, "cellStyle").truthy() ? ref(row, "cellStyle") : make_object({{"type", 2}}), R.S);
    w.BL(R.style_id(ref(row, "style")));
    w.BD(numk(row, "height", 0.25));
  }
  w.BL(static_cast<double>(fields.size()));
  for (const Value& h : fields) {
    w.H(kHardOwn, h);
  }
  write_cell_style(w, ref(T, "tableCellStyle").truthy() ? ref(T, "tableCellStyle") : make_object({{"type", 4}}), R.S);
  const Array& merged = arr(T, "merged");
  w.BL(static_cast<double>(merged.size()));
  for (const Value& m : merged) {
    w.BL(num(ref(m, "top")));
    w.BL(num(ref(m, "left")));
    w.BL(num(ref(m, "bottom")));
    w.BL(num(ref(m, "right")));
  }
  w.H(kHardPtr, ref(r, "styleH"));
  w.BS(nnum(ref(T, "tableBits"), 38));
  w.P3(orv(ref(r, "horizDir"), make_object({{"x", 1}, {"y", 0}, {"z", 0}})));
  const Value& br = ref(T, "breaks");
  w.BL(br.truthy() ? 1 : 0);
  if (br.truthy()) {
    w.BL(ivk(br, "options"));
    w.BL(nnum(ref(br, "flow"), 1));
    w.BD(numk(br, "spacing"));
    w.BL(ivk(br, "unknown1"));
    w.BL(ivk(br, "unknown2"));
    const Array& hs = arr(br, "heights");
    w.BL(static_cast<double>(hs.size()));
    for (const Value& x : hs) {
      w.P3(ref(x, "position"));
      w.BD(numk(x, "height"));
      w.BL(ivk(x, "flags"));
    }
  }
  const Array& rr = arr(T, "rowRanges");
  if (!rr.empty()) {
    w.BL(static_cast<double>(rr.size()));
    for (const Value& x : rr) {
      w.P3(ref(x, "position"));
      w.BL(ivk(x, "start"));
      w.BL(ivk(x, "end"));
    }
  } else {
    w.BL(1);
    w.BD3(0, 0, 0);
    w.BL(0);
    w.BL(static_cast<double>(nr) - 1);
  }
}

struct TableState {
  StyleRefs S;
  std::shared_ptr<TableRefs> refs;
};

}  // namespace

// ---------------------------------------------------------------- tableWriter

struct TableWriter::Impl {
  Writer& W;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): lives inside its Writer
  explicit Impl(Writer& w) : W(w) {}
  std::unordered_map<std::string, const Value*> by_h;
  std::set<std::string> skip_handles;
  struct StyleRec {
    RecPtr rec;
    Value style;  // the source TABLESTYLE or the default one
    Array tables;
  };
  std::vector<std::string> style_keys;  // insertion order
  std::unordered_map<std::string, std::shared_ptr<StyleRec>> styles;
  std::vector<RecPtr> obj_recs;
  RecPtr dict;
  double n_tables = 0, n_field_cells = 0;

  [[nodiscard]] const Value* byh(const Value& hex) const {
    const Key k = hex_key(hex);
    const auto it = by_h.find(k ? *k : std::string("\x01null"));
    return it == by_h.end() ? nullptr : it->second;
  }

  std::shared_ptr<StyleRec> add_style(const Value* o) {
    std::string k;
    if (o != nullptr) {
      const Key hk = hex_key(ref(*o, "handle"));
      k = hk ? *hk : std::string("\x01null");
    } else {
      k = "*default";
    }
    if (const auto it = styles.find(k); it != styles.end()) {
      return it->second;
    }
    auto sr = std::make_shared<StyleRec>();
    sr->style = o != nullptr ? *o : default_table_style();
    const double h = W.next();
    sr->rec = std::make_shared<Rec>();
    sr->rec->v = make_object({{"t", "TABLESTYLE"}, {"h", h}, {"owner", 0}});
    if (o != nullptr) {
      W.map_src(ref(*o, "handle"), h);
    }
    std::string name;
    if (ref(sr->style, "name").truthy()) {
      name = str_of(ref(sr->style, "name"));
    } else if (o != nullptr) {
      name = "Style" + hex_digits(static_cast<std::uint64_t>(h), false);
    } else {
      name = "Standard";
    }
    std::set<std::string> used;
    for (const Value& x : arr(dict->v, "items")) {
      used.insert(base::js_to_upper(x.as_array()[0].as_string()));
    }
    std::string n = name;
    int i = 2;
    while (used.contains(base::js_to_upper(n))) {
      n = name + "_" + std::to_string(i++);
    }
    dict->v.find("items")->push(Array{n, h});
    styles[k] = sr;
    style_keys.push_back(k);
    // the encoder (ENC.TABLESTYLE): reactors and xdic settle first
    Writer* Wp = &W;
    std::weak_ptr<StyleRec> weak = sr;
    sr->rec->encode = [Wp, weak](Rec& rec, Ctx& ctx) -> std::optional<ByteBuf> {
      const auto sp = weak.lock();
      if (!sp) {
        return std::nullopt;
      }
      StyleRefs R{Wp};
      Array reactors;
      if (const double od = num(ref(rec.v, "ownerDict")); od != 0) {
        reactors.emplace_back(od);
      }
      for (const Value& t : sp->tables) {
        if (const double th = Wp->src(t); th != 0) {
          reactors.emplace_back(th);
        }
      }
      rec.v.set("reactors", std::move(reactors));
      if (!tr(rec.v, "xdic") && tr(sp->style, "extDict")) {
        const double x = Wp->src(ref(sp->style, "extDict"));
        rec.v.set("xdic", x != 0 ? Value(x) : Value());
      }
      ObjWriter w(ctx.version);
      begin(w, ctx.class_num("TABLESTYLE"), rec.v);
      encode_table_style(w, rec.v, sp->style, R);
      return w::finish(w);
    };
    obj_recs.push_back(sr->rec);
    return sr;
  }

  void ensure_dict() {
    if (dict) {
      return;
    }
    const Value* src_style = nullptr;
    for (const Value& o : arr(W.model, "objects")) {
      if (is_str(ref(o, "type"), "TABLESTYLE")) {
        src_style = &o;
        break;
      }
    }
    const Value* src_dict = src_style != nullptr ? byh(ref(*src_style, "owner")) : nullptr;
    dict = std::make_shared<Rec>();
    dict->v =
        make_object({{"t", "DICTIONARY"}, {"h", W.next()}, {"owner", 0}, {"items", Array{}}, {"hardOwner", false}});
    if (src_dict != nullptr) {
      W.map_src(ref(*src_dict, "handle"), num(ref(dict->v, "h")));
    }
    for (const Value& o : arr(W.model, "objects")) {
      if (is_str(ref(o, "type"), "TABLESTYLE")) {
        (void)add_style(&o);
      }
    }
  }

  /// fieldRecs: a FIELD and its child fields (the depth is capped: a model's fields nest a few levels)
  double field_recs(const Value& f, double table_h, int depth = 0) {  // NOLINT(misc-no-recursion)
    if (depth > 64) {
      return 0;
    }
    const Key k = hex_key(ref(f, "handle"));
    if (k && !k->empty() && W.src_map.contains(*k)) {
      return W.src_map.at(*k);
    }
    auto rec = std::make_shared<Rec>();
    const double h = W.next();
    rec->v = make_object({{"t", "FIELD"}, {"h", h}, {"owner", 0}});
    if (k && !k->empty()) {
      W.map_src_key(*k, h);
    }
    Array kids;
    for (const Value& x : arr(f, "children")) {
      if (const double kh = field_recs(x, h, depth + 1); kh != 0) {
        kids.emplace_back(kh);
      }
    }
    for (const RecPtr& kid : obj_recs) {
      if (is_str(ref(kid->v, "t"), "FIELD") && std::ranges::find(kids, ref(kid->v, "h")) != kids.end()) {
        kid->v.set("parent", h);
      }
    }
    const Value* src_obj = k ? byh(Value(*k)) : nullptr;
    const Value src_owner = src_obj != nullptr ? ref(*src_obj, "owner") : Value();
    const bool has_src = src_obj != nullptr;
    auto field = std::make_shared<Value>(f);
    Writer const* Wp = &W;
    rec->encode = [Wp, field, kids, has_src, src_owner, table_h](Rec& r, Ctx& ctx) -> std::optional<ByteBuf> {
      if (const double p = num(ref(r.v, "parent")); p != 0) {
        r.v.set("owner", p);
        r.v.set("reactors", Array{p});
      } else {
        const double o = has_src ? Wp->src(src_owner) : 0;
        r.v.set("owner", o != 0 ? o : table_h);
        r.v.set("reactors", o != 0 ? Value(Array{o}) : Value(Array{}));
      }
      ObjWriter w(ctx.version);
      begin(w, ctx.class_num("FIELD"), r.v);
      object_common(w, r.v);
      Array objs;
      for (const Value& x : arr(*field, "objects")) {
        if (const double n = Wp->src(x); n != 0) {
          objs.emplace_back(n);
        }
      }
      write_field(w, *field, kids, objs, [Wp](const Value& hex) { return Wp->src(hex); });
      return w::finish(w);
    };
    obj_recs.push_back(rec);
    return h;
  }
};

TableWriter::TableWriter(Writer& w) : impl(std::make_unique<Impl>(w)) {
  Impl& I = *impl;
  const Array& objs = arr(w.model, "objects");
  for (const Value& o : objs) {
    const Key k = hex_key(ref(o, "handle"));
    I.by_h[k ? *k : std::string("\x01null")] = &o;
  }
  std::vector<Value> xdicts;
  for (const Value& o : objs) {
    if (is_str(ref(o, "type"), "TABLESTYLE") && tr(o, "extDict")) {
      xdicts.push_back(ref(o, "extDict"));
    }
  }
  for (const Value& b : arr(w.model, "blocks")) {
    for (const Value& e : arr(b, "entities")) {
      if (upper(ref(e, "type")) == "ACAD_TABLE" && tr(e, "extDict")) {
        xdicts.push_back(ref(e, "extDict"));
      }
    }
  }
  static const std::set<std::string> rt_entries = {"ACAD_XREC_ROUNDTRIP",
                                                   "ACAD_ROUNDTRIP_2008_TABLESTYLE_CELLSTYLEMAP"};
  static const std::set<std::string> round_trip = {"TABLECONTENT", "TABLEGEOMETRY", "DATATABLE", "CELLSTYLEMAP"};
  for (const Value& xh : xdicts) {
    const Value* d = I.byh(xh);
    if (d == nullptr || !is_str(ref(*d, "type"), "DICTIONARY") || !ref(*d, "entries").truthy() ||
        !ref(*d, "entries").is_object()) {
      continue;
    }
    const auto& en = ref(*d, "entries").as_object();
    for (const base::Member* m : base::js_members(en)) {
      if (!rt_entries.contains(m->key)) {
        continue;
      }
      const Key hk = hex_key(m->value);
      I.skip_handles.insert(hk ? *hk : std::string("\x01null"));
      const Value* x = I.byh(m->value);
      if (x != nullptr && is_str(ref(*x, "type"), "XRECORD")) {
        for (const Value& it : arr(*x, "data")) {
          const Array& p = it.as_array();
          if ((is(p[0], 360) || is(p[0], 361)) && p.size() > 1) {
            const Value* t = I.byh(p[1]);
            if (t != nullptr && round_trip.contains(str_of(ref(*t, "type")))) {
              const Key tk = hex_key(p[1]);
              I.skip_handles.insert(tk ? *tk : std::string("\x01null"));
            }
          }
        }
      }
    }
    if (!en.empty() && std::ranges::all_of(en, [](const base::Member& m) { return rt_entries.contains(m.key); })) {
      const Key k = hex_key(xh);
      I.skip_handles.insert(k ? *k : std::string("\x01null"));
    }
  }
}

TableWriter::~TableWriter() = default;

bool TableWriter::skip(const Value& o) const {
  return impl->skip_handles.contains(upper(ref(o, "handle")));
}

std::variant<std::vector<RecPtr>, std::string> TableWriter::convert(const Value& e, double owner) const {
  Impl& I = *impl;
  Writer& W = I.W;
  BlockW* B = nullptr;
  if (const auto it = W.block_by_name.find(upper(nullish(ref(e, "block")) ? Value("") : ref(e, "block")));
      it != W.block_by_name.end()) {
    B = it->second;
  } else {
    const Key k = hex_key(nullish(ref(e, "blockRecord")) ? ref(e, "block") : ref(e, "blockRecord"));
    for (BlockW& x : W.blocks) {
      if (hex_key(ref(x.s(), "record")) == k) {
        B = &x;
        break;
      }
    }
  }
  if (B == nullptr || B->kind != "block") {
    return std::string("ACAD_TABLE without its block");
  }
  const Value& t = ref(e, "table");
  Value head = W.common(e, "ACAD_TABLE", owner);
  head.set("insert", base::pt3(numk(ref(e, "insert"), "x"), numk(ref(e, "insert"), "y"), numk(ref(e, "insert"), "z")));
  head.set("scale", ref(e, "scale"));
  head.set("rotation", numk(e, "rotation"));
  head.set("extrusion", ref(e, "extrusion"));
  head.set("horizDir", ref(e, "horizDir"));
  head.set("blockRef", static_cast<double>(W.block_index(B)));
  B->inserts.push_back(num(ref(head, "h")));
  if (!t.truthy() || !ref(t, "rows").is_array() || !ref(t, "columns").is_array() || arr(t, "rows").empty() ||
      arr(t, "columns").empty()) {
    head.set("t", "INSERT");
    head.set("attribs", Array{});
    head.set("cols", 1);
    head.set("rows", 1);
    head.set("colSpacing", 0);
    head.set("rowSpacing", 0);
    auto r = std::make_shared<Rec>();
    r->v = std::move(head);
    return std::vector<RecPtr>{r};
  }
  I.n_tables += 1;
  I.ensure_dict();
  std::shared_ptr<Impl::StyleRec> sv;
  if (tr(t, "style")) {
    if (const Value* so = I.byh(ref(t, "style")); so != nullptr && is_str(ref(*so, "type"), "TABLESTYLE")) {
      sv = I.add_style(so);
    }
  }
  if (!sv) {
    const std::string want = base::js_to_upper(ref(t, "styleName").truthy() ? str_of(ref(t, "styleName")) : "");
    for (const std::string& k : I.style_keys) {
      if (upper(ref(I.styles[k]->style, "name")) == want) {
        sv = I.styles[k];
        break;
      }
    }
  }
  if (!sv) {
    for (const std::string& k : I.style_keys) {
      if (base::js_to_lower(str_of(ref(I.styles[k]->style, "name"))) == "standard" &&
          !nullish(ref(I.styles[k]->style, "name"))) {
        sv = I.styles[k];
        break;
      }
    }
  }
  if (!sv) {
    sv = I.add_style(nullptr);
  }
  sv->tables.push_back(ref(e, "handle"));
  auto refs = std::make_shared<TableRefs>();
  refs->S = StyleRefs{&W};
  refs->style_ids = {{"_TITLE", 1}, {"_HEADER", 2}, {"_DATA", 3}, {"TABLE", 4}};
  for (const Value& c : arr(sv->style, "cellStyles")) {
    if (tr(c, "name")) {
      refs->style_ids[upper(ref(c, "name"))] = num(ref(c, "id"), std::nan(""));
    }
  }
  // cells: fields → FIELD records (their handles kept in the cell), field-less copies keep their text
  Array cells;
  for (const Value& row : arr(t, "cells")) {
    Array r;
    if (row.is_array()) {
      for (const Value& x : row.as_array()) {
        r.push_back(x);
      }
    }
    cells.emplace_back(std::move(r));
  }
  table_canonical_borders(cells);
  for (Value& row : cells) {
    for (Value& x : row.as_array()) {
      if (tr(x, "field") && !is_str(ref(x, "type"), "block")) {
        x.set("__fieldH", I.field_recs(ref(x, "field"), num(ref(head, "h"))));
        I.n_field_cells += 1;
      }
    }
  }
  Value table = t;
  table.set("cells", std::move(cells));
  head.set("table", std::move(table));
  auto rec = std::make_shared<Rec>();
  rec->v = std::move(head);
  std::weak_ptr<Impl::StyleRec> weak = sv;
  Writer* Wp = &W;
  rec->encode = [refs, weak, Wp](Rec& r, Ctx& ctx) -> std::optional<ByteBuf> {
    const auto sp = weak.lock();
    r.v.set("styleH", sp ? num(ref(sp->rec->v, "h")) : 0.0);
    const double bi = num(ref(r.v, "blockRef"), -1);
    r.v.set("block", bi >= 0 && bi < static_cast<double>(Wp->blocks.size())
                         ? Wp->blocks[static_cast<std::size_t>(bi)].hdr
                         : 0.0);
    ObjWriter w(ctx.version);
    begin(w, ctx.class_num("ACAD_TABLE"), r.v);
    entity_common(w, r.v);
    w.P3(ref(r.v, "insert"));
    insert_scale(w, ref(r.v, "scale"));
    w.BD(numk(r.v, "rotation"));
    const Value& ex = ref(r.v, "extrusion");
    if (ex.truthy()) {
      w.BD3(numk(ex, "x"), numk(ex, "y"), numk(ex, "z", 1));
    } else {
      w.BD3(0, 0, 1);
    }
    w.B(false);
    w.H(kHardPtr, ref(r.v, "block"));
    if (w.r2010) {
      new_table(w, r.v, *refs);
    } else {
      old_table(w, r.v, *refs);
    }
    return w::finish(w);
  };
  return std::vector<RecPtr>{rec};
}

void TableWriter::finish(std::vector<RecPtr>& recs) const {
  Impl& I = *impl;
  Writer& W = I.W;
  if (!I.dict) {
    return;
  }
  const double nod_h = W.h("nod");
  I.dict->v.set("owner", nod_h);
  I.dict->v.set("reactors", Array{nod_h});
  const double dh = num(ref(I.dict->v, "h"));
  for (const std::string& k : I.style_keys) {
    I.styles[k]->rec->v.set("owner", dh);
    I.styles[k]->rec->v.set("ownerDict", dh);
  }
  recs.push_back(I.dict);
  for (const RecPtr& r : I.obj_recs) {
    recs.push_back(r);
  }
  for (const RecPtr& r : recs) {
    if (num(ref(r->v, "h")) == nod_h && is_str(ref(r->v, "t"), "DICTIONARY")) {
      Array& items = r->v.find("items")->as_array();
      if (std::ranges::none_of(items, [](const Value& x) { return is_str(x.as_array()[0], "ACAD_TABLESTYLE"); })) {
        items.emplace_back(Array{"ACAD_TABLESTYLE", dh});
        std::ranges::stable_sort(items, [](const Value& a, const Value& b) {
          return js_less(a.as_array()[0].as_string(), b.as_array()[0].as_string());
        });
      }
      break;
    }
  }
  W.report.set("tables", make_object({{"tables", I.n_tables},
                                      {"styles", static_cast<double>(I.styles.size())},
                                      {"fieldCells", I.n_field_cells}}));
}

}  // namespace cc::dwg::w
