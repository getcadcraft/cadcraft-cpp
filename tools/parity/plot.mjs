#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Plot parity (M5.6b): every drawing in the given folders opened as CadCraft 0.5 opens it (readDwg → importDwg →
// Doc.load), then each sheet plotted (plot.js layoutSheet → displayList → toPdf) and the model at its extents on A3
// (modelSheet), against `cadcraft-cli plot-parity <file>`. The PDFs must be byte for byte the same: both sides leave
// the page content uncompressed (the JS without CompressionStream) and date the file 2026-10-16.
//
//   node tools/parity/plot.mjs <dir|file.dwg>… [--detail] [--max N]
import { readFileSync, statSync, readdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const app = join(js05, 'site/public_html/assets/apps/cadcraft/dev');
const imp = (p) => import(pathToFileURL(join(app, p)).href);
const FIXED_NOW = Date.UTC(2026, 9, 16);
const RealDate = Date;
globalThis.Date = class extends RealDate {
  constructor(...a) { if (a.length) super(...a); else super(FIXED_NOW); }
  static now() { return FIXED_NOW; }
};
globalThis.CompressionStream = undefined;   // the page content stays as written
const { init } = await imp('i18n.js');
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });
const { readDwg } = await imp('dwg/read.js');
const { importDwg } = await imp('dwg-import.js');
const { Doc } = await imp('doc.js');
const { layoutSheet, modelSheet, displayList, toPdf, extentsOf } = await imp('plot.js');
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : 3;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && args[k - 1] === '--max'));
const files = [];
const walkDir = (p) => { const s = statSync(p); if (s.isDirectory()) for (const n of readdirSync(p).sort()) walkDir(join(p, n)); else if (/\.dwg$/i.test(p)) files.push(p); };
for (const r of roots) walkDir(resolve(r));

const latin1 = (b) => Buffer.from(b).toString('latin1');
let same = 0, n = 0, pdfs = 0;
for (const f of files) {
  let ref;
  try {
    const data = readFileSync(f);
    const model = readDwg(new Uint8Array(data));
    if (!model.blocks?.length) continue;   // unreadable: the app refuses it (cadcraft-cli says so)
    const d = new Doc();
    d.load(importDwg(model, { versionTag: new TextDecoder().decode(data.subarray(0, 6)) }));
    ref = [];
    for (const l of d.layouts) ref.push({ name: l.name, pdf: latin1((await toPdf(displayList(d, layoutSheet(d, l)))).bytes) });
    const ext = extentsOf(d);
    if (ext) ref.push({ name: '*model*', pdf: latin1((await toPdf(displayList(d, modelSheet(d, { area: ext })))).bytes) });
  } catch (e) { continue; }
  n++;
  let got;
  try {
    got = JSON.parse(execFileSync(cli, ['plot-parity', f], { maxBuffer: 2 ** 31 - 1 }).toString())
      .map((x) => ({ name: x.name, pdf: Buffer.from(x.pdf, 'base64').toString('latin1') }));
  } catch (e) { got = { error: String(e).slice(0, 200) }; }
  const name = f.replace(testfiles + '/', '');
  const bad = [];
  if (!Array.isArray(got)) bad.push('error: ' + got.error);
  else {
    if (got.length !== ref.length) bad.push(`sheets: js=${ref.length} cpp=${got.length}`);
    for (let i = 0; i < Math.min(got.length, ref.length); i++) {
      pdfs++;
      if (got[i].name !== ref[i].name) bad.push(`sheet ${i}: js=${ref[i].name} cpp=${got[i].name}`);
      if (got[i].pdf === ref[i].pdf) continue;
      const a = ref[i].pdf.split('\n'), b = got[i].pdf.split('\n');
      let k = 0;
      while (k < a.length && a[k] === b[k]) k++;
      bad.push(`${ref[i].name} line ${k + 1}: js=${JSON.stringify(a[k])?.slice(0, 140)} cpp=${JSON.stringify(b[k])?.slice(0, 140)}`);
    }
  }
  if (!bad.length) { same++; continue; }
  console.log(`DIFF ${name}: ${bad.length}`);
  for (const x of bad.slice(0, maxShow)) console.log('   ' + x);
}
console.log(`${same}/${n} drawings plot the same (${pdfs} PDFs)`);
