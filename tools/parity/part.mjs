#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// PartCraft and lib/shapes: CadCraft 0.5's frozen modules against `cadcraft-cli part` on the same call scripts.
// Adapted from the Rust fork's pcdiff (our own harness): each generator in tools/parity/part/gen-*.mjs builds call
// scripts from a seeded PRNG (mulberry32) — random expressions, parameter tables, sketches, parts and feature trees,
// tampered files, section and part tables — plus fixed cases taken from 0.5's Node tests. Every script runs on the JS
// driver (tools/parity/part/driver.mjs) and on the CLI; each call's result is diffed with the parity kit's canon/diff
// (tolerance 1e-9; --exact: none). A call the CLI does not know answers {err: "unknown op …"}, so a generator whose ops
// are not ported yet shows as differences, never as a match.
//
//   node tools/parity/part.mjs [--seed N] [--seeds A-B] [--only gen,…] [--max N] [--cli path] [--exact] [--keep dir]
//                              [--quiet] [--list] [--golden-out dir]
// --golden-out writes <dir>/<generator>.json: [{script, js}] for every script, the references the C++ tests replay.
import { writeFileSync, mkdtempSync, mkdirSync, readdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const KIT = (process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com') + '/tools/fork-parity/lib';
const { canon, diff } = await import(pathToFileURL(KIT + '/canon.mjs').href);
const D = await import(pathToFileURL(path.join(HERE, 'part/driver.mjs')).href);

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
let seeds = [Number(opt('--seed', 1))];
if (opt('--seeds')) { const [a, b] = opt('--seeds').split('-').map(Number); seeds = []; for (let s = a; s <= b; s++) seeds.push(s); }
const ONLY = opt('--only', '') ? new Set(opt('--only').split(',')) : null;
const MAX = Number(opt('--max', 8));
const CLI = opt('--cli', process.env.CC_CLI || path.resolve(HERE, '../../build/release/apps/cadcraft-cli/cadcraft-cli'));
const TOL = args.includes('--exact') ? 0 : 1e-9;
const QUIET = args.includes('--quiet');
const KEEP = opt('--keep', '');
const GOLDEN = opt('--golden-out', '');

export function mulberry32(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

for (const f of readdirSync(path.join(HERE, 'part')).filter((f) => /^ops-.*\.mjs$/.test(f)).sort()) {
  try { Object.assign(D.EXTOPS, (await import(pathToFileURL(path.join(HERE, 'part', f)).href)).OPS || {}); }
  catch (e) { console.log(`(skipped ${f}: ${e.message})`); }
}
const gens = [];
for (const f of readdirSync(path.join(HERE, 'part')).filter((f) => /^gen-.*\.mjs$/.test(f)).sort()) {
  let m;
  try { m = await import(pathToFileURL(path.join(HERE, 'part', f)).href); } catch (e) { console.log(`(skipped ${f}: ${e.stack})`); continue; }
  for (const [name, gen] of Object.entries(m.GENS || {})) if (!ONLY || ONLY.has(name) || ONLY.has(f.slice(4, -4))) gens.push([name, gen]);
}
if (args.includes('--list')) { console.log(gens.map(([n]) => n).join('\n')); process.exit(0); }

const tmp = KEEP || mkdtempSync(path.join(os.tmpdir(), 'part-parity-'));
if (KEEP) mkdirSync(KEEP, { recursive: true });
const golden = new Map();
let total = 0, bad = 0, calls = 0;
const per = new Map();
for (const seed of seeds) {
  for (const [name, gen] of gens) {
    const rnd = mulberry32(seed * 7919 + name.length * 131 + name.charCodeAt(0));
    let scripts;
    try { scripts = await gen(rnd, seed); } catch (e) { console.log(`generator ${name} seed ${seed} failed: ${e.stack}`); bad++; continue; }
    for (let k = 0; k < scripts.length; k++) {
      const script = scripts[k];
      const file = path.join(tmp, `${name}-${seed}-${k}.json`);
      const text = JSON.stringify(script);
      writeFileSync(file, text);
      let js, cpp;
      try { js = await D.run(JSON.parse(text)); } catch (e) { console.log(`JS driver failed ${name} seed ${seed} #${k}: ${e.stack}`); bad++; continue; }
      if (GOLDEN) { if (!golden.has(name)) golden.set(name, []); golden.get(name).push({ script: JSON.parse(text), js: canon(js) }); }
      try { cpp = JSON.parse(execFileSync(CLI, ['part', file], { maxBuffer: 1 << 30, encoding: 'utf8' })); }
      catch (e) { console.log(`CLI failed ${name} seed ${seed} #${k}: ${String(e.message).slice(0, 400)}`); bad++; total++; continue; }
      const st = per.get(name) || { calls: 0, diffs: 0 }; per.set(name, st);
      for (let i = 0; i < script.calls.length; i++) {
        calls++; st.calls++;
        const d = diff(canon(js[i]), canon(cpp[i]), { abs: TOL, rel: TOL, max: MAX });
        total++;
        if (d.length) {
          bad++; st.diffs++;
          if (!QUIET) {
            console.log(`\n✗ ${name} seed ${seed} script ${k} call ${i} ${script.calls[i].op}  (${file})`);
            for (const x of d) console.log(`   ${x.path}: js ${JSON.stringify(x.a)?.slice(0, 300)} | cpp ${JSON.stringify(x.b)?.slice(0, 300)} [${x.kind}]`);
          }
        }
      }
    }
  }
}
if (GOLDEN) {
  mkdirSync(GOLDEN, { recursive: true });
  for (const [name, list] of golden) writeFileSync(path.join(GOLDEN, name + '.json'), JSON.stringify(list));
}
console.log('\n' + [...per].map(([n, s]) => `${n.padEnd(22)} ${String(s.calls).padStart(7)} calls  ${s.diffs} differences`).join('\n'));
console.log(`\n${calls} records, ${bad} differences (seeds ${seeds[0]}–${seeds[seeds.length - 1]}, ${TOL ? 'tolerance 1e-9' : 'exact'})`);
process.exit(bad ? 1 : 0);
