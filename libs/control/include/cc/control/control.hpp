// SPDX-License-Identifier: Apache-2.0
// cc::control — L2. Studio's local MCP server and JSON control channel: desktop-shell src-tauri/src/control.rs
// (CadCraft Studio 0.5, d132555) with exactly its rules, on our own small HTTP/1.1 reader (no web server library):
//
// listens on 127.0.0.1 only (port 47615, else the next free one up to +20) · `Authorization: Bearer <token>` on every
// request (32 random bytes, base64url; compared in constant time; never in a URL) else 401 + WWW-Authenticate · Host
// other than 127.0.0.1:<port> / localhost:<port> → 403 · any Origin header → 403 · POST /mcp (JSON answers, no SSE):
// `initialize` issues Mcp-Session-Id (32 hex); later requests need it (missing 400, unknown 404); a notification or a
// response → 202; GET → 405; DELETE with the session → 204 · POST /control (same token, optional X-CadCraft-Session)
// · JSON only (415), ≤ 4 MB (413), query strings 400 · anything else 404. An answer with error code -32002 (the person
// pressed Disconnect) ends the session. At most 32 sessions (the oldest goes).
//
// On top of control.rs (AGENTS.md § 9): a refused request closes its connection; a request that is not HTTP is closed
// without an answer; at most 16 connections at a time; 30 s idle timeout; request heads capped at 64 KB.
//
// The JSON-RPC message itself goes to `forward(message, session)` — in an app, the dispatcher on the UI thread —
// whose answer (or nullopt for none) is sent back; it may take up to 120 s.
//
// Locks (docs/architecture.md, Locks): control.sessions, then control.token; each held only for a lookup.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cc/base/error.hpp"
#include "cc/base/value.hpp"

namespace cc::control {

inline constexpr std::uint16_t kDefaultPort = 47615;
inline constexpr std::size_t kMaxBody = std::size_t{4} * 1024 * 1024;
inline constexpr std::size_t kMaxSessions = 32;
inline constexpr std::size_t kMaxConnections = 16;
inline constexpr std::size_t kMaxHead = std::size_t{64} * 1024;
inline constexpr std::size_t kMaxHeaders = 100;

/// 32 random bytes from the operating system, base64url without padding (43 characters).
[[nodiscard]] std::string random_token();
/// 16 random bytes as 32 lower-case hex digits.
[[nodiscard]] std::string session_id();
/// Constant time for equal lengths; the same amount of work when the lengths differ.
[[nodiscard]] bool token_matches(std::string_view given, std::string_view token) noexcept;
[[nodiscard]] std::string base64url(std::span<const std::uint8_t> bytes);

struct Request {
  std::string method;
  std::string target;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool too_large = false;  // the body was over kMaxBody (only kMaxBody + 1 bytes were read)
  bool close = false;      // Connection: close, or HTTP/1.0 without keep-alive
  /// The first header of that name (case-insensitive).
  [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
};

struct Response {
  int status = 200;
  std::optional<base::Value> body;
  std::vector<std::pair<std::string, std::string>> headers;
  bool close = false;  // close the connection after it
};

/// What reading a request found when there was no request.
enum class ReadError : std::uint8_t { closed, not_http, too_big, timeout };

/// Reads one request from `recv` (a function that fills a buffer and returns the bytes read, 0 at the end, -1 on an
/// error or the idle timeout).
[[nodiscard]] std::expected<Request, ReadError> read_request(const std::function<std::ptrdiff_t(std::span<char>)>& recv,
                                                             std::string& carry);
/// The bytes of a response.
[[nodiscard]] std::string write_response(const Response& r);

using Forward =
    std::function<std::optional<std::optional<base::Value>>(const base::Value& msg, const std::string& session)>;

/// The server's state: its port, the token, the sessions (control.rs `Running`).
class Running {
 public:
  Running(std::uint16_t port, std::string token);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::string token() const;
  /// A new token: every session ends.
  void set_token(std::string token);
  [[nodiscard]] bool drop_session(const std::string& id);
  void clear_sessions();
  [[nodiscard]] std::size_t session_count() const;

  /// One request by control.rs's rules. `forward`'s outer nullopt = the hub could not answer (Internal error).
  [[nodiscard]] Response handle(const Request& req, const Forward& forward);

 private:
  struct Session {
    std::chrono::steady_clock::time_point last;
  };
  std::uint16_t port_;
  mutable std::mutex token_mutex_;  // control.token
  std::string token_;
  mutable std::mutex sessions_mutex_;  // control.sessions
  std::map<std::string, Session, std::less<>> sessions_;
};

#if !defined(__EMSCRIPTEN__)
/// The listening server: 127.0.0.1 only, one thread per connection (at most kMaxConnections).
class Server {
 public:
  /// Starts on the first free port of [first, first + 20). `forward` runs on the connection's thread.
  [[nodiscard]] static base::Result<std::unique_ptr<Server>> start(std::string token, Forward forward,
                                                                   std::uint16_t first = kDefaultPort);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;

  [[nodiscard]] Running& running() noexcept { return *running_; }
  /// {"running": true, "url", "port", "token"} (control.rs `info`).
  [[nodiscard]] base::Value info() const;
  /// Stops listening; connections end; sessions are cleared.
  void stop();

 private:
  Server() = default;
  void accept_loop(const std::stop_token& stop);
  void serve(std::intptr_t fd);

  std::unique_ptr<Running> running_;
  Forward forward_;
  std::intptr_t listen_fd_ = -1;
  std::atomic<std::size_t> connections_{0};
  std::jthread acceptor_;
  std::mutex threads_mutex_;  // control.threads (after control.sessions and control.token)
  std::vector<std::jthread> threads_;
};
#endif

}  // namespace cc::control
