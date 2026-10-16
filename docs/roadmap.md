# Roadmap

Last updated 2026-10-16 · Change: first version (milestones from the fork-cpp kick-off).

The C++ fork of CadCraft. The reference is the CadCraft 0.5 JavaScript build; **speed decides**, and **parity on the
corpus is the gate** (AGENTS.md section 15). Every milestone ends with a short report: what works, parity, timings,
sanitizer and fuzz status, a screenshot, what's next. The Rust fork runs the same milestones in parallel; both use the
benchmark table format below so the numbers compare.

| # | Milestone | Ends with | Status |
|---|---|---|---|
| M1 | Source tree + gate | `cmake/` (cc_library with layer check, hardening, sanitizer/fuzz presets), CMakePresets.json, vcpkg.json, .clang-tidy, .clang-format, `tools/*.cmake` (format, layers, tidy, assets, licenses, osv, identity, corpus, fuzz smoke), CI workflow incl. ASan/UBSan, TSan, MSVC + GCC jobs, wasm preset; CI green on the empty skeleton | in progress |
| M2 | cc-dwg reader in C++ (`libs/io-dwg`) | one bounds-checked reader; parity with `lib/dwg` readDwg on the corpus (six versions + ODA truth); libFuzzer target clean under ASan/UBSan; `cadcraft-cli info` / `convert --json`; the out-of-process importer helper; benchmark vs JS readDwg on the largest files; parity numbers + timings | in progress: reader at parity 78/78 (M2.1) |
| M3 | CadCraft 2D viewer | SDL3 + Dear ImGui + WebGPU (Dawn / emdawnwebgpu); open DWG/DXF, model + sheets, pan/zoom, layers; desktop and web; deployed to cpp.getcadcraft.com through `forks/deploy.sh`; frame-time benchmark on the big drawings | planned |
| M4 | CadCraft drafting commands | coroutine commands that ask questions (LINE … TRIM), undo, save DWG/DXF at parity with `dwg-save.js` | planned |
| M5+ | The rest | m3d kernel + ModelCraft (Manifold behind `SolidOps`), then cc-ifc + BimCraft, the cc-skp port (OpenSKP C++ is the oracle and the time to beat), the other apps. **Re-planned with measured numbers after M3.** | planned |

## Benchmark table format (shared with the Rust fork)

The table comes from the parity kit, `node tools/fork-parity/bench.mjs --engine dwg --forks js,rust,cpp` in the site
repo (forks-infra; README § Benchmark table). Timings are in-process after one warm-up: open = reading the file into
memory, parse = the engine call on those bytes; the table shows the best run, the detail table the median.
Speed-up = JS ÷ fork. Parity = the kit's verdict with its per-engine tolerance (dwg: abs 1e-9 + rel 1e-12):
"same" or "<n> paths".

| File | Version | Size (MB) | 0.5 JS (ms) | Rust (ms) | Speed-up | Parity | C++ (ms) | Speed-up | Parity |
|---|---|---|---|---|---|---|---|---|---|

CLI contract: `cadcraft-cli parity dwg <file>` (the model as JSON) and `cadcraft-cli bench dwg <file> --runs N`.

## Tablets (AGENTS.md section 10, Mobile)

CadCraft 0.5 Studio runs on Android and iPad. The fork covers tablets **through the web build in the tablet's
browser first** (WebGPU: Chrome on Android, Safari 26+ on iPad): a touch layout (pinch/pan, long press = Enter,
40 px hit areas), the browser file picker and downloads, recovery in OPFS. SDL3 builds for Android and iOS exist,
but Dear ImGui is not touch-first; a packaged tablet app is decided with measured numbers at the M3 re-plan, not
before.
