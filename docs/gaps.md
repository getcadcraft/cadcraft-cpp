# Gaps

Last updated 2026-10-16 · Change: first version.

What the C++ fork does not do yet that CadCraft 0.5 does, ranked by how much it matters.

| Rank | Gap | Since | Notes |
|---|---|---|---|
| 1 | The apps are not ported yet (M3 onward) | 2026-10-16 | See docs/roadmap.md |
| 2 | DWG code pages other than Windows-1252 decode as Windows-1252 (the JS uses TextDecoder for 26 of them) | 2026-10-16 | No corpus file uses one; table generation from the WHATWG encodings is the fix |
| 4 | Out-of-process import on Windows (job object, restricted token) and macOS (sandbox profile): the helper and its client are Linux-only so far; other platforms read in-process | 2026-10-16 | M3 builds the desktop app on Windows and macOS |
| 3 | `read_dwg` has no strict mode (the JS `strict: true` throws on the first damaged object) | 2026-10-16 | Not used by the apps |
| known issue | Screen readers: Dear ImGui has no accessibility tree | 2026-10-15 | Owner, 2026-10-15: known issue, skipped for now. Never claim screen-reader support. |
