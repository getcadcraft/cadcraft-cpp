// SPDX-License-Identifier: Apache-2.0
// One decoded object of AcDb:AcDbObjects (objects-read.js `rec`). Decoder fields live in `f` in the order the JS sets
// them (keys starting with "_" are private and never reach the model); handle references in `h` (the JS `rec._h`);
// the common object/entity data and the framing in typed members.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bits.hpp"

namespace cc::dwg {

/// DXF-style handle text: upper-case hex, no leading zeros, "" for 0 (model.js handleHex).
[[nodiscard]] std::string handle_hex(std::int64_t h);

struct ClassInfo {
  int num = 0;
  int proxy_flags = 0;
  std::string app_name;
  std::string cpp_name;
  std::string dxf_name;
  bool was_zombie = false;
  bool is_entity = false;
  bool has_r18 = false;  // instanceCount, dwgVersion, maintVersion read (R2004+)
  std::int32_t instance_count = 0;
  std::int32_t dwg_version = 0;
  std::int32_t maint_version = 0;
};

struct Ctx {
  int ver = 15;
  int maint = 0;
  Codepage cp;
  std::unordered_map<int, ClassInfo> classes;
  bool strict = false;
  std::vector<std::string>* notes = nullptr;
};

struct Eed {
  std::int64_t app = 0;
  base::Array items;  // [code, value] pairs
};

struct HandleRef {
  std::string name;
  bool is_list = false;
  std::int64_t one = 0;
  std::vector<std::int64_t> list;
};

struct Rec {
  std::int64_t handle = 0;
  int type_num = 0;
  std::string type;
  bool is_entity = false;
  std::optional<std::string> class_name;
  Value f = base::object_with_capacity(12);
  std::vector<HandleRef> h;
  std::vector<Eed> eed;
  bool has_eed = false;
  std::size_t size = 0;
  bool crc_bad = false;

  // common entity data (20.4.1)
  std::optional<Value> color;
  std::optional<double> ltscale;
  std::optional<bool> invisible;
  std::optional<std::uint32_t> transparency;
  bool color_book = false;
  int entmode = 0;
  std::int32_t nreact = 0;
  std::uint32_t no_xdic = 0;
  std::uint32_t ds = 0;
  std::uint32_t bylayerlt = 0;
  std::uint32_t nolinks = 0;
  std::optional<std::uint32_t> ltflags;
  std::uint32_t psflags = 0;
  std::uint32_t matflags = 0;
  std::uint32_t vs_full = 0;
  std::uint32_t vs_face = 0;
  std::uint32_t vs_edge = 0;
  std::optional<std::uint32_t> lw;
  std::uint32_t r14bits = 0;
  std::optional<std::uint64_t> gfx_pos;
  double graphics = 0;

  // framing (passthrough.js and the re-decoders read these)
  Bytes buf;
  std::uint64_t data_start = 0;
  std::uint64_t data_end = 0;
  std::uint64_t endbit = 0;
  std::uint64_t obj_end = 0;
  std::uint64_t h_pos = 0;
  std::int64_t data_left = 0;
  bool unknown = false;
  std::optional<std::string> error;

  // ---- handle references
  void set_h(std::string_view name, std::int64_t v);
  void set_hl(std::string_view name, std::vector<std::int64_t> v);
  [[nodiscard]] const HandleRef* href(std::string_view name) const;
  /// The single handle (0 when absent or a list).
  [[nodiscard]] std::int64_t ref(std::string_view name) const;
  [[nodiscard]] bool has_h(std::string_view name) const { return href(name) != nullptr; }
  /// The list (empty when absent).
  [[nodiscard]] const std::vector<std::int64_t>& refs(std::string_view name) const;

  // ---- fields
  Value& set(std::string_view key, Value v) { return f.set(key, std::move(v)); }
  [[nodiscard]] const Value& get(std::string_view key) const { return f[key]; }
  [[nodiscard]] double num(std::string_view key, double def = 0) const { return f[key].num_or(def); }
};

/// The decoded records in object-map order, with lookup by handle (the JS `records` Map).
class Records {
 public:
  Rec& add(Rec r) {
    index_[r.handle] = items_.size();
    items_.push_back(std::move(r));
    return items_.back();
  }
  [[nodiscard]] const Rec* get(std::int64_t h) const {
    if (h == 0) {
      return nullptr;
    }
    auto it = index_.find(h);
    return it == index_.end() ? nullptr : &items_[it->second];
  }
  [[nodiscard]] Rec* get(std::int64_t h) {
    if (h == 0) {
      return nullptr;
    }
    auto it = index_.find(h);
    return it == index_.end() ? nullptr : &items_[it->second];
  }
  [[nodiscard]] std::vector<Rec>& all() { return items_; }
  [[nodiscard]] const std::vector<Rec>& all() const { return items_; }
  [[nodiscard]] std::size_t size() const { return items_.size(); }

 private:
  std::vector<Rec> items_;
  std::unordered_map<std::int64_t, std::size_t> index_;
};

/// 20.1 the string stream of an R2007+ object or section ending at `endbit`; nullopt when there is none.
struct StreamRange {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
};
[[nodiscard]] std::optional<StreamRange> string_stream_start(Bytes buf, std::uint64_t endbit,
                                                             std::uint64_t min_bit = 0);

/// objects-read.js decodeObject: frame and decode the object at `offset`. Throws DecodeError / ReadOverrun when the
/// object cannot be framed; a decoder that fails leaves `error` and what it read.
[[nodiscard]] Rec decode_object(Bytes buf, std::size_t offset, const Ctx& ctx);

/// header-vars.js decodeHeaderVars → {vars, error}.
struct HeaderResult {
  Value vars = Value::object();
  std::optional<std::string> error;
};
[[nodiscard]] HeaderResult decode_header_vars(Bits& r, int ver, Bits* handles);

/// table-read.js canonicalBorders (also used by the writer).
void table_canonical_borders(base::Array& cells);
/// DXF group code → value kind (objects-read.js groupType); "" when unknown.
[[nodiscard]] std::string_view group_type(int c);

/// passthrough.js captureRaw: the undecoded bits of a record, for the model's `raw`.
[[nodiscard]] std::optional<Value> capture_raw(const Rec& rec, int ver);

/// A list of n handle references (objects-read.js hrefs).
[[nodiscard]] std::vector<std::int64_t> hrefs(Bits& h, std::int64_t ref, std::int64_t n);
/// A count of items of at least `min_bits` each must fit in what is left (objects-read.js fits).
std::int64_t fits(const Bits& r, std::int64_t n, std::int64_t min_bits);

/// Hex of bytes, upper case (objects-read.js hexBytes).
[[nodiscard]] std::string hex_bytes(const std::vector<std::uint8_t>& b);

// Decoders of the other modules (same signature as objects.cpp's).
using Decoder = void (*)(Bits& r, Bits& h, Rec& rec, const Ctx& ctx);
void decode_acis_entity(Bits& r, Bits& h, Rec& rec, const Ctx& ctx);  // solid.cpp
void decode_surface(Bits& r, Bits& h, Rec& rec, const Ctx& ctx);      // solid.cpp
[[nodiscard]] Decoder table_decoder(std::string_view type);           // table.cpp (nullptr when not a table type)

}  // namespace cc::dwg
