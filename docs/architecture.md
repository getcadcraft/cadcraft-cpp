# Architecture

Last updated 2026-10-16 · Change: first version (M1: toolchain, build, gate).

## Toolchain (pinned)

| Tool | Version | Where (this server) | Notes |
|---|---|---|---|
| Clang / clang-tidy / clang-format / lld / libc++ | 22.1.8 | `/usr/lib/llvm-22` (apt.llvm.org `llvm-toolchain-jammy-22`) | side by side; the system `clang` stays 14 |
| CMake | 4.4.4 | `/opt/cadcraft-cpp/cmake-4.4.4` | Kitware release tarball, SHA-256 checked |
| Ninja | 1.13.2 | `/opt/cadcraft-cpp/ninja-1.13.2` | |
| vcpkg | tag 2026.07.29 = baseline `9e593bb18ea69cc5095e012465dcd675a822ed0d` | `/opt/cadcraft-cpp/vcpkg` | manifest mode; the baseline is in `vcpkg.json` and `vcpkg-configuration.json` |
| Emscripten SDK | 6.0.10 | `/opt/cadcraft-cpp/emsdk` | |
| osv-scanner | 2.7.0 | `/opt/cadcraft-cpp/osv-2.7.0` | not usable on vcpkg (see "Gate", osv) |
| GCC (CI job only) | 14 | GitHub `ubuntu-24.04` image | the server has GCC 11 only; installing a newer libstdc++ system-wide would touch other sites |
| MSVC (CI job only) | 19.4x | GitHub `windows-2022` image | |
| Node (reference runs) | 24.19.0 | system | runs the 0.5 JS build for parity and timings |

`. tools/env.sh` puts these first on PATH for the current shell; nothing else on the server changes.

## Build

- `cc_library(name LAYER n …)` (cmake/CcLibrary.cmake) is the only way a library enters the build; configure fails
  when a library links a library of the same or a higher layer, or a UI toolkit below layer 6. `tools/layers.cmake`
  also greps L0–L5 sources for SDL/ImGui/WebGPU includes.
- cmake/Policy.cmake gives every target of ours: the warning set as errors, the hardened standard library
  (libc++ `_LIBCPP_HARDENING_MODE` FAST, EXTENSIVE in Debug; libstdc++ `_GLIBCXX_ASSERTIONS`; MSVC
  `_MSVC_STL_HARDENING`), compiler hardening, and the preset's sanitizers or fuzzing instrumentation.
- vcpkg triplets in `triplets/`: `x64-linux-cc` (clang + libc++ + lld, so dependencies share our standard library),
  `x64-linux-gcc`, `wasm32-emscripten-cc` (wasm exceptions, threads, SIMD). Dependencies are built release-only, so a
  Debug build of ours links -O2 dependencies.
- Presets: dev, release, asan (ASan + UBSan), ubsan, tsan, fuzz, ci, gcc, msvc, wasm. Each has a workflow preset.

## Gate

`cmake --workflow --preset ci` = Release build with `-Werror`, the unit tests, and these ctest entries (label `gate`;
`heavy` ones build another preset):

| Check | Script |
|---|---|
| gate.format | `tools/format.cmake` — clang-format `--dry-run --Werror` on every source |
| gate.layers | `tools/layers.cmake` |
| gate.tidy | `tools/tidy.cmake` — clang-tidy on files changed against origin/main (`CC_TIDY_ALL=1`: all) |
| gate.assets | `tools/assets.cmake` |
| gate.licenses | `tools/licenses.cmake` — each port's SPDX licence from vcpkg's SBOM against the allowlist |
| gate.osv | `tools/osv.cmake` — osv-scanner cannot read vcpkg (its SBOM purls are `pkg:vcpkg`, which OSV does not index), so the script asks api.osv.dev, the same database, by each port's upstream repository and tag (ecosystem GIT); a hit fails unless waived in docs/security-waivers.md |
| gate.identity | `tools/identity.cmake` — every commit by CadCraft, no assistant attribution |
| gate.asan, gate.tsan, gate.wasm, gate.fuzz (heavy) | `cmake --workflow --preset <p>` |
| gate.fuzz-smoke (heavy) | `tools/fuzz_smoke.cmake` — each libFuzzer target 30 s on a copy of its corpus |

CI (`.github/workflows/ci.yml`) runs the same pieces as parallel jobs, plus GCC and MSVC builds.

## Errors

Libraries return `std::expected<T, Error>` at their public boundary. **One deliberate internal exception:** the bit
readers (`cc::base::BitReader`, and the DWG decoder built on it) throw `ReadOverrun` on a read past the end. The
decoder catches it at each record boundary and turns it into a note on that record, exactly as cc-dwg's
`DwgReadError` does in the JS reference — so a damaged record keeps the same fields it has in the JS model, which
the parity gate compares. It never crosses a library's public API.

## Locks

None yet. Every lock gets a name and a place in this order when it is added (AGENTS.md section 6, Threads).
