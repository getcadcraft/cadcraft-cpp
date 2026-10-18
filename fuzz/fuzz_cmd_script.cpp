// SPDX-License-Identifier: Apache-2.0
// Command scripts from strangers (an agent, a script file): JSON → run_script on a fresh drawing must never crash,
// hang or trip a sanitizer, whatever the steps type, click or cancel, and stops at its step limit.
#include <cstddef>
#include <cstdint>
#include <span>

#include "cc/base/json.hpp"
#include "cc/base/reader.hpp"
#include "cc/cmd/script.hpp"
#include "cc/i18n/i18n.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (std::size_t{1} << 16U)) {
    return 0;
  }
  static const cc::i18n::Catalog kWords;  // no catalog: the words are their keys
  const auto bytes = std::as_bytes(std::span(data, size));
  const auto v = cc::base::parse_json(cc::base::as_text(bytes));
  if (!v) {
    return 0;
  }
  try {
    (void)cc::cmd::run_script(*v, kWords, {.max_steps = 2000});
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) — a command's refusal the engine would print
  }
  return 0;
}
