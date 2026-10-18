// SPDX-License-Identifier: Apache-2.0
// dyn-blocks.js dynSave and dynEntities from CadCraft 0.5. Not ported: editedVisibility (a definition whose
// visibility states were edited in the block editor, which the fork does not have yet, is saved plain: docs/gaps.md).
#include "dyn_save.hpp"

#include <cmath>
#include <string>
#include <utility>

#include "cc/base/js.hpp"

namespace cc::save::detail {

using base::Array;
using base::Value;

namespace {

/// parseInt(h, 16) of a handle string: its leading hex digits, NaN when none.
double parse_hex(const std::string& s) {
  const std::string_view t = base::js_trim_start(s);
  double v = 0;
  bool any = false;
  std::size_t i = 0;
  bool neg = false;
  if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
    neg = t[i] == '-';
    ++i;
  }
  if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
    i += 2;
  }
  for (; i < t.size(); ++i) {
    const char c = t[i];
    int d = -1;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    }
    if (d < 0) {
      break;
    }
    v = v * 16 + d;
    any = true;
  }
  if (!any) {
    return std::nan("");
  }
  return neg ? -v : v;
}

Value pair(double c, Value v) {
  return Value(Array{Value(c), std::move(v)});
}

Value dict_obj(const std::string& handle, const std::string& owner, Value entries) {
  return base::make_object({{"handle", handle},
                            {"owner", owner},
                            {"reactors", base::array_of(Value(owner))},
                            {"type", "DICTIONARY"},
                            {"entries", std::move(entries)},
                            {"hardOwner", 1},
                            {"cloning", 1}});
}
Value xrec_obj(const Value& handle, const Value& owner, Value data) {
  return base::make_object({{"handle", handle},
                            {"owner", owner},
                            {"reactors", base::array_of(owner)},
                            {"type", "XRECORD"},
                            {"data", std::move(data)},
                            {"cloning", 1}});
}
/// data.map(([c, v]) => (c === 1 ? [1, state] : [c, v]))
Value state_data(const Value& data, const Value& state) {
  Array out;
  if (data.is_array()) {
    for (const Value& it : data.as_array()) {
      const Value c = it.is_array() && !it.as_array().empty() ? it.as_array()[0] : Value();
      const Value v = it.is_array() && it.as_array().size() > 1 ? it.as_array()[1] : Value();
      out.emplace_back(c == Value(1) ? Array{Value(1), state} : Array{c, v});
    }
  }
  return out;
}

/// JS === between two values of a dyn record (numbers, strings)
bool strict_eq(const Value& a, const Value& b) {
  return a.kind() == b.kind() && a == b;
}

/// dynEntities(b): the block's file entities as they are now, or null when it no longer lines up.
Value dyn_entities(const Value& b) {
  const Value d = b.get("dyn");
  if (!d.truthy()) {
    return {nullptr};
  }
  const Value all = d.get("all");
  const Value shown = d.get("shown");
  if (!all.is_array() || !shown.is_array() || all.as_array().size() != shown.as_array().size()) {
    return {nullptr};
  }
  const Value ad = d.get("ad");
  const Value attdefs = b.get("attdefs");
  const std::size_t na = attdefs.is_array() ? attdefs.as_array().size() : 0;
  const std::size_t nd = ad.is_array() ? ad.as_array().size() : 0;
  if (na != nd) {
    return {nullptr};
  }
  const Value hv = b.get("entities");
  const Array empty;
  const Array& have = hv.is_array() ? hv.as_array() : empty;
  std::size_t k = 0;
  Array out;
  bool ok = true;
  for (std::size_t i = 0; i < all.as_array().size(); ++i) {
    const Value& parts = all.as_array()[i];
    if (!shown.as_array()[i].truthy()) {
      out.push_back(parts);
      continue;
    }
    const std::size_t np = parts.is_array() ? parts.as_array().size() : 0;
    Array now;
    for (std::size_t j = k; j < k + np && j < have.size(); ++j) {
      now.push_back(have[j]);
    }
    k += np;
    bool same = now.size() == np;
    for (std::size_t j = 0; same && j < np; ++j) {
      same = now[j].truthy() && now[j].get("type") == parts.as_array()[j].get("type");
    }
    if (!same) {
      ok = false;
      out.emplace_back(nullptr);
    } else {
      out.emplace_back(std::move(now));
    }
  }
  if (k != have.size() || !ok) {
    return {nullptr};
  }
  return out;
}

int ver_num(const std::string& v) {
  static constexpr std::array<std::pair<std::string_view, int>, 8> kV = {{{"AC1012", 13},
                                                                          {"AC1014", 14},
                                                                          {"AC1015", 15},
                                                                          {"AC1018", 18},
                                                                          {"AC1021", 21},
                                                                          {"AC1024", 24},
                                                                          {"AC1027", 27},
                                                                          {"AC1032", 32}}};
  for (const auto& [k, n] : kV) {
    if (k == v) {
      return n;
    }
  }
  return 0;
}

}  // namespace

std::string hex_key(const Value& h) {
  if (h.is_undefined() || h.is_null()) {
    return {};
  }
  std::string s = base::js_to_upper(base::js_string(h));
  std::size_t z = 0;
  while (z + 1 < s.size() && s[z] == '0') {
    ++z;
  }
  return s.substr(z);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — dynSave's set-up, step by step as the JS
DynSave::DynSave(const doc::Doc& d) : src_(d.dyn_src) {
  version_ = src_.truthy() && src_.get("version").truthy() ? base::js_string(src_.get("version")) : std::string();
  if (!src_.truthy() || !src_.get("objects").is_array()) {
    return;
  }
  active_ = true;
  std::set<std::string> own;
  if (const Value rs = src_.get("records"); rs.is_array()) {
    for (const Value& r : rs.as_array()) {
      own.insert(base::js_string(r));  // a Set of the values; compared below with ===, here by their text
    }
  }
  const auto own_has = [&](const Value& v) { return v.is_string() && own.contains(v.as_string()); };
  for (const Value& o : src_["objects"].as_array()) {
    src_objs_[hex_key(o.get("handle"))] = o;  // Map(set) keeps the last for a repeated key
  }
  d.blocks().each([&](const std::string& name, const Value& b) {
    const Value dyn = b.get("dyn");
    if (!dyn.truthy()) {
      return;
    }
    bool mine = false;
    if (dyn.get("made").truthy()) {
      const Value* of = d.blocks().get(base::js_string(dyn.get("of")));
      const Value ofd = of != nullptr ? of->get("dyn") : Value();
      mine = own_has(ofd.truthy() ? ofd.get("src") : Value());
    } else {
      mine = own_has(dyn.get("src"));
    }
    if (!mine) {
      return;
    }
    const Value all = dyn_entities(b);
    const bool def = dyn.get("def").truthy();
    const bool vis_edited = dyn.get("visEdited").truthy();
    if (all.truthy() && (!def || !vis_edited)) {
      Value copy = dyn;
      copy.set("all", all);
      blocks_.emplace_back(name, std::move(copy));
    } else if (def) {
      dropped_ += 1;
    }
  });
  // a copy whose definition is no longer dynamic goes plain with it
  const auto snapshot = blocks_;
  for (const auto& [name, dd] : snapshot) {
    const Value of = dd.get("of");
    const Value* ofb = of.truthy() ? block(base::js_string(of)) : nullptr;
    const bool of_def = ofb != nullptr && ofb->get("def").truthy();
    if (!dd.get("def").truthy() && !of_def) {
      std::erase_if(blocks_, [&](const auto& p) { return p.first == name; });
    }
  }
  if (const Value c = src_.get("classes"); c.is_array()) {
    classes_ = c;
  }
  if (const Value m = src_.get("maintenance"); m.is_number() && std::isfinite(m.as_number())) {
    maintenance_ = m;
  }
  double top = 0;
  const auto seen = [&](const Value& h) {
    const double n = parse_hex(base::js_string(h));
    if (std::isfinite(n) && n > top) {
      top = n;
    }
  };
  for (const Value& o : src_["objects"].as_array()) {
    seen(o.get("handle"));
  }
  for (const std::string& r : own) {
    seen(Value(r));
  }
  for (const auto& [name, dd] : blocks_) {
    if (const Value h = dd.get("h"); h.is_array()) {
      for (const Value& x : h.as_array()) {
        seen(x);
      }
    }
  }
  fake_ = top + 0x10000;
  for (const auto& [name, dd] : blocks_) {
    rec_for_[name] = dd.get("made").truthy() ? new_h() : base::js_string(dd.get("src"));
  }
  for (auto& [name, dd] : blocks_) {
    if (dd.get("made").truthy()) {
      const Value* defb = block(base::js_string(dd.get("of")));
      const Value defsrc = defb != nullptr ? defb->get("src") : Value();
      dd.set("xd", base::make_object({{"AcDbBlockRepBTag", Array{pair(1070, 1), pair(1005, defsrc)}}}));
    }
  }
  ver_num_ = ver_num(version_);
  for (const Value& c : classes_.as_array()) {
    if (c.get("dxfName") == Value("ACDB_BLOCKREPRESENTATION_DATA")) {
      rep_class_ = c;
      break;
    }
  }
}

const Value* DynSave::block(const std::string& name) const {
  for (const auto& [k, v] : blocks_) {
    if (k == name) {
      return &v;
    }
  }
  return nullptr;
}

Value* DynSave::block_mut(const std::string& name) {
  for (auto& [k, v] : blocks_) {
    if (k == name) {
      return &v;
    }
  }
  return nullptr;
}

std::string DynSave::new_h() {
  const double v = fake_;
  fake_ += 1;
  return base::hex_upper(static_cast<std::uint64_t>(v));
}

Value DynSave::record(const std::string& name) const {
  const auto it = rec_for_.find(name);
  return it == rec_for_.end() || it->second.empty() ? Value(nullptr) : Value(it->second);
}

std::string DynSave::clone_tree(const Value& root, const std::string& owner, const Value& state,
                                const Value& vis_node) {
  std::vector<std::pair<std::string, std::string>> map;  // old hexKey → new handle, in insertion order
  const auto find = [&](const std::string& k) -> const std::string* {
    for (const auto& [o, n] : map) {
      if (o == k) {
        return &n;
      }
    }
    return nullptr;
  };
  // NOLINTNEXTLINE(misc-no-recursion) — a dictionary tree; each object visited once
  const std::function<void(const Value&, int)> walk = [&](const Value& h, int depth) {
    const auto it = src_objs_.find(hex_key(h));
    if (it == src_objs_.end() || depth > 256) {
      return;
    }
    const Value& o = it->second;
    if (find(base::js_string(o.get("handle"))) != nullptr) {  // map.has(o.handle): the raw handle, as the JS
      return;
    }
    map.emplace_back(hex_key(o.get("handle")), new_h());
    if (const Value en = o.get("entries"); en.is_object() && !en.is_point()) {
      for (const base::Member* m : base::js_members(en.as_object())) {
        walk(m->value, depth + 1);
      }
    }
  };
  walk(root, 0);
  for (const auto& [old, nh] : map) {
    const Value& o = src_objs_.at(old);
    std::string ow = owner;
    if (old != hex_key(root)) {
      const std::string* p = find(hex_key(o.get("owner")));
      ow = p != nullptr ? *p : owner;
    }
    if (const Value en = o.get("entries"); en.truthy()) {
      Value entries = Value::object();
      if (en.is_object() && !en.is_point()) {
        for (const base::Member* m : base::js_members(en.as_object())) {
          const std::string* p = find(hex_key(m->value));
          entries.set(m->key, p != nullptr ? Value(*p) : m->value);
        }
      }
      extra_.push_back(dict_obj(nh, ow, std::move(entries)));
    } else if (o.get("type") == Value("XRECORD")) {
      const auto pit = src_objs_.find(hex_key(o.get("owner")));
      Value key;
      if (pit != src_objs_.end()) {
        if (const Value pe = pit->second.get("entries"); pe.is_object() && !pe.is_point()) {
          for (const base::Member* m : base::js_members(pe.as_object())) {
            if (hex_key(m->value) == old) {
              key = Value(m->key);
              break;
            }
          }
        }
      }
      const Value data = o.get("data");
      Value nd;
      if (!state.is_undefined() && key.is_string() && base::js_string(vis_node) == key.as_string()) {
        nd = state_data(data, state);
      } else {
        nd = data.is_array() ? data : Value(Array{});
      }
      extra_.push_back(xrec_obj(Value(nh), Value(ow), std::move(nd)));
    } else {
      Value c = o;
      c.set("handle", nh);
      c.set("owner", ow);
      c.set("reactors", base::array_of(Value(ow)));
      extra_.push_back(std::move(c));
    }
  }
  const std::string* r = find(hex_key(root));
  return r != nullptr ? *r : std::string();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) — out.ref, as the JS
Value DynSave::ref(const Value& e) {
  const Value d = e.get("dyn");
  const Value* blk = d.truthy() ? block(base::js_string(e.get("block"))) : nullptr;
  if (blk == nullptr) {
    return {nullptr};
  }
  const Value blkv = *blk;
  const Value* defp = blkv.get("def").truthy() ? blk : block(base::js_string(blkv.get("of")));
  const Value def = defp != nullptr ? *defp : Value();
  const Value vis = def.truthy() ? def.get("vis") : Value();
  Value v;
  if (vis.truthy()) {
    if (const Value vals = d.get("values"); vals.is_array()) {
      for (const Value& x : vals.as_array()) {
        if (strict_eq(x.get("node"), vis.get("node"))) {
          v = x;
          break;
        }
      }
    }
  }
  const Value state = v.truthy() && v.get("state").is_string() ? v.get("state") : d.get("state");
  const Value dsrc = d.get("src");
  const bool first = dsrc.truthy() && !used_.contains(base::js_string(dsrc));
  const std::string handle = first ? base::js_string(dsrc) : new_h();
  used_.insert(handle);
  const Value dx = d.get("x");
  if (dx.truthy() && first) {
    const Value xr = v.truthy() ? v.get("xrecord") : Value();
    if (xr.truthy()) {
      const auto it = src_objs_.find(hex_key(xr));
      if (it != src_objs_.end() && state.is_string()) {
        const Value& xo = it->second;
        bool differs = false;
        if (const Value data = xo.get("data"); data.is_array()) {
          for (const Value& it2 : data.as_array()) {
            if (it2.is_array() && it2.as_array().size() > 1 && it2.as_array()[0] == Value(1) &&
                !strict_eq(it2.as_array()[1], state)) {
              differs = true;
            }
          }
        }
        if (differs) {
          replaced_[hex_key(xo.get("handle"))] =
              xrec_obj(xo.get("handle"), xo.get("owner"), state_data(xo.get("data"), state));
        }
      }
    }
    return base::make_object({{"handle", handle}, {"extDict", dx}});
  }
  if (dx.truthy()) {
    return base::make_object(
        {{"handle", handle}, {"extDict", clone_tree(dx, handle, state, vis.truthy() ? vis.get("node") : Value())}});
  }
  if (blkv.get("def").truthy() || rep_class_.is_undefined() || ver_num_ == 0) {
    return {nullptr};
  }
  const std::string xd = new_h();
  const std::string rep = new_h();
  const std::string rd = new_h();
  const std::string adc = new_h();
  const std::string ebd = new_h();
  extra_.push_back(dict_obj(xd, handle, base::make_object({{"AcDbBlockRepresentation", rep}})));
  extra_.push_back(dict_obj(rep, xd, base::make_object({{"AcDbRepData", rd}, {"AppDataCache", adc}})));
  extra_.push_back(
      base::make_object({{"handle", rd},
                         {"owner", rep},
                         {"reactors", base::array_of(Value(rep))},
                         {"type", "ACDB_BLOCKREPRESENTATION_DATA"},
                         {"undecoded", true},
                         {"typeNum", rep_class_.get("num")},
                         {"className", rep_class_.get("cppName")},
                         {"raw", base::make_object({{"ver", ver_num_},
                                                    {"n", ver_num_ >= 21 ? 11 : 10},
                                                    {"data", "QEA="},
                                                    {"h", base::array_of(Value(Array{Value(5), def.get("src")}))}})}}));
  extra_.push_back(dict_obj(adc, rep, base::make_object({{"ACAD_ENHANCEDBLOCKDATA", ebd}})));
  Value entries = Value::object();
  if (vis.truthy() && state.is_string()) {
    const std::string xr = new_h();
    entries.set(base::js_string(vis.get("node")), xr);
    Value pt = v.truthy() ? v.get("point") : Value();
    if (!pt.truthy()) {
      pt = vis.get("point");
    }
    if (!pt.truthy()) {
      pt = base::pt2(0, 0);
    }
    extra_.push_back(
        xrec_obj(Value(xr), Value(ebd),
                 Array{Value(Array{Value(10), base::make_object({{"x", pt.get("x")}, {"y", pt.get("y")}, {"z", 0}})}),
                       pair(1, state)}));
  }
  extra_.push_back(dict_obj(ebd, adc, std::move(entries)));
  return base::make_object({{"handle", handle}, {"extDict", xd}});
}

Array DynSave::objects() const {
  Array out;
  for (const Value& o : src_["objects"].as_array()) {
    const auto it = replaced_.find(hex_key(o.get("handle")));
    out.push_back(it != replaced_.end() ? it->second : o);
  }
  for (const Value& x : extra_) {
    out.push_back(x);
  }
  return out;
}

}  // namespace cc::save::detail
