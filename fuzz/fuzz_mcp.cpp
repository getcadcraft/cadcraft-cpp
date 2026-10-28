// SPDX-License-Identifier: Apache-2.0
// The MCP dispatcher (libs/mcp) on hostile JSON-RPC lines, as `cadcraft-cli mcp` reads them: parse, then handle with a
// tool whose schema uses every check (patterns, enums, anyOf, defaults). Never a crash; every answer stringifies.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cc/base/json.hpp"
#include "cc/mcp/dispatcher.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static cc::mcp::Dispatcher* d = [] {
    static cc::mcp::Dispatcher disp;
    auto schema = cc::base::parse_json(
        R"({"type":"object","properties":{"doc":{"type":"string","pattern":"^d[0-9]{1,6}$"},"n":{"type":"integer","minimum":1,"default":2},)"
        R"("s":{"anyOf":[{"type":"string","maxLength":3},{"type":"object","required":["h"],"additionalProperties":false}]},)"
        R"("e":{"enum":["a",1]},"a":{"type":"array","items":{"type":"number"},"maxItems":4}},"required":["doc"],"additionalProperties":false})");
    (void)disp.tool({"t", "", "", "read", *schema, std::nullopt,
                     [](cc::base::Value& a, cc::mcp::CallCtx&) -> cc::mcp::ToolAnswer { return a; }});
    return &disp;
  }();
  const std::string_view text(reinterpret_cast<const char*>(data),
                              size);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  auto msg = cc::base::parse_json(text, cc::base::JsonLimits{std::size_t{1} << 20U, 64, std::size_t{1} << 20U});
  if (!msg)
    return 0;
  cc::mcp::Session s;
  if (auto a = d->handle(*msg, s))
    (void)cc::mcp::stringify(*a, 1);
  return 0;
}
