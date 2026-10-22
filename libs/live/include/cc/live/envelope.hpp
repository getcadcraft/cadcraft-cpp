// SPDX-License-Identifier: Apache-2.0
// cc::live — L2. The envelope every Studio Live message is (docs/LIVE-PROTOCOL.md § 2):
// { t: type, s: session id, c: client id, n: client counter, q: seq, b: body }.
// Written in the order t s c n q b, leaving out zeros, empty strings and nil; read in any order, an absent field as
// zero / empty / nil, unknown fields ignored. `t` is required.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "cc/base/error.hpp"
#include "cc/live/msgpack.hpp"

namespace cc::live {

struct Envelope {
  std::string t;
  std::string s;
  std::uint64_t c = 0;
  std::uint64_t n = 0;
  std::uint64_t q = 0;
  Mp b;
  friend bool operator==(const Envelope&, const Envelope&) = default;
};

[[nodiscard]] Mp to_mp(const Envelope& e);
/// A map with a string `t` (other fields of the wrong type read as absent).
[[nodiscard]] base::Result<Envelope> envelope_from(const Mp& v);
[[nodiscard]] MpBytes encode(const Envelope& e);
[[nodiscard]] base::Result<Envelope> decode(std::span<const std::uint8_t> bytes);

}  // namespace cc::live
