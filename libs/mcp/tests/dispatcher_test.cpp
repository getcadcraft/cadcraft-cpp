// SPDX-License-Identifier: Apache-2.0
// libs/mcp against CadCraft 0.5's dispatcher (host.js "mcp-core"): tests/golden/js-0.5-mcp-dispatcher.json holds what
// the frozen JS answered to a script of JSON-RPC messages with the test tools below on a fixed clock
// (tools/gen/mcp-dispatcher.mjs). Here the same tools answer the same script: every answer, every activity-log entry
// and every checkSchema case must be the same.
#include "cc/mcp/dispatcher.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cc/base/file.hpp"
#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/base/value.hpp"
#include "golden.hpp"

using cc::base::Value;

namespace {

Value golden() {
  const auto b = cc::base::read_file(CC_MCP_GOLDEN, std::uint64_t{1} << 24U);
  REQUIRE(b.has_value());
  auto v = cc::base::parse_json(cc::base::as_text(*b));
  REQUIRE(v.has_value());
  return *v;
}

Value json(std::string_view text) {
  auto v = cc::base::parse_json(text);
  REQUIRE(v.has_value());
  return *v;
}

void same(const Value& want, const Value& got, const std::string& where) {
  std::vector<std::string> diffs;
  // compare through JSON, as the transports see it
  const Value back = json(cc::mcp::stringify(got));
  cc::test::compare(want, back, where, diffs);
  for (const std::string& d : diffs) {
    UNSCOPED_INFO(d);
  }
  CHECK(diffs.empty());
}

const char* const kStrict = R"({"type":"object","properties":{
  "doc":{"type":"string","pattern":"^d[0-9]{1,6}$"},
  "colour":{"type":"string","pattern":"^#[0-9a-fA-F]{6}$"},
  "count":{"type":"integer","minimum":1,"maximum":10},
  "ratio":{"type":"number","minimum":0,"maximum":1},
  "name":{"type":"string","minLength":2,"maxLength":5},
  "kind":{"enum":["a","b",3]},
  "fixed":{"const":true},
  "pts":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":3},
  "mode":{"type":"string","default":"fast"},
  "shape":{"anyOf":[{"type":"string","maxLength":3},{"type":"object","properties":{"w":{"type":"number","default":1}},"required":["h"],"additionalProperties":false}]},
  "any":{"type":["string","null"]}},
  "required":["doc"],"additionalProperties":false})";

}  // namespace

TEST_CASE("mcp.golden: the 0.5 dispatcher's answers, log and schema checks") {
  const Value g = golden();
  double clock = g["t0"].as_number();
  std::vector<Value> log;
  cc::mcp::Options o;
  o.server = {"cadcraft", "CadCraft", "0.5.0"};
  o.now = [&clock] { return clock; };
  o.allowed = [] { return std::vector<std::string>{"read", "edit"}; };
  o.how_to_allow = [](std::string_view gr) { return "Restart with --allow read,edit," + std::string(gr) + "."; };
  o.instructions = [] { return std::optional<std::string>("Test instructions."); };
  o.info = [] { return json(R"({"app":"cadcraft","version":"0.5.0","extra":1})"); };
  o.log = [&log](const Value& e) { log.push_back(e); };
  cc::mcp::Dispatcher d(o);
  using cc::mcp::ToolAnswer;
  const auto add = [&d](cc::mcp::Tool t) {
    const bool ok = d.tool(std::move(t)).has_value();
    REQUIRE(ok);
  };
  add({"echo", "Echo", "Returns its arguments.", "read", json(kStrict), std::nullopt,
       [](Value& a, cc::mcp::CallCtx&) -> ToolAnswer { return a; }});
  add({"plain", "", "No schema; returns a number.", "read", std::nullopt, std::nullopt,
       [](Value&, cc::mcp::CallCtx&) -> ToolAnswer { return Value(42); }});
  add({"nothing", "", "", "read", std::nullopt, std::nullopt,
       [](Value&, cc::mcp::CallCtx&) -> ToolAnswer { return Value(); }});
  add({"loose", "", "", "read",
       json(R"({"type":"object","properties":{"a":{"type":"integer"}},"additionalProperties":{"type":"string"}})"),
       std::nullopt, [](Value& a, cc::mcp::CallCtx&) -> ToolAnswer {
         Value r = Value::object();
         r.append("got", a);
         r.append("undo", "Assistant: loose");
         return r;
       }});
  add({"fails", "", "", "edit", std::nullopt, std::nullopt, [](Value&, cc::mcp::CallCtx&) -> ToolAnswer {
         return std::unexpected(cc::mcp::ToolError{"the drawing is locked"});
       }});
  add({"refuses", "", "", "edit", std::nullopt, std::nullopt,
       [](Value&, cc::mcp::CallCtx&) -> ToolAnswer { return std::unexpected(cc::mcp::ToolError{"not now", true}); }});
  add({"soft_error", "", "", "read", std::nullopt, std::nullopt,
       [](Value&, cc::mcp::CallCtx&) -> ToolAnswer { return json(R"({"error":"no such layer","isError":true})"); }});
  add({"picture", "", "", "export", std::nullopt, std::nullopt, [](Value&, cc::mcp::CallCtx&) -> ToolAnswer {
         return cc::mcp::ToolResult{json(R"([{"type":"image","data":"iVBORw0K","mimeType":"image/png"}])"),
                                    json(R"({"width":2,"undo":"x"})")};
       }});
  add({"long_args", "", "", "read", std::nullopt, std::nullopt,
       [](Value&, cc::mcp::CallCtx&) -> ToolAnswer { return json(R"({"ok":true})"); }});
  d.method("custom.echo", [](const Value& p, cc::mcp::Session&) -> std::expected<Value, cc::mcp::RpcError> {
    Value r = Value::object();
    r.append("p", p);
    return r;
  });

  cc::mcp::Session session;
  session.id = "test";
  const auto& script = g["script"].as_array();
  const auto& answers = g["answers"].as_array();
  REQUIRE(script.size() == answers.size());
  for (std::size_t i = 0; i < script.size(); ++i) {
    clock += g["step"].as_number();
    INFO("message " << i << ": " << cc::mcp::stringify(script[i]));
    const auto got = d.handle(script[i], session);
    if (answers[i].is_null()) {
      CHECK_FALSE(got.has_value());
    } else {
      REQUIRE(got.has_value());
      if (got) {
        same(answers[i], *got, "answer");
      }
    }
  }
  const auto& want_log = g["log"].as_array();
  REQUIRE(log.size() == want_log.size());
  for (std::size_t i = 0; i < log.size(); ++i) {
    INFO("log " << i);
    same(want_log[i], log[i], "log");
    CHECK(cc::mcp::stringify(log[i]) == cc::mcp::stringify(want_log[i]));  // the log line, key order included
  }
  for (const Value& c : g["schema"].as_array()) {
    INFO(cc::mcp::stringify(c["schema"]) << " on " << cc::mcp::stringify(c["value"]));
    Value v = c["value"];
    const auto problems = cc::mcp::check_schema(c["schema"], v);
    std::vector<std::string> want;
    for (const Value& p : c["problems"].as_array()) {
      want.push_back(p.as_string());
    }
    CHECK(problems == want);
    same(c["after"], v, "after");
  }
}

TEST_CASE("mcp: registration rules and the pattern subset") {
  cc::mcp::Dispatcher d;
  const auto run = [](Value&, cc::mcp::CallCtx&) -> cc::mcp::ToolAnswer { return Value(); };
  CHECK_FALSE(d.tool({"Bad", "", "", "read", std::nullopt, std::nullopt, run}).has_value());
  CHECK_FALSE(d.tool({"ok", "", "", "admin", std::nullopt, std::nullopt, run}).has_value());
  CHECK_FALSE(d.tool({"ok", "", "", "read", std::nullopt, std::nullopt, {}}).has_value());
  const auto id = d.tool({"ok", "", "", "read", std::nullopt, std::nullopt, run});
  REQUIRE(id.has_value());
  const auto id2 = d.tool({"ok", "Again", "", "read", std::nullopt, std::nullopt, run});
  REQUIRE(id2.has_value());
  d.remove("ok", *id);  // an old registration: nothing happens
  CHECK(d.has("ok"));
  d.remove("ok", *id2);
  CHECK_FALSE(d.has("ok"));
  CHECK(cc::mcp::pattern_matches("^d[0-9]{1,6}$", "d123456") == true);
  CHECK(cc::mcp::pattern_matches("^d[0-9]{1,6}$", "d1234567") == false);
  CHECK(cc::mcp::pattern_matches("^#[0-9a-fA-F]{6}$", "#00ffAA") == true);
  CHECK(cc::mcp::pattern_matches("SOLID", "a SOLID b") == true);
  CHECK(cc::mcp::pattern_matches("^a(b|c)$", "ab") == std::nullopt);
  CHECK(cc::mcp::iso_time(0) == "1970-01-01T00:00:00.000Z");
  CHECK(cc::mcp::iso_time(1792108800123) == "2026-10-16T00:00:00.123Z");
}
