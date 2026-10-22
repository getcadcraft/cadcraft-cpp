// SPDX-License-Identifier: Apache-2.0
#include "cc/live/envelope.hpp"

#include <utility>

namespace cc::live {

Mp to_mp(const Envelope& e) {
  Map m;
  m.set("t", Mp(e.t));
  if (!e.s.empty()) {
    m.set("s", Mp(e.s));
  }
  if (e.c != 0) {
    m.set("c", Mp(e.c));
  }
  if (e.n != 0) {
    m.set("n", Mp(e.n));
  }
  if (e.q != 0) {
    m.set("q", Mp(e.q));
  }
  if (!e.b.is_nil()) {
    m.set("b", e.b);
  }
  return {std::move(m)};
}

base::Result<Envelope> envelope_from(const Mp& v) {
  const Mp* t = v.get("t");
  if (t == nullptr || !t->is_string()) {
    return base::fail(base::Errc::invalid, "live: an envelope needs a type");
  }
  Envelope e;
  e.t = t->as_string();
  if (const Mp* s = v.get("s"); s != nullptr && s->is_string()) {
    e.s = s->as_string();
  }
  const auto num = [&v](const char* k, std::uint64_t& out) {
    if (const Mp* x = v.get(k); x != nullptr && !x->to_uint(out)) {
      out = 0;
    }
  };
  num("c", e.c);
  num("n", e.n);
  num("q", e.q);
  if (const Mp* b = v.get("b")) {
    e.b = *b;
  }
  return e;
}

MpBytes encode(const Envelope& e) {
  return pack(to_mp(e));
}

base::Result<Envelope> decode(std::span<const std::uint8_t> bytes) {
  auto v = unpack(bytes);
  if (!v) {
    return std::unexpected(std::move(v.error()));
  }
  return envelope_from(*v);
}

}  // namespace cc::live
