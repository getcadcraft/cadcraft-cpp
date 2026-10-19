// SPDX-License-Identifier: Apache-2.0
// linetypes.js (CadCraft 0.5): the linetype names a drawing can use and the words for linetypes and lineweights —
// LINETYPE, LWEIGHT and the Properties panel share them.
#include <algorithm>
#include <string>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cmd/props.hpp"

namespace cc::cmd {

using base::Value;

/// linetypes.js linetypeNames(doc): Continuous, the drawing's, the standard ones
std::vector<std::string> linetype_names(const doc::Doc& d) {
  std::vector<std::string> out{"Continuous"};
  const auto add = [&out](const std::string& n) {
    if (std::ranges::none_of(out,
                             [&n](const std::string& x) { return base::js_to_upper(x) == base::js_to_upper(n); })) {
      out.push_back(n);
    }
  };
  d.linetypes.each([&](const std::string& n, const Value&) { add(n); });
  for (const char* b : {"DASHED", "HIDDEN", "CENTER", "PHANTOM", "DOT", "DASHDOT", "BORDER", "DIVIDE"}) {
    for (const char* suffix : {"", "2", "X2"}) {
      const std::string n = std::string(b) + suffix;
      if (std::ranges::none_of(out, [&n](const std::string& x) { return base::js_to_upper(x) == n; })) {
        out.push_back(n);
      }
    }
  }
  return out;
}
/// linetypes.js ltLabel(name)
std::string lt_label(const i18n::Catalog& w, const Value& n) {
  if (n.is_undefined() || n.is_null() || (n.is_string() && base::js_to_lower(n.as_string()) == "bylayer")) {
    return w.t("lt.bylayer");
  }
  if (n.is_string() && base::js_to_lower(n.as_string()) == "byblock") {
    return w.t("lt.byblock");
  }
  if (!n.truthy() || (n.is_string() && base::js_to_lower(n.as_string()) == "continuous")) {
    return w.t("lt.continuous");
  }
  return base::js_string(n);
}
/// linetypes.js lwLabel(mm)
std::string lw_label(const i18n::Catalog& words, const Value& w) {
  if (w.is_undefined() || w.is_null() || w == Value(-1)) {
    return words.t("lt.bylayer");
  }
  if (w == Value(-2)) {
    return words.t("lt.byblock");
  }
  if (w == Value(-3)) {
    return words.t("lw.default");
  }
  return words.t("lw.mm", {{"mm", base::js_to_fixed(base::js_to_number(w), 2)}});
}

}  // namespace cc::cmd
