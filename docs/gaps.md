# Gaps

Last updated 2026-10-16 · Change: draw lists, collation.

What the C++ fork does not do yet that CadCraft 0.5 does, ranked by how much it matters.

| Rank | Gap | Since | Notes |
|---|---|---|---|
| 1 | The apps are not ported yet (M3 onward) | 2026-10-16 | See docs/roadmap.md |
| 2 | DWG code pages other than Windows-1252 decode as Windows-1252 (the JS uses TextDecoder for 26 of them) | 2026-10-16 | No corpus file uses one; table generation from the WHATWG encodings is the fix |
| 3 | `read_dwg` has no strict mode (the JS `strict: true` throws on the first damaged object) | 2026-10-16 | Not used by the apps |
| 4 | Out-of-process import on Windows (job object, restricted token) and macOS (sandbox profile): the helper and its client are Linux-only so far; other platforms read in-process | 2026-10-16 | M3 builds the desktop app on Windows and macOS |
| 5 | Hostile input only: where the JS reader goes on with a NaN object size (`readMS` gives `0 * 2**shift` = NaN past shift 1024; `p + NaN > len` is false), the C++ refuses the object; a damaged-object note is worded differently. Seen on fuzz input `fuzz/corpus/dwg/regress-c0449ba28405`, never on the corpus | 2026-10-16 | Arguably a JS bug (a 0.6 fix for the owner to decide); copying NaN-bounded reads would be unsafe |
| 6 | DXF: no binary DXF (the JS refuses it too, with the same words) | 2026-10-16 | Same as 0.5 |
| 7 | Hostile input only, draw lists: where the JS compiler would run out of memory or time (a MINSERT of 10⁹ cells, a block nesting that multiplies), or throw (a spline whose degree its control points cannot carry, a dimension block that contains itself), the C++ stops at its caps (`CompileOptions::max_coords`, `max_visits`, depth 200) and marks the drawing truncated, or draws the control polygon | 2026-10-16 | `tools/parity/compile.mjs --isolate` counts "JS out of memory" separately; regressions in fuzz/corpus/compile |
| 8 | `localeCompare` (layer order) is measured for ASCII and every Windows-1252 character (tools/gen/js-collation.mjs); other scripts sort by code point after Latin, where ICU interleaves scripts and applies compatibility mappings. `toLowerCase`'s final-sigma rule is not applied | 2026-10-16 | Only the order of the layer list; no corpus file has such names |
| known issue | Screen readers: Dear ImGui has no accessibility tree | 2026-10-15 | Owner, 2026-10-15: known issue, skipped for now. Never claim screen-reader support. |
