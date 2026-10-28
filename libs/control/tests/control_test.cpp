// SPDX-License-Identifier: Apache-2.0
// libs/control by desktop-shell control.rs's rules: the checks of CadCraft 0.5's tools/mcp/test/check-server.mjs
// (which also passes against this server: 16/16) request by request through Running::handle, the HTTP reader on
// hostile heads and bodies, and one real round trip over a socket.
#include "cc/control/control.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/json.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using cc::base::Value;
using cc::control::Request;
using cc::control::Running;

namespace {

constexpr std::uint16_t kPort = 47615;

/// The stand-in hub of control.rs's own test: an MCP answer to anything with an id.
std::optional<std::optional<Value>> hub(const Value& m, const std::string& /*unused*/) {
  if (!m.has("id")) {
    return std::optional<Value>{};
  }
  auto v = cc::base::parse_json(R"({"jsonrpc":"2.0","id":0,"result":{"tools":[],"protocolVersion":"2025-06-18"}})");
  Value r = *v;
  r.set("id", m.get("id"));
  return std::optional<Value>{r};
}

Request post(std::string token, std::string body, std::string target = "/mcp") {
  Request r;
  r.method = "POST";
  r.target = std::move(target);
  r.headers = {{"Host", "127.0.0.1:" + std::to_string(kPort)},
               {"Content-Type", "application/json"},
               {"Authorization", "Bearer " + std::move(token)}};
  r.body = std::move(body);
  return r;
}

void set(Request& r, std::string_view name, std::string value) {
  for (auto& [k, v] : r.headers) {
    if (k == name) {
      v = std::move(value);
      return;
    }
  }
  r.headers.emplace_back(name, std::move(value));
}

const char* const kInit =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})";

}  // namespace

TEST_CASE("control: the rules check-server.mjs checks, request by request") {
  const std::string tok = cc::control::random_token();
  Running run(kPort, tok);
  CHECK(run.handle(post("nope", kInit), hub).status == 401);
  {
    Request r = post(tok, kInit);
    set(r, "Authorization", "");
    const auto a = run.handle(r, hub);
    CHECK(a.status == 401);
    bool www = false;
    for (const auto& [k, v] : a.headers) {
      www = www || (k == "WWW-Authenticate" && v.contains("Bearer"));
    }
    CHECK(www);
    CHECK(a.close);
  }
  {
    Request r = post(tok, kInit, "/mcp?token=" + tok);
    set(r, "Authorization", "");
    CHECK(run.handle(r, hub).status == 401);                             // a token in the URL does not count
    CHECK(run.handle(post(tok, kInit, "/mcp?x=1"), hub).status == 400);  // query strings
  }
  {
    Request r = post(tok, kInit);
    set(r, "Origin", "https://evil.example");
    CHECK(run.handle(r, hub).status == 403);
    Request h = post(tok, kInit);
    set(h, "Host", "evil.example:" + std::to_string(kPort));
    CHECK(run.handle(h, hub).status == 403);
    Request l = post(tok, kInit);
    set(l, "Host", "LOCALHOST:" + std::to_string(kPort));
    CHECK(run.handle(l, hub).status == 200);
  }
  {
    Request g = post(tok, "");
    g.method = "GET";
    CHECK(run.handle(g, hub).status == 405);
    Request p = post(tok, "");
    p.method = "PUT";
    CHECK(run.handle(p, hub).status == 405);
    Request t = post(tok, kInit);
    set(t, "Content-Type", "text/plain");
    CHECK(run.handle(t, hub).status == 415);
    CHECK(run.handle(post(tok, kInit, "/other"), hub).status == 404);
    CHECK(run.handle(post(tok, "{nope"), hub).status == 400);
    CHECK(run.handle(post(tok, "[1]"), hub).status == 400);
    CHECK(run.handle(post(tok, "3"), hub).status == 400);
  }
  CHECK(run.handle(post(tok, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"), hub).status == 400);
  {
    Request r = post(tok, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    set(r, "Mcp-Session-Id", "deadbeef");
    CHECK(run.handle(r, hub).status == 404);
  }
  const auto init = run.handle(post(tok, kInit), hub);
  REQUIRE(init.status == 200);
  std::string sid;
  for (const auto& [k, v] : init.headers) {
    if (k == "Mcp-Session-Id") {
      sid = v;
    }
  }
  CHECK(sid.size() == 32);
  REQUIRE(init.body.has_value());
  CHECK(init.body.value_or(Value()).get("result").get("protocolVersion").is_string());
  {
    Request n = post(tok, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    set(n, "Mcp-Session-Id", sid);
    CHECK(run.handle(n, hub).status == 202);
    Request p = post(tok, R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
    set(p, "Mcp-Session-Id", sid);
    CHECK(run.handle(p, hub).status == 200);
    Request d = post(tok, "");
    d.method = "DELETE";
    set(d, "Mcp-Session-Id", sid);
    CHECK(run.handle(d, hub).status == 204);
    CHECK(run.handle(p, hub).status == 404);
    CHECK(run.handle(d, hub).status == 404);
  }
  const auto ctl = run.handle(post(tok, R"({"jsonrpc":"2.0","id":9,"method":"tools.list"})", "/control"), hub);
  CHECK(ctl.status == 200);
  REQUIRE(ctl.body.has_value());
  CHECK(ctl.body.value_or(Value()).get("result").get("tools").is_array());
  {
    Request big = post(tok, "");
    big.too_large = true;
    CHECK(run.handle(big, hub).status == 413);
  }
  // the hub cannot answer → Internal error; -32002 (Disconnect) ends the session
  {
    const auto broken = [](const Value&, const std::string&) -> std::optional<std::optional<Value>> {
      return std::nullopt;
    };
    const auto a = run.handle(post(tok, R"({"jsonrpc":"2.0","id":5,"method":"x"})", "/control"), broken);
    CHECK(a.body.value_or(Value()).get("error").get("code").num_or(0) == -32603);
    const auto bye = [](const Value& m, const std::string&) -> std::optional<std::optional<Value>> {
      Value e = Value::object();
      e.append("code", -32002);
      e.append("message", "Disconnected");
      Value r = Value::object();
      r.append("jsonrpc", "2.0");
      r.append("id", m.get("id"));
      r.append("error", e);
      return std::optional<Value>{r};
    };
    const auto i2 = run.handle(post(tok, kInit), hub);
    std::string s2;
    for (const auto& [k, v] : i2.headers) {
      if (k == "Mcp-Session-Id") {
        s2 = v;
      }
    }
    Request p = post(tok, R"({"jsonrpc":"2.0","id":6,"method":"ping"})");
    set(p, "Mcp-Session-Id", s2);
    CHECK(run.handle(p, bye).status == 200);
    CHECK(run.handle(p, hub).status == 404);
  }
  // at most 32 sessions: the oldest goes
  for (int i = 0; i < 40; ++i) {
    (void)run.handle(post(tok, kInit), hub);
  }
  CHECK(run.session_count() <= 33);  // 32 /mcp sessions and the /control one
  // a new token ends every session
  run.set_token(cc::control::random_token());
  CHECK(run.session_count() == 0);
  CHECK(run.handle(post(tok, kInit), hub).status == 401);
}

TEST_CASE("control: tokens and the HTTP reader") {
  const std::string t = cc::control::random_token();
  CHECK(t.size() == 43);
  CHECK(t != cc::control::random_token());
  CHECK(cc::control::token_matches(t, t));
  CHECK_FALSE(cc::control::token_matches(std::string_view(t).substr(0, 42), t));
  CHECK_FALSE(cc::control::token_matches("", t));
  CHECK_FALSE(cc::control::token_matches(t, ""));
  const std::array<std::uint8_t, 2> fb{0xfb, 0xff};
  CHECK(cc::control::base64url(fb) == "-_8");
  CHECK(cc::control::session_id().size() == 32);

  const auto read = [](std::string wire) {
    std::size_t at = 0;
    std::string carry;
    const auto recv = [&wire, &at](std::span<char> b) -> std::ptrdiff_t {
      const std::size_t n = std::min(b.size(), wire.size() - at);
      std::ranges::copy(std::string_view(wire).substr(at, n), b.begin());
      at += n;
      return static_cast<std::ptrdiff_t>(n);
    };
    return cc::control::read_request(recv, carry);
  };
  const auto ok = read("POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}");
  REQUIRE(ok.has_value());
  CHECK(ok->body == "{}");
  const auto chunked = read("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n");
  REQUIRE(chunked.has_value());
  CHECK(chunked->body == "{}");
  CHECK_FALSE(read(R"({"method":"auth"})"
                   "\n")
                  .has_value());  // a bare JSON line is not HTTP
  CHECK_FALSE(read("GET / HTTP/2\r\n\r\n").has_value());
  CHECK_FALSE(read("POST /mcp HTTP/1.1\r\nContent-Length: -1\r\n\r\n").has_value());
  CHECK_FALSE(read("POST /mcp HTTP/1.1\r\n" + std::string(70'000, 'a') + "\r\n\r\n").has_value());
  const auto big =
      read("POST /mcp HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n" + std::string(cc::control::kMaxBody + 1, 'x'));
  REQUIRE(big.has_value());
  CHECK(big->too_large);
  const auto close = read("POST /mcp HTTP/1.0\r\n\r\n");
  REQUIRE(close.has_value());
  CHECK(close->close);
}

#if !defined(_WIN32)
TEST_CASE("control: a real round trip on 127.0.0.1") {
  auto s = cc::control::Server::start(cc::control::random_token(), hub, 47740);
  REQUIRE(s.has_value());
  const Value info = (*s)->info();
  const auto port = static_cast<std::uint16_t>(info.get("port").as_number());
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);  // NOLINT
  const std::string body = R"({"jsonrpc":"2.0","id":9,"method":"tools.list"})";
  const std::string req = "POST /control HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nAuthorization: Bearer " + info.get("token").as_string() +
                          "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                          "\r\nConnection: close\r\n\r\n" + body;
  REQUIRE(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));
  std::string got;
  std::array<char, 4096> buf{};
  for (ssize_t n = 0; (n = ::recv(fd, buf.data(), buf.size(), 0)) > 0;) {
    got.append(buf.data(), static_cast<std::size_t>(n));
  }
  ::close(fd);
  CHECK(got.starts_with("HTTP/1.1 200 OK\r\n"));
  CHECK(got.contains(R"("tools":[])"));
  (*s)->stop();
}
#endif
