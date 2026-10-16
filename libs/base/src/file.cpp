// SPDX-License-Identifier: Apache-2.0
#include "cc/base/file.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cc::base {

namespace fs = std::filesystem;

Result<std::vector<std::byte>> read_file(const fs::path& path, std::uint64_t max_bytes) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec) {
    return fail(Errc::io, "cannot read the file size: " + ec.message());
  }
  if (size > max_bytes) {
    return fail(Errc::too_big, "the file is larger than the " + std::to_string(max_bytes) + "-byte limit");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(Errc::io, "cannot open the file");
  }
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  if (size != 0) {
    // std::istream::read takes char*; the cast is the one place a byte buffer meets the stream API.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (std::cmp_not_equal(in.gcount(), size)) {
      return fail(Errc::io, "the file changed while it was read");
    }
  }
  return data;
}

namespace {

std::string temp_suffix() {
  std::random_device rd;
  std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFFU);
  constexpr int kHexWidth = 6;
  std::string s(kHexWidth, '0');
  unsigned v = dist(rd);
  for (int i = kHexWidth - 1; i >= 0; --i) {
    s[static_cast<std::size_t>(i)] = "0123456789abcdef"[v & 15U];
    v >>= 4U;
  }
  return ".tmp-" + s;
}

}  // namespace

#if defined(_WIN32)

Result<void> write_file_atomic(const fs::path& path, std::span<const std::byte> data) {
  const fs::path tmp = fs::path(path).concat(temp_suffix());
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return fail(Errc::io, "cannot create the temporary file");
  }
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1U << 30U));
    DWORD wrote = 0;
    ok = WriteFile(h, data.subspan(done).data(), chunk, &wrote, nullptr) != 0;
    done += wrote;
  }
  ok = ok && FlushFileBuffers(h) != 0;
  CloseHandle(h);
  if (!ok || MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::error_code ec;
    fs::remove(tmp, ec);
    return fail(Errc::io, "cannot write the file");
  }
  return {};
}

#elif defined(__EMSCRIPTEN__)

Result<void> write_file_atomic(const fs::path& path, std::span<const std::byte> data) {
  const fs::path tmp = fs::path(path).concat(temp_suffix());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out.flush()) {
      return fail(Errc::io, "cannot write the file");
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    return fail(Errc::io, "cannot replace the file: " + ec.message());
  }
  return {};
}

#else

namespace {

class Fd {
 public:
  explicit Fd(int fd) noexcept : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&&) = delete;
  Fd& operator=(Fd&&) = delete;
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  int release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
  }

 private:
  int fd_;
};

}  // namespace

Result<void> write_file_atomic(const fs::path& path, std::span<const std::byte> data) {
  const fs::path tmp = fs::path(path).concat(temp_suffix());
  constexpr mode_t kMode = 0644;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — open(2) is variadic by definition.
  Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, kMode));
  if (fd.get() < 0) {
    return fail(Errc::io, "cannot create the temporary file");
  }
  std::size_t done = 0;
  while (done < data.size()) {
    const auto rest = data.subspan(done);
    const ssize_t n = ::write(fd.get(), rest.data(), rest.size());
    if (n < 0) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return fail(Errc::io, "cannot write the temporary file");
    }
    done += static_cast<std::size_t>(n);
  }
  if (::fsync(fd.get()) != 0 || ::close(fd.release()) != 0) {
    std::error_code ec;
    fs::remove(tmp, ec);
    return fail(Errc::io, "cannot flush the temporary file");
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fail(Errc::io, "cannot replace the file");
  }
  fs::path dir = path.parent_path();
  if (dir.empty()) {
    dir = ".";
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  Fd dfd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dfd.get() >= 0) {
    ::fsync(dfd.get());
  }
  return {};
}

#endif

}  // namespace cc::base
