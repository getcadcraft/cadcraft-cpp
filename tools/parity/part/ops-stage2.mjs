// SPDX-License-Identifier: Apache-2.0
// pcdiff/ops-stage2.mjs — JS ops for cc-part stage 2 (M10.2): the .pcscript reader/writer (script.js), parameter uses
// (params-uses.js), the sketch tools (sketch-tools.js), curve intersection (sketch-intersect.js) and vec.js.
// Mirrored by crates/part/src/script2.rs.
import { DEV, S } from './driver.mjs';

const SC = await import(DEV + 'script.js');
const PU = await import(DEV + 'params-uses.js');
const ST = await import(DEV + 'sketch-tools.js');
const SI = await import(DEV + 'sketch-intersect.js');
const V = await import(DEV + 'vec.js');

const clone = (v) => (v === undefined ? undefined : structuredClone(v));
const pieceOf = (x) => (x ? { a: x.a, b: x.b, hi: x.hi, period: x.period } : null);
const cv = (sk, id) => SI.curveOf(sk, S.byId(sk, id));

export const OPS = {
  'pcscript.read': (text, files) => {
    const readFile = files ? (p) => { if (Object.hasOwn(files, p)) return files[p]; throw new Error('no such file'); } : null;
    try { return SC.scriptToPart(text, { readFile }); }
    catch (e) { if (e instanceof SC.ScriptError) return { message: e.message, errors: e.errors }; throw e; }
  },
  'pcscript.write': (part, o) => SC.partToScript(part, o || {}),
  'pcscript.looks': (text) => SC.looksLikeScript(text),
  'uses.scriptParams': (text) => PU.scriptParams(text),
  'uses.paramUses': (part) => Object.fromEntries([...PU.paramUses(part)].map(([k, u]) => [k, { ...u, count: PU.usesCount(u) }])),
  'tools.crossings': (sk, id) => { const r = ST.crossings(sk, S.byId(sk, id)); return { cuts: r.cuts.map((c) => ({ t: c.t, p: c.p, by: c.by.id })), range: r.range }; },
  'tools.trimPiece': (sk, id, at) => pieceOf(ST.trimPiece(sk, id, at)),
  'tools.trim': (sk, id, at) => { sk = clone(sk); const res = ST.trim(sk, id, at); return { res, sk }; },
  'tools.extend': (sk, id, at) => { sk = clone(sk); const res = ST.extend(sk, id, at); return { res, sk }; },
  'tools.chainOf': (sk, id, ids) => ST.chainOf(sk, id, ids ?? null),
  'tools.offsetSide': (sk, chain, p) => ST.offsetSide(sk, chain, p),
  'tools.offset': (sk, chain, d, link) => { sk = clone(sk); const res = ST.offset(sk, chain, d, { link: link ?? null }); return { res, sk }; },
  'tools.mirror': (sk, ids, axis) => { sk = clone(sk); const res = ST.mirror(sk, ids, axis); return { res, sk }; },
  'tools.patternLinear': (sk, ids, d, n) => { sk = clone(sk); const res = ST.patternLinear(sk, ids, d, n); return { res, sk }; },
  'tools.patternCircular': (sk, ids, c, n, ang) => { sk = clone(sk); const res = ST.patternCircular(sk, ids, c, n, ang ?? undefined); return { res, sk }; },
  'tools.addPolygon': (sk, c, v, n) => { sk = clone(sk); const res = ST.addPolygon(sk, c, v, n); return { res, sk }; },
  'tools.addSlot': (sk, a, b, w) => { sk = clone(sk); const res = ST.addSlot(sk, a, b, w); return { res, sk }; },
  'tools.projectEdge': (sk, frame, edge) => { sk = clone(sk); const res = ST.projectEdge(sk, frame, edge); return { res, sk }; },
  'isect.intersect': (sk, a, b, infA, infB) => SI.intersect(cv(sk, a), cv(sk, b), { infA: !!infA, infB: !!infB }),
  'isect.probe': (sk, id, ts, pts) => {
    const c = cv(sk, id);
    if (!c) return null;
    return { range: SI.rangeOf(c), size: SI.sizeOf(c), at: ts.map((t) => SI.pointOn(c, t)), tan: ts.map((t) => SI.tangentOn(c, t)), params: pts.map((p) => SI.paramOf(c, p)) };
  },
  'isect.intersectCurves': (A, B, infA, infB) => SI.intersect(A, B, { infA: !!infA, infB: !!infB }),
  'isect.normAng': (a) => SI.normAng(a),
  'vec.frameFor': (n, p) => V.frameFor(n, p),
  'vec.basePlanes': () => V.BASE_PLANES,
  'vec.rotationAbout': (p, d, deg) => V.rotationAbout(p, d, deg),
  'vec.reflection': (o, n) => V.reflection(o, n),
  'vec.matMul': (a, b) => V.matMul(a, b),
  'vec.applyMat': (m, p) => V.applyMat(m, p),
  'vec.applyDir': (m, d) => V.applyDir(m, d),
  'vec.frameMatrix': (f) => V.frameMatrix(f),
  'vec.toPlane': (f, p) => V.toPlane(f, p),
  'vec.toWorld': (f, u, v) => V.toWorld(f, u, v),
  'vec.flipFrame': (f) => V.flipFrame(f),
  'vec.solveDense': (A, b) => V.solveDense(A, b),
  'vec.fmt': (n, dp) => V.fmt(n, dp ?? undefined),
  'vec.norm': (a) => V.norm(a),
  'json.parseError': (s) => { try { JSON.parse(s); return null; } catch (e) { return e.message; } },
};
