// SPDX-License-Identifier: Apache-2.0
// The bounds-checked readers. Every parser reads input through these; nothing else touches raw bytes
// (AGENTS.md section 4).
//
//   ByteReader  — little-endian fields at a byte cursor; every read returns Result<T>.
//   BitReader   — MSB-first bit cursor (DWG bit streams). Reads past `end` throw ReadOverrun, which a parser
//                 catches at its record boundary and turns into an error (see docs/architecture.md, "Errors").
//   byte_at()   — checked single-byte access for code that walks a span by index.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include "cc/base/error.hpp"

namespace cc::base {

using Bytes = std::span<const std::byte>;

/// The bytes as unsigned chars — the one sanctioned view change between std::byte and unsigned char storage (both
/// may alias any object; the readers index octets as numbers).
[[nodiscard]] inline std::span<const std::uint8_t> as_u8(Bytes b) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const std::uint8_t*>(b.data()), b.size()};
}

[[nodiscard]] inline Result<std::uint8_t> byte_at(Bytes b, std::size_t i) {
  if (i >= b.size()) {
    return fail(Errc::out_of_range, "read past the end");
  }
  return std::to_integer<std::uint8_t>(b[i]);
}

class ByteReader {
 public:
  explicit ByteReader(Bytes data) noexcept : data_(data) {}

  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

  Result<void> seek(std::size_t p) {
    if (p > data_.size()) {
      return fail(Errc::out_of_range, "seek past the end");
    }
    pos_ = p;
    return {};
  }
  Result<void> skip(std::size_t n) {
    if (n > remaining()) {
      return fail(Errc::out_of_range, "skip past the end");
    }
    pos_ += n;
    return {};
  }
  Result<Bytes> bytes(std::size_t n) {
    if (n > remaining()) {
      return fail(Errc::out_of_range, "read past the end");
    }
    Bytes out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
  }
  Result<std::uint8_t> read_u8() { return read_le<std::uint8_t>(); }
  Result<std::uint16_t> read_u16_le() { return read_le<std::uint16_t>(); }
  Result<std::uint32_t> read_u32_le() { return read_le<std::uint32_t>(); }
  Result<std::uint64_t> read_u64_le() { return read_le<std::uint64_t>(); }
  Result<std::int16_t> read_i16_le() {
    return read_le<std::uint16_t>().transform([](std::uint16_t v) { return std::bit_cast<std::int16_t>(v); });
  }
  Result<std::int32_t> read_i32_le() {
    return read_le<std::uint32_t>().transform([](std::uint32_t v) { return std::bit_cast<std::int32_t>(v); });
  }
  Result<double> read_f64_le() {
    return read_le<std::uint64_t>().transform([](std::uint64_t v) { return std::bit_cast<double>(v); });
  }

 private:
  template <class T>
  Result<T> read_le() {
    if (sizeof(T) > remaining()) {
      return fail(Errc::out_of_range, "read past the end");
    }
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      v = static_cast<T>(v |
                         static_cast<T>(static_cast<T>(std::to_integer<std::uint8_t>(data_[pos_ + i])) << (8U * i)));
    }
    pos_ += sizeof(T);
    return v;
  }

  Bytes data_;
  std::size_t pos_ = 0;
};

/// Thrown by BitReader on a read past its end. Parsers catch it at a record boundary.
class ReadOverrun : public std::exception {
 public:
  ReadOverrun(std::uint64_t at, std::uint64_t end) noexcept : at_(at), end_(end) {}
  [[nodiscard]] const char* what() const noexcept override { return "read past the end"; }
  [[nodiscard]] std::uint64_t at() const noexcept { return at_; }
  [[nodiscard]] std::uint64_t end() const noexcept { return end_; }

 private:
  std::uint64_t at_;
  std::uint64_t end_;
};

/// MSB-first bit cursor over a byte span. Positions are in bits; `end` never exceeds the span.
class BitReader {
 public:
  BitReader() noexcept = default;
  explicit BitReader(Bytes data) noexcept : data_(data), end_(static_cast<std::uint64_t>(data.size()) * 8U) {}
  BitReader(Bytes data, std::uint64_t start_bit, std::uint64_t end_bit) noexcept
      : data_(data), end_(clamp_end(data, end_bit)), pos_(start_bit) {}

  [[nodiscard]] std::uint64_t pos() const noexcept { return pos_; }
  [[nodiscard]] std::uint64_t end() const noexcept { return end_; }
  [[nodiscard]] Bytes data() const noexcept { return data_; }
  /// Bits left before `end`; negative once a caller has moved `pos` past it.
  [[nodiscard]] std::int64_t bits_left() const noexcept {
    return static_cast<std::int64_t>(end_) - static_cast<std::int64_t>(pos_);
  }
  void set_pos(std::uint64_t p) noexcept { pos_ = p; }
  void set_end(std::uint64_t e) noexcept { end_ = clamp_end(data_, e); }
  void align() noexcept { pos_ = (pos_ + 7U) & ~std::uint64_t{7}; }

  [[noreturn]] void overrun(std::uint64_t n) const { throw ReadOverrun(pos_ + n, end_); }

  std::uint32_t bit() {
    if (pos_ >= end_) {
      overrun(1);
    }
    const std::uint64_t p = pos_++;
    return (byte(p >> 3U) >> (7U - (p & 7U))) & 1U;
  }
  std::uint32_t bits2() {
    if (pos_ + 2 > end_) {
      overrun(2);
    }
    const std::uint64_t p = pos_;
    pos_ += 2;
    const auto s = static_cast<unsigned>(p & 7U);
    const std::uint64_t i = p >> 3U;
    if (s < 7) {
      return (byte(i) >> (6U - s)) & 3U;
    }
    return ((byte(i) & 1U) << 1U) | (byte(i + 1) >> 7U);
  }
  /// Up to 32 bits, MSB first.
  std::uint32_t bits(unsigned n) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
      v = (v << 1U) | bit();
    }
    return v;
  }
  /// One byte at any bit position.
  std::uint32_t rc() {
    if (pos_ + 8 > end_) {
      overrun(8);
    }
    const std::uint64_t p = pos_;
    pos_ += 8;
    const std::uint64_t i = p >> 3U;
    const auto s = static_cast<unsigned>(p & 7U);
    if (s == 0) {
      return byte(i);
    }
    return ((byte(i) << s) | (byte(i + 1) >> (8U - s))) & 0xFFU;
  }
  /// n bytes at any bit position into `out` (resized).
  template <class Out>
  void raw(std::size_t n, Out& out) {
    if (static_cast<std::uint64_t>(n) * 8U > static_cast<std::uint64_t>(bits_left() < 0 ? 0 : bits_left())) {
      overrun(static_cast<std::uint64_t>(n) * 8U);
    }
    out.resize(n);
    if ((pos_ & 7U) == 0) {
      const std::uint64_t i = pos_ >> 3U;
      for (std::size_t k = 0; k < n; ++k) {
        out[k] = static_cast<Out::value_type>(byte(i + k));
      }
      pos_ += static_cast<std::uint64_t>(n) * 8U;
      return;
    }
    for (std::size_t k = 0; k < n; ++k) {
      out[k] = static_cast<Out::value_type>(rc());
    }
  }

 private:
  static std::uint64_t clamp_end(Bytes d, std::uint64_t e) noexcept {
    const std::uint64_t cap = static_cast<std::uint64_t>(d.size()) * 8U;
    return e < cap ? e : cap;
  }
  // Callers check positions against `end_`, which never exceeds the span; the hardened standard library
  // checks the index again.
  [[nodiscard]] std::uint32_t byte(std::uint64_t i) const {
    return std::to_integer<std::uint32_t>(data_[static_cast<std::size_t>(i)]);
  }

  Bytes data_;
  std::uint64_t end_ = 0;
  std::uint64_t pos_ = 0;
};

}  // namespace cc::base
