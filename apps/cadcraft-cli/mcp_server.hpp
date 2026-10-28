// SPDX-License-Identifier: Apache-2.0
// `cadcraft-cli mcp` (tools/mcp/server.mjs of CadCraft 0.5): see mcp_server.cpp.
#pragma once

#include <span>
#include <string>

namespace cc::cli {

/// Runs the stdio MCP server with the arguments after `mcp`; returns the exit code.
[[nodiscard]] int run_mcp(std::span<const std::string> args);

}  // namespace cc::cli
