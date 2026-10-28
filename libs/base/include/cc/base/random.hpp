// SPDX-License-Identifier: Apache-2.0
// cc::base — L0. Bytes from the operating system's random generator (getrandom, arc4random_buf, BCryptGenRandom,
// getentropy on the web), for tokens, session ids and file ids. Never a seeded generator.
#pragma once

#include <cstdint>
#include <span>

namespace cc::base {

/// Fills `out` from the OS. Blocks only until the OS generator is ready (once, at boot).
void os_random(std::span<std::uint8_t> out) noexcept;

}  // namespace cc::base
