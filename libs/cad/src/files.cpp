// SPDX-License-Identifier: Apache-2.0
#include "cc/cad/files.hpp"

#include "parts.hpp"
#include "props.hpp"

namespace cc::cad {

const base::Value& dim_defaults() {
  return detail::dim_defaults();
}

base::Value clean_panes(const base::Value& v) {
  return detail::clean_panes_value(v);
}

base::Value split3d(base::Value& drawing, const i18n::Catalog& words) {
  return detail::split3d(drawing, words);
}

}  // namespace cc::cad
