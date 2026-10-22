#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes tests/golden/skp/: cc-skp's hand-made test files (the builders of CadCraft 0.5's tools/skp-test/test.mjs,
// copied below unchanged) and what the frozen 0.5 readSkp ($FORK_PARITY_JS/lib/skp) makes of each — its model as JSON
// (bytes as {"$base64": …}) or its error {code, message}. libs/io-skp's test reads every file and compares.
// Synthetic bytes only: no corpus file goes into the repo.
//   node tools/gen/skp-fixtures.mjs
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

const root = process.env.FORK_PARITY_JS || join(process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles', 'fork-parity/js-0.5');
const lib = (p) => import(pathToFileURL(join(root, 'lib/skp', p)).href);
const { readSkp } = await lib('read.js');
const { LIMITS } = await lib('util.js');
const { crc32 } = await lib('zip.js');
const OUT = fileURLToPath(new URL('../../tests/golden/skp/', import.meta.url));
mkdirSync(OUT, { recursive: true });

// ---- the builders of tools/skp-test/test.mjs (0.5) ----
const enc = new TextEncoder();
const cat = (...parts) => {
  const norm = (p) => p instanceof Uint8Array ? p : typeof p === 'string' ? enc.encode(p)
    : p.every((x) => typeof x === 'number') ? Uint8Array.from(p) : cat(...p);
  const arrs = parts.map(norm);
  const out = new Uint8Array(arrs.reduce((n, a) => n + a.length, 0));
  let o = 0;
  for (const a of arrs) { out.set(a, o); o += a.length; }
  return out;
};
const u16 = (v) => new Uint8Array([v & 255, (v >> 8) & 255]);
const u32 = (v) => new Uint8Array([v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
const f64 = (...vs) => { const b = new Uint8Array(8 * vs.length); const dv = new DataView(b.buffer); vs.forEach((v, i) => dv.setFloat64(i * 8, v, true)); return b; };
const idBytes = (v) => { const b = []; do { b.push(v & 255); v = Math.floor(v / 256); } while (v > 0); return new Uint8Array(b); };
/** One record: tag as its two file bytes (0xC409 → C4 09), u32 length, payload. */
const rec = (tag, ...payload) => { const p = cat(...payload); return cat(new Uint8Array([tag >> 8, tag & 255]), u32(p.length), p); };
const idRec = (id) => rec(0xdc05, rec(0xde05, idBytes(id)));
const info = (id, { mat, layer, flags = 6 } = {}) => rec(0xd007, idRec(id), mat !== undefined ? rec(0xd107, idBytes(mat)) : [], layer !== undefined ? rec(0xd207, idBytes(layer)) : [], rec(0xd307, [flags]));
const strRec = (s) => cat([0xff, 0xfe, 0xff, s.length], ...[...s].map((c) => u16(c.charCodeAt(0))));

/** Minimal ZIP writer: entries [{name, data, method (0|8), flags}] → bytes. */
async function makeZip(entries) {
  const locals = [], centrals = [];
  let off = 0;
  for (const e of entries) {
    const name = enc.encode(e.name);
    const method = e.method ?? 0;
    const data = method === 8 ? await deflateRaw(e.data) : (e.stored ?? e.data);
    const crc = e.crc ?? crc32(e.data);
    const lh = cat(u32(0x04034b50), u16(20), u16(e.flags ?? 0), u16(method), u16(0), u16(0), u32(crc), u32(data.length), u32(e.data.length), u16(name.length), u16(0), name);
    centrals.push(cat(u32(0x02014b50), u16(20), u16(20), u16(e.flags ?? 0), u16(method), u16(0), u16(0), u32(crc), u32(data.length), u32(e.usize ?? e.data.length), u16(name.length), u16(0), u16(0), u16(0), u16(0), u32(0), u32(off), name));
    locals.push(lh, data);
    off += lh.length + data.length;
  }
  const cd = cat(...centrals);
  return cat(...locals, cd, u32(0x06054b50), u16(0), u16(0), u16(entries.length), u16(entries.length), u32(cd.length), u32(off), u16(0));
}
async function deflateRaw(data) {
  const cs = new CompressionStream('deflate-raw');
  const w = cs.writable.getWriter(); w.write(data); w.close();
  return new Uint8Array(await new Response(cs.readable).arrayBuffer());
}
const vffHeader = (version = '25.0.575') => cat(strRec('SketchUp Model'), strRec('{' + version + '}'), 'VFF', [8, 0, 1, 0, 0x11, 0, 0x3f, 0x71, 0x52, 0x7c]);

const MAT_XML = (name, r, g, b, tex) => '<?xml version="1.0"?><materialDocument><mat:material name="' + name + '" colorRed="' + r + '" colorGreen="' + g + '" colorBlue="' + b + '" trans="0.25" useTrans="1">' +
  (tex ? '<mat:texture textureFilename="C:\\x\\' + tex + '" xScale="10" yScale="20"><mat:images><mat:image id="1" path="./' + tex + '" file_name="' + tex + '"/></mat:images></mat:texture>' : '') + '</mat:material></materialDocument>';
const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13]);

/**
 * The known-geometry model:
 *   root: square face 0..10 × 0..10 at z = 0 with a 4..6 hole, front material 1 (textured), back material 2, on tag 3;
 *         one loose hidden+soft edge (20,0,0)–(20,5,0); one instance of definition D: rotate 90° about Z, scale × 2
 *         (w = 0.5), move (100, 0, 0), painted material 2, tag 3, hidden, named "Inst".
 *   D (component, id 500): unit triangle (0,0,0) (1,0,0) (0,1,0) whose two edges along the axes are smooth.
 */
function knownModel({ version, extraDefs = [], rootInsts = null, vertsOverride = null } = {}) {
  let id = 1000;
  const next = () => id++;
  const verts = [], edges = [], vid = new Map();
  const V = (x, y, z) => { const k = x + ',' + y + ',' + z; if (!vid.has(k)) { const i = next(); vid.set(k, i); verts.push(rec(0xc409, idRec(i), rec(0xc509, f64(x, y, z)))); } return vid.get(k); };
  const E = (a, b, flags = 6) => { const i = next(); edges.push(rec(0xb80b, info(i, { flags }), rec(0xb90b, idBytes(a)), rec(0xba0b, idBytes(b)))); return i; };
  const loopRec = (ids, rev) => rec(0x9411, rec(0x9511, ...ids.map((e, k) => rec(0xa00f, rec(0xa10f, idBytes(e)), rec(0xa20f, [rev[k] ? 1 : 0])))));
  // Outer square, counter-clockwise seen from +Z; edges stored one way, used forward.
  const sq = [V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)];
  const se = sq.map((v, i) => E(v, sq[(i + 1) % 4]));
  // Hole stored counter-clockwise too; the loop uses its edges reversed (so it runs clockwise).
  const hl = [V(4, 4, 0), V(6, 4, 0), V(6, 6, 0), V(4, 6, 0)];
  const he = hl.map((v, i) => E(v, hl[(i + 1) % 4], 6 | 0x08));
  const holeIds = [he[3], he[2], he[1], he[0]];
  const face = rec(0xac0d, info(next(), { mat: 1, layer: 3 }), rec(0xad0d, f64(0, 0, 1, 0)),
    rec(0xae0d, loopRec(se, [0, 0, 0, 0]), loopRec(holeIds, [1, 1, 1, 1])), rec(0xaf0d, idBytes(2)));
  E(V(20, 0, 0), V(20, 5, 0), 6 | 0x01 | 0x08);
  const inst = rec(0x8c13, rec(0x6419, info(next(), { mat: 2, layer: 3, flags: 7 }), rec(0x6519, 'Inst'),
    rec(0x6619, f64(0, -1, 0, 1, 0, 0, 0, 0, 1, 50, 0, 0, 0.5)), rec(0x6719, idBytes(500)), rec(0x6819, new Uint8Array(16))));
  const rootEnts = rec(0x8813, info(next()), rec(0x8913, ...(vertsOverride || verts)), rec(0x8a13, ...edges), rec(0x8b13, face), rootInsts ?? inst);
  // Definition D.
  const dv = [], de = [];
  const dV = (x, y, z) => { const i = next(); dv.push(rec(0xc409, idRec(i), rec(0xc509, f64(x, y, z)))); return i; };
  const dE = (a, b, flags) => { const i = next(); de.push(rec(0xb80b, info(i, { flags }), rec(0xb90b, idBytes(a)), rec(0xba0b, idBytes(b)))); return i; };
  const t = [dV(0, 0, 0), dV(1, 0, 0), dV(0, 1, 0)];
  const te = [dE(t[0], t[1], 6 | 0x10), dE(t[1], t[2], 6), dE(t[2], t[0], 6 | 0x10)];
  const dFace = rec(0xac0d, info(next()), rec(0xad0d, f64(0, 0, 1, 0)), rec(0xae0d, loopRec(te, [0, 0, 0])));
  const defD = rec(0x7c15, rec(0x8813, info(500), rec(0x8913, ...dv), rec(0x8a13, ...de), rec(0x8b13, dFace)),
    rec(0x7d15, new Uint8Array([0x1b, 0xf0, 0xcc, 0xa1, 0xcf, 0x72, 0x5a, 0x4a, 0x86, 0xd6, 0xa4, 0x2d, 0x71, 0xd5, 0xb3, 0xa4])),
    rec(0x7e15, 'Triangle'), rec(0x8315, u32(0)));
  const mats = rec(0xf701, rec(0xd430, rec(0xd530,
    rec(0xc832, idRec(1), rec(0xcc32, 'Brick')), rec(0xc832, idRec(2), rec(0xcc32, 'Red')))));
  const layerRec = (lid, name, hidden) => rec(0x8c3c, idRec(lid), rec(0x8d3c, name), rec(0x8e3c, [hidden ? 1 : 0]), rec(0x8f3c, rec(0xc832, idRec(lid + 100), rec(0xcc32, 'Layer_' + name))));
  const layers = rec(0xf801, rec(0x983a, rec(0x993a, layerRec(3, 'Walls', true), layerRec(4, 'Layer0', false))));
  const defs = rec(0xf901, rec(0x7017, rec(0x7117, defD, ...extraDefs)));
  const md = rec(0xf401, mats, layers, defs, rec(0xf601, rootEnts));
  const meta = rec(0x6400, rec(0x6d00, 'Millimeter'));
  return async (overrides = {}) => cat(vffHeader(version), await makeZip(overrides.entries || [
    { name: 'meta/model_thumbnail.png', data: PNG },
    { name: 'meta/meta.dat', data: meta },
    { name: 'materials/Brick/material.xml', data: enc.encode(MAT_XML('Brick', 200, 100, 50, 'brick.png')), method: 8 },
    { name: 'materials/Brick/brick.png', data: PNG },
    { name: 'materials/Red/material.xml', data: enc.encode(MAT_XML('Red', 255, 0, 0)) },
    { name: 'materials/Layer_Walls/material.xml', data: enc.encode(MAT_XML('Layer_Walls', 1, 2, 3)) },
    { name: 'model.dat', data: md, method: 8 },
  ]));
}


// ---- the files ----
const files = [];
const add = (name, bytes) => files.push([name, bytes]);
const build = knownModel();
add('known', await build());
const ghost = rec(0x8c13, rec(0x6419, info(9200), rec(0x6619, f64(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)), rec(0x6719, idBytes(4242))));
add('ghost-instance', await knownModel({ rootInsts: ghost })());
add('cut-in-magic', vffHeader().subarray(0, 20));
add('empty', new Uint8Array(0));
add('a-zip', enc.encode('PK\x03\x04 hello'));
add('wrong-magic', cat(strRec('Sketchup Modex'), strRec('{25.0.1}')));
add('version-no-braces', cat(strRec('SketchUp Model'), strRec('25.0.1')));
add('vff-no-archive', cat(vffHeader(), 'nothing here'));
const loop = rec(0x8c13, rec(0x6419, info(9000), rec(0x6619, f64(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)), rec(0x6719, idBytes(600))));
const selfDef = rec(0x7c15, rec(0x8813, info(600), loop), rec(0x7e15, 'Self'), rec(0x8315, u32(1)));
add('self-nested', await knownModel({ extraDefs: [selfDef] })());
const chain = [];
for (let i = 0; i < LIMITS.depth + 2; i++) {
  const child = i + 1 < LIMITS.depth + 2 ? rec(0x8c13, rec(0x6419, info(20000 + i), rec(0x6619, f64(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)), rec(0x6719, idBytes(700 + i + 1)))) : [];
  chain.push(rec(0x7c15, rec(0x8813, info(700 + i), child), rec(0x7e15, 'L' + i)));
}
add('too-deep', await knownModel({ extraDefs: chain })());
add('nan-vertex', await knownModel({ vertsOverride: [rec(0xc409, idRec(1), rec(0xc509, f64(NaN, 0, 0)))] })());
const badInst = rec(0x8c13, rec(0x6419, info(9100), rec(0x6619, f64(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0)), rec(0x6719, idBytes(500))));
add('placement-w0', await knownModel({ rootInsts: badInst })());
add('record-overrun', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: cat(rec(0xf401, rec(0xf601, new Uint8Array(3))), new Uint8Array([0xf6, 0x01, 255, 255, 0, 0])) }])));
add('too-new', cat(vffHeader('99.0.1'), await makeZip([{ name: 'model.dat', data: new Uint8Array([1, 2, 3]) }])));
const data = enc.encode('hello hello hello hello zip');
add('no-model-dat', cat(vffHeader(), await makeZip([{ name: 'other.dat', data }])));
// The ZIP units of test.mjs, as files: model.dat with each fault.
const md = new Uint8Array([0xf4, 0x01, 0, 0, 0, 0]);
add('zip-bad-crc', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: md, crc: 1234 }])));
add('zip-encrypted', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: md, flags: 1 }])));
add('zip-bzip2', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: md, method: 12, stored: md }])));
const z = await makeZip([{ name: 'model.dat', data: md }]);
add('zip64-locator', cat(vffHeader(), z.subarray(0, z.length - 22), u32(0x07064b50), new Uint8Array(16), z.subarray(z.length - 22)));
const zFF = z.slice(); zFF.set(u32(0xffffffff), zFF.length - 22 + 16);
add('zip-ffff-offset', cat(vffHeader(), zFF));
add('zip-cut-end', cat(vffHeader(), z.subarray(0, z.length - 5)));
add('zip-entry-too-big', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: md, usize: LIMITS.entryBytes + 1 }])));
add('zip-inflates-past', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: cat(md, new Uint8Array(64)), method: 8, usize: 5 }])));
const zBroken = await makeZip([{ name: 'model.dat', data: cat(md, new Uint8Array(64)), method: 8 }]);
zBroken[30 + 9 + 2] ^= 0xff;
add('zip-broken-deflate', cat(vffHeader(), zBroken));
add('no-geometry', cat(vffHeader(), await makeZip([{ name: 'model.dat', data: md }])));
// a few truncations of the known model (each a refusal or a model, as the JS says)
const known = await build();
for (const cut of [60, 100, 200, Math.floor(known.length / 2), known.length - 22, known.length - 1]) add('known-cut-' + cut, known.subarray(0, cut));

const b64 = (u8) => Buffer.from(u8).toString('base64');
const plain = (v) => JSON.parse(JSON.stringify(v, (k, x) => (x instanceof Uint8Array ? { $base64: b64(x) } : x)));
const index = [];
for (const [name, bytes] of files) {
  let result;
  try { result = { value: plain(await readSkp(bytes)) }; } catch (e) { result = { error: { code: e.code || e.name, message: e.message } }; }
  writeFileSync(join(OUT, name + '.skp'), bytes);
  writeFileSync(join(OUT, name + '.json'), JSON.stringify(result) + '\n');
  index.push(name);
}
writeFileSync(join(OUT, 'index.json'), JSON.stringify(index, null, 1) + '\n');
console.log(index.length + ' fixtures');
