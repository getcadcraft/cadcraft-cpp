// SPDX-License-Identifier: Apache-2.0
// Script ops for drive.js (tools/parity/civil/ops-civil.mjs has their JS twins).
#include "cc/civil/drive.hpp"
#include "cc/civil/script.hpp"
#include "cc/civil/tin.hpp"

namespace cc::civil {

void add_drive_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["drive.speeds"] = [](const base::Array& a, ScriptCtx&) -> base::Value {
    return drive_speeds(arg(a, 0).is_string() && arg(a, 0).as_string() == "m");
  };
  m["drive.eye"] = [](const base::Array& a, ScriptCtx&) -> base::Value {
    return eye_height(arg(a, 0).is_string() && arg(a, 0).as_string() == "m");
  };
  m["drive.sun"] = [](const base::Array& a, ScriptCtx&) -> base::Value {
    return sun_position(num(arg(a, 0)), num(arg(a, 1)), num(arg(a, 2)));
  };
  m["drive.sunVector"] = [](const base::Array& a, ScriptCtx&) -> base::Value {
    return sun_vector(num(arg(a, 0)), num(arg(a, 1)));
  };
  m["drive.frames"] = [](const base::Array& a, ScriptCtx& c) -> base::Value {
    const auto al = reg_get<Alignment>(c, str(arg(a, 0)));
    if (!al) {
      throw ScriptError{"Cannot read properties of undefined (reading 'start')"};
    }
    const base::Value cfg = arg(a, 1).truthy() ? arg(a, 1) : base::Value::object();
    const auto prof = field(cfg, "profile").truthy() ? reg_get<Profile>(c, str(field(cfg, "profile"))) : nullptr;
    const auto surf = field(cfg, "ground").truthy() ? reg_get<Surface>(c, str(field(cfg, "ground"))) : nullptr;
    std::function<std::optional<double>(double, double)> ground;
    if (surf) {
      ground = [surf](double x, double y) { return surf->elevation_at(x, y); };
    }
    ElevationAlong elev(*al, prof.get(), ground);
    base::Array out;
    for (const base::Value& sta : arr(arg(a, 2))) {
      out.push_back(drive_frame(*al, elev, num(sta), nullish(arg(a, 3)) ? base::Value() : arg(a, 3)));
    }
    return out;
  };
}

}  // namespace cc::civil
