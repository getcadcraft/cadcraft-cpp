# Session log

Last updated 2026-10-20 · Change: M5.8a (TTR / TTT, TEXTMASK, XCLIP, the settings commands, HELP).

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
- M4.3a: writer speed (release, best of 7, same model, load ~5): example-2007 R2000 7.8 ms vs JS 11.7, R2004 11.1 vs
  19.7, R2007 15.3 vs 43.7, R2018 16.1 vs 30.0; civil-example 9.5–16.4 vs 14.6–28.9; blocks-and-tables (thousands of
  blocks) R2000 76 → 63.5 vs 71 after the per-record std::regex tests became plain string tests (regex was 12 % of the
  profile). 1.1–2.9× the JS; no other change made (the gains left are Value copies in the records, as in the reader).
- M4.3a: the writer on MSVC (a one-element `Array{Array{…}}` flattens there and nests on clang/GCC: `base::array_of`)
  and GCC 11's -Wshadow; CI green on every job.

## 2026-10-18 (session fork-cpp)

- M4.4: saving a drawing as DWG — `libs/save` (L4) `cc::save::save_dwg`, CadCraft 0.5's dwg-save.js and what it calls:
  libs/cad gained geom.hpp (font.js widths, textLayout, bounds, attribute texts, leaders), dims.hpp (dims.js +
  dimstyle.js + units.js formatLength) and save_parts.hpp (the dwg-props.js write side, colours, hatch patterns,
  dwg-vports.js, pictures.js, xclip.js); the dynamic-block save (dyn-blocks.js dynSave) is in libs/save; `libs/m3d`
  (L2) is the first slice of the ModelCraft kernel (definitions, faces, toJSON, earcut faceTris, meshDef) and
  model3d.js's fromFile / forFile3d — the Doc now builds its 3D model at load as the JS does. `cadcraft-cli dwg-save`.
  **Parity: 648 / 648 saves byte-identical to the JS** (the 108 readable corpus DWGs opened as the app opens them,
  saved in all six versions; tools/parity/dwg-save.mjs, which also compares the writer models with `--model`), the
  reports equal too; golden test on the public fixtures (24 saves, read back); fuzz target `dwg_save` (1.75 M runs
  clean). What byte parity took: (1) Node's Math.sin / Math.cos are fdlibm's, not the C library's (a dimension's
  measurement differed in the last bit) — `base::js_sin` / `js_cos` are V8's, argument reduction included, bit for bit
  against Node on 3,000,000 inputs; (2) Math.hypot is V8's own Kahan sum — `base::js_hypot`; (3) toFixed rounds an
  exact half away from zero (printf rounds to even) — `base::js_to_fixed`; (4) the writer's `+v || 0` turns -0 into 0
  (a sheet's corner at -0 was written as -0); (5) `-ffp-contract=off` everywhere (GCC fuses a*b+c by default where
  FMA exists). All earlier parities re-checked after the trig change: import 110/110, doc 110/110, writer 648/648,
  DXF 104/104, draw lists 214/214. Speed 0.84–1.61× the JS after removing a quadratic copy in the writer's attributes
  and a block copy per reference (docs/perf-postmortem.md). Not ported (docs/gaps.md): the constraints payload, reading
  CadCraft's own 3D model back, re-writing edited visibility states.
- M4.4a: GCC 14 builds again (`std::min({…})` needed `<algorithm>`; -Wnull-dereference on unchecked vertex lookups in
  m3d, now `Def::vert`, which refuses a missing vertex like `Def::points`).
- M4.5: saving as DXF — `cc::save::write_dxf` (dxf.js writeDxf, R12) and `write_dxf2000` (dxf2000.js, R2000, built on
  the DWG writer model: `save_dwg` with `model_only` + `explode_leaders`); `cadcraft-cli dxf-save <in> r12|2000 <out>`.
  **Parity: 216 / 216 saves byte-identical to the JS** (the 108 readable corpus DWGs opened as the app opens them, both
  formats; tools/parity/dxf-save.mjs), reports equal; golden test on the public fixtures (8 saves); fuzz target
  `dwg_save` now saves DXF too (171,947 runs clean). What byte parity took: `Number(n.toPrecision(15))` with an exact
  half rounded away from zero, and on the way a bug in `base::js_to_exponential` (an exact half with a positive
  exponent lost the exponent: `from_chars` takes no `+`), fixed with a regression test; DWG save re-checked after it.  Speed (JS and C++ alternating, machine under load): R12 1.35–5.89× the JS, R2000 0.93–1.89× (it builds the DWG writer
  model first); R2000 read containers by copy at first (civil-example 11 ms → 2.4 ms after reading them in place;
  docs/perf-postmortem.md).
- M4.6a: the command line — `libs/cmd` (L5): commands.js's Engine (typed points, lengths with fractions and feet and
  inches, options, defaults, the prompt line, Enter-to-repeat, noun-verb selection with ALL / L / P / W / C / R / A / ?)
  with commands as C++20 coroutines; Esc destroys the waiting command's coroutine frames (no exception). 17 commands:
  LINE PLINE CIRCLE XLINE RAY ARC RECTANG POLYGON TEXT ERASE MOVE COPY ROTATE SCALE MIRROR UNDO REDO; geom.js txEntity in
  libs/cad. `cadcraft-cli script` runs command scripts headless; tools/parity/commands.mjs runs the same scripts through
  the frozen 0.5 Engine with the same stub host. **28 / 28 scripts the same** (everything printed and prompted, the model,
  the current settings, next id, undo and redo labels); golden test; fuzz target `cmd_script` (929,479 runs clean).
- M4.6b: OFFSET (by distance or through a point; lines, arcs, circles, open and closed polylines with bulges), TRIM
  (Quick and Standard mode, a pick, a fence, two clicks on nothing, a crossing window, EDGEMODE, erase, undo) and EXTEND
  (lines, arcs, polyline ends; boundaries; fence) — geom.js's editing geometry in `libs/cad` edit.hpp (primitives and
  their intersections, offsetEntity, trimSplit / pieceAt / pieceEntity / trimResult, pathCrossings, extendEntity,
  extensionPart, inWindow; geom.cpp's primitives now shared through src/prims.hpp, a bulged arc knows it runs clockwise).
  **45 / 45 command scripts the same as the 0.5 Engine.** Counts printed through the catalog's plural forms (`tn`), as
  the JS's t() picks them.
- M4.7a: what a document draws — `libs/docview` (L4): walk.js with doc.js resolveColor, linetypes.js resolveLt /
  resolveLw / dashPrims (patternOf now `doc::pattern_of`), hatch.js hatchLines, walk.js polyBands and maskLoop;
  `cadcraft-cli walk`; tools/parity/walk.mjs compares every event (colour, lineweight, alpha, the dashed primitives,
  fills, placed texts, points, viewports, masks, construction lines) with the frozen 0.5 walk: **108 / 108 corpus
  drawings draw the same**; golden test on the public fixtures.
- M4.7b: drafting in the viewer — a DWG opens as the CadCraft document (import → Doc) and is drawn through walk
  (`docview::to_drawing`: strokes as polylines, fills, each text line at its baseline, sheets with their viewports),
  rebuilt after every change; the command line under the canvas (the prompt, typed lines, Enter, Space, a right click
  as Enter, Esc, Ctrl+Z / Ctrl+Y), a click answers the waiting prompt, the rubber band and the selection drawn over the
  view (ImGui draw list), noun-verb picks, layers turned on and off in the document as an undo step. Checked on the
  desktop build under Xvfb: LINE by clicks, CIRCLE by a click and a typed radius, RECTANG, Esc, undo.
- M4.7c: geom.js snapPoints / tangentFrom / grips / gripMove and view.js snap in `libs/cad` snap.hpp (primitives carry
  geom.js's `box` flag so text boxes and arrowheads are not snapped to); the viewer snaps every point prompt (end, mid,
  centre, quadrant, intersection, perpendicular, node, insertion by default; F3), ortho (F8), selection windows by a
  left drag (left → right inside, right → left crossing; idle or at Select objects), grips on the selection (click a
  grip, click where it goes: one undo step) with the snap marker and the hot grip drawn. Checked under Xvfb.  The gate's fuzz smoke found undefined behaviour in the script runner (a `pick` of 1.44e23 cast to an id): ids from
  scripts are now range-checked; the input is a corpus entry and a regression test.
- M4.7d: Save and Save as in the viewer, as cadcraft.js saveFile / saveAs: the drawing's own format and DWG version
  (or the last chosen), written in place (atomic write) on the desktop when it came from a file of that kind, else the
  save dialog (`Services::pick_save`, SDL's dialog; the path comes back through the inbox); on the web a download
  (`Services::download`, EM_JS glue). Save as: DWG 2000–2018, DXF 2000, DXF R12. The report's notes go to the command
  history. Checked under Xvfb: a circle drawn, Ctrl+S, the file reopened by the CLI with the circle in it. **M4 done.**
- M4.7e: a drawing opened in the viewer takes its file's name (meta.name, meta.source, as cadcraft.js does), so Save
  offers "plan.dwg", not "Drawing1.dwg". Web build deployed to cpp.getcadcraft.com and checked in headless Chromium:
  open a DWG, draw, Ctrl+S downloads the DWG with the new objects in it.
- M4.7f: GCC 14's -Wnull-dereference false positive on `vector::insert(begin, {x, y})` in walk.js polyBands (CI's
  GCC job red since M4.7c): the band's right side is collected and appended reversed instead (same ring).
- M5 planned (docs/roadmap.md § M5 plan): CadCraft complete before the other apps, in command batches.
- M5.1a: ELLIPSE, POINT, DUPLICATE, SELECTALL, DESELECT, STRETCH, BREAK, BREAKATPOINT, LENGTHEN, JOIN, EXPLODE
  (libs/cmd/src/reshape.cpp) on geom.js's paramOn, breakEntity, curveLength, lengthenEntity, joinEntities,
  stretchEntity, explodeEntity (libs/cad/src/reshape.cpp). Command parity 66/66 scripts (21 new in
  tests/commands/reshape.json: every branch, error and option of each command); goldens regenerated.
- M5.1b: FILLET and CHAMFER (libs/cmd/src/corner.cpp) on geom.js cornerTarget, filletPair, chamferPair, polyCorners
  (libs/cad/src/corners.cpp): lines, arcs, circles, polyline segments, parallel lines (half circle), shift = sharp
  corner, P every corner, M, U, T trim mode, D/A/E chamfer settings. Command parity 74/74 (8 new scripts in
  tests/commands/corners.json).
- M5.1c: ARRAY / ARRAYRECT / ARRAYPOLAR, REVCLOUD (geom.js pathOf, resamplePath, revcloud: libs/cad/src/cloud.cpp),
  MATCHPROP, MTEXT, MLEADER, TEXTEDIT (libs/cmd/src/annotate.cpp; text typed at the command line, as the JS without
  an in-place editor). Command parity 82/82 (8 new scripts in tests/commands/annotate.json).
- M5.1d: HATCH (libs/cmd/src/hatch.cpp) on hatch.js loopsOf and boundaryAt (libs/cad/src/boundary.cpp: the planar
  graph with the JS's grid hash, vertex snapping, faces and islands): pick inside, S select closed objects, P / ? /
  SC / A / U, SOLID, the too-dense warning (docview hatch_lines). Command parity 88/88 (6 new scripts in
  tests/commands/hatch.json).
- M5.2: dimensions (libs/cmd/src/dims.cpp): DIMLINEAR (picked points or an object's ends, horizontal / vertical by the
  pointer in the UCS, H / V / R, T text), DIMALIGNED, DIMRADIUS, DIMDIAMETER, DIMANGULAR (two lines: dims.js
  angularFromLines, an arc, a circle, three points), DIMCONTINUE, DIMBASELINE (with U), the DIMSCALE … DIMEXO settings
  and DIMLUNIT. Command parity 95/95 (7 new scripts in tests/commands/dims.json).
- M5.2a: MSVC took `Array{Array}` in hatch.js loopsOf as a copy of the inner array (the trap noted at M4.3): loops
  are made with base::array_of. CI's MSVC job was red on M5.1d (cmd.golden, the hatch scripts).
- M5.3a: BLOCK (attribute definitions go into the block), ATTDEF (modes I / C / P), INSERT (by name, any case; S / R;
  each attribute asked), PURGE (nested unused blocks too), LIBRARY / BPARAMETER / INSERT F and L say what they need
  (libs/cmd/src/blocks.cpp). Command parity 99/99 (4 new scripts in tests/commands/blocks.json).
- M5.2b: CI's GCC job was red from M5.1b on: a lambda's local shadowed an enclosing variable it does not capture (GCC's
  -Wshadow sees it, Clang's does not). Fixed (corners.cpp, corner.cpp), and Clang builds now add
  -Wshadow-uncaptured-local so the gate catches the next one.
- M5.3b: the block editor: BEDIT opens a block as a space of its own (Doc::block_edit, set_space_map, take_id; its
  attribute definitions as texts), BCLOSE saves it as one undo step (squashed) or discards every edit made in it;
  BVSTATE / BVHIDE / BVSHOW say what they need (visibility states are not ported). The command harness now compares
  the block definitions too (tools/parity/commands.mjs, cadcraft-cli script): 103/103 scripts.
- M5.3c: GCC 14 found two more in CI: annotate.cpp ranged over `poly.get("pts").as_array()` (get() copies: a dangling
  temporary, a real bug) and flagged FILLET's option list built inside a co_await (kept in named locals now; the P
  option's helper takes its functions by value).
- M5.4a: inquiry (libs/cmd/src/inquiry.cpp): DIST, AREA (points, objects, add / subtract totals), MEASUREGEOM,
  ID (in the UCS), LIST (commands.js describe, attribute rows), UNITS (length format, one undo step); geom.js
  polygonMeasure / entityArea / pointInPoly (libs/cad/src/measure.cpp); units.js formatArea. Command parity 109/109
  (6 new scripts in tests/commands/inquiry.json).
- M5.4b: SELECT, QSELECT (qselect.js QS_PROPS, operators, * / ? wildcards), SELECTSIMILAR (with its settings),
  FIND (findtext.js findMatches / applyMatches: text, attribute values, dimension words around <>)
  (libs/cmd/src/select.cpp; a small code-point matcher stands in for the JS RegExps). BEDIT / INSERT name lists sort
  with base::js_locale_compare_numeric (ICU root collation) instead of an approximation. Command parity 113/113
  (4 new scripts in tests/commands/select.json).
- M5.4c: UCS (ucs.js: origin + axis, World, Previous, Z, 3point, OBject, NAmed save / restore / delete / ?),
  TURNLEFT / TURNRIGHT / TURNWORLD, PLAN, ZOOM (E / A / W / P / S / C, a typed 2x), NAMEDVIEW, and the host's
  LAYER, NEW, OPEN, SAVE, SAVEAS, DXFOUT, PLOT, SHORTCUTS, COMPARESETTINGS (libs/cmd/src/views.cpp; cmd::Host gained
  the view and file hooks, the viewer implements zoom and saves once the command ends). The harness compares the UCS
  and named views too. Command parity 119/119 (6 new scripts in tests/commands/views.json).
- M5.5a: layertools.js (libs/cmd/src/layers.cpp): LAYISO / LAYUNISO, LAYOFF / LAYFRZ by pick with U, LAYMCUR,
  LAYON / LAYTHW, LAYMRG and LAYDEL (model, sheets and blocks), RENAME (layers and blocks), COUNT, LINETYPE,
  LTSCALE, LWEIGHT; XREF ? lists a drawing's links; XATTACH, PUBLISH, SHEETSET say what they need. The harness
  compares the layers and the drawing's meta too, and takes a { layer, patch, current } step (what the Layers panel
  does). Parity found one more dangling `get(k).as_array()` reference (LAYUNISO): base::Value's as_array / as_object /
  as_string / as_bytes now return by value on a temporary, which ends that bug class. Command parity 124/124
  (5 new scripts in tests/commands/layers.json).
- 2026-10-19: the full-corpus suites after the base::Value change: import 110/110, doc 110/110, walk 108/108,
  DXF save 216/216 and DWG save 648/648 byte-identical. (A first walk run read 0/108: build/release's CLI was older
  than the `walk` command; rebuilt.)
- M5.5b: the properties model. expr.js (libs/cad/src/expr.cpp: tokenizer, recursive descent, tree walk; units,
  degrees, if / comparisons), propexpr.js (libs/cmd/src/propexpr.cpp: lengths in the drawing's format or with a
  unit, feet-inches, d°m's", radians / grads, expressions with or without "=", min / max / > 0 / whole numbers) and
  propdefs.js (libs/cmd/src/props.cpp, cc/cmd/props.hpp: the general fields and every type's — line, circle, arc,
  ellipse, point, polyline vertices, text, block reference, hatch, multileader, viewport, the 59 dimension fields —
  groupsOf, defsFor, valueOf, applyValue as one PROPERTIES step with locked objects counted and an error refusing
  the whole change). The harness takes { props } (every field of a group with its shared value) and { prop, value }
  steps on both sides. Command parity 132/132 (8 new scripts in tests/commands/props.json).
- M5.5c: PROPERTIES (PR, PROPS, CH, MO, DDMODIFY), PROPERTIESCLOSE, QPMODE (QP) and the hidden _PROPPICK
  (propcmds.js; libs/cmd/src/propcmds.cpp; cmd::Host gained the props_* hooks; CommandDef.hidden keeps _COMMANDS out
  of lists). The viewer's Properties panel (libs/ui-imgui/src/props_panel.cpp) on the right: the selection's groups,
  the sections, one control per field kind (typed values read as propexpr.js reads them, choices, layers, colours,
  linetypes, lineweights, transparency, materials, the polyline vertex stepper), "Varies", a pick button per point
  (both axes in one undo step), and Quick Properties by the cursor. The layers panel: "0" first then natural order,
  the current layer highlighted (a click makes another current), and the on / freeze / lock / plot switches drawn
  in code — each one undo step as in 0.5. linetypeNames / ltLabel / lwLabel are shared in libs/cmd/src/linetypes.cpp.
  Command parity 133/133. Checked under Xvfb with dwg-samples/example_2018.dwg: SELECTALL, PR, the linetype scale
  typed for all 58 objects, the Arc group's Geometry section. From here every fork build, gate and harness runs in
  forkrust.slice (2.5 cores, nice 19, -j3) — the owner's rule for the VPS; the full gate passes that way.
- M5.6a (2026-10-20): sheets. sheets.js (libs/cmd sheets.hpp: papers, UNIT_MM, scale lists with the architectural
  ones for imperial drawings, scale labels, best scale, model extents, aimViewport, a new sheet's border, title block
  and viewport) and the commands (libs/cmd/src/layouts.cpp): LAYOUT, MVIEW, VPORTS (MVIEW on a sheet, panes in the
  model), SPLIT, VPVIEW (a window or a named view), VPLAYER (? / Freeze / Thaw / Reset / Vpvisdflt, wildcards, All /
  Select / Current), MSPACE / PSPACE, VIEWLABEL. VIEWLABEL's live field needs doc.js editHooks: doc::Doc::edit_hooks
  run inside a change before it is recorded, and the engine installs the view-label hook. The host gives the clock
  (Host::now_ms: the title block's date), panes and viewport entry; a script answers as the JS stub does. The harness
  compares the layouts (paper and objects) and the open space, takes a { space } step, and runs both sides on one
  fixed clock (2026-10-16 UTC). Command parity 138/138 (5 new scripts in tests/commands/sheets.json).
- M5.6b (2026-10-20): plotting. plot.js in libs/plot (L5): layoutSheet (each viewport not turned off as a clipped view
  of the model, its frozen layers left out, then the paper), modelSheet (a window on A3 by default, 1:n or fitted),
  displayList (fills, strokes grouped by colour / lineweight / alpha, masks, points, texts; patterned hatches as a
  light tone with their outline; construction lines clipped to the view), the PDF 1.4 writer (Helvetica in
  WinAnsiEncoding, arcs as Béziers, transparency as ExtGState, one or many pages with bookmarks), extentsOf. sheets.js
  moved down to libs/sheets (L4) so plot and the commands share it. The page content is compressed through an
  injected compressor (cc::image::zlib_compress where the render libraries are built). `cadcraft-cli plot <in.dwg>
  --out <out.pdf> [--sheet NAME | --model]` writes a PDF; `plot-parity` gives the uncompressed PDFs to
  tools/parity/plot.mjs. Parity: 108/108 corpus drawings, 307 PDFs byte for byte (each sheet and the model at its
  extents; the two 10-byte files the reader refuses are skipped as in walk.mjs). Speed, carey/Carey Foodservice.dwg
  (11 sheets, ~230 MB of PDF), both in forkrust.slice: C++ 7.5 s (display lists 3.5 s) against JS 19.8–33.3 s
  (display lists 7.0 s); the first C++ version took 18.7 s — a 52 MB page copied by an initializer list and the
  content kept as millions of small strings. CI: M5.5c failed MSVC (C4244, an int → float in a layer-switch icon),
  fixed here.
- M5.6c (2026-10-20): sheets in the viewer. The tabs follow the document's open space (LAYOUT opens its new sheet,
  VPVIEW picks on the model and comes back), a new sheet's view is fitted, and PLOT / Ctrl+P / File › Plot to PDF
  plots the open sheet at 1:1 or the model at its extents on the default paper (A3, Tabloid for imperial drawings):
  the desktop's save dialog (a PDF filter), a download on the web; a plot never touches the drawing's saved state.
  Checked under Xvfb: LAYOUT on dwg-samples/example_2018.dwg opens Layout3 with its title block and viewport.
- M5.7a (2026-10-20): CadCraft's own DXF reader, dxf.js readDxf (libs/cad/src/read_dxf.cpp, cc/cad/read_dxf.hpp):
  HEADER (units, length format, dimension variables, UCS, insertion base), TABLES (layers, dimension and text
  styles, linetypes, views, panes, UCSs, block records), BLOCKS (attribute definitions, xrefs, CadCraft's constraint
  and 3D blocks, CCLAYOUT_n sheets), ENTITIES (every type dxf.js reads, ATTRIBs onto their references, hidden objects
  left out) and OBJECTS (layouts, XCLIP dictionaries and filters); layouts from LAYOUT objects or an R12 paper space;
  multileaders, pictures and wipeouts through importDwg on the drawing model (extrasFrom: the import now tags what
  it made of them, as the JS's EXTRA set). libs/cad cannot reach io-dxf (L3), so the caller hands in
  dxf::read_dxf_model. dims.js angularFromLines moved down to libs/cad (DIMANGULAR and the reader share it).
  `cadcraft-cli doc / import` read a DXF this way, the viewer opens a DXF as an editable document (format labels as
  cadcraft.js: "DWG 2018 (AC1032)"), and fuzz/fuzz_cad_dxf.cpp fuzzes it. Parity: tools/parity/doc.mjs takes DXF
  files (decoded UTF-8, else Windows-1252, as the app): 214/214 corpus files (110 DWG, 104 DXF) load the same; the
  doc golden test covers the three public DXF fixtures. A count in a file (vertices, hatch edges, MINSERT copies) is
  capped where the JS would loop on.
- M5.7b (2026-10-20): the clipboard. clipboard.js and blockcopy.js (libs/cmd clipboard.hpp / .cpp): makeClip (the
  objects without ids, the blocks they need nested first, their layers' settings; viewports stay on their sheet),
  basePoint, pastePreview, pasteClip, planCopy / applyCopy (a block of the same name keeps this drawing's definition,
  anonymous ones get a free name, layers come with their settings); COPYCLIP, CUTCLIP, COPYBASE, PASTECLIP, PASTEORIG
  (libs/cmd/src/clipcmds.cpp). The clipboard is the host's: the viewer keeps one for every drawing it opens, a script
  keeps its own (and, like 0.5 without a browser, says only this window can paste). The viewer: NEW (a blank drawing;
  with unsaved changes 0.5's "Unsaved changes" dialog asks first), OPEN (the file picker), Ctrl+C / Ctrl+X / Ctrl+V.
  Command parity 141/141 (3 new scripts in tests/commands/clipboard.json). Parity found a dangling pointer of my own
  (a map of blocks pointing into a `get()` copy) before any test ran it under ASan.
- M5.8a (2026-10-20): tancircle.js (libs/cad tancircle.hpp: curveAt, circleTTR, circleTTT by Newton from seeded
  starts) behind CIRCLE T / TTT and TTR; TEXTMASK (textmask-cmd.js) and XCLIP (xclip.js) — registered after
  commands.js's own, as cadcraft.js does (the harness now loads textmask-cmd.js and xclip-cmd.js too); clipped
  references draw clipped (libs/docview xclip.hpp: clipLine, clipArc, clipRing, clipPoly; the walk keeps a stack of
  clips, applied innermost first like 0.5's nested clipOut outputs — so screen, plots and extents clip); POLARANG,
  LINKVIEWS, OPENIN3D, LASSOMODE, LWDISPLAY through Host::settings (the viewer keeps the polar angle and drives LWT;
  a script answers as 0.5 without a browser); HELP. Command parity 145/145 (4 new scripts in
  tests/commands/drafting-extras.json), walk 108/108, a unit test for a clipped reference.

