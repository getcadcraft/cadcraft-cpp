// SPDX-License-Identifier: Apache-2.0
// Script ops for alignment.js and profile.js (tools/parity/civil/driver.mjs has their JS twins).
#include <cmath>
#include <limits>
#include <numbers>

#include "cc/civil/alignment.hpp"
#include "cc/civil/script.hpp"

namespace cc::civil {
namespace {

using base::Array;
using base::make_object;
using base::pt2;
using base::Value;

Value strs(const std::vector<std::string>& v) {
  Array a;
  for (const auto& s : v) {
    a.emplace_back(s);
  }
  return a;
}
std::optional<double> opt_num(const Value& v) {
  return nullish(v) ? std::nullopt : std::optional(num(v));
}
Value opt_value(const std::optional<double>& v) {
  return v ? Value(*v) : Value(nullptr);
}

Value element_value(const Element& e) {
  if (!e.arc) {
    return make_object({{"type", "line"},
                        {"p0", pt2(e.p0.x, e.p0.y)},
                        {"p1", pt2(e.p1.x, e.p1.y)},
                        {"dir", e.dir},
                        {"sta", e.sta},
                        {"len", e.len}});
  }
  return make_object({{"type", "arc"},
                      {"p0", pt2(e.p0.x, e.p0.y)},
                      {"p1", pt2(e.p1.x, e.p1.y)},
                      {"c", pt2(e.c.x, e.c.y)},
                      {"r", e.r},
                      {"ccw", e.ccw},
                      {"delta", e.delta},
                      {"a0", e.a0},
                      {"sta", e.sta},
                      {"len", e.len},
                      {"pi", pt2(e.pi.x, e.pi.y)},
                      {"piIndex", e.pi_index},
                      {"T", e.T},
                      {"E", e.E},
                      {"M", e.M},
                      {"chord", e.chord},
                      {"dirIn", e.dir_in},
                      {"dirOut", e.dir_out}});
}

Value al_view(const Alignment& a) {
  Array els;
  for (const Element& e : a.elements) {
    els.push_back(element_value(e));
  }
  Array pis;
  for (const Pi& p : a.pis) {
    const double r = base::js_to_number(p.r);
    pis.push_back(make_object({{"x", p.x}, {"y", p.y}, {"r", (std::isnan(r) || r == 0) ? 0.0 : r}}));
  }
  return make_object({{"elements", std::move(els)},
                      {"start", a.start},
                      {"errors", strs(a.errors)},
                      {"pis", std::move(pis)},
                      {"length", a.length},
                      {"end", a.end},
                      {"ok", a.ok()}});
}

Value prof_view(const Profile& p) {
  Array grades;
  for (const Grade& g : p.grades) {
    grades.push_back(make_object({{"from", g.from}, {"to", g.to}, {"g", g.g}}));
  }
  Array curves;
  for (const ProfileCurve& c : p.curves) {
    Value o = make_object({{"i", c.i},
                           {"pvi", c.pvi},
                           {"zPvi", c.z_pvi},
                           {"bvc", c.bvc},
                           {"evc", c.evc},
                           {"L", c.L},
                           {"g1", c.g1},
                           {"g2", c.g2},
                           {"A", c.A},
                           {"K", c.K},
                           {"zBvc", c.z_bvc},
                           {"zEvc", c.z_evc},
                           {"kind", c.crest ? "crest" : "sag"}});
    if (c.extreme) {
      o.set(c.crest ? "high" : "low", make_object({{"sta", c.extreme->first}, {"z", c.extreme->second}}));
    }
    curves.push_back(std::move(o));
  }
  return make_object({{"pvis", Array(p.pvis.begin(), p.pvis.end())},
                      {"grades", std::move(grades)},
                      {"curves", std::move(curves)},
                      {"errors", strs(p.errors)},
                      {"start", p.start},
                      {"end", p.end},
                      {"ok", p.ok()}});
}

std::vector<Pi> to_pis(const Value& v) {
  std::vector<Pi> out;
  for (const Value& p : arr(v)) {
    const Value x = field(p, "x");
    const Value y = field(p, "y");
    // Number.isFinite: only numbers pass
    out.push_back(
        {x.is_number() ? x.as_number() : std::nan(""), y.is_number() ? y.as_number() : std::nan(""), field(p, "r")});
  }
  return out;
}

}  // namespace

void add_alignment_ops(std::map<std::string, ScriptOp, std::less<>>& m) {
  m["alignment.normAng"] = [](const Array& a, ScriptCtx&) -> Value { return norm_ang(num(arg(a, 0))); };
  m["alignment.build"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value in = arg(a, 0);
    const Value start = field(in, "start");
    auto al = build_alignment(to_pis(field(in, "pis")), start.is_undefined() ? 0 : num(start), *c.words);
    c.keep(al);
    return al_view(*al);
  };
  m["al.pointAt"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto p = self_as<Alignment>(c).point_at(num(arg(a, 0)));
    return p ? make_object({{"x", p->x}, {"y", p->y}, {"dir", p->dir}}) : Value(nullptr);
  };
  m["al.stationOffset"] = [](const Array& a, ScriptCtx& c) -> Value {
    const auto r = self_as<Alignment>(c).station_offset(num(arg(a, 0)), num(arg(a, 1)));
    if (!r) {
      return nullptr;
    }
    return make_object(
        {{"sta", r->sta}, {"off", r->off}, {"x", r->x}, {"y", r->y}, {"outside", r->outside}, {"dist", r->dist}});
  };
  m["al.polyline"] = [](const Array& a, ScriptCtx& c) -> Value {
    const double max_seg = opt_num(arg(a, 0)).value_or(std::numeric_limits<double>::infinity());
    const double max_turn = opt_num(arg(a, 1)).value_or(std::numbers::pi / 360);
    Array out;
    for (const auto& p : self_as<Alignment>(c).polyline(max_seg, max_turn)) {
      out.push_back(make_object({{"x", p.x}, {"y", p.y}, {"sta", p.sta}}));
    }
    return out;
  };
  m["al.ticks"] = [](const Array& a, ScriptCtx& c) -> Value {
    Array out;
    for (const auto& t : self_as<Alignment>(c).ticks(num(arg(a, 0)))) {
      out.push_back(make_object({{"sta", t.sta}, {"x", t.x}, {"y", t.y}, {"dir", t.dir}}));
    }
    return out;
  };
  m["al.keyStations"] = [](const Array&, ScriptCtx& c) -> Value {
    Array out;
    for (const auto& k : self_as<Alignment>(c).key_stations()) {
      out.push_back(make_object({{"sta", k.sta}, {"kind", k.kind}}));
    }
    return out;
  };
  m["profile.sampleSurface"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value sn = arg(a, 0);
    const auto s = nullish(sn) ? nullptr : reg_get<Surface>(c, str(sn));
    const auto al = reg_get<Alignment>(c, str(arg(a, 1)));
    Array out;
    if (!al) {
      return out;
    }
    const Value step = field(arg(a, 2), "step");
    for (const auto& g : sample_surface(s.get(), *al, step.is_undefined() ? std::nullopt : std::optional(num(step)))) {
      out.push_back(make_object({{"sta", g.sta}, {"z", opt_value(g.z)}}));
    }
    return out;
  };
  m["profile.build"] = [](const Array& a, ScriptCtx& c) -> Value {
    auto p = build_profile(arg(a, 0), *c.words);
    c.keep(p);
    return prof_view(*p);
  };
  m["prof.elevationAt"] = [](const Array& a, ScriptCtx& c) -> Value {
    return opt_value(self_as<Profile>(c).elevation_at(num(arg(a, 0))));
  };
  m["prof.gradeAt"] = [](const Array& a, ScriptCtx& c) -> Value {
    return opt_value(self_as<Profile>(c).grade_at(num(arg(a, 0))));
  };
  m["prof.sample"] = [](const Array& a, ScriptCtx& c) -> Value {
    const Value st = arg(a, 0);
    Array out;
    for (const auto& s : self_as<Profile>(c).sample(nullish(st) ? std::nullopt : std::optional(num(st)))) {
      out.push_back(make_object({{"sta", s.sta}, {"z", opt_value(s.z)}}));
    }
    return out;
  };
}

}  // namespace cc::civil
