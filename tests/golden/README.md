# Golden references

`js-0.5-dwg.tar.gz` — the CadCraft 0.5 JavaScript reader's output (cc-dwg `readDwg`, frozen at site commit 594ab3e)
for the public fixtures listed in `tools/corpus-public.txt`, in the parity kit's canonical JSON (keys sorted,
`report.ms` left out). Made with the kit (`tools/fork-parity/ref.mjs --public-out <dir>` in the CadCraft site repo),
then `tar --sort=name --owner=0 --group=0 --mtime=2026-10-16 -czf js-0.5-dwg.tar.gz dwg`. The fixtures themselves are
fetched by `tools/corpus.cmake`; the private corpus never enters this repo.

The test `io-dwg.golden` reads each fixture and compares the model with its reference, numbers within the kit's DWG
tolerance (abs 1e-9 + rel 1e-12). The full corpus check (78 files) runs with the kit on the build server.

`js-0.5-dxf.tar.gz` — BimCraft 0.5's `readDxfModel` (dxf-model.js, same frozen commit) for the three public DXF
fixtures, decoded as BimCraft decodes them (UTF-8, Windows-1252 when that gives U+FFFD), in the same canonical JSON. The
parity kit has no DXF engine, so these come from the fork's own script: `node tools/parity/dxf.mjs build/corpus/fixtures
--golden-out <dir>/dxf`, then the same `tar` line with `dxf`. The test `io-dxf.golden` compares them as above; the full
DXF check (104 files: the corpus `_truth` exports, apparel patterns, public fixtures) is `tools/parity/dxf.mjs`.
