// SPDX-License-Identifier: Apache-2.0
// gen-civil.mjs (from the Rust fork's cvdiff) — call-script cases for the design modules: grading, corridors + superelevation, pipes + drainage, sections,
// feature lines, description keys, parcels, traverses, staking, mass haul, solve-why, clip, point edits, drive + sun,
// and the project (edits, undo, derived results, the project file).
import { R, I, pick, chance, origin, terrain, randPoints, randLine, randPoly } from './gen-core.mjs';

const site = (rnd, n = 220, size) => randPoints(rnd, n, { size: size || R(rnd, 300, 1500) });
function road(rnd, ox, oy, size, n = 3) {
  const pis = [];
  let x = ox + size * 0.1, y = oy + size * R(rnd, 0.2, 0.8), dir = R(rnd, -0.4, 0.4);
  for (let i = 0; i < n; i++) { pis.push({ x, y, r: i === 0 || i === n - 1 ? 0 : R(rnd, size * 0.1, size * 0.5) }); dir += R(rnd, -0.8, 0.8); const L = size * R(rnd, 0.25, 0.45); x += L * Math.cos(dir); y += L * Math.sin(dir); }
  return pis;
}
function pvisFor(rnd, start, len, zf) {
  const out = [];
  const k = I(rnd, 2, 5);
  for (let i = 0; i < k; i++) { const sta = start + (len * i) / (k - 1); out.push({ sta, z: zf + R(rnd, -6, 6), L: i === 0 || i === k - 1 ? 0 : pick(rnd, [0, R(rnd, 40, 200)]) }); }
  return out;
}
const features = (rnd, ox, oy, size, f) => {
  const out = [];
  for (let i = 0; i < I(rnd, 1, 4); i++) {
    const closed = chance(rnd, 0.5);
    const pts = closed ? randPoly(rnd, ox, oy, size * 0.4, I(rnd, 3, 7)).map((p) => ({ x: p.x + size * 0.2, y: p.y + size * 0.2, z: Math.round(f(p.x, p.y) * 100) / 100 + R(rnd, -2, 2) })) : randLine(rnd, ox, oy, size, f, I(rnd, 2, 6));
    out.push({ id: 'f' + (i + 1), name: 'FL ' + (i + 1), pts, closed, use: pick(rnd, ['eg', 'design', 'design', 'off']), daylight: chance(rnd, 0.5), cut: pick(rnd, [2, 3]), fill: pick(rnd, [2, 3, 4]) });
  }
  return out;
};
function network(rnd, ox, oy, size, withHydro) {
  const n = I(rnd, 2, 7), structures = [], pipes = [];
  for (let i = 0; i < n; i++) structures.push({ id: 's' + (i + 1), name: 'MH' + (i + 1), x: ox + size * R(rnd, 0.1, 0.9), y: oy + size * R(rnd, 0.1, 0.9), rim: chance(rnd, 0.3) ? R(rnd, 80, 1200) : null, sumpDepth: pick(rnd, [0, 0.5, 1]), dia: 48 });
  for (let i = 1; i < n; i++) {
    const from = 's' + (i + 1), to = 's' + (chance(rnd, 0.8) ? i : I(rnd, 1, n));
    const up = R(rnd, 60, 1100), dn = up - R(rnd, -0.5, 4);
    pipes.push({ id: 'q' + i, name: 'P' + i, from, to, dia: pick(rnd, [12, 15, 18, 24, 300]), invUp: chance(rnd, 0.1) ? null : up, invDn: chance(rnd, 0.1) ? null : dn });
  }
  const net = { name: 'Storm', type: 'storm', rules: { minSlope: pick(rnd, [0, 0.005, 0.01]), minCover: pick(rnd, [0, 2, 0.6]), minDrop: pick(rnd, [0, 0.1, 0.03]) }, structures, pipes };
  if (withHydro) {
    net.catchments = [];
    for (let k = 0; k < I(rnd, 0, 4); k++) { const c = randPoly(rnd, ox, oy, size * 0.3, I(rnd, 3, 6)); net.catchments.push({ id: 'k' + (k + 1), name: 'C' + (k + 1), pts: c, C: R(rnd, 0.2, 0.9), tc: R(rnd, 5, 30), to: chance(rnd, 0.9) ? pick(rnd, structures).id : 'nope' }); }
    if (chance(rnd, 0.7)) net.hydro = { idf: { a: R(rnd, 40, 120), b: R(rnd, 5, 15), c: R(rnd, 0.6, 0.9) }, n: pick(rnd, [0.013, 0.012, 0.024]), kj: R(rnd, 0, 1), tailwater: chance(rnd, 0.3) ? R(rnd, 60, 1000) : null, minTc: pick(rnd, [5, 10]), vAssumed: 3 };
  }
  return net;
}

export const GENS = {
  grading(rnd) {
    const scripts = [];
    for (let k = 0; k < 4; k++) {
      const { pts, ox, oy, size, f } = site(rnd, 260);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }];
      for (let i = 0; i < 6; i++) {
        const p0 = { x: ox + size * R(rnd, 0.2, 0.8), y: oy + size * R(rnd, 0.2, 0.8) }, a = R(rnd, -Math.PI, Math.PI);
        calls.push({ op: 'grading.castSlope', args: [ 'S', p0, Math.cos(a), Math.sin(a), f(p0.x, p0.y) + R(rnd, -10, 10), pick(rnd, [2, 3]), pick(rnd, [2, 3]), chance(rnd, 0.5) ? { rate: R(rnd, 0.5, 1), step: R(rnd, 0.2, 2), far: size } : null ] });
      }
      const padPts = randPoly(rnd, ox + size * 0.2, oy + size * 0.2, size * 0.5, I(rnd, 3, 8)).map((p) => (chance(rnd, 0.3) ? { ...p, z: f(p.x, p.y) } : p));
      const pad = { pts: padPts, z: f(ox + size / 2, oy + size / 2) + R(rnd, -5, 5), cut: pick(rnd, [2, 3, 1.5]), fill: pick(rnd, [2, 3]) };
      calls.push({ op: 'grading.daylightPad', args: ['S', pad, chance(rnd, 0.5) ? { spacing: R(rnd, 2, 20) } : null] });
      calls.push({ op: 'grading.selfCrosses', args: [padPts] });
      const { pts: pts2 } = randPoints(rnd, 200, { origin: [ox, oy], size, f: (x, y) => f(x, y) + R(rnd, -3, 3) });
      calls.push({ op: 'tin.buildSurface', args: [{ points: pts2 }], as: 'T' });
      calls.push({ op: 'grading.gridVolume', args: ['S', 'T', chance(rnd, 0.5) ? { cell: R(rnd, 2, 20) } : { region: [randPoly(rnd, ox, oy, size, 5)] }] });
      calls.push({ op: 'grading.gridVolume', args: ['S', 'T', {}] });
      // gradedSurface: pads (and a corridor) graded in one after another, then the volumes between
      const pad2 = { name: chance(rnd, 0.7) ? 'B' : undefined, pts: randPoly(rnd, ox + size * 0.6, oy + size * 0.6, size * 0.2, 4), z: f(ox + size * 0.7, oy + size * 0.7) + R(rnd, -3, 3), cut: 2, fill: 3, options: chance(rnd, 0.4) ? { spacing: R(rnd, 3, 15) } : undefined };
      const items = [{ ...pad, name: 'A' }, pad2];
      if (chance(rnd, 0.5)) {
        const pis = road(rnd, ox, oy, size);
        calls.push({ op: 'alignment.build', args: [{ pis, start: 0 }], as: 'A' }, { op: 'profile.build', args: [pvisFor(rnd, 0, size * 0.5, f(ox + size / 2, oy + size / 2))], as: 'P' });
        items.splice(I(rnd, 0, 2), 0, { name: 'Rd', corridor: { alignment: 'A', profile: 'P', asm: { lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 25 } } });
      }
      if (chance(rnd, 0.2)) items.push({ name: 'Bad', pts: [{ x: ox, y: oy }, { x: ox + 1, y: oy }], z: 0, cut: 2, fill: 2 });
      const bls = chance(rnd, 0.5) ? [{ name: 'b', pts: randLine(rnd, ox, oy, size, f, 3) }] : [];
      calls.push({ op: 'grading.gradedSurface', args: ['S', pts, bls, items], as: 'G' });
      calls.push({ op: 'grading.gridVolume', args: ['S', 'G', {}] });
      scripts.push({ calls });
    }
    return scripts;
  },
  corridor(rnd) {
    const scripts = [];
    for (let k = 0; k < 4; k++) {
      const { pts, ox, oy, size, f } = site(rnd, 260, R(rnd, 600, 1500));
      const pis = road(rnd, ox, oy, size);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 1000 }], as: 'A' }];
      const len = Math.hypot(pis[1].x - pis[0].x, pis[1].y - pis[0].y) + Math.hypot(pis[2].x - pis[1].x, pis[2].y - pis[1].y);
      calls.push({ op: 'profile.build', args: [pvisFor(rnd, 1000 + R(rnd, -20, 20), len * 0.8, f(ox + size / 2, oy + size / 2))], as: 'P' });
      const sup = chance(rnd, 0.5) ? { on: true, delta: pick(rnd, [0.005, 0.0067]), rate: [[7000, null], [3000, 0.02], [1000, 0.04], [0, 0.06]] } : null;
      const asm = { lane: pick(rnd, [12, 11, 3.6]), laneSlope: 0.02, shoulder: pick(rnd, [4, 8]), shoulderSlope: 0.04, cut: pick(rnd, [2, 3]), fill: pick(rnd, [3, 4]), step: pick(rnd, [10, 25, 50]), ...(sup ? { super: sup } : {}) };
      calls.push({ op: 'corridor.patch', args: ['S', 'A', 'P', asm] });
      calls.push({ op: 'corridor.defaultAssembly', args: [pick(rnd, ['ft', 'm'])] });
      calls.push({ op: 'super.plan', args: ['A', { super: sup || { on: true, delta: 0.005, rate: [[0, 0.04]] }, nc: 0.02, lane: 12 }], as: 'SP' });
      for (let i = 0; i < 10; i++) calls.push({ op: 'super.slopesAt', self: 'SP', args: [1000 + R(rnd, -50, len + 50), 0.02] });
      calls.push({ op: 'super.keyStations', self: 'SP' });
      calls.push({ op: 'super.defaults', args: ['m'] }, { op: 'super.clean', args: [{ on: true, delta: 0.1, rate: [[100, 0.05], [-1, 0.02], [500, null], ['x', 1]] }, 'ft'] }, { op: 'super.clean', args: [null, 'ft'] });
      calls.push({ op: 'super.rateFor', args: [R(rnd, 0, 8000), [[7000, null], [4300, 0.02], [0, 0.06]]] });
      scripts.push({ calls });
    }
    return scripts;
  },
  pipes(rnd) {
    const scripts = [];
    for (let k = 0; k < 5; k++) {
      const { pts, ox, oy, size } = site(rnd, 150);
      const net = network(rnd, ox, oy, size, true);
      const units = pick(rnd, ['ft', 'm']);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }];
      calls.push({ op: 'pipes.analyze', args: [net, 'S', units] }, { op: 'pipes.analyze', args: [net, null, units] });
      calls.push({ op: 'pipes.autoInverts', args: [net, 'S', units, chance(rnd, 0.3) ? { only: [net.pipes[0]?.id ?? 'q1'] } : null] });
      calls.push({ op: 'pipes.flowOrder', args: [net] }, { op: 'pipes.defaultNetwork', args: [units] });
      calls.push({ op: 'hydro.flows', args: [net, units], as: 'F' }, { op: 'hydro.size', args: [net, 'F', units] }, { op: 'hydro.grade', args: [net, 'F', units, chance(rnd, 0.5) ? 'S' : null] });
      for (let i = 0; i < 6; i++) {
        const D = R(rnd, 0.5, 6), S = pick(rnd, [0, R(rnd, 0.001, 0.05)]), n = pick(rnd, [0.013, 0.024]);
        calls.push({ op: 'hydro.manningFull', args: [D, S, n, units] }, { op: 'hydro.partFull', args: [R(rnd, -0.1, D * 1.1), D] }, { op: 'hydro.normalDepth', args: [R(rnd, 0, 40), D, S, n, units] });
      }
      calls.push({ op: 'hydro.defaults', args: [units] }, { op: 'hydro.clean', args: [{ idf: { a: -1, b: 2000, c: 0.7 }, n: 2, kj: 11, tailwater: 5, minTc: 0 }, units] }, { op: 'hydro.cleanCatchment', args: [{ id: 'k5', name: 'A\u0001b', pts: [{ x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 1 }, { x: 'z', y: 0 }], C: 2, tc: 0, to: 's1' }] });
      calls.push({ op: 'hydro.area', args: [randPoly(rnd, ox, oy, size, 5), units] }, { op: 'hydro.idf', args: [R(rnd, 5, 60), { idf: { a: 60, b: 10, c: 0.75 } }] });
      scripts.push({ calls });
    }
    return scripts;
  },
  sections(rnd) {
    const scripts = [];
    for (let k = 0; k < 3; k++) {
      const { pts, ox, oy, size, f } = site(rnd, 260, R(rnd, 600, 1200));
      const pis = road(rnd, ox, oy, size);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 0 }], as: 'A' }];
      const { pts: pts2 } = randPoints(rnd, 220, { origin: [ox, oy], size, f: (x, y) => f(x, y) + 3 });
      calls.push({ op: 'tin.buildSurface', args: [{ points: pts2 }], as: 'T' });
      const smp = { step: pick(rnd, [25, 50, 100]), left: R(rnd, 20, 120), right: R(rnd, 20, 120), extra: [R(rnd, 0, 300)] };
      calls.push({ op: 'sec.stations', args: ['A', { step: smp.step, extra: smp.extra }] }, { op: 'sec.line', args: ['A', R(rnd, 0, 300), 50, 60] });
      calls.push({ op: 'sec.build', args: [{ alignment: 'A', ground: 'S', design: chance(rnd, 0.7) ? 'T' : 'S', regions: chance(rnd, 0.5) ? [randPoly(rnd, ox, oy, size, 6)] : null, corridor: chance(rnd, 0.5) ? { CL: [{ x: ox, y: oy, z: 100 }, { x: ox + size, y: oy + size, z: 120 }] } : null, sampling: smp }], as: 'X' });
      calls.push({ op: 'sec.csv', self: 'X', args: [{ units: pick(rnd, ['ft', 'm']) }] });
      calls.push({ op: 'sec.clean', args: [{ step: -1, left: 5, right: 1e5, extra: [5, 5, 'x', 1e9, 2] }, 'm'] }, { op: 'sec.defaults', args: ['ft'] });
      calls.push({ op: 'sec.surfaceSection', args: ['S', { x: ox - 10, y: oy + size / 3 }, { x: ox + size + 10, y: oy + size / 2 }, -50, 50] });
      const run = (o0) => [{ o: o0, z: 10 }, { o: o0 + 10, z: 12 }, { o: o0 + 25, z: 9 }];
      calls.push({ op: 'sec.cutFill', args: [[run(-20)], [run(-18).map((p) => ({ ...p, z: p.z + R(rnd, -3, 3) }))]] });
      calls.push({ op: 'sec.zAt', args: [[run(-20)], R(rnd, -25, 10)] }, { op: 'sec.clipRuns', args: [[run(-20)], [[-15, -5], [0, 3]]] });
      scripts.push({ calls });
    }
    return scripts;
  },
  features(rnd) {
    const scripts = [];
    for (let k = 0; k < 3; k++) {
      const { pts, ox, oy, size, f } = site(rnd, 200);
      const fs = features(rnd, ox, oy, size, f);
      const pis = road(rnd, ox, oy, size);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 0 }], as: 'A' }];
      for (const fe of fs) {
        calls.push({ op: 'fl.stations', args: [fe] }, { op: 'fl.segments', args: [fe] }, { op: 'fl.setElevation', args: [fe, I(rnd, 0, fe.pts.length), R(rnd, 0, 200)] });
        calls.push({ op: 'fl.setGrade', args: [fe, I(rnd, 0, fe.pts.length - 1), R(rnd, -0.1, 0.1)] }, { op: 'fl.raise', args: [fe, R(rnd, -5, 5)] }, { op: 'fl.evenGrade', args: [fe, 0, fe.pts.length - 1] });
        calls.push({ op: 'fl.fromSurface', args: [fe, 'S', chance(rnd, 0.5) ? [0, 1] : null] }, { op: 'fl.offset', args: [fe, pick(rnd, [R(rnd, -20, 20), 0]), pick(rnd, [{ dz: 1 }, { slope: -0.02 }, null])] });
        calls.push({ op: 'fl.item', args: [fe] }, { op: 'fl.crossings', args: ['A', fe] }, { op: 'fl.runs', args: ['A', fe, R(rnd, 5, 100)] });
        calls.push({ op: 'fl.clean', args: [{ ...fe, id: pick(rnd, ['f3', 'x', 7]), name: '  n\u0002 ', cut: 1e4 }, 'ft'] });
      }
      calls.push({ op: 'fl.breaklines', args: [fs, 'eg'] }, { op: 'fl.profile', args: ['A', fs, { within: R(rnd, 0, 50) }] });
      calls.push({ op: 'fl.overlay', args: [fs, { k: R(rnd, 0.1, 3), ox, oy }, chance(rnd, 0.5) ? { skip: 'f1' } : null] });
      for (const g of [0.02, -0.035, 0, '$NaN', 1e-7]) calls.push({ op: 'fl.gradeLabel', args: [g] });
      scripts.push({ calls });
    }
    return scripts;
  },
  codes(rnd) {
    const calls = [];
    const descs = ['EP B', 'EP', 'EP E', 'EP1', 'EP1 BEG', 'TC C', 'TC', 'TC CLOSE', 'TREE oak', 'grd', '', 'EP START x', 'CL END', 'BC c', 'FL', 'XX Y'];
    const pts = [];
    for (let i = 0; i < 60; i++) pts.push({ n: String(pick(rnd, [i + 1, 100 - i, 'A' + i])), x: R(rnd, 0, 100), y: R(rnd, 0, 100), z: chance(rnd, 0.1) ? null : R(rnd, 0, 10), d: pick(rnd, descs) });
    const keys = [{ code: 'EP*', group: 'EP', use: 'breakline', label: 'Edge' }, { code: 'T?', group: '', use: 'points', label: '' }, { code: 'TREE', group: 'VEG', use: 'off', label: 'Tree' }];
    calls.push({ op: 'dk.cleanKeys', args: [[...keys, { code: 'a b' }, { code: '+++' }, null, { code: 'grd', group: 'g r', use: 'x', label: 'l\u0001' }]] });
    calls.push({ op: 'dk.cleanLineCodes', args: [{ on: true, begin: 'B, BEG; s', end: ['E', 'B', 'x y'], close: [] }] }, { op: 'dk.cleanLineCodes', args: [null] });
    for (const d of descs) calls.push({ op: 'dk.parseDesc', args: [d] });
    calls.push({ op: 'dk.groups', args: [keys, pts] }, { op: 'dk.figures', args: [pts, { keys, only: chance(rnd, 0.5) ? ['EP', 'TC'] : null }] }, { op: 'dk.figures', args: [pts, {}] }, { op: 'dk.keyStats', args: [pts, keys] });
    return [{ calls }];
  },
  parcels(rnd) {
    const calls = [];
    const brg = () => `${pick(rnd, ['N', 'S'])} ${I(rnd, 0, 89)}°${I(rnd, 0, 59)}'${I(rnd, 0, 59)}" ${pick(rnd, ['E', 'W'])}`;
    for (const s of ['N 45°30\'15" E', 'N45-30-15E', 'S 0 E', 'AZ 123°45\'00"', 'AZ 123.75', 'N 91 E', 'n 10 30 w', 'AZIMUTH 400', 'x', '', 'S 45.5 W', 'N 45’30″ E']) calls.push({ op: 'pc.parseBearing', args: [s] });
    for (const s of ['45°30\'15"', '45-30-15', '45 30 15', '45.5', '45 60', '45.5 30', '', '1 2 3 4']) calls.push({ op: 'pc.parseAngle', args: [s] });
    const lines = [];
    for (let i = 0; i < I(rnd, 3, 8); i++) lines.push(chance(rnd, 0.25) ? `C R ${I(rnd, 50, 500)} ${pick(rnd, ['L', 'D'])} ${pick(rnd, [R(rnd, 20, 150).toFixed(2), '20°30\'00"'])} ${pick(rnd, ['left', 'right'])}${chance(rnd, 0.3) ? ' CB ' + brg() : ''}` : `${pick(rnd, ['', 'thence ', 'Then, '])}${brg()} ${R(rnd, 10, 400).toFixed(2)} ${pick(rnd, ['', 'feet', 'ft.', 'm'])}`);
    lines.push('garbage', 'C R 0 L 5 left', 'C R 100 L 5', '# note');
    calls.push({ op: 'pc.parseCalls', args: [lines.join('\n')] });
    const start = { x: 500000 + R(rnd, 0, 1000), y: 4000000 + R(rnd, 0, 1000) };
    calls.push({ op: 'pc.runCalls', args: [start, [{ kind: 'line', dir: 0.3, dist: 100 }, { kind: 'curve', R: 200, delta: 0.4, turn: 1, chordDir: null }, { kind: 'line', dir: 2.5, dist: 80 }, { kind: 'curve', R: 150, delta: 0.3, turn: -1, chordDir: 4 }]] });
    const pts = randPoly(rnd, start.x, start.y, 300, I(rnd, 3, 7)).map((p) => (chance(rnd, 0.3) ? { ...p, b: R(rnd, -0.4, 0.4) } : p));
    const parcel = { id: 'r1', name: 'Lot 1', pts, ...(chance(rnd, 0.5) ? { pob: 'the NE corner' } : {}) };
    calls.push({ op: 'pc.courses', args: [pts] }, { op: 'pc.area', args: [pts] }, { op: 'pc.perimeter', args: [pts] }, { op: 'pc.centroid', args: [pts] });
    for (const u of ['ft', 'm']) calls.push({ op: 'pc.legal', args: [parcel, { units: u, pob: parcel.pob ?? null }] }, { op: 'pc.labels', args: [parcel, { units: u, unitPerMm: R(rnd, 0.5, 5) }] }, { op: 'pc.areaWords', args: [R(rnd, 100, 1e6), u] });
    calls.push({ op: 'pc.clean', args: [{ id: 'r9', name: 'P\u0001', pts: [...pts, { x: 'a', y: 1 }, { x: 1, y: 2, b: 2 }], pob: '  here ' }] }, { op: 'pc.clean', args: [{ pts: [{ x: 0, y: 0 }, { x: 1, y: 1 }] }] });
    return [{ calls }];
  },
  traverse(rnd) {
    const calls = [];
    for (const mode of ['angles', 'bearings']) {
      const lines = [];
      const n = I(rnd, 3, 7);
      for (let i = 0; i < n; i++) lines.push(mode === 'angles' ? `${pick(rnd, [`${I(rnd, 60, 300)}°${I(rnd, 0, 59)}'${I(rnd, 0, 59)}"`, `${I(rnd, 60, 300)} ${I(rnd, 0, 59)} ${I(rnd, 0, 59)}`, `${R(rnd, 60, 300).toFixed(4)}`])} ${R(rnd, 50, 400).toFixed(2)} ${pick(rnd, ['', 'T' + i, 'ft'])}` : `${pick(rnd, ['N', 'S'])} ${I(rnd, 0, 89)} ${I(rnd, 0, 59)} ${I(rnd, 0, 59)} ${pick(rnd, ['E', 'W'])} ${R(rnd, 50, 400).toFixed(2)} T${i}`);
      if (mode === 'angles') lines.push(`close ${I(rnd, 60, 300)}-00-10`);
      lines.push('bad line', 'AZ 45.5 100 A1');
      calls.push({ op: 'tv.parse', args: [lines.join('\r\n'), mode] });
    }
    const legs = [];
    for (let i = 0; i < 4; i++) legs.push({ angle: 270 + R(rnd, -0.01, 0.01), dist: 100 + R(rnd, -0.05, 0.05), name: 'S' + i });
    calls.push({ op: 'tv.run', args: [{ start: { x: 500000, y: 4000000 }, mode: 'angles', backsight: 0, legs, close: 'loop', closingAngle: 270 }], as: 'T' });
    calls.push({ op: 'tv.report', self: 'T', args: [{ units: 'ft' }] });
    calls.push({ op: 'tv.run', args: [{ start: { x: 1000, y: 2000 }, mode: 'bearings', legs: legs.map((l, i) => ({ dir: i * 1.3, dist: l.dist, name: '' })), close: { x: 1100, y: 2050 } }], as: 'T2' });
    calls.push({ op: 'tv.report', self: 'T2', args: [{ units: 'm' }] }, { op: 'tv.dir2az', args: [R(rnd, -7, 7)] });
    return [{ calls }];
  },
  staking(rnd) {
    const { pts, ox, oy, size, f } = site(rnd, 200, 900);
    const pis = road(rnd, ox, oy, size);
    const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 500 }], as: 'A' }];
    calls.push({ op: 'profile.build', args: [pvisFor(rnd, 500, 600, f(ox + size / 2, oy + size / 2))], as: 'P' });
    calls.push({ op: 'sk.stations', args: ['A', { from: 500 + R(rnd, -50, 100), to: 500 + R(rnd, 300, 900), step: pick(rnd, [25, 50, 0]), keys: chance(rnd, 0.7) }] });
    calls.push({ op: 'sk.rows', args: ['A', { stations: [500, 550.5, 600, 777], offsets: [-12, 0, 12], design: { profile: 'P' }, ground: { surface: 'S' }, first: 10, prefix: 'K' }] });
    const sp = pts.slice(0, 30);
    calls.push({ op: 'sk.pointRows', args: ['A', sp, { design: { profile: 'P' }, ground: chance(rnd, 0.5) ? { surface: 'S' } : null }] });
    for (const t of ['12 L, CL, 12 R', '-12 0 12', 'L12 CL R12', '12L 12R', 'L 5 R 5', '5 L 5', 'x', 'L-5', 'L5R', '200000']) calls.push({ op: 'sk.parseOffsets', args: [t] });
    for (const v of [1.234, -0.004, 0.0049, -7.5, null, '$NaN']) calls.push({ op: 'sk.cutFillText', args: [v] }, { op: 'sk.offsetText', args: [typeof v === 'number' ? v : 3] });
    const rows = [{ id: '=K1', sta: 512.3, off: -12, x: 1, y: 2, design: 100, ground: 101.5, cf: 1.5 }, { id: 'K2', sta: 600, off: 0, x: 3, y: 4, design: null, ground: 99, cf: null }];
    calls.push({ op: 'sk.csv', args: [rows, { units: 'm', title: 'Stakes, "west"' }] }, { op: 'sk.csv', args: [rows, {}] });
    return [{ calls }];
  },
  masshaul(rnd) {
    const rows = [];
    let sta = 0;
    for (let i = 0; i < I(rnd, 2, 25); i++) { rows.push({ sta, cut: 0, fill: 0, cutVol: i ? R(rnd, 0, 500) * (chance(rnd, 0.3) ? 0 : 1) : 0, fillVol: i ? R(rnd, 0, 500) : 0 }); sta += R(rnd, 10, 100); }
    const calls = [{ op: 'mh.massHaul', args: [rows, { factor: pick(rnd, [1, 0.9, 1.05, 7, -1]) }], as: 'M' }, { op: 'mh.csv', self: 'M', args: [{ units: pick(rnd, ['ft', 'm']) }] }];
    return [{ calls }];
  },
  why(rnd) {
    const calls = [];
    for (let k = 0; k < 4; k++) {
      const pis = road(rnd, 0, 0, 500, I(rnd, 3, 6)).map((p, i, a) => ({ ...p, r: i === 0 || i === a.length - 1 ? 0 : R(rnd, 50, 2000) }));
      calls.push({ op: 'why.curve', args: [pis, { minR: pick(rnd, [0, 300, 1000]) }] }, { op: 'why.maxRadius', args: [pis, I(rnd, 0, pis.length - 1)] });
      const pvis = pvisFor(rnd, 0, 600, 100).map((p, i, a) => ({ ...p, L: i === 0 || i === a.length - 1 ? 0 : R(rnd, 50, 800) }));
      calls.push({ op: 'why.vertical', args: [pvis] });
    }
    return [{ calls }];
  },
  clip(rnd) {
    const { pts } = randPoints(rnd, 25, { round: true });
    const P = pts.map((p) => ({ ...p, d: pick(rnd, [p.d, '=SUM(A1)', '+5', '-x', 'a,"b"']) }));
    const calls = [];
    for (const o of ['PNEZD', 'PENZD']) calls.push({ op: 'clip.pointsText', args: [P, o] }, { op: 'clip.make', args: [P, { units: 'm', order: o, source: ' Site\u0001 ' }] });
    const clip = { format: 'civilcraft-clip', v: 1, units: 'ft', at: 5, source: 's', pts: [...P.map((p) => [p.n, p.x, p.y, p.z, p.d]), ['', 1, 2, 3, ''], ['x', 1e10, 2], 'junk', [P[0].n, 1, 2, 3, 'dup']], csv: 'abc' };
    calls.push({ op: 'clip.read', args: [clip] }, { op: 'clip.read', args: [{ ...clip, v: 2 }] });
    const text = P.slice(0, 5).map((p) => [p.n, p.y, p.x, p.z ?? '', p.d].join(',')).join('\r\n');
    calls.push({ op: 'clip.textPoints', args: [text, 'PNEZD'] }, { op: 'clip.choose', args: [clip, text] }, { op: 'clip.choose', args: [clip, 'abc'] }, { op: 'clip.choose', args: [null, 'hello'] }, { op: 'clip.choose', args: [null, null] });
    const existing = ['1', '2', '3', '10', 'A', '277'];
    calls.push({ op: 'clip.renumber', args: [P.slice(0, 12), existing, { mode: pick(rnd, ['clash', 'always']), start: pick(rnd, [null, 500, 0]) }] });
    calls.push({ op: 'clip.renumberText', args: [[['10', '277'], ['11', '278'], ['12', '279'], ['A', '280'], ['20', '281'], ['22', '283'], ['30', '290']], 3] });
    calls.push({ op: 'clip.moveBy', args: [P.slice(0, 3), 10.5, -3] }, { op: 'clip.centre', args: [P] }, { op: 'clip.centre', args: [[]] });
    return [{ calls }];
  },
  pointedit(rnd) {
    const { pts } = randPoints(rnd, 10, { round: true });
    const calls = [];
    const patches = [{ n: '99' }, { n: pts[1].n }, { n: '' }, { n: 'x'.repeat(41) }, { x: '1,234.5' }, { y: 'abc' }, { z: '' }, { z: null }, { z: 2e6 }, { z: ' 12.5 ' }, { d: 'a\u0001b' }, { d: 'x'.repeat(201) }, {}, { x: pts[0].x }, { x: 1e9 }, { x: ', 5' }];
    for (const p of patches) calls.push({ op: 'pe.check', args: [pts, pts[0].n, p] });
    calls.push({ op: 'pe.check', args: [pts, 'nope', { x: 1 }] });
    return [{ calls }];
  },
  drive(rnd) {
    const { pts, ox, oy, size, f } = site(rnd, 150, 800);
    const pis = road(rnd, ox, oy, size);
    const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 0 }], as: 'A' }, { op: 'profile.build', args: [pvisFor(rnd, 50, 300, f(ox, oy))], as: 'P' }];
    const stas = []; for (let i = 0; i < 12; i++) stas.push(R(rnd, -50, 1000));
    calls.push({ op: 'drive.frames', args: ['A', { profile: chance(rnd, 0.6) ? 'P' : null, ground: 'S' }, stas, { eye: 3.5, ahead: pick(rnd, [100, 30]), origin: { x: ox, y: oy, z: 100 }, exag: pick(rnd, [1, 2]) }] });
    for (let i = 0; i < 10; i++) calls.push({ op: 'drive.sun', args: [Math.floor(R(rnd, 0, 4e12)), R(rnd, -80, 80), R(rnd, -180, 180)] });
    calls.push({ op: 'drive.sunVector', args: [R(rnd, 0, 360), R(rnd, -10, 90)] }, { op: 'drive.speeds', args: ['ft'] }, { op: 'drive.speeds', args: ['m'] }, { op: 'drive.eye', args: ['m'] });
    return [{ calls }];
  },
  project(rnd) {
    const scripts = [];
    for (let k = 0; k < 3; k++) {
      const { pts, ox, oy, size, f } = site(rnd, 180, R(rnd, 500, 1000));
      const units = pick(rnd, ['ft', 'm']);
      const calls = [{ op: 'project.new', as: 'P' }];
      calls.push({ op: 'project.change', self: 'P', args: ['Units', [{ set: ['units'], value: units }, { set: ['name'], value: 'Site ' + k }]] });
      calls.push({ op: 'project.setPoints', self: 'P', args: [pts, 'pts.csv'] }, { op: 'project.groups', self: 'P' });
      calls.push({ op: 'project.change', self: 'P', args: ['Groups', [{ set: ['surface', 'groups', 'EP'], value: 'breakline' }, { set: ['surface', 'groups', 'TREE'], value: 'off' }]] });
      calls.push({ op: 'project.eg', self: 'P' }, { op: 'project.contours', self: 'P' }, { op: 'project.egBreaklines', self: 'P' });
      const pis = road(rnd, ox, oy, size);
      const al = { id: 'a1', name: 'Main', start: 0, pis, pvis: pvisFor(rnd, 0, size * 0.6, f(ox + size / 2, oy + size / 2)), corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 25, ...(chance(rnd, 0.4) ? { super: { on: true, delta: 0.005, rate: [[3000, null], [0, 0.04]] } } : {}) }, sampling: chance(rnd, 0.7) ? { step: 50, left: 80, right: 80, extra: [] } : null };
      calls.push({ op: 'project.change', self: 'P', args: ['Alignment', [{ push: ['alignments'], value: al }]] });
      const pad = { id: 'p2', name: 'Pad', pts: randPoly(rnd, ox + size * 0.5, oy + size * 0.5, size * 0.3, 4), z: f(ox + size * 0.6, oy + size * 0.6), cut: 2, fill: 2 };
      if (chance(rnd, 0.7)) calls.push({ op: 'project.change', self: 'P', args: ['Pad', [{ push: ['pads'], value: pad }]] });
      const fs = features(rnd, ox, oy, size, f);
      calls.push({ op: 'project.change', self: 'P', args: ['Features', [{ set: ['features'], value: fs }]] });
      calls.push({ op: 'project.alignment', self: 'P', args: ['a1'] }, { op: 'project.egProfile', self: 'P', args: ['a1'] }, { op: 'project.designProfile', self: 'P', args: ['a1'] });
      calls.push({ op: 'project.graded', self: 'P' }, { op: 'project.volumes', self: 'P', args: [chance(rnd, 0.5) ? R(rnd, 5, 20) : null] }, { op: 'project.sections', self: 'P', args: ['a1'] });
      const net = network(rnd, ox, oy, size, true);
      calls.push({ op: 'project.change', self: 'P', args: ['Network', [{ set: ['network'], value: net }]] }, { op: 'project.networkCheck', self: 'P' });
      calls.push({ op: 'project.editPoints', self: 'P', args: ['Delete', { remove: [pts[0].n, pts[1].n] }] }, { op: 'project.editPoints', self: 'P', args: ['Add', { add: [{ n: 'N1', x: ox + 5, y: oy + 5, z: 100, d: 'NEW code' }] }] });
      calls.push({ op: 'project.editPoint', self: 'P', args: [pts[3].n, { x: pts[3].x + 1, z: '' }] });
      calls.push({ op: 'project.setCoding', self: 'P', args: [[{ code: 'EP*', group: 'EP', use: 'breakline' }], { begin: ['B'] }] });
      calls.push({ op: 'project.figures', self: 'P', args: [null] }, { op: 'project.view', self: 'P' });
      for (let i = 0; i < 4; i++) calls.push({ op: 'project.undo', self: 'P' });
      calls.push({ op: 'project.view', self: 'P' }, { op: 'project.redo', self: 'P' }, { op: 'project.eg', self: 'P' });
      calls.push({ op: 'project.setDisplay', self: 'P', args: ['interval', pick(rnd, [2, 5, 0.5])] }, { op: 'project.contours', self: 'P' });
      calls.push({ op: 'project.setTin', self: 'P', args: ['TIN', { pts: [{ x: ox, y: oy, z: 1 }, { x: ox + 100, y: oy, z: 2 }, { x: ox, y: oy + 100, z: 3 }, null], faces: [[0, 1, 2], [0, 1, 3]] }, []] }, { op: 'project.eg', self: 'P' }, { op: 'project.undo', self: 'P' });
      calls.push({ op: 'project.nextId', self: 'P', args: ['x'] }, { op: 'project.toJSON', self: 'P', as: 'J' });
      scripts.push({ calls });
      // the project file back in (a JSON written by the JS, then tampered)
      scripts.push({ calls: [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [pts.slice(0, 30), 'a.csv'] }, { op: 'project.change', self: 'P', args: ['A', [{ push: ['alignments'], value: al }]] }] });
    }
    // fromJSON on tampered files
    const bad = [{ format: 'nope' }, { format: 'civilcraft-project', version: 2 }, {
      format: 'civilcraft-project', version: 1, name: 'T\u0001est', units: 'm', seq: 3, points: [['1', 1, 2, 3, 'a'], ['2', 'x', 2], ['3\u0000', 5, 6, null, 'd', { name: 'B', layer: 'L', handle: 'h' }]],
      surface: { name: '', groups: { A: 'points', B: 'bogus' }, breaklines: [{ name: 'b', pts: [{ x: 1, y: 2, z: 3 }, { x: 4, y: 5, z: 6 }] }, { pts: [{ x: 1, y: 2, z: 3 }] }], boundary: [{ x: 0, y: 0 }, { x: 1, y: 0 }], maxEdge: -1, descKeys: [{ code: 'EP*' }], lineCodes: { begin: 'S' }, tin: { pts: [{ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 1 }, { x: 0, y: 1, z: 2 }, 'x'], faces: [[0, 1, 2], [0, 1, 9], [0, 1]] } },
      display: { interval: 2, labels: 'yes', surface: 'zz', bandCount: -1, major: 10 },
      alignments: [{ id: 'a7', name: 'Rd', pis: [{ x: 0, y: 0 }, { x: 100, y: 0, r: -5 }, { x: 200, y: 50 }], pvis: [{ sta: 0, z: 10, L: -1 }], corridor: { on: true, lane: -1, cut: 0, super: { on: true } }, sampling: { step: 25 }, minR: 300 }, { pis: [{ x: 0, y: 0 }] }],
      pads: [{ id: 'bad id', pts: [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }], z: 5, cut: -2 }],
      features: [{ id: 'f9', pts: [{ x: 0, y: 0, z: 1 }, { x: 5, y: 5 }], use: 'design', closed: true }],
      parcels: [{ id: 'r2', name: 'Lot', pts: [{ x: 0, y: 0 }, { x: 10, y: 0, b: 0.3 }, { x: 10, y: 10 }] }],
      network: { name: 'N', type: 'sanitary', rules: { minSlope: 0.01 }, structures: [{ id: 's1', x: 0, y: 0 }, { id: 's2', x: 10, y: 0, rim: 5 }, { x: 'q' }], pipes: [{ id: 'q1', from: 's1', to: 's2', dia: 0 }, { from: 's1', to: 's1' }], catchments: [{ id: 'k1', pts: [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 1, y: 1 }], to: 'zz' }], hydro: { n: 0.02 } },
      crs: { code: 2277, name: 'TX', proj4: '+proj=lcc +lat_1=1', unit: 'ftUS', grid: 'us_noaa_conus.tif', local: { scale: 1.0001, rotation: 0.1, from: [0, 0], to: [1, 1] } },
      gis: { layers: [{ id: 'g1', name: 'L', geom: 'pt', fields: [{ name: 'a', type: 'number' }, { name: 'a' }], color: 'red', source: { file: 'f.shp', format: 'Shapefile', crs: 'EPSG:2277' }, label: { field: 'a' } }, { id: 'zz', geom: 'pt' }], maps: [{ kind: 'xyz', image: 'img1', n: 5, bad: {} }] },
      gisFeatures: { g1: [[{ t: 'pt', c: [1, 2] }, { a: 5, b: 1 }], [{ t: 'pt', c: [1] }, {}], [null, { a: 'x\u0001' }]] },
      crsGrids: { 'us_noaa_conus.tif': 'AAAA' }, gisImages: { img1: 'data:image/png;base64,AAAA', img2: 'x' },
    }];
    for (const b of bad) scripts.push({ calls: [{ op: 'project.fromJSON', args: [b], as: 'Q' }, { op: 'project.toJSON', self: 'Q' }, { op: 'project.eg', self: 'Q' }] });
    return scripts;
  },
};
