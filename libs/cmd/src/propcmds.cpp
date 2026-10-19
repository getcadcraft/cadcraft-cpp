// SPDX-License-Identifier: Apache-2.0
// propcmds.js (CadCraft 0.5): the Properties panel's commands. The panel is the host's (Host::props_*):
//   PROPERTIES (PR, PROPS, CH, MO, DDMODIFY)  open the Properties panel
//   PROPERTIESCLOSE (PRCLOSE)                 fold it away
//   QPMODE (QP)                               Quick Properties by the cursor on / off
//   _PROPPICK                                 hidden: the pick button beside a point — asks for the point, with snaps
// None of them clears the selection.
#include <string>
#include <utility>
#include <vector>

#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

Task<void> properties_cmd(Ctx& c) {
  c.host.props_open();
  co_return;
}
Task<void> propertiesclose_cmd(Ctx& c) {
  c.host.props_close();
  co_return;
}
Task<void> qpmode_cmd(Ctx& c) {
  c.host.props_toggle_quick();
  co_return;
}
Task<void> proppick_cmd(Ctx& c) {
  const std::string what = c.host.props_pick_label();
  const Answer p = co_await c.point(c.t("props.pick.prompt", {{"what", what}}));
  if (p.kind == Answer::Kind::point) {
    c.host.props_picked(p.p);
  }
}

}  // namespace

void add_props(std::vector<CommandDef>& t) {
  const auto keep = [&t](CommandDef d, bool hidden = false) {
    d.keep_selection = true;
    d.hidden = hidden;
    t.push_back(std::move(d));
  };
  keep(def("PROPERTIES", {"PR", "PROPS", "CH", "MO", "DDMODIFY"}, properties_cmd, "cmd.properties.help", true));
  keep(def("PROPERTIESCLOSE", {"PRCLOSE"}, propertiesclose_cmd, "cmd.propertiesclose.help", true));
  keep(def("QPMODE", {"QP"}, qpmode_cmd, "cmd.qpmode.help", true));
  keep(def("_PROPPICK", {}, proppick_cmd, "", true), true);
}

}  // namespace cc::cmd::detail
