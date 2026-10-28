# Session log

Last updated 2026-10-21 · Change: M7.1 (cc-ifc STEP reader); M5.8i1 (the LISP core); M7.2 (IFC schema tables); M8.1 (lib/shapes, the PartCraft parity harness); M7.1 (cc-ifc STEP reader); M9.1 (CivilCraft's TIN).

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
- M5.8b (2026-10-20): pictures (pictures.js, pictures-cmd.js; WL.14). The drawing keeps the picture files it is given
  (libs/doc pictures.hpp: sniffPicture / jpegInfo by content, 64 MB and 100 Mpixel caps, findPicture by base name or a
  name containing it); IMAGEATTACH (IAT) places a PNG or JPEG by corner, width and angle, IMAGE (IM) lists the files
  and takes the missing ones, IMAGECLIP (ICL) clips to a rectangle or polygon (a shared picture block gets a copy of its
  own) or removes the clip. A command waits for the host's picker through a new wait (Ctx::files →
  Host::pick_pictures → Engine::give_files); scripts queue files with a `{pictures}` step, as the harness's host does.
  The walk reports pictures (out.picture: placement, frame, fade) and leaves a picture's name out once its file is
  there. Command parity 148/148 (tests/commands/pictures.json), a fuzz target (picture), a unit test.
- M5.8c (2026-10-20): pictures in PDFs (plot.js WL.13, pictures.js pdfImage). libs/image picture.hpp ports 0.5's own
  PNG decoder (decodePng: grey, grey + alpha, RGB, RGBA, palette + tRNS, 16-bit to 8, interlaced refused; bounds-checked,
  100 Mpixel cap), its box filter (shrink) and pdfImage: a JPEG goes in as it is (DCTDecode, grey or RGB), a PNG
  deflated with its transparency as a soft mask, shrunk to `max_dpi` when printed denser. libs/plot writes the images
  under the linework, clipped to their frames, faded, one image per file per PDF, numbered after everything else as
  0.5 does. libs/image moved to L2 and is built everywhere (zlib a base dependency; libs/doc shares jpegInfo). A command
  script may ask for the model's PDF (`{"plot": {"mono", "maxDpi"}}`); tests/commands/pictures-pdf.json: RGBA,
  palette + tRNS, 16-bit grey, RGB, grey + alpha with every PNG filter, a clipped picture, a JPEG kept, a CMYK JPEG left
  out, 1 dpi shrinking, a shared file — 4/4 the same. The PDFs are compared with each Flate stream inflated and the
  lengths and xref offsets masked (plot::pdf_canonical): Node's zlib is Chromium's, which deflates the same bytes
  differently from zlib 1.3.2; every inflated byte, every image and every other byte of the file match. Corpus plot
  parity unchanged (no corpus drawing has its picture files). Fuzz target png_decode.
- M5.8e (2026-10-20): constraints and parameters. libs/cad gsolve.hpp (gsolve.js: damped Newton with the least-norm
  step, Levenberg–Marquardt, a halving line search, Gram–Schmidt analysis — every Map order, summation order and Math
  call as the JS's, so solves match to the bit) and params.hpp (params.js: evaluateVars in dependency order with
  cycles marked, applyParams, driveBack / driveVar by secant then bracket and bisect, setVar, removeVar inlining);
  expr.hpp gains literal, formatNumber, renameIn. libs/doc gcons.hpp (gcons.js: making, measuring, solving, status,
  afterEdit, forFile, resolveParametric with every field of a file's payload checked) and params_doc.hpp
  (params-doc.js: each edit on a draft, resolved, solved, written in one undo step). Doc::change re-solves after every
  edit that moves constrained geometry and refuses (rolls back, Doc::note) one the constraints cannot follow;
  Doc::load resolves a file's payload by its CCREF keys. libs/cmd: the 27 parametric commands of pcommands.js except
  SWBOUT (GEOMCONSTRAINT / GC*, DIMCONSTRAINT / DC*, DELCONSTRAINT, AUTOCONSTRAIN, PARAMETERS, -PARAMETERS,
  CONSTRAINTBAR, DCDISPLAY). The harness compares the constraints and the parameters too. Command parity 159/159
  (tests/commands/constraints.json: 7 scripts — every geometric and dimensional type, a move the rest follows, a
  move refused, AUTOCONSTRAIN with settings, conversions, parameters driven back through a chain, a cycle refused);
  doc parity with constraints compared. Fuzz target parametric; unit tests.
- M5.8f (2026-10-20): constraints in files and through edits. The DWG and DXF R12 writers put "CCREF:<key>" on each
  constrained entity and the payload in the block CADCRAFT_PARAMETRIC, as dwg-save.js and dxf.js do (gcons.forFile);
  ckeep.js (libs/doc ckeep.hpp) keeps them through MIRROR (copied onto the copies, or re-signed when the source is
  erased) and TRIM (onto the piece that still has what each holds, direction rules on every piece); the clipboard
  carries the constraints among the copied objects (clipConstraints / pasteConstraints, dimensional ones renamed). A
  command script may ask for its drawing saved (`{"save": ["dxf", "dwg2018"]}`, libs/engine script_save): the files
  compare byte for byte and are opened again, their constraints compared too. tests/commands/constraints-save.json and
  constraints-keep.json (5 scripts); commands 164/164; corpus DXF and DWG saving unchanged.
- M5.8d (2026-10-20): pictures on screen (0.5 drawPictures). PNG and JPEG are decoded by wuffs (memory-safe by
  construction, as section 3 asks: v0.4.0-alpha.10, its single-file release pinned by commit and hash,
  cmake/Wuffs.cmake; libs/image picture-decode, render builds only) to straight RGBA, box-filtered down to the GPU's
  largest texture when bigger. The walk's pictures become drawing::Bitmap (docview to_drawing) and a scene batch that
  draws first in each group as view.js does: the frame's fans into the stencil, then the picture's quad textured with
  its file, faded by its alpha. The renderer keeps one texture per file until the drawing's files change
  (picture_version); IMAGE giving files redraws without an edit. The viewer picks pictures through the inbox (desktop:
  SDL's dialog, PNG / JPEG; web: a file input), so IMAGEATTACH and IMAGE work in the app. Test: a picture and a clipped
  copy rendered offscreen through the whole pipeline, their pixels checked.
- M5.8g (2026-10-20): constraints in the viewer (gview.js, params-panel.js). Model space shows constraint bars (a row
  of glyph boxes beside each constrained line, segment, circle or point; coincident ones a small square) and
  dimensional constraints drawn as dimensions in their own colour at a fixed size on screen ("d1=40", "fx: r=d1/10");
  CONSTRAINTBAR and DCDISPLAY switch them. A click on a glyph or label selects that constraint (its objects
  highlighted), Delete removes it, a second click on a label edits it (value or expression; a number on a driven value
  solves what drives it). PARAMETERS opens the Parameters panel: name, expression, value per row; typing an expression
  links it, a number in a driven value solves back (asking which parameter when several could), x deletes (a used
  parameter can be written into what uses it), Add makes a user parameter; each edit is one undo step. The
  document's notes (a solve that could not follow) go to the command history. Test: a DWG saved with constraints
  opens with them, PARAMETERS opens the panel, the overlay draws with both switches.
- M5.8g2 (2026-10-20): SWBOUT and SolidWorks sketch exports (swsketch.js), in libs/doc swsketch. SWBOUT (SWMACRO)
  writes the model as a VBA macro for SolidWorks (.swb, CRLF, Windows-1252): lines, circles, arcs, points and
  polylines (bulges as arcs, vertices joined by coincident relations) on the Front Plane in metres, the geometric
  constraints as sketch relations (a line's middle as a held construction point), the dimensional ones as named
  dimensions, user parameters as global variables and expressions as equations (sqrt as sqr, atan as atn …; %,
  comparisons and functions SolidWorks lacks written as their value, with a note). The app saves it (desktop) or
  downloads it (web); headless the engine keeps it. A .json export from the SolidWorks-side macro is untrusted input
  (format, version, limits, ids, every reference checked) and one of its sketches opens as the drawing: segments and
  loose points as entities (construction ones on CONSTRUCTION), the origin as a fixed construction point, shared
  points as coincident constraints, relations and dimensions as constraints, globals and equations as parameters; a
  dialog asks which sketch when there are several; never saved back over the .json in place. Parity: tests/commands
  swsketch.json, 13 scripts (macro text, notes, opened document, constraints, parameters, seven malformed exports), the
  same as the JS; commands 177/177. Fuzz target swsketch (reader, opener, macro writer). Known difference: an export
  whose globals list holds a null opens here; 0.5 throws on it (g.name of null).
- M5.8h1 (2026-10-20): point clouds, the reader (pointcloud.js, las.js) in a new L2 library, libs/cloud. The LAS
  1.0–1.4 header with its variable-length and extended records (every size, count and offset checked; a file cut short
  says how many points it holds), text clouds (x y z, x y z r g b, x y z i, x y z i r g b; a PTS count line; spaces,
  tabs, commas or semicolons; the first 64 KB tell the columns), every point read once and at most a budget kept,
  evenly (the keeper halves when full), positions as float32 relative to a whole-unit origin, colours (the file's,
  16-bit or 8-bit), intensity and class; the colour modes (height ramp from the 1st–99th percentile, intensity 2nd–98th,
  ASPRS class colours), slices, the plan grid for snapping and the summary. `cadcraft-cli cloud <file> [--budget N]`;
  tools/parity/cloud.mjs runs pointcloud.js on the same files: 14/14 the same (LAS formats 0/2/3/7/8, XYZ, PTS of the
  0.5 tests' room, generated once into testfiles/generated/pointcloud-room; at the default budget and at 1000). Unit
  test and fuzz target cloud. Not yet: coordinate systems (M5.8h2), LAZ (M5.8h3), the commands and the underlay (M5.8h4).
- M5.8h3 (2026-10-21): LAZ point clouds through laz-perf 3.4.0 (Apache-2.0; what 0.5 runs as WebAssembly), fetched by
  commit and hash (cmake/LazPerf.cmake), built with our hardened standard library. Bit-flipped and cut LAZ files
  trapped the hardened library inside it (a chunk read past its end), the fuzzer found an assert and a seek before the
  buffer, and reading showed a raw walk past the chunk table, so seven patches there (and NDEBUG, as 0.5's build) make it refuse or read zeros instead: no read past a buffer, no byte left undefined, layer, chunk-table
  and extra-bytes sizes capped. Parity 34/34 (the room fixtures with their two LAZ files, 18, and the fuzz seeds, 16); a 7-minute local fuzz of the patched decoder, 24 049 inputs, found nothing after the seventh patch; damaged LAZ files differ from 0.5 by design
  (its WebAssembly reads leftover heap) and are not compared. Fuzz target cloud with tiny synthetic seeds
  (tools/fuzz/cloud_seeds.py): LAS, LAZ, XYZ, PTS.
- M5.8h4 (2026-10-21): point clouds attached (cloud-cad.js). libs/cloud Attached (CloudAttach: where the cloud sits,
  its units against the drawing's, the building-scan slice at 1.2 m, the points shown, Node snap through the plan grid);
  POINTCLOUDATTACH (PCATTACH) through the host's picker (desktop: SDL's dialog; web: the file input) and POINTCLOUD (PC:
  Slice, Height, Thickness, Color, Size, Visible, Zoom, Detach) in libs/cmd; parity tests/commands/clouds.json 7/7
  (LAS, LAZ, XYZ, PTS, a building-height cloud at a point, a replaced cloud, two refusals, cancels; the harness gained a
  cloudFile step and the cloud's state in its results, and imports cloud-cad.js as cadcraft.js does). In the viewer the
  cloud is drawn under the linework of the Model tab as 0.5 draws it (one picture of the canvas, the highest point
  winning each pixel, squares of the point size), redrawn only when the view or the cloud changes; Node snaps to its
  points after the drawing's own (view.js snapExtra, cad::best_snap's new `extra`); attaching zooms to it (fitPoints,
  0.9). Viewer test: the picker's file attached, drawn, sliced, detached.
- M5.8h5 (2026-10-21): VIEWBASE (FLATSHOT), flatview.js: libs/m3d flatview projects the 3D model into a view (top,
  bottom, front, back, left, right, iso): every hard edge, a soft edge only where it is the outline, each edge sampled
  against the model's triangles seen from the view through a 96 × 96 grid and its in-front / behind changes found by
  halving; pieces on one line merged, hidden pieces under visible ones dropped. The command asks the view, hidden lines,
  the scale (on a sheet the largest standard scale that fits) and where, and places one block VIEW-<View>-<n> with
  VIEW-VISIBLE and VIEW-HIDDEN layers in one undo step. Parity tests/commands/viewbase.json 6/6 (3DFACE drawings: a box
  in front of a pyramid, every view, scales typed and bad, a sheet); the harness gained a {dxf} step that starts a script
  from a drawing.

## 2026-10-21 (session fork-cpp, new machine)

- The fork moved from the VPS to a WSL2 machine (Ubuntu 24.04, 32 threads): the same pinned toolchain under
  `/opt/cadcraft-cpp` (LLVM 22.1.8 from apt.llvm.org, CMake 4.4.4, Ninja 1.13.2, vcpkg 2026.07.29, emsdk 6.0.10,
  osv-scanner 2.7.0) plus GCC 14 for GCC-only warnings, so `tools/env.sh` works unchanged; the clone and builds live on
  the Linux filesystem, the parity inputs outside the repo as before.
- FZ.3 (2026-10-21): the nightly fuzz of `cloud` (run 39571113094) found a heap read out of bounds in laz-perf's
  arithmetic decoder: a damaged stream's value passes its interval, and `decodeSymbol` indexes the model's decoder table
  past its end (point format 7+; reached from the RGB field, the point14 field and the integer corrector). The input was
  not kept, so it was found again locally in 2.5 minutes, nine inputs in 10 minutes, all this one read. cmake/LazPerf.cmake
  patch 8 throws `lazperf::error` when the table index passes `table_size` (a sound stream reaches `table_size` at most:
  the table has two spare entries), so the reader stops with 0.5's las.laz.stopped note. The patch marker now holds the
  patch set's number: a build directory patched by an older set extracts the source again, so a new patch reaches the
  CI caches and old ones are never applied twice. Two inputs (point14, RGB) in fuzz/corpus/cloud and a regression test
  that trips ASan without the patch. Fuzzing the patched target found two more problems within 5 minutes (FZ.4, FZ.5).
- FZ.4 (2026-10-21): fuzzing the patched target found a stack overflow in laz-perf's GPS time decoder (point formats
  1 and 3): every switch to another of its four sequences is a recursive call, and a damaged stream can switch without
  end. A sound stream recurses once at most (the encoder switches to the sequence that fits), so patch 9 throws
  `lazperf::error` past depth 8 and the reader stops with the note. Input in fuzz/corpus/cloud and a regression test
  that fails without the patch.
- FZ.5 (2026-10-21): the same run found 10-second timeouts in our point loop: a 2.7 KB LAS 1.4 file claiming 7e16
  points in one chunk, whose layers patch 1 kept serving as zeros, so laz-perf decoded garbage for as many points as
  its 32-bit count allows (hours). A sound layer is never read past its end (the encoder pads it), so patch 1 now stops
  a layer read more than 64 bytes past its end with `lazperf::error`. Here that is the first point, so the file is
  refused as an empty cloud (pc.empty). Input in fuzz/corpus/cloud and a regression test. With patches 8–10: cloud parity
  34/34 (the room fixtures and the seeds; damaged LAZ files differ from 0.5 by design, as before), and fuzz target
  cloud for 15 minutes × 16 workers (1.01 M inputs) with no crash, timeout or out-of-memory.
- M5.8i0 (2026-10-21): groundwork for LISP (M5.8i). The evaluator awaits the drawing as commands do, but a library may
  link only lower layers, so `Task<T>` moved from libs/cmd to libs/base (cc::cmd::Task names it still), and units.js
  parseLength (with mixed numbers and the plain-number test) moved from the command engine to libs/cad beside
  formatLength, for rtos / distof. Measured on the frozen 0.5: its evaluator awaits at every step, so recursion is bounded
  only by memory (`(r 100000)` runs); its reader recurses on the native stack and stops near 5 300 nested lists with
  "Maximum call stack size exceeded".
- REL.1 (2026-10-21): the owner set the release plan — the forks release two versions behind CadCraft (CadCraft 0.7 →
  fork 0.5, 0.9 → 0.7), fork 0.5 is everything in 0.5 (eight apps and Studio) at parity with js-0.5, CadCraft 0.7 is
  expected within about a month, and the work runs as parallel workstreams. docs/roadmap.md has the plan, the size
  measured (about 190 k unique JS lines left) and workstreams W1–W6.

## 2026-10-21 (W2: m3d kernel + ModelCraft, worktree w2-modelcraft)

- M6.1 (2026-10-21): Manifold 3.5.4 behind `SolidOps` (libs/solid, L2), shared by W2 (ModelCraft's solids.js, text3d.js),
  W3 (cc-ifc geo-bool.js), W4 (PartCraft) and CadCraft's 3D tab. cmake/Manifold.cmake fetches Manifold at tag v3.5.4
  (commit ce50d78) and the Clipper2 commit its build fetches (46f6391), both hash-checked, and builds them sequential
  (MANIFOLD_PAR=-1, no TBB), `-ffp-contract=off`, with the hardened standard library and the sanitizers. Manifold 3.5.4
  has its own trigonometry (musl's), so libm does not matter; libc++ 22's sort.h is libc++ 21's. Found on the way:
  (1) libc++ 22's `multimap::find` returns another of equal keys than libc++ 21's, which AssembleHalfedges relies on
  (one configure-time patch: `lower_bound`, which also stops where a hostile mesh has no next edge instead of reading
  `end()`); (2) the lazy CSG tree flattens a nested boolean only when no other handle holds its node, so the case
  runner keeps every solid alive as the JS harness keeps every Manifold (6 of 44 cases differed until it did).
  tools/parity/manifold.mjs (seeded cases: welded boxes and prisms as solids.js welds them, cubes, cylinders, spheres,
  turns, add / subtract / intersect, batch union / intersection, trim and split by a plane, a refused open mesh) against
  manifold-3d 3.5.4: **10,004 / 10,004 identical**, every float compared as bits (status, emptiness, volume, vertex
  properties, triangles). `cadcraft-cli manifold <cases.json>`; tests/golden/js-0.5-manifold.json (24 cases) replayed
  by `solid.SolidOps gives the manifold-3d 3.5.4 wasm's results bit for bit`. Fuzz target `solid` (hostile meshes and
  JSON cases): within a minute it found Manifold's Merge reading the vertices of a triangle index past them (and the
  constructor would read past its runs, or divide by a numProp of 0), then CreateHalfedges pairing the halfedges of a
  mesh whose edges do not pair up and its manifold check reading past them; SolidOps now checks those before Manifold
  sees the mesh, after Manifold's own checks so a refused mesh gets the status Manifold gives it (regression tests +
  fuzz/corpus/solid/regress-*). Then 12 minutes × 10 workers (1.45 M inputs): clean. Note: the local frozen copy
  (`~/cadcraft-data/testfiles/fork-parity/js-0.5/vendor/manifold-3d/3.5.4`) lacks manifold.js / manifold.wasm; the
  snapshot's copy (/var/www/CADCRAFT/testfiles/fork-parity/js-0.5) has them, byte-identical to npm's manifold-3d 3.5.4
  (MANIFOLD_JS points the script at it).
- M13.1 (2026-10-21, W6): Studio Live's wire format and session decisions, `libs/live` (L2, shared by every app).
  live-client.js's MessagePack ported as it behaves in JS, not as MessagePack allows: a number is a double and an
  integral one in the safe range writes as the smallest int (so a float64 2.0 comes back as int 2), integers past 2^53
  are BigInts, a decoded map is a plain object (array-index keys first in number order, a repeated key keeps its place,
  `__proto__` dropped, other key types through String()), strings through TextDecoder (U+FFFD, a leading BOM dropped)
  and TextEncoder (a lone surrogate written as U+FFFD), ext types read as `{ext, data}`, depth 512, a count over what is
  left refused before allocating. The envelope `{t s c n q b}` (zeros left out). `SessionCore`: desktop-shell
  live/session.rs `check` / `commit` (d132555) — role → locks → holds → each key by mode (p, c, d, s/r), one `by` per
  key, `exists` for any key a commit wrote, a delete dropping its key's hold. tools/live-test/vectors.json passes
  whole: 54 envelopes encode to their bytes and back, 4 other encodings decode, 6 bad inputs are refused, the 11
  scenarios decide every step (seq, reason and `by`) as the 0.5 server. Fuzz target live_msgpack (the writer's bytes are
  a fixed point; the core decides whatever envelope comes out), seeded with the vectors.
- M14.1 (2026-10-21, W6): cc-skp in C++, `libs/io-skp` (L3): read.js, zip.js (zlib, as DecompressionStream reads: a
  stream that does not end or has bytes after its end is damaged), vff.js (2021+: the model.dat record tree, the
  material.xml subset, units, thumbnail, UVs) and legacy.js (SketchUp 3 … 2020: the MFC store map, the slot-base
  bootstrap, annotation burns, released definition objects, every recovery point where the JS catches), each function
  ported in order; the JS's English from messages.js (tools/gen/skp-messages.mjs). The model is typed (the shape W2
  imports); `to_value` is the JS object for the kit. **Parity 27/27 on the SKP corpus** (SketchUp 3, 6, 7, 14, 17, 18,
  20, 25, 26: `run.mjs --fork cpp --engine skp`, the kit's tolerance), and 31/31 on cc-skp's own hand-made test files
  (tools/skp-test/test.mjs's builders, regenerated by tools/gen/skp-fixtures.mjs with the frozen readSkp's answers into
  tests/golden/skp/: the known model, the refusals, the ZIP faults, cuts). Timings (bench.mjs, best of 7, a machine
  under load ~130 on 32 threads from the other streams' gates): the 5.25 MB 2026 file 153 ms against the JS's 554 ms
  here (OpenSKP's C++ took 630 ms on it, CLAUDE.md § 15), the 2 MB SketchUp 6 file 11.1 ms against 275 ms (25×);
  3.6–25× the JS on the eight largest. Fuzz target skp (the hand-made files and the small corpus files as seeds, in
  the build tree only): 15 min × 8 workers, 558 k inputs, then 10 min more on the final code, nothing found. `cadcraft-cli
  parity skp` / `bench skp`.

## 2026-10-21 (W3: cc-ifc + BimCraft, worktree w3-bimcraft)

- M7.1 (2026-10-21): cc-ifc's STEP reader (lib/ifc/step.js) in a new L3 library, libs/io-ifc: `scan_step` indexes every
  instance of the DATA section(s) in one pass (ids as the JS's Int32Array, max id as its double, type names, the header,
  scan errors with their offsets), `parse_params` / `parse_complex` parse one instance when it is read (Raw values: $,
  *, numbers through JS Number(), strings decoded in UTF-16 as the JS does — raw UTF-8 or Latin-1, '' and the \S\ \X\
  \X2\ \X4\ escapes, the upper-casing that finds \X0\ — refs, enumerations, binaries, typed values, aggregates; errors
  worded as step.js, with its byte offsets). `cadcraft-cli parity / bench ifc-step` (the kit's NDJSON). Parity: kit
  ifc-step **118/118 exact** on the IFC corpus; tools/parity/ifc-step.mjs runs any files through both (the fuzzer's
  5,071 kept inputs: 5,071/5,071 the same). Golden: tests/golden/js-0.5-ifc-step.tar.gz (the public bimcraft-house.ifc,
  every line). Fuzz target ifc_step: its first 11 minutes × 16 workers found 16 timeouts, one cause — a DATA keyword
  after ENDSEC with no ";" sends the 0.5 scan back to byte 0 for ever (the JS hangs too); the scan now ends there
  (regression test, fuzz/corpus/ifc_step/regress-data-no-semicolon, docs/gaps.md #25); then 11 minutes × 16 workers
  (about 2.4 M inputs) clean. Aggregates nested past 1,000 levels stop with V8's words (V8's own limit moves with the
  JIT: gaps #24).
  Bench (kit `bench.mjs --engine ifc-step --largest 6 --runs 5`, 2026-10-21, best of 5, JS / C++ ms): Molio_with_URIs
  70.5 MB 1413 / 614 (2.30×), IFC_Schependomlaan_geo 47 MB 1754 / 503 (3.49×), IFC Schependomlaan 2088 / 478 (4.36×),
  Clinic_Structural 18.2 MB 583 / 359 (1.62×), Duplex_MEP 17.0 MB 1041 / 155 (6.71×), Clinic_Architectural 12.4 MB
  327 / 92.7 (3.53×); parity same on all. The lever: Raw values allocate per value (strings, item vectors); the Rust
  fork's 6–8× says a flatter value store pays.
- FZ.6 (2026-10-22): the nightly fuzz of cmd_script (run 39749853648, the first nightly after FZ.3–FZ.5: cloud then ran
  its 600 s clean) timed out on a script whose MIRRORs of everything double the drawing. The stack was sampled in
  Doc::change's copy of the step's ops for the edit hooks, but moving them instead of copying them changed nothing
  measurable (1.10 s vs 1.29 s for 19,600 objects mirrored to 156,800, release build): the time is the drawing's growth,
  which 0.5 has too. So the bound belongs to the fuzzer, not the app: `ScriptLimits::max_objects` (0 = none, as 0.5;
  the CLI and the parity runs keep none) stops a script with "*script object limit*" once the drawing passes it, and
  fuzz_cmd_script sets 20,000 (0.5's own ARRAY cap). The CI artifact was not kept, so the regression input is a
  synthetic doubling script (fuzz/corpus/cmd_script/regress-mirror-doubling.json: 12 s of CPU under ASan before, 5.5 s
  after) and a test. cmd_script then ran 10 minutes × 4 workers clean.
- SEC.1 (2026-10-27): gate.osv failed on main from the nightly of 2026-10-23 on: brotli 1.2.0, CVE-2026-111497 (an
  out-of-bounds write in the decoder on a crafted stream, published 2026-10-22; fixed in 1.2.1). Brotli came in only
  through FreeType's `brotli` feature, which decodes WOFF2 fonts; the fork's FreeType loads its own bundled OFL fonts
  only, never a document's or a WOFF2, so the feature is dropped instead of patching a port: brotli is no longer built.

## 2026-10-21 (W1: CadCraft LISP, worktree cc-lisp)

- M5.8i1 (2026-10-21, W1): the LISP core, libs/lisp (L4): CadCraft 0.5's lisp/types.js, reader.js, interp.js,
  builtins.js, vl.js, wild.js and missing.js — values (integers apart from reals and wrapping at 32 bits, strings in
  WTF-8 indexed in UTF-16 units as the JS indexes them, interned symbols, shared conses and functions freed without
  recursion), the reader, the evaluator (coroutines, `Task<Value>`, so `(command …)` can await the drawing in M5.8i2;
  dynamic scope restored by RAII; `*error*` heard where the error happens, as 0.5), every built-in that needs no
  drawing in the JS's definition order, wildcards matched by hand with the JS RegExp's semantics and V8's words for a
  bad range. `cadcraft-cli lisp <sessions.json>` and tools/parity/lisp.mjs (each session gets fresh JS modules, so the
  module-scope counters start over as the CLI's do) compare every step's value, error, cancel flag and printed pieces:
  **27/27 sessions, 980/980 steps** (tests/lisp: the 0.5 suite's "The language" section — its 171 cases bar textbox,
  reader errors, *error* and dynamic scope, output, files, the loop — and about 800 more cases of ours: numbers and how
  reals print, comparison, lists, sorting, strings and surrogates, wildcards, rtos / angtos / angtof / distof / cvunit,
  geometry, special forms, routines' commands, files, the nesting limits). Goldens in tests/golden/js-0.5-lisp.tar.gz,
  replayed by `lisp.golden`. Measured: a plain recursion takes about 0.8 KB a level in release, 2.5 KB under ASan
  (4 KB with 30 arguments), and `(r 100000)` runs in 0.44 s where the JS takes 1.2 s; `Interp::kMaxDepth` = 250 000
  nested evaluations keeps a runaway near 0.65–1 GB under ASan. Limits where 0.5 has only memory or V8's stack (read
  nesting 4 096, equal 7 000 deep, spreads of 125 000 elements, the longest string) answer with V8's own words
  (docs/gaps.md #26). fuzz target `lisp` (read + evaluate with a 100 000-step budget that also counts the list elements,
  string units and wildcard work a built-in does, and a 64 K longest string; 28 seeds): its first minute found the
  reader overflowing the native stack under ASan below its 4 096 cap (now an explicit stack; regress-reader-nesting.lsp
  and a test); then wildcards on long strings were slow (now the greedy matcher, and charged), and a GCC 14 build found
  co_await inside ?: evaluated on both arms (rewritten as if; GCC passes the suite). Then 15 min × 4 workers,
  385 209 inputs: no crash, timeout or out-of-memory.
- M7.2 (2026-10-21): cc-ifc's schema.js. The three schema tables (IFC2X3, IFC4, IFC4X3; owner's exception to section 4,
  attribution kept: docs/architecture.md) converted from the frozen 0.5 tables by tools/gen/ifc-schema.mjs into
  libs/io-ifc/src/schema_tables.inc, the same rows in the same order; `Schema` builds every entity once (attributes,
  supertypes first, parsed type codes, derived sets, subtypes depth first in table order) and is immutable after, so
  threads may share it; `type_code` / `name_of_code` (CRC-32, the apps' IFC constants). Test: a digest of every entity
  and type computed the same way in node from js-0.5 (3/3 schemas the same), type codes, schema names.

## 2026-10-21 (workstream W4: PartCraft)

- M8.1 (2026-10-21): lib/shapes in C++ (libs/shapes, L1): sections.js (the 1335-row AISC v16.0 / EN 10365 table, built
  in the JS's order with its field order; displayName, findSection in any case, familyRows; outline with fdlibm's
  sin / cos, exactArea, bbox with Math.min's NaN and -0, shoelace, loopLength with V8's hypot), parts.js (ten families,
  sizes, default options, makePart on untrusted options as Number() reads them, LIBRARY_SET) and items.js (section and
  elevation items, part items for every family, local items with the CC0 credit; the description cut at 500 UTF-16
  units). The rows come from the frozen JS through tools/gen/shapes-tables.mjs (each number as the JS evaluates it).
  The parity harness is the Rust fork's pcdiff adapted for `cadcraft-cli part` (tools/parity/part.mjs, generators and
  JS driver in tools/parity/part/, paths from the env): libs/partcraft `run_script` answers the driver's op names, and a
  generator gen-shapes.mjs covers lib/shapes (every section's record, outline at seg 6 and 8, area, loops and section
  item; every family and size; seeded cases: names in any case and with junk, hand-made profiles, untrusted options,
  elevation lengths, local items). Parity: 46 596 / 46 596 calls the same, exact (zero tolerance), seeds 1–30. Golden
  tests/golden/js-0.5-part.tar.gz (seed 1, every 9th section: 15 996 calls) replayed by test_partcraft. Fuzz target
  part_script (any JSON script through run_script): 11 minutes × 16 workers (about 8.6 M inputs), and again on the final
  code 10.5 minutes × 12 workers, nothing found; a
  segment count over 4096 is refused there (the JS runs out of memory; docs/gaps.md).
- 0.5's PartCraft tests run against the frozen app from a scratch layout (bin/test-partcraft*.mjs and
  tools/partcraft-test/ from forks/work/ref): test-partcraft.mjs needs OpenCascade (replicad-opencascadejs, LGPL) for its
  exact-kernel sections and stops in its simulation section; test-partcraft-params.mjs needs tools/partcraft-cli/partcraft.mjs
  and test-partcraft-sw.mjs tools/partcraft-sw/fixtures/, neither copied; bin/test-shapes.mjs was not copied either.
## 2026-10-21 (session fork-cpp W5: CivilCraft, DetailCraft, MarkupCraft, TakeoffCraft)

- The 0.5 app tests run against the frozen js-0.5 (forks/work/ref, a scratch repo of links): test-civilcraft-*.mjs 21
  of 29 pass whole (3d 107, agent 25, blocks 58, cad 141, codes 33, drape 28, features 44, flprofile 26, hydro 37, labels 20,
  mapout 19, masshaul 16, parcels 52, points 46, sections 73, sheets 73, staking 26, super 37, traverse 34, undo 10, why 19);
  the other 8 stop on this machine's missing oracles (Python pyproj, numpy, rasterio, ephem; poppler; a DWG reader set),
  not on the app.
- M9.1 (2026-10-21): CivilCraft's TIN, alignments and profiles. libs/appkit (L2, code the W5 apps share): lasso-core.js, JS's stable
  Array#sort that stays defined for inconsistent comparators, String(number). libs/civil (L3): units.js (stations,
  bearings, grouped numbers), points.js (point files in every column order, groups, lasso picks, the PNEZD writer),
  robust-predicates' orient2d followed stage by stage (tin.js reads its magnitude) and an exact incircle sign,
  Delaunator 5.1.0 (ISC) and tin.js (the constrained Delaunay surface: crossings split, Sloan's flips, Lawson,
  boundary clipping, long-edge peeling; elevations through the triangle grid, contours chained in the JS's Map order,
  bands, slopes, outlines, faces), alignment.js (PIs with circular curves, stations and offsets, polylines, ticks) and
  profile.js (PVIs, parabolic curves by L or K, high and low points, ground sampled from a surface). libs/base gained
  `js_tan` (V8's fdlibm tan: alignment curves' tangent lengths differed in the last bit with the C library's; checked bit
  for bit against Node on 3 000 000 inputs), `js_locale_en` (toLocaleString('en-US') with fixed fractions: the
  shortest decimal rounded half up, as ICU does) and `js_locale_compare` (localeCompare without numeric).
  `civilcraft-cli script|bench|ops`; tools/parity/civil.mjs runs seeded call scripts (the Rust fork's cvdiff
  generators, ours, adapted: survey coordinates, duplicates, crossings, NaN) on the frozen JS and the CLI, exact
  (tolerance 0), and skips scripts whose ops are not ported yet: seeds 1–60, units / points / TIN 49 800 calls and
  alignments / profiles 26 520 calls, 0 differ (the 0.5 tests' own scenarios, gen-tests t-main, where ported: 9 780 calls at seeds 1–30); a
  10 000- and a 100 000-point surface with breaklines (contours, bands, outline) 0 differ. Timings (in-process, best
  of 3): 10k points + 20 breaklines + contours + 2 000 elevations 469 ms JS / 94 ms C++ (5.0×); 100k + 100 breaklines
  1 884 / 626 ms (3.0×). Goldens tests/golden/js-0.5-civil.tar.gz (seed 1); fuzz targets civil_points and civil_script.
- CI.3 (2026-10-27): the fuzz smoke ran its targets one after another: 505 of a gate's ~800 test seconds with 23
  targets, and over 3 hours for the nightly's 600 s each. tools/fuzz_smoke.cmake now runs `CC_FUZZ_JOBS` targets side by
  side (default half the logical cores, at most 8; CI sets 4), one process each, and reports every failing target, not
  only the first. CI uploads the crashing and hanging inputs (fuzz-findings-<run>, kept 30 days) when the fuzz job fails:
  twice a nightly's input was lost and had to be found again.
