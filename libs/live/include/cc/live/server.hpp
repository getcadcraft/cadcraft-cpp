// SPDX-License-Identifier: Apache-2.0
// cc::live — L2. Studio Live's session server: desktop-shell src-tauri/src/live/session.rs (Studio 0.5, d132555),
// ported. A pure state machine — no sockets, no clock of its own, no threads: the transport feeds it frames and the
// time and carries out what it returns (Out). What it decides (docs/LIVE-PROTOCOL.md):
// - who gets in: the PAKE handshake against the invite secrets (short codes, link keys, rejoin tickets, assistants'
//   keys), wrong-try counting and code rotation; then hello → waiting → the host approves (a rejoin skips approval);
// - the order: every edit gets the next seq or a refusal, all or nothing (the SessionCore rules: role, locks, holds,
//   versions), rate-limited per client, through the replica's validate when there is one;
// - holds (30 s unless refreshed), locks (host only), presence fan-out (16 ms), pings every 2 s (3 missed = gone),
//   flood control (warn over 200 queued, removed over 1000), 20 people, a log of 10 000 commits;
// - catch-up from the log, the same file (hash) or a snapshot (one joiner at a time, 256 KB chunks, `revs` first);
//   `saved`, `end`, host handoff (`handover` → state chunks → `moved`), assistants as their own clients (LV.18).
//
// One difference from the Rust server: map keys that are client ids (presence `peers`) are written as strings, as a JS
// client reads them anyway (rmpv writes integers there).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cc/live/crypto.hpp"
#include "cc/live/envelope.hpp"
#include "cc/live/session_core.hpp"

namespace cc::live {

using ConnId = std::uint64_t;
using Ms = std::uint64_t;

inline constexpr Ms kPingEvery = 2'000;
inline constexpr Ms kGoneAfter = 6'000;
inline constexpr Ms kPresenceEvery = 16;
inline constexpr Ms kHoldTtl = 30'000;
inline constexpr std::size_t kSnapshotChunk = std::size_t{256} * 1024;
inline constexpr std::size_t kRevsChunk = 10'000;
inline constexpr std::size_t kMaxEditBytes = std::size_t{1024} * 1024;
inline constexpr double kRatePerSec = 100.0;
inline constexpr std::size_t kBacklogWarn = 200;
inline constexpr std::size_t kBacklogRemove = 1000;
inline constexpr Ms kRejoinWindow = Ms{10} * 60'000;
inline constexpr Ms kHandshakeTimeout = 10'000;
inline constexpr Ms kCodeTtl = Ms{10} * 60'000;
inline constexpr std::uint32_t kWrongTries = 3;
inline constexpr Ms kAssistantKeyTtl = 60'000;
inline constexpr std::size_t kMaxAssistants = 4;
inline constexpr std::size_t kHandoverChunk = std::size_t{256} * 1024;

struct ServerConfig {
  std::string app;
  std::uint32_t schema = 1;
  std::size_t max_clients = 20;
  std::size_t log_keep = 10'000;
  bool auto_approve = false;  // spike and test servers: approve every joiner as an editor
  std::string local_token;    // a test harness's privileged secret; empty in Studio
  bool plain = false;         // test servers only: no handshake, no frame encryption
};

/// What the session wants done, in order.
struct OutTo {
  ConnId conn;
  Envelope env;
};
struct OutClose {
  ConnId conn;
  std::string why;
};
struct OutSecure {
  ConnId conn;
  std::shared_ptr<Session> keys;  // seal and open every later frame of this connection with these
};
struct OutRekey {
  ConnId conn;
};
using Out = std::variant<OutTo, OutClose, OutSecure, OutRekey>;

struct ServerStats {
  std::uint64_t commits = 0, refused = 0, presence_out = 0, removed_flood = 0, wrong_tries = 0, rotations = 0;
};

class SessionServer {
 public:
  explicit SessionServer(ServerConfig cfg);

  /// A session carrying on from export_state() (on the new host after a handoff): everyone starts gone.
  [[nodiscard]] static std::expected<SessionServer, std::string> import_state(ServerConfig cfg, const Mp& state,
                                                                              Ms now);
  /// The whole state (`format: cadcraft-live-state`).
  [[nodiscard]] Mp export_state() const;

  /// A transport connection opened. `trusted` = in-process; `via` = local | lan | relay.
  void open(ConnId conn, bool trusted, std::string_view via, Ms now);
  /// The transport closed (or a frame failed to open).
  [[nodiscard]] std::vector<Out> closed(ConnId conn, Ms now);
  [[nodiscard]] std::vector<Out> frame(ConnId conn, Envelope env, Ms now);
  /// Something for the host's window from outside the session (the relay's state).
  [[nodiscard]] std::vector<Out> notice(const Envelope& env);
  [[nodiscard]] std::vector<Out> tick(Ms now);

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] bool ended() const noexcept { return ended_; }
  [[nodiscard]] const ServerConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const ServerStats& stats() const noexcept { return stats_; }

 private:
  enum class State : std::uint8_t { waiting, active, gone };
  enum class KeyKind : std::uint8_t { code, link, ticket, assistant };
  enum class Handover : std::uint8_t { none, waiting, done };

  struct Client {
    std::uint32_t id = 0;
    std::string name, device, via, kid;
    Mp proof;
    std::string assertion;
    Mp picture;
    std::string aud;
    Bytes ticket;
    Role role = Role::editor;
    std::string prefix, token;
    std::optional<ConnId> conn;
    State state = State::waiting;
    Ms last_seen = 0, gone_at = 0;
    std::uint64_t last_n = 0;
    Mp presence;
    bool presence_dirty = false;
    double bucket = kRatePerSec;
    Ms bucket_at = 0;
    std::deque<Envelope> backlog;
    bool warned = false;
    std::string hash;
    std::deque<std::pair<std::uint64_t, Mp>> refusals;
    std::uint32_t of = 0;
  };
  struct Handshake {
    std::string kid;
    Ms started = 0;
    std::unique_ptr<HostHandshake> host;
  };
  struct Conn {
    std::optional<std::uint32_t> client;
    bool trusted = false;
    bool secure = false;
    bool sealed = false;
    std::optional<Handshake> hs;
    std::string via, kid;
    Ms opened = 0;
    std::string aud;
    Bytes ticket;
    std::uint32_t of = 0;
  };
  struct Invite {
    KeyKind kind = KeyKind::code;
    std::string code;          // KeyKind::code
    std::uint32_t person = 0;  // KeyKind::assistant
    Bytes secret;
    std::string context;
    bool one_time = false;
    Ms ttl = 0;
    Ms expires = 0;
    std::uint32_t wrong = 0;
  };
  struct Commit {
    std::uint64_t q = 0;
    std::uint32_t c = 0;
    std::uint64_t n = 0;
    std::string label;
    Mp ops;
    Mp touched;
  };
  struct Queued {
    std::uint32_t c = 0;
    std::uint64_t n = 0;
    std::string label;
    Mp ops;
    std::vector<Touch> touched;
  };
  struct KeyState {
    std::uint64_t rev = 0;
    std::uint32_t by = 0;
    bool deleted = false;
  };

  [[nodiscard]] Mp state(std::optional<std::uint32_t> to) const;
  Mp handover_state(std::uint32_t to);
  void drop_client(std::uint32_t c, Ms now, bool removed, std::vector<Out>& out);
  void on_pake(ConnId conn, const Envelope& env, Ms now, std::vector<Out>& out);
  void wrong_try(const std::string& kid, Ms now, std::vector<Out>& out);
  void make_code(bool one_time, Ms ttl, std::optional<std::string> replaces, Ms now, std::vector<Out>& out);
  void on_hello(ConnId conn, const Envelope& env, Ms now, std::vector<Out>& out);
  std::uint32_t new_client(ConnId conn, const Mp& b, Role role, Ms now);
  void rejoin_key(std::uint32_t c);
  void tell_carried(std::vector<Out>& out);
  void activate(std::uint32_t c, std::vector<Out>& out);
  void welcome(std::uint32_t c, std::optional<std::uint64_t> from, std::vector<Out>& out);
  void pump_snapshots(std::vector<Out>& out);
  void on_message(std::uint32_t c, ConnId conn, Envelope env, Ms now, std::vector<Out>& out);
  void on_assistant(std::uint32_t c, ConnId conn, Ms now, std::vector<Out>& out);
  void remove_assistants(std::uint32_t person, Ms now, std::vector<Out>& out);
  void host_people(const Envelope& env, Ms now, std::vector<Out>& out);
  void on_handover(std::uint32_t host, const Envelope& env, std::vector<Out>& out);
  void on_moved(std::uint32_t c, const Envelope& env, std::vector<Out>& out);
  void rotate_keys(std::vector<Out>& out);
  void end(std::string_view reason, std::vector<Out>& out);
  void on_edit(std::uint32_t c, ConnId conn, Envelope env, Ms now, std::vector<Out>& out);
  void drain_backlog(std::uint32_t c, Ms now);
  void pump(std::vector<Out>& out);
  void on_valid(const Envelope& env, std::vector<Out>& out);
  [[nodiscard]] std::optional<std::pair<std::string, std::optional<std::uint32_t>>> check(const Queued& e) const;
  void refuse(const Queued& e, std::string reason, std::optional<std::uint32_t> by, std::vector<Out>& out);
  void commit(Queued e, std::vector<Out>& out);
  void on_snap(const Envelope& env, std::vector<Out>& out);
  void send(std::uint32_t c, Envelope env, std::vector<Out>& out) const;
  [[nodiscard]] std::optional<std::uint32_t> role_client(Role role) const;
  void to_role(Role role, const Envelope& env, std::vector<Out>& out) const;
  void broadcast(const Envelope& env, std::optional<std::uint32_t> except, std::vector<Out>& out) const;

  std::string id_;
  ServerConfig cfg_;
  std::map<std::uint32_t, Client> clients_;
  std::map<ConnId, Conn> conns_;
  std::uint32_t next_client_ = 1;
  std::uint64_t seq_ = 0;
  std::deque<Commit> log_;
  std::map<std::string, KeyState> keys_;
  std::map<std::string, std::pair<std::uint64_t, std::uint32_t>> props_;
  std::map<std::string, std::uint64_t> struct_rev_;
  std::map<std::string, std::pair<std::uint32_t, Ms>> holds_;
  std::set<std::string> locks_;
  std::map<std::string, Invite> invites_;
  std::deque<Queued> queue_;
  std::optional<Queued> validating_;
  std::optional<std::uint32_t> replica_;
  std::uint64_t saved_q_ = 0;
  std::string saved_hash_;
  std::deque<std::uint32_t> snap_wait_;
  std::optional<std::uint32_t> snap_busy_;
  Ms last_presence_ = 0;
  Ms last_ping_ = 0;
  bool ended_ = false;
  Handover handover_ = Handover::none;
  std::uint32_t handover_to_ = 0;
  std::vector<std::string> carried_;
  Ms now_ = 0;
  ServerStats stats_;
};

}  // namespace cc::live
