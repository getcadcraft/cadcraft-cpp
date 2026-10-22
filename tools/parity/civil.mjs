#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// CivilCraft's engine: the FROZEN 0.5 JS modules against `civilcraft-cli script` (libs/civil). Adapted from the Rust
// fork's cvdiff harness (ours).
//
//   node tools/parity/civil.mjs [--seeds A-B] [--only gen,…] [--max N] [--exact] [--quiet] [--list]
//                               [--golden-out <dir>]
//
// Each generator in civil/gen-*.mjs builds call scripts from a seeded PRNG (mulberry32) — survey-scale coordinates
// (x ≈ 500 000, y ≈ 4 000 000 m; state-plane feet), duplicates, crossings, odd input. Every script runs on the JS
// driver (civil/driver.mjs) and the CLI; each call's result is diffed with the parity kit's canon/diff (default
// tolerance 0: exact; --tol x to loosen for a look, never for a gate). --golden-out writes each script and the JS
// results (<name>.json, <name>.out.json) for tests/golden/js-0.5-civil.tar.gz.
import { writeFileSync, mkdtempSync, mkdirSync, readdirSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { canon, diff } from './civil/kit.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const D = await import(pathToFileURL(path.join(HERE, 'civil/driver.mjs')).href);

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
let seeds = [Number(opt('--seed', 1))];
if (opt('--seeds')) { const [a, b] = opt('--seeds').split('-').map(Number); seeds = []; for (let s = a; s <= b; s++) seeds.push(s); }
const ONLY = opt('--only', '') ? new Set(opt('--only').split(',')) : null;
const MAX = Number(opt('--max', 8));
const CLI = opt('--cli', process.env.CIVIL_CLI || path.resolve(HERE, '../../build/release/apps/civilcraft-cli/civilcraft-cli'));
const TOL = Number(opt('--tol', 0));
const QUIET = args.includes('--quiet');
const GOLD = opt('--golden-out', '');
// scripts whose ops the C++ side does not have yet are skipped and counted (`civilcraft-cli ops` lists them)
const PORTED = new Set(execFileSync(CLI, ['ops'], { encoding: 'utf8' }).split('\n').filter(Boolean));
let skipped = 0;

const wf = (v) => typeof v === 'string' ? v.toWellFormed() : Array.isArray(v) ? v.map(wf) : v && typeof v === 'object' ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, wf(x)])) : v;

for (const f of readdirSync(path.join(HERE, 'civil')).filter((f) => /^ops-.*\.mjs$/.test(f)).sort()) {
  Object.assign(D.EXTOPS, (await import(pathToFileURL(path.join(HERE, 'civil', f)).href)).OPS || {});
}
const gens = [];
for (const f of readdirSync(path.join(HERE, 'civil')).filter((f) => /^gen-.*\.mjs$/.test(f)).sort()) {
  const m = await import(pathToFileURL(path.join(HERE, 'civil', f)).href);
  for (const [name, gen] of Object.entries(m.GENS || {})) if (!ONLY || ONLY.has(name) || ONLY.has(f.slice(4, -4))) gens.push([name, gen]);
}
if (args.includes('--list')) { console.log(gens.map(([n]) => n).join('\n')); process.exit(0); }
const mulberry32 = (a) => () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };

const tmp = mkdtempSync(path.join(os.tmpdir(), 'civil-parity-'));
if (GOLD) mkdirSync(GOLD, { recursive: true });
let bad = 0, calls = 0, scriptsRun = 0, jsMs = 0, cppMs = 0;
const per = new Map();
for (const seed of seeds) {
  for (const [name, gen] of gens) {
    const rnd = mulberry32(seed * 7919 + name.length * 131 + name.charCodeAt(0));
    const scripts = await gen(rnd, seed);
    for (let k = 0; k < scripts.length; k++) {
      const script = scripts[k];
      if (!script.calls.every((c) => PORTED.has(c.op))) { skipped++; continue; }
      const file = path.join(tmp, `${name}-${seed}-${k}.json`);
      writeFileSync(file, JSON.stringify(script));
      let js, cpp;
      const t0 = performance.now();
      try { js = await D.run(script); } catch (e) { console.log(`JS driver failed ${name} seed ${seed} #${k}: ${e.stack}`); bad++; continue; }
      const t1 = performance.now();
      try { cpp = JSON.parse(execFileSync(CLI, ['script', file], { maxBuffer: 1 << 30, encoding: 'utf8' })); }
      catch (e) { console.log(`CLI failed ${name} seed ${seed} #${k}: ${String(e.message).slice(0, 400)}`); bad++; continue; }
      jsMs += t1 - t0; cppMs += performance.now() - t1; scriptsRun++;
      if (GOLD) { writeFileSync(path.join(GOLD, `${name}-${seed}-${k}.json`), JSON.stringify(script)); writeFileSync(path.join(GOLD, `${name}-${seed}-${k}.out.json`), JSON.stringify(js)); }
      const st = per.get(name) || { calls: 0, diffs: 0 }; per.set(name, st);
      for (let i = 0; i < script.calls.length; i++) {
        calls++; st.calls++;
        const d = diff(canon(js[i]), wf(canon(cpp[i])), { abs: TOL, rel: TOL, max: MAX });
        if (d.length) {
          bad++; st.diffs++;
          if (!QUIET) {
            console.log(`\n✗ ${name} seed ${seed} script ${k} call ${i} ${script.calls[i].op}`);
            for (const x of d) console.log(`   ${x.path}: js ${JSON.stringify(x.a)} | cpp ${JSON.stringify(x.b)} [${x.kind}]`);
          }
        }
      }
    }
  }
}
rmSync(tmp, { recursive: true, force: true });
for (const [n, s] of per) console.log(`${n.padEnd(22)} ${String(s.calls).padStart(6)} calls  ${s.diffs ? s.diffs + ' differ' : 'ok'}`);
console.log(`\n${calls} calls in ${scriptsRun} scripts, ${bad} differ (seeds ${seeds[0]}–${seeds[seeds.length - 1]}, tolerance ${TOL}); ${skipped} scripts skipped (ops not ported yet); JS ${jsMs.toFixed(0)} ms, C++ ${cppMs.toFixed(0)} ms incl. process start`);
process.exit(bad ? 1 : 0);
