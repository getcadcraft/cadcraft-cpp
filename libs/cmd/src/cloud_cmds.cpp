// SPDX-License-Identifier: Apache-2.0
// cloud-cad.js's commands (CadCraft 0.5 SG.1): POINTCLOUDATTACH (PCATTACH) picks a LAS / LAZ / XYZ / PTS file and
// where it goes (Enter: its own coordinates); POINTCLOUD (PC) sets the attached cloud's slice, its height and
// thickness, colours, point size and visibility, zooms to it or detaches it. The cloud lives in the host (Host::cloud):
// not part of the drawing, not undone.
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/cloud/attach.hpp"
#include "cc/cloud/cloud.hpp"
#include "cc/m3d/drawing3d.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

/// commandRegistry.option: the label as given (cloud-cad.js's options are English)
Option opt(std::string key, std::string label) {
  return {.key = std::move(key), .label = std::move(label), .en = {}, .hidden = false};
}

/// toLocaleString('en-US') of a whole number
std::string grouped(double v) {
  std::string n;
  base::append_number(n, std::abs(v));
  std::string out = v < 0 ? "-" : "";
  for (std::size_t i = 0; i < n.size(); ++i) {
    if (i > 0 && (n.size() - i) % 3 == 0) {
      out += ',';
    }
    out += n[i];
  }
  return out;
}
/// String(Math.round(v * 1000) / 1000)
std::string fmt(double v) {
  std::string s;
  base::append_number(s, base::js_round(v * 1000) / 1000);
  return s;
}

cloud::DrawingUnits units_of(const Ctx& c) {
  const base::Value u = c.doc.meta.get("units");
  const std::string units = u.truthy() ? base::js_string(u) : std::string();
  return {.units = units, .m_per_unit = m3d::m_per_unit(units)};
}

/// The colour modes and their keys (COLOR_KEYS) and option labels
struct ColorKey {
  std::string_view key, mode, label;
};
constexpr std::array<ColorKey, 5> kColorKeys{{{"F", "rgb", "File"},
                                              {"H", "height", "Height"},
                                              {"I", "intensity", "Intensity"},
                                              {"C", "class", "Class"},
                                              {"O", "single", "One"}}};

/// attachFile: read a cloud file and attach it; prints how many points show. `insert` nothing = the file's own
/// coordinates. A file that cannot be read says why (pc.failed).
Task<bool> attach_file(Ctx& c, const PickedFile& file, bool ask) {
  cloud::Attached* pc = c.host.cloud();
  if (pc == nullptr) {
    c.print(c.t("pc.nobrowser"));
    co_return false;
  }
  c.print(c.t("pc.reading", {{"file", file.name}}));
  std::vector<std::byte> bytes(file.bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(file.bytes[i]);
  }
  const auto info = cloud::read_info(file.name, bytes, c.words);
  if (!info) {
    c.print(c.t("pc.failed", {{"file", file.name}, {"why", info.error().message}}));
    co_return false;
  }
  const double t0 = c.host.now_ms();
  auto cl = cloud::load_cloud(file.name, bytes, *info, c.host.cloud_budget(), c.words);
  if (!cl) {
    c.print(c.t("pc.failed", {{"file", file.name}, {"why", cl.error().message}}));
    co_return false;
  }
  std::optional<std::array<double, 2>> insert;
  if (ask) {
    Ask a;
    a.options = {opt("F", "File coordinates")};
    a.none = true;
    const Answer p = co_await c.point(c.t("pc.prompt.insert"), std::move(a));
    if (p.kind == Answer::Kind::point) {
      insert = std::array<double, 2>{p.p.x, p.p.y};
    }
  }
  pc->attach(std::move(*cl), insert);
  if (!pc->cloud) {
    co_return false;
  }
  const cloud::Cloud& att = *pc->cloud;
  const cloud::Summary s = cloud::summary(att, c.words);
  c.print(c.t("pc.opened", {{"file", file.name},
                            {"kept", grouped(static_cast<double>(s.kept))},
                            {"read", grouped(static_cast<double>(s.read))},
                            {"s", base::js_to_fixed((c.host.now_ms() - t0) / 1000, 1)}}));
  for (const std::string& n : att.notes) {
    c.print("  " + n);
  }
  const cloud::DrawingUnits u = units_of(c);
  if (pc->slice.on) {
    c.print("  " + c.t("pc.sliced", {{"h", fmt(pc->slice_spec(u).h)}}));
  }
  if (u.units.empty() || !att.unit) {
    c.print("  " + c.t(att.unit ? "pc.units.drawing" : "pc.units.cloud"));
  }
  c.print(c.t("pc.next"));
  c.host.zoom_cloud();
  c.host.cloud_changed();
  co_return true;
}

Task<void> pointcloudattach_cmd(Ctx& c) {
  if (!c.host.can_pick_cloud()) {
    c.print(c.t("pc.nobrowser"));
    co_return;
  }
  const Answer f = co_await c.cloud_file();
  if (f.files.empty()) {
    co_return;
  }
  (void)co_await attach_file(c, f.files.front(), true);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the JS loop, option by option
Task<void> pointcloud_cmd(Ctx& c) {
  cloud::Attached* pc = c.host.cloud();
  if (pc == nullptr || !pc->has()) {
    c.print(c.t("pc.none"));
    co_return;
  }
  for (;;) {
    const cloud::DrawingUnits u = units_of(c);
    const cloud::Attached::SliceSpec s = pc->slice_spec(u);
    const cloud::Summary sum = cloud::summary(*pc->cloud, c.words);
    c.print(c.t("pc.state", {{"file", pc->cloud->name},
                             {"kept", grouped(static_cast<double>(sum.kept))},
                             {"slice", pc->slice.on ? c.t("pc.state.slice", {{"h", fmt(s.h)}, {"thick", fmt(s.thick)}})
                                                    : c.t("pc.state.all")},
                             {"color", c.t("pc.color." + pc->color)}}));
    Ask a;
    a.options = {opt("S", "Slice"), opt("H", "Height"),  opt("T", "Thickness"), opt("C", "Color"),
                 opt("SI", "Size"), opt("V", "Visible"), opt("Z", "Zoom"),      opt("D", "Detach")};
    const Answer r = co_await c.keyword(c.t("pc.prompt.option"), std::move(a));
    const std::string k = r.key();
    if (k.empty()) {
      co_return;
    }
    if (k == "S") {
      pc->slice.on = !pc->slice.on;
      pc->changed();
      c.print(c.t(pc->slice.on ? "pc.slice.on.cmd" : "pc.slice.off.cmd"));
    } else if (k == "H") {
      Ask n;
      n.def = s.h;
      n.length = true;
      const Answer v = co_await c.number(c.t("pc.prompt.height"), std::move(n));
      if (v.kind == Answer::Kind::number && std::isfinite(v.number)) {
        pc->slice.h = v.number;
        pc->slice.on = true;
        pc->changed();
      }
    } else if (k == "T") {
      Ask n;
      n.def = s.thick;
      n.length = true;
      const Answer v = co_await c.number(c.t("pc.prompt.thick"), std::move(n));
      if (v.kind == Answer::Kind::number && std::isfinite(v.number) && v.number > 0) {
        pc->slice.thick = v.number;
        pc->changed();
      } else {
        c.print(c.t("pc.thick.bad"));
      }
    } else if (k == "C") {
      const std::vector<std::string> modes = cloud::color_modes(*pc->cloud);
      Ask ca;
      for (const ColorKey& ck : kColorKeys) {
        if (std::ranges::find(modes, ck.mode) != modes.end()) {
          ca.options.push_back(opt(std::string(ck.key), std::string(ck.label)));
        }
      }
      for (const ColorKey& ck : kColorKeys) {
        if (ck.mode == pc->color) {
          ca.def = std::string(ck.key);
        }
      }
      const Answer cr = co_await c.keyword(c.t("pc.prompt.color"), std::move(ca));
      const std::string ck = cr.key();
      for (const ColorKey& x : kColorKeys) {
        if (x.key == ck) {
          pc->color = std::string(x.mode);
          pc->recolor({120, 120, 120});
          pc->changed();
        }
      }
    } else if (k == "SI") {
      Ask n;
      n.def = static_cast<double>(pc->size);
      const Answer v = co_await c.number(c.t("pc.prompt.size"), std::move(n));
      if (v.kind == Answer::Kind::number && v.number >= 1 && v.number <= 8) {
        pc->size = static_cast<int>(base::js_round(v.number));
        pc->changed();
      } else {
        c.print(c.t("pc.size.bad"));
      }
    } else if (k == "V") {
      pc->visible = !pc->visible;
      pc->changed();
    } else if (k == "Z") {
      c.host.zoom_cloud();
    } else if (k == "D") {
      const std::string name = pc->cloud->name;
      pc->detach();
      c.print(c.t("pc.detached", {{"file", name}}));
      c.host.cloud_changed();
      co_return;
    }
    c.host.cloud_changed();
  }
}

}  // namespace

void add_cloud(std::vector<CommandDef>& t) {
  CommandDef at = def("POINTCLOUDATTACH", {"PCATTACH", "POINTCLOUDATT"}, pointcloudattach_cmd, "pc.help.attach", true);
  at.keep_selection = true;
  t.push_back(std::move(at));
  CommandDef pc = def("POINTCLOUD", {"PC"}, pointcloud_cmd, "pc.help.cmd", true);
  pc.keep_selection = true;
  t.push_back(std::move(pc));
}

}  // namespace cc::cmd::detail
