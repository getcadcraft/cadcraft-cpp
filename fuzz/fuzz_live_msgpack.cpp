// SPDX-License-Identifier: Apache-2.0
// Studio Live's MessagePack reader and the session core on hostile frames: whatever decodes must write back to bytes
// that decode to the same value and write the same bytes again (the writer's form is a fixed point), and an envelope
// read from it must be decided by the session core without trouble.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "cc/live/envelope.hpp"
#include "cc/live/msgpack.hpp"
#include "cc/live/session_core.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);
  const auto v = cc::live::unpack(in);
  if (!v)
    return 0;
  const auto once = cc::live::pack(*v);
  const auto again = cc::live::unpack(once);
  if (!again || cc::live::pack(*again) != once)
    std::abort();  // (NaN != NaN, so bytes, not values)
  if (const auto env = cc::live::envelope_from(*v)) {
    cc::live::SessionCore core;
    core.join(1, cc::live::Role::host);
    core.join(2, cc::live::Role::editor);
    const auto touched = cc::live::touched_from(env->b.get("touched"));
    (void)core.edit(env->c % 3, touched);
    (void)core.edit(2, touched);
    (void)cc::live::commit_touched(touched);
  }
  return 0;
}
