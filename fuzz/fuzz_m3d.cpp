// SPDX-License-Identifier: Apache-2.0
// The ModelCraft kernel on hostile input: any bytes as a native .modelcraft file (Model::load, the untrusted reader),
// then the model written back and its first definitions edited; or, when the JSON is a script ({steps: […]}), up to 40
// kernel edits as the parity scripts make them (tools/parity/m3d.mjs). Never a crash, a hang or a sanitizer report.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/model.hpp"
#include "cc/m3d/script.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 18)) {
    return 0;
  }
  const std::string_view text(reinterpret_cast<const char*>(data), size);  // NOLINT
  cc::base::JsonLimits lim;
  lim.max_depth = 64;
  const auto j = cc::base::parse_json(text, lim);
  if (!j || !j->is_object()) {
    return 0;
  }
  if (const cc::base::Value* steps = j->find("steps"); steps != nullptr && steps->is_array()) {
    if (steps->as_array().size() > 40) {
      return 0;
    }
    (void)cc::m3d::run_script(*j, 4000);  // edits that double the model 40 times are a timeout, not a bug
    return 0;
  }
  cc::m3d::Model m;
  if (!m.load(*j)) {
    return 0;
  }
  (void)cc::base::to_json(m.to_json(false));
  for (const std::string& id : m.def_ids()) {
    cc::m3d::Def& d = *m.def_mut(id);
    (void)cc::m3d::solid_info(d);
    (void)cc::m3d::solid_problems(d);
    (void)cc::m3d::cleanup(d);
  }
  (void)m.change("x", [](cc::m3d::Model& mm) { mm.purge(); });
  (void)m.undo();
  return 0;
}
