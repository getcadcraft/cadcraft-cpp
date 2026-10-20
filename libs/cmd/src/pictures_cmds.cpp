// SPDX-License-Identifier: Apache-2.0
// pictures-cmd.js (CadCraft 0.5 WL.14) in C++: IMAGEATTACH (IAT) a PNG or JPEG placed by its lower-left corner, width
// and angle; IMAGE (IM) the files a drawing's pictures use; IMAGECLIP (ICL) a picture's clip — each one undo step. The
// files come from the host's picker (Ctx::files → Host::pick_pictures); registered with what cadcraft.js adds when it
// loads.
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/js.hpp"
#include "cc/cad/edit.hpp"
#include "cc/cad/pictures.hpp"
#include "cc/doc/pictures.hpp"
#include "commands.hpp"

namespace cc::cmd::detail {

namespace {

std::optional<double> number_of(const Answer& a) {
  if (a.kind == Answer::Kind::number) {
    return a.number;
  }
  if (a.kind == Answer::Kind::typed && a.typed.is_number()) {
    return a.typed.as_number();
  }
  return std::nullopt;
}
std::string key_of(const Answer& a) {
  if (a.kind == Answer::Kind::option) {
    return a.option;
  }
  if (a.kind == Answer::Kind::text) {
    return a.text;
  }
  return a.kind == Answer::Kind::typed ? base::js_string(a.typed) : std::string();
}
bool is_picture(const Ctx& c, const Value* e) {
  if (e == nullptr || !(e->get("type") == Value("insert"))) {
    return false;
  }
  const Value* b = c.doc.blocks().get(base::js_string(e->get("block")));
  return b != nullptr && doc::picture_frame(*b) != nullptr;
}
/// pictureKey(m) without its extension: k.replace(/\.[a-z0-9]+$/, '')
std::string stem_of(const std::string& k) {
  const std::size_t dot = k.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= k.size()) {
    return k;
  }
  for (std::size_t i = dot + 1; i < k.size(); ++i) {
    const char ch = k[i];
    const bool lower = ch >= 'a' && ch <= 'z';
    const bool digit = ch >= '0' && ch <= '9';
    if (!lower && !digit) {
      return k;
    }
  }
  return k.substr(0, dot);
}
std::string count(double n) {
  std::string s;
  base::append_number(s, n);
  return s;
}

Task<void> imageattach_cmd(Ctx& c) {
  if (!c.host.can_pick_pictures()) {
    c.print(c.t("pic.nobrowser"));
    co_return;
  }
  const Answer picked = co_await c.files(false);
  if (picked.files.empty()) {
    co_return;
  }
  const PickedFile& f = picked.files.front();
  const auto rec = doc::add_picture(c.doc, f.name, f.bytes);
  if (!rec) {
    c.print(c.t(rec.error(), {{"file", f.name}}));
    co_return;
  }
  const std::string file = (*rec)->name;
  const double pw = (*rec)->w;
  const double ph = (*rec)->h;
  const Answer at = co_await c.point(c.t("pic.prompt.corner", {{"file", file}}));
  if (at.kind != Answer::Kind::point) {
    co_return;
  }
  Ask aw;
  aw.base = at.p;
  aw.def = pw;
  const std::optional<double> width =
      number_of(co_await c.distance(c.t("pic.prompt.width", {{"w", count(pw)}, {"h", count(ph)}}), std::move(aw)));
  const double w = width && *width > 0 ? *width : pw;
  Ask aa;
  aa.base = at.p;
  aa.def = 0;
  const std::optional<double> ang = number_of(co_await c.angle(c.t("pic.prompt.angle"), std::move(aa)));
  const double a = ang.value_or(0);
  const double px = w / pw;
  const Value pic = base::make_object(
      {{"file", file}, {"x", 0}, {"y", 0}, {"ux", px}, {"uy", 0}, {"vx", 0}, {"vy", px}, {"w", pw}, {"h", ph}});
  const std::string name = doc::free_picture_name(c.doc);
  c.doc.change("IMAGEATTACH", [&] {
    (void)c.doc.define_block(cad::picture_block(name, pic, nullptr));
    (void)c.doc.add(base::make_object({{"type", "insert"},
                                       {"layer", c.doc.current.get("layer")},
                                       {"color", nullptr},
                                       {"block", name},
                                       {"x", at.p.x},
                                       {"y", at.p.y},
                                       {"sx", 1},
                                       {"sy", 1},
                                       {"rot", a}}));
  });
  c.print(c.t("pic.attached", {{"file", file}, {"w", count(pw)}, {"h", count(ph)}}));
}

Task<void> image_cmd(Ctx& c) {
  std::vector<std::pair<std::string, std::string>> used;  // key → the file as the drawing names it
  c.doc.blocks().each([&](const std::string&, const Value& b) {
    if (const Value* fr = doc::picture_frame(b)) {
      const std::string file = base::js_string(fr->get("pic").get("file"));
      const std::string k = doc::picture_key(file);
      bool found = false;
      for (auto& [uk, uf] : used) {
        if (uk == k) {
          uf = file;  // a Map keeps the key's place, with the newest value
          found = true;
        }
      }
      if (!found) {
        used.emplace_back(k, file);
      }
    }
  });
  if (used.empty()) {
    c.print(c.t("pic.none"));
    co_return;
  }
  std::vector<std::string> missing;
  for (const auto& [k, f] : used) {
    if (doc::find_picture(c.doc, f) == nullptr) {
      missing.push_back(f);
    }
  }
  c.print(c.words.tn("pic.list", static_cast<double>(used.size()),
                     {{"missing", count(static_cast<double>(missing.size()))}}));
  for (const auto& [k, f] : used) {
    c.print("  " + c.t(doc::find_picture(c.doc, f) != nullptr ? "pic.row.loaded" : "pic.row.missing", {{"file", f}}));
  }
  if (missing.empty()) {
    co_return;
  }
  Answer picked;
  if (c.host.can_pick_pictures()) {
    picked = co_await c.files(true);
  } else {
    c.print(c.t("pic.nobrowser"));
  }
  double n = 0;
  for (const PickedFile& f : picked.files) {
    const std::string k = doc::picture_key(f.name);
    const std::string stem = stem_of(k);
    // the exact name, else a file whose name contains the one the drawing names (eTransmit and other exports add a
    // prefix)
    std::optional<std::string> target;
    for (const std::string& m : missing) {
      if (doc::picture_key(m) == k) {
        target = m;
        break;
      }
    }
    if (!target) {
      for (const std::string& m : missing) {
        const std::string s = stem_of(doc::picture_key(m));
        if (base::utf16_length(s) >= 3 && stem.contains(s)) {
          target = m;
          break;
        }
      }
    }
    if (!target) {
      c.print(c.t("pic.unused", {{"file", f.name}}));
      continue;
    }
    const auto rec = doc::add_picture(c.doc, f.name, f.bytes);
    if (!rec) {
      c.print(c.t(rec.error(), {{"file", f.name}}));
      continue;
    }
    if (doc::picture_key(*target) != k) {  // known by the drawing's name too
      const doc::PictureFile copy = **rec;
      const std::string tk = doc::picture_key(*target);
      bool set = false;
      for (auto& [pk, pv] : c.doc.pictures) {
        if (pk == tk) {
          pv = copy;
          set = true;
        }
      }
      if (!set) {
        c.doc.pictures.emplace_back(tk, copy);
      }
    }
    ++n;
  }
  if (n > 0) {
    ++c.doc.picture_version;  // the views redraw when it changes
  }
  c.print(c.words.tn("pic.added", n, {}));
}

Task<void> imageclip_cmd(Ctx& c) {
  const Answer p = co_await c.entity(c.t("pic.prompt.pick"));
  if (p.kind != Answer::Kind::entity) {
    co_return;
  }
  const Value* ref0 = c.doc.entities().get(p.id);
  if (!is_picture(c, ref0)) {
    c.print(c.t("pic.notpicture"));
    co_return;
  }
  const Value ref = *ref0;
  const std::string bname = base::js_string(ref.get("block"));
  Ask ak;
  ak.options = {c.opt("R", c.t("opt.rectangular")), c.opt("P", c.t("opt.polygonal")), c.opt("D", c.t("opt.delete"))};
  ak.def = "R";
  const std::string o = key_of(co_await c.keyword(c.t("pic.prompt.clip"), std::move(ak)));
  if (o.empty()) {
    co_return;
  }
  std::optional<std::vector<Pt>> pts;
  if (o == "R") {
    const Answer a = co_await c.point(c.t("pic.prompt.corner1"));
    if (a.kind != Answer::Kind::point) {
      co_return;
    }
    Ask ab;
    ab.base = a.p;
    const Answer b = co_await c.point(c.t("pic.prompt.corner2"), std::move(ab));
    if (b.kind != Answer::Kind::point) {
      co_return;
    }
    pts = std::vector<Pt>{{a.p.x, a.p.y}, {b.p.x, a.p.y}, {b.p.x, b.p.y}, {a.p.x, b.p.y}};
  } else if (o == "P") {
    std::vector<Pt> list;
    for (;;) {
      Ask av;
      if (!list.empty()) {
        av.base = list.back();
      }
      av.none = list.size() >= 3;
      const Answer q =
          co_await c.point(c.t(list.size() < 3 ? "pic.prompt.vertex" : "pic.prompt.vertex_or_enter"), std::move(av));
      if (q.kind == Answer::Kind::nothing) {
        break;
      }
      if (q.kind != Answer::Kind::point) {
        co_return;
      }
      list.push_back(q.p);
    }
    if (list.size() < 3) {
      co_return;
    }
    pts = std::move(list);
  }
  const Value* blk = c.doc.blocks().get(bname);
  if (blk == nullptr) {
    co_return;
  }
  const Value block = *blk;
  const Value* fr = doc::picture_frame(block);
  // the clip in the block's own coordinates (the reference may be moved, turned and scaled)
  const cad::Mat2 m = cad::insert_matrix(ref, block);
  const double d = m[0] * m[3] - m[1] * m[2];
  const cad::Mat2 inv{
      m[3] / d, -m[1] / d, -m[2] / d, m[0] / d, (m[2] * m[5] - m[3] * m[4]) / d, (m[1] * m[4] - m[0] * m[5]) / d};
  std::optional<Value> local;
  if (pts) {
    base::Array a;
    for (const Pt q : *pts) {
      a.emplace_back(base::make_object(
          {{"x", inv[0] * q.x + inv[2] * q.y + inv[4]}, {"y", inv[1] * q.x + inv[3] * q.y + inv[5]}}));
    }
    local = Value(std::move(a));
  }
  // a picture block other references share gets a copy of its own first (a clip belongs to one picture, as in the file)
  std::size_t shared = 0;
  const auto count_refs = [&shared, &bname](const Value& e) {
    if (e.get("type") == Value("insert") && base::js_string(e.get("block")) == bname) {
      ++shared;
    }
  };
  c.doc.model()->each([&](const Id&, const Value& e) { count_refs(e); });
  for (const doc::Layout& l : c.doc.layouts()) {
    l.entities->each([&](const Id&, const Value& e) { count_refs(e); });
  }
  c.doc.blocks().each([&](const std::string&, const Value& b) {
    for (const Value& e : b.get("entities").as_array()) {
      count_refs(e);
    }
  });
  const std::string name = shared > 1 ? doc::free_picture_name(c.doc) : bname;
  Value plain_pic = fr->get("pic");
  plain_pic.erase("clipped");
  c.doc.change("IMAGECLIP", [&] {
    Value nb = cad::picture_block(name, plain_pic, local ? &*local : nullptr);
    // keep the name text as it was (its place and size)
    const Value nents = nb.get("entities");
    base::Array ents{nents.as_array().front()};
    const Value old = block.get("entities");
    bool skipped = false;  // the frame (the first poly carrying pic) is replaced; everything else kept
    for (const Value& e : old.as_array()) {
      if (!skipped && e.get("type") == Value("poly") && e.get("pic").truthy()) {
        skipped = true;
        continue;
      }
      ents.push_back(e);
    }
    nb.set("entities", Value(std::move(ents)));
    (void)c.doc.define_block(nb);
    if (name != bname) {
      Value r2 = ref;
      r2.set("block", name);
      (void)c.doc.update(p.id, r2);
    }
  });
  c.print(c.t(pts ? "pic.clipped" : "pic.unclipped"));
}

}  // namespace

void add_pictures(std::vector<CommandDef>& t) {
  t.push_back(def("IMAGEATTACH", {"IAT", "IMAGEATT"}, imageattach_cmd, "pic.help.attach"));
  t.push_back(def("IMAGE", {"IM"}, image_cmd, "pic.help.image", true));
  t.push_back(def("IMAGECLIP", {"ICL"}, imageclip_cmd, "pic.help.clip"));
}

}  // namespace cc::cmd::detail
