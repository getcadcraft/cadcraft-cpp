// SPDX-License-Identifier: Apache-2.0
// io.js's exports (CadCraft 0.5): GLB, OBJ + MTL, STL, DXF R12 and the cc-dwg model, Collada, IFC4 — the same bytes
// as the JS for the same model (tools/parity/m3d-io.mjs). Numbers are written as the JS writes them (toFixed,
// toPrecision, String(number), JSON.stringify), and every write happens in the JS's order (its template literals
// evaluate their nested calls left to right: here they are separate statements).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/base/value.hpp"
#include "cc/m3d/io.hpp"
#include "io_internal.hpp"

namespace cc::m3d::io {

using base::Array;
using base::Value;

namespace detail {

std::string num_text(double x) {
  std::string s;
  base::append_number(s, x);
  return s;
}

double to_precision(double x, int p) {
  if (!std::isfinite(x)) {
    return x;
  }
  const std::string e = base::js_to_exponential(x, p - 1);
  return std::strtod(e.c_str(), nullptr);
}

std::string xp(std::string_view c) {
  return is_image_paint(c) ? paint_hex(c) : std::string(c);
}

std::string f6(double x) {
  double v = std::strtod(base::js_to_fixed(x, 6).c_str(), nullptr);
  if (v == 0) {
    return "0";
  }
  return num_text(v);
}

std::string replace_spaces(std::string_view s) {
  // .replace(/\s+/g, '_')
  std::string out;
  std::size_t i = 0;
  bool in_space = false;
  while (i < s.size()) {
    const std::size_t n = base::js_space_at(s, i);
    if (n != 0) {
      if (!in_space) {
        out += '_';
      }
      in_space = true;
      i += n;
      continue;
    }
    in_space = false;
    out += s[i++];
  }
  return out;
}

}  // namespace detail

using namespace detail;  // NOLINT(google-build-using-namespace): this library's own helpers

namespace {

constexpr Vec3 zup_to_yup(const Vec3& p) {
  return {p[0], p[2], -p[1]};
}

bool is_ident_io(const Mat4& m) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (!(std::abs(m[i] - (i % 5 == 0 ? 1.0 : 0.0)) <
          1e-12)) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      return false;
    }
  }
  return true;
}

double srgb_to_lin(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

std::array<double, 3> hex_to_linear(std::string_view hex) {
  const std::string h = paint_hex(hex);
  const auto n = static_cast<std::uint32_t>(std::strtoul(h.substr(1).c_str(), nullptr, 16));
  return {srgb_to_lin(((n >> 16U) & 255U) / 255.0), srgb_to_lin(((n >> 8U) & 255U) / 255.0),
          srgb_to_lin((n & 255U) / 255.0)};
}

struct Tri {
  std::array<Vec3, 3> p;
  Vec3 n{};
  std::string color;  // '' none
  std::optional<std::array<Vec3, 3>> ns;
};

/// defTris: the triangles of one definition's own faces, with flat normals (and per-corner ones where soft edges join).
std::vector<Tri> def_tris(const Def& def, const std::string& dflt) {
  std::vector<Tri> out;
  const auto smooth = smooth_normals(def);
  for (const auto& [fid, f] : def.faces) {
    const Vec3 n = face_normal(def, f);
    if (len(n) == 0) {
      continue;
    }
    for (const auto& t : face_tris(def, f)) {
      Tri tri{{def.vert(t[0]), def.vert(t[1]), def.vert(t[2])}, n, xp(!f.color.empty() ? f.color : dflt), std::nullopt};
      if (!smooth.empty()) {
        const auto has = [&](Id v) { return smooth.contains({fid, v}); };
        if (has(t[0]) || has(t[1]) || has(t[2])) {
          std::array<Vec3, 3> ns{};
          for (std::size_t k = 0; k < 3; ++k) {
            const auto it = smooth.find({fid, t.at(k)});
            ns.at(k) = it != smooth.end() ? it->second : n;
          }
          tri.ns = ns;
        }
      }
      out.push_back(std::move(tri));
    }
  }
  return out;
}

/// flatTris: the triangles of a definition and everything inside it, in its coordinates.
std::vector<Tri> flat_tris(const Model& model, const Def& def) {
  std::vector<Tri> out;
  model.walk(
      [&](const Def& d, const Mat4& wm, const std::vector<Id>& /*path*/, const std::string& pc) {
        const bool mirror = det3(wm) < 0;
        for (const Tri& t : def_tris(d, pc)) {
          std::array<Vec3, 3> ps = {m3d::apply(wm, t.p[0]), m3d::apply(wm, t.p[1]), m3d::apply(wm, t.p[2])};
          if (mirror) {
            std::swap(ps[1], ps[2]);
          }
          out.push_back({ps, norm(cross(sub(ps[1], ps[0]), sub(ps[2], ps[0]))), t.color, std::nullopt});
        }
        return true;
      },
      &def);
  return out;
}

void put_u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i) {
    b.at(at + i) = static_cast<std::uint8_t>((v >> (8U * i)) & 255U);
  }
}
void put_f32(std::vector<std::uint8_t>& b, std::size_t at, double v) {
  put_u32(b, at, std::bit_cast<std::uint32_t>(static_cast<float>(v)));
}
void append_f32(std::vector<std::uint8_t>& b, float v) {
  const auto u = std::bit_cast<std::uint32_t>(v);
  for (std::size_t i = 0; i < 4; ++i) {
    b.push_back(static_cast<std::uint8_t>((u >> (8U * i)) & 255U));
  }
}
void append_u32(std::vector<std::uint8_t>& b, std::uint32_t u) {
  for (std::size_t i = 0; i < 4; ++i) {
    b.push_back(static_cast<std::uint8_t>((u >> (8U * i)) & 255U));
  }
}

bool edge_has_face(const std::unordered_map<std::uint64_t, std::vector<Id>>& ef, const Edge& e) {
  return ef.contains(edge_key(e.a, e.b));
}

}  // namespace

// ---------------------------------------------------------------------------- glTF binary

std::vector<std::uint8_t> export_glb(const Model& model) {
  Array nodes;
  Array meshes;
  Array materials;
  Array accessors;
  Array views;
  std::vector<std::uint8_t> bin;
  const auto push_view = [&](const std::vector<std::uint8_t>& bytes, int target) {
    while (bin.size() % 4 != 0) {
      bin.push_back(0);
    }
    views.push_back(base::make_object(
        {{"buffer", 0}, {"byteOffset", Value(bin.size())}, {"byteLength", Value(bytes.size())}, {"target", target}}));
    bin.insert(bin.end(), bytes.begin(), bytes.end());
    return views.size() - 1;
  };
  std::map<std::string, std::size_t> mat_index;
  const auto material = [&](const std::string& color) {
    const auto it = mat_index.find(color);
    if (it != mat_index.end()) {
      return it->second;
    }
    Value pbr = base::make_object({{"metallicFactor", 0}, {"roughnessFactor", 0.9}});
    if (!color.empty()) {
      const auto l = hex_to_linear(color);
      pbr.set("baseColorFactor", Array{l[0], l[1], l[2], 1});
    }
    std::string name = "Default";
    if (!color.empty()) {
      name = is_material_paint(color) ? "Material " + color.substr(4) : "Colour " + color;
    }
    materials.push_back(
        base::make_object({{"name", name}, {"pbrMetallicRoughness", std::move(pbr)}, {"doubleSided", true}}));
    mat_index.emplace(color, materials.size() - 1);
    return materials.size() - 1;
  };
  std::map<std::string, std::optional<std::size_t>> mesh_of;
  const auto mesh_for = [&](const Def& def, const std::string& dflt) -> std::optional<std::size_t> {
    const std::string key = def.id + "|" + dflt;
    if (const auto it = mesh_of.find(key); it != mesh_of.end()) {
      return it->second;
    }
    const std::vector<Tri> tris = def_tris(def, dflt);
    if (tris.empty()) {
      mesh_of.emplace(key, std::nullopt);
      return std::nullopt;
    }
    OrderedMap<std::string, std::vector<const Tri*>> by_color;
    for (const Tri& t : tris) {
      std::vector<const Tri*>* l = by_color.get(t.color);
      if (l == nullptr) {
        l = &by_color.set(t.color, {});
      }
      l->push_back(&t);
    }
    Array prims;
    for (const auto& [k, list] : by_color) {
      const std::size_t n = list.size() * 3;
      std::vector<float> pos(n * 3);
      std::vector<float> nor(n * 3);
      std::vector<std::uint8_t> idx;
      idx.reserve(n * 4);
      for (std::size_t i = 0; i < list.size(); ++i) {
        const Tri& t = *list[i];
        for (std::size_t k2 = 0; k2 < 3; ++k2) {
          const std::size_t o = (i * 3 + k2) * 3;
          const Vec3& p = t.p.at(k2);
          pos[o] = static_cast<float>(p[0]);
          pos[o + 1] = static_cast<float>(p[1]);
          pos[o + 2] = static_cast<float>(p[2]);
          const Vec3& q = t.ns ? t.ns->at(k2) : t.n;
          nor[o] = static_cast<float>(q[0]);
          nor[o + 1] = static_cast<float>(q[1]);
          nor[o + 2] = static_cast<float>(q[2]);
          append_u32(idx, static_cast<std::uint32_t>(i * 3 + k2));
        }
      }
      std::array<double, 3> mn = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::infinity()};
      std::array<double, 3> mx = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity()};
      for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t a = 0; a < 3; ++a) {
          const auto v = static_cast<double>(pos[i * 3 + a]);
          mn.at(a) = std::min(mn.at(a), v);
          mx.at(a) = std::max(mx.at(a), v);
        }
      }
      std::vector<std::uint8_t> pb;
      pb.reserve(pos.size() * 4);
      for (const float f : pos) {
        append_f32(pb, f);
      }
      std::vector<std::uint8_t> nb;
      nb.reserve(nor.size() * 4);
      for (const float f : nor) {
        append_f32(nb, f);
      }
      const std::size_t pv = push_view(pb, 34962);
      const std::size_t nv = push_view(nb, 34962);
      const std::size_t iv = push_view(idx, 34963);
      accessors.push_back(base::make_object({{"bufferView", Value(pv)},
                                             {"componentType", 5126},
                                             {"count", Value(n)},
                                             {"type", "VEC3"},
                                             {"min", Array{mn[0], mn[1], mn[2]}},
                                             {"max", Array{mx[0], mx[1], mx[2]}}}));
      const std::size_t pa = accessors.size() - 1;
      accessors.push_back(base::make_object(
          {{"bufferView", Value(nv)}, {"componentType", 5126}, {"count", Value(n)}, {"type", "VEC3"}}));
      const std::size_t na = accessors.size() - 1;
      accessors.push_back(base::make_object(
          {{"bufferView", Value(iv)}, {"componentType", 5125}, {"count", Value(n)}, {"type", "SCALAR"}}));
      const std::size_t ia = accessors.size() - 1;
      const std::size_t mat = material(k);
      prims.push_back(
          base::make_object({{"attributes", base::make_object({{"POSITION", Value(pa)}, {"NORMAL", Value(na)}})},
                             {"indices", Value(ia)},
                             {"material", Value(mat)},
                             {"mode", 4}}));
    }
    meshes.push_back(base::make_object({{"name", def.name}, {"primitives", std::move(prims)}}));
    mesh_of.emplace(key, meshes.size() - 1);
    return meshes.size() - 1;
  };
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as io.js
  const std::function<std::size_t(const Def&, const Mat4*, const std::string&, int, const std::string&)> node_for =
      [&](const Def& def, const Mat4* m, const std::string& name, int depth, const std::string& dflt) {
        Value node = base::make_object({{"name", name}});
        if (m != nullptr && !is_ident_io(*m)) {
          Array mm;
          for (const double v : *m) {
            mm.emplace_back(std::abs(v) < 1e-15 ? 0.0 : v);
          }
          node.set("matrix", std::move(mm));
        }
        nodes.push_back(node);
        const std::size_t idx = nodes.size() - 1;
        if (const auto mesh = mesh_for(def, dflt)) {
          nodes[idx].set("mesh", Value(*mesh));
        }
        if (depth < 32) {
          Array kids;
          for (const auto& [iid, inst] : def.insts) {
            if (const Def* d = model.def(inst.def)) {
              const std::string nm = !inst.name.empty() ? inst.name : d->name;
              kids.emplace_back(node_for(*d, &inst.m, nm, depth + 1, !inst.color.empty() ? inst.color : dflt));
            }
          }
          if (!kids.empty()) {
            nodes[idx].set("children", std::move(kids));
          }
        }
        return idx;
      };
  nodes.push_back(base::make_object(
      {{"name", "ModelCraft (Z up → Y up)"}, {"matrix", Array{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1}}}));
  const std::size_t top =
      node_for(model.root(), nullptr, !model.root().name.empty() ? model.root().name : "Model", 0, {});
  nodes[0].set("children", Array{Value(top)});
  Value gltf = base::make_object({{"asset", base::make_object({{"version", "2.0"}, {"generator", "ModelCraft"}})},
                                  {"scene", 0},
                                  {"scenes", Array{base::make_object({{"nodes", Array{Value(0)}}})}},
                                  {"nodes", std::move(nodes)}});
  const bool any_mesh = !meshes.empty();
  if (any_mesh) {
    gltf.set("meshes", std::move(meshes));
  }
  if (!materials.empty()) {
    gltf.set("materials", std::move(materials));
  }
  std::size_t byte_length = bin.size();
  if (any_mesh) {
    gltf.set("accessors", std::move(accessors));
    gltf.set("bufferViews", std::move(views));
    gltf.set("buffers", Array{base::make_object({{"byteLength", Value(byte_length)}})});
  } else {
    byte_length = 0;
  }
  const std::string json = base::js_stringify(gltf);
  std::size_t jlen = json.size();
  while (jlen % 4 != 0) {
    ++jlen;
  }
  const std::size_t bin_len = (byte_length + 3) / 4 * 4;
  const std::size_t total = 12 + 8 + jlen + (byte_length != 0 ? 8 + bin_len : 0);
  std::vector<std::uint8_t> out(total, 0);
  put_u32(out, 0, 0x46546c67U);
  put_u32(out, 4, 2);
  put_u32(out, 8, static_cast<std::uint32_t>(total));
  put_u32(out, 12, static_cast<std::uint32_t>(jlen));
  put_u32(out, 16, 0x4e4f534aU);
  std::ranges::copy(json, out.begin() + 20);
  for (std::size_t i = 20 + json.size(); i < 20 + jlen; ++i) {
    out[i] = 0x20;
  }
  if (byte_length != 0) {
    std::size_t o = 20 + jlen;
    put_u32(out, o, static_cast<std::uint32_t>(bin_len));
    put_u32(out, o + 4, 0x004e4942U);
    o += 8;
    std::ranges::copy(bin, out.begin() + static_cast<std::ptrdiff_t>(o));
  }
  return out;
}

// ---------------------------------------------------------------------------- OBJ, MTL

namespace {

std::string mat_name(const std::string& c) {
  if (c.empty()) {
    return "Default";
  }
  if (is_material_paint(c)) {
    return "Material_" + c.substr(4);
  }
  return "Colour_" + base::js_to_lower(c.substr(1));
}

}  // namespace

std::string export_obj(const Model& model, std::optional<std::string_view> mtl) {
  std::vector<std::string> lines = {"# ModelCraft OBJ export — meters, Y up (converted from the model's Z up)"};
  if (mtl) {
    lines.push_back("mtllib " + replace_spaces(*mtl));
  }
  std::size_t v_base = 1;
  std::size_t n_base = 1;
  std::string cur_mat;
  bool have_mat = false;
  model.walk([&](const Def& d, const Mat4& wm, const std::vector<Id>& path, const std::string& pc) {
    const auto ef = edge_faces(d);
    std::vector<Edge> loose;
    for (const auto& [eid, e] : d.edges) {
      if (!edge_has_face(ef, e)) {
        loose.push_back(e);
      }
    }
    if (d.faces.empty() && loose.empty()) {
      return true;
    }
    const bool mirror = det3(wm) < 0;
    std::string name;
    if (!path.empty()) {
      name = d.name + " ";
      for (std::size_t i = 0; i < path.size(); ++i) {
        name += (i != 0 ? "." : "") + std::to_string(path[i]);
      }
    } else {
      name = !d.name.empty() ? d.name : "Model";
    }
    lines.push_back("o " + replace_spaces(name));
    std::unordered_map<Id, std::size_t> vi;
    std::vector<Vec3> vs;
    const auto V = [&](Id id) {
      if (const auto it = vi.find(id); it != vi.end()) {
        return it->second;
      }
      const std::size_t k = v_base + vs.size();
      vi.emplace(id, k);
      vs.push_back(zup_to_yup(m3d::apply(wm, d.vert(id))));
      return k;
    };
    struct FaceOut {
      std::vector<std::size_t> ids, ns;
      std::string mat;
    };
    std::vector<FaceOut> faces;
    std::vector<Vec3> norms;
    std::unordered_map<std::string, std::size_t> n_index;
    const auto N = [&](const Vec3& n) {
      const std::string k = f6(n[0]) + " " + f6(n[1]) + " " + f6(n[2]);
      if (const auto it = n_index.find(k); it != n_index.end()) {
        return it->second;
      }
      const std::size_t i = n_base + norms.size();
      n_index.emplace(k, i);
      norms.push_back(n);
      return i;
    };
    const auto smooth = smooth_normals(d);
    for (const auto& [fid, f] : d.faces) {
      std::vector<Vec3> pts;
      pts.reserve(f.loops[0].size());
      for (const Id v : f.loops[0]) {
        pts.push_back(m3d::apply(wm, d.vert(v)));
      }
      Vec3 nn{0, 0, 0};
      for (std::size_t i = 0; i < pts.size(); ++i) {
        nn = add(nn, cross(pts[i], pts[(i + 1) % pts.size()]));
      }
      std::optional<std::size_t> flat_i;
      const auto flat = [&]() {
        if (!flat_i) {
          flat_i = N(zup_to_yup(norm(mirror ? mul(nn, -1) : nn)));
        }
        return *flat_i;
      };
      std::vector<Loop> pieces;
      if (f.loops.size() == 1) {
        pieces.push_back(f.loops[0]);
      } else {
        for (const auto& t : face_tris(d, f)) {
          pieces.push_back({t[0], t[1], t[2]});
        }
      }
      for (Loop loop : pieces) {
        if (mirror) {
          std::ranges::reverse(loop);
        }
        FaceOut fo;
        for (const Id v : loop) {
          fo.ids.push_back(V(v));
        }
        for (const Id v : loop) {
          const auto it = smooth.find({fid, v});
          fo.ns.push_back(it != smooth.end() ? N(zup_to_yup(norm(apply_dir(wm, it->second)))) : flat());
        }
        if (mtl) {
          fo.mat = mat_name(xp(!f.color.empty() ? f.color : pc));
        }
        faces.push_back(std::move(fo));
      }
    }
    for (const Edge& e : loose) {
      (void)V(e.a);
      (void)V(e.b);
    }
    for (const Vec3& p : vs) {
      lines.push_back("v " + f6(p[0]) + " " + f6(p[1]) + " " + f6(p[2]));
    }
    for (const Vec3& n : norms) {
      lines.push_back("vn " + f6(n[0]) + " " + f6(n[1]) + " " + f6(n[2]));
    }
    for (const FaceOut& f : faces) {
      if (mtl && !f.mat.empty() && (!have_mat || f.mat != cur_mat)) {
        lines.push_back("usemtl " + f.mat);
        cur_mat = f.mat;
        have_mat = true;
      }
      std::string l = "f";
      for (std::size_t k = 0; k < f.ids.size(); ++k) {
        l += " " + std::to_string(f.ids[k]) + "//" + std::to_string(f.ns[k]);
      }
      lines.push_back(std::move(l));
    }
    for (const Edge& e : loose) {
      lines.push_back("l " + std::to_string(vi.at(e.a)) + " " + std::to_string(vi.at(e.b)));
    }
    v_base += vs.size();
    n_base += norms.size();
    return true;
  });
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      out += '\n';
    }
    out += lines[i];
  }
  return out + "\n";
}

std::string export_mtl(const Model& model) {
  std::set<std::string> used;  // [...used].sort(): code-unit order, and these are ASCII
  model.walk([&](const Def& d, const Mat4& /*wm*/, const std::vector<Id>& /*path*/, const std::string& pc) {
    for (const auto& [fid, f] : d.faces) {
      used.insert(xp(!f.color.empty() ? f.color : pc));
    }
    return true;
  });
  std::string out = "# ModelCraft materials";
  for (const std::string& c : used) {
    std::array<std::string, 3> rgb = {"0.800000", "0.800000", "0.800000"};
    if (!c.empty()) {
      const std::string h = paint_hex(c);
      for (std::size_t i = 0; i < 3; ++i) {
        const auto v = std::strtoul(h.substr(1 + 2 * i, 2).c_str(), nullptr, 16);
        rgb.at(i) = base::js_to_fixed(static_cast<double>(v) / 255, 6);
      }
    }
    out += "\n\nnewmtl " + mat_name(c) + "\nKd " + rgb[0] + " " + rgb[1] + " " + rgb[2] +
           "\nKa 0 0 0\nKs 0 0 0\nd 1\nillum 1";
  }
  return out + "\n";
}

// ---------------------------------------------------------------------------- STL

std::vector<std::uint8_t> export_stl(const Model& model) {
  const std::vector<Tri> tris = flat_tris(model, model.root());
  std::vector<std::uint8_t> out(84 + tris.size() * 50, 0);
  const std::string_view head = "ModelCraft STL, millimeters";
  std::ranges::copy(head, out.begin());
  put_u32(out, 80, static_cast<std::uint32_t>(tris.size()));
  std::size_t o = 84;
  for (const Tri& t : tris) {
    const std::array<Vec3, 4> vs = {t.n, mul(t.p[0], 1000), mul(t.p[1], 1000), mul(t.p[2], 1000)};
    for (const Vec3& v : vs) {
      put_f32(out, o, v[0]);
      put_f32(out, o + 4, v[1]);
      put_f32(out, o + 8, v[2]);
      o += 12;
    }
    o += 2;
  }
  return out;
}

}  // namespace cc::m3d::io
