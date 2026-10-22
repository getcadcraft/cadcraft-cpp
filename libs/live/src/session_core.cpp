// SPDX-License-Identifier: Apache-2.0
// Studio Live's session decisions (docs/LIVE-PROTOCOL.md § 6). See session_core.hpp.
#include "cc/live/session_core.hpp"

#include <algorithm>

namespace cc::live {

std::string_view role_name(Role r) noexcept {
  switch (r) {
    case Role::host: return "host";
    case Role::editor: return "editor";
    case Role::viewer: return "viewer";
    case Role::assistant: return "assistant";
    case Role::replica: return "replica";
  }
  return "viewer";
}

std::optional<Role> parse_role(std::string_view s) noexcept {
  if (s == "host") {
    return Role::host;
  }
  if (s == "editor") {
    return Role::editor;
  }
  if (s == "viewer") {
    return Role::viewer;
  }
  if (s == "assistant") {
    return Role::assistant;
  }
  if (s == "replica") {
    return Role::replica;
  }
  return std::nullopt;
}

bool can_edit(Role r) noexcept {
  return r == Role::host || r == Role::editor || r == Role::assistant;
}

std::optional<Touch> touch_from(const Mp& item) {
  const Mp* k = item.get("k");
  if (k == nullptr || !k->is_string()) {
    return std::nullopt;
  }
  Touch t;
  t.k = k->as_string();
  if (const Mp* m = item.get("m"); m != nullptr && m->is_string() && !m->as_string().empty()) {
    t.m = m->as_string()[0];
  }
  if (const Mp* f = item.get("f"); f != nullptr && f->is_string()) {
    t.f = f->as_string();
  }
  if (const Mp* r = item.get("r"); r != nullptr) {
    std::uint64_t v = 0;
    if (r->to_uint(v)) {
      t.r = v;
    }
  }
  if (const Mp* g = item.get("g"); g != nullptr) {
    std::uint64_t v = 0;
    t.g = (g->is_bool() && g->as_bool()) || (g->to_uint(v) && v > 0);
  }
  return t;
}

std::vector<Touch> touched_from(const Mp* list) {
  std::vector<Touch> out;
  if (list == nullptr || !list->is_array()) {
    return out;
  }
  std::size_t seen = 0;
  for (const Mp& item : list->as_array()) {
    if (++seen > 100'000) {
      break;
    }
    if (auto t = touch_from(item)) {
      out.push_back(std::move(*t));
    }
  }
  return out;
}

Mp commit_touched(std::span<const Touch> touched) {
  MpArray out;
  out.reserve(touched.size());
  for (const Touch& t : touched) {
    Map m;
    m.set("k", Mp(t.k));
    m.set("m", Mp(std::string(1, t.m)));
    if (t.f) {
      m.set("f", Mp(*t.f));
    }
    out.emplace_back(std::move(m));
  }
  return {std::move(out)};
}

// ---------------------------------------------------------------------------------------------------- clients
void SessionCore::join(std::uint64_t c, Role role) {
  clients_[c] = role;
}

std::vector<std::string> SessionCore::leave(std::uint64_t c) {
  clients_.erase(c);
  return release(c, {}, true);
}

std::optional<Role> SessionCore::role_of(std::uint64_t c) const {
  const auto it = clients_.find(c);
  if (it == clients_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void SessionCore::set_role(std::uint64_t c, Role role) {
  if (auto it = clients_.find(c); it != clients_.end()) {
    it->second = role;
  }
}

// ---------------------------------------------------------------------------------------------------- holds, locks
std::vector<std::string> SessionCore::hold(std::uint64_t c, std::span<const std::string> keys, std::uint64_t now_ms) {
  std::vector<std::string> added;
  for (const std::string& k : keys) {
    auto it = holds_.find(k);
    if (it == holds_.end()) {
      holds_.emplace(k, Hold{c, now_ms});
      added.push_back(k);
    } else if (it->second.c == c) {
      it->second.at = now_ms;  // refreshed
    }
  }
  return added;
}

std::vector<std::string> SessionCore::release(std::uint64_t c, std::span<const std::string> keys, bool all) {
  std::vector<std::string> cleared;
  if (all) {
    for (auto it = holds_.begin(); it != holds_.end();) {
      if (it->second.c == c) {
        cleared.push_back(it->first);
        it = holds_.erase(it);
      } else {
        ++it;
      }
    }
    return cleared;
  }
  for (const std::string& k : keys) {
    if (auto it = holds_.find(k); it != holds_.end() && it->second.c == c) {
      holds_.erase(it);
      cleared.push_back(k);
    }
  }
  return cleared;
}

std::vector<std::string> SessionCore::expire(std::uint64_t now_ms) {
  std::vector<std::string> cleared;
  for (auto it = holds_.begin(); it != holds_.end();) {
    if (now_ms >= it->second.at && now_ms - it->second.at >= kHoldMs) {
      cleared.push_back(it->first);
      it = holds_.erase(it);
    } else {
      ++it;
    }
  }
  return cleared;
}

std::uint64_t SessionCore::holder(std::string_view key) const {
  const auto it = holds_.find(key);
  return it == holds_.end() ? 0 : it->second.c;
}

std::vector<std::pair<std::string, std::uint64_t>> SessionCore::holds() const {
  std::vector<std::pair<std::string, std::uint64_t>> out;
  out.reserve(holds_.size());
  for (const auto& [k, h] : holds_) {
    out.emplace_back(k, h.c);
  }
  return out;
}

std::vector<std::string> SessionCore::lock(std::span<const std::string> keys) {
  std::vector<std::string> added;
  for (const std::string& k : keys) {
    if (locks_.emplace(k, true).second) {
      added.push_back(k);
    }
  }
  return added;
}

std::vector<std::string> SessionCore::unlock(std::span<const std::string> keys) {
  std::vector<std::string> removed;
  for (const std::string& k : keys) {
    if (auto it = locks_.find(k); it != locks_.end()) {
      locks_.erase(it);
      removed.push_back(k);
    }
  }
  return removed;
}

bool SessionCore::locked(std::string_view key) const {
  return locks_.contains(key);
}

std::vector<std::string> SessionCore::locks() const {
  std::vector<std::string> out;
  out.reserve(locks_.size());
  for (const auto& [k, on] : locks_) {
    out.push_back(k);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------- edits
namespace {
std::string prop_key(std::string_view k, std::string_view f) {
  std::string s(k);
  s += '\0';
  s += f;
  return s;
}
}  // namespace

const SessionCore::Key* SessionCore::key(std::string_view k) const {
  const auto it = keys_.find(k);
  return it == keys_.end() ? nullptr : &it->second;
}

std::uint64_t SessionCore::rev(std::string_view k) const {
  const Key* s = key(k);
  return s == nullptr ? 0 : s->rev;
}

std::vector<std::pair<std::string, std::uint64_t>> SessionCore::revs() const {
  std::vector<std::pair<std::string, std::uint64_t>> out;
  out.reserve(keys_.size());
  for (const auto& [k, s] : keys_) {
    out.emplace_back(k, s.rev);
  }
  return out;
}

Decision SessionCore::check(std::uint64_t c, std::span<const Touch> touched) const {
  const auto refuse = [](std::string reason, std::uint64_t by = 0) {
    return Decision{false, 0, std::move(reason), by};
  };
  const auto role = role_of(c);
  if (!role) {
    return refuse("gone");
  }
  if (!can_edit(*role)) {
    return refuse("role");
  }
  for (const Touch& t : touched) {
    if (locked(t.k)) {
      return refuse("locked");
    }
  }
  for (const Touch& t : touched) {
    if (t.m == 's' || t.m == 'd' || (t.m == 'r' && t.g)) {
      const std::uint64_t h = holder(t.k);
      if (h != 0 && h != c) {
        return refuse("held", h);
      }
    }
  }
  for (const Touch& t : touched) {
    const Key* ks = key(t.k);
    const bool deleted = ks != nullptr && ks->deleted;
    const std::uint64_t by = ks == nullptr ? 0 : ks->by;
    switch (t.m) {
      case 'p':
        if (deleted) {
          return refuse("deleted", by);
        }
        if (t.r && t.f) {
          const auto it = props_.find(prop_key(t.k, *t.f));
          const Write w = it == props_.end() ? Write{} : it->second;
          if (w.q != *t.r) {
            return refuse("changed", w.by);
          }
        }
        break;
      case 'c':
        if (ks != nullptr && !deleted) {
          return refuse("exists", by);
        }
        break;
      case 'd':
        if (deleted) {
          return refuse("deleted", by);
        }
        if (t.r) {  // delete wins over property writes; a condition is on structure only
          const auto it = struct_rev_.find(t.k);
          if ((it == struct_rev_.end() ? 0 : it->second) > *t.r) {
            return refuse("changed", by);
          }
        }
        break;
      default:  // s, r: not deleted and, with r, unchanged
        if (deleted) {
          return refuse("deleted", by);
        }
        if (t.r && (ks == nullptr ? 0 : ks->rev) != *t.r) {
          return refuse("changed", by);
        }
        break;
    }
  }
  return Decision{true, seq_ + 1, {}, 0};
}

Decision SessionCore::edit(std::uint64_t c, std::span<const Touch> touched, std::vector<std::string>* cleared) {
  Decision d = check(c, touched);
  if (!d.ok) {
    return d;
  }
  seq_ = d.q;
  const std::uint64_t q = seq_;
  for (const Touch& t : touched) {
    if (t.m == 'r') {
      continue;
    }
    Key& ks = keys_[t.k];
    ks.rev = q;
    ks.by = c;
    if (t.m == 'p') {
      if (t.f) {
        props_[prop_key(t.k, *t.f)] = Write{q, c};
      }
      continue;
    }
    ks.deleted = t.m == 'd';
    if (t.m == 'c') {
      // a re-created key (undo of a delete) starts its properties afresh
      const std::string pre = prop_key(t.k, "");
      for (auto it = props_.lower_bound(pre); it != props_.end() && it->first.starts_with(pre);) {
        it = props_.erase(it);
      }
    }
    struct_rev_[t.k] = q;
    if (t.m == 'd') {
      if (auto h = holds_.find(t.k); h != holds_.end()) {
        holds_.erase(h);
        if (cleared != nullptr) {
          cleared->push_back(t.k);
        }
      }
    }
  }
  return d;
}

}  // namespace cc::live
