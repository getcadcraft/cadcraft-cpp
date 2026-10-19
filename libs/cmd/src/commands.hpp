// SPDX-License-Identifier: Apache-2.0
// cc::cmd internals: the command table (commands.js COMMANDS, in its order) and the helpers the commands share.
#pragma once

#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"
#include "cc/cmd/engine.hpp"

namespace cc::cmd::detail {

using base::Value;

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTau = kPi * 2;

/// commands.js def(name, aliases, fn, { help, noRepeat }).
inline CommandDef def(std::string name, std::vector<std::string> aliases, CommandFn fn, std::string help,
                      bool no_repeat = false) {
  CommandDef d;
  d.name = std::move(name);
  d.aliases = std::move(aliases);
  d.fn = std::move(fn);
  d.help = std::move(help);
  d.no_repeat = no_repeat;
  return d;
}

/// Every command, in commands.js's order.
const std::vector<CommandDef>& command_table();

// the commands, one function per area (draw.cpp, modify.cpp)
void add_draw(std::vector<CommandDef>& t);
void add_modify(std::vector<CommandDef>& t);
void add_trim(std::vector<CommandDef>& t);
void add_reshape(std::vector<CommandDef>& t);
void add_corner(std::vector<CommandDef>& t);
void add_annotate(std::vector<CommandDef>& t);
void add_hatch(std::vector<CommandDef>& t);

/// commands.js undoInside(c): undo the command's last step (U at a prompt).
void undo_inside(Ctx& c);

// ---- geom.js
inline double dist(Pt a, Pt b) {
  return base::js_hypot(b.x - a.x, b.y - a.y);
}
inline double angle_of(Pt a, Pt b) {
  return base::js_atan2(b.y - a.y, b.x - a.x);
}
inline Pt polar(Pt p, double ang, double d) {
  return {p.x + base::js_cos(ang) * d, p.y + base::js_sin(ang) * d};
}
inline double norm_angle(double a) {
  a = std::fmod(a, kTau);
  return a < 0 ? a + kTau : a;
}
inline double deg(double r) {
  return r * 180 / kPi;
}
inline double rad(double d) {
  return d * kPi / 180;
}
/// { x, y, b } of a polyline vertex
inline Value vtx(double x, double y, double b = 0) {
  return base::make_object({{"x", x}, {"y", y}, {"b", b}});
}
inline Value line_e(double x1, double y1, double x2, double y2) {
  return base::make_object({{"type", "line"}, {"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}});
}
/// commands.js rect(a, b): the closed polyline with corners a and b
Value rect(Pt a, Pt b);
/// The id of a stored entity.
inline Id id_of(const Value& e) {
  return static_cast<Id>(base::js_to_number(e.get("id")));
}
inline Pt pt_of(const Value& p) {
  return {base::js_to_number(p.get("x")), base::js_to_number(p.get("y"))};
}

}  // namespace cc::cmd::detail
