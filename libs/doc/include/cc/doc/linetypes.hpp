// SPDX-License-Identifier: Apache-2.0
// cc::doc — linetypes.js patternOf: the dash pattern a drawing's linetype draws with.
#pragma once

#include <string>

#include "cc/base/value.hpp"
#include "cc/doc/doc.hpp"

namespace cc::doc {

/// linetypes.js patternOf(doc, name): the dash pattern of a linetype in drawing units (the drawing's own, else one of
/// the standard ones, metric or inch by the drawing), or null when it draws continuous.
[[nodiscard]] base::Value pattern_of(const Doc& doc, const std::string& name);

}  // namespace cc::doc
