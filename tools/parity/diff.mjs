#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Every differing path between the parity kit's JS reference and this fork's `cadcraft-cli parity dwg` output, grouped
// by path shape. The kit (tools/fork-parity in the CadCraft site repo) and its references live outside this repo:
//   CADCRAFT_SITE=/path/to/getcadcraft.com node tools/parity/diff.mjs <file.dwg> [--max 400] [--all]
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { join, relative, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = process.env.CADCRAFT_SITE;
if (!site) { console.error('set CADCRAFT_SITE to the site repo'); process.exit(2); }
const { canon, diff } = await import(join(site, 'tools/fork-parity/lib/canon.mjs'));
const args = process.argv.slice(2);
const file = resolve(args[0]);
const max = Number(args[args.indexOf('--max') + 1]) || 400;
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const ref = JSON.parse(gunzipSync(readFileSync(join(testfiles, 'fork-parity/ref/dwg', relative(testfiles, file) + '.json.gz')))).value;
const here = dirname(fileURLToPath(import.meta.url));
const cli = process.env.CC_CLI || join(here, '../../build/release/apps/cadcraft-cli/cadcraft-cli');
const out = JSON.parse(execFileSync(cli, ['parity', 'dwg', file], { maxBuffer: 1 << 30 }).toString());
const drop = (m) => { if (m.report) delete m.report.ms; return m; };
const d = diff(canon(drop(ref)), canon(drop(out)), { abs: 1e-9, rel: 1e-12, max: 1e6 });
const groups = new Map();
for (const x of d) {
  const k = x.path.replace(/\[\d+\]/g, '[]');
  if (!groups.has(k)) groups.set(k, []);
  groups.get(k).push(x);
}
console.log(`${d.length} differences in ${groups.size} path shapes`);
for (const [k, list] of [...groups].sort((a, b) => b[1].length - a[1].length).slice(0, args.includes('--all') ? 1e9 : 40)) {
  const x = list[0];
  console.log(`${String(list.length).padStart(6)}  ${k}  e.g. ${x.path}: js=${JSON.stringify(x.a)?.slice(0, 80)} cpp=${JSON.stringify(x.b)?.slice(0, 80)} (${x.kind})`);
}
