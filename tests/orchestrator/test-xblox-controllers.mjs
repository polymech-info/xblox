#!/usr/bin/env node
/**
 * XBlox controller smoke tests.
 *
 * This exercises interactive controller blocks from a deterministic harness.
 * `keyWait` normally polls physical keyboard state; with
 * PM_XBLOX_KEYWAIT_STDIN=1 it also accepts one shortcut token per stdin line.
 *
 *   npm run test:xblox-controllers
 */
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');
const CLI_MAX_BUFFER = 16 * 1024 * 1024;
const SHORTCUT = 'SHIFT + F12';

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

let passed = 0;
let failed = 0;

function ok(cond, label, detail = '') {
  if (cond) {
    passed += 1;
    console.log(`  ✓ ${label}`);
  } else {
    failed += 1;
    console.error(`  ✗ ${label}${detail ? `\n    ${detail}` : ''}`);
  }
}

function xbloxInfo(exe) {
  const r = spawnSync(exe, ['--log-level', 'off', 'xblox', 'info', '--json'], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
    maxBuffer: CLI_MAX_BUFFER,
  });
  try {
    return JSON.parse(r.stdout || '{}');
  } catch {
    return {};
  }
}

function waitForOutput(state, pattern, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve) => {
    const tick = () => {
      if (pattern.test(state.output)) return resolve(true);
      if (Date.now() >= deadline) return resolve(false);
      setTimeout(tick, 50);
    };
    tick();
  });
}

async function runSttJournalStdinSmoke(exe) {
  const work = mkdtempSync(join(tmpdir(), 'xblox-controllers-'));
  const journal = join(work, 'journal.txt');
  const src = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox', 'stt-journal.xblox');
  const state = { output: '' };

  const child = spawn(exe, [
    '--log-level', 'trace',
    'xblox', 'run',
    '--src', src,
    '--journal', journal,
  ], {
    cwd: PACKAGE_ROOT,
    stdio: ['pipe', 'pipe', 'pipe'],
    env: {
      ...process.env,
      PM_XBLOX_KEYWAIT_STDIN: '1',
    },
  });

  child.stdout.on('data', (d) => { state.output += d.toString(); });
  child.stderr.on('data', (d) => { state.output += d.toString(); });

  try {
    ok(await waitForOutput(state, /Press Shift\+F12 to start recording|\[keyEvent\] key=Shift\+F12/, 8_000), 'stt journal controller is running');

    child.stdin.write(`${SHORTCUT}\n`);
    ok(await waitForOutput(state, /\[keyEvent\] key=SHIFT \+ F12[^\n]*pressed=1/, 5_000), 'stdin shortcut produces keyEvent press');
    ok(await waitForOutput(state, /start recording/, 5_000), 'first shortcut enters start branch');
    ok(await waitForOutput(state, /\[audioRecordStart\] session=/, 8_000), 'audioRecordStart starts microphone session');

    await new Promise((r) => setTimeout(r, 600));
    child.stdin.write(`${SHORTCUT}\n`);
    ok(await waitForOutput(state, /stop recording/, 5_000), 'second shortcut enters stop branch');
    ok(await waitForOutput(state, /\[audioRecordStop\] session=/, 8_000), 'audioRecordStop writes a WAV');
    ok(await waitForOutput(state, /appended:/, 30_000), 'transcript is appended after stop');

    await waitForOutput(state, /appended:/, 30_000);
  } finally {
    try { child.stdin.end(); } catch { /* ignore */ }
    try { child.kill('SIGINT'); } catch { /* ignore */ }
    await new Promise((resolve) => {
      const t = setTimeout(resolve, 2_000);
      child.once('exit', () => { clearTimeout(t); resolve(); });
    });
    try { child.kill('SIGKILL'); } catch { /* ignore */ }
    rmSync(work, { recursive: true, force: true });
  }

  if (failed) {
    console.error('\n--- captured xblox controller output ---');
    console.error(state.output.slice(-12_000));
  }
}

async function main() {
  const exe = cliExePath();
  console.log(`XBlox controller tests: ${exe}`);

  const info = xbloxInfo(exe);
  const keyWait = Array.isArray(info.blocks) ? info.blocks.find((b) => b.kind === 'keyWait') : null;
  ok(!!keyWait, 'manifest contains keyWait');
  ok(keyWait?.execFlags?.is_container === true, 'keyWait is declared as a container');
  ok(keyWait?.params?.some((p) => p.name === 'keys' && p.kind === 'shortcut'), 'keyWait keys param is shortcut');

  await runSttJournalStdinSmoke(exe);

  console.log(`\nXBlox controller tests complete: passed=${passed} failed=${failed}`);
  process.exit(failed ? 1 : 0);
}

main().catch((err) => {
  console.error(err?.stack || err);
  process.exit(1);
});
