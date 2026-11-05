#!/usr/bin/env node
/**
 * XBlox IPC / session tests: cross-process session registry + control bus.
 *
 * Exercises the loopback control plane wired in src/xblox/xblox_session.cpp and
 * the `xblox session …` CLI verbs (list / start / stop / ping / pong), plus the
 * session a looping `xblox run … --loop` registers.
 *
 * Invoked via:
 *   npm run test:xblox-ipc
 */

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert } from '../../../../tests/orchestrator/test-commons.js';
import { buildMetricsBundle, createMetricsCollector, renderMarkdownReport } from '../../../../tests/orchestrator/reports.js';
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '../../../../tests/orchestrator/test-report-accumulate.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

function createTestReport() {
  return {
    meta: {},
    notes: [],
    suites: [],
    _cur: null,
    beginSuite(name) {
      if (this._cur) this.endSuite();
      this._cur = { name, steps: [], t0: performance.now() };
    },
    endSuite() {
      if (!this._cur) return;
      this._cur.totalMs = performance.now() - this._cur.t0;
      this.suites.push(this._cur);
      this._cur = null;
    },
    step(label, ms, detail = '') {
      if (!this._cur) return;
      this._cur.steps.push({ label, ms: Math.round(ms * 100) / 100, detail: detail || '' });
    },
    finalize(stats, wallMs, exe, extra = {}) {
      if (this._cur) this.endSuite();
      this.meta = {
        generatedAt: new Date().toISOString(),
        node: process.version,
        argv: process.argv.slice(2),
        exe,
        passed: stats.passed,
        failed: stats.failed,
        wallClockMs: Math.round(wallMs * 100) / 100,
        ...extra,
      };
    },
  };
}

function writeTestReportFile(rep, metricsCollector, startedAtIso) {
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  const failed = rep.meta.failed ?? 0;
  const ok = failed === 0 && !rep.meta.abortReason && !rep.meta.uncaughtError;

  let md = renderMarkdownReport({
    ok,
    passed: rep.meta.passed,
    failed,
    abortReason: rep.meta.abortReason,
    uncaughtError: rep.meta.uncaughtError,
    error: rep.meta.uncaughtError,
    meta: {
      cwd: process.cwd(),
      displayName: 'xblox-ipc',
      testName: 'xblox-ipc',
      writtenAt: finishedAtIso,
      generatedAt: rep.meta.generatedAt,
      exe: rep.meta.exe,
      argv: Array.isArray(rep.meta.argv) ? rep.meta.argv.join(' ') : String(rep.meta.argv ?? ''),
      wallClockMs: rep.meta.wallClockMs,
      workDir: rep.meta.workDir,
    },
    metrics,
    images: [],
    mediaSuites: rep.suites,
    integrationNotes: rep.notes,
  });

  const acc = isAccumulateMode() && getChapterTitle();
  if (acc) {
    md += '\n---\n\n*Per-run: `tests/test-report-last.md` — this run is also a chapter in `tests/test-report-all.md`.*\n';
  } else {
    md += '\n---\n\n*Artifact: `tests/test-report-last.md` — overwritten on each test run.*\n';
  }
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  if (acc) {
    appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
    console.log(`  (aggregated report chapter: ${TEST_REPORT_ALL_PATH})`);
  }
}

// ── CLI helpers ──────────────────────────────────────────────────────────────
function sessionCli(exe, args, timeout = 15_000) {
  const t0 = performance.now();
  const r = spawnSync(exe, ['xblox', '--log-level', 'off', 'session', ...args], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
  });
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    ms: performance.now() - t0,
  };
}

function parseJson(text) {
  const s = String(text || '').trim();
  if (!s) return null;
  try {
    return JSON.parse(s);
  } catch {
    // tolerate leading log/banner lines: parse from first '{' or '['.
    const i = Math.min(...[s.indexOf('{'), s.indexOf('[')].filter((n) => n >= 0));
    if (Number.isFinite(i)) {
      try {
        return JSON.parse(s.slice(i));
      } catch {
        /* fall through */
      }
    }
    return null;
  }
}

function listSessions(exe) {
  const r = sessionCli(exe, ['list', '--json']);
  const arr = parseJson(r.stdout);
  return Array.isArray(arr) ? arr : [];
}

const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

async function waitFor(fn, { timeoutMs = 8000, intervalMs = 150 } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() >= deadline) return null;
    await sleep(intervalMs);
  }
}

function spawnBg(exe, args) {
  const child = spawn(exe, args, {
    cwd: PACKAGE_ROOT,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.on('data', () => {});
  child.stderr.on('data', () => {});
  let exited = false;
  let exitCode = null;
  child.on('exit', (code) => {
    exited = true;
    exitCode = code;
  });
  return {
    child,
    get exited() {
      return exited;
    },
    get exitCode() {
      return exitCode;
    },
    kill() {
      try {
        child.kill('SIGKILL');
      } catch {
        /* ignore */
      }
    },
  };
}

async function waitExit(handle, timeoutMs = 6000) {
  return waitFor(() => (handle.exited ? true : false), { timeoutMs, intervalMs: 100 });
}

// ── Suite 1: bare session host (start → list → ping → pong → stop) ───────────
async function suiteBareHost(rep, assert, exe) {
  console.log('\n── XBlox IPC: bare session host ──\n');
  rep.beginSuite('XBlox IPC bare host');

  const key = `name:ipc-bare-${Date.now()}`;
  const host = spawnBg(exe, ['xblox', '--log-level', 'off', 'session', 'start', '--key', key]);

  try {
    const t0 = performance.now();
    const found = await waitFor(() => listSessions(exe).find((d) => d.key === key));
    rep.step('start → appears in list', performance.now() - t0, `key=${key}`);
    assert(!!found, `bare host: session ${key} appears in list`);
    if (!found) return;

    assert(found.transport === (process.platform === 'win32' ? 'tcp' : 'uds'),
      `bare host: transport is ${process.platform === 'win32' ? 'tcp' : 'uds'} (got ${found.transport})`);
    assert(typeof found.endpoint === 'string' && found.endpoint.length > 0,
      `bare host: endpoint advertised (got "${found.endpoint}")`);
    assert(found.loop === false, 'bare host: loop=false');

    // ping → pong
    {
      const r = sessionCli(exe, ['ping', '--key', key, '--json']);
      const reply = parseJson(r.stdout);
      rep.step('ping', r.ms, `type=${reply?.type}`);
      assert(r.status === 0, `bare host: ping exits 0 (got ${r.status})`);
      assert(reply?.type === 'pong', `bare host: ping → pong (got ${reply?.type})`);
      assert(String(reply?.key) === key, 'bare host: pong echoes session key');
    }

    // pong → ping (symmetric)
    {
      const r = sessionCli(exe, ['pong', '--key', key, '--json']);
      const reply = parseJson(r.stdout);
      rep.step('pong', r.ms, `type=${reply?.type}`);
      assert(reply?.type === 'ping', `bare host: pong → ping (got ${reply?.type})`);
    }

    // stop → host exits, session leaves the registry
    {
      const r = sessionCli(exe, ['stop', '--key', key, '--json']);
      const reply = parseJson(r.stdout);
      rep.step('stop', r.ms, `type=${reply?.type}`);
      assert(reply?.type === 'stop_ack', `bare host: stop → stop_ack (got ${reply?.type})`);

      const exitedOk = await waitExit(host, 6000);
      assert(!!exitedOk, 'bare host: process exits after stop');

      const gone = await waitFor(() => (listSessions(exe).some((d) => d.key === key) ? false : true), {
        timeoutMs: 4000,
      });
      assert(!!gone, 'bare host: descriptor removed from registry after stop');
    }
  } finally {
    host.kill();
    rep.endSuite();
  }
}

// ── Suite 2: looping run registers a session that `stop` cancels ─────────────
async function suiteLoopRun(rep, assert, exe, workDir) {
  console.log('\n── XBlox IPC: looping run session ──\n');
  rep.beginSuite('XBlox IPC loop run');

  const docPath = join(workDir, 'loop.blocks.json');
  writeFileSync(
    docPath,
    `${JSON.stringify(
      {
        version: 1,
        context: { n: 0 },
        roots: [{ kind: 'log', level: 'info', message: 'tick' }],
      },
      null,
      2,
    )}\n`,
  );

  const run = spawnBg(exe, [
    'xblox',
    '--log-level',
    'off',
    'run',
    '--src',
    docPath,
    '--no-wait',
    '--loop',
    '--loop-interval-ms',
    '80',
  ]);

  try {
    const t0 = performance.now();
    const found = await waitFor(() => listSessions(exe).find((d) => d.source && d.source.endsWith('loop.blocks.json')));
    rep.step('loop run → appears in list', performance.now() - t0, `key=${found?.key}`);
    assert(!!found, 'loop run: session appears in list');
    if (!found) return;

    assert(found.loop === true, 'loop run: loop=true');
    assert(found.key.startsWith('file:'), `loop run: file-keyed session (got ${found.key})`);

    // ping the live loop
    {
      const r = sessionCli(exe, ['ping', '--key', found.key, '--json']);
      const reply = parseJson(r.stdout);
      rep.step('ping loop', r.ms, `type=${reply?.type}`);
      assert(reply?.type === 'pong', `loop run: ping → pong (got ${reply?.type})`);
    }

    // stop cancels the loop → run process exits, session leaves registry
    {
      const r = sessionCli(exe, ['stop', '--key', found.key, '--json']);
      const reply = parseJson(r.stdout);
      rep.step('stop loop', r.ms, `type=${reply?.type}`);
      assert(reply?.type === 'stop_ack', `loop run: stop → stop_ack (got ${reply?.type})`);

      const exitedOk = await waitExit(run, 8000);
      assert(!!exitedOk, 'loop run: run process exits after stop');

      const gone = await waitFor(() => (listSessions(exe).some((d) => d.key === found.key) ? false : true), {
        timeoutMs: 4000,
      });
      assert(!!gone, 'loop run: descriptor removed after the loop stops');
    }
  } finally {
    run.kill();
    rep.endSuite();
  }
}

// ── Suite 3: messaging (directed + broadcast) + inbox drain ──────────────────
async function suiteMessaging(rep, assert, exe) {
  console.log('\n── XBlox IPC: messaging ──\n');
  rep.beginSuite('XBlox IPC messaging');

  const keyA = `name:ipc-msg-a-${Date.now()}`;
  const keyB = `name:ipc-msg-b-${Date.now()}`;
  const a = spawnBg(exe, ['xblox', '--log-level', 'off', 'session', 'start', '--key', keyA]);
  const b = spawnBg(exe, ['xblox', '--log-level', 'off', 'session', 'start', '--key', keyB]);

  try {
    const up = await waitFor(() => {
      const live = listSessions(exe).map((d) => d.key);
      return live.includes(keyA) && live.includes(keyB) ? true : false;
    });
    assert(!!up, 'messaging: both hosts registered');
    if (!up) return;

    // directed send → recv on A
    {
      const r = sessionCli(exe, ['send', '--key', keyA, '--from', 'tester', '--message', 'hello-A', '--json']);
      const reply = parseJson(r.stdout);
      rep.step('send directed', r.ms, `type=${reply?.type}`);
      assert(reply?.type === 'send_ack', `messaging: send → send_ack (got ${reply?.type})`);

      const rr = sessionCli(exe, ['recv', '--key', keyA, '--json']);
      const msgs = parseJson(rr.stdout);
      rep.step('recv directed', rr.ms, `count=${Array.isArray(msgs) ? msgs.length : '?'}`);
      assert(Array.isArray(msgs) && msgs.length === 1, `messaging: A inbox has 1 message (got ${msgs?.length})`);
      assert(msgs?.[0]?.message === 'hello-A', 'messaging: message text round-trips');
      assert(msgs?.[0]?.from === 'tester', 'messaging: from attribution round-trips');

      // drained — second recv is empty
      const rr2 = sessionCli(exe, ['recv', '--key', keyA, '--json']);
      const msgs2 = parseJson(rr2.stdout);
      assert(Array.isArray(msgs2) && msgs2.length === 0, 'messaging: inbox drained after recv');
    }

    // broadcast → both A and B receive
    {
      const r = sessionCli(exe, ['broadcast', '--from', 'tester', '--message', 'ping-all']);
      rep.step('broadcast', r.ms, `exit=${r.status}`);
      // `broadcast` fans out to every live session in the user's registry. In a
      // developer desktop there may be unrelated sessions, so partial failures
      // should not make this fixture flaky; the contract under test is that the
      // two test sessions receive the broadcast.
      assert(r.status === 0 || r.status === 1, `messaging: broadcast completes without crashing (got ${r.status})`);

      const aMsgs = parseJson(sessionCli(exe, ['recv', '--key', keyA, '--json']).stdout) || [];
      const bMsgs = parseJson(sessionCli(exe, ['recv', '--key', keyB, '--json']).stdout) || [];
      assert(aMsgs.some((m) => m.message === 'ping-all'), 'messaging: broadcast reached A');
      assert(bMsgs.some((m) => m.message === 'ping-all'), 'messaging: broadcast reached B');
    }
  } finally {
    sessionCli(exe, ['stop', '--key', keyA]);
    sessionCli(exe, ['stop', '--key', keyB]);
    a.kill();
    b.kill();
    rep.endSuite();
  }
}

// ── Suite 4: scope/variable exchange (bare KV + live loop scope) ─────────────
async function suiteVarExchange(rep, assert, exe, workDir) {
  console.log('\n── XBlox IPC: variable exchange ──\n');
  rep.beginSuite('XBlox IPC variable exchange');

  // (a) bare host acts as a shared KV store
  const key = `name:ipc-var-${Date.now()}`;
  const host = spawnBg(exe, ['xblox', '--log-level', 'off', 'session', 'start', '--key', key]);
  try {
    const up = await waitFor(() => listSessions(exe).find((d) => d.key === key));
    assert(!!up, 'vars: bare host registered');
    if (up) {
      sessionCli(exe, ['set', '--key', key, '--name', 'greeting', '--value', '"hi"']);
      const got = parseJson(sessionCli(exe, ['get', '--key', key, '--name', 'greeting', '--json']).stdout);
      rep.step('bare set/get', 0, `value=${JSON.stringify(got?.value)}`);
      assert(got?.found === true && got?.value === 'hi', `vars: bare get returns set value (got ${JSON.stringify(got?.value)})`);

      sessionCli(exe, ['set', '--key', key, '--name', 'count', '--value', '42']);
      const vars = parseJson(sessionCli(exe, ['vars', '--key', key, '--json']).stdout);
      assert(vars?.count === 42 && vars?.greeting === 'hi', 'vars: list returns all set vars');
    }
  } finally {
    sessionCli(exe, ['stop', '--key', key]);
    host.kill();
  }

  // (b) live looping run publishes its scope; external set lands next pass
  const docPath = join(workDir, 'counter.blocks.json');
  writeFileSync(
    docPath,
    `${JSON.stringify(
      {
        version: 1,
        context: { n: 0 },
        roots: [{ kind: 'setVariable', name: 'n', expression: 'n + 1' }],
      },
      null,
      2,
    )}\n`,
  );
  const run = spawnBg(exe, [
    'xblox', '--log-level', 'off', 'run', '--src', docPath, '--no-wait', '--loop', '--loop-interval-ms', '150',
  ]);
  try {
    const found = await waitFor(() => listSessions(exe).find((d) => d.source && d.source.endsWith('counter.blocks.json')));
    assert(!!found, 'vars: looping run registered');
    if (found) {
      const getN = () => parseJson(sessionCli(exe, ['get', '--key', found.key, '--name', 'n', '--json']).stdout);
      // n should climb — published snapshot exposes live scope. Wait for a clear
      // threshold so the externally-forced reset below is unambiguously observable.
      const climbing = await waitFor(() => {
        const v = getN();
        return v?.found && typeof v.value === 'number' && v.value >= 5 ? v.value : false;
      }, { timeoutMs: 8000 });
      rep.step('live scope publish', 0, `n=${climbing}`);
      assert(!!climbing, `vars: run publishes climbing counter n (got ${climbing})`);

      // externally force n back to 0; it should be applied at a pass boundary,
      // dropping the published value clearly below the threshold it had reached.
      sessionCli(exe, ['set', '--key', found.key, '--name', 'n', '--value', '0']);
      const reset = await waitFor(() => {
        const v = getN();
        return v?.found && typeof v.value === 'number' && v.value < (climbing || 5) ? v.value : false;
      }, { timeoutMs: 5000 });
      assert(reset !== null, `vars: external set is applied into the live loop (n dropped below ${climbing})`);

      // cwd / source surfaced via info
      const info = parseJson(sessionCli(exe, ['info', '--key', found.key, '--json']).stdout);
      assert(typeof info?.cwd === 'string' && info.cwd.length > 0, 'vars: info exposes cwd');
      assert(typeof info?.source === 'string' && info.source.endsWith('counter.blocks.json'), 'vars: info exposes xblox source file');
    }
  } finally {
    sessionCli(exe, ['stop', '--all']);
    run.kill();
    rep.endSuite();
  }
}

// ── Suite 5: stale-descriptor reaping + missing-target errors ────────────────
async function suiteRegistryHygiene(rep, assert, exe) {
  console.log('\n── XBlox IPC: registry hygiene ──\n');
  rep.beginSuite('XBlox IPC registry hygiene');

  // start a host, kill it hard (no graceful stop), confirm `list` reaps it.
  const key = `name:ipc-stale-${Date.now()}`;
  const host = spawnBg(exe, ['xblox', '--log-level', 'off', 'session', 'start', '--key', key]);
  const appeared = await waitFor(() => listSessions(exe).find((d) => d.key === key));
  assert(!!appeared, 'hygiene: stale-test host registered');

  host.kill();
  await waitExit(host, 5000);

  const reaped = await waitFor(() => (listSessions(exe).some((d) => d.key === key) ? false : true), {
    timeoutMs: 5000,
  });
  rep.step('reap dead descriptor', 0, `key=${key}`);
  assert(!!reaped, 'hygiene: list reaps descriptor whose owner died');

  // ping/stop against a non-existent key fails cleanly (non-zero, no crash).
  const r = sessionCli(exe, ['ping', '--key', 'name:does-not-exist', '--json']);
  rep.step('ping missing target', r.ms, `exit=${r.status}`);
  assert(r.status !== 0, `hygiene: ping unknown key exits non-zero (got ${r.status})`);

  rep.endSuite();
}

// ── main ─────────────────────────────────────────────────────────────────────
const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const rep = createTestReport();
const t0 = performance.now();
const exe = cliExePath();
const workDir = mkdtempSync(join(tmpdir(), 'pm-xblox-ipc-'));

try {
  if (!existsSync(exe)) throw new Error(`tanit-cli not found at ${exe}`);
  await suiteBareHost(rep, stats.assert, exe);
  await suiteLoopRun(rep, stats.assert, exe, workDir);
  await suiteMessaging(rep, stats.assert, exe);
  await suiteVarExchange(rep, stats.assert, exe, workDir);
  await suiteRegistryHygiene(rep, stats.assert, exe);
} catch (e) {
  console.error(e);
  rep.meta.abortReason = 'uncaught exception';
  rep.meta.uncaughtError = e?.stack || String(e);
} finally {
  rep.finalize(stats, performance.now() - t0, exe, { workDir });
  writeTestReportFile(rep, metricsCollector, startedAtIso);
  if (process.env.PM_KEEP_XBLOX_TEST_TMP !== '1') {
    try {
      rmSync(workDir, { recursive: true, force: true });
    } catch {
      /* ignore cleanup */
    }
  }
}

console.log(`\n${stats.failed === 0 ? '✅' : '❌'} xblox-ipc: ${stats.passed} passed, ${stats.failed} failed`);
if (stats.failed > 0 || rep.meta.abortReason) {
  process.exitCode = 1;
}
