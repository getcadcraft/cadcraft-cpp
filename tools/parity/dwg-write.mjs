#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// DWG writer parity (M4.3): CadCraft 0.5 saves DWG with cc-dwg's writeDwg (lib/dwg/write.js). This reads every .dwg
// under the given folders with the frozen 0.5 reader, writes it in each version with the 0.5 writer, and compares the
// bytes with `cadcraft-cli dwg-write <file> <version> <out> --created <ms> --seeded` — byte for byte — and the report
// with the parity kit's canon + diff.
// Both sides get the same date and the same random bytes (the GUIDs a file carries): s = s * 1103515245 + 12345,
// byte = s >>> 24, s = 1 at the start of each write. The JS writer's CLASSED table is put back to its loaded state
// before each write (the JS keeps classes of custom objects it met in earlier writes of the same session; one save is
// one fresh write here).
//   node tools/parity/dwg-write.mjs <dir|file>… [--versions AC1015,AC1018] [--detail] [--max N]
//   … --golden-out <file.json>: also write the JS results (name, version, size, CRC-32 of the bytes, canonical report)
//   for the libs/io-dwg write test (tests/golden/README.md)
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
const app = join(js05, 'site/public_html/assets/apps/cadcraft/dev');
let seed = 1;
Object.defineProperty(globalThis, 'crypto', {
  value: { getRandomValues(a) { for (let i = 0; i < a.length; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; a[i] = seed >>> 24; } return a; } },
  configurable: true,
});
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const { readDwg } = await imp('dwg/read.js');
const { writeDwg } = await imp('dwg/write.js');
const { CLASSED } = await imp('dwg/objects-write.js');
const { crc32 } = await imp('dwg/crc.js');
const classedKeys = new Set(Object.keys(CLASSED));
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const opt = (k) => (args.includes(k) ? args[args.indexOf(k) + 1] : null);
const maxShow = opt('--max') ? Number(opt('--max')) : (detail ? 30 : 3);
const versions = (opt('--versions') || 'AC1015,AC1018,AC1021,AC1024,AC1027,AC1032').split(',');
const goldenOut = opt('--golden-out') ? resolve(opt('--golden-out')) : null;
const golden = [];
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--versions', '--golden-out'].includes(args[k - 1])));
const files = [];
const walk = (p) => { const s = statSync(p); if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n)); else if (/\.dwg$/i.test(p)) files.push(p); };
for (const r of roots) walk(resolve(r));
const created = Date.UTC(2026, 9, 17, 12, 0, 0);
const tmp = mkdtempSync(join(tmpdir(), 'ccw-'));
const sha = (b) => createHash('sha1').update(b).digest('hex');
let same = 0, n = 0, bytesSame = 0;
for (const f of files) {
  let model;
  try { model = readDwg(new Uint8Array(readFileSync(f))); } catch { continue; }
  if (!model || !model.blocks?.length) continue;
  for (const v of versions) {
    n++;
    for (const k of Object.keys(CLASSED)) if (!classedKeys.has(k)) delete CLASSED[k];
    seed = 1;
    let ref;
    try {
      const r = writeDwg(model, { version: v, created: new Date(created) });
      ref = { sha: sha(r.bytes), size: r.bytes.length, report: r.report };
      if (goldenOut) golden.push({ name: basename(f), version: v, size: r.bytes.length, crc32: crc32(r.bytes), report: canon(r.report) });
    } catch (e) { ref = { error: e.message }; }
    const out = join(tmp, 'o.dwg');
    let got;
    try {
      const rep = JSON.parse(execFileSync(cli, ['dwg-write', f, v, out, '--created', String(created), '--seeded'], { maxBuffer: 1 << 30 }).toString());
      const b = readFileSync(out);
      got = { sha: sha(b), size: b.length, report: rep };
    } catch (e) {
      const t = e.stdout?.toString() || '';
      got = { error: (t ? JSON.parse(t).error?.message : String(e)).slice(0, 200) };
    }
    const bytesOk = ref.sha && ref.sha === got.sha;
    if (bytesOk) bytesSame++;
    const d = diff(canon(ref.report ?? { error: ref.error }), canon(got.report ?? { error: got.error }), { abs: 1e-9, rel: 1e-12 });
    if (bytesOk && !d.length) { same++; continue; }
    console.log(`${f.replace(testfiles + '/', '')} ${v}: ${bytesOk ? 'bytes same' : `bytes differ (${ref.size ?? ref.error} vs ${got.size ?? got.error})`}, report ${d.length} difference(s)`);
    for (const x of d.slice(0, maxShow)) console.log('   ', x.path, x.kind, 'js=' + JSON.stringify(x.a)?.slice(0, 120), 'cpp=' + JSON.stringify(x.b)?.slice(0, 120));
  }
}
rmSync(tmp, { recursive: true, force: true });
if (goldenOut) writeFileSync(goldenOut, JSON.stringify({ created, seed: 'lcg 1103515245 12345, byte = s >>> 24', writes: golden }));
console.log(`${same}/${n} same (${bytesSame}/${n} byte-identical files)`);
