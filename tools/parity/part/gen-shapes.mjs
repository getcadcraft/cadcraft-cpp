// SPDX-License-Identifier: Apache-2.0
// tools/parity/part/gen-shapes.mjs — lib/shapes (sections.js, parts.js, items.js): every section's record, outline
// (seg 6 and 8), exact area and loop measures; every part family × size with its defaults; library items for every
// section (section view) and every LIBRARY_SET part; then seeded cases: names in any case and with junk, hand-made
// profiles (plates, odd seg counts, zero and negative sizes), untrusted part options (strings, blanks, arrays,
// booleans, out of range, non-integers), elevations of any length, and local items.
import * as D from './driver.mjs';
import { ri, pick, chance } from './gen-core.mjs';

const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;
const chunks = (list, n) => { const out = []; for (let i = 0; i < list.length; i += n) out.push(list.slice(i, i + n)); return out; };

function fixed(every) {
  const scripts = [];
  if (every === 1) return sectionScripts(D.SEC.sectionRows());
  scripts.push({ calls: ['shapes.constants', 'shapes.sources', 'shapes.families', 'shapes.partStandards', 'shapes.partFamilies',
    'shapes.librarySet', 'shapes.partTags', 'shapes.cc0', 'shapes.sectionRows'].map((op) => ({ op, args: [] })) });
  scripts.push(...sectionScripts(D.SEC.sectionRows().filter((_, i) => i % every === 0)));
  scripts.push({ calls: ['W', 'C', 'L', 'Pipe', 'IPE', 'UPN', 'nope'].map((id) => ({ op: 'shapes.familyRows', args: [id] })) });
  const calls = [];
  for (const f of D.PARTS.PART_FAMILIES) {
    calls.push({ op: 'shapes.findFamily', args: [f.id] });
    calls.push({ op: 'shapes.partSizes', args: [f.id] });
    for (const z of D.PARTS.partSizes(f.id)) {
      calls.push({ op: 'shapes.partOptions', args: [f.id, z.size] });
      calls.push({ op: 'shapes.makePart', args: [f.id, z.size, null] });
      calls.push({ op: 'shapes.partItem', args: [f.id, z.size, null] });
    }
  }
  scripts.push({ calls });
  scripts.push({ calls: D.PARTS.LIBRARY_SET.map(([id, size, opts]) => ({ op: 'shapes.partItem', args: [id, size, opts] })) });
  return scripts;
}

function sectionScripts(rows) {
  const scripts = [];
  for (const part of chunks(rows, 60)) {
    const calls = [];
    for (const s of part) {
      calls.push({ op: 'shapes.findSection', args: [s.name] });
      calls.push({ op: 'shapes.exactArea', args: [s] });
      for (const seg of [6, 8]) calls.push({ op: 'shapes.outline', args: [s, seg] });
      const loops = D.SEC.outline(s, 6);
      for (const l of loops) for (const op of ['shapes.bbox', 'shapes.shoelace', 'shapes.loopLength']) calls.push({ op, args: [l] });
      calls.push({ op: 'shapes.sectionItem', args: [s.name, null] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

const JUNK = [' ', '  ', '\t', 'x', 'X', '-', '.', '/', '0', 'é', '中', ''];
function mangleName(r, n) {
  const k = r();
  if (k < 0.3) return n.toLowerCase();
  if (k < 0.45) return '  ' + n + ' \t';
  if (k < 0.55) return n.replace(/x/g, 'X');
  if (k < 0.7) { const at = ri(r, 0, n.length); return n.slice(0, at) + pick(r, JUNK) + n.slice(at); }
  if (k < 0.8) return n.slice(0, ri(r, 0, n.length));
  if (k < 0.85) return pick(r, ['', ' ', 'W', 'HSS', 'PIPE4STD', 'pipe4std', 'L4X4X1/2', 'l4x4x1/2', 'HEB1000', 'UPN50', 'ipe 300']);
  return n;
}
const RAW_NAMES = ['W12X26', 'W12x26', 'HSS4X4X1/4', 'HSS4.500X0.237', 'HSS6.625X0.280', 'Pipe4STD', 'L4X3X1/2', 'C15X33.9',
  'X1X', '1X2', 'AX1', '1/X2', '.X5', 'X', '', 'W10X49', 'S24X121', 'HSS28.000X1.000', '12X', 'x12X3X'];

function profile(r) {
  const kind = pick(r, ['I', 'C', 'L', 'RHS', 'CHS', 'PL', 'T', '']);
  const v = () => (chance(r, 0.08) ? pick(r, [0, -5, 1e-9, 1e6]) : round(r() * 400 + 1, ri(r, 0, 3)));
  const p = { name: 'P' + ri(r, 1, 999), kind };
  if (kind === 'I' || kind === 'C') Object.assign(p, { d: v(), bf: v(), tw: v() / 10, tf: v() / 10, r: chance(r, 0.4) ? 0 : v() / 20 });
  if (kind === 'L') { Object.assign(p, { b: v(), t: v() / 10 }); if (chance(r, 0.5)) p.h = v(); }
  if (kind === 'RHS') { const t = v() / 20; Object.assign(p, { h: v(), b: v(), t, ro: 2 * t, ri: t }); }
  if (kind === 'CHS') Object.assign(p, { D: v(), t: v() / 20 });
  if (kind === 'PL') Object.assign(p, { b: v(), t: v() / 10 });
  if (chance(r, 0.1)) p.areaPub = v();
  return p;
}
const OPT_VALUES = [12, 40, 0, -1, 4, 500, 501, 3.5, 20.5, 1e9, '', '12', ' 40 ', 'abc', '1e2', '0x10', true, false, [30], [1, 2], {}, null, '$null'];

function genShapes(r) {
  const scripts = [];
  const rows = D.SEC.sectionRows();
  let calls = [];
  for (let i = 0; i < 60; i++) calls.push({ op: 'shapes.findSection', args: [mangleName(r, pick(r, rows).name)] });
  for (let i = 0; i < 20; i++) calls.push({ op: 'shapes.findSection', args: [pick(r, [5, null, true, ['W12X26'], { name: 'W12X26' }])] });
  for (const n of RAW_NAMES) calls.push({ op: 'shapes.displayName', args: [chance(r, 0.3) ? mangleName(r, n) : n] });
  calls.push({ op: 'shapes.displayName', args: [12] });
  for (let i = 0; i < 6; i++) calls.push({ op: 'shapes.familyRows', args: [pick(r, ['W', 'w', 'HSS', 'Pipe', 'PL', '', 'IPE', 'HEB'])] });
  scripts.push({ calls });
  calls = [];
  for (let i = 0; i < 80; i++) {
    const p = profile(r);
    const seg = pick(r, [null, 1, 2, 3, 6, 8, 2.5, 0, 12]);
    calls.push({ op: 'shapes.exactArea', args: [p] });
    calls.push({ op: 'shapes.outline', args: [p, seg] });
    for (const l of D.SEC.outline(p, seg ?? undefined)) {
      calls.push({ op: 'shapes.bbox', args: [l] });
      calls.push({ op: 'shapes.shoelace', args: [l] });
      calls.push({ op: 'shapes.loopLength', args: [l] });
    }
  }
  calls.push({ op: 'shapes.bbox', args: [[]] }, { op: 'shapes.shoelace', args: [[]] }, { op: 'shapes.loopLength', args: [[[1, 2]]] });
  scripts.push({ calls });
  calls = [];
  const fams = D.PARTS.PART_FAMILIES.map((f) => f.id).concat(['nope', '']);
  for (let i = 0; i < 120; i++) {
    const id = pick(r, fams);
    const sizes = D.PARTS.partSizes(id).map((z) => z.size);
    const size = sizes.length && chance(r, 0.85) ? pick(r, sizes) : pick(r, ['M7', '', 'm1', 'M8 ', 5, null, '6000', '2x2']);
    const opts = {};
    for (const k of ['length', 'teeth', 'width', 'bore', 'junk']) if (chance(r, 0.4)) opts[k] = pick(r, OPT_VALUES);
    if (opts.length === undefined && chance(r, 0.5) && (id === 'asme-hex-bolt')) opts.length = pick(r, [0.25, 0.5, 1, 1.5, 6, 6.5, 24, 25, 1 / 3, 2.0625]);
    const o = chance(r, 0.08) ? pick(r, ['$null', 'abc', [], 7]) : opts;
    calls.push({ op: 'shapes.partOptions', args: [id, size] });
    calls.push({ op: 'shapes.makePart', args: [id, size, o] });
    calls.push({ op: 'shapes.partItem', args: [id, size, o === '$null' ? null : o] });
  }
  scripts.push({ calls });
  calls = [];
  for (let i = 0; i < 40; i++) {
    const n = pick(r, rows).name;
    const o = pick(r, [{ view: 'elevation' }, { view: 'elevation', length: pick(r, [3000, 1, 0, -5, 1e6, 1e6 + 1, 1234.5678, '2500', 'abc', null, 6096.4, 999999.995]) },
      { view: 'section' }, { view: 'ELEVATION' }, {}, { length: 10 }]);
    calls.push({ op: 'shapes.sectionItem', args: [n, o] });
    const it = (() => { try { return ITEM(n, o); } catch { return null; } })();
    if (it) calls.push({ op: 'shapes.localItem', args: [it] });
    calls.push({ op: 'shapes.memberLoops', args: [n] });
  }
  calls.push({ op: 'shapes.sectionItem', args: ['NOPE', null] });
  for (const it of [{ name: 'x' }, { name: 'y', source_attrs: { 'CC-PART-STD': '' } }, { name: 5, source_attrs: { 'CC-PART-STD': 'ISO 1' }, license: 1 }, {}]) calls.push({ op: 'shapes.localItem', args: [it] });
  scripts.push({ calls });
  return scripts;
}
const ITEM = (n, o) => D.ITEMS.sectionItem(D.SEC.findSection(n), o ?? undefined);

// `shapes` takes every 9th section (the committed goldens are seed 1 of it); `shapes-all` every section, once.
export const GENS = {
  shapes: (r, seed) => (seed === 1 ? fixed(9) : []).concat(genShapes(r)),
  'shapes-all': (r, seed) => (seed === 1 ? fixed(1) : []),
};
