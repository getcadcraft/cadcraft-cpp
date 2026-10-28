#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// IFC model parity: cc-ifc 0.5's model.js (IfcFile, the frozen copy) against `cadcraft-cli ifc-model`, on any .ifc files.
// Both sides dump the same facts — schema, counts, the types present in the JS Map's order, every attribute by name of
// the first 300 lines, products (inherited ofType), an inverse index, refsTo — then make the same edits (a new wall, a
// changed line, a removed line, a new line with every kind of value) and save with a fixed time stamp: the saved bytes'
// size and FNV-1a hash, and the counts after. Exact comparison.
//   CADCRAFT_SITE=/path/to/site node tools/parity/ifc-model.mjs <dir|file>… [--detail]
//   --golden-out <dir>: also write each JS dump as <dir>/<name>.json (tests/golden/README.md)
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const site = process.env.CADCRAFT_SITE;
if (!site) { console.error('set CADCRAFT_SITE to the site repo'); process.exit(2); }
const { canon, stringify } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const { IfcFile } = await import(pathToFileURL(join(js05, 'lib/ifc/model.js')).href);
const { DERIVED } = await import(pathToFileURL(join(js05, 'lib/ifc/step.js')).href);
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const maxMb = args.includes('--max-mb') ? Number(args[args.indexOf('--max-mb') + 1]) : 30;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--golden-out', '--max-mb'].includes(args[k - 1])));

const files = [];
const walk = (p) => {
  const s = statSync(p);
  if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n));
  else if (/\.ifc$/i.test(p) && s.size <= maxMb * 1e6) files.push(p);
};
for (const r of roots) walk(resolve(r));

const err = (e) => ({ $error: String(e?.message || e) });
const tryv = (f) => { try { return f(); } catch (e) { return err(e); } };

/** The dump (the C++ side builds the same in apps/cadcraft-cli: cmd_ifc_model). */
export function dumpModel(bytes) {
  let f;
  try { f = IfcFile.open(bytes); } catch (e) { return { open: err(e) }; }
  const out = { schemaName: f.schemaName, guessed: !!f.schema.guessed, fileSchema: f.fileSchema, maxId: f.maxId, count: f.count };
  out.types = [...f.typesPresent()];
  const ids = [...f.ids()];
  out.first = ids.slice(0, 300).map((id) => {
    const t = f.type(id);
    const e = t ? f.schema.entity(t) : null;
    const attrs = {};
    if (e) for (const a of e.attrs) attrs[a.name] = tryv(() => f.attr(id, a.name) ?? '$undefined');
    return [id, t, e ? attrs : tryv(() => f.complex(id))];
  });
  out.products = tryv(() => { const p = f.ofType('IFCPRODUCT', true); return [p.length, p.slice(0, 50)]; });
  out.inverse = tryv(() => { const m = f.inverse('IFCRELDEFINESBYPROPERTIES', 'RelatedObjects'); return [m.size, [...m].slice(0, 30)]; });
  out.refs = tryv(() => ids.slice(0, 40).map((id) => f.refsTo(id)));
  // edits
  out.edits = tryv(() => {
    const r = [];
    const wall = f.add('IFCWALL', ['2O2Fr$t4X7Zf8NOew3FLOH', null, 'New é wall \u{1F600}', DERIVED, null, null, null, null, { enum: 'STANDARD' }]);
    r.push(wall);
    if (ids.length > 1) r.push(f.remove(ids[1]));
    if (ids.length > 2 && f.params(ids[2])) { const p = f.params(ids[2]).slice(); p.push(1.5, 2, -0, 1e21, 1.25e-7, [1, [2, 3]], { typed: 'IFCLENGTHMEASURE', value: 3 }, { typed: 'IFCLABEL', value: "it's" }); r.push(f.set(ids[2], f.type(ids[2]), p)); }
    r.push(f.set(f.maxId + 5, 'ifcCartesianPoint', [[0, 0.5, 1e-3]]));
    r.push(tryv(() => f.set(0, 'IFCWALL', [])));
    r.push(f.remove(999999999));
    const saved = f.save({ timeStamp: '2026-10-21T00:00:00' });
    let h = 2166136261;
    for (const c of saved) h = Math.imul(h ^ c, 16777619) >>> 0;
    r.push(saved.length, h);
    r.push(f.count, [...f.typesPresent()].slice(-5), f.ofType('IFCWALL', true).slice(-3), f.maxId);
    return r;
  });
  return out;
}

let same = 0;
for (const file of files) {
  const ref = canon(dumpModel(new Uint8Array(readFileSync(file))));
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(file) + '.json'), stringify(ref)); }
  let got;
  try { got = canon(JSON.parse(execFileSync(cli, ['ifc-model', file], { maxBuffer: 2 ** 31 - 1, encoding: 'utf8' }))); }
  catch (e) { got = { crash: String(e.status ?? e.message) }; }
  const diffs = [];
  for (const k of new Set([...Object.keys(ref), ...Object.keys(got)])) {
    const a = stringify(ref[k]), b = stringify(got[k]);
    if (a !== b) diffs.push(`${k}: js=${a?.slice(0, 300)} cpp=${b?.slice(0, 300)}`);
  }
  if (!diffs.length) same++;
  else {
    console.log(`DIFF ${file}: ${diffs.length}`);
    for (const d of diffs.slice(0, detail ? 20 : 3)) console.log('   ' + d);
  }
}
console.log(`${same}/${files.length} match`);
process.exit(same === files.length ? 0 : 1);
