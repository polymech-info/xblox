import { createAssert, finish, requireExe, run } from './commons.mjs';

const checks = requireExe(createAssert());
const missing = run([]);
checks.assert(missing.status === 2, 'no args exits 2');

const bad = run(['run']);
checks.assert(bad.status === 2, 'run without a document exits 2');

const absent = run(['run', 'fixtures/missing.xblox']);
checks.assert(absent.status === 1, 'missing document exits 1');
checks.assert(absent.json && absent.json.ok === false, 'missing document ok:false');
checks.assert(absent.json && absent.json.code === 'EIO', 'missing document code EIO');

finish(checks);
