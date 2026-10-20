// SPDX-License-Identifier: Apache-2.0
// dwg-save.js (CadCraft 0.5) in C++: one closure-holding function in the JS, one class here (Saver) whose members are
// the JS's closure variables, called in the JS's order — the order fixes the tables' order, the counts' key order and
// the handles made on the way, so the written bytes come out the same.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/geom.hpp"
#include "cc/cad/save_parts.hpp"
#include "cc/doc/gcons.hpp"
#include "cc/dwg/write.hpp"
#include "cc/m3d/drawing3d.hpp"
#include "cc/save/dwg.hpp"
#include "dxf_common.hpp"
#include "dyn_save.hpp"

namespace cc::save {

using base::Array;
using base::Value;

namespace {

constexpr double kPi = std::numbers::pi;

constexpr std::array<DwgVersion, 6> kVersions = {{{"2018", "AC1032"},
                                                  {"2013", "AC1027"},
                                                  {"2010", "AC1024"},
                                                  {"2007", "AC1021"},
                                                  {"2004", "AC1018"},
                                                  {"2000", "AC1015"}}};

// ---------------------------------------------------------------- JS helpers

Value get(const Value& o, std::string_view k) {
  return o.get(k);
}
double num(const Value& v) {
  return base::js_to_number(v);
}
bool finite(const Value& v) {
  return v.is_number() && std::isfinite(v.as_number());
}
bool nullish(const Value& v) {
  return v.is_undefined() || v.is_null();
}
Value or_(const Value& v, const Value& d) {
  return v.truthy() ? v : d;
}
Value nn(const Value& v, const Value& d) {
  return nullish(v) ? d : v;
}
std::string str(const Value& v) {
  return base::js_string(v);
}
bool is_str(const Value& v, std::string_view s) {
  return v.is_string() && v.as_string() == s;
}
/// /^standard$/i
bool is_standard(const Value& v) {
  return v.is_string() && base::js_to_lower(v.as_string()) == "standard";
}
/// P(x, y, z = 0) → { x, y, z }
Value P(const Value& x, const Value& y, const Value& z = Value(0)) {
  return base::make_object({{"x", x}, {"y", y}, {"z", z}});
}
Value P(double x, double y, double z = 0) {
  return base::pt3(x, y, z);
}
Value pair(double c, Value v) {
  return Value(Array{Value(c), std::move(v)});
}
/// { ...a, ...b } of two plain objects (b's keys after a's, an existing key keeps its place)
void spread_into(Value& a, const Value& b) {
  if (b.is_object() && !b.is_point()) {
    for (const base::Member* m : base::js_members(b.as_object())) {
      a.set(m->key, m->value);
    }
  }
}
std::size_t len(const Value& v) {
  return v.is_array() ? v.as_array().size() : 0;
}
/// s.replace(/\\/g, '\\\\').replace(/\{/g, '\\{').replace(/\}/g, '\\}') per line, joined with '\P'
std::string mtext_body(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '\n') {
      out += "\\P";
    } else if (c == '\\' || c == '{' || c == '}') {
      out += '\\';
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}
/// parseInt(s, 16) & 0xffffff of a '#rrggbb'
double rgb_of(const std::string& c) {
  double v = 0;
  bool any = false;
  for (std::size_t i = 1; i < c.size(); ++i) {
    const char ch = c[i];
    int d = -1;
    if (ch >= '0' && ch <= '9') {
      d = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      d = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      d = ch - 'A' + 10;
    }
    if (d < 0) {
      break;
    }
    v = v * 16 + d;
    any = true;
  }
  return any ? static_cast<double>(static_cast<std::uint32_t>(base::js_to_int32(v)) & 0xFFFFFFU) : 0;
}
std::string hex_up(double n) {
  return base::hex_upper(static_cast<std::uint64_t>(n));
}

// ---------------------------------------------------------------- the saver

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) — a saver lives for one save_dwg call
class Saver {
 public:
  Saver(const doc::Doc& d, const DwgSaveOptions& o, const i18n::Catalog& w) : doc_(d), opts_(o), words_(w) {}
  DwgSaveResult run();

 private:
  const doc::Doc& doc_;
  const DwgSaveOptions& opts_;
  const i18n::Catalog& words_;
  std::string vkey_, code_;
  Value counts_ = Value::object();
  std::vector<std::string> used_lt_;  // a JS Set: insertion order
  std::map<std::string, std::string> lt_handles_;
  Array blocks_;
  std::optional<detail::DynSave> ds_;
  Array dyn_records_, vp_overrides_, clip_objects_, pic_objects_;
  std::vector<std::pair<std::string, std::string>> pic_defs_;
  int dim_n_ = 0;
  std::vector<std::pair<std::string, Value>> text_styles_;  // upper-case name → { name, font, height, … }
  std::vector<std::pair<std::string, Value>> dim_styles_;   // upper-case name → { name, vars }

  [[nodiscard]] bool r2000() const { return vkey_ == "2000"; }
  void note(const std::string& k, double n = 1) {
    const Value have = counts_.get(k);
    counts_.set(k, (have.truthy() ? num(have) : 0) + n);
  }
  [[nodiscard]] double count(std::string_view k) const {
    const Value v = counts_.get(k);
    return v.truthy() ? num(v) : 0;
  }
  [[nodiscard]] const Value* blk(const Value& name) const { return doc_.blocks().get(str(name)); }

  Value color(const Value& c) {
    if (nullish(c)) {
      return base::make_object({{"index", 256}});
    }
    if (c.is_string()) {
      note("truecolor");
      return base::make_object({{"rgb", rgb_of(c.as_string())}, {"index", cad::nearest_aci(c.as_string())}});
    }
    return base::make_object({{"index", c}});
  }
  static Value lw100(const Value& w, double absent) {
    if (nullish(w)) {
      return absent;
    }
    if (num(w) < 0) {
      return w;
    }
    return base::js_round(num(w) * 100);
  }
  Value lt_of(const Value& n) {
    if (n.truthy()) {
      const std::string s = base::js_to_lower(str(n));
      if (s != "bylayer" && s != "byblock" && s != "continuous") {
        const std::string name = str(n);
        if (std::ranges::find(used_lt_, name) == used_lt_.end()) {
          used_lt_.push_back(name);
        }
      }
      return n;
    }
    return "ByLayer";
  }

  std::string pic_def(const Value& file, const Value& size) {
    std::string k = cad::picture_key(file);
    if (k.empty()) {
      k = "image";
    }
    for (const auto& [kk, h] : pic_defs_) {
      if (kk == k) {
        return h;
      }
    }
    const std::string h = "CC" + hex_up(0x7A000000 + static_cast<double>(pic_defs_.size()));
    pic_defs_.emplace_back(k, h);
    pic_objects_.push_back(base::make_object({{"handle", h},
                                              {"type", "IMAGEDEF"},
                                              {"fileName", file.truthy() ? str(file) : std::string("image")},
                                              {"size", size},
                                              {"loaded", true},
                                              {"classVersion", 0},
                                              {"resUnits", 0},
                                              {"pixelSize", base::pt2(1, 1)}}));
    return h;
  }

  Array conv(const Value& e);
  Array conv0(const Value& e);
  Array multileader(const Value& e, const Value& base);
  Value text_fields(const Value& e, const std::string& value);
  Value text(const Value& e, const Value& base);
  Value mask_fields(const Value& m);
  Array hatch(const Value& e, const Value& base);
  Value pattern_lines(const Value& e);
  Array dimension(const Value& e, const Value& base);
  Value viewport(const Value& e, const Value& base, double id = 0);
  Array insert(const Value& e, const Value& base, double z);

  std::string text_style(const Value& n);
  Value ds_vars(const Value& st);
  std::string dim_style_name(const Value& n);
  [[nodiscard]] Value pattern_of(const std::string& name) const;
  int depth_ = 0;  // conv ↔ dimension: a dimension block holding a dimension (32 levels at most)
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

// mergeCc(have, add) into { ...have, ...it }
Value merge_xdata(const Value& have, const Value& add) {
  Value out = add;
  if (have.truthy() && have.get("CADCRAFT").truthy() && add.get("CADCRAFT").truthy()) {
    const Value have_cc = have.get("CADCRAFT");
    const Value add_cc = add.get("CADCRAFT");
    Array cc = have_cc.as_array();
    for (const Value& x : add_cc.as_array()) {
      cc.push_back(x);
    }
    out.set("CADCRAFT", std::move(cc));
  }
  Value res = have.truthy() ? have : Value::object();
  spread_into(res, out);
  return res;
}

// NOLINTNEXTLINE(misc-no-recursion) — through dimension(); depth_ caps it
Array Saver::conv(const Value& e) {
  if (depth_ > 32) {
    return {};
  }
  ++depth_;
  Array out = conv0(e);
  --depth_;
  const Value x = cad::props_xdata(e, r2000());
  if (!out.empty() && !x.as_object().empty()) {
    out[0].set("xdata", merge_xdata(out[0].get("xdata"), x));
  }
  if (const Value mat = get(e, "mat"); mat.is_string() && !mat.as_string().empty()) {
    note("material");
  }
  return out;
}

// NOLINTBEGIN(misc-no-recursion) — conv0 → dimension → conv: depth_ caps it
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — conv0's switch, as the JS
Array Saver::conv0(const Value& e) {
  const Value etr = get(e, "tr");
  std::optional<double> tr;
  if (!r2000()) {
    tr = cad::tr_word(etr);
  }
  if (r2000() && !nullish(etr)) {
    note("tr2000");
  }
  Value base = base::make_object({{"layer", or_(get(e, "layer"), "0")}});
  base.set("color", color(get(e, "color")));
  base.set("linetype", lt_of(get(e, "lt")));
  base.set("lineweight", lw100(get(e, "lw"), -1));
  if (const Value lts = get(e, "lts"); num(lts) > 0 && !(lts == Value(1))) {
    base.set("ltscale", lts);
  }
  if (tr) {
    base.set("transparency", *tr);
  }
  if (const Value th = get(e, "th"); th.truthy()) {
    base.set("thickness", th);
  }
  const Value ez = get(e, "z");
  const double z = finite(ez) ? ez.as_number() : 0;
  const std::string type = str(get(e, "type"));
  const auto with = [&](std::initializer_list<base::Init> rest) {
    Value o = base;
    for (const base::Init& m : rest) {
      o.set(m.key, std::move(m.value));
    }
    return o;
  };
  if (type == "line") {
    const Value z1 = get(e, "z1");
    const Value z2 = get(e, "z2");
    return {with({{"type", "LINE"},
                  {"start", P(get(e, "x1"), get(e, "y1"), finite(z1) ? z1 : Value(0))},
                  {"end", P(get(e, "x2"), get(e, "y2"), finite(z2) ? z2 : Value(0))}})};
  }
  if (type == "circle") {
    return {with({{"type", "CIRCLE"}, {"center", P(get(e, "cx"), get(e, "cy"), z)}, {"radius", get(e, "r")}})};
  }
  if (type == "arc") {
    return {with({{"type", "ARC"},
                  {"center", P(get(e, "cx"), get(e, "cy"), z)},
                  {"radius", get(e, "r")},
                  {"startAngle", get(e, "a0")},
                  {"endAngle", get(e, "a1")}})};
  }
  if (type == "point") {
    return {with({{"type", "POINT"}, {"position", P(get(e, "x"), get(e, "y"), z)}})};
  }
  if (type == "xline" || type == "ray") {
    const double dx = num(get(e, "dx"));
    const double dy = num(get(e, "dy"));
    const double n = base::js_hypot(dx, dy);
    if (!(n > 0) || !finite(get(e, "x")) || !finite(get(e, "y"))) {
      return {};
    }
    return {with({{"type", type == "xline" ? "XLINE" : "RAY"},
                  {"start", P(get(e, "x"), get(e, "y"), z)},
                  {"direction", P(dx / n, dy / n)}})};
  }
  if (type == "poly") {
    const Value pts = get(e, "pts");
    bool each = false;
    if (pts.is_array()) {
      for (const Value& p : pts.as_array()) {
        each = each || finite(get(p, "sw")) || finite(get(p, "ew"));
      }
    }
    const Value w = get(e, "w");
    const double g = finite(w) && w.as_number() > 0 ? w.as_number() : 0;
    Array vertices;
    if (pts.is_array()) {
      for (const Value& p : pts.as_array()) {
        Value v = base::make_object({{"x", get(p, "x")}, {"y", get(p, "y")}, {"bulge", or_(get(p, "b"), 0)}});
        if (each) {
          const Value sw = get(p, "sw");
          const Value ew = get(p, "ew");
          v.set("startWidth", finite(sw) ? sw : Value(g));
          v.set("endWidth", finite(ew) ? ew : Value(g));
        }
        vertices.push_back(std::move(v));
      }
    }
    Value o = with({{"type", "LWPOLYLINE"}, {"vertices", std::move(vertices)}, {"closed", get(e, "closed").truthy()}});
    if (g != 0 && !each) {
      o.set("constantWidth", g);
    }
    if (z != 0) {
      o.set("elevation", z);
    }
    return {o};
  }
  if (type == "ellipse") {
    return {with({{"type", "ELLIPSE"},
                  {"center", P(get(e, "cx"), get(e, "cy"), z)},
                  {"majorAxis", P(get(e, "mx"), get(e, "my"))},
                  {"ratio", get(e, "ratio")},
                  {"startParam", get(e, "t0")},
                  {"endParam", get(e, "t1")}})};
  }
  if (type == "text") {
    return {text(e, base)};
  }
  if (type == "hatch") {
    return get(e, "wipe").truthy() ? Array{} : hatch(e, base);
  }
  if (type == "insert") {
    return insert(e, base, z);
  }
  if (type == "dim") {
    return dimension(e, base);
  }
  if (type == "viewport") {
    return {viewport(e, base)};
  }
  if (type == "mleader") {
    if (opts_.explode_leaders) {
      note("mleader");
      // explodeEntity(e, doc.blocks): leaderGeometry(e), each part on the leader's layer and in its colour
      Array out;
      for (Value p : cad::leader_geometry(e)) {
        p.set("layer", get(e, "layer"));
        p.set("color", nn(get(e, "color"), Value(nullptr)));
        for (Value& x : conv(p)) {
          out.push_back(std::move(x));
        }
      }
      return out;
    }
    return multileader(e, base);
  }
  note("other");
  return {};
}

// NOLINTEND(misc-no-recursion)
// NOLINTNEXTLINE(readability-function-cognitive-complexity) — the reference case of conv0, as the JS
Array Saver::insert(const Value& e, const Value& base, double z) {
  const Value* b = blk(get(e, "block"));
  const Value pic = !opts_.model_only && b != nullptr ? cad::picture_placement(e, *b) : Value(nullptr);
  if (pic.truthy()) {
    note("picture");
    Value o = base;
    o.set("type", "IMAGE");
    const Value ins = pic.get("insert");
    o.set("insert", P(ins.get("x"), ins.get("y"), z));
    o.set("u", P(pic.get("u").get("x"), pic.get("u").get("y")));
    o.set("v", P(pic.get("v").get("x"), pic.get("v").get("y")));
    o.set("size", pic.get("size"));
    o.set("def", pic_def(pic.get("file"), pic.get("size")));
    o.set("displayFlags", 7);
    o.set("brightness", 50);
    o.set("contrast", 50);
    o.set("fade", base::js_to_int32(num(pic.get("fade"))));
    if (const Value clip = pic.get("clip"); clip.truthy()) {
      o.set("clipping", true);
      o.set("clipType", 2);
      o.set("clip", clip);
    } else {
      o.set("clipping", false);
      o.set("clip", Array{});
    }
    return {o};
  }
  const Value attrs = get(e, "attrs");
  Array atts;
  if (b != nullptr && attrs.truthy() && attrs.is_object() && !attrs.is_point() && !attrs.as_object().empty()) {
    atts = cad::world_attribs(e, *b);
  }
  const Value dr = get(e, "dyn").truthy() && ds_ ? ds_->ref(e) : Value(nullptr);
  const Value cp = cad::clip_poly(e);
  Value clip_ref(nullptr);
  if (cp.truthy() && opts_.model_only) {
    clip_ref = base::make_object({{"xclip", base::make_object({{"pts", cp}})}});
  } else if (cp.truthy() && dr.truthy() && dr.get("extDict").truthy()) {
    note("clipDyn");
  } else if (cp.truthy()) {
    const auto fh = [&] { return "CC" + hex_up(0x7F000000 + static_cast<double>(clip_objects_.size())) + "X"; };
    const std::string ih = dr.truthy() && dr.get("handle").truthy() ? str(dr.get("handle")) : fh() + "I";
    const std::string xd = fh();
    clip_objects_.emplace_back(nullptr);
    const std::string d2 = fh();
    clip_objects_.emplace_back(nullptr);
    const std::string sf = fh();
    clip_objects_.emplace_back(nullptr);
    clip_objects_.resize(clip_objects_.size() - 3);
    const auto dict = [](const std::string& h, const std::string& owner, Value entries) {
      return base::make_object({{"handle", h},
                                {"owner", owner},
                                {"reactors", base::array_of(Value(owner))},
                                {"type", "DICTIONARY"},
                                {"entries", std::move(entries)},
                                {"hardOwner", 1},
                                {"cloning", 1}});
    };
    clip_objects_.push_back(dict(xd, ih, base::make_object({{"ACAD_FILTER", d2}})));
    clip_objects_.push_back(dict(d2, xd, base::make_object({{"SPATIAL", sf}})));
    Value f = cad::filter_of(cp);
    f.set("handle", sf);
    f.set("owner", d2);
    f.set("reactors", base::array_of(Value(d2)));
    clip_objects_.push_back(std::move(f));
    clip_ref = base::make_object({{"handle", ih}, {"extDict", xd}});
  }
  Value o = base;
  o.set("type", "INSERT");
  o.set("block", get(e, "block"));
  o.set("insert", P(get(e, "x"), get(e, "y"), z));
  const Value sz = get(e, "sz");
  o.set("scale", base::make_object({{"x", nn(get(e, "sx"), 1)},
                                    {"y", nn(get(e, "sy"), 1)},
                                    {"z", finite(sz) && sz.as_number() != 0 ? sz : Value(1)}}));
  o.set("rotation", or_(get(e, "rot"), 0));
  Array attribs;
  for (const Value& t : atts) {
    Value a = text_fields(t, cad::one_line(str(nn(get(t, "s"), ""))));
    a.set("layer", or_(get(t, "layer"), or_(get(e, "layer"), "0")));
    a.set("color", color(get(t, "color")));
    a.set("tag", get(t, "tag"));
    a.set("flags", get(t, "invisible").truthy() ? 1 : 0);
    attribs.push_back(std::move(a));
  }
  o.set("attribs", std::move(attribs));
  if (dr.truthy()) {
    o.set("handle", dr.get("handle"));
    if (const Value x = dr.get("extDict"); x.truthy()) {
      o.set("extDict", x);
    }
  }
  if (clip_ref.truthy()) {
    spread_into(o, clip_ref);
  }
  return {o};
}

Array Saver::multileader(const Value& e, const Value& base) {
  const Value pts = get(e, "pts");
  if (!pts.is_array() || pts.as_array().size() < 2) {
    note("other");
    return {};
  }
  const Value st = cad::leader_style(e);
  const double dir = get(e, "dir") == Value(-1) ? -1 : 1;
  const cad::Landing L = cad::landing(e);
  const double asz = num(st.get("asz"));
  const double h = num(get(e, "h")) > 0 ? num(get(e, "h")) : asz;
  Value txt;
  for (const Value& p : cad::leader_geometry(e)) {
    if (p.get("type") == Value("text")) {
      txt = p;
      break;
    }
  }
  const Value& last = pts.as_array().back();
  const Value ez = get(e, "z");
  const double mz = finite(ez) ? ez.as_number() : 0;
  const std::string label = mtext_body(str(nn(get(e, "s"), "")));
  note("mleaderOut");
  const Value dogleg = st.get("dogleg");
  Value ctx = base::make_object({{"scale", 1},
                                 {"textHeight", h},
                                 {"arrowSize", st.get("asz")},
                                 {"landingGap", st.get("gap")},
                                 {"leftAttach", 2},
                                 {"rightAttach", 2},
                                 {"contentBase", P(L.text.x, L.text.y)},
                                 {"hasText", txt.truthy()},
                                 {"hasBlock", false}});
  if (txt.truthy()) {
    const Value ww = get(e, "ww");
    ctx.set("text", base::make_object({{"label", label},
                                       {"location", P(txt.get("x"), txt.get("y"), mz)},
                                       {"direction", P(1, 0)},
                                       {"width", num(ww) > 0 ? ww : Value(0)},
                                       {"height", 0},
                                       {"lineSpacing", or_(get(e, "ls"), 1)},
                                       {"alignment", dir == 1 ? 1 : 3}}));
  }
  Array lpts;
  for (std::size_t i = 0; i + 1 < pts.as_array().size(); ++i) {
    const Value& q = pts.as_array()[i];
    lpts.push_back(P(get(q, "x"), get(q, "y"), mz));
  }
  ctx.set(
      "roots",
      base::array_of(base::make_object({{"connection", P(get(last, "x"), get(last, "y"), mz)},
                                        {"direction", P(dir, 0)},
                                        {"landingDistance", dogleg},
                                        {"lines", base::array_of(base::make_object({{"points", std::move(lpts)}}))}})));
  Value o = base;
  o.set("type", "MULTILEADER");
  o.set("leaderType", 1);
  o.set("contentType", txt.truthy() ? 2 : 0);
  o.set("landingEnabled", true);
  o.set("doglegEnabled", num(dogleg) > 0);
  o.set("landingDistance", dogleg);
  o.set("arrowSize", st.get("asz"));
  o.set("textLeftAttach", 2);
  o.set("textRightAttach", 2);
  o.set("scale", 1);
  o.set("context", std::move(ctx));
  return {o};
}

double align_h(const Value& ha) {
  if (is_str(ha, "center")) {
    return 1;
  }
  return is_str(ha, "right") ? 2 : 0;
}
double align_v(const Value& va) {
  if (is_str(va, "bottom")) {
    return 1;
  }
  if (is_str(va, "middle")) {
    return 2;
  }
  return is_str(va, "top") ? 3 : 0;
}

Value Saver::text_fields(const Value& e, const std::string& value) {
  const double ha = align_h(get(e, "ha"));
  const double va = align_v(get(e, "va"));
  const Value ez = get(e, "z");
  const double z = finite(ez) ? ez.as_number() : 0;
  Value o = base::make_object({{"insert", P(get(e, "x"), get(e, "y"), z)},
                               {"align", ha != 0 || va != 0 ? P(get(e, "x"), get(e, "y"), z) : Value(nullptr)},
                               {"height", get(e, "h")},
                               {"value", value},
                               {"rotation", or_(get(e, "rot"), 0)},
                               {"widthFactor", or_(get(e, "wf"), 1)},
                               {"hAlign", ha},
                               {"vAlign", va}});
  if (const Value s = get(e, "style"); s.truthy() && !is_standard(s)) {
    o.set("style", text_style(s));
  }
  if (const Value obl = get(e, "obl"); obl.truthy()) {
    o.set("oblique", obl);
  }
  if (z != 0) {
    o.set("elevation", z);
  }
  return o;
}

Value Saver::text(const Value& e, const Value& base) {
  const std::string s = str(nn(get(e, "s"), ""));
  const Value ww = get(e, "ww");
  if (!s.contains('\n') && !(num(ww) > 0) && !get(e, "bgm").truthy()) {
    Value o = base;
    o.set("type", "TEXT");
    spread_into(o, text_fields(e, s));
    return o;
  }
  if (const Value wf = get(e, "wf"); wf.truthy() && !(wf == Value(1))) {
    note("mtextwidth");
  }
  const cad::TextLayout lay = cad::text_layout(e);
  Value va = or_(get(e, "va"), "baseline");
  double ax = num(get(e, "x"));
  double ay = num(get(e, "y"));
  Value at_x = get(e, "x");
  Value at_y = get(e, "y");
  if (!(is_str(va, "top") || is_str(va, "middle") || is_str(va, "bottom"))) {
    const Value rv = get(e, "rot");
    const double r = rv.truthy() ? num(rv) : 0;
    const double c = cc::base::js_cos(r);
    const double sn = cc::base::js_sin(r);
    const double h = num(get(e, "h"));
    // mApply(mRotate(r), { x: 0, y: h }) with m = [c, s, -s, c, 0, 0]
    const double dx = c * 0 + -sn * h + 0;
    const double dy = sn * 0 + c * h + 0;
    ax += dx;
    ay += dy;
    at_x = ax;
    at_y = ay;
    va = "top";
  }
  std::string body = mtext_body(s);
  if (const Value obl = get(e, "obl"); obl.truthy()) {
    const double deg = num(obl) * 180 / kPi;
    body = "\\Q" + str(Value(base::js_number(base::js_to_fixed(deg, 6)))) + ";" + body;
  }
  const Value rv = get(e, "rot");
  const double r = rv.truthy() ? num(rv) : 0;
  const Value ez = get(e, "z");
  const double z = finite(ez) ? ez.as_number() : 0;
  double row = 0;
  if (is_str(va, "middle")) {
    row = 1;
  } else if (is_str(va, "bottom")) {
    row = 2;
  }
  Value o = base;
  o.set("type", "MTEXT");
  o.set("insert", P(at_x, at_y, z));
  o.set("xAxis", P(cc::base::js_cos(r), cc::base::js_sin(r)));
  o.set("rotation", r);
  o.set("height", get(e, "h"));
  o.set("width", num(ww) > 0 ? ww : Value(0));
  o.set("attach", row * 3 + align_h(get(e, "ha")) + 1);
  o.set("value", body);
  o.set("lineSpacing", or_(get(e, "ls"), 1));
  o.set("lineSpacingStyle", 1);
  o.set("extWidth", lay.box_w);
  o.set("extHeight", lay.box_h);
  if (const Value st = get(e, "style"); st.truthy() && !is_standard(st)) {
    o.set("style", text_style(st));
  }
  spread_into(o, mask_fields(get(e, "bgm")));
  return o;
}

Value Saver::mask_fields(const Value& m) {
  if (!m.truthy() || !m.is_object()) {
    return Value::object();
  }
  if (r2000() && !opts_.model_only) {
    note("mask2000");
    return Value::object();
  }
  const bool win = m.get("color") == Value("bg");
  const Value sc = m.get("scale");
  return base::make_object(
      {{"backgroundFlags", win ? 3 : 1},
       {"backgroundScale", finite(sc) && sc.as_number() >= 1 && sc.as_number() <= 5 ? sc : Value(1.5)},
       {"backgroundColor", win ? base::make_object({{"index", 7}}) : color(nn(m.get("color"), 7))},
       {"backgroundTransparency", 0}});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — hatch(), as the JS
Array Saver::hatch(const Value& e, const Value& base) {
  std::vector<std::vector<std::pair<Value, Value>>> rings;
  if (const Value loops = get(e, "loops"); loops.is_array()) {
    for (const Value& l : loops.as_array()) {
      std::vector<std::pair<Value, Value>> pts;
      const std::size_t n = len(l);
      for (std::size_t k = 0; k + 1 < n; k += 2) {
        pts.emplace_back(l.as_array()[k], l.as_array()[k + 1]);
      }
      // openRing: the repeated closing point dropped
      if (pts.size() > 1 && base::js_hypot(num(pts.front().first) - num(pts.back().first),
                                           num(pts.front().second) - num(pts.back().second)) < 1e-12) {
        pts.pop_back();
      }
      if (pts.size() >= 3) {
        rings.push_back(std::move(pts));
      }
    }
  }
  if (rings.empty()) {
    return {};
  }
  const Value ez = get(e, "z");
  const double z = finite(ez) ? ez.as_number() : 0;
  const bool solid = get(e, "solid").truthy();
  if (solid && rings.size() == 1 && rings[0].size() <= 4 && !get(e, "wipe").truthy()) {
    const auto& r = rings[0];
    const auto& a = r[0];
    const auto& b = r[1];
    const auto& c = r[2];
    const auto& d = r.size() > 3 ? r[3] : r[2];
    Value o = base;
    o.set("type", "SOLID");
    o.set("points",
          Array{P(a.first, a.second, z), P(b.first, b.second, z), P(d.first, d.second, z), P(c.first, c.second, z)});
    if (z != 0) {
      o.set("elevation", z);
    }
    return {o};
  }
  Array loops;
  for (std::size_t i = 0; i < rings.size(); ++i) {
    Array vertices;
    for (const auto& [x, y] : rings[i]) {
      vertices.push_back(base::make_object({{"x", x}, {"y", y}, {"bulge", 0}}));
    }
    loops.push_back(
        base::make_object({{"flags", i != 0 ? 0 : 1},
                           {"polyline", base::make_object({{"closed", true}, {"vertices", std::move(vertices)}})}}));
  }
  Value more = Value::object();
  if (z != 0) {
    more.set("elevation", z);
  }
  const Value island = get(e, "island");
  double style = 0;
  if (is_str(island, "outer")) {
    style = 1;
  } else if (is_str(island, "ignore")) {
    style = 2;
  }
  more.set("style", style);
  Value o = base;
  o.set("type", "HATCH");
  if (solid) {
    o.set("pattern", "SOLID");
    o.set("solid", true);
    o.set("loops", std::move(loops));
    spread_into(o, more);
    return {o};
  }
  const Value lines = pattern_lines(e);
  if (!lines.truthy()) {
    note("hatchUnknown");
  }
  const Value pat = get(e, "pattern");
  const std::string up = base::js_to_upper(pat.truthy() ? str(pat) : std::string("USER"));
  const std::string up0 = base::js_to_upper(pat.truthy() ? str(pat) : std::string());
  // .slice(0, 31) in UTF-16 units
  const auto u16 = base::to_utf16(up);
  std::string name;
  base::append_utf16(name, std::span<const std::uint16_t>(u16).first(std::min<std::size_t>(31, u16.size())));
  o.set("pattern", name);
  o.set("solid", false);
  o.set("patternType", cad::hatch_pattern(up0) != nullptr ? 1 : 2);
  o.set("angle", or_(get(e, "pangle"), 0));
  const Value ps = get(e, "pscale");
  o.set("scale", num(ps) > 0 ? ps : Value(1));
  o.set("loops", std::move(loops));
  o.set("lines", lines.truthy() ? lines : Value(Array{}));
  spread_into(o, more);
  return {o};
}

Value Saver::pattern_lines(const Value& e) {
  Array fams;
  const Value pl = get(e, "patlines");
  if (pl.is_array() && !pl.as_array().empty()) {
    fams = pl.as_array();
  } else {
    const Value pat = get(e, "pattern");
    const Value* def = cad::hatch_pattern(base::js_to_upper(pat.truthy() ? str(pat) : std::string()));
    if (def == nullptr) {
      return {nullptr};
    }
    const Value ps = get(e, "pscale");
    const double k = (num(ps) > 0 ? num(ps) : 1) / (cad::hatch_is_metric(e, doc_.meta) ? 1 : 25.4);
    const Value pa = get(e, "pangle");
    const double turn = pa.truthy() ? num(pa) : 0;
    const double c = cc::base::js_cos(turn);
    const double s = cc::base::js_sin(turn);
    const Value def_lines = def->get("lines");
    for (const Value& f : def_lines.as_array()) {
      const double fx = num(f.get("x"));
      const double fy = num(f.get("y"));
      Array dashes;
      const Value fd = f.get("dashes");
      for (const Value& d : fd.as_array()) {
        dashes.emplace_back(num(d) * k);
      }
      fams.push_back(base::make_object({{"angle", num(f.get("angle")) + turn * 180 / kPi},
                                        {"x", (fx * c - fy * s) * k},
                                        {"y", (fx * s + fy * c) * k},
                                        {"dx", num(f.get("dx")) * k},
                                        {"dy", num(f.get("dy")) * k},
                                        {"dashes", std::move(dashes)}}));
    }
  }
  const Value oxv = get(e, "ox");
  const Value oyv = get(e, "oy");
  const double ox = finite(oxv) ? oxv.as_number() : 0;
  const double oy = finite(oyv) ? oyv.as_number() : 0;
  Array out;
  for (const Value& f : fams) {
    const double a = num(f.get("angle")) * kPi / 180;
    const double ca = cc::base::js_cos(a);
    const double sa = cc::base::js_sin(a);
    const double dx = num(f.get("dx"));
    const double dy = num(f.get("dy"));
    const Value dv = f.get("dashes");
    out.push_back(
        base::make_object({{"angle", a},
                           {"base", base::make_object({{"x", num(f.get("x")) + ox}, {"y", num(f.get("y")) + oy}})},
                           {"offset", base::make_object({{"x", dx * ca - dy * sa}, {"y", dx * sa + dy * ca}})},
                           {"dashes", dv.is_array() ? dv : Value(Array{})}}));
  }
  return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,misc-no-recursion) — dimension(), as the JS (via conv)
Array Saver::dimension(const Value& e, const Value& base) {
  const Value bn = get(e, "block");
  const Value* own = bn.truthy() ? blk(bn) : nullptr;
  const std::string name = "*D" + std::to_string(++dim_n_);
  Array ents;
  Array parts;
  if (own == nullptr) {
    parts = cad::dim_geometry(e);
  } else if (const Value oe = own->get("entities"); oe.is_array()) {
    parts = oe.as_array();
  }
  for (const Value& p : parts) {
    for (Value& x : conv(p)) {
      ents.push_back(std::move(x));
    }
  }
  blocks_.push_back(base::make_object({{"name", name}, {"base", P(0, 0)}, {"entities", std::move(ents)}}));
  note("dim");
  const Value st = cad::dim_style(e);
  const Value kind = get(e, "kind");
  double dim_type = 0;
  if (is_str(kind, "aligned")) {
    dim_type = 1;
  } else if (is_str(kind, "diameter")) {
    dim_type = 3;
  } else if (is_str(kind, "radius")) {
    dim_type = 4;
  } else if (is_str(kind, "angular")) {
    dim_type = 5;
  }
  const Value tp = cad::dim_text_point(e);
  Value d = base;
  d.set("type", "DIMENSION");
  d.set("dimType", dim_type);
  d.set("block", name);
  d.set("style", dim_style_name(get(e, "dimstyle")));
  d.set("textMid", P(tp.get("x"), tp.get("y")));
  d.set("text", or_(get(e, "text"), ""));
  d.set("measurement", cad::dim_measure(e));
  d.set("attach", 5);
  if (const Value tr = get(e, "trot"); tr.truthy()) {
    d.set("textRotation", tr);
  }
  d.set("flags", static_cast<double>(static_cast<std::int32_t>(dim_type) | 32 | 128));
  const auto pt = [](const Value& p) { return std::pair{num(get(p, "x")), num(get(p, "y"))}; };
  if (is_str(kind, "linear") || is_str(kind, "aligned")) {
    const auto [p1x, p1y] = pt(get(e, "p1"));
    const auto [p2x, p2y] = pt(get(e, "p2"));
    const auto [lx, ly] = pt(get(e, "loc"));
    const Value rv = get(e, "rot");
    const double rot = rv.truthy() ? num(rv) : 0;
    const double r = is_str(kind, "aligned") ? base::js_atan2(p2y - p1y, p2x - p1x) : rot;
    const double ux = cc::base::js_cos(r);
    const double uy = cc::base::js_sin(r);
    const double d2 = (p2x - lx) * ux + (p2y - ly) * uy;
    d.set("defPoint", P(lx + ux * d2, ly + uy * d2));
    d.set("xline1", P(get(get(e, "p1"), "x"), get(get(e, "p1"), "y")));
    d.set("xline2", P(get(get(e, "p2"), "x"), get(get(e, "p2"), "y")));
    d.set("angle", is_str(kind, "linear") ? r : 0);
  } else if (is_str(kind, "radius") || is_str(kind, "diameter")) {
    const auto [cx, cy] = pt(get(e, "c"));
    const auto [p1x, p1y] = pt(get(e, "p1"));
    const auto [lx, ly] = pt(get(e, "loc"));
    const double r = base::js_hypot(p1x - cx, p1y - cy);
    const double dl = base::js_hypot(lx - cx, ly - cy);
    const double a = dl > 1e-12 ? base::js_atan2(ly - cy, lx - cx) : base::js_atan2(p1y - cy, p1x - cx);
    const Value near = P(cx + r * cc::base::js_cos(a), cy + r * cc::base::js_sin(a));
    const Value far = P(cx - r * cc::base::js_cos(a), cy - r * cc::base::js_sin(a));
    d.set("defPoint", is_str(kind, "radius") ? P(get(get(e, "c"), "x"), get(get(e, "c"), "y")) : far);
    d.set("center", near);
    d.set("leaderLen", 0);
  } else if (is_str(kind, "angular")) {
    d.set("defPoint", P(get(get(e, "loc"), "x"), get(get(e, "loc"), "y")));
    d.set("xline1", P(get(get(e, "p1"), "x"), get(get(e, "p1"), "y")));
    d.set("xline2", P(get(get(e, "p2"), "x"), get(get(e, "p2"), "y")));
    d.set("center", P(get(get(e, "c"), "x"), get(get(e, "c"), "y")));
  } else {
    note("other");
    Value o = base;
    o.set("type", "INSERT");
    o.set("block", name);
    o.set("insert", P(0, 0));
    return {o};
  }
  Value xd = base::make_object({{"ACAD", cad::dstyle_items(st, false)}});
  if (Array cc = cad::cc_dim_items(st, false); !cc.empty()) {
    xd.set("CADCRAFT", std::move(cc));
  }
  d.set("xdata", std::move(xd));
  return {d};
}

Value Saver::viewport(const Value& e, const Value& base, double id) {
  const std::uint32_t status =
      0x8060U | (get(e, "locked").truthy() ? 16384U : 0U) | (get(e, "on") == Value(false) ? 131072U : 0U);
  Array frz;
  if (const Value f = get(e, "frz"); f.is_array()) {
    for (const Value& n : f.as_array()) {
      if (n.is_string() && !n.as_string().empty()) {
        frz.push_back(n);
      }
    }
  }
  const Value lov = get(e, "lov");
  const bool has_lov =
      !opts_.model_only && lov.truthy() && lov.is_object() && !lov.is_point() && !lov.as_object().empty();
  std::string vh;
  if (has_lov) {
    vh = "CC" + hex_up(0x7C000000 + static_cast<double>(vp_overrides_.size()));
    vp_overrides_.push_back(base::make_object({{"h", vh}, {"lov", lov}}));
  }
  Value o = base;
  if (!vh.empty()) {
    o.set("handle", vh);
  }
  if (const Value tw = get(e, "tw"); finite(tw) && tw.as_number() != 0) {
    o.set("twist", tw);
  }
  o.set("type", "VIEWPORT");
  o.set("center", P(get(e, "x"), get(e, "y")));
  o.set("width", get(e, "w"));
  o.set("height", get(e, "h"));
  o.set("viewCenter", P(get(e, "cx"), get(e, "cy")));
  o.set("viewTarget", P(0, 0));
  const Value s = get(e, "s");
  o.set("viewHeight", num(get(e, "h")) / (num(s) > 0 ? num(s) : 1));
  o.set("id", id);
  o.set("status", static_cast<double>(status));
  if (!frz.empty()) {
    o.set("frozenLayers", std::move(frz));
  }
  return o;
}

std::string Saver::text_style(const Value& n) {
  const std::string k = base::js_to_upper(str(n));
  for (const auto& [kk, v] : text_styles_) {
    if (kk == k) {
      return str(v.get("name"));
    }
  }
  text_styles_.emplace_back(
      k, base::make_object({{"name", str(n)}, {"font", "txt"}, {"height", 0}, {"widthFactor", 1}, {"oblique", 0}}));
  return str(n);
}

Value Saver::ds_vars(const Value& st) {
  Value v = cad::vars_of(st);
  if (const Value tx = get(st, "txsty"); st.truthy() && tx.truthy() && !is_standard(tx)) {
    v.set("DIMTXSTY", text_style(tx));
  }
  return v;
}

std::string Saver::dim_style_name(const Value& n) {
  if (!n.truthy() || is_standard(n)) {
    return "Standard";
  }
  const std::string k = base::js_to_upper(str(n));
  for (const auto& [kk, v] : dim_styles_) {
    if (kk == k) {
      return str(v.get("name"));
    }
  }
  Value vars = ds_vars(doc_.current.get("dim"));
  dim_styles_.emplace_back(k, base::make_object({{"name", str(n)}, {"vars", std::move(vars)}}));
  return str(n);
}

Value Saver::pattern_of(const std::string& name) const {
  return detail::pattern_of(doc_, name);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) — saveDwg's body, in order
DwgSaveResult Saver::run() {
  vkey_ = dwg_version_key(opts_.version);
  if (vkey_.empty()) {
    vkey_ = "2000";
  }
  for (const DwgVersion& v : kVersions) {
    if (v.key == vkey_) {
      code_ = std::string(v.code);
    }
  }
  // layers
  Array layers;
  doc_.layers().each([&](const std::string&, const Value& l) {
    Value o = base::make_object({{"name", l.get("name")}});
    o.set("color", color(or_(l.get("color"), 7)));
    o.set("off", !l.get("on").truthy());
    o.set("frozen", l.get("frozen").truthy());
    o.set("locked", l.get("locked").truthy());
    o.set("plot", !(l.get("plot") == Value(false)));
    o.set("frozenInNewViewports", l.get("newVpFrozen").truthy());
    o.set("linetype", lt_of(or_(l.get("linetype"), "Continuous")));
    o.set("lineweight", lw100(l.get("lineweight"), -3));
    layers.push_back(std::move(o));
  });
  // blocks to write: every named one, and unnamed ones a reference uses
  std::set<std::string> insert_used;
  const auto use = [&](const Value& e) {
    if (get(e, "type") == Value("insert")) {
      insert_used.insert(str(get(e, "block")));
    }
  };
  doc_.each_entity(use);
  doc_.blocks().each([&](const std::string&, const Value& b) {
    if (const Value es = b.get("entities"); es.is_array()) {
      for (const Value& e : es.as_array()) {
        use(e);
      }
    }
  });
  if (!opts_.model_only) {
    ds_.emplace(doc_);
  }
  if (ds_ && ds_->dropped() != 0) {
    note("dynDropped", ds_->dropped());
  }
  // text and dimension styles
  doc_.text_styles.each([&](const std::string& n, const Value& v) {
    Value o = base::make_object({{"name", n},
                                 {"font", or_(v.get("font"), "txt")},
                                 {"height", or_(v.get("h"), 0)},
                                 {"widthFactor", or_(v.get("wf"), 1)},
                                 {"oblique", or_(v.get("obl"), 0)}});
    const std::string k = base::js_to_upper(n);
    for (auto& [kk, have] : text_styles_) {
      if (kk == k) {
        have = std::move(o);
        return;
      }
    }
    text_styles_.emplace_back(k, std::move(o));
  });
  doc_.dim_styles.each([&](const std::string& n, const Value& st) {
    Value vars = ds_vars(st);
    const std::string k = base::js_to_upper(n);
    bool have = false;
    for (auto& [kk, v] : dim_styles_) {
      if (kk == k) {
        v = base::make_object({{"name", n}, {"vars", vars}});
        have = true;
      }
    }
    if (!have) {
      dim_styles_.emplace_back(k, base::make_object({{"name", n}, {"vars", std::move(vars)}}));
    }
  });
  // blocks
  doc_.blocks().each([&](const std::string& bname, const Value& b) {
    const std::string name = str(b.get("name"));
    if (name.starts_with('*') && !insert_used.contains(name)) {
      return;
    }
    const Value bb = b.get("base");
    const Value bx = bb.truthy() ? bb.get("x") : Value();
    const Value by = bb.truthy() ? bb.get("y") : Value();
    const Value base_pt = P(or_(bx, 0), or_(by, 0));
    const Value ents_v = b.get("entities");
    const bool xroot = cad::is_xref_root(b);
    if (xroot && len(ents_v) == 0) {
      const Value xp = b.get("xref").get("path");
      blocks_.push_back(base::make_object({{"name", b.get("name")},
                                           {"base", base_pt},
                                           {"entities", Array{}},
                                           {"flags", 4},
                                           {"xrefPath", xp.truthy() ? xp : b.get("name")}}));
      note("xrefLink");
      return;
    }
    if (xroot) {
      note("xrefBound");
    }
    Array attdefs;
    if (const Value ad = b.get("attdefs"); ad.is_array()) {
      for (const Value& d : ad.as_array()) {
        const Value def = get(d, "def");
        Value o = text_fields(d, cad::one_line(def.truthy() ? str(def) : std::string()));
        o.set("type", "ATTDEF");
        o.set("layer", or_(get(d, "layer"), "0"));
        o.set("color", color(get(d, "color")));
        o.set("tag", get(d, "tag"));
        o.set("prompt", or_(get(d, "prompt"), ""));
        o.set("flags", (get(d, "invisible").truthy() ? 1 : 0) | (get(d, "constant").truthy() ? 2 : 0) |
                           (get(d, "preset").truthy() ? 8 : 0));
        attdefs.push_back(std::move(o));
      }
    }
    const Value* dyn = ds_ ? ds_->block(bname) : nullptr;
    if (dyn != nullptr) {
      const Value dynv = *dyn;
      const Value adl = dynv.get("ad");
      const Value hs = dynv.get("h");
      const Value shown = dynv.get("shown");
      Array ents;
      const Value allv = dynv.get("all");
      const Array& all = allv.as_array();
      for (std::size_t i = 0; i < all.size(); ++i) {
        Array out;
        // adAt: Map(ad[k] → attdefs[k]); a repeated index keeps the last k
        std::optional<std::size_t> ad_k;
        if (adl.is_array()) {
          for (std::size_t k = 0; k < adl.as_array().size(); ++k) {
            if (adl.as_array()[k] == Value(i)) {
              ad_k = k;
            }
          }
        }
        if (ad_k) {
          out.push_back(*ad_k < attdefs.size() ? attdefs[*ad_k] : Value());
        } else if (all[i].is_array()) {
          for (const Value& p : all[i].as_array()) {
            for (Value& x : conv(p)) {
              out.push_back(std::move(x));
            }
          }
        }
        if (!out.empty() && out[0].truthy()) {
          if (const Value h = hs.is_array() && i < hs.as_array().size() ? hs.as_array()[i] : Value(); h.truthy()) {
            out[0].set("handle", h);
          }
        }
        const bool is_shown = shown.is_array() && i < shown.as_array().size() && shown.as_array()[i].truthy();
        if (!is_shown) {
          for (Value& x : out) {
            if (x.is_object()) {
              x.set("invisible", true);
            }
          }
        }
        for (Value& x : out) {
          ents.push_back(std::move(x));
        }
      }
      const Value record = ds_->record(bname);
      blocks_.push_back(base::make_object(
          {{"name", b.get("name")}, {"record", record}, {"base", base_pt}, {"entities", std::move(ents)}}));
      Value r = base::make_object({{"handle", record}, {"name", b.get("name")}});
      if (const Value x = dynv.get("x"); x.truthy()) {
        r.set("extDict", x);
      }
      if (const Value xd = dynv.get("xd"); xd.truthy()) {
        r.set("xdata", xd);
      }
      dyn_records_.push_back(std::move(r));
      return;
    }
    Array ents;
    if (ents_v.is_array()) {
      for (const Value& e : ents_v.as_array()) {
        for (Value& x : conv(e)) {
          ents.push_back(std::move(x));
        }
      }
    }
    for (Value& a : attdefs) {
      ents.push_back(std::move(a));
    }
    blocks_.push_back(base::make_object({{"name", b.get("name")}, {"base", base_pt}, {"entities", std::move(ents)}}));
  });
  // constraints + parameters (gcons.js): CADCRAFT xdata "CCREF:<key>" on each constrained entity, block
  // CADCRAFT_PARAMETRIC
  const auto pf = doc::gcons::for_file(doc_);
  const auto strings = [](const std::vector<std::string>& strs) {
    Array a;
    for (const std::string& s : strs) {
      a.push_back(pair(1000, s));
    }
    return base::make_object({{"CADCRAFT", Value(std::move(a))}});
  };
  Array entities;
  doc_.model()->each([&](const doc::Id& id, const Value& e) {
    std::vector<Value> out = conv(e);
    if (pf && !out.empty()) {
      for (const auto& [rid, key] : pf->refs) {
        if (rid == static_cast<double>(id)) {
          // { ...(out[0].xdata || {}), ...strings(['CCREF:' + key]) }: the CADCRAFT strings are replaced
          const Value had = out.front().get("xdata");
          Value xd = had.is_object() && !had.is_point() ? had : Value::object();
          xd.set("CADCRAFT", strings({"CCREF:" + key}).get("CADCRAFT"));
          out.front().set("xdata", xd);
        }
      }
    }
    for (Value& x : out) {
      entities.push_back(std::move(x));
    }
  });
  if (pf) {
    Array pts;
    for (const std::string& s : pf->points) {
      pts.push_back(
          base::make_object({{"type", "POINT"}, {"layer", "0"}, {"position", P(0, 0)}, {"xdata", strings({s})}}));
    }
    blocks_.push_back(base::make_object(
        {{"name", std::string(doc::gcons::kParamBlock)}, {"base", P(0, 0)}, {"entities", std::move(pts)}}));
  }
  // the 3D model
  const Value units_v = doc_.meta.get("units");
  const std::string units = units_v.truthy() ? str(units_v) : std::string();
  const auto m3 = m3d::for_file3d(doc_.model3d, units);
  if (m3) {
    bool have = false;
    for (const Value& l : layers) {
      have = have || l.get("name") == Value(m3d::kM3dLayer);
    }
    if (!have) {
      layers.push_back(base::make_object({{"name", m3d::kM3dLayer},
                                          {"color", base::make_object({{"index", 8}})},
                                          {"off", false},
                                          {"frozen", false},
                                          {"locked", false},
                                          {"plot", true}}));
    }
    for (const auto& f : m3->faces) {
      Array pts;
      for (const auto& p : f.pts) {
        pts.push_back(P(p[0], p[1], p[2]));
      }
      entities.push_back(base::make_object(
          {{"type", "3DFACE"}, {"layer", m3d::kM3dLayer}, {"points", std::move(pts)}, {"invisibleEdges", f.invis}}));
    }
    Array pts;
    for (const std::string& s : m3->points) {
      pts.push_back(base::make_object({{"type", "POINT"},
                                       {"layer", "0"},
                                       {"position", P(0, 0)},
                                       {"xdata", base::make_object({{"CADCRAFT", base::array_of(pair(1000, s))}})}}));
    }
    blocks_.push_back(base::make_object({{"name", m3d::kM3dBlock}, {"base", P(0, 0)}, {"entities", std::move(pts)}}));
  }
  // sheets
  Array layouts = {base::make_object({{"name", "Model"}, {"tabOrder", 0}})};
  Array spaces;
  for (std::size_t i = 0; i < doc_.layouts().size(); ++i) {
    const doc::Layout& L = doc_.layouts()[i];
    const Value& p = L.paper;
    const bool inch = p.get("unit") == Value("in");
    const double mm = inch ? 25.4 : 1;
    const std::string record = "CC" + std::to_string(i + 1);
    const double x0 = num(p.get("x0"));
    const double y0 = num(p.get("y0"));
    const double w = num(p.get("w"));
    const double h = num(p.get("h"));
    Value sheet = viewport(base::make_object({{"x", x0 + w / 2},
                                              {"y", y0 + h / 2},
                                              {"w", p.get("w")},
                                              {"h", p.get("h")},
                                              {"cx", x0 + w / 2},
                                              {"cy", y0 + h / 2},
                                              {"s", 1}}),
                           base::make_object({{"layer", "0"}, {"color", color(Value(nullptr))}}), 1);
    const auto u16 = base::to_utf16(L.name);
    std::string lname;
    base::append_utf16(lname, std::span<const std::uint16_t>(u16).first(std::min<std::size_t>(255, u16.size())));
    const Value key = p.get("key");
    sheet.set("xdata",
              base::make_object(
                  {{"CADCRAFT", Array{pair(1000, "LAYOUT"), pair(1000, lname), pair(1000, p.get("unit")),
                                      pair(1000, key.truthy() ? key : Value("")), pair(1040, p.get("x0")),
                                      pair(1040, p.get("y0")), pair(1040, p.get("w")), pair(1040, p.get("h"))}}}));
    double vp_id = 1;
    Array ents = {sheet};
    L.entities->each([&](const doc::Id&, const Value& e) {
      if (get(e, "type") == Value("viewport")) {
        Value b = base::make_object({{"layer", or_(get(e, "layer"), "0")}});
        b.set("color", color(get(e, "color")));
        ents.push_back(viewport(e, b, ++vp_id));
      } else {
        for (Value& x : conv(e)) {
          ents.push_back(std::move(x));
        }
      }
    });
    spaces.push_back(
        base::make_object({{"name", i != 0 ? "*Paper_Space" + std::to_string(i - 1) : std::string("*Paper_Space")},
                           {"record", record},
                           {"base", P(0, 0)},
                           {"entities", std::move(ents)}}));
    const std::string pw = str(p.get("w"));
    const std::string ph = str(p.get("h"));
    const Value paper_name =
        key.truthy()
            ? key
            : Value(std::string("Custom_(").append(pw).append("_x_").append(ph).append(inch ? "_Inches)" : "_MM)"));
    layouts.push_back(base::make_object(
        {{"name", L.name},
         {"tabOrder", static_cast<double>(i + 1)},
         {"block", record},
         {"paper", base::make_object({{"name", paper_name},
                                      {"w", w * mm},
                                      {"h", h * mm},
                                      {"units", inch ? 0 : 1},
                                      {"rotation", 0},
                                      {"plotType", 5},
                                      {"scaleType", 16},
                                      {"scale", 1},
                                      {"left", 0},
                                      {"bottom", 0},
                                      {"right", 0},
                                      {"top", 0}})},
         {"limits", base::make_object({{"min", base::make_object({{"x", p.get("x0")}, {"y", p.get("y0")}})},
                                       {"max", base::make_object({{"x", x0 + w}, {"y", y0 + h}})}})},
         {"insbase", P(0, 0)}}));
  }
  // extents
  cad::Bounds ext;
  const cad::BlockLookup lookup = [&](const std::string& n) { return doc_.blocks().get(n); };
  doc_.model()->each([&](const doc::Id&, const Value& e) {
    const cad::Bounds b = cad::entity_bounds(e, lookup);
    if (b.valid()) {
      ext.min_x = std::min(ext.min_x, b.min_x);
      ext.min_y = std::min(ext.min_y, b.min_y);
      ext.max_x = std::max(ext.max_x, b.max_x);
      ext.max_y = std::max(ext.max_y, b.max_y);
    }
  });
  // header
  const Value& meta = doc_.meta;
  const bool imperial = units == "inches" || units == "feet" || units == "miles" || units == "microinches" ||
                        units == "mils" || units == "yards";
  Value header = base::make_object(
      {{"INSUNITS", cad::unit_code(units)}, {"MEASUREMENT", imperial ? 0 : 1}, {"LUNITS", or_(meta.get("lunits"), 2)}});
  if (const Value lp = meta.get("luprec"); finite(lp)) {
    header.set("LUPREC", lp);
  }
  if (const Value ib = meta.get("insbase"); ib.truthy()) {
    header.set("INSBASE", P(or_(ib.get("x"), 0), or_(ib.get("y"), 0)));
  }
  const auto axes = [](const Value& u) {
    const double ang = num(u.get("ang"));
    return std::array<Value, 3>{P(u.get("x"), u.get("y")), P(cc::base::js_cos(ang), cc::base::js_sin(ang)),
                                P(-cc::base::js_sin(ang), cc::base::js_cos(ang))};
  };
  if (doc_.ucs.truthy()) {
    const auto a = axes(doc_.ucs);
    header.set("UCSORG", a[0]);
    header.set("UCSXDIR", a[1]);
    header.set("UCSYDIR", a[2]);
  }
  Array ucs_table;
  if (doc_.ucs_named.is_array()) {
    for (const Value& u : doc_.ucs_named.as_array()) {
      const auto a = axes(u);
      ucs_table.push_back(
          base::make_object({{"name", u.get("name")}, {"origin", a[0]}, {"xAxis", a[1]}, {"yAxis", a[2]}}));
    }
  }
  if (ext.valid()) {
    header.set("EXTMIN", P(ext.min_x, ext.min_y));
    header.set("EXTMAX", P(ext.max_x, ext.max_y));
  }
  Array views;
  double paper_views = 0;
  if (doc_.views.is_array()) {
    for (const Value& v : doc_.views.as_array()) {
      const bool ps = v.get("layout").truthy();
      paper_views += ps ? 1 : 0;
      views.push_back(base::make_object({{"name", v.get("name")},
                                         {"center", base::make_object({{"x", v.get("cx")}, {"y", v.get("cy")}})},
                                         {"width", v.get("w")},
                                         {"height", v.get("h")},
                                         {"target", P(0, 0)},
                                         {"direction", P(0, 0, 1)},
                                         {"paperSpace", ps}}));
    }
  }
  if (paper_views > 0) {
    note("paperView", paper_views);
  }
  // per-viewport layer overrides: XRECORDs in each layer's extension dictionary
  Array ovr_objects;
  if (!vp_overrides_.empty()) {
    std::vector<std::pair<std::string, std::string>> lt_h;
    const auto lt_find = [&](const std::string& n) -> const std::string* {
      for (const auto& [k, v] : lt_h) {
        if (k == n) {
          return &v;
        }
      }
      return nullptr;
    };
    double k = 0;
    const auto h = [&] {
      const double v = k;
      k += 1;
      return "CC" + hex_up(0x7E000000 + v);
    };
    struct Kind {
      const char* kind;
      const char* tag;
    };
    constexpr std::array<Kind, 4> kKinds = {{{"COLOR", "ADSK_LYR_COLOR_OVERRIDE"},
                                             {"LINETYPE", "ADSK_LYR_LINETYPE_OVERRIDE"},
                                             {"LINEWT", "ADSK_LYR_LINEWT_OVERRIDE"},
                                             {"ALPHA", "ADSK_LYR_ALPHA_OVERRIDE"}}};
    const auto val = [&](std::size_t kind, const Value& o) -> Value {
      switch (kind) {
        case 0: {
          const Value c = o.get("color");
          if (nullish(c)) {
            return {nullptr};
          }
          if (c.is_string()) {
            return pair(420, base::js_to_int32(static_cast<double>(
                                 0xC2000000U | (static_cast<std::uint32_t>(rgb_of(c.as_string())) & 0xFFFFFFU))));
          }
          return pair(420, base::js_to_int32(static_cast<double>(
                               0xC3000000U | (static_cast<std::uint32_t>(base::js_to_int32(num(c))) & 0xFFU))));
        }
        case 1: {
          const Value lt = o.get("lt");
          if (!lt.truthy()) {
            return {nullptr};
          }
          const std::string n = str(lt_of(lt));
          if (const std::string* have = lt_find(n)) {
            return pair(343, *have);
          }
          const std::string ln = str(lt);
          if (const std::string* have2 = lt_find(ln)) {  // ltH.set(o.lt, …) keyed by o.lt
            return pair(343, *have2);
          }
          lt_h.emplace_back(ln, "CC" + hex_up(0x7D000000 + static_cast<double>(lt_h.size())));
          return pair(343, lt_h.back().second);
        }
        case 2: {
          const Value lw = o.get("lw");
          return finite(lw) && lw.as_number() >= 0 ? pair(91, base::js_round(lw.as_number() * 100)) : Value(nullptr);
        }
        default: {
          const Value tr = o.get("tr");
          if (!finite(tr)) {
            return {nullptr};
          }
          const auto w = cad::tr_word(tr);
          return pair(440, base::js_to_int32(w ? *w : 0));
        }
      }
    };
    for (Value& L : layers) {
      Value entries = Value::object();
      Array recs;
      const std::string up = base::js_to_upper(str(L.get("name")));
      for (std::size_t ki = 0; ki < kKinds.size(); ++ki) {
        Array data;
        for (const Value& vo : vp_overrides_) {
          const Value lov = vo.get("lov");
          Value o;
          for (const base::Member* m : base::js_members(lov.as_object())) {
            if (base::js_to_upper(m->key) == up) {
              o = m->value;
              break;
            }
          }
          const Value g = o.truthy() ? val(ki, o) : Value();
          if (g.truthy()) {
            data.push_back(pair(335, vo.get("h")));
            data.push_back(g);
          }
        }
        if (!data.empty()) {
          const std::string xh = h();
          entries.set(std::string("ADSK_XREC_LAYER_") + kKinds.at(ki).kind + "_OVR", xh);
          Array d = {pair(102, std::string("{") + kKinds.at(ki).tag)};
          for (Value& x : data) {
            d.push_back(std::move(x));
          }
          d.push_back(pair(102, "}"));
          recs.push_back(
              base::make_object({{"handle", xh}, {"type", "XRECORD"}, {"data", std::move(d)}, {"cloning", 1}}));
        }
      }
      if (recs.empty()) {
        continue;
      }
      L.set("handle", h());
      L.set("extDict", h());
      const Value lh = L.get("handle");
      const Value lx = L.get("extDict");
      ovr_objects.push_back(base::make_object({{"handle", lx},
                                               {"owner", lh},
                                               {"reactors", base::array_of(lh)},
                                               {"type", "DICTIONARY"},
                                               {"entries", std::move(entries)},
                                               {"hardOwner", 1},
                                               {"cloning", 1}}));
      for (Value& r : recs) {
        r.set("owner", lx);
        r.set("reactors", base::array_of(lx));
        ovr_objects.push_back(std::move(r));
      }
    }
    for (const auto& [name, lh] : lt_h) {
      if (std::ranges::find(used_lt_, name) == used_lt_.end()) {
        used_lt_.push_back(name);
      }
      lt_handles_[name] = lh;
    }
  }
  Array linetypes;
  for (const std::string& name : used_lt_) {
    const Value* own = doc_.linetypes.get(name);
    const Value pat = pattern_of(name);
    const Array pattern = pat.is_array() ? pat.as_array() : Array{};
    Value o = base::make_object({{"name", name}});
    if (const auto it = lt_handles_.find(name); it != lt_handles_.end()) {
      o.set("handle", it->second);
    }
    const Value desc = own != nullptr ? own->get("description") : Value();
    o.set("description", desc.truthy() ? desc : Value(name));
    double plen = 0;
    Array elements;
    for (const Value& v : pattern) {
      plen = plen + std::abs(num(v));
      elements.push_back(base::make_object({{"length", v}}));
    }
    o.set("patternLength", plen);
    o.set("elements", std::move(elements));
    linetypes.push_back(std::move(o));
  }
  if (const Value lts = meta.get("ltscale"); num(lts) > 0 && !(lts == Value(1))) {
    header.set("LTSCALE", lts);
  }
  Array dimstyles = {base::make_object({{"name", "Standard"}, {"vars", ds_vars(doc_.current.get("dim"))}})};
  for (const auto& [k, d] : dim_styles_) {
    if (!is_standard(d.get("name"))) {
      dimstyles.push_back(d);
    }
  }
  Array styles;
  for (const auto& [k, s] : text_styles_) {
    styles.push_back(s);
  }
  Value tables = base::make_object({{"layers", std::move(layers)},
                                    {"views", std::move(views)},
                                    {"linetypes", std::move(linetypes)},
                                    {"appids", base::array_of(base::make_object({{"name", "CADCRAFT"}}))},
                                    {"dimstyles", std::move(dimstyles)},
                                    {"styles", std::move(styles)}});
  if (doc_.vports.truthy() || doc_.view_twist != 0) {
    Value fb(nullptr);
    if (ext.valid()) {
      fb = base::make_object({{"cx", (ext.min_x + ext.max_x) / 2},
                              {"cy", (ext.min_y + ext.max_y) / 2},
                              {"h", std::max({ext.max_y - ext.min_y, (ext.max_x - ext.min_x) / 1.6, 1e-6}) * 1.05}});
    }
    Array vps;
    for (const Value& r : cad::vport_records(doc_.vports, fb, doc_.view_twist)) {
      Value o = base::make_object({{"name", "*Active"},
                                   {"lowerLeft", base::make_object({{"x", r.get("x0")}, {"y", r.get("y0")}})},
                                   {"upperRight", base::make_object({{"x", r.get("x1")}, {"y", r.get("y1")}})},
                                   {"aspect", r.get("aspect")}});
      if (const Value tw = r.get("tw"); tw.truthy()) {
        o.set("twist", tw);
      }
      if (num(r.get("h")) > 0) {
        o.set("center", base::make_object({{"x", r.get("cx")}, {"y", r.get("cy")}}));
        o.set("viewHeight", r.get("h"));
      }
      if (const Value sh = r.get("sheet"); sh.truthy()) {
        const auto u16 = base::to_utf16(str(sh));
        std::string s;
        base::append_utf16(s, std::span<const std::uint16_t>(u16).first(std::min<std::size_t>(245, u16.size())));
        o.set("xdata", base::make_object({{"CADCRAFT", base::array_of(pair(1000, "PANESHEET:" + s))}}));
      }
      vps.push_back(std::move(o));
    }
    tables.set("vports", std::move(vps));
  }
  if (!ucs_table.empty()) {
    tables.set("ucs", std::move(ucs_table));
  }
  Array all_blocks = {base::make_object({{"name", "*Model_Space"}, {"entities", entities}})};
  for (Value& s : spaces) {
    all_blocks.push_back(std::move(s));
  }
  for (Value& b : blocks_) {
    all_blocks.push_back(std::move(b));
  }
  Value model = base::make_object({{"header", std::move(header)},
                                   {"tables", std::move(tables)},
                                   {"blocks", std::move(all_blocks)},
                                   {"layouts", std::move(layouts)}});
  if (!dyn_records_.empty()) {
    if (Value* t = model.find("tables")) {
      t->set("blockRecords", dyn_records_);
    }
    model.set("objects", ds_->objects());
    model.set("classes", ds_->classes());
    model.set("version", ds_->version());
    if (finite(ds_->maintenance())) {
      model.set("maintenance", ds_->maintenance());
    }
  }
  if (!pic_objects_.empty() || !ovr_objects.empty() || !clip_objects_.empty()) {
    Array objs = model.get("objects").is_array() ? model.get("objects").as_array() : Array{};
    for (const Value& x : pic_objects_) {
      objs.push_back(x);
    }
    for (Value& x : ovr_objects) {
      objs.push_back(std::move(x));
    }
    for (const Value& x : clip_objects_) {
      objs.push_back(x);
    }
    model.set("objects", std::move(objs));
  }
  DwgSaveResult res;
  res.counts = counts_;
  if (opts_.model_only) {
    res.model = std::move(model);
    return res;
  }
  dwg::WriteOptions wo;
  wo.version = code_;
  wo.created_ms = opts_.created_ms;
  wo.random = opts_.random;
  dwg::WriteResult wr = dwg::write_dwg(model, wo);
  res.model = std::move(model);
  Value top = Value::object();
  for (const Value& e : entities) {
    const std::string k = str(e.get("type"));
    const Value have = top.get(k);
    top.set(k, (have.truthy() ? num(have) : 0) + 1);
  }
  Array notes;
  if (count("truecolor") != 0 && r2000()) {
    notes.emplace_back(words_.t("dwg.save.truecolor"));
  }
  if (const double n = count("hatchUnknown"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.hatch.unknown", n));
  }
  if (count("mtextwidth") != 0) {
    notes.emplace_back(words_.t("dwg.save.mtextwidth"));
  }
  if (const double n = count("paperView"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.paperview", n));
  }
  if (m3) {
    notes.emplace_back(words_.tn("dwg.save.model3d", static_cast<double>(m3->faces.size()),
                                 {{"layer", std::string(m3d::kM3dLayer)}, {"block", std::string(m3d::kM3dBlock)}}));
  }
  if (const double n = count("xrefBound"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.xref.bound", n));
  }
  if (const double n = count("xrefLink"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.xref.link", n));
  }
  if (const double n = count("other"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.other", n));
  }
  if (count("material") != 0) {
    notes.emplace_back(words_.t("dwg.save.material"));
  }
  if (count("tr2000") != 0) {
    notes.emplace_back(words_.t("dwg.save.tr2000"));
  }
  if (const double n = count("mask2000"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.mask2000", n));
  }
  if (const double n = count("clipDyn"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.clipdyn", n));
  }
  if (const double n = count("dynDropped"); n != 0) {
    notes.emplace_back(words_.tn("dwg.save.dyn.dropped", n));
  }
  if (!dyn_records_.empty() && !ds_->version().empty() && ds_->version() != code_) {
    double n = 0;
    for (const Value& r : dyn_records_) {
      const Value* b = ds_->block(str(r.get("name")));
      n += b != nullptr && b->get("def").truthy() ? 1 : 0;
    }
    notes.emplace_back(words_.tn("dwg.save.dyn.version", n, {{"v", words_.t("dwg.version", {{"v", vkey_}})}}));
  }
  if (const Value own = wr.report.get("notes"); own.is_array()) {
    for (const Value& t : own.as_array()) {
      if (!str(t).contains("true colour")) {
        notes.push_back(t);
      }
    }
  }
  Value report = wr.report;
  report.set("version", vkey_);
  report.set("notes", std::move(notes));
  Value counts = report.get("counts").is_object() ? report.get("counts") : Value::object();
  spread_into(counts, counts_);
  report.set("counts", std::move(counts));
  report.set("top", std::move(top));
  res.bytes = std::move(wr.bytes);
  res.report = std::move(report);
  return res;
}

}  // namespace

std::span<const DwgVersion> dwg_versions() noexcept {
  return kVersions;
}

std::string dwg_version_key(std::string_view v) {
  std::string s = base::js_to_upper(v);
  if (!s.empty() && s[0] == 'R') {
    s.erase(0, 1);
  }
  for (const DwgVersion& x : kVersions) {
    if (x.key == s || x.code == s) {
      return std::string(x.key);
    }
  }
  return {};
}

DwgSaveResult save_dwg(const doc::Doc& doc, const DwgSaveOptions& opts, const i18n::Catalog& words) {
  Saver s(doc, opts, words);
  return s.run();
}

}  // namespace cc::save
