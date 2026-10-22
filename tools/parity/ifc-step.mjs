#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// IFC STEP parity outside the kit: cc-ifc's step.js (the frozen 0.5 copy, through the kit's own `ifc-step` engine)
// against `cadcraft-cli parity ifc-step`, on any files — the corpus, the public fixtures, and the fuzzers' corpora
// (differential fuzzing: every input the fuzzer kept, JS against C++). Exact comparison, as the kit's (tolerance 0).
//   CADCRAFT_SITE=/path/to/site node tools/parity/ifc-step.mjs <dir|file>… [--any] [--detail]
//   --any: every file under a folder (fuzz corpora), not only *.ifc
//   --golden-out <dir>: also write each JS result, canonical, as <dir>/<name>.json ({head, lines}) (tests/golden/README.md)
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = process.env.CADCRAFT_SITE;
if (!site) { console.error('set CADCRAFT_SITE to the site repo'); process.exit(2); }
const { canon, stringify } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const { ENGINES } = await import(join(site, 'tools/fork-parity/lib/engines.mjs'));
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const any = args.includes('--any'), detail = args.includes('--detail');
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const roots = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && args[k - 1] === '--golden-out'));

const files = [];
const walk = (p) => {
  const s = statSync(p);
  if (s.isDirectory()) for (const n of readdirSync(p).sort()) walk(join(p, n));
  else if (any || /\.ifc$/i.test(p)) files.push(p);
};
for (const r of roots) walk(resolve(r));

const js = async (bytes) => {
  try { return { ok: true, v: canon(await ENGINES['ifc-step'].run(bytes)) }; }
  catch (e) { return { ok: false, message: String(e.message || e) }; }
};
const cpp = (f) => {
  let text;
  try { text = execFileSync(cli, ['parity', 'ifc-step', f], { maxBuffer: 2 ** 31 - 1, encoding: 'utf8' }); }
  catch (e) { if (e.status !== 1) return { crash: `exit ${e.status} ${e.signal || ''}` }; text = e.stdout; }
  const lines = text.split('\n').filter((l) => l.length);
  const head = JSON.parse(lines[0]);
  if (head.$error) return { ok: false, message: head.$error.message };
  const out = {};
  for (const l of lines.slice(1)) { const [id, t, p] = JSON.parse(l); out[id] = [t, canon(p)]; }
  return { ok: true, v: { ...canon(head), lines: out } };
};

let same = 0;
for (const f of files) {
  const ref = await js(new Uint8Array(readFileSync(f)));
  if (goldenOut && ref.ok) {
    mkdirSync(goldenOut, { recursive: true });
    const { lines, ...head } = ref.v;
    writeFileSync(join(goldenOut, basename(f) + '.json'), stringify({ head, lines: Object.entries(lines).map(([id, l]) => [Number(id), ...l]) }));
  }
  const got = cpp(f);
  const diffs = [];
  if (got.crash) diffs.push(`C++ crashed: ${got.crash}`);
  else if (ref.ok !== got.ok) diffs.push(`JS ${ref.ok ? 'reads' : 'refuses: ' + ref.message} / C++ ${got.ok ? 'reads' : 'refuses: ' + got.message}`);
  else if (!ref.ok) { if (ref.message !== got.message) diffs.push(`refusals differ: ${ref.message} / ${got.message}`); }
  else {
    const { lines: a, ...ha } = ref.v, { lines: b, ...hb } = got.v;
    if (stringify(ha) !== stringify(hb)) diffs.push(`head: js=${stringify(ha).slice(0, 300)} cpp=${stringify(hb).slice(0, 300)}`);
    for (const id of new Set([...Object.keys(a), ...Object.keys(b)])) {
      const x = stringify(a[id]), y = stringify(b[id]);
      if (x !== y) diffs.push(`#${id}: js=${x?.slice(0, 200)} cpp=${y?.slice(0, 200)}`);
    }
  }
  if (!diffs.length) same++;
  else {
    console.log(`DIFF ${f}: ${diffs.length}`);
    for (const d of diffs.slice(0, detail ? 30 : 3)) console.log(`   ${d}`);
  }
}
console.log(`${same}/${files.length} match`);
process.exit(same === files.length ? 0 : 1);
