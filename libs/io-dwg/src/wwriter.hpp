// SPDX-License-Identifier: Apache-2.0
// The state of one writeDwg run (write.js's closure: handles, names, tables, blocks, records), shared with the ports of
// the writer's other modules: nongraphic-write.js + mleader-write.js + passthrough.js (wcopy.cpp), table-write.js
// (wtable.cpp), solid-write.js + surface-write.js + acis.js + acds.js (wsolid.cpp).
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "wobjects.hpp"

namespace cc::dwg::w {

/// hexKey: a handle (number or hex string) as upper-case hex without leading zeros; nullopt for undefined / null.
using Key = std::optional<std::string>;
[[nodiscard]] Key hex_key(const Value& h);
[[nodiscard]] std::string hex_upper(double v);

struct BlockW {
  const Value* src = nullptr;  // the model block (or `own`, for one the writer adds)
  Value own;
  std::string name;
  std::string kind;  // model | paper | paper2 | block
  double hdr = 0, block = 0, endblk = 0, layout = 0;
  std::vector<std::vector<RecPtr>> ents;
  std::vector<double> inserts;
  const Value* rec_src = nullptr;
  [[nodiscard]] const Value& s() const { return src != nullptr ? *src : own; }
};

struct LayerW {
  RecPtr rec;
};

struct Writer;

/// nongraphic-write.js extendWriter (wcopy.cpp).
struct CopyWriter {
  explicit CopyWriter(Writer& w);
  ~CopyWriter();
  CopyWriter(const CopyWriter&) = delete;
  CopyWriter& operator=(const CopyWriter&) = delete;
  CopyWriter(CopyWriter&&) = delete;
  CopyWriter& operator=(CopyWriter&&) = delete;
  /// Records, a skip reason, or nothing (not an entity of this module).
  [[nodiscard]] std::optional<std::variant<std::vector<RecPtr>, std::string>> convert(const Value& e,
                                                                                      const std::string& type,
                                                                                      double owner) const;
  /// layouts: each written layout's source handle and new handle
  void finish(std::vector<RecPtr>& recs, const std::vector<std::pair<Value, double>>& layouts);
  std::map<std::string, double, std::less<>> header_handles;
  struct Impl;
  std::unique_ptr<Impl> impl;
};

/// table-write.js tableWriter (wtable.cpp).
struct TableWriter {
  explicit TableWriter(Writer& w);
  ~TableWriter();
  TableWriter(const TableWriter&) = delete;
  TableWriter& operator=(const TableWriter&) = delete;
  TableWriter(TableWriter&&) = delete;
  TableWriter& operator=(TableWriter&&) = delete;
  [[nodiscard]] std::variant<std::vector<RecPtr>, std::string> convert(const Value& e, double owner) const;
  void finish(std::vector<RecPtr>& recs) const;
  /// COPY_SKIP: the source handles the object copier leaves out (round-trip data of tables and table styles).
  [[nodiscard]] bool skip(const Value& o) const;
  struct Impl;
  std::unique_ptr<Impl> impl;
};

/// solid-write.js solidWriter + surface-write.js surfaceWriter (wsolid.cpp).
struct SolidWriter {
  explicit SolidWriter(Writer& w);
  ~SolidWriter();
  SolidWriter(const SolidWriter&) = delete;
  SolidWriter& operator=(const SolidWriter&) = delete;
  SolidWriter(SolidWriter&&) = delete;
  SolidWriter& operator=(SolidWriter&&) = delete;
  [[nodiscard]] std::optional<std::vector<RecPtr>> convert_surface(const Value& e, const std::string& type,
                                                                   double owner) const;
  std::optional<std::vector<RecPtr>> convert_solid(const Value& e, const std::string& type, double owner,
                                                   const std::function<RecPtr(const std::string&)>& c) const;
  void finish() const;
  void finish_surfaces(std::vector<RecPtr>& recs) const;
  [[nodiscard]] std::optional<ByteBuf> data_storage() const;
  struct Impl;
  std::unique_ptr<Impl> impl;
};

struct Writer {
  const Value& model;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): one write of this model
  std::string version;
  bool r2004 = false, r2007 = false;
  Value report;  // { version, notes, counts, skipped, layers, handles, renamed, objects, entities, … }
  std::function<void(std::span<std::uint8_t>)> random;

  // handles
  double seed = 1;
  double next() { return seed++; }
  std::map<std::string, double, std::less<>> H;
  [[nodiscard]] double h(std::string_view k) const {
    const auto it = H.find(k);
    return it == H.end() ? 0 : it->second;
  }
  std::unordered_map<std::string, double> src_map;        // model handle → new handle
  std::vector<std::pair<std::string, double>> src_order;  // the same in the JS Map's insertion order
  std::unordered_map<std::string, std::size_t> src_index;
  void map_src(const Value& src, double h);
  void map_src_key(const std::string& k, double h);
  [[nodiscard]] double src(const Value& hex) const;  // srcMap.get(hexKey(hex)) || 0
  [[nodiscard]] bool has_src(const Key& k) const { return k && src_map.contains(*k); }

  // names
  std::unordered_map<std::string, std::set<std::string>> used_names;
  std::string clean_name(const std::string& table, const Value& name, const std::string& fallback);
  void reserve(const std::string& table, const std::string& name);

  // tables
  std::unordered_map<std::string, double> lt_by_name;
  std::unordered_map<std::string, double> style_by_name;
  std::unordered_map<std::string, RecPtr> layer_by_name, layer_by_new;
  std::unordered_map<std::string, double> app_by_name;
  std::unordered_map<std::string, double> dim_by_name;
  std::vector<RecPtr> ltypes, styles, layers, appids, dimstyles, views, ucs_list;
  std::set<std::string> xref_block_names;
  double lost_rgb = 0;
  struct NameNotes {
    double renamed_layers = 0, renamed_blocks = 0;
  } notes;

  /// ltypeRef → { flag, h? }
  [[nodiscard]] Value ltype_ref(const Value& name) const;
  [[nodiscard]] double style_ref(const Value& name) const;
  [[nodiscard]] double dim_ref(const Value& name) const;
  double layer_ref(const Value& name);
  RecPtr add_layer(const Value& l);
  double add_app(const Value& name, const Value& src = Value());
  [[nodiscard]] Value eed_of(const Value& x);
  [[nodiscard]] Value color(const Value& c);
  /// common(e, t, owner): the common entity fields of a record (allocates its handle first)
  [[nodiscard]] Value common(const Value& e, std::string_view t, double owner);

  // blocks
  std::deque<BlockW> blocks;
  std::unordered_map<std::string, BlockW*> block_by_name;
  BlockW* MS = nullptr;
  std::vector<BlockW*> PSs;
  BlockW* add_block(const Value* src, Value own, const std::string& kind, const std::string& forced);
  [[nodiscard]] BlockW* find_block(const Value& v);
  [[nodiscard]] std::size_t block_index(const BlockW* b) const;

  /// CLASSED: every class the writer knows (the modules' too); RAW:<dxf> entries join for copied custom classes
  std::map<std::string, ClassDef, std::less<>> classed;

  // the other modules
  std::unique_ptr<CopyWriter> X;
  std::unique_ptr<TableWriter> TBL;
  std::unique_ptr<SolidWriter> SOL;

  /// 16 random bytes as the JS guid() string (upper case)
  [[nodiscard]] std::string guid() const;
  void random_bytes(std::span<std::uint8_t> b) const;

  explicit Writer(const Value& m);
};

/// passthrough.js rawEntityCommon (wcopy.cpp; also the multileaders').
void raw_entity_common(ObjWriter& w, const Value& r, const Value& gfx_b64);
/// mleader-write.js encodeMleaderStyle (wcopy.cpp).
[[nodiscard]] std::optional<ByteBuf> encode_mleader_style(const Value& rv, const Value& o,
                                                          const std::function<double(const Value&)>& rf, Ctx& ctx);
/// table-write.js writeField (wtable.cpp; the copier's FIELD encoder uses it too).
void write_field(ObjWriter& w, const Value& f, const Array& kids, const Array& objs,
                 const std::function<double(const Value&)>& rf);

/// write.js aciFromRgb.
[[nodiscard]] double aci_from_rgb(double rgb);
/// write.js lwIndex.
[[nodiscard]] double lw_index(const Value& lw);
/// JS String#toUpperCase of String(v).
[[nodiscard]] std::string upper(const Value& v);
/// a < b on UTF-16 code units (JS string comparison).
[[nodiscard]] bool js_less(const std::string& a, const std::string& b);
/// JS /\s/ for one code point.
[[nodiscard]] bool js_space(std::uint32_t c) noexcept;

}  // namespace cc::dwg::w
