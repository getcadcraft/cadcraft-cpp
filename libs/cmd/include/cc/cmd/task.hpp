// SPDX-License-Identifier: Apache-2.0
// cc::cmd — a command is a coroutine: cc::base::Task (cc/base/task.hpp), named here as the commands have always named
// it.
#pragma once

#include "cc/base/task.hpp"

namespace cc::cmd {

template <class T>
using Task = base::Task<T>;

}  // namespace cc::cmd
