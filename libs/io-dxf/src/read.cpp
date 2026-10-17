// SPDX-License-Identifier: Apache-2.0
// Port of BimCraft dxf-model.js (CadCraft 0.5). Same model, same field order and JS types; JS's parseFloat / parseInt /
// String#trim / Number#toString semantics are reproduced where the model depends on them.
#include "cc/dxf/read.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/reader.hpp"

namespace cc::dxf {

using base::Array;
using base::js_parse_float;
using base::js_parse_int;
using base::js_trim;
using base::make_object;
using base::to_utf16;
using base::Value;

namespace {

constexpr double kD2R = std::numbers::pi / 180;

// UTF-16 round trip for the caret decoder
std::string from_utf16(const std::vector<std::uint16_t>& u) {
  std::string s;
  base::append_utf16(s, u);
  return s;
}

/// DXF caret escapes: "^ " is a caret, ^J and the like control characters (decode() in the JS).
std::string caret_decode(std::string_view s) {
  if (!s.contains('^')) {
    return std::string(s);
  }
  const std::vector<std::uint16_t> u = to_utf16(s);
  std::vector<std::uint16_t> out;
  out.reserve(u.size());
  for (std::size_t i = 0; i < u.size(); ++i) {
    // JS /\^(.)/: '.' is any code unit except a line terminator
    const auto is_term = [](std::uint16_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; };
    if (u[i] == '^' && i + 1 < u.size() && !is_term(u[i + 1])) {
      const std::uint16_t c = u[i + 1];
      out.push_back(c == ' ' ? std::uint16_t{'^'} : static_cast<std::uint16_t>(c & 31U));
      ++i;
    } else {
      out.push_back(u[i]);
    }
  }
  return from_utf16(out);
}

/// How many times `for (let i = 0; i < n; i++)` runs (NaN and negatives: none).
std::size_t js_count(double n) {
  return n > 0 ? base::clamp_cast<std::size_t>(std::ceil(n)) : 0;
}

/// hx(): String(v || '').trim().toUpperCase().replace(/^0+/, '')
std::string hx(const Value& v) {
  if (!v.truthy()) {
    return {};
  }
  std::string s;
  if (v.is_string()) {
    s = std::string(js_trim(v.as_string()));
  } else if (v.is_number()) {
    base::append_number(s, v.as_number());
  } else if (v.is_bool()) {
    s = "true";
  } else if (v.is_object()) {
    s = "[object Object]";
  }
  s = base::to_upper(s);
  std::size_t z = 0;
  while (z < s.size() && s[z] == '0') {
    ++z;
  }
  return s.substr(z);
}

/// A JS TypeError the reference throws on a malformed file: read_dxf_model returns its message, as the JS reader's
/// caller shows it.
class JsTypeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// String(v ?? fallback)
std::string str(const Value* v, std::string_view fallback);
std::string str(const Value& v, std::string_view fallback) {
  return str(&v, fallback);
}
// NOLINTNEXTLINE(misc-no-recursion) — an array's elements: as deep as the model's arrays
std::string str(const Value* v, std::string_view fallback) {
  if (!v || v->is_undefined() || v->is_null()) {
    return std::string(fallback);
  }
  if (v->is_string()) {
    return v->as_string();
  }
  if (v->is_bool()) {
    return v->as_bool() ? "true" : "false";
  }
  std::string s;
  if (v->is_number()) {
    base::append_number(s, v->as_number());  // NaN, Infinity as String() writes them
  } else if (v->is_object()) {
    s = "[object Object]";  // e.g. a header variable a stray point group turned into a point
  } else if (v->is_array()) {
    bool first = true;
    for (const Value& x : v->as_array()) {  // Array#join(","): null and undefined as ""
      if (!first) {
        s += ',';
      }
      first = false;
      s += str(&x, "");
    }
  }
  return s;
}

// ---------------------------------------------------------------- groups
/// A group: its code and the value it points at in the reader's value list (built once, never edited while read).
struct Group {
  int code;
  const Value* v;
};
using Groups = std::vector<Group>;

/// The value of a group by its code range (DXF reference, "Group code value types").
Value typed(int code, std::string_view v) {
  if ((code >= 10 && code <= 59) || (code >= 110 && code <= 149) || (code >= 210 && code <= 239) ||
      (code >= 460 && code <= 469) || (code >= 1010 && code <= 1059)) {
    const double x = js_parse_float(v);
    return std::isfinite(x) ? x : 0.0;
  }
  if ((code >= 60 && code <= 99) || (code >= 160 && code <= 179) || (code >= 270 && code <= 289) ||
      (code >= 370 && code <= 389) || (code >= 400 && code <= 409) || (code >= 420 && code <= 429) ||
      (code >= 440 && code <= 459) || (code >= 1060 && code <= 1071)) {
    const double x = js_parse_int(v);
    return std::isfinite(x) ? x : 0.0;
  }
  if (code >= 290 && code <= 299) {
    return js_parse_int(v) == 1;
  }
  return std::string(v);
}

const Value* val(const Groups& g, int code) {
  for (const Group& x : g) {
    if (x.code == code) {
      return x.v;
    }
  }
  return nullptr;
}
bool has(const Groups& g, int code) {
  return std::ranges::any_of(g, [&](const Group& x) { return x.code == code; });
}
/// `val(g, c) || 0` and friends
double num_or(const Value* v, double def) {
  if (v && v->truthy()) {
    return v->is_number() ? v->as_number() : def;
  }
  return def;
}
/// `val(g, c) ?? def` for numbers
Value nullish(const Value* v, const Value& def) {
  return v && !v->is_undefined() && !v->is_null() ? *v : def;
}

Value point(const Groups& g, int code, bool z = true, const Value* dflt = nullptr) {
  const Value* x = nullptr;
  const Value* y = nullptr;
  const Value* zz = nullptr;
  for (const Group& e : g) {
    if (e.code == code && !x) {
      x = e.v;
    } else if (e.code == code + 10 && !y) {
      y = e.v;
    } else if (e.code == code + 20 && !zz) {
      zz = e.v;
    }
  }
  if (!x && dflt != nullptr) {
    return *dflt;
  }
  auto or0 = [](const Value* v) { return v && v->truthy() ? *v : Value(0); };
  Value p = make_object({{"x", or0(x)}, {"y", or0(y)}});
  if (z) {
    p.set("z", or0(zz));
  }
  return p;
}
Value points(const Groups& g, int code, bool z = true) {
  Array out;
  for (const Group& e : g) {
    if (e.code == code) {
      out.push_back(z ? make_object({{"x", (*e.v)}, {"y", 0}, {"z", 0}}) : make_object({{"x", (*e.v)}, {"y", 0}}));
    } else if (e.code == code + 10 && !out.empty()) {
      out.back().set("y", (*e.v));
    } else if (z && e.code == code + 20 && !out.empty()) {
      out.back().set("z", (*e.v));
    }
  }
  return out;
}
const Value* sub_val(const Groups& g, std::string_view sub, int code) {
  bool inside = false;
  for (const Group& e : g) {
    if (e.code == 100) {
      inside = (*e.v).is_string() && (*e.v).as_string() == sub;
    } else if (inside && e.code == code) {
      return e.v;
    }
  }
  return nullptr;
}
Value sub_point(const Groups& g, std::string_view sub, int code, bool z = false) {
  bool inside = false;
  Value p = z ? base::pt3(0, 0, 0) : base::pt2(0, 0);
  for (const Group& e : g) {
    if (e.code == 100) {
      inside = (*e.v).is_string() && (*e.v).as_string() == sub;
    } else if (inside && e.code == code) {
      p.set("x", (*e.v));
    } else if (inside && e.code == code + 10) {
      p.set("y", (*e.v));
    } else if (inside && e.code == code + 20) {
      p.set("z", (*e.v));
    }
  }
  return p;
}
Value color_of(double index, const Value* rgb) {
  if (rgb && !rgb->is_undefined()) {
    const auto r = base::js_to_int32(rgb->num_or(0));
    return make_object({{"rgb", static_cast<double>(r & 0xFFFFFF)}, {"index", index}});
  }
  return make_object({{"index", index}});
}
Value xdata(const Groups& g) {
  Value out;
  std::string app;
  bool have_app = false;
  for (std::size_t k = 0; k < g.size(); ++k) {
    const int c = g[k].code;
    if (c == 1001) {
      if (out.is_undefined()) {
        out = Value::object();
      }
      app = str((*g[k].v), "");
      if (!out.has(app)) {
        out.set(app, Array{});
      }
      have_app = true;
      continue;
    }
    if (!have_app || c < 1000) {
      continue;
    }
    Value* list = out.find(app);
    if (c >= 1010 && c <= 1013) {
      Value p = make_object({{"x", (*g[k].v)}, {"y", 0}, {"z", 0}});
      if (k + 1 < g.size() && g[k + 1].code == c + 10) {
        p.set("y", (*g[++k].v));
      }
      if (k + 1 < g.size() && g[k + 1].code == c + 20) {
        p.set("z", (*g[++k].v));
      }
      list->push(Array{c, std::move(p)});
    } else {
      list->push(Array{c, (*g[k].v)});
    }
  }
  return out;
}

/// A JS object spread {...e, more}: a copy of e, then the extra members.
/// The entity is consumed: no argument list in this file reads `e` beside the spread.
Value spread(Value o, std::initializer_list<base::Init> more) {
  for (const base::Init& m : more) {
    o.set(m.key, std::move(m.value));
  }
  return o;
}

std::string hex_count(int n) {
  std::string s = base::hex_upper(static_cast<std::uint64_t>(n));
  return s;
}

class Reader {
 public:
  explicit Reader(std::vector<int> codes, std::vector<Value> vals)
      : codes_(std::move(codes)), vals_(std::move(vals)), n_(codes_.size()) {
    model_ = make_object(
        {{"version", ""},
         {"maintenance", 0},
         {"codepage", ""},
         {"header", Value::object()},
         {"classes", Array{}},
         {"tables", make_object({{"layers", Array{}},
                                 {"linetypes", Array{}},
                                 {"styles", Array{}},
                                 {"dimstyles", Array{}},
                                 {"views", Array{}},
                                 {"ucs", Array{}},
                                 {"vports", Array{}},
                                 {"appids", Array{}},
                                 {"blockRecords", Array{}}})},
         {"blocks", Array{}},
         {"layouts", Array{}},
         {"objects", Array{}},
         {"report", make_object({{"counts", Value::object()}, {"notes", Array{}}, {"unknown", Value::object()}})}});
  }

  Value run() {
    while (i_ < n_) {
      if (!is0("SECTION")) {
        ++i_;
        continue;
      }
      ++i_;
      std::string sec;
      if (i_ < n_ && codes_[i_] == 2) {
        sec = str(vals_[i_++], "");
      }
      if (sec == "HEADER") {
        read_header(group());
      } else if (sec == "TABLES") {
        read_tables();
      } else if (sec == "BLOCKS") {
        read_blocks();
      } else if (sec == "ENTITIES") {
        read_entities(loose_, true, "ENDSEC");
      } else if (sec == "OBJECTS") {
        read_objects();
      }
      while (i_ < n_ && !is0("ENDSEC")) {
        ++i_;
        (void)group();
      }
      ++i_;
    }
    place();
    return std::move(model_);
  }

 private:
  [[nodiscard]] bool is0(std::string_view name) const {
    return i_ < n_ && codes_[i_] == 0 && vals_[i_].is_string() && vals_[i_].as_string() == name;
  }
  Groups group() {
    Groups g;
    while (i_ < n_ && codes_[i_] != 0) {
      g.push_back(Group{codes_[i_], &vals_[i_]});
      ++i_;
    }
    return g;
  }
  std::string handle_of(const Groups& g) {
    const Value* v = val(g, 5);
    if (!v || v->is_undefined() || v->is_null()) {
      v = val(g, 105);
    }
    std::string h = v ? hx(*v) : std::string();
    if (h.empty()) {
      h = "DXF" + hex_count(++auto_);
    }
    return h;
  }
  Array& table(std::string_view t) { return model_.find("tables")->find(t)->as_array(); }

  void read_header(const Groups& g) {
    Value& H = *model_.find("header");
    std::string name;
    bool have = false;
    for (const Group& e : g) {
      if (e.code == 9) {
        name = str((*e.v), "");
        if (!name.empty() && name[0] == '$') {
          name.erase(0, 1);
        }
        have = true;
        continue;
      }
      if (!have || name.empty()) {
        continue;
      }
      if (e.code >= 10 && e.code <= 39) {
        Value* p = H.find(name);
        if (p == nullptr || !p->is_object()) {
          p = &H.set(name, make_object({{"x", 0}, {"y", 0}}));
        }
        std::string_view axis = "z";
        if (e.code < 20) {
          axis = "x";
        } else if (e.code < 30) {
          axis = "y";
        }
        p->set(axis, *e.v);
      } else {
        H.set(name, (*e.v));
      }
    }
    model_.set("version", str(nullish_truthy(H["ACADVER"]), ""));
    model_.set("maintenance", H["ACADMAINTVER"].truthy() ? H["ACADMAINTVER"] : Value(0));
    model_.set("codepage", str(nullish_truthy(H["DWGCODEPAGE"]), ""));
  }
  static const Value* nullish_truthy(const Value& v) {  // String(x || '')
    if (v.truthy()) {
      return &v;
    }
    return nullptr;
  }

  void read_tables() {
    while (i_ < n_ && !is0("ENDSEC")) {
      if (!is0("TABLE")) {
        ++i_;
        (void)group();
        continue;
      }
      ++i_;
      const Value* tv = val(group(), 2);
      while (i_ < n_ && !is0("ENDTAB") && !is0("ENDSEC")) {
        const Value kind = vals_[i_];
        ++i_;
        const Groups g = group();
        if (tv && kind == *tv && kind.is_string()) {
          table_entry(kind.as_string(), g);
        }
      }
      if (is0("ENDTAB")) {
        ++i_;
        (void)group();
      }
    }
  }

  void table_entry(const std::string& kind, const Groups& g) {
    const std::string handle = handle_of(g);
    const std::string name = str(val(g, 2), "");
    const double flags = num_or(val(g, 70), 0);
    if (kind == "LAYER") {
      const double ci = nullish(val(g, 62), 7).num_or(0);
      const Value* lw = val(g, 370);
      Value l = Value::object();
      l.set("handle", handle);
      l.set("name", name);
      l.set("flags", flags);
      const double aci = std::abs(ci) != 0 ? std::abs(ci) : 7;
      l.set("color", color_of(aci, val(g, 420)));
      l.set("linetypeName", str(val(g, 6), "Continuous"));
      l.set("lineweight", nullish(lw, -3));
      const Value* plot = val(g, 290);
      l.set("plot", !(plot != nullptr) || !plot->is_bool() || plot->as_bool());
      l.set("off", ci < 0);
      const auto fl = base::js_to_int32(flags);
      l.set("frozen", (fl & 1) != 0);
      l.set("locked", (fl & 4) != 0);
      table("layers").push_back(std::move(l));
    } else if (kind == "LTYPE") {
      table("linetypes")
          .push_back(make_object({{"handle", handle},
                                  {"name", name},
                                  {"description", str(val(g, 3), "")},
                                  {"patternLength", num_or(val(g, 40), 0)},
                                  {"elements", Array{}}}));
    } else if (kind == "STYLE") {
      table("styles").push_back(make_object({{"handle", handle},
                                             {"name", name},
                                             {"flags", flags},
                                             {"height", num_or(val(g, 40), 0)},
                                             {"widthFactor", nullish(val(g, 41), 1)},
                                             {"oblique", num_or(val(g, 50), 0) * kD2R},
                                             {"generation", num_or(val(g, 71), 0)},
                                             {"lastHeight", num_or(val(g, 42), 0)},
                                             {"font", str(val(g, 3), "")},
                                             {"bigFont", str(val(g, 4), "")}}));
    } else if (kind == "DIMSTYLE") {
      static constexpr std::array<std::pair<int, std::string_view>, 13> kDimVars = {{{40, "DIMSCALE"},
                                                                                     {41, "DIMASZ"},
                                                                                     {42, "DIMEXO"},
                                                                                     {43, "DIMDLI"},
                                                                                     {44, "DIMEXE"},
                                                                                     {140, "DIMTXT"},
                                                                                     {147, "DIMGAP"},
                                                                                     {77, "DIMTAD"},
                                                                                     {78, "DIMZIN"},
                                                                                     {271, "DIMDEC"},
                                                                                     {179, "DIMADEC"},
                                                                                     {144, "DIMLFAC"},
                                                                                     {141, "DIMCEN"}}};
      Value vars = Value::object();
      for (const Group& e : g) {
        for (const auto& [c, nm] : kDimVars) {
          if (c == e.code) {
            vars.set(nm, (*e.v));
          }
        }
      }
      table("dimstyles").push_back(make_object({{"handle", handle}, {"name", name}, {"vars", std::move(vars)}}));
    } else if (kind == "BLOCK_RECORD") {
      const Value* e280 = val(g, 280);
      table("blockRecords")
          .push_back(make_object({{"handle", handle},
                                  {"name", name},
                                  {"flags", 0},
                                  {"layout", val(g, 340) ? hx(*val(g, 340)) : std::string()},
                                  {"insertUnits", num_or(val(g, 70), 0)},
                                  {"explodable", !(e280 != nullptr) || !e280->is_number() || e280->as_number() != 0},
                                  {"scalable", num_or(val(g, 281), 0)}}));
    } else if (kind == "APPID") {
      table("appids").push_back(make_object({{"handle", handle}, {"name", name}, {"flags", flags}}));
    } else if (kind == "VPORT") {
      table("vports").push_back(make_object({{"handle", handle}, {"name", name}, {"flags", flags}}));
    }
  }

  void read_blocks() {
    while (i_ < n_ && !is0("ENDSEC")) {
      if (!is0("BLOCK")) {
        ++i_;
        (void)group();
        continue;
      }
      ++i_;
      const Groups g = group();
      const Value* nm = val(g, 2);
      if (!nm || nm->is_undefined()) {
        nm = val(g, 3);
      }
      Value blk = Value::object();
      blk.set("name", str(nm, ""));
      blk.set("handle", handle_of(g));
      blk.set("record", val(g, 330) ? hx(*val(g, 330)) : std::string());
      blk.set("base", point(g, 10));
      blk.set("flags", num_or(val(g, 70), 0));
      blk.set("xrefPath", str(val(g, 1), ""));
      blk.set("description", str(val(g, 4), ""));
      Array ents;
      read_entities(ents, false, "ENDBLK");
      blk.set("entities", std::move(ents));
      if (is0("ENDBLK")) {
        ++i_;
        blk.set("endHandle", handle_of(group()));
      }
      model_.find("blocks")->push(std::move(blk));
    }
  }

  struct Follow {
    std::string type;
    Groups g;
  };

  void read_entities(Array& list, bool top, std::string_view stop) {
    while (i_ < n_ && !is0("ENDSEC") && !is0(stop)) {
      if (codes_[i_] != 0) {
        ++i_;
        continue;
      }
      const std::string type = str(vals_[i_], "");
      ++i_;
      const Groups g = group();
      std::vector<Follow> follow;
      bool has_follow = false;
      const Value* v66 = val(g, 66);
      if (type == "POLYLINE" || (type == "INSERT" && v66 && v66->num_or(0) == 1)) {
        has_follow = true;
        while (i_ < n_ && codes_[i_] == 0 && (is0("VERTEX") || is0("ATTRIB"))) {
          std::string t = vals_[i_].as_string();
          ++i_;
          follow.push_back(Follow{std::move(t), group()});
        }
        if (is0("SEQEND")) {
          ++i_;
          (void)group();
        }
      }
      Value e = entity(type, g, has_follow ? &follow : nullptr);
      if (e.is_undefined()) {
        continue;
      }
      if (top) {
        const Value* v67 = val(g, 67);
        e.set("_paper", (v67 != nullptr) && v67->num_or(0) == 1);
      }
      list.push_back(std::move(e));
    }
  }

  void read_objects() {
    while (i_ < n_ && !is0("ENDSEC")) {
      if (codes_[i_] != 0) {
        ++i_;
        continue;
      }
      const std::string type = str(vals_[i_], "");
      ++i_;
      const Groups g = group();
      Array& objs = model_.find("objects")->as_array();
      if (type == "LAYOUT") {
        std::string sub;
        std::string block;
        std::string owner;
        for (const Group& e : g) {
          if (e.code == 100) {
            sub = str((*e.v), "");
          } else if (e.code == 330) {
            if (sub == "AcDbLayout") {
              block = hx((*e.v));
            } else if (owner.empty()) {
              owner = hx((*e.v));
            }
          }
        }
        Value lay = Value::object();
        lay.set("handle", handle_of(g));
        lay.set("owner", owner);
        lay.set("type", type);
        lay.set("name", str(sub_val(g, "AcDbLayout", 1), ""));
        lay.set("tabOrder", num_or(sub_val(g, "AcDbLayout", 71), 0));
        lay.set("flags", num_or(sub_val(g, "AcDbLayout", 70), 0));
        lay.set("block", block);
        lay.set("limMin", sub_point(g, "AcDbLayout", 10));
        lay.set("limMax", sub_point(g, "AcDbLayout", 11));
        lay.set("insBase", sub_point(g, "AcDbLayout", 12));
        lay.set("extMin", sub_point(g, "AcDbLayout", 14));
        lay.set("extMax", sub_point(g, "AcDbLayout", 15));
        lay.set("plot", make_object({{"pageSetupName", str(val(g, 1), "")},
                                     {"printer", str(val(g, 2), "")},
                                     {"paperSize", str(val(g, 4), "")},
                                     {"paperWidth", num_or(val(g, 44), 0)},
                                     {"paperHeight", num_or(val(g, 45), 0)},
                                     {"paperUnits", num_or(val(g, 72), 0)},
                                     {"rotation", num_or(val(g, 73), 0)}}));
        objs.push_back(std::move(lay));
      } else if (type == "IMAGEDEF") {
        const Value* v280 = val(g, 280);
        objs.push_back(make_object({{"handle", handle_of(g)},
                                    {"owner", val(g, 330) ? hx(*val(g, 330)) : std::string()},
                                    {"type", type},
                                    {"fileName", str(val(g, 1), "")},
                                    {"size", point(g, 10, false)},
                                    {"loaded", (v280 != nullptr) && v280->num_or(0) == 1}}));
      } else if (type != "DICTIONARY" && type != "ENDSEC") {
        objs.push_back(make_object(
            {{"handle", handle_of(g)}, {"owner", val(g, 330) ? hx(*val(g, 330)) : std::string()}, {"type", type}}));
      }
    }
  }

  // ---- one entity (recursive once: an INSERT's ATTRIBs are entities)
  // NOLINTNEXTLINE(misc-no-recursion)
  Value entity(const std::string& type, Groups g, const std::vector<Follow>* follow) {
    const Value* lw = val(g, 370);
    // room for the common members and the largest type's (TEXT/ATTRIB ≈ 29): an empty object grew 1→2→…→32, six
    // allocations and five moves of every member per entity
    Value e = base::object_with_capacity(32);
    e.set("handle", handle_of(g));
    e.set("type", type);
    e.set("owner", val(g, 330) ? hx(*val(g, 330)) : std::string());
    e.set("layer", str(val(g, 8), "0"));
    e.set("linetype", str(val(g, 6), "ByLayer"));
    e.set("color", color_of(nullish(val(g, 62), 256).num_or(0), val(g, 420)));
    e.set("lineweight", nullish(lw, -1));
    e.set("ltscale", nullish(val(g, 48), 1));
    const Value* v60 = val(g, 60);
    e.set("invisible", (v60 != nullptr) && v60->num_or(0) == 1);
    Value xd = xdata(g);
    if (!xd.is_undefined()) {
      e.set("xdata", std::move(xd));
    }
    const Value def_ext = base::pt3(0, 0, 1);
    const Value ext = point(g, 210, true, &def_ext);
    auto P = [&](int c) { return point(g, c); };
    auto num = [&](int c, double def) { return num_or(val(g, c), def); };
    if (type == "LINE") {
      return spread(std::move(e), {{"start", P(10)}, {"end", P(11)}, {"thickness", num(39, 0)}, {"extrusion", ext}});
    }
    if (type == "POINT") {
      return spread(
          std::move(e),
          {{"position", P(10)}, {"thickness", num(39, 0)}, {"extrusion", ext}, {"xAxisAngle", num(50, 0) * kD2R}});
    }
    if (type == "CIRCLE") {
      return spread(std::move(e),
                    {{"center", P(10)}, {"radius", num(40, 0)}, {"thickness", num(39, 0)}, {"extrusion", ext}});
    }
    if (type == "ARC") {
      return spread(std::move(e), {{"center", P(10)},
                                   {"radius", num(40, 0)},
                                   {"thickness", num(39, 0)},
                                   {"extrusion", ext},
                                   {"startAngle", num(50, 0) * kD2R},
                                   {"endAngle", num(51, 0) * kD2R}});
    }
    if (type == "ELLIPSE") {
      return spread(std::move(e), {{"center", P(10)},
                                   {"majorAxis", P(11)},
                                   {"extrusion", ext},
                                   {"ratio", nullish(val(g, 40), 1)},
                                   {"startParam", num(41, 0)},
                                   {"endParam", nullish(val(g, 42), 2 * std::numbers::pi)}});
    }
    if (type == "LWPOLYLINE") {
      Array vs;
      for (const Group& x : g) {
        if (x.code == 10) {
          vs.push_back(make_object({{"x", (*x.v)}, {"y", 0}, {"bulge", 0}, {"startWidth", 0}, {"endWidth", 0}}));
        } else if (vs.empty()) {
          continue;
        } else if (x.code == 20) {
          vs.back().set("y", (*x.v));
        } else if (x.code == 40) {
          vs.back().set("startWidth", (*x.v));
        } else if (x.code == 41) {
          vs.back().set("endWidth", (*x.v));
        } else if (x.code == 42) {
          vs.back().set("bulge", (*x.v));
        }
      }
      const auto f = base::js_to_int32(num(70, 0));
      const double cw = num(43, 0);
      if (cw != 0 && !has(g, 40) && !has(g, 41)) {
        for (Value& v : vs) {
          v.set("startWidth", cw);
          v.set("endWidth", cw);
        }
      }
      return spread(std::move(e), {{"constantWidth", cw},
                                   {"elevation", num(38, 0)},
                                   {"thickness", num(39, 0)},
                                   {"extrusion", ext},
                                   {"closed", (f & 1) != 0},
                                   {"plinegen", (f & 128) != 0},
                                   {"vertices", std::move(vs)}});
    }
    if (type == "POLYLINE") {
      const double flags_d = num(70, 0);
      const auto flags = base::js_to_int32(flags_d);
      struct V {
        const Groups* g;
        std::int64_t f;
      };
      std::vector<V> vx;
      if (follow != nullptr) {
        for (const Follow& s : *follow) {
          if (s.type == "VERTEX") {
            vx.push_back(V{&s.g, base::js_to_int32(num_or(val(s.g, 70), 0))});
          }
        }
      }
      auto vert = [](const V& s) {
        Value p = point(*s.g, 10);
        p.set("flags", static_cast<double>(s.f));
        return p;
      };
      if ((flags & 64) != 0) {
        Array vertices;
        Array faces;
        for (const V& s : vx) {
          if ((s.f & 64) != 0) {
            vertices.push_back(vert(s));
          }
        }
        for (const V& s : vx) {
          if ((s.f & 128) != 0 && (s.f & 64) == 0) {
            faces.emplace_back(Array{num_or(val(*s.g, 71), 0), num_or(val(*s.g, 72), 0), num_or(val(*s.g, 73), 0),
                                     num_or(val(*s.g, 74), 0)});
          }
        }
        const double nv = num(71, 0) != 0 ? num(71, 0) : static_cast<double>(vertices.size());
        const double nf = num(72, 0) != 0 ? num(72, 0) : static_cast<double>(faces.size());
        Value o = spread(std::move(e),
                         {{"type", "POLYLINE_PFACE"}, {"numVertices", nv}, {"numFaces", nf}, {"flags", flags_d}});
        o.set("vertices", std::move(vertices));
        o.set("faces", std::move(faces));
        o.set("closed", false);
        return o;
      }
      Array vs;
      if ((flags & 16) != 0) {
        for (const V& s : vx) {
          vs.push_back(vert(s));
        }
        return spread(std::move(e), {{"type", "POLYLINE_MESH"},
                                     {"flags", flags_d},
                                     {"curveType", num(75, 0)},
                                     {"m", num(71, 0)},
                                     {"n", num(72, 0)},
                                     {"mDensity", num(73, 0)},
                                     {"nDensity", num(74, 0)},
                                     {"vertices", std::move(vs)},
                                     {"closed", (flags & 1) != 0}});
      }
      if ((flags & 8) != 0) {
        for (const V& s : vx) {
          vs.push_back(vert(s));
        }
        return spread(std::move(e), {{"type", "POLYLINE_3D"},
                                     {"flags", flags_d},
                                     {"curveType", num(75, 0)},
                                     {"closed", (flags & 1) != 0},
                                     {"vertices", std::move(vs)}});
      }
      for (const V& s : vx) {
        Value p = point(*s.g, 10);
        p.set("startWidth", num_or(val(*s.g, 40), 0));
        p.set("endWidth", num_or(val(*s.g, 41), 0));
        p.set("bulge", num_or(val(*s.g, 42), 0));
        p.set("tangent", num_or(val(*s.g, 50), 0) * kD2R);
        p.set("flags", static_cast<double>(s.f));
        vs.push_back(std::move(p));
      }
      return spread(std::move(e), {{"type", "POLYLINE_2D"},
                                   {"flags", flags_d},
                                   {"curveType", num(75, 0)},
                                   {"startWidth", num(40, 0)},
                                   {"endWidth", num(41, 0)},
                                   {"thickness", num(39, 0)},
                                   {"elevation", point(g, 10).get("z")},
                                   {"extrusion", ext},
                                   {"closed", (flags & 1) != 0},
                                   {"vertices", std::move(vs)}});
    }
    if (type == "TEXT" || type == "ATTRIB" || type == "ATTDEF") {
      return text_ent(std::move(e), g, ext);
    }
    if (type == "MTEXT") {
      const auto cut = std::ranges::find_if(g, [](const Group& x) { return x.code == 101; });
      if (cut != g.end()) {
        g.erase(cut, g.end());  // an embedded object (columns, R2018) repeats codes
      }
      Value x_axis;
      if (val(g, 11)) {
        x_axis = point(g, 11);
      } else {
        const double a = num(50, 0) * kD2R;
        x_axis = base::pt3(std::cos(a), std::sin(a), 0);
      }
      std::string value;
      for (const Group& x : g) {
        if (x.code == 3) {
          value += str((*x.v), "");
        }
      }
      value = caret_decode(value + str(val(g, 1), ""));
      return spread(std::move(e), {{"insert", point(g, 10)},
                                   {"extrusion", ext},
                                   {"xAxis", x_axis},
                                   {"width", num(41, 0)},
                                   {"rectHeight", num(46, 0)},
                                   {"height", num(40, 0)},
                                   {"attach", num(71, 1)},
                                   {"direction", num(72, 1)},
                                   {"value", value},
                                   {"style", str(val(g, 7), "Standard")},
                                   {"lineSpacingStyle", num(73, 1)},
                                   {"lineSpacing", num(44, 1)}});
    }
    if (type == "INSERT") {
      const double cols = num(70, 1);
      const double rows = num(71, 1);
      Array attribs;
      if (follow != nullptr) {
        for (const Follow& s : *follow) {
          if (s.type == "ATTRIB") {
            Value a = entity("ATTRIB", s.g, nullptr);
            a.set("owner", e["handle"]);
            attribs.push_back(std::move(a));
          }
        }
      }
      Value ins = spread(
          std::move(e),
          {{"type", cols * rows > 1 ? "MINSERT" : "INSERT"},
           {"insert", P(10)},
           {"scale",
            make_object({{"x", nullish(val(g, 41), 1)}, {"y", nullish(val(g, 42), 1)}, {"z", nullish(val(g, 43), 1)}})},
           {"rotation", num(50, 0) * kD2R},
           {"extrusion", ext},
           {"block", str(val(g, 2), "")}});
      ins.set("attribs", std::move(attribs));
      if (cols * rows > 1) {
        ins.set("cols", cols);
        ins.set("rows", rows);
        ins.set("colSpacing", num(44, 0));
        ins.set("rowSpacing", num(45, 0));
      }
      return ins;
    }
    if (type == "DIMENSION" || type == "ARC_DIMENSION") {
      static constexpr std::array<std::string_view, 7> kDimTypes = {
          "DIMENSION_LINEAR", "DIMENSION_ALIGNED", "DIMENSION_ANG2LN",  "DIMENSION_DIAMETER",
          "DIMENSION_RADIUS", "DIMENSION_ANG3PT",  "DIMENSION_ORDINATE"};
      const double flags = num(70, 0);
      const auto dim_type = type == "ARC_DIMENSION" ? 8 : base::js_to_int32(flags) & 7;
      // DIM_TYPES[dimType] || 'DIMENSION_LINEAR' (type 7 has no name)
      std::string t = "DIMENSION_LINEAR";
      if (type == "ARC_DIMENSION") {
        t = type;
      } else if (dim_type < 7) {
        t = std::string(kDimTypes[static_cast<std::size_t>(dim_type)]);
      }
      Value d = spread(std::move(e), {{"type", t},
                                      {"dimType", static_cast<double>(dim_type)},
                                      {"flags", flags},
                                      {"block", str(val(g, 2), "")},
                                      {"style", str(val(g, 3), "Standard")},
                                      {"extrusion", ext},
                                      {"defPoint", P(10)},
                                      {"textMid", P(11)},
                                      {"insert", P(12)},
                                      {"text", str(val(g, 1), "")},
                                      {"measurement", num(42, 0)},
                                      {"attach", num(71, 5)},
                                      {"lineSpacingStyle", num(72, 1)},
                                      {"lineSpacing", num(41, 1)},
                                      {"textRotation", num(53, 0) * kD2R},
                                      {"horizDir", num(51, 0) * kD2R}});
      if (has(g, 13)) {
        d.set("xline1", P(13));
      }
      if (has(g, 14)) {
        d.set("xline2", P(14));
      }
      if (has(g, 15)) {
        d.set("center", P(15));
      }
      if (has(g, 16)) {
        d.set("arcPoint", P(16));
      }
      if (has(g, 40)) {
        d.set("leaderLen", *val(g, 40));
      }
      if (dim_type <= 1) {
        d.set("angle", num(50, 0) * kD2R);
        d.set("obliqueAngle", num(52, 0) * kD2R);
      }
      return d;
    }
    if (type == "LEADER") {
      return spread(std::move(e), {{"vertices", points(g, 10)},
                                   {"style", str(val(g, 3), "")},
                                   {"arrowhead", nullish(val(g, 71), 1)},
                                   {"pathType", num(72, 0)},
                                   {"annotType", nullish(val(g, 73), 3)},
                                   {"annotation", val(g, 340) ? hx(*val(g, 340)) : std::string()},
                                   {"extrusion", ext}});
    }
    if (type == "HATCH") {
      return hatch(std::move(e), g, ext);
    }
    if (type == "SOLID" || type == "TRACE") {
      Array pts{P(10), P(11), P(12), has(g, 13) ? P(13) : P(12)};
      Value elev = pts[0].get("z");
      return spread(std::move(e),
                    {{"thickness", num(39, 0)}, {"elevation", elev}, {"points", std::move(pts)}, {"extrusion", ext}});
    }
    if (type == "3DFACE") {
      return spread(std::move(e), {{"points", Array{P(10), P(11), P(12), has(g, 13) ? P(13) : P(12)}},
                                   {"invisibleEdges", num(70, 0)}});
    }
    if (type == "SPLINE") {
      const auto f = base::js_to_int32(num(70, 0));
      Array knots;
      Array w;
      for (const Group& x : g) {
        if (x.code == 40) {
          knots.push_back((*x.v));
        } else if (x.code == 41) {
          w.push_back((*x.v));
        }
      }
      Value s = spread(std::move(e), {{"degree", num(71, 3)},
                                      {"flags", static_cast<double>(f)},
                                      {"closed", (f & 1) != 0},
                                      {"periodic", (f & 2) != 0},
                                      {"rational", (f & 4) != 0},
                                      {"extrusion", ext},
                                      {"knots", std::move(knots)},
                                      {"control", points(g, 10)},
                                      {"fit", points(g, 11)},
                                      {"knotTolerance", nullish(val(g, 42), 1e-10)},
                                      {"controlTolerance", nullish(val(g, 43), 1e-10)},
                                      {"fitTolerance", nullish(val(g, 44), 1e-10)}});
      if (!w.empty()) {
        s.set("weights", std::move(w));
      }
      if (has(g, 12)) {
        s.set("startTangent", P(12));
      }
      if (has(g, 13)) {
        s.set("endTangent", P(13));
      }
      return s;
    }
    if (type == "RAY" || type == "XLINE") {
      return spread(std::move(e), {{"start", P(10)}, {"direction", P(11)}});
    }
    if (type == "VIEWPORT") {
      const Value def_dir = base::pt3(0, 0, 1);
      Array frozen;
      for (const Group& x : g) {
        if (x.code == 331) {
          frozen.emplace_back(hx((*x.v)));
        }
      }
      return spread(std::move(e), {{"center", P(10)},
                                   {"width", num(40, 0)},
                                   {"height", num(41, 0)},
                                   {"id", num(69, 0)},
                                   {"viewCenter", point(g, 12, false)},
                                   {"viewTarget", P(17)},
                                   {"viewDirection", point(g, 16, true, &def_dir)},
                                   {"viewHeight", num(45, 0)},
                                   {"twist", num(51, 0) * kD2R},
                                   {"lensLength", nullish(val(g, 42), 50)},
                                   {"frontClip", num(43, 0)},
                                   {"backClip", num(44, 0)},
                                   {"status", num(90, 0)},
                                   {"frozenLayers", std::move(frozen)}});
    }
    if (type == "IMAGE" || type == "WIPEOUT") {
      const Value* v280 = val(g, 280);
      return spread(std::move(e), {{"insert", P(10)},
                                   {"u", P(11)},
                                   {"v", P(12)},
                                   {"size", point(g, 13, false)},
                                   {"def", val(g, 340) ? hx(*val(g, 340)) : std::string()},
                                   {"defReactor", val(g, 360) ? hx(*val(g, 360)) : std::string()},
                                   {"displayFlags", num(70, 0)},
                                   {"clipping", (v280 != nullptr) && v280->num_or(0) == 1},
                                   {"brightness", nullish(val(g, 281), 50)},
                                   {"contrast", nullish(val(g, 282), 50)},
                                   {"fade", num(283, 0)},
                                   {"clipType", num(71, 1)},
                                   {"clip", points(g, 14, false)}});
    }
    if (type == "TOLERANCE") {
      return spread(std::move(e), {{"insert", P(10)},
                                   {"xDirection", P(11)},
                                   {"extrusion", ext},
                                   {"text", str(val(g, 1), "")},
                                   {"style", str(val(g, 3), "")}});
    }
    if (type == "MLINE") {
      Array vs;
      const Value p11 = points(g, 11);
      const Value p12 = points(g, 12);
      const Value p13 = points(g, 13);
      const Array& a11 = p11.as_array();
      for (std::size_t k = 0; k < a11.size(); ++k) {
        Value dir = k < p12.as_array().size() ? p12.as_array()[k] : base::pt3(1, 0, 0);
        Value mit = k < p13.as_array().size() ? p13.as_array()[k] : base::pt3(0, 1, 0);
        vs.push_back(make_object({{"point", a11[k]}, {"direction", std::move(dir)}, {"miter", std::move(mit)}}));
      }
      return spread(std::move(e), {{"style", str(val(g, 2), "STANDARD")},
                                   {"scale", nullish(val(g, 40), 1)},
                                   {"justification", num(70, 0)},
                                   {"closed", (base::js_to_int32(num(71, 0)) & 2) != 0},
                                   {"base", P(10)},
                                   {"extrusion", ext},
                                   {"vertices", std::move(vs)}});
    }
    if (type == "MULTILEADER") {
      return mleader(std::move(e), g);
    }
    if (type == "ACAD_TABLE") {
      return spread(std::move(e),
                    {{"insert", P(10)}, {"extrusion", ext}, {"block", str(val(g, 2), "")}, {"partial", true}});
    }
    if (type == "3DSOLID" || type == "REGION" || type == "BODY" || type == "LIGHT" || type == "ACAD_PROXY_ENTITY") {
      return e;  // type only
    }
    return spread(std::move(e), {{"type", "UNKNOWN"}, {"dxfName", type}});
  }

  static Value text_ent(Value e, const Groups& g, const Value& ext) {
    const std::string type = e["type"].as_string();
    const bool att = type != "TEXT";
    const bool marked = has(g, 100);
    std::string_view own;
    if (att) {
      own = type == "ATTRIB" ? "AcDbAttribute" : "AcDbAttributeDefinition";
    }
    auto T = [&](int code) { return marked ? sub_val(g, "AcDbText", code) : val(g, code); };
    auto A = [&](int code) { return marked ? sub_val(g, own, code) : val(g, code); };
    const Value insert = marked ? sub_point(g, "AcDbText", 10, true) : point(g, 10);
    Value align;
    if (marked) {
      if (sub_val(g, "AcDbText", 11)) {
        align = sub_point(g, "AcDbText", 11, true);
      }
    } else if (has(g, 11)) {
      align = point(g, 11);
    }
    const Value* v_text = T(1);
    Value t = spread(std::move(e), {{"insert", insert},
                                    {"align", align.is_undefined() ? insert : align},
                                    {"height", num_or(T(40), 0)},
                                    {"value", caret_decode(str(v_text, ""))},
                                    {"rotation", num_or(T(50), 0) * kD2R},
                                    {"widthFactor", nullish(T(41), 1)},
                                    {"oblique", num_or(T(51), 0) * kD2R},
                                    {"style", str(T(7), "Standard")},
                                    {"hAlign", num_or(T(72), 0)},
                                    {"vAlign", num_or(att ? A(74) : T(73), 0)},
                                    {"generation", num_or(T(71), 0)},
                                    {"thickness", num_or(T(39), 0)},
                                    {"elevation", insert.get("z")},
                                    {"extrusion", ext}});
    if (att) {
      t.set("tag", str(A(2), ""));
      t.set("flags", num_or(A(70), 0));
      const Value* lock = A(280);
      t.set("lockPosition", (lock != nullptr) && lock->num_or(0) == 1);
      t.set("fieldLength", num_or(A(73), 0));
      if (type == "ATTDEF") {
        t.set("prompt", caret_decode(str(A(3), "")));
      }
    }
    return t;
  }

  /// HATCH: its groups repeat per loop and edge, so they are read in order.
  static Value hatch(Value e, const Groups& g, const Value& ext) {
    std::size_t k = 0;
    auto at = [&](int c) { return k < g.size() && g[k].code == c; };
    auto take = [&]() -> const Value& { return *g[k++].v; };
    auto takef = [&]() { return take().num_or(0); };
    auto xy = [&](int cx) {
      Value p = base::pt2(0, 0);
      if (at(cx)) {
        p.set("x", take());
      }
      if (at(cx + 10)) {
        p.set("y", take());
      }
      return p;
    };
    const Value* v70 = val(g, 70);
    const Value* v71 = val(g, 71);
    Value h = spread(std::move(e), {{"pattern", str(val(g, 2), "")},
                                    {"solid", (v70 != nullptr) && v70->num_or(0) == 1},
                                    {"associative", (v71 != nullptr) && v71->num_or(0) == 1},
                                    {"elevation", point(g, 10).get("z")},
                                    {"extrusion", ext}});
    h.set("loops", Array{});
    h.set("seeds", Array{});
    while (k < g.size() && !at(91)) {
      ++k;
    }
    const double n_loops = k < g.size() ? takef() : 0;
    for (std::size_t L = 0, L_n = js_count(n_loops); L < L_n && k < g.size(); ++L) {
      while (k < g.size() && !at(92)) {
        ++k;
      }
      if (k >= g.size()) {
        break;
      }
      const auto flags = base::js_to_int32(takef());
      Value loop = make_object({{"flags", static_cast<double>(flags)}});
      if ((flags & 2) != 0) {
        const bool has_bulges = at(72) ? takef() == 1 : false;
        const bool closed = at(73) ? takef() == 1 : true;
        const double cnt = at(93) ? takef() : 0;
        Array vertices;
        for (std::size_t v = 0, v_n = js_count(cnt); v < v_n && k < g.size(); ++v) {
          Value p = xy(10);
          const Value bulge = at(42) ? take() : Value(0);
          vertices.push_back(make_object({{"x", p.get("x")}, {"y", p.get("y")}, {"bulge", bulge}}));
        }
        loop.set("polyline",
                 make_object({{"closed", closed}, {"hasBulges", has_bulges}, {"vertices", std::move(vertices)}}));
      } else {
        const double cnt = at(93) ? takef() : 0;
        Array edges;
        for (std::size_t d = 0, d_n = js_count(cnt); d < d_n && k < g.size(); ++d) {
          while (k < g.size() && !at(72)) {
            ++k;
          }
          if (k >= g.size()) {  // HATCH_EDGE[take()] past the last group: the JS throws
            throw JsTypeError("Cannot read properties of undefined (reading '1')");
          }
          const double et = takef();
          if (et == 1) {
            Value s = xy(10);
            edges.push_back(make_object({{"type", "line"}, {"start", std::move(s)}, {"end", xy(11)}}));
          } else if (et == 2) {
            Value center = xy(10);
            const Value radius = at(40) ? take() : Value(0);
            const double s = at(50) ? takef() : 0;
            const double t = at(51) ? takef() : 360;
            const bool ccw = at(73) ? takef() == 1 : true;
            edges.push_back(make_object({{"type", "arc"},
                                         {"center", std::move(center)},
                                         {"radius", radius},
                                         {"startAngle", s * kD2R},
                                         {"endAngle", t * kD2R},
                                         {"ccw", ccw}}));
          } else if (et == 3) {
            Value center = xy(10);
            Value major = xy(11);
            const Value ratio = at(40) ? take() : Value(1);
            const double s = at(50) ? takef() : 0;
            const double t = at(51) ? takef() : 360;
            const bool ccw = at(73) ? takef() == 1 : true;
            edges.push_back(make_object({{"type", "ellipse"},
                                         {"center", std::move(center)},
                                         {"majorAxis", std::move(major)},
                                         {"ratio", ratio},
                                         {"startAngle", s * kD2R},
                                         {"endAngle", t * kD2R},
                                         {"ccw", ccw}}));
          } else if (et == 4) {
            Value sp = Value::object();
            sp.set("type", "spline");
            sp.set("degree", at(94) ? take() : Value(3));
            sp.set("rational", at(73) ? takef() == 1 : false);
            sp.set("periodic", at(74) ? takef() == 1 : false);
            sp.set("knots", Array{});
            sp.set("control", Array{});
            const double nk = at(95) ? takef() : 0;
            const double nc = at(96) ? takef() : 0;
            for (std::size_t q = 0, q_n = js_count(nk); q < q_n && at(40); ++q) {
              sp.find("knots")->push(take());
            }
            Array w;
            for (std::size_t q = 0, q_n = js_count(nc); q < q_n && at(10); ++q) {
              sp.find("control")->push(xy(10));
              if (at(42)) {
                w.push_back(take());
              }
            }
            if (!w.empty()) {
              sp.set("weights", std::move(w));
            }
            if (at(97)) {
              const double nf = takef();
              Array fit;
              for (std::size_t q = 0, q_n = js_count(nf); q < q_n && at(11); ++q) {
                fit.push_back(xy(11));
              }
              sp.set("fit", std::move(fit));
            }
            if (at(12)) {
              sp.set("startTangent", xy(12));
            }
            if (at(13)) {
              sp.set("endTangent", xy(13));
            }
            edges.push_back(std::move(sp));
          } else {
            break;
          }
        }
        loop.set("edges", std::move(edges));
      }
      Array boundary;
      if (at(97)) {
        const double nb = takef();
        for (std::size_t b = 0, b_n = js_count(nb); b < b_n && at(330); ++b) {
          boundary.emplace_back(hx(take()));
        }
      }
      loop.set("boundary", std::move(boundary));
      h.find("loops")->push(std::move(loop));
    }
    for (; k < g.size(); ++k) {
      const int c = g[k].code;
      const Value& v = (*g[k].v);
      if (c == 75) {
        h.set("style", v);
      } else if (c == 76) {
        h.set("patternType", v);
      } else if (c == 52) {
        h.set("angle", v.num_or(0) * kD2R);
      } else if (c == 41) {
        h.set("scale", v);
      } else if (c == 77) {
        h.set("double", v.num_or(0) == 1);
      } else if (c == 98) {
        const double ns = v.num_or(0);
        ++k;
        for (std::size_t s = 0, s_n = js_count(ns); s < s_n && k < g.size() && g[k].code == 10; ++s) {
          Value p = make_object({{"x", (*g[k].v)}, {"y", 0}});
          ++k;
          if (k < g.size() && g[k].code == 20) {
            p.set("y", (*g[k].v));
            ++k;
          }
          h.find("seeds")->push(std::move(p));
        }
        --k;
      }
    }
    return h;
  }

  /// MULTILEADER: the context data's leaders (lines), text and block.
  static Value mleader(Value e, const Groups& g) {
    Value ctx = make_object({{"roots", Array{}}, {"text", nullptr}, {"block", nullptr}});
    std::string depth;
    const auto end_it = std::ranges::find_if(
        g, [](const Group& x) { return x.code == 301 && (*x.v).is_string() && (*x.v).as_string() == "}"; });
    const Groups own = end_it != g.end() ? Groups(std::next(end_it), g.end()) : g;
    const Value* v291 = val(own, 291);
    const Value* v290 = val(own, 290);
    Value m = spread(std::move(e), {{"style", val(own, 340) ? hx(*val(own, 340)) : std::string()},
                                    {"leaderType", nullish(val(own, 170), 1)},
                                    {"contentType", nullish(val(own, 172), 2)},
                                    {"doglegEnabled", !(v291 != nullptr) || !v291->is_bool() || v291->as_bool()},
                                    {"landingEnabled", !(v290 != nullptr) || !v290->is_bool() || v290->as_bool()},
                                    {"arrowSize", num_or(val(own, 42), 0)}});
    std::ptrdiff_t root = -1;
    std::ptrdiff_t line_root = -1;  // the JS `line` (a leader line of some root; a new LEADER{ keeps it)
    std::ptrdiff_t line = -1;
    auto roots = [&]() -> Array& { return ctx.find("roots")->as_array(); };
    auto merge = [&](std::string_view key, std::string_view sub, Value v) {
      Value* t = ctx.find(key);
      if (t == nullptr || !t->is_object()) {
        ctx.set(key, Value::object());
        t = ctx.find(key);
      }
      t->set(sub, std::move(v));
    };
    for (std::size_t k = 0; k < g.size(); ++k) {
      const int c = g[k].code;
      const Value& v = *g[k].v;
      auto pt = [&](int cx) {
        Value p = make_object({{"x", v}, {"y", 0}, {"z", 0}});
        if (k + 1 < g.size() && g[k + 1].code == cx + 10) {
          p.set("y", (*g[++k].v));
        }
        if (k + 1 < g.size() && g[k + 1].code == cx + 20) {
          p.set("z", (*g[++k].v));
        }
        return p;
      };
      const bool sv = v.is_string();
      if (c == 300 && sv && v.as_string() == "CONTEXT_DATA{") {
        depth = "ctx";
        continue;
      }
      if (c == 302 && sv && v.as_string() == "LEADER{") {
        roots().push_back(make_object({{"connection", nullptr}, {"direction", nullptr}, {"lines", Array{}}}));
        root = static_cast<std::ptrdiff_t>(roots().size()) - 1;
        depth = "leader";
        continue;
      }
      if (c == 304 && sv && v.as_string() == "LEADER_LINE{" && root >= 0) {
        Array& lines = roots()[static_cast<std::size_t>(root)].find("lines")->as_array();
        lines.push_back(make_object({{"points", Array{}}}));
        line_root = root;
        line = static_cast<std::ptrdiff_t>(lines.size()) - 1;
        depth = "line";
        continue;
      }
      if (c == 304 && depth == "ctx") {
        merge("text", "label", str(v, ""));
        continue;
      }
      if (c == 305 && sv && v.as_string() == "}") {
        depth = "leader";
        continue;
      }
      if (c == 303 && sv && v.as_string() == "}") {
        depth = "ctx";
        continue;
      }
      if (c == 301 && sv && v.as_string() == "}") {
        depth.clear();
        continue;
      }
      // (the JS throws a TypeError on a leader value outside a LEADER{ or a line point outside a LEADER_LINE{: so does
      // this port, with its words, so such a file is refused the same way)
      if (depth == "line" && c == 10) {
        if (line < 0) {
          throw JsTypeError("Cannot read properties of null (reading 'points')");
        }
        Value p = pt(10);
        roots()[static_cast<std::size_t>(line_root)]
            .find("lines")
            ->as_array()[static_cast<std::size_t>(line)]
            .find("points")
            ->push(std::move(p));
      } else if (depth == "leader" && (c == 10 || c == 11 || c == 40)) {
        std::string_view field = "doglegLength";
        if (c == 10) {
          field = "connection";
        } else if (c == 11) {
          field = "direction";
        }
        Value p = c == 40 ? v : pt(c);
        if (root < 0) {
          throw JsTypeError("Cannot set properties of null (setting '" + std::string(field) + "')");
        }
        roots()[static_cast<std::size_t>(root)].set(field, std::move(p));
      } else if (depth == "ctx" && c == 41) {
        ctx.set("textHeight", v);
      } else if (depth == "ctx" && c == 140) {
        ctx.set("arrowSize", v);
      } else if (depth == "ctx" && c == 12) {
        merge("text", "location", pt(12));
      } else if (depth == "ctx" && c == 13) {
        merge("text", "direction", pt(13));
      } else if (depth == "ctx" && c == 290) {
        ctx.set("hasText", v);
      } else if (depth == "ctx" && c == 296) {
        ctx.set("hasBlock", v);
      } else if (depth == "ctx" && c == 341) {
        merge("block", "handle", hx(v));
      } else if (depth == "ctx" && c == 15) {
        merge("block", "location", pt(15));
      } else if (depth == "ctx" && c == 16) {
        merge("block", "scale", pt(16));
      } else if (depth == "ctx" && c == 46) {
        merge("block", "rotation", v);
      } else if (depth == "ctx" && c == 47 && ctx["block"].truthy()) {
        Value* b = ctx.find("block");
        if (!b->has("matrix")) {
          b->set("matrix", Array{});
        }
        b->find("matrix")->push(v);
      }
    }
    m.set("context", std::move(ctx));
    Array labels;
    for (std::size_t k = 0; k < own.size(); ++k) {
      if (own[k].code != 330) {
        continue;
      }
      Value l = make_object({{"attdefHandle", hx((*own[k].v))}, {"text", ""}, {"index", 0}, {"width", 0}});
      for (++k; k < own.size() && (own[k].code == 177 || own[k].code == 44 || own[k].code == 302); ++k) {
        if (own[k].code == 177) {
          l.set("index", (*own[k].v));
        } else if (own[k].code == 44) {
          l.set("width", (*own[k].v));
        } else {
          l.set("text", caret_decode(str((*own[k].v), "")));
        }
      }
      --k;
      labels.push_back(std::move(l));
    }
    m.set("blockLabels", std::move(labels));
    const Value& blk = m["context"]["block"];
    if (m["contentType"].num_or(0) == 1 && blk.is_object() && blk["handle"].truthy()) {
      m.set("contentBlock", blk["handle"]);
    }
    return m;
  }

  /// Blocks, records and layouts tied together; loose entities into their space; counts.
  void place() {
    Array& blocks = model_.find("blocks")->as_array();
    for (Value& b : blocks) {
      const std::string up = base::to_upper(b["name"].as_string());
      if (up == "$MODEL_SPACE") {
        b.set("name", "*Model_Space");
      } else if (up == "$PAPER_SPACE") {
        b.set("name", "*Paper_Space");
      }
    }
    // records by upper-case name (a JS Map: a later record of the same name wins)
    auto rec_index_by_name = [&](const std::string& name) -> std::ptrdiff_t {
      const auto it = rec_by_name_.find(base::to_upper(name));
      return it == rec_by_name_.end() ? -1 : static_cast<std::ptrdiff_t>(it->second);
    };
    {
      const Array& recs = table("blockRecords");
      for (std::size_t r = 0; r < recs.size(); ++r) {
        rec_by_name_[base::to_upper(recs[r]["name"].as_string())] = r;
      }
    }
    for (Value& b : blocks) {
      std::ptrdiff_t ri = -1;
      const std::string rec = b["record"].as_string();
      if (!rec.empty()) {
        const Array& recs = table("blockRecords");
        for (std::size_t r = 0; r < recs.size(); ++r) {
          if (recs[r]["handle"].as_string() == rec) {
            ri = static_cast<std::ptrdiff_t>(r);
            break;
          }
        }
      }
      if (ri < 0) {
        ri = rec_index_by_name(b["name"].as_string());
      }
      if (ri < 0) {
        Value r = make_object({{"handle", "R" + hex_count(++auto_)},
                               {"name", b["name"]},
                               {"flags", 0},
                               {"layout", ""},
                               {"insertUnits", 0},
                               {"explodable", true},
                               {"scalable", 0}});
        table("blockRecords").push_back(std::move(r));
        ri = static_cast<std::ptrdiff_t>(table("blockRecords").size()) - 1;
        rec_by_name_[base::to_upper(b["name"].as_string())] = static_cast<std::size_t>(ri);
      }
      Value& r = table("blockRecords")[static_cast<std::size_t>(ri)];
      b.set("record", r["handle"]);
      r.set("flags", b["flags"]);
      r.set("base", b["base"]);
      r.set("xrefPath", b["xrefPath"]);
      r.set("description", b["description"]);
      const Value rh = r["handle"];
      for (Value& ent : b.find("entities")->as_array()) {
        ent.set("owner", rh);
      }
    }
    auto space = [&](std::string_view name) -> std::size_t {
      for (std::size_t k = 0; k < blocks.size(); ++k) {
        if (base::to_upper(blocks[k]["name"].as_string()) == name) {
          return k;
        }
      }
      // R12: no *Model_Space / *Paper_Space blocks
      const std::string h = "R" + hex_count(++auto_);
      const std::string nm = name == "*MODEL_SPACE" ? "*Model_Space" : "*Paper_Space";
      table("blockRecords")
          .push_back(make_object({{"handle", h},
                                  {"name", nm},
                                  {"flags", 0},
                                  {"layout", ""},
                                  {"insertUnits", 0},
                                  {"explodable", true},
                                  {"scalable", 0},
                                  {"base", base::pt3(0, 0, 0)}}));
      blocks.push_back(make_object({{"name", nm},
                                    {"handle", h},
                                    {"record", h},
                                    {"base", base::pt3(0, 0, 0)},
                                    {"flags", 0},
                                    {"xrefPath", ""},
                                    {"description", ""},
                                    {"entities", Array{}}}));
      return blocks.size() - 1;
    };
    // blocks by record (a JS Map built before loose entities are placed: later blocks win on a shared record)
    std::unordered_map<std::string, std::size_t> by_record;
    for (std::size_t k = 0; k < blocks.size(); ++k) {
      by_record[blocks[k]["record"].as_string()] = k;
    }
    auto link = [&](Value& e) {
      const auto it =
          rec_by_name_.find(base::to_upper(e["block"].is_string() ? e["block"].as_string() : std::string()));
      if (it != rec_by_name_.end()) {
        e.set("blockRecord", table("blockRecords")[it->second]["handle"]);
      }
    };
    auto linkable = [](const Value& e) {
      const Value& tv = e["type"];
      if (!tv.is_string()) {
        return false;
      }
      const std::string& t = tv.as_string();
      return t == "INSERT" || t == "MINSERT" || t == "ACAD_TABLE";
    };
    for (Value& e : loose_) {
      if (linkable(e)) {
        link(e);
      }
    }
    for (Value& b : blocks) {
      for (Value& e : b.find("entities")->as_array()) {
        if (linkable(e)) {
          link(e);
        }
      }
    }
    for (Value& e : loose_) {
      const bool paper = e["_paper"].truthy();
      e.erase("_paper");
      std::size_t bi = 0;
      const std::string owner = e["owner"].is_string() ? e["owner"].as_string() : std::string();
      const auto it = owner.empty() ? by_record.end() : by_record.find(owner);
      if (it != by_record.end()) {
        bi = it->second;
      } else {
        bi = space(paper ? "*PAPER_SPACE" : "*MODEL_SPACE");
      }
      Value& b = blocks[bi];
      e.set("owner", b["record"]);
      b.find("entities")->push(std::move(e));
    }
    // layouts: the LAYOUT objects, else Model (+ Layout1 when there is paper space)
    Array& layouts = model_.find("layouts")->as_array();
    for (const Value& o : model_.find("objects")->as_array()) {
      if (o["type"].as_string() != "LAYOUT") {
        continue;
      }
      const Value& p = o["plot"];
      layouts.push_back(make_object({{"handle", o["handle"]},
                                     {"owner", o["owner"]},
                                     {"name", o["name"]},
                                     {"tabOrder", o["tabOrder"]},
                                     {"flags", o["flags"]},
                                     {"block", o["block"]},
                                     {"paper", make_object({{"name", p["paperSize"]},
                                                            {"w", p["paperWidth"]},
                                                            {"h", p["paperHeight"]},
                                                            {"units", p["paperUnits"]},
                                                            {"rotation", p["rotation"]},
                                                            {"pageSetupName", p["pageSetupName"]},
                                                            {"printer", p["printer"]}})},
                                     {"limits", make_object({{"min", o["limMin"]}, {"max", o["limMax"]}})},
                                     {"insbase", o["insBase"]},
                                     {"extents", make_object({{"min", o["extMin"]}, {"max", o["extMax"]}})},
                                     {"viewports", Array{}}}));
    }
    if (layouts.empty()) {
      const Value& H = model_["header"];
      const std::size_t ms = space("*MODEL_SPACE");
      auto hp = [&](std::string_view k, const Value& def) { return H[k].truthy() ? H[k] : def; };
      layouts.push_back(make_object(
          {{"handle", "L1"},
           {"name", "Model"},
           {"tabOrder", 0},
           {"flags", 1},
           {"block", blocks[ms]["record"]},
           {"limits", make_object({{"min", hp("LIMMIN", base::pt2(0, 0))}, {"max", hp("LIMMAX", base::pt2(12, 9))}})},
           {"viewports", Array{}}}));
      for (const Value& b : blocks) {
        if (base::to_upper(b["name"].as_string()) == "*PAPER_SPACE") {
          if (!b["entities"].as_array().empty()) {
            layouts.push_back(make_object({{"handle", "L2"},
                                           {"name", "Layout1"},
                                           {"tabOrder", 1},
                                           {"flags", 0},
                                           {"block", b["record"]},
                                           {"limits", make_object({{"min", hp("PLIMMIN", base::pt2(0, 0))},
                                                                   {"max", hp("PLIMMAX", base::pt2(0, 0))}})},
                                           {"viewports", Array{}}}));
          }
          break;
        }
      }
    }
    for (const Value& l : layouts) {
      for (Value& r : table("blockRecords")) {
        if (r["handle"] == l["block"]) {
          if (!r["layout"].truthy()) {
            r.set("layout", l["handle"]);
          }
          break;
        }
      }
    }
    (void)space("*MODEL_SPACE");
    Value& rep = *model_.find("report");
    for (Value& b : blocks) {
      const std::string up = base::to_upper(b["name"].as_string());
      std::string sp;
      if (up == "*MODEL_SPACE") {
        sp = "model";
      } else if (up.starts_with("*PAPER_SPACE")) {
        sp = "paper";
      }
      for (Value& e : b.find("entities")->as_array()) {
        if (!sp.empty()) {
          e.set("space", sp);
        }
        const std::string t = str(&e["type"], "undefined");  // counts[e.type]
        Value* cnt = rep.find("counts")->find(t);
        if (cnt == nullptr) {
          rep.find("counts")->set(t, 1);
        } else {
          *cnt = cnt->num_or(0) + 1;
        }
        if (t == "UNKNOWN") {
          const std::string dn = str(&e["dxfName"], "undefined");
          Value* u = rep.find("unknown")->find(dn);
          if (u == nullptr) {
            rep.find("unknown")->set(dn, 1);
          } else {
            *u = u->num_or(0) + 1;
          }
        }
      }
    }
  }

  std::vector<int> codes_;
  std::vector<Value> vals_;
  std::size_t n_ = 0;
  std::size_t i_ = 0;
  int auto_ = 0;
  Value model_;
  Array loose_;
  std::unordered_map<std::string, std::size_t> rec_by_name_;
};

}  // namespace

base::Result<Value> read_dxf_model(std::string_view text) {
  if (text.starts_with("AutoCAD Binary DXF")) {
    return base::fail(base::Errc::unsupported, "This is a binary DXF. Save it as ASCII DXF and try again.");
  }
  // text.split(/\r?\n/)
  std::vector<std::string_view> lines;
  lines.reserve(static_cast<std::size_t>(std::ranges::count(text, '\n')) +
                1);  // grown by doubling it was half the bytes
  std::size_t s = 0;
  for (std::size_t k = 0; k < text.size(); ++k) {
    if (text[k] == '\n') {
      std::size_t e = k;
      if (e > s && text[e - 1] == '\r') {
        --e;
      }
      lines.push_back(text.substr(s, e - s));
      s = k + 1;
    }
  }
  lines.push_back(text.substr(s));
  std::vector<int> codes;
  std::vector<Value> vals;
  codes.reserve(lines.size() / 2);
  vals.reserve(lines.size() / 2);
  for (std::size_t i = 0; i + 1 < lines.size(); i += 2) {
    const double c = js_parse_int(lines[i]);
    if (std::isnan(c)) {
      if (js_trim(lines[i]).empty() && i + 2 >= lines.size()) {
        break;
      }
      std::string line;
      base::append_number(line, static_cast<double>(i + 1));
      return base::fail(base::Errc::invalid, "This file is not a readable DXF (bad group code on line " + line + ").");
    }
    const auto code = static_cast<int>(std::clamp(c, -2147483648.0, 2147483647.0));
    const std::string_view raw = lines[i + 1];
    codes.push_back(code);
    const bool keep = code == 1 || code == 3 || (code >= 1000 && code <= 1009);
    vals.push_back(typed(code, keep ? raw : js_trim(raw)));
  }
  try {
    Reader r(std::move(codes), std::move(vals));
    return r.run();
  } catch (const std::exception& e) {
    return base::fail(base::Errc::invalid, e.what());
  }
}

namespace {
/// Valid UTF-8 (the WHATWG decoder's rules: no overlongs, no surrogates, nothing past U+10FFFF), and no U+FFFD in it.
bool clean_utf8(std::span<const std::byte> bytes) {
  std::size_t i = 0;
  const std::size_t n = bytes.size();
  auto b = [&](std::size_t k) { return std::to_integer<unsigned>(bytes[k]); };
  while (i < n) {
    const unsigned c = b(i);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t need = 0;
    unsigned lo = 0x80;
    unsigned hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      lo = c == 0xE0 ? 0xA0 : 0x80;
      hi = c == 0xED ? 0x9F : 0xBF;
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      lo = c == 0xF0 ? 0x90 : 0x80;
      hi = c == 0xF4 ? 0x8F : 0xBF;
    } else {
      return false;
    }
    if (i + need >= n) {
      return false;
    }
    if (b(i + 1) < lo || b(i + 1) > hi) {
      return false;
    }
    for (std::size_t k = 2; k <= need; ++k) {
      if (b(i + k) < 0x80 || b(i + k) > 0xBF) {
        return false;
      }
    }
    if (c == 0xEF && b(i + 1) == 0xBF && b(i + 2) == 0xBD) {
      return false;  // U+FFFD: BimCraft re-reads such text as Windows-1252
    }
    i += need + 1;
  }
  return true;
}
}  // namespace

std::string decode_text(std::span<const std::byte> bytes) {
  {
    std::span<const std::byte> body = bytes;
    if (body.size() >= 3 && std::to_integer<unsigned>(body[0]) == 0xEF && std::to_integer<unsigned>(body[1]) == 0xBB &&
        std::to_integer<unsigned>(body[2]) == 0xBF) {
      body = body.subspan(3);
    }
    if (clean_utf8(body)) {
      return std::string(base::as_text(body));
    }
  }
  // TextDecoder('utf-8'): a leading BOM dropped, invalid sequences → U+FFFD (the WHATWG maximal-subpart rule).
  std::string out;
  out.reserve(bytes.size());
  bool bad = false;
  std::size_t i = 0;
  auto b = [&](std::size_t k) { return std::to_integer<unsigned>(bytes[k]); };
  if (bytes.size() >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF) {
    i = 3;
  }
  while (i < bytes.size()) {
    const unsigned c = b(i);
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    std::size_t need = 0;
    unsigned lo = 0x80;
    unsigned hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
      need = 2;
      if (c == 0xE0) {
        lo = 0xA0;
      } else if (c == 0xED) {
        hi = 0x9F;
      }
    } else if (c >= 0xF0 && c <= 0xF4) {
      need = 3;
      if (c == 0xF0) {
        lo = 0x90;
      } else if (c == 0xF4) {
        hi = 0x8F;
      }
    } else {
      out += "\xEF\xBF\xBD";
      bad = true;
      ++i;
      continue;
    }
    std::size_t k = 1;
    bool ok = true;
    for (; k <= need; ++k) {
      if (i + k >= bytes.size()) {
        ok = false;
        break;
      }
      const unsigned x = b(i + k);
      const unsigned l = k == 1 ? lo : 0x80;
      const unsigned h = k == 1 ? hi : 0xBF;
      if (x < l || x > h) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      out += "\xEF\xBF\xBD";
      bad = true;
      i += k;  // the maximal subpart is replaced by one U+FFFD
      continue;
    }
    for (std::size_t q = 0; q <= need; ++q) {
      out += static_cast<char>(b(i + q));
    }
    i += need + 1;
  }
  // `text.includes('\ufffd')`: a replacement character the file itself holds counts too.
  if (!bad && !out.contains("\xEF\xBF\xBD")) {
    return out;
  }
  // BimCraft: text with a replacement character is read again as Windows-1252.
  static constexpr std::array<std::uint16_t, 32> kHigh = {
      0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
  std::string w;
  w.reserve(bytes.size());
  for (const std::byte x : bytes) {
    const auto c = std::to_integer<unsigned>(x);
    base::append_code_point(w, c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80U] : c);
  }
  return w;
}

}  // namespace cc::dxf
