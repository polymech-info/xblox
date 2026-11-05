import { readdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const suites = readdirSync(here)
  .filter((name) => name.startsWith('test-') && name.endsWith('.mjs'))
  .sort();

let failed = 0;
for (const suite of suites) {
  const r = spawnSync(process.execPath, [join(here, suite)], { stdio: 'inherit' });
  if ((r.status ?? 1) !== 0) failed++;
}
process.exit(failed ? 1 : 0);
