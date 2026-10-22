// SPDX-License-Identifier: Apache-2.0
// Loads the FROZEN CivilCraft 0.5 modules in Node: the import map onto pinned vendor versions, i18n with en.json.
// (Adapted from the Rust fork's cvdiff harness, ours.)
import fs from 'node:fs';
import path from 'node:path';
import { registerHooks } from 'node:module';
import { pathToFileURL } from 'node:url';

process.env.TZ = 'UTC';   // writers that print local dates (LandXML, PDF info) see UTC, as the C++ side does

const testfiles = process.env.CADCRAFT_TESTFILES || '/var/www/CADCRAFT/testfiles';
const js05 = process.env.FORK_PARITY_JS || path.join(testfiles, 'fork-parity/js-0.5');
export const APP = path.join(js05, 'site/public_html/assets/apps/civilcraft/dev');
// the site's vendored libraries (the frozen copy has none): $CADCRAFT_VENDOR, else the site checkouts
const VENDOR = [process.env.CADCRAFT_VENDOR, process.env.CADCRAFT_SITE && path.join(process.env.CADCRAFT_SITE, 'public_html/assets/vendor'),
  '/var/www/CADCRAFT/getcadcraft.com/public_html/assets/vendor']
  .filter(Boolean).find((d) => fs.existsSync(path.join(d, 'delaunator/5.1.0/index.js')));
if (!VENDOR) throw new Error('vendored libraries not found: set CADCRAFT_VENDOR to the site\'s public_html/assets/vendor');
// the versions the 0.5 manifest's `current` links pointed at (pinned so a later upgrade cannot move the oracle)
const MAP = {
  delaunator: `${VENDOR}/delaunator/5.1.0/index.js`,
  'robust-predicates': `${VENDOR}/robust-predicates/3.0.3/index.js`,
  proj4: `${VENDOR}/proj4/2.22.0/proj4.esm.min.js`,
  geotiff: `${VENDOR}/geotiff/3.0.5/geotiff.esm.min.js`,
  'laz-perf': `${VENDOR}/laz-perf/0.0.7/laz-perf.js`,
  'sql.js': `${VENDOR}/sql.js/1.14.2/sql-wasm.esm.js`,
};
registerHooks({
  resolve(spec, ctx, next) {
    const m = MAP[spec];
    if (m) return { url: pathToFileURL(m).href, shortCircuit: true };
    return next(spec, ctx);
  },
});
export const mod = (f) => import(pathToFileURL(path.join(APP, f)).href);
const I18N = await mod('i18n.js');
I18N.init({ locale: 'en', strings: JSON.parse(fs.readFileSync(path.join(APP, 'lang/en.json'), 'utf8')) });

/** "$NaN" / "$Inf" / "$-Inf" → numbers, recursively (the harness's way to pass them through JSON). */
export function dec(v) {
  if (v === '$NaN') return NaN;
  if (v === '$Inf') return Infinity;
  if (v === '$-Inf') return -Infinity;
  if (Array.isArray(v)) return v.map(dec);
  if (v && typeof v === 'object') { const o = {}; for (const [k, x] of Object.entries(v)) o[k] = dec(x); return o; }
  return v;
}
