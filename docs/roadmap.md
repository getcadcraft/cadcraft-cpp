# Roadmap

Last updated 2026-10-18 · Change: M4.6a done (the command line, 17 commands at parity); M4.6b (OFFSET, TRIM, EXTEND) next.

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
| M4 | CadCraft drafting commands | coroutine commands that ask questions (LINE … TRIM), undo, save DWG/DXF at parity with `dwg-save.js` | in progress: M4.1–M4.5 and M4.6a done (import, document, DWG writer, DWG save, DXF save, the command line); M4.6b next |
| M5+ | The rest | m3d kernel + ModelCraft (Manifold behind `SolidOps`), then cc-ifc + BimCraft, the cc-skp port (OpenSKP C++ is the oracle and the time to beat), the other apps. **Re-planned with measured numbers after M3.** | planned |

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
| M4.6 | Commands as coroutines (LINE, PLINE, CIRCLE, ARC, TEXT, MOVE, COPY, ROTATE, SCALE, ERASE, OFFSET, TRIM, EXTEND, UNDO/REDO) through `Session::execute`, scripted input; the CLI runs them headless | the JS command tests ported; command scripts give the same drawing JSON — **M4.6a done 2026-10-18**: libs/cmd, the engine and 17 commands (LINE PLINE CIRCLE XLINE RAY ARC RECTANG POLYGON TEXT ERASE MOVE COPY ROTATE SCALE MIRROR UNDO REDO), 28/28 scripts the same as the 0.5 Engine (prints, prompts, model, undo labels); **M4.6b**: OFFSET, TRIM, EXTEND |
| M4.7 | Drafting in the viewer: command line, prompts in the status bar, snaps, grips; Save / Save as (desktop picker, web download) | screenshots against the 0.5 app; save → reopen |
