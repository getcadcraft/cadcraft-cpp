// SPDX-License-Identifier: Apache-2.0
// desktop-shell control.rs, ported. See control.hpp.
#include "cc/control/control.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

#include "cc/base/json.hpp"
#include "cc/base/random.hpp"

#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace cc::control {

using base::Value;

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

Value rpc_err(const Value& id, int code, std::string_view message) {
  Value e = Value::object();
  e.append("code", code);
  e.append("message", std::string(message));
  Value r = Value::object();
  r.append("jsonrpc", "2.0");
  r.append("id", id);
  r.append("error", std::move(e));
  return r;
}

Response reply(int status, std::optional<Value> body, std::vector<std::pair<std::string, std::string>> extra = {}) {
  Response r;
  r.status = status;
  r.body = std::move(body);
  r.headers = std::move(extra);
  r.close = status >= 400;  // a refused request closes its connection (AGENTS.md § 9)
  return r;
}

std::string_view reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    default: return "Error";
  }
}

}  // namespace

std::string base64url(std::span<const std::uint8_t> bytes) {
  static constexpr std::string_view kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::size_t len = std::min<std::size_t>(3, bytes.size() - i);
    const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                            (len > 1 ? static_cast<std::uint32_t>(bytes[i + 1]) << 8U : 0U) |
                            (len > 2 ? static_cast<std::uint32_t>(bytes[i + 2]) : 0U);
    out += kA[(n >> 18U) & 63U];
    out += kA[(n >> 12U) & 63U];
    if (len > 1) {
      out += kA[(n >> 6U) & 63U];
    }
    if (len > 2) {
      out += kA[n & 63U];
    }
  }
  return out;
}

std::string random_token() {
  std::array<std::uint8_t, 32> b{};
  base::os_random(b);
  return base64url(b);
}

std::string session_id() {
  std::array<std::uint8_t, 16> b{};
  base::os_random(b);
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t x : b) {
    s += kHex[x >> 4U];
    s += kHex[x & 15U];
  }
  return s;
}

bool token_matches(std::string_view given, std::string_view token) noexcept {
  if (token.empty() || given.size() != token.size()) {
    unsigned acc = 0;
    for (const char x : token) {
      acc |= static_cast<unsigned>(static_cast<unsigned char>(x) ^ static_cast<unsigned char>(x));
    }
    volatile unsigned sink = acc;  // the same work, never optimised away
    (void)sink;
    return false;
  }
  unsigned acc = 0;
  for (std::size_t i = 0; i < token.size(); ++i) {
    acc |= static_cast<unsigned>(static_cast<unsigned char>(given[i]) ^ static_cast<unsigned char>(token[i]));
  }
  return acc == 0;
}

std::optional<std::string> Request::header(std::string_view name) const {
  for (const auto& [k, v] : headers) {
    if (k.size() == name.size() && std::ranges::equal(k, name, [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        })) {
      return v;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------- HTTP
std::expected<Request, ReadError> read_request(const std::function<std::ptrdiff_t(std::span<char>)>& recv,
                                               std::string& carry) {
  std::array<char, 16384> buf{};
  // the head
  std::size_t head_end = std::string::npos;
  while ((head_end = carry.find("\r\n\r\n")) == std::string::npos) {
    if (carry.size() > kMaxHead) {
      return std::unexpected(ReadError::too_big);
    }
    const std::ptrdiff_t n = recv(buf);
    if (n < 0) {
      return std::unexpected(ReadError::timeout);
    }
    if (n == 0) {
      return std::unexpected(carry.empty() ? ReadError::closed : ReadError::not_http);
    }
    carry.append(buf.data(), static_cast<std::size_t>(n));
  }
  if (head_end > kMaxHead) {
    return std::unexpected(ReadError::too_big);
  }
  const std::string head = carry.substr(0, head_end);
  carry.erase(0, head_end + 4);
  Request req;
  std::size_t line_end = head.find("\r\n");
  const std::string first = head.substr(0, line_end);
  // METHOD SP target SP HTTP/1.x
  const std::size_t s1 = first.find(' ');
  const std::size_t s2 = s1 == std::string::npos ? std::string::npos : first.find(' ', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos || first.find(' ', s2 + 1) != std::string::npos) {
    return std::unexpected(ReadError::not_http);
  }
  req.method = first.substr(0, s1);
  req.target = first.substr(s1 + 1, s2 - s1 - 1);
  const std::string version = first.substr(s2 + 1);
  if ((version != "HTTP/1.1" && version != "HTTP/1.0") || req.method.empty() || req.target.empty() ||
      !std::ranges::all_of(req.method, [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; })) {
    return std::unexpected(ReadError::not_http);
  }
  std::size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
  while (pos < head.size()) {
    const std::size_t e = head.find("\r\n", pos);
    const std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? head.size() : e + 2;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      return std::unexpected(ReadError::not_http);
    }
    if (req.headers.size() >= kMaxHeaders) {
      return std::unexpected(ReadError::too_big);
    }
    std::string value = line.substr(colon + 1);
    const auto first_ch = value.find_first_not_of(" \t");
    value = first_ch == std::string::npos ? std::string() : value.substr(first_ch);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.pop_back();
    }
    req.headers.emplace_back(line.substr(0, colon), std::move(value));
  }
  const std::string conn = lower(req.header("Connection").value_or(""));
  req.close = conn == "close" || (version == "HTTP/1.0" && conn != "keep-alive");
  // the body: Content-Length, or chunked; at most kMaxBody + 1 bytes are kept
  const auto take = [&](std::size_t n, std::string& into) -> bool {
    while (carry.size() < n) {
      const std::ptrdiff_t got = recv(buf);
      if (got <= 0) {
        return false;
      }
      carry.append(buf.data(), static_cast<std::size_t>(got));
    }
    into.append(carry, 0, n);
    carry.erase(0, n);
    return true;
  };
  const std::string te = lower(req.header("Transfer-Encoding").value_or(""));
  if (te == "chunked") {
    for (;;) {
      std::size_t eol = std::string::npos;
      while ((eol = carry.find("\r\n")) == std::string::npos) {
        if (carry.size() > 64) {
          return std::unexpected(ReadError::not_http);
        }
        const std::ptrdiff_t got = recv(buf);
        if (got <= 0) {
          return std::unexpected(ReadError::not_http);
        }
        carry.append(buf.data(), static_cast<std::size_t>(got));
      }
      std::size_t size = 0;
      const std::string sline = carry.substr(0, eol);
      std::size_t k = 0;
      for (; k < sline.size() && std::isxdigit(static_cast<unsigned char>(sline[k])) != 0; ++k) {
        if (size > (kMaxBody + 1) * 16) {
          return std::unexpected(ReadError::too_big);
        }
        size = size * 16 + static_cast<std::size_t>(std::stoi(std::string(1, sline[k]), nullptr, 16));
      }
      if (k == 0) {
        return std::unexpected(ReadError::not_http);
      }
      carry.erase(0, eol + 2);
      if (size == 0) {
        std::string crlf;
        if (!take(2, crlf)) {
          return std::unexpected(ReadError::not_http);
        }
        break;
      }
      if (req.body.size() + size > kMaxBody + 1) {
        req.too_large = true;
        req.close = true;
        return req;
      }
      if (!take(size, req.body)) {
        return std::unexpected(ReadError::not_http);
      }
      std::string crlf;
      if (!take(2, crlf) || crlf != "\r\n") {
        return std::unexpected(ReadError::not_http);
      }
    }
  } else if (const auto cl = req.header("Content-Length")) {
    if (cl->empty() || !std::ranges::all_of(*cl, [](char c) { return c >= '0' && c <= '9'; }) || cl->size() > 12) {
      return std::unexpected(ReadError::not_http);
    }
    const std::size_t n = std::stoull(*cl);
    if (n > kMaxBody) {
      if (!take(kMaxBody + 1, req.body)) {
        return std::unexpected(ReadError::not_http);
      }
      req.too_large = true;
      req.close = true;  // the rest of the body is never read
      return req;
    }
    if (!take(n, req.body)) {
      return std::unexpected(ReadError::not_http);
    }
  }
  return req;
}

std::string write_response(const Response& r) {
  const std::string text = r.body ? base::to_json(*r.body) : std::string();
  std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + std::string(reason(r.status)) + "\r\n";
  out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
  if (r.body) {
    out += "Content-Type: application/json\r\n";
  }
  for (const auto& [k, v] : r.headers) {
    out.append(k).append(": ").append(v).append("\r\n");
  }
  if (r.close) {
    out += "Connection: close\r\n";
  }
  out += "Content-Length: " + std::to_string(text.size()) + "\r\n\r\n";
  out += text;
  return out;
}

// ---------------------------------------------------------------------------------------------------- Running
Running::Running(std::uint16_t port, std::string token) : port_(port), token_(std::move(token)) {}

std::string Running::token() const {
  const std::scoped_lock lock(token_mutex_);
  return token_;
}

void Running::set_token(std::string token) {
  {
    const std::scoped_lock lock(sessions_mutex_);
    sessions_.clear();
  }
  const std::scoped_lock lock(token_mutex_);
  token_ = std::move(token);
}

bool Running::drop_session(const std::string& id) {
  const std::scoped_lock lock(sessions_mutex_);
  return sessions_.erase(id) != 0;
}

void Running::clear_sessions() {
  const std::scoped_lock lock(sessions_mutex_);
  sessions_.clear();
}

std::size_t Running::session_count() const {
  const std::scoped_lock lock(sessions_mutex_);
  return sessions_.size();
}

Response Running::handle(const Request& req, const Forward& forward) {
  const Value null(nullptr);
  if (req.too_large) {
    Response r = reply(413, rpc_err(null, -32000, "Too large"), {{"Connection", "close"}});
    r.close = true;
    return r;
  }
  const std::string host = lower(req.header("Host").value_or(""));
  if (host != "127.0.0.1:" + std::to_string(port_) && host != "localhost:" + std::to_string(port_)) {
    return reply(403, rpc_err(null, -32000, "Forbidden host"));
  }
  if (req.header("Origin")) {
    return reply(403, rpc_err(null, -32000, "Requests from web pages are refused"));
  }
  const std::size_t q = req.target.find('?');
  const std::string path = req.target.substr(0, q);
  const bool has_query = q != std::string::npos;
  if (path != "/mcp" && path != "/control") {
    return reply(404, rpc_err(null, -32000, "Not found"));
  }
  const std::string auth = req.header("Authorization").value_or("");
  const bool ok = auth.starts_with("Bearer ") && token_matches(std::string_view(auth).substr(7), token());
  if (!ok) {
    return reply(401, rpc_err(null, -32000, "A valid token is needed (Authorization: Bearer \xe2\x80\xa6)"),
                 {{"WWW-Authenticate", "Bearer realm=\"CadCraft Studio\""}});
  }
  if (has_query) {
    return reply(400, rpc_err(null, -32000, "No query strings"));
  }
  const bool mcp = path == "/mcp";
  const auto sid = req.header("Mcp-Session-Id");
  std::string method = req.method;
  for (char& c : method) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (method == "GET") {
    return reply(405, rpc_err(null, -32000, "No event stream here: POST JSON-RPC messages"),
                 {{"Allow", "POST, DELETE"}});
  }
  if (method == "DELETE" && mcp) {
    const bool gone = sid && drop_session(*sid);
    if (gone) {
      return reply(204, std::nullopt);
    }
    return reply(404, rpc_err(null, -32000, "No such session"));
  }
  if (method != "POST") {
    return reply(405, rpc_err(null, -32000, "POST only"), {{"Allow", mcp ? "POST, DELETE" : "POST"}});
  }
  const std::string ctype = lower(req.header("Content-Type").value_or(""));
  if (!(ctype == "application/json" || ctype.starts_with("application/json;") ||
        ctype.starts_with("application/json "))) {
    return reply(415, rpc_err(null, -32000, "Content-Type must be application/json"));
  }
  auto parsed = base::parse_json(req.body, base::JsonLimits{kMaxBody, 128, kMaxBody});
  if (!parsed) {
    return reply(400, rpc_err(null, -32700, "Parse error"));
  }
  const Value& msg = *parsed;
  if (msg.is_array()) {
    return reply(400, rpc_err(null, -32600, "Batches are not supported"));
  }
  if (!msg.is_object()) {
    return reply(400, rpc_err(null, -32600, "Invalid request"));
  }
  const Value id = msg.has("id") ? msg.get("id") : Value(nullptr);
  const bool is_init = msg.get("method").is_string() && msg.get("method").as_string() == "initialize";
  std::string session;
  if (mcp) {
    if (is_init) {
      const std::scoped_lock lock(sessions_mutex_);
      if (sessions_.size() >= kMaxSessions) {
        const auto oldest = std::ranges::min_element(sessions_, {}, [](const auto& kv) { return kv.second.last; });
        if (oldest != sessions_.end()) {
          sessions_.erase(oldest);
        }
      }
      session = session_id();
      sessions_[session] = Session{std::chrono::steady_clock::now()};
    } else {
      if (!sid) {
        return reply(400, rpc_err(id, -32000, "Mcp-Session-Id is required (initialize first)"));
      }
      const std::scoped_lock lock(sessions_mutex_);
      if (!sessions_.contains(*sid)) {
        return reply(404, rpc_err(id, -32000, "This session has ended; initialize again"));
      }
      session = *sid;
    }
  } else {
    std::string cid;
    const std::string given = req.header("X-CadCraft-Session").value_or("control");
    // .chars().take(64) — characters, then only [A-Za-z0-9_-] of them
    std::size_t chars = 0;
    for (std::size_t i = 0; i < given.size() && chars < 64; ++chars) {
      const auto c = static_cast<unsigned char>(given[i]);
      std::size_t n = 4;
      if (c < 0x80) {
        n = 1;
      } else if (c < 0xe0) {
        n = 2;
      } else if (c < 0xf0) {
        n = 3;
      }
      if (n == 1 && (std::isalnum(c) != 0 || c == '_' || c == '-')) {
        cid += static_cast<char>(c);
      }
      i += n;
    }
    session = "control:" + (cid.empty() ? std::string("control") : cid);
    const std::scoped_lock lock(sessions_mutex_);
    sessions_.try_emplace(session, Session{std::chrono::steady_clock::now()});
  }
  {
    const std::scoped_lock lock(sessions_mutex_);
    if (auto it = sessions_.find(session); it != sessions_.end()) {
      it->second.last = std::chrono::steady_clock::now();
    }
  }
  std::optional<Value> answer;
  if (auto got = forward(msg, session)) {
    answer = std::move(*got);
  } else {
    answer = rpc_err(id, -32603, "Internal error");
  }
  std::optional<std::int64_t> err_code;
  if (answer && answer->is_object()) {
    const Value code = answer->get("error").get("code");
    if (code.is_number() && std::floor(code.as_number()) == code.as_number()) {
      err_code = static_cast<std::int64_t>(code.as_number());
    }
  }
  if ((mcp && is_init && err_code) || err_code == -32002) {
    (void)drop_session(session);
  }
  std::vector<std::pair<std::string, std::string>> hdr;
  if (mcp) {
    hdr.emplace_back("Mcp-Session-Id", session);
  }
  if (!msg.has("id") || !msg.has("method")) {
    Response r = reply(202, std::nullopt, hdr);
    return r;
  }
  Response r = reply(200, answer ? std::move(*answer) : rpc_err(id, -32603, "No answer"), hdr);
  return r;
}

// ---------------------------------------------------------------------------------------------------- Server
#if !defined(__EMSCRIPTEN__)
namespace {
#if defined(_WIN32)
void close_socket(std::intptr_t s) {
  closesocket(static_cast<SOCKET>(s));
}
int poll_one(std::intptr_t fd, int ms) {
  WSAPOLLFD p{static_cast<SOCKET>(fd), POLLRDNORM, 0};
  return WSAPoll(&p, 1, ms);
}
std::intptr_t open_socket() {
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  return s == INVALID_SOCKET ? -1 : static_cast<std::intptr_t>(s);
}
std::ptrdiff_t recv_some(std::intptr_t fd, std::span<char> b) {
  const int n =
      ::recv(static_cast<SOCKET>(fd), b.data(), static_cast<int>(std::min<std::size_t>(b.size(), 1U << 20U)), 0);
  return n < 0 ? -1 : n;
}
std::ptrdiff_t send_some(std::intptr_t fd, std::string_view s) {
  const int n =
      ::send(static_cast<SOCKET>(fd), s.data(), static_cast<int>(std::min<std::size_t>(s.size(), 1U << 20U)), 0);
  return n < 0 ? -1 : n;
}
void no_delay(std::intptr_t fd) {
  const BOOL one = TRUE;
  (void)::setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof one);  // NOLINT
}
#else
void close_socket(std::intptr_t s) {
  ::close(static_cast<int>(s));
}
int poll_one(std::intptr_t fd, int ms) {
  pollfd p{static_cast<int>(fd), POLLIN, 0};
  return ::poll(&p, 1, ms);
}
std::intptr_t open_socket() {
  return ::socket(AF_INET, SOCK_STREAM, 0);
}
std::ptrdiff_t recv_some(std::intptr_t fd, std::span<char> b) {
  return ::recv(static_cast<int>(fd), b.data(), b.size(), 0);
}
std::ptrdiff_t send_some(std::intptr_t fd, std::string_view s) {
#if defined(MSG_NOSIGNAL)
  return ::send(static_cast<int>(fd), s.data(), s.size(), MSG_NOSIGNAL);
#else
  return ::send(static_cast<int>(fd), s.data(), s.size(), 0);
#endif
}
void no_delay(std::intptr_t fd) {
  const int one = 1;
  (void)::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
  (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
#endif
constexpr int kIdleMs = 30'000;
}  // namespace

base::Result<std::unique_ptr<Server>> Server::start(std::string token, Forward forward, std::uint16_t first) {
#if defined(_WIN32)
  static const bool wsa = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  if (!wsa)
    return base::fail(base::Errc::io, "the network is not available");
#endif
  std::unique_ptr<Server> s(new Server());  // NOLINT(cppcoreguidelines-owning-memory) — the constructor is private
  for (std::uint32_t p = first; p < static_cast<std::uint32_t>(first) + 20 && p <= 65535; ++p) {
    const std::intptr_t fd = open_socket();
    if (fd < 0) {
      continue;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(p));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
#if defined(_WIN32)
    const auto sock = static_cast<SOCKET>(fd);
#else
    const auto sock = static_cast<int>(fd);
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) the sockets API
    const auto* sa = reinterpret_cast<const sockaddr*>(&addr);
    if (::bind(sock, sa, sizeof addr) == 0 && ::listen(sock, 16) == 0) {
      s->listen_fd_ = fd;
      s->running_ = std::make_unique<Running>(static_cast<std::uint16_t>(p), std::move(token));
      break;
    }
    close_socket(fd);
  }
  if (s->listen_fd_ < 0) {
    return base::fail(base::Errc::io, "no free port near " + std::to_string(first));
  }
  s->forward_ = std::move(forward);
  Server* self = s.get();
  s->acceptor_ = std::jthread([self](const std::stop_token& st) { self->accept_loop(st); });
  return s;
}

Server::~Server() {
  stop();
}

base::Value Server::info() const {
  Value v = Value::object();
  v.append("running", true);
  v.append("url", "http://127.0.0.1:" + std::to_string(running_->port()) + "/mcp");
  v.append("port", running_->port());
  v.append("token", running_->token());
  return v;
}

void Server::stop() {
  if (listen_fd_ >= 0) {
    acceptor_.request_stop();
#if !defined(_WIN32)
    ::shutdown(static_cast<int>(listen_fd_), SHUT_RDWR);
#endif
    close_socket(listen_fd_);
    listen_fd_ = -1;
  }
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  std::vector<std::jthread> threads;
  {
    const std::scoped_lock lock(threads_mutex_);
    threads.swap(threads_);
  }
  for (auto& t : threads) {
    t.request_stop();
  }
  threads.clear();  // joins
  if (running_) {
    running_->clear_sessions();
  }
}

void Server::accept_loop(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    if (poll_one(listen_fd_, 200) <= 0) {
      continue;
    }
#if defined(_WIN32)
    const SOCKET a = ::accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
    const std::intptr_t fd = a == INVALID_SOCKET ? -1 : static_cast<std::intptr_t>(a);
#else
    const std::intptr_t fd = ::accept(static_cast<int>(listen_fd_), nullptr, nullptr);
#endif
    if (fd < 0) {
      continue;
    }
    if (connections_.load() >= kMaxConnections) {
      close_socket(fd);
      continue;
    }
    ++connections_;
    const std::scoped_lock lock(threads_mutex_);
    std::erase_if(threads_, [](std::jthread& t) { return !t.joinable(); });
    threads_.emplace_back([this, fd](const std::stop_token&) { serve(fd); });
  }
}

void Server::serve(std::intptr_t fd) {
  no_delay(fd);
  const auto recv = [fd](std::span<char> b) -> std::ptrdiff_t {
    if (poll_one(fd, kIdleMs) <= 0) {
      return -1;  // the idle timeout
    }
    const std::ptrdiff_t n = recv_some(fd, b);
    return n < 0 ? -1 : n;
  };
  const auto send_all = [fd](const std::string& s) {
    std::size_t at = 0;
    while (at < s.size()) {
      const std::ptrdiff_t n = send_some(fd, std::string_view(s).substr(at));
      if (n <= 0) {
        return false;
      }
      at += static_cast<std::size_t>(n);
    }
    return true;
  };
  std::string carry;
  for (;;) {
    auto req = read_request(recv, carry);
    if (!req) {
      break;  // closed, not HTTP, too big or idle: closed without an answer
    }
    const Response r = running_->handle(*req, forward_);
    if (!send_all(write_response(r)) || r.close || req->close) {
      break;
    }
  }
  close_socket(fd);
  --connections_;
}
#endif

}  // namespace cc::control
