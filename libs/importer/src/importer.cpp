// SPDX-License-Identifier: Apache-2.0
#include "cc/importer/importer.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "cc/base/json.hpp"

#if defined(__linux__)
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <cstddef>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define CC_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CC_SANITIZED 1
#endif

namespace cc::importer {

using base::Errc;
using base::fail;

#if defined(__linux__)

namespace {

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd_(o.release()) {}
  Fd& operator=(Fd&& o) noexcept {
    reset(o.release());
    return *this;
  }
  ~Fd() { reset(); }
  [[nodiscard]] int get() const noexcept { return fd_; }
  int release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
  }
  void reset(int f = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = f;
  }

 private:
  int fd_ = -1;
};

std::string signal_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL (a bounds check trapped)";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGKILL: return "SIGKILL (a limit was reached)";
    case SIGXCPU: return "SIGXCPU (the CPU-time limit)";
    case SIGSYS: return "SIGSYS (a system call the filter refuses)";
    default: return "signal " + std::to_string(sig);
  }
}

}  // namespace

base::Result<base::Value> run_helper(const std::filesystem::path& helper, std::string_view format,
                                     std::span<const std::byte> bytes, const Limits& limits) {
  // stdin: a socket pair, so a dead helper gives EPIPE (MSG_NOSIGNAL) instead of SIGPIPE in the caller.
  std::array<int, 2> in{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in.data()) != 0) {
    return fail(Errc::io, "cannot create the importer's input channel");
  }
  Fd in_parent(in[0]);
  Fd in_child(in[1]);
  std::array<int, 2> out{-1, -1};
  if (::pipe2(out.data(), O_CLOEXEC) != 0) {
    return fail(Errc::io, "cannot create the importer's output channel");
  }
  Fd out_parent(out[0]);
  Fd out_child(out[1]);

  posix_spawn_file_actions_t fa{};
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in_child.get(), 0);
  posix_spawn_file_actions_adddup2(&fa, out_child.get(), 1);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  std::string exe = helper.string();
  std::string fmt(format);
  std::vector<char*> argv{exe.data(), fmt.data(), nullptr};
  pid_t pid = 0;
  // An empty environment: the helper reads nothing from it.
  std::array<char*, 1> envp{nullptr};
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    return fail(Errc::io, "cannot start the importer (" + std::error_code(rc, std::generic_category()).message() + ")");
  }
  in_child.reset();
  out_child.reset();

  // Send the file, then read the answer, both under one wall-clock deadline.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(limits.max_seconds);
  auto ms_left = [&]() {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::max<std::int64_t>(0, left.count()));
  };
  bool timed_out = false;
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    pollfd p{in_parent.get(), POLLOUT, 0};
    if (::poll(&p, 1, ms_left()) <= 0) {
      timed_out = true;
      break;
    }
    const auto rest = bytes.subspan(sent);
    const ssize_t n = ::send(in_parent.get(), rest.data(), std::min<std::size_t>(rest.size(), 1U << 20U), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      break;  // the helper went away; its exit status says why
    }
    sent += static_cast<std::size_t>(n);
  }
  ::shutdown(in_parent.get(), SHUT_WR);
  std::string json;
  std::array<char, 1U << 16U> buf{};
  bool too_big = false;
  while (!timed_out) {
    pollfd p{out_parent.get(), POLLIN, 0};
    const int pr = ::poll(&p, 1, ms_left());
    if (pr == 0) {
      timed_out = true;
      break;
    }
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    const ssize_t n = ::read(out_parent.get(), buf.data(), buf.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    if (json.size() + static_cast<std::size_t>(n) > limits.max_output) {
      too_big = true;
      break;
    }
    json.append(buf.data(), static_cast<std::size_t>(n));
  }
  if (timed_out || too_big) {
    ::kill(pid, SIGKILL);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out) {
    return fail(Errc::too_big,
                "the importer stopped: it took longer than " + std::to_string(limits.max_seconds) + " s");
  }
  if (too_big) {
    return fail(Errc::too_big, "the importer stopped: its answer was larger than the limit");
  }
  if (WIFSIGNALED(status)) {
    return fail(Errc::invalid, "the importer stopped: " + signal_name(WTERMSIG(status)));
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  auto v = base::parse_json(json);
  if (!v) {
    return fail(Errc::invalid, "the importer stopped (exit " + std::to_string(code) + "): " + v.error().message);
  }
  if (code != 0) {
    const base::Value& e = (*v)["$error"];
    std::string m = e["message"].is_string() ? e["message"].as_string() : "exit " + std::to_string(code);
    return fail(code == 3 ? Errc::unsupported : Errc::invalid, m);
  }
  return v;
}

bool lock_down_helper(const Limits& limits) {
  auto cap = [](int res, rlim_t v) {
    rlimit r{v, v};
    return ::setrlimit(res, &r) == 0;
  };
  bool ok = cap(RLIMIT_CPU, static_cast<rlim_t>(limits.max_seconds)) && cap(RLIMIT_CORE, 0) && cap(RLIMIT_FSIZE, 0);
#if !defined(CC_SANITIZED)
  // Sanitizers reserve terabytes of address space, and LeakSanitizer clones a thread to check for leaks at exit (an
  // NPROC cap of 0 binds everyone but root): these caps are for the shipping build.
  ok = ok && cap(RLIMIT_AS, static_cast<rlim_t>(limits.max_memory)) && cap(RLIMIT_NPROC, 0);
#endif
#if defined(__linux__) && !defined(CC_SANITIZED)
  // seccomp: after this only these system calls are allowed; anything else kills the helper. (Sanitizer runtimes
  // need more to report, so sanitizer builds rely on the rlimits.)
  // prctl(2) is variadic by definition.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {  // NOLINT(cppcoreguidelines-pro-type-vararg)
    return false;
  }
#if defined(__x86_64__)
  constexpr std::uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
  constexpr std::uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
  return ok;  // no filter for this architecture (the rlimits still apply)
#endif
  // libc++abi's guard for function-local statics asks for the thread id (gettid); glibc's abort path asks for the
  // process id. Neither reaches anything outside the helper.
  constexpr std::array<long, 16> kAllowed = {
      SYS_read,   SYS_write,   SYS_close,    SYS_exit,  SYS_exit_group,   SYS_brk,           SYS_mmap,   SYS_munmap,
      SYS_mremap, SYS_madvise, SYS_mprotect, SYS_futex, SYS_rt_sigreturn, SYS_clock_gettime, SYS_gettid, SYS_getpid};
  constexpr auto ld = [](std::uint32_t k) { return sock_filter{BPF_LD | BPF_W | BPF_ABS, 0, 0, k}; };
  std::vector<sock_filter> prog;
  prog.push_back(ld(offsetof(seccomp_data, arch)));
  prog.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 1, 0, kArch});
  prog.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS});
  prog.push_back(ld(offsetof(seccomp_data, nr)));
  for (const long nr : kAllowed) {
    prog.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<std::uint32_t>(nr)});
    prog.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW});
  }
  prog.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS});
  sock_fprog fprog{static_cast<unsigned short>(prog.size()), prog.data()};
  ok = ok &&
       ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0) == 0;  // NOLINT(cppcoreguidelines-pro-type-vararg)
#endif
  return ok;
}

#else  // Windows (job object + restricted token) and macOS (sandbox profile) come with those builds (docs/gaps.md).

base::Result<base::Value> run_helper(const std::filesystem::path& /*helper*/, std::string_view /*format*/,
                                     std::span<const std::byte> /*bytes*/, const Limits& /*limits*/) {
  return fail(Errc::unsupported, "out-of-process import is not built for this platform yet");
}

bool lock_down_helper(const Limits& /*limits*/) {
  return true;
}

#endif

}  // namespace cc::importer
