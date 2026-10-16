// SPDX-License-Identifier: Apache-2.0
// cc::base — L0. Error type and Result alias shared by the byte readers and the checked integer helpers.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace cc::base {

enum class Errc : std::uint8_t {
  out_of_range,  // a read or seek past the end of the input
  overflow,      // an integer operation that would wrap
  invalid,       // input that breaks the format's rules
  too_big,       // input over a configured limit
  unsupported,   // a valid input this build does not handle
  io,            // the operating system refused a file operation
};

struct Error {
  Errc code = Errc::invalid;
  std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(Errc code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message)});
}

}  // namespace cc::base
