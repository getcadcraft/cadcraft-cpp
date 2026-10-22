# Roadmap

Last updated 2026-10-21 · Change: M7 plan (W3: cc-ifc + BimCraft), M7.1 done.

The C++ fork of CadCraft. The reference is the CadCraft 0.5 JavaScript build; **speed decides**, and **parity on the
corpus is the gate** (AGENTS.md section 15). Every milestone ends with a short report: what works, parity, timings,
sanitizer and fuzz status, a screenshot, what's next. The Rust fork runs the same milestones in parallel; both use the
benchmark table format below so the numbers compare.

| # | Milestone | Ends with | Status |
|---|---|---|---|
| M1 | Source tree + gate | `cmake/` (cc_library with layer check, hardening, sanitizer/fuzz presets), CMakePresets.json, vcpkg.json, .clang-tidy, .clang-format, `tools/*.cmake` (format, layers, tidy, assets, licenses, osv, identity, corpus, fuzz smoke), CI workflow incl. ASan/UBSan, TSan, MSVC + GCC jobs, wasm preset; CI green on the empty skeleton | done |
| M2 | cc-dwg reader in C++ (`libs/io-dwg`) | one bounds-checked reader; parity with `lib/dwg` readDwg on the corpus (six versions + ODA truth); libFuzzer target clean under ASan/UBSan; `cadcraft-cli info` / `convert --json`; the out-of-process importer helper; benchmark vs JS readDwg on the largest files; parity numbers + timings | done: 78/78; also DXF 104/104 (M3.1) |
| M3 | CadCraft 2D viewer | SDL3 + Dear ImGui + WebGPU (Dawn / emdawnwebgpu); open DWG/DXF, model + sheets, pan/zoom, layers; desktop and web; deployed to cpp.getcadcraft.com through `forks/deploy.sh`; frame-time benchmark on the big drawings | done: draw lists 202/202; live on cpp.getcadcraft.com (?sample, ?bench) |
| M3.5 | Speed (added at the re-plan) | native reader ≥ 3× the JS on every corpus file (was 1.1–3.3×; Rust 2.1–4.4×); browser open → first frame ≥ 1.5× the JS (was on par) | done (a–h): 1.66–3.95×, browser cold ~1.4×; targets not met yet, levers in docs/perf-postmortem.md |
| M4 | CadCraft drafting commands | coroutine commands that ask questions (LINE … TRIM), undo, save DWG/DXF at parity with `dwg-save.js` | done 2026-10-18: import, document, DWG writer, DWG and DXF save, the command line (20 commands), drafting in the viewer (snaps, grips, windows, save) |
| M5 | CadCraft complete | the rest of commands.js (about 150 commands) in batches by area, CadCraft's own DXF reader, the panels (layers, properties), sheets and plotting — each batch at parity through tools/parity/commands.mjs scripts | planned (M5 plan below) |
| M6+ | The other apps | m3d kernel + ModelCraft (Manifold behind `SolidOps`), then cc-ifc + BimCraft, the cc-skp port (OpenSKP C++ is the oracle and the time to beat), the other apps. **Re-planned 2026-10-21: fork 0.5, workstreams W1–W6 below.** | planned |

## Release plan (owner, 2026-10-21): two versions behind

The forks (this one and the Rust fork) release two minor versions behind CadCraft: **when CadCraft 0.7 is released,
fork 0.5 is released; when 0.9 is released, fork 0.7.** A fork release `0.N` means parity with the frozen JS `0.N`
(`testfiles/fork-parity/js-0.N`, read-only), checked by the parity scripts and goldens as today.

| CadCraft (JS) released | Fork released | Parity target | Frozen copy |
|---|---|---|---|
| 0.7 (owner, 2026-10-21: within about a month) | **0.5** | js-0.5 | `fork-parity/js-0.5` (site 594ab3e, exists) |
| 0.9 | 0.7 | js-0.7 | `fork-parity/js-0.7`, frozen from the 0.7 release commit when 0.7 ships |

- **Fork 0.5 scope (owner): everything in 0.5** — the eight apps (CadCraft, BimCraft, CivilCraft, DetailCraft,
  MarkupCraft, ModelCraft, PartCraft, TakeoffCraft) and Studio, at parity with js-0.5. A feature not at parity is a row
  in docs/gaps.md and blocks the release unless the owner waives it.
- The release itself (tag, packages, signing, links, announcements) still waits for the owner (AGENTS.md section 15):
  the fork reports "0.5 ready" with the parity numbers.
- After fork 0.5, the work target moves to js-0.7: the 0.5 → 0.7 JS diff is ported as its own milestones.
- Size (2026-10-21, unique JS lines once files the apps share are counted once): about 226 k in the eight apps,
  22.5 k in the engines (lib/dwg, ifc, skp, shapes), 10 k in the shell (assets/js). Ported: lib/dwg and most of
  CadCraft. Left: about 190 k lines in about a month, against about 8–10 k lines a day for one stream so far — so
  fork 0.5 runs as **parallel workstreams** (owner, 2026-10-21), each in its own git worktree and build directory,
  owning its own libraries (AGENTS.md section 11, Parallel agents); one integrator rebases, gates and pushes each
  commit. A burn-down (lines ported, parity counts per app) is reported weekly; a slip is reported as soon as it shows.

### Fork 0.5 workstreams

| W | Stream | JS (unique lines) | Libraries (new unless noted) | Parity gate |
|---|---|---|---|---|
| W1 | CadCraft complete: LISP (M5.8i), the rest of gaps, the 3D tab once W2's kernel lands | lisp/ 4.4 k + rest | libs/lisp, libs/cmd (existing) | command scripts, LISP scripts, 0.5 test-cadcraft-lisp cases |
| W2 | m3d kernel + ModelCraft (Manifold behind `SolidOps`) | 14 k + shared 22 k | libs/m3d (existing), libs/solid (M6.1: Manifold 3.5.4, 10,004 / 10,004 cases bit-identical to the wasm), libs/modelcraft | model JSON, mesh / export bytes |
| W3 | cc-ifc + BimCraft | 54 k + lib/ifc 6 k | libs/io-ifc, libs/bimcraft | IFC model JSON on the corpus, BimCraft documents |
| W4 | PartCraft | 52 k | libs/partcraft | documents, exports |
| W5 | CivilCraft, DetailCraft, MarkupCraft, TakeoffCraft | 46 k | one library each | each app's documents and exports |
| W6 | Studio (the shell: assets/js, app switching, control channel and MCP per control.rs) + cc-skp | 10 k + lib/skp 2.5 k | libs/live, libs/shell, libs/io-skp | MCP tool shapes (host.js / AGENTS-SPEC.md), Live vectors, SKP models (beat OpenSKP's time) — **M13.1 done 2026-10-21** (libs/live: MessagePack, envelope, session decisions; vectors.json whole) |

Order inside each stream: the JS tests and goldens first, then the engine, then the UI. Code that several apps share
in 0.5 (the same file in several app folders) is ported once, by the first stream that needs it, into a shared library
the others link. M5.8h2 (a cloud's coordinate system) still waits for the owner's decision on the EPSG catalog.

## Re-plan after M3 (measured 2026-10-16, quiet machine)

| Measure | 0.5 JS | C++ | Rust |
|---|---|---|---|
| DWG parse, 8 largest corpus files (kit bench, best of 7) | 21.9–60.9 ms | 8.2–44.0 ms (1.1–3.3×) | 6.1–24.7 ms (2.1–4.4×) |
| Draw lists (compile), largest files | 0.4–8.9 ms | 0.06–5.7 ms (1.3–6.7×) | — |
| Browser, kitchen DXF, open → first frame (cold / warm) | 188–209 / 43–88 ms | 192–201 / 58–87 ms | — |
| Native: scene build / CPU per frame | — | 19–21 ms / < 0.02 ms | — |

What the numbers say: the reader logic is fast, but the model is a generic JS-shaped `Value` tree (small allocations,
linear key search), and on the web that runs on one thread. M3.5 therefore comes before M4:

1. **The model's shape** (done, M3.5e–g; review: docs/perf-postmortem.md): copies and regrowth fixed, compact points,
   append for new members; DXF −13 % best / −24 % median, DWG 0–5.5 %. Interned keys not done (§4 rules; bytes
   measured free). Next levers: blocks-and-tables-imperial.dwg vs Rust, the DWG decode split, the web parse in a worker.
2. **Threads on the web**: the read in a worker (pthreads are on; COOP/COEP are sent), so a big file never blocks a
   frame and a trap costs only the worker.
3. **First frame** (done, M3.5c): pipelines compiled in the background at start (web), the ASCII glyphs of the atlas
   built ahead, the `?sample` file fetched while the device is still being set up. Not yet measured on a quiet machine.
4. **Touch input** for the web build (done, M3.5d): pinch zoom about the fingers, two-finger pan, one-finger drag,
   double tap = fit; a touch layout with 40 px hit areas from the first finger until a real mouse moves.
5. **Hashed file names** for the test host (done, M3.5b): `web-dist` target, immutable caching.

Then M4 as planned; M5+ order unchanged.

## Benchmark table format (shared with the Rust fork)

The table comes from the parity kit, `node tools/fork-parity/bench.mjs --engine dwg --forks js,rust,cpp` in the site
repo (forks-infra; README § Benchmark table). Timings are in-process after one warm-up: open = reading the file into
memory, parse = the engine call on those bytes; the table shows the best run, the detail table the median.
Speed-up = JS ÷ fork. Parity = the kit's verdict with its per-engine tolerance (dwg: abs 1e-9 + rel 1e-12):
"same" or "<n> paths".

| File | Version | Size (MB) | 0.5 JS (ms) | Rust (ms) | Speed-up | Parity | C++ (ms) | Speed-up | Parity |
|---|---|---:|---:|---:|---:|---|---:|---:|---|
| example-2007.dwg | AC1015 (R2000) | 0.64 | 23.3 | 7.29 | 3.20× | same | 8.33 | 2.80× | same |
| blocks-and-tables-imperial.dwg | AC1015 (R2000) | 0.63 | 60.9 | 21.1 | 2.89× | same | 26.1 | 2.33× | same |
| example-2010.dwg | AC1015 (R2000) | 0.63 | 26.9 | 6.08 | 4.43× | same | 8.21 | 3.28× | same |
| example-2000.dwg | AC1015 (R2000) | 0.55 | 25.5 | 10.9 | 2.35× | same | 11.4 | 2.23× | same |
| kitchen-equipment-r2000.dwg | AC1015 (R2000) | 0.55 | 48.0 | 20.9 | 2.29× | same | 44.0 | 1.09× | same |
| kitchen-equipment.dwg | AC1015 (R2000) | 0.55 | 52.1 | 24.7 | 2.11× | same | 41.1 | 1.27× | same |
| example-2010.dwg | AC1021 (R2007) | 0.43 | 21.9 | 9.21 | 2.38× | same | 13.0 | 1.69× | same |
| example-2007.dwg | AC1021 (R2007) | 0.43 | 24.0 | 7.70 | 3.11× | same | 10.1 | 2.37× | same |

(2026-10-16 13:50 UTC, `bench.mjs --engine dwg --forks js,rust,cpp --largest 8 --runs 7`.)

CLI contract: `cadcraft-cli parity dwg <file>` (the model as JSON) and `cadcraft-cli bench dwg <file> --runs N`.

## Tablets (AGENTS.md section 10, Mobile)

CadCraft 0.5 Studio runs on Android and iPad. The fork covers tablets **through the web build in the tablet's
browser first** (WebGPU: Chrome on Android, Safari 26+ on iPad): a touch layout (pinch/pan and 40 px hit areas:
done in M3.5d; long press = Enter comes with the drafting commands in M4), the browser file picker and downloads, recovery in OPFS. SDL3 builds for Android and iOS exist,
but Dear ImGui is not touch-first; a packaged tablet app is decided with measured numbers at the M3 re-plan, not
before.

## M4 plan (2026-10-17)

The reference is CadCraft 0.5's own pipeline: a file is read (cc-dwg / dxf.js), turned into a **drawing** (plain data:
entities, layers, blocks, layouts, meta: `dwg-import.js` `importDwg`), loaded into the **document** (`doc.js`: one undo
step per change), edited by **commands** (`commands.js`), and saved by `dwg-save.js` through cc-dwg's writer
(`lib/dwg/*-write.js`, six versions) or as DXF. Every stage has a JSON or byte form, so each step below is gated by
parity on the corpus with no new test files: corpus DWG → read (M2) → import → save → bytes, JS against C++.

| Step | Ends with | Parity gate |
|---|---|---|
| M4.1 | `libs/cad` (L2): the drawing type and `import_dwg` (dwg-import.js) | the drawing's JSON = JS importDwg's on the 110 corpus DWGs — **done 2026-10-17: 110/110** |
| M4.2 | `libs/doc` (L3): Doc with spaces, layers, blocks, change/undo/redo | the loaded document = the JS Doc's on the corpus — **done 2026-10-17: 110/110**; behaviour tests |
| M4.3 | `libs/io-dwg` writer (bits, CRC, container R2000…R2018, objects, tables, R2004 LZ, R2007) | bytes = JS writeDwg's for the same writer model, six versions — **done 2026-10-17: 648/648 byte-identical** |
| M4.4 | `dwg-save.js` port (document → writer model) | corpus DWG → import → save: bytes = JS, six versions; and the saved files read back by our reader — **done 2026-10-18: 648/648 byte-identical** |
| M4.5 | DXF save (dxf.js writeDxf R12, dxf2000.js writeDxf2000) | bytes = JS on the corpus — **done 2026-10-18: 216/216 byte-identical** (108 DWGs × R12 and R2000) |
| M4.6 | Commands as coroutines (LINE, PLINE, CIRCLE, ARC, TEXT, MOVE, COPY, ROTATE, SCALE, ERASE, OFFSET, TRIM, EXTEND, UNDO/REDO) through `Session::execute`, scripted input; the CLI runs them headless | the JS command tests ported; command scripts give the same drawing JSON — **M4.6a done 2026-10-18**: libs/cmd, the engine and 17 commands (LINE PLINE CIRCLE XLINE RAY ARC RECTANG POLYGON TEXT ERASE MOVE COPY ROTATE SCALE MIRROR UNDO REDO), 28/28 scripts the same as the 0.5 Engine (prints, prompts, model, undo labels); **M4.6b done 2026-10-18**: OFFSET, TRIM (Quick / Standard, fence, crossing, edge mode, erase), EXTEND, on geom.js's editing geometry (libs/cad edit.hpp) — 45/45 scripts |
| M4.7 | Drafting in the viewer: command line, prompts in the status bar, snaps, grips; Save / Save as (desktop picker, web download) | screenshots against the 0.5 app; save → reopen — **M4.7a done 2026-10-18**: libs/docview, walk.js (what a document draws: colours, linetype dashes, pattern hatches, widths, masks), 108/108 corpus drawings draw the same event for event; **M4.7b done**: the viewer opens a DWG as the CadCraft document and draws it through walk (docview::to_drawing, rebuilt after each change); the command line under the canvas (prompts, typed input, Enter / Space / right click, Esc, Ctrl+Z / Ctrl+Y), clicks to the waiting prompt, the rubber band and the selection over the view, noun-verb picks, layers turned on and off in the document — checked by screenshots (Xvfb); **M4.7c done**: object snaps (end, mid, centre, quadrant, intersection, perpendicular, node, insertion; F3) and ortho (F8) for every point prompt, selection windows (left → right inside, right → left crossing) idle and at Select objects, grips on the selection (click a grip, click where it goes: one undo step); **M4.7d done**: Save (Ctrl+S: the drawing's own format and DWG version, in place on the desktop when it came from such a file, else the save dialog; a download on the web) and Save as (Ctrl+Shift+S: DWG 2000–2018, DXF 2000, DXF R12), the report's notes in the command history — checked under Xvfb: save in place, reopen with the circle drawn |

## M5 plan (2026-10-18): CadCraft complete

Each step ports commands.js commands with the geom.js / helper code they use, parity-checked by command scripts
(tools/parity/commands.mjs) against the frozen 0.5 Engine, then shown in the viewer.

| Step | Commands and pieces |
|---|---|
| M5.1a | ELLIPSE, POINT, DUPLICATE, SELECTALL, DESELECT, EXPLODE, STRETCH, BREAK, BREAKATPOINT, LENGTHEN, JOIN (geom.js stretchEntity, breakEntity, lengthenEntity, joinEntities, explodeEntity) |
| M5.1b | FILLET, CHAMFER (geom.js corners) |
| M5.1c | ARRAY / ARRAYRECT / ARRAYPOLAR, REVCLOUD, MATCHPROP, MTEXT, MLEADER, TEXTEDIT |
| M5.1d | HATCH (hatch.js patterns, boundaries) |
| M5.2 | dimensions: DIMLINEAR, DIMALIGNED, DIMRADIUS, DIMDIAMETER, DIMANGULAR, DIMCONTINUE, DIMBASELINE, DIMLUNIT |
| M5.3a | blocks: BLOCK, ATTDEF, INSERT, PURGE |
| M5.3b | the block editor (BEDIT … BCLOSE, BVSTATE / BVHIDE / BVSHOW), DELBLOCKS |
| M5.4a | inquiry: DIST, AREA, MEASUREGEOM, ID, LIST, UNITS |
| M5.4b | selection: SELECT, QSELECT, SELECTSIMILAR, FIND |
| M5.4c | views: ZOOM, NAMEDVIEW, UCS and the turned views, PLAN |
| M5.5a | the layer tools (layertools.js): LAYISO … LAYDEL, RENAME, COUNT, LINETYPE, LTSCALE, LWEIGHT, XREF ? |
| M5.5b | the properties model: expr.js, propexpr.js (typed values), propdefs.js (every type's fields, shared values, one undo step) — **done 2026-10-19** |
| M5.5c | PROPERTIES / PROPERTIESCLOSE / QPMODE / _PROPPICK and the properties panel, the layers panel's switches — **done 2026-10-19** |
| M5.6 | sheets and plotting: LAYOUT, MVIEW, VPORTS, MSPACE / PSPACE, VPLAYER, PLOT (PDF), split screen — **M5.6a done 2026-10-20** (the sheet commands); **M5.6b done 2026-10-20** (plot.js, the PDF writer, `cadcraft-cli plot`, 307 PDFs byte for byte); **M5.6c done 2026-10-20** (the tabs follow the document, PLOT in the viewer); MSPACE / PSPACE inside a viewport and split panes wait (docs/gaps.md) |
| M5.7 | files: CadCraft's DXF reader (dxf.js readDxf) so a DXF opens as a document, NEW, OPEN, the clipboard commands — **M5.7a done 2026-10-20** (the reader, 214/214 documents); **M5.7b done 2026-10-20** (the clipboard, NEW / OPEN in the viewer) |
| M5.8 | constraints and parameters (gcons.js, params.js), xrefs, pictures, sheet sets — **M5.8a done 2026-10-20** (TTR / TTT, TEXTMASK, XCLIP, the settings commands, HELP); **M5.8b done 2026-10-20** (picture commands, the drawing's picture files); **M5.8c done 2026-10-20** (pictures in PDFs); **M5.8d done 2026-10-20** (pictures on screen, wuffs); **M5.8e done 2026-10-20** (constraints and parameters: solver, parameter table, the commands); **M5.8f done 2026-10-20** (constraints in saved files, through MIRROR, TRIM and the clipboard); **M5.8g done 2026-10-20** (the viewer's constraint overlay and Parameters panel); **M5.8g2 done 2026-10-20** (SWBOUT, SolidWorks sketch exports opened); **M5.8h1 done 2026-10-20** (point clouds: libs/cloud reader); M5.8h2 coordinate systems (waits on the owner: the EPSG-derived catalog), **M5.8h3 done 2026-10-21** (LAZ, laz-perf), **M5.8h4 done 2026-10-21** (POINTCLOUDATTACH / POINTCLOUD, the cloud under the drawing, Node snap), **M5.8h5 done 2026-10-21** (VIEWBASE); M5.8i AutoLISP (APPLOAD, ALIASEDIT, REINIT; lisp/) |

## M7 plan (W3, 2026-10-21): cc-ifc + BimCraft

cc-ifc (lib/ifc, 6.3 k lines) is ported first and gated by the parity kit's two IFC engines on the corpus
(`run.mjs --fork cpp --engine ifc-step,ifc-geom`), then BimCraft (54 k lines) on top of it.

| Step | Ends with | Parity gate |
|---|---|---|
| M7.1 | `libs/io-ifc` STEP reader (step.js), `cadcraft-cli parity / bench ifc-step`, fuzz target `ifc_step`, tools/parity/ifc-step.mjs (differential on any files) | kit ifc-step 118/118 exact — **done 2026-10-21** |
| M7.2 | schema tables (schema.js, generated from the 0.5 tables), model.js (IfcFile: lines, types, edits, save), api.js's line and property calls | model calls against the JS on the corpus |
| M7.3 | geometry without booleans: geo-math, geo-mesh, geo-tri, geo-ctx, geo-style, geo-curve, geo-profile, geo-solid, geo-brep, geo-nurbs, geo-align, geometry.js; `parity / bench ifc-geom` | kit ifc-geom on the files with no opening or boolean |
| M7.4 | geo-bool.js on Manifold 3.5.4 (W2's cmake/Manifold.cmake, SolidOps) | kit ifc-geom 115/115 |
| M7.5 | the importer helper for IFC (out of process, as DWG); fuzz targets for the model and the geometry | the helper's model = in-process on the corpus |
| M7.6+ | BimCraft: its document and commands (ifc-model.js, ifc-edit.js, ifc-props, ifc-tree, ifc-find, ifc-qto, …), then the UI on W2's 3D view | test-bimcraft cases that pass on js-0.5, BimCraft documents |

