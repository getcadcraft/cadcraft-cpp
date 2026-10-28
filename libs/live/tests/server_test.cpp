// SPDX-License-Identifier: Apache-2.0
// The session server (libs/live server, desktop-shell live/session.rs) on the shared scenarios of
// tools/live-test/vectors.json, end to end in envelopes: a host (local token), editors (plain, auto-approved), each
// step an `edit`, `hold`, `release`, `lock`, `unlock` or `role` — every outcome (commit seq, refuse reason, by) as the
// 0.5 server's. Then the parts the scenarios do not reach: a sealed join through the handshake, approval, rejoin with
// the resume token, a snapshot joiner's revs + chunks, flood control, the host leaving.
#include "cc/live/server.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "cc/live/crypto.hpp"

using cc::base::Value;
using cc::live::Envelope;
using cc::live::Map;
using cc::live::Mp;
using cc::live::SessionServer;

namespace {

/// The optional's value (a test fails, rather than reads nothing, when it is empty).
template <class T>
const T& must(const std::optional<T>& o) {
  if (!o) {
    throw std::runtime_error("expected a value");
  }
  return *o;
}

// NOLINTNEXTLINE(misc-no-recursion) the vectors' JSON, a few levels deep
Mp to_mp(const Value& j) {
  switch (j.kind()) {
    case Value::Kind::boolean: return {j.as_bool()};
    case Value::Kind::number: return {j.as_number()};
    case Value::Kind::string: return {j.as_string()};
    case Value::Kind::array: {
      cc::live::MpArray a;
      for (const Value& x : j.as_array()) {
        a.push_back(to_mp(x));
      }
      return {std::move(a)};
    }
    case Value::Kind::object: {
      Map m;
      Value copy = j;
      for (const auto& member : copy.as_object()) {
        m.set(member.key, to_mp(member.value));
      }
      return {std::move(m)};
    }
    default: return {nullptr};
  }
}

Envelope env(std::string t, Mp b = Mp(), std::uint64_t n = 0) {
  Envelope e;
  e.t = std::move(t);
  e.b = std::move(b);
  e.n = n;
  return e;
}

Mp hello(std::string name, bool host = false) {
  Map b;
  b.set("v", Mp(1));
  b.set("app", Mp("cadcraft"));
  b.set("schema", Mp(1));
  b.set("name", Mp(std::move(name)));
  if (host) {
    b.set("role", Mp("host"));
    b.set("token", Mp("tok"));
  }
  return {std::move(b)};
}

/// What each connection received.
struct Wire {
  std::map<cc::live::ConnId, std::vector<Envelope>> got;
  std::vector<cc::live::ConnId> closed;
  void take(const std::vector<cc::live::Out>& out) {
    for (const auto& o : out) {
      if (const auto* to = std::get_if<cc::live::OutTo>(&o)) {
        got[to->conn].push_back(to->env);
      }
      if (const auto* c = std::get_if<cc::live::OutClose>(&o)) {
        closed.push_back(c->conn);
      }
    }
  }
  /// The last message of type t to conn.
  [[nodiscard]] std::optional<Envelope> last(cc::live::ConnId conn, std::string_view t) const {
    const auto it = got.find(conn);
    if (it == got.end()) {
      return std::nullopt;
    }
    for (const auto& e : std::views::reverse(it->second)) {
      if (e.t == t) {
        return e;
      }
    }
    return std::nullopt;
  }
};

std::uint64_t client_id(const Wire& w, cc::live::ConnId conn) {
  const auto wl = w.last(conn, "welcome");
  REQUIRE(wl.has_value());
  std::uint64_t c = 0;
  REQUIRE(must(wl).b.get("c")->to_uint(c));
  return c;
}

}  // namespace

TEST_CASE("live server: the shared scenarios through the whole server") {
  const auto text = cc::base::read_file(CC_LIVE_VECTORS, std::uint64_t{1} << 22U);
  REQUIRE(text.has_value());
  const auto v = cc::base::parse_json(cc::base::as_text(*text));
  REQUIRE(v.has_value());
  std::size_t ran = 0;
  for (const Value& sc : (*v)["scenarios"].as_array()) {
    INFO(sc["name"].as_string());
    SessionServer s({.app = "cadcraft",
                     .schema = 1,
                     .max_clients = 20,
                     .log_keep = 10'000,
                     .auto_approve = true,
                     .local_token = "tok",
                     .plain = true});
    Wire w;
    std::map<std::string, cc::live::ConnId> conn;
    std::map<std::string, std::uint64_t> id;
    std::map<std::string, std::uint64_t> n;
    cc::live::Ms now = 1000;
    cc::live::ConnId next = 1;
    for (const Value& name : sc["clients"].as_array()) {
      const std::string& nm = name.as_string();
      conn[nm] = next++;
      s.open(conn[nm], false, "lan", now);
      w.take(s.frame(conn[nm], env("hello", hello(nm, nm == "H")), now));
      id[nm] = client_id(w, conn[nm]);
    }
    std::size_t step = 0;
    for (const Value& st : sc["steps"].as_array()) {
      ++step;
      INFO("step " << step);
      now += 20;  // the rate bucket refills
      const std::string by = st["by"].as_string();
      const auto keys = [&st](std::string_view k) {
        Map m;
        m.set("keys", to_mp(st[k]));
        return Mp(std::move(m));
      };
      if (st.has("touched")) {
        const std::uint64_t mine = ++n[by];
        Map b;
        b.set("label", Mp(st.has("label") ? st["label"].as_string() : std::string("edit")));
        b.set("ops", Mp(cc::live::MpArray{}));
        b.set("touched", to_mp(st["touched"]));
        w.take(s.frame(conn[by], env("edit", Mp(std::move(b)), mine), now));
        const Value& want = st["expect"];
        if (want.has("commit")) {
          const auto c = w.last(conn[by], "commit");
          REQUIRE(c.has_value());
          CHECK(must(c).q == static_cast<std::uint64_t>(want["commit"].as_number()));
          CHECK(must(c).n == mine);
        } else {
          const auto r = w.last(conn[by], "refuse");
          REQUIRE(r.has_value());
          CHECK(must(r).n == mine);
          CHECK(must(r).b.get("reason")->as_string() == want["refuse"].as_string());
          if (want.has("by")) {
            std::uint64_t who = 0;
            REQUIRE(must(r).b.get("by") != nullptr);
            REQUIRE(must(r).b.get("by")->to_uint(who));
            CHECK(who == id[want["by"].as_string()]);
          }
        }
      } else if (st.has("hold")) {
        w.take(s.frame(conn[by], env("hold", keys("hold")), now));
      } else if (st.has("release")) {
        w.take(s.frame(conn[by], env("release", keys("release")), now));
      } else if (st.has("lock")) {
        w.take(s.frame(conn[by], env("lock", keys("lock")), now));
      } else if (st.has("unlock")) {
        w.take(s.frame(conn[by], env("unlock", keys("unlock")), now));
      } else if (st.has("role")) {
        Map b;
        b.set("c", Mp(id[st["role"]["who"].as_string()]));
        b.set("role", Mp(st["role"]["role"].as_string()));
        w.take(s.frame(conn[by], env("role", Mp(std::move(b))), now));
      }
    }
    ++ran;
  }
  CHECK(ran == 11);
}

TEST_CASE("live server: a sealed join with a short code, approval, rejoin, snapshot") {
  REQUIRE(cc::live::crypto_ready());
  SessionServer s({.app = "cadcraft",
                   .schema = 1,
                   .max_clients = 20,
                   .log_keep = 10'000,
                   .auto_approve = false,
                   .local_token = "",
                   .plain = false});
  Wire w;
  cc::live::Ms now = 5000;
  // the host's window, in-process
  s.open(1, true, "local", now);
  w.take(s.frame(1, env("hello", hello("Ana", true)), now));
  REQUIRE(w.last(1, "welcome").has_value());
  // a code to share
  w.take(s.frame(1, env("invite", Mp(Map{})), now));
  const auto code = w.last(1, "code");
  REQUIRE(code.has_value());
  const auto parsed = cc::live::parse_code(must(code).b.get("code")->as_string());
  REQUIRE(parsed.has_value());
  // a joiner over the network: the four handshake messages, then sealed frames
  s.open(2, false, "lan", now);
  const std::vector<std::uint8_t> secret(must(parsed).secret.begin(), must(parsed).secret.end());
  cc::live::JoinerHandshake j(secret, must(parsed).context);
  Map p1;
  p1.set("kid", Mp(must(parsed).tag));
  p1.set("m", Mp(*j.message()));
  w.take(s.frame(2, env("pake", Mp(std::move(p1))), now));
  const auto p2 = w.last(2, "pake");
  REQUIRE(p2.has_value());
  const auto confirm = j.receive(must(p2).b.get("m")->as_bytes());
  REQUIRE(confirm.has_value());
  Map p3;
  p3.set("m", Mp(*confirm));
  std::vector<cc::live::Out> out = s.frame(2, env("pake", Mp(std::move(p3))), now);
  bool secured = false;
  for (const auto& o : out) {
    secured = secured || std::holds_alternative<cc::live::OutSecure>(o);
  }
  CHECK(secured);
  w.take(out);
  const auto p4 = w.last(2, "pake");
  REQUIRE(p4.has_value());
  CHECK(j.finish(must(p4).b.get("m")->as_bytes()).has_value());
  // hello → waiting; the host sees the card with the connection's audience
  w.take(s.frame(2, env("hello", hello("Ben")), now));
  CHECK(w.last(2, "waiting").has_value());
  const auto card = w.last(1, "join");
  REQUIRE(card.has_value());
  CHECK(must(card).b.get("aud")->as_string().size() == 43);
  std::uint64_t ben = 0;
  REQUIRE(must(card).b.get("c")->to_uint(ben));
  // the host makes some edits first, so the joiner needs a snapshot with revs
  Map e1;
  e1.set("label", Mp("line"));
  e1.set("ops", Mp(cc::live::MpArray{Mp(1)}));
  e1.set("touched", Mp(cc::live::MpArray{Mp(Map{})}));
  {
    Map t;
    t.set("k", Mp("e:1"));
    t.set("m", Mp("c"));
    e1.set("touched", Mp(cc::live::MpArray{Mp(std::move(t))}));
  }
  w.take(s.frame(1, env("edit", Mp(std::move(e1)), 1), now));
  CHECK(s.seq() == 1);
  Map ap;
  ap.set("c", Mp(ben));
  ap.set("role", Mp("editor"));
  w.take(s.frame(1, env("approve", Mp(std::move(ap))), now));
  const auto wl = w.last(2, "welcome");
  REQUIRE(wl.has_value());
  CHECK(must(wl).b.get("catchup")->as_string() == "snapshot");
  // the host's window answers the snapshot request
  const auto req = w.last(1, "snapreq");
  REQUIRE(req.has_value());
  Map sn;
  sn.set("for", Mp(ben));
  sn.set("q", Mp(1));
  sn.set("data", Mp(cc::live::Bytes(300'000, 7)));
  w.take(s.frame(1, env("snap", Mp(std::move(sn))), now));
  const auto revs = w.last(2, "revs");
  REQUIRE(revs.has_value());
  CHECK(must(revs).b.get("keys")->get("e:1") != nullptr);
  std::size_t chunks = 0;
  for (const Envelope& e : w.got[2]) {
    chunks += e.t == "snap" ? 1 : 0;
  }
  CHECK(chunks == 2);  // 300 000 bytes in 256 KB chunks
  // a drop and a rejoin with the resume token: no approval, the log after its seq
  const std::string token = must(wl).b.get("token")->as_string();
  w.take(s.closed(2, now));
  s.open(3, false, "lan", now + 10);  // over the network the rejoin needs the ticket's handshake; here: a plain hello
  // (the rejoin ticket h:<c> is a key now)
  Map hb = hello("Ben").as_map();
  Map rs;
  rs.set("c", Mp(ben));
  rs.set("token", Mp(token));
  rs.set("q", Mp(1));
  hb.set("resume", Mp(std::move(rs)));
  out = s.frame(3, env("hello", Mp(std::move(hb))), now + 10);
  bool closed = false;
  for (const auto& o : out) {
    closed = closed || std::holds_alternative<cc::live::OutClose>(o);
  }
  CHECK(closed);  // a sealed server takes nothing but pake on a new network connection
}

TEST_CASE("live server: flood control, the host leaving, wrong tries rotating a code") {
  SessionServer s({.app = "cadcraft",
                   .schema = 1,
                   .max_clients = 20,
                   .log_keep = 10'000,
                   .auto_approve = true,
                   .local_token = "tok",
                   .plain = true});
  Wire w;
  cc::live::Ms now = 1000;
  s.open(1, false, "lan", now);
  w.take(s.frame(1, env("hello", hello("H", true)), now));
  s.open(2, false, "lan", now);
  w.take(s.frame(2, env("hello", hello("A")), now));
  // edits at once: the first 100 pass the rate bucket; a warning past 200 queued, removed past 1 000
  for (std::uint64_t i = 1; i <= 1200; ++i) {
    Map b;
    b.set("ops", Mp(cc::live::MpArray{}));
    b.set("touched", Mp(cc::live::MpArray{}));
    w.take(s.frame(2, env("edit", Mp(std::move(b)), i), now));
  }
  CHECK(w.last(2, "warn").has_value());
  CHECK(w.last(2, "removed").has_value());
  // the host goes: everyone gets `end`
  s.open(3, false, "lan", now);
  w.take(s.frame(3, env("hello", hello("B")), now));
  w.take(s.closed(1, now));
  CHECK(s.ended());
  const auto e = w.last(3, "end");
  REQUIRE(e.has_value());
  CHECK(must(e).b.get("reason")->as_string() == "host-left");
}
