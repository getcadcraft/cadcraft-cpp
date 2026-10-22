#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// LISP parity (M5.8i): LISP sessions (tests/lisp/*.json: steps that evaluate text, load a file, put a file in the
// session's memory, or call a routine's command) run through CadCraft 0.5's createLisp (lisp/runtime.js, no drawing)
// and through `cadcraft-cli lisp <file>` (libs/lisp run_session). Compared, step by step: ok or not, the value as
// prin1 prints it, the error's message, whether it was a cancel, and every piece the language printed with its piece
// flag (princ / prin1 / print / prompt pieces vs whole error lines); then the routines' commands and the defun c:…
// names reported. Each session gets fresh JS modules (the lisp/ files are imported again under a new query), so the
// module-scope counters (selection set numbers, temp file names) start over, as the CLI's do.
//   node tools/parity/lisp.mjs <sessions.json>… [--detail] [--max N]
//   … --golden-out <dir>: also write the JS results of each file, canonical, as <dir>/<file name> (the libs/lisp
//     test replays the sessions against them; tests/golden/README.md)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname, basename } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { registerHooks } from 'node:module';

const site = process.env.CADCRAFT_SITE || '/var/www/CADCRAFT/getcadcraft.com';
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5');
const app = join(js05, 'site/public_html/assets/apps/cadcraft/dev');
const lispDir = pathToFileURL(join(app, 'lisp')).href + '/';
// a lisp/ module imported by a lisp/ module of session n is that session's own copy
registerHooks({
  resolve(spec, ctx, next) {
    const r = next(spec, ctx);
    const q = ctx.parentURL && ctx.parentURL.startsWith(lispDir) ? new URL(ctx.parentURL).search : '';
    if (q && r.url.startsWith(lispDir) && !r.url.includes('?')) return { ...r, url: r.url + q };
    return r;
  },
});
const { init } = await import(pathToFileURL(join(app, 'i18n.js')).href);
init({ locale: 'en', strings: JSON.parse(readFileSync(join(app, 'lang/en.json'), 'utf8')) });

const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const args = process.argv.slice(2);
const detail = args.includes('--detail');
const maxShow = args.includes('--max') ? Number(args[args.indexOf('--max') + 1]) : (detail ? 30 : 5);
const goldenOut = args.includes('--golden-out') ? resolve(args[args.indexOf('--golden-out') + 1]) : null;
const files = args.filter((a, k) => !a.startsWith('--') && !(k > 0 && ['--max', '--golden-out'].includes(args[k - 1])));

let generation = 0;
async function runJs(session) {
  const q = '?s=' + ++generation;
  const { createLisp } = await import(lispDir + 'runtime.js' + q);
  const { show } = await import(lispDir + 'types.js' + q);
  let pieces = [];
  const L = createLisp({ out: (s, p) => pieces.push([String(s), p === true]) });
  const defuns = [];
  L.onDefun = (n) => defuns.push(n);
  const steps = [];
  for (const s of session.steps) {
    pieces = [];
    let r = null;
    if (typeof s.eval === 'string') r = await L.evalText(s.eval);
    else if (s.load) { L.putFile(s.load.name, s.load.text); r = await L.loadText(s.load.text, s.load.name); }
    else if (s.put) L.putFile(s.put.name, s.put.text);
    else if (typeof s.call === 'string') r = await L.callCommand(s.call);
    const step = r ? { ok: r.ok, ...(r.ok ? { value: show(r.value) } : { error: String(r.error) }), cancel: !!r.cancel } : {};
    step.out = pieces;
    steps.push(step);
  }
  return { name: session.name, steps, commands: L.commands(), defuns };
}

let same = 0, n = 0, stepsSame = 0, stepsAll = 0;
for (const f of files) {
  const sessions = JSON.parse(readFileSync(resolve(f), 'utf8'));
  let got;
  try {
    got = execFileSync(cli, ['lisp', resolve(f)], { maxBuffer: 1 << 30 }).toString().trim().split('\n').map((l) => JSON.parse(l));
  } catch (e) { got = []; console.log(`${f}: cadcraft-cli failed: ${String(e).slice(0, 200)}`); }
  const refs = [];
  for (const [k, sc] of sessions.entries()) {
    n++;
    const ref = canon(await runJs(sc));
    refs.push(ref);
    const out = canon(got[k] || { error: 'missing' });
    for (const [i] of sc.steps.entries()) {
      stepsAll++;
      if (!diff(ref.steps?.[i], out.steps?.[i], { abs: 0, rel: 0, max: 1 }).length) stepsSame++;
    }
    const d = diff(ref, out, { abs: 0, rel: 0, max: 1e6 });
    if (!d.length) { same++; continue; }
    console.log(`DIFF ${basename(f)} ${sc.name}: ${d.length}`);
    for (const x of d.slice(0, maxShow)) console.log(`   ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 160)} cpp=${JSON.stringify(x.b)?.slice(0, 160)}`);
  }
  if (goldenOut) { mkdirSync(goldenOut, { recursive: true }); writeFileSync(join(goldenOut, basename(f)), JSON.stringify(refs)); }
}
console.log(`${same}/${n} sessions the same (${stepsSame}/${stepsAll} steps)`);
