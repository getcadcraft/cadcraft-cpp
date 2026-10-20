#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Document parity (M4.2, DXF since M5.7a): CadCraft 0.5 opens a DWG as readDwg → importDwg → new Doc().load() (doc.js), a DXF as
// readDxf → new Doc().load() (the text decoded as UTF-8, else as Windows-1252). This dumps that
// document (maps as [key, value] lists: model, layers, blocks, layouts, views, meta, current, styles, panes, UCS) from
// the frozen 0.5 copy and `cadcraft-cli doc <file>` on every .dwg under the given folders, and compares the two with
// the parity kit's canon + diff (dwg tolerances abs 1e-9 + rel 1e-12), on every .dwg and .dxf under the given folders.
// The JS i18n is initialised with CadCraft's English catalog, so report notes are the English the app shows (the C++
// side uses the same catalog, assets/lang/en.json).
//   CADCRAFT_SITE=/path/to/getcadcraft.com node tools/parity/doc.mjs <dir|file>… [--detail] [--max N]
//   … --golden-out <dir>: also write each JS drawing, canonical, as <dir>/<name>.json (tests/golden/README.md)
import { readFileSync, statSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, relative, dirname, basename } from 'node:path';
import { execFileSync } from 'node:child_process';
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
const { readDxf } = await imp('dxf.js');
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : (detail ? 30 : 3);
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && (args[k - 1] === '--max' || args[k - 1] === '--golden-out')));

const files = [];
const walk = (p) => {
  const s = statSync(p);
  if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n));
  else if (/\.(dwg|dxf)$/i.test(p)) files.push(p);
};
for (const r of roots) walk(resolve(r));

// cadcraft.js readDrawing for a DWG: the version tag is the file's first six bytes
const jsImport = (data, dxf) => {
  try {
    const d = new Doc();
    if (dxf) {
      let text = new TextDecoder('utf-8').decode(data);
      if (text.includes('\ufffd')) text = new TextDecoder('windows-1252').decode(data);
      d.load(readDxf(text));
    } else {
      const model = readDwg(data);
      if (!model || !model.blocks?.length) return { $error: { message: 'unreadable' } };
      d.load(importDwg(model, { versionTag: new TextDecoder().decode(data.subarray(0, 6)) }));
    }
    const map = (m) => [...m.entries()];
    return { model: map(d.model), layers: map(d.layers), blocks: map(d.blocks),
      layouts: d.layouts.map((l) => ({ name: l.name, paper: l.paper, entities: map(l.entities) })), views: d.views, meta: d.meta,
      current: d.current, linetypes: map(d.linetypes), textStyles: map(d.textStyles), dimStyles: map(d.dimStyles), vports: d.vports,
      ucs: d.ucs, ucsNamed: d.ucsNamed, viewTwist: d.viewTwist, dynSrc: d.dynSrc, nextId: d.nextId };
  } catch (e) { return { $error: { message: e.message } }; }
};

let same = 0;
for (const f of files) {
  const data = new Uint8Array(readFileSync(f));
  const ref = jsImport(data, /\.dxf$/i.test(f));   // canon() drops undefined and keeps NaN / Infinity as "$NaN" / "$Inf", as the C++ side writes them
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f) + '.json'), JSON.stringify(canon(ref))); }
  let out;
  try {
    out = JSON.parse(execFileSync(cli, ['doc', f], { maxBuffer: 1 << 30 }).toString());
  } catch (e) {
    out = e.stdout?.length ? JSON.parse(e.stdout.toString()) : { $error: { message: String(e).slice(0, 200) } };
  }
  if (out.$error) out = { $error: { message: out.$error.message } };   // errors compare by message (as dxf.mjs)
  const d = diff(canon(ref), canon(out), { abs: 1e-9, rel: 1e-12, max: 1e6 });
  const name = relative(testfiles, f);
  if (d.length === 0) { same++; continue; }
  console.log(`DIFF ${name}: ${d.length}`);
  for (const x of d.slice(0, maxShow)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 100)} cpp=${JSON.stringify(x.b)?.slice(0, 100)}`);
}
console.log(`${same}/${files.length} same`);
