// SPDX-License-Identifier: Apache-2.0
// Studio Live's frames (libs/live crypto) on hostile bytes: the first byte picks the receiving side, the rest is fed to
// Session::open as frames (split at 0xFF markers), after a real handshake. Never a crash; a frame that opens decodes as
// an envelope or is refused.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cc/live/crypto.hpp"
#include "cc/live/envelope.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1)
    return 0;
  const std::vector<std::uint8_t> secret{'s'};
  cc::live::JoinerHandshake j(secret, "lan:FZ");
  cc::live::HostHandshake h(secret, "lan:FZ");
  auto a = j.message();
  if (!a)
    return 0;
  auto b = h.receive(*a);
  if (!b)
    return 0;
  auto c = j.receive(*b);
  if (!c)
    return 0;
  auto fin = h.finish(*c);
  if (!fin)
    return 0;
  auto joiner = j.finish(fin->first);
  if (!joiner)
    return 0;
  cc::live::Session& to = (data[0] & 1U) != 0 ? fin->second : *joiner;
  std::vector<std::uint8_t> frame;
  for (std::size_t i = 1; i <= size; ++i) {
    if (i == size || data[i] == 0xff) {
      if (auto o = to.open(frame); o && !o->rekey)
        (void)cc::live::decode(o->body);
      frame.clear();
    } else {
      frame.push_back(data[i]);
    }
  }
  return 0;
}
