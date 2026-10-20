// SPDX-License-Identifier: Apache-2.0
// cc::cad — the picture block (pictures.js pictureBlock, as dwg-import.js makes it since 0.4 C6): the frame (a closed
// polyline carrying `pic`: the clip in block coordinates, or the picture's rectangle) and the file's name as text.
#pragma once

#include <string>

#include "cc/base/value.hpp"

namespace cc::cad {

/// pictureBlock(name, pic, clip): `clip` an array of {x, y} (three or more: the frame is the clip), or nullptr.
[[nodiscard]] base::Value picture_block(const std::string& name, const base::Value& pic, const base::Value* clip);

}  // namespace cc::cad
