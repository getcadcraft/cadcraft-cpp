// SPDX-License-Identifier: Apache-2.0
// commands.js's modify commands (CadCraft 0.5) in C++: ERASE, MOVE, COPY, ROTATE, SCALE, MIRROR, UNDO, REDO.
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
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
Mat m_scale(double s) {
  return {s, 0, 0, s, 0, 0};
}
/// mAbout(p, m): m about the point p
Mat m_about(Pt p, const Mat& m) {
  return m_mul(m_translate(p.x, p.y), m_mul(m, m_translate(-p.x, -p.y)));
}
/// mMirror(p, q): the reflection across the line p → q
Mat m_mirror(Pt p, Pt q) {
  const double a = angle_of(p, q);
  const double c = base::js_cos(2 * a);
  const double s = base::js_sin(2 * a);
  return m_about(p, {c, s, s, -c, 0, 0});
}

/// The selected objects as they are now.
std::vector<Value> sources(const Ctx& c, const IdSet& ids) {
  std::vector<Value> out;
  for (const Id id : ids) {
    if (const Value* e = c.doc.entities().get(id)) {
      out.push_back(*e);
    }
  }
  return out;
}

/// commands.js apply(c, src, m, copy): one undo step that moves (or copies) every object through m.
void apply(Ctx& c, const std::vector<Value>& src, const Mat& m, bool copy) {
  c.doc.change(copy ? "COPY" : "MOVE", [&] {
    for (const Value& e : src) {
      const Id id = id_of(e);
      const Value* now = c.doc.entities().get(id);
      Value t = cad::tx_entity(now != nullptr ? *now : e, m);
      if (copy) {
        t.erase("id");
        (void)c.doc.add(t);
      } else {
        (void)c.doc.update(id, t);
      }
    }
  });
}

Task<void> erase_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  c.doc.change("ERASE", [&] {
    for (const Id id : ids.set) {
      c.doc.remove(id);
    }
  });
  c.host.set_selection({});
  c.print(std::to_string(ids.set.size()) + " erased");
}

Task<void> move_like(Ctx& c, bool copy) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Value> src = sources(c, ids.set);
  Ask ab;
  ab.options = {c.opt("D", "Displacement")};
  const Answer b = co_await c.point(c.t("cmd.stretch.base"), std::move(ab));
  if (b.is("D")) {
    const Answer d = co_await c.point(c.t("cmd.stretch.disp"));
    apply(c, src, m_translate(d.p.x, d.p.y), copy);
    co_return;
  }
  const Pt base = b.p;
  for (;;) {
    c.preview([&src, base](Pt cur) {
      base::Array out;
      for (const Value& e : src) {
        out.push_back(cad::tx_entity(e, m_translate(cur.x - base.x, cur.y - base.y)));
      }
      return out;
    });
    Ask ap;
    ap.base = base;
    ap.none = true;
    if (copy) {
      ap.options = {c.opt("U", "Undo")};
    }
    const Answer p = co_await c.point(c.t("cmd.mea.p2"), std::move(ap));
    if (!p.truthy()) {
      co_return;
    }
    if (p.is("U")) {
      undo_inside(c);
      continue;
    }
    apply(c, src, m_translate(p.p.x - base.x, p.p.y - base.y), copy);
    if (!copy) {
      co_return;
    }
  }
}

Task<void> move_cmd(Ctx& c) {
  co_await move_like(c, false);
}
Task<void> copy_cmd(Ctx& c) {
  co_await move_like(c, true);
}

Task<void> rotate_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Value> src = sources(c, ids.set);
  const Pt base = (co_await c.point(c.t("cmd.stretch.base"))).p;
  c.preview([&src, base](Pt cur) {
    base::Array out;
    for (const Value& e : src) {
      out.push_back(cad::tx_entity(e, m_about(base, m_rotate(angle_of(base, cur)))));
    }
    return out;
  });
  Ask aa;
  aa.base = base;
  const Value last = c.engine.state.get("lastRotation");
  aa.def = last.is_undefined() || last.is_null() ? Value(0) : last;
  const Answer a = co_await c.angle(c.t("cmd.mtext.rot"), std::move(aa));
  if (a.kind != Answer::Kind::number) {
    co_return;
  }
  c.engine.state.set("lastRotation", a.number);
  apply(c, src, m_about(base, m_rotate(a.number)), false);
}

Task<void> scale_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Value> src = sources(c, ids.set);
  const Pt base = (co_await c.point(c.t("cmd.stretch.base"))).p;
  c.preview([&src, base](Pt cur) {
    const double s = std::max(dist(base, cur), 1e-9);
    base::Array out;
    for (const Value& e : src) {
      out.push_back(cad::tx_entity(e, m_about(base, m_scale(s))));
    }
    return out;
  });
  Ask as;
  as.base = base;
  const Answer s = co_await c.distance(c.t("cmd.scale.specify_scale_factor"), std::move(as));
  if (!s.truthy()) {
    co_return;
  }
  apply(c, src, m_about(base, m_scale(s.number)), false);
}

Task<void> mirror_cmd(Ctx& c) {
  const Answer ids = co_await c.select();
  if (ids.kind != Answer::Kind::set || ids.set.empty()) {
    co_return;
  }
  const std::vector<Value> src = sources(c, ids.set);
  const Pt a = (co_await c.point(c.t("cmd.mirror.specify_first_point_mirror"))).p;
  c.preview([&src, a](Pt cur) {
    base::Array out;
    if (dist(a, cur) > 1e-12) {
      for (const Value& e : src) {
        out.push_back(cad::tx_entity(e, m_mirror(a, cur)));
      }
    }
    return out;
  });
  Ask ab;
  ab.base = a;
  const Pt b = (co_await c.point(c.t("cmd.mirror.specify_second_point_mirror"), std::move(ab))).p;
  if (dist(a, b) < 1e-12) {
    co_return;
  }
  c.preview(nullptr);
  Ask ak;
  ak.options = {c.opt("Y", "Yes"), c.opt("N", "No")};
  ak.def = "N";
  const Answer k = co_await c.keyword(c.t("cmd.mirror.erase_source_objects"), std::move(ak));
  const bool erase = k.key() == "Y";
  // constraints come along in the JS (ckeep.js); the fork keeps none yet (docs/gaps.md)
  apply(c, src, m_mirror(a, b), !erase);
}

Task<void> undo_cmd(Ctx& c) {
  const auto l = c.doc.undo();
  c.print(l ? "Undid " + *l : c.t("cmd.undo.nothing_undo"));
  co_return;
}

Task<void> redo_cmd(Ctx& c) {
  const auto l = c.doc.redo();
  c.print(l ? "Redid " + *l : c.t("cmd.redo.nothing_redo"));
  co_return;
}

}  // namespace

/// commands.js undoInside(c): undo the command's last step (U at a prompt).
void undo_inside(Ctx& c) {
  if (c.doc.mark() > c.engine.cmd_mark) {
    (void)c.doc.undo();
    c.doc.clear_redo();
  } else {
    c.print(c.t("cmd.extend.nothing_undo_command"));
  }
}

void add_modify(std::vector<CommandDef>& t) {
  t.push_back(def("ERASE", {"E", "DELETE"}, erase_cmd, "cmd.erase.remove_objects"));
  t.push_back(def("MOVE", {"M"}, move_cmd, "cmd.move.move_objects_base_point"));
  t.push_back(def("COPY", {"CO", "CP"}, copy_cmd, "cmd.copy.copy_objects_many_times"));
  t.push_back(def("ROTATE", {"RO"}, rotate_cmd, "cmd.rotate.rotate_objects_about_base"));
  t.push_back(def("SCALE", {"SC"}, scale_cmd, "cmd.scale.scale_objects_about_base"));
  t.push_back(def("MIRROR", {"MI"}, mirror_cmd, "cmd.mirror.mirror_objects_across_line"));
  t.push_back(def("UNDO", {"U"}, undo_cmd, "cmd.undo.undo_last_command", true));
  t.push_back(def("REDO", {}, redo_cmd, "cmd.redo.redo_what_was_undone", true));
}

const std::vector<CommandDef>& command_table() {
  static const std::vector<CommandDef> kTable = [] {
    std::vector<CommandDef> t;
    add_draw(t);
    add_modify(t);
    add_trim(t);
    add_reshape(t);
    add_corner(t);
    add_annotate(t);
    add_hatch(t);
    add_dims(t);
    add_blocks(t);
    add_inquiry(t);
    add_select(t);
    add_views(t);
    return t;
  }();
  return kTable;
}

}  // namespace cc::cmd::detail
