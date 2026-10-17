# Session log

Last updated 2026-10-17 · Change: M3.5b–d.

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
- M3.5 (measured, dropped): converting the builder's entities on threads (fields copied, or moved) gave nothing
  (29–36 ms vs 30 ms, load 10–14): the conversion is allocation-bound and threads contend in the allocator. Parallel
  decode (M3.5a) stays. The next lever is the model's shape (fewer, smaller allocations), not more threads.
- M3.5b: web deploy folder with content-hashed names (`cmake --build --preset wasm --target web-dist` →
  build/wasm/web-dist via tools/web-dist.cmake): cadcraft-web-<12 hex>.js/.wasm/.data and cadcraft-<hex>.css, served
  `immutable` for a year by the test host; index.html stays `no-cache`. Deployed 2026-10-16 and checked live.
- M3.5c: first frame. The web build compiles the four render pipelines in the background
  (`CreateRenderPipelineAsync`; files wait in the page until they exist, so a first frame never draws nothing), the
  viewer makes the printable ASCII glyphs (+ ° ± ⌀) when it starts, and `?sample` fetches the drawing while the WebGPU
  device is still on its way. `?bench` now reports open → first frame as soon as the six opens are done (a software
  GPU can take minutes for the 240 zoom frames after). Timing not taken: load 21–37 from other sessions' builds made
  SwiftShader frames 1–4 s; re-measure on a quiet machine.
- M3.5d: touch. Pinch zooms about the fingers' midpoint (≤ 2× per event), two fingers pan, one finger drags through
  the mouse SDL makes of it, a double tap (≤ 300 ms taps, ≤ 400 ms apart, ≤ 30 px) fits. The first finger switches a
  touch layout on (40 px rows from the font size, wider panel and scroll bars, 48 px tabs, 40 px menu rows); a real
  mouse moving switches it off. The canvas has `touch-action: none`. Test: libs/ui-imgui/tests/viewer_touch_test.cpp
  (Dawn null adapter, ImGui without a window, SDL finger events by hand); checked in headless Chromium with CDP touch.
- M3.5c measured (2026-10-17, quiet machine, headless Chromium + SwiftShader, kitchen DXF): open → first frame cold
  148–217 ms (before 192–201), warm 64–109 ms (before 58–87); the scene step is ~1 ms now (glyphs made at start); the
  DXF parse (50–165 ms in the browser vs ~26 ms native) is what is left.
- M3.5e: fewer copies and allocations in the readers, measured before the typed-model redesign.
  Profile (perf, frame-pointer build; cmd_bench's own model teardown is outside the timer and was excluded): inside the
  read, `Value` copies and the destruction of their originals were ~15–20 %; DWG spends ~13 % freeing its decoded records.
  Kept: `make_object` / the DXF `spread` take `base::Init` (a mutable value, moved out of the initializer_list instead of
  copied); a DXF entity object is created with room for 32 members (it grew 1→2→…→32: six allocations per entity);
  the DXF line list is reserved. Kitchen files, interleaved A/B (14 rounds × 7 runs): DXF −6.5 % median (−6.4 % best),
  allocations 320k → 219k, bytes 136 → 85 MB per read; DWG −7.9 % median (−9.2 % best). Models byte-identical to
  before on the whole corpus (104 DXF, 110 DWG).
  Dropped after measuring: freeing the DWG records later (leaking them as the upper bound made the read 1.6× *slower*:
  the allocator reuses what was just freed), `spread` reserving per call (+10 %: most spreads overwrite keys),
  records' field objects starting at 24 members (noise).
  What the numbers say about the typed-model redesign: points (112/168-byte objects) are 21 % (DWG) / 35 % (DXF) of
  the remaining allocations; removing 22 % of allocations above bought ~6 %, and 24 MB fewer bytes bought nothing. Inline
  points would be worth roughly 4–10 %, against changing ~1,700 `set` call sites under the parity gate.
- M3.5f: compact points. `Value` holds {x, y} / {x, y, z} of numbers inline (two new variant alternatives, still 32
  bytes; is_object() is true, JSON and == treat them as the plain object). pt2/pt3 and make_object({x, y[, z]}) make
  them; x/y/z read with `get()` (by value); find / operator[] / a const as_object() on one throw std::logic_error, so a
  generic walker that meets a point fails loudly (the corpus check found two: table.cpp `deep`, dynblock `usable_point`).
  Allocations per read: DXF 219k → 132k (−40 %), DWG 244k → 194k (−20 %). Time, interleaved A/B (12 × 7 runs, kitchen):
  best −10 % (DXF) / −16 % (DWG), medians within noise (±3 %). Models and draw lists byte-identical on the corpus
  (104 DXF, 110 DWG).
- M3.5g: the members a fresh entity object gets first (DXF `entity`, DWG `convert` up to colorBook) are appended, not
  searched with `set` (the keys are new and distinct). Kitchen A/B (12 × 7): DXF −3.3 % best / −3.8 % median, DWG −3.0 % /
  −5.5 %. Models and draw lists byte-identical on the corpus.
- M3.5 keys: interned or compact (< 24-byte) member keys are not done. File data become keys (dictionary entry names,
  xdata app names, class names), so a process-wide intern table grows without bound on hostile files (AGENTS.md §4: cap
  every allocation derived from input), and every sub-24-byte key layout needs a tagged pointer, a union or an owning raw
  pointer, all ruled out by §4. The saving would be bytes (Member 56 → 40–48), and 24 MB fewer bytes per read measured
  0 % (M3.5e); the time cost of keys was the `set` search, done in M3.5g.
- M4.1: `libs/cad` (L2): `cc::cad::import_dwg`, CadCraft 0.5's dwg-import.js and the pieces of eight other modules it
  calls, ported function for function on base::Value; `cadcraft-cli import <file.dwg>` prints the drawing.
  **Parity: 110 / 110 corpus DWGs give the JS importDwg's drawing** (tools/parity/import.mjs; golden test on the four
  public fixtures, tests/golden/js-0.5-import.tar.gz). On the way: (1) Math.atan2 is V8's fdlibm code, which differs from
  the C library's in the last bit on 15 % of inputs — here a dimension angle became 2π in the JS and 0 in C++, so
  `base::js_atan2` is V8's algorithm (bit-identical with Node on 2,000,000 pairs; THIRD_PARTY.md); (2) JS objects list
  array-index keys ("0", "1"…) first in ascending order — `base::js_members` gives that order wherever a port iterates
  keys from a file (dictionary entries decided the order of dynSrc.objects); (3) a dynamic block's `dyn.all` shares its
  objects with `entities` in the JS, so attribute resolution shows in both — mirrored after resolveAttribs; (4) `at()` /
  `item()` of a temporary return by value (a reference into g()'s copy dangled). The import does not cap anything the
  JS does not, except MINSERT copies (docs/gaps.md).
- M4.2: `libs/doc` (L3): `cc::doc::Doc`, CadCraft 0.5's doc.js — spaces as insertion-ordered maps (JS Map semantics:
  a deleted entry set again goes last), every edit through `change()` as one undo step (nested changes join the outer
  one; an exception rolls the change back), mark / squash, undo / redo of entities, blocks, layouts, meta, views, the
  layer table and named styles, a layer renamed with its objects in one step, and `load()`. `cadcraft-cli doc <file>`
  prints the loaded document. **Parity: 110 / 110 corpus DWGs load into the JS Doc's document** (tools/parity/doc.mjs;
  golden on the public fixtures). Not ported yet: constraints (the file's payload is kept as text), the 3D model (its
  faces and JSON are kept), Studio Live's hook, add-on edit hooks (docs/gaps.md).
- M4.3: the DWG writer, `cc::dwg::write_dwg` (libs/io-dwg/src/w*.cpp, ~13k lines): cc-dwg's write.js and its modules
  (bits-write, crc, lz-r2004-write, r2007-write with Reed–Solomon, container-write, objects-write, nongraphic-write with
  the raw passthrough and proxies, mleader-write, table-write, solid-write, surface-write, acis, acds). **Parity: 648 /
  648 writes byte-identical** to the JS writer (the 108 readable corpus DWGs × R2000, 2004, 2007, 2010, 2013, 2018; same
  date and the same seeded random bytes for the GUIDs: tools/parity/dwg-write.mjs), the reports equal too. Golden: the
  JS sizes, CRC-32s and reports for the public fixtures × six versions; tests for the compressors against the reader's
  decompressors, read-back of written files, an empty model; fuzz target `dwg_write` (read hostile bytes → write → read).
  `cadcraft-cli dwg-write <in> <version> <out>`.
