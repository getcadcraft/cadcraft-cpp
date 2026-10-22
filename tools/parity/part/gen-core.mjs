// SPDX-License-Identifier: Apache-2.0
// pcdiff/gen-core.mjs — seeded cases for expr.js, params.js, configs.js, gdt.js, bom.js, threads.js, form.js and
// sketch-spline.js (no sketch / part documents: gen-sketch.mjs and gen-part.mjs build those).
import * as D from './driver.mjs';

export const ri = (r, a, b) => a + Math.floor(r() * (b - a + 1));
export const pick = (r, l) => l[Math.floor(r() * l.length)];
export const chance = (r, p) => r() < p;
const round = (x, d = 3) => Math.round(x * 10 ** d) / 10 ** d;

export function numLit(r) {
  const k = r();
  if (k < 0.3) return String(ri(r, 0, 200));
  if (k < 0.55) return String(round(r() * 100, ri(r, 1, 4)));
  if (k < 0.65) return '.' + ri(r, 1, 99);
  if (k < 0.72) return ri(r, 1, 9) + '.';
  if (k < 0.8) return ri(r, 1, 9) + pick(r, ['e', 'E']) + pick(r, ['', '+', '-']) + ri(r, 0, 3);
  if (k < 0.85) return pick(r, ['0', '00', '007', '1e999', '1e-320', '123456789012345678901234567890']);
  return String(round(r() * 1e6, 2));
}
const UNITS = ['mm', 'cm', 'm', 'in', 'ft', 'deg', 'rad', '°', '"', "'", ' "', " '", ' °', 'min', 'inch', 'mmx'];
const OPS = [' + ', ' - ', '*', ' / ', '%', '^', ' × ', ' − ', '÷', '·', '–', '+', '-'];
const CMPS = [' < ', '<=', ' > ', '>=', '==', '!=', '≤', '≥', '≠'];
const FN = [['sin', 1], ['cos', 1], ['tan', 1], ['asin', 1], ['acos', 1], ['atan', 1], ['atan2', 2], ['sqrt', 1], ['pow', 2], ['exp', 1], ['ln', 1], ['log', 1], ['abs', 1], ['sign', 1], ['ceil', 1], ['floor', 1], ['int', 1], ['round', 1], ['round', 2], ['min', 3], ['max', 2], ['if', 3], ['SIN', 1], ['nope', 1], ['min', 0], ['round', 3]];
export function randExpr(r, names, depth = 0) {
  const k = r();
  if (depth > 4 || k < 0.32) {
    const c = r();
    if (c < 0.5) return numLit(r) + (chance(r, 0.25) ? pick(r, UNITS) : '');
    if (c < 0.82 && names.length) return pick(r, names);
    if (c < 0.9) return 'pi';
    return pick(r, ['x', 'Width', 'w_2', '__proto__', 'constructor', 'toString', 'PI', 'm', 'in']);
  }
  if (k < 0.56) return randExpr(r, names, depth + 1) + pick(r, OPS) + randExpr(r, names, depth + 1);
  if (k < 0.66) return '(' + randExpr(r, names, depth + 1) + ')';
  if (k < 0.72) return pick(r, ['-', '+', '−', '- ']) + randExpr(r, names, depth + 1);
  if (k < 0.78) return randExpr(r, names, depth + 1) + pick(r, CMPS) + randExpr(r, names, depth + 1);
  const [f, n] = pick(r, FN);
  const args = [];
  for (let i = 0; i < n; i++) args.push(randExpr(r, names, depth + 1));
  return f + (chance(r, 0.1) ? ' ' : '') + '(' + args.join(pick(r, [', ', ',', ' ,'])) + ')';
}
function mangle(r, s) {
  const k = r();
  if (k < 0.75) return s;
  const junk = pick(r, [';', '`', '$', '#', '@', '{', '}', 'é', '中', '\t', '  ', ')', '(', ',', '.', '..', 'e', '=', '!', '<', '[', ' ', ' ']);
  const at = ri(r, 0, s.length);
  return s.slice(0, at) + junk + s.slice(at);
}
export const NAMES = ['w', 'width', 'h', 'height', 'depth', 'r', 'n', 'bolt_d', 'disc_t', 'hub_h', 'a1', 'B', 'x2', 'gap', 't'];

function genExpr(r) {
  const scripts = [];
  for (let s = 0; s < 6; s++) {
    const calls = [];
    const names = NAMES.filter(() => chance(r, 0.5));
    for (let i = 0; i < 40; i++) {
      const src = mangle(r, randExpr(r, names));
      const values = {};
      for (const n of names) if (chance(r, 0.85)) values[n] = round((r() - 0.3) * 200, ri(r, 0, 3));
      calls.push({ op: 'expr.parse', args: [src, chance(r, 0.5) ? names : null] });
      calls.push({ op: 'expr.compute', args: [src, values] });
      if (chance(r, 0.3)) calls.push({ op: 'expr.literal', args: [src] });
    }
    for (let i = 0; i < 25; i++) calls.push({ op: 'expr.literal', args: [pick(r, ['-', '', '+']) + numLit(r) + (chance(r, 0.4) ? pick(r, UNITS) : '')] });
    for (let i = 0; i < 30; i++) {
      const v = pick(r, [r() * 1000, -r() * 1e-6, (r() - 0.5) * 1e20, 0.1 + 0.2, 1 / 3, ri(r, -9, 9), 123456.7890123456, -0, 1e21, 1e-7, 5e-324, 1.0000000000005]);
      calls.push({ op: 'expr.formatNumber', args: [v] });
    }
    calls.push({ op: 'expr.formatNumber', args: ['$NaN'] }, { op: 'expr.formatNumber', args: ['$Inf'] });
    for (let i = 0; i < 15; i++) {
      const src = mangle(r, randExpr(r, names.length ? names : ['w']));
      calls.push({ op: 'expr.renameIn', args: [src + ' 25mm + 2' + pick(r, names.length ? names : ['w']) + ' 1.' + pick(r, ['w', 'e5']), pick(r, names.length ? names : ['w']), pick(r, ['q', 'zz', '(3)', 'w'])] });
    }
    for (let i = 0; i < 12; i++) {
      const n = pick(r, ['w', 'x', 'h']);
      const src = pick(r, [`${n} / 2 + 3`, `${n} ^ 2`, `sqrt(${n}) * 4`, `if(${n} > 10, ${n}, 2 * ${n})`, `sin(${n})`, `${n} * ${n} - 7 * ${n}`, `1 / ${n}`, `exp(${n} / 50)`, `3`, `${n} % 7`, `round(${n}, 1)`]);
      const values = { [n]: chance(r, 0.8) ? round(r() * 100, 2) : 0, other: 5 };
      calls.push({ op: 'expr.solveFor', args: [src, values, n, round((r() - 0.2) * 200, 2)] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

export function randParams(r, { cycles = true } = {}) {
  const p = { vars: [], links: [] };
  const n = ri(r, 0, 9);
  const used = [];
  for (let i = 0; i < n; i++) {
    const name = pick(r, NAMES.filter((x) => !used.includes(x)).concat(['v' + i]));
    used.push(name);
    const pool = cycles ? used : used.slice(0, -1);
    const expr = chance(r, 0.45) ? numLit(r) + (chance(r, 0.2) ? pick(r, ['mm', 'in', 'deg']) : '') : randExpr(r, pool.filter(() => chance(r, 0.6)));
    const v = { name, expr };
    if (chance(r, 0.2)) v.note = pick(r, ['wall', 'the disc\u0001 thickness', '', 'x'.repeat(250)]);
    p.vars.push(v);
  }
  return p;
}

function genParams(r) {
  const scripts = [];
  for (let s = 0; s < 8; s++) {
    const calls = [];
    for (let k = 0; k < 6; k++) {
      const p = randParams(r, { cycles: chance(r, 0.4) });
      const names = p.vars.map((v) => v.name);
      calls.push({ op: 'params.evaluateVars', args: [p, null] });
      const ov = {}; for (const nm of names) if (chance(r, 0.3)) ov[nm] = round(r() * 50, 2);
      calls.push({ op: 'params.evaluateVars', args: [p, ov] });
      calls.push({ op: 'params.setVar', args: [p, pick(r, NAMES.concat(['sin', 'mm', 'PI', '2x', 'a-b', '__proto__', 'constructor', 'M'])), mangle(r, randExpr(r, names)), chance(r, 0.3) ? { note: pick(r, ['n', '', null]) } : {}] });
      if (names.length) {
        calls.push({ op: 'params.setVar', args: [p, pick(r, NAMES), pick(r, ['3', names[0] + ' + 1', '2 * ' + pick(r, names)]), { rename: pick(r, names) }] });
        const nm = pick(r, names);
        calls.push({ op: 'params.removeVar', args: [p, nm, chance(r, 0.5)] });
        calls.push({ op: 'params.driveVar', args: [p, nm, round((r() - 0.2) * 150, 2), chance(r, 0.2) ? pick(r, names) : null] });
        calls.push({ op: 'params.drivingBehind', args: [p, randExpr(r, names)] });
        calls.push({ op: 'params.usesOf', args: [p, nm] });
        const ovs = {}; for (const x of names) if (chance(r, 0.4)) ovs[x] = round(r() * 9, 1);
        calls.push({ op: 'params.applyOverrides', args: [p, chance(r, 0.15) ? { nope: 1 } : ovs] });
        // design tables
        const named = chance(r, 0.6);
        const cols = names.filter(() => chance(r, 0.6));
        if (chance(r, 0.1)) cols.push('ghost');
        const sep = pick(r, [',', ';', '\t']);
        const lines = [(named ? [pick(r, ['name', 'Config', 'CONFIGURATION', 'x'])] : []).concat(cols).join(sep)];
        for (let i = 0; i < ri(r, 0, 4); i++) lines.push((named ? [pick(r, ['"Big, wide"', 'small', '', ' row ', '"q""uote"'])] : []).concat(cols.map(() => pick(r, [numLit(r), '', '2in', 'w*2', ' 5 ', '-3']))).join(sep));
        calls.push({ op: 'params.readDesignTable', args: [(chance(r, 0.2) ? '﻿' : '') + lines.join(pick(r, ['\n', '\r\n'])) + (chance(r, 0.5) ? '\n\n' : ''), p] });
      }
      // files
      const raw = structuredClone(p);
      if (chance(r, 0.5)) raw.links = [{ target: 'f1.depth', expr: '2' }, { target: pick(r, ['f2.d', '../x', 'f1.depth', '', 'a'.repeat(130)]), expr: pick(r, ['w', '1 +', 'x'.repeat(1001)]) }];
      const t = r();
      if (t < 0.1) raw.vars = 'x'; else if (t < 0.15) raw.vars.push(5); else if (t < 0.2) raw.vars.push({ name: 'w', expr: 3 }); else if (t < 0.25) raw.vars.push(null);
      calls.push({ op: 'params.read', args: [chance(r, 0.05) ? [1, 2] : raw, chance(r, 0.5) ? ['f1.depth', 'f2.d'] : null] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

function genConfigs(r) {
  const scripts = [];
  for (let s = 0; s < 6; s++) {
    const calls = [];
    for (let k = 0; k < 8; k++) {
      const p = randParams(r, { cycles: false });
      const names = p.vars.map((v) => v.name);
      const rows = [];
      for (let i = 0; i < ri(r, 0, 4); i++) {
        const values = {}; for (const n of names) if (chance(r, 0.5)) values[n] = round(r() * 100, 2);
        rows.push({ name: pick(r, ['Small', 'small', 'Big', ' Large\u0001 ', '', 'M' + i, 'x'.repeat(70), '=SUM', '-5', '+a']), values });
      }
      const raw = { rows, active: pick(r, ['Small', 'Big', null, 'nope', 5]) };
      if (chance(r, 0.1) && rows[0]) rows[0].values.ghost = 1;
      if (chance(r, 0.1) && rows[0] && names[0]) rows[0].values[names[0]] = 'x';
      calls.push({ op: 'configs.read', args: [chance(r, 0.05) ? { rows: 'x' } : raw, p] });
      calls.push({ op: 'configs.rowFromCurrent', args: [p, pick(r, ['Row 1', ' new\u0007', ''])] });
      const cfg = rows.length ? { rows: rows.filter((x, i) => rows.findIndex((y) => y.name.toLowerCase() === x.name.toLowerCase()) === i && x.name.trim()), active: null } : null;
      const row = { name: pick(r, ['small', 'New', 'BIG']), values: Object.fromEntries(names.filter(() => chance(r, 0.5)).map((n) => [n, round(r() * 9, 1)])) };
      calls.push({ op: 'configs.saveRow', args: [cfg, row] });
      calls.push({ op: 'configs.applyRow', args: [p, chance(r, 0.1) ? { name: 'g', values: { ghost: 1 } } : row] });
      calls.push({ op: 'configs.rowMatches', args: [p, row] });
      if (cfg && cfg.rows.length) {
        calls.push({ op: 'configs.renameRow', args: [cfg, cfg.rows[0].name, pick(r, ['Renamed', cfg.rows[cfg.rows.length - 1].name.toUpperCase(), '', '  x  '])] });
        calls.push({ op: 'configs.deleteRow', args: [cfg, pick(r, cfg.rows).name] });
        const after = structuredClone(p);
        if (after.vars.length && chance(r, 0.5)) after.vars[0].name = 'renamed';
        if (after.vars.length > 1 && chance(r, 0.3)) after.vars.pop();
        calls.push({ op: 'configs.followParams', args: [cfg, p, after] });
        calls.push({ op: 'configs.csv', args: [cfg] });
      }
      if (names.length) calls.push({ op: 'configs.rowsFromTable', args: [['name', ...names].join(',') + '\n' + ['A', ...names.map(() => numLit(r))].join(',') + '\nB,' + names.map(() => pick(r, ['1', 'w', ''])).join(','), p] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

const CHARS = ['straightness', 'flatness', 'circularity', 'cylindricity', 'profileLine', 'profileSurface', 'perpendicularity', 'angularity', 'parallelism', 'position', 'concentricity', 'symmetry', 'circularRunout', 'totalRunout', 'nope', 'toString'];
function randNote(r, i) {
  const kind = pick(r, ['datum', 'fcf', 'finish', 'finish', 'fcf', 'other']);
  const n = { id: chance(r, 0.95) ? 'g' + (i + 1) : pick(r, ['g', 'x1', 'g1234567890']), kind, view: pick(r, ['front', 'top', 'right', 'iso', 'section', 'detail', 'side']), at: [round((r() - 0.5) * 200, 2), round((r() - 0.5) * 200, 2)], off: [round((r() - 0.5) * 60, 2), round((r() - 0.5) * 60, 2)] };
  if (chance(r, 0.05)) n.at = [1e7, 0];
  if (kind === 'datum') n.letter = pick(r, ['A', 'B', 'I', 'O', 'Q', 'Z', 'AB', '', 'a', '\u0001C']);
  if (kind === 'finish') Object.assign(n, { ra: pick(r, [1.6, 0.8, 3.2, 0, -1, 2000, 12.5]), process: pick(r, ['any', 'removal', 'noRemoval', 'cut', 'toString']), note: pick(r, ['', 'ground', 'x'.repeat(50), 'lap\u0085ped']) });
  if (kind === 'fcf') {
    const ds = [];
    for (let k = 0; k < ri(r, 0, 4); k++) ds.push(pick(r, [{ letter: pick(r, ['A', 'B', 'C', 'A', 'I']), mod: pick(r, ['', 'M', 'L', 'X']) }, null, 0, { letter: 'D' }]));
    Object.assign(n, { char: pick(r, CHARS), dia: chance(r, 0.4), tol: pick(r, [0.1, 0.05, 0, 0.0125, 1001]), mod: pick(r, ['', 'M', 'L', 'Q', undefined]), datums: ds });
  }
  return n;
}
function genGdt(r) {
  const scripts = [];
  for (let s = 0; s < 5; s++) {
    const calls = [];
    for (let k = 0; k < 30; k++) {
      const notes = []; for (let i = 0; i < ri(r, 0, 5); i++) notes.push(randNote(r, i));
      calls.push({ op: 'gdt.readNotes', args: [chance(r, 0.03) ? 'x' : notes] });
      for (const n of notes) {
        calls.push({ op: 'gdt.validate', args: [n] });
        let ok = null; try { ok = D.G.readNotes([n]); } catch { ok = null; }
        if (ok) { calls.push({ op: 'gdt.describe', args: [ok[0]] }); calls.push({ op: 'gdt.geometry', args: [ok[0], [round(r() * 400, 2), round(r() * 280, 2)]] }); }
      }
      calls.push({ op: 'gdt.newAnnotation', args: [pick(r, ['datum', 'fcf', 'finish', 'x']), pick(r, [{}, { char: pick(r, CHARS.slice(0, 14)) }, { id: 'g9', letter: 'C', at: [1, 2] }, { ra: 3.2, process: 'any', note: 'n' }, { datums: [{ letter: 'B' }] }])] });
    }
    calls.push({ op: 'gdt.textW', args: ['Ra 1.6 ⌀ Ø é ü € ™ 中 \t x', 3.5] });
    scripts.push({ calls });
  }
  return scripts;
}

function genBom(r) {
  const calls = [];
  for (let k = 0; k < 40; k++) {
    calls.push({ op: 'bom.readMaterial', args: [pick(r, [null, { name: 'Steel', density: 7.85 }, { density: 2.7 }, { name: ' \u0001Alu ', density: 2.7 }, { name: 'x', density: 0 }, { name: 'x', density: 26 }, 'steel', { name: 5, density: 1 }, [1]])] });
    calls.push({ op: 'bom.readPartProps', args: [pick(r, [null, { number: ' 123 ', description: 'Bracket\u0002', material: { name: 'Brass', density: 8.5 } }, { number: 'x'.repeat(50) }, { material: { density: 'x' } }, { number: '' }])] });
    calls.push({ op: 'bom.presetOf', args: [pick(r, [null, { name: 'Steel (A36)', density: 7.85 }, { name: 'Acero', density: 7.85 }, { name: 'Custom', density: 7.85 }, { name: 'q', density: 1.24 }, { name: 'q', density: 9 }])] });
    calls.push({ op: 'bom.csvCell', args: [pick(r, ['=1+1', 'a,b', ' x', 'x ', 'q"r', '\tt', 12.5, null, '-3', 'plain', '@x']), chance(r, 0.5) ? { text: false } : {}] });
  }
  calls.push({ op: 'bom.materials', args: [] });
  const parts = { p1: { name: 'Plate', number: 'P-1', material: { name: 'Steel', density: 7.85 } }, p2: { name: 'Pin, short', description: '=bad' }, p3: { name: 'Nut' } };
  const asm = { parts, instances: [{ part: 'p1' }, { part: 'p2' }, { part: 'p1' }, { part: 'p3' }, { part: 'p1' }] };
  for (const vols of [{ p1: { volume: 1234.5678, exact: true }, p2: { volume: 10 } }, {}, { p1: { volume: 1e6 }, p2: { volume: 2 }, p3: { volume: 3 } }]) {
    calls.push({ op: 'bom.rows', args: [asm, vols] });
    calls.push({ op: 'bom.csv', args: [D.BOM.bomRows(asm, new Map(Object.entries(vols).map(([k, v]) => [k, { stats: v }])))] });
  }
  return [{ calls }];
}

function genThreads(r) {
  const calls = [];
  const sizes = ['M1.6', 'M3', 'M6', 'M10', 'M24', 'M64', 'Tr8x1_5', 'Tr16x4', 'Tr60x9', 'M7', 'constructor'];
  for (const form of ['iso', 'tr', 'x']) for (const size of sizes) {
    calls.push({ op: 'threads.dims', args: [form, size] });
    calls.push({ op: 'threads.profileCorners', args: [form, size, pick(r, [0, -0.1, 0.1, 0.25])] });
    calls.push({ op: 'threads.callout', args: [form, size, { external: chance(r, 0.5), length: round(r() * 40, 3), through: chance(r, 0.3), modelled: chance(r, 0.3) }] });
    calls.push({ op: 'threads.sizeLabel', args: [form, size] });
  }
  for (let k = 0; k < 40; k++) calls.push({ op: 'threads.sizeFor', args: [pick(r, ['iso', 'tr']), round(r() * 70, 3), chance(r, 0.5)] });
  for (const [form, size] of [['iso', 'M10'], ['tr', 'Tr16x4'], ['iso', 'M3']]) {
    const c = D.TH.profileCorners(form, size, 0.1);
    for (let k = 0; k < 10; k++) calls.push({ op: 'threads.profileAt', args: [c, round((r() - 0.5) * 20, 4)] });
    calls.push({ op: 'threads.boltSection', args: [c, { hand: pick(r, ['right', 'left']), segments: pick(r, [96, 48, 64, 10, 100.5]) }] });
  }
  return [{ calls }];
}

function genForm(r) {
  const calls = [];
  for (const a of [[1, 1, 1, 100, 100, 100], [2, 2, 2, 100, 100, 100], [3, 1, 2, 10, 20, 30], [0, 40, 1.6, 5, 5, 5]]) calls.push({ op: 'form.box', args: a });
  for (let k = 0; k < 20; k++) {
    const f = D.FM.box(ri(r, 1, 2), ri(r, 1, 2), 1, 50, 60, 70);
    const t = r();
    if (t < 0.15) f.faces.push([0, 1, 1]);
    else if (t < 0.25) f.verts.push([1, 2]);
    else if (t < 0.35) f.faces[0] = [...f.faces[0]].reverse();
    else if (t < 0.45) f.creases = [[0, 1, 5], [1, 0, 2], [0, 7, 1]];
    else if (t < 0.55) f.creases = [[f.faces[0][0], f.faces[0][1], pick(r, [0, 11, 3])]];
    else if (t < 0.6) f.corners = [0, 1, 1, 2];
    else if (t < 0.65) f.sym = { axis: pick(r, ['x', 'w', 'toString']), at: 0, side: pick(r, [1, -1, 2, undefined]) };
    else if (t < 0.7) f.version = 2;
    else if (t < 0.75) f.format = 'x';
    calls.push({ op: 'form.read', args: [chance(r, 0.2) ? JSON.stringify(f) : f] });
  }
  calls.push({ op: 'form.read', args: ['{'] });
  return [{ calls }];
}

function genSpline(r) {
  const scripts = [];
  for (let s = 0; s < 4; s++) {
    const calls = [];
    for (let k = 0; k < 10; k++) {
      const n = ri(r, 2, 7);
      const Q = []; for (let i = 0; i < n; i++) Q.push([round(i * 10 + r() * 8, 3), round((r() - 0.5) * 30, 3)]);
      const closed = n >= 3 && chance(r, 0.3);
      const o = closed ? { closed: true } : { a0: chance(r, 0.4) ? round(r() * 6, 3) : null, a1: chance(r, 0.4) ? round(r() * 6, 3) : null };
      if (chance(r, 0.05)) Q.push(Q[Q.length - 1]);
      calls.push({ op: 'spline.interpFit', args: [Q, o] });
      calls.push({ op: 'spline.controlCurve', args: [Q, { closed }] });
      let c; try { c = D.SP.interpFit(Q, o); } catch { continue; }
      const cc = D.SP.controlCurve(Q, { closed });
      for (const curve of [c, cc]) {
        const [lo, hi] = D.SP.domain(curve);
        calls.push({ op: 'spline.tessellate', args: [curve, ri(r, 1, 16)] });
        calls.push({ op: 'spline.tessellateTol', args: [curve, pick(r, [0.01, 0.1, 0.001])] });
        calls.push({ op: 'spline.areaTerm', args: [curve] });
        calls.push({ op: 'spline.arcLength', args: [curve] });
        const u = lo + (hi - lo) * r();
        calls.push({ op: 'spline.pointAt', args: [curve, u] }, { op: 'spline.derivsAt', args: [curve, u, ri(r, 0, 4)] }, { op: 'spline.curvatureAt', args: [curve, u] });
        calls.push({ op: 'spline.closestParam', args: [curve, [round(r() * 60, 2), round((r() - 0.5) * 40, 2)], ri(r, 4, 12)] });
        if (!curve.periodic) {
          calls.push({ op: 'spline.insertKnot', args: [curve, u] });
          calls.push({ op: 'spline.reverse', args: [curve] }, { op: 'spline.normalizeKnots', args: [curve] });
        }
        const a = lo + (hi - lo) * r() * 0.5, b = a + (hi - lo) * (0.1 + r() * (curve.periodic ? 0.9 : 0.4));
        calls.push({ op: 'spline.segment', args: [curve, a, b] });
        calls.push({ op: 'spline.offsetFit', args: [curve, pick(r, [0.5, -0.5, 2, -3, 20]), pick(r, [0.001, 0.01])] });
      }
      const m = ri(r, 2, 6), p = Math.min(3, m - 1);
      const U = D.SP.clampedUniform(m, p);
      if (chance(r, 0.5)) U[ri(r, 0, U.length - 1)] = pick(r, [0.5, -1, 9, 'x']);
      calls.push({ op: 'spline.knotProblem', args: [chance(r, 0.1) ? null : U, m] });
    }
    scripts.push({ calls });
  }
  return scripts;
}

export const GENS = { expr: genExpr, params: genParams, configs: genConfigs, gdt: genGdt, bom: genBom, threads: genThreads, form: genForm, spline: genSpline };
