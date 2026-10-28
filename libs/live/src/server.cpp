// SPDX-License-Identifier: Apache-2.0
// desktop-shell live/session.rs, ported function by function. See server.hpp.
#include "cc/live/server.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "cc/base/random.hpp"

namespace cc::live {

namespace {

// ---- reading bodies as session.rs's msg.rs helpers do
const Mp* field(const Mp& b, std::string_view k) {
  return b.get(k);
}
std::optional<std::string_view> get_str(const Mp& b, std::string_view k) {
  const Mp* v = field(b, k);
  if (v == nullptr || !v->is_string()) {
    return std::nullopt;
  }
  return std::string_view(v->as_string());
}
std::optional<std::uint64_t> get_u64(const Mp& b, std::string_view k) {
  const Mp* v = field(b, k);
  std::uint64_t out = 0;
  if (v == nullptr || !v->to_uint(out)) {
    return std::nullopt;
  }
  return out;
}
std::optional<bool> get_bool(const Mp& b, std::string_view k) {
  const Mp* v = field(b, k);
  if (v == nullptr || !v->is_bool()) {
    return std::nullopt;
  }
  return v->as_bool();
}
const MpArray& get_arr(const Mp& b, std::string_view k) {
  static const MpArray kEmpty;
  const Mp* v = field(b, k);
  return v != nullptr && v->is_array() ? v->as_array() : kEmpty;
}
std::optional<Bytes> get_bin(const Mp& b, std::string_view k) {
  const Mp* v = field(b, k);
  if (v == nullptr || !v->is_bytes()) {
    return std::nullopt;
  }
  return v->as_bytes();
}
std::vector<std::string> strings_of(const MpArray& a, std::size_t max = std::numeric_limits<std::size_t>::max()) {
  std::vector<std::string> out;
  for (const Mp& v : a) {
    if (out.size() >= max) {
      break;
    }
    if (v.is_string()) {
      out.push_back(v.as_string());
    }
  }
  return out;
}
/// `.chars().take(n)` on UTF-8.
std::string take_chars(std::string_view s, std::size_t n) {
  std::size_t i = 0;
  std::size_t chars = 0;
  while (i < s.size() && chars < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      i += 1;
    } else if (c < 0xe0) {
      i += 2;
    } else if (c < 0xf0) {
      i += 3;
    } else {
      i += 4;
    }
    ++chars;
  }
  return std::string(s.substr(0, std::min(i, s.size())));
}

Mp map_of(std::initializer_list<std::pair<std::string_view, Mp>> pairs) {
  Map m;
  for (const auto& [k, v] : pairs) {
    m.set(k, v);
  }
  return {std::move(m)};
}
Mp strs(const std::vector<std::string>& v) {
  MpArray a;
  a.reserve(v.size());
  for (const std::string& s : v) {
    a.emplace_back(s);
  }
  return {std::move(a)};
}
Envelope env(std::string t, Mp b = Mp()) {
  Envelope e;
  e.t = std::move(t);
  e.b = std::move(b);
  return e;
}

std::string rand_hex(std::size_t n) {
  std::vector<std::uint8_t> b(n);
  base::os_random(b);
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t x : b) {
    s += kHex[x >> 4U];
    s += kHex[x & 15U];
  }
  return s;
}
Bytes rand_bytes(std::size_t n) {
  Bytes b(n);
  base::os_random(b);
  return b;
}

/// Client-id prefixes for new object ids: short, never reused in a session.
std::string prefix_for(std::uint32_t id) {
  static constexpr std::string_view kA = "abcdefghjkmnpqrstuvwxyz";
  std::size_t n = id;
  std::string s;
  do {
    s.insert(s.begin(), kA[n % kA.size()]);
    n /= kA.size();
  } while (n != 0);
  return s;
}

bool same_bytes(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned d = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    d |= static_cast<unsigned>(a[i] ^ b[i]);
  }
  return d == 0;
}

std::string ticket_kid(std::uint32_t c) {
  return "h:" + std::to_string(c);
}
std::string prop_key(std::string_view k, std::string_view f) {
  std::string s(k);
  s += '\0';
  s += f;
  return s;
}

Envelope holds_change(const std::vector<std::pair<std::string, std::uint32_t>>& set,
                      const std::vector<std::string>& clear) {
  Map s;
  for (const auto& [k, c] : set) {
    s.set(k, Mp(c));
  }
  return env("holds", map_of({{"set", Mp(std::move(s))}, {"clear", strs(clear)}}));
}

Envelope code_gone(std::string_view kid, std::string_view reason) {
  return env("code-gone", map_of({{"kid", Mp(kid)}, {"reason", Mp(reason)}}));
}

std::optional<Role> role_of_name(std::string_view s) {
  if (s == "edit") {
    return Role::editor;  // session.rs Role::parse also takes the short names
  }
  if (s == "view") {
    return Role::viewer;
  }
  return parse_role(s);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- state
SessionServer::SessionServer(ServerConfig cfg) : id_(rand_hex(8)), cfg_(std::move(cfg)) {}

Mp SessionServer::handover_state(std::uint32_t to) {
  std::set<std::uint32_t> sealed;
  for (const auto& [id, c] : clients_) {
    if (c.conn) {
      const auto it = conns_.find(*c.conn);
      if (it != conns_.end() && it->second.sealed) {
        sealed.insert(id);
      }
    }
  }
  for (auto& [id, c] : clients_) {
    if (id != to && c.role != Role::replica && (c.ticket.empty() || !sealed.contains(id))) {
      c.ticket = rand_bytes(32);
    }
  }
  return state(to);
}

Mp SessionServer::state(std::optional<std::uint32_t> to) const {
  // Edits queued but not yet decided are not in the log: their authors re-send them to the new host.
  std::map<std::uint32_t, std::uint64_t> undecided;
  const auto note = [&undecided](const Queued& e) {
    auto [it, fresh] = undecided.try_emplace(e.c, e.n);
    if (!fresh) {
      it->second = std::min(it->second, e.n);
    }
  };
  for (const Queued& e : queue_) {
    note(e);
  }
  if (validating_) {
    note(*validating_);
  }
  MpArray clients;
  for (const auto& [id, c] : clients_) {
    if (c.state == State::waiting || c.token.empty() || c.role == Role::replica) {
      continue;
    }
    Role role = c.role;
    if (to && *to == id) {
      role = Role::host;
    } else if (to && c.role == Role::host) {
      role = Role::editor;
    }
    const auto u = undecided.find(id);
    const std::uint64_t last_n = u != undecided.end() ? std::min(c.last_n, u->second - 1) : c.last_n;
    Map m;
    m.set("c", Mp(id));
    m.set("name", Mp(c.name));
    m.set("device", Mp(c.device));
    m.set("role", Mp(role_name(role)));
    m.set("prefix", Mp(c.prefix));
    m.set("token", Mp(c.token));
    m.set("last_n", Mp(last_n));
    if (!c.ticket.empty()) {
      m.set("ticket", Mp(c.ticket));
    }
    if (c.of != 0) {
      m.set("of", Mp(c.of));
    }
    clients.emplace_back(std::move(m));
  }
  MpArray log;
  for (const Commit& cm : log_) {
    log.push_back(map_of({{"q", Mp(cm.q)},
                          {"c", Mp(cm.c)},
                          {"n", Mp(cm.n)},
                          {"label", Mp(cm.label)},
                          {"ops", cm.ops},
                          {"touched", cm.touched}}));
  }
  MpArray keys;
  for (const auto& [k, v] : keys_) {
    const auto sr = struct_rev_.find(k);
    keys.emplace_back(MpArray{Mp(k), Mp(v.rev), Mp(v.by), Mp(v.deleted), Mp(sr == struct_rev_.end() ? 0 : sr->second)});
  }
  MpArray props;
  for (const auto& [k, v] : props_) {
    props.emplace_back(MpArray{Mp(k), Mp(v.first), Mp(v.second)});
  }
  MpArray invites;
  for (const auto& [kid, k] : invites_) {
    if (k.kind == KeyKind::code && k.expires > now_) {
      invites.push_back(map_of({{"code", Mp(k.code)},
                                {"secret", Mp(k.secret)},
                                {"one_time", Mp(k.one_time)},
                                {"left", Mp(k.expires - now_)}}));
    }
  }
  std::vector<std::string> locks(locks_.begin(), locks_.end());
  return map_of({{"format", Mp("cadcraft-live-state")},
                 {"v", Mp(1)},
                 {"session", Mp(id_)},
                 {"app", Mp(cfg_.app)},
                 {"schema", Mp(cfg_.schema)},
                 {"seq", Mp(seq_)},
                 {"next_client", Mp(next_client_)},
                 {"saved", map_of({{"q", Mp(saved_q_)}, {"hash", Mp(saved_hash_)}})},
                 {"locks", strs(locks)},
                 {"clients", Mp(std::move(clients))},
                 {"log", Mp(std::move(log))},
                 {"keys", Mp(std::move(keys))},
                 {"props", Mp(std::move(props))},
                 {"invites", Mp(std::move(invites))}});
}

Mp SessionServer::export_state() const {
  return state(std::nullopt);
}

std::expected<SessionServer, std::string> SessionServer::import_state(ServerConfig cfg, const Mp& st, Ms now) {
  if (get_str(st, "format") != std::optional<std::string_view>("cadcraft-live-state") || get_u64(st, "v") != 1U) {
    return std::unexpected("not a session state");
  }
  if (get_str(st, "app").value_or("") != cfg.app || get_u64(st, "schema") != std::optional<std::uint64_t>(cfg.schema)) {
    return std::unexpected("another app or schema");
  }
  SessionServer s(std::move(cfg));
  s.id_ = take_chars(get_str(st, "session").value_or(""), 64);
  s.seq_ = get_u64(st, "seq").value_or(0);
  s.next_client_ = static_cast<std::uint32_t>(get_u64(st, "next_client").value_or(1));
  if (const Mp* sv = field(st, "saved")) {
    s.saved_q_ = get_u64(*sv, "q").value_or(0);
    s.saved_hash_ = std::string(get_str(*sv, "hash").value_or(""));
  }
  for (const std::string& k : strings_of(get_arr(st, "locks"))) {
    s.locks_.insert(k);
  }
  for (const Mp& c : get_arr(st, "clients")) {
    const auto id = get_u64(c, "c");
    if (!id) {
      return std::unexpected("client id");
    }
    Client cl;
    cl.id = static_cast<std::uint32_t>(*id);
    cl.name = std::string(get_str(c, "name").value_or(""));
    cl.device = std::string(get_str(c, "device").value_or(""));
    cl.via = "lan";
    if (auto t = get_bin(c, "ticket"); t && t->size() == 32) {
      cl.ticket = *t;
    }
    cl.role = role_of_name(get_str(c, "role").value_or("")).value_or(Role::editor);
    cl.prefix = std::string(get_str(c, "prefix").value_or(""));
    cl.token = std::string(get_str(c, "token").value_or(""));
    cl.state = State::gone;
    cl.last_seen = now;
    cl.gone_at = now;
    cl.last_n = get_u64(c, "last_n").value_or(0);
    cl.bucket_at = now;
    cl.of = static_cast<std::uint32_t>(get_u64(c, "of").value_or(0));
    s.clients_[cl.id] = std::move(cl);
  }
  for (const Mp& e : get_arr(st, "log")) {
    const auto q = get_u64(e, "q");
    if (!q) {
      return std::unexpected("log seq");
    }
    Commit cm;
    cm.q = *q;
    cm.c = static_cast<std::uint32_t>(get_u64(e, "c").value_or(0));
    cm.n = get_u64(e, "n").value_or(0);
    cm.label = std::string(get_str(e, "label").value_or(""));
    if (const Mp* o = field(e, "ops")) {
      cm.ops = *o;
    }
    const Mp* t = field(e, "touched");
    cm.touched = t != nullptr ? *t : Mp(MpArray{});
    s.log_.push_back(std::move(cm));
  }
  for (const Mp& k : get_arr(st, "keys")) {
    if (!k.is_array()) {
      return std::unexpected("key");
    }
    const auto& a = k.as_array();
    if (a.empty() || !a[0].is_string()) {
      return std::unexpected("key name");
    }
    const auto at = [&a](std::size_t i) -> std::uint64_t {
      std::uint64_t v = 0;
      return i < a.size() && a[i].to_uint(v) ? v : 0;
    };
    if (const std::uint64_t sr = at(4); sr > 0) {
      s.struct_rev_[a[0].as_string()] = sr;
    }
    s.keys_[a[0].as_string()] =
        KeyState{at(1), static_cast<std::uint32_t>(at(2)), a.size() > 3 && a[3].is_bool() && a[3].as_bool()};
  }
  for (const Mp& p : get_arr(st, "props")) {
    if (!p.is_array()) {
      return std::unexpected("prop");
    }
    const auto& a = p.as_array();
    if (a.empty() || !a[0].is_string()) {
      return std::unexpected("prop key");
    }
    std::uint64_t q = 0;
    std::uint64_t by = 0;
    if (a.size() > 1) {
      (void)a[1].to_uint(q);
    }
    if (a.size() > 2) {
      (void)a[2].to_uint(by);
    }
    s.props_[a[0].as_string()] = {q, static_cast<std::uint32_t>(by)};
  }
  const std::string context = "rejoin:" + s.id_;
  for (const auto& [id, c] : s.clients_) {
    if (c.ticket.empty()) {
      continue;
    }
    Invite k;
    k.kind = KeyKind::ticket;
    k.secret = c.ticket;
    k.context = context;
    k.ttl = std::numeric_limits<Ms>::max() / 4;
    k.expires = k.ttl;
    s.invites_[ticket_kid(id)] = std::move(k);
  }
  for (const Mp& i : get_arr(st, "invites")) {
    const auto code = get_str(i, "code");
    const auto secret = get_bin(i, "secret");
    if (!code || !secret) {
      continue;
    }
    const auto parsed = parse_code(*code);
    if (!parsed) {
      continue;
    }
    const Bytes sec(parsed->secret.begin(), parsed->secret.end());
    if (sec != *secret || s.invites_.contains(parsed->tag)) {
      continue;
    }
    const Ms left = std::min<Ms>(get_u64(i, "left").value_or(0), Ms{24} * 3'600'000);
    if (left == 0) {
      continue;
    }
    Invite k;
    k.kind = KeyKind::code;
    k.code = std::string(*code);
    k.secret = sec;
    k.context = "lan:" + parsed->tag;
    k.one_time = get_bool(i, "one_time").value_or(false);
    k.ttl = left;
    k.expires = now + left;
    s.invites_[parsed->tag] = std::move(k);
    s.carried_.push_back(parsed->tag);
  }
  return s;
}

// ---------------------------------------------------------------------------------------------------- connections
void SessionServer::open(ConnId conn, bool trusted, std::string_view via, Ms now) {
  Conn cx;
  cx.trusted = trusted;
  cx.secure = trusted || cfg_.plain;
  cx.via = std::string(via);
  cx.opened = now;
  conns_[conn] = std::move(cx);
}

std::vector<Out> SessionServer::closed(ConnId conn, Ms now) {
  std::vector<Out> out;
  const auto it = conns_.find(conn);
  if (it == conns_.end()) {
    return out;
  }
  Conn cx = std::move(it->second);
  conns_.erase(it);
  if (cx.hs) {
    wrong_try(cx.hs->kid, now, out);  // quit after message 2 without proving the code
  }
  if (cx.client) {
    const auto c = clients_.find(*cx.client);
    if (c != clients_.end() && c->second.conn == conn) {
      drop_client(*cx.client, now, false, out);
    }
  }
  return out;
}

// NOLINTNEXTLINE(misc-no-recursion) a person leaving drops their assistants, who have none of their own
void SessionServer::drop_client(std::uint32_t c, Ms now, bool removed, std::vector<Out>& out) {
  auto it = clients_.find(c);
  if (it == clients_.end() || it->second.state == State::gone) {
    return;
  }
  const bool was_waiting = it->second.state == State::waiting;
  const Role role = it->second.role;
  it->second.state = State::gone;
  it->second.gone_at = now;
  if (handover_ == Handover::waiting && handover_to_ == c) {
    // the new host went before it said where it is: hand over called off, edits commit here again
    handover_ = Handover::none;
    to_role(Role::host, env("handover-off", map_of({{"to", Mp(c)}, {"reason", Mp("gone")}})), out);
  }
  it = clients_.find(c);
  if (it == clients_.end()) {
    return;
  }
  if (removed) {
    it->second.token.clear();  // no rejoin
    invites_.erase(ticket_kid(c));
  }
  if (it->second.token.empty()) {
    remove_assistants(c, now, out);
  }
  it = clients_.find(c);
  if (it == clients_.end()) {
    return;
  }
  if (auto conn = it->second.conn) {
    it->second.conn.reset();
    conns_.erase(*conn);
    out.emplace_back(OutClose{*conn, removed ? "removed" : "gone"});
  }
  if (was_waiting) {
    clients_.erase(c);
    return;
  }
  if (replica_ == c) {
    replica_.reset();
    if (validating_) {
      queue_.push_front(std::move(*validating_));  // decided again without a validator
      validating_.reset();
    }
  }
  if (snap_busy_ == c) {
    snap_busy_.reset();
  }
  std::erase(snap_wait_, c);
  std::vector<std::string> released;
  for (const auto& [k, h] : holds_) {
    if (h.first == c) {
      released.push_back(k);
    }
  }
  for (const std::string& k : released) {
    holds_.erase(k);
  }
  if (!released.empty()) {
    broadcast(holds_change({}, released), std::nullopt, out);
  }
  if (role != Role::replica) {
    broadcast(env("peer", map_of({{"c", Mp(c)}, {"state", Mp(removed ? "removed" : "gone")}})), c, out);
  }
  if (role == Role::host && !removed) {
    end("host-left", out);  // the host quit: every client keeps its copy
  }
  pump_snapshots(out);
}

// ---------------------------------------------------------------------------------------------------- frames
std::vector<Out> SessionServer::frame(ConnId conn, Envelope e, Ms now) {
  now_ = now;
  std::vector<Out> out;
  if (ended_) {
    out.emplace_back(OutClose{conn, "ended"});
    return out;
  }
  const auto it = conns_.find(conn);
  if (it == conns_.end()) {
    return out;
  }
  if (!it->second.secure) {
    // a test harness's host or replica (local token, never set in Studio) says hello without a handshake
    const bool tok =
        !cfg_.local_token.empty() && get_str(e.b, "token") == std::optional<std::string_view>(cfg_.local_token);
    if (e.t == "hello" && tok && !it->second.client) {
      it->second.secure = true;
      on_hello(conn, e, now, out);
      pump(out);
      return out;
    }
    if (e.t == "pake") {
      on_pake(conn, e, now, out);
    } else {
      conns_.erase(conn);
      out.emplace_back(OutClose{conn, "handshake expected"});
    }
    return out;
  }
  if (!it->second.client) {
    on_hello(conn, e, now, out);
  } else {
    const std::uint32_t c = *it->second.client;
    if (auto cl = clients_.find(c); cl != clients_.end()) {
      cl->second.last_seen = now;
    }
    on_message(c, conn, std::move(e), now, out);
  }
  pump(out);
  return out;
}

void SessionServer::on_pake(ConnId conn, const Envelope& e, Ms now, std::vector<Out>& out) {
  const Bytes m = get_bin(e.b, "m").value_or(Bytes{});
  Conn& cx = conns_.at(conn);
  if (!cx.hs) {
    const std::string kid(get_str(e.b, "kid").value_or(""));
    const auto key = invites_.find(kid);
    if (key == invites_.end() || key->second.expires <= now) {
      conns_.erase(conn);  // unknown or expired: nothing was tried, so not counted
      out.emplace_back(OutClose{conn, "unknown code"});
      return;
    }
    auto host = std::make_unique<HostHandshake>(key->second.secret, key->second.context);
    auto reply = host->receive(m);
    if (!reply) {
      conns_.erase(conn);
      out.emplace_back(OutClose{conn, "bad handshake"});
      return;
    }
    cx.kid = kid;
    cx.hs = Handshake{kid, now, std::move(host)};
    out.emplace_back(OutTo{conn, env("pake", map_of({{"m", Mp(std::move(*reply))}}))});
    return;
  }
  const std::string kid = cx.hs->kid;
  std::unique_ptr<HostHandshake> host = std::move(cx.hs->host);
  if (!host) {
    return;
  }
  auto fin = host->finish(m);
  if (!fin) {
    cx.hs.reset();  // wrong_code (or a malformed confirmation): a wrong try, and the connection closes
    conns_.erase(conn);
    out.emplace_back(OutClose{conn, "wrong code"});
    wrong_try(kid, now, out);
    return;
  }
  cx.hs.reset();
  cx.secure = true;
  cx.sealed = true;
  cx.aud = to_b64url(fin->second.exporter("assertion"));
  const cc::live::Key t = fin->second.exporter("ticket");
  cx.ticket.assign(t.begin(), t.end());
  out.emplace_back(OutTo{conn, env("pake", map_of({{"m", Mp(fin->first)}}))});
  out.emplace_back(OutSecure{conn, std::make_shared<Session>(std::move(fin->second))});
  std::uint32_t of = 0;
  if (auto k = invites_.find(kid); k != invites_.end()) {
    k->second.wrong = 0;
    if (k->second.kind == KeyKind::assistant) {
      of = k->second.person;
    }
    if (k->second.one_time) {
      invites_.erase(k);
      if (of == 0) {
        to_role(Role::host, code_gone(kid, "used"), out);
      }
    }
  }
  if (of != 0) {
    if (auto c = conns_.find(conn); c != conns_.end()) {
      c->second.of = of;  // an assistant's key (LV.18)
    }
  }
}

void SessionServer::wrong_try(const std::string& kid, Ms now, std::vector<Out>& out) {
  ++stats_.wrong_tries;
  auto it = invites_.find(kid);
  if (it == invites_.end()) {
    return;
  }
  if (++it->second.wrong < kWrongTries) {
    return;
  }
  ++stats_.rotations;
  const Invite key = std::move(it->second);
  invites_.erase(it);
  if (key.kind == KeyKind::ticket || key.kind == KeyKind::assistant) {
    return;  // nobody's invite: just gone
  }
  to_role(Role::host, code_gone(kid, "wrong-tries"), out);
  if (key.kind == KeyKind::code) {
    make_code(key.one_time, key.ttl, kid, now, out);
  }
}

void SessionServer::make_code(bool one_time, Ms ttl, std::optional<std::string> replaces, Ms now,
                              std::vector<Out>& out) {
  std::string code;
  std::optional<Code> parsed;
  for (;;) {
    code = new_code();
    parsed = parse_code(code);
    if (parsed && !invites_.contains(parsed->tag)) {
      break;
    }
  }
  Invite k;
  k.kind = KeyKind::code;
  k.code = code;
  k.secret.assign(parsed->secret.begin(), parsed->secret.end());
  k.context = "lan:" + parsed->tag;
  k.one_time = one_time;
  k.ttl = ttl;
  k.expires = now + ttl;
  invites_[parsed->tag] = std::move(k);
  Map b;
  b.set("code", Mp(code));
  b.set("tag", Mp(parsed->tag));
  b.set("expires", Mp(ttl));
  b.set("one_time", Mp(one_time));
  if (replaces) {
    b.set("reason", Mp("wrong-tries"));
    b.set("replaces", Mp(*replaces));
  }
  to_role(Role::host, env("code", Mp(std::move(b))), out);
}

std::vector<Out> SessionServer::notice(const Envelope& e) {
  std::vector<Out> out;
  to_role(Role::host, e, out);
  return out;
}

// ---------------------------------------------------------------------------------------------------- hello
void SessionServer::on_hello(ConnId conn, const Envelope& e, Ms now, std::vector<Out>& out) {
  const auto deny = [&](std::string_view reason) {
    conns_.erase(conn);
    out.emplace_back(OutTo{conn, env("deny", map_of({{"reason", Mp(reason)}}))});
    out.emplace_back(OutClose{conn, std::string(reason)});
  };
  if (e.t != "hello") {
    conns_.erase(conn);
    out.emplace_back(OutClose{conn, "hello expected"});
    return;
  }
  const Mp& b = e.b;
  if (get_u64(b, "v").value_or(0) != 1) {
    deny("protocol");
    return;
  }
  const std::string_view app = get_str(b, "app").value_or("");
  if (!cfg_.app.empty() && app != cfg_.app) {
    deny("app");
    return;
  }
  if (get_u64(b, "schema").value_or(0) != cfg_.schema) {
    deny("schema");
    return;
  }
  const auto want = role_of_name(get_str(b, "role").value_or(""));
  const bool tok_ok =
      !cfg_.local_token.empty() && get_str(b, "token") == std::optional<std::string_view>(cfg_.local_token);
  if (want == Role::host || want == Role::replica) {
    if (!(conns_.at(conn).trusted || tok_ok)) {
      deny("role");
      return;
    }
    if (*want == Role::host && std::ranges::any_of(clients_, [](const auto& kv) {
          return kv.second.role == Role::host && kv.second.state != State::gone;
        })) {
      deny("host-taken");
      return;
    }
    const std::uint32_t c = new_client(conn, b, *want, now);
    if (*want == Role::replica) {
      replica_ = c;
    }
    activate(c, out);
    return;
  }
  // Rejoin: client id + resume token, no new approval.
  if (const Mp* r = field(b, "resume")) {
    const auto rc = static_cast<std::uint32_t>(get_u64(*r, "c").value_or(0));
    const std::string_view tok = get_str(*r, "token").value_or("");
    const auto cl_it = clients_.find(rc);
    if (cl_it == clients_.end() || cl_it->second.token.empty() || cl_it->second.token != tok ||
        cl_it->second.state == State::waiting) {
      deny("resume");
      return;
    }
    if (auto old = cl_it->second.conn) {
      conns_.erase(*old);
      out.emplace_back(OutClose{*old, "replaced"});
    }
    const std::uint64_t from = get_u64(*r, "q").value_or(0);
    Client& cl = clients_.at(rc);
    const Conn& cx = conns_.at(conn);
    const bool was_gone = cl.state == State::gone;
    cl.conn = conn;
    cl.state = State::active;
    cl.last_seen = now;
    cl.via = cx.via;
    if (!cx.kid.empty() && !cx.kid.starts_with("h:")) {
      cl.kid = cx.kid;
    }
    cl.aud = cx.aud;
    if (cl.ticket.empty() && !cx.ticket.empty()) {
      cl.ticket = cx.ticket;  // the first sealed connection's stays theirs
    }
    cl.hash = std::string(get_str(b, "hash").value_or(""));
    conns_.at(conn).client = rc;
    // holds come back if nobody took the objects
    std::vector<std::pair<std::string, std::uint32_t>> taken;
    for (const std::string& k : strings_of(get_arr(*r, "holds"), 10'000)) {
      if (!holds_.contains(k)) {
        holds_[k] = {rc, now + kHoldTtl};
        taken.emplace_back(k, rc);
      }
    }
    welcome(rc, from, out);
    rejoin_key(rc);
    if (clients_.at(rc).role == Role::host) {
      tell_carried(out);
    }
    // refusals it may have missed: every one at or after the oldest edit it still has pending
    const std::uint64_t oldest = get_u64(*r, "n").value_or(std::numeric_limits<std::uint64_t>::max());
    for (const auto& [n, rb] : clients_.at(rc).refusals) {
      if (n < oldest) {
        continue;
      }
      Envelope again = env("refuse", rb);
      again.n = n;
      out.emplace_back(OutTo{conn, std::move(again)});
    }
    if (!taken.empty()) {
      broadcast(holds_change(taken, {}), std::nullopt, out);
    }
    if (was_gone) {
      broadcast(env("peer", map_of({{"c", Mp(rc)}, {"state", Mp("back")}})), rc, out);
    }
    return;
  }
  // An assistant (LV.18): its person's one-time key, through the handshake or in hello.assistant.
  std::uint32_t of = conns_.at(conn).of;
  if (of == 0) {
    if (const Mp* a = field(b, "assistant")) {
      const bool open = conns_.at(conn).trusted || cfg_.plain || tok_ok;
      const std::string kid(get_str(*a, "kid").value_or(""));
      const Bytes sec = get_bin(*a, "secret").value_or(Bytes{});
      const auto k = invites_.find(kid);
      const bool ok = open && k != invites_.end() && k->second.kind == KeyKind::assistant && k->second.expires > now &&
                      same_bytes(k->second.secret, sec);
      if (!ok) {
        deny("role");
        return;
      }
      of = k->second.person;
      invites_.erase(k);
    }
  }
  if (of != 0) {
    const auto p = clients_.find(of);
    const bool person_ok = p != clients_.end() && p->second.state == State::active &&
                           (p->second.role == Role::host || p->second.role == Role::editor);
    if (!person_ok) {
      deny("role");
      return;
    }
  }
  const auto people = static_cast<std::size_t>(std::ranges::count_if(
      clients_, [](const auto& kv) { return kv.second.state != State::gone && kv.second.role != Role::replica; }));
  if (people > cfg_.max_clients) {
    deny("full");
    return;
  }
  const std::uint32_t c = new_client(conn, b, of != 0 ? Role::assistant : Role::editor, now);
  clients_.at(c).of = of;
  if (cfg_.auto_approve || (of != 0 && clients_.at(of).role == Role::host)) {
    activate(c, out);
    return;
  }
  out.emplace_back(OutTo{conn, env("waiting")});
  const Client& cl = clients_.at(c);
  Map jb;
  jb.set("c", Mp(c));
  jb.set("name", Mp(cl.name));
  jb.set("device", Mp(cl.device));
  jb.set("via", Mp(cl.via));
  jb.set("kid", Mp(cl.kid));
  jb.set("proof", cl.proof);
  jb.set("assertion", Mp(cl.assertion));
  jb.set("picture", cl.picture);
  if (!cl.aud.empty()) {
    jb.set("aud", Mp(cl.aud));
  }
  if (cl.of != 0) {
    const auto p = clients_.find(cl.of);
    jb.set("assistant",
           map_of({{"of", Mp(cl.of)}, {"of_name", Mp(p != clients_.end() ? p->second.name : std::string())}}));
  }
  to_role(Role::host, env("join", Mp(std::move(jb))), out);
}

std::uint32_t SessionServer::new_client(ConnId conn, const Mp& b, Role role, Ms now) {
  const std::uint32_t id = next_client_++;
  Conn& cx = conns_.at(conn);
  cx.client = id;
  Client cl;
  cl.id = id;
  cl.name = take_chars(get_str(b, "name").value_or(""), 80);
  cl.device = take_chars(get_str(b, "device").value_or(""), 80);
  cl.via = cx.via;
  cl.kid = cx.kid;
  if (const Mp* p = field(b, "proof")) {
    cl.proof = *p;
  }
  if (const auto a = get_str(b, "assertion"); a && a->size() <= 2048) {
    cl.assertion = std::string(*a);
  }
  if (const auto p = get_bin(b, "picture"); p && p->size() <= std::size_t{16} * 1024) {
    cl.picture = Mp(*p);
  }
  cl.aud = cx.aud;
  cl.ticket = cx.ticket;
  cl.role = role;
  cl.prefix = prefix_for(id);
  cl.token = rand_hex(16);
  cl.conn = conn;
  cl.state = State::waiting;
  cl.last_seen = now;
  cl.bucket_at = now;
  cl.hash = std::string(get_str(b, "hash").value_or(""));
  clients_[id] = std::move(cl);
  return id;
}

void SessionServer::rejoin_key(std::uint32_t c) {
  const auto it = clients_.find(c);
  if (it == clients_.end()) {
    return;
  }
  const Client& cl = it->second;
  const bool sealed = cl.conn && conns_.contains(*cl.conn) && conns_.at(*cl.conn).sealed;
  const std::string kid = ticket_kid(c);
  if (!sealed || cl.ticket.empty() || invites_.contains(kid)) {
    return;
  }
  Invite k;
  k.kind = KeyKind::ticket;
  k.secret = cl.ticket;
  k.context = "rejoin:" + id_;
  k.ttl = std::numeric_limits<Ms>::max() / 4;
  k.expires = k.ttl;
  invites_[kid] = std::move(k);
}

void SessionServer::tell_carried(std::vector<Out>& out) {
  const auto tags = std::exchange(carried_, {});
  for (const std::string& tag : tags) {
    const auto it = invites_.find(tag);
    if (it == invites_.end() || it->second.kind != KeyKind::code) {
      continue;
    }
    const Invite& k = it->second;
    to_role(Role::host,
            env("code", map_of({{"code", Mp(k.code)},
                                {"tag", Mp(tag)},
                                {"expires", Mp(k.expires > now_ ? k.expires - now_ : 0)},
                                {"one_time", Mp(k.one_time)},
                                {"carried", Mp(true)}})),
            out);
  }
}

void SessionServer::activate(std::uint32_t c, std::vector<Out>& out) {
  const auto it = clients_.find(c);
  if (it == clients_.end()) {
    return;
  }
  it->second.state = State::active;
  const Role role = it->second.role;
  welcome(c, std::nullopt, out);
  rejoin_key(c);
  if (role == Role::host) {
    tell_carried(out);
  }
  if (role != Role::replica) {
    const Client& cl = clients_.at(c);
    Map pb;
    pb.set("c", Mp(c));
    pb.set("state", Mp("in"));
    pb.set("name", Mp(cl.name));
    pb.set("role", Mp(role_name(role)));
    pb.set("via", Mp(cl.via));
    if (cl.of != 0) {
      pb.set("of", Mp(cl.of));
    }
    broadcast(env("peer", Mp(std::move(pb))), c, out);
  }
}

void SessionServer::welcome(std::uint32_t c, std::optional<std::uint64_t> from_in, std::vector<Out>& out) {
  const Client& cl = clients_.at(c);
  if (!cl.conn) {
    return;
  }
  const ConnId conn = *cl.conn;
  // the log reaches back to (first kept seq - 1): a client at that seq or later can be caught up from it
  const std::uint64_t reach = log_.empty() ? seq_ : log_.front().q - 1;
  std::optional<std::uint64_t> from;
  if (from_in) {
    if (*from_in <= seq_ && *from_in >= reach) {
      from = from_in;
    }
  } else if (!cl.hash.empty() && cl.hash == saved_hash_ && saved_q_ >= reach) {
    from = saved_q_;
  }
  MpArray peers;
  for (const auto& [id, p] : clients_) {
    if (id == c || p.state != State::active || p.role == Role::replica) {
      continue;
    }
    Map v;
    v.set("c", Mp(id));
    v.set("name", Mp(p.name));
    v.set("role", Mp(role_name(p.role)));
    v.set("via", Mp(p.via));
    if (p.of != 0) {
      v.set("of", Mp(p.of));
    }
    peers.emplace_back(std::move(v));
  }
  Map holds;
  for (const auto& [k, h] : holds_) {
    holds.set(k, Mp(h.first));
  }
  std::vector<std::string> locks(locks_.begin(), locks_.end());
  const bool assistant = cl.of != 0;
  std::string catchup = "snapshot";
  if (cl.role != Role::replica && from) {
    catchup = "log";
  } else if (cl.role == Role::replica || assistant) {
    catchup = "none";
  }
  Map b;
  b.set("c", Mp(c));
  b.set("prefix", Mp(cl.prefix));
  b.set("role", Mp(role_name(cl.role)));
  b.set("token", Mp(cl.token));
  b.set("session", Mp(id_));
  b.set("app", Mp(cfg_.app));
  b.set("schema", Mp(cfg_.schema));
  b.set("peers", Mp(std::move(peers)));
  b.set("holds", Mp(std::move(holds)));
  b.set("locks", strs(locks));
  b.set("saved", map_of({{"q", Mp(saved_q_)}, {"hash", Mp(saved_hash_)}}));
  b.set("catchup", Mp(catchup));
  b.set("last_n", Mp(cl.last_n));
  Envelope w = env("welcome", Mp(std::move(b)));
  w.s = id_;
  w.q = seq_;
  out.emplace_back(OutTo{conn, std::move(w)});
  if (catchup == "log" && from) {
    for (const Commit& cm : log_) {
      if (cm.q > *from) {
        out.emplace_back(OutTo{conn, [&cm] {
                                 Envelope e = env("commit", map_of({{"c", Mp(cm.c)},
                                                                    {"n", Mp(cm.n)},
                                                                    {"label", Mp(cm.label)},
                                                                    {"ops", cm.ops},
                                                                    {"touched", cm.touched}}));
                                 e.q = cm.q;
                                 e.c = cm.c;
                                 e.n = cm.n;
                                 return e;
                               }()});
      }
    }
  } else if (catchup == "snapshot") {
    snap_wait_.push_back(c);
    pump_snapshots(out);
  }
}

namespace {
Envelope commit_env(std::uint64_t q, std::uint32_t c, std::uint64_t n, const std::string& label, const Mp& ops,
                    const Mp& touched) {
  Envelope e;
  e.t = "commit";
  e.b = map_of({{"c", Mp(c)}, {"n", Mp(n)}, {"label", Mp(label)}, {"ops", ops}, {"touched", touched}});
  e.q = q;
  e.c = c;
  e.n = n;
  return e;
}
}  // namespace

void SessionServer::pump_snapshots(std::vector<Out>& out) {
  if (snap_busy_) {
    return;
  }
  while (!snap_wait_.empty()) {
    const std::uint32_t c = snap_wait_.front();
    snap_wait_.pop_front();
    const auto it = clients_.find(c);
    if (it == clients_.end() || it->second.state != State::active) {
      continue;
    }
    std::optional<std::uint32_t> src = replica_ ? replica_ : role_client(Role::host);
    if (src == c) {
      src.reset();
    }
    if (!src) {
      // nobody can make one (a test server without host or replica): an empty snapshot at seq 0, then the log
      Envelope e = env("snap", map_of({{"i", Mp(0)}, {"of", Mp(0)}}));
      send(c, std::move(e), out);
      if (log_.empty() || log_.front().q <= 1) {
        for (const Commit& cm : log_) {
          send(c, commit_env(cm.q, cm.c, cm.n, cm.label, cm.ops, cm.touched), out);
        }
      }
      continue;
    }
    snap_busy_ = c;
    send(*src, env("snapreq", map_of({{"for", Mp(c)}})), out);
    return;
  }
}

// ---------------------------------------------------------------------------------------------------- messages
void SessionServer::on_message(std::uint32_t c, ConnId conn, Envelope e, Ms now, std::vector<Out>& out) {
  const Role role = clients_.at(c).role;
  const State state = clients_.at(c).state;
  if (state == State::waiting) {
    // nothing reaches a joiner before approval, and nothing from it counts except bye
    if (e.t == "bye") {
      conns_.erase(conn);
      clients_.erase(c);
      out.emplace_back(OutClose{conn, "bye"});
    }
    return;
  }
  const std::string& t = e.t;
  if (t == "ping") {
    Envelope p;
    p.t = "pong";
    p.n = e.n;
    out.emplace_back(OutTo{conn, std::move(p)});
  } else if (t == "pong") {
  } else if (t == "bye") {
    if (auto it = clients_.find(c); it != clients_.end()) {
      it->second.token.clear();  // a clean leave: no rejoin
    }
    drop_client(c, now, false, out);
  } else if (t == "edit") {
    on_edit(c, conn, std::move(e), now, out);
  } else if (t == "assistant") {
    on_assistant(c, conn, now, out);
  } else if (t == "hold") {
    std::vector<std::pair<std::string, std::uint32_t>> set;
    for (const std::string& k : strings_of(get_arr(e.b, "keys"), 10'000)) {
      const auto it = holds_.find(k);
      if (it != holds_.end() && it->second.first != c) {
        continue;
      }
      const bool fresh = it == holds_.end();
      holds_[k] = {c, now + kHoldTtl};
      if (fresh) {
        set.emplace_back(k, c);
      }
    }
    if (!set.empty()) {
      broadcast(holds_change(set, {}), std::nullopt, out);
    }
  } else if (t == "release") {
    const bool all = get_bool(e.b, "all") == true;
    std::vector<std::string> keys;
    if (all) {
      for (const auto& [k, h] : holds_) {
        if (h.first == c) {
          keys.push_back(k);
        }
      }
    } else {
      keys = strings_of(get_arr(e.b, "keys"));
    }
    std::vector<std::string> gone;
    for (const std::string& k : keys) {
      const auto it = holds_.find(k);
      if (it != holds_.end() && it->second.first == c) {
        holds_.erase(it);
        gone.push_back(k);
      }
    }
    if (!gone.empty()) {
      broadcast(holds_change({}, gone), std::nullopt, out);
    }
  } else if (t == "presence") {
    Client& cl = clients_.at(c);
    cl.presence = std::move(e.b);
    cl.presence_dirty = true;
  } else if ((t == "approve" || t == "deny" || t == "remove" || t == "role") && role == Role::host) {
    host_people(e, now, out);
  } else if ((t == "lock" || t == "unlock") && role == Role::host) {
    std::vector<std::string> add;
    std::vector<std::string> del;
    for (const std::string& k : strings_of(get_arr(e.b, "keys"), 100'000)) {
      if (t == "lock") {
        if (locks_.insert(k).second) {
          add.push_back(k);
        }
      } else if (locks_.erase(k) != 0) {
        del.push_back(k);
      }
    }
    if (!add.empty() || !del.empty()) {
      broadcast(env("locks", map_of({{"add", strs(add)}, {"del", strs(del)}})), std::nullopt, out);
    }
  } else if (t == "invite" && role == Role::host) {
    const bool one_time = get_bool(e.b, "one_time").value_or(false);
    const Ms ttl = std::clamp<Ms>(get_u64(e.b, "ttl").value_or(kCodeTtl), 10'000, Ms{24} * 3'600'000);
    make_code(one_time, ttl, std::nullopt, now, out);
  } else if (t == "key" && role == Role::host) {
    const std::string kid(get_str(e.b, "kid").value_or(""));
    Bytes secret = get_bin(e.b, "secret").value_or(Bytes{});
    const std::string context(get_str(e.b, "context").value_or(""));
    if (kid.empty() || kid.size() > 64 || secret.size() < 16 || !context.starts_with("relay:")) {
      return;
    }
    const Ms ttl = get_u64(e.b, "ttl").value_or(Ms{8} * 3'600'000);
    Invite k;
    k.kind = KeyKind::link;
    k.secret = std::move(secret);
    k.context = context;
    k.one_time = get_bool(e.b, "one_time").value_or(false);
    k.ttl = ttl;
    k.expires = now + ttl;
    invites_[kid] = std::move(k);
  } else if (t == "uninvite" && role == Role::host) {
    std::vector<std::string> kids;
    if (const auto one = get_str(e.b, "kid")) {
      kids.emplace_back(*one);
    } else {
      for (const auto& [kid, k] : invites_) {
        if (k.kind != KeyKind::ticket) {
          kids.push_back(kid);  // handoff tickets are not invites
        }
      }
    }
    for (const std::string& k : kids) {
      if (invites_.erase(k) != 0) {
        to_role(Role::host, code_gone(k, "stopped"), out);
      }
    }
  } else if (t == "save" && role == Role::host) {
    saved_q_ = std::min(get_u64(e.b, "q").value_or(seq_), seq_);
    saved_hash_ = take_chars(get_str(e.b, "hash").value_or(""), 200);
    const Mp* at = field(e.b, "at");
    broadcast(
        env("saved", map_of({{"q", Mp(saved_q_)}, {"hash", Mp(saved_hash_)}, {"at", at != nullptr ? *at : Mp()}})),
        std::nullopt, out);
  } else if (t == "end" && role == Role::host) {
    end("host-ended", out);
  } else if (t == "handover" && role == Role::host) {
    on_handover(c, e, out);
  } else if (t == "moved") {
    on_moved(c, e, out);
  } else if (t == "valid" && role == Role::replica) {
    on_valid(e, out);
  } else if (t == "snap" && (role == Role::replica || role == Role::host)) {
    on_snap(e, out);
  }
}

void SessionServer::on_assistant(std::uint32_t c, ConnId conn, Ms now, std::vector<Out>& out) {
  const auto reply = [](Mp b) { return env("assistant-key", std::move(b)); };
  const Client& cl = clients_.at(c);
  if ((cl.role != Role::host && cl.role != Role::editor) || cl.of != 0) {
    out.emplace_back(OutTo{conn, reply(map_of({{"reason", Mp("role")}}))});
    return;
  }
  const auto used = static_cast<std::size_t>(
      std::ranges::count_if(clients_,
                            [c](const auto& kv) { return kv.second.of == c && kv.second.state != State::gone; }) +
      std::ranges::count_if(invites_, [c, now](const auto& kv) {
        return kv.second.kind == KeyKind::assistant && kv.second.person == c && kv.second.expires > now;
      }));
  if (used >= kMaxAssistants) {
    out.emplace_back(OutTo{conn, reply(map_of({{"reason", Mp("limit")}}))});
    return;
  }
  const std::string kid = "a:" + std::to_string(c) + ":" + rand_hex(4);
  const Bytes secret = rand_bytes(32);
  const std::string context = "assistant:" + id_;
  Invite k;
  k.kind = KeyKind::assistant;
  k.person = c;
  k.secret = secret;
  k.context = context;
  k.one_time = true;
  k.ttl = kAssistantKeyTtl;
  k.expires = now + kAssistantKeyTtl;
  invites_[kid] = std::move(k);
  out.emplace_back(OutTo{conn, reply(map_of({{"kid", Mp(kid)}, {"secret", Mp(secret)}, {"context", Mp(context)}}))});
}

// NOLINTNEXTLINE(misc-no-recursion) see drop_client
void SessionServer::remove_assistants(std::uint32_t person, Ms now, std::vector<Out>& out) {
  std::erase_if(invites_, [person](const auto& kv) {
    return kv.second.kind == KeyKind::assistant && kv.second.person == person;
  });
  std::vector<std::uint32_t> theirs;
  for (const auto& [id, x] : clients_) {
    if (x.of == person && x.state != State::gone) {
      theirs.push_back(id);
    }
  }
  for (const std::uint32_t a : theirs) {
    if (const auto it = clients_.find(a); it != clients_.end()) {
      if (const std::optional<ConnId> to = it->second.conn) {
        out.emplace_back(OutTo{*to, env("removed", map_of({{"reason", Mp("person")}}))});
      }
    }
    drop_client(a, now, true, out);
  }
  if (!theirs.empty()) {
    rotate_keys(out);
  }
}

void SessionServer::host_people(const Envelope& e, Ms now, std::vector<Out>& out) {
  const auto target = static_cast<std::uint32_t>(get_u64(e.b, "c").value_or(0));
  const auto it = clients_.find(target);
  if (it == clients_.end()) {
    return;
  }
  if (it->second.role == Role::host || it->second.role == Role::replica) {
    return;
  }
  const State state = it->second.state;
  const std::string& t = e.t;
  if (t == "approve" && state == State::waiting) {
    Role role = role_of_name(get_str(e.b, "role").value_or("")).value_or(Role::editor);
    if (role == Role::host || role == Role::replica) {
      role = Role::editor;
    }
    if (it->second.of != 0 && role != Role::viewer) {
      role = Role::assistant;  // an assistant is let in as one
    }
    it->second.role = role;
    activate(target, out);
  } else if (t == "deny" && state == State::waiting) {
    if (const auto conn = it->second.conn) {
      out.emplace_back(OutTo{*conn, env("deny", map_of({{"reason", Mp("host")}}))});
      out.emplace_back(OutClose{*conn, "denied"});
      conns_.erase(*conn);
    }
    clients_.erase(it);
  } else if (t == "remove" && state != State::gone) {
    if (const auto conn = it->second.conn) {
      out.emplace_back(OutTo{*conn, env("removed", map_of({{"reason", Mp("host")}}))});
    }
    drop_client(target, now, true, out);
    rotate_keys(out);
  } else if (t == "role" && state == State::active) {
    Role role = role_of_name(get_str(e.b, "role").value_or("")).value_or(Role::viewer);
    if (it->second.of != 0 && role == Role::editor) {
      role = Role::assistant;
    }
    if (role == Role::editor || role == Role::viewer || role == Role::assistant) {
      it->second.role = role;
      broadcast(env("peer", map_of({{"c", Mp(target)}, {"state", Mp("role")}, {"role", Mp(role_name(role))}})),
                std::nullopt, out);
      if (role == Role::viewer && clients_.at(target).of == 0) {
        remove_assistants(target, now, out);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------- handoff
void SessionServer::on_handover(std::uint32_t host, const Envelope& e, std::vector<Out>& out) {
  if (handover_ != Handover::none) {
    return;
  }
  const auto to = static_cast<std::uint32_t>(get_u64(e.b, "to").value_or(0));
  const auto it = clients_.find(to);
  const bool ok = it != clients_.end() && to != host && it->second.state == State::active &&
                  (it->second.role == Role::editor || it->second.role == Role::viewer);
  if (!ok) {
    send(host, env("handover-off", map_of({{"to", Mp(to)}, {"reason", Mp("unknown")}})), out);
    return;
  }
  handover_ = Handover::waiting;
  handover_to_ = to;
  const Bytes bytes = pack(handover_state(to));
  const bool relay = std::ranges::any_of(invites_, [](const auto& kv) { return kv.second.kind == KeyKind::link; }) ||
                     std::ranges::any_of(clients_, [](const auto& kv) {
                       return kv.second.state == State::active && kv.second.via == "relay";
                     });
  const std::size_t of = std::max<std::size_t>(1, (bytes.size() + kHandoverChunk - 1) / kHandoverChunk);
  for (std::size_t i = 0; i < of; ++i) {
    const std::size_t a = std::min(i * kHandoverChunk, bytes.size());
    const std::size_t z = std::min((i + 1) * kHandoverChunk, bytes.size());
    Map b;
    b.set("from", Mp(host));
    b.set("i", Mp(i));
    b.set("of", Mp(of));
    b.set("data",
          Mp(Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(a), bytes.begin() + static_cast<std::ptrdiff_t>(z))));
    if (i == 0) {
      b.set("bytes", Mp(bytes.size()));
      b.set("relay", Mp(relay));
    }
    send(to, env("handover", Mp(std::move(b))), out);
  }
}

void SessionServer::on_moved(std::uint32_t c, const Envelope& e, std::vector<Out>& out) {
  if (handover_ != Handover::waiting || handover_to_ != c) {
    return;
  }
  std::vector<std::string> addrs;
  for (const Mp& v : get_arr(e.b, "addrs")) {
    if (addrs.size() >= 16) {
      break;
    }
    if (v.is_string() && !v.as_string().empty() && v.as_string().size() <= 64) {
      addrs.push_back(v.as_string());
    }
  }
  const std::uint64_t port = get_u64(e.b, "port").value_or(0);
  if (addrs.empty() || port == 0 || port > 65'535) {
    return;
  }
  handover_ = Handover::done;
  std::optional<std::string> room;
  if (const auto r = get_str(e.b, "room"); r && r->size() <= 64) {
    room = std::string(*r);
  }
  for (const auto& [id, x] : clients_) {
    if (id == c || x.state != State::active || x.role == Role::replica || !x.conn) {
      continue;
    }
    const auto cx = conns_.find(*x.conn);
    const bool sealed = cx == conns_.end() || cx->second.sealed;
    Map b;
    b.set("addrs", strs(addrs));
    b.set("port", Mp(port));
    b.set("host", Mp(c));
    if (room) {
      b.set("room", Mp(*room));
    }
    if (!sealed && !x.ticket.empty()) {
      b.set("ticket", Mp(x.ticket));
    }
    out.emplace_back(OutTo{*x.conn, env("moved", Mp(std::move(b)))});
  }
}

void SessionServer::rotate_keys(std::vector<Out>& out) {
  for (const auto& [conn, cx] : conns_) {  // std::map: sorted by id, as session.rs sorts them
    if (!cx.sealed) {
      continue;
    }
    out.emplace_back(OutRekey{conn});
    out.emplace_back(OutTo{conn, env("rekey")});
  }
}

void SessionServer::end(std::string_view reason, std::vector<Out>& out) {
  if (ended_) {
    return;
  }
  broadcast(env("end", map_of({{"reason", Mp(reason)}, {"q", Mp(seq_)}})), std::nullopt, out);
  for (auto& [id, cl] : clients_) {
    if (cl.conn) {
      out.emplace_back(OutClose{*cl.conn, "ended"});
      cl.conn.reset();
    }
    cl.state = State::gone;
  }
  for (const auto& [conn, cx] : conns_) {
    if (!cx.client) {
      out.emplace_back(OutClose{conn, "ended"});
    }
  }
  conns_.clear();
  invites_.clear();
  ended_ = true;
}

// ---------------------------------------------------------------------------------------------------- edits
void SessionServer::on_edit(std::uint32_t c, ConnId conn, Envelope e, Ms now, std::vector<Out>& out) {
  if (clients_.at(c).role == Role::replica) {
    return;
  }
  Client& cl = clients_.at(c);
  cl.backlog.push_back(std::move(e));
  if (cl.backlog.size() > kBacklogRemove) {
    ++stats_.removed_flood;
    out.emplace_back(OutTo{conn, env("removed", map_of({{"reason", Mp("flood")}}))});
    drop_client(c, now, true, out);
    rotate_keys(out);
    return;
  }
  if (cl.backlog.size() > kBacklogWarn && !cl.warned) {
    cl.warned = true;
    out.emplace_back(OutTo{conn, env("warn", map_of({{"reason", Mp("flood")}}))});
  }
  drain_backlog(c, now);
}

void SessionServer::drain_backlog(std::uint32_t c, Ms now) {
  Client& cl = clients_.at(c);
  const double dt = static_cast<double>(now > cl.bucket_at ? now - cl.bucket_at : 0) / 1000.0;
  cl.bucket = std::min(cl.bucket + dt * kRatePerSec, kRatePerSec);
  cl.bucket_at = now;
  while (cl.bucket >= 1.0 && !cl.backlog.empty()) {
    Envelope e = std::move(cl.backlog.front());
    cl.backlog.pop_front();
    const std::uint64_t n = e.n;
    if (n <= cl.last_n) {
      continue;  // re-sent after a rejoin and already decided
    }
    cl.bucket -= 1.0;
    Queued q;
    q.c = c;
    q.n = n;
    // session.rs: items without a string key are skipped; a missing `m` is `s`
    q.touched = touched_from(field(e.b, "touched"));
    q.label = take_chars(get_str(e.b, "label").value_or(""), 200);
    const Mp* ops = field(e.b, "ops");
    q.ops = ops != nullptr ? *ops : Mp(MpArray{});
    queue_.push_back(std::move(q));
    cl.last_n = n;
  }
  if (cl.backlog.size() < kBacklogWarn / 2) {
    cl.warned = false;
  }
}

void SessionServer::pump(std::vector<Out>& out) {
  if (handover_ != Handover::none) {
    return;  // handing over: the new host decides what is still queued
  }
  while (!validating_ && !queue_.empty()) {
    Queued e = std::move(queue_.front());
    queue_.pop_front();
    if (auto why = check(e)) {
      refuse(e, why->first, why->second, out);
      continue;
    }
    if (replica_) {
      Envelope v = env("validate", map_of({{"c", Mp(e.c)}, {"n", Mp(e.n)}, {"label", Mp(e.label)}, {"ops", e.ops}}));
      v.q = seq_ + 1;
      send(*replica_, std::move(v), out);
      validating_ = std::move(e);
    } else {
      commit(std::move(e), out);
    }
  }
}

void SessionServer::on_valid(const Envelope& e, std::vector<Out>& out) {
  if (!validating_) {
    return;
  }
  Queued q = std::move(*validating_);
  validating_.reset();
  if (handover_ != Handover::none) {
    queue_.push_front(std::move(q));  // left undecided: its author re-sends it to the new host
    return;
  }
  if (get_u64(e.b, "n") != q.n || get_u64(e.b, "c") != std::optional<std::uint64_t>(q.c)) {
    refuse(q, "invalid", std::nullopt, out);  // out of step: refuse rather than guess
    return;
  }
  if (get_bool(e.b, "ok") == true) {
    commit(std::move(q), out);
  } else {
    refuse(q, take_chars(get_str(e.b, "reason").value_or("invalid"), 200), std::nullopt, out);
  }
}

std::optional<std::pair<std::string, std::optional<std::uint32_t>>> SessionServer::check(const Queued& e) const {
  using Why = std::pair<std::string, std::optional<std::uint32_t>>;
  const auto cl = clients_.find(e.c);
  if (cl == clients_.end()) {
    return Why{"gone", std::nullopt};
  }
  if (!can_edit(cl->second.role)) {
    return Why{"role", std::nullopt};
  }
  for (const Touch& t : e.touched) {
    if (locks_.contains(t.k)) {
      return Why{"locked", std::nullopt};
    }
  }
  for (const Touch& t : e.touched) {
    if (t.m == 's' || t.m == 'd' || (t.m == 'r' && t.g)) {
      const auto h = holds_.find(t.k);
      if (h != holds_.end() && h->second.first != e.c) {
        return Why{"held", h->second.first};
      }
    }
  }
  for (const Touch& t : e.touched) {
    const auto ks = keys_.find(t.k);
    const bool known = ks != keys_.end();
    const bool deleted = known && ks->second.deleted;
    const std::optional<std::uint32_t> by = known ? std::optional(ks->second.by) : std::nullopt;
    switch (t.m) {
      case 'p':
        if (deleted) {
          return Why{"deleted", by};
        }
        if (t.r && t.f) {
          const auto p = props_.find(prop_key(t.k, *t.f));
          const auto [pr, pby] = p == props_.end() ? std::pair<std::uint64_t, std::uint32_t>{0, 0} : p->second;
          if (pr != *t.r) {
            return Why{"changed", pby != 0 ? std::optional(pby) : std::nullopt};
          }
        }
        break;
      case 'c':
        if (known && !deleted) {
          return Why{"exists", by};
        }
        break;
      case 'd':
        if (deleted) {
          return Why{"deleted", by};
        }
        if (t.r) {  // delete wins over property writes; a condition is on structure only
          const auto sr = struct_rev_.find(t.k);
          if ((sr == struct_rev_.end() ? 0 : sr->second) > *t.r) {
            return Why{"changed", by};
          }
        }
        break;
      default:
        if (deleted) {
          return Why{"deleted", by};
        }
        if (t.r && (known ? ks->second.rev : 0) != *t.r) {
          return Why{"changed", by};
        }
        break;
    }
  }
  return std::nullopt;
}

void SessionServer::refuse(const Queued& e, std::string reason, std::optional<std::uint32_t> by,
                           std::vector<Out>& out) {
  ++stats_.refused;
  Map b;
  b.set("n", Mp(e.n));
  b.set("reason", Mp(std::move(reason)));
  if (by) {
    b.set("by", Mp(*by));
    if (const auto p = clients_.find(*by); p != clients_.end()) {
      b.set("by_name", Mp(p->second.name));
    }
  }
  const Mp body(std::move(b));
  if (auto cl = clients_.find(e.c); cl != clients_.end()) {
    cl->second.refusals.emplace_back(e.n, body);
    while (cl->second.refusals.size() > 1000) {
      cl->second.refusals.pop_front();
    }
  }
  Envelope r = env("refuse", body);
  r.n = e.n;
  send(e.c, std::move(r), out);
}

void SessionServer::commit(Queued e, std::vector<Out>& out) {
  const std::uint64_t q = ++seq_;
  for (const Touch& t : e.touched) {
    if (t.m == 'r') {
      continue;
    }
    KeyState& ks = keys_[t.k];
    ks.rev = q;
    ks.by = e.c;
    if (t.m == 'p') {
      if (t.f) {
        props_[prop_key(t.k, *t.f)] = {q, e.c};
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
    if (t.m == 'd' && holds_.erase(t.k) != 0) {
      broadcast(holds_change({}, {t.k}), std::nullopt, out);
    }
  }
  Commit cm;
  cm.q = q;
  cm.c = e.c;
  cm.n = e.n;
  cm.label = std::move(e.label);
  cm.ops = std::move(e.ops);
  cm.touched = commit_touched(e.touched);
  ++stats_.commits;
  broadcast(commit_env(cm.q, cm.c, cm.n, cm.label, cm.ops, cm.touched), std::nullopt, out);
  log_.push_back(std::move(cm));
  while (log_.size() > cfg_.log_keep) {
    log_.pop_front();
  }
}

void SessionServer::on_snap(const Envelope& e, std::vector<Out>& out) {
  if (!snap_busy_) {
    return;
  }
  const std::uint32_t target = *snap_busy_;
  if (get_u64(e.b, "for") != std::optional<std::uint64_t>(target)) {
    return;
  }
  const std::uint64_t q = std::min(get_u64(e.b, "q").value_or(seq_), seq_);
  const Bytes data = get_bin(e.b, "data").value_or(Bytes{});
  const std::string enc(get_str(e.b, "enc").value_or(""));
  // the keys' versions as of the snapshot, before its chunks
  std::vector<std::pair<std::string, std::uint64_t>> known;
  for (const auto& [k, v] : keys_) {
    if (v.rev > 0 && v.rev <= q) {
      known.emplace_back(k, v.rev);
    }
  }
  for (std::size_t at = 0; at < known.size(); at += kRevsChunk) {
    Map m;
    for (std::size_t i = at; i < std::min(known.size(), at + kRevsChunk); ++i) {
      m.set(known[i].first, Mp(known[i].second));
    }
    Envelope r = env("revs", map_of({{"keys", Mp(std::move(m))}}));
    r.q = q;
    send(target, std::move(r), out);
  }
  const std::size_t total = std::max<std::size_t>(1, (data.size() + kSnapshotChunk - 1) / kSnapshotChunk);
  for (std::size_t i = 0; i < total; ++i) {
    const std::size_t a = std::min(i * kSnapshotChunk, data.size());
    const std::size_t z = std::min((i + 1) * kSnapshotChunk, data.size());
    Map b;
    b.set("i", Mp(i));
    b.set("of", Mp(total));
    b.set("data",
          Mp(Bytes(data.begin() + static_cast<std::ptrdiff_t>(a), data.begin() + static_cast<std::ptrdiff_t>(z))));
    if (i == 0) {
      b.set("enc", Mp(enc));
      b.set("bytes", Mp(data.size()));
    }
    Envelope s = env("snap", Mp(std::move(b)));
    s.q = q;
    send(target, std::move(s), out);
  }
  for (const Commit& cm : log_) {
    if (cm.q > q) {
      send(target, commit_env(cm.q, cm.c, cm.n, cm.label, cm.ops, cm.touched), out);
    }
  }
  snap_busy_.reset();
  pump_snapshots(out);
}

// ---------------------------------------------------------------------------------------------------- time
std::vector<Out> SessionServer::tick(Ms now) {
  now_ = now;
  std::vector<Out> out;
  if (ended_) {
    return out;
  }
  std::vector<std::uint32_t> ids;
  for (const auto& [id, c] : clients_) {
    if (!c.backlog.empty()) {
      ids.push_back(id);
    }
  }
  for (const std::uint32_t c : ids) {
    drain_backlog(c, now);
  }
  pump(out);
  // presence fan-out
  if (now - std::min(now, last_presence_) >= kPresenceEvery) {
    last_presence_ = now;
    std::vector<std::pair<std::uint32_t, Mp>> dirty;
    for (auto& [id, c] : clients_) {
      if (c.presence_dirty && c.state == State::active) {
        c.presence_dirty = false;
        dirty.emplace_back(id, c.presence);
      }
    }
    if (!dirty.empty()) {
      for (const auto& [id, c] : clients_) {
        if (c.state != State::active || c.role == Role::replica || !c.conn) {
          continue;
        }
        Map peers;
        for (const auto& [pc, pv] : dirty) {
          if (pc != id) {
            peers.set(std::to_string(pc), pv);
          }
        }
        if (peers.empty()) {
          continue;
        }
        ++stats_.presence_out;
        out.emplace_back(OutTo{*c.conn, env("presence", map_of({{"peers", Mp(std::move(peers))}}))});
      }
    }
  }
  // pings, and 3 missed = gone
  if (now - std::min(now, last_ping_) >= kPingEvery) {
    last_ping_ = now;
    for (const auto& [conn, cx] : conns_) {
      if (!cx.client) {
        continue;
      }
      Envelope p;
      p.t = "ping";
      p.n = now;
      out.emplace_back(OutTo{conn, std::move(p)});
    }
  }
  std::vector<std::uint32_t> late;
  for (const auto& [id, c] : clients_) {
    if (c.state == State::gone || !c.conn || now - std::min(now, c.last_seen) <= kGoneAfter) {
      continue;
    }
    const auto cx = conns_.find(*c.conn);
    if (cx != conns_.end() && cx->second.trusted) {
      continue;
    }
    late.push_back(id);
  }
  for (const std::uint32_t c : late) {
    drop_client(c, now, false, out);
  }
  // handshakes that never proved the code, and connections that never said hello
  std::vector<std::pair<ConnId, std::optional<std::string>>> stale;
  for (const auto& [conn, cx] : conns_) {
    if (cx.client || cx.trusted) {
      continue;
    }
    const Ms since = cx.hs ? cx.hs->started : cx.opened;
    if (now - std::min(now, since) > kHandshakeTimeout) {
      stale.emplace_back(conn, cx.hs ? std::optional(cx.hs->kid) : std::nullopt);
    }
  }
  for (const auto& [conn, kid] : stale) {
    conns_.erase(conn);
    out.emplace_back(OutClose{conn, "timeout"});
    if (kid) {
      wrong_try(*kid, now, out);
    }
  }
  // hold expiry
  std::vector<std::string> expired;
  for (const auto& [k, h] : holds_) {
    if (h.second < now) {
      expired.push_back(k);
    }
  }
  if (!expired.empty()) {
    for (const std::string& k : expired) {
      holds_.erase(k);
    }
    broadcast(holds_change({}, expired), std::nullopt, out);
  }
  // code expiry
  std::vector<std::string> old;
  for (const auto& [kid, k] : invites_) {
    if (k.expires <= now) {
      old.push_back(kid);
    }
  }
  for (const std::string& kid : old) {
    const bool quiet = invites_.at(kid).kind == KeyKind::assistant;
    invites_.erase(kid);
    if (!quiet) {
      to_role(Role::host, code_gone(kid, "expired"), out);
    }
  }
  // forget dropped clients after the rejoin window
  std::vector<std::uint32_t> forgotten;
  for (const auto& [id, c] : clients_) {
    if (c.state == State::gone && now - std::min(now, c.gone_at) > kRejoinWindow) {
      forgotten.push_back(id);
    }
  }
  for (const std::uint32_t c : forgotten) {
    clients_.erase(c);
    invites_.erase(ticket_kid(c));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------- sending
void SessionServer::send(std::uint32_t c, Envelope e, std::vector<Out>& out) const {
  const auto it = clients_.find(c);
  if (it == clients_.end()) {
    return;
  }
  if (const std::optional<ConnId> to = it->second.conn) {
    out.emplace_back(OutTo{*to, std::move(e)});
  }
}

std::optional<std::uint32_t> SessionServer::role_client(Role role) const {
  for (const auto& [id, c] : clients_) {
    if (c.role == role && c.state == State::active) {
      return id;
    }
  }
  return std::nullopt;
}

void SessionServer::to_role(Role role, const Envelope& e, std::vector<Out>& out) const {
  for (const auto& [id, c] : clients_) {
    if (c.role == role && c.state == State::active && c.conn) {
      out.emplace_back(OutTo{*c.conn, e});
    }
  }
}

void SessionServer::broadcast(const Envelope& e, std::optional<std::uint32_t> except, std::vector<Out>& out) const {
  for (const auto& [id, c] : clients_) {
    if (c.state == State::active && except != id && c.conn) {
      out.emplace_back(OutTo{*c.conn, e});
    }
  }
}

}  // namespace cc::live
