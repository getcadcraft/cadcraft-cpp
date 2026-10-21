// SPDX-License-Identifier: Apache-2.0
// cc::base — a coroutine that is awaited: Task<T> is what a command (libs/cmd) or a LISP evaluation (libs/lisp), and
// each helper it awaits, returns. A Task starts when it is awaited (or when its owner starts it), runs until it asks
// for input (the owner keeps the asking frame and resumes it with the answer) and hands its result to whoever awaited
// it. Destroying a Task destroys its frame and, through the Task it was awaiting, every frame below it: that is how Esc
// cancels a command (no exception; the frames' destructors run, as the JS's `finally` blocks do). An exception thrown
// inside travels up to whoever awaits, as a rejected promise does in the JS. Awaiting resumes the callee by symmetric
// transfer, so a long chain of awaits does not grow the native stack.
#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cc::base {

template <class T>
class [[nodiscard]] Task;

namespace detail {

struct PromiseBase {
  std::coroutine_handle<> continuation;
  std::exception_ptr error;

  [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
  struct Final {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
      const std::coroutine_handle<> c = h.promise().continuation;
      return c ? c : std::noop_coroutine();
    }
    void await_resume() const noexcept {}
  };
  [[nodiscard]] Final final_suspend() const noexcept { return {}; }
  void unhandled_exception() noexcept { error = std::current_exception(); }
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
 public:
  struct promise_type : detail::PromiseBase {
    std::optional<T> value;
    Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
    void return_value(T v) { value = std::move(v); }
  };

  Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  Task& operator=(Task&& o) noexcept {
    if (this != &o) {
      reset();
      h_ = std::exchange(o.h_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { reset(); }

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
    h_.promise().continuation = caller;
    return h_;
  }
  T await_resume() {
    if (h_.promise().error) {
      std::rethrow_exception(h_.promise().error);
    }
    std::optional<T>& v = h_.promise().value;
    if (!v) {
      throw std::logic_error("cc::base::Task: resumed without a result");
    }
    return std::move(*v);
  }

 private:
  explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
  void reset() noexcept {
    if (h_) {
      h_.destroy();
      h_ = {};
    }
  }
  std::coroutine_handle<promise_type> h_;
};

template <>
class [[nodiscard]] Task<void> {
 public:
  struct promise_type : detail::PromiseBase {
    Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
    void return_void() const noexcept {}
  };

  Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  Task& operator=(Task&& o) noexcept {
    if (this != &o) {
      reset();
      h_ = std::exchange(o.h_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { reset(); }

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
    h_.promise().continuation = caller;
    return h_;
  }
  void await_resume() const {
    if (h_.promise().error) {
      std::rethrow_exception(h_.promise().error);
    }
  }

  // ---- for the engine, which runs a command's root task
  /// Runs the task until it first waits for input or ends.
  void start() const { h_.resume(); }
  [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }
  /// The exception the task ended with, if any.
  [[nodiscard]] std::exception_ptr error() const noexcept { return h_ ? h_.promise().error : nullptr; }

 private:
  explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
  void reset() noexcept {
    if (h_) {
      h_.destroy();
      h_ = {};
    }
  }
  std::coroutine_handle<promise_type> h_;
};

}  // namespace cc::base
