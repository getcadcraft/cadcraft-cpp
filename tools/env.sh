# shellcheck shell=bash
# Source this file (`. tools/env.sh`) to put the pinned toolchain first on PATH for this shell only.
# It changes nothing system-wide. Paths can be overridden before sourcing.
#
#   CC_LLVM_ROOT   LLVM 22 (apt.llvm.org packages install it here side by side with the system clang)
#   CC_TOOLS       CMake, Ninja, vcpkg, emsdk, osv-scanner (see docs/architecture.md, "Toolchain")
: "${CC_LLVM_ROOT:=/usr/lib/llvm-22}"
: "${CC_TOOLS:=/opt/cadcraft-cpp}"
export VCPKG_ROOT="${VCPKG_ROOT:-$CC_TOOLS/vcpkg}"
export EMSDK="${EMSDK:-$CC_TOOLS/emsdk}"
export VCPKG_DISABLE_METRICS=1
export PATH="$CC_LLVM_ROOT/bin:$CC_TOOLS/cmake-4.4.4/bin:$CC_TOOLS/ninja-1.13.2:$CC_TOOLS/osv-2.7.0:$VCPKG_ROOT:$PATH"
if [ -f "$EMSDK/emsdk_env.sh" ]; then
  # emsdk_env.sh prints its own PATH changes; keep the shell quiet, and keep our clang first.
  EMSDK_QUIET=1 . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
  export PATH="$CC_LLVM_ROOT/bin:$PATH"
fi
[ -x "$CC_TOOLS/osv-2.7.0/osv-scanner_linux_amd64" ] && [ ! -e "$CC_TOOLS/osv-2.7.0/osv-scanner" ] \
  && ln -s osv-scanner_linux_amd64 "$CC_TOOLS/osv-2.7.0/osv-scanner"
true
