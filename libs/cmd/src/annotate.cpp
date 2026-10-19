// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.1c: ARRAY, ARRAYRECT, ARRAYPOLAR, REVCLOUD, MATCHPROP, MTEXT, MLEADER,
// TEXTEDIT. A host without an in-place text editor (the CLI, scripts) takes text at the command line, as the JS does.
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/dims.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/geom.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

using Mat = cad::Mat2;

Mat m_mul(const Mat& m, const Mat& n) {
  return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],        m[0] * n[2] + m[2] * n[3],
          m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
Mat m_translate(double x, double y) {
  return {1, 0, 0, 1, x, y};
}
Mat m_rotate(double r) {
  const double c = base::js_cos(r);
  const double s = base::js_sin(r);
  return {c, s, -s, c, 0, 0};
}
Mat m_about(Pt p, const Mat& m) {
  return m_mul(m_translate(p.x, p.y), m_mul(m, m_translate(-p.x, -p.y)));
}
Pt m_apply(const Mat& m, Pt p) {
  return {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]};
}
std::string type_of(const Value& e) {
  return base::js_string(e.get("type"));
}
/// commands.js fmt(n)
std::string fmt(double n) {
  if (std::abs(n) < 1e-10) {
    return "0";
  }
  std::string s;
  base::append_number(s, base::js_number(base::js_to_fixed(n, 4)));
  return s;
}
std::string fmt_len(const Ctx& c, double n) {
  const Value lu = c.doc.meta.get("lunits");
  return cad::format_length(n, lu.is_undefined() || lu.is_null() ? Value(2) : lu, c.doc.meta.get("luprec"));
}
/// String(s).trim() is not empty
bool has_words(const std::string& s) {
  return s.find_first_not_of(" \t\n\r\f\v") != std::string::npos;
}
double num_or(const Value& v, double d) {
  return v.is_number() ? v.as_number() : d;
}
/// v || d for numbers
double or_num(const Value& v, double d) {
  const double x = base::js_to_number(v);
  return v.truthy() && !std::isnan(x) ? x : d;
}
std::vector<Value> current(const Ctx& c, const IdSet& ids) {
  std::vector<Value> out;
  for (const Id id : ids) {
    if (const Value* e = c.doc.entities().get(id)) {
      out.push_back(*e);
    }
  }
  return out;
}

// ---- ARRAY

struct ArraySpec {
  bool rect = true;
  double cols = 4, rows = 3, dx = 1, dy = 1;
  Pt center;
  double items = 6, fill = kTau;
  bool rotate = true;
};

/// commands.js arrayCopies(src, a, limit): the copies an array makes (not the originals), or nothing over the limit
std::optional<base::Array> array_copies(const std::vector<Value>& src, const ArraySpec& a, std::size_t limit = 20000) {
  base::Array out;
  cad::Bounds b;
  for (const Value& e : src) {
    const cad::Bounds eb = cad::entity_bounds(e, nullptr);
    if (eb.valid()) {
      b = {std::min(b.min_x, eb.min_x), std::min(b.min_y, eb.min_y), std::max(b.max_x, eb.max_x),
           std::max(b.max_y, eb.max_y)};
    }
  }
  const auto add = [&](const Mat& m) {
    for (const Value& e : src) {
      out.push_back(cad::tx_entity(e, m));
      if (out.size() > limit) {
        return false;
      }
    }
    return true;
  };
  if (a.rect) {
    // rows, cols and items are whole numbers from 1 to 10000 (the prompts take nothing else)
    const auto rows = static_cast<int>(a.rows);
    const auto cols = static_cast<int>(a.cols);
    for (int r = 0; r < rows; ++r) {
      for (int k = 0; k < cols; ++k) {
        if (r == 0 && k == 0) {
          continue;
        }
        if (!add(m_translate(k * a.dx, r * a.dy))) {
          return std::nullopt;
        }
      }
    }
    return out;
  }
  const bool full = std::abs(std::abs(a.fill) - kTau) < 1e-9;
  const double step = full ? a.fill / a.items : a.fill / std::max(1.0, a.items - 1);
  const Pt mid = b.valid() ? Pt{(b.min_x + b.max_x) / 2, (b.min_y + b.max_y) / 2} : a.center;
  const auto items = static_cast<int>(a.items);
  for (int i = 1; i < items; ++i) {
    const Mat turn = m_about(a.center, m_rotate(step * i));
    Mat m = turn;
    if (!a.rotate) {  // keep the objects upright
      const Pt p = m_apply(turn, mid);
      m = m_translate(p.x - mid.x, p.y - mid.y);
    }
    if (!add(m)) {
      return std::nullopt;
    }
  }
  return out;
}

bool whole(const Answer& v) {
  return v.kind == Answer::Kind::number && std::floor(v.number) == v.number && v.number >= 1 && v.number <= 10000;
}

/// commands.js arrayCmd's ask(key, def, test, msg): a number, or the old value with a word about it
Task<double> array_ask(Ctx& c, std::string key, double def, bool count, bool length) {
  Ask an;
  an.def = def;
  an.length = length;
  const Answer v = co_await c.number(c.t(key), std::move(an));
  if (count ? whole(v) : (v.kind == Answer::Kind::number && std::isfinite(v.number))) {
    co_return v.number;
  }
  c.print(c.t(count ? "cmd.array.whole" : "cmd.array.number"));
  co_return def;
}

Task<void> array_like(Ctx& c, std::string kind) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  std::vector<Value> src;
  for (const Value& e : current(c, ids.set)) {
    if (type_of(e) != "viewport") {
      src.push_back(e);
    }
  }
  if (kind.empty()) {
    Ask ak;
    ak.options = {c.opt("R", c.t("opt.rectangular")), c.opt("PO", c.t("opt.polar"))};
    ak.def = "R";
    const Answer k = co_await c.keyword(c.t("cmd.array.type"), std::move(ak));
    kind = k.key() == "PO" ? "polar" : "rect";
  }
  ArraySpec a;
  if (kind == "rect") {
    cad::Bounds b;
    for (const Value& e : src) {
      const cad::Bounds eb = cad::entity_bounds(e, [&c](const std::string& n) { return c.doc.blocks().get(n); });
      if (eb.valid()) {
        b = {std::min(b.min_x, eb.min_x), std::min(b.min_y, eb.min_y), std::max(b.max_x, eb.max_x),
             std::max(b.max_y, eb.max_y)};
      }
    }
    const double w = b.valid() ? std::max(b.max_x - b.min_x, 1e-6) : 1;
    const double h = b.valid() ? std::max(b.max_y - b.min_y, 1e-6) : 1;
    a.dx = w * 1.5;  // AutoCAD's starting grid
    a.dy = h * 1.5;
  } else {
    a.rect = false;
    a.center = (co_await c.point(c.t("cmd.array.center"))).p;
  }
  for (;;) {
    c.preview([&src, &a](Pt) {
      auto cp = array_copies(src, a, 3000);
      return cp ? std::move(*cp) : base::Array{};
    });
    c.print(
        a.rect ? c.t("cmd.array.rect.now",
                     {{"cols", fmt(a.cols)}, {"rows", fmt(a.rows)}, {"dx", fmt_len(c, a.dx)}, {"dy", fmt_len(c, a.dy)}})
               : c.t("cmd.array.polar.now", {{"items", fmt(a.items)},
                                             {"fill", fmt(deg(a.fill))},
                                             {"rotate", c.t(a.rotate ? "opt.yes" : "opt.no")}}));
    Ask ao;
    ao.options = a.rect ? std::vector<Option>{c.opt("COU", c.t("opt.count")), c.opt("S", c.t("opt.spacing")),
                                              c.opt("COL", c.t("opt.columns")), c.opt("R", c.t("opt.rows")),
                                              c.opt("X", c.t("opt.exit"))}
                        : std::vector<Option>{c.opt("I", c.t("opt.items")), c.opt("F", c.t("opt.fill")),
                                              c.opt("ROT", c.t("opt.rotateitems")), c.opt("X", c.t("opt.exit"))};
    ao.def = "X";
    const Answer k = co_await c.keyword(c.t("cmd.array.option"), std::move(ao));
    const std::string o = k.key();
    if (o == "X" || o.empty()) {
      break;
    }
    if (o == "COU" || o == "COL") {
      a.cols = co_await array_ask(c, "cmd.array.cols", a.cols, true, false);
    }
    if (o == "COU" || o == "R") {
      a.rows = co_await array_ask(c, "cmd.array.rows", a.rows, true, false);
    }
    if (o == "S" || o == "COL") {
      a.dx = co_await array_ask(c, "cmd.array.dx", a.dx, false, true);
    }
    if (o == "S" || o == "R") {
      a.dy = co_await array_ask(c, "cmd.array.dy", a.dy, false, true);
    }
    if (o == "I") {
      a.items = co_await array_ask(c, "cmd.array.items", a.items, true, false);
    }
    if (o == "F") {
      Ask af;
      af.def = deg(a.fill);
      const Answer v = co_await c.number(c.t("cmd.array.fill"), std::move(af));
      if (v.kind == Answer::Kind::number && std::isfinite(v.number) && v.number != 0 && std::abs(v.number) <= 360) {
        a.fill = rad(v.number);
      } else {
        c.print(c.t("cmd.array.fill.bad"));
      }
    }
    if (o == "ROT") {
      Ask ar;
      ar.options = {c.opt("Y", c.t("opt.yes")), c.opt("N", c.t("opt.no"))};
      ar.def = a.rotate ? "Y" : "N";
      const Answer r = co_await c.keyword(c.t("cmd.array.rotate"), std::move(ar));
      a.rotate = r.key() != "N";
    }
  }
  c.preview(nullptr);
  if ((a.rect ? a.cols * a.rows : a.items) < 2) {
    c.print(c.t("cmd.array.one"));
    co_return;
  }
  const std::optional<base::Array> copies = array_copies(src, a);
  if (!copies) {
    c.print(c.t("cmd.array.toomany"));
    co_return;
  }
  c.doc.change("ARRAY", [&] {
    for (const Value& e : *copies) {
      Value x = e;
      x.erase("id");
      (void)c.doc.add(x);
    }
  });
  c.print(c.words.tn("cmd.array.done", static_cast<double>(copies->size())));
}
Task<void> array_cmd(Ctx& c) {
  co_await array_like(c, "");
}
Task<void> arrayrect_cmd(Ctx& c) {
  co_await array_like(c, "rect");
}
Task<void> arraypolar_cmd(Ctx& c) {
  co_await array_like(c, "polar");
}

// ---- REVCLOUD

std::vector<cad::P2> pts_of(const Value& poly) {
  std::vector<cad::P2> out;
  for (const Value& p : poly.get("pts").as_array()) {
    out.push_back({base::js_to_number(p.get("x")), base::js_to_number(p.get("y"))});
  }
  return out;
}

Task<void> revcloud_cmd(Ctx& c) {
  const Value dim = c.doc.current.get("dim");
  Value st = c.engine.state.get("revcloud");
  if (!st.is_object()) {
    st = base::make_object({{"arc", or_num(dim.get("txt"), 2.5) * or_num(dim.get("scale"), 1) * 4}, {"type", "R"}});
  }
  double arc = base::js_to_number(st.get("arc"));
  std::string type = base::js_string(st.get("type"));
  const auto save = [&c, &arc, &type] {
    c.engine.state.set("revcloud", base::make_object({{"arc", arc}, {"type", type}}));
  };
  save();
  const auto type_word = [&c, &type] {
    if (type == "R") {
      return c.t("opt.rectangular");
    }
    return c.t(type == "P" ? "opt.polygonal" : "opt.freehand");
  };
  c.print(c.t("cmd.rc.now", {{"arc", fmt_len(c, arc)}, {"type", type_word()}}));
  const auto cloud = [&c, &arc](const std::vector<cad::P2>& pts, bool closed) {
    return c.ent(cad::revcloud(pts, closed, arc, false));
  };
  for (;;) {
    Ask ap;
    ap.options = {c.opt("A", c.t("opt.arclength")), c.opt("O", c.t("opt.object")), c.opt("R", c.t("opt.rectangular")),
                  c.opt("P", c.t("opt.polygonal")), c.opt("F", c.t("opt.freehand"))};
    const Answer p = co_await c.point(c.t("cmd.rc.first"), std::move(ap));
    if (p.is("A")) {
      Ask aa;
      aa.def = arc;
      const Answer v = co_await c.distance(c.t("cmd.rc.arc"), std::move(aa));
      if (v.kind == Answer::Kind::number && v.number > 0) {
        arc = v.number;
        save();
      }
      continue;
    }
    if (p.is("R") || p.is("P") || p.is("F")) {
      type = p.option;
      save();
      c.print(c.t("cmd.rc.now", {{"arc", fmt_len(c, arc)}, {"type", type_word()}}));
      continue;
    }
    if (p.is("O")) {
      for (;;) {
        const Answer pick = co_await c.entity(c.t("cmd.rc.object"));
        const Value* ep = c.doc.entities().get(pick.id);
        const std::optional<cad::Path> path = ep != nullptr ? cad::path_of(*ep) : std::nullopt;
        if (!path || !c.doc.editable(*ep)) {
          c.print(c.t("cmd.rc.notobject"));
          continue;
        }
        const Value e = *ep;
        IdSet sel;
        sel.add(pick.id);
        c.host.set_selection(std::move(sel));
        Ask ay;
        ay.options = {c.opt("Y", c.t("opt.yes")), c.opt("N", c.t("opt.no"))};
        ay.def = "N";
        const Answer k = co_await c.keyword(c.t("cmd.rc.reverse"), std::move(ay));
        c.host.set_selection({});
        Value made = cad::revcloud(path->pts, path->closed, arc, k.key() == "Y");
        made.set("layer", e.get("layer"));
        const Value col = e.get("color");
        made.set("color", col.is_undefined() || col.is_null() ? Value(nullptr) : col);
        c.doc.change("REVCLOUD", [&] {  // as AutoCAD: the object becomes the cloud
          c.doc.remove(id_of(e));
          (void)c.doc.add(made);
        });
        c.print(c.t("cmd.rc.done"));
        co_return;
      }
    }
    const Pt p0 = p.p;
    if (type == "R") {
      c.preview([p0, cloud](Pt cur) {
        const Value r = rect(p0, cur);
        if (std::abs(cur.x - p0.x) > 1e-9 && std::abs(cur.y - p0.y) > 1e-9) {
          return base::Array{cloud(pts_of(r), true)};
        }
        return base::Array{r};
      });
      Ask aq;
      aq.base = p0;
      const Pt q = (co_await c.point(c.t("cmd.rc.corner"), std::move(aq))).p;
      c.preview(nullptr);
      if (std::abs(q.x - p0.x) < 1e-9 || std::abs(q.y - p0.y) < 1e-9) {
        c.print(c.t("cmd.rc.flat"));
        co_return;
      }
      (void)c.add(cloud(pts_of(rect(p0, q)), true));
    } else if (type == "P") {
      std::vector<cad::P2> pts{{p0.x, p0.y}};
      for (;;) {
        c.preview([&pts, cloud](Pt cur) {
          if (pts.size() >= 2) {
            std::vector<cad::P2> all = pts;
            all.push_back({cur.x, cur.y});
            return base::Array{cloud(all, true)};
          }
          return base::Array{line_e(pts[0].x, pts[0].y, cur.x, cur.y)};
        });
        Ask aq;
        aq.base = Pt{pts.back().x, pts.back().y};
        aq.none = true;
        if (pts.size() > 1) {
          aq.options = {c.opt("U", c.t("opt.undo"))};
        }
        const Answer q = co_await c.point(c.t("cmd.rc.next"), std::move(aq));
        if (!q.truthy()) {
          break;
        }
        if (q.is("U")) {
          pts.pop_back();
          continue;
        }
        if (base::js_hypot(q.p.x - pts.back().x, q.p.y - pts.back().y) > 1e-12) {
          pts.push_back({q.p.x, q.p.y});
        }
      }
      c.preview(nullptr);
      if (pts.size() < 3) {
        c.print(c.t("cmd.area.few"));
        co_return;
      }
      (void)c.add(cloud(pts, true));
    } else {
      // freehand: the path follows the pointer; a point is taken every arc length
      std::vector<cad::P2> pts{{p0.x, p0.y}};
      c.preview([&pts, &arc, cloud](Pt cur) {
        if (base::js_hypot(cur.x - pts.back().x, cur.y - pts.back().y) >= arc) {
          pts.push_back({cur.x, cur.y});
        }
        if (pts.size() < 2) {
          return base::Array{};
        }
        std::vector<cad::P2> all = pts;
        all.push_back({cur.x, cur.y});
        return base::Array{cloud(all, false)};
      });
      Ask aq;
      aq.none = true;
      const Answer q = co_await c.point(c.t("cmd.rc.freehand"), std::move(aq));
      c.preview(nullptr);
      if (q.truthy()) {
        pts.push_back({q.p.x, q.p.y});
      }
      if (pts.size() < 3) {
        c.print(c.t("cmd.rc.short"));
        co_return;
      }
      (void)c.add(cloud(pts, true));  // as AutoCAD, a freehand cloud closes back to its start
    }
    c.print(c.t("cmd.rc.done"));
    co_return;
  }
}

// ---- MATCHPROP

struct MatchKey {
  const char* key;
  const char* field;
  const char* label;
};
constexpr std::array<MatchKey, 5> kMatchKeys{{{"C", "color", "opt.color"},
                                              {"LA", "layer", "opt.layer"},
                                              {"T", "text", "opt.text"},
                                              {"D", "dim", "opt.dimension"},
                                              {"H", "hatch", "opt.hatch"}}};

/// commands.js matchProps(src, dst, on)
Value match_props(const Value& src, const Value& dst, const Value& on) {
  Value out = dst;
  const auto is_on = [&on](const char* k) { return on.get(k).truthy(); };
  if (is_on("layer")) {
    out.set("layer", src.get("layer"));
  }
  if (is_on("color")) {
    const Value col = src.get("color");
    out.set("color", col.is_undefined() || col.is_null() ? Value(nullptr) : col);
  }
  const std::string st = type_of(src);
  const std::string dt = type_of(dst);
  if (is_on("text") && st == "text" && dt == "text") {
    out.set("h", src.get("h"));
    out.set("wf", src.get("wf").truthy() ? src.get("wf") : Value(1));
  }
  if (is_on("dim") && st == "dim" && dt == "dim" && src.get("st").truthy()) {
    out.set("st", src.get("st"));
    out.erase("block");
  }
  if (is_on("hatch") && st == "hatch" && dt == "hatch") {
    out.set("solid", src.get("solid").truthy());
    for (const char* k : {"pattern", "pscale", "pangle", "piso", "patlines"}) {
      const Value v = src.get(k);
      if (!v.is_undefined()) {
        out.set(k, v);
      } else {
        out.erase(k);
      }
    }
  }
  return out;
}

Task<void> matchprop_cmd(Ctx& c) {
  Value on = c.engine.state.get("matchOn");
  if (!on.is_object()) {
    on = base::make_object({{"color", true}, {"layer", true}, {"text", true}, {"dim", true}, {"hatch", true}});
    c.engine.state.set("matchOn", on);
  }
  const Answer pick = co_await c.entity(c.t("cmd.ma.source"));
  const Value* sp = c.doc.entities().get(pick.id);
  if (sp == nullptr) {
    co_return;
  }
  const Value src = *sp;
  IdSet sel;
  sel.add(pick.id);
  c.host.set_selection(std::move(sel));
  const auto say = [&c] {
    const Value o = c.engine.state.get("matchOn");
    std::string list;
    for (const MatchKey& m : kMatchKeys) {
      if (o.get(m.field).truthy()) {
        list += (list.empty() ? "" : " ") + c.t(m.label);
      }
    }
    c.print(c.t("cmd.ma.active", {{"list", list.empty() ? c.t("cmd.ma.nothing") : list}}));
  };
  say();
  for (;;) {
    Ask as;
    as.fresh = true;
    as.options = {c.opt("S", c.t("opt.settings"))};
    Answer ids = co_await c.select(c.t("cmd.ma.dest"), std::move(as));
    if (ids.is("S")) {
      for (;;) {
        Ask ak;
        for (const MatchKey& m : kMatchKeys) {
          ak.options.push_back(c.opt(m.key, c.t(m.label)));
        }
        ak.none = true;
        const Answer k = co_await c.keyword(c.t("cmd.ma.toggle"), std::move(ak));
        if (k.kind != Answer::Kind::option) {
          break;
        }
        for (const MatchKey& m : kMatchKeys) {
          if (k.option == m.key) {
            Value o = c.engine.state.get("matchOn");
            o.set(m.field, !o.get(m.field).truthy());
            c.engine.state.set("matchOn", o);
          }
        }
        say();
      }
      continue;
    }
    ids.set.erase(id_of(src));
    if (ids.set.empty()) {
      co_return;
    }
    std::size_t n = 0;
    const Value now_on = c.engine.state.get("matchOn");
    c.doc.change("MATCHPROP", [&] {
      for (const Id id : ids.set) {
        const Value* d = c.doc.entities().get(id);
        if (d != nullptr && c.doc.editable(*d)) {
          const Value nd = match_props(src, *d, now_on);
          (void)c.doc.update(id, nd);
          ++n;
        }
      }
    });
    c.print(c.words.tn("cmd.ma.done", static_cast<double>(n)));
    co_return;
  }
}

// ---- MTEXT, MLEADER, TEXTEDIT

/// commands.js textWords(c, e) without an in-place editor: lines typed at the command line (an empty line ends)
Task<std::optional<std::string>> text_words(Ctx& c) {
  std::vector<std::string> lines;
  for (;;) {
    Ask a;
    a.raw = true;
    const Answer line = co_await c.string(lines.empty() ? c.t("cmd.mtext.enter") : c.t("cmd.mtext.more"), std::move(a));
    if (line.kind != Answer::Kind::text || line.text.empty()) {
      break;
    }
    lines.push_back(line.text);
  }
  if (lines.empty()) {
    co_return std::nullopt;
  }
  std::string s;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    s += (i != 0 ? "\n" : "") + lines[i];
  }
  co_return s;
}

double ucs_angle(const Ctx& c) {
  return c.doc.in_model() && c.doc.ucs.truthy() ? or_num(c.doc.ucs.get("ang"), 0) : 0;
}

struct Just {
  const char* key;
  const char* ha;
  const char* va;
};
constexpr std::array<Just, 9> kJust{{{"TL", "left", "top"},
                                     {"TC", "center", "top"},
                                     {"TR", "right", "top"},
                                     {"ML", "left", "middle"},
                                     {"MC", "center", "middle"},
                                     {"MR", "right", "middle"},
                                     {"BL", "left", "bottom"},
                                     {"BC", "center", "bottom"},
                                     {"BR", "right", "bottom"}}};
const Just* just_of(const std::string& k) {
  for (const Just& j : kJust) {
    if (k == j.key) {
      return &j;
    }
  }
  return nullptr;
}

Task<void> mtext_cmd(Ctx& c) {
  double h = base::js_to_number(c.doc.current.get("textHeight"));
  double ls = num_or(c.engine.state.get("mtextLs"), 1);
  double rot = norm_angle(ucs_angle(c));  // along the UCS (DR.8)
  const Value jv = c.engine.state.get("mtextJust");
  std::string just = jv.truthy() ? base::js_string(jv) : "TL";
  std::optional<double> width;
  const Pt a = (co_await c.point(c.t("cmd.mtext.first"))).p;
  const auto u = [&rot] { return Pt{base::js_cos(rot), base::js_sin(rot)}; };
  const auto n = [&rot] { return Pt{-base::js_sin(rot), base::js_cos(rot)}; };
  struct Frame {
    double left, right, top, bottom;
  };
  const auto frame = [a, u, n](Pt b) {  // the box from corner a to b, in the text's own direction
    const Pt d{b.x - a.x, b.y - a.y};
    const double lx = d.x * u().x + d.y * u().y;
    const double ly = d.x * n().x + d.y * n().y;
    return Frame{std::min(0.0, lx), std::max(0.0, lx), std::max(0.0, ly), std::min(0.0, ly)};
  };
  const auto at_of = [a, u, n](double x, double y) {
    return Pt{a.x + u().x * x + n().x * y, a.y + u().y * x + n().y * y};
  };
  std::optional<Pt> b;
  for (;;) {
    c.preview([frame, at_of](Pt cur) {
      const Frame f = frame(cur);
      base::Array pts;
      for (const auto& [x, y] : {std::pair{f.left, f.top}, std::pair{f.right, f.top}, std::pair{f.right, f.bottom},
                                 std::pair{f.left, f.bottom}}) {
        const Pt p = at_of(x, y);
        pts.push_back(vtx(p.x, p.y));
      }
      return base::Array{base::make_object({{"type", "poly"}, {"closed", true}, {"pts", std::move(pts)}})};
    });
    Ask ap;
    ap.base = a;
    ap.options = {c.opt("H", c.t("opt.height")), c.opt("J", c.t("opt.justify")), c.opt("L", c.t("opt.linespacing")),
                  c.opt("R", c.t("opt.rotation")), c.opt("W", c.t("opt.width"))};
    const Answer p = co_await c.point(c.t("cmd.mtext.corner"), std::move(ap));
    if (p.is("H")) {
      Ask ah;
      ah.base = a;
      ah.def = h;
      const Answer v = co_await c.distance(c.t("cmd.mtext.height"), std::move(ah));
      if (v.kind == Answer::Kind::number && v.number > 0) {
        h = v.number;
      }
      continue;
    }
    if (p.is("J")) {
      Ask aj;
      for (const Just& j : kJust) {
        aj.options.push_back(c.opt(j.key, j.key));
      }
      aj.def = just;
      const Answer k = co_await c.keyword(c.t("cmd.mtext.just"), std::move(aj));
      if (just_of(k.key()) != nullptr) {
        just = k.key();
        c.engine.state.set("mtextJust", just);
      }
      continue;
    }
    if (p.is("L")) {
      Ask al;
      al.def = ls;
      const Answer f = co_await c.number(c.t("cmd.mtext.ls"), std::move(al));
      if (f.kind == Answer::Kind::number && f.number >= 0.25 && f.number <= 4) {
        ls = f.number;
        c.engine.state.set("mtextLs", ls);
      } else {
        c.print(c.t("cmd.mtext.ls.bad"));
      }
      continue;
    }
    if (p.is("R")) {
      Ask ar;
      ar.base = a;
      ar.def = rot;
      ar.ucs = true;
      const Answer r = co_await c.angle(c.t("cmd.mtext.rot"), std::move(ar));
      rot = norm_angle(r.kind == Answer::Kind::number && !std::isnan(r.number) ? r.number : 0);
      continue;
    }
    if (p.is("W")) {
      Ask aw;
      aw.base = a;
      aw.def = 0;
      const Answer w = co_await c.distance(c.t("cmd.mtext.width"), std::move(aw));
      width = w.kind == Answer::Kind::number && !std::isnan(w.number) ? w.number : 0;
      break;
    }
    b = p.p;
    break;
  }
  c.preview(nullptr);
  const Just* j = just_of(just);
  const std::string ha = j != nullptr ? j->ha : "left";
  const std::string va = j != nullptr ? j->va : "top";
  Pt at = a;
  double ww = width ? *width : 0;
  if (b) {
    const Frame f = frame(*b);
    ww = f.right - f.left;
    double lx = (f.left + f.right) / 2;
    if (ha == "left") {
      lx = f.left;
    } else if (ha == "right") {
      lx = f.right;
    }
    double ly = (f.top + f.bottom) / 2;
    if (va == "top") {
      ly = f.top;
    } else if (va == "bottom") {
      ly = f.bottom;
    }
    at = at_of(lx, ly);
  }
  Value e = c.ent(base::make_object({{"type", "text"},
                                     {"x", at.x},
                                     {"y", at.y},
                                     {"h", h},
                                     {"rot", rot},
                                     {"s", ""},
                                     {"ha", ha},
                                     {"va", va},
                                     {"wf", 1}}));
  if (ww > 1e-9) {
    e.set("ww", ww);
  }
  if (ls != 1) {
    e.set("ls", ls);
  }
  const std::optional<std::string> words = co_await text_words(c);
  if (!words || !has_words(*words)) {
    c.print(c.t("cmd.mtext.empty"));
    co_return;
  }
  e.set("s", *words);
  c.doc.current.set("textHeight", e.get("h"));
  (void)c.add(e);
}

Task<void> mleader_cmd(Ctx& c) {
  const Value dim = c.doc.current.get("dim");
  const double k = or_num(dim.get("scale"), 1);
  Value ml = c.engine.state.get("mleader");
  if (!ml.is_object()) {
    ml = Value::object();
  }
  const double dasz = base::js_to_number(dim.get("asz")) * k;
  double asz = num_or(ml.get("asz"), dasz);
  double dogleg = num_or(ml.get("dogleg"), dasz);
  const Value gv = dim.get("gap");
  const double gap = (gv.is_undefined() || gv.is_null() ? 0.625 : base::js_to_number(gv)) * k;
  c.engine.state.set("mleader", ml);
  Pt tip;
  for (;;) {
    Ask ap;
    ap.options = {c.opt("A", c.t("opt.arrowsize")), c.opt("L", c.t("opt.landing"))};
    const Answer p = co_await c.point(c.t("cmd.mld.tip"), std::move(ap));
    if (p.is("A") || p.is("L")) {
      const bool arrow = p.is("A");
      Ask ad;
      ad.def = arrow ? asz : dogleg;
      const Answer v = co_await c.distance(c.t(arrow ? "cmd.mld.asz" : "cmd.mld.dogleg"), std::move(ad));
      if (v.kind == Answer::Kind::number && v.number >= 0) {
        (arrow ? asz : dogleg) = v.number;
        ml.set(arrow ? "asz" : "dogleg", v.number);
        c.engine.state.set("mleader", ml);
      }
      continue;
    }
    tip = p.p;
    break;
  }
  const double th = base::js_to_number(dim.get("txt")) * k;
  const auto make = [&c, tip, th, asz, dogleg, gap](Pt to) {
    return c.ent(base::make_object({{"type", "mleader"},
                                    {"pts", base::Array{base::make_object({{"x", tip.x}, {"y", tip.y}}),
                                                        base::make_object({{"x", to.x}, {"y", to.y}})}},
                                    {"dir", to.x >= tip.x ? 1 : -1},
                                    {"s", ""},
                                    {"h", th},
                                    {"st", base::make_object({{"asz", asz}, {"dogleg", dogleg}, {"gap", gap}})}}));
  };
  c.preview([tip, make](Pt cur) {
    return base::js_hypot(cur.x - tip.x, cur.y - tip.y) > 1e-12 ? base::Array{make(cur)} : base::Array{};
  });
  Ask al;
  al.base = tip;
  const Pt land = (co_await c.point(c.t("cmd.mld.landing"), std::move(al))).p;
  c.preview(nullptr);
  if (base::js_hypot(land.x - tip.x, land.y - tip.y) < 1e-12) {
    c.print(c.t("cmd.mld.same"));
    co_return;
  }
  Value made = make(land);
  if (const std::optional<std::string> words = co_await text_words(c)) {
    made.set("s", *words);
  }
  (void)c.add(made);
  if (!has_words(base::js_string(made.get("s")))) {
    c.print(c.t("cmd.mld.notext"));
  }
}

Task<void> textedit_cmd(Ctx& c) {
  std::vector<Id> pre;
  for (const Id id : c.host.selection()) {
    const Value* e = c.doc.entities().get(id);
    if (e != nullptr && (type_of(*e) == "text" || type_of(*e) == "mleader") && c.doc.editable(*e)) {
      pre.push_back(id);
    }
  }
  const bool once = !pre.empty();  // started on selected text (a double-click): edit that, then end
  c.host.set_selection({});
  std::size_t next = 0;
  for (;;) {
    if (once && next >= pre.size()) {
      co_return;
    }
    Id id = 0;
    if (next < pre.size()) {
      id = pre[next++];
    } else {
      Ask a;
      a.none = true;
      const Answer pick = co_await c.entity(c.t("cmd.textedit.pick"), std::move(a));
      if (!pick.truthy() || pick.kind != Answer::Kind::entity) {
        co_return;
      }
      id = pick.id;
      const Value* e = c.doc.entities().get(id);
      if (e == nullptr || (type_of(*e) != "text" && type_of(*e) != "mleader")) {
        c.print(c.t("cmd.textedit.only"));
        continue;
      }
    }
    const Value* ep = c.doc.entities().get(id);
    if (ep == nullptr) {
      continue;
    }
    IdSet sel;
    sel.add(id);
    c.host.set_selection(std::move(sel));
    Ask as;
    as.def = ep->get("s");
    as.raw = true;
    const Answer v = co_await c.string(c.t("cmd.textedit.enter"), std::move(as));
    c.host.set_selection({});
    std::string s;
    if (v.kind == Answer::Kind::text) {
      s = v.text;
    } else if (v.kind == Answer::Kind::typed) {
      s = base::js_string(v.typed);
    }
    if (has_words(s)) {
      const Value* now = c.doc.entities().get(id);
      if (now == nullptr) {
        continue;
      }
      Value nx = *now;
      nx.set("s", s);
      const Value ww = nx.get("ww");
      if (!ww.is_number() || !(ww.as_number() > 1e-9)) {
        nx.erase("ww");
      }
      const Value l = nx.get("ls");
      if (!l.truthy() || (l.is_number() && l.as_number() == 1)) {
        nx.erase("ls");
      }
      c.doc.change("TEXTEDIT", [&] { (void)c.doc.update(id, nx); });
    }
  }
}

}  // namespace

void add_annotate(std::vector<CommandDef>& t) {
  t.push_back(def("ARRAY", {"AR"}, array_cmd, "cmd.array.help"));
  t.push_back(def("ARRAYRECT", {}, arrayrect_cmd, "cmd.arrayrect.help"));
  t.push_back(def("ARRAYPOLAR", {}, arraypolar_cmd, "cmd.arraypolar.help"));
  t.push_back(def("REVCLOUD", {"RC"}, revcloud_cmd, "cmd.rc.help"));
  t.push_back(def("MATCHPROP", {"MA", "PAINTER"}, matchprop_cmd, "cmd.ma.help"));
  t.push_back(def("MTEXT", {"MT", "T"}, mtext_cmd, "cmd.mtext.help"));
  t.push_back(def("MLEADER", {"MLD", "MLEAD"}, mleader_cmd, "cmd.mld.help"));
  t.push_back(def("TEXTEDIT", {"ED", "DDEDIT"}, textedit_cmd, "cmd.textedit.help"));
}

}  // namespace cc::cmd::detail
