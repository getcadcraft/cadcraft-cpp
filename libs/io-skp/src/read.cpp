// SPDX-License-Identifier: Apache-2.0
// lib/skp read.js: readSkp / sniffSkp, ported, and the model as the JS object (to_value). See skp.hpp.
#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include "cc/base/js.hpp"
#include "cc/skp/skp.hpp"
#include "common.hpp"
#include "readers.hpp"

namespace cc::skp {

namespace {

using detail::fail;
using detail::msg;

constexpr std::string_view kMagic = "SketchUp Model";

struct Record {
  std::string text;
  std::size_t end = 0;
};

/// One "FF FE FF" string record at p, or nullopt.
std::optional<Record> string_record(std::span<const std::uint8_t> b, std::size_t p) {
  if (p + 4 > b.size() || b[p] != 0xff || b[p + 1] != 0xfe || b[p + 2] != 0xff) {
    return std::nullopt;
  }
  std::size_t n = b[p + 3];
  std::size_t q = p + 4;
  if (n == 0xff) {
    if (q + 2 > b.size()) {
      return std::nullopt;
    }
    n = static_cast<std::size_t>(b[q]) | (static_cast<std::size_t>(b[q + 1]) << 8U);
    q += 2;
    if (n == 0xffff) {
      if (q + 4 > b.size()) {
        return std::nullopt;
      }
      n = static_cast<std::size_t>(b[q]) | (static_cast<std::size_t>(b[q + 1]) << 8U) |
          (static_cast<std::size_t>(b[q + 2]) << 16U) | (static_cast<std::size_t>(b[q + 3]) << 24U);
      q += 4;
    }
  }
  if (n > 4096 || q + n * 2 > b.size()) {
    return std::nullopt;
  }
  std::vector<std::uint16_t> units(n);
  for (std::size_t i = 0; i < n; ++i) {
    units[i] = static_cast<std::uint16_t>(b[q + i * 2] | (b[q + i * 2 + 1] << 8U));
  }
  Record r;
  base::append_utf16(r.text, units);
  r.end = q + n * 2;
  return r;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

Result<Model> guarded(std::span<const std::uint8_t> bytes, const ReadOptions& opts) {
  const i18n::Catalog* cat = opts.catalog;
  if (bytes.size() > detail::Limits::file_bytes) {
    fail("too_big",
         msg(cat, "ccskp.too_big.file", {{"size", std::to_string(detail::Limits::file_bytes / 1048576) + " MB"}}));
  }
  auto sniff = sniff_skp(bytes, opts);
  if (!sniff) {
    return std::unexpected(std::move(sniff.error()));
  }
  Model m = sniff->vff ? detail::read_vff(bytes, *sniff, opts) : detail::read_legacy(bytes, *sniff, opts);
  m.vff = sniff->vff;
  m.version = sniff->version;
  m.major = sniff->major;
  m.root = 0;
  m.counts.defs = m.defs.size();
  for (const Definition& d : m.defs) {
    m.counts.faces += d.faces.size();
    m.counts.edges += d.edges.size();
    m.counts.insts += d.insts.size();
  }
  m.counts.materials = m.materials.size();
  m.counts.layers = m.layers.size();
  return m;
}

}  // namespace

Result<Sniff> sniff_skp(std::span<const std::uint8_t> b, const ReadOptions& opts) {
  const i18n::Catalog* cat = opts.catalog;
  const auto not_skp = [cat] { return std::unexpected(Error{"not_skp", msg(cat, "ccskp.not_skp")}); };
  const auto r1 = string_record(b, 0);
  if (!r1 || r1->text != kMagic) {
    // the start of "SketchUp Model" but the file ends inside it: a cut-off SketchUp file
    if (!r1 && b.size() >= 8) {
      std::vector<std::uint8_t> head{0xff, 0xfe, 0xff, static_cast<std::uint8_t>(kMagic.size())};
      for (const char c : kMagic) {
        head.push_back(static_cast<std::uint8_t>(c));
        head.push_back(0);
      }
      if (b.size() < head.size() && std::equal(b.begin(), b.end(), head.begin())) {
        return std::unexpected(
            Error{"damaged", msg(cat, "ccskp.damaged", {{"reason", msg(cat, "ccskp.damaged.header_cut")}})});
      }
    }
    return not_skp();
  }
  const auto r2 = string_record(b, r1->end);
  // /^\{(\d+)\.([\d.]*)\}$/
  std::optional<std::pair<std::string, std::string>> m;
  if (r2) {
    const std::string& t = r2->text;
    if (t.size() >= 4 && t.front() == '{' && t.back() == '}') {
      std::size_t i = 1;
      while (i < t.size() && is_digit(t[i])) {
        ++i;
      }
      if (i > 1 && i < t.size() - 1 && t[i] == '.') {
        const std::string rest = t.substr(i + 1, t.size() - i - 2);
        if (std::ranges::all_of(rest, [](char c) { return is_digit(c) || c == '.'; })) {
          m = std::pair{t.substr(1, i - 1), rest};
        }
      }
    }
  }
  if (!m || !r2) {
    return std::unexpected(
        Error{"damaged", msg(cat, "ccskp.damaged", {{"reason", msg(cat, "ccskp.damaged.no_version")}})});
  }
  Sniff s;
  const auto& [digits, rest] = m.value();
  s.version = digits + (rest.empty() ? "" : "." + rest);
  s.major = base::js_parse_int(digits);
  s.body_start = r2->end;
  // VFF: "VFF" right after the version, or a ZIP local header within the next 64 bytes
  const std::size_t at = s.body_start;
  if (at + 2 < b.size() && b[at] == 0x56 && b[at + 1] == 0x46 && b[at + 2] == 0x46) {
    s.vff = true;
  } else if (b.size() >= 4) {
    const std::size_t end = std::min(b.size() - 4, at + 64);
    for (std::size_t i = at; i <= end; ++i) {
      if (b[i] == 0x50 && b[i + 1] == 0x4b && b[i + 2] == 3 && b[i + 3] == 4) {
        s.vff = true;
        break;
      }
    }
  }
  return s;
}

Result<Model> read_skp(std::span<const std::uint8_t> bytes, const ReadOptions& opts) {
  try {
    return guarded(bytes, opts);
  } catch (const detail::Fail& f) {
    return std::unexpected(f.error);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error{"too_big", msg(opts.catalog, "ccskp.too_big.unpacked")});
  } catch (const std::exception& e) {
    return std::unexpected(Error{"damaged", msg(opts.catalog, "ccskp.legacy.failed_internal", {{"reason", e.what()}})});
  }
}

// ---------------------------------------------------------------------------------------------------- to_value
namespace {

base::Value p3(const P3& p) {
  return base::Value(base::Array{base::Value(p[0]), base::Value(p[1]), base::Value(p[2])});
}
base::Value opt_index(const std::optional<std::uint32_t>& i) {
  return i ? base::Value(*i) : base::Value(nullptr);
}
base::Value rgb(const std::optional<Rgb>& c) {
  if (!c) {
    return {nullptr};
  }
  return base::Value(base::Array{base::Value((*c)[0]), base::Value((*c)[1]), base::Value((*c)[2])});
}
base::Value uv_value(const std::vector<std::vector<P2>>& uvs) {
  base::Array uv;
  for (const auto& loop : uvs) {
    base::Array l;
    l.reserve(loop.size());
    for (const P2& p : loop) {
      l.emplace_back(base::Array{base::Value(p[0]), base::Value(p[1])});
    }
    uv.emplace_back(std::move(l));
  }
  return {std::move(uv)};
}
std::string_view kind_name(DefKind k) {
  switch (k) {
    case DefKind::model: return "model";
    case DefKind::component: return "component";
    case DefKind::group: return "group";
    case DefKind::image: return "image";
  }
  return "component";
}

}  // namespace

base::Value to_value(const Model& m) {
  using base::Array;
  using base::Value;
  Value out = Value::object();
  out.append("format", "skp");
  out.append("container", m.vff ? "vff" : "legacy");
  out.append("version", m.version);
  out.append("major", m.major);
  Array materials;
  for (const Material& mat : m.materials) {
    Value v = Value::object();
    v.append("name", mat.name);
    v.append("color", rgb(mat.color));
    v.append("alpha", mat.alpha);
    if (mat.texture) {
      Value t = Value::object();
      t.append("name", mat.texture->name);
      t.append("mime", mat.texture->mime);
      t.append("bytes", Value(base::ByteVec(mat.texture->bytes)));
      t.append("width", mat.texture->width);
      t.append("height", mat.texture->height);
      v.append("texture", std::move(t));
    } else {
      v.append("texture", nullptr);
    }
    materials.push_back(std::move(v));
  }
  out.append("materials", std::move(materials));
  Array layers;
  for (const Layer& l : m.layers) {
    Value v = Value::object();
    v.append("name", l.name);
    v.append("visible", l.visible);
    v.append("color", rgb(l.color));
    layers.push_back(std::move(v));
  }
  out.append("layers", std::move(layers));
  Array defs;
  for (const Definition& d : m.defs) {
    Value v = Value::object();
    v.append("name", d.name);
    v.append("kind", std::string(kind_name(d.kind)));
    v.append("guid", d.guid ? Value(*d.guid) : Value(nullptr));
    Array faces;
    for (const Face& f : d.faces) {
      Value fv = Value::object();
      Array loops;
      for (const auto& loop : f.loops) {
        Array l;
        l.reserve(loop.size());
        for (const P3& p : loop) {
          l.push_back(p3(p));
        }
        loops.emplace_back(std::move(l));
      }
      fv.append("loops", std::move(loops));
      fv.append("front", opt_index(f.front));
      fv.append("back", opt_index(f.back));
      fv.append("layer", opt_index(f.layer));
      if (const auto& uvs = f.uv) {
        fv.append("uv", uv_value(*uvs));
      } else {
        fv.append("uv", nullptr);
      }
      if (m.vff || f.hidden) {
        fv.append("hidden", f.hidden);  // classic files say it only when true
      }
      faces.push_back(std::move(fv));
    }
    v.append("faces", std::move(faces));
    Array edges;
    for (const Edge& e : d.edges) {
      Value ev = Value::object();
      ev.append("a", p3(e.a));
      ev.append("b", p3(e.b));
      ev.append("layer", opt_index(e.layer));
      ev.append("soft", e.soft);
      ev.append("smooth", e.smooth);
      ev.append("hidden", e.hidden);
      edges.push_back(std::move(ev));
    }
    v.append("edges", std::move(edges));
    Array soft;
    for (const auto& s : d.soft) {
      soft.emplace_back(Array{p3(s[0]), p3(s[1])});
    }
    v.append("soft", std::move(soft));
    Array insts;
    for (const Instance& in : d.insts) {
      Value iv = Value::object();
      iv.append("def", in.def);
      Array mm;
      for (const double x : in.m) {
        mm.emplace_back(x);
      }
      iv.append("m", std::move(mm));
      iv.append("name", in.name);
      iv.append("layer", opt_index(in.layer));
      iv.append("hidden", in.hidden);
      iv.append("material", opt_index(in.material));
      insts.push_back(std::move(iv));
    }
    v.append("insts", std::move(insts));
    defs.push_back(std::move(v));
  }
  out.append("defs", std::move(defs));
  out.append("root", m.root);
  if (m.thumbnail) {
    Value t = Value::object();
    t.append("mime", m.thumbnail->mime);
    t.append("bytes", Value(base::ByteVec(m.thumbnail->bytes)));
    out.append("thumbnail", std::move(t));
  } else {
    out.append("thumbnail", nullptr);
  }
  Value units = Value::object();
  units.append("length", m.length_unit ? Value(*m.length_unit) : Value(nullptr));
  out.append("units", std::move(units));
  Array notes;
  for (const std::string& n : m.notes) {
    notes.emplace_back(n);
  }
  out.append("notes", std::move(notes));
  Value counts = Value::object();
  counts.append("defs", m.counts.defs);
  counts.append("faces", m.counts.faces);
  counts.append("edges", m.counts.edges);
  counts.append("insts", m.counts.insts);
  counts.append("materials", m.counts.materials);
  counts.append("layers", m.counts.layers);
  out.append("counts", std::move(counts));
  return out;
}

}  // namespace cc::skp
