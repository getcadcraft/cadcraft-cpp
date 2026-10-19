#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Command parity (M4.6): command scripts (typed lines, clicks with the object a pick finds, Esc, windows, a
// preselection) run through CadCraft 0.5's command line (commands.js Engine) on a fresh Doc with a stub host, and
// through `cadcraft-cli script <file>` — the same host, in C++. Compared: everything printed and prompted, the model
// after (doc tolerances: abs 1e-9 + rel 1e-12), the block definitions, the current settings, the next id, the undo and redo labels.
//   node tools/parity/commands.mjs <scripts.json>… [--detail] [--max N]
//   … --golden-out <dir>: also write the JS results of each file, canonical, as <dir>/<file name> (the libs/cmd
//     test replays the scripts against them; tests/golden/README.md)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
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
const { Doc } = await imp('doc.js');
const { Engine } = await imp('commands.js');
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : (detail ? 30 : 5);
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const files = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--golden-out'].includes(args[k - 1])));
const settle = () => new Promise((r) => setImmediate(r));

async function runJs(script) {
  const doc = new Doc();
  const log = [];
  const host = {
    print: (s) => log.push(['print', String(s)]),
    prompt: (s) => log.push(['prompt', String(s)]),
    cursor: null, selection: new Set(), shift: false, nextPick: null, windowIds: null,
    setSelection(s) { this.selection = new Set(s); },
    pickAt() { const id = this.nextPick; this.nextPick = null; return id; },
    pickWindow() { const ids = this.windowIds || []; this.windowIds = null; return ids; },
    preview() {}, onCommand() {},
  };
  const eng = new Engine(doc, host);
  for (const s of script.steps) {
    if (typeof s === 'string') eng.text(s);
    else if (s.click) { host.nextPick = typeof s.pick === 'number' ? s.pick : null; eng.click({ x: s.click[0], y: s.click[1] }); }
    else if (s.cursor) host.cursor = { x: s.cursor[0], y: s.cursor[1] };
    else if (s.esc) eng.escape();
    else if (s.window) { host.windowIds = s.ids || []; eng.window({ minX: s.window[0], minY: s.window[1], maxX: s.window[2], maxY: s.window[3] }, !!s.crossing); }
    else if (s.select) host.selection = new Set(s.select);
    else if ('shift' in s) host.shift = !!s.shift;
    await settle();
  }
  eng.escape();
  await settle();
  await eng.done;
  return { name: script.name, log, model: [...doc.model.entries()], blocks: [...doc.blocks.entries()], current: doc.current, nextId: doc.nextId,
    undo: doc.undoStack.map((u) => u.label), redo: doc.redoStack.map((u) => u.label) };
}

let same = 0, n = 0;
for (const f of files) {
  const scripts = JSON.parse(readFileSync(resolve(f), 'utf8'));
  let got;
  try {
    got = execFileSync(cli, ['script', resolve(f)], { maxBuffer: 1 << 30 }).toString().trim().split('\n').map((l) => JSON.parse(l));
  } catch (e) { got = []; console.log(`${f}: cadcraft-cli failed: ${String(e).slice(0, 200)}`); }
  const refs = [];
  for (const [k, sc] of scripts.entries()) {
    n++;
    const ref = await runJs(sc);
    refs.push(canon(ref));
    const out = got[k] || { error: 'missing' };
    const d = diff(canon(ref), canon(out), { abs: 1e-9, rel: 1e-12, max: 1e6 });
    if (!d.length) { same++; continue; }
    console.log(`DIFF ${sc.name}: ${d.length}`);
    for (const x of d.slice(0, maxShow)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 140)} cpp=${JSON.stringify(x.b)?.slice(0, 140)}`);
  }
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f)), JSON.stringify(refs)); }
}
console.log(`${same}/${n} scripts the same`);
