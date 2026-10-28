// SPDX-License-Identifier: Apache-2.0
// io.js's DXF R12 / DWG model (cadScene), Collada and IFC4 exports.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
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
using namespace detail;  // NOLINT(google-build-using-namespace): this library's own helpers

namespace {

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

// ---- ACI, the AutoCAD colour index (the DXF reference's palette)
const std::array<std::array<int, 3>, 256>& aci() {
  static const std::array<std::array<int, 3>, 256> t = [] {
    std::array<std::array<int, 3>, 256> a{};
    const std::array<std::array<int, 3>, 9> first = {{{255, 0, 0},
                                                      {255, 255, 0},
                                                      {0, 255, 0},
                                                      {0, 255, 255},
                                                      {0, 0, 255},
                                                      {255, 0, 255},
                                                      {255, 255, 255},
                                                      {128, 128, 128},
                                                      {192, 192, 192}}};
    for (std::size_t i = 0; i < 9; ++i) {
      a.at(i + 1) = first.at(i);
    }
    const std::array<double, 5> VV = {255, 204, 153, 127, 76};
    for (int i = 10; i < 250; ++i) {
      const double hue = std::floor((i - 10) / 10.0) * 15;
      const int kk = (i - 10) % 10;
      const double v = VV.at(static_cast<std::size_t>(kk >> 1));
      const bool pastel = (kk & 1) != 0;
      const double h6 = hue / 60;
      const double f = h6 - std::floor(h6);
      const double lo = pastel ? v / 2 : 0;
      const double q = v - (v - lo) * f;
      const double u = lo + (v - lo) * f;
      const std::array<std::array<double, 3>, 6> opts = {
          {{v, u, lo}, {q, v, lo}, {lo, v, u}, {lo, q, v}, {u, lo, v}, {v, lo, q}}};
      const auto& c = opts.at(static_cast<std::size_t>(std::floor(h6)) % 6);
      a.at(static_cast<std::size_t>(i)) = {static_cast<int>(base::js_round(c[0])),
                                           static_cast<int>(base::js_round(c[1])),
                                           static_cast<int>(base::js_round(c[2]))};
    }
    const std::array<int, 6> greys = {51, 91, 132, 173, 214, 255};
    for (std::size_t i = 0; i < 6; ++i) {
      a.at(250 + i) = {greys.at(i), greys.at(i), greys.at(i)};
    }
    return a;
  }();
  return t;
}

bool hex6(std::string_view s) {
  if (s.starts_with('#')) {
    s.remove_prefix(1);
  }
  return s.size() == 6 && std::ranges::all_of(s, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         });
}

/// df(x): +(+x).toPrecision(15), -0 and |v| < 1e-12 as 0, as text (DXF)
std::string df(double x) {
  double v = to_precision(x, 15);
  if (v == 0 || std::abs(v) < 1e-12) {
    v = 0;
  }
  return num_text(v);
}

struct CadEnt {
  char t = 'f';  // f(ace) l(ine) i(nsert)
  std::array<Vec3, 4> pts{};
  int inv = 0;
  std::string color;  // '' null
  Vec3 a{}, b{};
  std::string block;
  Insert ins;
};
struct CadBlock {
  std::string name;
  std::vector<CadEnt> ents;
};
struct CadScene {
  std::string unit_name;
  int insunits = 6;
  std::vector<CadBlock> blocks;
  std::vector<CadEnt> ents;
  CadReport report;
};

base::Result<CadScene> cad_scene(const Model& model) {
  std::string unit_name = "meters";
  double k = 1;
  int insunits = 6;
  if (model.units == "ft-in" || model.units == "in") {
    unit_name = "inches";
    k = 1 / 0.0254;
    insunits = 1;
  } else if (model.units == "mm") {
    unit_name = "millimeters";
    k = 1000;
    insunits = 4;
  } else if (model.units == "cm") {
    unit_name = "centimeters";
    k = 100;
    insunits = 5;
  }
  CadScene sc;
  sc.unit_name = unit_name;
  sc.insunits = insunits;
  sc.report.unit = unit_name;
  std::map<std::string, std::string> names;
  std::unordered_set<std::string> used;
  const auto block_name = [&](const std::string& id, const std::string& nm) {
    if (const auto it = names.find(id); it != names.end()) {
      return it->second;
    }
    const std::string up = base::js_to_upper(!nm.empty() ? nm : id);
    std::string b;  // .replace(/[^A-Z0-9_$-]+/g, '_')
    bool run = false;
    for (const char c : up) {
      const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '-';
      if (ok) {
        b += c;
        run = false;
      } else if (!run) {
        b += '_';
        run = true;
      }
    }
    const std::size_t s0 = b.find_first_not_of('_');  // .replace(/^_+|_+$/g, '')
    b = s0 == std::string::npos ? std::string() : b.substr(s0, b.find_last_not_of('_') - s0 + 1);
    if (b.empty()) {
      b = "BLOCK";
    }
    b = b.substr(0, 26);
    std::string n = b;
    int i = 2;
    while (used.contains(n)) {
      n = b.substr(0, 26) + "_" + std::to_string(i++);
    }
    used.insert(n);
    names.emplace(id, n);
    return n;
  };
  std::unordered_set<std::string> done;
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most (walk-like), and a component is built once
  std::function<std::string(const Def&, const std::string&, int)> block_of;
  // NOLINTNEXTLINE(misc-no-recursion)
  std::function<std::vector<CadEnt>(const Def&, const Mat4&, const std::string&, int)> ents_of =
      [&](const Def& def, const Mat4& m, const std::string& pc, int depth) {
        std::vector<CadEnt> out;
        const auto P = [&](Id v) { return mul(m3d::apply(m, def.vert(v)), k); };
        const auto ef = edge_faces(def);
        std::unordered_set<std::uint64_t> soft;
        for (const auto& [eid, e] : def.edges) {
          if (e.soft) {
            soft.insert(edge_key(e.a, e.b));
          }
        }
        const bool mirror = det3(m) < 0;
        for (const auto& [fid, f] : def.faces) {
          ++sc.report.faces;
          std::unordered_set<std::uint64_t> real;
          for (const Loop& l : f.loops) {
            for (std::size_t i = 0; i < l.size(); ++i) {
              real.insert(edge_key(l[i], l[(i + 1) % l.size()]));
            }
          }
          for (auto t : face_tris(def, f)) {
            if (mirror) {
              std::swap(t[1], t[2]);
            }
            const auto vis = [&](Id a, Id b) {
              return real.contains(edge_key(a, b)) && !soft.contains(edge_key(a, b));
            };
            const int inv = (vis(t[0], t[1]) ? 0 : 1) | (vis(t[1], t[2]) ? 0 : 2) | 4 | (vis(t[2], t[0]) ? 0 : 8);
            CadEnt e;
            e.t = 'f';
            e.pts = {P(t[0]), P(t[1]), P(t[2]), P(t[2])};
            e.inv = inv;
            e.color = xp(!f.color.empty() ? f.color : pc);
            out.push_back(std::move(e));
            ++sc.report.triangles;
          }
        }
        for (const auto& [eid, e] : def.edges) {
          if (const auto it = ef.find(edge_key(e.a, e.b)); it != ef.end() && !it->second.empty()) {
            continue;
          }
          CadEnt l;
          l.t = 'l';
          l.a = P(e.a);
          l.b = P(e.b);
          out.push_back(std::move(l));
          ++sc.report.lines;
        }
        for (const auto& [iid, inst] : def.insts) {
          const Def* child = model.def(inst.def);
          if (child == nullptr || depth > 32) {
            continue;
          }
          const Mat4 wm = mmul(m, inst.m);
          const auto ins = insert_of(wm, k);
          const std::string cc = !inst.color.empty() ? inst.color : pc;
          if (ins) {
            CadEnt e;
            e.t = 'i';
            e.block = block_of(*child, cc, depth + 1);
            e.ins = *ins;
            out.push_back(std::move(e));
            ++sc.report.inserts;
          } else {
            ++sc.report.flattened;
            auto more = ents_of(*child, wm, cc, depth + 1);
            std::ranges::move(more, std::back_inserter(out));
          }
        }
        return out;
      };
  block_of = [&](const Def& def, const std::string& pc, int depth) {
    const std::string key = def.id + "|" + pc;
    const std::string name = block_name(key, !pc.empty() ? def.name + " " + pc.substr(1) : def.name);
    if (!done.contains(key)) {
      done.insert(key);
      ++sc.report.blocks;
      auto ents = ents_of(def, ident(), pc, depth);
      sc.blocks.push_back({name, std::move(ents)});
    }
    return name;
  };
  sc.ents = ents_of(model.root(), ident(), {}, 0);
  if (sc.report.triangles + sc.report.lines + sc.report.inserts > 2000000) {
    return base::fail(base::Errc::too_big, tr("m3d.io.this_model_is_too_big_to"));
  }
  return sc;
}

}  // namespace

int nearest_aci(std::string_view hex) {
  const std::string h = paint_hex(hex);
  if (!hex6(h)) {
    return 7;
  }
  const std::string_view s = std::string_view(h).substr(h.starts_with('#') ? 1 : 0);
  const auto ch = [&](std::size_t o) {
    return static_cast<int>(std::strtol(std::string(s.substr(o, 2)).c_str(), nullptr, 16));
  };
  const int r = ch(0);
  const int g = ch(2);
  const int b = ch(4);
  int best = 7;
  double bd = std::numeric_limits<double>::infinity();
  for (std::size_t i = 1; i < 256; ++i) {
    const auto& c = aci().at(i);
    const double d = std::pow(c[0] - r, 2) + std::pow(c[1] - g, 2) + std::pow(c[2] - b, 2);
    if (d < bd) {
      bd = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

std::optional<Insert> insert_of(const Mat4& m, double k) {
  std::array<Vec3, 3> c = {Vec3{m[0], m[1], m[2]}, Vec3{m[4], m[5], m[6]}, Vec3{m[8], m[9], m[10]}};
  Vec3 s = {len(c[0]), len(c[1]), len(c[2])};
  if (std::ranges::any_of(s, [](double x) { return !(x > 1e-12); })) {
    return std::nullopt;
  }
  std::array<Vec3, 3> r = {mul(c[0], 1 / s[0]), mul(c[1], 1 / s[1]), mul(c[2], 1 / s[2])};
  if (std::abs(dot(r[0], r[1])) > 1e-9 || std::abs(dot(r[0], r[2])) > 1e-9 || std::abs(dot(r[1], r[2])) > 1e-9) {
    return std::nullopt;
  }
  if (dot(cross(r[0], r[1]), r[2]) < 0) {
    s[2] = -s[2];
    r[2] = mul(r[2], -1);
  }
  const Vec3 n = r[2];
  const Vec3 ax =
      norm(std::abs(n[0]) < 1.0 / 64 && std::abs(n[1]) < 1.0 / 64 ? cross({0, 1, 0}, n) : cross({0, 0, 1}, n));
  const Vec3 ay = cross(n, ax);
  const Vec3 T = {m[12] * k, m[13] * k, m[14] * k};
  return Insert{{dot(T, ax), dot(T, ay), dot(T, n)}, s, base::js_atan2(dot(r[0], ay), dot(r[0], ax)), n};
}

base::Result<DxfExport> export_dxf(const Model& model) {
  auto sc = cad_scene(model);
  if (!sc) {
    return std::unexpected(sc.error());
  }
  std::vector<std::string> L;
  const auto w = [&](int c, const std::string& v) {
    L.push_back(std::to_string(c));
    L.push_back(v);
  };
  const auto ent = [&](const CadEnt& e) {
    if (e.t == 'f') {
      w(0, "3DFACE");
      w(8, "0");
      if (!e.color.empty()) {
        w(62, std::to_string(nearest_aci(e.color)));
      }
      for (int i = 0; i < 4; ++i) {
        const Vec3& p = e.pts.at(static_cast<std::size_t>(i));
        w(10 + i, df(p[0]));
        w(20 + i, df(p[1]));
        w(30 + i, df(p[2]));
      }
      w(70, std::to_string(e.inv));
    } else if (e.t == 'l') {
      w(0, "LINE");
      w(8, "0");
      w(10, df(e.a[0]));
      w(20, df(e.a[1]));
      w(30, df(e.a[2]));
      w(11, df(e.b[0]));
      w(21, df(e.b[1]));
      w(31, df(e.b[2]));
    } else {
      w(0, "INSERT");
      w(8, "0");
      w(2, e.block);
      w(10, df(e.ins.ins[0]));
      w(20, df(e.ins.ins[1]));
      w(30, df(e.ins.ins[2]));
      w(41, df(e.ins.scale[0]));
      w(42, df(e.ins.scale[1]));
      w(43, df(e.ins.scale[2]));
      w(50, df((e.ins.rot * 180) / kPi));
      w(210, df(e.ins.n[0]));
      w(220, df(e.ins.n[1]));
      w(230, df(e.ins.n[2]));
    }
  };
  w(0, "SECTION");
  w(2, "HEADER");
  w(9, "$ACADVER");
  w(1, "AC1009");
  w(9, "$INSBASE");
  w(10, "0");
  w(20, "0");
  w(30, "0");
  w(0, "ENDSEC");
  w(0, "SECTION");
  w(2, "TABLES");
  w(0, "TABLE");
  w(2, "LTYPE");
  w(70, "1");
  w(0, "LTYPE");
  w(2, "CONTINUOUS");
  w(70, "0");
  w(3, "Solid line");
  w(72, "65");
  w(73, "0");
  w(40, "0");
  w(0, "ENDTAB");
  w(0, "TABLE");
  w(2, "LAYER");
  w(70, "1");
  w(0, "LAYER");
  w(2, "0");
  w(70, "0");
  w(62, "7");
  w(6, "CONTINUOUS");
  w(0, "ENDTAB");
  w(0, "ENDSEC");
  w(0, "SECTION");
  w(2, "BLOCKS");
  for (const CadBlock& b : sc->blocks) {
    w(0, "BLOCK");
    w(8, "0");
    w(2, b.name);
    w(70, "0");
    w(10, "0");
    w(20, "0");
    w(30, "0");
    w(3, b.name);
    for (const CadEnt& e : b.ents) {
      ent(e);
    }
    w(0, "ENDBLK");
    w(8, "0");
  }
  w(0, "ENDSEC");
  w(0, "SECTION");
  w(2, "ENTITIES");
  for (const CadEnt& e : sc->ents) {
    ent(e);
  }
  w(0, "ENDSEC");
  w(0, "EOF");
  return DxfExport{join(L, "\r\n") + "\r\n", sc->report};
}

base::Result<DwgExport> to_dwg_model(const Model& model) {
  auto sc = cad_scene(model);
  if (!sc) {
    return std::unexpected(sc.error());
  }
  const auto color = [](const std::string& hex) {
    const std::string h = paint_hex(hex);
    if (!hex6(h)) {
      return base::make_object({{"index", 256}});
    }
    const auto rgb = std::strtol(h.substr(h.starts_with('#') ? 1 : 0).c_str(), nullptr, 16);
    return base::make_object({{"rgb", Value(static_cast<double>(rgb))}, {"index", nearest_aci(hex)}});
  };
  const auto P = [](const Vec3& p) { return base::make_object({{"x", p[0]}, {"y", p[1]}, {"z", p[2]}}); };
  const auto conv = [&](const CadEnt& e) {
    if (e.t == 'f') {
      Array pts;
      for (const Vec3& p : e.pts) {
        pts.push_back(P(p));
      }
      return base::make_object({{"type", "3DFACE"},
                                {"layer", "0"},
                                {"color", color(e.color)},
                                {"points", std::move(pts)},
                                {"invisibleEdges", e.inv}});
    }
    if (e.t == 'l') {
      return base::make_object({{"type", "LINE"},
                                {"layer", "0"},
                                {"color", base::make_object({{"index", 256}})},
                                {"start", P(e.a)},
                                {"end", P(e.b)}});
    }
    return base::make_object(
        {{"type", "INSERT"},
         {"layer", "0"},
         {"color", base::make_object({{"index", 256}})},
         {"block", e.block},
         {"insert", P(e.ins.ins)},
         {"scale", base::make_object({{"x", e.ins.scale[0]}, {"y", e.ins.scale[1]}, {"z", e.ins.scale[2]}})},
         {"rotation", e.ins.rot},
         {"extrusion", P(e.ins.n)}});
  };
  const bool metric = sc->insunits != 1;
  Array ms;
  for (const CadEnt& e : sc->ents) {
    ms.push_back(conv(e));
  }
  Array blocks;
  blocks.push_back(base::make_object({{"name", "*Model_Space"}, {"entities", std::move(ms)}}));
  blocks.push_back(base::make_object({{"name", "*Paper_Space"}, {"entities", Array{}}}));
  for (const CadBlock& b : sc->blocks) {
    Array es;
    for (const CadEnt& e : b.ents) {
      es.push_back(conv(e));
    }
    blocks.push_back(base::make_object(
        {{"name", b.name}, {"base", base::make_object({{"x", 0}, {"y", 0}, {"z", 0}})}, {"entities", std::move(es)}}));
  }
  Value dwg = base::make_object(
      {{"version", "AC1015"},
       {"codepage", "ANSI_1252"},
       {"header",
        base::make_object(
            {{"INSUNITS", sc->insunits}, {"MEASUREMENT", metric ? 1 : 0}, {"LUNITS", 2}, {"LUPREC", metric ? 3 : 4}})},
       {"tables", base::make_object({{"layers", Array{base::make_object({{"name", "0"},
                                                                         {"color", base::make_object({{"index", 7}})},
                                                                         {"off", false},
                                                                         {"frozen", false},
                                                                         {"locked", false},
                                                                         {"plot", true}})}}})},
       {"blocks", std::move(blocks)},
       {"layouts", Array{}},
       {"objects", Array{}}});
  return DwgExport{std::move(dwg), sc->report};
}

// ---------------------------------------------------------------------------- Collada

// The XML is written as io.js writes it, one string per element with its quoted attributes, so it reads against the JS.
// NOLINTBEGIN(modernize-raw-string-literal, performance-inefficient-string-concatenation)

namespace {

/// xmlText: characters XML 1.0 cannot hold dropped, & < > " escaped
std::string xml_text(std::string_view s) {
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x20 && b != 0x09 && b != 0x0a && b != 0x0d) {
      ++i;
      continue;
    }
    if (b == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBF &&
        (static_cast<unsigned char>(s[i + 2]) == 0xBE || static_cast<unsigned char>(s[i + 2]) == 0xBF)) {
      i += 3;  // U+FFFE, U+FFFF
      continue;
    }
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += s[i]; break;
    }
    ++i;
  }
  return out;
}

/// fx(x): +(+x).toPrecision(12), -0 and |v| < 1e-12 as 0 (Collada numbers)
std::string fx(double x) {
  double v = to_precision(x, 12);
  if (v == 0 || std::abs(v) < 1e-12) {
    v = 0;
  }
  return num_text(v);
}

/// ncName: runs of characters outside [\p{L}\p{N}._-] → '_', and '_' first unless it starts with a letter or '_'.
/// Every character past ASCII counts as a letter here (the JS asks Unicode's categories: docs/gaps.md).
std::string nc_name(std::string_view s) {
  std::string t;
  bool run = false;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    const bool ok = u >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (ok) {
      t += c;
      run = false;
    } else if (!run) {
      t += '_';
      run = true;
    }
  }
  const bool letter = !t.empty() && (static_cast<unsigned char>(t[0]) >= 0x80 || (t[0] >= 'a' && t[0] <= 'z') ||
                                     (t[0] >= 'A' && t[0] <= 'Z') || t[0] == '_');
  return letter ? t : "_" + t;
}

std::string name_extra(std::string_view s) {
  return "<extra><technique profile=\"ModelCraft\"><name>" + xml_text(s) + "</name></technique></extra>";
}

std::string row_major(const Mat4& m) {
  std::vector<std::string> v;
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      v.push_back(fx(m.at(c * 4 + r)));
    }
  }
  return join(v, " ");
}

bool convex_face(const Def& def, const Face& f, const Vec3& n) {
  const Frame fr = frame(n);
  std::vector<Vec2> ps;
  for (const Id v : f.loops[0]) {
    ps.push_back(to2(fr, def.vert(v)));
  }
  const std::size_t k = ps.size();
  if (k <= 3) {
    return true;
  }
  double size = 0;
  for (const Vec2& p : ps) {
    size = std::max({size, std::abs(p[0] - ps[0][0]), std::abs(p[1] - ps[0][1])});
  }
  const double tol = -1e-12 * size * size;
  for (std::size_t i = 0; i < k; ++i) {
    const Vec2& a = ps[i];
    const Vec2& b = ps[(i + 1) % k];
    const Vec2& c = ps[(i + 2) % k];
    if ((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) < tol) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string export_dae(const Model& model, std::string_view name, std::string_view date_iso) {
  OrderedMap<std::string, std::size_t> color_idx;
  model.walk([&](const Def& d, const Mat4& /*wm*/, const std::vector<Id>& /*p*/, const std::string& pc) {
    for (const auto& [fid, f] : d.faces) {
      const std::string c = xp(!f.color.empty() ? f.color : pc);
      if (!c.empty() && !color_idx.has(c)) {
        color_idx.set(c, color_idx.size());
      }
    }
    return true;
  });
  const auto cidx = [&](const std::string& c) {
    const std::size_t* i = color_idx.get(c);
    return i != nullptr ? std::to_string(*i) : std::string("undefined");
  };
  std::vector<std::string> geoms;
  struct GInfo {
    std::string id;
    std::vector<std::size_t> mats;
  };
  std::map<std::string, std::optional<GInfo>> geom_info;
  const auto geom_for = [&](const Def& d, const std::string& dflt) -> std::optional<GInfo> {
    const std::string key = d.id + "|" + dflt;
    if (const auto it = geom_info.find(key); it != geom_info.end()) {
      return it->second;
    }
    geom_info.emplace(key, std::nullopt);
    const std::string gid = "geom-" + d.id + (!dflt.empty() ? "-c" + cidx(dflt) : "");
    std::unordered_map<Id, std::size_t> vi;
    std::vector<double> pos;
    std::vector<double> nrm;
    const auto V = [&](Id v) {
      if (const auto it = vi.find(v); it != vi.end()) {
        return it->second;
      }
      const std::size_t i = pos.size() / 3;
      vi.emplace(v, i);
      const Vec3& p = d.vert(v);
      pos.insert(pos.end(), p.begin(), p.end());
      return i;
    };
    struct Set {
      std::vector<std::size_t> vcount, p;
    };
    OrderedMap<std::string, Set> sets;
    for (const auto& [fid, f] : d.faces) {
      const Vec3 n = face_normal(d, f);
      if (len(n) == 0) {
        continue;
      }
      const std::size_t ni = nrm.size() / 3;
      nrm.insert(nrm.end(), n.begin(), n.end());
      const std::string k = xp(!f.color.empty() ? f.color : dflt);
      Set* set = sets.get(k);
      if (set == nullptr) {
        set = &sets.set(k, {});
      }
      std::vector<Loop> polys;
      if (f.loops.size() == 1 && convex_face(d, f, n)) {
        polys.push_back(f.loops[0]);
      } else {
        for (const auto& t : face_tris(d, f)) {
          polys.push_back({t[0], t[1], t[2]});
        }
      }
      for (const Loop& loop : polys) {
        set->vcount.push_back(loop.size());
        for (const Id v : loop) {
          set->p.push_back(V(v));
          set->p.push_back(ni);
        }
      }
    }
    const auto ef = edge_faces(d);
    std::vector<std::size_t> lines;
    for (const auto& [eid, e] : d.edges) {
      if (!ef.contains(edge_key(e.a, e.b))) {
        lines.push_back(V(e.a));
        lines.push_back(V(e.b));
      }
    }
    if (sets.empty() && lines.empty()) {
      return std::nullopt;
    }
    const auto src = [](const std::string& sid, const std::vector<double>& arr) {
      std::vector<std::string> nums;
      nums.reserve(arr.size());
      for (const double x : arr) {
        nums.push_back(fx(x));
      }
      return "<source id=\"" + sid + "\"><float_array id=\"" + sid + "-array\" count=\"" + std::to_string(arr.size()) +
             "\">" + join(nums, " ") + "</float_array>" + "<technique_common><accessor source=\"#" + sid +
             "-array\" count=\"" + std::to_string(arr.size() / 3) +
             "\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" "
             "type=\"float\"/></accessor></technique_common></source>";
    };
    const auto ids = [](const std::vector<std::size_t>& v) {
      std::string s;
      for (std::size_t i = 0; i < v.size(); ++i) {
        s += (i != 0 ? " " : "") + std::to_string(v[i]);
      }
      return s;
    };
    std::string parts = src(gid + "-pos", pos);
    if (!nrm.empty()) {
      parts += src(gid + "-nrm", nrm);
    }
    parts += "<vertices id=\"" + gid + "-vtx\"><input semantic=\"POSITION\" source=\"#" + gid + "-pos\"/></vertices>";
    GInfo info{gid, {}};
    for (const auto& [k, set] : sets) {
      std::string mat;
      if (!k.empty()) {
        mat = " material=\"mat-" + cidx(k) + "\"";
        info.mats.push_back(*color_idx.get(k));
      }
      parts += "<polylist count=\"" + std::to_string(set.vcount.size()) + "\"" + mat +
               "><input semantic=\"VERTEX\" source=\"#" + gid +
               "-vtx\" offset=\"0\"/><input semantic=\"NORMAL\" source=\"#" + gid + "-nrm\" offset=\"1\"/>" +
               "<vcount>" + ids(set.vcount) + "</vcount><p>" + ids(set.p) + "</p></polylist>";
    }
    if (!lines.empty()) {
      parts += "<lines count=\"" + std::to_string(lines.size() / 2) + "\"><input semantic=\"VERTEX\" source=\"#" + gid +
               "-vtx\" offset=\"0\"/><p>" + ids(lines) + "</p></lines>";
    }
    geoms.push_back("<geometry id=\"" + gid + "\" name=\"" + xml_text(nc_name(d.name)) + "\"><mesh>" + parts +
                    "</mesh></geometry>");
    geom_info[key] = info;
    return info;
  };
  std::size_t node_n = 0;
  struct Comp {
    const Def* d;
    std::string cc, cid;
  };
  std::vector<Comp> comps;
  std::unordered_set<std::string> comp_seen;
  const auto inst_geom = [&](const Def& d, const std::string& dflt) -> std::string {
    const auto g = geom_for(d, dflt);
    if (!g) {
      return "";
    }
    std::string bind;
    if (!g->mats.empty()) {
      bind = "<bind_material><technique_common>";
      for (const std::size_t i : g->mats) {
        bind +=
            "<instance_material symbol=\"mat-" + std::to_string(i) + "\" target=\"#mat-" + std::to_string(i) + "\"/>";
      }
      bind += "</technique_common></bind_material>";
    }
    return "<instance_geometry url=\"#" + g->id + "\">" + bind + "</instance_geometry>";
  };
  // NOLINTNEXTLINE(misc-no-recursion): 32 levels at most, as io.js
  std::function<std::string(const Inst&, const std::string&, int)> inst_node;
  // NOLINTNEXTLINE(misc-no-recursion)
  const std::function<std::string(const Def&, const std::string&, int)> content =
      [&](const Def& d, const std::string& dflt, int depth) {
        std::string s = inst_geom(d, dflt);
        if (depth <= 32) {
          for (const auto& [iid, inst] : d.insts) {
            s += inst_node(inst, dflt, depth + 1);
          }
        }
        return s;
      };
  inst_node = [&](const Inst& inst, const std::string& dflt, int depth) -> std::string {
    const Def* d = model.def(inst.def);
    if (d == nullptr) {
      return "";
    }
    const std::string cc = !inst.color.empty() ? inst.color : dflt;
    const std::string nm = !inst.name.empty() ? inst.name : d->name;
    const std::string head = "<node id=\"node-" + std::to_string(++node_n) + "\" name=\"" + xml_text(nc_name(nm)) +
                             "\" type=\"NODE\"><matrix sid=\"transform\">" + row_major(inst.m) + "</matrix>";
    if (d->kind == "component") {
      const std::string cid = "comp-" + d->id + (!cc.empty() ? "-c" + cidx(cc) : "");
      if (!comp_seen.contains(cid)) {
        comp_seen.insert(cid);
        comps.push_back({d, cc, cid});
      }
      return head + "<instance_node url=\"#" + cid + "\"/>" + name_extra(nm) + "</node>";
    }
    std::string body = content(*d, cc, depth);
    return head + body + name_extra(nm) + "</node>";
  };
  const std::string root_content = content(model.root(), {}, 0);
  const std::string scene = "<visual_scene id=\"scene\" name=\"" + xml_text(nc_name(name)) +
                            "\"><node id=\"model\" name=\"" +
                            xml_text(nc_name(!model.root().name.empty() ? model.root().name : "Model")) +
                            "\" type=\"NODE\">" + root_content + "</node></visual_scene>";
  std::vector<std::string> lib_nodes;
  // NOLINTNEXTLINE(modernize-loop-convert): `comps` grows while component nodes place other components
  for (std::size_t i = 0; i < comps.size(); ++i) {
    const Comp c = comps[i];  // NOLINT(performance-unnecessary-copy-initialization): comps may grow below
    std::string body = content(*c.d, c.cc, 0);
    lib_nodes.push_back("<node id=\"" + c.cid + "\" name=\"" + xml_text(nc_name(c.d->name)) + "\" type=\"NODE\">" +
                        body + name_extra(c.d->name) + "</node>");
  }
  std::vector<std::string> out = {
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>",
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">",
      "<asset><contributor><authoring_tool>ModelCraft</authoring_tool></contributor><created>" + std::string(date_iso) +
          "</created><modified>" + std::string(date_iso) +
          "</modified><unit name=\"meter\" meter=\"1\"/><up_axis>Z_UP</up_axis></asset>"};
  const std::vector<std::string> colors = color_idx.keys();
  if (!colors.empty()) {
    std::string eff = "<library_effects>";
    std::string mats = "<library_materials>";
    for (std::size_t i = 0; i < colors.size(); ++i) {
      const std::string h = paint_hex(colors[i]);
      std::vector<std::string> rgb;
      for (std::size_t o = 1; o <= 5; o += 2) {
        rgb.push_back(fx(static_cast<double>(std::strtol(h.substr(o, 2).c_str(), nullptr, 16)) / 255));
      }
      eff += "<effect id=\"eff-" + std::to_string(i) +
             "\"><profile_COMMON><technique sid=\"common\"><lambert><diffuse><color sid=\"diffuse\">" + join(rgb, " ") +
             " 1</color></diffuse></lambert></technique></profile_COMMON></effect>";
      const std::string mname =
          is_material_paint(colors[i]) ? "Material_" + colors[i].substr(4) : "Colour_" + colors[i].substr(1);
      mats += "<material id=\"mat-" + std::to_string(i) + "\" name=\"" + mname + "\"><instance_effect url=\"#eff-" +
              std::to_string(i) + "\"/></material>";
    }
    out.push_back(eff + "</library_effects>");
    out.push_back(mats + "</library_materials>");
  }
  if (!geoms.empty()) {
    out.push_back("<library_geometries>" + join(geoms, "\n") + "</library_geometries>");
  }
  if (!lib_nodes.empty()) {
    out.push_back("<library_nodes>" + join(lib_nodes, "\n") + "</library_nodes>");
  }
  out.push_back("<library_visual_scenes>" + scene + "</library_visual_scenes>");
  out.emplace_back("<scene><instance_visual_scene url=\"#scene\"/></scene>");
  out.emplace_back("</COLLADA>");
  out.emplace_back("");
  return join(out, "\n");
}

// NOLINTEND(modernize-raw-string-literal, performance-inefficient-string-concatenation)

// ---------------------------------------------------------------------------- IFC4

std::string ifc_guid(std::string_view seed) {
  static constexpr std::string_view kIfc64 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
  std::array<std::uint32_t, 4> h{};
  for (std::uint32_t r = 0; r < 4; ++r) {
    std::uint32_t x = 0x811c9dc5U ^ (r * 0x9e3779b9U);
    const std::string s = std::to_string(r) + ":" + std::string(seed);
    for (const std::uint16_t u : base::to_utf16(s)) {
      x ^= u;
      x *= 0x01000193U;
    }
    x ^= x >> 15U;
    x *= 0x2c1b3c6dU;
    x ^= x >> 12U;
    h.at(r) = x;
  }
  // n = h0 h1 h2 h3 (128 bits, h0 highest): bits [6j, 6j + 6) for j = 20 … 0, after n >> 126
  const auto bits = [&](unsigned at, unsigned count) {
    std::uint32_t v = 0;
    for (unsigned b = 0; b < count; ++b) {
      const unsigned bit = at + b;  // 0 = the lowest bit of h3
      const std::uint32_t word = h.at(3 - bit / 32);
      v |= ((word >> (bit % 32)) & 1U) << b;
    }
    return v;
  };
  std::string out(1, kIfc64.at(bits(126, 2)));
  for (int j = 20; j >= 0; --j) {
    out += kIfc64.at(bits(static_cast<unsigned>(j) * 6, 6));
  }
  return out;
}

std::string step_string(std::string_view s) {
  std::string out = "'";
  std::size_t i = 0;
  while (i < s.size()) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 4;
    if (b < 0x80) {
      n = 1;
    } else if (b < 0xE0) {
      n = 2;
    } else if (b < 0xF0) {
      n = 3;
    }
    const std::string_view ch = s.substr(i, n);
    i += n;
    if (n == 1) {
      const char c = (b < 0x20 || b == 0x7f) ? ' ' : ch[0];
      if (c == '\'') {
        out += "''";
      } else if (c == '\\') {
        out += "\\\\";
      } else {
        out += c;
      }
      continue;
    }
    out += "\\X2\\";
    for (const std::uint16_t u : base::to_utf16(ch)) {
      static constexpr std::string_view kHex = "0123456789ABCDEF";
      for (int sh = 12; sh >= 0; sh -= 4) {
        out += kHex.at((u >> sh) & 15U);
      }
    }
    out += "\\X0\\";
  }
  return out + "'";
}

namespace {

/// real(x): toFixed(9), -0 / tiny as 0, exponent form as toExponential(9) with E, and a '.' if it has none (STEP)
std::string real(double x) {
  double v = std::strtod(base::js_to_fixed(x, 9).c_str(), nullptr);
  if (v == 0 || std::abs(v) < 1e-12) {
    v = 0;
  }
  std::string s = num_text(v);
  if (s.contains('e')) {
    s = base::js_to_exponential(x, 9);
    std::ranges::replace(s, 'e', 'E');
  }
  if (!s.contains('.') && !s.contains('E')) {
    s += '.';
  }
  return s;
}

bool rigid(const Mat4& m) {
  const std::array<Vec3, 3> cols = {Vec3{m[0], m[1], m[2]}, Vec3{m[4], m[5], m[6]}, Vec3{m[8], m[9], m[10]}};
  if (std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) > 1e-12 || std::abs(m[15] - 1) > 1e-12) {
    return false;
  }
  for (std::size_t a = 0; a < 3; ++a) {
    if (std::abs(len(cols.at(a)) - 1) > 1e-9) {
      return false;
    }
    for (std::size_t b = a + 1; b < 3; ++b) {
      if (std::abs(dot(cols.at(a), cols.at(b))) > 1e-9) {
        return false;
      }
    }
  }
  return det3(m) > 0;
}

}  // namespace

std::string export_ifc(const Model& model, std::string_view name, std::string_view timestamp) {
  std::vector<std::string> lines;
  std::size_t n = 0;
  const auto L = [&](const std::string& s) {
    lines.push_back("#" + std::to_string(++n) + "=" + s + ";");
    return "#" + std::to_string(n);
  };
  const std::string salt(name);
  const auto G = [&](const std::string& k) { return step_string(ifc_guid(salt + "|" + k)); };
  const auto dir = [&](const Vec3& v) {
    return L("IFCDIRECTION((" + real(v[0]) + "," + real(v[1]) + "," + real(v[2]) + "))");
  };
  const auto pt = [&](const Vec3& p) {
    return L("IFCCARTESIANPOINT((" + real(p[0]) + "," + real(p[1]) + "," + real(p[2]) + "))");
  };
  const std::string origin = pt({0, 0, 0});
  const std::string z_dir = dir({0, 0, 1});
  const std::string x_dir = dir({1, 0, 0});
  const std::string world_axes = L("IFCAXIS2PLACEMENT3D(" + origin + "," + z_dir + "," + x_dir + ")");
  const std::string ctx = L("IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05," + world_axes + ",$)");
  const std::string body =
      L("IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*," + ctx + ",$,.MODEL_VIEW.,$)");
  const std::string u1 = L("IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.)");
  const std::string u2 = L("IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.)");
  const std::string u3 = L("IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.)");
  const std::string u4 = L("IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.)");
  const std::string units = L("IFCUNITASSIGNMENT((" + u1 + "," + u2 + "," + u3 + "," + u4 + "))");
  const std::string project =
      L("IFCPROJECT(" + G("project") + ",$," + step_string(name) + ",$,$,$,$,(" + ctx + ")," + units + ")");
  const auto place = [&](const std::string& rel, const std::string& axes) {
    const std::string a = !axes.empty() ? axes : L("IFCAXIS2PLACEMENT3D(" + origin + ",$,$)");
    return L("IFCLOCALPLACEMENT(" + rel + "," + a + ")");
  };
  const std::string site_pl = place("$", "");
  const std::string bld_pl = place(site_pl, "");
  const std::string site = L("IFCSITE(" + G("site") + ",$,'Site',$,$," + site_pl + ",$,$,.ELEMENT.,$,$,$,$,$)");
  const std::string building =
      L("IFCBUILDING(" + G("building") + ",$,'Building',$,$," + bld_pl + ",$,$,.ELEMENT.,$,$,$)");
  struct Level {
    std::string name;
    double elevation = 0;
    std::string pl, id;
    std::vector<std::string> items;
  };
  std::vector<Level> levels;
  std::vector<Storey> st = model.storeys;
  if (st.empty()) {
    st.push_back({"Level 1", 0});
  }
  for (std::size_t k = 0; k < st.size(); ++k) {
    const std::string p = pt({0, 0, st[k].elevation});
    const std::string axes = L("IFCAXIS2PLACEMENT3D(" + p + ",$,$)");
    const std::string pl = place(bld_pl, axes);
    const std::string id = L("IFCBUILDINGSTOREY(" + G("storey" + (k != 0 ? "|" + std::to_string(k) : "")) + ",$," +
                             step_string(st[k].name) + ",$,$," + pl + ",$,$,.ELEMENT.," + real(st[k].elevation) + ")");
    levels.push_back({st[k].name, st[k].elevation, pl, id, {}});
  }
  (void)L("IFCRELAGGREGATES(" + G("agg-project") + ",$,$,$," + project + ",(" + site + "))");
  (void)L("IFCRELAGGREGATES(" + G("agg-site") + ",$,$,$," + site + ",(" + building + "))");
  {
    std::vector<std::string> ids;
    ids.reserve(levels.size());
    for (const Level& l : levels) {
      ids.push_back(l.id);
    }
    (void)L("IFCRELAGGREGATES(" + G("agg-building") + ",$,$,$," + building + ",(" + join(ids, ",") + "))");
  }
  const auto level_of = [&](double z) -> Level& {
    Level* s = levels.data();
    for (Level& l : levels) {
      if (l.elevation <= z + 1e-6) {
        s = &l;
      }
    }
    return *s;
  };
  struct FaceLoops {
    std::vector<std::vector<Vec3>> loops;
  };
  const auto proxy = [&](const std::string& key2, const std::string& label, const std::vector<FaceLoops>& faces,
                         bool closed, const Mat4* m) {
    if (faces.empty()) {
      return;
    }
    double z = std::numeric_limits<double>::infinity();
    for (const FaceLoops& f : faces) {
      for (const Vec3& p : f.loops[0]) {
        z = std::min(z, m != nullptr ? m3d::apply(*m, p)[2] : p[2]);
      }
    }
    Level& lv = level_of(z);
    std::unordered_map<std::string, std::string> pts;
    const auto P = [&](const Vec3& p) {
      const std::string k2 =
          base::js_to_fixed(p[0], 9) + "," + base::js_to_fixed(p[1], 9) + "," + base::js_to_fixed(p[2], 9);
      if (const auto it = pts.find(k2); it != pts.end()) {
        return it->second;
      }
      const std::string id = pt(p);
      pts.emplace(k2, id);
      return id;
    };
    std::vector<std::string> fs;
    for (const FaceLoops& f : faces) {
      std::vector<std::string> bounds;
      for (std::size_t j = 0; j < f.loops.size(); ++j) {
        std::vector<std::string> ps;
        for (const Vec3& p : f.loops[j]) {
          ps.push_back(P(p));
        }
        const std::string loop = L("IFCPOLYLOOP((" + join(ps, ",") + "))");
        bounds.push_back(L(std::string(j != 0 ? "IFCFACEBOUND" : "IFCFACEOUTERBOUND") + "(" + loop + ",.T.)"));
      }
      fs.push_back(L("IFCFACE((" + join(bounds, ",") + "))"));
    }
    std::string item;
    std::string kind;
    if (closed) {
      const std::string shell = L("IFCCLOSEDSHELL((" + join(fs, ",") + "))");
      item = L("IFCFACETEDBREP(" + shell + ")");
      kind = "Brep";
    } else {
      const std::string shell = L("IFCOPENSHELL((" + join(fs, ",") + "))");
      item = L("IFCSHELLBASEDSURFACEMODEL((" + shell + "))");
      kind = "SurfaceModel";
    }
    const std::string rep = L("IFCSHAPEREPRESENTATION(" + body + ",'Body','" + kind + "',(" + item + "))");
    const std::string shape = L("IFCPRODUCTDEFINITIONSHAPE($,$,(" + rep + "))");
    std::string axes;
    if (m != nullptr) {
      const std::string p = pt({(*m)[12], (*m)[13], (*m)[14] - lv.elevation});
      const std::string d1 = dir(norm({(*m)[8], (*m)[9], (*m)[10]}));
      const std::string d2 = dir(norm({(*m)[0], (*m)[1], (*m)[2]}));
      axes = L("IFCAXIS2PLACEMENT3D(" + p + "," + d1 + "," + d2 + ")");
    } else if (lv.elevation != 0) {
      const std::string p = pt({0, 0, -lv.elevation});
      axes = L("IFCAXIS2PLACEMENT3D(" + p + ",$,$)");
    }
    const std::string pl = place(lv.pl, axes);
    const std::string el = L("IFCBUILDINGELEMENTPROXY(" + G("proxy|" + key2) + ",$," + step_string(label) + ",$,$," +
                             pl + "," + shape + ",$,.NOTDEFINED.)");
    lv.items.push_back(el);
  };
  const auto faces_of = [&](const Def& def, const Mat4& m0) {
    std::vector<FaceLoops> out;
    model.walk(
        [&](const Def& d, const Mat4& wm, const std::vector<Id>& /*p*/, const std::string& /*c*/) {
          const bool mirror = det3(wm) < 0;
          for (const auto& [fid, f] : d.faces) {
            FaceLoops fl;
            for (const Loop& l : f.loops) {
              std::vector<Vec3> ps;
              for (const Id v : l) {
                ps.push_back(m3d::apply(wm, d.vert(v)));
              }
              if (mirror) {
                std::ranges::reverse(ps);
              }
              fl.loops.push_back(std::move(ps));
            }
            out.push_back(std::move(fl));
          }
          return true;
        },
        &def, m0);
    return out;
  };
  const Def& root = model.root();
  for (const auto& [iid, inst] : root.insts) {
    const Def* d = model.def(inst.def);
    if (d == nullptr) {
      continue;
    }
    const bool closed = d->insts.empty() && solid_info(*d).closed;
    const std::string label = !inst.name.empty() ? inst.name : d->name;
    if (rigid(inst.m)) {
      proxy("i" + std::to_string(iid), label, faces_of(*d, ident()), closed, &inst.m);
    } else {
      proxy("i" + std::to_string(iid), label, faces_of(*d, inst.m), closed && det3(inst.m) > 0, nullptr);
    }
  }
  if (!root.faces.empty()) {
    std::vector<FaceLoops> own;
    for (const auto& [fid, f] : root.faces) {
      FaceLoops fl;
      for (const Loop& l : f.loops) {
        fl.loops.push_back(root.points(l));
      }
      own.push_back(std::move(fl));
    }
    proxy("loose", "Loose geometry", own, solid_info(root).closed, nullptr);
  }
  for (std::size_t k = 0; k < levels.size(); ++k) {
    if (!levels[k].items.empty()) {
      (void)L("IFCRELCONTAINEDINSPATIALSTRUCTURE(" + G("contained" + (k != 0 ? "|" + std::to_string(k) : "")) +
              ",$,$,$,(" + join(levels[k].items, ",") + ")," + levels[k].id + ")");
    }
  }
  std::vector<std::string> out = {"ISO-10303-21;",
                                  "HEADER;",
                                  "FILE_DESCRIPTION(('ViewDefinition [ReferenceView]'),'2;1');",
                                  "FILE_NAME(" + step_string(std::string(name) + ".ifc") + ",'" +
                                      std::string(timestamp) + "',(''),(''),'ModelCraft','ModelCraft','');",
                                  "FILE_SCHEMA(('IFC4'));",
                                  "ENDSEC;",
                                  "DATA;"};
  out.insert(out.end(), lines.begin(), lines.end());
  out.emplace_back("ENDSEC;");
  out.emplace_back("END-ISO-10303-21;");
  out.emplace_back("");
  return join(out, "\n");
}

}  // namespace cc::m3d::io
