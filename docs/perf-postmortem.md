# Post-mortem: the M3.5 speed work (C++ fork)

Last updated 2026-10-18 · Change: the DWG writer and save (M4.3a, M4.4) added.

For review by the owner. What M3.5 set out to do, what each change measured, what was kept, what was dropped and why,
and where the time goes now. Every number here was measured on this machine (8 cores, shared with other sessions);
how, is in [Method](#method).

## Summary

| | Before M3.5 (M3.4, 2026-10-16) | After M3.5g (2026-10-17) |
|---|---|---|
| DWG parse vs 0.5 JS, 8 largest corpus files (kit table) | 1.09–3.28× | **1.66–3.95×** |
| DWG parse, kitchen-equipment.dwg (R2000) | 41.1 ms | **25.4 ms** |
| DXF parse, kitchen-equipment.dxf (1.4 MB) | ~26–28 ms | **~22–24 ms** (−13 % best, −24 % median) |
| Allocations per read, kitchen DWG / DXF | 244k / 320k | **194k / 132k** |
| Browser, kitchen DXF, open → first frame, cold | 192–201 ms (JS: 188–209) | **124–149 ms** |
| Browser, same, warm | 58–87 ms (JS: 43–88) | 45–133 ms (noisy; parse 35–42 ms at best) |
| Parity | DWG 78/78, DXF 104/104, draw lists 202/202 | unchanged; every step byte-identical on the corpus |

- **The big DWG win was threads** (M3.5a: objects decoded on up to 8 threads): kitchen DWG 34.8 → 25.1 ms. The model
  changes after it (M3.5e–g) are worth 0–5.5 % (best) on the DWG files, but 13–24 % on DXF.
- **The first-frame work paid off in the browser**: cold open → first frame is now ~30 % under the JS build's.
- **Bytes do not matter here, allocation counts and copies do**: 24 MB fewer bytes per read measured 0 %; 62k fewer
  allocations measured −6 %.
- **Two plan items were measured and not done**, for stated reasons: deferring the frees (makes things slower) and
  interned keys (conflicts with the fork's memory-safety rules; the saving would have been bytes).
- **Targets** (docs/roadmap.md: ≥ 3× native, ≥ 1.5× browser): native reaches 3× on 3 of 8 files; the browser first
  frame is ~1.4× (cold). Not met yet; the remaining levers are below.

## The steps, one by one

Each line is an interleaved A/B against the step before it (unless noted), on the kitchen files.

| Step | What | Measured | Kept? |
|---|---|---|---|
| M3.5a | DWG objects decode on up to 8 threads (each object independent; results taken back in map order) | kitchen DWG best 34.8 → 25.1 ms | yes |
| — | Builder entity conversion on threads | 29–36 ms vs 30 ms: nothing (allocation-bound, threads contend in malloc) | no |
| — | mimalloc (web), jemalloc (native) | ≤ 8 %, within noise | no |
| M3.5b | Web files with content-hashed names, cached immutable | delivery only | yes |
| M3.5c | First frame: render pipelines compiled in the background (web), ASCII glyphs made at start, `?sample` fetched while the GPU starts | browser cold 192–201 → 148–217 ms; scene step ~20 → ~1 ms | yes |
| M3.5d | Touch input (pinch, pan, double tap, 40 px layout) | not a speed item | yes |
| M3.5e | `make_object` / DXF `spread` move their values out of the braced list (no copy); DXF entities sized once (were grown 1→32 in six steps); DXF line list reserved | DXF −6.5 %, DWG −7.9 % (median) vs M3.5d; DXF allocations 320k → 219k | yes |
| — | Free the DWG decoded records later (leaking them as the upper bound) | **1.6× slower**: the allocator reuses what was just freed; a growing heap faults in new pages | no |
| — | DXF `spread` reserving room on every call | **+10 %**: most spreads overwrite existing keys, the reserve reallocates for nothing | no |
| — | DXF line list reserve on its own | −24 MB per read, 0 % time | kept (harmless) |
| — | DWG records' field objects start at 24 members | ±5 %, noise | no |
| M3.5f | Compact points: `{x, y}` / `{x, y, z}` of numbers stored inline in `Value` (no allocation) | allocations DXF −40 %, DWG −20 %; best −10 % / −16 %, medians within noise | yes |
| M3.5g | A fresh entity's first members are appended, not searched | −3 to −5.5 % (best and median, both formats) | yes |
| — | Interned / compact member keys (Member 56 → 40–48 bytes) | not built: see below | no |

## What did not work, and why

- **Deferring frees.** The profile showed ~13 % of a DWG read freeing the decoded records. Not freeing them at all
  (the upper bound for freeing on another thread) made the read 1.6× slower: freed blocks are what the next read
  allocates from. Freeing is not waste here; allocating is.
- **Bigger reservations.** Reserving ahead only pays where the final size is known; where keys are overwritten
  (spread) it reallocates for nothing.
- **Interned keys.** File data become object keys (dictionary entry names, xdata application names, unknown class names).
  A process-wide intern table would grow without bound on hostile files, against AGENTS.md §4 ("cap every allocation
  derived from input"), and every key layout under 24 bytes needs a tagged pointer, a union or an owning raw pointer,
  which §4 also rules out. The saving would have been bytes per member, and bytes measured free. The part of key
  handling that cost time (the linear `set` search) was fixed in M3.5g instead.
- **Allocators.** mimalloc and jemalloc gave ≤ 8 %: the cost was the number of allocations and copies, not the
  allocator.

## Where the time goes now

perf, frame-pointer release build, kitchen files, samples inside the timed read only:

- DXF: the group reader and text entities (`read_dxf_model`, `text_ent`, `decode_text`, `js_trim`, `js_parse_float`),
  `Value::set` searches in the spreads, `Value` construction/destruction dispatch.
- DWG: the model builder (`convert`, `type_fields` ≈ 60–75 % inclusive), freeing the decoded records (~13 %),
  `attach_data_storage`, decompression (`lz2004`).
- Browser: the DXF parse is ~2× native when warm (35–42 ms vs ~22 ms) and dominates open → first frame; the scene and
  upload are 1–2 ms.

## Next levers (in the plan, not done yet)

1. **blocks-and-tables-imperial.dwg**: 26 ms vs Rust's 16 ms, the one file where the fork is clearly behind Rust;
   profile it first.
2. **The DWG decode split**: fixed chunks per thread; on a busy machine the slowest thread holds the others. Small
   chunks from a shared counter should be steadier (measure on a busy and a quiet machine).
3. **The web parse in a worker** (pthreads are on): a big file never blocks a frame; and the browser's warm parse is
   2× native — look at wasm tiering and the allocator under pthreads.
4. **Fewer allocations per DWG record** (handle lists, EED, field objects): the records are what is freed.

## M4.3a / M4.4: the DWG writer and the save

The save (dwg-save.js → cc-dwg writeDwg) went in at parity first (648/648 byte-identical), then was measured: release
build, a document loaded once, best of 7 saves after 3 warm-ups, same machine, load average 5–8 (2026-10-18 02:05 UTC).
JS = the frozen 0.5 `saveDwg` in Node 24.

| File | Version | 0.5 JS (ms) | C++ (ms) | Speed-up |
|---|---|---:|---:|---:|
| example-2007.dwg | 2000 | 7.7 | 6.5 | 1.17× |
| example-2007.dwg | 2018 | 13.7 | 8.7 | 1.58× |
| civil-example-imperial.dwg | 2000 | 3.8 | 4.5 | 0.84× |
| civil-example-imperial.dwg | 2018 | 8.1 | 5.0 | 1.61× |
| blocks-and-tables-imperial.dwg | 2000 | 38.7 | 39.9 | 0.97× |
| blocks-and-tables-imperial.dwg | 2018 | 47.9 | 42.3 | 1.13× |
| architectural-annotation-scaling-and-multileaders.dwg | 2000 | 22.6 | 20.7 | 1.09× |
| architectural-annotation-scaling-and-multileaders.dwg | 2018 | 30.9 | 24.7 | 1.25× |
| kitchen-equipment.dwg | 2000 | 73.9 | 71.9 | 1.03× |
| kitchen-equipment.dwg | 2018 | 113.8 | 81.2 | 1.40× |

What changed on the way, each measured and parity-checked (648/648 after each):

| Step | Change | Effect | Kept |
|---|---|---|---|
| M4.3a | per-record `std::regex` tests in the writer → plain string tests | blocks-and-tables R2000 write 76 → 63.5 ms (regex was 12 % of the profile) | yes |
| M4.4 | the writer merged each ATTRIB's reference as a deep copy of the INSERT (its whole `attribs` array included) per attribute — quadratic; the JS spread is shallow. Now only the eight fields `common()` reads | kitchen R2000 save ~125 → ~80 ms (writer `convert` 31 → 17 % of samples) | yes |
| M4.4 | `world_attribs` copied the reference and its block (all its entities) per reference to drop constant attributes; now filtered in place | part of the line above | yes |

Where the time goes now (kitchen, R2000, frame-pointer build): the writer ~65 % of the save (record conversion 17 %,
object encoders 14 %, the rest container and tables), the save's own conversion ~20 % (references and their
attributes), and across both `Value::set` searches and member copies (~25 % together). Not a single hot spot.

Next levers (not done; the plan goes on with M4.5):

1. **The save model as typed records**, not `Value` objects: the writer reads each field once; today it is built by
   string-keyed sets and read by string-keyed lookups. The biggest lever, and a redesign of the writer's input.
2. **One dimension geometry per dimension**: `dim_text_point` builds the geometry a second time (the JS caches it per
   entity object); a cache for the save's duration.
3. **`Value::set` on objects built in a known order** (`append` where the key cannot be there yet).

## Method

- **A/B, interleaved**: `ab.sh A B <engine> <file> <rounds>` runs each binary `bench --runs 7` in turn, rounds × 2;
  reports the best of all runs and the median of the per-round medians. Ratios B/A; < 1 is faster. Load average was
  2–6 for every number in this document; runs at load 20–50 (other sessions' builds) were thrown away.
- **Parity, every step**: models (`cadcraft-cli parity`, with the timing field `report.ms` removed) and draw lists
  (`compile --json`) of every corpus file (104 DXF, 110 DWG under testfiles/) byte-compared with the step before;
  plus the unit and golden suites, ASan/UBSan, TSan and fuzz smoke in the gate, and the DXF differential fuzz inputs
  through JS and C++ (441/447 equal, the same 6 known hostile divergences before and after; docs/gaps.md).
- **Profiles**: `perf record -e cpu-clock -g` on a frame-pointer release build (the Release build's DWARF does not
  unwind here); allocation counts and sizes with a small LD_PRELOAD counter (dev tool, not in the repo).
- **Kit table**: `node tools/fork-parity/bench.mjs --engine dwg --forks js,rust,cpp --largest 8 --runs 7` in the site
  repo, 2026-10-17 17:52 UTC (JS and Rust re-measured in the same run).
- **Browser**: headless Chromium with SwiftShader WebGPU on the deployed build, `?bench` (six opens: first cold).

### The kit table, 2026-10-17 17:52 UTC

| File | Version | Size (MB) | 0.5 JS (ms) | Rust (ms) | Speed-up | Parity | C++ (ms) | Speed-up | Parity |
|---|---|---:|---:|---:|---:|---|---:|---:|---|
| example-2007.dwg | AC1015 (R2000) | 0.64 | 17.8 | 6.06 | 2.94× | same | 6.94 | 2.57× | same |
| blocks-and-tables-imperial.dwg | AC1015 (R2000) | 0.63 | 43.1 | 16.2 | 2.67× | same | 26.0 | 1.66× | same |
| example-2010.dwg | AC1015 (R2000) | 0.63 | 28.6 | 7.01 | 4.08× | same | 7.24 | 3.95× | same |
| example-2000.dwg | AC1015 (R2000) | 0.55 | 24.5 | 10.9 | 2.24× | same | 11.8 | 2.08× | same |
| kitchen-equipment-r2000.dwg | AC1015 (R2000) | 0.55 | 52.3 | 26.6 | 1.96× | same | 24.7 | 2.11× | same |
| kitchen-equipment.dwg | AC1015 (R2000) | 0.55 | 44.1 | 19.2 | 2.29× | same | 25.4 | 1.74× | same |
| example-2010.dwg | AC1021 (R2007) | 0.43 | 19.9 | 7.30 | 2.72× | same | 8.54 | 2.33× | same |
| example-2007.dwg | AC1021 (R2007) | 0.43 | 25.9 | 8.10 | 3.20× | same | 8.64 | 3.00× | same |

### C++ alone, M3.5d → M3.5g (interleaved, 8 rounds × 7 runs, B/A)

| File | best | median |
|---|---:|---:|
| AC1015 example-2007.dwg | 0.967 | 1.034 |
| AC1015 blocks-and-tables-imperial.dwg | 0.969 | 1.025 |
| AC1015 example-2010.dwg | 0.970 | 1.019 |
| AC1015 example-2000.dwg | 0.978 | 0.916 |
| AC1015 kitchen-equipment-r2000.dwg | 0.945 | 0.903 |
| AC1015 kitchen-equipment.dwg | 0.972 | 0.841 |
| AC1021 example-2010.dwg | 0.996 | 0.997 |
| AC1021 example-2007.dwg | 1.015 | 1.003 |
| kitchen-equipment.dxf | 0.871 | 0.759 |

Allocations per read, kitchen DWG 243,887 → 193,979; kitchen DXF 320,126 → 131,662 (bytes 136 → 70 MB).
