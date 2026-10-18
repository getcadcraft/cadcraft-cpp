#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes libs/cad/src/font_widths.inc from CadCraft 0.5's font.js (charAdvance over every code point):
//   node tools/parity/fontgen.mjs > libs/cad/src/font_widths.inc
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const app = join(process.env.FORK_PARITY_JS || join(testfiles, 'fork-parity/js-0.5'), 'site/public_html/assets/apps/cadcraft/dev');
const { charAdvance } = await import(pathToFileURL(join(app, 'font.js')).href);
const rows = [];
for (let cp = 0; cp <= 0x10ffff; cp++) {
  if (cp >= 0xd800 && cp <= 0xdfff) continue;
  if (cp >= 32 && cp < 127) continue;
  const w = charAdvance(String.fromCodePoint(cp));
  const def = cp >= 0x2e80 ? 1000 : 556;
  if (w !== def) rows.push([cp, w]);
}
// runs: [first, last, width]
const runs = [];
for (const [cp, w] of rows) { const r = runs[runs.length - 1]; if (r && r[1] === cp - 1 && r[2] === w) r[1] = cp; else runs.push([cp, cp, w]); }
console.error(rows.length, 'exceptions in', runs.length, 'runs');
let out = '// Generated from CadCraft 0.5 font.js charAdvance (tools/parity/fontgen.mjs): code points outside\n// printable ASCII whose width is not the default (556 below U+2E80, 1000 from there), as runs {first, last, width}.\n';
for (let i = 0; i < runs.length; i += 4) out += runs.slice(i, i + 4).map(([a, b, w]) => `{0x${a.toString(16).toUpperCase()}, 0x${b.toString(16).toUpperCase()}, ${w}},`).join(' ') + '\n';
process.stdout.write(out);
