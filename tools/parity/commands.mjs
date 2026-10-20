#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Command parity (M4.6): command scripts (typed lines, clicks with the object a pick finds, Esc, windows, a
// preselection, a layer set as the Layers panel does) run through CadCraft 0.5's command line (commands.js Engine) on a fresh Doc with a stub host, and
// through `cadcraft-cli script <file>` — the same host, in C++. Compared: everything printed and prompted, the model
// after (doc tolerances: abs 1e-9 + rel 1e-12), the block definitions, the layers, the drawing's meta, the UCS (current and saved), the named views, the current settings, the next id, the undo and redo labels.
//   node tools/parity/commands.mjs <scripts.json>… [--detail] [--max N]
//   … --golden-out <dir>: also write the JS results of each file, canonical, as <dir>/<file name> (the libs/cmd
//     test replays the scripts against them; tests/golden/README.md)
import zlib from 'node:zlib';
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
await imp('pictures-cmd.js');   // what cadcraft.js registers when it loads (IMAGEATTACH…, TEXTMASK, XCLIP)
await imp('textmask-cmd.js');
await imp('xclip-cmd.js');
const { VARIES, groupsOf, defsFor, valueOf, applyValue } = await imp('propdefs.js');
const { readValue } = await imp('propexpr.js');
const { modelSheet, displayList, toPdf, extentsOf } = await imp('plot.js');
const { writeDxf, readDxf } = await imp('dxf.js');
const { saveDwg } = await imp('dwg-save.js');
const { CLASSED } = await imp('dwg/objects-write.js');
const classedKeys = new Set(Object.keys(CLASSED));
const { readDwg } = await imp('dwg/read.js');
const { importDwg } = await imp('dwg-import.js');
const NUMERIC = ['len', 'ang', 'num', 'int', 'area'];

/** The Properties panel (props.js): {props, ids?, group?, vertex?} lists the group's fields with the value they share;
 * {prop, value, ids?, group?, vertex?} sets one (a number field's text read as props.js reads it). */
function propsStep(doc, host, s, log) {
  const ents = (Array.isArray(s.ids) ? s.ids : [...host.selection]).map((id) => doc.get(id)).filter(Boolean);
  const groups = groupsOf(ents);
  const g = groups.find((x) => x.key === (typeof s.group === 'string' ? s.group : groups[0]?.key));
  if (!g) { log.push(['props', null]); return; }
  const gents = g.ids.map((id) => doc.get(id));
  const ctx = { doc, vertex: gents.length === 1 ? (s.vertex | 0) : 0 };
  const defs = defsFor(g.key, gents, ctx);
  if (s.props) {
    log.push(['props', groups.map((x) => [x.key, x.n, x.label]), defs.map((d) => {
      const v = valueOf(d, gents, ctx);
      return [d.key, d.sec, d.kind, d.label, v === VARIES ? '\u0000varies' : v ?? null];
    })]);
    return;
  }
  const d = defs.find((x) => x.key === s.prop);
  if (!d) { log.push(['prop', s.prop, 'missing']); return; }
  let v = s.value;
  if (NUMERIC.includes(d.kind)) {
    const r = readValue(String(s.value), d.kind === 'area' ? 'num' : d.kind, { lunits: doc.meta.lunits, units: doc.meta.units, paper: null, gt0: d.gt0, min: d.min, max: d.max });
    if (!r.ok) { log.push(['prop', s.prop, 'read', r.error]); return; }
    v = r.value;
  }
  const r = applyValue(doc, d, gents, v, ctx);
  log.push(['prop', s.prop, r.ok, r.changed, r.locked, r.error ?? null]);
}
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : (detail ? 30 : 5);
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const files = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--golden-out'].includes(args[k - 1])));
const settle = () => new Promise((r) => setImmediate(r));
// one clock on both sides (a new sheet's title block is dated): 2026-10-16, UTC — cadcraft-cli script uses the same
const FIXED_NOW = Date.UTC(2026, 9, 16);
const RealDate = Date;
globalThis.Date = class extends RealDate {
  constructor(...a) { if (a.length) super(...a); else super(FIXED_NOW); }
  static now() { return FIXED_NOW; }
};

/** A PDF as libs/plot pdf_canonical makes it: Flate streams inflated, lengths and the xref table masked. */
function pdfCanonical(bytes) {
  const b = Buffer.from(bytes), s = b.toString('latin1');
  const xr = s.lastIndexOf('\nxref\n'), end = xr >= 0 ? xr + 1 : s.length;   // the table (not startxref's)
  let out = '', at = 0;
  while (at < end) {
    const st = s.indexOf('>>\nstream\n', at);
    if (st < 0 || st >= end) break;
    const open = s.lastIndexOf('<<', st);
    if (open < 0 || open < at || /[<>]/.test(s.slice(open + 2, st))) { out += s.slice(at, st + 10); at = st + 10; continue; }
    const dict = s.slice(open + 2, st), m = /\/Length (\d*)/.exec(dict), len = m ? +(m[1] || 0) : 0, body = st + 10;
    if (body + len > s.length) break;
    const d = m ? dict.replace(/\/Length \d*/, '/Length *') : dict;
    out += s.slice(at, open) + '<<' + d + '>>\nstream\n';
    const raw = b.subarray(body, body + len);
    if (d.includes('/Filter /FlateDecode')) { try { out += '#inflated ' + zlib.inflateSync(raw).toString('base64'); } catch { out += '#bad-flate'; } }
    else out += '#raw ' + raw.toString('base64');
    at = body + len;
  }
  return out + s.slice(at, Math.max(end, at));
}

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
    pictures: [],
    async pickPictures() { const f = this.pictures; this.pictures = []; return f; },
  };
  const eng = new Engine(doc, host);
  for (const s of script.steps) {
    if (typeof s === 'string') eng.text(s);
    else if (s.pictures) host.pictures = s.pictures.map((f) => ({ name: f.name, bytes: new Uint8Array(Buffer.from(f.bytes?.$base64 || '', 'base64')) }));
    else if (s.click) { host.nextPick = typeof s.pick === 'number' ? s.pick : null; eng.click({ x: s.click[0], y: s.click[1] }); }
    else if (s.cursor) host.cursor = { x: s.cursor[0], y: s.cursor[1] };
    else if (s.esc) eng.escape();
    else if (s.window) { host.windowIds = s.ids || []; eng.window({ minX: s.window[0], minY: s.window[1], maxX: s.window[2], maxY: s.window[3] }, !!s.crossing); }
    else if (s.select) host.selection = new Set(s.select);
    else if ('shift' in s) host.shift = !!s.shift;
    else if (typeof s.layer === 'string') doc.setLayer(s.layer, s.patch && typeof s.patch === 'object' ? s.patch : {}, { current: !!s.current });   // the Layers panel
    else if (s.props || typeof s.prop === 'string') propsStep(doc, host, s, log);
    else if ('space' in s) doc.setSpace(typeof s.space === 'string' ? doc.layouts.find((l) => l.name === s.space) || null : null);   // the space tabs
    await settle();
  }
  eng.escape();
  await settle();
  await eng.done;
  // a script may ask for the model's PDF (the pictures in it: plot.js WL.13)
  let pdf = {};
  if (script.plot) {
    const ext = extentsOf(doc);
    if (ext) {
      const r = await toPdf(displayList(doc, modelSheet(doc, { area: ext }), { mono: script.plot.mono !== false }), { maxDpi: script.plot.maxDpi || 0 });
      pdf = { pdf: pdfCanonical(r.bytes), pdfReport: { pictures: r.report.pictures, shrunk: r.report.shrunk, kept: r.report.kept } };
    }
  }
  // a script may ask for its drawing saved (and opened again: the constraints that come back)
  if (script.save) {
    const saved = {}, again = (d) => ({ constraints: d.constraints, params: d.params });
    for (const fmt of script.save) {
      if (fmt === 'dxf') {
        const text = writeDxf(doc).text, d2 = new Doc();
        d2.load(readDxf(text));
        saved.dxf = text; saved.dxfReopened = again(d2);
      } else if (fmt.startsWith('dwg')) {
        for (const k of Object.keys(CLASSED)) if (!classedKeys.has(k)) delete CLASSED[k];
        const was = Object.getOwnPropertyDescriptor(globalThis, 'crypto');
        let seed = 1;   // tools/parity/dwg-save.mjs's GUID bytes
        Object.defineProperty(globalThis, 'crypto', { value: { getRandomValues(a) { for (let i = 0; i < a.length; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; a[i] = seed >>> 24; } return a; } }, configurable: true });
        let r;
        try { r = saveDwg(doc, { version: fmt.slice(3), created: new Date(FIXED_NOW) }); } finally { if (was) Object.defineProperty(globalThis, 'crypto', was); }
        const d2 = new Doc(), model = readDwg(r.bytes);
        if (model?.blocks?.length) d2.load(importDwg(model, { versionTag: new TextDecoder().decode(r.bytes.subarray(0, 6)) }));
        saved[fmt] = Buffer.from(r.bytes).toString('base64'); saved[fmt + 'Reopened'] = again(d2);
      }
    }
    pdf.saved = saved;
  }
  return { ...pdf, name: script.name, log, model: [...doc.model.entries()], blocks: [...doc.blocks.entries()], layers: [...doc.layers.entries()], meta: doc.meta, ucs: doc.ucs, ucsNamed: doc.ucsNamed, views: doc.views, current: doc.current, nextId: doc.nextId,
    layouts: doc.layouts.map((l) => [l.name, l.paper, [...l.entities.entries()]]), space: doc.layout ? doc.layout.name : null,
    undo: doc.undoStack.map((u) => u.label), redo: doc.redoStack.map((u) => u.label), constraints: doc.constraints, params: doc.params };
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
