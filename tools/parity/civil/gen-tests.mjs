// SPDX-License-Identifier: Apache-2.0
// gen-tests.mjs (from the Rust fork's cvdiff) — the frozen 0.5 test files' DOM-free scenarios as call scripts (forks/work/ref/test-civilcraft*.mjs):
// the same inputs the tests feed the modules (the fixture site tools/civilcraft-test/site-topo.csv, the hand-worked
// cases), so every value those tests assert is compared between the JS and Rust. Seed 1 runs the tests' own
// coordinates; every other seed moves the whole scenario to other survey coordinates (state-plane / UTM magnitudes).
import fs from 'node:fs';

// the 0.5 tests' public fixture (the site repo's tools/civilcraft-test/site-topo.csv)
const CSV = fs.readFileSync([process.env.CADCRAFT_SITE, '/var/www/CADCRAFT/getcadcraft.com'].filter(Boolean)
  .map((d) => d + '/tools/civilcraft-test/site-topo.csv').find((f) => fs.existsSync(f)), 'utf8');
export const E0 = 6150000, N0 = 2080000;

/** The shift of a seed: none for seed 1, else survey-scale offsets (ft or m), with odd fractions. */
export function shiftOf(seed) {
  if (seed === 1) return { dx: 0, dy: 0 };
  const k = seed - 1;
  return { dx: [-5650000, 3115000.37, -6100000.5, 450000.125, 1234567.891][k % 5] + k * 1000.25, dy: [-1650000, 10070000.73, 2000000.5, 4100000.0625, -987654.321][k % 5] - k * 777.5 };
}
/** The fixture points (moved), as the CSV's text and as point objects. */
export function fixture(seed) {
  const { dx, dy } = shiftOf(seed);
  const rows = CSV.trim().split(/\r?\n/).slice(1).map((l) => l.split(','));
  const points = rows.map((r) => ({ n: r[0], x: Number(r[2]) + dx, y: Number(r[1]) + dy, z: Number(r[3]), d: r.slice(4).join(',') }));
  const text = 'Point,Northing,Easting,Elevation,Description\n' + points.map((p) => [p.n, (p.y).toFixed(3), (p.x).toFixed(3), p.z.toFixed(2), p.d].join(',')).join('\n') + '\n';
  return { points, text, E: E0 + dx, N: N0 + dy, dx, dy };
}
const mv = (s) => { const { dx, dy } = shiftOf(s); return (p) => ({ ...p, x: p.x + dx, y: p.y + dy }); };
const rngT = (seed) => () => { seed |= 0; seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
const grid = (i0, i1, j0, j1, step, f, m) => { const p = []; for (let i = i0; i <= i1; i++) for (let j = j0; j <= j1; j++) p.push(m({ x: i * step, y: j * step, z: f(i * step, j * step) })); return p; };

export const GENS = {
  // test-civilcraft.mjs
  't-main'(rnd, seed) {
    const fx = fixture(seed), M = mv(seed), { E, N } = fx;
    const scripts = [];
    // units and stations
    {
      const calls = [];
      for (let s = -500; s <= 5000; s += 7.31 * 37) calls.push({ op: 'units.formatStation', args: [s, 'ft'] }, { op: 'units.parseStation', args: [String(s.toFixed(2)), 'ft'] });
      for (const t of ['12+34.56', '1+00', '-0+50', '1234.5', '12+3', 'x', '1+234+5', '0+00.004']) calls.push({ op: 'units.parseStation', args: [t, 'ft'] }, { op: 'units.parseStation', args: [t, 'm'] });
      scripts.push({ calls });
    }
    // point files
    {
      const calls = [{ op: 'points.parsePoints', args: [fx.text] }, { op: 'points.pointGroups', args: [fx.points] }];
      calls.push({ op: 'points.parsePoints', args: ['1 5000.000 1000.000 100.5 EP\n2 5010 1001 100.7 EP LT\n', { format: 'PENZD' }] }, { op: 'points.parsePoints', args: ['1000.0\t2000.0\t50.0\n1001\t2002\t51\n1003\t2001\t52\n'] });
      calls.push({ op: 'points.parsePoints', args: ['Pt,N,E,Z,Desc\n10,1,2,3,"TREE, 12"" OAK"\n'] }, { op: 'points.parsePoints', args: ['Point,Easting,Northing,Elevation,Description\n1,10,20,30,A\n'] });
      calls.push({ op: 'points.parsePoints', args: ['# a comment\nP,N,E,Z,D\n1,100,200,10,A\n2,abc,200,10,B\n3,100\n1,101,201,11,C\n4,102,202,x,D\n5,103,203,,E\n'] });
      calls.push({ op: 'points.writePoints', args: [fx.points] });
      for (const k of ['FL', 'TBN', 'TBS', 'RIDGE', 'GRD', 'NOPE']) calls.push({ op: 'points.groupPoints', args: [fx.points, k] });
      scripts.push({ calls });
    }
    // TIN on random input (1500 points, zig / diag / thru breaklines)
    {
      const r = rngT(7), pts = [];
      for (let i = 0; i < 1500; i++) pts.push(M({ x: 2e6 + r() * 1000, y: 6e5 + r() * 800, z: 100 + r() * 20 }));
      const bl = [
        { name: 'zig', pts: [0, 1, 2, 3, 4, 5, 6].map((k) => M({ x: 2e6 + 50 + k * 140, y: 6e5 + (k % 2 ? 600 : 200), z: 110 })) },
        { name: 'diag', pts: [M({ x: 2e6 + 20, y: 6e5 + 780, z: 105 }), M({ x: 2e6 + 980, y: 6e5 + 30, z: 115 })] },
        { name: 'thru', pts: [pts[10], pts[20], pts[30]].map((p) => ({ ...p })) },
      ];
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts, breaklines: bl }], as: 'S' }, { op: 'surface.stats', self: 'S' }, { op: 'tin.polyArea', args: [[...pts, ...bl.flatMap((b) => b.pts)]] }];
      calls.push({ op: 'tin.convexHull', args: [[...pts, ...bl.flatMap((b) => b.pts)]] });
      for (let k = 0; k < 6; k++) { const a = bl[0].pts[k], b = bl[0].pts[k + 1]; for (const t of [0.13, 0.5, 0.77]) calls.push({ op: 'surface.elevationAt', self: 'S', args: [a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t] }); }
      calls.push({ op: 'tin.buildSurface', args: [{ points: [{ x: 0, y: 0, z: 1 }, { x: 10, y: 0, z: 1 }, { x: 0, y: 10, z: 1 }, { x: 0, y: 0, z: 1 }, { x: 0, y: 0.00001, z: 5 }].map(M) }] });
      scripts.push({ calls });
    }
    // planes, contours, bands, slopes
    {
      const plane = (x, y) => 100 + 0.02 * x - 0.03 * y;
      const r = rngT(3), pts = [];
      for (let i = 0; i < 400; i++) { const x = r() * 500, y = r() * 300; pts.push({ x, y, z: plane(x, y) }); }
      for (const [x, y] of [[0, 0], [500, 0], [500, 300], [0, 300]]) pts.push({ x, y, z: plane(x, y) });
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts.map(M) }], as: 'S' }];
      for (let i = 0; i < 40; i++) calls.push({ op: 'surface.elevationAt', self: 'S', args: [M({ x: r() * 500 }).x, M({ y: r() * 300 }).y] });
      calls.push({ op: 'surface.stats', self: 'S' }, { op: 'surface.contours', self: 'S', args: [1] }, { op: 'surface.slopes', self: 'S' }, { op: 'surface.bands', self: 'S', args: [[95, 97, 99, 101, 103, 105]] });
      calls.push({ op: 'tin.buildSurface', args: [{ points: [{ x: 0, y: 0, z: 0 }, { x: 100, y: 0, z: 0 }, { x: 100, y: 100, z: 0 }, { x: 0, y: 100, z: 0 }, { x: 50, y: 50, z: 10 }].map(M) }], as: 'C' }, { op: 'surface.contours', self: 'C', args: [2.5] });
      scripts.push({ calls });
    }
    // fixture breaklines, boundary, edge trimming, faces
    {
      const bls = ['FL', 'TBN', 'TBS', 'RIDGE'].map((k) => ({ name: k, pts: fx.points.filter((p) => p.d.split(/\s+/)[0].toUpperCase() === k).sort((a, b) => Number(a.n) - Number(b.n)) }));
      const calls = bls.map((b) => ({ op: 'points.groupPoints', args: [fx.points, b.name] }));
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points }], as: 'PL' }, { op: 'tin.buildSurface', args: [{ points: fx.points, breaklines: bls }], as: 'EG' });
      const r = rngT(11);
      for (let i = 0; i < 60; i++) { const u = 5 + r() * 390, v = 5 + r() * 290; calls.push({ op: 'surface.elevationAt', self: 'EG', args: [E + u, N + v] }, { op: 'surface.elevationAt', self: 'PL', args: [E + u, N + v] }); }
      const B = [{ x: E + 20, y: N + 20 }, { x: E + 380, y: N + 20 }, { x: E + 380, y: N + 280 }, { x: E + 20, y: N + 280 }];
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points, breaklines: bls, boundary: B }], as: 'B' }, { op: 'surface.elevationAt', self: 'B', args: [E + 20, N + 20, 1e-6] });
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points, boundary: [{ x: E - 100, y: N + 150 }, { x: E + 200, y: N - 100 }, { x: E + 600, y: N + 150 }, { x: E + 200, y: N + 450 }] }] });
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points, breaklines: bls, maxEdge: 30 }] });
      calls.push({ op: 'surface.toFaces', self: 'EG', as: 'F' }, { op: 'surface.outline', self: 'EG' }, { op: 'surface.edges', self: 'EG' }, { op: 'surface.stats', self: 'EG' });
      scripts.push({ calls });
    }
    // alignments
    {
      const calls = [{ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 500, y: 0, r: 200 }, { x: 500, y: 600 }].map(M), start: 1000 }], as: 'A' }];
      const P = M({ x: 0, y: 0 });
      calls.push({ op: 'al.pointAt', self: 'A', args: [1000] }, { op: 'al.stationOffset', self: 'A', args: [P.x + 150, P.y - 10] }, { op: 'al.stationOffset', self: 'A', args: [P.x + 150, P.y + 25] }, { op: 'al.stationOffset', self: 'A', args: [P.x + 300 + 150 * Math.SQRT1_2, P.y + 200 - 150 * Math.SQRT1_2] });
      for (let s = 1000; s <= 2000; s += 13.7 * 5) calls.push({ op: 'al.pointAt', self: 'A', args: [s] });
      calls.push({ op: 'al.ticks', self: 'A', args: [100] }, { op: 'al.polyline', self: 'A' }, { op: 'al.keyStations', self: 'A' });
      calls.push({ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 100, y: 0, r: 50 }, { x: 100, y: -100 }].map(M) }] }, { op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 100, y: 0, r: 500 }, { x: 100, y: 100, r: 500 }, { x: 0, y: 100 }].map(M) }] }, { op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 200, y: 50 }].map(M) }] });
      scripts.push({ calls });
    }
    // profiles
    {
      const calls = [{ op: 'profile.build', args: [[{ sta: 0, z: 100 }, { sta: 500, z: 110, L: 200 }, { sta: 1000, z: 100 }]], as: 'P' }];
      for (const s of [0, 400, 450, 500, 550, 600, 1000, 1001]) calls.push({ op: 'prof.elevationAt', self: 'P', args: [s] }, { op: 'prof.gradeAt', self: 'P', args: [s] });
      calls.push({ op: 'profile.build', args: [[{ sta: 0, z: 100 }, { sta: 300, z: 94 }, { sta: 600, z: 97, K: 40 }, { sta: 900, z: 97 }]] }, { op: 'profile.build', args: [[{ sta: 0, z: 100 }, { sta: 400, z: 92, L: 300 }, { sta: 1000, z: 98 }]], as: 'L' }, { op: 'prof.sample', self: 'L' });
      calls.push({ op: 'profile.build', args: [[{ sta: 0, z: 100 }, { sta: 100, z: 102, L: 300 }, { sta: 200, z: 100 }]] });
      const plane = (x, y) => 50 + 0.01 * x + 0.02 * y;
      calls.push({ op: 'tin.buildSurface', args: [{ points: grid(0, 10, 0, 10, 100, plane, M) }], as: 'S' }, { op: 'alignment.build', args: [{ pis: [{ x: 50, y: 50 }, { x: 900, y: 120, r: 150 }, { x: 700, y: 900 }].map(M) }], as: 'A' }, { op: 'profile.sampleSurface', args: ['S', 'A'] });
      scripts.push({ calls });
    }
    // grading and volumes (hand-worked solids, the fixture pad)
    {
      const calls = [{ op: 'tin.buildSurface', args: [{ points: grid(0, 10, 0, 10, 10, (x) => x / 10, M) }], as: 'B' }, { op: 'tin.buildSurface', args: [{ points: grid(0, 10, 0, 10, 10, () => 5, M) }], as: 'T' }];
      calls.push({ op: 'grading.gridVolume', args: ['B', 'T', { cell: 1 }] }, { op: 'grading.gridVolume', args: ['B', 'T', { cell: 5 }] });
      const pts = grid(-10, 10, -10, 10, 10, () => 0, M);
      const pad = { pts: [{ x: -10, y: -10 }, { x: 10, y: -10 }, { x: 10, y: 10 }, { x: -10, y: 10 }].map(M), z: 2, cut: 2, fill: 2 };
      calls.push({ op: 'tin.buildSurface', args: [{ points: pts }], as: 'F' }, { op: 'grading.daylightPad', args: ['F', pad] }, { op: 'grading.gradedSurface', args: ['F', pts, [], [pad]], as: 'G' });
      const c = M({ x: 0, y: 0 });
      for (const [x, y] of [[3, -4], [12, 0], [30, 30]]) calls.push({ op: 'surface.elevationAt', self: 'G', args: [c.x + x, c.y + y] });
      calls.push({ op: 'tin.buildSurface', args: [{ points: pts.map((p) => ({ ...p, z: 10 })) }], as: 'F2' }, { op: 'grading.daylightPad', args: ['F2', { ...pad, z: 8, cut: 3, fill: 2 }] });
      calls.push({ op: 'tin.buildSurface', args: [{ points: pts.map((p) => ({ ...p, z: (p.x - c.x) / 10 })) }], as: 'F3' }, { op: 'grading.daylightPad', args: ['F3', { ...pad, z: 0, cut: 2, fill: 2 }] });
      calls.push({ op: 'grading.daylightPad', args: ['F', { ...pad, pts: [{ x: -30, y: -30 }, { x: 30, y: -30 }, { x: 30, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 30 }, { x: -30, y: 30 }].map(M) }] });
      const bls = ['FL', 'TBN', 'TBS', 'RIDGE'].map((k) => ({ name: k, pts: fx.points.filter((p) => p.d.split(/\s+/)[0].toUpperCase() === k).sort((a, b) => Number(a.n) - Number(b.n)) }));
      const fpad = { pts: [{ x: E + 150, y: N + 120 }, { x: E + 230, y: N + 120 }, { x: E + 230, y: N + 180 }, { x: E + 150, y: N + 180 }], z: 254, cut: 2, fill: 3 };
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points, breaklines: bls }], as: 'EG' }, { op: 'grading.gradedSurface', args: ['EG', fx.points, bls, [fpad]], as: 'FG' }, { op: 'grading.gridVolume', args: ['EG', 'FG', {}] });
      scripts.push({ calls });
    }
    // corridors
    {
      const pts = grid(-5, 25, -8, 8, 10, () => 100, M);
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'F' }, { op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 200, y: 0 }].map(M) }], as: 'A' }, { op: 'profile.build', args: [[{ sta: 0, z: 103 }, { sta: 200, z: 103 }]], as: 'P' }, { op: 'corridor.defaultAssembly', args: ['ft'] }];
      const asm = { lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 25 };
      calls.push({ op: 'corridor.patch', args: ['F', 'A', 'P', asm] }, { op: 'grading.gradedSurface', args: ['F', pts, [], [{ name: 'c', corridor: { alignment: 'A', profile: 'P', asm } }]], as: 'G' }, { op: 'grading.gridVolume', args: ['F', 'G', { cell: 0.1 }] });
      const c = M({ x: 0, y: 0 });
      for (const [x, y] of [[100, 0], [100, -6], [100, 20], [100, 40]]) calls.push({ op: 'surface.elevationAt', self: 'G', args: [c.x + x, c.y + y] });
      calls.push({ op: 'profile.build', args: [[{ sta: 0, z: 97 }, { sta: 200, z: 97 }]], as: 'CP' }, { op: 'corridor.patch', args: ['F', 'A', 'CP', asm] });
      calls.push({ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 150, y: 0, r: 15 }, { x: 150, y: 50 }].map(M) }], as: 'CU' }, { op: 'profile.build', args: [[{ sta: 0, z: 103 }, { sta: 400, z: 103 }]], as: 'P2' }, { op: 'corridor.patch', args: ['F', 'CU', 'P2', asm] });
      scripts.push({ calls });
    }
    // pipe networks
    {
      const plane = (f) => grid(-2, 32, -5, 5, 10, f, M);
      const S = [{ id: 's1', name: 'S1', x: 0, y: 0, rim: null, sumpDepth: 0.5, dia: 48 }, { id: 's2', name: 'S2', x: 100, y: 0, rim: null, sumpDepth: 0, dia: 48 }, { id: 's3', name: 'S3', x: 200, y: 0, rim: null, sumpDepth: 0, dia: 48 }].map(M);
      const pipes = [{ id: 'q1', name: 'P1', from: 's1', to: 's2', dia: 12, invUp: null, invDn: null }, { id: 'q2', name: 'P2', from: 's2', to: 's3', dia: 12, invUp: null, invDn: null }];
      const net = { name: 'Storm', type: 'storm', rules: { minSlope: 0.005, minCover: 2, minDrop: 0.1 }, structures: S, pipes };
      const calls = [{ op: 'pipes.defaultNetwork', args: ['ft'] }, { op: 'pipes.defaultNetwork', args: ['m'] }, { op: 'tin.buildSurface', args: [{ points: plane(() => 100) }], as: 'FL' }, { op: 'tin.buildSurface', args: [{ points: plane((x) => 100 - 0.02 * (x - M({ x: 0 }).x)) }], as: 'SL' }, { op: 'tin.buildSurface', args: [{ points: plane((x) => { const u = x - M({ x: 0 }).x; return u > 140 && u < 160 ? 99 : 100; }) }], as: 'DP' }];
      for (const s of ['FL', 'SL', 'DP']) calls.push({ op: 'pipes.autoInverts', args: [net, s, 'ft'] }, { op: 'pipes.analyze', args: [net, s, 'ft'] });
      calls.push({ op: 'pipes.analyze', args: [{ ...net, pipes: [{ ...pipes[0], invUp: 96, invDn: 96.2 }, { ...pipes[1], invUp: 96.3, invDn: 96.29 }] }, 'FL', 'ft'] });
      calls.push({ op: 'pipes.analyze', args: [{ ...net, pipes: [{ ...pipes[0], invUp: 98.5, invDn: 98 }] }, 'FL', 'ft'] });
      calls.push({ op: 'pipes.analyze', args: [{ ...net, pipes: [{ ...pipes[0], invUp: 97, invDn: 96.5 }, { ...pipes[1], invUp: 96.4, invDn: 96 }, { id: 'q3', name: 'P3', from: 's3', to: 's1', dia: 12, invUp: 95, invDn: 94 }] }, 'FL', 'ft'] });
      calls.push({ op: 'pipes.analyze', args: [{ ...net, structures: [...S, M({ id: 's4', name: 'S4', x: 100, y: 50, rim: 101, sumpDepth: 0, dia: 48 })], pipes: [{ ...pipes[0], invUp: 97, invDn: 96.5 }, { ...pipes[1], invUp: 96.4, invDn: 96 }, { id: 'q4', name: 'P4', from: 's2', to: 's4', dia: 12, invUp: 96.3, invDn: 96 }] }, 'FL', 'ft'] });
      calls.push({ op: 'lx.write', args: [{ units: 'ft', networks: [{ ...net, pipes: [{ ...pipes[0], invUp: 97, invDn: 96.5 }, { ...pipes[1], invUp: 96.4, invDn: 96 }], structures: S.map((x) => ({ ...x, rimShown: 100, sump: 95 })) }], date: 0 }], as: 'X' }, { op: 'lx.read', args: [{ ref: 'X' }] });
      scripts.push({ calls });
    }
    // the fixture project: LandXML out and back in, DXF, PDF, DWG
    {
      const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site-topo.csv'] }];
      calls.push({ op: 'project.change', self: 'P', args: ['Breaklines from groups', [...['FL', 'TBN', 'TBS', 'RIDGE'].map((k) => ({ set: ['surface', 'groups', k], value: 'breakline' })), { set: ['surface', 'groups', 'CP'], value: 'off' }]] });
      const pis = [{ x: E + 10, y: N + 260, r: 0 }, { x: E + 200, y: N + 230, r: 120 }, { x: E + 300, y: N + 40, r: 90 }, { x: E + 390, y: N + 30, r: 0 }];
      calls.push({ op: 'project.change', self: 'P', args: ['Alignment', [{ push: ['alignments'], value: { id: 'a1', name: 'Access road', start: 1000, pis, pvis: [] } }]] });
      calls.push({ op: 'project.alignment', self: 'P', args: ['a1'] }, { op: 'project.egProfile', self: 'P', args: ['a1'] }, { op: 'project.eg', self: 'P' }, { op: 'project.contours', self: 'P' }, { op: 'project.egBreaklines', self: 'P' });
      calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points.filter((p) => !/^CP/.test(p.d)) }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 1000 }], as: 'A' });
      calls.push({ op: 'lx.write', args: [{ name: 'Fixture site', units: 'ft', points: fx.points, surfaces: [{ name: 'EG', desc: 'Existing ground', surface: 'S', breaklines: [], boundary: null }], alignments: [{ name: 'Access road', alignment: 'A' }], date: 0 }], as: 'X' }, { op: 'lx.read', args: [{ ref: 'X' }] });
      calls.push({ op: 'lx.write', args: [{ units: 'm', points: [{ n: 'A', x: 1, y: 2, z: 3, d: 'X & <Y>' }], date: 0 }], as: 'XM' }, { op: 'lx.read', args: [{ ref: 'XM' }] }, { op: 'lx.scaleImport', args: [{ ref: 'XM' }, 1 / 0.3048] });
      calls.push({ op: 'lx.read', args: ['<?xml version="1.0"?><LandXML xmlns="http://www.landxml.org/schema/LandXML-1.2" version="1.2"><Units><Metric linearUnit="meter" areaUnit="squareMeter" volumeUnit="cubicMeter" temperatureUnit="celsius" pressureUnit="milliBars"/></Units><Alignments><Alignment name="S" length="1" staStart="0"><CoordGeom><Line><Start>0 0</Start><End>0 10</End></Line><Spiral length="5" radiusEnd="50" radiusStart="INF" rot="cw" spiType="clothoid"><Start>0 10</Start><PI>0 12</PI><End>1 15</End></Spiral></CoordGeom></Alignment></Alignments><PipeNetworks/><!-- c --><CgPoints><CgPoint name="1">1 2 3</CgPoint><CgPoint name="2" pntRef="1"/></CgPoints></LandXML>'] }, { op: 'lx.read', args: ['<foo/>'] });
      const spec = { units: 'ft', points: fx.points, surface: 'S', contours: { interval: 1, major: 5 }, breaklines: [], alignments: [{ name: 'Access road', alignment: 'A' }], pads: [] };
      calls.push({ op: 'scene.plan', args: [spec, { mmPerUnit: 25.4 / 40 }], as: 'SC' }, { op: 'dxf.fromScene', args: ['SC', 25.4 / 40, 'ft'] });
      const ext = { minX: E - 15, minY: N - 12, maxX: E + 412, maxY: N + 309 };
      calls.push({ op: 'plot.fitScale', args: ['ft', ext, 'tabloid'] }, { op: 'plot.planSheet', args: ['SC', { extents: ext, paper: 'tabloid', scale: { label: '1" = 40\'', mmPerUnit: 25.4 / 40 }, title: 'Fixture site — existing ground', units: 'ft', notes: ['Contours 1 ft'] }], as: 'SH' }, { op: 'plot.toPdf', args: ['SH', {}] });
      for (const v of ['AC1015', 'AC1018', 'AC1021', 'AC1024', 'AC1027', 'AC1032']) calls.push({ op: 'dwg.fromScene', args: [{ scene: 'SC', mmPerUnit: 25.4 / 40, units: 'ft', version: v, created: Date.UTC(2026, 9, 10, 12) }] });
      const every = { units: 'm', layers: [{ name: 'Lay/er:1', color: 3 }, { name: 'Lay_er_1', color: 4 }, { name: 'Pads', color: 'blue' }], created: Date.UTC(2026, 9, 10, 12), entities: [
        { type: 'line', layer: 'Lay/er:1', a: { x: -12.5, y: 3.25, z: 1.5 }, b: { x: 100.125, y: -7, z: -2 } }, { type: 'line', layer: 'Lay_er_1', a: { x: 0, y: 0 }, b: { x: 0, y: 10 } },
        { type: 'poly', layer: 'Pads', pts: [{ x: 1, y: 1 }, { x: 5, y: 1 }, { x: 5, y: 4 }], closed: true }, { type: 'poly', layer: 'C-TOPO-MAJR', pts: [{ x: 0.1, y: 0.2 }, { x: 0.1, y: 0.3 }, { x: 0.4, y: 0.3 }], elev: -3.75 },
        { type: 'poly', layer: 'Pads', pts: [{ x: 1, y: 1 }] }, { type: 'poly3d', layer: 'C-TOPO-BRKL', pts: [{ x: 1, y: 2, z: 3 }, { x: 4, y: 5, z: 6 }, { x: 7, y: 9, z: 8 }], closed: true },
        { type: 'arc', layer: 'C-ROAD-CNTR', c: { x: 50, y: 60 }, r: 12.5, a0: 5.5, a1: 0.5 }, { type: 'circle', layer: 'C-STRM-STRC', c: { x: -5, y: -6 }, r: 0.6 }, { type: 'point', layer: 'V-NODE', x: 3, y: 4, z: -1.25 },
        { type: 'text', layer: 'V-NODE-TEXT', x: 10, y: 20, h: 1.5, rot: 0, text: 'Left 12° ±0.5 Ω € — ½', align: 'left' }, { type: 'text', layer: 'C-ROAD-STAN', x: 11, y: 21, h: 2, rot: 1.25, text: '11+00', align: 'center' }, { type: 'text', layer: 'C-ROAD-STAN', x: 12, y: 22, h: 2.5, rot: -0.5, text: 'PC 12+34.56', align: 'right' }] };
      for (const v of ['AC1015', 'AC1018', 'AC1032']) calls.push({ op: 'dwg.write', args: [{ ...every, version: v }] });
      calls.push({ op: 'dxf.write', args: [every] }, { op: 'xml.parse', args: ['<a><b></a>'] }, { op: 'xml.parse', args: ['<!DOCTYPE x [<!ENTITY e SYSTEM "file:///etc/passwd">]><x a="&lt;&#65;&e;">t&amp;</x>'] });
      scripts.push({ calls });
    }
    // project: undo and the project file
    {
      const padPts = [{ x: E + 150, y: N + 120 }, { x: E + 230, y: N + 120 }, { x: E + 230, y: N + 180 }, { x: E + 150, y: N + 180 }];
      const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site-topo.csv'] }, { op: 'project.eg', self: 'P' }];
      calls.push({ op: 'project.change', self: 'P', args: ['FL breakline', [{ set: ['surface', 'groups', 'FL'], value: 'breakline' }]] }, { op: 'project.eg', self: 'P' });
      calls.push({ op: 'project.change', self: 'P', args: ['Pad', [{ push: ['pads'], value: { id: 'p1', name: 'Pad A', pts: padPts, z: 254, cut: 2, fill: 3 } }]] }, { op: 'project.graded', self: 'P' }, { op: 'project.volumes', self: 'P' });
      calls.push({ op: 'project.undo', self: 'P' }, { op: 'project.view', self: 'P' }, { op: 'project.redo', self: 'P' }, { op: 'project.toJSON', self: 'P' });
      calls.push({ op: 'project.reload', self: 'P', args: [[]], as: 'Q' }, { op: 'project.eg', self: 'Q' }, { op: 'project.graded', self: 'Q' });
      calls.push({ op: 'project.reload', self: 'P', args: [[{ set: ['network'], value: { name: 'Storm', type: 'storm', rules: { minSlope: 0.005, minCover: 2, minDrop: 0.1 }, structures: [{ id: 's1', name: 'S1', x: E + 50, y: N + 50, rim: null, sumpDepth: 0, dia: 48 }, { id: 's2', name: 'S2', x: E + 150, y: N + 50, rim: 250, sumpDepth: 0, dia: 48 }], pipes: [{ id: 'q1', name: 'P1', from: 's1', to: 's2', dia: 12, invUp: 245, invDn: 244 }] } }]], as: 'R' }, { op: 'project.networkCheck', self: 'R' });
      calls.push({ op: 'project.reload', self: 'P', args: [[{ push: ['pads'], value: { id: 'x', pts: [{ x: 'a' }], z: '$NaN' } }, { push: ['points'], value: ['bad', 'x', null] }, { set: ['surface', 'groups', 'EVIL'], value: '<script>' }, { set: ['name'], value: 'A\u0000B' }]], as: 'T' }, { op: 'project.toJSON', self: 'T' });
      calls.push({ op: 'project.fromJSON', args: [{ format: 'nope' }] });
      scripts.push({ calls });
    }
    // copy and paste of survey points
    {
      const P = (n, x, y, z, d = 'GRD') => ({ n: String(n), x, y, z, d });
      const have = [P(1, 0, 0, 1), P(2, 1, 0, 1), P(10, 2, 0, 1), P('CP1', 3, 0, 1)].map(M);
      const calls = [{ op: 'clip.renumber', args: [[P(1, 5, 5, 1), P(2, 6, 5, 1), P(3, 7, 5, 1)].map(M), have] }, { op: 'clip.renumber', args: [[P(3, 0, 0, 1), P(4, 0, 0, 1)].map(M), have, { mode: 'always' }] }];
      calls.push({ op: 'clip.renumber', args: [[P(1, 0, 0, 1), P(2, 0, 0, 1), P(3, 0, 0, 1)].map(M), [...have, M(P(1001, 0, 0, 1))], { mode: 'always', start: 1000 }] }, { op: 'clip.renumber', args: [[P('CP1', 0, 0, 1), P('CP1', 1, 1, 1)].map(M), have] }, { op: 'clip.renumber', args: [[P(20, 0, 0, 1), P(20, 1, 1, 1), P(21, 2, 2, 1)].map(M), have] });
      for (let k = 0; k < 3; k++) calls.push({ op: 'clip.renumber', args: [fx.points, fx.points, { mode: k ? 'always' : 'clash', start: k === 2 ? 5 : null }] });
      const tricky = [P(5, 6150000.1234, 2080000.5, 250.25, 'TREE, 12" OAK'), P(6, 6150010, 2080001, null, '=HYPERLINK("x")'), P(7, -1.5, -2.25, -3, '-5 OFFSET')].map(M);
      calls.push({ op: 'clip.pointsText', args: [tricky, 'PNEZD'] }, { op: 'clip.pointsText', args: [tricky, 'PENZD'] });
      calls.push({ op: 'clip.textPoints', args: ['Point,Easting,Northing,Elevation,Description\n1,10,20,30,A\n', 'PNEZD'] }, { op: 'clip.textPoints', args: ['101\t5000.5\t1000.25\t99.5\tEP\n102\t5010\t1001\t99.7\tEP\n', 'PNEZD'] });
      calls.push({ op: 'points.parsePoints', args: ['1,5000,1000,10,A\n2,5001,1001,11,B\n', { prefer: 'PENZD' }] }, { op: 'clip.make', args: [tricky, { units: 'm', order: 'PNEZD', source: 'tab1' }] });
      const clip = { format: 'civilcraft-clip', v: 1, units: 'm', order: 'PNEZD', source: 'tab1', at: 0, pts: tricky.map((p) => [p.n, p.x, p.y, p.z, p.d]) };
      const evil = { format: 'civilcraft-clip', v: 1, units: '<script>', at: 'x', source: 'a\u0000b', pts: [['1\u0007', 'NaN', 2, 3, 'x'], ['2', 1, '$Inf', 3, 'x'], ['3', 1, 2, 1e12, 'y'], ['4', 1, 2, 'z', 'D\u0000<b>' + 'x'.repeat(500)], ['4', 5, 6, 7, 'dup']] };
      calls.push({ op: 'clip.read', args: [clip] }, { op: 'clip.read', args: [evil] }, { op: 'clip.choose', args: [clip, '101\t5000.5\t1000.25\t99.5\tEP\n'] }, { op: 'clip.choose', args: [null, 'hello'] });
      calls.push({ op: 'clip.moveBy', args: [tricky, 500, 0] }, { op: 'clip.centre', args: [tricky] }, { op: 'clip.renumberText', args: [{ renamed: [['1', '1001'], ['2', '1002']] }, 3] });
      calls.push({ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site-topo.csv'] }, { op: 'project.change', self: 'P', args: ['FL breakline', [{ set: ['surface', 'groups', 'FL'], value: 'breakline' }]] }, { op: 'project.eg', self: 'P' });
      calls.push({ op: 'project.editPoints', self: 'P', args: ['Cut 3 points', { remove: ['1', '2', '3'] }] }, { op: 'project.eg', self: 'P' }, { op: 'clip.renumber', args: [fx.points.filter((q) => ['1', '2', '3'].includes(q.n)), fx.points.filter((q) => !['1', '2', '3'].includes(q.n))] });
      calls.push({ op: 'project.editPoints', self: 'P', args: ['New code', { add: [P(900, E + 10, N + 10, 251, 'MH RIM')] }] }, { op: 'project.groups', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.eg', self: 'P' });
      for (const t of ['n', 'nz', 'nzd', 'x', 'd']) calls.push({ op: 'scene.plan', args: [{ units: 'ft', points: [M(P(5, 0, 0, 250.5, 'TREE OAK'))] }, { mmPerUnit: 1, show: { pointLabels: true, pointLabelText: t } }] });
      scripts.push({ calls });
    }
    return scripts;
  },
};

const Pt = (M) => (n, x, y, z, d = 'GRD') => M({ n: String(n), x, y, z, d });
Object.assign(GENS, {
  // test-civilcraft-codes.mjs
  't-codes'(rnd, seed) {
    const M = mv(seed), P = Pt(M), fx = fixture(seed);
    const calls = [{ op: 'dk.cleanLineCodes', args: [null] }];
    for (const d of ['ep1 b back of walk', 'BLDG CLOSE', 'FL end', 'FL START', 'CP 1 IRON ROD', 'TREE 12 OAK', '', '   ', 'EP B']) calls.push({ op: 'dk.parseDesc', args: [d, null] });
    calls.push({ op: 'dk.parseDesc', args: ['EP B', { on: false, begin: ['B'], end: ['E'], close: ['C'] }] }, { op: 'dk.cleanLineCodes', args: [{ begin: 'X' }] });
    const keys = [{ code: 'ep*', group: 'ep', use: 'breakline', label: 'Edge of pavement' }, { code: 'E?', group: 'ELEC' }, { code: 'TR*', group: 'TREES', use: 'off' }, { code: 'CP', group: '', use: 'off', label: 'Control' }, { code: 'A.B', group: 'DOT' }];
    calls.push({ op: 'dk.cleanKeys', args: [keys] }, { op: 'dk.groups', args: [keys, ['EP1', 'EP2 B', 'E1', 'EX', 'TREE', 'CP', 'A.B', 'AXB', 'GRD', 'ep']] });
    calls.push({ op: 'dk.keyStats', args: [[P(1, 0, 0, 0, 'EP1'), P(2, 0, 0, 0, 'EP2 B'), P(3, 0, 0, 0, 'TREE'), P(4, 0, 0, 0, 'GRD')], keys] }, { op: 'dk.cleanLineCodes', args: [{ on: true, begin: 'B, X', end: ['E', 'X'], close: 'C;E' }] });
    const pts = [P(1, 0, 0, 10, 'EP1 B'), P(2, 10, 0, 11, 'EP1'), P(3, 20, 0, 12, 'EP1 E'), P(4, 30, 0, 13, 'EP1 B'), P(5, 40, 0, 14, 'EP1'), P(6, 0, 5, 10, 'EP2 B'), P(7, 10, 5, 10, 'GRD'), P(8, 20, 5, 10, 'EP2'), P(9, 0, 20, 1, 'BLDG B'), P(10, 10, 20, 1, 'BLDG'), P(11, 10, 30, 1, 'BLDG'), P(12, 0, 30, 1, 'BLDG C'), P(13, 50, 50, 1, 'FL'), P(14, 60, 50, 1, 'FL'), P(15, 70, 50, 1, 'FL')];
    calls.push({ op: 'dk.figures', args: [pts, { keys: [{ code: 'EP*', group: 'EP' }] }] }, { op: 'dk.figures', args: [[...pts].reverse(), { keys: [{ code: 'EP*', group: 'EP' }] }] });
    const scripts = [{ calls }];
    // the surface from a field-coded curb
    const cp = []; let n = 1;
    for (let x = -20; x <= 120; x += 20) for (let y = -40; y <= 40; y += 20) cp.push(P(n++, x, y, 100, 'GRD'));
    for (const [x, c] of [[0, 'TC B'], [10, 'TC'], [30, 'TC E'], [70, 'TC B'], [90, 'TC'], [100, 'TC E']]) cp.push(P(100 + x, x, 0.5, 101, c));
    const lc = { on: true, begin: ['B'], end: ['E'], close: ['C'] };
    const c2 = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [cp, 'curb'] }, { op: 'project.view', self: 'P' }];
    c2.push({ op: 'project.setCoding', self: 'P', args: [[{ code: 'TC', group: 'CURB', use: 'breakline', label: 'Top of curb' }], lc] }, { op: 'project.change', self: 'P', args: ['GRD use', [{ set: ['surface', 'groups', 'GRD'], value: 'off' }]] });
    c2.push({ op: 'project.setCoding', self: 'P', args: [[{ code: 'TC', group: 'CURB', use: 'breakline', label: 'Top of curb' }, { code: 'G*', group: 'GRD', use: 'breakline' }], lc] }, { op: 'project.undo', self: 'P' }, { op: 'project.undo', self: 'P' });
    c2.push({ op: 'project.egBreaklines', self: 'P' }, { op: 'project.eg', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.redo', self: 'P' }, { op: 'project.figures', self: 'P', args: [null] });
    c2.push({ op: 'project.change', self: 'P', args: ['B off', [{ set: ['surface', 'lineCodes', 'on'], value: false }]] }, { op: 'project.egBreaklines', self: 'P' }, { op: 'project.reload', self: 'P', args: [[]], as: 'Q' }, { op: 'project.view', self: 'Q' });
    c2.push({ op: 'project.fromJSON', args: [{ format: 'civilcraft-project', version: 1, units: 'ft', surface: { groups: {} } }], as: 'O' }, { op: 'project.view', self: 'O' });
    c2.push({ op: 'project.new', as: 'F' }, { op: 'project.setPoints', self: 'F', args: [fx.points, 'site'] }, { op: 'project.change', self: 'F', args: ['groups', [...['FL', 'TBN', 'TBS', 'RIDGE'].map((k) => ({ set: ['surface', 'groups', k], value: 'breakline' })), { set: ['surface', 'groups', 'CP'], value: 'off' }]] }, { op: 'project.eg', self: 'F' }, { op: 'project.figures', self: 'F', args: [null] });
    scripts.push({ calls: c2 });
    return scripts;
  },
});

const flatPts = (M, z = 100) => { const pts = []; let n = 1; for (let x = -200; x <= 300; x += 20) for (let y = -200; y <= 300; y += 20) pts.push(M({ n: String(n++), x, y, z, d: 'G' })); return pts; };
Object.assign(GENS, {
  // test-civilcraft-drive.mjs
  't-drive'(rnd, seed) {
    const M = mv(seed);
    const calls = [{ op: 'alignment.build', args: [{ pis: [{ x: 1000, y: 5000 }, { x: 1300, y: 5000 }].map(M) }], as: 'A' }, { op: 'profile.build', args: [[{ sta: 0, z: 100, L: 0 }, { sta: 300, z: 106, L: 0 }]], as: 'P' }, { op: 'profile.build', args: [[]], as: 'E' }];
    calls.push({ op: 'tin.buildSurface', args: [{ points: [[900, 4900], [1400, 4900], [1400, 5100], [900, 5100]].map(([x, y]) => M({ x, y, z: 50 })) }], as: 'G' });
    calls.push({ op: 'tin.buildSurface', args: [{ points: [[900, 4900], [1400, 4900], [1400, 5100], [900, 5100]].map(([x, y]) => M({ x, y, z: x / 100 })) }], as: 'H' });
    calls.push({ op: 'tin.buildSurface', args: [{ points: [[900, 4900], [1150, 4900], [1150, 5100], [900, 5100]].map(([x, y]) => M({ x, y, z: 7 })) }], as: 'K' });
    const o = M({ x: 1000, y: 5000 });
    calls.push({ op: 'drive.frames', args: ['A', { profile: 'P', ground: 'G' }, [50, 0, 150, 290, 300, 310], { eye: 3.5, ahead: 100, origin: { x: o.x, y: o.y, z: 90 }, exag: 2 }] }, { op: 'drive.frames', args: ['A', { profile: 'P', ground: 'G' }, [290], { ahead: 100 }] });
    calls.push({ op: 'drive.frames', args: ['A', { profile: 'E', ground: 'H' }, [0, 100, 200, 300], {}] }, { op: 'drive.frames', args: ['A', { ground: 'K' }, [0, 100, 149, 151, 200, 300], {}] });
    for (const [iso, lat, lon] of [['2026-06-21T18:00:00Z', 40, -105], ['2026-06-21T02:00:00Z', -33.87, 151.21], ['2026-03-20T06:00:00Z', 40, 0], ['2026-12-21T12:00:00Z', 51.48, 0]]) for (let m = 0; m < 1440; m += 97) calls.push({ op: 'drive.sun', args: [Date.parse(iso) + (m - 720) * 60000, lat, lon] });
    for (const [lat, lon] of [[40.0, -105.27], [-33.87, 151.21], [51.48, 0], [19.43, -99.13], [64.84, -147.72], [0, 0]]) for (const day of ['2026-01-15', '2026-04-10', '2026-07-04', '2026-10-12']) for (const h of [2, 11, 19]) calls.push({ op: 'drive.sun', args: [Date.parse(`${day}T${String(h).padStart(2, '0')}:17:00Z`), lat, lon] });
    calls.push({ op: 'drive.sunVector', args: [90, 30] }, { op: 'drive.sunVector', args: [180, 0] });
    return [{ calls }];
  },
  // test-civilcraft-features.mjs
  't-features'(rnd, seed) {
    const M = mv(seed);
    const F = (pts, extra = {}) => ({ id: 'f1', name: 'FL', pts: pts.map(([x, y, z]) => M({ x, y, z })), closed: false, use: 'eg', daylight: false, cut: 2, fill: 3, ...extra });
    const f = F([[0, 0, 100], [30, 40, 101], [30, 100, 98]]);
    const calls = [{ op: 'fl.segments', args: [f] }, { op: 'fl.stations', args: [f] }, { op: 'fl.setGrade', args: [f, 0, -0.01] }, { op: 'fl.setGrade', args: [f, 1, 0.1] }, { op: 'fl.evenGrade', args: [F([[0, 0, 100], [10, 0, 0], [30, 0, 0], [40, 0, 104]]), 0, 3] }, { op: 'fl.raise', args: [f, 2.5] }, { op: 'fl.setElevation', args: [f, 1, 99.5] }];
    calls.push({ op: 'tin.buildSurface', args: [{ points: [[-10, -10], [200, -10], [200, 200], [-10, 200]].map(([x, y]) => M({ x, y, z: 50 + 0.1 * x - 0.2 * y })) }], as: 'S' }, { op: 'fl.fromSurface', args: [F([[0, 0, 0], [100, 50, 0], [500, 500, 7]]), 'S'] });
    const closed = F([[0, 0, 1], [10, 0, 2], [10, 10, 3], [0, 10, 4]], { closed: true });
    calls.push({ op: 'fl.segments', args: [closed] }, { op: 'fl.stations', args: [closed] }, { op: 'fl.gradeLabel', args: [0.0123] }, { op: 'fl.gradeLabel', args: [-0.5] });
    const g = F([[0, 0, 100], [100, 0, 102]]);
    calls.push({ op: 'fl.offset', args: [g, 10, { dz: -0.5 }] }, { op: 'fl.offset', args: [g, -6, { slope: 0.02 }] }, { op: 'fl.offset', args: [F([[0, 0, 0], [100, 0, 0], [100, 100, 0]]), 5] });
    const sq = F([[0, 0, 0], [100, 0, 0], [100, 100, 0], [0, 100, 0]], { closed: true });
    calls.push({ op: 'fl.offset', args: [sq, 10] }, { op: 'fl.offset', args: [sq, -10] });
    calls.push({ op: 'fl.clean', args: [{ id: 'f12', name: 'A\u0007B', pts: [{ x: 0, y: 0, z: 'x' }, { x: 1e10, y: 0 }, M({ x: 5, y: 5, z: 2e7 }), M({ x: 6, y: 6, z: 3 })], closed: true, use: 'weird', daylight: 'yes', cut: -1, fill: 1e6 }] });
    const scripts = [{ calls }];
    // feature line as a breakline, closed design line, the project file
    const c2 = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [flatPts(M), 'flat'] }];
    c2.push({ op: 'project.change', self: 'P', args: ['Draw feature line', [{ push: ['features'], value: F([[-50, 10, 103], [150, 10, 103]], { id: 'f1' }) }]] }, { op: 'project.eg', self: 'P' }, { op: 'project.egBreaklines', self: 'P' });
    c2.push({ op: 'project.change', self: 'P', args: ['Feature line off', [{ set: ['features', 0, 'use'], value: 'off' }]] }, { op: 'project.eg', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.eg', self: 'P' });
    c2.push({ op: 'project.new', as: 'Q' }, { op: 'project.change', self: 'Q', args: ['fl only', [{ push: ['features'], value: F([[0, 0, 1], [10, 0, 2], [10, 10, 3], [0, 10, 4]], { id: 'f1', closed: true }) }]] }, { op: 'project.eg', self: 'Q' });
    const plane = (x, y) => 101 + 0.01 * x + 0.01 * y;
    c2.push({ op: 'project.new', as: 'D' }, { op: 'project.setPoints', self: 'D', args: [flatPts(M), 'flat'] });
    c2.push({ op: 'project.change', self: 'D', args: ['Draw', [{ push: ['features'], value: F([[0, 0, plane(0, 0)], [100, 0, plane(100, 0)], [100, 100, plane(100, 100)], [0, 100, plane(0, 100)]], { id: 'f1', closed: true, use: 'design' }) }]] }, { op: 'project.graded', self: 'D' });
    c2.push({ op: 'project.change', self: 'D', args: ['Sample', [{ push: ['alignments'], value: { id: 'a1', name: 'A', start: 0, pis: [M({ x: 0, y: 50, r: 0 }), M({ x: 100, y: 50, r: 0 })], pvis: [], corridor: null, sampling: { step: 10, left: 70, right: 70, extra: [] } } }]] }, { op: 'project.sections', self: 'D', args: ['a1'] });
    c2.push({ op: 'project.change', self: 'D', args: ['Daylight', [{ set: ['features', 0, 'daylight'], value: true }]] }, { op: 'project.graded', self: 'D' }, { op: 'project.volumes', self: 'D' });
    c2.push({ op: 'project.change', self: 'D', args: ['open', [{ set: ['features', 0, 'closed'], value: false }]] }, { op: 'project.graded', self: 'D' });
    c2.push({ op: 'project.new', as: 'J' }, { op: 'project.setPoints', self: 'J', args: [flatPts(M), 'flat'] }, { op: 'project.change', self: 'J', args: ['fl', [{ push: ['features'], value: F([[0, 0, 1], [5, 0, 2], [5, 5, 3]], { id: 'f7', closed: true, use: 'design', daylight: true, cut: 1.5, fill: 4 }) }]] });
    c2.push({ op: 'project.reload', self: 'J', args: [[{ push: ['features'], value: { pts: 'junk' } }, { push: ['features'], value: { id: 'evil', pts: [{ x: 0, y: 0 }, { x: 1, y: 1 }] } }]], as: 'K' }, { op: 'project.toJSON', self: 'K' }, { op: 'project.graded', self: 'K' });
    scripts.push({ calls: c2 });
    return scripts;
  },
});

Object.assign(GENS, {
  // test-civilcraft-flprofile.mjs
  't-flprofile'(rnd, seed) {
    const M = mv(seed);
    const fl = (id, pts, extra = {}) => ({ id, name: id.toUpperCase(), pts: pts.map(([x, y, z]) => M({ x, y, z })), closed: false, use: 'eg', ...extra });
    const calls = [{ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 100, y: 0 }].map(M), start: 1000 }], as: 'ST' }];
    const curbX = fl('x', [[50, -10, 100], [50, 10, 104]]), curb = fl('c', [[10, -6, 100], [50, -6, 102], [90, -6, 106]]);
    for (const f of [curbX, fl('k', [[20, -5, 90], [30, 5, 91], [40, -5, 93]]), fl('v', [[60, -4, 80], [60, 0, 82], [60, 4, 84]]), fl('b', [[70, -3, 50], [80, -3, 50], [80, 3, 52], [70, 3, 52]], { closed: true })]) calls.push({ op: 'fl.crossings', args: ['ST', f] });
    calls.push({ op: 'fl.runs', args: ['ST', curb, 10] }, { op: 'fl.runs', args: ['ST', fl('r', [[90, 4, 1], [10, 4, 2]]), 5] }, { op: 'fl.runs', args: ['ST', fl('l', [[10, 2, 1], [30, 2, 2], [40, 30, 3], [60, 2, 4], [80, 2, 5]]), 5] }, { op: 'fl.runs', args: ['ST', fl('cr', [[10, -3, 10], [30, 3, 12]]), 5] });
    calls.push({ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 100, y: 0, r: 50 }, { x: 100, y: 100 }].map(M), start: 0 }], as: 'CV' });
    const ang = -Math.PI / 4;
    calls.push({ op: 'fl.crossings', args: ['CV', fl('rad', [[50 + 40 * Math.cos(ang), 50 + 40 * Math.sin(ang), 200], [50 + 60 * Math.cos(ang), 50 + 60 * Math.sin(ang), 210]])] }, { op: 'fl.crossings', args: ['CV', fl('ch', [[60, 40, 5], [110, 40, 7]])] });
    calls.push({ op: 'fl.profile', args: ['ST', [curbX, curb, fl('far', [[10, 50, 1], [90, 50, 2]])], { within: 10 }] }, { op: 'fl.raise', args: [curbX, 1] });
    const o = M({ x: 0, y: 0 });
    const ln = fl('g', [[0, 0, 100], [100, 0, 98], [100, 30, 98], [100, 130, 99]]);
    calls.push({ op: 'fl.overlay', args: [[ln], { k: 1, ox: o.x, oy: o.y }] }, { op: 'fl.overlay', args: [[fl('bk', [[100, 0, 1], [0, 0, 2]])], { k: 1, ox: o.x, oy: o.y }] }, { op: 'fl.overlay', args: [[ln], { k: 0.1, ox: o.x, oy: o.y }] });
    calls.push({ op: 'scene.plan', args: [{ units: 'ft', points: [M({ n: '1', x: 0, y: 0, z: 1, d: 'A' })], contours: { interval: 1, major: 5 }, breaklines: [], alignments: [], pads: [] }, { mmPerUnit: 1 }] });
    return [{ calls }];
  },
  // test-civilcraft-hydro.mjs
  't-hydro'(rnd, seed) {
    const M = mv(seed);
    const sq = (x, y, s) => [{ x, y }, { x: x + s, y }, { x: x + s, y: y + s }, { x, y: y + s }].map(M);
    const net = (dia1 = 12, dia2 = 15) => {
      const A1 = Math.sqrt(43560), A2 = Math.sqrt(2 * 43560);
      return { name: 'Storm', type: 'storm', rules: { minSlope: 0.005, minCover: 2, minDrop: 0.1 },
        structures: [{ id: 's1', name: 'S1', x: 0, y: 0, rim: 110, sumpDepth: 0, dia: 48 }, { id: 's2', name: 'S2', x: 200, y: 0, rim: 108, sumpDepth: 0, dia: 48 }, { id: 's3', name: 'S3', x: 400, y: 0, rim: 106, sumpDepth: 0, dia: 48 }].map(M),
        pipes: [{ id: 'p1', name: 'P1', from: 's1', to: 's2', dia: dia1, invUp: 105, invDn: 103 }, { id: 'p2', name: 'P2', from: 's2', to: 's3', dia: dia2, invUp: 102.6, invDn: 100.6 }],
        catchments: [{ id: 'k1', name: 'A1', pts: sq(-300, 50, A1), C: 0.5, tc: 10, to: 's1' }, { id: 'k2', name: 'A2', pts: sq(100, 50, A2), C: 0.8, tc: 15, to: 's2' }], hydro: null };
    };
    const calls = [{ op: 'hydro.clean', args: [null, 'ft'] }, { op: 'hydro.clean', args: [null, 'm'] }, { op: 'hydro.manningFull', args: [1, 0.01, 0.013, 'ft'] }, { op: 'hydro.normalDepth', args: [1, 1, 0.01, 0.013, 'ft'] }];
    calls.push({ op: 'hydro.partFull', args: [0.4, 1] }, { op: 'hydro.cleanCatchment', args: [{ id: 'k3', name: 'A\u0007', pts: [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 'x', y: 1 }, { x: 1, y: 1 }, { x: 0, y: 1 }], C: 2, tc: -1, to: 's1' }] });
    const N = net();
    calls.push({ op: 'hydro.flows', args: [N, 'ft'], as: 'F' }, { op: 'hydro.area', args: [N.catchments[0], 'ft'] });
    calls.push({ op: 'hydro.flows', args: [{ ...N, catchments: [{ ...N.catchments[0], pts: sq(0, 0, 100) }], hydro: { idf: { a: 1700, b: 12, c: 0.8 }, n: 0.013 } }, 'm'] });
    calls.push({ op: 'hydro.flows', args: [{ ...N, pipes: [...N.pipes, { id: 'p3', name: 'P3', from: 's3', to: 's1', dia: 12, invUp: 100, invDn: 99 }] }, 'ft'] });
    const N1 = net(12, 12); N1.catchments[1].C = 1; N1.catchments[1].pts = sq(100, 50, Math.sqrt(0.6 * 43560));
    calls.push({ op: 'hydro.flows', args: [N1, 'ft'], as: 'F1' }, { op: 'hydro.size', args: [N1, 'F1', 'ft'] }, { op: 'hydro.grade', args: [N1, 'F1', 'ft'] });
    const N2 = { ...N1, pipes: N1.pipes.map((p) => ({ ...p, dia: 18 })) };
    calls.push({ op: 'hydro.flows', args: [N2, 'ft'], as: 'F2' }, { op: 'hydro.grade', args: [N2, 'F2', 'ft'] });
    const N3 = { ...N2, hydro: { tailwater: 103 } };
    calls.push({ op: 'hydro.flows', args: [N3, 'ft'], as: 'F3' }, { op: 'hydro.grade', args: [N3, 'F3', 'ft'] });
    const N4 = { ...N2, structures: N2.structures.map((s) => (s.id === 's2' ? { ...s, rim: 103.5 } : s)), hydro: { tailwater: 104 } };
    calls.push({ op: 'hydro.flows', args: [N4, 'ft'], as: 'F4' }, { op: 'hydro.grade', args: [N4, 'F4', 'ft'] });
    calls.push({ op: 'project.new', as: 'P' }, { op: 'project.change', self: 'P', args: ['net', [{ set: ['network'], value: { ...net(), hydro: { idf: { a: 70, b: 12, c: 0.8 }, n: 0.012, kj: 0.3, tailwater: 101 } } }]] }, { op: 'project.reload', self: 'P', args: [[]], as: 'Q' }, { op: 'project.toJSON', self: 'Q' }, { op: 'project.networkCheck', self: 'Q' });
    return [{ calls }];
  },
});

Object.assign(GENS, {
  // test-civilcraft-masshaul.mjs
  't-masshaul'(rnd, seed) {
    const M = mv(seed);
    const rows = (vols) => vols.map(([c, f], i) => ({ sta: i * 100, cutVol: c, fillVol: f }));
    const calls = [{ op: 'mh.massHaul', args: [rows([[0, 0], [1000, 0], [1000, 0], [0, 1800], [0, 600]]), { factor: 0.9 }], as: 'R' }, { op: 'mh.csv', self: 'R', args: [{ units: 'ft' }] }];
    calls.push({ op: 'mh.massHaul', args: [rows([[0, 0], [900, 0], [900, 0], [0, 600]]), { factor: 1 }] }, { op: 'mh.massHaul', args: [rows([[0, 0], [1000, 0], [1000, 0], [0, 2000], [0, 400]]), { factor: 0.9 }] }, { op: 'mh.massHaul', args: [rows([[0, 0], [1000, 0], [0, 1050]]), { factor: 1.05 }] });
    const pts = []; let n = 1;
    for (let x = -100; x <= 1100; x += 25) for (let y = -150; y <= 150; y += 25) pts.push(M({ n: String(n++), x, y, z: 100 + 3 * Math.sin(x / 80), d: 'G' }));
    calls.push({ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [pts, 'wave'] });
    calls.push({ op: 'project.change', self: 'P', args: ['road', [{ push: ['alignments'], value: { id: 'a1', name: 'Main', start: 0, pis: [M({ x: 0, y: 0, r: 0 }), M({ x: 1000, y: 0, r: 0 })], pvis: [{ sta: 0, z: 101, L: 0 }, { sta: 1000, z: 101, L: 0 }], corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10 }, sampling: { step: 25, left: 80, right: 80, extra: [] } } }]] });
    calls.push({ op: 'project.sections', self: 'P', args: ['a1'] }, { op: 'mh.project', args: ['P', 'a1', { factor: 1 }], as: 'MH' }, { op: 'mh.csv', self: 'MH', args: [{ units: 'ft' }] }, { op: 'mh.project', args: ['P', 'a1', { factor: 0.85 }] });
    return [{ calls }];
  },
});

Object.assign(GENS, {
  // test-civilcraft-parcels.mjs
  't-parcels'(rnd, seed) {
    const M = mv(seed);
    const calls = [];
    for (const b of ['N 45°30\'15" E', 'N45-30-15E', 'S 30 00 00 W', 'S 30.5 E', 'N 10 W', 'N 0 E', 'AZ 270', 'az 123°45\'00"', 'N 95 E', 'N 10 61 E', 'N 10.5 30 E', '45', '', 'AZ 361']) calls.push({ op: 'pc.parseBearing', args: [b] });
    for (const a of ['10°30\'', '10-30-00', '90', 'x', '1 2 3 4']) calls.push({ op: 'pc.parseAngle', args: [a] });
    const deed = 'N 0 E 200\nthence N 90°00\'00" E, 100.00 feet;\nS 0 E 200\n# a comment\nS 90 W 100';
    calls.push({ op: 'pc.parseCalls', args: [deed] }, { op: 'pc.runText', args: [M({ x: 1000, y: 5000 }), deed] }, { op: 'pc.runText', args: [M({ x: 0, y: 0 }), 'N 0 E 200\nN 90 E 100\nS 0 E 200\nS 90 W 99.95'] });
    calls.push({ op: 'pc.runText', args: [M({ x: 0, y: 0 }), 'N 0 E 100\nC R 100 L ' + (Math.PI * 50) + ' right'] }, { op: 'pc.runText', args: [M({ x: 0, y: 0 }), 'N 90 E 50\nC R 100 D 60-00-00 left CB N 0 E'] });
    calls.push({ op: 'pc.parseCalls', args: ['C R 300 D 10°00\'00" right'] }, { op: 'pc.parseCalls', args: ['N 45 E\nN 95 E 10\nC R 100 L 10\nC L 10 left\nN 45 E 0\nhello'] });
    const sq = { id: 'r1', name: 'Lot 1', pts: [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100, b: 1 }, { x: 0, y: 100 }].map(M) };
    const n = sq.pts.length, rev = sq.pts.map((_, i) => { const p = sq.pts[n - 1 - i], from = sq.pts[(2 * n - 2 - i) % n]; return { x: p.x, y: p.y, ...(from.b ? { b: -from.b } : {}) }; });
    for (const pts of [sq.pts, rev]) calls.push({ op: 'pc.courses', args: [pts] }, { op: 'pc.area', args: [pts] }, { op: 'pc.perimeter', args: [pts] }, { op: 'pc.centroid', args: [pts] });
    calls.push({ op: 'pc.areaWords', args: [10000, 'ft'] }, { op: 'pc.areaWords', args: [10000, 'm'] }, { op: 'pc.legal', args: [sq, { units: 'ft' }] }, { op: 'pc.legal', args: [{ name: 'Lot 2', pts: [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }, { x: 0, y: 100 }].map(M) }, { units: 'm', pob: 'the north-east corner of Lot 1' }] });
    calls.push({ op: 'pc.labels', args: [sq, { units: 'ft', unitPerMm: 1 }] }, { op: 'pc.clean', args: [{ id: 'r5', name: 'A\u0007B', pts: [{ x: 0, y: 0, b: 7 }, { x: 'x', y: 1 }, { x: 1, y: 0, b: 0.5 }, { x: 1, y: 1 }] }] });
    calls.push({ op: 'project.new', as: 'P' }, { op: 'project.change', self: 'P', args: ['Parcel', [{ push: ['parcels'], value: sq }]] }, { op: 'project.reload', self: 'P', args: [[]], as: 'Q' }, { op: 'project.toJSON', self: 'Q' });
    return [{ calls }];
  },
  // test-civilcraft-points.mjs
  't-points'(rnd, seed) {
    const fx = fixture(seed), M = mv(seed);
    const pts = fx.points.map((p) => ({ ...p })), a = pts[10], b = pts[11];
    const calls = [];
    for (const patch of [{ x: '1,234.5', y: String(a.y), z: ' 101.25 ' }, { z: '' }, { z: null }, { n: b.n }, { n: '   ' }, { n: 'x'.repeat(41) }, { n: ' CP-1\u0007 ' }, ...['abc', '1e10', 'NaN', 'Infinity', '12..3', '--1', {}, []].map((x) => ({ x })), { d: 'EP\tedge\n' }, { x: a.x, d: a.d }, { x: 'oops', z: 'bad' }]) calls.push({ op: 'pe.check', args: [pts, a.n, patch] });
    calls.push({ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [pts, 'site-topo.csv'] }, { op: 'project.eg', self: 'P' });
    const q = pts[40];
    calls.push({ op: 'project.editPoint', self: 'P', args: [q.n, { z: q.z + 10 }] }, { op: 'project.eg', self: 'P' }, { op: 'project.undo', self: 'P' });
    calls.push({ op: 'project.editPoint', self: 'P', args: [q.n, { x: q.x + 3, y: q.y - 4 }, 'Move point ' + q.n] }, { op: 'project.view', self: 'P' }, { op: 'project.undo', self: 'P' });
    calls.push({ op: 'project.editPoint', self: 'P', args: [q.n, { n: 'S-12345' }] }, { op: 'project.undo', self: 'P' }, { op: 'project.editPoint', self: 'P', args: [q.n, { d: 'NEWCODE spot' }] }, { op: 'project.groups', self: 'P' }, { op: 'project.undo', self: 'P' });
    const [p0, p1, p2] = pts.filter((x) => x.z !== null).slice(0, 3);
    calls.push({ op: 'project.change', self: 'P', args: ['Draw breakline', [{ push: ['surface', 'breaklines'], value: { name: 'Breakline 1', pts: [p0, p1, p2].map((x) => ({ x: x.x, y: x.y, z: x.z })) } }]] });
    calls.push({ op: 'project.editPoint', self: 'P', args: [p1.n, { x: p1.x + 1, z: p1.z + 2 }] }, { op: 'project.view', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.editPoint', self: 'P', args: [p1.n, { x: p1.x + 1, z: '' }] }, { op: 'project.view', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.editPoint', self: 'P', args: [p1.n, { d: 'renamed' }] }, { op: 'project.view', self: 'P' });
    calls.push({ op: 'project.new', as: 'T' }, { op: 'project.setPoints', self: 'T', args: [pts, 'site-topo.csv'] }, { op: 'project.change', self: 'T', args: ['tin', [{ set: ['surface', 'tin'], value: { faces: [], pts: [] } }]] }, { op: 'project.editPoint', self: 'T', args: [pts[0].n, { z: 1 }] });
    calls.push({ op: 'project.new', as: 'Q' }, { op: 'project.setPoints', self: 'Q', args: [pts, 'site-topo.csv'] }, { op: 'project.editPoint', self: 'Q', args: [pts[5].n, { n: 'CTRL 5', x: 100, y: 200, z: 3.5, d: 'CP control' }] }, { op: 'project.reload', self: 'Q', args: [[]], as: 'R' }, { op: 'project.view', self: 'R' });
    const P = [{ n: '1', x: 0, y: 0 }, { n: '2', x: 10, y: 0 }, { n: '3', x: 10, y: 10 }, { n: '4', x: 30, y: 30 }, { n: '5', x: 20, y: 5 }].map(M);
    const L = [{ x: -2, y: -2 }, { x: 12, y: -2 }, { x: 12, y: 12 }, { x: 8, y: 12 }, { x: 8, y: 2 }, { x: -2, y: 2 }].map(M), edge = [{ x: -2, y: -2 }, { x: 10.05, y: -2 }, { x: 10.05, y: 2 }, { x: -2, y: 2 }].map(M), fence = [{ x: -5, y: 0.05 }, { x: 10.05, y: 0.05 }, { x: 10.05, y: 10.5 }].map(M);
    for (const [poly, mode, tol] of [[L, 'window', 0.1], [L, 'crossing', 0.1], [edge, 'window', 0.1], [edge, 'crossing', 0.1], [fence, 'fence', 0.1], [fence, 'fence', 0.01]]) calls.push({ op: 'points.lassoPoints', args: [P, poly, mode, tol] });
    return [{ calls }];
  },
});

const flatSiteCalls = (M, zRoad, name = 'P') => {
  const pts = []; let n = 1;
  for (let x = -100; x <= 1100; x += 25) for (let y = -250; y <= 250; y += 25) pts.push(M({ n: String(n++), x, y, z: 100, d: 'G' }));
  return [{ op: 'project.new', as: name }, { op: 'project.setPoints', self: name, args: [pts, 'flat'] }, { op: 'project.change', self: name, args: ['road', [{ push: ['alignments'], value: { id: 'a1', name: 'Road', start: 0, pis: [M({ x: 0, y: 0, r: 0 }), M({ x: 1000, y: 0, r: 0 })], pvis: [{ sta: 0, z: zRoad, L: 0 }, { sta: 1000, z: zRoad, L: 0 }], corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10 }, sampling: null } }]] }];
};
const smp = (step, left, right, extra = []) => ({ step, left, right, extra });
Object.assign(GENS, {
  // test-civilcraft-sections.mjs
  't-sections'(rnd, seed) {
    const M = mv(seed), fx = fixture(seed);
    const calls = [{ op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 500, y: 0, r: 200 }, { x: 500, y: 500 }].map(M), start: 1000 }], as: 'A' }, { op: 'sec.stations', args: ['A', { step: 50, extra: [1234.5, -5, 99999] }] }, { op: 'al.keyStations', self: 'A' }, { op: 'sec.line', args: ['A', 1100, 30, 40] }];
    const run = (...pts) => [pts.map(([o, z]) => ({ o, z }))];
    for (const [g, d] of [[run([0, 0], [10, 0]), run([0, 2], [10, 2])], [run([0, 0], [10, 0]), run([0, 2], [10, -2])], [run([0, 0], [4, 4], [10, 4]), run([0, 1], [10, 1])], [run([-5, 0], [5, 0]), run([0, 3], [10, 3])], [[[{ o: -10, z: 0 }, { o: -2, z: 0 }], [{ o: 2, z: 0 }, { o: 10, z: 0 }]], run([-10, 1], [10, 1])]]) calls.push({ op: 'sec.cutFill', args: [g, d] });
    const pts = [];
    for (let i = 0; i < 400; i++) { const x = (i * 37.3) % 200, y = (i * 91.7) % 200; pts.push(M({ x, y, z: 50 + Math.sin(x / 23) * 7 + Math.cos(y / 17) * 5 })); }
    calls.push({ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'sec.surfaceSection', args: ['S', M({ x: 10, y: 15 }), M({ x: 190, y: 170 }), -100, 100] }, { op: 'sec.surfaceSection', args: ['S', M({ x: -500, y: -500 }), M({ x: -400, y: -400 }), 0, 100] }, { op: 'sec.surfaceSection', args: ['S', M({ x: -100, y: 100 }), M({ x: 100, y: 100 }), 0, 200] });
    calls.push({ op: 'sec.zAt', args: [[[{ o: -1, z: 1 }, { o: 1, z: 3 }]], 0.5] });
    const line = { a: M({ x: -50, y: 0 }), b: M({ x: 50, y: 0 }), left: 50, right: 50 };
    const sq = (x0, x1) => [{ x: x0, y: -5 }, { x: x1, y: -5 }, { x: x1, y: 5 }, { x: x0, y: 5 }].map(M);
    calls.push({ op: 'sec.regionSpans', args: [line, [sq(-30, -10), sq(5, 20)]] }, { op: 'sec.clipRuns', args: [[[{ o: -50, z: 1 }, { o: 0, z: 2 }, { o: 50, z: 3 }]], [[-30, -10], [5, 20]]] });
    calls.push({ op: 'sec.clean', args: [{ step: -1, left: 1e9, right: 'x', extra: [5, 5, '$NaN', 'a', 3, 1e9] }, 'm'] }, { op: 'sec.clean', args: [{ step: 50, left: 60, right: 60 }, 'ft'] });
    const scripts = [{ calls }];
    for (const zRoad of [105, 95]) {
      const c = flatSiteCalls(M, zRoad);
      c.push({ op: 'project.change', self: 'P', args: ['Sample lines', [{ set: ['alignments', 0, 'sampling'], value: smp(50, 60, 60) }]] }, { op: 'sec.project', args: ['P', 'a1'], as: 'X' }, { op: 'project.volumes', self: 'P' }, { op: 'sec.csv', self: 'X', args: [{ units: 'ft' }] });
      if (zRoad === 105) {
        c.push({ op: 'sheets.sections', args: ['X', { paper: 'tabloid', scale: { label: '1" = 20\'', mmPerUnit: 25.4 / 20 }, vex: 5, units: 'ft', title: 'Flat', name: 'Road' }], as: 'XS' }, { op: 'plot.toPdfPages', args: ['XS', { title: 'Flat Road' }] });
        c.push({ op: 'sheets.sections', args: ['X', { paper: 'letter', scale: { label: '1" = 5\'', mmPerUnit: 25.4 / 5 }, vex: 10, units: 'ft', name: 'R' }] });
        c.push({ op: 'project.change', self: 'P', args: ['corridor off', [{ set: ['alignments', 0, 'corridor', 'on'], value: false }, { set: ['alignments', 0, 'sampling'], value: smp(100, 50, 50) }]] }, { op: 'sec.project', args: ['P', 'a1'] });
        c.push({ op: 'project.change', self: 'P', args: ['Sample lines', [{ set: ['alignments', 0, 'corridor', 'on'], value: true }, { set: ['alignments', 0, 'sampling'], value: smp(25, 40, 45, [512.5]) }]] });
        c.push({ op: 'project.reload', self: 'P', args: [[{ push: ['alignments', 0, 'sampling', 'extra'], value: 'junk' }, { push: ['alignments', 0, 'sampling', 'extra'], value: 1e12 }]], as: 'Q' }, { op: 'project.toJSON', self: 'Q' }, { op: 'project.undo', self: 'P' }, { op: 'project.view', self: 'P' });
      }
      scripts.push({ calls: c });
    }
    // the design counts only inside the graded regions (fixture + pad + a base line)
    const c3 = [{ op: 'project.new', as: 'F' }, { op: 'project.setPoints', self: 'F', args: [fx.points, 'site'] }];
    const cx = fx.E + 200, cy = fx.N + 150;
    c3.push({ op: 'project.change', self: 'F', args: ['pad', [{ push: ['pads'], value: { id: 'p1', name: 'Pad', pts: [{ x: cx - 40, y: cy - 30 }, { x: cx + 40, y: cy - 30 }, { x: cx + 40, y: cy + 30 }, { x: cx - 40, y: cy + 30 }], z: 254, cut: 2, fill: 2 } }, { push: ['alignments'], value: { id: 'a1', name: 'Base', start: 0, pis: [{ x: cx - 120, y: cy }, { x: cx + 120, y: cy }], pvis: [], corridor: null, sampling: smp(10, 120, 120) } }]] });
    c3.push({ op: 'project.graded', self: 'F' }, { op: 'sec.project', args: ['F', 'a1'] }, { op: 'project.volumes', self: 'F' });
    scripts.push({ calls: c3 });
    return scripts;
  },
  // test-civilcraft-sheets.mjs
  't-sheets'(rnd, seed) {
    const M = mv(seed), fx = fixture(seed);
    const s40 = { label: '1" = 40\'', mmPerUnit: 25.4 / 40 };
    const scripts = [];
    for (const ang of [0, 45, 160]) {
      const a = (ang * Math.PI) / 180, p0 = M({ x: 5000, y: 2000 });
      const eg = []; for (let s = 0; s <= 1000; s += 5) eg.push({ sta: s, z: 100 + 0.01 * s + 2 * Math.sin(s / 80) });
      const calls = [{ op: 'alignment.build', args: [{ pis: [p0, { x: p0.x + Math.cos(a) * 1000, y: p0.y + Math.sin(a) * 1000 }] }], as: 'A' }, { op: 'profile.build', args: [[{ sta: 0, z: 101 }, { sta: 500, z: 108, L: 200 }, { sta: 1000, z: 104 }]], as: 'PR' }];
      calls.push({ op: 'scene.plan', args: [{ units: 'ft', alignments: [{ name: 'Main St', alignment: 'A', pis: [] }] }, { mmPerUnit: s40.mmPerUnit }], as: 'SC' });
      if (ang === 0) {
        calls.push({ op: 'sheets.plan', args: ['A', { paper: 'tabloid', scale: s40 }] }, { op: 'sheets.plan', args: ['A', { paper: 'tabloid', scale: { label: '1" = 10\'', mmPerUnit: 2.54 } }] }, { op: 'sheets.plan', args: ['A', { paper: 'tabloid', scale: s40, from: 200, to: 400 }] });
        calls.push({ op: 'alignment.build', args: [{ pis: [M({ x: 0, y: 0 }), M({ x: 900, y: 0 })] }], as: 'B' }, { op: 'sheets.plan', args: ['B', { paper: 'a3', scale: { label: '1:500', mmPerUnit: 2 }, units: 'm' }] });
        const steep = []; for (let s = 0; s <= 1000; s += 10) steep.push({ sta: s, z: 100 + s * 0.08 });
        calls.push({ op: 'sheets.pp', args: ['SC', { alignment: 'A', eg: steep, profile: null, paper: 'tabloid', scale: s40, vex: 10, units: 'ft', name: 'Steep' }] });
      }
      calls.push({ op: 'sheets.pp', args: ['SC', { alignment: 'A', eg, profile: 'PR', paper: 'tabloid', scale: s40, vex: 10, units: 'ft', title: 'Main St', name: 'Main St' }], as: 'PP' }, { op: 'plot.toPdfPages', args: ['PP', { title: 'Main St' }] });
      scripts.push({ calls });
    }
    // the fixture site, a corridor, the PDF
    const y = fx.N + 150, x0 = fx.E - 15, x1 = fx.E + 412;
    const pis = [{ x: x0 + 20, y, r: 0 }, { x: (x0 + x1) / 2, y: y + 40, r: 150 }, { x: x1 - 20, y, r: 0 }];
    const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site'] }, { op: 'project.change', self: 'P', args: ['road', [{ push: ['alignments'], value: { id: 'a1', name: 'Site road', start: 0, pis, pvis: [{ sta: 0, z: 252, L: 0 }, { sta: 380, z: 254, L: 0 }], corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10 }, sampling: null } }]] }];
    calls.push({ op: 'project.egProfile', self: 'P', args: ['a1'] }, { op: 'project.graded', self: 'P' });
    calls.push({ op: 'tin.buildSurface', args: [{ points: fx.points }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 0 }], as: 'A' }, { op: 'profile.build', args: [[{ sta: 0, z: 252, L: 0 }, { sta: 380, z: 254, L: 0 }]], as: 'PR' }, { op: 'profile.sampleSurface', args: ['S', 'A'] });
    const sc20 = { label: '1" = 20\'', mmPerUnit: 25.4 / 20 };
    calls.push({ op: 'scene.plan', args: [{ units: 'ft', surface: 'S', contours: { interval: 1, major: 5 }, alignments: [{ name: 'Site road', alignment: 'A', pis }], corridors: [{ alignment: 'A', profile: 'PR', asm: { lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10 } }] }, { mmPerUnit: sc20.mmPerUnit }], as: 'SC' });
    const eg = []; for (let s = 0; s <= 400; s += 10) eg.push({ sta: s, z: 250 + Math.sin(s / 50) });
    calls.push({ op: 'sheets.pp', args: ['SC', { alignment: 'A', eg, profile: 'PR', paper: 'tabloid', scale: sc20, vex: 5, units: 'ft', title: 'Sample site', name: 'Site road' }], as: 'PP' }, { op: 'plot.toPdfPages', args: ['PP', { title: 'Sample site' }] });
    calls.push({ op: 'plot.planSheet', args: ['SC', { extents: { minX: x0, minY: fx.N - 12, maxX: x1, maxY: fx.N + 309 }, paper: 'tabloid', scale: sc20, title: 'Plan', units: 'ft' }], as: 'SH' }, { op: 'plot.toPdf', args: ['SH', {}] });
    scripts.push({ calls });
    return scripts;
  },
  // test-civilcraft-staking.mjs
  't-staking'(rnd, seed) {
    const M = mv(seed);
    const calls = [{ op: 'alignment.build', args: [{ pis: [{ x: 1000, y: 5000 }, { x: 1200, y: 5000 }].map(M) }], as: 'A' }, { op: 'profile.build', args: [[{ sta: 0, z: 100, L: 0 }, { sta: 200, z: 104, L: 0 }]], as: 'PR' }];
    calls.push({ op: 'sk.stations', args: ['A', { from: 0, to: 200, step: 50 }] }, { op: 'sk.stations', args: ['A', { from: 25, to: 130, step: 50 }] }, { op: 'alignment.build', args: [{ pis: [{ x: 0, y: 0 }, { x: 300, y: 0, r: 200 }, { x: 500, y: -200 }].map(M) }], as: 'C' }, { op: 'sk.stations', args: ['C', { step: 100 }] });
    const design = { profile: 'PR' }, ground = { flat: 101 };
    calls.push({ op: 'sk.rows', args: ['A', { stations: [100], offsets: [-12, 0, 12], design, ground, first: 101, prefix: 'S' }] }, { op: 'sk.rows', args: ['A', { stations: [0], offsets: [30], design, ground }] });
    for (const t of ['12 L, 0, 12 R', 'L12 CL R12', '-12 0 12 12', '16.5L 16.5r', '12 X', 'L-12', 'L12R']) calls.push({ op: 'sk.parseOffsets', args: [t] });
    for (const v of [1.234, -0.5, 0, null]) calls.push({ op: 'sk.cutFillText', args: [v] }, { op: 'sk.offsetText', args: [v ?? 3] });
    const pts = [{ n: '7', x: 1150, y: 4990, z: 103.5 }, { n: '8', x: 1050, y: 5005, z: 100 }, { n: '9', x: 900, y: 5000, z: 100 }].map(M);
    calls.push({ op: 'sk.pointRows', args: ['A', pts, { design }] });
    const rows = { stations: [0, 100], offsets: [0], design, ground, prefix: '=S' };
    calls.push({ op: 'sk.rows', args: ['A', rows] });
    const stations = []; for (let s = 0; s <= 200; s += 5) stations.push(s);
    calls.push({ op: 'sk.stations', args: ['A', { step: 5 }] });
    const exRows = stations.flatMap((s, i) => [-12, 0, 12].map((o, k) => ({ id: String(1 + i * 3 + k), sta: s, off: o, x: M({ x: 1000 + s }).x, y: M({ y: 5000 - o }).y, design: 100 + s * 0.02, ground: 101, cf: 100 + s * 0.02 - 101 })));
    calls.push({ op: 'sk.csv', args: [exRows.slice(0, 6), { units: 'ft', title: 'Main St stakes' }] }, { op: 'sheets.staking', args: [exRows, { paper: 'letter', units: 'ft', title: 'Main St', name: 'Main St' }], as: 'SK' }, { op: 'plot.toPdfPages', args: ['SK', { title: 'Staking' }] });
    return [{ calls }];
  },
});

Object.assign(GENS, {
  // test-civilcraft-super.mjs
  't-super'(rnd, seed) {
    const M = mv(seed);
    const calls = [{ op: 'super.defaults', args: ['ft'] }, { op: 'super.defaults', args: ['m'] }, { op: 'super.clean', args: [{ on: true, delta: 9, rate: [[100, 0.05], ['x', 1], [500, null], [300, 0.9], [-1, 0.02]] }, 'ft'] }];
    const sp = { on: true, delta: 0.005, rate: [[7000, null], [5000, 0.02], [3000, 0.03], [2000, 0.04], [1000, 0.06], [0, 0.08]] };
    const al = (pis) => ({ op: 'alignment.build', args: [{ pis: pis.map(M) }] });
    const cases = [['R', [{ x: 0, y: 0 }, { x: 1000, y: 0, r: 2000 }, { x: 1700, y: -700 }]], ['L', [{ x: 0, y: 0 }, { x: 1000, y: 0, r: 2000 }, { x: 1700, y: 700 }]], ['F', [{ x: 0, y: 0 }, { x: 1000, y: 0, r: 9000 }, { x: 2000, y: -50 }]], ['T', [{ x: 0, y: 0 }, { x: 500, y: 0, r: 600 }, { x: 1000, y: -60 }]], ['S2', [{ x: 0, y: 0 }, { x: 1000, y: 0, r: 1000 }, { x: 1400, y: -100, r: 1000 }, { x: 2400, y: -100 }]]];
    for (const [k, pis] of cases) {
      calls.push({ ...al(pis), as: k }, { op: 'super.plan', args: [k, { super: sp, nc: 0.02, lane: 12 }], as: 'P' + k }, { op: 'super.keyStations', self: 'P' + k });
      for (let s = 0; s <= 2400; s += 37) calls.push({ op: 'super.slopesAt', self: 'P' + k, args: [s, 0.02] });
    }
    for (const r of [500, 1500, 2500, 6000, 8000]) calls.push({ op: 'super.rateFor', args: [r, sp.rate] });
    const pts = []; let n = 1;
    for (let x = -200; x <= 2000; x += 40) for (let y = -900; y <= 200; y += 40) pts.push(M({ n: String(n++), x, y, z: 100, d: 'G' }));
    calls.push({ op: 'project.new', as: 'Q' }, { op: 'project.setPoints', self: 'Q', args: [pts, 'flat'] });
    calls.push({ op: 'project.change', self: 'Q', args: ['road', [{ push: ['alignments'], value: { id: 'a1', name: 'Curve', start: 0, pis: [M({ x: 0, y: 0, r: 0 }), M({ x: 1000, y: 0, r: 2000 }), M({ x: 1700, y: -700, r: 0 })], pvis: [{ sta: 0, z: 103, L: 0 }, { sta: 1990, z: 103, L: 0 }], corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10, super: sp }, sampling: null } }]] });
    calls.push({ op: 'project.graded', self: 'Q' }, { op: 'project.change', self: 'Q', args: ['Sample', [{ set: ['alignments', 0, 'sampling'], value: { step: 10000, left: 60, right: 60, extra: [1000] } }]] }, { op: 'sec.project', args: ['Q', 'a1'] }, { op: 'project.reload', self: 'Q', args: [[]], as: 'B' }, { op: 'project.toJSON', self: 'B' });
    return [{ calls }];
  },
  // test-civilcraft-traverse.mjs
  't-traverse'(rnd, seed) {
    const M = mv(seed), SEC = 1 / 3600;
    const calls = [{ op: 'tv.parse', args: ['270°00\'00" 100 B\n270-00-10 100.00 feet C\n270 00 10 100, D\n# comment\n270.0027778 100\nclose 270°00\'10"', 'angles'] }, { op: 'tv.parse', args: ['N 0 E 100 B\nN 90°00\'00" E 100\nAZ 180-00-00 100 D\nS 90 W 100', 'bearings'] }, { op: 'tv.parse', args: ['361 100\n270\nN 95 E 10\nclose 400', 'angles'] }];
    const legs = [{ angle: 270, dist: 100, name: 'B' }, { angle: 270 + 10 * SEC, dist: 100, name: 'C' }, { angle: 270 + 10 * SEC, dist: 100, name: 'D' }, { angle: 270 + 10 * SEC, dist: 100, name: '' }];
    const o0 = M({ x: 0, y: 0 });
    calls.push({ op: 'tv.run', args: [{ start: o0, mode: 'angles', backsight: 90, legs, close: 'loop', closingAngle: 270 + 10 * SEC }] });
    calls.push({ op: 'tv.run', args: [{ start: o0, mode: 'angles', backsight: 90, legs: legs.map((l, i) => ({ ...l, dist: i === 3 ? 99.96 : 100 })), close: 'loop', closingAngle: 270 + 10 * SEC }] }, { op: 'tv.run', args: [{ start: o0, mode: 'angles', backsight: 90, legs, close: 'loop' }] });
    calls.push({ op: 'tv.run', args: [{ start: M({ x: 500, y: 500 }), mode: 'bearings', legsText: 'N 0 E 100 B\nN 90 E 100 C\nS 0 E 100 D\nS 90 W 100.05', close: 'loop' }] });
    calls.push({ op: 'tv.run', args: [{ start: o0, mode: 'bearings', legsText: 'N 90 E 100\nN 90 E 100', close: M({ x: 200.1, y: 0.1 }) }] });
    calls.push({ op: 'tv.run', args: [{ start: o0, mode: 'bearings', legsText: 'N 0 E 100 B\nN 90 E 100 =C\nS 0 E 100 D\nS 90 W 99.96', close: 'loop' }], as: 'R' }, { op: 'tv.report', self: 'R', args: [{ units: 'ft' }] });
    return [{ calls }];
  },
  // test-civilcraft-undo.mjs
  't-undo'(rnd, seed) {
    const fx = fixture(seed);
    const b = { minX: fx.E - 15, minY: fx.N - 12, maxX: fx.E + 412, maxY: fx.N + 309 };
    const pts = [], faces = [];
    for (let j = 0; j < 3; j++) for (let i = 0; i < 3; i++) pts.push({ x: b.minX + ((b.maxX - b.minX) * i) / 2, y: b.minY + ((b.maxY - b.minY) * j) / 2, z: 500 });
    for (let j = 0; j < 2; j++) for (let i = 0; i < 2; i++) { const a = j * 3 + i; faces.push([a, a + 1, a + 4], [a, a + 4, a + 3]); }
    const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site'] }, { op: 'project.eg', self: 'P' }];
    calls.push({ op: 'project.setTin', self: 'P', args: ['Surface from dem.tif', { pts, faces, from: 'grid' }, [{ set: ['name'], value: 'From the grid' }]] }, { op: 'project.eg', self: 'P' }, { op: 'project.change', self: 'P', args: ['Rename', [{ set: ['surface', 'name'], value: 'EG 2' }]] });
    calls.push({ op: 'project.undo', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.eg', self: 'P' }, { op: 'project.redo', self: 'P' }, { op: 'project.redo', self: 'P' }, { op: 'project.view', self: 'P' });
    calls.push({ op: 'project.setTin', self: 'P', args: ['Rebuild the surface from the points', null, null] }, { op: 'project.eg', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.editPoints', self: 'P', args: ['Delete one', { remove: [fx.points[0].n] }] }, { op: 'project.undo', self: 'P' }, { op: 'project.undo', self: 'P' }, { op: 'project.toJSON', self: 'P' }, { op: 'project.view', self: 'P' });
    return [{ calls }];
  },
  // test-civilcraft-why.mjs
  't-why'(rnd, seed) {
    const M = mv(seed);
    const withR = (pis, rs) => pis.map((p, i) => M({ ...p, r: rs[i] ?? 0 }));
    const Z = [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }, { x: 200, y: 100 }];
    const calls = [{ op: 'why.curve', args: [withR(Z, [0, 80, 80, 0])] }, { op: 'why.curve', args: [withR([{ x: 0, y: 0 }, { x: 50, y: 0 }, { x: 50, y: 500 }], [0, 200, 0])] }, { op: 'why.curve', args: [withR(Z, [0, 200, 30, 0])] }, { op: 'why.curve', args: [withR([{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 40, y: 0 }], [0, 30, 0])] }];
    calls.push({ op: 'why.curve', args: [withR([{ x: 0, y: 0 }, { x: 500, y: 0 }, { x: 1000, y: 500 }], [0, 100, 0]), { minR: 150 }] }, { op: 'why.curve', args: [withR([{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }], [0, 50, 0]), { minR: 150 }] });
    for (let i = 1; i < 3; i++) calls.push({ op: 'why.maxRadius', args: [withR(Z, [0, 80, 80, 0]), i] });
    calls.push({ op: 'why.vertical', args: [[{ sta: 0, z: 100 }, { sta: 100, z: 102, L: 300 }, { sta: 200, z: 99 }]] }, { op: 'why.vertical', args: [[{ sta: 0, z: 100 }, { sta: 100, z: 104, L: 120 }, { sta: 180, z: 103, L: 100 }, { sta: 400, z: 110 }]] }, { op: 'why.vertical', args: [[{ sta: 0, z: 100 }, { sta: 100, z: 102, K: 60 }, { sta: 200, z: 99 }]] });
    return [{ calls }];
  },
});

const MAPJPG = fs.readFileSync('/var/www/CADCRAFT/getcadcraft.com/tools/civilcraft-test/fixtures/map-8x6.jpg').toString('base64');
Object.assign(GENS, {
  // test-civilcraft-3d.mjs: the fixture site in 3D
  't-3d'(rnd, seed) {
    const fx = fixture(seed), { E, N } = fx;
    const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site-topo.csv'] }];
    calls.push({ op: 'project.change', self: 'P', args: ['Breaklines from groups', [...['FL', 'TBN', 'TBS', 'RIDGE'].map((k) => ({ set: ['surface', 'groups', k], value: 'breakline' })), { set: ['surface', 'groups', 'CP'], value: 'off' }]] });
    const pis = [{ x: E + 10, y: N + 260, r: 0 }, { x: E + 200, y: N + 230, r: 120 }, { x: E + 300, y: N + 40, r: 90 }, { x: E + 390, y: N + 30, r: 0 }];
    calls.push({ op: 'project.change', self: 'P', args: ['Alignment', [{ push: ['alignments'], value: { id: 'a1', name: 'Access road', start: 1000, pis, pvis: [{ sta: 1000, z: 252.5, L: 0 }, { sta: 1300, z: 256, L: 120 }, { sta: 1580, z: 248.3, L: 0 }], corridor: { on: true, lane: 12, laneSlope: 0.02, shoulder: 4, shoulderSlope: 0.04, cut: 2, fill: 3, step: 10 } } }]] });
    calls.push({ op: 'project.change', self: 'P', args: ['Pad', [{ push: ['pads'], value: { id: 'p1', name: 'Pad A', pts: [{ x: E + 60, y: N + 120 }, { x: E + 120, y: N + 120 }, { x: E + 120, y: N + 170 }, { x: E + 60, y: N + 170 }], z: 252, cut: 2, fill: 2 } }]] });
    const net = { name: 'Storm', type: 'storm', rules: { minSlope: 0.005, minCover: 2, minDrop: 0.1 }, structures: [{ id: 's1', name: 'S1', x: E + 250, y: N + 150, rim: null, sumpDepth: 0.5, dia: 48 }, { id: 's2', name: 'S2', x: E + 170, y: N + 110, rim: null, sumpDepth: 0, dia: 48 }, { id: 's3', name: 'S3', x: E + 60, y: N + 70, rim: null, sumpDepth: 1, dia: 60 }], pipes: [{ id: 'q1', name: 'P1', from: 's1', to: 's2', dia: 12, invUp: 244, invDn: 243.2 }, { id: 'q2', name: 'P2', from: 's2', to: 's3', dia: 18, invUp: 243, invDn: 242 }] };
    calls.push({ op: 'project.change', self: 'P', args: ['Pipes', [{ set: ['network'], value: net }]] }, { op: 's3.input', args: ['P'], as: 'I' });
    calls.push({ op: 's3.build', args: ['I', { exag: 1, color: 'single' }] }, { op: 's3.build', args: ['I', { exag: 3, color: 'single' }] }, { op: 's3.build', args: ['I', { exag: 2, color: 'bands', bandCount: 8, show: { fg: false } }] }, { op: 's3.build', args: ['I', { exag: 2, show: { fg: false, triangles: true } }] });
    calls.push({ op: 's3.build', args: ['I', { show: { contours: false, breaklines: false, alignments: false, corridors: false, pads: false, boundary: false, pipes: false } }] }, { op: 's3.input', args: ['P', { noProfile: true }], as: 'IN' }, { op: 's3.build', args: ['IN', { show: { fg: false } }] });
    calls.push({ op: 's3.input', args: ['P', { network: { ...net, pipes: [{ ...net.pipes[0], invUp: 242.7 }, net.pipes[1]] } }], as: 'IB' }, { op: 's3.build', args: ['IB', {}] }, { op: 's3.input', args: ['P', { network: { ...net, pipes: [{ ...net.pipes[0], invUp: null, invDn: null }, net.pipes[1]] } }], as: 'II' }, { op: 's3.build', args: ['II', {}] });
    calls.push({ op: 's3.build', args: ['I', { exag: 2, show: { points: true } }], as: 'SP' });
    for (let i = 0; i < 30; i++) calls.push({ op: 's3.pick', args: ['SP', { w: 1000, h: 700, view: 'iso' }, 100 + i * 27, 120 + (i * 53) % 460] });
    calls.push({ op: 's3.build', args: ['I', { exag: 2, show: { fg: false, pipes: false } }], as: 'NP' }, { op: 's3.pick', args: ['NP', { w: 800, h: 600, view: 'top', persp: false }, 400, 300] });
    calls.push({ op: 's3.build', args: ['I', { show: { eg: false, fg: false, contours: false, alignments: false, corridors: false, pads: false, breaklines: false } }], as: 'PO' }, { op: 's3.pick', args: ['PO', { w: 800, h: 600, view: 'top', persp: false }, 400, 300] }, { op: 's3.pick', args: ['PO', { w: 800, h: 600, view: 'top', persp: false }, 2, 2] });
    calls.push({ op: 's3.declutter', args: ['SP', { w: 1000, h: 700, view: 'iso' }] });
    calls.push({ op: 's3.build', args: ['I', { exag: 3, color: 'bands', show: { points: true, triangles: false } }], as: 'G' }, { op: 'glb.export', args: ['G', { name: 'Fixture site' }] }, { op: 'obj.export', args: ['G', { name: 'Fixture site' }] });
    return [{ calls }];
  },
  // test-civilcraft-agent.mjs: the tools on the sample site
  't-agent'(rnd, seed) {
    const fx = fixture(seed);
    const xs = fx.points.map((q) => q.x), ys = fx.points.map((q) => q.y);
    const box = { minX: Math.min(...xs), minY: Math.min(...ys), maxX: Math.max(...xs), maxY: Math.max(...ys) };
    const mid = { x: (box.minX + box.maxX) / 2, y: (box.minY + box.maxY) / 2 };
    const calls = [{ op: 'project.new', as: 'P' }, { op: 'project.setPoints', self: 'P', args: [fx.points, 'site-topo.csv'] }, { op: 'project.change', self: 'P', args: ['n', [{ set: ['name'], value: 'Sample site' }, { push: ['alignments'], value: { id: 'a1', name: 'Main Street', start: 1000, pis: [{ x: box.minX + 10, y: mid.y }, { x: box.maxX - 10, y: mid.y }], pvis: [] } }]] }];
    const R = (t, a) => calls.push({ op: 'agent.run', args: ['P', t, a] });
    R('site_describe', {}); R('points_list', { group: 'tree' }); R('points_list', { description: 'ridge' }); R('points_list', { within: { minX: box.minX, minY: box.minY, maxX: mid.x, maxY: box.maxY }, limit: 1000 }); R('points_list', { offset: 270, limit: 10 });
    const q0 = fx.points[100];
    R('surface_elevation', { points: [{ x: q0.x, y: q0.y }, { x: box.maxX + 1000, y: box.maxY + 1000 }] }); R('surface_elevation', { points: [mid], surface: 'graded' });
    R('alignment_station', { points: [{ x: box.minX + 10, y: mid.y + 5 }, { x: mid.x, y: mid.y - 3 }] }); R('alignment_station', { alignment: 'main street', stations: [1000, 1000 + box.maxX - box.minX - 20, 1050 + box.maxX - box.minX] });
    R('alignment_station', { alignment: 'Side Road', stations: [1000] }); R('alignment_station', {});
    R('points_select', { group: 'TREE' }); R('points_select', { numbers: fx.points.slice(0, 3).map((q) => q.n), mode: 'add' }); R('points_select', { numbers: [fx.points[0].n, 'NOPE-1'] }); R('points_select', {}); R('points_select', { within: { minX: box.minX, minY: box.minY, maxX: mid.x, maxY: box.maxY }, mode: 'remove' });
    calls.push({ op: 'project.view', self: 'P' });
    return [{ calls }];
  },
  // test-civilcraft-mapout.mjs
  't-mapout'(rnd, seed) {
    const M = mv(seed);
    const scale = { label: '1" = 40\'', mmPerUnit: 25.4 / 40 };
    const ext = { minX: 1000, minY: 2000, maxX: 1400, maxY: 2250 };
    const e2 = { ...M({ x: ext.minX, y: ext.minY }) }, e3 = M({ x: ext.maxX, y: ext.maxY });
    const E = { minX: e2.x, minY: e2.y, maxX: e3.x, maxY: e3.y };
    const calls = [{ op: 'plot.scalesFor', args: ['ft'] }, { op: 'plot.sheetWindow', args: [E, { paper: 'tabloid', scale }] }];
    calls.push({ op: 'alignment.build', args: [{ pis: [M({ x: 1000, y: 2000 }), M({ x: 1400, y: 2250 })] }], as: 'A' }, { op: 'scene.plan', args: [{ units: 'ft', alignments: [{ name: '', alignment: 'A', pis: [] }] }, { mmPerUnit: scale.mmPerUnit }], as: 'SC' });
    const win = { minX: E.minX - 3.9, minY: E.minY - 63.7, maxX: E.maxX + 3.9, maxY: E.maxY + 63.7 };
    calls.push({ op: 'plot.planSheet', args: ['SC', { extents: E, paper: 'tabloid', scale, title: 'Map test', units: 'ft', images: [{ jpeg: MAPJPG, pxW: 8, pxH: 6, box: win }] }], as: 'SH' }, { op: 'plot.toPdf', args: ['SH', {}] }, { op: 'plot.toPdfPages', args: ['SH', {}] });
    calls.push({ op: 'plot.planSheet', args: ['SC', { extents: E, paper: 'tabloid', scale, title: 'No map', units: 'ft' }], as: 'S2' }, { op: 'plot.toPdf', args: ['S2', {}] });
    const box = { minX: 3115000, minY: 10070000, maxX: 3115400, maxY: 10070300 };
    for (const v of ['AC1015', 'AC1032']) calls.push({ op: 'dwg.write', args: [{ units: 'ft', layers: [{ name: 'C-TOPO-MAJR', color: 9 }, { name: 'G-MAP-IMAG', color: 8 }], entities: [{ type: 'poly', layer: 'C-TOPO-MAJR', pts: [{ x: box.minX, y: box.minY }, { x: box.maxX, y: box.maxY }], elev: 100 }], version: v, created: 0, images: [{ fileName: 'site-map.png', pxW: 800, pxH: 600, box, layer: 'G-MAP-IMAG' }] }] });
    return [{ calls }];
  },
});
