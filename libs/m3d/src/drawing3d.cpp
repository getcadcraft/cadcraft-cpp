// SPDX-License-Identifier: Apache-2.0
// model3d.js from CadCraft 0.5 (fromFile, forFile3d, cleanView, the unit tables).
#include "cc/m3d/drawing3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"

namespace cc::m3d {

using base::Array;
using base::Value;

namespace {

bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}

/// x.toPrecision(12) read back as a number
double precision12(double x) {
  return base::js_number(base::js_to_exponential(x, 11));
}

/// A planar quad is convex when every corner turns the same way about its normal.
bool convex_quad(const std::array<Vec3, 4>& P) {
  Vec3 n{0, 0, 0};
  for (std::size_t i = 0; i < 4; ++i) {
    const Vec3& a = P.at(i);
    const Vec3& b = P.at((i + 1) % 4);
    n[0] += (a[1] - b[1]) * (a[2] + b[2]);
    n[1] += (a[2] - b[2]) * (a[0] + b[0]);
    n[2] += (a[0] - b[0]) * (a[1] + b[1]);
  }
  double sign = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const Vec3& a = P.at(i);
    const Vec3& b = P.at((i + 1) % 4);
    const Vec3& c = P.at((i + 2) % 4);
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const Vec3 v{c[0] - b[0], c[1] - b[1], c[2] - b[2]};
    const Vec3 x{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const double d = x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
    double s = 0;
    if (d > 0) {
      s = 1;
    } else if (d < 0) {
      s = -1;
    }
    if (s == 0) {
      return false;
    }
    if (sign != 0 && s != sign) {
      return false;
    }
    sign = s;
  }
  return true;
}

}  // namespace

double m_per_unit(std::string_view u) {
  static constexpr std::array<std::pair<std::string_view, double>, 11> kM = {{{"", 0.001},
                                                                              {"inches", 0.0254},
                                                                              {"feet", 0.3048},
                                                                              {"miles", 1609.344},
                                                                              {"millimeters", 0.001},
                                                                              {"centimeters", 0.01},
                                                                              {"meters", 1},
                                                                              {"kilometers", 1000},
                                                                              {"microinches", 2.54e-8},
                                                                              {"mils", 2.54e-5},
                                                                              {"yards", 0.9144}}};
  for (const auto& [k, v] : kM) {
    if (k == u) {
      return v;
    }
  }
  return 0.001;
}

std::string display_unit(std::string_view u) {
  if (u == "inches" || u == "mils" || u == "microinches") {
    return "in";
  }
  if (u == "feet" || u == "yards" || u == "miles") {
    return "ft-in";
  }
  if (u == "centimeters") {
    return "cm";
  }
  if (u == "meters" || u == "kilometers") {
    return "m";
  }
  return "mm";
}

Value clean_view(const Value& v) {
  if (!v.is_object() || v.is_point()) {
    return {nullptr};
  }
  const auto P = [](const Value& p) -> std::optional<Vec3> {
    if (!p.is_array() || p.as_array().size() != 3) {
      return std::nullopt;
    }
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
      const Value& x = p.as_array()[i];
      if (!finite(x) || std::abs(x.as_number()) >= 1e9) {
        return std::nullopt;
      }
      out.at(i) = precision12(x.as_number());
    }
    return out;
  };
  const auto eye = P(v.get("eye"));
  const auto target = P(v.get("target"));
  if (!eye || !target ||
      base::js_hypot((*eye)[0] - (*target)[0], (*eye)[1] - (*target)[1], (*eye)[2] - (*target)[2]) < 1e-9) {
    return {nullptr};
  }
  const Value f = v.get("fov");
  const double fov = finite(f) && f.as_number() >= 5 && f.as_number() <= 120 ? f.as_number() : 35;
  const auto arr = [](const Vec3& p) { return Value(Array{p[0], p[1], p[2]}); };
  Value out = base::make_object(
      {{"eye", arr(*eye)}, {"target", arr(*target)}, {"persp", !(v.get("persp") == Value(false))}, {"fov", fov}});
  const Value st = v.get("style");
  if (st.is_string() && (st.as_string() == "shaded" || st.as_string() == "edges" || st.as_string() == "hidden" ||
                         st.as_string() == "wire" || st.as_string() == "xray")) {
    out.set("style", st);
  }
  return out;
}

FromFile from_file(Model& m, const Value& in3d, std::string_view units, std::string_view faces_name) {
  FromFile r;
  m.reset();
  m.units = display_unit(units);
  const double k = m_per_unit(units);
  const Value fv = in3d.is_object() ? in3d.get("faces") : Value();
  const Value view = in3d.is_object() ? in3d.get("view") : Value();
  m.saved_view = view.truthy() ? view : Value(nullptr);
  if (!fv.is_array()) {
    return r;
  }
  const auto& faces = fv.as_array();
  constexpr std::size_t kMax = 200000;
  std::vector<std::vector<Vec3>> polys;
  for (std::size_t fi = 0; fi < faces.size() && fi < kMax; ++fi) {
    const Value pv = faces[fi].get("pts");
    std::vector<Vec3> pts;
    if (pv.is_array()) {
      for (const Value& p : pv.as_array()) {
        bool ok = p.is_array() && p.as_array().size() >= 3;
        if (ok) {
          for (const Value& x : p.as_array()) {
            ok = ok && finite(x) && std::abs(x.as_number() * k) < 1e7;
          }
        }
        if (!ok) {
          pts.clear();
          break;
        }
        const auto& a = p.as_array();
        const Vec3 q{a[0].as_number() * k, a[1].as_number() * k, a[2].as_number() * k};
        if (pts.empty() || std::abs(pts.back()[0] - q[0]) > 1e-9 || std::abs(pts.back()[1] - q[1]) > 1e-9 ||
            std::abs(pts.back()[2] - q[2]) > 1e-9) {
          pts.push_back(q);
        }
      }
    }
    if (pts.size() > 1 && std::abs(pts.front()[0] - pts.back()[0]) <= 1e-9 &&
        std::abs(pts.front()[1] - pts.back()[1]) <= 1e-9 && std::abs(pts.front()[2] - pts.back()[2]) <= 1e-9) {
      pts.pop_back();
    }
    if (pts.size() >= 3) {
      polys.push_back(std::move(pts));
    }
  }
  if (!polys.empty()) {
    const Def& d = mesh_def(m, polys, faces_name);
    const std::string id = d.id;
    (void)m.place(m.root(), id, ident());
    r.loose = polys.size();
  }
  if (faces.size() > kMax) {
    r.left_out = faces.size() - kMax;
  }
  return r;
}

std::optional<File3d> for_file3d(const Model& m, std::string_view units) {
  if (m.empty()) {
    return std::nullopt;
  }
  const double k = m_per_unit(units);
  File3d out;
  m.walk([&](const Def& def, const Mat4& wm) {
    const auto W = [&](std::int64_t v) {
      const Vec3 p = apply(wm, *def.verts.get(v));
      return Vec3{p[0] / k, p[1] / k, p[2] / k};
    };
    for (const auto& [fid, f] : def.faces.items()) {
      const auto& l0 = f.loops.at(0);
      bool simple = f.loops.size() == 1 && l0.size() == 3;
      if (f.loops.size() == 1 && l0.size() == 4) {
        const auto q = def.points(l0);
        simple = convex_quad({q[0], q[1], q[2], q[3]});
      }
      if (simple) {
        Face3d face;
        for (std::size_t i = 0; i < l0.size(); ++i) {
          face.pts.at(i) = W(l0[i]);
        }
        if (l0.size() == 3) {
          face.pts[3] = face.pts[2];
        }
        out.faces.push_back(face);
        continue;
      }
      std::set<std::pair<std::int64_t, std::int64_t>> along;
      for (const auto& l : f.loops) {
        for (std::size_t i = 0; i < l.size(); ++i) {
          const std::int64_t a = l[i];
          const std::int64_t b = l[(i + 1) % l.size()];
          along.emplace(a, b);
          along.emplace(b, a);
        }
      }
      for (const auto& [a, b, c] : face_tris(def, f)) {
        const auto vis = [&](std::int64_t p, std::int64_t q) { return along.contains({p, q}); };
        const int invis = (vis(a, b) ? 0 : 1) | (vis(b, c) ? 0 : 2) | (vis(c, a) ? 0 : 8);
        const Vec3 C = W(c);
        out.faces.push_back(Face3d{.pts = {W(a), W(b), C, C}, .invis = invis});
      }
    }
  });
  // JSON.stringify(m.toJSON()) with every UTF-16 unit outside 0x21–0x7e as \uXXXX
  const auto units16 = base::to_utf16(base::js_stringify(m.to_json()));
  std::string text;
  text.reserve(units16.size());
  for (const std::uint16_t u : units16) {
    if (u >= 0x21 && u <= 0x7e) {
      text += static_cast<char>(u);
    } else {
      const std::string h = base::hex_lower(u);
      text += "\\u" + std::string(4 - std::min<std::size_t>(4, h.size()), '0') + h;
    }
  }
  for (std::size_t i = 0; i < text.size(); i += 230) {
    out.points.push_back("CC3D:" + std::to_string(out.points.size() + 1) + ":" + text.substr(i, 230));
  }
  if (const Value view = clean_view(m.saved_view); view.truthy()) {
    out.points.push_back("CCCAM:" + base::js_stringify(view));
  }
  out.text = std::move(text);
  return out;
}

}  // namespace cc::m3d
