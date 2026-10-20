// SPDX-License-Identifier: Apache-2.0
#include "cc/cad/pictures.hpp"

#include "parts.hpp"

namespace cc::cad {

base::Value picture_block(const std::string& name, const base::Value& pic, const base::Value* clip) {
  return detail::picture_block(name, pic, clip);
}

}  // namespace cc::cad
