# CadCraft in C++: one page

Stage: fork 0.5 (parity with CadCraft 0.5 in every app; released when CadCraft 0.7 is — docs/roadmap.md, Release plan). Milestones: [docs/roadmap.md](docs/roadmap.md). Session log:
[docs/progress.md](docs/progress.md). Architecture and toolchain: [docs/architecture.md](docs/architecture.md).

## Headline numbers

DWG reader parity (tools/fork-parity, 2026-10-16): **78 / 78 match**, 0 known differences.

First timings (bench.mjs, best of 5, 2026-10-16, machine busy with three compiles — re-measured in M2.2):

| File | Version | Size (MB) | 0.5 JS (ms) | C++ (ms) | Speed-up | Parity |
|---|---|---:|---:|---:|---:|---|
| example-2010.dwg | AC1015 (R2000) | 0.63 | 33.1 | 8.51 | 3.89× | same after M2.1 |
| example-2000.dwg | AC1015 (R2000) | 0.55 | 41.4 | 12.4 | 3.34× | same after M2.1 |
| blocks-and-tables-imperial.dwg | AC1015 (R2000) | 0.63 | 57.1 | 29.0 | 1.97× | same after M2.1 |
| kitchen-equipment.dwg | AC1015 (R2000) | 0.55 | 54.3 | 56.4 | 0.96× | same after M2.1 |

## Progress log

- 2026-10-21 M9.1: CivilCraft's TIN, alignments and profiles (libs/civil) exact against the 0.5 JS on 76 000 seeded calls; 3–5× faster.
- 2026-10-16 M3.3: the 2D viewer on desktop and web (SDL3 + Dawn / emdawnwebgpu, ImGui); open → first frame in the browser on par with 0.5 JS, not faster yet.
- 2026-10-16 M3.2: draw lists (dwg-compile.js) at parity on 202/202; differential fuzzing JS vs C++ (2728 inputs match).
- 2026-10-16 M3.1: io-dxf (BimCraft dxf-model.js) at parity on 104/104 corpus DXFs; fuzz_dxf; base JS-semantics helpers.
- 2026-10-16 M2.4: reader unit tests ported; long fuzz run, three fixes with regression inputs.
- 2026-10-16 M2.2: sandboxed import helper, JSON parser, golden parity test in CI.
- 2026-10-16 M2.1: io-dwg reader at parity with readDwg on the whole corpus (78/78).

- 2026-10-16 M1: toolchain (LLVM 22, CMake 4.4.4, Ninja 1.13.2, vcpkg 2026.07.29, emsdk 6.0.10), source tree, gate.
