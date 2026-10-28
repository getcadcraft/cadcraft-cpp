// SPDX-License-Identifier: Apache-2.0
// meshfaces.js: the mesh kernel's named faces and edges, in the JS's order (its Maps iterate in insertion order, and
// its edge keys are doubles: u · 2^32 + v).
#include "cc/partcraft/meshfaces.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "jsx.hpp"
#include "ops.hpp"

namespace cc::partcraft::meshfaces {
namespace {

using base::Value;
using vec::V3;
namespace V = vec;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double k2p32 = 4294967296.0;
constexpr double kInf = std::numeric_limits<double>::infinity();

double js_max(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a > b ? a : b;
}
double js_min(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return kNaN;
  }
  return a < b ? a : b;
}
double num(const Value& v) {
  return base::js_to_number(v);
}
double hyp(const V3& v) {
  return base::js_hypot(v[0], v[1], v[2]);
}
/// `[...a]`: a copy of an array (the fork's surfaces always hold arrays here).
Value copy3(const Value& a) {
  if (a.is_array()) {
    return a;
  }
  return V::value(V::v3(a));
}

enum class Type : std::uint8_t { kPlane, kCylinder, kCone, kSphere, kTorus, kThread, kOther };

struct Surf {
  Type type = Type::kOther;
  V3 n{}, o{};      // plane
  V3 ao{}, ad{};    // axis
  V3 c{};           // sphere
  double r = kNaN;  // cylinder, sphere, torus (tube)
  double k = kNaN, R = kNaN, r0 = kNaN, r1 = kNaN, t0 = kNaN, t1 = kNaN;
  bool has_within = false, has_span = false;
  V3 wo{}, wd{};
  double wr = kNaN;
  V3 sa{}, sd{};
  double slen = kNaN;
};

Type type_of(const Value& t) {
  if (!t.is_string()) {
    return Type::kOther;
  }
  const std::string& s = t.as_string();
  if (s == "plane") {
    return Type::kPlane;
  }
  if (s == "cylinder") {
    return Type::kCylinder;
  }
  if (s == "cone") {
    return Type::kCone;
  }
  if (s == "sphere") {
    return Type::kSphere;
  }
  if (s == "torus") {
    return Type::kTorus;
  }
  if (s == "thread") {
    return Type::kThread;
  }
  return Type::kOther;
}

Surf parse(const Value& s) {
  if (s.is_null() || s.is_undefined()) {
    ops::throw_read(s, "type");
  }
  Surf o;
  o.type = type_of(s.get("type"));
  o.n = V::v3(s.get("n"));
  o.o = V::v3(s.get("o"));
  const Value ax = s.get("axis");
  o.ao = V::v3(ax.get("o"));
  o.ad = V::v3(ax.get("d"));
  o.c = V::v3(s.get("c"));
  o.r = num(s.get("r"));
  o.k = num(s.get("k"));
  o.R = num(s.get("R"));
  o.r0 = num(s.get("r0"));
  o.r1 = num(s.get("r1"));
  o.t0 = num(s.get("t0"));
  o.t1 = num(s.get("t1"));
  const Value w = s.get("within");
  if (w.truthy()) {
    o.has_within = true;
    o.wo = V::v3(w.get("o"));
    o.wd = V::v3(w.get("d"));
    o.wr = num(w.get("r"));
  }
  const Value sp = s.get("span");
  if (sp.truthy()) {
    o.has_span = true;
    o.sa = V::v3(sp.get("a"));
    o.sd = V::v3(sp.get("d"));
    o.slen = num(sp.get("len"));
  }
  return o;
}

/// radial(p, ax): [distance from the axis, height along it].
std::pair<double, double> radial(const V3& p, const V3& o, const V3& d) {
  const V3 v = V::sub(p, o);
  const double h = V::dot(v, d);
  return {hyp(V::sub(v, V::mul(d, h))), h};
}

using Tri = std::array<V3, 3>;

bool on_surface(const Surf& s, const Tri& P, const std::optional<V3>& nt, double tol, double sag) {
  if (s.has_within || s.has_span) {
    const V3 c = V::mul(V::add(V::add(V::add(V3{0, 0, 0}, P[0]), P[1]), P[2]), 1.0 / 3);
    if (s.has_within && radial(c, s.wo, s.wd).first > s.wr + tol) {
      return false;
    }
    if (s.has_span) {
      const double u = V::dot(V::sub(c, s.sa), s.sd);
      if (u < -tol || u > s.slen + tol) {
        return false;
      }
    }
  }
  const auto every = [&](auto f) { return std::ranges::all_of(P, f); };
  switch (s.type) {
    case Type::kPlane:
      return (!nt || std::abs(V::dot(*nt, s.n)) > 0.9) &&
             every([&](const V3& p) { return std::abs(V::dot(V::sub(p, s.o), s.n)) <= tol; });
    case Type::kCylinder:
      if (nt && std::abs(V::dot(*nt, s.ad)) > 0.5) {
        return false;
      }
      return every([&](const V3& p) {
        const double d = radial(p, s.ao, s.ad).first - s.r;
        return d <= tol + 1e-6 * s.r && d >= -(tol + sag * s.r);
      });
    case Type::kCone:
      if (nt && std::abs(V::dot(*nt, s.ad)) > 0.999) {
        return false;
      }
      return every([&](const V3& p) {
        const auto [rho, h] = radial(p, s.ao, s.ad);
        const double d = rho - h * s.k;
        return h >= -tol && d <= tol * (1 + s.k) && d >= -(tol * (1 + s.k) + sag * h * s.k);
      });
    case Type::kSphere:
      return every([&](const V3& p) {
        const double d = hyp(V::sub(p, s.c)) - s.r;
        return d <= tol && d >= -(tol + sag * s.r);
      });
    case Type::kTorus:
      return every([&](const V3& p) {
        const auto [rho, h] = radial(p, s.ao, s.ad);
        const double d = base::js_hypot(rho - s.R, h) - s.r;
        return d <= tol && d >= -(tol + sag * (s.R + s.r));
      });
    case Type::kThread:
      if (nt && std::abs(V::dot(*nt, s.ad)) > 0.99) {
        return false;
      }
      return every([&](const V3& p) {
        const auto [rho, h] = radial(p, s.ao, s.ad);
        return rho >= s.r0 - tol - sag * s.r0 && rho <= s.r1 + tol && h >= s.t0 - tol && h <= s.t1 + tol;
      });
    case Type::kOther: break;
  }
  return false;
}

/// A Map keyed by double edge keys, iterated in insertion order.
struct EdgeTris {
  std::vector<double> keys;
  std::vector<std::vector<std::size_t>> lists;
  std::unordered_map<double, std::size_t> index;

  void add(double key, std::size_t t) {
    auto [it, fresh] = index.try_emplace(key, keys.size());
    if (fresh) {
      keys.push_back(key);
      lists.emplace_back();
    }
    lists[it->second].push_back(t);
  }
  [[nodiscard]] const std::vector<std::size_t>* get(double key) const {
    auto it = index.find(key);
    return it == index.end() ? nullptr : &lists[it->second];
  }
};

double ekey(double u, double v) {
  return u < v ? u * k2p32 + v : v * k2p32 + u;
}

/// String(x) for a name sort (Array#sort's default compares strings by UTF-16 units).
bool str_less(const Value& a, const Value& b) {
  return base::to_utf16(base::js_string(a)) < base::to_utf16(base::js_string(b));
}

}  // namespace

// ---- surface records

Value plane(const std::string& name, const V3& n, const V3& o) {
  return base::make_object({{"name", name}, {"type", "plane"}, {"n", V::value(V::norm(n))}, {"o", V::value(o)}});
}
Value cylinder(const std::string& name, const V3& o, const V3& d, double r) {
  return base::make_object({{"name", name},
                            {"type", "cylinder"},
                            {"axis", base::make_object({{"o", V::value(o)}, {"d", V::value(V::norm(d))}})},
                            {"r", r}});
}
Value cone(const std::string& name, const V3& apex, const V3& d, double k) {
  return base::make_object({{"name", name},
                            {"type", "cone"},
                            {"axis", base::make_object({{"o", V::value(apex)}, {"d", V::value(V::norm(d))}})},
                            {"k", k}});
}
Value sphere(const std::string& name, const V3& c, double r) {
  return base::make_object({{"name", name}, {"type", "sphere"}, {"c", V::value(c)}, {"r", r}});
}
Value torus(const std::string& name, const V3& c, const V3& d, double R, double r) {
  return base::make_object({{"name", name},
                            {"type", "torus"},
                            {"axis", base::make_object({{"o", V::value(c)}, {"d", V::value(V::norm(d))}})},
                            {"R", R},
                            {"r", r}});
}
Value other(const std::string& name) {
  return base::make_object({{"name", name}, {"type", "other"}});
}
Value thread(const std::string& name, const V3& o, const V3& d, double r0, double r1, double t0, double t1) {
  return base::make_object({{"name", name},
                            {"type", "thread"},
                            {"axis", base::make_object({{"o", V::value(o)}, {"d", V::value(V::norm(d))}})},
                            {"r0", r0},
                            {"r1", r1},
                            {"t0", t0},
                            {"t1", t1}});
}
Value within(const Value& s, const V3& o, const V3& d, double r) {
  Value out = jsx::spread(s);
  out.set("within", base::make_object({{"o", V::value(o)}, {"d", V::value(V::norm(d))}, {"r", r}}));
  return out;
}
Value span(const Value& s, const V3& a, const V3& b) {
  const V3 d = V::sub(b, a);
  const double len = V::len(d);
  Value out = jsx::spread(s);
  out.set("span", base::make_object({{"a", V::value(a)}, {"d", V::value(V::mul(d, 1 / len))}, {"len", len}}));
  return out;
}

Value move_surfs(const Value& surfs, const V::M4& m, const std::string& suffix) {
  if (!surfs.truthy()) {
    return nullptr;
  }
  if (!surfs.is_array()) {
    throw std::runtime_error("surfs.map is not a function");
  }
  base::Array out;
  for (const Value& s0 : surfs.as_array()) {
    if (s0.is_null() || s0.is_undefined()) {
      ops::throw_read(s0, "name");
    }
    Value s = jsx::spread(s0);
    const Value n0 = s.get("name");
    const Value name = n0.truthy() ? Value(base::js_string(n0) + suffix) : n0;
    const Value w = s.get("within");
    const Value sp = s.get("span");
    if (w.truthy()) {
      Value x = jsx::spread(w);
      x.set("o", V::value(V::apply_mat(m, V::v3(w.get("o")))));
      x.set("d", V::value(V::norm(V::apply_dir(m, V::v3(w.get("d"))))));
      s.set("within", std::move(x));
    }
    if (sp.truthy()) {
      Value x = jsx::spread(sp);
      x.set("a", V::value(V::apply_mat(m, V::v3(sp.get("a")))));
      x.set("d", V::value(V::norm(V::apply_dir(m, V::v3(sp.get("d"))))));
      s.set("span", std::move(x));
    }
    s.set("name", name);
    switch (type_of(s.get("type"))) {
      case Type::kPlane:
        s.set("n", V::value(V::norm(V::apply_dir(m, V::v3(s.get("n"))))));
        s.set("o", V::value(V::apply_mat(m, V::v3(s.get("o")))));
        break;
      case Type::kCylinder:
      case Type::kCone:
      case Type::kTorus:
      case Type::kThread: {
        const Value ax = s.get("axis");
        if (ax.is_null() || ax.is_undefined()) {
          ops::throw_read(ax, "o");
        }
        s.set("axis", base::make_object({{"o", V::value(V::apply_mat(m, V::v3(ax.get("o"))))},
                                         {"d", V::value(V::norm(V::apply_dir(m, V::v3(ax.get("d")))))}}));
        break;
      }
      case Type::kSphere: s.set("c", V::value(V::apply_mat(m, V::v3(s.get("c"))))); break;
      case Type::kOther: break;
    }
    out.push_back(std::move(s));
  }
  return out;
}

// ---- namedFaces

Named named_faces(const Mesh& mesh, const std::vector<std::int32_t>& tri_tool, const Value& tool_surfs,
                  double segments) {
  const double sag = 1.5 * (1 - base::js_cos(std::numbers::pi / js_max(8, segments)));
  const auto& P = mesh.positions;
  const auto& I = mesh.indices;
  const std::size_t nt = I.size() / 3;
  const auto pv = [&](double i) {
    if (!(i >= 0) || i >= static_cast<double>(P.size())) {
      return kNaN;
    }
    return static_cast<double>(P[static_cast<std::size_t>(i)]);
  };
  const auto pt = [&](double i) { return V3{pv(i * 3), pv(i * 3 + 1), pv(i * 3 + 2)}; };
  V3 lo{kInf, kInf, kInf};
  V3 hi{-kInf, -kInf, -kInf};
  for (std::size_t i = 0; i < P.size(); i += 3) {
    for (std::size_t k = 0; k < 3; ++k) {
      const double x = pv(static_cast<double>(i + k));
      lo.at(k) = js_min(lo.at(k), x);
      hi.at(k) = js_max(hi.at(k), x);
    }
  }
  const double size = js_max(1e-9, base::js_hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]));
  const double tol = 2e-6 * size + 1e-7;

  // the tools' surface lists, parsed once (null: not a list)
  const base::Array* lists = tool_surfs.is_array() ? &tool_surfs.as_array() : nullptr;
  std::vector<std::optional<std::vector<Surf>>> parsed(lists != nullptr ? lists->size() : 0);
  const auto surfs_of = [&](std::int64_t ti) -> const std::vector<Surf>* {
    if (ti < 0 || lists == nullptr || static_cast<std::size_t>(ti) >= lists->size()) {
      return nullptr;
    }
    const auto u = static_cast<std::size_t>(ti);
    const Value& list = (*lists)[u];
    if (!list.truthy()) {
      return nullptr;
    }
    if (!list.is_array()) {
      throw std::runtime_error("surfs.findIndex is not a function");
    }
    if (!parsed[u]) {
      std::vector<Surf> v;
      v.reserve(list.as_array().size());
      for (const Value& s : list.as_array()) {
        v.push_back(parse(s));
      }
      parsed[u] = std::move(v);
    }
    return &*parsed[u];
  };
  // triTool[t]: past the end is undefined (no tool, and never equal to a tool)
  constexpr std::int64_t kNone = std::int64_t{1} << 40;
  const auto tool = [&](std::size_t t) -> std::int64_t { return t < tri_tool.size() ? tri_tool[t] : kNone; };

  // 1. each triangle → (tool, surface index)
  std::vector<std::int32_t> surf_of(nt, -1);
  std::vector<std::pair<std::int64_t, std::size_t>> surf_key;
  std::unordered_map<std::string, std::size_t> key_index;
  std::vector<double> tri_area(nt);
  std::vector<V3> tri_cen(nt);
  std::vector<V3> tri_n(nt);
  for (std::size_t t = 0; t < nt; ++t) {
    const V3 a = pt(I[t * 3]);
    const V3 b = pt(I[t * 3 + 1]);
    const V3 c = pt(I[t * 3 + 2]);
    const V3 cr = V::cross(V::sub(b, a), V::sub(c, a));
    const double ln = hyp(cr);
    tri_area[t] = ln / 2;
    for (std::size_t k = 0; k < 3; ++k) {
      tri_cen[t].at(k) = (a.at(k) + b.at(k) + c.at(k)) / 3;
      tri_n[t].at(k) = ln > 0 ? cr.at(k) / ln : 0;
    }
    const std::int64_t ti = tool(t);
    const std::vector<Surf>* surfs = ti >= 0 && ti != kNone ? surfs_of(ti) : nullptr;
    if (surfs == nullptr) {
      continue;
    }
    const double longest = js_max(js_max(hyp(V::sub(b, a)), hyp(V::sub(c, b))), hyp(V::sub(a, c)));
    std::optional<V3> n;
    if (ln > 1e-6 * longest * longest) {
      n = tri_n[t];
    }
    const Tri tri{a, b, c};
    std::ptrdiff_t hit = -1;
    for (const Type want : {Type::kPlane, Type::kCylinder, Type::kCone, Type::kSphere, Type::kTorus, Type::kThread}) {
      for (std::size_t i = 0; i < surfs->size(); ++i) {
        if ((*surfs)[i].type == want && on_surface((*surfs)[i], tri, n, tol, sag)) {
          hit = static_cast<std::ptrdiff_t>(i);
          break;
        }
      }
      if (hit >= 0) {
        break;
      }
    }
    if (hit < 0) {
      continue;
    }
    const std::string key = std::to_string(ti) + ':' + std::to_string(hit);
    auto [it, fresh] = key_index.try_emplace(key, surf_key.size());
    if (fresh) {
      surf_key.emplace_back(ti, static_cast<std::size_t>(hit));
    }
    surf_of[t] = static_cast<std::int32_t>(it->second);
  }
  // 1b. what is left joins an edge-neighbour's surface of the same tool
  EdgeTris edge_tris;
  for (std::size_t t = 0; t < nt; ++t) {
    for (std::size_t k = 0; k < 3; ++k) {
      edge_tris.add(ekey(I[t * 3 + k], I[t * 3 + (k + 1) % 3]), t);
    }
  }
  for (int pass = 0; pass < 4; ++pass) {
    int changed = 0;
    for (std::size_t t = 0; t < nt; ++t) {
      const std::int64_t ti = tool(t);
      if (surf_of[t] >= 0 || ti < 0) {  // kNone (undefined) is not < 0
        continue;
      }
      bool found = false;
      for (std::size_t k = 0; k < 3 && !found; ++k) {
        const auto* near = edge_tris.get(ekey(I[t * 3 + k], I[t * 3 + (k + 1) % 3]));
        if (near == nullptr) {
          continue;
        }
        for (const std::size_t q : *near) {
          if (q != t && surf_of[q] >= 0 && surf_key[static_cast<std::size_t>(surf_of[q])].first == ti) {
            surf_of[t] = surf_of[q];
            ++changed;
            found = true;
            break;
          }
        }
      }
    }
    if (changed == 0) {
      break;
    }
  }
  // 2. connected triangles on one surface → one face
  std::vector<std::size_t> parent(nt);
  for (std::size_t i = 0; i < nt; ++i) {
    parent[i] = i;
  }
  const auto find = [&](std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  for (const auto& tris : edge_tris.lists) {
    if (tris.size() != 2) {
      continue;
    }
    const std::size_t p = tris[0];
    const std::size_t q = tris[1];
    if (surf_of[p] >= 0 && surf_of[p] == surf_of[q]) {
      const std::size_t rp = find(p);
      const std::size_t rq = find(q);
      if (rp != rq) {
        parent[rp] = rq;
      }
    }
  }
  struct Acc {
    std::int64_t ti;
    std::size_t k;
    double area = 0, cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0;
  };
  Named out;
  out.tri_face.assign(nt, -1);
  std::vector<Acc> faces;
  std::unordered_map<std::size_t, std::size_t> face_of_root;
  for (std::size_t t = 0; t < nt; ++t) {
    if (surf_of[t] < 0) {
      continue;
    }
    const std::size_t r = find(t);
    auto [it, fresh] = face_of_root.try_emplace(r, faces.size());
    if (fresh) {
      const auto& sk = surf_key[static_cast<std::size_t>(surf_of[t])];
      faces.push_back({sk.first, sk.second});
    }
    const std::size_t fi = it->second;
    out.tri_face[t] = static_cast<std::int32_t>(fi);
    Acc& f = faces[fi];
    const double A = tri_area[t];
    f.area += A;
    f.cx += tri_cen[t][0] * A;
    f.cy += tri_cen[t][1] * A;
    f.cz += tri_cen[t][2] * A;
    f.nx += tri_n[t][0] * A;
    f.ny += tri_n[t][1] * A;
    f.nz += tri_n[t][2] * A;
  }
  // the records, and what the edges read of them
  struct FaceInfo {
    Value name;  // its name, or null
    Type type;
    bool has_axis;
    V3 ao, ad;
    double r;
  };
  std::vector<FaceInfo> info;
  base::Array recs;
  for (const Acc& f : faces) {
    const Value& raw = (*lists)[static_cast<std::size_t>(f.ti)].as_array()[f.k];
    const Surf& s = (*parsed[static_cast<std::size_t>(f.ti)])[f.k];
    const V3 at = f.area > 0 ? V3{f.cx / f.area, f.cy / f.area, f.cz / f.area} : V3{0, 0, 0};
    const Value nm = raw.get("name").truthy() ? raw.get("name") : Value(nullptr);
    Value rec = base::make_object({{"name", nm}, {"type", raw.get("type")}, {"area", f.area}, {"at", V::value(at)}});
    const Value ax = raw.get("axis");
    const auto axis = [&] { return base::make_object({{"o", copy3(ax.get("o"))}, {"d", copy3(ax.get("d"))}}); };
    FaceInfo fi{nm, s.type, false, s.ao, s.ad, s.r};
    switch (s.type) {
      case Type::kPlane: {
        const double sign = V::dot({f.nx, f.ny, f.nz}, s.n) < 0 ? -1 : 1;
        rec.set("n", V::value(V::mul(s.n, sign)));
        rec.set("o", copy3(raw.get("o")));
        break;
      }
      case Type::kCylinder:
        rec.set("axis", axis());
        rec.set("r", raw.get("r"));
        fi.has_axis = true;
        break;
      case Type::kCone:
        rec.set("axis", axis());
        rec.set("surf", base::make_object({{"type", "cone"},
                                           {"o", copy3(ax.get("o"))},
                                           {"d", copy3(ax.get("d"))},
                                           {"r0", 0.0},
                                           {"k", raw.get("k")}}));
        fi.has_axis = true;
        break;
      case Type::kSphere:
        rec.set("surf", base::make_object({{"type", "sphere"}, {"c", copy3(raw.get("c"))}, {"r", raw.get("r")}}));
        break;
      case Type::kThread:
        rec.set("axis", axis());
        rec.set("r", raw.get("r1"));
        break;
      case Type::kTorus:
        rec.set("axis", axis());
        rec.set("surf", base::make_object({{"type", "torus"},
                                           {"c", copy3(ax.get("o"))},
                                           {"d", copy3(ax.get("d"))},
                                           {"R", raw.get("R")},
                                           {"r", raw.get("r")}}));
        fi.has_axis = true;
        break;
      case Type::kOther: break;
    }
    info.push_back(std::move(fi));
    recs.push_back(std::move(rec));
  }

  // 3. edges between two faces, chained per face pair
  std::vector<std::string> pair_keys;
  std::vector<std::vector<std::pair<double, double>>> pair_lists;
  std::unordered_map<std::string, std::size_t> pair_index;
  for (std::size_t e = 0; e < edge_tris.keys.size(); ++e) {
    const auto& tris = edge_tris.lists[e];
    if (tris.size() != 2) {
      continue;
    }
    const std::int32_t fa = out.tri_face[tris[0]];
    const std::int32_t fb = out.tri_face[tris[1]];
    if (fa < 0 || fb < 0 || fa == fb) {
      continue;
    }
    const double key = edge_tris.keys[e];
    const double u = std::floor(key / k2p32);
    const double v = std::fmod(key, k2p32);
    const std::string pk =
        fa < fb ? std::to_string(fa) + '|' + std::to_string(fb) : std::to_string(fb) + '|' + std::to_string(fa);
    auto [it, fresh] = pair_index.try_emplace(pk, pair_keys.size());
    if (fresh) {
      pair_keys.push_back(pk);
      pair_lists.emplace_back();
    }
    pair_lists[it->second].emplace_back(u, v);
  }
  base::Array edges;
  for (std::size_t pi = 0; pi < pair_keys.size(); ++pi) {
    const std::string& pk = pair_keys[pi];
    const auto bar = pk.find('|');
    const auto fa = static_cast<std::size_t>(std::stoul(pk.substr(0, bar)));
    const auto fb = static_cast<std::size_t>(std::stoul(pk.substr(bar + 1)));
    const auto& list = pair_lists[pi];
    std::unordered_map<double, std::vector<std::pair<double, std::size_t>>> adj;
    for (std::size_t i = 0; i < list.size(); ++i) {
      adj[list[i].first].emplace_back(list[i].second, i);
      adj[list[i].second].emplace_back(list[i].first, i);
    }
    const auto adj_of = [&](double x) -> const std::vector<std::pair<double, std::size_t>>* {
      auto it = adj.find(x);
      return it == adj.end() ? nullptr : &it->second;
    };
    std::vector<std::uint8_t> used(list.size(), 0);
    for (std::size_t s0 = 0; s0 < list.size(); ++s0) {
      if (used[s0] != 0) {
        continue;
      }
      std::vector<double> stack{list[s0].first};
      std::vector<double> seen_order{list[s0].first};
      std::unordered_set<double> seen{list[s0].first};
      while (!stack.empty()) {
        const double x = stack.back();
        stack.pop_back();
        if (const auto* nb = adj_of(x)) {
          for (const auto& [y, i] : *nb) {
            if (seen.insert(y).second) {
              seen_order.push_back(y);
              stack.push_back(y);
            }
          }
        }
      }
      double start = list[s0].first;
      for (const double x : seen_order) {
        const auto* nb = adj_of(x);
        if ((nb != nullptr ? nb->size() : 0) == 1) {
          start = x;
          break;
        }
      }
      std::vector<double> chain{start};
      for (double cur = start;;) {
        const auto* nb = adj_of(cur);
        const std::pair<double, std::size_t>* step = nullptr;
        if (nb != nullptr) {
          for (const auto& st : *nb) {
            if (used[st.second] == 0) {
              step = &st;
              break;
            }
          }
        }
        if (step == nullptr) {
          break;
        }
        used[step->second] = 1;
        cur = step->first;
        chain.push_back(cur);
      }
      if (chain.size() < 2) {
        continue;
      }
      std::vector<V3> pts;
      pts.reserve(chain.size());
      for (const double x : chain) {
        pts.push_back(pt(x));
      }
      double len = 0;
      std::vector<double> cum{0};
      for (std::size_t i = 1; i < pts.size(); ++i) {
        len += hyp(V::sub(pts[i], pts[i - 1]));
        cum.push_back(len);
      }
      std::size_t mi = 1;
      while (mi < cum.size() - 1 && cum[mi] < len / 2) {
        ++mi;
      }
      const double f0 = (cum[mi] - len / 2) / js_max(1e-30, cum[mi] - cum[mi - 1]);
      V3 at = V::add(V::mul(pts[mi - 1], f0), V::mul(pts[mi], 1 - f0));
      Value na = info[fa].name.truthy() ? info[fa].name : Value("?");
      Value nb = info[fb].name.truthy() ? info[fb].name : Value("?");
      if (str_less(nb, na)) {
        std::swap(na, nb);
      }
      const std::string key = base::js_string(na) + '|' + base::js_string(nb);
      V3 a = pts.front();
      V3 b = pts.back();
      std::string type = "curve";
      Value circ;
      V3 cc{};
      V3 cn{};
      double cr = 0;
      const V3 dir = V::sub(pts.back(), pts.front());
      const double dl = hyp(dir);
      const auto straight = [&] {
        return std::ranges::all_of(
            pts, [&](const V3& p) { return hyp(V::cross(V::sub(p, pts[0]), V::mul(dir, 1 / dl))) <= tol * 10; });
      };
      if (dl > tol && straight()) {
        type = "line";
      } else {
        const FaceInfo& A = info[fa];
        const FaceInfo& B = info[fb];
        const FaceInfo* src = nullptr;
        if (A.has_axis) {
          src = &A;
        } else if (B.has_axis) {
          src = &B;
        }
        bool done = false;
        if (src != nullptr) {
          std::vector<std::pair<double, double>> rh;
          rh.reserve(pts.size());
          double rmax = -kInf;
          for (const V3& p : pts) {
            rh.push_back(radial(p, src->ao, src->ad));
            rmax = js_max(rmax, rh.back().first);
          }
          const bool round = std::ranges::all_of(rh, [&](const auto& x) {
            return x.first <= rmax + tol && x.first >= rmax - tol * 10 - sag * rmax &&
                   std::abs(x.second - rh[0].second) <= tol * 10;
          });
          if (round && rmax > tol) {
            const bool cyl = src->type == Type::kCylinder;
            type = "circle";
            cc = V::add(src->ao, V::mul(src->ad, rh[0].second));
            cn = src->ad;
            cr = cyl && std::abs(src->r - rmax) <= tol * 10 + sag * src->r ? src->r : rmax;
            done = true;
          }
        }
        if (!done && chain.front() == chain.back() && pts.size() > 4) {
          const std::vector<V3> ring(pts.begin(), pts.end() - 1);
          V3 sum{0, 0, 0};
          for (const V3& p : ring) {
            sum = V::add(sum, p);
          }
          const auto rn = static_cast<double>(ring.size());
          const V3 c = V::mul(sum, 1 / rn);
          double rs = 0;
          for (const V3& p : ring) {
            rs = rs + hyp(V::sub(p, c));
          }
          const double r = rs / rn;
          const std::size_t k = ring.size() / 4;
          const V3 n = V::norm(V::cross(V::sub(ring[0], c), V::sub(ring[k], c)));
          if (r > tol && std::ranges::all_of(ring, [&](const V3& p) {
                return std::abs(hyp(V::sub(p, c)) - r) <= tol * 10 && std::abs(V::dot(V::sub(p, c), n)) <= tol * 10;
              })) {
            type = "circle";
            cc = c;
            cn = n;
            cr = r;
          }
        }
      }
      if (type == "circle") {
        const auto on_c = [&](const V3& p) {
          const V3 v = V::sub(p, cc);
          const V3 w = V::sub(v, V::mul(cn, V::dot(v, cn)));
          const double wl = hyp(w);
          return wl > 0 ? V::add(cc, V::mul(w, cr / wl)) : p;
        };
        const bool closed = chain.front() == chain.back();
        a = on_c(a);
        b = closed ? a : on_c(b);
        at = closed ? V::sub(V::mul(cc, 2), a) : on_c(at);
        circ = base::make_object({{"c", V::value(cc)}, {"n", V::value(cn)}, {"r", cr}});
      }
      Value rec = base::make_object({{"faces", base::Array{na, nb}},
                                     {"key", key},
                                     {"at", V::value(at)},
                                     {"a", V::value(a)},
                                     {"b", V::value(b)},
                                     {"len", len},
                                     {"type", type}});
      if (!circ.is_undefined()) {
        rec.set("circ", std::move(circ));
      }
      const auto ei = static_cast<std::int32_t>(edges.size());
      edges.push_back(std::move(rec));
      for (std::size_t i = 1; i < pts.size(); ++i) {
        for (const V3* q : {&pts[i - 1], &pts[i]}) {
          for (const double x : *q) {
            out.edge_segs.push_back(static_cast<float>(x));
          }
        }
        out.seg_edge.push_back(ei);
      }
    }
  }
  out.faces = std::move(recs);
  out.edges = std::move(edges);
  return out;
}

Value named_value(const Named& n) {
  base::Array tf;
  tf.reserve(n.tri_face.size());
  for (const std::int32_t x : n.tri_face) {
    tf.emplace_back(static_cast<double>(x));
  }
  base::Array segs;
  segs.reserve(n.edge_segs.size());
  for (const float x : n.edge_segs) {
    segs.emplace_back(static_cast<double>(x));
  }
  base::Array se;
  se.reserve(n.seg_edge.size());
  for (const std::int32_t x : n.seg_edge) {
    se.emplace_back(static_cast<double>(x));
  }
  return base::make_object({{"triFace", std::move(tf)},
                            {"faces", n.faces},
                            {"edges", n.edges},
                            {"edgeSegs", std::move(segs)},
                            {"segEdge", std::move(se)}});
}

}  // namespace cc::partcraft::meshfaces
