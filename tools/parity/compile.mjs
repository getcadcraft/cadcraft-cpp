#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Draw-list parity: BimCraft 0.5's compileDrawing (the frozen copy the parity kit pins) against `cadcraft-cli compile
// <file> --json`, on every .dwg / .dxf under the given folders. Same serialisation and tolerance as the Rust fork's
// forks/tools/cdiff.mjs (Maps → value arrays, Sets and typed arrays → arrays, a JSON round trip; abs 1e-9 + rel 1e-9:
// the draw lists go through cos/sin/atan2/hypot, whose last bit differs between V8 and libm), so the forks compare.
// It also counts the files that match at the DWG reader's tolerance (rel 1e-12).
//   CADCRAFT_SITE=/path/to/getcadcraft.com node tools/parity/compile.mjs <dir|file>… [--detail] [--bench N]
//   … --golden-out <dir>: also write each JS result, canonical, as <dir>/<name>.json (tests/golden/README.md)
//   … --isolate: run each JS compile in a worker with a 2 GB heap and a 60 s limit (fuzz corpora: the JS has no size cap,
//       so a hostile file can exhaust its memory; that is reported as "JS out of memory", the C++ result must then be
//       marked truncated)
import { execFileSync } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, relative, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const site = process.env.CADCRAFT_SITE;
if (!site) { console.error('set CADCRAFT_SITE to the site repo'); process.exit(2); }
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const app = join(process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5'), 'site/public_html/assets/apps/bimcraft/dev');
const { readDwg } = await import(pathToFileURL(join(app, 'dwg/read.js')).href);
const { readDxfModel } = await import(pathToFileURL(join(app, 'dxf-model.js')).href);
const { compileDrawing } = await import(pathToFileURL(join(app, 'dwg-compile.js')).href);
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const isolate = args.includes('--isolate');
const benchRuns = args.includes('--bench') ? Number(args[args.indexOf('--bench') + 1]) || 5 : 0;
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && (args[k - 1] === '--bench' || args[k - 1] === '--golden-out')));

const files = [];
const walk = (p) => {
  const s = statSync(p);
  if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n));
  else if (/\.(dwg|dxf)$/i.test(p)) files.push(p);
};
for (const r of roots) walk(resolve(r));

const dxfText = (b) => {
  let text = new TextDecoder('utf-8').decode(b);
  if (text.includes('�')) text = new TextDecoder('windows-1252').decode(b);
  return text;
};
const parse = (b, dxf) => (dxf ? readDxfModel(dxfText(b)) : readDwg(b));
const plain = (v) => {
  if (v instanceof Map) return [...v.values()].map(plain);
  if (v instanceof Set) return [...v].map(plain);
  if (ArrayBuffer.isView(v)) return Array.from(v);
  if (Array.isArray(v)) return v.map(plain);
  if (v && typeof v === 'object') { const o = {}; for (const k of Object.keys(v)) o[k] = plain(v[k]); return o; }
  return v;
};
/** As the C++ CLI: the drawing, its xrefs ("<name>.dwg" beside it, any case), its version tag. */
function jsInput(f) {
  const bytes = new Uint8Array(readFileSync(f));
  const isDxf = /\.dxf$/i.test(f);
  const db = parse(bytes, isDxf);
  const xrefs = new Map();
  for (const b of (db.blocks || []).filter((r) => r.flags & 4)) {
    const cand = readdirSync(dirname(f)).find((n) => n.toLowerCase() === (b.name + '.dwg').toLowerCase());
    if (cand) xrefs.set(b.name.toLowerCase(), parse(new Uint8Array(readFileSync(join(dirname(f), cand))), false));
  }
  return { db, opts: { versionTag: isDxf ? '' : new TextDecoder().decode(bytes.subarray(0, 6)), xrefs }, mb: bytes.length / 1048576 };
}

/** compileDrawing in a worker: { js } or { err } ('JS out of memory' / 'JS timed out' / the thrown message). */
function jsIsolated(f) {
  const code = `
    const { parentPort, workerData } = require('node:worker_threads');
    (async () => {
      const { canon } = await import(workerData.canon);
      const { compileDrawing } = await import(workerData.compile);
      const { readDwg } = await import(workerData.dwg);
      const { readDxfModel } = await import(workerData.dxf);
      const fs = require('node:fs'), path = require('node:path');
      const f = workerData.file, bytes = new Uint8Array(fs.readFileSync(f)), isDxf = /\\.dxf$/i.test(f);
      const dxfText = (b) => { let t = new TextDecoder('utf-8').decode(b); if (t.includes('\\ufffd')) t = new TextDecoder('windows-1252').decode(b); return t; };
      const parse = (b, dxf) => (dxf ? readDxfModel(dxfText(b)) : readDwg(b));
      const plain = (v) => v instanceof Map ? [...v.values()].map(plain) : v instanceof Set ? [...v].map(plain) : ArrayBuffer.isView(v) ? Array.from(v)
        : Array.isArray(v) ? v.map(plain) : v && typeof v === 'object' ? Object.fromEntries(Object.keys(v).map((k) => [k, plain(v[k])])) : v;
      try {
        const db = parse(bytes, isDxf), xrefs = new Map();
        for (const b of (db.blocks || []).filter((r) => r.flags & 4)) {
          const cand = fs.readdirSync(path.dirname(f)).find((n) => n.toLowerCase() === (b.name + '.dwg').toLowerCase());
          if (cand) xrefs.set(b.name.toLowerCase(), parse(new Uint8Array(fs.readFileSync(path.join(path.dirname(f), cand))), false));
        }
        const d = compileDrawing(db, { versionTag: isDxf ? '' : new TextDecoder().decode(bytes.subarray(0, 6)), xrefs });
        parentPort.postMessage({ js: JSON.stringify(canon(JSON.parse(JSON.stringify(plain(d))))) });
      } catch (e) { parentPort.postMessage({ err: String(e?.message || e) }); }
    })();`;
  return new Promise((done) => {
    const w = new Worker(code, { eval: true, resourceLimits: { maxOldGenerationSizeMb: 2048 },
      workerData: { file: f, canon: pathToFileURL(join(site, 'tools/fork-parity/lib/canon.mjs')).href, compile: pathToFileURL(join(app, 'dwg-compile.js')).href,
        dwg: pathToFileURL(join(app, 'dwg/read.js')).href, dxf: pathToFileURL(join(app, 'dxf-model.js')).href } });
    const t = setTimeout(() => { w.terminate(); done({ err: 'JS timed out' }); }, 60000);
    w.on('message', (m) => { clearTimeout(t); w.terminate(); done(m.js ? { js: JSON.parse(m.js) } : m); });
    w.on('error', (e) => { clearTimeout(t); done({ err: /out of memory|ERR_WORKER_OUT_OF_MEMORY/i.test(String(e?.code || e)) ? 'JS out of memory' : String(e?.message || e) }); });
  });
}

let same = 0, strict = 0, jsOom = 0;
const rows = [];
for (const f of files) {
  const name = relative(testfiles, f);
  let js, jsErr = null, input = null;
  if (isolate) {
    const r = await jsIsolated(f);
    js = r.js; jsErr = r.err ?? null;
  } else {
    try { input = jsInput(f); js = canon(JSON.parse(JSON.stringify(plain(compileDrawing(input.db, input.opts))))); } catch (e) { jsErr = String(e?.message || e); }
  }
  if (goldenOut && js) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f) + '.json'), JSON.stringify(js)); }
  let cpp, cppErr = null;
  try {
    cpp = canon(JSON.parse(execFileSync(cli, ['compile', f, '--json'], { maxBuffer: 1 << 30, stdio: ['ignore', 'pipe', 'pipe'] }).toString()));
  } catch (e) {
    try { cppErr = JSON.parse(String(e.stdout)).$error.message; } catch { cppErr = String(e.message).slice(0, 200); }
  }
  const cppHuge = cppErr && /too long|longer than|maxBuffer|ENOBUFS/i.test(cppErr);   // a draw list past what Node can hold as one string
  if ((jsErr === 'JS out of memory' || jsErr === 'JS timed out') && ((!cppErr && cpp?.stats?.truncated) || cppHuge)) {
    jsOom++;  // the JS ran out of memory or time where the C++ stopped at its caps: the accepted outcome
    continue;
  }
  if (jsErr || cppErr) {
    if (jsErr === cppErr) same++, strict++;
    else console.log(`DIFF ${name}: JS ${jsErr ? 'threw ' + jsErr : 'ok'}, C++ ${cppErr ? 'failed ' + cppErr : 'ok'}`);
    continue;
  }
  const d = diff(js, cpp, { abs: 1e-9, rel: 1e-9, max: 1e7 });
  if (d.length === 0) {
    same++;
    if (diff(js, cpp, { abs: 1e-9, rel: 1e-12, max: 1 }).length === 0) strict++;
  } else {
    console.log(`DIFF ${name}: ${d.length}`);
    for (const x of d.slice(0, detail ? 30 : 3)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 90)} cpp=${JSON.stringify(x.b)?.slice(0, 90)}`);
  }
  if (benchRuns && input) {
    const t = [];
    compileDrawing(input.db, input.opts);
    for (let k = 0; k < benchRuns; k++) { const t0 = performance.now(); compileDrawing(input.db, input.opts); t.push(performance.now() - t0); }
    const out = execFileSync(cli, ['compile', f, '--bench', String(benchRuns)]).toString();
    const cppMs = Number(/= ([\d.]+) ms/.exec(out)?.[1]);
    rows.push({ name, mb: input.mb, js: Math.min(...t), cpp: cppMs, same: d.length === 0 });
  }
}
console.log(`${same}/${files.length} match (abs 1e-9 + rel 1e-9); ${strict}/${files.length} also at rel 1e-12` +
  (jsOom ? `; ${jsOom} where the JS ran out of memory or time and the C++ stopped at its caps (truncated)` : ''));
if (rows.length) {
  rows.sort((a, b) => b.mb - a.mb);
  console.log('| File | Size (MB) | 0.5 JS compile (ms) | C++ compile (ms) | Speed-up | Parity |');
  console.log('|---|---|---|---|---|---|');
  for (const r of rows) console.log(`| ${r.name} | ${r.mb.toFixed(2)} | ${r.js.toFixed(2)} | ${r.cpp.toFixed(2)} | ${(r.js / r.cpp).toFixed(2)}× | ${r.same ? 'same' : 'DIFF'} |`);
}
process.exit(same + jsOom === files.length ? 0 : 1);
