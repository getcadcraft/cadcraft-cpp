// SPDX-License-Identifier: Apache-2.0
// commands.js (CadCraft 0.5) in C++, M5.7b: the clipboard — COPYCLIP, CUTCLIP, COPYBASE, PASTECLIP, PASTEORIG
// (clipboard.js). The clipboard is the host's; a script host keeps it in memory as 0.5's does without a browser.
#include <string>
#include <utility>
#include <vector>

#include "cc/cad/geom.hpp"
#include "cc/cmd/clipboard.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

/// toClipboard(c, ids, base): the objects onto the clipboard; true when something was copied
bool to_clipboard(Ctx& c, const IdSet& ids, const std::optional<std::pair<double, double>>& base_pt) {
  const std::optional<Value> clip = make_clip(c.doc, ids.ids(), base_pt, c.host.now_ms());
  if (!clip) {
    c.print(c.t(!ids.empty() ? "cmd.copy.nothing_there_can_copied" : "cmd.copy.nothing_selected"));
    return false;
  }
  const bool shared = c.host.clipboard_shared();
  const std::size_t n = clip->get("entities").as_array().size();
  const std::string objects = c.words.tn("cmd.n.objects", static_cast<double>(n), {});
  std::size_t named = 0;
  for (const Value& b : clip->get("blocks").as_array()) {
    if (!base::js_string(b.get("name")).starts_with("*")) {
      ++named;
    }
  }
  std::string line =
      clip->get("blocks").as_array().empty()
          ? c.t("cmd.copy.copied", {{"objects", objects}})
          : c.t("cmd.copy.copied.blocks",
                {{"objects", objects}, {"blocks", c.words.tn("cmd.n.blockdefs", static_cast<double>(named), {})}});
  if (const double skipped = base::js_to_number(clip->get("skipped")); skipped > 0) {
    line += " " + c.words.tn("cmd.copy.viewports_stayed", skipped, {});
  }
  if (!shared) {
    line += " " + c.t("cmd.copy.too_big");
  }
  c.host.clipboard().put(*clip);
  c.print(line);
  return true;
}

Task<void> copyclip_cmd(Ctx& c) {
  const Answer a = co_await c.select(c.t("cmd.copyclip.select_objects_copy"));
  if (a.set.empty()) {
    co_return;
  }
  (void)to_clipboard(c, a.set, std::nullopt);
}

Task<void> cutclip_cmd(Ctx& c) {
  const Answer a = co_await c.select(c.t("cmd.cutclip.select_objects_cut"));
  if (a.set.empty()) {
    co_return;
  }
  if (!to_clipboard(c, a.set, std::nullopt)) {
    co_return;
  }
  c.doc.change("CUTCLIP", [&] {
    for (const Id id : a.set) {
      const Value* e = c.doc.entities().get(id);
      if (e != nullptr && !(e->get("type") == Value("viewport"))) {
        c.doc.remove(id);
      }
    }
  });
  c.host.set_selection({});
}

Task<void> copybase_cmd(Ctx& c) {
  const Pt base = (co_await c.point(c.t("cmd.stretch.base"))).p;
  const Answer a = co_await c.select(c.t("cmd.copyclip.select_objects_copy"));
  if (a.set.empty()) {
    co_return;
  }
  (void)to_clipboard(c, a.set, std::pair{base.x, base.y});
}

/// fromClipboard(c): the clipboard's contents, or nothing (and why)
std::optional<Value> from_clipboard(Ctx& c) {
  std::optional<Value> got = c.host.clipboard().get();
  if (!got) {
    c.print(c.t("cmd.copybase.clipboard_empty_copy_objects"));
  }
  return got;
}

void finish_paste(Ctx& c, const Value& clip, double dx, double dy, const std::string& label) {
  PasteResult r;
  c.doc.change(label, [&] { r = paste_clip(c.doc, clip, dx, dy, c.words); });
  IdSet sel;
  for (const Id id : r.ids) {
    sel.add(id);
  }
  c.host.set_selection(std::move(sel));
  const std::string objects = c.words.tn("cmd.n.objects", static_cast<double>(r.ids.size()), {});
  if (!r.layers.empty()) {
    std::string list;
    for (std::size_t i = 0; i < r.layers.size() && i < 12; ++i) {
      list += (i ? ", " : "") + r.layers[i];
    }
    if (r.layers.size() > 12) {
      list += " \xE2\x80\xA6";
    }
    c.print(c.words.tn("cmd.paste.pasted.layers", static_cast<double>(r.layers.size()),
                       {{"objects", objects}, {"layers", list}}));
  } else {
    c.print(c.t("cmd.paste.pasted", {{"objects", objects}}));
  }
  std::string renamed;
  for (const auto& [a, b] : r.renamed) {
    if (!a.starts_with("*")) {
      renamed += (renamed.empty() ? "" : ", ") + c.t("cmd.renamed_as", {{"from", a}, {"to", b}});
    }
  }
  if (!renamed.empty()) {
    c.print(c.t("cmd.paste.renamed", {{"list", renamed}}));
  }
  if (!r.note.empty()) {
    c.print(r.note);
  }
}

Task<void> pasteclip_cmd(Ctx& c) {
  const std::optional<Value> clip = from_clipboard(c);
  if (!clip) {
    co_return;
  }
  const auto [bx, by] = clip_base_point(*clip, c.host.paste_base());
  base::Array look = paste_preview(*clip);
  c.engine.last_point = Pt{bx, by};
  c.preview([shown = std::move(look), bx, by](Pt cur) {
    base::Array out;
    const cad::Mat2 m{1, 0, 0, 1, cur.x - bx, cur.y - by};
    for (const Value& e : shown) {
      out.push_back(cad::tx_entity(e, m));
    }
    return out;
  });
  const Pt p = (co_await c.point(c.t("cmd.pasteclip.specify_insertion_point"))).p;
  c.preview(nullptr);
  finish_paste(c, *clip, p.x - bx, p.y - by, "PASTECLIP");
}

Task<void> pasteorig_cmd(Ctx& c) {
  const std::optional<Value> clip = from_clipboard(c);
  if (!clip) {
    co_return;
  }
  finish_paste(c, *clip, 0, 0, "PASTEORIG");
}

}  // namespace

void add_clipboard(std::vector<CommandDef>& t) {
  CommandDef copy = def("COPYCLIP", {}, copyclip_cmd, "cmd.copyclip.help");
  copy.keep_selection = true;
  t.push_back(std::move(copy));
  t.push_back(def("CUTCLIP", {}, cutclip_cmd, "cmd.cutclip.cut_objects_clipboard_ctrl"));
  CommandDef base = def("COPYBASE", {}, copybase_cmd, "cmd.copybase.copy_objects_clipboard_base");
  base.keep_selection = true;
  t.push_back(std::move(base));
  CommandDef paste = def("PASTECLIP", {}, pasteclip_cmd, "cmd.pasteclip.paste_clipboard_follows_cursor");
  paste.keep_selection = true;
  t.push_back(std::move(paste));
  CommandDef orig = def("PASTEORIG", {}, pasteorig_cmd, "cmd.pasteorig.paste_clipboard_coordinates_was");
  orig.keep_selection = true;
  t.push_back(std::move(orig));
}

}  // namespace cc::cmd::detail
