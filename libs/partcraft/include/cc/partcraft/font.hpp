// SPDX-License-Identifier: Apache-2.0
// cc::partcraft::font — PartCraft 0.5's font.js: Helvetica advance widths (1/1000 em) for its drawing PDFs, which use
// the reader's built-in Helvetica (WinAnsi), so the widths centre and fit the text.
#pragma once

#include <cstdint>
#include <string_view>

namespace cc::partcraft::font {

/// charAdvance(ch): one code point's width in 1/1000 em (a close Windows-1252 stand-in for ⌀ − ′ ″ and tab; an accented
/// letter as its base letter; CJK and the like 1000; anything else 556).
[[nodiscard]] double char_advance(std::uint32_t cp);
/// advance(s): a UTF-8 string's width in 1/1000 em (a lone surrogate counts as one character).
[[nodiscard]] double advance(std::string_view s);

}  // namespace cc::partcraft::font
