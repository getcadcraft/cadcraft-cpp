// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.8a: the drawing window's settings from the command line — POLARANG,
// LINKVIEWS, OPENIN3D, LASSOMODE, LWDISPLAY — and HELP. A host without settings (a script) says what it cannot do,
// as 0.5 does without a browser.
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
std::string key_of(const Answer& a) {
  if (a.kind == Answer::Kind::option) {
    return a.option;
  }
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}

Task<void> polarang_cmd(Ctx& c) {
  base::Value* st = c.host.settings();
  if (st == nullptr) {
    c.print(c.t("cmd.polarang.browser"));
    co_return;
  }
  Ask an;
  const base::Value cur = st->get("polarInc");
  an.def = cur.is_undefined() || cur.is_null() ? base::Value(90) : cur;
  const Answer a = co_await c.number(c.t("cmd.polarang.ask"), std::move(an));
  const double v = a.kind == Answer::Kind::number ? a.number : base::js_to_number(a.typed);
  const bool usable = v > 0 && v <= 180;
  if (!usable) {
    c.print(c.t("cmd.polarang.bad"));
    co_return;
  }
  st = c.host.settings();  // looked up again after the prompt
  if (st == nullptr) {
    co_return;
  }
  st->set("polarInc", v);
  c.host.save_settings();
  c.print(c.t("cmd.polarang.set", {{"v", fmt(v)}}));
}

Task<void> linkviews_cmd(Ctx& c) {
  const std::optional<bool> on = c.host.link_views(std::nullopt);
  if (!on) {
    c.print(c.t("cmd.vports.browser"));
    co_return;
  }
  Ask ak;
  ak.options = {c.opt("ON", "ON"), c.opt("OFF", "OFF")};
  ak.def = *on ? "OFF" : "ON";
  const std::string k = base::js_to_upper(key_of(co_await c.keyword(c.t("cmd.linkviews.ask"), std::move(ak))));
  if (k != "ON" && k != "OFF") {
    co_return;
  }
  (void)c.host.link_views(k == "ON");
  c.print(c.t(k == "ON" ? "cmd.linkviews.on" : "cmd.linkviews.off"));
}

Task<void> openin3d_cmd(Ctx& c) {
  c.print(c.t("cmd.open3d.none"));  // the handoff to the 3D modeling app is the browser's (handoff.js)
  co_return;
}

Task<void> lassomode_cmd(Ctx& c) {
  const base::Value* st = c.host.settings();
  if (st == nullptr || !c.host.can_set_lasso()) {
    c.print(c.t("cmd.lassomode.browser"));
    co_return;
  }
  Ask an;
  an.def = st->get("lasso") == base::Value("box") ? 0 : 1;
  const Answer a = co_await c.number(c.t("cmd.lassomode.ask"), std::move(an));
  const double n = a.kind == Answer::Kind::number ? a.number : base::js_to_number(a.typed);
  if (n == 0 || n == 1) {
    c.host.set_lasso(n == 1);
  } else {
    c.print(c.t("cmd.lassomode.range"));
  }
}

Task<void> lwdisplay_cmd(Ctx& c) {
  const base::Value* st = c.host.settings();
  if (st == nullptr || !c.host.can_set_toggle()) {
    c.print(c.t("cmd.lwdisplay.browser"));
    co_return;
  }
  Ask ak;
  ak.options = {c.opt("ON", "ON"), c.opt("OFF", "OFF")};
  ak.def = st->get("lwt").truthy() ? "ON" : "OFF";
  const std::string k = key_of(co_await c.keyword(c.t("cmd.lwdisplay.ask"), std::move(ak)));
  if (k == "ON" || k == "OFF") {
    c.host.set_toggle("lwt", k == "ON");
  }
}

Task<void> help_cmd(Ctx& c) {
  for (const CommandDef* d : Engine::commands()) {
    std::string line = d->name;
    if (!d->aliases.empty()) {
      line += " (";
      for (std::size_t i = 0; i < d->aliases.size(); ++i) {
        line += (i ? ", " : "") + d->aliases[i];
      }
      line += ")";
    }
    c.print(line + " \xE2\x80\x94 " + (d->help.empty() ? std::string() : c.t(d->help)));
  }
  co_return;
}

CommandDef keep(CommandDef d) {
  d.keep_selection = true;
  return d;
}

}  // namespace

/// POLARANG, LINKVIEWS, OPENIN3D, LASSOMODE, LWDISPLAY: between UNITS-era settings, in commands.js's order
void add_settings(std::vector<CommandDef>& t) {
  t.push_back(keep(def("POLARANG", {}, polarang_cmd, "cmd.polarang.help", true)));
  t.push_back(keep(def("LINKVIEWS", {"SYNCVIEWS"}, linkviews_cmd, "cmd.linkviews.help", true)));
  t.push_back(keep(def("OPENIN3D", {}, openin3d_cmd, "cmd.open3d.help", true)));
  t.push_back(keep(def("LASSOMODE", {}, lassomode_cmd, "cmd.lassomode.help", true)));
  t.push_back(keep(def("LWDISPLAY", {"LWT"}, lwdisplay_cmd, "cmd.lwdisplay.help", true)));
}

void add_help(std::vector<CommandDef>& t) {
  t.push_back(keep(def("HELP", {"?"}, help_cmd, "cmd.help.list", true)));
}

}  // namespace cc::cmd::detail
