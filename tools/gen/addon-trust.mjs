#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes tests/golden/js-0.5-addon-trust.json: CadCraft 0.5's addons/trust.js (frozen js-0.5) on test packages signed
// with a test key (Node's Ed25519, seed 0x07 × 32): each package as the directory serves it, readPackageJson's answer,
// packageDigest's text, verifyPackage's verdict, and canonical() of a few values. libs/addons' test replays them.
//   node tools/gen/addon-trust.mjs
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import crypto from 'node:crypto';

const root = process.env.FORK_PARITY_JS || join(process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles', 'fork-parity/js-0.5');
const T = await import(pathToFileURL(join(root, 'site/public_html/assets/apps/cadcraft/dev/addons/trust.js')).href);

// a test key pair from a fixed seed (PKCS#8 wrapping of the 32-byte seed)
const seed = Buffer.alloc(32, 7);
const priv = crypto.createPrivateKey({ key: Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), seed]), format: 'der', type: 'pkcs8' });
const pubDer = crypto.createPublicKey(priv).export({ format: 'der', type: 'spki' });
const pub = pubDer.subarray(pubDer.length - 32);
const keys = { 'test-1': pub.toString('base64') };
const enc = new TextEncoder();
const b64 = (u8) => Buffer.from(u8).toString('base64');

async function make(manifest, files, { key = 'test-1', tamper = null } = {}) {
  const f = Object.fromEntries(Object.entries(files).map(([p, s]) => [p, typeof s === 'string' ? enc.encode(s) : s]));
  const digest = await T.packageDigest({ manifest, files: f });
  let sig = crypto.sign(null, Buffer.from(digest, 'utf8'), priv);
  const served = { format: 'cadcraft-addon-package', version: 1, manifest, files: Object.fromEntries(Object.entries(f).map(([p, u]) => [p, b64(u)])), signature: { alg: 'Ed25519', key, sig: b64(sig) } };
  if (tamper) tamper(served);
  return served;
}
const man = { id: 'hvac.ducts', version: '1.2.0', name: 'Ducts', entry: 'main.js', apps: ['cadcraft'] };
const files = { 'addon.json': JSON.stringify(man), 'main.js': 'export default {};\n', 'data/sizes.json': '[1,2,3]' };
const cases = [
  ['good', await make(man, files)],
  ['manifest keys in another order', await make(man, { ...files, 'addon.json': JSON.stringify({ apps: ['cadcraft'], version: '1.2.0', id: 'hvac.ducts', entry: 'main.js', name: 'Ducts' }) })],
  ['a file changed after signing', await make(man, files, { tamper: (s) => { s.files['main.js'] = b64(enc.encode('alert(1)\n')); } })],
  ['a file added after signing', await make(man, files, { tamper: (s) => { s.files['extra.js'] = b64(enc.encode('x')); } })],
  ['the manifest changed', await make(man, files, { tamper: (s) => { s.manifest = { ...s.manifest, version: '9.9.9' }; } })],
  ['addon.json differs from the manifest', await make(man, { ...files, 'addon.json': JSON.stringify({ ...man, name: 'Other' }) })],
  ['no addon.json', await make(man, { 'main.js': 'x' })],
  ['an unknown key', await make(man, files, { key: 'nope' })],
  ['unsigned', await make(man, files, { tamper: (s) => { delete s.signature; } })],
  ['another algorithm', await make(man, files, { tamper: (s) => { s.signature.alg = 'RSA'; } })],
  ['a bad signature', await make(man, files, { tamper: (s) => { s.signature.sig = b64(new Uint8Array(64)); } })],
  ['a path climbing out', await make(man, files, { tamper: (s) => { s.files['../x.js'] = b64(enc.encode('x')); } })],
  ['a bad format', { ...(await make(man, files)), format: 'zip' }],
  ['version 2', { ...(await make(man, files)), version: 2 }],
  ['files not base64', await make(man, files, { tamper: (s) => { s.files['main.js'] = '***'; } })],
  ['base64 with whitespace and no padding', await make(man, files, { tamper: (s) => { s.files['data/sizes.json'] = 'WzEs MiwzXQ'; } })],
];
const out = { about: 'CadCraft 0.5 addons/trust.js (frozen js-0.5) on tools/gen/addon-trust.mjs', keys, cases: [] };
for (const [name, served] of cases) {
  const read = T.readPackageJson(served);
  let digest = null;
  if (read.ok) { try { digest = await T.packageDigest(read.pkg); } catch { digest = null; } }
  const verdict = read.ok ? await T.verifyPackage(read.pkg, { keys }) : null;
  out.cases.push({ name, served, read: { ok: read.ok, reason: read.reason ?? null }, digest, verdict });
}
out.canonical = [{ b: 1, a: [true, null, 'x', { d: 2.5, c: -0 }] }, 'é ', [], {}, { 10: 1, 9: 2, b: 3 }].map((v) => ({ value: v, text: T.canonical(v) }));
writeFileSync(new URL('../../tests/golden/js-0.5-addon-trust.json', import.meta.url), JSON.stringify(out, null, 1) + '\n');
console.log(out.cases.map((c) => c.name + ': ' + (c.verdict ? (c.verdict.ok ? 'ok' : c.verdict.reason) : 'read ' + c.read.reason)).join('\n'));
