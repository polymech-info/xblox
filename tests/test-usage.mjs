import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const missing = run([]);
checks.assert(missing.status === 2, 'no args exits 2');

const bad = run(['run']);
checks.assert(bad.status === 2, 'run without --src exits 2');

const absent = run(['run', '--src', 'fixtures/missing.xblox']);
checks.assert(absent.status === 1, 'missing document exits 1');
checks.assert(absent.stderr.includes('cannot open'), 'missing document reports cannot open');

const info = run(['info']);
checks.assert(info.status === 0, 'info exits 0');
checks.assert(info.stdout.includes('XBlox blocks:'), 'info prints the block summary');

finish(checks);
