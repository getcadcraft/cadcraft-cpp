// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg table-read.js — ACAD_TABLE and its helper objects, decoded, and their model fields.
//
// Written from the ODA "Open Design Specification for .dwg files" v5.4.1: 20.4.96 TABLE (until R21, and R24+),
// 20.4.97 TABLECONTENT, 20.4.98 cell content geometry, 20.4.99 value, 20.4.100 custom data, 20.4.101 TABLESTYLE
// (until R21, R24) with 20.4.101.3 content format and 20.4.101.4 cell style, 20.4.102 CELLSTYLEMAP,
// 20.4.103 TABLEGEOMETRY, 20.4.76 FIELD, 20.4.77 FIELDLIST. Where the spec is wrong or silent the layout was found by
// experiment on the corpus (in the JS); those places are marked "Spec gap:".
//
//   table_decoder(type)               the decoders of TABLE_DECODERS (ACAD_TABLE, TABLESTYLE, CELLSTYLEMAP,
//                                     TABLECONTENT, TABLEGEOMETRY, FIELD, FIELDLIST); handle references are numbers
//                                     under keys starting with "_" (resolved by the model builders below).
//   table_entity_fields(rec, f, res)  the model fields of an ACAD_TABLE entity (f.table = { rows, columns, cells, … }).
//   table_object_fields(rec, f, res)  the same for TABLESTYLE / CELLSTYLEMAP / TABLECONTENT / TABLEGEOMETRY / FIELD.
//
// A decoder that fails part way leaves what the JS leaves: containers the JS attaches to the record before filling
// them are attached first here too and filled in place (looked up again after every other write to the record).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "messages.hpp"
#include "parts.hpp"
#include "record.hpp"

namespace cc::dwg {

using base::Array;
using base::make_object;

namespace {

// ---------------------------------------------------------------- small helpers
Value bool_v(bool b) {
  return {b};
}

const Value& kUndef() {
  static const Value u;
  return u;
}
/// o[k] for any value (undefined when o is not an object or has no k).
const Value& at(const Value& o, std::string_view k) {
  return o.is_object() ? o[k] : kUndef();
}
bool has_def(const Value& o, std::string_view k) {
  return !at(o, k).is_undefined();
}
bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
/// A JS number as an integer for bit tests and handles (0 when not a finite number).
std::int64_t ival(const Value& v) {
  const double d = v.num_or(0);
  if (!std::isfinite(d) || std::abs(d) > 9.0e18) {
    return 0;
  }
  return static_cast<std::int64_t>(d);
}
std::uint32_t bits32(const Value& v) {
  return static_cast<std::uint32_t>(ival(v) & 0xFFFFFFFF);
}
const Array& arr(const Value& v) {
  static const Array kEmpty;
  return v.is_array() ? v.as_array() : kEmpty;
}
std::size_t key_count(const Value& o) {
  return o.is_object() ? o.as_object().size() : 0;
}
Value hx(const Value& h) {
  return handle_hex(ival(h));
}
/// res.nameOf(h) ?? res.hx(h)
Value name_or_hx(const Resolver& res, const Value& hv) {
  const std::int64_t h = ival(hv);
  auto n = res.name_of(h);
  if (n && !n->is_null()) {
    return *n;
  }
  return handle_hex(h);
}
Value hex_list(const Array& a) {
  Array o;
  o.reserve(a.size());
  for (const Value& x : a) {
    o.emplace_back(hx(x));
  }
  return o;
}
Value hex_list(const std::vector<std::int64_t>& a) {
  Array o;
  o.reserve(a.size());
  for (const std::int64_t x : a) {
    o.emplace_back(handle_hex(x));
  }
  return o;
}
std::string num_text(double d) {
  std::string s;
  base::append_number(s, d);
  return s;
}
/// JSON.stringify for the comparisons the JS makes (undefined has no text, so it equals nothing).
std::string json_of(const Value& v) {
  return v.is_undefined() ? std::string("\x01undefined") : base::to_json(v);
}

/// A handle list read one by one; what was read stays on the record when a read fails (the JS pushes in place).
void read_hlist(Bits& h, Rec& rec, std::string_view name, std::int64_t n) {
  std::vector<std::int64_t> a;
  try {
    for (std::int64_t i = 0; i < n; ++i) {
      a.push_back(h.H(rec.handle));
    }
  } catch (...) {
    rec.set_hl(name, std::move(a));
    throw;
  }
  rec.set_hl(name, std::move(a));
}

/// The record field `k` (it must exist); looked up afresh, since setting another key may move it.
Value& field(Rec& rec, std::string_view k) {
  return *rec.f.find(k);
}
Value& tbl(Rec& rec) {
  return field(rec, "_table");
}

/**
 * Colours of the table objects. Spec gap: R2000 files (written by the ODA File Converter) store the R2004 form
 * (BS index, BL rgb word, RC name flags) in TABLESTYLE / CELLSTYLEMAP / TABLECONTENT, not the R2000 BS index —
 * found by experiment (the R2000 objects decode to the end only so, and match the converter's DXF).
 */
Value cmc(Bits& r) {
  if (r.version() >= 18) {
    return r.CMC();
  }
  const int v = r.version();
  r.set_version(18);
  try {
    Value c = r.CMC();
    r.set_version(v);
    return c;
  } catch (...) {
    r.set_version(v);
    throw;
  }
}

std::int64_t count(Bits& r, std::int64_t min_bits) {
  return fits(r, r.BL(), min_bits);
}

std::string utf16(const std::vector<std::uint8_t>& b) {
  std::vector<std::uint16_t> u;
  for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
    const auto c = static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8U));
    if (c == 0) {
      break;
    }
    u.push_back(c);
  }
  std::string s;
  base::append_utf16(s, u);
  return s;
}

std::vector<std::uint8_t> read_bytes(Bits& r, std::int64_t n) {
  return r.bytes(static_cast<std::size_t>(n));
}

// ---------------------------------------------------------------- 20.4.99 value
/**
 * A value (20.4.99). R2007+: flags BL, data type BL, the data unless flags & 1 (kGeneral), unit type BL, format
 * string TV, value string TV. Object ids (type 64) come from the handle stream.
 */
Value read_value(Bits& r, Bits& h, const Rec& rec, const Ctx& ctx) {
  Value v = Value::object();
  std::int32_t flags = 0;
  if (ctx.ver >= 21) {
    flags = r.BL();
    v.set("flags", flags);
  }
  const std::int32_t type = r.BL();
  v.set("dataType", type);
  if (ctx.ver < 21 || (flags & 1) == 0) {
    switch (type) {
      case 0:  // unknown
      case 1:  // long (the JS `| 0` is a no-op on a BL)
        v.set("value", r.BL());
        break;
      case 2: v.set("value", r.BD()); break;
      case 4:
        // Spec gap: from R2007 a string value is not a TV but BL byte count + UTF-16LE text with its NUL, in the
        // data stream (found by experiment: "test" = 10 bytes in example_2000 saved as R2007).
        if (ctx.ver >= 21) {
          const std::int64_t n = count(r, 8);
          v.set("value", utf16(read_bytes(r, n)));
        } else {
          v.set("value", r.TV());
        }
        break;
      case 8: v.set("bytes", hex_bytes(read_bytes(r, count(r, 8)))); break;  // date (Int64)
      case 16: {
        const std::int64_t n = count(r, 8);
        if (n >= 16) {
          v.set("value", r.RD2());
        } else {
          (void)read_bytes(r, n);
          v.set("value", nullptr);
        }
        break;
      }
      case 32: {
        const std::int64_t n = count(r, 8);
        if (n >= 24) {
          v.set("value", r.RD3());
        } else {
          (void)read_bytes(r, n);
          v.set("value", nullptr);
        }
        break;
      }
      case 64: v.set("_id", h.H(rec.handle)); break;
      case 128:
      case 256:
      case 512: v.set("bytes", hex_bytes(read_bytes(r, count(r, 8)))); break;
      default: throw DecodeError(msg("ccdwg.err.value_type", {{"type", static_cast<double>(type)}}));
    }
  }
  if (ctx.ver >= 21) {
    v.set("unitType", r.BL());
    v.set("format", r.TV());
    v.set("text", r.TV());
  }
  return v;
}

// ---------------------------------------------------------------- 20.4.100 custom data
Value read_custom_data(Bits& r, Bits& h, const Rec& rec, const Ctx& ctx) {
  const std::int64_t n = count(r, 2);
  Array out;
  for (std::int64_t i = 0; i < n; ++i) {
    out.push_back(make_object({{"name", r.TV()}, {"value", read_value(r, h, rec, ctx)}}));
  }
  return out;
}

// ---------------------------------------------------------------- 20.4.101.3 content format, 20.4.101.4 cell style
Value read_content_format(Bits& r, Bits& h, const Rec& rec) {
  return make_object({{"overrides", r.BL()},
                      {"propertyFlags", r.BL()},
                      {"dataType", r.BL()},
                      {"unitType", r.BL()},
                      {"format", r.TV()},
                      {"rotation", r.BD()},
                      {"blockScale", r.BD()},
                      {"alignment", r.BL()},
                      {"color", cmc(r)},
                      {"_textStyle", h.H(rec.handle)},
                      {"textHeight", r.BD()}});
}
Value read_cell_style(Bits& r, Bits& h, const Rec& rec) {
  Value s = make_object({{"styleType", r.BL()}});
  const std::int32_t has_data = r.BS();
  s.set("hasData", has_data);
  if (has_data == 0) {
    return s;
  }
  s.set("overrides", r.BL());
  s.set("mergeFlags", r.BL());
  s.set("background", cmc(r));
  s.set("contentLayout", r.BL());
  s.set("content", read_content_format(r, h, rec));
  const std::int32_t margin_flags = r.BS();
  s.set("marginFlags", margin_flags);
  if ((margin_flags & 1) != 0) {
    s.set("margins", make_object({{"vertical", r.BD()},
                                  {"horizontal", r.BD()},
                                  {"bottom", r.BD()},
                                  {"right", r.BD()},
                                  {"hSpacing", r.BD()},
                                  {"vSpacing", r.BD()}}));
  }
  const std::int64_t nb = count(r, 2);
  Array borders;
  for (std::int64_t i = 0; i < nb; ++i) {
    const std::int32_t edge = r.BL();
    Value b = make_object({{"edge", edge}});
    if (edge != 0) {
      b.set("overrides", r.BL());
      b.set("borderType", r.BL());
      b.set("color", cmc(r));
      b.set("lineweight", r.BL());
      b.set("_linetype", h.H(rec.handle));
      b.set("invisible", r.BL());
      b.set("doubleSpacing", r.BD());
    }
    borders.push_back(std::move(b));
  }
  s.set("borders", std::move(borders));
  return s;
}
/// A cell style with its id, class and name (the cell style map and the R24 table style).
Value read_named_cell_style(Bits& r, Bits& h, const Rec& rec) {
  Value s = read_cell_style(r, h, rec);
  s.set("id", r.BL());
  s.set("styleClass", r.BL());
  s.set("name", r.TV());
  return s;
}

Value read_content_geometry(Bits& r) {
  return make_object({{"distTopLeft", r.BD3()},
                      {"distCenter", r.BD3()},
                      {"contentWidth", r.BD()},
                      {"contentHeight", r.BD()},
                      {"width", r.BD()},
                      {"height", r.BD()},
                      {"flags", r.BL()}});
}

// ---------------------------------------------------------------- 20.4.97 table content (without the common object
// data)
Value read_cell(Bits& r, Bits& h, const Rec& rec, const Ctx& ctx) {
  Value c = make_object(
      {{"state", r.BL()}, {"tooltip", r.TV()}, {"customData", r.BL()}, {"custom", read_custom_data(r, h, rec, ctx)}});
  const std::int32_t has_link = r.BL();
  c.set("hasLink", has_link);
  if (has_link != 0) {
    c.set("_link", h.H(rec.handle));
    c.set("linkRows", r.BL());
    c.set("linkCols", r.BL());
    c.set("linkUnknown", r.BL());
  }
  const std::int64_t ncont = count(r, 2);
  Array contents;
  for (std::int64_t j = 0; j < ncont; ++j) {
    const std::int32_t type = r.BL();
    Value ct = make_object({{"type", type}});
    if (type == 1) {
      ct.set("value", read_value(r, h, rec, ctx));
    } else if (type == 2) {
      ct.set("_field", h.H(rec.handle));
    } else if (type == 4) {
      ct.set("_block", h.H(rec.handle));
    }
    const std::int64_t na = count(r, 2);
    Array attrs;
    for (std::int64_t a = 0; a < na; ++a) {
      attrs.push_back(make_object({{"_attdef", h.H(rec.handle)}, {"value", r.TV()}, {"index", r.BL()}}));
    }
    ct.set("attrs", std::move(attrs));
    const std::int32_t has_format = r.BS();
    ct.set("hasFormat", has_format);
    if (has_format != 0) {
      ct.set("format", read_content_format(r, h, rec));
    }
    contents.push_back(std::move(ct));
  }
  c.set("contents", std::move(contents));
  c.set("style", read_cell_style(r, h, rec));
  c.set("styleId", r.BL());
  const std::int32_t geom_flag = r.BL();
  c.set("geomFlag", geom_flag);
  if (geom_flag != 0) {
    c.set("geomUnknown", r.BL());
    c.set("geomA", r.BD());
    c.set("geomB", r.BD());
    const std::int32_t geom_data_flags = r.BL();
    c.set("geomDataFlags", geom_data_flags);
    c.set("_geomUnknown", h.H(rec.handle));
    if (geom_data_flags != 0) {
      c.set("geometry", read_content_geometry(r));
    }
  }
  return c;
}

Value read_table_content(Bits& r, Bits& h, const Rec& rec, const Ctx& ctx) {
  Value t = make_object({{"name", r.TV()}, {"description", r.TV()}});
  const std::int64_t nc = count(r, 8);
  Array columns;
  for (std::int64_t i = 0; i < nc; ++i) {
    Value c = make_object({{"name", r.TV()}, {"customData", r.BL()}, {"custom", read_custom_data(r, h, rec, ctx)}});
    c.set("style", read_cell_style(r, h, rec));
    c.set("styleId", r.BL());
    c.set("width", r.BD());
    columns.push_back(std::move(c));
  }
  t.set("columns", std::move(columns));
  const std::int64_t nr = count(r, 8);
  Array rows;
  for (std::int64_t i = 0; i < nr; ++i) {
    const std::int64_t n = count(r, 8);
    Array cells;
    for (std::int64_t k = 0; k < n; ++k) {
      cells.push_back(read_cell(r, h, rec, ctx));
    }
    Value row = make_object({{"cells", std::move(cells)}});
    row.set("customData", r.BL());
    row.set("custom", read_custom_data(r, h, rec, ctx));
    row.set("style", read_cell_style(r, h, rec));
    row.set("styleId", r.BL());
    row.set("height", r.BD());
    rows.push_back(std::move(row));
  }
  t.set("rows", std::move(rows));
  const std::int64_t nf = count(r, 0);
  Array fields;
  for (std::int64_t i = 0; i < nf; ++i) {
    fields.emplace_back(h.H(rec.handle));
  }
  t.set("_fields", std::move(fields));
  t.set("tableStyle", read_cell_style(r, h, rec));  // AcDbFormattedTableData: the table's cell style overrides
  const std::int64_t nm = count(r, 8);
  Array merged;
  for (std::int64_t i = 0; i < nm; ++i) {
    merged.push_back(make_object({{"top", r.BL()}, {"left", r.BL()}, {"bottom", r.BL()}, {"right", r.BL()}}));
  }
  t.set("merged", std::move(merged));
  t.set("_style", h.H(rec.handle));
  return t;
}

// ---------------------------------------------------------------- 20.4.96 TABLE
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

void insert_part(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("insert", r.BD3());
  rec.set("scale", read_scale(r, ctx));
  rec.set("rotation", r.BD());
  rec.set("extrusion", r.BD3());
  const std::uint32_t has_attribs = r.B();
  rec.set("_hasAttribs", has_attribs);
  std::int32_t owned = 0;
  rec.set("_owned", owned);
  if (ctx.ver >= 18 && has_attribs != 0) {
    owned = r.BL();
    rec.set("_owned", owned);
  }
  rec.set_h("block", h.H(rec.handle));
  if (has_attribs != 0) {
    if (ctx.ver < 18) {
      rec.set_h("firstAttrib", h.H(rec.handle));
      rec.set_h("lastAttrib", h.H(rec.handle));
    } else {
      read_hlist(h, rec, "attribs", owned);
    }
    rec.set_h("seqend", h.H(rec.handle));
  }
}

constexpr std::array<std::string_view, 4> kSides = {"top", "right", "bottom", "left"};
constexpr std::array<std::string_view, 3> kRowNames = {"title", "header", "data"};

/// An R13–R21 cell (20.4.96).
Value read_old_cell(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  const std::int32_t type = r.BS();
  Value c = make_object({{"type", type},
                         {"edgeFlags", r.RC()},
                         {"merged", r.B()},
                         {"autoFit", r.B()},
                         {"mergedWidth", r.BL()},
                         {"mergedHeight", r.BL()},
                         {"rotation", r.BD()}});
  const std::int64_t value_h = h.H(rec.handle);
  c.set("_value", value_h);
  if (type == 1) {
    // Spec gap: from R2007 a text cell stores no text of its own; the cell value (below) holds it (found by
    // experiment: the string stream of example_2000 saved as R2007 holds 2 strings per cell, not 3).
    if (value_h == 0 && ctx.ver < 21) {
      c.set("text", r.TV());
    }
  } else if (type == 2) {
    c.set("blockScale", r.BD());
    const std::uint32_t has_attrs = r.B();
    c.set("hasAttrs", has_attrs);
    if (has_attrs != 0) {
      const std::int32_t n = r.BS();
      Array attrs;
      for (std::int32_t k = 0; k < n; ++k) {
        attrs.push_back(make_object({{"_attdef", h.H(rec.handle)}, {"index", r.BS()}, {"value", r.TV()}}));
      }
      c.set("attrs", std::move(attrs));
    }
  }
  const std::uint32_t has_overrides = r.B();
  c.set("hasOverrides", has_overrides);
  if (has_overrides != 0) {
    const auto f = static_cast<std::uint32_t>(r.BL());
    c.set("overrideFlags", static_cast<std::int32_t>(f));
    const std::uint32_t virtual_edges = r.RC();
    c.set("virtualEdges", virtual_edges);
    if ((f & 0x01U) != 0) {
      c.set("alignment", r.BS());  // Spec gap: the spec says RS; it is a BS (found by experiment: blocks_and_tables)
    }
    if ((f & 0x02U) != 0) {
      c.set("fillNone", r.B());
    }
    if ((f & 0x04U) != 0) {
      c.set("background", cmc(r));
    }
    if ((f & 0x08U) != 0) {
      c.set("contentColor", cmc(r));
    }
    if ((f & 0x10U) != 0) {
      c.set("_textStyle", h.H(rec.handle));
    }
    if ((f & 0x20U) != 0) {
      c.set("textHeight", r.BD());
    }
    Value borders = Value::object();
    // Spec gap: on an edge marked virtual (178) the border flags stay set but no values are stored: they are the
    // adjacent cell's (its opposite edge) — found by having the ODA File Converter write a border override.
    for (unsigned k = 0; k < 4; ++k) {
      if ((virtual_edges & (1U << k)) != 0) {
        continue;
      }
      Value b = Value::object();
      if ((f & (0x40U << k)) != 0) {
        b.set("color", cmc(r));
      }
      if ((f & (0x400U << k)) != 0) {
        b.set("lineweight", r.BS());
      }
      // Spec gap: 1 = invisible (as the table-wide flags; the ODA File Converter turns 1 into R2010+ "invisible")
      if ((f & (0x4000U << k)) != 0) {
        b.set("visible", bool_v(r.BS() == 0));
      }
      if (key_count(b) != 0) {
        borders.set(kSides.at(k), std::move(b));
      }
    }
    c.set("borders", std::move(borders));
  }
  if (ctx.ver >= 21) {
    c.set("unknown", r.BL());
    c.set("value", read_value(r, h, rec, ctx));
  }
  return c;
}

/// A table-wide border list: present bit, flags BL, then one value per set flag bit (keys "0".."17", then flags,
/// in JS object key order).
template <class Read>
Value read_border(Bits& r, Read read) {
  if (r.B() == 0) {
    return {};
  }
  const std::int32_t f = r.BL();
  Value o = Value::object();
  for (unsigned i = 0; i < 18; ++i) {
    if ((static_cast<std::uint32_t>(f) & (1U << i)) != 0) {
      o.set(std::to_string(i), read());
    }
  }
  o.set("flags", f);
  return o;
}

/// R13–R21 table data: the grid, the cells and the table-wide overrides.
void old_table(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("_table", Value::object());
  tbl(rec).set("flags", r.BS());
  rec.set("horizDir", r.BD3());
  const std::int64_t nc = count(r, 2);
  const std::int64_t nr = count(r, 2);
  tbl(rec).set("colWidths", Array{});
  for (std::int64_t i = 0; i < nc; ++i) {
    const double w = r.BD();
    tbl(rec).find("colWidths")->push(w);
  }
  tbl(rec).set("rowHeights", Array{});
  for (std::int64_t i = 0; i < nr; ++i) {
    const double ht = r.BD();
    tbl(rec).find("rowHeights")->push(ht);
  }
  rec.set_h("tableStyle", h.H(rec.handle));
  tbl(rec).set("cells", Array{});
  for (std::int64_t i = 0; i < nr * nc; ++i) {
    Value c = read_old_cell(r, h, rec, ctx);
    tbl(rec).find("cells")->push(std::move(c));
  }
  // table-wide overrides
  const std::uint32_t has_overrides = r.B();
  tbl(rec).set("hasOverrides", has_overrides);
  if (has_overrides != 0) {
    const auto f = static_cast<std::uint32_t>(r.BL());
    tbl(rec).set("overrideFlags", static_cast<std::int32_t>(f));
    tbl(rec).set("overrides", Value::object());
    auto o = [&rec]() -> Value& { return *tbl(rec).find("overrides"); };
    if ((f & 1U) != 0) {
      const std::uint32_t v = r.B();
      o().set("titleSuppressed", v);
    }
    if ((f & 4U) != 0) {
      const std::int32_t v = r.BS();
      o().set("flowDirection", v);
    }
    if ((f & 8U) != 0) {
      const double v = r.BD();
      o().set("hMargin", v);
    }
    if ((f & 0x10U) != 0) {
      const double v = r.BD();
      o().set("vMargin", v);
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x20U << i)) != 0) {
        Value v = cmc(r);
        o().set(std::string(kRowNames.at(i)) + "Color", std::move(v));
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x100U << i)) != 0) {
        const std::uint32_t v = r.B();
        o().set(std::string(kRowNames.at(i)) + "FillNone", v);
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x800U << i)) != 0) {
        Value v = cmc(r);
        o().set(std::string(kRowNames.at(i)) + "Fill", std::move(v));
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x4000U << i)) != 0) {
        const std::int32_t v = r.BS();
        o().set(std::string(kRowNames.at(i)) + "Align", v);
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x20000U << i)) != 0) {
        const std::int64_t v = h.H(rec.handle);
        o().set("_" + std::string(kRowNames.at(i)) + "Style", v);
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      if ((f & (0x100000U << i)) != 0) {
        const double v = r.BD();
        o().set(std::string(kRowNames.at(i)) + "Height", v);
      }
    }
  }
  Value colors = read_border(r, [&r]() { return cmc(r); });
  tbl(rec).set("borderColors", std::move(colors));
  Value weights = read_border(r, [&r]() { return Value(r.BS()); });
  tbl(rec).set("borderLineweights", std::move(weights));
  Value visibility = read_border(r, [&r]() { return Value(r.BS()); });
  tbl(rec).set("borderVisibility", std::move(visibility));
}

/// R24+ table: the INSERT part, a few unknowns, the table content inline, then the break data.
void new_table(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("_table", Value::object());
  tbl(rec).set("unknownRC", r.RC());
  tbl(rec).set("_unknownH", h.H(rec.handle));
  tbl(rec).set("unknownBL", r.BL());
  if (ctx.ver == 24) {
    tbl(rec).set("unknownB", r.B());
  } else {
    tbl(rec).set("unknownBL2", r.BL());
  }
  Value content = read_table_content(r, h, rec, ctx);
  tbl(rec).set("content", std::move(content));
  tbl(rec).set("unknownBS", r.BS());
  rec.set("horizDir", r.BD3());
  const std::int32_t has_break = r.BL();
  tbl(rec).set("hasBreak", has_break);
  if (has_break != 0) {
    Value b = make_object(
        {{"options", r.BL()}, {"flow", r.BL()}, {"spacing", r.BD()}, {"unknown1", r.BL()}, {"unknown2", r.BL()}});
    tbl(rec).set("break", std::move(b));
    const std::int64_t n = count(r, 8);
    tbl(rec).find("break")->set("heights", Array{});
    for (std::int64_t i = 0; i < n; ++i) {
      Value x = make_object({{"position", r.BD3()}, {"height", r.BD()}, {"flags", r.BL()}});
      tbl(rec).find("break")->find("heights")->push(std::move(x));
    }
  }
  const std::int64_t n = count(r, 8);
  tbl(rec).set("rowRanges", Array{});
  for (std::int64_t i = 0; i < n; ++i) {
    Value x = make_object({{"position", r.BD3()}, {"start", r.BL()}, {"end", r.BL()}});
    tbl(rec).find("rowRanges")->push(std::move(x));
  }
}

void set_h_left(Rec& rec, const Bits& h) {
  rec.set("_hLeft", static_cast<double>(h.left()));
}

void dec_acad_table(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  insert_part(r, h, rec, ctx);
  if (ctx.ver >= 24) {
    new_table(r, h, rec, ctx);
  } else {
    old_table(r, h, rec, ctx);
  }
  set_h_left(rec, h);
}

// ---------------------------------------------------------------- 20.4.101 TABLESTYLE
void dec_tablestyle(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  if (ctx.ver >= 24) {
    rec.set("unknownRC", r.RC());
    rec.set("description", r.TV());
    rec.set("unknownBL1", r.BL());
    rec.set("unknownBL2", r.BL());
    rec.set_h("unknown", h.H(rec.handle));
    rec.set("tableCellStyle", read_named_cell_style(r, h, rec));
    // Spec gap: each of the cell styles that follow is preceded by a BL equal to its id (1 title, 2 header,
    // 3 data) — found by experiment (the R2010–R2018 table styles of the corpus decode to the end only so).
    const std::int64_t n = count(r, 8);
    rec.set("cellStyles", Array{});
    for (std::int64_t i = 0; i < n; ++i) {
      const std::int32_t pre = r.BL();
      Value cs = read_named_cell_style(r, h, rec);
      cs.set("preId", pre);
      field(rec, "cellStyles").push(std::move(cs));
    }
  } else {
    rec.set("description", r.TV());
    rec.set("flowDirection", r.BS());
    rec.set("flags", r.BS());
    rec.set("hMargin", r.BD());
    rec.set("vMargin", r.BD());
    rec.set("titleSuppressed", r.B());
    rec.set("headerSuppressed", r.B());
    rec.set("rowStyles", Array{});
    for (int i = 0; i < 3; ++i) {
      Value s = make_object({{"_textStyle", h.H(rec.handle)},
                             {"textHeight", r.BD()},
                             {"alignment", r.BS()},
                             {"textColor", cmc(r)},
                             {"fillColor", cmc(r)},
                             {"fillEnabled", r.B()},
                             {"borders", Array{}}});
      for (int k = 0; k < 6; ++k) {
        Value b = make_object({{"lineweight", r.BS()}, {"visible", r.B()}, {"color", cmc(r)}});
        s.find("borders")->push(std::move(b));
      }
      if (ctx.ver >= 21) {
        s.set("dataType", r.BL());
        s.set("unitType", r.BL());
        s.set("format", r.TV());
      }
      field(rec, "rowStyles").push(std::move(s));
    }
  }
  set_h_left(rec, h);
}

// ---------------------------------------------------------------- 20.4.102 CELLSTYLEMAP
void dec_cellstylemap(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  const std::int64_t n = count(r, 8);
  rec.set("cellStyles", Array{});
  for (std::int64_t i = 0; i < n; ++i) {
    Value cs = read_named_cell_style(r, h, rec);
    field(rec, "cellStyles").push(std::move(cs));
  }
  set_h_left(rec, h);
}

// ---------------------------------------------------------------- 20.4.97 TABLECONTENT (R21 round-trip object)
void dec_tablecontent(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("_content", read_table_content(r, h, rec, ctx));
  set_h_left(rec, h);
}

// ---------------------------------------------------------------- 20.4.103 TABLEGEOMETRY
void dec_tablegeometry(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  rec.set("rowCount", r.BL());
  rec.set("colCount", r.BL());
  const std::int64_t n = count(r, 8);
  rec.set("cells", Array{});
  for (std::int64_t i = 0; i < n; ++i) {
    Value c = make_object({{"flags", r.BL()},
                           {"widthWithGap", r.BD()},
                           {"heightWithGap", r.BD()},
                           {"_unknown", h.H(rec.handle)},
                           {"contents", Array{}}});
    const std::int64_t k = count(r, 8);
    for (std::int64_t j = 0; j < k; ++j) {
      Value x = make_object({{"distTopLeft", r.BD3()},
                             {"distCenter", r.BD3()},
                             {"contentWidth", r.BD()},
                             {"contentHeight", r.BD()},
                             {"width", r.BD()},
                             {"height", r.BD()},
                             {"unknown", r.BL()}});
      c.find("contents")->push(std::move(x));
    }
    field(rec, "cells").push(std::move(c));
  }
  set_h_left(rec, h);
}

// ---------------------------------------------------------------- 20.4.76 FIELD, 20.4.77 FIELDLIST
void dec_field(Bits& r, Bits& h, Rec& rec, const Ctx& ctx) {
  rec.set("evaluatorId", r.TV());
  rec.set("code", r.TV());
  const std::int64_t nc = count(r, 0);
  read_hlist(h, rec, "children", nc);
  const std::int64_t no = count(r, 0);
  read_hlist(h, rec, "objects", no);
  if (ctx.ver < 21) {
    rec.set("format", r.TV());
  }
  rec.set("evalOptions", r.BL());
  rec.set("filingOptions", r.BL());
  rec.set("state", r.BL());
  rec.set("evalStatus", r.BL());
  rec.set("evalError", r.BL());
  rec.set("evalMessage", r.TV());
  rec.set("value", read_value(r, h, rec, ctx));
  rec.set("valueString", r.TV());
  rec.set("valueStringLength", r.BL());
  const std::int64_t nk = count(r, 2);
  rec.set("childValues", Array{});
  for (std::int64_t i = 0; i < nk; ++i) {
    Value x = make_object({{"key", r.TV()}, {"value", read_value(r, h, rec, ctx)}});
    field(rec, "childValues").push(std::move(x));
  }
  set_h_left(rec, h);
}
void dec_fieldlist(Bits& r, Bits& h, Rec& rec, const Ctx& /*ctx*/) {
  const std::int64_t n = count(r, 0);
  rec.set("unknown", r.B());
  read_hlist(h, rec, "fields", n);
}

// ================================================================ model (version-neutral, see model.js ACAD_TABLE)
/// R24 border edge flag → side name (20.4.101.4)
std::string_view edge_side(std::int64_t e) {
  switch (e) {
    case 1: return "top";
    case 2: return "right";
    case 4: return "bottom";
    case 8: return "left";
    case 16: return "insideV";
    case 32: return "insideH";
    default: return {};
  }
}
/// R13–R21 table style borders, in stored order (20.4.101.1)
constexpr std::array<std::string_view, 6> kOldBorderOrder = {"top", "insideH", "bottom", "left", "insideV", "right"};
/// DEFAULT_STYLE_NAMES
Value default_style_name(const Value& id) {
  if (!id.is_number()) {
    return {};
  }
  const double d = id.as_number();
  if (d == 1) {
    return "_TITLE";
  }
  if (d == 2) {
    return "_HEADER";
  }
  if (d == 3) {
    return "_DATA";
  }
  if (d == 4) {
    return "Table";
  }
  return {};
}

/// A value (20.4.99) as model JSON: { dataType, unitType?, format?, text?, value? }
Value value_model(const Value& v) {
  if (!v.truthy()) {
    return {};
  }
  Value o = make_object({{"dataType", at(v, "dataType")}});
  if (has_def(v, "flags")) {
    o.set("flags", at(v, "flags"));
  }
  if (has_def(v, "value")) {
    o.set("value", at(v, "value"));
  }
  if (has_def(v, "bytes")) {
    o.set("bytes", at(v, "bytes"));
  }
  if (has_def(v, "_id")) {
    o.set("id", hx(at(v, "_id")));
  }
  if (at(v, "unitType").truthy()) {
    o.set("unitType", at(v, "unitType"));
  }
  if (at(v, "format").truthy()) {
    o.set("format", at(v, "format"));
  }
  if (at(v, "text").truthy()) {
    o.set("text", at(v, "text"));
  }
  return o;
}
/// The text a value shows.
Value value_text(const Value& v) {
  if (!v.truthy() || (has_def(v, "flags") && (ival(at(v, "flags")) & 1) != 0)) {
    return "";
  }
  if (at(v, "text").truthy()) {
    return at(v, "text");
  }
  const Value& dt = at(v, "dataType");
  const Value& val = at(v, "value");
  if (dt.is_number() && dt.as_number() == 4) {
    return nullish(val) ? Value("") : val;
  }
  if (dt.is_number() && (dt.as_number() == 1 || dt.as_number() == 2)) {
    if (val.is_undefined()) {
      return "";
    }
    if (val.is_number()) {
      return num_text(val.as_number());
    }
    if (val.is_string()) {
      return val;
    }
    return base::to_json(val);
  }
  return "";
}

/// Nodes a field tree may have: a cap on hostile files whose fields name each other many times over (the JS stops
/// only at depth 16; no real file comes near this).
constexpr int kFieldNodeBudget = 100000;

/// A FIELD record and its children as a model tree.
// NOLINTNEXTLINE(misc-no-recursion) — bounded by depth 16 and a node budget
Value field_model(std::int64_t handle, const Resolver& res, int depth, int& budget) {
  const Rec* r = res.get(handle);
  if (r == nullptr || r->type != "FIELD" || depth > 16 || budget <= 0) {
    return {};
  }
  --budget;
  Value f =
      make_object({{"handle", handle_hex(handle)}, {"evaluatorId", r->get("evaluatorId")}, {"code", r->get("code")}});
  if (r->get("format").truthy()) {
    f.set("format", r->get("format"));
  }
  for (const std::string_view k : {"evalOptions", "filingOptions", "state", "evalStatus", "evalError"}) {
    f.set(k, r->get(k));
  }
  if (r->get("evalMessage").truthy()) {
    f.set("evalMessage", r->get("evalMessage"));
  }
  f.set("value", value_model(r->get("value")));
  f.set("valueString", r->get("valueString"));
  if (!arr(r->get("childValues")).empty()) {
    Array cv;
    for (const Value& c : arr(r->get("childValues"))) {
      cv.push_back(make_object({{"key", at(c, "key")}, {"value", value_model(at(c, "value"))}}));
    }
    f.set("childValues", std::move(cv));
  }
  Array kids;
  for (const std::int64_t k : r->refs("children")) {
    Value m = field_model(k, res, depth + 1, budget);
    if (m.truthy()) {
      kids.push_back(std::move(m));
    }
  }
  if (!kids.empty()) {
    f.set("children", std::move(kids));
  }
  if (!r->refs("objects").empty()) {
    f.set("objects", hex_list(r->refs("objects")));
  }
  return f;
}
Value field_model(std::int64_t handle, const Resolver& res) {
  int budget = kFieldNodeBudget;
  return field_model(handle, res, 0, budget);
}

std::string field_string(const Value& v) {
  if (v.is_string()) {
    return v.as_string();
  }
  if (v.is_number()) {
    return num_text(v.as_number());
  }
  if (v.is_bool()) {
    return v.as_bool() ? "true" : "false";
  }
  if (v.is_null()) {
    return "null";
  }
  return base::to_json(v);
}

/// What a field shows: its cached value string, or its code with each %<\_FldIdx n>% replaced by child n.
// NOLINTNEXTLINE(misc-no-recursion) — bounded: field trees are built at most 16 deep (field_model)
std::string field_text(const Value& f) {
  if (!f.truthy()) {
    return {};
  }
  if (at(f, "valueString").truthy()) {
    return field_string(at(f, "valueString"));
  }
  const Array& kids = arr(at(f, "children"));
  const std::string code = at(f, "code").truthy() ? field_string(at(f, "code")) : std::string();
  static constexpr std::string_view kOpen = "%<\\_FldIdx ";
  std::string out;
  std::size_t p = 0;
  while (p < code.size()) {
    const std::size_t q = code.find(kOpen, p);
    if (q == std::string::npos) {
      break;
    }
    std::size_t d = q + kOpen.size();
    const std::size_t d0 = d;
    while (d < code.size() && code[d] >= '0' && code[d] <= '9') {
      ++d;
    }
    if (d == d0 || code.compare(d, 2, ">%") != 0) {
      out.append(code, p, q + 1 - p);
      p = q + 1;
      continue;
    }
    out.append(code, p, q - p);
    std::string_view digits = std::string_view(code).substr(d0, d - d0);
    while (digits.size() > 1 && digits.front() == '0') {
      digits.remove_prefix(1);
    }
    if (digits.size() <= 15) {
      std::size_t idx = 0;
      for (const char ch : digits) {
        idx = idx * 10 + static_cast<std::size_t>(ch - '0');
      }
      if (idx < kids.size() && kids[idx].truthy()) {
        out += field_text(kids[idx]);
      }
    }
    p = d + 2;
  }
  out.append(code, std::min(p, code.size()), std::string::npos);
  return out;
}

Value borders_model(const Value& list) {
  Value b = Value::object();
  for (const Value& x : arr(list)) {
    const Value& edge = at(x, "edge");
    if (!edge.truthy() || edge_side(ival(edge)).empty() || static_cast<double>(ival(edge)) != edge.num_or(0)) {
      continue;
    }
    Value s = make_object({{"overrides", at(x, "overrides")},
                           {"type", at(x, "borderType")},
                           {"color", at(x, "color")},
                           {"lineweight", at(x, "lineweight")},
                           {"visible", bool_v(!at(x, "invisible").truthy())},
                           {"doubleSpacing", at(x, "doubleSpacing")}});
    if (at(x, "_linetype").truthy()) {
      s.set("linetypeHandle", at(x, "_linetype"));
    }
    b.set(edge_side(ival(edge)), std::move(s));
  }
  return b;
}

/// A cell style (20.4.101.4 + content format 20.4.101.3) as model JSON, handles resolved.
Value cell_style_model(const Value& s, const Resolver& res) {
  if (!s.truthy()) {
    return {};
  }
  Value o = make_object({{"type", at(s, "styleType")}});
  if (has_def(s, "id")) {
    o.set("id", at(s, "id"));
    o.set("styleClass", at(s, "styleClass"));
    o.set("name", at(s, "name"));
  }
  if (!at(s, "hasData").truthy()) {
    return o;
  }
  const Value& c = at(s, "content");
  o.set("overrides", at(s, "overrides"));
  o.set("mergeFlags", at(s, "mergeFlags"));
  o.set("background", at(s, "background"));
  o.set("contentLayout", at(s, "contentLayout"));
  o.set("content", make_object({{"overrides", at(c, "overrides")},
                                {"propertyFlags", at(c, "propertyFlags")},
                                {"dataType", at(c, "dataType")},
                                {"unitType", at(c, "unitType")},
                                {"format", at(c, "format")},
                                {"rotation", at(c, "rotation")},
                                {"blockScale", at(c, "blockScale")},
                                {"align", at(c, "alignment")},
                                {"color", at(c, "color")},
                                {"textStyle", name_or_hx(res, at(c, "_textStyle"))},
                                {"textHeight", at(c, "textHeight")}}));
  if (at(s, "margins").truthy()) {
    o.set("margins", at(s, "margins"));
  }
  Value b = borders_model(at(s, "borders"));
  for (base::Member& m : b.as_object()) {
    if (at(m.value, "linetypeHandle").truthy()) {
      Value lt = name_or_hx(res, at(m.value, "linetypeHandle"));
      m.value.set("linetype", std::move(lt));
      m.value.erase("linetypeHandle");
    }
  }
  o.set("borders", std::move(b));
  return o;
}

/// The name a dictionary gives an object (its owner's entry), e.g. a table style's name.
Value entry_name(std::int64_t handle, const Resolver& res) {
  const Rec* r = res.get(handle);
  const Rec* d = r != nullptr ? res.get(r->ref("owner")) : nullptr;
  if (d == nullptr || !d->get("names").truthy() || !d->has_h("items")) {
    return {};
  }
  const auto& items = d->refs("items");
  const auto it = std::ranges::find(items, handle);
  if (it == items.end()) {
    return {};
  }
  const auto k = static_cast<std::size_t>(it - items.begin());
  const Array& names = arr(d->get("names"));
  return k < names.size() ? names[k] : Value();
}

/// Table style → neutral { name, description, flowDirection, flags, hMargin, vMargin, titleSuppressed,
/// headerSuppressed, cellStyles[] }
Value table_style_model(const Rec& r, const Resolver& res) {
  Value o = make_object({{"name", entry_name(r.handle, res)}, {"description", r.get("description")}});
  if (r.get("rowStyles").truthy()) {  // R13–R21
    o.set("flowDirection", r.get("flowDirection"));
    o.set("flags", r.get("flags"));
    o.set("hMargin", r.get("hMargin"));
    o.set("vMargin", r.get("vMargin"));
    o.set("titleSuppressed", bool_v(r.get("titleSuppressed").truthy()));
    o.set("headerSuppressed", bool_v(r.get("headerSuppressed").truthy()));
    Array styles;
    const Array& rows = arr(r.get("rowStyles"));
    static constexpr std::array<int, 3> kIds = {3, 1, 2};
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const Value& s = rows[i];
      const Value id = i < kIds.size() ? Value(kIds.at(i)) : Value();
      Value borders = Value::object();
      const Array& bl = arr(at(s, "borders"));
      for (std::size_t k = 0; k < bl.size(); ++k) {
        const Value& b = bl[k];
        const std::string side =
            k < kOldBorderOrder.size() ? std::string(kOldBorderOrder.at(k)) : std::string("undefined");
        borders.set(side, make_object({{"lineweight", at(b, "lineweight")},
                                       {"visible", bool_v(at(b, "visible").truthy())},
                                       {"color", at(b, "color")}}));
      }
      const bool is_data = id.is_number() && id.as_number() == 3;
      Value cs = make_object({{"id", id},
                              {"name", default_style_name(id)},
                              {"styleClass", is_data ? 1 : 2},
                              {"textStyle", name_or_hx(res, at(s, "_textStyle"))},
                              {"textHeight", at(s, "textHeight")},
                              {"align", at(s, "alignment")},
                              {"color", at(s, "textColor")},
                              {"background", at(s, "fillColor")},
                              {"backgroundEnabled", bool_v(at(s, "fillEnabled").truthy())},
                              {"borders", std::move(borders)}});
      if (has_def(s, "dataType")) {
        cs.set("dataType", at(s, "dataType"));
        cs.set("unitType", at(s, "unitType"));
        cs.set("format", at(s, "format"));
      }
      styles.push_back(std::move(cs));
    }
    o.set("cellStyles", std::move(styles));
  } else {  // R24+: the "Table" cell style carries the table-wide settings
    const Value& t = r.get("tableCellStyle");
    o.set("flags", r.get("unknownBL1"));
    o.set("unknown", make_object({{"rc", r.get("unknownRC")}, {"bl2", r.get("unknownBL2")}}));
    o.set("tableCellStyle", cell_style_model(t, res));
    o.set("flowDirection", has_def(t, "overrides") && (bits32(at(t, "mergeFlags")) & 0x10000U) != 0 ? 1 : 0);
    Array styles;
    for (const Value& s : arr(r.get("cellStyles"))) {
      Value m = cell_style_model(s, res);
      Value cs = make_object({{"id", at(s, "id")}, {"name", at(s, "name")}, {"styleClass", at(s, "styleClass")}});
      const Value& mc = at(m, "content");
      if (mc.truthy()) {
        const Value& bg = at(m, "background");
        const Value& bgi = at(bg, "index");
        const bool none = bg.truthy() && bgi.is_number() && bgi.as_number() == 257;
        cs.set("textStyle", at(mc, "textStyle"));
        cs.set("textHeight", at(mc, "textHeight"));
        cs.set("align", at(mc, "align"));
        cs.set("color", at(mc, "color"));
        cs.set("background", bg);
        cs.set("backgroundEnabled", bool_v(!none));
        cs.set("dataType", at(mc, "dataType"));
        cs.set("unitType", at(mc, "unitType"));
        cs.set("format", at(mc, "format"));
        cs.set("rotation", at(mc, "rotation"));
        cs.set("margins", at(m, "margins"));
        cs.set("borders", at(m, "borders"));
        cs.set("cell", m);
      }
      styles.push_back(std::move(cs));
    }
    o.set("cellStyles", std::move(styles));
    if (at(t, "margins").truthy()) {
      o.set("vMargin", at(at(t, "margins"), "vertical"));
      o.set("hMargin", at(at(t, "margins"), "horizontal"));
    }
  }
  return o;
}

Value style_of(const Value& handle, const Resolver& res) {
  const Rec* r = res.get(ival(handle));
  return r != nullptr && r->type == "TABLESTYLE" ? table_style_model(*r, res) : Value(nullptr);
}

Value attr_model(const Value& attdef, const Value& value, const Value& index, const Resolver& res) {
  Value a = make_object({{"attdef", hx(attdef)}, {"value", value}, {"index", index}});
  const Rec* d = res.get(ival(attdef));
  if (d != nullptr && has_def(d->f, "tag")) {
    a.set("tag", d->get("tag"));
  }
  return a;
}

/**
 * Border overrides on a shared edge belong to the upper cell (its bottom) and the left cell (its right): a top or
 * left override moves to the neighbour — as the ODA File Converter stores them (found by experiment).
 */
void canonical_borders(Array& cells) {
  struct Move {
    std::string_view side;
    int di;
    int dj;
    std::string_view to;
  };
  static constexpr std::array<Move, 2> kMoves = {Move{"top", -1, 0, "bottom"}, Move{"left", 0, -1, "right"}};
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (!cells[i].is_array()) {
      continue;
    }
    for (std::size_t j = 0; j < cells[i].as_array().size(); ++j) {
      auto own = [&cells, i, j]() -> Value& { return cells[i].as_array()[j]; };
      if (!at(at(own(), "overrides"), "borders").truthy()) {
        continue;
      }
      for (const Move& mv : kMoves) {
        const Value moved = at(at(at(own(), "overrides"), "borders"), mv.side);
        if (!moved.truthy()) {
          continue;
        }
        if ((mv.di < 0 && i == 0) || (mv.dj < 0 && j == 0)) {
          continue;
        }
        const std::size_t ni = mv.di < 0 ? i - 1 : i;
        const std::size_t nj = mv.dj < 0 ? j - 1 : j;
        if (!cells[ni].is_array() || nj >= cells[ni].as_array().size() || !cells[ni].as_array()[nj].truthy()) {
          continue;
        }
        Value& n = cells[ni].as_array()[nj];
        if (!at(n, "overrides").truthy()) {
          n.set("overrides", Value::object());
        }
        Value& ov = *n.find("overrides");
        Value nb = at(ov, "borders").is_object() ? at(ov, "borders") : Value::object();
        Value inner = at(nb, mv.to).is_object() ? at(nb, mv.to) : Value::object();
        if (moved.is_object()) {
          for (const base::Member& m : moved.as_object()) {
            inner.set(m.key, m.value);
          }
        }
        nb.set(mv.to, std::move(inner));
        ov.set("borders", std::move(nb));
        own().find("overrides")->find("borders")->erase(mv.side);
      }
      if (key_count(at(at(own(), "overrides"), "borders")) == 0) {
        own().find("overrides")->erase("borders");
        if (key_count(at(own(), "overrides")) == 0) {
          own().erase("overrides");
        }
      }
    }
  }
}

Value old_table_model(const Rec& rec, const Value& t, const Resolver& res) {
  const Value ts_h(static_cast<double>(rec.ref("tableStyle")));
  const Value style = style_of(ts_h, res);
  const Array& col_widths = arr(at(t, "colWidths"));
  const Array& row_heights = arr(at(t, "rowHeights"));
  const std::size_t nc = col_widths.size();
  const std::size_t nr = row_heights.size();
  Value T = make_object(
      {{"style", hx(ts_h)}, {"styleName", style.truthy() ? at(style, "name") : Value()}, {"flags", at(t, "flags")}});
  const Value o = at(t, "overrides").truthy() ? at(t, "overrides") : Value::object();
  Value fd = at(o, "flowDirection");
  if (nullish(fd)) {
    fd = style.truthy() ? at(style, "flowDirection") : Value(0);
  }
  if (nullish(fd)) {
    fd = 0;
  }
  T.set("flowDirection", fd);
  Value title_suppressed(false);
  if (has_def(o, "titleSuppressed")) {
    title_suppressed = bool_v(at(o, "titleSuppressed").truthy());
  } else if (style.truthy()) {
    title_suppressed = at(style, "titleSuppressed");
  }
  T.set("titleSuppressed", title_suppressed);
  const Value header_suppressed = style.truthy() ? at(style, "headerSuppressed") : Value(false);
  T.set("headerSuppressed", header_suppressed);
  auto row_style = [&](std::size_t i) -> std::string_view {
    std::size_t k = 0;
    if (!title_suppressed.truthy()) {
      if (i == k) {
        return "_TITLE";
      }
      ++k;
    }
    if (!header_suppressed.truthy()) {
      if (i == k) {
        return "_HEADER";
      }
    }
    return "_DATA";
  };
  // styleByName: the last cell style of each name
  auto style_by_name = [&style](const Value& name) -> const Value* {
    const Array& list = arr(at(style, "cellStyles"));
    for (const auto& it : std::views::reverse(list)) {
      if (at(it, "name") == name) {
        return &it;
      }
    }
    return nullptr;
  };
  Array rows;
  for (std::size_t i = 0; i < nr; ++i) {
    rows.push_back(make_object({{"height", row_heights[i]}, {"style", row_style(i)}}));
  }
  T.set("rows", rows);
  Array columns;
  for (const Value& w : col_widths) {
    columns.push_back(make_object({{"width", w}}));
  }
  T.set("columns", std::move(columns));
  Array cells_out;
  Array merged_out;
  const Array& cells = arr(at(t, "cells"));
  for (std::size_t i = 0; i < nr; ++i) {
    Array row;
    for (std::size_t j = 0; j < nc; ++j) {
      const std::size_t idx = i * nc + j;
      const Value& c = idx < cells.size() ? cells[idx] : kUndef();
      const Value& ct = at(c, "type");
      const bool is_block = ct.is_number() && ct.as_number() == 2;
      Value cell = make_object({{"type", is_block ? "block" : "text"}, {"value", ""}});
      if (is_block) {
        cell.set("block", name_or_hx(res, at(c, "_value")));
        cell.set("blockRecord", hx(at(c, "_value")));
        cell.set("blockScale", at(c, "blockScale"));
        if (at(c, "attrs").truthy()) {
          Array attrs;
          for (const Value& a : arr(at(c, "attrs"))) {
            attrs.push_back(attr_model(at(a, "_attdef"), at(a, "value"), at(a, "index"), res));
          }
          cell.set("attrs", std::move(attrs));
        }
      } else if (at(c, "_value").truthy()) {
        Value fm = field_model(ival(at(c, "_value")), res);
        std::string text = field_text(fm);
        cell.set("field", std::move(fm));
        cell.set("value", std::move(text));
      } else if (at(c, "value").truthy()) {
        cell.set("value", value_text(at(c, "value")));
      } else {
        cell.set("value", nullish(at(c, "text")) ? Value("") : at(c, "text"));
      }
      if (at(c, "value").truthy()) {
        cell.set("data", value_model(at(c, "value")));
      }
      cell.set("style", at(rows[i], "style"));
      cell.set("rotation", at(c, "rotation"));
      if (at(c, "autoFit").truthy()) {
        cell.set("autoFit", true);
      }
      const double mw = at(c, "mergedWidth").num_or(0);
      const double mh = at(c, "mergedHeight").num_or(0);
      if (mw > 1 || mh > 1) {
        const Value mrows = at(c, "mergedHeight").truthy() ? at(c, "mergedHeight") : Value(1);
        const Value mcols = at(c, "mergedWidth").truthy() ? at(c, "mergedWidth") : Value(1);
        const auto di = static_cast<double>(i);
        const auto dj = static_cast<double>(j);
        merged_out.push_back(make_object(
            {{"top", di}, {"left", dj}, {"bottom", di + mrows.num_or(0) - 1}, {"right", dj + mcols.num_or(0) - 1}}));
        cell.set("merged", make_object({{"rows", mrows}, {"cols", mcols}}));
      }
      if (at(c, "merged").truthy()) {
        cell.set("covered", true);
      }
      if (at(c, "edgeFlags").truthy() || at(c, "virtualEdges").truthy()) {
        cell.set("edges",
                 make_object({{"flags", at(c, "edgeFlags")},
                              {"virtual", at(c, "virtualEdges").truthy() ? at(c, "virtualEdges") : Value(0)}}));
      }
      if (at(c, "hasOverrides").truthy()) {
        Value ov = Value::object();
        const std::uint32_t fl = bits32(at(c, "overrideFlags"));
        if ((fl & 0x01U) != 0) {
          ov.set("align", at(c, "alignment"));
        }
        if ((fl & 0x02U) != 0) {
          ov.set("fillNone", bool_v(at(c, "fillNone").truthy()));
        }
        if ((fl & 0x04U) != 0) {
          ov.set("background", at(c, "background"));
        }
        if ((fl & 0x08U) != 0) {
          ov.set("color", at(c, "contentColor"));
        }
        if ((fl & 0x10U) != 0) {
          ov.set("textStyle", name_or_hx(res, at(c, "_textStyle")));
        }
        if ((fl & 0x20U) != 0) {
          ov.set("textHeight", at(c, "textHeight"));
        }
        if (at(c, "borders").truthy() && key_count(at(c, "borders")) != 0) {
          ov.set("borders", at(c, "borders"));
        }
        if (key_count(ov) != 0) {
          cell.set("overrides", std::move(ov));
        }
      }
      const Value* st = style_by_name(at(cell, "style"));
      const Value& cov = at(cell, "overrides");
      Value align;
      if (cov.truthy() && has_def(cov, "align")) {
        align = at(cov, "align");
      } else if (st != nullptr && st->truthy()) {
        align = at(*st, "align");
      }
      cell.set("align", std::move(align));
      row.push_back(std::move(cell));
    }
    cells_out.emplace_back(std::move(row));
  }
  canonical_borders(cells_out);
  T.set("cells", std::move(cells_out));
  T.set("merged", std::move(merged_out));
  if (at(t, "hasOverrides").truthy()) {
    Value ov = Value::object();
    if (o.is_object()) {
      for (const base::Member& m : o.as_object()) {
        if (!m.key.empty() && m.key[0] == '_') {
          ov.set(std::string_view(m.key).substr(1), name_or_hx(res, m.value));
        } else {
          ov.set(m.key, m.value);
        }
      }
    }
    T.set("overrides", std::move(ov));
    T.set("overrideFlags", at(t, "overrideFlags"));
  }
  for (const std::string_view k : {"borderColors", "borderLineweights", "borderVisibility"}) {
    if (at(t, k).truthy()) {
      T.set(k, at(t, k));
    }
  }
  return T;
}

Value new_table_model(const Value& t, const Resolver& res) {
  const Value& c = at(t, "content");
  const Value style = style_of(at(c, "_style"), res);
  Value T = make_object({{"style", hx(at(c, "_style"))}, {"styleName", style.truthy() ? at(style, "name") : Value()}});
  if (at(c, "name").truthy()) {
    T.set("name", at(c, "name"));
  }
  if (at(c, "description").truthy()) {
    T.set("description", at(c, "description"));
  }
  const Array& style_list = arr(at(style, "cellStyles"));
  auto style_name = [&style_list](const Value& id) -> Value {
    // names = new Map(cellStyles.map(s => [s.id, s.name])): the last style of each id wins
    for (const auto& it : std::views::reverse(style_list)) {
      if (at(it, "id") == id && id.is_number()) {
        const Value& n = at(it, "name");
        if (!nullish(n)) {
          return n;
        }
        break;
      }
    }
    Value d = default_style_name(id);
    if (!d.is_undefined()) {
      return d;
    }
    if (id.truthy()) {
      return "id" + field_string(id);
    }
    return {};
  };
  const Value ts = cell_style_model(at(c, "tableStyle"), res);
  if (ts.truthy() && has_def(ts, "overrides") && (bits32(at(ts, "mergeFlags")) & 0x10000U) != 0) {
    T.set("flowDirection", 1);
  } else {
    T.set("flowDirection", style.truthy() ? at(style, "flowDirection") : Value(0));
  }
  auto custom_model = [](const Value& list) {
    Array out;
    for (const Value& x : arr(list)) {
      out.push_back(make_object({{"name", at(x, "name")}, {"value", value_model(at(x, "value"))}}));
    }
    return out;
  };
  const Array& crows = arr(at(c, "rows"));
  Array rows;
  for (const Value& r : crows) {
    Value o = make_object({{"height", at(r, "height")}, {"style", style_name(at(r, "styleId"))}});
    Value st = cell_style_model(at(r, "style"), res);
    if (st.truthy() && has_def(st, "overrides")) {
      o.set("cellStyle", std::move(st));
    }
    if (!arr(at(r, "custom")).empty()) {
      o.set("custom", custom_model(at(r, "custom")));
    }
    rows.push_back(std::move(o));
  }
  T.set("rows", rows);
  Array columns;
  for (const Value& col : arr(at(c, "columns"))) {
    Value o = make_object({{"width", at(col, "width")}});
    if (at(col, "name").truthy()) {
      o.set("name", at(col, "name"));
    }
    if (at(col, "styleId").truthy()) {
      o.set("style", style_name(at(col, "styleId")));
    }
    Value st = cell_style_model(at(col, "style"), res);
    if (st.truthy() && has_def(st, "overrides")) {
      o.set("cellStyle", std::move(st));
    }
    if (!arr(at(col, "custom")).empty()) {
      o.set("custom", custom_model(at(col, "custom")));
    }
    columns.push_back(std::move(o));
  }
  T.set("columns", std::move(columns));
  if (ts.truthy() && has_def(ts, "overrides")) {
    T.set("tableCellStyle", ts);
  }
  const Array& merged = arr(at(c, "merged"));
  T.set("merged", Array(merged.begin(), merged.end()));
  // mergeAt (the last merge starting at a cell wins) and the covered cells. Only cells of the grid are ever looked
  // up, so the ranges are clamped to it (a hostile range cannot make the loops long).
  std::size_t max_cols = 0;
  for (const Value& r : crows) {
    max_cols = std::max(max_cols, arr(at(r, "cells")).size());
  }
  std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> merge_at;
  std::set<std::pair<std::int64_t, std::int64_t>> covered;
  for (std::size_t k = 0; k < merged.size(); ++k) {
    const Value& m = merged[k];
    const std::int64_t top = ival(at(m, "top"));
    const std::int64_t left = ival(at(m, "left"));
    const std::int64_t bottom = ival(at(m, "bottom"));
    const std::int64_t right = ival(at(m, "right"));
    merge_at[{top, left}] = k;
    const std::int64_t i1 = std::min(bottom, static_cast<std::int64_t>(crows.size()) - 1);
    const std::int64_t j1 = std::min(right, static_cast<std::int64_t>(max_cols) - 1);
    for (std::int64_t i = std::max<std::int64_t>(top, 0); i <= i1; ++i) {
      for (std::int64_t j = std::max<std::int64_t>(left, 0); j <= j1; ++j) {
        if (i != top || j != left) {
          covered.insert({i, j});
        }
      }
    }
  }
  Array cells_out;
  for (std::size_t i = 0; i < crows.size(); ++i) {
    Array row_out;
    const Array& xs = arr(at(crows[i], "cells"));
    for (std::size_t j = 0; j < xs.size(); ++j) {
      const Value& x = xs[j];
      Value cell = make_object({{"type", "text"}, {"value", ""}});
      const Array& contents = arr(at(x, "contents"));
      Value cf;
      if (!contents.empty() && contents[0].truthy()) {
        const Value& ct = contents[0];
        const double type = at(ct, "type").num_or(-1);
        if (type == 4) {
          cell.set("type", "block");
          cell.set("block", name_or_hx(res, at(ct, "_block")));
          cell.set("blockRecord", hx(at(ct, "_block")));
          if (at(ct, "format").truthy()) {
            cell.set("blockScale", at(at(ct, "format"), "blockScale"));
          }
          if (!arr(at(ct, "attrs")).empty()) {
            Array attrs;
            for (const Value& a : arr(at(ct, "attrs"))) {
              attrs.push_back(attr_model(at(a, "_attdef"), at(a, "value"), at(a, "index"), res));
            }
            cell.set("attrs", std::move(attrs));
          }
        } else if (type == 2) {
          Value fm = field_model(ival(at(ct, "_field")), res);
          std::string text = field_text(fm);
          cell.set("field", std::move(fm));
          cell.set("value", std::move(text));
        } else if (type == 1) {
          cell.set("value", value_text(at(ct, "value")));
          cell.set("data", value_model(at(ct, "value")));
        }
        if (at(ct, "format").truthy()) {
          const Value wrapped = make_object({{"styleType", 0},
                                             {"hasData", 1},
                                             {"overrides", 0},
                                             {"mergeFlags", 0},
                                             {"contentLayout", 0},
                                             {"content", at(ct, "format")},
                                             {"marginFlags", 0},
                                             {"borders", Array{}}});
          cf = at(cell_style_model(wrapped, res), "content");
          cell.set("contentFormat", cf);
        }
        if (contents.size() > 1) {
          cell.set("extraContents", static_cast<double>(contents.size() - 1));  // only the first is modelled
        }
      }
      cell.set("style", at(x, "styleId").truthy() ? style_name(at(x, "styleId")) : at(rows[i], "style"));
      Value st = cell_style_model(at(x, "style"), res);
      cell.set("rotation", cf.truthy() ? at(cf, "rotation") : Value(0));
      if (at(x, "state").truthy()) {
        cell.set("state", at(x, "state"));
      }
      if (at(x, "tooltip").truthy()) {
        cell.set("tooltip", at(x, "tooltip"));
      }
      if (!arr(at(x, "custom")).empty()) {
        cell.set("custom", custom_model(at(x, "custom")));
      }
      const auto key = std::make_pair(static_cast<std::int64_t>(i), static_cast<std::int64_t>(j));
      if (const auto it = merge_at.find(key); it != merge_at.end()) {
        const Value& m = merged[it->second];
        cell.set("merged", make_object({{"rows", at(m, "bottom").num_or(0) - at(m, "top").num_or(0) + 1},
                                        {"cols", at(m, "right").num_or(0) - at(m, "left").num_or(0) + 1}}));
      }
      if (covered.contains(key)) {
        cell.set("covered", true);
      }
      // overrides: the cell style's and the content format's flagged properties (20.4.101.3 bits)
      Value ov = Value::object();
      auto take = [&ov](std::uint32_t fl, const Value& src) {
        if (!src.truthy()) {
          return;
        }
        if ((fl & 0x10U) != 0) {
          ov.set("align", at(src, "align"));
        }
        if ((fl & 0x20U) != 0) {
          ov.set("color", at(src, "color"));
        }
        if ((fl & 0x40U) != 0) {
          ov.set("textStyle", at(src, "textStyle"));
        }
        if ((fl & 0x80U) != 0) {
          ov.set("textHeight", at(src, "textHeight"));
        }
      };
      if (st.truthy() && has_def(st, "overrides")) {
        const std::uint32_t so = bits32(at(st, "overrides"));
        take(so, at(st, "content"));
        if ((so & 0x200U) != 0) {
          const Value& bg = at(st, "background");
          ov.set("background", bg);
          const Value& bgi = at(bg, "index");
          if (bg.truthy() && bgi.is_number() && bgi.as_number() == 257) {
            ov.set("fillNone", true);
          }
        }
        cell.set("cellStyle", st);
      }
      if (cf.truthy()) {
        take(bits32(at(cf, "overrides")), cf);
      }
      const Value* sm = nullptr;  // style.cellStyles.find((s) => s.name === cell.style)
      for (const Value& s : style_list) {
        if (at(s, "name") == at(cell, "style")) {
          sm = &s;
          break;
        }
      }
      // R2010+ cells carry a border record per edge (override bits 26 on every one): an override is a value that
      // differs from the cell style's (found by having the ODA File Converter save an R2004 border override as R2018)
      if (st.truthy() && at(st, "borders").truthy()) {
        for (const std::string_view side : kSides) {
          const Value& b = at(at(st, "borders"), side);
          if (!b.truthy()) {
            continue;
          }
          const Value& smb = sm != nullptr ? at(at(*sm, "borders"), side) : kUndef();
          const Value d =
              smb.truthy()
                  ? smb
                  : make_object({{"color", make_object({{"index", 0}})}, {"lineweight", -2}, {"visible", true}});
          Value x2 = Value::object();
          const Value dc = nullish(at(d, "color")) ? make_object({{"index", 0}}) : at(d, "color");
          if (json_of(at(b, "color")) != json_of(dc)) {
            x2.set("color", at(b, "color"));
          }
          const Value dl = nullish(at(d, "lineweight")) ? Value(-2) : at(d, "lineweight");
          if (!(at(b, "lineweight") == dl)) {
            x2.set("lineweight", at(b, "lineweight"));
          }
          const Value dv = nullish(at(d, "visible")) ? Value(true) : at(d, "visible");
          if (!(at(b, "visible") == dv)) {
            x2.set("visible", at(b, "visible"));
          }
          if (key_count(x2) != 0) {
            if (!ov.has("borders")) {
              ov.set("borders", Value::object());
            }
            ov.find("borders")->set(side, std::move(x2));
          }
        }
      }
      Value align;
      if (has_def(ov, "align")) {
        align = at(ov, "align");
      } else if (sm != nullptr && sm->truthy()) {
        align = at(*sm, "align");
      }
      if (key_count(ov) != 0) {
        cell.set("overrides", std::move(ov));
      }
      cell.set("align", std::move(align));
      row_out.push_back(std::move(cell));
    }
    cells_out.emplace_back(std::move(row_out));
  }
  T.set("cells", std::move(cells_out));
  T.set("tableBits",
        at(t, "unknownBS"));  // the BS after the R24 table content (6 or 38 in the corpus; meaning unknown)
  canonical_borders(T.find("cells")->as_array());
  if (at(t, "hasBreak").truthy()) {
    T.set("breaks", at(t, "break"));
  }
  if (!arr(at(t, "rowRanges")).empty()) {
    T.set("rowRanges", at(t, "rowRanges"));
  }
  return T;
}

/// a plain copy with "_name" handle numbers turned into "name" hex strings
// NOLINTNEXTLINE(misc-no-recursion) — bounded at depth 64
Value deep(const Value& v, int depth = 0) {
  if (depth > 64) {
    return v;
  }
  if (v.is_array()) {
    Array o;
    o.reserve(v.as_array().size());
    for (const Value& x : v.as_array()) {
      o.push_back(deep(x, depth + 1));
    }
    return o;
  }
  if (!v.is_object() || v.is_point()) {  // a point has no "_" handle members
    return v;
  }
  Value o = Value::object();
  for (const base::Member& m : v.as_object()) {
    if (!m.key.empty() && m.key[0] == '_') {
      const Value& x = m.value;
      const std::string_view k = std::string_view(m.key).substr(1);
      if (x.is_array()) {
        o.set(k, hex_list(x.as_array()));
      } else if (x.is_number()) {
        o.set(k, hx(x));
      } else {
        o.set(k, deep(x, depth + 1));
      }
    } else {
      o.set(m.key, deep(m.value, depth + 1));
    }
  }
  return o;
}

}  // namespace

void table_canonical_borders(Array& cells) {
  canonical_borders(cells);
}

Decoder table_decoder(std::string_view type) {
  if (type == "ACAD_TABLE") {
    return dec_acad_table;
  }
  if (type == "TABLESTYLE") {
    return dec_tablestyle;
  }
  if (type == "CELLSTYLEMAP") {
    return dec_cellstylemap;
  }
  if (type == "TABLECONTENT") {
    return dec_tablecontent;
  }
  if (type == "TABLEGEOMETRY") {
    return dec_tablegeometry;
  }
  if (type == "FIELD") {
    return dec_field;
  }
  if (type == "FIELDLIST") {
    return dec_fieldlist;
  }
  return nullptr;
}

/// model fields of an ACAD_TABLE entity record (f = the entity being built)
void table_entity_fields(const Rec& rec, Value& f, const Resolver& res) {
  const Value block(static_cast<double>(rec.ref("block")));
  f.set("block", name_or_hx(res, block));
  f.set("blockRecord", hx(block));
  for (const std::string_view k : {"partial", "tableFlags", "rows", "cols", "colWidths", "rowHeights"}) {
    f.erase(k);
  }
  const Value& t = rec.get("_table");
  if (!t.truthy()) {
    return;
  }
  f.set("horizDir", rec.get("horizDir"));
  if (at(t, "content").truthy()) {
    f.set("table", new_table_model(t, res));
  } else {
    f.set("table", old_table_model(rec, t, res));
  }
}

/// model fields of the table objects (TABLESTYLE, CELLSTYLEMAP, TABLECONTENT, TABLEGEOMETRY, FIELD)
void table_object_fields(const Rec& rec, Value& f, const Resolver& res) {
  const std::string& type = rec.type;
  if (type == "TABLESTYLE") {
    static constexpr std::array<std::string_view, 5> kKeep = {"handle", "owner", "reactors", "extDict", "type"};
    std::vector<std::string> drop;
    for (const base::Member& m : f.as_object()) {
      if (std::ranges::find(kKeep, m.key) == kKeep.end()) {
        drop.push_back(m.key);
      }
    }
    for (const std::string& k : drop) {
      f.erase(k);
    }
    Value m = table_style_model(rec, res);
    for (base::Member& x : m.as_object()) {
      f.set(x.key, std::move(x.value));
    }
  } else if (type == "CELLSTYLEMAP") {
    Array styles;
    for (const Value& s : arr(rec.get("cellStyles"))) {
      styles.push_back(cell_style_model(s, res));
    }
    f.set("cellStyles", std::move(styles));
  } else if (type == "TABLECONTENT") {
    f.set("content", deep(rec.get("_content")));
  } else if (type == "TABLEGEOMETRY") {
    f.set("cells", deep(rec.get("cells")));
  } else if (type == "FIELD") {
    Value m = field_model(rec.handle, res);
    if (m.truthy()) {
      m.erase("children");
      for (base::Member& x : m.as_object()) {
        f.set(x.key, std::move(x.value));
      }
      f.set("children", hex_list(rec.refs("children")));
    }
  }
}

}  // namespace cc::dwg
