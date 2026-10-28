// SPDX-License-Identifier: Apache-2.0
// `cadcraft-cli mcp`: CadCraft's headless MCP server (CadCraft 0.5 tools/mcp/server.mjs, docs/AGENTS-SPEC.md § 5.1).
// MCP over stdio: newline-delimited JSON-RPC 2.0 on stdin / stdout, anything else on stderr.
//
//   cadcraft-cli mcp --root <folder> [--root …] [--allow read,edit,save,export] [--log <file.jsonl>] [--verbose]
//   (or CADCRAFT_MCP_ROOTS=<folders separated by : or ;> and CADCRAFT_MCP_ALLOW=read,edit in the environment)
//
// Files only inside the --root folders (libs/mcp Roots); tools only from the --allow groups (default read). Each app's
// tools are registered in 0.5's order by register_tools below: the streams that port an app add theirs there.
#include "mcp_server.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/file.hpp"
#include "cc/base/js.hpp"
#include "cc/base/json.hpp"
#include "cc/base/value.hpp"
#include "cc/mcp/dispatcher.hpp"
#include "cc/mcp/env.hpp"

namespace cc::cli {

namespace {

constexpr std::string_view kVersion = "0.5.0";

#if defined(_WIN32)
constexpr char kDelimiter = ';';
#else
constexpr char kDelimiter = ':';
#endif

void err(std::string_view s) {
  std::string line(s);
  line += '\n';
  (void)std::fwrite(line.data(), 1, line.size(), stderr);
}

std::vector<std::string> split(std::string_view s, char c) {
  std::vector<std::string> out;
  std::size_t a = 0;
  for (;;) {
    const std::size_t b = s.find(c, a);
    std::string part(base::js_trim(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a)));
    if (!part.empty()) {
      out.push_back(std::move(part));
    }
    if (b == std::string_view::npos) {
      break;
    }
    a = b + 1;
  }
  return out;
}

std::string help() {
  return std::string("cadcraft-mcp ") + std::string(kVersion) +
         " \xe2\x80\x94 CadCraft's tools for an AI assistant (MCP over stdio)\n\n"
         "  cadcraft-cli mcp --root <folder> [--root <folder> \xe2\x80\xa6] [--allow read,edit,save,export] [--log "
         "<file.jsonl>] "
         "[--verbose]\n\n"
         "  --root     a folder the assistant may open files in and write to (repeat for more). Without one, no file "
         "is "
         "reachable.\n"
         "  --allow    permission groups (default read):\n"
         "               read    open, describe, list, query and check drawings, IFC models, takeoffs and PDF markups\n"
         "               edit    run commands and scripts on open drawings (each call one undo step)\n"
         "               save    write drawings (DWG/DXF) back, or as new drawing files\n"
         "               export  write PDF plots, converted drawings, CSV/XLSX\n"
         "  --log      append every call as a JSON line (inside a --root folder or the current folder)\n"
         "  --verbose  also print every call on stderr\n\n"
         "  Or in the environment: CADCRAFT_MCP_ROOTS (folders separated by " +
         std::string(1, kDelimiter) + ") and CADCRAFT_MCP_ALLOW (read,edit,\xe2\x80\xa6).\n";
}

/// The headless tools, in 0.5's order (tools/cli/headless/tools.mjs headlessTools).
std::expected<void, std::string> register_tools(mcp::Dispatcher& d, mcp::Env& env) {
  const auto add = [&d](mcp::Tool t) -> std::expected<void, std::string> {
    auto r = d.tool(std::move(t));
    if (!r) {
      return std::unexpected(r.error());
    }
    return {};
  };
  if (auto r = add(mcp::files_list_tool(env)); !r) {
    return r;
  }
  // CadCraft (W1): drawing_open … drawing_plot go here.
  if (auto r = add(mcp::doc_close_tool(env)); !r) {
    return r;
  }
  // BimCraft (W3): ifc_open, ifc_query, ifc_properties; TakeoffCraft (W5): takeoff_open … takeoff_export;
  // MarkupCraft (W5): markups_list.
  return {};
}

}  // namespace

int run_mcp(std::span<const std::string> args) {
  std::vector<std::string> roots;
  std::vector<std::string> allow{"read"};
  bool allow_given = false;
  std::optional<std::string> log_path;
  bool verbose = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const auto val = [&]() -> std::optional<std::string> {
      if (i + 1 >= args.size()) {
        return std::nullopt;
      }
      return args[++i];
    };
    std::optional<std::string> v;
    if (a == "--root" || a == "-r") {
      v = val();
      if (!v) {
        err("cadcraft-mcp: " + a + " needs a value.");
        return 2;
      }
      roots.push_back(*v);
    } else if (a.starts_with("--root=")) {
      roots.push_back(a.substr(7));
    } else if (a == "--allow" || a.starts_with("--allow=")) {
      std::string list;
      if (a == "--allow") {
        v = val();
        if (!v) {
          err("cadcraft-mcp: --allow needs a value.");
          return 2;
        }
        list = *v;
      } else {
        list = a.substr(8);
      }
      allow = split(list, ',');
      allow_given = true;
    } else if (a == "--log") {
      v = val();
      if (!v) {
        err("cadcraft-mcp: --log needs a value.");
        return 2;
      }
      log_path = *v;
    } else if (a == "--verbose" || a == "-v") {
      verbose = true;
    } else if (a == "--help" || a == "-h") {
      std::cout << help();
      return 0;
    } else if (a == "--version") {
      std::cout << kVersion << '\n';
      return 0;
    } else {
      err("cadcraft-mcp: Unknown option " + a + ". See --help.");
      return 2;
    }
  }
  if (roots.empty()) {
    if (auto e = base::env_var("CADCRAFT_MCP_ROOTS"); e && !e->empty()) {
      roots = split(*e, kDelimiter);
    }
  }
  if (!allow_given) {
    if (auto e = base::env_var("CADCRAFT_MCP_ALLOW"); e && !e->empty()) {
      allow = split(*e, ',');
    }
  }
  if (std::ranges::find(allow, "all") != allow.end()) {
    allow.assign(mcp::tool_groups().begin(), mcp::tool_groups().end());
  }
  for (const std::string& g : allow) {
    if (std::ranges::find(mcp::tool_groups(), g) == mcp::tool_groups().end()) {
      err("cadcraft-mcp: Unknown permission \"" + g + "\". Groups: read, edit, save, export.");
      return 2;
    }
  }
  auto made = mcp::Roots::make(roots);
  if (!made) {
    err("cadcraft-mcp: " + made.error().message);
    return 2;
  }
  mcp::Env env;
  env.roots = std::move(*made);
  std::optional<std::string> log_file;
  if (log_path) {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(*log_path, ec);
    const auto dir = std::filesystem::canonical(abs.parent_path(), ec);
    const auto cwd = std::filesystem::canonical(std::filesystem::current_path(ec), ec);
    if (ec || (!env.roots.root_of(dir.string()) && dir != cwd)) {
      err("cadcraft-mcp: --log must be inside a --root folder or the current folder.");
      return 2;
    }
    log_file = (dir / abs.filename()).string();
  }
  // the order of the groups as given, without repeats (a JS Set)
  std::vector<std::string> allowed;
  for (const std::string& g : allow) {
    if (std::ranges::find(allowed, g) == allowed.end()) {
      allowed.push_back(g);
    }
  }
  const auto joined = [](const std::vector<std::string>& l, std::string_view sep) {
    std::string s;
    for (std::size_t i = 0; i < l.size(); ++i) {
      s += (i != 0 ? std::string(sep) : std::string()) + l[i];
    }
    return s;
  };
  mcp::Options o;
  o.server = {"cadcraft", "CadCraft", std::string(kVersion)};
  o.allowed = [&allowed] { return allowed; };
  o.how_to_allow = [&allowed, &joined](std::string_view g) {
    std::vector<std::string> l = allowed;
    if (std::ranges::find(l, g) == l.end()) {
      l.emplace_back(g);
    }
    return "The person can restart the server with --allow " + joined(l, ",") + ".";
  };
  o.instructions = [&env, &allowed, &joined]() -> std::optional<std::string> {
    return "CadCraft tools for DWG/DXF drawings (CadCraft), IFC models (BimCraft), takeoffs (TakeoffCraft) and PDF "
           "markups "
           "(MarkupCraft). Files: only inside " +
           (env.roots.any() ? joined(env.roots.list(), ", ") : std::string("no folders (no --root was given)")) +
           ". Allowed: " + joined(allowed, ", ") +
           ". Open a file first (drawing_open, ifc_open, takeoff_open) and pass the handle it returns. command_run "
           "types "
           "at CadCraft's command line, one prompt answer per list item; every edit is one undo step labelled "
           "\"Assistant: \xe2\x80\xa6\".";
  };
  o.log = [&log_file, verbose](const base::Value& entry) {
    if (log_file) {
      std::string line = mcp::stringify(entry) + "\n";
      if (std::FILE* f = std::fopen(log_file->c_str(), "ab")) {  // NOLINT(cppcoreguidelines-owning-memory)
        (void)std::fwrite(line.data(), 1, line.size(), f);
        (void)std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
      } else {
        err("cadcraft-mcp: could not write the log.");
      }
    }
    if (verbose) {
      std::string s = "[" + base::js_string(entry.get("time")) + "] " + base::js_string(entry.get("tool")) + " " +
                      base::js_string(entry.get("outcome")) + " " + base::js_string(entry.get("ms")) + " ms";
      if (entry.has("message")) {
        s += " \xe2\x80\x94 " + base::js_string(entry.get("message"));
      }
      err(s);
    }
  };
  mcp::Dispatcher d(std::move(o));
  if (auto r = register_tools(d, env); !r) {
    err("cadcraft-mcp: " + r.error());
    return 2;
  }
  err("cadcraft-mcp " + std::string(kVersion) + ": folders " +
      (env.roots.any() ? joined(env.roots.list(), ", ") : std::string("(none \xe2\x80\x94 no file access)")) +
      "; allowed: " + joined(allowed, ", ") + ".");

  mcp::Session session;
  session.id = "stdio";
  const auto send = [](const std::optional<base::Value>& msg) {
    if (!msg) {
      return;
    }
    std::string line = mcp::stringify(*msg) + "\n";
    (void)std::fwrite(line.data(), 1, line.size(), stdout);
    (void)std::fflush(stdout);
  };
  const auto error = [&send](const base::Value& id, int code, const char* message) {
    base::Value e = base::Value::object();
    e.append("code", code);
    e.append("message", message);
    base::Value r = base::Value::object();
    r.append("jsonrpc", "2.0");
    r.append("id", id);
    r.append("error", std::move(e));
    send(r);
  };
  constexpr std::size_t kMaxLine = std::size_t{4} << 20U;  // 4 MiB, checked before parsing
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (base::js_trim(line).empty()) {
      continue;
    }
    if (line.size() > kMaxLine) {
      error(base::Value(nullptr), -32700, "Parse error");
      continue;
    }
    auto msg = base::parse_json(line, base::JsonLimits{kMaxLine, 512, kMaxLine});
    if (!msg) {
      error(base::Value(nullptr), -32700, "Parse error");
      continue;
    }
    if (msg->is_array()) {
      error(base::Value(nullptr), -32600, "Batches are not supported");
      continue;
    }
    send(d.handle(*msg, session));
  }
  return 0;
}

}  // namespace cc::cli
