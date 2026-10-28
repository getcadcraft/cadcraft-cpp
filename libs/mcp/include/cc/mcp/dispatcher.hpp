// SPDX-License-Identifier: Apache-2.0
// cc::mcp — L2. The tool dispatcher every surface shares (CadCraft 0.5 host.js / tools/mcp/core.mjs "mcp-core",
// docs/AGENTS-SPEC.md § 1, § 6): the headless MCP server (`cadcraft-cli mcp`, stdio), the CLI, and the app windows'
// control channel. JSON-RPC 2.0: the MCP methods (initialize, ping, tools/list, tools/call, logging/setLevel, the
// notifications) and the JSON control channel's (control.hello, tools.list, tools.call, app.info), plus methods a
// host adds. Tool names, definitions, permission groups, the schema check, results, errors and the activity log are
// 0.5's, word for word, so an assistant sees the same server.
//
//   Dispatcher d({.server = {"cadcraft", "CadCraft", "0.5.0"}, .allowed = [] { return std::vector{"read"s}; }});
//   d.tool({.name = "drawing_open", .group = "read", …, .run = [](base::Value& args, CallCtx& ctx) { … }});
//   auto answer = d.handle(message, session);   // a JSON-RPC response, or nullopt for a notification
//
// A tool returns plain JSON (sent as structuredContent and as text) or a ToolResult (its own content list, e.g.
// images). A ToolError → a result with isError and its message. Arguments are checked against inputSchema first
// (defaults filled).
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cc/base/value.hpp"

namespace cc::mcp {

/// The MCP protocol versions this server speaks, newest first (host.js MCP_PROTOCOLS).
[[nodiscard]] std::span<const std::string_view> protocols();
/// The permission groups (host.js TOOL_GROUPS): read, edit, save, export.
[[nodiscard]] std::span<const std::string_view> tool_groups();
/// The annotations an MCP client shows for a group (groupHints).
[[nodiscard]] base::Value group_hints(std::string_view group);

/// checkSchema(schema, value, where): the problems (empty = fine). Fills in defaults of object properties.
[[nodiscard]] std::vector<std::string> check_schema(const base::Value& schema, base::Value& value,
                                                    std::string_view where = "arguments");
/// logArgs(args): strings over 200 characters cut, at most 20 keys, nested once.
[[nodiscard]] base::Value log_args(const base::Value& args, int depth = 0);
/// JSON.stringify(v, null, indent) (indent 0 = compact).
[[nodiscard]] std::string stringify(const base::Value& v, int indent = 0);
/// new Date(ms).toISOString().
[[nodiscard]] std::string iso_time(double ms);
/// Does `pattern` (the JSON-Schema subset the tools use: ^ $, literals, [classes], {m,n} * + ?) match `text`?
/// nullopt: a pattern outside the subset.
[[nodiscard]] std::optional<bool> pattern_matches(std::string_view pattern, std::string_view text);

struct Client {
  std::string name;
  std::string version;
};

/// One connection's state (the JS session object).
struct Session {
  std::string id;
  std::optional<Client> client;
  std::optional<std::string> client_name;  // session.clientName (a transport's own name for it)
  std::string protocol_version;
  std::optional<double> initialized_at;
  bool ready = false;
  std::string log_level;
};

/// toolResult(content, structured).
struct ToolResult {
  base::Value content = base::Value::array();
  std::optional<base::Value> structured;
};
/// A tool's failure: `refused` = the outcome logged is "refused" (else "error").
struct ToolError {
  std::string message;
  bool refused = false;
};
using ToolAnswer = std::expected<std::variant<base::Value, ToolResult>, ToolError>;

struct CallCtx {
  std::string group;
  std::optional<std::string> client;
  Session& session;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) a view for one call, never kept
  std::string tool;
  base::Value& log;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) the log entry being written
};

struct Tool {
  std::string name;  // ^[a-z][a-z0-9_]{0,63}$
  std::string title;
  std::string description;
  std::string group;  // read edit save export
  std::optional<base::Value> input_schema;
  std::optional<base::Value> output_schema;
  std::function<ToolAnswer(base::Value& args, CallCtx& ctx)> run;
};

struct ServerInfo {
  std::string name = "cadcraft";
  std::string title = "CadCraft";
  std::string version = "0.0.0";
};

struct Options {
  ServerInfo server;
  std::function<double()> now;                        // ms since the epoch (default: the system clock)
  std::function<std::vector<std::string>()> allowed;  // default: read
  std::function<std::string(std::string_view group)> how_to_allow;
  std::function<std::optional<std::string>()> instructions;
  std::function<base::Value()> info;
  std::function<void(const base::Value& entry)> log;
  std::function<void(std::string_view event, Session&)> on_event;
  bool list_changed = true;
};

/// A JSON-RPC error a call raises.
struct RpcError {
  int code = -32603;
  std::string message;
  std::optional<base::Value> data;
};

class Dispatcher {
 public:
  using Method = std::function<std::expected<base::Value, RpcError>(const base::Value& params, Session&)>;
  explicit Dispatcher(Options opts = {});

  /// Registers (or replaces) a tool. Errors for a bad name, group or no run(). Returns its registration number for
  /// `remove`.
  [[nodiscard]] std::expected<std::uint64_t, std::string> tool(Tool t);
  /// Unregisters, if `id` is still the tool's registration.
  void remove(std::string_view name, std::uint64_t id);
  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] const Tool* get(std::string_view name) const;
  /// The definitions the permissions allow, in registration order.
  [[nodiscard]] base::Value list() const;
  void on_notify(std::function<void(const base::Value&)> fn);
  void changed();
  void method(std::string name, Method fn);

  /// Runs a tool: permission, schema, the tool, the log → a CallToolResult (RpcError -32602 for an unknown tool).
  [[nodiscard]] std::expected<base::Value, RpcError> call(std::string_view name, const base::Value& args, Session& s);
  /// One JSON-RPC message → a response, or nullopt for a notification. Never throws.
  [[nodiscard]] std::optional<base::Value> handle(const base::Value& msg, Session& s);

  [[nodiscard]] const ServerInfo& server() const noexcept { return opts_.server; }

 private:
  struct Entry {
    Tool tool;
    std::uint64_t id = 0;
  };
  [[nodiscard]] std::vector<std::string> allowed() const;
  [[nodiscard]] base::Value definition(const Tool& t) const;
  [[nodiscard]] double now() const;

  Options opts_;
  std::vector<Entry> tools_;  // registration order (a JS Map: re-registering keeps the place)
  std::vector<std::function<void(const base::Value&)>> notifiers_;
  std::map<std::string, Method, std::less<>> methods_;
  std::uint64_t next_id_ = 1;
};

}  // namespace cc::mcp
