#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// DXF parity: BimCraft 0.5's readDxfModel (the frozen copy the parity kit pins) against `cadcraft-cli parity dxf`, on
// every .dxf under the given folders. The kit has no DXF engine, so this script is the fork's own gate, with the kit's
// comparison (canon + diff, dwg tolerances abs 1e-9 + rel 1e-12).
//   CADCRAFT_SITE=/path/to/getcadcraft.com node tools/parity/dxf.mjs <dir|file>… [--detail] [--bench N]
//   … --golden-out <dir>: also write each JS model, canonical, as <dir>/<name>.json (tests/golden/README.md)
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, relative, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const site = process.env.CADCRAFT_SITE;
if (!site) { console.error('set CADCRAFT_SITE to the site repo'); process.exit(2); }
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const { readDxfModel } = await import(pathToFileURL(join(js05, 'site/public_html/assets/apps/bimcraft/dev/dxf-model.js')).href);
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const benchRuns = args.includes('--bench') ? Number(args[args.indexOf('--bench') + 1]) || 5 : 0;
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && (args[k - 1] === '--bench' || args[k - 1] === '--golden-out')));

const files = [];
const walk = (p) => {
  const s = statSync(p);
  if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n));
  else if (/\.dxf$/i.test(p)) files.push(p);
};
for (const r of roots) walk(resolve(r));

// BimCraft openDwg for kind 'dxf': UTF-8, else Windows-1252 when that gave replacement characters.
const jsRead = (data) => {
  let text = new TextDecoder('utf-8').decode(data);
  if (text.includes('�')) text = new TextDecoder('windows-1252').decode(data);
  try { return readDxfModel(text); } catch (e) { return { $error: { message: e.message } }; }
};

let same = 0;
const rows = [];
for (const f of files) {
  const data = readFileSync(f);
  const ref = jsRead(data);
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f) + '.json'), JSON.stringify(canon(ref))); }
  let out;
  try {
    out = JSON.parse(execFileSync(cli, ['parity', 'dxf', f], { maxBuffer: 1 << 30 }).toString());
  } catch (e) {
    out = e.stdout ? JSON.parse(e.stdout.toString()) : { $error: { message: String(e) } };
  }
  if (out.$error) out = { $error: { message: out.$error.message } };
  const d = diff(canon(ref), canon(out), { abs: 1e-9, rel: 1e-12, max: 1e6 });
  const name = relative(testfiles, f);
  if (d.length === 0) same++;
  else {
    console.log(`DIFF ${name}: ${d.length}`);
    for (const x of d.slice(0, detail ? 30 : 3)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 90)} cpp=${JSON.stringify(x.b)?.slice(0, 90)}`);
  }
  if (benchRuns) {
    const t = [];
    jsRead(data);
    for (let k = 0; k < benchRuns; k++) { const t0 = performance.now(); jsRead(readFileSync(f)); t.push(performance.now() - t0); }
    const b = JSON.parse(execFileSync(cli, ['bench', 'dxf', f, '--runs', String(benchRuns)]).toString());
    const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
    const js = med(t), cpp = med(b.open_ms.map((o, k) => o + b.parse_ms[k]));
    rows.push({ name, mb: data.length / 1048576, js, cpp, same: d.length === 0 });
  }
}
console.log(`${same}/${files.length} match`);
if (rows.length) {
  rows.sort((a, b) => b.mb - a.mb);
  console.log('| File | Size (MB) | 0.5 JS (ms) | C++ (ms) | Speed-up | Parity |');
  console.log('|---|---|---|---|---|---|');
  for (const r of rows) console.log(`| ${r.name} | ${r.mb.toFixed(2)} | ${r.js.toFixed(1)} | ${r.cpp.toFixed(1)} | ${(r.js / r.cpp).toFixed(2)}× | ${r.same ? 'same' : 'DIFF'} |`);
}
process.exit(same === files.length ? 0 : 1);
