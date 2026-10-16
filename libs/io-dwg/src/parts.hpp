// SPDX-License-Identifier: Apache-2.0
// What read.js passes to the per-type model builders (table-read.js, mleader-read.js, solid-read.js, …): a resolver
// over the decoded records, and the builders' entry points.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#pragma once

#include "record.hpp"

namespace cc::dwg {

/// read.js `get`, `nameOf`, `appName`, `hx`.
struct Resolver {
  const Records* records = nullptr;
  [[nodiscard]] const Rec* get(std::int64_t h) const { return records->get(h); }
  /// The record's `name` field, or nullopt when there is no record or it has no name.
  [[nodiscard]] std::optional<Value> name_of(std::int64_t h) const {
    const Rec* r = get(h);
    if (r == nullptr) {
      return std::nullopt;
    }
    const Value* n = r->f.find("name");
    if (n == nullptr || n->is_undefined()) {
      return std::nullopt;
    }
    return *n;
  }
  /// nameOf(h) ?? hx(h)
  [[nodiscard]] Value name_or_hex(std::int64_t h) const {
    auto n = name_of(h);
    return n ? *n : Value(handle_hex(h));
  }
  /// The application name of an EED block (its APPID's name, else the handle in hex).
  [[nodiscard]] std::string app_name(std::int64_t h) const {
    auto n = name_of(h);
    if (n && n->is_string()) {
      return n->as_string();
    }
    if (n) {
      std::string s;
      base::write_json(*n, s);
      return s;
    }
    return handle_hex(h);
  }
};

// solid.cpp
void attach_data_storage(Records& records, const Bytes* sec, std::vector<std::string>& notes);
void solid_fields(const Rec& rec, Value& f, const Resolver& res);
void surface_fields(const Rec& rec, Value& f, const Resolver& res);
/// bits-util.js sliceBits.
[[nodiscard]] std::vector<std::uint8_t> slice_bits_of(Bytes buf, std::uint64_t from, std::uint64_t to);

// table.cpp (table-read.js)
void table_entity_fields(const Rec& rec, Value& f, const Resolver& res);
void table_object_fields(const Rec& rec, Value& f, const Resolver& res);

// mleader.cpp (mleader-read.js): the model fields, or nullopt when the record cannot be decoded again.
[[nodiscard]] std::optional<Value> read_mleader(const Rec& rec, const Ctx& ctx, const Resolver& res);
[[nodiscard]] std::optional<Value> read_mleader_style(const Rec& rec, const Ctx& ctx, const Resolver& res);

// dynblock.cpp (dynblock-read.js): adds dynamic block data to the finished model.
void read_dynamic(Value& model);

}  // namespace cc::dwg
