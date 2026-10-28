// SPDX-License-Identifier: Apache-2.0
// form.js (PartCraft 0.5), the part a part file needs: the cage format, its reader and the box primitive. Subdivision
// and the surface conversion come with the Form workspace.
#include "cc/partcraft/form.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "jsx.hpp"

namespace cc::partcraft::form {
namespace {

using base::Value;

struct Fail {
  std::string message;
};

bool fnum(const Value& v, double lim = 1e6) {
  return v.is_number() && std::isfinite(v.as_number()) && std::abs(v.as_number()) <= lim;
}
bool is_int(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number()) && v.as_number() == std::trunc(v.as_number());
}
/// ek(a, b): the edge key "<small>,<large>" (numbers as JS prints them).
std::string ek(double a, double b) {
  const auto s = [](double x) { return base::js_string(Value(x)); };
  return a < b ? s(a) + "," + s(b) : s(b) + "," + s(a);
}

Value empty_form() {
  return base::make_object({{"format", kFormat},
                            {"version", kVersion},
                            {"verts", base::Array{}},
                            {"faces", base::Array{}},
                            {"creases", base::Array{}},
                            {"corners", base::Array{}},
                            {"sym", nullptr}});
}

/// checked(): an oriented 2-manifold with boundary, or the reason.
std::optional<std::string> check(const base::Array& faces, const i18n::Catalog& w) {
  std::set<std::pair<double, double>> dir;
  std::map<std::string, int> cnt;
  for (const Value& g : faces) {
    const auto& a = g.as_array();
    std::set<double> uniq;
    for (const Value& v : a) {
      uniq.insert(v.as_number());
    }
    if (a.size() < 3 || uniq.size() != a.size()) {
      return w.t("form.err.degenerate");
    }
    for (std::size_t k = 0; k < a.size(); ++k) {
      const double v = a[k].as_number();
      const double u = a[(k + 1) % a.size()].as_number();
      if (!dir.insert({v, u}).second) {
        return w.t("form.err.flip");
      }
      ++cnt[ek(v, u)];
    }
  }
  for (const auto& [k, c] : cnt) {
    if (c > 2) {
      return w.t("form.err.nonmanifold");
    }
  }
  return std::nullopt;
}

}  // namespace

base::Result<Value> read_form(const Value& raw, const i18n::Catalog& w) {
  const auto fail = [&](std::string_view key, std::initializer_list<std::pair<std::string_view, std::string>> f = {}) {
    return base::fail(base::Errc::invalid, w.t(key, f));
  };
  Value j = raw;
  if (raw.is_string()) {
    auto p = base::parse_json(raw.as_string());
    if (!p) {
      return fail("form.file.not_json");
    }
    j = std::move(*p);
  }
  if (!j.truthy() || (!j.is_object() && !j.is_array()) || !(j.get("format") == Value(kFormat))) {
    return fail("form.file.not_form");
  }
  const Value ver = j.get("version");
  if (!is_int(ver) || ver.as_number() < 1) {
    return fail("form.file.no_version");
  }
  if (ver.as_number() > kVersion) {
    return fail("form.file.newer", {{"v", base::js_string(ver)}, {"mine", base::js_string(Value(kVersion))}});
  }
  const Value verts_v = j.get("verts");
  if (!verts_v.is_array() || verts_v.as_array().size() > kMaxVerts) {
    return fail("form.file.max_verts", {{"n", std::to_string(kMaxVerts)}});
  }
  const Value faces_v = j.get("faces");
  if (!faces_v.is_array() || faces_v.as_array().size() > kMaxFaces) {
    return fail("form.file.max_faces", {{"n", std::to_string(kMaxFaces)}});
  }
  base::Array verts;
  for (const Value& p : verts_v.as_array()) {
    if (!p.is_array() || p.as_array().size() != 3 ||
        !std::ranges::all_of(p.as_array(), [](const Value& x) { return fnum(x); })) {
      return fail("form.file.vertex");
    }
    verts.push_back(p);
  }
  const auto nv = static_cast<double>(verts.size());
  const auto vid = [nv](const Value& v) { return is_int(v) && v.as_number() >= 0 && v.as_number() < nv; };
  base::Array faces;
  for (const Value& f : faces_v.as_array()) {
    if (!f.is_array() || f.as_array().size() < 3 || f.as_array().size() > 64 ||
        !std::ranges::all_of(f.as_array(), vid)) {
      return fail("form.file.face");
    }
    std::set<double> uniq;
    for (const Value& v : f.as_array()) {
      uniq.insert(v.as_number());
    }
    if (uniq.size() != f.as_array().size()) {
      return fail("form.file.face");
    }
    faces.push_back(f);
  }
  if (const auto why = check(faces, w)) {
    return base::fail(base::Errc::invalid, *why);
  }
  Value out = empty_form();
  out.set("verts", verts);
  out.set("faces", faces);
  std::set<std::string> edges;
  for (const Value& g : faces) {
    const auto& a = g.as_array();
    for (std::size_t k = 0; k < a.size(); ++k) {
      edges.insert(ek(a[k].as_number(), a[(k + 1) % a.size()].as_number()));
    }
  }
  const Value creases = j.get("creases");
  if (!creases.is_undefined()) {
    if (!creases.is_array() || creases.as_array().size() > 4 * kMaxFaces) {
      return fail("form.file.creases");
    }
    std::vector<std::pair<std::string, base::Array>> dedupe;  // dedupeCreases: one per edge, the last wins, first place
    for (const Value& c : creases.as_array()) {
      const auto& a = c.is_array() ? c.as_array() : base::Array{};
      const bool weight = a.size() == 3 && a[2].is_number() && a[2].as_number() > 0 && a[2].as_number() <= kSharp;
      if (!c.is_array() || a.size() != 3 || !vid(a[0]) || !vid(a[1]) ||
          !edges.contains(ek(a[0].as_number(), a[1].as_number())) || !weight) {
        return fail("form.file.crease");
      }
      const double x = a[0].as_number();
      const double y = a[1].as_number();
      const std::string key = ek(x, y);
      base::Array row{std::min(x, y), std::max(x, y), a[2]};
      const auto it = std::ranges::find_if(dedupe, [&](const auto& p) { return p.first == key; });
      if (it != dedupe.end()) {
        it->second = std::move(row);
      } else {
        dedupe.emplace_back(key, std::move(row));
      }
    }
    base::Array cs;
    for (auto& [k, row] : dedupe) {
      cs.emplace_back(std::move(row));
    }
    out.set("creases", std::move(cs));
  }
  const Value corners = j.get("corners");
  if (!corners.is_undefined()) {
    if (!corners.is_array() || static_cast<double>(corners.as_array().size()) > nv ||
        !std::ranges::all_of(corners.as_array(), vid)) {
      return fail("form.file.corners");
    }
    base::Array cs;
    for (const Value& v : corners.as_array()) {
      if (std::ranges::find(cs, v) == cs.end()) {
        cs.push_back(v);
      }
    }
    out.set("corners", std::move(cs));
  }
  const Value sym = j.get("sym");
  if (!sym.is_undefined() && !sym.is_null()) {
    const Value axis = jsx::get(sym, "axis");
    const std::string ax = axis.is_undefined() ? "undefined" : base::js_string(axis);
    // `s.axis in AXES`: own keys x, y, z and everything a plain object inherits
    bool in_axes = ax == "x" || ax == "y" || ax == "z";
    for (const std::string_view n :
         {"constructor", "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "toLocaleString", "toString",
          "valueOf", "__proto__", "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__"}) {
      in_axes = in_axes || ax == n;
    }
    const Value side = jsx::get(sym, "side");
    const Value s = side.is_null() || side.is_undefined() ? Value(1) : side;
    if (!sym.truthy() || !in_axes || !fnum(jsx::get(sym, "at")) || !(s == Value(1) || s == Value(-1))) {
      return fail("form.file.symmetry");
    }
    out.set("sym", base::make_object({{"axis", axis}, {"at", jsx::get(sym, "at")}, {"side", s}}));
  }
  return out;
}

Value box(double nx, double ny, double nz, double sx, double sy, double sz) {
  std::array<double, 3> n{};
  const std::array<double, 3> in{nx, ny, nz};
  for (std::size_t i = 0; i < 3; ++i) {
    n[i] = std::isnan(in[i]) ? in[i] : std::max(1.0, std::min(32.0, base::js_round(in[i])));
  }
  const std::array<double, 3> size{sx, sy, sz};
  base::Array verts;
  std::map<std::string, double> index;
  const auto vid = [&](const std::array<double, 3>& l) {
    const std::string k =
        base::js_string(Value(l[0])) + "," + base::js_string(Value(l[1])) + "," + base::js_string(Value(l[2]));
    const auto it = index.find(k);
    if (it != index.end()) {
      return it->second;
    }
    const auto id = static_cast<double>(verts.size());
    index.emplace(k, id);
    verts.emplace_back(
        base::Array{(l[0] / n[0] - 0.5) * size[0], (l[1] / n[1] - 0.5) * size[1], (l[2] / n[2] - 0.5) * size[2]});
    return id;
  };
  constexpr std::array<std::array<std::size_t, 4>, 6> kSides = {
      {{0, 1, 1, 2}, {0, 0, 2, 1}, {1, 1, 2, 0}, {1, 0, 0, 2}, {2, 1, 0, 1}, {2, 0, 1, 0}}};
  base::Array faces;
  for (const auto& [ax, side, sa, ta] : kSides) {
    const int ns = std::isnan(n[sa]) ? 0 : static_cast<int>(n[sa]);  // whole numbers 1–32 (NaN: no faces, as the JS)
    const int nt = std::isnan(n[ta]) ? 0 : static_cast<int>(n[ta]);
    for (int ii = 0; !std::isnan(n[sa]) && ii < ns; ii++) {
      for (int ij = 0; !std::isnan(n[ta]) && ij < nt; ij++) {
        const double i = ii;
        const double jj = ij;
        const auto at = [&](double s, double t) {
          std::array<double, 3> l{0, 0, 0};
          l[ax] = side != 0 ? n[ax] : 0;
          l[sa] = s;
          l[ta] = t;
          return vid(l);
        };
        faces.emplace_back(base::Array{at(i, jj), at(i + 1, jj), at(i + 1, jj + 1), at(i, jj + 1)});
      }
    }
  }
  Value f = empty_form();
  f.set("verts", std::move(verts));
  f.set("faces", std::move(faces));
  return f;
}

}  // namespace cc::partcraft::form
