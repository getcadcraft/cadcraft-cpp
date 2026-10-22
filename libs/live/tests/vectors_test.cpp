// SPDX-License-Identifier: Apache-2.0
// Studio Live's shared vectors (docs/LIVE-PROTOCOL.md § 13; tools/live-test/vectors.json of CadCraft 0.5, copied to
// tests/golden/js-0.5-live-vectors.json): every envelope encodes to its bytes and decodes back, the other valid
// encodings decode, the bad inputs are refused, and the scenarios decide every edit as the 0.5 session server does.
// Plus the properties live-client.js's own test checks beyond the vectors (tools/live-test/test-vectors.mjs).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/live/envelope.hpp"
#include "cc/live/msgpack.hpp"
#include "cc/live/session_core.hpp"

using cc::base::Value;
using cc::live::Big;
using cc::live::Map;
using cc::live::Mp;
using cc::live::MpBytes;

namespace {

MpBytes from_hex(std::string_view h) {
  MpBytes out;
  const auto nib = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') {
      return static_cast<std::uint8_t>(c - '0');
    }
    return static_cast<std::uint8_t>(c - 'a' + 10);
  };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>((nib(h[i]) << 4U) | nib(h[i + 1])));
  }
  return out;
}

std::string to_hex(const MpBytes& b) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s;
  for (const std::uint8_t x : b) {
    s += kDigits[x >> 4U];
    s += kDigits[x & 15U];
  }
  return s;
}

/// test-vectors.mjs fromJson: {"$bin": hex} → bytes, {"$int": "digits"} → an integer (a Big past the safe range).
// NOLINTNEXTLINE(misc-no-recursion) — bounded by the vectors file
Mp from_json(const Value& j) {
  switch (j.kind()) {
    case Value::Kind::null:
    case Value::Kind::undefined: return {nullptr};
    case Value::Kind::boolean: return {j.as_bool()};
    case Value::Kind::number: return {j.as_number()};
    case Value::Kind::string: return {j.as_string()};
    case Value::Kind::array: {
      cc::live::MpArray a;
      for (const Value& x : j.as_array()) {
        a.push_back(from_json(x));
      }
      return {std::move(a)};
    }
    case Value::Kind::object: {
      const auto& o = j.as_object();
      if (o.size() == 1 && o[0].key == "$bin") {
        return {from_hex(o[0].value.as_string())};
      }
      if (o.size() == 1 && o[0].key == "$int") {
        const std::string& s = o[0].value.as_string();
        const bool neg = !s.empty() && s[0] == '-';
        const std::uint64_t mag = std::stoull(neg ? s.substr(1) : s);
        if (neg) {
          return mag <= 9007199254740991U ? Mp(-static_cast<double>(mag)) : Mp(Big{true, mag});
        }
        return {mag};
      }
      Map m;
      for (const auto& member : o) {
        m.set(member.key, from_json(member.value));
      }
      return {std::move(m)};
    }
    case Value::Kind::bytes: break;
  }
  return {nullptr};
}

/// assert.deepEqual on plain objects: map members in any order.
// NOLINTNEXTLINE(misc-no-recursion) — bounded by the vectors file
bool same(const Mp& a, const Mp& b) {
  if (a.kind() != b.kind()) {
    return false;
  }
  if (a.is_array()) {
    if (a.as_array().size() != b.as_array().size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.as_array().size(); ++i) {
      if (!same(a.as_array()[i], b.as_array()[i])) {
        return false;
      }
    }
    return true;
  }
  if (a.is_map()) {
    if (a.as_map().size() != b.as_map().size()) {
      return false;
    }
    // NOLINTNEXTLINE(misc-no-recursion) — bounded by the vectors file
    return std::ranges::all_of(a.as_map().entries(), [&b](const cc::live::MapEntry& e) {
      const Mp* w = b.as_map().find(e.first);
      return w != nullptr && same(e.second, *w);
    });
  }
  return a == b;
}

Value vectors() {
  const auto text = cc::base::read_file(CC_LIVE_VECTORS, std::uint64_t{1} << 22U);
  REQUIRE(text.has_value());
  const std::string_view s(reinterpret_cast<const char*>(text->data()), text->size());  // NOLINT
  auto v = cc::base::parse_json(s);
  REQUIRE(v.has_value());
  return *v;
}

}  // namespace

TEST_CASE("live: vectors.json is protocol version 1") {
  const Value v = vectors();
  CHECK(v["format"].as_string() == "cadcraft-live-vectors");
  CHECK(v["protocol"].as_number() == 1);
}

TEST_CASE("live: every envelope encodes to the shared bytes and decodes to the shared value") {
  const Value v = vectors();
  std::size_t n = 0;
  for (const Value& e : v["envelopes"].as_array()) {
    const Mp value = from_json(e["value"]);
    INFO(e["name"].as_string());
    CHECK(to_hex(cc::live::pack(value)) == e["hex"].as_string());
    const auto back = cc::live::unpack(from_hex(e["hex"].as_string()));
    REQUIRE(back.has_value());
    CHECK(same(*back, value));
    // through the envelope type too: same bytes
    const auto env = cc::live::envelope_from(value);
    REQUIRE(env.has_value());
    CHECK(to_hex(cc::live::encode(*env)) == e["hex"].as_string());
    ++n;
  }
  CHECK(n == 54);
}

TEST_CASE("live: other valid encodings decode, bad inputs are refused") {
  const Value v = vectors();
  for (const Value& e : v["decode_only"].as_array()) {
    INFO(e["name"].as_string());
    const auto back = cc::live::unpack(from_hex(e["hex"].as_string()));
    REQUIRE(back.has_value());
    CHECK(same(*back, from_json(e["value"])));
  }
  for (const Value& e : v["bad"].as_array()) {
    INFO(e["name"].as_string());
    CHECK_FALSE(cc::live::unpack(from_hex(e["hex"].as_string())).has_value());
  }
}

TEST_CASE("live: the writer's properties test-vectors.mjs checks") {
  // a Map keeps its order (here: keys set in that order)
  Map m;
  m.set("z", Mp(1));
  m.set("a", Mp(2));
  CHECK(to_hex(cc::live::pack(Mp(m))) == "82a17a01a16102");
  // a decoded __proto__ key is dropped
  const auto p = cc::live::unpack(from_hex("81a95f5f70726f746f5f5f81a178c3"));
  REQUIRE(p.has_value());
  CHECK(p->as_map().empty());
  // deep nesting is refused
  CHECK_FALSE(cc::live::unpack(MpBytes(1000, 0x91)).has_value());
  // a huge count with no data is refused before allocating
  CHECK_FALSE(cc::live::unpack(from_hex("ddffffffff")).has_value());
  CHECK_FALSE(cc::live::unpack(from_hex("dfffffffff")).has_value());
}

TEST_CASE("live: a decoded map is a JS object (index keys first, a repeated key keeps its place)") {
  // {"b":1, "2":2, "1":3, "b":4} → {"1":3, "2":2, "b":4}
  const auto v = cc::live::unpack(from_hex("84a16201a13202a13103a16204"));
  REQUIRE(v.has_value());
  const auto e = v->as_map().entries();
  REQUIRE(e.size() == 3);
  CHECK(e[0].first == "1");
  CHECK(e[1].first == "2");
  CHECK(e[2].first == "b");
  CHECK(e[2].second.as_number() == 4);
  // non-string keys become strings: {1: true, null: false}
  const auto k = cc::live::unpack(from_hex("8201c3c0c2"));
  REQUIRE(k.has_value());
  CHECK(k->get("1") != nullptr);
  CHECK(k->get("null") != nullptr);
  // integral floats re-encode as integers, as JS numbers do
  const auto f = cc::live::unpack(from_hex("cb4000000000000000"));
  REQUIRE(f.has_value());
  CHECK(to_hex(cc::live::pack(*f)) == "02");
  // ill-formed UTF-8 reads as U+FFFD (TextDecoder), a lone surrogate writes as U+FFFD (TextEncoder)
  const auto s = cc::live::unpack(from_hex("a3ff6162"));
  REQUIRE(s.has_value());
  CHECK(s->as_string() ==
        "\xef\xbf\xbd"
        "ab");
  CHECK(to_hex(cc::live::pack(Mp(std::string("\xed\xa0\x80")))) == "a3efbfbd");
}

TEST_CASE("live: the shared scenarios decide as the 0.5 session server") {
  const Value v = vectors();
  std::size_t ran = 0;
  for (const Value& sc : v["scenarios"].as_array()) {
    INFO(sc["name"].as_string());
    cc::live::SessionCore core;
    std::map<std::string, std::uint64_t> id;
    std::uint64_t next = 1;
    for (const Value& name : sc["clients"].as_array()) {
      id[name.as_string()] = next;
      core.join(next, name.as_string() == "H" ? cc::live::Role::host : cc::live::Role::editor);
      ++next;
    }
    std::uint64_t now = 0;
    std::size_t i = 0;
    for (const Value& st : sc["steps"].as_array()) {
      ++i;
      INFO("step " << i);
      now += 10;
      const std::uint64_t by = id.at(st["by"].as_string());
      const auto keys = [&st](std::string_view k) {
        std::vector<std::string> out;
        for (const Value& x : st[k].as_array()) {
          out.push_back(x.as_string());
        }
        return out;
      };
      if (st.has("touched")) {
        const Mp list = from_json(st["touched"]);
        const auto touched = cc::live::touched_from(&list);
        const auto d = core.edit(by, touched);
        const Value& want = st["expect"];
        if (want.has("commit")) {
          CHECK(d.reason.empty());
          CHECK(d.ok);
          CHECK(d.q == static_cast<std::uint64_t>(want["commit"].as_number()));
        } else {
          CHECK_FALSE(d.ok);
          CHECK(d.reason == want["refuse"].as_string());
          if (want.has("by")) {
            CHECK(d.by == id.at(want["by"].as_string()));
          }
        }
      } else if (st.has("hold")) {
        const auto k = keys("hold");
        core.hold(by, k, now);
      } else if (st.has("release")) {
        const auto k = keys("release");
        core.release(by, k);
      } else if (st.has("lock")) {
        const auto k = keys("lock");
        core.lock(k);
      } else if (st.has("unlock")) {
        const auto k = keys("unlock");
        core.unlock(k);
      } else if (st.has("role")) {
        const auto role = cc::live::parse_role(st["role"]["role"].as_string());
        REQUIRE(role.has_value());
        if (role) {
          core.set_role(id.at(st["role"]["who"].as_string()), *role);
        }
      }
    }
    ++ran;
  }
  CHECK(ran == 11);
}

TEST_CASE("live: holds expire after 30 s unless refreshed; someone else's hold is skipped") {
  cc::live::SessionCore core;
  core.join(1, cc::live::Role::host);
  core.join(2, cc::live::Role::editor);
  const std::vector<std::string> k{"e:1"};
  CHECK(core.hold(2, k, 0).size() == 1);
  CHECK(core.hold(1, k, 5).empty());  // held by 2: skipped
  CHECK(core.holder("e:1") == 2);
  core.hold(2, k, 20'000);  // refreshed
  CHECK(core.expire(40'000).empty());
  CHECK(core.expire(50'000).size() == 1);
  CHECK(core.holder("e:1") == 0);
}
