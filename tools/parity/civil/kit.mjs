// SPDX-License-Identifier: Apache-2.0
// The parity kit's canon / diff (tools/fork-parity/lib/canon.mjs in the site repo).
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const lib = [process.env.CADCRAFT_SITE, '/var/www/CADCRAFT/getcadcraft.com'].filter(Boolean)
  .map((d) => path.join(d, 'tools/fork-parity/lib/canon.mjs')).find((f) => fs.existsSync(f));
if (!lib) throw new Error('the parity kit (tools/fork-parity) was not found: set CADCRAFT_SITE');
export const { canon, diff } = await import(pathToFileURL(lib).href);
