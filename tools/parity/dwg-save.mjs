#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// DWG save parity (M4.4): CadCraft 0.5 opens a DWG as readDwg → importDwg → new Doc().load() and saves it with
// saveDwg (dwg-save.js). This does that with the frozen 0.5 copy for every .dwg under the given folders, in each
// version, and compares the bytes with `cadcraft-cli dwg-save <file> <version> <out> --created <ms> --seeded` —
// byte for byte — and the report with the parity kit's canon + diff.
// Both sides get the same date and the same random bytes (s = s * 1103515245 + 12345, byte = s >>> 24, s = 1 per
// save); the JS writer's CLASSED table is put back to its loaded state before each save (as tools/parity/dwg-write.mjs).
//   node tools/parity/dwg-save.mjs <dir|file>… [--versions 2000,2004] [--detail] [--max N]
//   … --model: compare the writer models instead (needs FORK_PARITY_APP = a copy of the 0.5 app whose dwg-save.js
//     calls globalThis.__saveModel(model) just before writeDwg; the frozen copy itself is never changed)
//   … --golden-out <file.json>: also write the JS results (name, version, size, CRC-32, canonical report) for the
//     libs/save test (tests/golden/README.md)
import { readFileSync, statSync, readdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';

const site = process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com';
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const args = process.argv.slice(2);
const modelMode = args.includes('--model');
const app = modelMode ? resolve(process.env.FORK_PARITY_APP || '') : join(js05, 'site/public_html/assets/apps/cadcraft/dev');
let seed = 1;
Object.defineProperty(globalThis, 'crypto', {
  value: { getRandomValues(a) { for (let i = 0; i < a.length; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; a[i] = seed >>> 24; } return a; } },
  configurable: true,
});
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const { init } = await imp('i18n.js');
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const { readDwg } = await imp('dwg/read.js');
const { importDwg } = await imp('dwg-import.js');
const { Doc } = await imp('doc.js');
const { saveDwg } = await imp('dwg-save.js');
const { CLASSED } = await imp('dwg/objects-write.js');
const { crc32 } = await imp('dwg/crc.js');
const classedKeys = new Set(Object.keys(CLASSED));
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const detail = args.includes('--detail');
const opt = (k) => (args.includes(k) ? args[args.indexOf(k) + 1] : null);
const maxShow = opt('--max') ? Number(opt('--max')) : (detail ? 30 : 3);
const versions = (opt('--versions') || '2000,2004,2007,2010,2013,2018').split(',');
const goldenOut = opt('--golden-out') ? resolve(opt('--golden-out')) : null;
const golden = [];
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--versions', '--golden-out'].includes(args[k - 1])));
const files = [];
const walk = (p) => { const s = statSync(p); if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n)); else if (/\.dwg$/i.test(p)) files.push(p); };
for (const r of roots) walk(resolve(r));
const created = Date.UTC(2026, 9, 17, 12, 0, 0);
const tmp = mkdtempSync(join(tmpdir(), 'ccs-'));
const sha = (b) => createHash('sha1').update(b).digest('hex');
const open = (data) => {
  const model = readDwg(data);
  if (!model || !model.blocks?.length) return null;
  const d = new Doc();
  d.load(importDwg(model, { versionTag: new TextDecoder().decode(data.subarray(0, 6)) }));
  return d;
};
let same = 0, n = 0, bytesSame = 0;
for (const f of files) {
  const data = new Uint8Array(readFileSync(f));
  let doc;
  try { doc = open(data); } catch { continue; }
  if (!doc) continue;
  for (const v of versions) {
    n++;
    for (const k of Object.keys(CLASSED)) if (!classedKeys.has(k)) delete CLASSED[k];
    seed = 1;
    let ref, refModel;
    globalThis.__saveModel = modelMode ? (m) => { refModel = canon(m); } : undefined;
    try {
      const r = saveDwg(doc, { version: v, created: new Date(created) });
      ref = { sha: sha(r.bytes), size: r.bytes.length, report: r.report };
      if (goldenOut) golden.push({ name: basename(f), version: v, size: r.bytes.length, crc32: crc32(r.bytes), report: canon(r.report) });
    } catch (e) { ref = { error: e.message }; }
    const out = join(tmp, 'o.dwg');
    let got, gotModel;
    try {
      const txt = execFileSync(cli, ['dwg-save', f, v, out, '--created', String(created), '--seeded', ...(modelMode ? ['--model'] : [])], { maxBuffer: 1 << 30 }).toString();
      const b = readFileSync(out);
      if (modelMode) gotModel = JSON.parse(txt);
      got = { sha: sha(b), size: b.length, report: modelMode ? null : JSON.parse(txt) };
    } catch (e) {
      const t = e.stdout?.toString() || '';
      got = { error: (t ? JSON.parse(t).$error?.message : String(e)).slice(0, 200) };
    }
    const name = f.replace(testfiles + '/', '');
    if (modelMode) {
      const d = diff(refModel ?? { error: ref.error }, canon(gotModel ?? { error: got.error }), { abs: 0, rel: 0, max: 1e6 });
      if (!d.length) { same++; continue; }
      console.log(`${name} ${v}: model ${d.length} difference(s)`);
      for (const x of d.slice(0, maxShow)) console.log('   ', x.path, x.kind, 'js=' + JSON.stringify(x.a)?.slice(0, 140), 'cpp=' + JSON.stringify(x.b)?.slice(0, 140));
      continue;
    }
    const bytesOk = ref.sha && ref.sha === got.sha;
    if (bytesOk) bytesSame++;
    const d = diff(canon(ref.report ?? { error: ref.error }), canon(got.report ?? { error: got.error }), { abs: 1e-9, rel: 1e-12 });
    if (bytesOk && !d.length) { same++; continue; }
    console.log(`${name} ${v}: ${bytesOk ? 'bytes same' : `bytes differ (${ref.size ?? ref.error} vs ${got.size ?? got.error})`}, report ${d.length} difference(s)`);
    for (const x of d.slice(0, maxShow)) console.log('   ', x.path, x.kind, 'js=' + JSON.stringify(x.a)?.slice(0, 120), 'cpp=' + JSON.stringify(x.b)?.slice(0, 120));
  }
}
rmSync(tmp, { recursive: true, force: true });
if (goldenOut) writeFileSync(goldenOut, JSON.stringify({ created, seed: 'lcg 1103515245 12345, byte = s >>> 24', saves: golden }));
console.log(modelMode ? `${same}/${n} models the same` : `${same}/${n} same (${bytesSame}/${n} byte-identical files)`);
