# Session log

Last updated 2026-10-16 · Change: M2.3–M2.4.

## 2026-10-16 (session fork-cpp)

- Read CPP-AGENTS.md (§ 15 and the owner decisions in docs/forks/README.md win) and the cc-dwg reader (`lib/dwg`).
- Toolchain installed without touching the system compilers: LLVM 22.1.8 from apt.llvm.org (side by side;
  `clang` is still 14), CMake 4.4.4, Ninja 1.13.2, vcpkg tag 2026.07.29, emsdk 6.0.10, osv-scanner 2.7.0 under
  `/opt/cadcraft-cpp`. `tools/env.sh` puts them first on PATH for one shell.
- forks-infra created github.com/getcadcraft/cadcraft-cpp (first commit: LICENSE, README, AGENTS.md); this
  session commits on top.
- M1: cc_library with the layer check, Policy.cmake (warnings, hardened libc++, compiler hardening, sanitizers),
  presets, triplets, gate scripts, CI workflow.
- M1 committed (c8632ce) and pushed; CI: GCC 14, TSan, wasm green on GitHub.
- M2.1: io-dwg — the cc-dwg reader ported module by module (bits, container + LZ + Reed–Solomon, header variables,
  objects, solids/surfaces/data storage, passthrough, the model builder; tables, multileaders and dynamic blocks ported
  by three parallel agents). Parity kit: **78 of 78 corpus files match the JS model, no known differences.** First
  benchmark 1.0–3.9× faster than JS (the generic Value tree dominates the profile; see ROADMAP.md).
- forks-infra froze the JS reference at testfiles/fork-parity/js-0.5 (commit 594ab3e); read engines and app code there.
- M2.2: the out-of-process importer (apps/cadcraft-importer + libs/importer: rlimits, seccomp filter, wall clock,
  output cap) with crash/hang/garbage tests; `cadcraft-cli info|convert --isolated`; base/json.hpp (bounded parser,
  fuzzed with a round-trip property); golden references for the public fixtures (tests/golden) checked in CI; gate
  fixes (tidy also checks files not committed yet). All 72 corpus DWGs parse inside the seccomp filter.
- M2.3: a first fuzz hour found undefined behaviour (double → int64 in the R2007 page map); `base::clamp_cast`, modular
  numbers and the object map kept as doubles like the JS; regression inputs in fuzz/corpus/dwg. M2.3a: the JSON parser
  refuses invalid UTF-8 (found by fuzz_json). Parity still 78/78.
- Long fuzz run (fuzz_dwg 30 min × 3 workers, fuzz_json 10 min, 9.6 M runs clean): three more DWG inputs —
  JS `(csize + 7) & ~7` cast through uint64 (now `base::js_to_int32`, JS's ToInt32) and `append_number` reading past
  to_chars' "inf" (now prints `NaN` / `Infinity` like String()). Regression inputs + unit tests. On one of them the
  JS decodes an object whose size is NaN and the C++ refuses it: only a note's wording differs (docs/gaps.md).
- M2.4: the JS reader unit tests ported (bits_test: spec ch. 2 worked examples, CRC, never-crash on truncation).
- M3.1: libs/io-dxf, BimCraft's dxf-model.js ported (JS parseFloat/parseInt/trim/caret semantics, BimCraft's
  UTF-8-else-1252 decode). Own parity script `tools/parity/dxf.mjs` (the kit has no DXF engine; same canon/diff and
  tolerance): **104 of 104 corpus DXFs match** (dwg-corpus `_truth` exports, apparel patterns, public fixtures) plus
  the synthetic fuzz seeds; golden test on the public fixtures; fuzz_dxf. `cadcraft-cli parity|bench dxf`.
- M3.1a: GitHub CI had been red since M2.3 (missed it: check `gh run list` after every push). MSVC refuses `getenv`
  (C4996) → `base::env_var` (`_dupenv_s` there); GCC 14 flagged an int → size_t conversion clang let through; ASan:
  the importer's NPROC 0 cap binds non-root users and LeakSanitizer clones a thread at exit, so sanitizer builds skip
  it as they skip the memory cap and seccomp (reproduced locally as `nobody`).
- M3.2: libs/drawing, BimCraft's dwg-compile.js (draw lists: spaces → batches of polylines, fills, texts; xrefs,
  multileaders, images, viewports) as typed C++. `tools/parity/compile.mjs` (the Rust fork's cdiff.mjs serialisation
  and tolerance): **202 of 202 corpus files match, all also at rel 1e-12**; golden test on the public fixtures;
  `cadcraft-cli compile <file> --json | --bench N`. Compile is 1.3–1.8× the JS (a few ms; the read dominates).
- Differential fuzzing: every input the DXF/compile fuzzers kept is run through the JS and the C++ (2728 inputs).
  It found what the corpus never shows: casts where the JS does ToInt32, the JS's `|| 'DIMENSION_LINEAR'`, TypeErrors
  the JS throws (mirrored with its words), `String(object)`, full-Unicode toUpperCase (tables generated from Node:
  tools/gen/js-case.mjs) and ICU's collation for every Windows-1252 character (tools/gen/js-collation.mjs). All 2728
  match now. Compile fuzz: a spline degree its control points cannot carry (JS throws; C++ draws the polygon) and a
  MINSERT of 10⁹ cells (the JS runs out of memory; C++ stops at `max_coords` and marks it truncated).
- M3.3: the 2D viewer. libs/text (FreeType, Archivo variable font at the condensed width, SDF atlas grown on demand;
  bitmap-to-SDF: 4× faster than FreeType's outline SDF), libs/render (dwg-view.js colours, fit/zoom/pan, viewport
  matrices, per-batch origin-relative float geometry for survey coordinates), libs/ui-imgui (WebGPU: instanced
  segment quads, even-odd stencil fills, SDF text with 4-sample coverage, 4× MSAA; ImGui chrome in the site's tokens;
  menu, space tabs, layers panel, status bar; 0.5 lang catalogs, keys unchanged), libs/engine (open: DWG/DXF by bytes,
  xrefs beside the file), libs/i18n, libs/image (PNG). Apps: cadcraft (desktop, SDL3 + Dawn), cadcraft-shot (headless
  PNG + --bench), cadcraft-web (Emscripten + emdawnwebgpu, ?sample, ?bench, file picker and drops as EM_JS glue, no
  inline script). Checked by eye against BimCraft's own dwg-view.js on the same sheet (headless Chromium): geometry,
  viewports, clips, fills and colours match; text differs in face (the page falls back to Arial).
- Dependencies: SDL3 and Dawn without default features (dbus → libsystemd is LGPL, D3D12 → DXC is NCSA); Dear ImGui
  fetched by commit (its vcpkg bindings would pull those defaults back in). Each triplet now has its own vcpkg
  installed tree (a shared one let one preset's install remove another's packages); the wasm triplet chain-loads
  cmake/toolchains/emscripten-cc.cmake so its C flags (threads, wasm longjmp) reach the dependencies at all.
- Timings (quiet machine, release): open → first frame in the browser equals the JS (C++ 192–201 ms cold, 58–87 warm;
  JS 188–209 / 43–88) — the generic Value tree and one thread cost wasm the reader's native 2–4× lead. Native: parse
  15–72 ms, compile 3–5, scene 19–21, CPU per frame < 0.02 ms (GPU time on lavapipe is a CPU rasteriser: not a GPU
  number).
- M3.3a: text was upside down in the browser (Chrome's software WebGPU) and right natively: the text shader turned
  glyphs by atan2(−dy, dx) and took its cos/sin, and a software atan2 returned π for −0. It now uses the normalised
  baseline direction itself (exact, no trig round trip). Found by checking the deployed site in headless Chromium.
- M3.5a: DWG objects decode on up to 8 threads (decoders only read the bytes and the immutable context; results go
  to their map slot, so the model is byte-identical: tested, kit 78/78). Kitchen DWG best 34.8 → 25.1 ms on the
  loaded server (median 40.0 → 33.4). The import helper and the fuzzer stay on one thread. Smaller field reservation
  per record (32 → 12): ~6 %. Tried and dropped: mimalloc in the web build and jemalloc natively (within noise /
  6–8 %): the cost is the model's shape, not the allocator.
  The gate's ASan run caught a use-after-scope in the first version (a worker read a local that the block destroyed
  before joining the threads); fixed by capturing by value. Hardware never hit the window: the sanitizer did.
