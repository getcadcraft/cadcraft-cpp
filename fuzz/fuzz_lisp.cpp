// SPDX-License-Identifier: Apache-2.0
// libFuzzer: LISP text read and evaluated (libs/lisp) with a step budget and a short longest string, so a runaway
// loop or a doubling structure ends; also the command line's completeness test and the wildcard matcher on the
// input's two halves.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cc/lisp/lisp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> raw(data, size);
  const std::span<const std::byte> bytes = std::as_bytes(raw);
  const std::string text = cc::lisp::decode_source(bytes);
  (void)cc::lisp::balanced(text);
  const std::size_t half = std::min<std::size_t>(text.size() / 2, 512);  // its own budget: the cost is text × pattern
  (void)cc::lisp::wcmatch(std::string_view(text).substr(0, half), std::string_view(text).substr(half, 512));
  cc::lisp::reset_counters();
  cc::lisp::Options o;
  o.max_steps = 100000;
  o.max_string = std::size_t{1} << 16U;
  o.out = [](const std::string& /*text*/, bool /*piece*/) {};
  cc::lisp::Interp L(std::move(o));
  L.put_file("lib.lsp", "(defun libfn (x) (* x 2))");
  (void)cc::lisp::run_now(L.eval_text(text));
  (void)cc::lisp::run_now(L.call_command("TEST"));
  return 0;
}
