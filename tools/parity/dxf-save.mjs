#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// DXF save parity (M4.5): CadCraft 0.5 opens a DWG as readDwg → importDwg → new Doc().load() and saves it as DXF with
// writeDxf (dxf.js, R12) or writeDxf2000 (dxf2000.js, R2000). This does that with the frozen 0.5 copy for every .dwg
// under the given folders, in both formats, and compares the text with `cadcraft-cli dxf-save <file> r12|2000 <out>`
// — byte for byte (UTF-8) — and the report with the parity kit's canon + diff.
//   node tools/parity/dxf-save.mjs <dir|file>… [--formats r12,2000] [--detail] [--max N] [--lines]
//   … --lines: for a file that differs, print the first differing lines of both
//   … --golden-out <file.json>: also write the JS results (name, format, size, CRC-32 of the UTF-8 text, canonical
//     report) for the libs/save test (tests/golden/README.md)
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
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const { init } = await imp('i18n.js');
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const { readDwg } = await imp('dwg/read.js');
const { importDwg } = await imp('dwg-import.js');
const { Doc } = await imp('doc.js');
const { writeDxf } = await imp('dxf.js');
const { writeDxf2000 } = await imp('dxf2000.js');
const { crc32 } = await imp('dwg/crc.js');
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const showLines = args.includes('--lines');
const opt = (k) => (args.includes(k) ? args[args.indexOf(k) + 1] : null);
const maxShow = opt('--max') ? Number(opt('--max')) : (detail ? 30 : 3);
const formats = (opt('--formats') || 'r12,2000').split(',');
const goldenOut = opt('--golden-out') ? resolve(opt('--golden-out')) : null;
const golden = [];
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--formats', '--golden-out'].includes(args[k - 1])));
const files = [];
const walk = (p) => { const s = statSync(p); if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n)); else if (/\.dwg$/i.test(p)) files.push(p); };
for (const r of roots) walk(resolve(r));
const tmp = mkdtempSync(join(tmpdir(), 'ccx-'));
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
  for (const fmt of formats) {
    n++;
    let ref, refText = '';
    try {
      const r = fmt === '2000' ? writeDxf2000(doc) : writeDxf(doc);
      refText = r.text;
      const b = Buffer.from(r.text, 'utf8');
      ref = { sha: sha(b), size: b.length, report: r.report };
      if (goldenOut) golden.push({ name: basename(f), format: fmt, size: b.length, crc32: crc32(new Uint8Array(b)), report: canon(ref.report) });
    } catch (e) { ref = { error: e.message }; }
    const out = join(tmp, 'o.dxf');
    let got, gotText = '';
    try {
      const txt = execFileSync(cli, ['dxf-save', f, fmt, out], { maxBuffer: 1 << 30 }).toString();
      const b = readFileSync(out);
      gotText = b.toString('utf8');
      got = { sha: sha(b), size: b.length, report: JSON.parse(txt) };
    } catch (e) {
      const t = e.stdout?.toString() || '';
      got = { error: (t ? JSON.parse(t).$error?.message : String(e)).slice(0, 200) };
    }
    const name = f.replace(testfiles + '/', '');
    const bytesOk = ref.sha && ref.sha === got.sha;
    if (bytesOk) bytesSame++;
    const d = diff(canon(ref.report ?? { error: ref.error }), canon(got.report ?? { error: got.error }), { abs: 0, rel: 0 });
    if (bytesOk && !d.length) { same++; continue; }
    console.log(`${name} ${fmt}: ${bytesOk ? 'bytes same' : `bytes differ (${ref.size ?? ref.error} vs ${got.size ?? got.error})`}, report ${d.length} difference(s)`);
    for (const x of d.slice(0, maxShow)) console.log('   ', x.path, x.kind, 'js=' + JSON.stringify(x.a)?.slice(0, 120), 'cpp=' + JSON.stringify(x.b)?.slice(0, 120));
    if (showLines && !bytesOk && refText && gotText) {
      const a = refText.split('\r\n'), b = gotText.split('\r\n');
      let shown = 0;
      for (let i = 0; i < Math.max(a.length, b.length) && shown < maxShow; i++) {
        if (a[i] !== b[i]) { console.log(`    line ${i + 1}: js=${JSON.stringify(a[i])} cpp=${JSON.stringify(b[i])}  (after ${JSON.stringify(a.slice(Math.max(0, i - 3), i))})`); shown++; }
      }
    }
  }
}
rmSync(tmp, { recursive: true, force: true });
if (goldenOut) writeFileSync(goldenOut, JSON.stringify({ saves: golden }));
console.log(`${same}/${n} same (${bytesSame}/${n} byte-identical files)`);
