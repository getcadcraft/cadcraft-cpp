// SPDX-License-Identifier: Apache-2.0
// lib/skp legacy.js: classic SketchUp files (SketchUp 3 … 2020), ported. Format knowledge in cc-skp came from
// OpenSKP (MIT, see NOTICE: its legacy walk was cc-skp's map); this follows cc-skp's JavaScript function by function.
//
// The body is ONE uncompressed MFC CArchive object stream with a single store map: every new class record (0xFFFF,
// schema u16, name length u16, ASCII name) and every new object takes the next index; later references are back-refs
// to that index (u16, or 0x7FFF + u32 once indexes get big); 0x8000|n = a new object of the class at index n. The
// stream opens with CVersionMap, then document settings that are not walked, the material manager, the layer list,
// the definition list, the model's own entity list and the options dictionaries. Because the settings region is
// skipped, the store-map base is bootstrapped from the first back-references (slot_bases).
//
// Values the JS keeps as plain objects live here in one pool per class; a slot holds its class and its value's index
// (-1 = no value: being read, or released after its definition was built, as the JS drops them for the collector).
// Recursion follows the object nesting, capped at kMaxObjectDepth (and attribute arrays at 8).
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "common.hpp"
#include "readers.hpp"

namespace cc::skp::detail {

namespace {

/// A record that could not be made sense of; caught inside the walk where the JS tolerates it.
struct Misparse {
  std::string message;
};
/// Where the JS would stop with a TypeError (only on hostile input): becomes ccskp.legacy.failed_internal.
struct Internal {
  std::string message;
};

constexpr std::size_t kMaxObjectDepth = 2 * static_cast<std::size_t>(Limits::depth) + 32;
constexpr std::int64_t kMaxSlots = 64'000'000;

std::string at_text(std::size_t p) {
  return std::to_string(p);
}

[[noreturn]] void misparse(const i18n::Catalog* cat, std::string_view key, Fills fills = {}) {
  throw Misparse{msg(cat, key, fills)};
}

// ------------------------------------------------------------------------------------------------ byte helpers
std::int64_t u16at(std::span<const std::uint8_t> b, std::int64_t p) {
  if (p < 0 || p + 2 > static_cast<std::int64_t>(b.size())) {
    return -1;
  }
  const auto q = static_cast<std::size_t>(p);
  return b[q] | (b[q + 1] << 8U);
}
std::int64_t u32at(std::span<const std::uint8_t> b, std::int64_t p) {
  if (p < 0 || p + 4 > static_cast<std::int64_t>(b.size())) {
    return -1;
  }
  const auto q = static_cast<std::size_t>(p);
  return static_cast<std::int64_t>(static_cast<std::uint32_t>(b[q]) | (static_cast<std::uint32_t>(b[q + 1]) << 8U) |
                                   (static_cast<std::uint32_t>(b[q + 2]) << 16U) |
                                   (static_cast<std::uint32_t>(b[q + 3]) << 24U));
}
/// JS `x & 0x80000000` on u32at's answer (-1 included): non-zero?
bool high_bit(std::int64_t x) {
  return (static_cast<std::uint64_t>(x) & 0x80000000ULL) != 0;
}
bool str_at(std::span<const std::uint8_t> b, std::size_t p) {
  return p + 3 <= b.size() && b[p] == 0xff && b[p + 1] == 0xfe && b[p + 2] == 0xff;
}
bool ascii_at(std::span<const std::uint8_t> b, std::int64_t p, std::string_view s) {
  if (p < 0 || p + static_cast<std::int64_t>(s.size()) > static_cast<std::int64_t>(b.size())) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (b[static_cast<std::size_t>(p) + i] != static_cast<std::uint8_t>(s[i])) {
      return false;
    }
  }
  return true;
}
/// First index of `needle` (-1 = any byte) in b[start, end), or -1.
std::int64_t find(std::span<const std::uint8_t> b, std::span<const int> needle, std::int64_t start,
                  std::int64_t end = std::numeric_limits<std::int64_t>::max()) {
  const std::int64_t last =
      std::min<std::int64_t>(end, static_cast<std::int64_t>(b.size())) - static_cast<std::int64_t>(needle.size());
  for (std::int64_t i = std::max<std::int64_t>(0, start); i <= last; ++i) {
    bool hit = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (needle[j] >= 0 && std::cmp_not_equal(b[static_cast<std::size_t>(i) + j], needle[j])) {
        hit = false;
        break;
      }
    }
    if (hit) {
      return i;
    }
  }
  return -1;
}
constexpr std::array<int, 3> kStr = {0xff, 0xfe, 0xff};
/// A new-class record for `name`: FF FF <schema u16> <len u16> name.
std::vector<int> class_record(std::string_view name) {
  std::vector<int> v{0xff, 0xff, -1, -1, static_cast<int>(name.size()), 0};
  for (const char c : name) {
    v.push_back(static_cast<unsigned char>(c));
  }
  return v;
}

/// CString: FF FE FF, length u8 (0xFF → u16, 0xFFFF → u32) in UTF-16 code units, then the text.
std::string str(Reader& r, const i18n::Catalog* cat) {
  if (!str_at(r.bytes(), r.pos)) {
    misparse(cat, "ccskp.legacy.text_record", {{"at", at_text(r.pos)}});
  }
  r.pos += 3;
  std::uint64_t n = r.u8();
  if (n == 0xff) {
    n = r.u16();
    if (n == 0xffff) {
      n = r.u32();
    }
  }
  if (n * 2 > r.left()) {
    misparse(cat, "ccskp.legacy.text_long", {{"at", at_text(r.pos)}});
  }
  return r.utf16(static_cast<std::size_t>(n));
}

template <std::size_t N>
std::array<double, N> f64s(Reader& r) {
  std::array<double, N> a{};
  for (std::size_t i = 0; i < N; ++i) {
    a[i] = r.f64();
  }
  return a;
}
std::int64_t peek16(Reader& r) {
  r.need(2);
  return u16at(r.bytes(), static_cast<std::int64_t>(r.pos));
}

// ------------------------------------------------------------------------------------------------ classes, values
enum class Cls : std::uint8_t {
  none,  // [null, null, null]
  reserved,
  premodel,
  other,  // a class record whose name has no reader
  CVertex,
  CEdge,
  CCurve,
  CArcCurve,
  CEdgeUse,
  CLoop,
  CFace,
  CLayer,
  CMaterial,
  CDib,
  CAttributeContainer,
  CAttributeNamed,
  CCamera,
  CThumbnail,
  CRelationship,
  CComponentDefinition,
  CImage,
  CComponentInstance,
  CGroup,
  CFaceTextureCoords,
  CConstructionLine,
  CConstructionPoint,
  CSectionPlane,
  CSkFont,
  CDimensionLinear,
  CText,
};

struct ClassName {
  std::string_view name;
  Cls cls;
};
constexpr std::array<ClassName, 26> kClasses = {{
    {"CVertex", Cls::CVertex},
    {"CEdge", Cls::CEdge},
    {"CCurve", Cls::CCurve},
    {"CArcCurve", Cls::CArcCurve},
    {"CEdgeUse", Cls::CEdgeUse},
    {"CLoop", Cls::CLoop},
    {"CFace", Cls::CFace},
    {"CLayer", Cls::CLayer},
    {"CMaterial", Cls::CMaterial},
    {"CDib", Cls::CDib},
    {"CAttributeContainer", Cls::CAttributeContainer},
    {"CAttributeNamed", Cls::CAttributeNamed},
    {"CCamera", Cls::CCamera},
    {"CThumbnail", Cls::CThumbnail},
    {"CRelationship", Cls::CRelationship},
    {"CComponentDefinition", Cls::CComponentDefinition},
    {"CImage", Cls::CImage},
    {"CComponentInstance", Cls::CComponentInstance},
    {"CGroup", Cls::CGroup},
    {"CFaceTextureCoords", Cls::CFaceTextureCoords},
    {"CConstructionLine", Cls::CConstructionLine},
    {"CConstructionPoint", Cls::CConstructionPoint},
    {"CSectionPlane", Cls::CSectionPlane},
    {"CSkFont", Cls::CSkFont},
    {"CDimensionLinear", Cls::CDimensionLinear},
    {"CText", Cls::CText},
}};
Cls cls_of(std::string_view name) {
  for (const ClassName& c : kClasses) {
    if (c.name == name) {
      return c.cls;
    }
  }
  return Cls::other;
}
std::string_view name_of(Cls c) {
  for (const ClassName& n : kClasses) {
    if (n.cls == c) {
      return n.name;
    }
  }
  if (c == Cls::reserved) {
    return "reserved";
  }
  if (c == Cls::premodel) {
    return "premodel";
  }
  return {};
}

/// An object reference as the JS's [slot, className, value]: slot nullopt = null; idx -1 = value null.
struct Obj {
  std::optional<std::int64_t> slot;
  Cls cls = Cls::none;
  std::int32_t idx = -1;
  [[nodiscard]] bool has_value() const { return idx >= 0; }
};
/// A value only (an element of a loop's uses, a face's loops, an attribute container's children).
struct Ref {
  Cls cls = Cls::none;
  std::int32_t idx = -1;
};

struct Db {
  std::uint32_t mat = 0;
  bool hidden = false;
  bool soft = false;
  bool smooth = false;
  std::optional<std::int64_t> layer;
};
struct VEdge {
  Db db;
  std::optional<std::int64_t> v1;
  std::optional<std::int64_t> v2;
};
struct VUse {
  std::optional<std::int64_t> edge;
  std::uint8_t sense = 0;
};
struct VLoop {
  std::vector<Ref> uses;
};
struct VFace {
  Db db;
  std::array<double, 4> plane{};
  std::vector<Ref> loops;
  std::uint16_t back = 0;
  std::int32_t ftc = -1;
};
struct VLayer {
  std::string name;
  bool visible = true;
  std::array<std::uint8_t, 4> rgba{};
};
struct VTex {
  std::array<std::uint8_t, 4> rgba{};
  double opacity = 0;
  std::uint8_t use_opacity = 0;
  std::span<const std::uint8_t> image;
  double w = 0;
  double h = 0;
  std::string file;
};
struct VMaterial {
  std::string name;
  std::array<std::uint8_t, 4> rgba{};
  double opacity = 0;
  std::uint8_t use_opacity = 0;
  std::optional<VTex> tex;
};
struct VDib {
  std::uint32_t subtype = 0;
  std::span<const std::uint8_t> data;
};
struct VAttrs {
  std::vector<Ref> children;
};
struct VFtc {
  std::array<double, 9> front{};
  bool projected = false;
};
struct VDef {
  std::string name;
  std::string guid;
  std::vector<Obj> ents;
};
struct VInst {
  Cls cls = Cls::CComponentInstance;
  Db db;
  std::optional<std::int64_t> def;
  std::array<double, 13> xf{};
  std::string name;
};

struct Slot {
  bool used = false;
  bool is_class = false;
  bool released = false;
  Cls cls = Cls::none;
  std::int32_t idx = -1;
  std::int32_t name = -1;  // a class record's name, when it has no reader (index into Archive::other_names_)
};

/// The store map: dense from `base` (allocation is sequential), sparse elsewhere.
class Slots {
 public:
  void reset(std::int64_t base) {
    lo_ = base;
    dense_.clear();
    sparse_.clear();
  }
  [[nodiscard]] const Slot* get(std::int64_t s) const {
    if (s >= lo_ && s - lo_ < static_cast<std::int64_t>(dense_.size())) {
      const Slot& e = dense_[static_cast<std::size_t>(s - lo_)];
      return e.used ? &e : nullptr;
    }
    const auto it = sparse_.find(s);
    return it == sparse_.end() ? nullptr : &it->second;
  }
  Slot* get_mut(std::int64_t s) { return const_cast<Slot*>(std::as_const(*this).get(s)); }  // NOLINT
  void set(std::int64_t s, Slot e) {
    e.used = true;
    if (s >= lo_ && s - lo_ < static_cast<std::int64_t>(dense_.size())) {
      dense_[static_cast<std::size_t>(s - lo_)] = e;
    } else if (s == lo_ + static_cast<std::int64_t>(dense_.size())) {
      dense_.push_back(e);
      sparse_.erase(s);
    } else {
      sparse_[s] = e;
    }
  }
  /// Every slot in [from, to) of the dense part (where objects are allocated).
  template <class F>
  void each_dense(std::int64_t from, std::int64_t to, const F& fn) {
    const std::int64_t a = std::max(from, lo_);
    const std::int64_t z = std::min(to, lo_ + static_cast<std::int64_t>(dense_.size()));
    for (std::int64_t s = a; s < z; ++s) {
      Slot& e = dense_[static_cast<std::size_t>(s - lo_)];
      if (e.used) {
        fn(e);
      }
    }
  }

 private:
  std::int64_t lo_ = 0;
  std::vector<Slot> dense_;
  std::unordered_map<std::int64_t, Slot> sparse_;
};

struct Layout {
  double major = 0;
  bool v3 = false;
  bool pid = false;
  bool attr_tail = false;
  bool inst_guid = false;
};

Layout layout_for(double major, const std::map<std::string, std::uint32_t>& schemas) {
  const auto sch = [&schemas](const char* name, double fallback) {
    const auto it = schemas.find(name);
    return it == schemas.end() ? fallback : static_cast<double>(it->second);
  };
  Layout l;
  l.major = major;
  l.v3 = major < 4;
  l.pid = sch("CEntity", major >= 17 ? 5 : 3) >= 5;
  l.attr_tail = sch("CAttributeNamed", major >= 7 ? 1 : 0) >= 1;
  l.inst_guid = sch("CComponentInstance", major >= 14 ? 5 : 4) >= 5;
  return l;
}

struct Built {
  std::string name;
  std::string guid;
  struct Face {
    std::vector<std::vector<P3>> loops;
    std::array<double, 4> plane{};
    std::uint32_t front = 0;  // store-map index, 0 = none
    std::uint32_t back = 0;
    std::optional<std::int64_t> layer;
    bool hidden = false;
    std::int32_t ftc = -1;
  };
  struct LooseEdge {
    P3 a{};
    P3 b{};
    std::optional<std::int64_t> layer;
    bool soft = false;
    bool smooth = false;
    bool hidden = false;
  };
  std::vector<Face> faces;
  std::vector<LooseEdge> edges;
  std::vector<std::array<P3, 2>> soft;
  std::vector<std::int32_t> insts;  // VInst indexes
};

class Archive {
 public:
  Archive(std::span<const std::uint8_t> bytes, Layout lay, const i18n::Catalog* cat)
      : b(bytes), lay(lay), r(bytes, 0, cat), cat(cat) {}

  std::span<const std::uint8_t> b;
  Layout lay;
  Reader r;
  const i18n::Catalog* cat;
  Slots slots;
  std::map<Cls, std::int64_t> class_slot;  // class → index of its class record (named classes only)
  std::int64_t next = 0;
  std::int64_t base = 0;
  std::size_t depth = 0;
  std::uint64_t vertices = 0;
  std::optional<std::int64_t> current_loop;
  std::optional<std::int64_t> current_slot;
  std::vector<std::pair<std::int64_t, std::int64_t>> burns;
  std::int64_t burn_total = 0;
  std::optional<std::int64_t> annot_mark;
  std::vector<std::int64_t> burn_credits;
  std::optional<std::size_t> cline_tail;
  std::uint64_t root_cut = 0;
  struct Skipped {
    std::uint64_t text = 0, dims = 0, sections = 0, guides = 0;
  } skipped;
  std::vector<std::string> other_names;

  // value pools
  std::vector<P3> verts;
  std::vector<VEdge> edges;
  std::vector<VUse> uses;
  std::vector<VLoop> loops;
  std::vector<VFace> faces;
  std::vector<VLayer> layers;
  std::vector<VMaterial> materials;
  std::vector<VDib> dibs;
  std::vector<VAttrs> attrs;
  std::vector<VFtc> ftcs;
  std::vector<VDef> defs;
  std::vector<VInst> insts;

  // what read() does with each completed definition
  std::function<void(Archive&, std::int64_t, std::int32_t)> on_definition;

  std::int64_t alloc(Slot e) {
    if (next - base > kMaxSlots) {
      fail("too_big", msg(cat, "ccskp.too_big.objects"));
    }
    const std::int64_t s = next++;
    slots.set(s, e);
    return s;
  }

  /// [slot, class, value] of a slot's entry for a back-reference.
  [[nodiscard]] Obj entry_obj(std::int64_t s, const Slot& e) const { return Obj{s, e.cls, e.released ? -1 : e.idx}; }

  // NOLINTNEXTLINE(misc-no-recursion) — object nesting is capped at kMaxObjectDepth
  Obj obj(Reader& rd, Cls expect = Cls::none);
  // NOLINTNEXTLINE(misc-no-recursion)
  Obj of_class(Reader& rd, std::int64_t cslot, Cls expect);
  // NOLINTNEXTLINE(misc-no-recursion)
  Obj new_obj(Reader& rd, Cls cls, std::string_view name);

  [[nodiscard]] std::optional<std::int64_t> translate(std::int64_t slot) const {
    std::int64_t off = 0;
    for (const auto& [start, width] : burns) {
      if (slot < start) {
        break;
      }
      if (slot < start + width) {
        return std::nullopt;
      }
      off += width;
    }
    return slot - off;
  }

  Obj backref(std::int64_t slot, Reader& rd) {
    if (!burns.empty() && slot >= burns[0].first) {
      const auto t = translate(slot);
      if (!t) {
        return Obj{slot, Cls::reserved, -1};
      }
      slot = *t;
    }
    const Slot* e = slots.get(slot);
    if (e == nullptr) {
      if (slot < base) {
        return Obj{slot, Cls::premodel, -1};
      }
      misparse(cat, "ccskp.legacy.ref_unread", {{"at", at_text(rd.pos)}});
    }
    if (e->is_class) {
      misparse(cat, "ccskp.legacy.ref_class_record", {{"at", at_text(rd.pos)}});
    }
    return entry_obj(slot, *e);
  }

  /// Class-ref to `slot` at p, short (0x8000|slot) or escaped (0x7FFF + 0x80000000|slot).
  [[nodiscard]] bool is_class_ref(std::int64_t p, std::int64_t slot) const {
    if (slot <= 0x7fff) {
      return u16at(b, p) == (0x8000 | slot);
    }
    return u16at(b, p) == 0x7fff &&
           u32at(b, p + 2) ==
               static_cast<std::int64_t>((0x80000000ULL | static_cast<std::uint64_t>(slot)) & 0xffffffffULL);
  }

  /// Is there a CLayer record at `at` (new class record, or a class-ref to the known CLayer class)?
  [[nodiscard]] bool layer_record_at(std::int64_t at, bool any_class_ref = false) const {
    const std::int64_t t = u16at(b, at);
    if (t == 0xffff && u16at(b, at + 4) == 6 && ascii_at(b, at + 6, "CLayer")) {
      return true;
    }
    if (const auto it = class_slot.find(Cls::CLayer); it != class_slot.end() && is_class_ref(at, it->second)) {
      return true;
    }
    return any_class_ref && t != -1 && (t & 0x8000) != 0 && t != 0xffff;
  }

  /// Layer records past the declared count, with null separators.
  void trailing_layers(std::vector<Obj>& out, bool any_class_ref = false) {
    while (r.pos + 2 <= r.end) {
      if (u16at(b, static_cast<std::int64_t>(r.pos)) == 0) {
        r.pos += 2;
        continue;
      }
      if (!layer_record_at(static_cast<std::int64_t>(r.pos), any_class_ref)) {
        break;
      }
      const Obj o = obj(r, Cls::CLayer);
      if (o.has_value()) {
        out.push_back(o);
      }
    }
  }

  [[nodiscard]] const Slot* slot(std::optional<std::int64_t> s) const { return s ? slots.get(*s) : nullptr; }
};

/// Can the u16 at `at` start an object read without doubt (null, escape, class record, known class-ref)?
bool strict_tag(const Archive& ar, std::int64_t at, bool allow_null = true) {
  const std::int64_t t = u16at(ar.b, at);
  if (t == -1) {
    return false;
  }
  if (t == 0) {
    return allow_null;
  }
  if (t == 0x7fff || t == 0xffff) {
    return true;
  }
  if ((t & 0x8000) != 0) {
    const Slot* e = ar.slots.get(t & 0x7fff);
    return e != nullptr && e->is_class;
  }
  return false;
}
/// Like strict_tag, but a plain back-ref inside the walked range also counts.
bool plausible_tag(const Archive& ar, std::int64_t at) {
  const std::int64_t t = u16at(ar.b, at);
  if (t == -1) {
    return false;
  }
  if (t == 0 || t == 0x7fff || t == 0xffff) {
    return true;
  }
  if ((t & 0x8000) != 0) {
    const Slot* e = ar.slots.get(t & 0x7fff);
    return e != nullptr && e->is_class;
  }
  return t < ar.next;
}

/// SketchUp 2020's filler before a count (legacy.js retryAfterFiller). Returns the count (cursor moved) or nullopt.
std::optional<std::uint32_t> retry_after_filler(Archive& ar, Reader& r, std::int64_t count_pos, std::uint32_t limit) {
  std::int64_t m = -1;
  for (std::int64_t i = count_pos; i < count_pos + 12 && i + 4 <= static_cast<std::int64_t>(ar.b.size()); ++i) {
    if (i >= 0 && str_at(ar.b, static_cast<std::size_t>(i))) {
      m = i;
      break;
    }
  }
  if (m < 0 || ar.b[static_cast<std::size_t>(m) + 3] != 0) {
    return std::nullopt;
  }
  for (std::int64_t pad = 1; pad <= 29; pad += 4) {
    const std::int64_t at = m + 4 + pad;
    const std::int64_t n = u32at(ar.b, at);
    if (n == -1) {
      break;
    }
    if (n > 0 && std::cmp_less_equal(n, limit) && plausible_tag(ar, at + 4)) {
      r.pos = static_cast<std::size_t>(at + 4);
      return static_cast<std::uint32_t>(n);
    }
  }
  return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ shared blocks
struct Pre {
  Obj attrs;
};
// NOLINTNEXTLINE(misc-no-recursion)
Pre preamble(Archive& ar, Reader& r) {
  const Obj attrs = ar.obj(r, Cls::CAttributeContainer);
  if (ar.lay.pid) {
    const std::uint8_t mask = r.u8();
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((mask & (1U << bit)) != 0) {
        r.u8();
      }
    }
  }
  return {attrs};
}
// NOLINTNEXTLINE(misc-no-recursion)
void early_preamble(Archive& ar, Reader& r) {
  if (!ar.lay.v3) {
    preamble(ar, r);
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
Db drawbase(Archive& ar, Reader& r) {
  const auto blk = r.take(ar.lay.v3 ? 5 : 8);
  Db db;
  const std::int64_t t = peek16(r);
  if (t == 0x7fff || (t & 0x8000) != 0) {
    const Obj o = ar.obj(r, Cls::CLayer);
    if (o.cls == Cls::CLayer) {
      db.layer = o.slot;
    }
  } else {
    r.pos += 2;
    std::optional<std::int64_t> s;
    if (t != 0) {
      s = ar.burns.empty() ? std::optional<std::int64_t>(t) : ar.translate(t);
    }
    if (s && *s != 0) {
      const Slot* e = ar.slots.get(*s);
      if (e != nullptr && e->cls == Cls::CLayer) {
        db.layer = s;
      }
    }
  }
  if (ar.lay.v3) {
    return db;
  }
  db.mat = static_cast<std::uint32_t>(blk[0] | (blk[1] << 8U));
  db.hidden = blk[2] != 0;
  db.soft = blk[5] != 0;
  db.smooth = blk[6] != 0;
  return db;
}

// ------------------------------------------------------------------------------------------------ entity readers
// Each returns its value's index in its class's pool (0 for the classes that keep nothing).
using ReadFn = std::int32_t (*)(Archive&, Reader&);

std::int32_t push_index(std::size_t n) {
  return static_cast<std::int32_t>(n - 1);
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_vertex(Archive& ar, Reader& r) {
  early_preamble(ar, r);
  if (++ar.vertices > Limits::vertices) {
    fail("too_big", msg(ar.cat, "ccskp.too_big.vertices", {{"max", std::to_string(Limits::vertices)}}));
  }
  const auto p = f64s<3>(r);
  if (!std::ranges::all_of(p, ok_num)) {
    misparse(ar.cat, "ccskp.legacy.vertex_range", {{"at", at_text(r.pos)}});
  }
  ar.verts.push_back(p);
  return push_index(ar.verts.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_edge(Archive& ar, Reader& r) {
  preamble(ar, r);
  VEdge e;
  e.db = drawbase(ar, r);
  e.v1 = ar.obj(r, Cls::CVertex).slot;
  e.v2 = ar.obj(r, Cls::CVertex).slot;
  const Obj c = ar.obj(r);
  if (c.cls != Cls::none && c.cls != Cls::CCurve && c.cls != Cls::CArcCurve) {
    misparse(ar.cat, "ccskp.legacy.edge_curve", {{"type", std::string(name_of(c.cls))}, {"at", at_text(r.pos)}});
  }
  ar.edges.push_back(e);
  return push_index(ar.edges.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_curve(Archive& ar, Reader& r) {
  preamble(ar, r);
  r.u8();
  r.u32();
  return 0;
}
// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_arc_curve(Archive& ar, Reader& r) {
  preamble(ar, r);
  r.skip(5);
  r.skip(std::size_t{14} * 8);
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_edge_use(Archive& ar, Reader& r) {
  early_preamble(ar, r);
  VUse u;
  u.edge = ar.obj(r, Cls::CEdge).slot;
  u.sense = r.u8();
  // The parent-loop back-ref must name the loop being read; running AHEAD right after an annotation = burned indexes.
  const std::size_t p0 = r.pos;
  const std::int64_t tag = r.u16();
  std::optional<std::int64_t> parent;
  if (tag == 0x7fff) {
    const std::uint32_t big = r.u32();
    if ((big & 0x80000000U) != 0) {
      misparse(ar.cat, "ccskp.legacy.edgeuse_parent", {{"at", at_text(r.pos)}});
    }
    parent = big;
  } else if (tag == 0xffff || (tag & 0x8000) != 0) {
    misparse(ar.cat, "ccskp.legacy.edgeuse_parent", {{"at", at_text(r.pos)}});
  } else if (tag != 0) {
    parent = tag;
  }
  const std::optional<std::int64_t> want =
      ar.current_loop ? std::optional<std::int64_t>(*ar.current_loop + ar.burn_total) : std::nullopt;
  if (parent != want) {
    const std::int64_t delta = parent && want ? *parent - *want : 0;
    if (delta > 0 && delta <= 4096 && ar.annot_mark) {
      ar.burns.emplace_back(*ar.annot_mark + ar.burn_total, delta);
      ar.burn_total += delta;
      ar.annot_mark.reset();
      if (!ar.burn_credits.empty()) {
        ++ar.burn_credits.back();
      }
    } else {
      r.pos = p0;
      misparse(ar.cat, "ccskp.legacy.edgeuse_loop", {{"at", at_text(r.pos)}});
    }
  }
  ar.uses.push_back(u);
  return push_index(ar.uses.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_loop(Archive& ar, Reader& r) {
  const auto prev = ar.current_loop;
  ar.current_loop = ar.current_slot;
  early_preamble(ar, r);
  r.skip(2);
  VLoop loop;
  while (peek16(r) != 0) {
    const Obj o = ar.obj(r, Cls::CEdgeUse);
    loop.uses.push_back(Ref{o.cls, o.idx});
  }
  r.pos += 2;
  ar.current_loop = prev;
  ar.loops.push_back(std::move(loop));
  return push_index(ar.loops.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_face(Archive& ar, Reader& r) {
  const Pre pre = preamble(ar, r);
  VFace f;
  f.db = drawbase(ar, r);
  Obj attrs = pre.attrs;
  if (ar.lay.v3 && attrs.has_value() &&
      attrs.cls == Cls::CMaterial) {  // SketchUp 3: the opening pointer = front material
    f.db.mat = static_cast<std::uint32_t>(attrs.slot.value_or(0));
    attrs = Obj{};
  }
  f.plane = f64s<4>(r);
  const std::uint32_t n = r.u32();
  if (n > 10000) {
    misparse(ar.cat, "ccskp.legacy.loop_count", {{"at", at_text(r.pos)}});
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    const Obj o = ar.obj(r, Cls::CLoop);
    f.loops.push_back(Ref{o.cls, o.idx});
  }
  f.back = r.u16();
  if (attrs.has_value() && attrs.cls == Cls::CAttributeContainer) {
    for (const Ref& c : ar.attrs[static_cast<std::size_t>(attrs.idx)].children) {
      if (c.idx >= 0 && c.cls == Cls::CFaceTextureCoords) {
        f.ftc = c.idx;
      }
    }
  }
  ar.faces.push_back(std::move(f));
  return push_index(ar.faces.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_attr_container(Archive& ar, Reader& r) {
  preamble(ar, r);
  VAttrs a;
  while (peek16(r) != 0) {
    const Obj o = ar.obj(r, Cls::CAttributeNamed);
    a.children.push_back(Ref{o.cls, o.idx});
  }
  r.pos += 2;
  ar.attrs.push_back(std::move(a));
  return push_index(ar.attrs.size());
}

/// A typed attribute / option value; only what readUnits needs is kept (a number, a string, an array's String()).
struct Typed {
  enum class Kind : std::uint8_t { null, number, string, array };
  Kind kind = Kind::null;
  double num = 0;
  std::string text;  // a string, or an array's String()
};
// NOLINTNEXTLINE(misc-no-recursion) — depth capped at 8
Typed typed_value(Reader& r, int depth, const i18n::Catalog* cat) {
  const std::uint8_t t = r.u8();
  switch (t) {
    case 0x00: return {};
    case 0x04: return {Typed::Kind::number, static_cast<double>(r.i32()), {}};
    case 0x06:
    case 0x0c: return {Typed::Kind::number, r.f64(), {}};
    case 0x07: return {Typed::Kind::number, static_cast<double>(r.u8()), {}};
    case 0x09: return {Typed::Kind::number, static_cast<double>(r.u32()), {}};
    case 0x0a: {
      Typed v{Typed::Kind::string, 0, str(r, cat)};
      return v;
    }
    case 0x11:
    case 0x12: {
      const auto a = f64s<3>(r);
      return {Typed::Kind::array, 0, num_text(a[0]) + "," + num_text(a[1]) + "," + num_text(a[2])};
    }
    case 0x0b: {
      const std::uint32_t n = r.u32();
      if (n > 100000 || depth > 8) {
        misparse(cat, "ccskp.legacy.attr_array", {{"at", at_text(r.pos)}});
      }
      std::string s;
      for (std::uint32_t i = 0; i < n; ++i) {
        const Typed x = typed_value(r, depth + 1, cat);
        if (i != 0) {
          s += ',';
        }
        if (x.kind == Typed::Kind::number) {
          s += num_text(x.num);
        }
        if (x.kind == Typed::Kind::string || x.kind == Typed::Kind::array) {
          s += x.text;
        }
      }
      return {Typed::Kind::array, 0, s};
    }
    default: misparse(cat, "ccskp.legacy.attr_type", {{"type", std::to_string(t)}, {"at", at_text(r.pos)}});
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_attr_named(Archive& ar, Reader& r) {
  preamble(ar, r);
  r.skip(4);
  (void)str(r, ar.cat);
  for (;;) {
    const std::string key = str(r, ar.cat);
    if (key.empty()) {
      break;
    }
    (void)typed_value(r, 0, ar.cat);
  }
  if (ar.lay.attr_tail) {
    r.u32();
  }
  return 0;
}

/// CDib: subtype u32, length u32, the image; a JPEG (subtype 1) is followed by a u32 quality.
std::int32_t read_dib(Archive& ar, Reader& r) {
  VDib d;
  d.subtype = r.u32();
  const std::uint32_t len = r.u32();
  if (len > r.left()) {
    misparse(ar.cat, "ccskp.legacy.image_long", {{"at", at_text(r.pos)}});
  }
  d.data = r.take(len);
  if (d.subtype == 1 && len > 0) {
    r.u32();
  }
  ar.dibs.push_back(d);
  return push_index(ar.dibs.size());
}

/// A material's texture (legacy.js textureBlock).
// NOLINTNEXTLINE(misc-no-recursion)
VTex texture_block(Archive& ar, Reader& r) {
  std::int32_t dib = -1;
  if (ar.lay.v3) {
    dib = read_dib(ar, r);
  } else {
    r.pos -= 1;
    preamble(ar, r);
    const Obj o = ar.obj(r, Cls::CDib);
    if (!o.has_value() || o.cls != Cls::CDib) {
      misparse(ar.cat, "ccskp.legacy.texture_image", {{"at", at_text(r.pos)}});
    }
    dib = o.idx;
  }
  const std::int64_t m = find(ar.b, kStr, static_cast<std::int64_t>(r.pos), static_cast<std::int64_t>(r.pos) + 28);
  if (m - static_cast<std::int64_t>(r.pos) == 20) {
    r.u32();
  } else if (m - static_cast<std::int64_t>(r.pos) != 16) {
    misparse(ar.cat, "ccskp.legacy.texture_size", {{"at", at_text(r.pos)}});
  }
  VTex t;
  t.w = r.f64();
  t.h = r.f64();
  t.file = str(r, ar.cat);
  const auto avg = r.take(9);
  std::copy_n(avg.begin(), 4, t.rgba.begin());
  (void)str(r, ar.cat);
  r.skip(8);
  t.opacity = r.f64();
  t.use_opacity = r.u8();
  t.image = ar.dibs[static_cast<std::size_t>(dib)].data;
  return t;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_layer(Archive& ar, Reader& r) {
  early_preamble(ar, r);
  VLayer l;
  l.name = str(r, ar.cat);
  std::vector<std::uint8_t> mid;
  while (mid.size() < 8 && !str_at(ar.b, r.pos)) {
    mid.push_back(r.u8());  // flag bytes; the first is "hidden"
  }
  (void)str(r, ar.cat);  // internal name "Layer_<name>"
  const std::uint16_t flags = r.u16();
  l.visible = mid.empty() || mid[0] == 0;
  if ((flags & 0xffU) != 0) {
    // colour by layer with a TEXTURED material: the layer carries a whole texture block
    const VTex tex = texture_block(ar, r);
    r.skip(4);
    l.rgba = tex.rgba;
    ar.layers.push_back(std::move(l));
    return push_index(ar.layers.size());
  }
  const auto rgba = r.take(4);
  std::copy_n(rgba.begin(), 4, l.rgba.begin());
  (void)str(r, ar.cat);
  r.skip(ar.lay.v3 ? 17 : 21);
  // v18 zero-material files: 12 zero bytes + u32 before another CLayer
  if (r.pos + 18 <= r.end &&
      std::all_of(ar.b.begin() + static_cast<std::ptrdiff_t>(r.pos),
                  ar.b.begin() + static_cast<std::ptrdiff_t>(r.pos + 12), [](std::uint8_t x) { return x == 0; }) &&
      ar.layer_record_at(static_cast<std::int64_t>(r.pos) + 16, true)) {
    r.pos += 16;
  }
  // v18 template Layer0 after a custom tag: <parent u16><active u16> when both name CLayer objects
  const std::int64_t a = u16at(ar.b, static_cast<std::int64_t>(r.pos));
  const std::int64_t b = u16at(ar.b, static_cast<std::int64_t>(r.pos) + 2);
  if (a > 0 && b > 0 && (a & 0x8000) == 0 && (b & 0x8000) == 0 && a != 0x7fff && b != 0x7fff &&
      (!ar.current_slot || a != *ar.current_slot)) {
    const Slot* ea = ar.slots.get(a);
    const Slot* eb = ar.slots.get(b);
    if (ea != nullptr && eb != nullptr && !ea->is_class && !eb->is_class && ea->cls == Cls::CLayer &&
        eb->cls == Cls::CLayer) {
      r.pos += 2;
    }
  }
  ar.layers.push_back(std::move(l));
  return push_index(ar.layers.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_material(Archive& ar, Reader& r) {
  early_preamble(ar, r);
  VMaterial m;
  m.name = str(r, ar.cat);
  const std::uint16_t tex_flag = r.u16();
  if (tex_flag == 0) {
    const auto rgba = r.take(4);
    std::copy_n(rgba.begin(), 4, m.rgba.begin());
    (void)str(r, ar.cat);
    r.skip(8);
    m.opacity = r.f64();
    m.use_opacity = r.u8();
  } else {
    VTex tex = texture_block(ar, r);
    m.rgba = tex.rgba;
    m.opacity = tex.opacity;
    m.use_opacity = tex.use_opacity;
    m.tex = std::move(tex);
  }
  ar.materials.push_back(std::move(m));
  return push_index(ar.materials.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_ftc(Archive& ar, Reader& r) {
  preamble(ar, r);
  r.u32();
  const auto ks = f64s<24>(r);
  for (int side = 0; side < 2; ++side) {
    const std::uint32_t n = r.u32();
    if (n > 64) {
      misparse(ar.cat, "ccskp.legacy.texture_pins", {{"at", at_text(r.pos)}});
    }
    r.skip(static_cast<std::size_t>(n) * 32);
  }
  const std::uint32_t ff = r.u32();
  r.u32();
  VFtc f;
  std::copy_n(ks.begin(), 9, f.front.begin());
  f.projected = (ff & 2U) != 0;
  ar.ftcs.push_back(f);
  return push_index(ar.ftcs.size());
}

std::int32_t read_camera(Archive& ar, Reader& r) {
  r.skip(137);
  r.u16();
  (void)str(r, ar.cat);
  r.skip(33);
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_thumbnail(Archive& ar, Reader& r) {
  preamble(ar, r);
  if (ar.lay.v3) {
    r.skip(129);
    return 0;
  }
  ar.obj(r, Cls::CCamera);
  ar.obj(r, Cls::CDib);
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_image(Archive& ar, Reader& r) {
  preamble(ar, r);
  VInst in;
  in.cls = Cls::CImage;
  in.db = drawbase(ar, r);
  in.def = ar.obj(r).slot;
  const auto xf = f64s<12>(r);
  std::ranges::copy(xf, in.xf.begin());
  in.xf[12] = 1;
  r.f64();
  (void)str(r, ar.cat);
  r.skip(16);
  ar.insts.push_back(std::move(in));
  return push_index(ar.insts.size());
}

/// A reference to an entity that may point FORWARD.
std::optional<std::int64_t> entity_ref(Reader& r, const i18n::Catalog* cat) {
  const std::int64_t tag = r.u16();
  if (tag == 0) {
    return std::nullopt;
  }
  if (tag == 0x7fff) {
    const std::uint32_t big = r.u32();
    if ((big & 0x80000000U) != 0) {
      misparse(cat, "ccskp.legacy.entity_ref", {{"at", at_text(r.pos)}});
    }
    return big;
  }
  if (tag == 0xffff || (tag & 0x8000) != 0) {
    misparse(cat, "ccskp.legacy.entity_ref", {{"at", at_text(r.pos)}});
  }
  return tag;
}

// NOLINTNEXTLINE(misc-no-recursion)
void connection_paths(Archive& ar, Reader& r) {
  (void)entity_ref(r, ar.cat);
  for (int k = 0; k < 2; ++k) {
    const std::uint32_t n = r.u32();
    if (n > 1000) {
      misparse(ar.cat, "ccskp.legacy.dim_path", {{"at", at_text(r.pos)}});
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int64_t t = peek16(r);
      bool is_new = t == 0xffff || (t != 0x7fff && (t & 0x8000) != 0);
      if (t == 0x7fff) {
        is_new = high_bit(u32at(ar.b, static_cast<std::int64_t>(r.pos) + 2));
      }
      if (is_new) {
        ar.obj(r);
      } else {
        (void)entity_ref(r, ar.cat);
      }
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_relationship(Archive& ar, Reader& r) {
  preamble(ar, r);
  (void)entity_ref(r, ar.cat);
  (void)entity_ref(r, ar.cat);
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_construction_line(Archive& ar, Reader& r) {
  preamble(ar, r);
  drawbase(ar, r);
  r.skip(std::size_t{8} * 8);
  // The tail is 0, 4 or 7 bytes by the build that wrote the file: calibrate on the first one and keep it.
  if (!ar.cline_tail) {
    const std::size_t dflt = ar.lay.major == 17 ? 7 : 4;
    std::vector<std::size_t> order{dflt};
    for (const std::size_t c : {std::size_t{0}, std::size_t{4}, std::size_t{7}}) {
      if (c != dflt) {
        order.push_back(c);
      }
    }
    std::optional<std::size_t> k;
    for (const bool allow_null : {false, true}) {
      for (const std::size_t c : order) {
        if (strict_tag(ar, static_cast<std::int64_t>(r.pos + c), allow_null)) {
          k = c;
          break;
        }
      }
      if (k) {
        break;
      }
    }
    ar.cline_tail = k.value_or(dflt);
  }
  r.skip(*ar.cline_tail);
  ++ar.skipped.guides;
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_construction_point(Archive& ar, Reader& r) {
  preamble(ar, r);
  drawbase(ar, r);
  r.skip(48);
  r.u8();
  ++ar.skipped.guides;
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_section_plane(Archive& ar, Reader& r) {
  preamble(ar, r);
  drawbase(ar, r);
  r.need(8);
  const double first = Reader::f64_at(ar.b, r.pos);
  if (!(std::fabs(first) <= 1.0001)) {
    ar.obj(r);  // optional object pointer before the plane
  }
  r.skip(32);
  if (str_at(ar.b, r.pos)) {
    (void)str(r, ar.cat);
    (void)str(r, ar.cat);
  }
  ++ar.skipped.sections;
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_sk_font(Archive& ar, Reader& r) {
  ar.obj(r, Cls::CAttributeContainer);
  if (ar.lay.pid) {
    r.u8();
  }
  (void)str(r, ar.cat);
  r.skip(15);
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_dim_linear(Archive& ar, Reader& r) {
  preamble(ar, r);
  drawbase(ar, r);
  (void)str(r, ar.cat);
  ar.obj(r, Cls::CSkFont);
  r.skip(37);
  (void)entity_ref(r, ar.cat);
  connection_paths(ar, r);
  r.skip(32);
  (void)entity_ref(r, ar.cat);
  connection_paths(ar, r);
  r.skip(72);
  ++ar.skipped.dims;
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_text(Archive& ar, Reader& r) {
  preamble(ar, r);
  drawbase(ar, r);
  ar.obj(r, Cls::CSkFont);
  // a variable middle, closed by `01 00 00 00 ?? 00 03 00 00 00 01` right before the text record
  auto p = static_cast<std::int64_t>(r.pos);
  std::int64_t at = 0;
  for (;;) {
    at = find(ar.b, kStr, p, static_cast<std::int64_t>(r.pos) + 512);
    if (at < 0) {
      misparse(ar.cat, "ccskp.legacy.text_label", {{"at", at_text(r.pos)}});
    }
    const std::int64_t k = at - 11;
    if (std::cmp_greater_equal(k, r.pos) && u32at(ar.b, k) == 1 && ar.b[static_cast<std::size_t>(k) + 5] == 0 &&
        u32at(ar.b, k + 6) == 3 && ar.b[static_cast<std::size_t>(k) + 10] == 1) {
      break;
    }
    p = at + 3;
  }
  r.pos = static_cast<std::size_t>(at);
  (void)str(r, ar.cat);
  r.skip(5);
  // optional leader attachments in the escaped 6-byte form only
  while (u16at(ar.b, static_cast<std::int64_t>(r.pos)) == 0x7fff && r.pos + 6 <= r.end &&
         !high_bit(u32at(ar.b, static_cast<std::int64_t>(r.pos) + 2))) {
    r.pos += 6;
  }
  ++ar.skipped.text;
  return 0;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::vector<Obj> read_entity_list(Archive& ar, Reader& r, std::uint32_t count, bool is_root) {
  std::vector<Obj> ents;
  ar.burn_credits.push_back(0);
  const ScopeExit pop([&ar] { ar.burn_credits.pop_back(); });
  while (ents.size() < count) {
    const std::size_t p = r.pos;
    const bool credit = !is_root && ar.burn_credits.back() > 0;
    // burned indexes also count as phantom list items: the definition tail (nrel = 0, padding, GUID, name record at
    // +22) marks the list's real end
    if (credit && u32at(ar.b, static_cast<std::int64_t>(p)) == 0 && str_at(ar.b, p + 22)) {
      break;
    }
    try {
      ents.push_back(ar.obj(r));
    } catch (const Misparse&) {
      if (is_root || credit) {
        r.pos = p;
        if (is_root) {
          ar.root_cut = count - ents.size();
        }
        break;
      }
      throw;
    }
  }
  return ents;
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_definition(Archive& ar, Reader& r) {
  preamble(ar, r);
  std::size_t undecoded = 20;  // an undecoded block
  if (ar.lay.pid) {
    undecoded = 22;
  } else if (ar.lay.v3) {
    undecoded = 13;
  }
  r.skip(undecoded);
  const std::uint32_t nl = r.u32();
  if (nl > 10000) {
    misparse(ar.cat, "ccskp.legacy.def_layers", {{"at", at_text(r.pos)}});
  }
  for (std::uint32_t got = 0; got < nl;) {
    if (peek16(r) == 0) {  // SketchUp 2020 puts null separators between layers
      r.pos += 2;
      continue;
    }
    ar.obj(r, Cls::CLayer);
    ++got;
  }
  if (r.u16() == 0x7fff) {
    r.u32();
  }
  std::optional<std::uint32_t> count;
  if (ar.lay.major >= 20) {
    count = retry_after_filler(ar, r, static_cast<std::int64_t>(r.pos), 5'000'000);
  }
  if (!count) {
    r.u32();
    count = r.u32();
  }
  if (*count > 5'000'000 || *count == 0) {
    if (const auto n = retry_after_filler(ar, r, static_cast<std::int64_t>(r.pos) - 4, 5'000'000)) {
      count = n;
    }
  }
  if (*count > 5'000'000) {
    misparse(ar.cat, "ccskp.legacy.def_entities", {{"at", at_text(r.pos)}});
  }
  VDef def;
  def.ents = read_entity_list(ar, r, *count, false);
  std::uint32_t nrel = r.u32();
  if (nrel > 100000) {
    if (const auto n = retry_after_filler(ar, r, static_cast<std::int64_t>(r.pos) - 4, 100000)) {
      nrel = *n;
    }
  }
  if (nrel > 100000) {
    misparse(ar.cat, "ccskp.legacy.def_misaligned", {{"at", at_text(r.pos)}});
  }
  for (std::uint32_t i = 0; i < nrel; ++i) {
    ar.obj(r, Cls::CRelationship);
  }
  r.u16();
  // the 16-byte GUID is followed by the name record; SketchUp 2020 can put 2 more bytes before the GUID
  if (!str_at(ar.b, r.pos + 16)) {
    for (std::size_t s = 1; s <= 4; ++s) {
      if (str_at(ar.b, r.pos + s + 16)) {
        r.pos += s;
        break;
      }
    }
  }
  {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    for (const std::uint8_t c : r.take(16)) {
      def.guid += kDigits[c >> 4U];
      def.guid += kDigits[c & 15U];
    }
  }
  def.name = str(r, ar.cat);
  (void)str(r, ar.cat);
  (void)str(r, ar.cat);
  r.u32();
  // an undecoded block (~39–47 bytes), then this definition's CThumbnail
  std::int64_t tpos = -1;
  const auto ts = ar.class_slot.find(Cls::CThumbnail);
  for (std::int64_t off = 0; off < 96; ++off) {
    const std::int64_t p = static_cast<std::int64_t>(r.pos) + off;
    if ((u16at(ar.b, p) == 0xffff && u16at(ar.b, p + 4) == 10 && ascii_at(ar.b, p + 6, "CThumbnail")) ||
        (ts != ar.class_slot.end() && ar.is_class_ref(p, ts->second))) {
      tpos = p;
      break;
    }
  }
  if (tpos < 0) {
    misparse(ar.cat, "ccskp.legacy.def_end", {{"at", at_text(r.pos)}});
  }
  r.pos = static_cast<std::size_t>(tpos);
  ar.obj(r, Cls::CThumbnail);
  ar.defs.push_back(std::move(def));
  return push_index(ar.defs.size());
}

// NOLINTNEXTLINE(misc-no-recursion)
std::int32_t read_instance(Archive& ar, Reader& r) {
  preamble(ar, r);
  VInst in;
  in.db = drawbase(ar, r);
  const Obj d = ar.obj(r, Cls::CComponentDefinition);
  if (d.cls != Cls::CComponentDefinition) {
    misparse(ar.cat, "ccskp.legacy.inst_not_def", {{"at", at_text(r.pos)}});
  }
  in.def = d.slot;
  in.xf = f64s<13>(r);
  if (!ar.lay.v3 || str_at(ar.b, r.pos)) {
    in.name = str(r, ar.cat);  // SketchUp 3 instances have no name
  }
  if (ar.lay.inst_guid) {
    r.skip(16);
  }
  const Slot* self = ar.slot(ar.current_slot);
  in.cls = self != nullptr ? self->cls : Cls::CComponentInstance;
  ar.insts.push_back(std::move(in));
  return push_index(ar.insts.size());
}

ReadFn reader_for(Cls c) {
  switch (c) {
    case Cls::CVertex: return read_vertex;
    case Cls::CEdge: return read_edge;
    case Cls::CCurve: return read_curve;
    case Cls::CArcCurve: return read_arc_curve;
    case Cls::CEdgeUse: return read_edge_use;
    case Cls::CLoop: return read_loop;
    case Cls::CFace: return read_face;
    case Cls::CLayer: return read_layer;
    case Cls::CMaterial: return read_material;
    case Cls::CDib: return read_dib;
    case Cls::CAttributeContainer: return read_attr_container;
    case Cls::CAttributeNamed: return read_attr_named;
    case Cls::CCamera: return read_camera;
    case Cls::CThumbnail: return read_thumbnail;
    case Cls::CRelationship: return read_relationship;
    case Cls::CComponentDefinition: return read_definition;
    case Cls::CImage: return read_image;
    case Cls::CComponentInstance:
    case Cls::CGroup: return read_instance;
    case Cls::CFaceTextureCoords: return read_ftc;
    case Cls::CConstructionLine: return read_construction_line;
    case Cls::CConstructionPoint: return read_construction_point;
    case Cls::CSectionPlane: return read_section_plane;
    case Cls::CSkFont: return read_sk_font;
    case Cls::CDimensionLinear: return read_dim_linear;
    case Cls::CText: return read_text;
    default: return nullptr;
  }
}

// ------------------------------------------------------------------------------------------------ the walk
Obj Archive::obj(Reader& rd, Cls expect) {
  const std::int64_t tag = rd.u16();
  if (tag == 0) {
    return Obj{};
  }
  if (tag == 0x7fff) {
    const std::uint32_t big = rd.u32();
    return (big & 0x80000000U) != 0 ? of_class(rd, big & 0x7fffffffU, expect) : backref(big, rd);
  }
  if (tag == 0xffff) {
    rd.u16();  // schema
    const std::uint16_t len = rd.u16();
    if (len > 40) {
      misparse(cat, "ccskp.legacy.class_name", {{"at", at_text(rd.pos)}});
    }
    const std::string name = rd.ascii(len);
    const Cls c = cls_of(name);
    Slot e;
    e.is_class = true;
    e.cls = c;
    if (c == Cls::other) {
      other_names.push_back(name);
      e.name = static_cast<std::int32_t>(other_names.size() - 1);
    }
    const std::int64_t s = alloc(e);
    if (c != Cls::other) {
      class_slot[c] = s;
    }
    return new_obj(rd, c, name);
  }
  if ((tag & 0x8000) != 0) {
    return of_class(rd, tag & 0x7fff, expect);
  }
  return backref(tag, rd);
}

Obj Archive::of_class(Reader& rd, std::int64_t cslot, Cls expect) {
  const Slot* e = slots.get(cslot);
  Slot learned;
  if (e == nullptr) {
    // a class first defined in the settings region that was skipped: learn it from what is expected here
    if (expect == Cls::none) {
      misparse(cat, "ccskp.legacy.class_unknown", {{"at", at_text(rd.pos)}});
    }
    learned.is_class = true;
    learned.cls = expect;
    slots.set(cslot, learned);
    class_slot[expect] = cslot;
    e = &learned;
  }
  if (!e->is_class) {
    misparse(cat, "ccskp.legacy.ref_as_class", {{"at", at_text(rd.pos)}});
  }
  const Cls c = e->cls;
  const std::string name = c == Cls::other ? other_names[static_cast<std::size_t>(e->name)] : std::string(name_of(c));
  return new_obj(rd, c, name);
}

Obj Archive::new_obj(Reader& rd, Cls cls, std::string_view name) {
  const ReadFn read = reader_for(cls);
  if (read == nullptr) {
    misparse(cat, "ccskp.legacy.type_unknown", {{"type", std::string(name)}, {"at", at_text(rd.pos)}});
  }
  if (++depth > kMaxObjectDepth) {
    fail("too_big", msg(cat, "ccskp.too_big.object_depth", {{"levels", std::to_string(Limits::depth)}}));
  }
  Slot e;
  e.cls = cls;
  const std::int64_t s = alloc(e);
  const auto prev = current_slot;
  current_slot = s;
  std::int32_t v = -1;
  {
    const ScopeExit restore([this, prev] {
      current_slot = prev;
      --depth;
    });
    v = read(*this, rd);
  }
  if (Slot* done = slots.get_mut(s)) {
    done->idx = v;
  }
  if (cls == Cls::CComponentDefinition && on_definition) {
    on_definition(*this, s, v);
  }
  if (cls == Cls::CDimensionLinear || cls == Cls::CText) {
    annot_mark = next;
  }
  return Obj{s, cls, v};
}

// ------------------------------------------------------------------------------------------------ header pieces
std::optional<std::string> header_version(std::span<const std::uint8_t> b) {
  std::string s;
  for (std::size_t i = 0; i < std::min<std::size_t>(b.size(), 0x80); i += 2) {
    const unsigned c = static_cast<unsigned>(b[i]) | (i + 1 < b.size() ? static_cast<unsigned>(b[i + 1]) << 8U : 0U);
    s += c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
  }
  // /\{(\d+)\.([\d.]*)\}/
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '{') {
      continue;
    }
    std::size_t j = i + 1;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
      ++j;
    }
    if (j == i + 1 || j >= s.size() || s[j] != '.') {
      continue;
    }
    std::size_t k = j + 1;
    while (k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) {
      ++k;
    }
    if (k >= s.size() || s[k] != '}') {
      continue;
    }
    return s.substr(i + 1, j - i - 1) + "." + s.substr(j + 1, k - j - 1);
  }
  return std::nullopt;
}

struct VersionMap {
  std::map<std::string, std::uint32_t> schemas;
  std::size_t end = 0;
};
std::optional<VersionMap> read_version_map(std::span<const std::uint8_t> b, std::int64_t from,
                                           const i18n::Catalog* cat) {
  const auto pat = class_record("CVersionMap");
  std::int64_t at = from >= 0 ? find(b, pat, from, from + (1 << 16)) : -1;
  if (at < 0) {
    at = find(b, pat, 0, std::min<std::int64_t>(static_cast<std::int64_t>(b.size()), 1 << 16));
  }
  if (at < 0) {
    return std::nullopt;
  }
  Reader r(b, static_cast<std::size_t>(at) + 6 + 11, cat);
  VersionMap vm;
  for (int i = 0; i < 500; ++i) {
    std::string name = str(r, cat);
    const std::uint32_t v = r.u32();
    if (name == "End-Of-Version-Map") {
      vm.end = r.pos;
      return vm;
    }
    vm.schemas[std::move(name)] = v;
  }
  return std::nullopt;
}

std::optional<std::string> sniff_image(std::span<const std::uint8_t> d) {
  if (d.size() > 8 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4e && d[3] == 0x47) {
    return "image/png";
  }
  if (d.size() > 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) {
    return "image/jpeg";
  }
  if (d.size() > 2 && d[0] == 0x42 && d[1] == 0x4d) {
    return "image/bmp";
  }
  if (d.size() > 6 && ascii_at(d, 0, "GIF8")) {
    return "image/gif";
  }
  if (d.size() > 4 && ((d[0] == 0x49 && d[1] == 0x49) || (d[0] == 0x4d && d[1] == 0x4d))) {
    return "image/tiff";
  }
  if (d.size() > 4 && ascii_at(d, 0, "8BPS")) {
    return "image/vnd.adobe.photoshop";
  }
  if (d.size() > 40 && u32at(d, 0) == 40) {
    return "image/x-dib";
  }
  return std::nullopt;
}

/// A bare Windows DIB → a .bmp file.
Bytes dib_to_bmp(std::span<const std::uint8_t> d) {
  const std::int64_t bits = u16at(d, 14);
  const std::int64_t clr_used = u32at(d, 32);
  const std::int64_t comp = u32at(d, 16);
  // (clrUsed || (bits <= 8 ? 1 << bits : 0)) * 4 + (comp === 3 ? 12 : 0)
  std::uint64_t colours = 0;
  if (clr_used != 0) {
    colours = static_cast<std::uint64_t>(clr_used);
  } else if (bits <= 8) {
    colours = (std::uint64_t{1} << (static_cast<std::uint64_t>(bits) & 31U)) & 0xffffffffULL;  // JS shifts mod 32
  }
  const std::uint64_t palette = colours * 4 + (comp == 3 ? 12 : 0);
  Bytes out(14 + d.size());
  const auto put32 = [&out](std::size_t p, std::uint64_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
      out[p + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xffU);
    }
  };
  out[0] = 0x42;
  out[1] = 0x4d;
  put32(2, out.size());
  put32(10, 14 + 40 + palette);
  std::ranges::copy(d, out.begin() + 14);
  return out;
}

std::optional<Thumbnail> read_thumbnail_image(std::span<const std::uint8_t> b, std::size_t from,
                                              const i18n::Catalog* cat) {
  const auto pat = class_record("CDib");
  const std::int64_t at = find(b, pat, static_cast<std::int64_t>(from), static_cast<std::int64_t>(from) + 64);
  if (at < 0) {
    return std::nullopt;
  }
  try {
    Reader r(b, static_cast<std::size_t>(at) + 10, cat);
    r.u32();
    const auto data = r.take(r.u32());
    const auto mime = sniff_image(data);
    if (!mime) {
      return std::nullopt;
    }
    if (*mime == "image/x-dib") {
      return Thumbnail{"image/bmp", dib_to_bmp(data)};
    }
    return Thumbnail{*mime, Bytes(data.begin(), data.end())};
  } catch (const Fail&) {
    return std::nullopt;
  }
}

std::optional<std::string> read_units(std::span<const std::uint8_t> b, const i18n::Catalog* cat) {
  std::vector<int> key{0xff, 0xfe, 0xff, 12};
  for (const char c : std::string_view("UnitsOptions")) {
    key.push_back(c);
    key.push_back(0);
  }
  const std::int64_t at = find(b, key, 0);
  if (at < 0) {
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 5> kUnits = {"in", "ft", "mm", "cm", "m"};
  try {
    Reader r(b, static_cast<std::size_t>(at), cat);
    (void)str(r, cat);
    for (int i = 0; i < 24 && str_at(b, r.pos); ++i) {
      const std::string k = str(r, cat);
      const Typed v = typed_value(r, 0, cat);
      if (k == "LengthUnit") {
        // ['in', 'ft', 'mm', 'cm', 'm'][v] ?? null: the property named String(v)
        std::string idx;
        if (v.kind == Typed::Kind::number) {
          idx = num_text(v.num);
        }
        if (v.kind == Typed::Kind::string || v.kind == Typed::Kind::array) {
          idx = v.text;
        }
        if (idx.size() == 1 && idx[0] >= '0' && idx[0] <= '4') {
          return std::string(kUnits[static_cast<std::size_t>(idx[0] - '0')]);
        }
        return std::nullopt;
      }
    }
  } catch (const Fail&) {  // an options block not understood: no unit
    return std::nullopt;
  } catch (const Misparse&) {
    return std::nullopt;
  }
  return std::nullopt;
}

struct Start {
  std::size_t start = 0;
  std::uint32_t mat_count = 0;
};
Start model_start(std::span<const std::uint8_t> b, const Layout& lay, std::size_t from, const i18n::Catalog* cat) {
  const std::int64_t mat = find(b, class_record("CMaterial"), static_cast<std::int64_t>(from));
  if (mat >= 0) {
    const std::int64_t n = u32at(b, mat - 4);
    if (n > 100000) {
      misparse(cat, "ccskp.legacy.material_count");
    }
    return {static_cast<std::size_t>(mat), static_cast<std::uint32_t>(std::max<std::int64_t>(n, 0))};
  }
  const std::int64_t layer = find(b, class_record("CLayer"), static_cast<std::int64_t>(from));
  if (layer < 0) {
    misparse(cat, "ccskp.legacy.no_lists");
  }
  const std::int64_t start = layer - (lay.pid ? 9 : 8);
  return {static_cast<std::size_t>(std::max<std::int64_t>(start, 0)), 0};
}

/// Candidate slot bases (legacy.js slotBases).
std::vector<std::int64_t> slot_bases(std::span<const std::uint8_t> b, const Layout& lay, const Start& st,
                                     const i18n::Catalog* cat) {
  Archive probe(b, lay, cat);
  constexpr std::int64_t kB0 = 1 << 20;
  probe.slots.reset(kB0);
  probe.next = probe.base = kB0;
  probe.r.pos = st.start;
  if (st.mat_count >= 2) {
    probe.obj(probe.r, Cls::CMaterial);
    const std::int64_t t = u16at(b, static_cast<std::int64_t>(probe.r.pos));
    if (t == 0xffff || (t & 0x8000) == 0) {
      misparse(cat, "ccskp.legacy.numbering");
    }
    return {t & 0x7fff};
  }
  for (std::uint32_t i = 0; i < st.mat_count; ++i) {
    probe.obj(probe.r, Cls::CMaterial);
  }
  probe.r.u32();
  if (lay.pid) {
    probe.r.u8();
  }
  std::uint32_t n = 0;
  if (!lay.v3) {
    n = probe.r.u32();
    if (n < 1 || n > 100000) {
      misparse(cat, "ccskp.legacy.layer_count");
    }
  }
  std::vector<Obj> got;
  got.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    got.push_back(probe.obj(probe.r, Cls::CLayer));
  }
  probe.trailing_layers(got, true);
  const Obj last = probe.obj(probe.r);
  if (last.cls != Cls::premodel) {
    misparse(cat, "ccskp.legacy.layer_end");
  }
  std::vector<std::int64_t> out;
  for (const Obj& o : got) {
    if (!o.slot) {
      continue;  // a null: s - (null - B0) ≥ B0, filtered
    }
    const std::int64_t c = last.slot.value_or(0) - (o.slot.value() - kB0);
    if (c > 0 && c < kB0) {
      out.push_back(c);
    }
  }
  return out;
}

struct Walked {
  std::vector<std::pair<std::optional<std::int64_t>, std::int32_t>> materials;  // slot, value
  std::vector<std::pair<std::optional<std::int64_t>, std::int32_t>> layers;
  std::vector<Obj> root;
};

[[noreturn]] void wrong_value(std::string_view what) {
  throw Internal{std::string(what)};
}

// NOLINTNEXTLINE(misc-no-recursion)
Walked walk_model(Archive& ar, const Start& st) {
  Reader& r = ar.r;
  const i18n::Catalog* cat = ar.cat;
  r.pos = st.start;
  Walked w;
  for (std::uint32_t i = 0; i < st.mat_count; ++i) {
    const Obj o = ar.obj(r, Cls::CMaterial);
    if (!o.has_value() || o.cls != Cls::CMaterial) {
      wrong_value("a material list item that is not a material");
    }
    w.materials.emplace_back(o.slot, o.idx);
  }
  // layer list: <u32><u8 if pid><u32 count>; SketchUp 3 writes no count
  r.u32();
  if (ar.lay.pid) {
    r.u8();
  }
  const std::uint64_t declared = ar.lay.v3 ? std::numeric_limits<std::uint64_t>::max() : r.u32();
  if (declared > 100000 && !ar.lay.v3) {
    misparse(cat, "ccskp.legacy.layer_count");
  }
  std::vector<Obj> layers;
  while (layers.size() < declared) {
    const std::int64_t t = peek16(r);
    if (t == 0) {
      r.pos += 2;
      continue;
    }
    if (t != 0xffff && (t & 0x8000) == 0) {
      break;  // a back-ref: the definition-list anchor
    }
    const Obj o = ar.obj(r, Cls::CLayer);
    if (o.has_value()) {
      layers.push_back(o);
    }
  }
  ar.trailing_layers(layers);
  for (const Obj& o : layers) {
    if (o.cls != Cls::CLayer) {
      wrong_value("a layer list item that is not a layer");
    }
    w.layers.emplace_back(o.slot, o.idx);
  }
  // definition list: a reference to the ACTIVE layer, then the count
  if (ar.obj(r).cls != Cls::CLayer) {
    misparse(cat, "ccskp.legacy.def_list");
  }
  std::uint32_t nd = r.u32();
  if (nd > 1'000'000) {
    if (const auto n = retry_after_filler(ar, r, static_cast<std::int64_t>(r.pos) - 4, 1'000'000)) {
      nd = *n;
    }
  }
  if (nd > 1'000'000) {
    misparse(cat, "ccskp.legacy.def_count");
  }
  for (std::uint32_t i = 0; i < nd; ++i) {
    ar.obj(r, Cls::CComponentDefinition);
  }
  // definitions written back to back past the count
  for (;;) {
    const auto ds = ar.class_slot.find(Cls::CComponentDefinition);
    const std::int64_t t = u16at(ar.b, static_cast<std::int64_t>(r.pos));
    const bool is_def = (ds != ar.class_slot.end() && t == (0x8000 | ds->second)) ||
                        (t == 0xffff && ascii_at(ar.b, static_cast<std::int64_t>(r.pos) + 6, "CComponentDefinition"));
    if (!is_def) {
      break;
    }
    ar.obj(r);
  }
  // the model's own entity list
  std::uint32_t nr = r.u32();
  if (nr > 5'000'000) {
    if (const auto n = retry_after_filler(ar, r, static_cast<std::int64_t>(r.pos) - 4, 5'000'000)) {
      nr = *n;
    }
  }
  if (nr > 5'000'000) {
    misparse(cat, "ccskp.legacy.entity_count");
  }
  w.root = read_entity_list(ar, r, nr, true);
  return w;
}

// ------------------------------------------------------------------------------------------------ the model
/// The vertex at slot s (null when not a vertex, or released).
const P3* point(const Archive& ar, std::optional<std::int64_t> s) {
  const Slot* e = ar.slot(s);
  if (e == nullptr || e->released || e->idx < 0 || e->cls != Cls::CVertex) {
    return nullptr;
  }
  return &ar.verts[static_cast<std::size_t>(e->idx)];
}
/// The value of slot s when it is an edge.
const VEdge* edge_at(const Archive& ar, std::optional<std::int64_t> s) {
  const Slot* e = ar.slot(s);
  if (e == nullptr || e->released || e->idx < 0 || e->cls != Cls::CEdge) {
    return nullptr;
  }
  return &ar.edges[static_cast<std::size_t>(e->idx)];
}

Built build_entities(const Archive& ar, const std::vector<Obj>& ents, std::uint64_t& bad_faces) {
  Built g;
  std::vector<std::pair<std::int64_t, const VEdge*>> edges;  // insertion order
  std::unordered_map<std::int64_t, std::size_t> edge_at_index;
  std::unordered_set<std::int64_t> used;
  const auto add_edge = [&](std::int64_t s, const VEdge* e) {
    if (edge_at_index.try_emplace(s, edges.size()).second) {
      edges.emplace_back(s, e);
    }
  };
  for (const Obj& o : ents) {
    if (!o.has_value()) {
      continue;
    }
    if (o.cls == Cls::CEdge) {
      add_edge(*o.slot, &ar.edges[static_cast<std::size_t>(o.idx)]);
    } else if (o.cls == Cls::CComponentInstance || o.cls == Cls::CGroup || o.cls == Cls::CImage) {
      g.insts.push_back(o.idx);
    } else if (o.cls == Cls::CFace) {
      const VFace& v = ar.faces[static_cast<std::size_t>(o.idx)];
      Built::Face f;
      for (const Ref& lp : v.loops) {
        if (lp.idx < 0) {
          continue;
        }
        if (lp.cls != Cls::CLoop) {
          wrong_value("lp.uses is not iterable");
        }
        std::vector<P3> pts;
        std::vector<std::int64_t> pt_slots;
        std::optional<std::int64_t> prev;
        for (const Ref& u : ar.loops[static_cast<std::size_t>(lp.idx)].uses) {
          if (u.idx < 0 || u.cls != Cls::CEdgeUse) {
            continue;  // u.edge undefined: no edge
          }
          const VUse& use = ar.uses[static_cast<std::size_t>(u.idx)];
          const VEdge* e = edge_at(ar, use.edge);
          if (e == nullptr) {
            continue;
          }
          add_edge(*use.edge, e);
          used.insert(*use.edge);
          const std::optional<std::int64_t> vs = use.sense != 0 ? e->v2 : e->v1;
          if (vs != prev) {
            if (const P3* p = point(ar, vs)) {
              pts.push_back(*p);
              pt_slots.push_back(*vs);
              prev = vs;
            }
          }
        }
        if (pts.size() > 1 && pt_slots.front() == pt_slots.back()) {
          pts.pop_back();
        }
        if (pts.size() >= 3) {
          f.loops.push_back(std::move(pts));
        }
      }
      if (f.loops.empty()) {
        ++bad_faces;
        continue;
      }
      f.plane = v.plane;
      f.front = v.db.mat;
      f.back = v.back;
      f.layer = v.db.layer;
      f.hidden = v.db.hidden;
      f.ftc = v.ftc;
      g.faces.push_back(std::move(f));
    }
  }
  for (const auto& [s, e] : edges) {
    const P3* a = point(ar, e->v1);
    const P3* b = point(ar, e->v2);
    if (a == nullptr || b == nullptr) {
      continue;
    }
    if (!used.contains(s)) {
      g.edges.push_back({*a, *b, e->db.layer, e->db.soft, e->db.smooth, e->db.hidden});
    } else if (e->db.soft || e->db.smooth) {
      g.soft.push_back({*a, *b});
    }
  }
  return g;
}

/// SketchUp's 13 doubles → column-major 4×4 (the 3×3 part row by row, the translation, w as the divisor).
std::optional<std::array<double, 16>> matrix_of(const std::array<double, 13>& xf) {
  const double w = ok_num(xf[12]) && std::fabs(xf[12]) > 1e-12 ? xf[12] : 1;
  std::array<double, 16> m = {xf[0], xf[3], xf[6], 0, xf[1], xf[4],  xf[7],  0,
                              xf[2], xf[5], xf[8], 0, xf[9], xf[10], xf[11], w};
  if (!std::ranges::all_of(m, ok_num)) {
    return std::nullopt;
  }
  if (w != 1) {
    for (std::size_t i = 0; i < 15; ++i) {
      m[i] /= w;
    }
    m[15] = 1;
  }
  return m;
}

std::vector<std::vector<P2>> face_uv(const std::vector<std::vector<P3>>& loops, const P3& n, const VFtc* ftc,
                                     double tile_w, double tile_h) {
  const auto [nx, ny, nz] = n;
  P3 xr;
  P3 yr;
  const double cl = base::js_hypot(nx, ny);
  if (cl < 1e-9) {
    xr = {1, 0, 0};
    yr = {0, nz >= 0 ? 1.0 : -1.0, 0};
  } else {
    xr = {-ny / cl, nx / cl, 0};
    yr = {-nz * xr[1], nz * xr[0], nx * xr[1] - ny * xr[0]};
  }
  std::optional<std::array<double, 9>> inv;
  if (ftc != nullptr) {
    const auto& [a, b, c, d, e, f, g, h, i] = ftc->front;
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::isfinite(det) && std::fabs(det) > 1e-12) {
      const double k = 1 / det;
      inv = std::array<double, 9>{(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
                                  (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
                                  (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
    }
  }
  const double tw = ok_num(tile_w) && tile_w > 1e-9 ? tile_w : 1;
  const double th = ok_num(tile_h) && tile_h > 1e-9 ? tile_h : 1;
  std::vector<std::vector<P2>> out;
  out.reserve(loops.size());
  for (const auto& pts : loops) {
    std::vector<P2> l;
    l.reserve(pts.size());
    for (const P3& p : pts) {
      const double px = p[0] * xr[0] + p[1] * xr[1] + p[2] * xr[2];
      const double py = p[0] * yr[0] + p[1] * yr[1] + p[2] * yr[2];
      if (!inv) {
        l.push_back({px / tw, py / th});
        continue;
      }
      const auto& v = *inv;
      double q = px * v[2] + py * v[5] + v[8];
      if (std::fabs(q) < 1e-12) {
        q = 1;
      }
      l.push_back({(px * v[0] + py * v[3] + v[6]) / q / tw, (px * v[1] + py * v[4] + v[7]) / q / th});
    }
    out.push_back(std::move(l));
  }
  return out;
}

P3 newell(const std::vector<P3>& pts) {
  double x = 0;
  double y = 0;
  double z = 0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const P3& a = pts[i];
    const P3& b = pts[(i + 1) % pts.size()];
    x += (a[1] - b[1]) * (a[2] + b[2]);
    y += (a[2] - b[2]) * (a[0] + b[0]);
    z += (a[0] - b[0]) * (a[1] + b[1]);
  }
  return {x, y, z};
}

/// Math.min(1, Math.max(0, x)) with JS's NaN.
double clamp01(double x) {
  if (std::isnan(x)) {
    return x;
  }
  return std::min(1.0, std::max(0.0, x));
}

bool web_image(const std::optional<std::string>& mime) {
  return mime && (*mime == "image/png" || *mime == "image/jpeg" || *mime == "image/gif" || *mime == "image/bmp");
}

/// /^\d+(\.\d+)*$/
bool plain_version(std::string_view v) {
  if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0]))) {
    return false;
  }
  bool after_dot = false;
  for (const char c : v) {
    if (c == '.') {
      if (after_dot) {
        return false;
      }
      after_dot = true;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      after_dot = false;
    } else {
      return false;
    }
  }
  return !after_dot;
}

Model read_classic(std::span<const std::uint8_t> b, const Sniff& header, const ReadOptions& opts) {
  const i18n::Catalog* cat = opts.catalog;
  const auto step = [&opts](double f) {
    if (opts.cancelled && opts.cancelled()) {
      fail("aborted", "aborted");
    }
    if (opts.on_progress) {
      opts.on_progress(Progress{"legacy", static_cast<std::uint64_t>(std::round(f * 100)), 100});
    }
  };
  step(0);
  const std::optional<std::string> version =
      plain_version(header.version) ? std::optional(header.version) : header_version(b);
  const double major = header.major;  // read.cpp's parseInt of the header's digits
  if (!std::isfinite(major)) {
    fail("not_skp", msg(cat, "ccskp.legacy.no_version"));
  }
  if (major < 3) {
    fail("unsupported", msg(cat, "ccskp.legacy.too_old", {{"major", num_text(major)}}));
  }
  if (major > 20) {
    fail("unsupported", msg(cat, "ccskp.legacy.unknown_major", {{"major", num_text(major)}}));
  }

  const auto vmap = read_version_map(b, static_cast<std::int64_t>(header.body_start), cat);
  if (!vmap) {
    damaged(cat, "ccskp.damaged.no_version_map");
  }
  const Layout lay = layout_for(major, vmap->schemas);
  std::optional<Thumbnail> thumbnail = read_thumbnail_image(b, vmap->end, cat);
  const Start st = model_start(b, lay, vmap->end, cat);

  // Build each definition as soon as its record completes, then release its entity objects.
  std::map<std::int64_t, Built> built;
  std::vector<std::int64_t> built_order;
  std::uint64_t bad_faces = 0;
  const auto on_definition = [&built, &built_order, &bad_faces](Archive& ar, std::int64_t slot, std::int32_t v) {
    VDef& def = ar.defs[static_cast<std::size_t>(v)];
    Built g = build_entities(ar, def.ents, bad_faces);
    g.name = def.name;
    g.guid = def.guid;
    built.insert_or_assign(slot, std::move(g));
    built_order.push_back(slot);
    def.ents.clear();
    def.ents.shrink_to_fit();
    ar.slots.each_dense(slot + 1, ar.next, [](Slot& e) {
      if (!e.is_class && e.idx >= 0 &&
          (e.cls == Cls::CVertex || e.cls == Cls::CEdge || e.cls == Cls::CEdgeUse || e.cls == Cls::CLoop ||
           e.cls == Cls::CFace)) {
        e.released = true;
      }
    });
  };

  std::optional<Archive> walked_ar;
  std::optional<Walked> walked;
  std::optional<std::string> last;
  for (const std::int64_t base : slot_bases(b, lay, st, cat)) {
    built.clear();
    built_order.clear();
    bad_faces = 0;
    walked_ar.emplace(b, lay, cat);
    Archive& ar = *walked_ar;
    ar.slots.reset(base);
    ar.next = ar.base = base;
    ar.on_definition = on_definition;
    try {
      walked = walk_model(ar, st);
      break;
    } catch (const Misparse& e) {
      last = e.message;
    }
  }
  if (!walked || !walked_ar) {
    if (last) {
      throw Misparse{last.value()};
    }
    misparse(cat, "ccskp.legacy.no_numbering");
  }
  step(0.8);
  const Archive& ar = *walked_ar;
  const Built root_built = build_entities(ar, walked->root, bad_faces);

  Model m;
  m.vff = false;
  m.major = major;
  m.version = version ? *version : num_text(major);

  // materials (file order), keyed by store-map index
  std::unordered_map<std::int64_t, std::uint32_t> mat_index;
  std::uint64_t bad_textures = 0;
  for (std::size_t i = 0; i < walked->materials.size(); ++i) {
    const auto& [s, idx] = walked->materials[i];
    if (s) {
      mat_index.insert_or_assign(*s, static_cast<std::uint32_t>(i));
    }
    const VMaterial& v = ar.materials[static_cast<std::size_t>(idx)];
    Material out;
    out.name = v.name;
    out.color = Rgb{v.rgba[0], v.rgba[1], v.rgba[2]};
    out.alpha = v.use_opacity != 0 ? clamp01(1 - v.opacity) : 1;
    if (v.tex && !v.tex->image.empty()) {
      auto mime = sniff_image(v.tex->image);
      Bytes data(v.tex->image.begin(), v.tex->image.end());
      if (mime && *mime == "image/x-dib") {
        mime = "image/bmp";
        data = dib_to_bmp(v.tex->image);
      }
      if (!web_image(mime)) {
        ++bad_textures;
      }
      Texture t;
      const std::string& src = v.tex->file.empty() ? v.name : v.tex->file;
      const std::size_t cut = src.find_last_of("/\\");
      t.name = cut == std::string::npos ? src : src.substr(cut + 1);
      if (t.name.empty()) {
        t.name = v.name;
      }
      t.mime = mime ? *mime : "application/octet-stream";
      t.bytes = std::move(data);
      t.width = ok_num(v.tex->w) ? v.tex->w : 1;
      t.height = ok_num(v.tex->h) ? v.tex->h : 1;
      out.texture = std::move(t);
    }
    m.materials.push_back(std::move(out));
  }

  // layers: Layer0 first
  auto layer_list = walked->layers;
  const auto l0 = std::ranges::find_if(
      layer_list, [&ar](const auto& l) { return ar.layers[static_cast<std::size_t>(l.second)].name == "Layer0"; });
  const bool has_l0 = l0 != layer_list.end();
  if (has_l0 && l0 != layer_list.begin()) {
    const auto keep = *l0;
    layer_list.erase(l0);
    layer_list.insert(layer_list.begin(), keep);
  }
  for (const auto& [s, idx] : layer_list) {
    const VLayer& v = ar.layers[static_cast<std::size_t>(idx)];
    m.layers.push_back(Layer{v.name, v.visible, Rgb{v.rgba[0], v.rgba[1], v.rgba[2]}});
  }
  std::unordered_map<std::int64_t, std::uint32_t> layer_index;
  std::optional<std::uint32_t> null_layer;  // the key null of the JS Map: the Layer0 put in front
  if (!has_l0) {
    m.layers.insert(m.layers.begin(), Layer{"Layer0", true, std::nullopt});
    null_layer = 0;
  }
  for (std::size_t i = 0; i < layer_list.size(); ++i) {
    const auto at = static_cast<std::uint32_t>(i + (has_l0 ? 0 : 1));
    if (const std::optional<std::int64_t>& slot = layer_list[i].first; slot) {
      layer_index.insert_or_assign(*slot, at);
    } else {
      null_layer = at;
    }
  }
  const auto L = [&](const std::optional<std::int64_t>& s) -> std::uint32_t {
    if (!s) {
      return 0;
    }
    const auto it = layer_index.find(*s);
    return it == layer_index.end() ? 0 : it->second;
  };
  (void)null_layer;

  // definitions: the model first, then every definition in file order (slot order)
  std::vector<std::int64_t> def_slots;
  def_slots.reserve(built.size());
  for (const auto& [s, g] : built) {
    def_slots.push_back(s);
  }
  std::unordered_map<std::int64_t, std::uint32_t> def_index;
  for (std::size_t i = 0; i < def_slots.size(); ++i) {
    def_index[def_slots[i]] = static_cast<std::uint32_t>(i + 1);
  }
  std::unordered_map<std::int64_t, DefKind> kind_of;
  const auto note_kinds = [&](const Built& g) {
    for (const std::int32_t ii : g.insts) {
      const VInst& it = ar.insts[static_cast<std::size_t>(ii)];
      if (!it.def) {
        continue;  // a null def never names a definition slot
      }
      DefKind k = DefKind::component;
      if (it.cls == Cls::CGroup) {
        k = DefKind::group;
      } else if (it.cls == Cls::CImage) {
        k = DefKind::image;
      }
      if (!kind_of.contains(*it.def) || k != DefKind::component) {
        kind_of[*it.def] = k;
      }
    }
  };
  note_kinds(root_built);
  for (const std::int64_t s : built_order) {
    note_kinds(built.at(s));
  }

  struct {
    std::uint64_t unknown_mat = 0, bad_inst = 0, projected = 0, textured = 0, flipped = 0;
  } counts;
  const auto M = [&](std::uint32_t s) -> std::optional<std::uint32_t> {
    if (s == 0) {
      return std::nullopt;
    }
    const auto it = mat_index.find(s);
    if (it == mat_index.end()) {
      ++counts.unknown_mat;
      return std::nullopt;
    }
    return it->second;
  };
  const auto to_def = [&](const Built& g, std::string name, DefKind kind, std::optional<std::string> guid) {
    Definition d;
    d.name = std::move(name);
    d.kind = kind;
    d.guid = std::move(guid);
    for (const Built::Face& f : g.faces) {
      const P3 n{f.plane[0], f.plane[1], f.plane[2]};
      const P3 nn = newell(f.loops[0]);
      if (nn[0] * n[0] + nn[1] * n[1] + nn[2] * n[2] < 0) {
        ++counts.flipped;
      }
      Face out;
      out.front = M(f.front);
      const Texture* tex = nullptr;
      if (out.front && m.materials[*out.front].texture) {
        tex = &*m.materials[*out.front].texture;
      }
      if (tex != nullptr) {
        ++counts.textured;
        const VFtc* ftc = f.ftc >= 0 ? &ar.ftcs[static_cast<std::size_t>(f.ftc)] : nullptr;
        if (ftc != nullptr && ftc->projected) {
          ++counts.projected;
        }
        out.uv = face_uv(f.loops, n, ftc, tex->width, tex->height);
      }
      out.loops = f.loops;
      out.back = M(f.back);
      out.layer = L(f.layer);
      out.hidden = f.hidden;
      d.faces.push_back(std::move(out));
    }
    for (const Built::LooseEdge& e : g.edges) {
      d.edges.push_back(Edge{e.a, e.b, L(e.layer), e.soft, e.smooth, e.hidden});
    }
    d.soft = g.soft;
    for (const std::int32_t ii : g.insts) {
      const VInst& it = ar.insts[static_cast<std::size_t>(ii)];
      const auto di = it.def ? def_index.find(*it.def) : def_index.end();
      const auto mx = matrix_of(it.xf);
      if (di == def_index.end() || !mx) {
        ++counts.bad_inst;
        continue;
      }
      Instance inst;
      inst.def = di->second;
      inst.m = *mx;
      inst.name = it.name;
      inst.layer = L(it.db.layer);
      inst.hidden = it.db.hidden;
      inst.material = M(it.db.mat);
      d.insts.push_back(std::move(inst));
    }
    return d;
  };
  m.defs.push_back(to_def(root_built, "Model", DefKind::model, std::nullopt));
  for (const std::int64_t s : def_slots) {
    const Built& g = built.at(s);
    const auto k = kind_of.find(s);
    m.defs.push_back(to_def(g, g.name, k == kind_of.end() ? DefKind::component : k->second,
                            g.guid.empty() ? std::nullopt : std::optional(g.guid)));
  }
  step(0.95);

  const auto& sk = ar.skipped;
  const auto note = [&](std::string_view key, std::uint64_t n) {
    if (n != 0) {
      m.notes.push_back(msg(cat, key, {{"n", std::to_string(n)}}));
    }
  };
  note("ccskp.note.text_labels", sk.text);
  note("ccskp.note.dimensions", sk.dims);
  note("ccskp.note.section_planes", sk.sections);
  note("ccskp.note.guides", sk.guides);
  note("ccskp.note.face_corners", bad_faces);
  note("ccskp.note.inst_broken", counts.bad_inst);
  note("ccskp.note.material_unknown", counts.unknown_mat);
  note("ccskp.note.texture_format", bad_textures);
  note("ccskp.note.texture_projected", counts.projected);
  note("ccskp.note.face_flipped", counts.flipped);
  note("ccskp.note.root_cut", ar.root_cut);
  if (lay.v3) {
    m.notes.push_back(msg(cat, "ccskp.note.v3"));
  }
  m.notes.push_back(msg(cat, "ccskp.note.not_read"));
  m.thumbnail = std::move(thumbnail);
  m.length_unit = read_units(b, cat);
  step(1);
  return m;
}

}  // namespace

Model read_legacy(std::span<const std::uint8_t> bytes, const Sniff& header, const ReadOptions& opts) {
  const i18n::Catalog* cat = opts.catalog;
  if (bytes.size() > Limits::file_bytes) {
    fail("too_big", msg(cat, "ccskp.too_big.file", {{"size", "1 GB"}}));
  }
  try {
    return read_classic(bytes, header, opts);
  } catch (const Misparse& e) {
    fail("damaged", msg(cat, "ccskp.legacy.failed", {{"reason", e.message}}));
  } catch (const Internal& e) {
    fail("damaged", msg(cat, "ccskp.legacy.failed_internal", {{"reason", e.message}}));
  }
}

}  // namespace cc::skp::detail
