// SPDX-License-Identifier: Apache-2.0
// Port of cc-dwg bits-read.js — the DWG bit stream reader (ODA spec v5.4.1 ch. 2: B BB 3B BS BL BLL BD DD RC RS RL
// RD MC MS H T TU TV CMC ENC BE BT OT; 2.12 object type; 2.13 handle references; 20.1 the R2007+ string stream).
//
// Every read is bounds-checked by cc::base::BitReader. A read past the end throws base::ReadOverrun; an impossible
// code throws DecodeError. Both are caught at the record boundary (objects.cpp), as the JS DwgReadError is.
#pragma once

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"

namespace cc::dwg {

using base::Value;
using Bytes = std::span<const std::uint8_t>;
using U16 = std::vector<std::uint16_t>;

/// The JS DwgReadError: an impossible code or a damaged structure.
class DecodeError : public std::exception {
 public:
  explicit DecodeError(std::string m) : m_(std::move(m)) {}
  [[nodiscard]] const char* what() const noexcept override { return m_.c_str(); }

 private:
  std::string m_;
};

/// The text of a caught exception as the JS reader words it.
[[nodiscard]] std::string error_text(const std::exception& e);

/// A text decoder for a DWGCODEPAGE number (bits-read.js codepageDecoder).
struct Codepage {
  std::string name;
  bool known = false;
  int table = 0;  // 0 = Windows-1252
};
[[nodiscard]] Codepage codepage_for(int num);
/// Code-page bytes → UTF-16 units (\U+XXXX escapes not yet decoded).
[[nodiscard]] U16 decode_bytes(const Codepage& cp, Bytes b);
/// "\U+XXXX" escapes → the character (bits-read.js unescapeU).
void unescape_u(U16& s);
[[nodiscard]] std::string utf8(const U16& s);
/// Code-page bytes → UTF-8 with escapes decoded (ctx.decode in the JS).
[[nodiscard]] std::string decode_text(const Codepage& cp, Bytes b);

/// The 32-bit colour word of CMC/ENC (bits-read.js colourFromRgbWord).
[[nodiscard]] Value colour_from_rgb_word(int index, std::uint32_t word);

struct Enc {
  Value color;
  bool has_transparency = false;
  std::uint32_t transparency = 0;
  bool has_book = false;
};

class Bits {
 public:
  Bits() = default;
  Bits(Bytes buf, int version, const Codepage* cp, std::uint64_t start_bit, std::uint64_t end_bit)
      : r_(std::as_bytes(buf), start_bit, end_bit), buf_(buf), version_(version), cp_(cp) {}
  Bits(Bytes buf, int version, const Codepage* cp) : r_(std::as_bytes(buf)), buf_(buf), version_(version), cp_(cp) {}

  [[nodiscard]] std::uint64_t pos() const noexcept { return r_.pos(); }
  [[nodiscard]] std::uint64_t end() const noexcept { return r_.end(); }
  void set_pos(std::uint64_t p) noexcept { r_.set_pos(p); }
  void set_end(std::uint64_t e) noexcept { r_.set_end(e); }
  /// end − pos as the JS computes it (may be negative).
  [[nodiscard]] std::int64_t left() const noexcept { return r_.bits_left(); }
  [[nodiscard]] Bytes buf() const noexcept { return buf_; }
  [[nodiscard]] int version() const noexcept { return version_; }
  void set_version(int v) noexcept { version_ = v; }
  [[nodiscard]] const Codepage* codepage() const noexcept { return cp_; }
  /// R2007+: the reader positioned on the string stream (TV reads from it). Non-owning.
  Bits* str = nullptr;
  /// The reference code of the last H().
  int hcode = 0;

  std::uint32_t B() { return r_.bit(); }
  std::uint32_t BB() { return r_.bits2(); }
  std::uint32_t B3();
  std::uint32_t bits(unsigned n) { return r_.bits(n); }
  std::uint32_t RC() { return r_.rc(); }
  std::int32_t RS();
  std::uint32_t RSu();
  std::uint32_t RL();
  std::int32_t RLs() { return static_cast<std::int32_t>(RL()); }
  double RLL();
  double RD();
  std::vector<std::uint8_t> bytes(std::size_t n);

  std::int32_t BS();
  std::int32_t BL();
  double BLL();
  double BD();
  double DD(double def);
  Value RD2();
  Value RD3();
  Value BD2();
  Value BD3();
  Value DD3(const Value& d);
  Value BE();
  double BT();
  std::int32_t OT();
  std::int64_t H(std::int64_t ref = 0);
  std::string T();
  std::string TU();
  /// UTF-16 units of a TU (no escape decoding), for callers that need the raw form.
  U16 TU16();
  std::string TV();
  Value CMC();
  Enc ENC();

 private:
  base::BitReader r_;
  Bytes buf_;
  int version_ = 15;
  const Codepage* cp_ = nullptr;
};

/// Byte-level modular numbers (2.6, 2.7). `pos` advances; reads past `b` throw DecodeError.
[[nodiscard]] std::int64_t read_mc(Bytes b, std::size_t& pos);
[[nodiscard]] std::uint64_t read_mcu(Bytes b, std::size_t& pos);
[[nodiscard]] std::uint64_t read_ms(Bytes b, std::size_t& pos);

}  // namespace cc::dwg
