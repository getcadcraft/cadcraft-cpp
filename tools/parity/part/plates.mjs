// SPDX-License-Identifier: Apache-2.0
// pcdiff/plates.mjs — the big PartCraft test parts (M13): a perforated plate with an N × N grid of through holes (one hole,
// a linear pattern of it, a linear pattern of both), as .pcscript and as .partcraft.json (0.5 script.js scriptToPart →
// part.js toJSON). Written to testfiles/generated/partcraft/plate-<N>.{pcscript,partcraft.json} only when missing.
//   node pcdiff/plates.mjs [N…]   (default 10 15 20 30 40; registered in testfiles/LARGE-FILES.md)
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
const DEV = (process.env.FORK_PARITY_JS || (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/fork-parity/js-0.5') + '/site/public_html/assets/apps/partcraft/dev/';
const OUT = (process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles') + '/generated/partcraft/';
const I18N = await import(DEV + 'i18n.js');
I18N.init({ locale: 'en', strings: JSON.parse(readFileSync(DEV + 'lang/en.json', 'utf8')) });
const SC = await import(DEV + 'script.js');
const P = await import(DEV + 'part.js');
const script = (N) => {
  const L = N * 20 + 20;
  return `pcscript 1\npart "Perforated plate ${N} x ${N}"\n  resolution 64\n\nparam pitch = 20\nparam n = ${N}\nparam bore = 9\n\nsketch Outline\n  on top\n  point a 0, 0\n  point b ${L}, 0\n  point c ${L}, -${L}\n  point d 0, -${L}\n  line l1 a b\n  line l2 b c\n  line l3 c d\n  line l4 d a\n\nextrude Plate\n  sketch Outline\n  depth 8\n\nsketch Centre\n  on face Plate end\n  point h 20, -20\n\nhole Hole\n  sketch Centre\n  d = bore\n  through\n\nlpattern Row\n  seeds Hole\n  dir x\n  count = n\n  spacing = pitch\n\nlpattern Grid\n  seeds Hole, Row\n  dir z\n  count = n\n  spacing = pitch\n`;
};
mkdirSync(OUT, { recursive: true });
const ns = process.argv.slice(2).map(Number).filter((n) => n > 0);
for (const N of ns.length ? ns : [10, 15, 20, 30, 40]) {
  const s = OUT + `plate-${N}.pcscript`, j = OUT + `plate-${N}.partcraft.json`;
  if (!existsSync(s)) writeFileSync(s, script(N));
  if (!existsSync(j)) writeFileSync(j, P.toJSON(SC.scriptToPart(readFileSync(s, 'utf8')).part));
}
