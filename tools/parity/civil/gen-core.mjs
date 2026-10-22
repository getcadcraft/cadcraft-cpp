// SPDX-License-Identifier: Apache-2.0
// gen-core.mjs — call-script cases (from the Rust fork's cvdiff) for units.js, points.js, tin.js, alignment.js, profile.js.
export const R = (rnd, lo, hi) => lo + (hi - lo) * rnd();
export const I = (rnd, lo, hi) => Math.floor(R(rnd, lo, hi + 1));
export const pick = (rnd, list) => list[Math.floor(rnd() * list.length)];
export const chance = (rnd, p) => rnd() < p;

/** A site origin at survey magnitudes: UTM metres, state-plane feet, or small local. */
export function origin(rnd) {
  return pick(rnd, [[500000, 4000000], [2100000.5, 700000.25], [0, 0], [312345.678, 4876543.21], [-1000, 250]]);
}
/** A smooth terrain z(x, y) with a ridge and a swale. */
export function terrain(rnd, ox, oy, size) {
  const a = R(rnd, 80, 1200), b = R(rnd, -0.05, 0.05), c = R(rnd, -0.05, 0.05), k = R(rnd, 0.5, 4) / size, amp = R(rnd, 0, size * 0.05);
  return (x, y) => a + b * (x - ox) + c * (y - oy) + amp * Math.sin((x - ox) * k) * Math.cos((y - oy) * k * 0.7);
}
export function randPoints(rnd, n, opts = {}) {
  const [ox, oy] = opts.origin || origin(rnd), size = opts.size || R(rnd, 20, 3000), f = opts.f || terrain(rnd, ox, oy, size);
  const pts = [];
  const grid = chance(rnd, 0.25);
  const step = size / Math.max(2, Math.round(Math.sqrt(n)));
  for (let i = 0; i < n; i++) {
    let x, y;
    if (grid) { x = ox + (i % Math.round(Math.sqrt(n))) * step; y = oy + Math.floor(i / Math.round(Math.sqrt(n))) * step; }
    else { x = ox + R(rnd, 0, size); y = oy + R(rnd, 0, size); }
    if (opts.round) { x = Math.round(x * 1000) / 1000; y = Math.round(y * 1000) / 1000; }
    let z = Math.round(f(x, y) * 1000) / 1000;
    if (chance(rnd, 0.03)) z = null;
    pts.push({ n: String(i + 1), x, y, z, d: pick(rnd, ['GRD', 'GRD shot', 'EP', 'EP1 B', 'TC', 'BC', 'FL', 'TREE oak', '', 'grd', 'CL']) });
    if (chance(rnd, 0.03) && pts.length) { const q = pts[Math.floor(rnd() * pts.length)]; pts.push({ n: String(i + 1) + 'd', x: q.x + R(rnd, -5e-5, 5e-5), y: q.y + R(rnd, -5e-5, 5e-5), z: q.z === null ? 1 : q.z + (chance(rnd, 0.5) ? 0 : 0.5), d: 'DUP' }); }
  }
  return { pts, ox, oy, size, f };
}
export function randLine(rnd, ox, oy, size, f, k) {
  const pts = [];
  let x = ox + R(rnd, 0, size), y = oy + R(rnd, 0, size);
  for (let i = 0; i < k; i++) { pts.push({ x, y, z: Math.round(f(x, y) * 100) / 100 + (chance(rnd, 0.1) ? R(rnd, -2, 2) : 0) }); x += R(rnd, -size / 3, size / 3); y += R(rnd, -size / 3, size / 3); }
  return pts;
}
export function randPoly(rnd, ox, oy, size, k) {
  const cx = ox + size * R(rnd, 0.3, 0.7), cy = oy + size * R(rnd, 0.3, 0.7), out = [];
  for (let i = 0; i < k; i++) { const a = (i / k) * Math.PI * 2 + R(rnd, -0.2, 0.2), r = size * R(rnd, 0.15, 0.7); out.push({ x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) }); }
  return chance(rnd, 0.3) ? out.reverse() : out;
}

function surfaceCalls(rnd, S, ox, oy, size) {
  const calls = [];
  calls.push({ op: 'surface.stats', self: S });
  for (let i = 0; i < 25; i++) calls.push({ op: 'surface.elevationAt', self: S, args: [ox + R(rnd, -0.1, 1.1) * size, oy + R(rnd, -0.1, 1.1) * size, ...(chance(rnd, 0.3) ? [1e-6] : [])] });
  calls.push({ op: 'surface.contours', self: S, args: [pick(rnd, [0.5, 1, 2, 5, 0.25, 10]), ...(chance(rnd, 0.3) ? [{ base: R(rnd, -1, 1) }] : [])] });
  calls.push({ op: 'surface.bands', self: S, args: [[R(rnd, 50, 400), R(rnd, 400, 600), R(rnd, 600, 1300)].sort((a, b) => a - b)] });
  calls.push({ op: 'surface.slopes', self: S });
  calls.push({ op: 'surface.edges', self: S });
  calls.push({ op: 'surface.outline', self: S });
  calls.push({ op: 'surface.toFaces', self: S });
  calls.push({ op: 'surface.hull', self: S });
  return calls;
}

export const GENS = {
  units(rnd) {
    const calls = [];
    for (let i = 0; i < 60; i++) {
      const v = pick(rnd, [R(rnd, -500, 50000), Math.round(R(rnd, 0, 1e5)) / 100, 0, -0.004, 1234.565, 99.995, 999.9995, 1e21, NaN].map((x) => (Number.isNaN(x) ? '$NaN' : x)));
      const u = pick(rnd, ['ft', 'm', 'x']);
      calls.push({ op: 'units.formatStation', args: [v, u, ...(chance(rnd, 0.3) ? [I(rnd, 0, 4)] : [])] });
      calls.push({ op: 'units.fixed', args: [v, I(rnd, 0, 4)] });
      calls.push({ op: 'units.grouped', args: [v, I(rnd, 0, 3)] });
      calls.push({ op: 'units.pct', args: [typeof v === 'number' ? v / 1000 : v, I(rnd, 0, 3)] });
      calls.push({ op: 'units.volumeText', args: [v, u, I(rnd, 0, 2)] });
      calls.push({ op: 'units.areaText', args: [v, u] });
      calls.push({ op: 'units.bearing', args: [R(rnd, -10, 10)] });
      calls.push({ op: 'units.dms', args: [R(rnd, -400, 400)] });
    }
    for (const s of ['12+34.56', '1234.56', '-0+50', '+1+234.5', '1 + 2', '', 'abc', '12+', '+12', '.5', '5.', '1e3', '-1e-2', ' 7 ', '12+34.', '0x10'])
      for (const u of ['ft', 'm']) calls.push({ op: 'units.parseStation', args: [s, u] }, { op: 'units.isNum', args: [s] });
    return [{ calls }];
  },
  points(rnd) {
    const scripts = [];
    for (let k = 0; k < 6; k++) {
      const { pts } = randPoints(rnd, I(rnd, 1, 60));
      const fmt = pick(rnd, ['PNEZD', 'PENZD', 'PNEZ', 'NEZ', 'ENZD', 'NEZD']);
      const delim = pick(rnd, [',', '\t', ';', ' ', '  ']);
      const cols = (p) => fmt.split('').map((c) => (c === 'P' ? p.n : c === 'N' ? p.y.toFixed(3) : c === 'E' ? p.x.toFixed(3) : c === 'Z' ? (p.z === null ? '' : p.z.toFixed(2)) : p.d));
      const lines = [];
      if (chance(rnd, 0.4)) lines.push(pick(rnd, ['Point,Northing,Easting,Elevation,Description', 'pt n e z d', 'X,Y,Z', 'Name;Lat;Lon;Elev;Code', 'id\teasting\tnorthing\telev\tdesc', '# comment']));
      for (const p of pts) {
        let c = cols(p);
        if (chance(rnd, 0.05)) c = c.slice(0, 2);
        if (chance(rnd, 0.04)) c[1] = 'x';
        if (chance(rnd, 0.05) && delim === ',') c[c.length - 1] = '"' + c[c.length - 1] + ', quoted ""x"""';
        lines.push(c.join(delim));
        if (chance(rnd, 0.05)) lines.push(pick(rnd, ['', '; skip', '// skip', '#', '   ']));
      }
      if (chance(rnd, 0.1)) lines.push(lines[lines.length - 1] || '1,2,3');
      const text = (chance(rnd, 0.2) ? '﻿' : '') + lines.join(pick(rnd, ['\n', '\r\n', '\r']));
      const calls = [{ op: 'points.parsePoints', args: [text, pick(rnd, [undefined, { format: fmt }, { prefer: 'PENZD' }, { format: 'auto', prefer: 'PNEZD' }])].filter((x) => x !== undefined) }];
      calls.push({ op: 'points.pointGroups', args: [pts] });
      calls.push({ op: 'points.groupPoints', args: [pts, pick(rnd, ['GRD', 'EP', '(no description)', 'EP1'])] });
      calls.push({ op: 'points.writePoints', args: [pts, I(rnd, 0, 4)] });
      for (const d of ['GRD shot', '  ep1 b', '', 'ß x', 'Tree']) calls.push({ op: 'points.groupKey', args: [d] });
      const xs = pts.map((p) => p.x), ys = pts.map((p) => p.y);
      const x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
      for (const mode of ['window', 'crossing', 'fence']) {
        const path = []; for (let i = 0; i < I(rnd, 2, 7); i++) path.push({ x: R(rnd, x0, x1), y: R(rnd, y0, y1) });
        calls.push({ op: 'points.lassoPoints', args: [pts, path, mode, (x1 - x0) / 50] });
      }
      scripts.push({ calls });
    }
    return scripts;
  },
  tin(rnd) {
    const scripts = [];
    for (let k = 0; k < 5; k++) {
      const n = pick(rnd, [3, 4, 10, 60, 250, 900]);
      const { pts, ox, oy, size, f } = randPoints(rnd, n);
      const breaklines = [];
      for (let b = 0; b < I(rnd, 0, 4); b++) breaklines.push({ name: 'b' + b, pts: randLine(rnd, ox, oy, size, f, I(rnd, 2, 6)) });
      const boundary = chance(rnd, 0.4) ? randPoly(rnd, ox, oy, size, I(rnd, 3, 9)) : null;
      const maxEdge = chance(rnd, 0.3) ? size * R(rnd, 0.05, 0.5) : 0;
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts, breaklines, boundary, maxEdge }], as: 'S' }];
      calls.push(...surfaceCalls(rnd, 'S', ox, oy, size));
      calls.push({ op: 'tin.convexHull', args: [pts.map((p) => ({ x: p.x, y: p.y }))] }, { op: 'tin.clipConvex', args: [randPoly(rnd, ox, oy, size, 5), randPoly(rnd, ox, oy, size, 4)] });
      // the TIN back as faces
      scripts.push({ calls });
    }
    // a TIN from faces
    const { pts, ox, oy, size } = randPoints(rnd, 40);
    const P = pts.filter((p) => p.z !== null).map((p) => ({ x: p.x, y: p.y, z: p.z }));
    const faces = [];
    for (let i = 0; i < 60; i++) faces.push([I(rnd, 0, P.length - 1), I(rnd, 0, P.length - 1), chance(rnd, 0.05) ? 999 : I(rnd, 0, P.length - 1)]);
    scripts.push({ calls: [{ op: 'tin.surfaceFromFaces', args: [{ pts: [...P, null], faces }], as: 'F' }, ...surfaceCalls(rnd, 'F', ox, oy, size)] });
    return scripts;
  },
  alignment(rnd) {
    const scripts = [];
    for (let k = 0; k < 8; k++) {
      const [ox, oy] = origin(rnd), size = R(rnd, 100, 5000);
      const pis = [];
      let x = ox, y = oy, dir = R(rnd, -Math.PI, Math.PI);
      for (let i = 0; i < I(rnd, 2, 7); i++) {
        pis.push({ x, y, r: chance(rnd, 0.7) ? R(rnd, 20, size / 2) : 0 });
        dir += R(rnd, -1.6, 1.6); const L = R(rnd, size / 10, size / 2); x += L * Math.cos(dir); y += L * Math.sin(dir);
      }
      if (chance(rnd, 0.1)) pis.push({ ...pis[pis.length - 1] });
      const start = pick(rnd, [0, 1000, R(rnd, -100, 5000)]);
      const calls = [{ op: 'alignment.build', args: [{ pis, start }], as: 'A' }];
      for (let i = 0; i < 12; i++) calls.push({ op: 'al.pointAt', self: 'A', args: [start + R(rnd, -50, size * 2)] });
      for (let i = 0; i < 12; i++) calls.push({ op: 'al.stationOffset', self: 'A', args: [ox + R(rnd, -size, size), oy + R(rnd, -size, size)] });
      calls.push({ op: 'al.polyline', self: 'A', args: chance(rnd, 0.5) ? [R(rnd, 1, 50)] : [] });
      calls.push({ op: 'al.ticks', self: 'A', args: [pick(rnd, [50, 100, 20, 0])] });
      calls.push({ op: 'al.keyStations', self: 'A' });
      calls.push({ op: 'alignment.normAng', args: [R(rnd, -20, 20)] });
      scripts.push({ calls });
    }
    return scripts;
  },
  profile(rnd) {
    const scripts = [];
    for (let k = 0; k < 6; k++) {
      const { pts, ox, oy, size } = randPoints(rnd, 150, { size: 800 });
      const pis = [{ x: ox + 10, y: oy + 10, r: 0 }, { x: ox + size * 0.5, y: oy + size * R(rnd, 0.2, 0.8), r: R(rnd, 50, 300) }, { x: ox + size - 10, y: oy + size - 10, r: 0 }];
      const calls = [{ op: 'tin.buildSurface', args: [{ points: pts }], as: 'S' }, { op: 'alignment.build', args: [{ pis, start: 1000 }], as: 'A' }];
      calls.push({ op: 'profile.sampleSurface', args: ['S', 'A', ...(chance(rnd, 0.5) ? [{ step: R(rnd, 1, 30) }] : [])] });
      const pvis = []; let sta = 1000;
      for (let i = 0; i < I(rnd, 2, 6); i++) { pvis.push({ sta, z: R(rnd, 100, 200), ...(chance(rnd, 0.5) ? { L: pick(rnd, [0, R(rnd, 20, 300)]) } : chance(rnd, 0.5) ? { K: R(rnd, 10, 200) } : {}) }); sta += R(rnd, 50, 400); }
      if (chance(rnd, 0.2)) pvis.reverse();
      calls.push({ op: 'profile.build', args: [pvis], as: 'P' });
      for (let i = 0; i < 15; i++) { const s = R(rnd, 950, sta + 50); calls.push({ op: 'prof.elevationAt', self: 'P', args: [s] }, { op: 'prof.gradeAt', self: 'P', args: [s] }); }
      calls.push({ op: 'prof.sample', self: 'P', args: chance(rnd, 0.5) ? [R(rnd, 1, 20)] : [] });
      scripts.push({ calls });
    }
    return scripts;
  },
};
