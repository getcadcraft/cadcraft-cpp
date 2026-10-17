// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg bits-write.js — the DWG bit and byte writers (ODA spec v5.4.1 ch. 2: B BB 3B BS BL BLL BD DD BT BE RC
// RS RL RD MC MS H T TU TV CMC ENC OT), the drawing code page text (ANSI_1252 with \U+XXXX) and the text clean-up.
//
// Numbers arrive as JS numbers (double) and are coerced as the JS operators do (ToInt32 for `&`, `>>`, `|`, ToUint32
// for `>>>`), so a value the model carries out of range is written as the 0.5 writer writes it.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/value.hpp"

namespace cc::dwg::w {

using base::Value;
using ByteBuf = std::vector<std::uint8_t>;

/// 'AC1012' … 'AC1032' → its place in bits-write.js ORDER (-1 when unknown).
[[nodiscard]] int v_index(std::string_view v) noexcept;
[[nodiscard]] inline bool at_least(std::string_view v, std::string_view min) noexcept {
  return v_index(v) >= v_index(min);
}

/// The characters encode1252 could not keep (it writes "?" for them): one count per write, read for the report.
struct TextLoss {
  double lost = 0;
};
/// The counter of the write in progress on this thread (writeDwg resets it, as the JS resets encode1252.lost).
TextLoss& text_loss() noexcept;

/// cleanText: NUL removed, other control characters (but TAB) and DEL become spaces. On UTF-8.
[[nodiscard]] std::string clean_text(std::string_view s);
/// String(s ?? '') then cleanText.
[[nodiscard]] std::string clean_text_of(const Value& s);
/// encode1252: ASCII and U+00A0–U+00FF as single bytes, other BMP characters as \U+XXXX, the rest "?" (counted).
[[nodiscard]] ByteBuf encode1252(std::string_view utf8);

struct NormColor {
  bool rgb_set = false;
  std::uint32_t rgb = 0;
  Value index;  // a number, or undefined (an RGB colour without one)
  Value name, book;
};
/// normColor: { index } | { rgb, index?, name?, book? } | number | null.
[[nodiscard]] NormColor norm_color(const Value& c);

/// handleBytes: the value's bytes, most significant first, none for 0.
[[nodiscard]] std::vector<std::uint8_t> handle_bytes(const Value& v);
[[nodiscard]] std::vector<std::uint8_t> handle_bytes(double v);

class BitWriter {
 public:
  explicit BitWriter(std::string_view version = "AC1015", std::size_t size = 256);

  std::string version;
  bool r2004 = false, r2007 = false, r2010 = false, r2013 = false, r2018 = false;
  ByteBuf buf;
  std::uint64_t bit = 0;
  /// R2007+ object writers redirect TV to their string stream (objects-write.js ObjWriter sets this).
  BitWriter* str = nullptr;
  bool str_used = false;

  void B(bool v);
  void B(double v) { B(v != 0 && v == v); }
  void bits(double v, int n);
  void BB(double v) { bits(static_cast<double>(base::js_to_int32(v) & 3), 2); }
  void B3(double v);
  void RC(double v);
  /// RC of a byte (no JS coercion)
  void RC8(std::uint8_t v);
  void RS(double v);
  void RL(double v);
  void RD(double v);
  void BS(double v);
  void BL(double v);
  void BLL(double v);
  void BD(double v);
  void DD(double v, double def);
  void BT(double v);
  void BE(double x = 0, double y = 0, double z = 1);
  void RD2(double x, double y) {
    RD(x);
    RD(y);
  }
  void RD3(double x, double y, double z) {
    RD(x);
    RD(y);
    RD(z);
  }
  void BD2(double x, double y) {
    BD(x);
    BD(y);
  }
  void BD3(double x, double y, double z) {
    BD(x);
    BD(y);
    BD(z);
  }
  /// P3(p): p?.x || 0 … (NaN, -0 and missing become 0).
  void P3(const Value& p);
  void P2(const Value& p);
  void CMC(const Value& color);
  void ENC(const Value& color, const Value& transparency);
  void OT(double t);
  void H(double code, const Value& value);
  void H(double code, double value = 0);
  void T8(const Value& s);
  void TU(const Value& s);
  /// TU of raw UTF-16 units (cleanText applied to the units).
  void TU16(const std::vector<std::uint16_t>& u);
  /// TV of raw UTF-16 units (R2007+ only: into the string stream when one is set).
  void TV16(const std::vector<std::uint16_t>& u);
  /// TV: T8 up to R2004, TU from R2007 — into the string stream when one is set (ObjWriter).
  void TV(const Value& s);
  void TV(std::string_view s) { TV(Value(s)); }
  /// Copy another writer's first n bits (all by default).
  void append(const BitWriter& w);
  void append(const BitWriter& w, std::uint64_t n);
  void patch_RL(std::uint64_t at, double v);
  /// Pad to the byte boundary; the bytes written so far.
  [[nodiscard]] ByteBuf bytes() const;

 private:
  void grow(std::uint64_t n);
  void T8_bytes(const ByteBuf& b);
};

/// "|0" and ">>>0" of a JS value (ToNumber first).
[[nodiscard]] inline std::int32_t to_i32(const Value& v) {
  return base::js_to_int32(base::js_to_number(v));
}

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t size = 1U << 16U) { buf.reserve(size); }
  ByteBuf buf;
  [[nodiscard]] std::size_t pos() const noexcept { return buf.size(); }
  void raw(std::span<const std::uint8_t> a) { buf.insert(buf.end(), a.begin(), a.end()); }
  void raw(std::initializer_list<std::uint8_t> a) { buf.insert(buf.end(), a.begin(), a.end()); }
  void zeros(std::size_t n) { buf.resize(buf.size() + n, 0); }
  void RC(double v) { buf.push_back(static_cast<std::uint8_t>(base::js_to_int32(v) & 0xFF)); }
  void RS(double v);
  void RL(double v);
  void RLL(double v);
  void RLL(std::uint64_t v);
  void RD(double v);
  void set_RL(std::size_t at, double v);
  void MC(double v);
  void UMC(double v);
  void MS(double v);
  [[nodiscard]] const ByteBuf& bytes() const noexcept { return buf; }
};

/// base64 → bytes (Node's Buffer.from(s, 'base64'): characters outside the alphabet are skipped).
[[nodiscard]] ByteBuf unb64(std::string_view s);
/// sliceBits: bits [from, to) of buf as bytes from bit 0.
[[nodiscard]] ByteBuf slice_bits(std::span<const std::uint8_t> buf, std::uint64_t from, std::uint64_t to);
/// appendBits: n bits of u8 (from bit 0) into the writer.
void append_bits(BitWriter& w, std::span<const std::uint8_t> u8, std::uint64_t n);

[[nodiscard]] std::vector<std::uint8_t> mc_bytes(double v);
[[nodiscard]] std::vector<std::uint8_t> umc_bytes(double v);

}  // namespace cc::dwg::w
