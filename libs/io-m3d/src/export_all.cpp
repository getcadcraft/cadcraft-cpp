// SPDX-License-Identifier: Apache-2.0
// Every export of a model script's model, for the parity harness and the golden test.
#include <span>
#include <string>

#include "cc/base/value.hpp"
#include "cc/m3d/io.hpp"
#include "cc/m3d/model.hpp"
#include "cc/m3d/script.hpp"

namespace cc::m3d::io {

using base::Value;

namespace {

std::string b64(const std::vector<std::uint8_t>& b) {
  return base::base64(std::span<const std::uint8_t>(b));
}

}  // namespace

Value export_all(const Value& script) {
  Value out = base::make_object({{"name", script.get("name")}});
  const Value run = run_script(script);
  const Value& steps = run["steps"];
  if (!steps.is_array() || steps.as_array().empty()) {
    out.set("err", "no steps");
    return out;
  }
  Model m;
  if (auto r = m.load(steps.as_array().back()["model"]); !r) {
    out.set("err", r.error().message);
    return out;
  }
  out.set("glb", b64(export_glb(m)));
  out.set("stl", b64(export_stl(m)));
  out.set("obj", export_obj(m));
  out.set("objMtl", export_obj(m, "model.mtl"));
  out.set("mtl", export_mtl(m));
  if (auto d = export_dxf(m)) {
    out.set("dxf", d->text);
  } else {
    out.set("dxfErr", d.error().message);
  }
  if (auto d = to_dwg_model(m)) {
    out.set("dwg", d->dwg);
  } else {
    out.set("dwgErr", d.error().message);
  }
  out.set("dae", export_dae(m, "Parity model", "2026-10-16T00:00:00Z"));
  out.set("ifc", export_ifc(m, "Parity model", "2026-10-16T00:00:00"));
  return out;
}

}  // namespace cc::m3d::io
