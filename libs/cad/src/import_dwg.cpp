// SPDX-License-Identifier: Apache-2.0
// dwg-import.js (CadCraft 0.5) in C++: the reader's model → the drawing. The order of the work is the JS order — it
// decides the names of the unnamed blocks made here (*L, *I) and the report's counts.
#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/js.hpp"
#include "cc/base/js_math.hpp"
#include "cc/cad/import.hpp"
#include "parts.hpp"
#include "props.hpp"
#include "util.hpp"

namespace cc::cad {

namespace {

using namespace detail;  // NOLINT(google-build-using-namespace) — the port reads like the JS it mirrors

/// A JS Map key for a value (SameValueZero on the kinds handles come in: strings and numbers).
std::string vkey(const Value& v) {
  if (v.is_number()) {
    return "n" + str(v);
  }
  if (v.is_string()) {
    return "s" + v.as_string();
  }
  return "o" + str(v);
}

/// x ? -x : x for a value (JS `f ? -x : x`; undefined stays undefined when not flipped)
Value flip_x(bool f, const Value& x) {
  return f ? Value(-num(x)) : x;
}

/// ['a', 'b', 'c'][i] — undefined unless i is 0, 1 or 2
Value pick3(double i, const char* a, const char* b, const char* c) {
  if (i == 0) {
    return a;
  }
  if (i == 1) {
    return b;
  }
  if (i == 2) {
    return c;
  }
  return {};
}

/// JS `x % 3` on a number
double js_mod(double a, double b) {
  return std::fmod(a, b);
}

double dist(const Value& a, const Value& b) {
  return std::hypot(num(g(b, "x")) - num(g(a, "x")), num(g(b, "y")) - num(g(a, "y")));
}

/// P(p): a finite 2D point {x, y}, or null
Value point2(const Value& p) {
  if (p.truthy() && finite(g(p, "x")) && finite(g(p, "y"))) {
    return base::pt2(g(p, "x").as_number(), g(p, "y").as_number());
  }
  return {nullptr};
}

/// /^\*active$/i
bool is_active(const Value& name) {
  const std::string s = name.truthy() ? str(name) : std::string();
  return s.size() == 7 && s[0] == '*' && base::js_to_lower(s.substr(1)) == "active";
}

class Importer {
 public:
  Importer(const Value& db, std::string_view version_tag, const i18n::Catalog& words, const ImportLimits& limits)
      : db_(&db), tag_(version_tag), words_(&words), limits_(limits) {}

  Value run();

 private:
  const Value* db_;
  std::string tag_;
  const i18n::Catalog* words_;
  ImportLimits limits_;

  Value report_ = base::make_object(
      {{"read", 0}, {"skipped", Value::object()}, {"paperSpace", 0}, {"notes", Array{}}, {"splines", 0}});
  std::unordered_map<std::string, Value> lt_by_handle_;
  std::unordered_map<std::string, Value> layer_by_handle_;
  std::unordered_map<std::string, Value> vp_overrides_;  // viewport handle (upper case) → { layer: { … } }
  Value dim_style_;
  std::unordered_map<std::string, Value> dim_styles_;  // upper-case name → style
  const Array* block_list_ = nullptr;
  Array blocks_;
  int anon_n_ = 0;
  std::unordered_set<std::string> taken_;
  std::unordered_map<std::string, const Value*> image_defs_;
  std::unordered_map<std::string, const Value*> obj_by_h_;

  // ---- small pieces
  void skip(const Value& t) {
    Value& sk = *report_.find("skipped");
    const std::string k = str(t);
    const Value* cur = sk.find(k);
    sk.set(k, (cur != nullptr && cur->truthy() ? num(*cur) : 0) + 1);
  }
  void bump(std::string_view k) {
    const Value cur = report_.get(k);
    report_.set(k, (cur.truthy() ? num(cur) : 0) + 1);
  }
  [[nodiscard]] Value lt_name(const Value& v) const {
    if (v.is_undefined() || v.is_null() || (v.is_string() && v.as_string().empty())) {
      return {nullptr};
    }
    const auto it = lt_by_handle_.find(base::js_to_upper(str(v)));
    if (it != lt_by_handle_.end() && it->second.truthy()) {
      return it->second;
    }
    return str(v);
  }
  static Value lw_mm(const Value& v) {
    if (v.is_number() && (v.as_number() == -2 || v.as_number() == -3)) {
      return v;
    }
    if (finite(v) && v.as_number() >= 0) {
      return v.as_number() / 100;
    }
    return {};
  }
  std::string free_name(const std::string& pre) {
    std::string n;
    do {
      n = pre + std::to_string(++anon_n_);
    } while (taken_.contains(base::js_to_upper(n)));
    taken_.insert(base::js_to_upper(n));
    return n;
  }
  static Value color(const Value& e) {
    const Value c = or_(g(e, "color"), Value::object());
    const Value rgb = g(c, "rgb");
    if (!rgb.is_undefined()) {
      return hex_color(num(rgb));
    }
    Value i = g(c, "index");
    if (i.is_number() && i.as_number() == 0) {
      return 0;
    }
    if (num(i) >= 1 && num(i) <= 255 && !i.is_undefined() && !i.is_null()) {
      return i;
    }
    return {nullptr};
  }
  [[nodiscard]] Value style_of(const Value& e) const {
    Value o = Value::object();
    const Value n = lt_name(g(e, "linetype"));
    if (n.truthy()) {
      const std::string ns = str(n);
      const std::string low = base::js_to_lower(ns);
      if (low != "bylayer") {
        Value lt = n;
        if (low == "byblock") {
          lt = "ByBlock";
        } else if (low == "continuous") {
          lt = "Continuous";
        }
        o.set("lt", std::move(lt));
      }
    }
    if (const Value w = lw_mm(g(e, "lineweight")); !w.is_undefined()) {
      o.set("lw", w);
    }
    if (const Value s = g(e, "ltscale"); finite(s) && s.as_number() > 0 && std::abs(s.as_number() - 1) > 1e-12) {
      o.set("lts", s);
    }
    return o;
  }
  static Value style_name(const Value& n) {
    if (n.is_string() && !n.as_string().empty() && !is_standard(n.as_string())) {
      return base::make_object({{"style", n}});
    }
    return Value::object();
  }
  static void spread(Value& into, const Value& from) {  // Object.assign(into, from) / {...into, ...from}
    if (!from.is_object()) {
      return;
    }
    for (const base::Member& m : from.as_object()) {
      into.set(m.key, m.value);
    }
  }
  static Value with_type(std::string_view type, const Value& base) {  // { type, ...base }
    Value o = base::make_object({{"type", type}});
    spread(o, base);
    return o;
  }

  [[nodiscard]] Value text_of(const Value& e, const Value& base, bool f) const;
  [[nodiscard]] Value attdef_of(const Value& e) const;
  [[nodiscard]] Value dimension(const Value& e) const;
  Value multileader(const Value& e, const Value& base);
  Value picture(const Value& e, const Value& base);
  Value convert_base(const Value& e);
  Value convert(const Value& e);
  void with_props(Value& c, const Value& e) const;
  static Value mask_of(const Value& e);
  [[nodiscard]] Value style_from(const Value& src, const Value& base) const;
  void layer_overrides();
};

// ---------------------------------------------------------------- styles

void lunit_ok(Value& st) {
  const Value* lu = st.find("lunit");
  if (lu == nullptr || lu->is_undefined()) {
    return;
  }
  const double v = base::js_round(num(*lu));
  if (v >= 1 && v <= 5) {
    st.set("lunit", v);
  } else if (v == 6) {
    st.set("lunit", 2);
  } else {
    st.erase("lunit");
  }
}

Value Importer::style_from(const Value& src, const Value& base) const {
  Value st = base;
  st_from_vars(src, st);
  if (!(num(st.get("scale")) > 0)) {
    st.set("scale", 1);
  }
  for (const char* k : {"dec", "adec", "tad", "zin"}) {
    const Value v = st.get(k);
    st.set(k, js_max(0, js_min(8, base::js_round(v.truthy() ? num(v) : 0))));
  }
  lunit_ok(st);
  return st;
}

// ---------------------------------------------------------------- texts

Value Importer::text_of(const Value& e, const Value& base, bool f) const {
  const Value height = g(e, "height");
  const Value insert = g(e, "insert");
  if (!height.truthy() || !insert.truthy()) {
    return {nullptr};
  }
  const Value value = g(e, "value");
  const std::string s = plain(value.truthy() ? str(value) : std::string());
  // const h = e.hAlign || 0, v = e.vAlign || 0 — compared strictly below (h === 1 …)
  const Value hv = or_(g(e, "hAlign"), 0);
  const Value vv = or_(g(e, "vAlign"), 0);
  const double h = hv.is_number() ? hv.as_number() : kNaN;
  const double v = vv.is_number() ? vv.as_number() : kNaN;
  const bool aligned = hv != Value(0) || vv != Value(0);
  const Value align = g(e, "align");
  const bool use_align = aligned && hv != Value(3) && hv != Value(5) && align.truthy() &&
                         (g(align, "x").truthy() || g(align, "y").truthy());
  const Value p = use_align ? align : insert;
  Value t = with_type("text", base);
  t.set("x", flip_x(f, g(p, "x")));
  t.set("y", g(p, "y"));
  t.set("h", height);
  t.set("rot", norm_angle(num(or_(g(e, "rotation"), 0))));
  t.set("wf", or_(g(e, "widthFactor"), 1));
  t.set("s", s);
  const char* ha = "left";
  if (h == 1 || h == 4) {
    ha = "center";
  } else if (h == 2) {
    ha = "right";
  }
  const char* va = "baseline";
  if (h == 4 || v == 2) {
    va = "middle";
  } else if (v == 3) {
    va = "top";
  } else if (v == 1) {
    va = "bottom";
  }
  t.set("ha", ha);
  t.set("va", va);
  spread(t, style_name(g(e, "style")));
  if (const Value obl = g(e, "oblique"); finite(obl) && std::abs(obl.as_number()) > 1e-12) {
    t.set("obl", obl);
  }
  return t;
}

Value Importer::attdef_of(const Value& e) const {
  if (!g(e, "tag").truthy()) {
    return {nullptr};
  }
  const Value ext = g(e, "extrusion");
  const bool f = ext.truthy() && num(g(ext, "z")) < -0.5;
  const Value t = text_of(e, base::make_object({{"layer", or_(g(e, "layer"), "0")}, {"color", color(e)}}), f);
  if (!t.truthy()) {
    return {nullptr};
  }
  const auto fl = base::js_to_int32(num(or_(g(e, "flags"), 0)));
  const Value prompt = g(e, "prompt");
  AttdefFlags flags{.tag = g(e, "tag"),
                    .prompt = plain(prompt.truthy() ? str(prompt) : std::string()),
                    .def = str(g(t, "s")),
                    .invisible = (fl & 1) != 0,
                    .constant = (fl & 2) != 0,
                    .preset = (fl & 8) != 0};
  return attdef(t, flags);
}

Value Importer::mask_of(const Value& e) {
  const auto f = base::js_to_int32(num(g(e, "backgroundFlags")));
  if ((f & 1) == 0) {
    return Value::object();
  }
  const Value c = or_(g(e, "backgroundColor"), Value::object());
  Value col;
  if ((f & 2) != 0) {
    col = "bg";
  } else if (!g(c, "rgb").is_undefined()) {
    col = hex_color(num(g(c, "rgb")));
  } else {
    const double i = std::abs(num(nn(g(c, "index"), 7)));
    col = i != 0 && !std::isnan(i) ? i : 7.0;
  }
  const double sc = num(g(e, "backgroundScale"));
  return base::make_object(
      {{"bgm", base::make_object({{"color", col}, {"scale", std::isfinite(sc) && sc >= 1 && sc <= 5 ? sc : 1.5}})}});
}

// ---------------------------------------------------------------- dimensions

Value Importer::dimension(const Value& e) const {
  const Value tv0 = nn(g(e, "dimType"), 0);
  const double t = tv0.is_number() ? tv0.as_number() : kNaN;  // compared strictly (t === 0 …)
  const Value style = g(e, "style");
  const std::string up = base::js_to_upper(style.truthy() ? str(style) : std::string());
  const auto si = dim_styles_.find(up);
  Value st = si != dim_styles_.end() && si->second.truthy() ? si->second : dim_style_;
  const auto ds = dstyle_map(xdata_of(g(e, "xdata"), "ACAD"));
  if (!ds.empty()) {
    st_from_codes(
        [&](double c) -> Value {
          for (const auto& [k, v] : ds) {
            if (k == c) {
              return v;
            }
          }
          return {};
        },
        st);
  }
  cc_dim_from(cadcraft_strings(g(e, "xdata")), st);
  if (!(num(st.get("scale")) > 0)) {
    st.set("scale", 1);
  }
  lunit_ok(st);
  const Value mv = g(e, "measurement");
  const bool has_meas = mv.is_number() && mv.as_number() > 0;
  const double meas = has_meas ? mv.as_number() : 0;
  const auto close = [&](double v) { return !has_meas || std::abs(v - meas) <= 1e-6 * js_max(1, meas); };
  const Value tv = g(e, "text");
  std::string text = mtext_plain(tv.truthy() ? str(tv) : std::string());
  if (text == "<>") {
    text.clear();
  }
  const Value tp = point2(g(e, "textMid"));
  const Value text_at = tp.truthy() && (g(tp, "x").truthy() || g(tp, "y").truthy()) ? tp : Value(nullptr);
  Value d(nullptr);
  if (t == 0 || t == 1) {
    const Value p1 = point2(g(e, "xline1"));
    const Value p2 = point2(g(e, "xline2"));
    const Value q2 = point2(g(e, "defPoint"));
    if (p1.is_null() || p2.is_null() || q2.is_null() || dist(p1, p2) < 1e-12) {
      return {nullptr};
    }
    if (t == 1) {
      if (close(dist(p1, p2))) {
        d = base::make_object({{"kind", "aligned"}, {"p1", p1}, {"p2", p2}, {"loc", q2}});
      }
    } else {
      std::vector<double> cands;
      if (dist(p2, q2) > 1e-9) {
        cands.push_back(
            norm_angle(base::js_atan2(num(g(q2, "y")) - num(g(p2, "y")), num(g(q2, "x")) - num(g(p2, "x"))) +
                       std::numbers::pi / 2));
      }
      cands.push_back(norm_angle(num(or_(g(e, "angle"), 0))));
      cands.push_back(0);
      cands.push_back(std::numbers::pi / 2);
      const double dx = num(g(p2, "x")) - num(g(p1, "x"));
      const double dy = num(g(p2, "y")) - num(g(p1, "y"));
      for (const double r : cands) {
        if (close(std::abs(dx * std::cos(r) + dy * std::sin(r)))) {
          d = base::make_object({{"kind", "linear"}, {"p1", p1}, {"p2", p2}, {"loc", q2}, {"rot", r}});
          break;
        }
      }
    }
  } else if (t == 4 || t == 3) {
    const Value a = point2(g(e, "defPoint"));
    const Value p1 = point2(g(e, "center"));
    if (a.is_null() || p1.is_null()) {
      return {nullptr};
    }
    const Value c =
        t == 4 ? a : base::pt2((num(g(a, "x")) + num(g(p1, "x"))) / 2, (num(g(a, "y")) + num(g(p1, "y"))) / 2);
    const double r = dist(c, p1);
    if (r < 1e-12 || !close(t == 4 ? r : 2 * r)) {
      return {nullptr};
    }
    d = base::make_object(
        {{"kind", t == 4 ? "radius" : "diameter"}, {"c", c}, {"p1", p1}, {"loc", text_at.truthy() ? text_at : p1}});
  } else if (t == 5) {
    const Value c = point2(g(e, "center"));
    const Value p1 = point2(g(e, "xline1"));
    const Value p2 = point2(g(e, "xline2"));
    const Value loc = point2(g(e, "defPoint"));
    if (!c.is_null() && !p1.is_null() && !p2.is_null() && !loc.is_null() && dist(c, p1) > 1e-12 &&
        dist(c, p2) > 1e-12 && dist(c, loc) > 1e-12) {
      d = base::make_object({{"kind", "angular"}, {"c", c}, {"p1", p1}, {"p2", p2}, {"loc", loc}});
    }
  }
  if (!d.truthy()) {
    return {nullptr};
  }
  Value out = d;
  out.set("text", text);
  out.set("st", std::move(st));
  if (style.truthy() && !is_standard(str(style))) {
    out.set("dimstyle", str(style));
  }
  if (const Value tr = g(e, "textRotation"); finite(tr) && std::abs(tr.as_number()) > 1e-12) {
    out.set("trot", norm_angle(tr.as_number()));
  }
  return out;
}

// ---------------------------------------------------------------- multileaders and pictures

Value Importer::multileader(const Value& e, const Value& base) {
  const Value cx = or_(g(e, "context"), Value::object());
  const Value roots = or_(g(cx, "roots"), Array{});
  const auto P = [](const Value& p) { return base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}}); };
  const Value ctext = g(cx, "text");
  const std::string label = g(cx, "hasText").truthy() && ctext.truthy() && g(ctext, "label").truthy()
                                ? mtext_plain(str(g(ctext, "label")))
                                : std::string();
  const Value asz = nn(g(cx, "arrowSize"), nn(g(e, "arrowSize"), 0));
  const Value th = g(cx, "textHeight");
  const Value h = num(th) > 0 ? th : or_(asz, 2.5);
  const Value& r0 = item(roots, 0);
  const Value r0lines = r0.truthy() ? g(r0, "lines") : Value();
  const Value l0 = item(r0lines, 0);
  const Value l0pts = l0.truthy() ? g(l0, "points") : Value();
  if (len(roots) == 1 && len(r0lines) == 1 && !g(cx, "hasBlock").truthy() && !label.empty() &&
      g(e, "leaderType") != Value(2) && g(r0, "connection").truthy() && len(l0pts) != 0) {
    Array pts;
    for (const Value& p : l0pts.as_array()) {
      pts.push_back(P(p));
    }
    pts.push_back(P(g(r0, "connection")));
    const Value dirv = g(r0, "direction");
    const double dir = num(nn(dirv.truthy() ? g(dirv, "x") : Value(), 1)) < 0 ? -1 : 1;
    Value dogleg = 0;
    if (g(e, "doglegEnabled") != Value(false)) {
      dogleg = nn(g(r0, "landingDistance"), nn(g(r0, "doglegLength"), nn(g(e, "landingDistance"), 0)));
    }
    const Value ww = num(g(ctext, "width")) > 0 ? g(ctext, "width") : Value(0);
    const Value lsv = g(ctext, "lineSpacing");
    const Value ls = num(lsv) > 0 && lsv != Value(1) ? lsv : Value(0);
    bump("mleaders");
    Value out = with_type("mleader", base);
    out.set("pts", std::move(pts));
    out.set("dir", dir);
    out.set("s", label);
    out.set("h", h);
    if (ww.truthy()) {
      out.set("ww", ww);
    }
    if (ls.truthy()) {
      out.set("ls", ls);
    }
    Value stv = base::make_object({{"asz", asz}, {"dogleg", dogleg}});
    if (const Value gap = g(cx, "landingGap"); finite(gap)) {
      stv.set("gap", gap);
    }
    out.set("st", std::move(stv));
    return out;
  }
  Array parts;
  const double aszn = num(asz);
  if (roots.is_array()) {
    for (const Value& r : roots.as_array()) {
      const Value lines = g(r, "lines");
      const Value conn = g(r, "connection");
      if (lines.is_array()) {
        for (const Value& l : lines.as_array()) {
          Array pts;
          if (const Value lp = g(l, "points"); lp.is_array()) {
            for (const Value& p : lp.as_array()) {
              pts.push_back(P(p));
            }
          }
          if (conn.truthy()) {
            pts.push_back(P(conn));
          }
          if (pts.size() < 2) {
            continue;
          }
          if (g(e, "leaderType") != Value(0)) {
            Array bp;
            for (const Value& p : pts) {
              Value q = p;
              q.set("b", 0);
              bp.push_back(std::move(q));
            }
            parts.push_back(base::make_object(
                {{"type", "poly"}, {"layer", "0"}, {"color", 0}, {"closed", false}, {"pts", std::move(bp)}}));
          }
          const Value& a = pts[0];
          const Value& b = pts[1];
          const double d = dist(a, b);
          if (aszn > 0 && d > 0) {
            const double ax = num(g(a, "x"));
            const double ay = num(g(a, "y"));
            const double ux = (num(g(b, "x")) - ax) / d;
            const double uy = (num(g(b, "y")) - ay) / d;
            const double bx = ax + ux * aszn;
            const double by = ay + uy * aszn;
            const double hw = aszn / 6;
            parts.push_back(base::make_object(
                {{"type", "hatch"},
                 {"layer", "0"},
                 {"color", 0},
                 {"solid", true},
                 {"loops", Array{Array{ax, ay, bx - uy * hw, by + ux * hw, bx + uy * hw, by - ux * hw, ax, ay}}}}));
          }
        }
      }
      const Value dl = nn(g(r, "landingDistance"), nn(g(r, "doglegLength"), g(e, "landingDistance")));
      const Value rdir = g(r, "direction");
      if (conn.truthy() && g(e, "doglegEnabled").truthy() && rdir.truthy() && dl.truthy()) {
        const double cxn = num(g(conn, "x"));
        const double cyn = num(g(conn, "y"));
        const double dln = num(dl);
        parts.push_back(base::make_object({{"type", "line"},
                                           {"layer", "0"},
                                           {"color", 0},
                                           {"x1", g(conn, "x")},
                                           {"y1", g(conn, "y")},
                                           {"x2", cxn + num(g(rdir, "x")) * dln},
                                           {"y2", cyn + num(g(rdir, "y")) * dln}}));
      }
    }
  }
  if (!label.empty() && ctext.truthy() && g(ctext, "location").truthy()) {
    const Value loc = g(ctext, "location");
    const double ap = js_max(0, num(or_(g(ctext, "alignment"), 1)) - 1);
    const Value tdir = g(ctext, "direction");
    const double rot = tdir.truthy() && (g(tdir, "x").truthy() || g(tdir, "y").truthy())
                           ? base::js_atan2(num(g(tdir, "y")), num(g(tdir, "x")))
                           : 0;
    Value t = base::make_object({{"type", "text"},
                                 {"layer", "0"},
                                 {"color", 0},
                                 {"x", g(loc, "x")},
                                 {"y", g(loc, "y")},
                                 {"h", h},
                                 {"rot", norm_angle(rot)},
                                 {"s", label},
                                 {"ha", pick3(js_mod(ap, 3), "left", "center", "right")},
                                 {"va", "top"}});
    if (num(g(ctext, "width")) > 0) {
      t.set("ww", g(ctext, "width"));
    }
    parts.push_back(std::move(t));
  }
  const Value bc = g(cx, "hasBlock").truthy() ? g(cx, "block") : Value(false);
  if (bc.truthy()) {
    const Value bh = nn(g(bc, "blockHandle"), g(bc, "handle"));
    const Value* src = nullptr;
    for (const Value& b : *block_list_) {
      if (g(b, "record") == bh) {
        src = &b;
        break;
      }
    }
    if (src == nullptr) {
      for (const Value& b : *block_list_) {
        if (g(b, "name") == g(bc, "name")) {
          src = &b;
          break;
        }
      }
    }
    if (src != nullptr) {
      std::vector<std::pair<Value, Value>> tags;  // new Map(…): a later ATTDEF of the same handle wins
      if (const Value& es = at(*src, "entities"); es.is_array()) {
        for (const Value& x : es.as_array()) {
          if (g(x, "type") == Value("ATTDEF")) {
            const Value hd = g(x, "handle");
            auto it = std::ranges::find_if(tags, [&](const auto& p) { return p.first == hd; });
            if (it != tags.end()) {
              it->second = g(x, "tag");
            } else {
              tags.emplace_back(hd, g(x, "tag"));
            }
          }
        }
      }
      Value attrs = Value::object();
      if (const Value bl = g(e, "blockLabels"); bl.is_array()) {
        for (const Value& a : bl.as_array()) {
          const Value ah = g(a, "attdefHandle");
          const auto it = std::ranges::find_if(tags, [&](const auto& p) { return p.first == ah; });
          if (it != tags.end()) {
            attrs.set(str(it->second), mtext_plain(str(nn(g(a, "text"), ""))));
          }
        }
      }
      const Value m = g(bc, "matrix");
      const Value sc = g(bc, "scale");
      const Value sx = or_(sc.truthy() ? g(sc, "x") : Value(), 1);
      const Value sy = or_(sc.truthy() ? g(sc, "y") : Value(), 1);
      const bool m16 = m.truthy() && len(m) == 16;
      const Value atp = m16 ? base::make_object({{"x", item(m, 3)}, {"y", item(m, 7)}}) : P(g(bc, "location"));
      const double rot = m16 ? base::js_atan2(num(item(m, 4)), num(item(m, 0))) : num(or_(g(bc, "rotation"), 0));
      Value ins = base::make_object({{"type", "insert"},
                                     {"layer", "0"},
                                     {"color", 0},
                                     {"block", g(*src, "name")},
                                     {"x", g(atp, "x")},
                                     {"y", g(atp, "y")},
                                     {"sx", sx},
                                     {"sy", sy},
                                     {"rot", norm_angle(rot)}});
      if (!attrs.as_object().empty()) {
        ins.set("attrs", std::move(attrs));
      }
      parts.push_back(std::move(ins));
    }
  }
  if (parts.empty()) {
    skip("MULTILEADER");
    return {nullptr};
  }
  const std::string name = free_name("*L");
  blocks_.push_back(base::make_object(
      {{"name", name}, {"base", base::make_object({{"x", 0}, {"y", 0}})}, {"entities", std::move(parts)}}));
  bump("mleaderBlocks");
  Value out = with_type("insert", base);
  out.set("block", name);
  for (const char* k : {"x", "y"}) {
    out.set(k, 0);
  }
  out.set("sx", 1);
  out.set("sy", 1);
  out.set("rot", 0);
  return out;
}

Value Importer::picture(const Value& e, const Value& base) {
  const Value u = g(e, "u");
  const Value v = g(e, "v");
  const Value p = g(e, "insert");
  const Value size = g(e, "size");
  const Value wv = size.truthy() ? g(size, "x") : Value();
  const Value hv = size.truthy() ? g(size, "y") : Value();
  if (!u.truthy() || !v.truthy() || !p.truthy() || !(num(wv) > 0) || !(num(hv) > 0)) {
    skip(g(e, "type"));
    return {nullptr};
  }
  const double w = num(wv);
  const double h = num(hv);
  const auto W = [&](const Value& q) {
    const double qx = num(g(q, "x"));
    const double qy = num(g(q, "y"));
    return base::make_object({{"x", num(g(p, "x")) + (qx + 0.5) * num(g(u, "x")) + (h - 0.5 - qy) * num(g(v, "x"))},
                              {"y", num(g(p, "y")) + (qx + 0.5) * num(g(u, "y")) + (h - 0.5 - qy) * num(g(v, "y"))},
                              {"b", 0}});
  };
  const Value clipv = g(e, "clip");
  const bool clipping = g(e, "clipping").truthy() && clipv.is_array() && clipv.as_array().size() >= 2;
  Array clip;
  if (clipping) {
    clip = clipv.as_array();
  } else {
    clip = {base::pt2(-0.5, -0.5), base::pt2(w - 0.5, h - 0.5)};
  }
  if (clip.size() == 2) {
    const Value a = clip[0];
    const Value b = clip[1];
    clip = {base::make_object({{"x", g(a, "x")}, {"y", g(a, "y")}}),
            base::make_object({{"x", g(b, "x")}, {"y", g(a, "y")}}),
            base::make_object({{"x", g(b, "x")}, {"y", g(b, "y")}}),
            base::make_object({{"x", g(a, "x")}, {"y", g(b, "y")}})};
  }
  Array pts;
  for (const Value& q : clip) {
    pts.push_back(W(q));
  }
  if (g(e, "type") == Value("WIPEOUT")) {
    bump("wipeouts");
    Value frame = base::make_object({{"type", "poly"}, {"closed", true}, {"pts", std::move(pts)}});
    spread(frame, base);
    return frame;
  }
  const Value pts_value = pts;
  std::string file;
  if (const auto it = image_defs_.find(vkey(g(e, "def"))); it != image_defs_.end()) {
    const Value fn = g(*it->second, "fileName");
    file = std::string(base::js_trim(fn.truthy() ? str(fn) : std::string()));
  }
  if (file.empty()) {
    file = "image";
  }
  Value pic = base::make_object({{"file", file},
                                 {"x", g(p, "x")},
                                 {"y", g(p, "y")},
                                 {"ux", g(u, "x")},
                                 {"uy", g(u, "y")},
                                 {"vx", g(v, "x")},
                                 {"vy", g(v, "y")},
                                 {"w", wv},
                                 {"h", hv}});
  if (num(g(e, "fade")) > 0) {
    pic.set("fade", g(e, "fade"));
  }
  const std::string name = free_name("*I");
  blocks_.push_back(picture_block(name, pic, clipping ? &pts_value : nullptr));
  bump("images");
  Value out = with_type("insert", base);
  out.set("block", name);
  out.set("x", 0);
  out.set("y", 0);
  out.set("sx", 1);
  out.set("sy", 1);
  out.set("rot", 0);
  return out;
}

// ---------------------------------------------------------------- the converter

/// splinePoints(e): fit points, else the control points, else the B-spline evaluated at up to 400 + 1 points.
Value spline_points(const Value& e) {
  Value fit = g(e, "fit");
  if (fit.truthy() && len(fit) > 1) {
    return fit;
  }
  Value cp = or_(g(e, "control"), Array{});
  const Value kv = or_(g(e, "knots"), Array{});
  const double p = num(or_(g(e, "degree"), 3));
  if (len(cp) < 2) {
    return Array{};
  }
  if (static_cast<double>(len(kv)) != static_cast<double>(len(cp)) + p + 1) {
    return cp;
  }
  const auto pi = static_cast<std::size_t>(p);
  const Array& c = cp.as_array();
  std::vector<double> k;
  for (const Value& x : kv.as_array()) {
    k.push_back(num(x));
  }
  const Value wv = g(e, "weights");
  const bool weighted = wv.truthy() && len(wv) == c.size();
  const double lo = k[pi];
  const double hi = k[k.size() - pi - 1];
  const double n = js_min(400, js_max(16, static_cast<double>(c.size()) * 8));
  Array out;
  const auto steps = static_cast<std::size_t>(n);  // n: a whole number of samples (16–400)
  for (std::size_t si = 0; si <= steps; ++si) {
    const auto s = static_cast<double>(si);
    const double uu = s == n ? hi - 1e-9 * (hi - lo) : lo + (hi - lo) * (s / n);
    std::size_t span = pi;
    while (span < c.size() - 1 && uu >= k[span + 1]) {
      ++span;
    }
    std::vector<std::array<double, 3>> d;
    for (std::size_t j = 0; j <= pi; ++j) {
      const Value& q = c[span - pi + j];
      const double wt = weighted ? num(wv.as_array()[span - pi + j]) : 1;
      d.push_back({num(g(q, "x")) * wt, num(g(q, "y")) * wt, wt});
    }
    for (std::size_t r = 1; r <= pi; ++r) {
      for (std::size_t j = pi; j >= r; --j) {
        const std::size_t i = span - pi + j;
        const double den = k[i + pi - r + 1] - k[i];
        const double a = den == 0 ? 0 : (uu - k[i]) / den;
        d[j] = {(1 - a) * d[j - 1][0] + a * d[j][0], (1 - a) * d[j - 1][1] + a * d[j][1],
                (1 - a) * d[j - 1][2] + a * d[j][2]};
        if (j == 0) {
          break;
        }
      }
    }
    out.push_back(base::pt2(d[pi][0] / d[pi][2], d[pi][1] / d[pi][2]));
  }
  return out;
}

void arc_between(Array& pts, const Value& p, const Value& q, double b) {
  const double theta = 4 * base::js_atan(b);
  const double px = num(g(p, "x"));
  const double py = num(g(p, "y"));
  const double qx = num(g(q, "x"));
  const double qy = num(g(q, "y"));
  const double c = std::hypot(qx - px, qy - py);
  if (c == 0 || std::isnan(c)) {
    return;
  }
  const double r = c / (2 * std::sin(theta / 2));
  const double mx = (px + qx) / 2;
  const double my = (py + qy) / 2;
  const double h = r * std::cos(theta / 2);
  const double ux = -(qy - py) / c;
  const double uy = (qx - px) / c;
  const double cx = mx + ux * h;
  const double cy = my + uy * h;
  const double a0 = base::js_atan2(py - cy, px - cx);
  const double n = js_max(2, std::ceil(16 * std::abs(theta) / std::numbers::pi));
  const auto segs = static_cast<std::size_t>(n);  // n = max(2, ceil(…)): a whole number
  for (std::size_t ki = 1; ki < segs; ++ki) {
    const auto k = static_cast<double>(ki);
    const double t = a0 + theta * k / n;
    pts.emplace_back(cx + std::abs(r) * std::cos(t));
    pts.emplace_back(cy + std::abs(r) * std::sin(t));
  }
}

Array hatch_loops(const Value& e) {
  Array rings;
  const Value loops = g(e, "loops");
  if (!loops.is_array()) {
    return rings;
  }
  for (const Value& path : loops.as_array()) {
    Array pts;
    const auto push2 = [&](const Value& x, const Value& y) {
      pts.push_back(x);
      pts.push_back(y);
    };
    if (const Value pl = g(path, "polyline"); pl.truthy()) {
      const Value vs = or_(g(pl, "vertices"), Array{});
      const std::size_t n = len(vs);
      for (std::size_t i = 0; i < n; ++i) {
        const Value& a = vs.as_array()[i];
        const Value& b = vs.as_array()[(i + 1) % n];
        push2(g(a, "x"), g(a, "y"));
        const double bulge = num(or_(g(a, "bulge"), 0));
        if (std::abs(bulge) > 1e-9) {
          arc_between(pts, a, b, bulge);
        }
      }
      if (n != 0) {
        push2(g(vs.as_array()[0], "x"), g(vs.as_array()[0], "y"));
      }
    } else if (const Value edges = g(path, "edges"); edges.is_array()) {
      for (const Value& ed : edges.as_array()) {
        const Value type = g(ed, "type");
        if (type == Value("line")) {
          const Value s = g(ed, "start");
          const Value t = g(ed, "end");
          push2(g(s, "x"), g(s, "y"));
          push2(g(t, "x"), g(t, "y"));
        } else if (type == Value("arc")) {
          double s = num(g(ed, "startAngle"));
          double t = num(g(ed, "endAngle"));
          const bool cw = g(ed, "ccw") == Value(false);
          if (cw) {
            s = -s;
            t = -t;
          }
          double sweep = cw ? s - t : t - s;
          while (sweep <= 0) {
            sweep += kTau;
          }
          const double dir = cw ? -1 : 1;
          const double n = js_max(4, std::ceil(32 * sweep / kTau));
          const Value c = g(ed, "center");
          const double cxn = num(g(c, "x"));
          const double cyn = num(g(c, "y"));
          const double r = num(g(ed, "radius"));
          for (std::size_t qi = 0; static_cast<double>(qi) <= n; ++qi) {  // n = max(4, ceil(…))
            const auto q = static_cast<double>(qi);
            const double a = s + dir * sweep * q / n;
            pts.emplace_back(cxn + r * std::cos(a));
            pts.emplace_back(cyn + r * std::sin(a));
          }
        } else if (type == Value("ellipse")) {
          const Value ma = g(ed, "majorAxis");
          const double mx = num(g(ma, "x"));
          const double my = num(g(ma, "y"));
          const double h = std::hypot(mx, my);
          const double lenv = h != 0 && !std::isnan(h) ? h : 1;
          const double ratio = num(or_(g(ed, "ratio"), 1));
          const double rot = base::js_atan2(my, mx);
          double sweep = num(g(ed, "endAngle")) - num(g(ed, "startAngle"));
          while (sweep <= 0) {
            sweep += kTau;
          }
          const double n = js_max(8, std::ceil(48 * sweep / kTau));
          const Value c = g(ed, "center");
          const double cxn = num(g(c, "x"));
          const double cyn = num(g(c, "y"));
          const double sa = num(g(ed, "startAngle"));
          for (std::size_t qi = 0; static_cast<double>(qi) <= n; ++qi) {  // n = max(8, ceil(…))
            const auto q = static_cast<double>(qi);
            const double t = sa + sweep * q / n;
            const double x = lenv * std::cos(t);
            const double y = lenv * ratio * std::sin(t);
            pts.emplace_back(cxn + x * std::cos(rot) - y * std::sin(rot));
            pts.emplace_back(cyn + x * std::sin(rot) + y * std::cos(rot));
          }
        } else if (type == Value("spline")) {
          Value sp = base::make_object({{"fit", g(ed, "fit")},
                                        {"control", g(ed, "control")},
                                        {"knots", g(ed, "knots")},
                                        {"degree", g(ed, "degree")},
                                        {"weights", g(ed, "weights")}});
          const Value ps = spline_points(sp);
          for (const Value& p : ps.as_array()) {
            push2(g(p, "x"), g(p, "y"));
          }
        }
      }
    }
    if (pts.size() >= 6) {
      rings.emplace_back(std::move(pts));
    }
  }
  return rings;
}

// NOLINTNEXTLINE(misc-no-recursion) — see convert()
Value Importer::convert_base(const Value& e) {
  if (g(e, "invisible").truthy()) {
    return {nullptr};
  }
  Value base = base::make_object({{"layer", or_(g(e, "layer"), "0")}, {"color", color(e)}});
  spread(base, style_of(e));
  const Value ext = g(e, "extrusion");
  const bool f = ext.truthy() && num(g(ext, "z")) < -0.5;
  const auto X = [&](const Value& x) { return flip_x(f, x); };
  const Value et = g(e, "type");
  const std::string type = et.is_string() && et.as_string().starts_with("DIMENSION_") ? "DIMENSION" : str(et);
  const auto ent = [&](std::string_view t) { return with_type(t, base); };
  if (type == "LINE") {
    Value o = ent("line");
    const Value s = g(e, "start");
    const Value t = g(e, "end");
    o.set("x1", g(s, "x"));
    o.set("y1", g(s, "y"));
    o.set("x2", g(t, "x"));
    o.set("y2", g(t, "y"));
    return o;
  }
  if (type == "POINT") {
    Value o = ent("point");
    const Value p = g(e, "position");
    o.set("x", nn(p.truthy() ? g(p, "x") : Value(), 0));
    o.set("y", nn(p.truthy() ? g(p, "y") : Value(), 0));
    return o;
  }
  if (type == "CIRCLE") {
    Value o = ent("circle");
    const Value c = g(e, "center");
    o.set("cx", X(g(c, "x")));
    o.set("cy", g(c, "y"));
    o.set("r", g(e, "radius"));
    return o;
  }
  if (type == "ARC") {
    double a0 = num(g(e, "startAngle"));
    double a1 = num(g(e, "endAngle"));
    if (f) {
      const double n0 = std::numbers::pi - a1;
      const double n1 = std::numbers::pi - a0;
      a0 = n0;
      a1 = n1;
    }
    Value o = ent("arc");
    const Value c = g(e, "center");
    o.set("cx", X(g(c, "x")));
    o.set("cy", g(c, "y"));
    o.set("r", g(e, "radius"));
    o.set("a0", norm_angle(a0));
    o.set("a1", norm_angle(a1));
    return o;
  }
  if (type == "LWPOLYLINE" || type == "POLYLINE_2D") {
    std::vector<const Value*> vs;
    const Value verts = g(e, "vertices");
    if (verts.is_array()) {
      for (const Value& v : verts.as_array()) {
        if ((base::js_to_int32(num(g(v, "flags"))) & 16) == 0) {
          vs.push_back(&v);
        }
      }
    }
    if (vs.size() < 2) {
      return {nullptr};
    }
    const Value cwv = g(e, "constantWidth");
    const Value cw = num(cwv) > 0 ? cwv : Value(0);
    bool each = false;
    if (!cw.truthy()) {
      for (const Value* v : vs) {
        if (num(g(*v, "startWidth")) > 0 || num(g(*v, "endWidth")) > 0) {
          each = true;
          break;
        }
      }
    }
    const auto wd = [](const Value& x) { return finite(x) && x.as_number() > 0 ? x : Value(0); };
    Value o = ent("poly");
    o.set("closed", g(e, "closed").truthy());
    if (cw.truthy()) {
      o.set("w", cw);
    }
    Array pts;
    for (const Value* v : vs) {
      Value p = base::make_object(
          {{"x", X(g(*v, "x"))}, {"y", g(*v, "y")}, {"b", (f ? -1 : 1) * num(or_(g(*v, "bulge"), 0))}});
      if (each) {
        p.set("sw", wd(g(*v, "startWidth")));
        p.set("ew", wd(g(*v, "endWidth")));
      }
      pts.push_back(std::move(p));
    }
    o.set("pts", std::move(pts));
    return o;
  }
  if (type == "POLYLINE_3D") {
    Array pts;
    if (const Value verts = g(e, "vertices"); verts.is_array()) {
      for (const Value& v : verts.as_array()) {
        if ((base::js_to_int32(num(g(v, "flags"))) & 16) == 0) {
          pts.push_back(base::make_object({{"x", g(v, "x")}, {"y", g(v, "y")}, {"b", 0}}));
        }
      }
    }
    if (pts.size() < 2) {
      return {nullptr};
    }
    Value o = ent("poly");
    o.set("closed", g(e, "closed").truthy());
    o.set("pts", std::move(pts));
    return o;
  }
  if (type == "ELLIPSE") {
    Value o = ent("ellipse");
    const Value c = g(e, "center");
    const Value m = g(e, "majorAxis");
    o.set("cx", g(c, "x"));
    o.set("cy", g(c, "y"));
    o.set("mx", g(m, "x"));
    o.set("my", g(m, "y"));
    o.set("ratio", or_(g(e, "ratio"), 1));
    o.set("t0", nn(g(e, "startParam"), 0));
    o.set("t1", nn(g(e, "endParam"), kTau));
    return o;
  }
  if (type == "SPLINE") {
    const Value pts = spline_points(e);
    if (len(pts) < 2) {
      skip("SPLINE");
      return {nullptr};
    }
    report_.set("splines", num(report_.get("splines")) + 1);
    Value o = ent("poly");
    o.set("closed", false);
    Array out;
    for (const Value& p : pts.as_array()) {
      out.push_back(base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}, {"b", 0}}));
    }
    o.set("pts", std::move(out));
    return o;
  }
  if (type == "TEXT" || type == "ATTRIB") {
    const Value t = text_of(e, base, f);
    if (type == "ATTRIB") {
      if (!t.truthy() || !g(e, "tag").truthy()) {
        return {nullptr};
      }
      Value o = t;
      o.set("tag", g(e, "tag"));
      if ((base::js_to_int32(num(g(e, "flags"))) & 1) != 0) {
        o.set("invisible", true);
      }
      return o;
    }
    return t.truthy() && !base::js_trim(str(g(t, "s"))).empty() ? t : Value(nullptr);
  }
  if (type == "MTEXT") {
    const Value value = g(e, "value");
    const std::string vs = value.truthy() ? str(value) : std::string();
    const std::string s = mtext_plain(vs);
    const Value height = g(e, "height");
    if (s.empty() || !height.truthy()) {
      return {nullptr};
    }
    const Value xa = g(e, "xAxis");
    const double rot = xa.truthy() && (g(xa, "x").truthy() || g(xa, "y").truthy())
                           ? base::js_atan2(num(g(xa, "y")), num(g(xa, "x")))
                           : 0;
    const double ap = num(or_(g(e, "attach"), 1)) - 1;
    const Value wv = g(e, "width");
    const Value ww = num(wv) > 0 ? wv : Value(0);
    const Value lsv = g(e, "lineSpacing");
    const Value ls = num(lsv) > 0 && lsv != Value(1) ? lsv : Value(0);
    // /^\\Q(-?[\d.]+);/: an oblique angle as the text's first format code
    double obl = 0;
    if (vs.starts_with("\\Q")) {
      std::size_t i = 2;
      if (i < vs.size() && vs[i] == '-') {
        ++i;
      }
      const std::size_t d0 = i;
      while (i < vs.size() && ((vs[i] >= '0' && vs[i] <= '9') || vs[i] == '.')) {
        ++i;
      }
      if (i > d0 && i < vs.size() && vs[i] == ';') {
        const double q = base::js_number(std::string_view(vs).substr(2, i - 2));
        if (std::isfinite(q) && q != 0) {
          obl = q * std::numbers::pi / 180;
        }
      }
    }
    Value o = ent("text");
    const Value ins = g(e, "insert");
    o.set("x", g(ins, "x"));
    o.set("y", g(ins, "y"));
    o.set("h", height);
    o.set("rot", norm_angle(rot));
    o.set("s", s);
    o.set("ha", pick3(js_mod(ap, 3), "left", "center", "right"));
    o.set("va", pick3(std::floor(ap / 3), "top", "middle", "bottom"));
    if (ww.truthy()) {
      o.set("ww", ww);
    }
    if (ls.truthy()) {
      o.set("ls", ls);
    }
    spread(o, style_name(g(e, "style")));
    if (obl != 0) {
      o.set("obl", obl);
    }
    spread(o, mask_of(e));
    return o;
  }
  if (type == "SOLID" || type == "TRACE") {
    const Value p = or_(g(e, "points"), Array{});
    std::array<Value, 4> raw = {item(p, 0), item(p, 1), item(p, 3).truthy() ? item(p, 3) : item(p, 2), item(p, 2)};
    Array flat;
    std::vector<std::array<Value, 2>> c;
    for (const Value& q : raw) {
      if (q.truthy()) {
        c.push_back({X(g(q, "x")), g(q, "y")});
      }
    }
    for (const auto& q : c) {
      flat.push_back(q[0]);
      flat.push_back(q[1]);
    }
    if (!c.empty()) {
      flat.push_back(c[0][0]);
      flat.push_back(c[0][1]);
    }
    Value o = ent("hatch");
    o.set("solid", true);
    o.set("loops", Array{Value(std::move(flat))});
    return o;
  }
  if (type == "HATCH") {
    Array loops = hatch_loops(e);
    if (loops.empty()) {
      skip(words_->t("dwg-import.hatch_boundary_not_read"));
      return {nullptr};
    }
    Value h = ent("hatch");
    const bool solid = g(e, "solid").truthy();
    h.set("solid", solid);
    h.set("pattern", or_(g(e, "pattern"), ""));
    h.set("loops", std::move(loops));
    const Value style = g(e, "style");
    if (style == Value(1)) {
      h.set("island", "outer");
    } else if (style == Value(2)) {
      h.set("island", "ignore");
    }
    if (g(e, "associative").truthy()) {
      h.set("assoc", true);
    }
    if (!solid) {
      if (const Value sc = g(e, "scale"); num(sc) > 0) {
        h.set("pscale", sc);
      }
      if (const Value an = g(e, "angle"); an.truthy()) {
        h.set("pangle", norm_angle(num(an)));
      }
      Array pl;
      if (const Value lines = g(e, "patternLines"); lines.is_array()) {
        for (const Value& l : lines.as_array()) {
          const Value b = g(l, "base");
          const Value o = g(l, "offset");
          if (finite(g(l, "angle")) && b.truthy() && finite(g(b, "x")) && finite(g(b, "y")) && o.truthy() &&
              finite(g(o, "x")) && finite(g(o, "y"))) {
            pl.push_back(l);
          }
        }
      }
      if (!pl.empty() && pl.size() <= 64) {
        Array out;
        for (const Value& l : pl) {
          const double ang = num(g(l, "angle"));
          const double ca = std::cos(ang);
          const double sa = std::sin(ang);
          const Value b = g(l, "base");
          const Value o = g(l, "offset");
          const double ox = num(g(o, "x"));
          const double oy = num(g(o, "y"));
          Array dashes;
          if (const Value ds = g(l, "dashes"); ds.is_array()) {
            for (const Value& d : ds.as_array()) {
              if (finite(d) && dashes.size() < 32) {
                dashes.push_back(d);
              }
            }
          }
          out.push_back(base::make_object({{"angle", ang * 180 / std::numbers::pi},
                                           {"x", g(b, "x")},
                                           {"y", g(b, "y")},
                                           {"dx", ox * ca + oy * sa},
                                           {"dy", -ox * sa + oy * ca},
                                           {"dashes", std::move(dashes)}}));
        }
        h.set("patlines", std::move(out));
      }
    }
    return h;
  }
  if (type == "INSERT" || type == "MINSERT") {
    if (!g(e, "block").truthy()) {
      return {nullptr};
    }
    const Value ins_pt = g(e, "insert");
    const Value sc = g(e, "scale");
    Value x = g(ins_pt, "x");
    double rot = num(or_(g(e, "rotation"), 0));
    Value sx = or_(sc.truthy() ? g(sc, "x") : Value(), 1);
    if (f) {
      x = -num(x);
      rot = std::numbers::pi - rot;
      sx = -num(sx);
    }
    Value ins = ent("insert");
    ins.set("block", g(e, "block"));
    ins.set("x", x);
    ins.set("y", g(ins_pt, "y"));
    ins.set("sx", sx);
    ins.set("sy", or_(sc.truthy() ? g(sc, "y") : Value(), 1));
    ins.set("rot", norm_angle(rot));
    if (sc.truthy()) {
      if (const Value sz = g(sc, "z"); sz.truthy() && sz != Value(1)) {
        ins.set("sz", sz);
      }
    }
    Array out;
    const double cols = js_max(1, num(or_(g(e, "cols"), 1)));
    const double rows = js_max(1, num(or_(g(e, "rows"), 1)));
    const double c = std::cos(num(ins.get("rot")));
    const double s = std::sin(num(ins.get("rot")));
    const double ix = num(ins.get("x"));
    const double iy = num(ins.get("y"));
    const double cs = num(or_(g(e, "colSpacing"), 0));
    const double rs = num(or_(g(e, "rowSpacing"), 0));
    // `for (let r = 0; r < rows; r++)`: rows and cols may be fractional (2.5 → 3 rounds), as in the JS
    for (std::size_t ri = 0; static_cast<double>(ri) < rows; ++ri) {
      const auto r = static_cast<double>(ri);
      for (std::size_t ki = 0; static_cast<double>(ki) < cols; ++ki) {
        const auto k = static_cast<double>(ki);
        if (out.size() >= limits_.minsert_copies) {
          break;  // the JS has no limit (docs/gaps.md)
        }
        const double dx = k * cs;
        const double dy = r * rs;
        Value one = ins;
        one.set("x", ix + dx * c - dy * s);
        one.set("y", iy + dx * s + dy * c);
        out.push_back(std::move(one));
      }
      if (out.size() >= limits_.minsert_copies) {
        break;
      }
    }
    Array attribs;
    if (const Value as = g(e, "attribs"); as.is_array()) {
      for (const Value& a : as.as_array()) {
        if (g(a, "invisible").truthy()) {
          continue;
        }
        Value copy = a;
        copy.set("type", "ATTRIB");
        Value t = convert(copy);
        if (t.truthy()) {
          attribs.push_back(std::move(t));
        }
      }
    }
    if (!out.empty()) {
      out[0].set("_attribs", std::move(attribs));
    }
    return out;
  }
  if (type == "3DFACE") {
    return face3d_from_dwg(e, base);
  }
  if (type == "MULTILEADER") {
    return multileader(e, base);
  }
  if (type == "IMAGE" || type == "WIPEOUT") {
    return picture(e, base);
  }
  if (type == "MLINE") {
    Array pts;
    if (const Value verts = g(e, "vertices"); verts.is_array()) {
      for (const Value& v : verts.as_array()) {
        const Value p = g(v, "point");
        if (p.truthy() && finite(g(p, "x"))) {
          pts.push_back(base::make_object({{"x", g(p, "x")}, {"y", g(p, "y")}, {"b", 0}}));
        }
      }
    }
    if (pts.size() < 2) {
      return {nullptr};
    }
    Value o = ent("poly");
    o.set("closed", g(e, "closed").truthy());
    o.set("pts", std::move(pts));
    return o;
  }
  if (type == "DIMENSION") {
    const Value layer = or_(g(e, "layer"), "");
    if (base::js_to_lower(str(layer)) == "*layernamefordynamicconstraint") {
      skip(words_->t("dwg-import.dimension_parametric_constraint"));
      return {nullptr};
    }
    const Value d = dimension(e);
    if (d.truthy()) {
      Value o = ent("dim");
      spread(o, d);
      if (const Value b = g(e, "block"); b.truthy()) {
        o.set("block", b);
      }
      return o;
    }
    if (!g(e, "block").truthy()) {
      skip(words_->t("dwg-import.dimension_no_block_draw"));
      return {nullptr};
    }
    bump("dimBlocks");
    Value o = ent("insert");
    o.set("block", g(e, "block"));
    o.set("x", 0);
    o.set("y", 0);
    o.set("sx", 1);
    o.set("sy", 1);
    o.set("rot", 0);
    o.set("dim", true);
    return o;
  }
  if (type == "ARC_DIMENSION" || type == "LARGE_RADIAL_DIMENSION") {
    if (!g(e, "block").truthy()) {
      skip(str(et) + " (no block to draw it from)");
      return {nullptr};
    }
    bump("dimBlocks");
    Value o = ent("insert");
    o.set("block", g(e, "block"));
    o.set("x", 0);
    o.set("y", 0);
    o.set("sx", 1);
    o.set("sy", 1);
    o.set("rot", 0);
    o.set("dim", true);
    return o;
  }
  if (type == "ACAD_TABLE") {
    if (!g(e, "block").truthy()) {
      skip(words_->t("dwg-import.acad_table_no_block"));
      return {nullptr};
    }
    Value o = ent("insert");
    o.set("block", g(e, "block"));
    o.set("x", 0);
    o.set("y", 0);
    o.set("sx", 1);
    o.set("sy", 1);
    o.set("rot", 0);
    return o;
  }
  if (type == "VIEWPORT") {
    const Value c = g(e, "center");
    const Value t = or_(g(e, "viewTarget"), base::pt2(0, 0));
    const Value dc = or_(g(e, "viewCenter"), c);
    if (!c.truthy() || !(num(g(e, "width")) > 0) || !(num(g(e, "height")) > 0)) {
      return {nullptr};
    }
    const double s = num(g(e, "viewHeight")) > 0 ? num(g(e, "height")) / num(g(e, "viewHeight")) : 1;
    Value vp = ent("viewport");
    vp.set("x", g(c, "x"));
    vp.set("y", g(c, "y"));
    vp.set("w", g(e, "width"));
    vp.set("h", g(e, "height"));
    vp.set("cx", num(g(dc, "x")) + num(or_(g(t, "x"), 0)));
    vp.set("cy", num(g(dc, "y")) + num(or_(g(t, "y"), 0)));
    vp.set("s", s);
    if (const Value tw = g(e, "twist"); finite(tw) && std::abs(tw.as_number()) > 1e-12) {
      vp.set("tw", tw);
    }
    const std::uint32_t stt = base::js_to_uint32(num(g(e, "status")));
    if ((stt & 16384U) != 0) {
      vp.set("locked", true);
    }
    if ((stt & 131072U) != 0) {
      vp.set("on", false);
    }
    Array frz;
    if (const Value fl = g(e, "frozenLayers"); fl.is_array()) {
      for (const Value& h : fl.as_array()) {
        const auto it = layer_by_handle_.find(base::js_to_upper(h.truthy() ? str(h) : std::string()));
        if (it == layer_by_handle_.end() || !it->second.is_string() || it->second.as_string().empty()) {
          continue;
        }
        if (std::ranges::find(frz, it->second) == frz.end()) {
          frz.push_back(it->second);
        }
      }
    }
    if (!frz.empty()) {
      vp.set("frz", std::move(frz));
    }
    const Value hd = g(e, "handle");
    if (const auto it = vp_overrides_.find(base::js_to_upper(hd.truthy() ? str(hd) : std::string()));
        it != vp_overrides_.end()) {
      vp.set("lov", it->second);
    }
    const Value& ccl = xdata_of(g(e, "xdata"), "CADCRAFT");
    const auto cc = [&](std::size_t i) { return item(item(ccl, i), 1); };
    if (cc(0) == Value("LAYOUT") && std::isfinite(num(cc(4))) && std::isfinite(num(cc(5))) &&
        std::isfinite(num(cc(6))) && std::isfinite(num(cc(7)))) {
      vp.set("cc", base::make_object({{"unit", cc(2) == Value("in") ? "in" : "mm"},
                                      {"key", cc(3).truthy() ? str(cc(3)) : std::string()},
                                      {"x0", num(cc(4))},
                                      {"y0", num(cc(5))},
                                      {"w", num(cc(6))},
                                      {"h", num(cc(7))}}));
    }
    return vp;
  }
  if (type == "XLINE" || type == "RAY") {
    const Value d = g(e, "direction");
    const double n = d.truthy() ? std::hypot(num(g(d, "x")), num(g(d, "y"))) : 0;
    const Value s = g(e, "start");
    if (!s.truthy() || !finite(g(s, "x")) || !(n > 1e-12)) {
      skip(et);
      return {nullptr};
    }
    Value o = ent(type == "XLINE" ? "xline" : "ray");
    o.set("x", g(s, "x"));
    o.set("y", g(s, "y"));
    o.set("dx", num(g(d, "x")) / n);
    o.set("dy", num(g(d, "y")) / n);
    return o;
  }
  if (type == "ATTDEF") {
    return {nullptr};
  }
  skip(et == Value("UNKNOWN") ? or_(g(e, "dxfName"), "UNKNOWN") : et);
  return {nullptr};
}

double z_of(const Value& e, const std::string& type) {
  const auto fin = [](const Value& v) { return finite(v) ? v.as_number() : 0.0; };
  const Value et = g(e, "type");
  const std::string t = str(et);
  const auto pz = [&](const char* k) {
    const Value p = g(e, k);
    return p.truthy() ? g(p, "z") : Value();
  };
  if (t == "CIRCLE" || t == "ARC" || t == "ELLIPSE") {
    return fin(pz("center"));
  }
  if (t == "POINT") {
    return fin(pz("position"));
  }
  if (t == "TEXT" || t == "ATTRIB") {
    const double z = fin(pz("insert"));
    return z != 0 ? z : fin(g(e, "elevation"));
  }
  if (t == "MTEXT") {
    return fin(pz("insert"));
  }
  if (t == "INSERT") {
    return type == "insert" ? fin(pz("insert")) : 0;
  }
  if (t == "LWPOLYLINE" || t == "POLYLINE_2D" || t == "HATCH") {
    return fin(g(e, "elevation"));
  }
  if (t == "MULTILEADER") {
    if (type != "mleader") {
      return 0;
    }
    const Value c = g(e, "context");
    const Value& r0 = item(c.truthy() ? g(c, "roots") : Value(), 0);
    const Value conn = r0.truthy() ? g(r0, "connection") : Value();
    return fin(conn.truthy() ? g(conn, "z") : Value());
  }
  return 0;
}

void Importer::with_props(Value& c, const Value& e) const {
  if (!c.truthy() || (c.is_array() && c.as_array().empty())) {
    return;
  }
  Value* one = c.is_array() ? c.as_array().data() : &c;
  if (!one->truthy() || g(*one, "type") == Value("face3d")) {
    return;
  }
  const std::string one_type = str(g(*one, "type"));
  Value add = Value::object();
  if (const Value tr = tr_from(g(e, "transparency")); !tr.is_undefined()) {
    add.set("tr", tr);
  }
  if (const Value th = g(e, "thickness"); finite(th) && th.as_number() != 0) {
    static constexpr std::array<std::string_view, 6> kTh = {"line", "circle", "arc", "poly", "point", "text"};
    if (std::ranges::find(kTh, std::string_view(one_type)) != kTh.end()) {
      add.set("th", th);
    }
  }
  spread(add, props_from_xdata(g(e, "xdata")));
  if (one_type != "hatch") {
    add.erase("bg");
    add.erase("ox");
    add.erase("oy");
    add.erase("assoc");
  }
  if (const double z = z_of(e, one_type); z != 0 && !std::isnan(z)) {
    add.set("z", z);
  }
  const auto each = [&](const std::function<void(Value&)>& fn) {
    if (c.is_array()) {
      for (Value& x : c.as_array()) {
        fn(x);
      }
    } else {
      fn(c);
    }
  };
  each([&](Value& x) { spread(x, add); });
  if (one_type == "line") {
    const Value s = g(e, "start");
    const Value t = g(e, "end");
    const Value z1 = s.truthy() ? g(s, "z") : Value();
    const Value z2 = t.truthy() ? g(t, "z") : Value();
    each([&](Value& x) {
      x.erase("z");
      if (z1.truthy()) {
        x.set("z1", z1);
      }
      if (z2.truthy()) {
        x.set("z2", z2);
      }
    });
  }
  one = c.is_array() ? c.as_array().data() : &c;
  if (one_type == "hatch" && (add.get("ox").truthy() || add.get("oy").truthy())) {
    if (Value* pl = one->find("patlines"); pl != nullptr && pl->truthy() && pl->is_array()) {
      const double ox = num(or_(add.get("ox"), 0));
      const double oy = num(or_(add.get("oy"), 0));
      for (Value& l : pl->as_array()) {
        l.set("x", num(g(l, "x")) - ox);
        l.set("y", num(g(l, "y")) - oy);
      }
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion) — convert → convert_base → convert, once: an INSERT's attributes (depth 2)
Value Importer::convert(const Value& e) {
  Value c = convert_base(e);
  tag_from_xdata(c, g(e, "xdata"));
  with_props(c, e);
  Value* one = &c;
  if (c.is_array()) {
    one = c.as_array().empty() ? nullptr : c.as_array().data();
  }
  const bool is_insert = g(e, "type") == Value("INSERT");
  if (is_insert && one != nullptr && one->truthy() && g(*one, "type") == Value("insert")) {
    if (g(e, "dyn").truthy()) {
      if (Value d = dyn_ref_from_dwg(e); d.truthy()) {
        one->set("dyn", std::move(d));
      }
    }
    if (g(e, "extDict").truthy()) {
      // objByH.get(String(objByH.get(String(objByH.get(String(e.extDict))?.entries?.ACAD_FILTER))?.entries?.SPATIAL))
      const auto get = [&](const Value& h) -> const Value* {
        const auto it = obj_by_h_.find(str(h));
        return it == obj_by_h_.end() ? nullptr : it->second;
      };
      const auto entry = [&](const Value* o, std::string_view k) -> Value {
        if (o == nullptr) {
          return {};
        }
        const Value en = g(*o, "entries");
        return en.truthy() ? g(en, k) : Value();
      };
      const Value* xd = get(g(e, "extDict"));
      const Value* filter = get(entry(xd, "ACAD_FILTER"));
      const Value* f = get(entry(filter, "SPATIAL"));
      if (f != nullptr && g(*f, "type") == Value("SPATIAL_FILTER")) {
        if (Value clip = clip_from_filter(*f); clip.truthy()) {
          one->set("clip", std::move(clip));
        }
      }
    }
  }
  return c;
}

// ---------------------------------------------------------------- file-wide parts

void Importer::layer_overrides() {
  std::unordered_map<std::string, const Value*> objs;
  if (const Value& os = at(*db_, "objects"); os.is_array()) {
    for (const Value& o : os.as_array()) {
      objs[base::js_to_upper(str(g(o, "handle")))] = &o;
    }
  }
  const auto put = [&](const Value& vp, const Value& layer, std::string_view k, Value v) {
    const std::string key = base::js_to_upper(vp.truthy() ? str(vp) : std::string());
    if (key.empty()) {
      return;
    }
    Value& m = vp_overrides_.try_emplace(key, Value::object()).first->second;
    const std::string ln = str(layer);
    if (!m.has(ln)) {
      m.set(ln, Value::object());
    }
    m.find(ln)->set(k, std::move(v));
  };
  const Value& layers = at(at(*db_, "tables"), "layers");
  if (!layers.is_array()) {
    return;
  }
  for (const Value& l : layers.as_array()) {
    const Value xd = g(l, "extDict");
    const Value* d = nullptr;
    if (xd.truthy()) {
      if (const auto it = objs.find(base::js_to_upper(str(xd))); it != objs.end()) {
        d = it->second;
      }
    }
    if (d == nullptr) {
      continue;
    }
    const Value entries = g(*d, "entries");
    if (!entries.is_object()) {
      continue;
    }
    for (const base::Member* enp : base::js_members(entries.as_object())) {
      const base::Member& en = *enp;
      static constexpr std::string_view kPre = "ADSK_XREC_LAYER_";
      static constexpr std::string_view kPost = "_OVR";
      const std::string& name = en.key;
      if (!name.starts_with(kPre) || !name.ends_with(kPost) || name.size() <= kPre.size() + kPost.size()) {
        continue;
      }
      const std::string what = name.substr(kPre.size(), name.size() - kPre.size() - kPost.size());
      if (what != "COLOR" && what != "LINETYPE" && what != "LINEWT" && what != "ALPHA") {
        continue;
      }
      const auto xi = objs.find(base::js_to_upper(str(en.value)));
      if (xi == objs.end() || g(*xi->second, "type") != Value("XRECORD")) {
        continue;
      }
      const Value data = g(*xi->second, "data");
      if (!data.is_array()) {
        continue;
      }
      Value vp(nullptr);
      const Value lname = g(l, "name");
      for (const Value& pair : data.as_array()) {
        const Value c = item(pair, 0);
        const Value v = item(pair, 1);
        if (c == Value(335)) {
          vp = v;
          continue;
        }
        if (!vp.truthy()) {
          continue;
        }
        if (what == "COLOR" && (c == Value(420) || c == Value(62))) {
          const std::uint32_t w = base::js_to_uint32(num(v));
          const std::uint32_t how = c == Value(62) ? 0xC3U : w >> 24U;
          if (how == 0xC2U) {
            put(vp, lname, "color", hex_color(static_cast<double>(w & 0xFFFFFFU)));
          } else if (how == 0xC3U || c == Value(62)) {
            const double i = c == Value(62) ? std::abs(num(v)) : static_cast<double>(w & 0xFFU);
            if (i >= 1 && i <= 255) {
              put(vp, lname, "color", i);
            }
          }
        } else if (what == "LINETYPE" && c == Value(343)) {
          if (const Value n = lt_name(v); n.truthy()) {
            put(vp, lname, "lt", n);
          }
        } else if (what == "LINEWT" && c == Value(91)) {
          const double n = num(v);
          if (n >= 0 && n <= 211) {
            put(vp, lname, "lw", n / 100);
          }
        } else if (what == "ALPHA" && c == Value(440)) {
          if (const Value tr = tr_from(num(v)); !tr.is_undefined()) {
            put(vp, lname, "tr", tr);
          }
        } else {
          continue;
        }
        vp = Value(nullptr);
      }
    }
  }
}

Value split_screen(const Value& list) {
  Array recs;
  if (!list.is_array()) {
    return vports_from(recs);
  }
  for (const Value& v : list.as_array()) {
    if (!v.truthy() || !is_active(g(v, "name"))) {
      continue;
    }
    std::string sheet;
    bool has_sheet = false;
    for (const std::string& s : cadcraft_strings(g(v, "xdata"))) {
      if (s.starts_with("PANESHEET:")) {
        sheet = s;
        has_sheet = true;
        break;
      }
    }
    const Value ll = g(v, "lowerLeft");
    const Value ur = g(v, "upperRight");
    const Value vc = g(v, "viewCenter");
    const Value c = g(v, "center");
    const auto opt = [](const Value& p, const char* k) { return p.truthy() ? g(p, k) : Value(); };
    Value r = base::make_object({{"x0", opt(ll, "x")},
                                 {"y0", opt(ll, "y")},
                                 {"x1", opt(ur, "x")},
                                 {"y1", opt(ur, "y")},
                                 {"cx", nn(opt(vc, "x"), opt(c, "x"))},
                                 {"cy", nn(opt(vc, "y"), opt(c, "y"))},
                                 {"h", g(v, "viewHeight")},
                                 {"tw", finite(g(v, "twist")) ? g(v, "twist") : Value(0)}});
    if (has_sheet && !sheet.empty()) {
      r.set("sheet", sheet.substr(10));
    }
    recs.push_back(std::move(r));
  }
  return vports_from(recs);
}

Value ucs_from(const Value& H, const Value& table, const Value& vports) {
  const auto ucs_of = [](const Value& o, const Value& x) -> Value {
    if (o.truthy() && x.truthy() && finite(g(o, "x")) && finite(g(o, "y")) &&
        (g(x, "x").truthy() || g(x, "y").truthy())) {
      return base::make_object(
          {{"x", g(o, "x")}, {"y", g(o, "y")}, {"ang", base::js_atan2(num(g(x, "y")), num(g(x, "x")))}});
    }
    return {nullptr};
  };
  Value current = ucs_of(g(H, "UCSORG"), g(H, "UCSXDIR"));
  if (current.truthy() && std::abs(num(g(current, "x"))) < 1e-12 && std::abs(num(g(current, "y"))) < 1e-12 &&
      std::abs(num(g(current, "ang"))) < 1e-12) {
    current = Value(nullptr);
  }
  Array named;
  if (table.is_array()) {
    for (const Value& u : table.as_array()) {
      const Value c = ucs_of(g(u, "origin"), g(u, "xAxis"));
      if (c.truthy() && g(u, "name").truthy()) {
        Value o = base::make_object({{"name", str(g(u, "name"))}});
        for (const base::Member& m : c.as_object()) {
          o.set(m.key, m.value);
        }
        named.push_back(std::move(o));
      }
    }
  }
  double tw = 0;
  if (vports.is_array()) {
    for (const Value& v : vports.as_array()) {
      if (v.truthy() && is_active(g(v, "name"))) {
        tw = finite(g(v, "twist")) ? g(v, "twist").as_number() : 0;
        break;
      }
    }
  }
  Value out = Value::object();
  if (current.truthy() || !named.empty()) {
    out.set("ucs", base::make_object({{"current", current}, {"named", std::move(named)}}));
  }
  if (tw != 0) {
    out.set("viewTwist", tw);
  }
  return out;
}

Value named_views(const Value& list, const Array& layouts) {
  Array out;
  if (!list.is_array()) {
    return out;
  }
  for (const Value& v : list.as_array()) {
    const Value c = g(v, "center");
    if (!v.truthy() || !g(v, "name").truthy() || !(num(g(v, "width")) > 0) || !(num(g(v, "height")) > 0) ||
        !c.truthy() || !finite(g(c, "x")) || !finite(g(c, "y"))) {
      continue;
    }
    const Value t = or_(g(v, "target"), Value::object());
    const bool on_paper = g(v, "paperSpace").truthy();
    if (on_paper && layouts.empty()) {
      continue;
    }
    Value o = base::make_object({{"name", str(g(v, "name"))},
                                 {"cx", num(g(c, "x")) + num(or_(g(t, "x"), 0))},
                                 {"cy", num(g(c, "y")) + num(or_(g(t, "y"), 0))},
                                 {"w", g(v, "width")},
                                 {"h", g(v, "height")}});
    if (on_paper) {
      o.set("layout", g(layouts[0], "name"));
    }
    out.push_back(std::move(o));
  }
  return out;
}

Value length_format(const Value& H) {
  const double lu = base::js_round(num(g(H, "LUNITS")));
  const double pr = base::js_round(num(g(H, "LUPREC")));
  Value out = Value::object();
  if (lu >= 1 && lu <= 5 && lu != 2) {
    out.set("lunits", lu);
    if (pr >= 0 && pr <= 8) {
      out.set("luprec", pr);
    }
  }
  return out;
}

const char* release_of(std::string_view tag) {
  if (tag == "AC1009") {
    return "R12";
  }
  if (tag == "AC1012") {
    return "R13";
  }
  if (tag == "AC1014") {
    return "R14";
  }
  if (tag == "AC1015") {
    return "2000";
  }
  if (tag == "AC1018") {
    return "2004";
  }
  if (tag == "AC1021") {
    return "2007";
  }
  if (tag == "AC1024") {
    return "2010";
  }
  if (tag == "AC1027") {
    return "2013";
  }
  if (tag == "AC1032") {
    return "2018";
  }
  return "";
}

const char* units_of(const Value& v) {
  if (!v.is_number()) {
    return "";
  }
  const double i = v.as_number();
  if (i == 1) {
    return "inches";
  }
  if (i == 2) {
    return "feet";
  }
  if (i == 3) {
    return "miles";
  }
  if (i == 4) {
    return "millimeters";
  }
  if (i == 5) {
    return "centimeters";
  }
  if (i == 6) {
    return "meters";
  }
  if (i == 7) {
    return "kilometers";
  }
  if (i == 8) {
    return "microinches";
  }
  if (i == 9) {
    return "mils";
  }
  if (i == 10) {
    return "yards";
  }
  return "";
}

/// /^\*(model|paper)_space/i
bool is_space_block(const Value& name) {
  const std::string s = base::js_to_lower(name.truthy() ? str(name) : std::string());
  return s.starts_with("*model_space") || s.starts_with("*paper_space");
}

Value Importer::run() {
  const Value& tables = at(*db_, "tables");
  // linetypes: the table's patterns, names by handle
  if (const Value& lts = at(tables, "linetypes"); lts.is_array()) {
    for (const Value& l : lts.as_array()) {
      const Value h = g(l, "handle");
      lt_by_handle_[base::js_to_upper(h.truthy() ? str(h) : std::string())] = g(l, "name");
    }
  }
  Array linetypes;
  if (const Value& lts = at(tables, "linetypes"); lts.is_array()) {
    for (const Value& l : lts.as_array()) {
      const Value name = g(l, "name");
      const std::string low = base::js_to_lower(name.truthy() ? str(name) : std::string());
      const Value els = or_(g(l, "elements"), Array{});
      if (!name.truthy() || low == "byblock" || low == "bylayer" || low == "continuous" || len(els) == 0) {
        continue;
      }
      Array pattern;
      for (const Value& x : els.as_array()) {
        const double n = num(g(x, "length"));
        pattern.emplace_back(n != 0 && !std::isnan(n) ? n : 0.0);
      }
      linetypes.push_back(base::make_object(
          {{"name", name}, {"description", or_(g(l, "description"), "")}, {"pattern", std::move(pattern)}}));
    }
  }
  Array layers;
  if (const Value& ls = at(tables, "layers"); ls.is_array()) {
    for (const Value& l : ls.as_array()) {
      const Value h = g(l, "handle");
      layer_by_handle_[base::js_to_upper(h.truthy() ? str(h) : std::string())] = g(l, "name");
    }
  }
  layer_overrides();
  if (const Value& ls = at(tables, "layers"); ls.is_array()) {
    for (const Value& l : ls.as_array()) {
      const Value c = or_(g(l, "color"), Value::object());
      Value col;
      if (!g(c, "rgb").is_undefined()) {
        col = hex_color(num(g(c, "rgb")));
      } else {
        const double i = std::abs(num(nn(g(c, "index"), 7)));
        col = i != 0 && !std::isnan(i) ? i : 7.0;
      }
      const Value lw = lw_mm(g(l, "lineweight"));
      const Value name = g(l, "name");
      Value o =
          base::make_object({{"name", name},
                             {"color", col},
                             {"on", !g(l, "off").truthy()},
                             {"frozen", g(l, "frozen").truthy()},
                             {"locked", g(l, "locked").truthy()},
                             {"plot", g(l, "plot") != Value(false) && base::js_to_upper(str(name)) != "DEFPOINTS"},
                             {"linetype", or_(lt_name(g(l, "linetype")), "Continuous")},
                             {"lineweight", lw.is_undefined() || lw == Value(-2) ? Value(-3) : lw}});
      if (g(l, "frozenInNewViewports").truthy()) {
        o.set("newVpFrozen", true);
      }
      layers.push_back(std::move(o));
    }
  }

  // dimension styles: the header's settings, then each named style
  const Value H = or_(g(*db_, "header"), Value::object());
  dim_style_ = style_from(H, dim_defaults());
  const Value& dss = at(tables, "dimstyles");
  if (dss.is_array()) {
    for (const Value& d : dss.as_array()) {
      dim_styles_[base::js_to_upper(str(g(d, "name")))] = style_from(or_(g(d, "vars"), Value::object()), dim_style_);
    }
    for (const Value& d : dss.as_array()) {
      const auto it = dim_styles_.find(base::js_to_upper(str(g(d, "name"))));
      const Value vars = g(d, "vars");
      const Value tx = vars.truthy() ? g(vars, "DIMTXSTY_NAME") : Value();
      if (it != dim_styles_.end() && it->second.truthy() && tx.truthy() && !is_standard(str(tx))) {
        it->second.set("txsty", str(tx));
      }
    }
  }
  Array named_dim_styles;
  if (dss.is_array()) {
    for (const Value& d : dss.as_array()) {
      const Value name = g(d, "name");
      if (!name.truthy() || is_standard(str(name)) || (base::js_to_int32(num(g(d, "flags"))) & 16) != 0) {
        continue;
      }
      const auto it = dim_styles_.find(base::js_to_upper(str(name)));
      named_dim_styles.emplace_back(Array{str(name), it == dim_styles_.end() ? Value() : it->second});
    }
  }
  Array text_styles;
  if (const Value& sts = at(tables, "styles"); sts.is_array()) {
    for (const Value& s : sts.as_array()) {
      const Value name = g(s, "name");
      const auto fl = base::js_to_int32(num(g(s, "flags")));
      if (!name.truthy() || g(s, "shapeFile").truthy() || (fl & 1) != 0 || (fl & 16) != 0 || is_standard(str(name))) {
        continue;
      }
      const Value font = g(s, "font");
      const Value ht = g(s, "height");
      const Value wf = g(s, "widthFactor");
      const Value ob = g(s, "oblique");
      text_styles.emplace_back(Array{str(name), base::make_object({{"font", font.truthy() ? str(font) : std::string()},
                                                                   {"h", finite(ht) ? ht : Value(0)},
                                                                   {"wf", num(wf) > 0 ? wf : Value(1)},
                                                                   {"obl", finite(ob) ? ob : Value(0)}})});
    }
  }

  // model space and every sheet are blocks too (*Model_Space, *Paper_Space…), in file order
  static const Array kNone;
  const Value& bl = at(*db_, "blocks");
  block_list_ = bl.is_array() ? &bl.as_array() : &kNone;
  const Value* model = nullptr;
  for (const Value& b : *block_list_) {
    if (base::js_to_lower(str(g(b, "name"))) == "*model_space") {
      model = &b;
      break;
    }
  }
  for (const Value& b : *block_list_) {
    taken_.insert(base::js_to_upper(str(g(b, "name"))));
  }
  if (const Value& objs = at(*db_, "objects"); objs.is_array()) {
    for (const Value& o : objs.as_array()) {
      if (g(o, "type") == Value("IMAGEDEF")) {
        image_defs_[vkey(g(o, "handle"))] = &o;
      }
    }
    for (const Value& o : objs.as_array()) {
      obj_by_h_[str(g(o, "handle"))] = &o;
    }
  }
  std::string parametric;
  const Value& records = at(tables, "blockRecords");
  for (const Value& b : *block_list_) {
    const Value name = g(b, "name");
    if (is_space_block(name)) {
      continue;
    }
    if (is_param_block(name)) {
      Array conv;
      if (const Value& es = at(b, "entities"); es.is_array()) {
        for (const Value& e : es.as_array()) {
          Value c = convert(e);
          if (c.truthy()) {
            conv.push_back(std::move(c));
          }
        }
      }
      parametric = parametric_text(conv);
      continue;
    }
    DynHooks hooks{.convert = [this](const Value& e) { return convert(e); },
                   .attdef_of = [this](const Value& e) { return attdef_of(e); },
                   .def_name = Value(),
                   .record = nullptr};
    if (const Value of = g(b, "dynamicOf"); of.truthy()) {
      hooks.def_name = Value(nullptr);
      for (const Value& x : *block_list_) {
        if (g(x, "record") == of) {
          hooks.def_name = g(x, "name");
          break;
        }
      }
    }
    if (records.is_array()) {
      for (const Value& r : records.as_array()) {
        if (g(r, "handle") == g(b, "record")) {
          hooks.record = &r;
          break;
        }
      }
    }
    const Value bb = g(b, "base");
    const auto base_pt = [&] {
      const Value bx = bb.truthy() ? g(bb, "x") : Value();
      const Value by = bb.truthy() ? g(bb, "y") : Value();
      return base::make_object({{"x", or_(bx, 0)}, {"y", or_(by, 0)}});
    };
    if (Value dyn_b = dyn_block_from_dwg(b, hooks); dyn_b.truthy()) {
      Value o = base::make_object({{"name", name}, {"base", base_pt()}, {"entities", dyn_b.get("entities")}});
      if (len(dyn_b.get("attdefs")) != 0) {
        o.set("attdefs", dyn_b.get("attdefs"));
      }
      const Value dyn = dyn_b.get("dyn");
      o.set("dyn", dyn);
      report_.set("dynamicBlocks", num(or_(report_.get("dynamicBlocks"), 0)) + (g(dyn, "def").truthy() ? 1 : 0));
      blocks_.push_back(std::move(o));
      continue;
    }
    Array ents;
    Array attdefs;
    if (const Value& es = at(b, "entities"); es.is_array()) {
      for (const Value& e : es.as_array()) {
        if (g(e, "type") == Value("ATTDEF")) {
          if (Value d = attdef_of(e); d.truthy()) {
            attdefs.push_back(std::move(d));
          }
          continue;
        }
        Value c = convert(e);
        if (c.is_array()) {
          for (Value& x : c.as_array()) {
            ents.push_back(std::move(x));
          }
        } else if (c.truthy()) {
          ents.push_back(std::move(c));
        }
      }
    }
    const bool xref = (base::js_to_int32(num(g(b, "flags"))) & 4) != 0;
    Value o = base::make_object({{"name", name}, {"base", base_pt()}, {"entities", xref ? Array{} : std::move(ents)}});
    if (!attdefs.empty() && !xref) {
      o.set("attdefs", std::move(attdefs));
    }
    if (xref) {
      const Value xp = g(b, "xrefPath");
      o.set("xref", base::make_object({{"path", xp.truthy() ? str(xp) : str(name)}, {"loaded", false}}));
      bump("xrefs");
    }
    blocks_.push_back(std::move(o));
  }

  Array entities;
  if (model != nullptr) {
    if (const Value& es = at(*model, "entities"); es.is_array()) {
      for (const Value& e : es.as_array()) {
        Value c = convert(e);
        if (!c.truthy()) {
          continue;
        }
        if (c.is_array()) {
          for (Value& x : c.as_array()) {
            entities.push_back(std::move(x));
          }
        } else {
          entities.push_back(std::move(c));
        }
        report_.set("read", num(report_.get("read")) + 1);
      }
    }
  }
  // the layouts' own blocks: paper-space entities by layout block
  std::vector<const Value*> layout_objs;
  if (const Value& lo = at(*db_, "layouts"); lo.is_array()) {
    for (const Value& l : lo.as_array()) {
      const Value n = g(l, "name");
      if (base::js_to_lower(n.truthy() ? str(n) : std::string()) == "model" || !g(l, "block").truthy()) {
        continue;
      }
      layout_objs.push_back(&l);
    }
  }
  std::vector<std::pair<std::string, Array>> paper_of;  // new Map(layoutObjs.map(l => [l.block, []])): first place kept
  for (const Value* l : layout_objs) {
    const std::string k = vkey(g(*l, "block"));
    if (std::ranges::find_if(paper_of, [&](const auto& p) { return p.first == k; }) == paper_of.end()) {
      paper_of.emplace_back(k, Array{});
    }
  }
  for (const Value& b : *block_list_) {
    const std::string k = vkey(g(b, "record"));
    auto it = std::ranges::find_if(paper_of, [&](const auto& p) { return p.first == k; });
    if (it == paper_of.end() || &b == model) {
      continue;
    }
    if (const Value& es = at(b, "entities"); es.is_array()) {
      for (const Value& e : es.as_array()) {
        Value c = convert(e);
        it = std::ranges::find_if(paper_of, [&](const auto& p) { return p.first == k; });
        if (c.is_array()) {
          for (Value& x : c.as_array()) {
            it->second.push_back(std::move(x));
          }
        } else if (c.truthy()) {
          it->second.push_back(std::move(c));
        }
        report_.set("paperSpace", num(report_.get("paperSpace")) + 1);
      }
    }
  }
  // attribute values: positions relative to their block
  {
    std::vector<Array*> lists = {&entities};
    for (Value& b : blocks_) {
      if (Value* es = b.find("entities"); es != nullptr && es->is_array()) {
        lists.push_back(&es->as_array());
      }
    }
    for (auto& [k, list] : paper_of) {
      lists.push_back(&list);
    }
    resolve_attribs(lists, blocks_);
  }
  // A dynamic block's `dyn.all` holds the very objects its `entities` shows (dynBlockFromDwg: entities =
  // all.filter(shown).flat()), so what resolveAttribs did to them shows in `all` too.
  for (Value& b : blocks_) {
    Value* dyn = b.find("dyn");
    const Value* ents = b.find("entities");
    if (dyn == nullptr || ents == nullptr || !ents->is_array()) {
      continue;
    }
    Value* all = dyn->find("all");
    const Value* shown = dyn->find("shown");
    if (all == nullptr || shown == nullptr || !all->is_array() || !shown->is_array()) {
      continue;
    }
    std::size_t k = 0;
    const Array& es = ents->as_array();
    for (std::size_t i = 0; i < all->as_array().size() && i < shown->as_array().size(); ++i) {
      if (!shown->as_array()[i].truthy()) {
        continue;
      }
      for (Value& x : all->as_array()[i].as_array()) {
        if (k < es.size()) {
          x = es[k++];
        }
      }
    }
  }
  Value& notes = *report_.find("notes");
  const auto note = [&](std::string_view report_key, std::string_view key) {
    const Value n = report_.get(report_key);
    if (n.truthy()) {
      notes.push(words_->tn(key, num(n)));
    }
  };
  note("splines", "dwg.read.splines");
  note("xrefs", "dwg.read.xrefs");
  note("mleaderBlocks", "dwg.read.mleaderblocks");
  note("wipeouts", "dwg.read.wipeouts");
  note("images", "dwg.read.images");
  note("dimBlocks", "dwg.read.dimblocks");

  // layouts, in tab order; the first viewport of each is the sheet itself
  const Value measurement = g(H, "MEASUREMENT");
  const Value insunits = g(H, "INSUNITS");
  const auto imperial_code = [](const Value& u) {
    if (!u.is_number()) {
      return false;
    }
    const double i = u.as_number();
    return i == 1 || i == 2 || i == 3 || i == 8 || i == 9 || i == 10;
  };
  const bool inches = measurement == Value(0) || (measurement.is_undefined() && imperial_code(insunits));
  std::vector<const Value*> sorted = layout_objs;
  std::ranges::stable_sort(sorted, [](const Value* a, const Value* b) {
    const double d = num(nn(g(*a, "tabOrder"), 0)) - num(nn(g(*b, "tabOrder"), 0));
    return d < 0;  // a comparator's NaN counts as 0 (equal)
  });
  Array layouts;
  for (const Value* l : sorted) {
    const std::string k = vkey(g(*l, "block"));
    auto it = std::ranges::find_if(paper_of, [&](const auto& p) { return p.first == k; });
    Array list = it == paper_of.end() ? Array{} : it->second;
    Value first_vp;
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (g(list[i], "type") == Value("viewport")) {
        first_vp = list[i];
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    const Value lim = g(*l, "limits");
    const Value mn = or_(lim.truthy() ? g(lim, "min") : Value(), base::pt2(0, 0));
    const Value mx = or_(lim.truthy() ? g(lim, "max") : Value(), base::pt2(0, 0));
    Value paper = base::make_object({{"unit", inches ? "in" : "mm"},
                                     {"x0", g(mn, "x")},
                                     {"y0", g(mn, "y")},
                                     {"w", num(g(mx, "x")) - num(g(mn, "x"))},
                                     {"h", num(g(mx, "y")) - num(g(mn, "y"))}});
    const Value own = first_vp.truthy() ? g(first_vp, "cc") : Value();
    if (own.truthy() && num(g(own, "w")) > 0 && num(g(own, "h")) > 0) {
      paper = base::make_object({{"unit", g(own, "unit")},
                                 {"x0", g(own, "x0")},
                                 {"y0", g(own, "y0")},
                                 {"w", g(own, "w")},
                                 {"h", g(own, "h")}});
    } else if (num(g(paper, "w")) <= 0 || num(g(paper, "h")) <= 0) {
      if (first_vp.truthy()) {
        paper.set("x0", num(g(first_vp, "x")) - num(g(first_vp, "w")) / 2);
        paper.set("y0", num(g(first_vp, "y")) - num(g(first_vp, "h")) / 2);
        paper.set("w", g(first_vp, "w"));
        paper.set("h", g(first_vp, "h"));
      } else {
        paper.set("x0", 0);
        paper.set("y0", 0);
        paper.set("w", inches ? 11 : 297);
        paper.set("h", inches ? 8.5 : 210);
      }
    }
    const std::string std_key = match_paper(num(g(paper, "w")), num(g(paper, "h")), str(g(paper, "unit")));
    const Value own_key = own.truthy() ? g(own, "key") : Value();
    if (own_key.truthy() && !std_key.empty() && Value(std_key) == own_key) {
      paper.set("key", own_key);
    } else if (!std_key.empty()) {
      paper.set("key", std_key);
    }
    const Value ln = g(*l, "name");
    layouts.push_back(
        base::make_object({{"name", or_(ln, "Layout")}, {"paper", std::move(paper)}, {"entities", std::move(list)}}));
  }
  if (!layouts.empty()) {
    report_.set("layouts", static_cast<double>(layouts.size()));
  }

  Value meta = base::make_object({{"version", tag_}, {"release", release_of(tag_)}, {"units", units_of(insunits)}});
  if (const Value ib = g(H, "INSBASE"); ib.truthy() && finite(g(ib, "x"))) {
    meta.set("insbase", base::make_object({{"x", g(ib, "x")}, {"y", or_(g(ib, "y"), 0)}}));
  }
  if (const Value lts = g(H, "LTSCALE"); num(lts) > 0 && lts != Value(1)) {
    meta.set("ltscale", lts);
  }
  const Value lf = length_format(H);  // a named value: a range-for over a temporary's member would dangle
  for (const base::Member& m : lf.as_object()) {
    meta.set(m.key, m.value);
  }
  Value drawing = base::make_object({{"entities", std::move(entities)},
                                     {"layers", std::move(layers)},
                                     {"blocks", Array{}},
                                     {"layouts", Array{}},
                                     {"dimStyle", dim_style_}});
  drawing.set("blocks", std::move(blocks_));
  const Array& layouts_ref = layouts;
  Value views = named_views(at(tables, "views"), layouts_ref);
  drawing.set("layouts", std::move(layouts));
  if (!parametric.empty()) {
    drawing.set("parametric", parametric);
  }
  drawing.set("linetypes", std::move(linetypes));
  drawing.set("meta", std::move(meta));
  drawing.set("views", std::move(views));
  drawing.set("vports", split_screen(at(tables, "vports")));
  const Value ucs = ucs_from(H, at(tables, "ucs"), at(tables, "vports"));
  for (const base::Member& m : ucs.as_object()) {
    drawing.set(m.key, m.value);
  }
  drawing.set("textStyles", std::move(text_styles));
  drawing.set("dimStyles", std::move(named_dim_styles));
  if (report_.get("dynamicBlocks").truthy()) {
    drawing.set("dynSrc", dyn_source(*db_));
  }
  drawing.set("report", std::move(report_));
  read3d_from_dwg(*db_, drawing, *words_);
  return drawing;
}

}  // namespace

base::Value import_dwg(const base::Value& db, std::string_view version_tag, const i18n::Catalog& words,
                       const ImportLimits& limits) {
  Importer imp(db, version_tag, words, limits);
  return imp.run();
}

}  // namespace cc::cad
