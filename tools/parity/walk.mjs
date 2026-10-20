#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Draw parity (M4.7): what CadCraft 0.5 draws for a drawing — walk.js over the model and each sheet of the document
// the app opens (readDwg → importDwg → Doc.load) — against `cadcraft-cli walk <file>`. Every event in order: strokes
// (colour, lineweight, alpha, the dashed primitives), fills, texts (the entity as placed), points, viewports, masks,
// construction lines. Numbers within abs 1e-9 + rel 1e-12.
//   node tools/parity/walk.mjs <dir|file>… [--detail] [--max N]
//   … --golden-out <dir>: also write each JS result, canonical, as <dir>/<name>.json (the libs/docview test)
import { readFileSync, statSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
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
const { walk } = await imp('walk.js');
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : (detail ? 30 : 3);
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--golden-out'].includes(args[k - 1])));
const files = [];
const walkDir = (p) => { const s = statSync(p); if (s.isDirectory()) for (const n of readdirSync(p).sort()) walkDir(join(p, n)); else if (/\.dwg$/i.test(p)) files.push(p); };
for (const r of roots) walkDir(resolve(r));

const prim = (q) => (q.k === 'L' ? ['L', q.x1, q.y1, q.x2, q.y2] : ['A', q.cx, q.cy, q.r, q.a0, q.a1, q.full ? 1 : 0, q.rev ? 1 : 0]);
const alpha = (o) => (o && Number.isFinite(o.alpha) ? o.alpha : null);
function events(doc, entities) {
  const ev = [];
  walk(doc, entities, {
    stroke(color, ps, lw, o) { ev.push(['s', color, lw, alpha(o), ps.map(prim)]); },
    fill(color, loops, solid, o) { ev.push(['f', color, !!solid, alpha(o), !!o?.wipe, loops.map((l) => [...l])]); },
    text(color, e, o) { ev.push(['t', color, alpha(o), e]); },
    point(color, x, y) { ev.push(['p', color, x, y]); },
    viewport(e) { ev.push(['v', e.id]); },
    picture(p) { const q = p.place; ev.push(['pic', p.file, q.x, q.y, q.ux, q.uy, q.vx, q.vy, q.w, q.h, [...p.clip], p.alpha]); },
    mask(color, loop, o) { ev.push(['m', color, !!o?.wipe, [...loop]]); },
    infinite(color, e, lw, o) { ev.push(['i', color, lw, alpha(o), e.type, e.x, e.y, e.dx, e.dy]); },
  });
  return ev;
}

let same = 0, n = 0;
for (const f of files) {
  const data = new Uint8Array(readFileSync(f));
  let ref;
  try {
    const model = readDwg(data);
    if (!model || !model.blocks?.length) continue;
    const d = new Doc();
    d.load(importDwg(model, { versionTag: new TextDecoder().decode(data.subarray(0, 6)) }));
    ref = { model: events(d, [...d.model.values()]), layouts: d.layouts.map((l) => events(d, [...l.entities.values()])) };
  } catch (e) { continue; }
  n++;
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f) + '.json'), JSON.stringify(canon(ref))); }
  let got;
  try { got = JSON.parse(execFileSync(cli, ['walk', f], { maxBuffer: 1 << 31 - 1 }).toString()); } catch (e) { got = { error: String(e).slice(0, 200) }; }
  const d = diff(canon(ref), canon(got), { abs: 1e-9, rel: 1e-12, max: 1e6 });
  const name = f.replace(testfiles + '/', '');
  if (!d.length) { same++; continue; }
  console.log(`DIFF ${name}: ${d.length}`);
  for (const x of d.slice(0, maxShow)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 160)} cpp=${JSON.stringify(x.b)?.slice(0, 160)}`);
}
console.log(`${same}/${n} drawings draw the same`);
