// SPDX-License-Identifier: Apache-2.0
// Studio Live's handshake and frames (CadCraft 0.5 bin/test-live-crypto.mjs, ported): the shared vectors
// (tools/live-crypto/vectors.json of 0.5, written by Studio's Rust end; copied to
// tests/golden/js-0.5-live-crypto-vectors.json) byte for byte — codes, links, room tags, every handshake message, the
// exporter and every frame — then both ends here: wrong codes, other contexts, replayed / reordered / tampered /
// reflected frames, rekeys with frames in flight, misuse, signed-in names, random codes and links.
#include "cc/live/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"

using cc::base::Value;
using cc::live::Bytes;
using cc::live::HostHandshake;
using cc::live::JoinerHandshake;
using cc::live::Session;

namespace {

Bytes unhex(std::string_view h) {
  Bytes out;
  const auto nib = [](char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>((nib(h[i]) << 4U) | nib(h[i + 1])));
  }
  return out;
}
std::string hex(std::span<const std::uint8_t> b) {
  static constexpr std::string_view kD = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t x : b) {
    s += kD[x >> 4U];
    s += kD[x & 15U];
  }
  return s;
}
Bytes text(std::string_view s) {
  return {s.begin(), s.end()};
}
std::array<std::uint8_t, 64> rng_of(std::string_view h) {
  const Bytes b = unhex(h);
  std::array<std::uint8_t, 64> r{};
  for (std::size_t i = 0; i < r.size() && i < b.size(); ++i) {
    r[i] = b[i];
  }
  return r;
}

Value vectors() {
  const auto b = cc::base::read_file(CC_LIVE_CRYPTO_VECTORS, std::uint64_t{1} << 24U);
  REQUIRE(b.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*b));
  REQUIRE(v.has_value());
  return *v;
}

/// Both ends, joined.
std::pair<Session, Session> pair(std::string_view js, std::string_view hs, std::string_view ctx,
                                 std::string_view hctx = {}) {
  JoinerHandshake j(text(js), ctx);
  HostHandshake h(text(hs), hctx.empty() ? ctx : hctx);
  auto a = j.message();
  REQUIRE(a.has_value());
  auto b = h.receive(*a);
  REQUIRE(b.has_value());
  auto cj = j.receive(*b);
  REQUIRE(cj.has_value());
  auto fin = h.finish(*cj);
  REQUIRE(fin.has_value());
  auto joiner = j.finish(fin->first);
  REQUIRE(joiner.has_value());
  return {std::move(*joiner), std::move(fin->second)};
}

template <class R>
std::string code_of(const R& r) {
  return r.has_value() ? std::string("ok") : r.error().code;
}

}  // namespace

TEST_CASE("live crypto: the shared vectors, byte for byte") {
  REQUIRE(cc::live::crypto_ready());
  const Value v = vectors();
  CHECK(v["format"].as_string() == "cadcraft-live-crypto-vectors");
  CHECK(v["version"].as_number() == 1);
  for (const Value& c : v["codes"].as_array()) {
    INFO(c["code"].as_string());
    const auto p = cc::live::parse_code(c["code"].as_string());
    if (c["tag"].is_null()) {
      CHECK_FALSE(p.has_value());
    } else {
      REQUIRE(p.has_value());
      if (!p) {
        continue;
      }
      const auto& pv = *p;
      CHECK(pv.tag == c["tag"].as_string());
      CHECK(hex(text(pv.secret)) == c["secret"].as_string());
      CHECK(pv.context == "lan:" + c["tag"].as_string());
    }
  }
  for (const Value& l : v["links"].as_array()) {
    INFO(l["fragment"].as_string());
    const auto s = cc::live::parse_link_secret(l["fragment"].as_string());
    if (l["secret"].is_null()) {
      CHECK_FALSE(s.has_value());
    } else {
      REQUIRE(s.has_value());
      if (!s) {
        continue;
      }
      const auto& sv = *s;
      CHECK(hex(sv) == l["secret"].as_string());
    }
  }
  for (const Value& r : v["room_tags"].as_array()) {
    CHECK(cc::live::room_tag(r["room"].as_string()) == r["tag"].as_string());
  }
  std::size_t cases = 0;
  for (const Value& c : v["cases"].as_array()) {
    INFO(c["name"].as_string());
    JoinerHandshake j(unhex(c["joiner_secret"].as_string()), c["context"].as_string(),
                      rng_of(c["joiner_rng"].as_string()));
    const auto a = j.message();
    REQUIRE(a.has_value());
    CHECK(hex(*a) == c["a"].as_string());
    HostHandshake h(unhex(c["host_secret"].as_string()), c["context"].as_string());
    const auto b = h.receive(*a, rng_of(c["host_rng"].as_string()));
    REQUIRE(b.has_value());
    CHECK(hex(*b) == c["b"].as_string());
    const auto cj = j.receive(*b);
    REQUIRE(cj.has_value());
    CHECK(hex(*cj) == c["joiner_confirm"].as_string());
    ++cases;
    if (c.has("host_result")) {
      CHECK(code_of(h.finish(*cj)) == c["host_result"].as_string());
      continue;
    }
    auto fin = h.finish(*cj);
    REQUIRE(fin.has_value());
    CHECK(hex(fin->first) == c["host_confirm"].as_string());
    auto joiner = j.finish(fin->first);
    REQUIRE(joiner.has_value());
    Session& host = fin->second;
    CHECK(hex(joiner->exporter("ticket")) == c["exporter_ticket"].as_string());
    CHECK(joiner->exporter("ticket") == host.exporter("ticket"));
    std::size_t n = 0;
    for (const Value& f : c["frames"].as_array()) {
      ++n;
      INFO("frame " << n);
      const bool from_joiner = f["from"].as_string() == "joiner";
      Session& from = from_joiner ? *joiner : host;
      Session& to = from_joiner ? host : *joiner;
      const bool rekey = f["kind"].as_string() == "rekey";
      const auto frame = rekey ? from.rekey() : from.seal(unhex(f["body"].as_string()));
      REQUIRE(frame.has_value());
      CHECK(hex(*frame) == f["frame"].as_string());
      const auto got = to.open(*frame);
      REQUIRE(got.has_value());
      CHECK(got->rekey == rekey);
      if (!rekey) {
        CHECK(hex(got->body) == f["body"].as_string());
      }
    }
  }
  CHECK(cases == 4);
}

TEST_CASE("live crypto: wrong codes, contexts and messages") {
  {
    auto [j, h] = pair("Q4M2X", "Q4M2X", "lan:K7");
    const Bytes big = [] {
      Bytes b(std::size_t{1} << 20U);
      for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = static_cast<std::uint8_t>(i * 13);
      }
      return b;
    }();
    const auto f = h.seal(big);
    REQUIRE(f.has_value());
    const auto o = j.open(*f);
    REQUIRE(o.has_value());
    CHECK(o->body == big);  // 1 MB frame round trip
    CHECK(j.side() == Session::Side::joiner);
    CHECK(h.side() == Session::Side::host);
    CHECK(j.epoch() == 0);
  }
  {
    JoinerHandshake j(text("Q4M2X"), "lan:K7");
    HostHandshake h(text("Q4M2Y"), "lan:K7");
    const auto cj = j.receive(*h.receive(*j.message()));
    REQUIRE(cj.has_value());
    CHECK(code_of(h.finish(*cj)) == "wrong_code");  // the host refuses the joiner first
    CHECK(code_of(h.finish(*cj)) == "state");       // one try
  }
  {
    JoinerHandshake j(text("Q4M2X"), "lan:K7");
    HostHandshake h(text("Q4M2X"), "lan:K8");
    CHECK(code_of(h.finish(*j.receive(*h.receive(*j.message())))) == "wrong_code");  // other session context
  }
  {
    JoinerHandshake j(text("Q4M2X"), "lan:K7");
    HostHandshake fake(text("ZZZZZ"), "lan:K7");
    const auto a = j.message();
    REQUIRE(a.has_value());
    const auto b = fake.receive(*a);
    REQUIRE(b.has_value());
    const auto c = j.receive(*b);
    CHECK(c.has_value());
    CHECK(code_of(j.finish(Bytes(32))) == "wrong_code");
  }
  {
    JoinerHandshake j(text("s"), "x");
    HostHandshake h(text("s"), "x");
    CHECK(code_of(h.receive(Bytes(33))) == "bad_message");  // side byte
    Bytes bad = *j.message();
    std::fill(bad.begin() + 1, bad.end(), 0xff);
    CHECK(code_of(h.receive(bad)) == "bad_message");  // off the curve
    const Bytes a = *j.message();
    CHECK(code_of(h.receive(std::span(a).first(32))) == "bad_message");
    CHECK(code_of(j.receive(a)) == "bad_message");  // its own A message reflected
    CHECK(code_of(j.finish(Bytes(32))) == "state");
  }
}

TEST_CASE("live crypto: frames out of order, tampered, reflected; rekeys") {
  {
    auto [j, h] = pair("s", "s", "relay:ROOM");
    const auto f1 = *j.seal(text("1"));
    const auto f2 = *j.seal(text("2"));
    CHECK(code_of(h.open(f2)) == "order");
    CHECK(h.open(f1).has_value());
    CHECK(code_of(h.open(f1)) == "order");  // replayed
    CHECK(h.open(f2).has_value());
    auto f3 = *j.seal(text("3"));
    f3.back() ^= 1U;
    CHECK(code_of(h.open(f3)) == "bad_frame");
    const auto f4 = *j.seal(text("4"));
    auto ahead = f4;
    ahead[10] ^= 2U;
    CHECK(code_of(h.open(ahead)) == "order");
    auto to_next = f4;
    to_next[10] ^= 1U;
    CHECK(code_of(h.open(to_next)) == "bad_frame");
    const auto e = *h.seal(text("x"));
    CHECK(code_of(h.open(e)) == "order");  // reflected
    CHECK(code_of(h.open(Bytes(20))) == "bad_frame");
    auto v2 = *h.seal(text("y"));
    v2[0] = 2;
    CHECK(code_of(j.open(v2)) == "bad_frame");
  }
  {
    auto [j, h] = pair("s", "s", "lan:AB");
    const auto before = *j.seal(text("in flight"));
    const auto r = *h.rekey();
    const auto o1 = h.open(before);
    REQUIRE(o1.has_value());
    CHECK_FALSE(o1->rekey);  // a joiner frame in flight under the old key still opens
    const auto o2 = j.open(r);
    REQUIRE(o2.has_value());
    CHECK(o2->rekey);
    CHECK(j.epoch() == 1);
    const auto after = *j.seal(text("new"));
    CHECK(after[2] == 1);
    CHECK(h.open(after).has_value());
    CHECK(j.open(*h.seal(text("ok"))).has_value());
    CHECK(code_of(h.open(before)) == "order");
    CHECK(code_of(j.rekey()) == "role");
    auto forged = *j.seal(text("z"));
    forged[1] = 1;
    CHECK(code_of(h.open(forged)) == "role");
  }
  {
    auto [j, h] = pair("s", "s", "lan:AB");
    const auto r1 = *h.rekey();
    CHECK(j.open(r1).has_value());
    const auto e1 = *j.seal(text("e1"));
    const auto r2 = *h.rekey();
    CHECK(h.open(e1).has_value());  // under the middle key
    CHECK(j.open(r2).has_value());
    CHECK(h.open(*j.seal(text("e2"))).has_value());
    CHECK(j.epoch() == 2);
    auto fake = *j.seal(text("x"));
    fake[2] = 9;
    CHECK(code_of(h.open(fake)) == "order");
  }
  {
    auto [j, h] = pair("s", "s", "lan:AB");
    CHECK(j.open(*h.seal(text("before"))).has_value());
    CHECK(j.open(*h.rekey()).has_value());
    const auto later = *h.seal(text("after"));
    auto [j2, h2] = pair("s", "s", "lan:AB");
    CHECK(code_of(j2.open(later)) == "order");  // pairwise keys
    CHECK(j.open(later).has_value());
  }
}

TEST_CASE("live crypto: signed-in names (LV.19)") {
  REQUIRE(cc::live::crypto_ready());
  std::array<std::uint8_t, crypto_sign_SEEDBYTES> seed{};
  seed.fill(7);
  std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> pub{};
  std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_seed_keypair(pub.data(), sk.data(), seed.data());
  const std::vector<cc::live::AssertionKey> keys{{"test0001", cc::live::to_b64url(pub)}};
  auto [j, h] = pair("s", "s", "relay:K7QX9M2B");
  const std::string aud = cc::live::assertion_audience(j);
  CHECK(aud == cc::live::assertion_audience(h));
  CHECK(aud.size() == 43);
  const double now = 1'800'000'000;
  const auto mk = [&](const std::string& claims, const std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES>& key) {
    const std::string body = cc::live::to_b64url(text(claims));
    const std::string msg = "cadcraft-live-assertion/1." + body;
    std::array<std::uint8_t, crypto_sign_BYTES> sig{};
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),  // NOLINT
                         key.data());
    return "v1." + body + "." + cc::live::to_b64url(sig);
  };
  Bytes pic(3000, 9);
  std::array<std::uint8_t, 32> d{};
  crypto_hash_sha256(d.data(), pic.data(), pic.size());
  const std::string pic_hash = hex(d);
  const auto claims = [&](double iat, double exp, const std::string& kid, const std::string& name) {
    return R"({"v":1,"kid":")" + kid + R"(","iss":"getcadcraft.com","sub":"abc","name":")" + name +
           R"(","verified":true,"pic":")" + pic_hash + R"(","aud":")" + aud + R"(","iat":)" +
           std::to_string(static_cast<long long>(iat)) + R"(,"exp":)" + std::to_string(static_cast<long long>(exp)) +
           "}";
  };
  const std::string base = claims(now, now + 300, "test0001", "Ana <b>");
  const auto good = cc::live::verify_assertion(mk(base, sk), aud, now + 10, keys);
  CHECK(good.ok);
  CHECK(good.name == "Ana <b>");
  CHECK(good.pic == pic_hash);
  CHECK(good.verified);
  auto [other, other_h] = pair("s", "s", "x");
  CHECK(cc::live::verify_assertion(mk(base, sk), cc::live::assertion_audience(other), now, keys).reason == "audience");
  CHECK(cc::live::verify_assertion(mk(base, sk), aud, now + 400, keys).reason == "expired");
  CHECK(cc::live::verify_assertion(mk(claims(now + 1000, now + 1300, "test0001", "Ana"), sk), aud, now, keys).reason ==
        "expired");
  std::array<std::uint8_t, crypto_sign_SEEDBYTES> seed8{};
  seed8.fill(8);
  std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> pub8{};
  std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> sk8{};
  crypto_sign_seed_keypair(pub8.data(), sk8.data(), seed8.data());
  CHECK(cc::live::verify_assertion(mk(base, sk8), aud, now, keys).reason == "signature");
  CHECK(cc::live::verify_assertion(mk(claims(now, now + 300, "nope", "Ana"), sk), aud, now, keys).reason == "key");
  const std::string t = mk(base, sk);
  const std::string forged =
      "v1." + cc::live::to_b64url(text(claims(now, now + 300, "test0001", "Ben"))) + t.substr(t.rfind('.'));
  CHECK(cc::live::verify_assertion(forged, aud, now, keys).reason == "signature");
  CHECK(cc::live::verify_assertion("v1.abc", aud, now, keys).reason == "format");
  CHECK(cc::live::verify_assertion(mk(base, sk), aud, now, cc::live::assertion_keys()).reason == "key");
  CHECK(cc::live::picture_matches(pic, pic_hash));
  CHECK_FALSE(cc::live::picture_matches(std::span(pic).subspan(1), pic_hash));
  CHECK_FALSE(cc::live::picture_matches(Bytes(20000), pic_hash));
  CHECK(cc::live::assertion_keys().size() == 1);
  CHECK(cc::live::assertion_keys()[0].key.size() == 43);
}

TEST_CASE("live crypto: codes and links") {
  std::vector<std::string> seen;
  bool good = true;
  for (int i = 0; i < 200; ++i) {
    const std::string c = cc::live::new_code();
    good = good && c.size() == 8 && c[2] == '-' && cc::live::parse_code(c).has_value();
    seen.push_back(c);
    const std::string l = cc::live::new_link_secret();
    const auto s = cc::live::parse_link_secret(l);
    good = good && l.size() == 22 && s && s->size() == 16 && cc::live::parse_link_secret("#" + l).has_value();
  }
  CHECK(good);
  std::ranges::sort(seen);
  CHECK(std::ranges::adjacent_find(seen) == seen.end());
  const auto p = cc::live::parse_code("K7-Q4M2X");
  const auto q = cc::live::parse_code("k7 q4m2x");
  REQUIRE(p.has_value());
  REQUIRE(q.has_value());
  if (!p || !q) {
    return;
  }
  const auto& pv = *p;
  const auto& qv = *q;
  auto [j, h] = pair(pv.secret, qv.secret, pv.context);
  CHECK(h.open(*j.seal(text("x"))).has_value());
  const std::string ls = cc::live::new_link_secret();
  const auto s1 = cc::live::parse_link_secret(ls);
  const auto s2 = cc::live::parse_link_secret("#" + ls);
  CHECK(s1.has_value());
  CHECK(s1 == s2);
}
