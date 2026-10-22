// SPDX-License-Identifier: Apache-2.0
// Small JavaScript semantics the W5 app ports (CivilCraft, DetailCraft, MarkupCraft, TakeoffCraft) share:
// String(number) and Array#sort with a comparator (Math.round is libs/base's).
#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "cc/base/checked.hpp"
#include "cc/base/value.hpp"

namespace cc::appkit {

/// Math.round (libs/base).
using base::js_round;

/// String(n) for a number.
[[nodiscard]] inline std::string num_str(double n) {
  std::string s;
  base::append_number(s, n);
  return s;
}

/// Array#sort(cmp) as V8 sorts: stable, an item moves before another only when cmp(item, other) < 0. A merge sort
/// that stays well defined when the comparator is not consistent (NaN): it never reads outside the range.
template <class T, class Cmp>
void js_sort(std::vector<T>& v, Cmp cmp) {
  const std::size_t n = v.size();
  if (n < 2) {
    return;
  }
  std::vector<T> tmp;
  tmp.reserve(n);
  for (std::size_t width = 1; width < n; width *= 2) {
    tmp.clear();
    for (std::size_t lo = 0; lo < n; lo += 2 * width) {
      const std::size_t mid = std::min(lo + width, n);
      const std::size_t hi = std::min(lo + 2 * width, n);
      std::size_t i = lo;
      std::size_t j = mid;
      while (i < mid && j < hi) {
        const double c = cmp(v[j], v[i]);
        if (c < 0) {
          tmp.push_back(std::move(v[j++]));
        } else {
          tmp.push_back(std::move(v[i++]));
        }
      }
      while (i < mid) {
        tmp.push_back(std::move(v[i++]));
      }
      while (j < hi) {
        tmp.push_back(std::move(v[j++]));
      }
    }
    v.swap(tmp);
  }
}

}  // namespace cc::appkit
