#!/usr/bin/env node
/**
 * XBlox-Next runtime harness — focused, legacy-free.
 *
 * Unlike test-xblox.mjs (which still exercises `runScript`/host commands), this
 * harness drives ONLY the native block runtime via the `xblox` CLI and asserts
 * on emitted variable state / events. It is the regression baseline for the new
 * scope-frame + NH_Parameter work tracked in docs/xblox-next.md §10.
 *
 *   npm run test:xblox-next
 *   npm run test:xblox-next -- --only scoping
 *
 * Suites: variables · expressions · loops · previous · containers · scoping ·
 *         transform · simulate · constraints · globs · hooks · iterate · network ·
 *         ipc-com · video · audio · bluetooth · ocr · vision · llm · std · jq · fsVars · refs
 *
 * NOTE on the `scoping` suite: it currently encodes the BASELINE behaviour
 * (single shared store — loop locals leak). Those assertions flip to the
 * isolate/collect expectations once slice S3 (scope frames) lands. Each such
 * assertion is tagged [BASELINE] so it is obvious what changes.
 */

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateSync } from 'node:zlib';
import { buildMetricsBundle, createMetricsCollector, renderMarkdownReport } from '../../../../tests/orchestrator/reports.js';
import { cliExePath } from '../../../../tests/orchestrator/test-commons.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');

// ── tiny test runner ────────────────────────────────────────────────────────
let passed = 0;
let failed = 0;
const failures = [];
const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const reportSuites = [];
const reportNotes = [];

function beginReportSuite(name) {
  return { name, steps: [], t0: performance.now() };
}

function reportStep(suite, label, startMs, detail = '') {
  if (!suite) return;
  suite.steps.push({
    label,
    ms: Math.round((performance.now() - startMs) * 100) / 100,
    detail,
  });
}

function endReportSuite(suite) {
  if (!suite) return;
  suite.totalMs = Math.round((performance.now() - suite.t0) * 100) / 100;
  reportSuites.push(suite);
}

function writeNextReport() {
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  let md = renderMarkdownReport({
    ok: failed === 0,
    passed,
    failed,
    meta: {
      cwd: process.cwd(),
      displayName: 'xblox-next',
      testName: 'xblox-next',
      writtenAt: finishedAtIso,
      generatedAt: finishedAtIso,
      exe: EXE,
      argv: process.argv.slice(2).join(' '),
    },
    metrics,
    images: [],
    mediaSuites: reportSuites,
    integrationNotes: reportNotes,
  });
  md += '\n---\n\n*Artifact: `tests/test-report-last.md` — overwritten on each test run.*\n';
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
}

function ok(cond, label) {
  if (cond) {
    passed += 1;
    console.log(`  \u2713 ${label}`);
  } else {
    failed += 1;
    failures.push(label);
    console.log(`  \u2717 ${label}`);
  }
}

function eq(actual, expected, label) {
  ok(Object.is(actual, expected) || JSON.stringify(actual) === JSON.stringify(expected),
     `${label} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

// ── CLI driver ────────────────────────────────────────────────────────────────
const EXE = cliExePath();
const WORK = mkdtempSync(join(tmpdir(), 'xblox-next-'));
function resolveModelsDir() {
  const fromEnv = (process.env.MODELS_DIR || process.env.PM_MODELS_DIR || '').trim();
  const candidates = [
    fromEnv,
    'D:\\models',
    join(PACKAGE_ROOT, 'dist', 'models'),
  ].filter(Boolean);
  for (const dir of candidates) {
    if (existsSync(dir)) return dir;
  }
  return candidates[candidates.length - 1];
}
const MODELS = resolveModelsDir();

let featureFlags;
function featureFlagsOrEmpty() {
  if (featureFlags) return featureFlags;
  try {
    featureFlags = JSON.parse(readFileSync(join(PACKAGE_ROOT, 'dist', 'shared', 'features.json'), 'utf8'));
  } catch {
    featureFlags = {};
  }
  return featureFlags;
}

function featureEnabled(name) {
  return featureFlagsOrEmpty()[name] === true;
}

function skipUnlessFeature(name, what) {
  if (featureEnabled(name)) return false;
  console.log(`  · ${name} off in dist/shared/features.json — skipping ${what}`);
  return true;
}

function skipUnlessAnyFeature(names, what) {
  if (names.some((name) => featureEnabled(name))) return false;
  console.log(`  · ${names.join(' / ')} off in dist/shared/features.json — skipping ${what}`);
  return true;
}

// FEATURE_COMMAND_IPC_NEXT shares settings across CLI processes. The follower
// logs "settings ipc: snapshot failed" through pm-media's own spdlog, which
// ignores --log-level and writes the default pattern to stdout.
function withoutSettingsIpcNoise(text) {
  if (!featureEnabled('FEATURE_COMMAND_IPC_NEXT')) return String(text);
  return String(text).replace(/^\[[^\n]*\] \[warning\] settings ipc: snapshot failed:[^\n]*\n/g, '');
}
const ONNX_DIR = join(PACKAGE_ROOT, 'tests', 'onnx');

function writeUserVariablesSettings(path) {
  writeFileSync(path, `${JSON.stringify({
    variables: {
      STRING_VAR: 'string value',
      INT_VAR: 2,
      SECRET_VAR: 'secret',
      BOOL_VAR: false,
    },
  }, null, 2)}\n`, 'utf8');
}

function runFixture(relPath, opts = {}) {
  const src = join(PACKAGE_ROOT, relPath);
  const args = [...(opts.globalArgs || []), 'xblox', '--log-level', 'off', 'run'];
  if (!opts.noJson) args.push('--json');
  if (!opts.wait) args.push('--no-wait');
  args.push('--src', src);
  if (opts.simulate) args.push('--simulate');
  if (opts.extraArgs) args.push(...opts.extraArgs);
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 30_000,
    ...(opts.input !== undefined ? { input: opts.input } : {}),
    ...(opts.env ? { env: { ...process.env, ...opts.env } } : {}),
  });
  const report = parseTrailingJson(r.stdout || '');
  return { status: r.status ?? -1, stdout: r.stdout || '', stderr: r.stderr || '', report, events: report.events || [] };
}

function run(doc, opts = {}) {
  const src = join(WORK, `flow-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(src, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
  // opts.noJson drops --json: exercises the muted-event path and leaves stdout
  // to the flow itself (the pipe-friendly production mode). opts.input is piped
  // to the child's stdin (without it, stdin is an immediately-EOF pipe).
  const args = [...(opts.globalArgs || []), 'xblox', '--log-level', 'off', 'run'];
  if (!opts.noJson) args.push('--json');
  if (!opts.wait) args.push('--no-wait');
  args.push('--src', src);
  if (opts.simulate) args.push('--simulate');
  if (opts.extraArgs) args.push(...opts.extraArgs);
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 30_000,
    ...(opts.input !== undefined ? { input: opts.input } : {}),
    ...(opts.env ? { env: { ...process.env, ...opts.env } } : {}),
  });
  const report = parseTrailingJson(r.stdout || '');
  return { status: r.status ?? -1, stdout: r.stdout || '', stderr: r.stderr || '', report, events: report.events || [] };
}

function runSelftest() {
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'selftest'], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
  });
  return { status: r.status ?? -1, stderr: r.stderr || '', report: parseTrailingJson(r.stdout || '') };
}

function xbloxInfoOptions(path) {
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'info', 'options', '--path', path], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
  });
  return { status: r.status ?? -1, stderr: r.stderr || '', options: parseAnyJson(r.stdout || '') };
}

function sessionCli(args, timeout = 15_000) {
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'session', ...args], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
  });
  return { status: r.status ?? -1, stdout: r.stdout || '', stderr: r.stderr || '' };
}

function parseAnyJson(text) {
  const s = String(text || '').trim();
  if (!s) return null;
  try { return JSON.parse(s); } catch { /* try from first JSON opener */ }
  const openers = [s.indexOf('{'), s.indexOf('[')].filter((n) => n >= 0);
  if (!openers.length) return null;
  try { return JSON.parse(s.slice(Math.min(...openers))); } catch { return null; }
}

function listSessions() {
  const r = sessionCli(['list', '--json']);
  const arr = parseAnyJson(r.stdout);
  return Array.isArray(arr) ? arr : [];
}

function runIpcCli(args, timeout = 15_000) {
  const r = spawnSync(EXE, ['--log-level', 'off', 'run-ipc', ...args], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
  });
  return { status: r.status ?? -1, stdout: r.stdout || '', stderr: r.stderr || '' };
}

function listRunIpcInstances() {
  const r = runIpcCli(['list', '--json']);
  const arr = parseAnyJson(r.stdout);
  return Array.isArray(arr) ? arr : [];
}

function fnv1a64Hex(text) {
  // Mirrors src/xblox/xblox_session.cpp exactly. This project uses a stable
  // identity hash, not the canonical FNV offset basis.
  let h = 1469598103934665603n;
  const prime = 0x100000001b3n;
  for (const byte of Buffer.from(text, 'utf8')) {
    h ^= BigInt(byte);
    h = (h * prime) & 0xffffffffffffffffn;
  }
  return h.toString(16).padStart(16, '0');
}

function xbloxFileSessionKey(src) {
  let p = resolve(src).replace(/\\/g, '/');
  if (process.platform === 'win32') p = p.toLowerCase();
  return `file:${fnv1a64Hex(p)}`;
}

const sleep = (ms) => new Promise((resolveSleep) => setTimeout(resolveSleep, ms));

async function waitFor(fn, { timeoutMs = 8000, intervalMs = 100 } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const value = fn();
    if (value) return value;
    if (Date.now() >= deadline) return null;
    await sleep(intervalMs);
  }
}

function spawnXbloxRun(src) {
  const child = spawn(EXE, ['xblox', '--log-level', 'off', 'run', '--json', '--src', src], {
    cwd: PACKAGE_ROOT,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  let exited = false;
  let exitCode = null;
  child.stdout.on('data', (d) => { stdout += String(d); });
  child.stderr.on('data', (d) => { stderr += String(d); });
  child.on('exit', (code) => { exited = true; exitCode = code; });
  return {
    child,
    get stdout() { return stdout; },
    get stderr() { return stderr; },
    get exited() { return exited; },
    get exitCode() { return exitCode; },
    kill() { try { child.kill('SIGKILL'); } catch { /* ignore */ } },
  };
}

// `xblox info --json` dumps the rich block manifest (params/defaults/execFlags)
// the host streams to the UI. Used to assert a block's *declared* interface
// without running it — deterministic and offline (no model/hardware needed).
function xbloxInfo() {
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'info', '--json'], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
    // The rich native manifest includes full block defaults, dynamic schemas,
    // grouped aliases, and command metadata; it legitimately exceeds Node's
    // 1 MiB spawnSync default buffer.
    maxBuffer: 16 * 1024 * 1024,
  });
  return parseTrailingJson(r.stdout || '');
}

function parseTrailingJson(stdout) {
  const text = String(stdout || '').trim();
  const candidates = [text.lastIndexOf('\n{'), text.indexOf('{')].filter((n) => n >= 0);
  for (const start of candidates) {
    try {
      return JSON.parse(text.slice(text[start] === '{' ? start : start + 1));
    } catch {
      /* try next */
    }
  }
  try { return JSON.parse(text); } catch { return { events: [], ok: false }; }
}

// curl reads file:// URLs offline & deterministically — used by the network
// suite so it never touches the real network. Windows wants file:///C:/...
function fileUrl(p) {
  const norm = String(p).replace(/\\/g, '/');
  return process.platform === 'win32' ? `file:///${norm}` : `file://${norm}`;
}

function crc32(buf) {
  const table = crc32.table || (crc32.table = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      t[n] = c >>> 0;
    }
    return t;
  })());
  let c = 0xffffffff;
  for (const b of buf) c = table[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBuf = Buffer.from(type, 'ascii');
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

function writeTextPng(filePath) {
  const font = {
    A:['01110','10001','10001','11111','10001','10001','10001'], B:['11110','10001','10001','11110','10001','10001','11110'],
    D:['11110','10001','10001','10001','10001','10001','11110'], E:['11111','10000','10000','11110','10000','10000','11111'],
    G:['01111','10000','10000','10011','10001','10001','01110'], I:['11111','00100','00100','00100','00100','00100','11111'],
    K:['10001','10010','10100','11000','10100','10010','10001'], L:['10000','10000','10000','10000','10000','10000','11111'],
    M:['10001','11011','10101','10101','10001','10001','10001'], O:['01110','10001','10001','10001','10001','10001','01110'],
    P:['11110','10001','10001','11110','10000','10000','10000'], R:['11110','10001','10001','11110','10100','10010','10001'],
    S:['01111','10000','10000','01110','00001','00001','11110'], T:['11111','00100','00100','00100','00100','00100','00100'],
    U:['10001','10001','10001','10001','10001','10001','01110'], X:['10001','10001','01010','00100','01010','10001','10001'],
    0:['01110','10001','10011','10101','11001','10001','01110'], 1:['00100','01100','00100','00100','00100','00100','01110'],
    2:['01110','10001','00001','00010','00100','01000','11111'], 3:['11110','00001','00001','01110','00001','00001','11110'],
    4:['00010','00110','01010','10010','11111','00010','00010'], 5:['11111','10000','10000','11110','00001','00001','11110'],
    6:['01110','10000','10000','11110','10001','10001','01110'], 7:['11111','00001','00010','00100','01000','01000','01000'],
    8:['01110','10001','10001','01110','10001','10001','01110'], 9:['01110','10001','10001','01111','00001','00001','01110'],
    '.':['00000','00000','00000','00000','00000','01100','01100'], ':':['00000','01100','01100','00000','01100','01100','00000'],
    ' ':['00000','00000','00000','00000','00000','00000','00000'],
  };
  const width = 900, height = 360, scale = 7;
  const rgba = Buffer.alloc(width * height * 4, 255);
  function setPixel(x, y) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const i = (y * width + x) * 4;
    rgba[i] = 0; rgba[i + 1] = 0; rgba[i + 2] = 0; rgba[i + 3] = 255;
  }
  function drawText(text, x, y) {
    let cx = x;
    for (const ch of text.toUpperCase()) {
      const glyph = font[ch] || font[' '];
      for (let gy = 0; gy < glyph.length; gy++) {
        for (let gx = 0; gx < glyph[gy].length; gx++) {
          if (glyph[gy][gx] !== '1') continue;
          for (let yy = 0; yy < scale; yy++) for (let xx = 0; xx < scale; xx++) setPixel(cx + gx * scale + xx, y + gy * scale + yy);
        }
      }
      cx += 6 * scale;
    }
  }
  drawText('PIXDRUG STORE', 55, 55);
  drawText('MILK 2.49', 55, 135);
  drawText('BREAD 3.20', 55, 205);
  drawText('TOTAL 5.69', 55, 275);
  const rawRows = [];
  for (let y = 0; y < height; y++) rawRows.push(Buffer.concat([Buffer.from([0]), rgba.subarray(y * width * 4, (y + 1) * width * 4)]));
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4); ihdr[8] = 8; ihdr[9] = 6;
  const png = Buffer.concat([
    Buffer.from([137,80,78,71,13,10,26,10]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(Buffer.concat(rawRows))),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
  writeFileSync(filePath, png);
}

function samePath(a, b) {
  const left = resolve(String(a || ''));
  const right = resolve(String(b || ''));
  return process.platform === 'win32'
    ? left.toLowerCase() === right.toLowerCase()
    : left === right;
}

const byKind = (events, kind) => events.filter((e) => e.kind === kind);
const setVars = (events) => byKind(events, 'setVariable');
const getVars = (events) => byKind(events, 'getVariable');
const logs = (events) => byKind(events, 'log').filter((e) => e.status === 'ok');
const lastLog = (events) => logs(events).at(-1);

// ── suites ──────────────────────────────────────────────────────────────────

function suiteEngine() {
  console.log('\n── engine (native scope frames + muParser var factory) ──');
  const { status, report } = runSelftest();
  eq(status, 0, 'engine: selftest exits 0');
  ok(report.ok === true, 'engine: all native self-test checks pass');
  for (const c of report.checks || []) ok(c.ok === true, `engine: ${c.label}`);
}

function suiteVariables() {
  console.log('\n── variables ──');
  const { status, events } = run({
    version: 1,
    context: { seed: 3 },
    roots: [
      { kind: 'setVariable', name: 'score', expression: 'seed + 4' },
      { kind: 'getVariable', name: 'score', target: 'copiedScore' },
      { kind: 'setVariable', name: 'flag', value: 1 },
      { kind: 'log', level: 'info', message: 'score + copiedScore + flag' },
    ],
  });
  eq(status, 0, 'variables: exits 0');
  ok(setVars(events).some((e) => e.data?.name === 'score' && e.data?.value === 7),
     'variables: setVariable evaluates expression against seed');
  ok(getVars(events).some((e) => e.data?.target === 'copiedScore' && e.data?.value === 7),
     'variables: getVariable copies value');
  eq(lastLog(events)?.message, '15', 'variables: log sees score+copiedScore+flag = 15');

  const beforeEpoch = Math.floor(Date.now() / 1000);
  const defaults = run({
    version: 1,
    roots: [
      { kind: 'getVariable', name: 'argv', target: 'scopeArgv' },
      { kind: 'getVariable', name: 'argv_str', target: 'scopeArgvStr' },
      { kind: 'getVariable', name: 'os', target: 'scopeOs' },
      { kind: 'getVariable', name: 'arch', target: 'scopeArch' },
      { kind: 'getVariable', name: 'cwd', target: 'scopeCwd' },
      { kind: 'getVariable', name: 'epoch', target: 'scopeEpoch' },
      { kind: 'getVariable', name: 'date', target: 'scopeDate' },
    ],
  });
  const afterEpoch = Math.floor(Date.now() / 1000);
  const defaultVars = Object.fromEntries(
    getVars(defaults.events).map((e) => [e.data?.target, e.data?.value])
  );
  console.log(`  · variables/defaults actual: ${JSON.stringify(defaultVars)}`);
  eq(defaults.status, 0, 'variables/defaults: exits 0');
  ok(Array.isArray(defaultVars.scopeArgv) && defaultVars.scopeArgv.includes('xblox') && defaultVars.scopeArgv.includes('run'),
     'variables/defaults: argv contains the full xblox run argument vector');
  ok(typeof defaultVars.scopeArgvStr === 'string' && defaultVars.scopeArgvStr.includes('xblox') && defaultVars.scopeArgvStr.includes('run'),
     'variables/defaults: argv_str contains the full command line string');
  eq(defaultVars.scopeOs, process.platform, 'variables/defaults: os matches Node process.platform');
  eq(defaultVars.scopeArch, process.arch, 'variables/defaults: arch matches Node process.arch');
  ok(samePath(defaultVars.scopeCwd, PACKAGE_ROOT), 'variables/defaults: cwd matches the spawned working directory');
  ok(Number.isInteger(defaultVars.scopeEpoch) && defaultVars.scopeEpoch >= beforeEpoch && defaultVars.scopeEpoch <= afterEpoch,
     'variables/defaults: epoch is current Unix epoch seconds');
  ok(typeof defaultVars.scopeDate === 'string' && !Number.isNaN(Date.parse(defaultVars.scopeDate)),
     'variables/defaults: date is an ISO-parseable timestamp');
  ok(Math.abs(Math.floor(Date.parse(defaultVars.scopeDate) / 1000) - defaultVars.scopeEpoch) <= 1,
     'variables/defaults: date and epoch describe the same instant');
}

function suiteWiringContract() {
  console.log('\n── typed wiring contract ──');

  const info = xbloxInfo();
  eq(info.version, 2, 'wiring/manifest: payload is v2');
  ok(Array.isArray(info.parameterOperations),
     'wiring/manifest: parameter-operation registry is present');
  const existsDef = (info.blocks || []).find((block) => block.kind === 'fsExists');
  const existsResult = (existsDef?.outputs || []).find((output) => output.slot === 'result');
  ok(existsDef?.manifestVersion === 2,
     'wiring/manifest: block descriptors advertise manifest v2');
  ok(existsResult?.kind === 'boolean' && existsResult?.primary === true,
     'wiring/manifest: fsExists declares its primary boolean result slot');
  ok(info.bindingDefaults?.accepts?.includes('blockOutput'),
     'wiring/manifest: runtime params default to block-output bindings');
  const storeAsParam = (existsDef?.params || []).find((param) => param.name === 'storeAs');
  ok(storeAsParam?.binding === false,
     'wiring/manifest: output-name params explicitly opt out of bindings');

  const contractErrors = [];
  for (const block of info.blocks || []) {
    const outputs = Array.isArray(block.outputs) ? block.outputs : [];
    const slots = outputs.map((output) => output.slot);
    const primaries = outputs.filter((output) => output.primary);
    const previousWriters = outputs.filter((output) => output.writesPrevious);
    if (slots.some((slot) => typeof slot !== 'string' || slot.length === 0))
      contractErrors.push(`${block.kind}: empty output slot`);
    if (new Set(slots).size !== slots.length)
      contractErrors.push(`${block.kind}: duplicate output slot`);
    if (outputs.length > 0 && primaries.length !== 1)
      contractErrors.push(`${block.kind}: expected exactly one primary output`);
    if (primaries.some((output) => output.slot !== 'result'))
      contractErrors.push(`${block.kind}: primary slot is not result`);
    if (block.previousBehavior === 'writes' &&
        (previousWriters.length !== 1 || !previousWriters[0]?.primary))
      contractErrors.push(`${block.kind}: invalid PREVIOUS writer`);
    if (block.previousBehavior !== 'writes' && previousWriters.length > 0)
      contractErrors.push(`${block.kind}: writesPrevious disagrees with previousBehavior`);
    const exportKeys = [];
    for (const output of outputs.filter((candidate) => candidate.scopeExport)) {
      const hasFixed = typeof output.exportName === 'string' && output.exportName.length > 0;
      const hasParam = typeof output.exportFromParam === 'string' && output.exportFromParam.length > 0;
      if (hasFixed && hasParam)
        contractErrors.push(`${block.kind}.${output.slot}: mixes fixed and parameter-derived exports`);
      else if (hasFixed)
        exportKeys.push(output.exportName);
      else if (hasParam)
        exportKeys.push(`param:${output.exportFromParam}:${output.exportSuffix || ''}`);
      else
        contractErrors.push(`${block.kind}.${output.slot}: scopeExport without export metadata`);
    }
    if (exportKeys.some((name) => typeof name !== 'string' || name.length === 0) ||
        new Set(exportKeys).size !== exportKeys.length)
      contractErrors.push(`${block.kind}: invalid or duplicate scope export`);
    for (const output of outputs.filter((candidate) => candidate.dynamic)) {
      if (!Array.isArray(output.supportedKinds) ||
          output.supportedKinds.length === 0 ||
          !output.supportedKinds.includes(output.kind))
        contractErrors.push(`${block.kind}.${output.slot}: invalid dynamic supportedKinds`);
    }
    if (outputs.length === 0 && block.previousBehavior === 'unknown')
      contractErrors.push(`${block.kind}: output/previous behavior is undeclared`);
  }
  ok(contractErrors.length === 0,
     `wiring/manifest: all visible output contracts validate${contractErrors.length ? ` (${contractErrors.join('; ')})` : ''}`);

  const fsReadDef = (info.blocks || []).find((block) => block.kind === 'fsRead');
  const fsReadResult = (fsReadDef?.outputs || []).find((output) => output.slot === 'result');
  ok(fsReadResult?.dynamic === true &&
     fsReadResult?.supportedKinds?.includes('string') &&
     fsReadResult?.supportedKinds?.includes('json_value'),
  'wiring/manifest: dynamic primary kinds serialize deterministically');
  const modelControlDef = (info.blocks || []).find((block) => block.kind === 'modelControl');
  const unloadedOutput = (modelControlDef?.outputs || []).find((output) => output.slot === 'unloaded');
  ok(unloadedOutput?.conditional === true && unloadedOutput?.kind === 'json_value',
     'wiring/manifest: conditional named outputs serialize deterministically');

  const wired = run({
    version: 2,
    context: { scopedMessage: 'from-scope' },
    roots: [
      { blockId: 'fsExists-k7m2p9', kind: 'fsExists', path: WORK, storeAs: '' },
      {
        blockId: 'log-m4n8q2',
        kind: 'log',
        level: 'info',
        bindings: {
          message: {
            kind: 'blockOutput',
            blockId: 'fsExists-k7m2p9',
            output: 'result',
          },
        },
      },
      {
        blockId: 'log-r5t1v7',
        kind: 'log',
        level: 'info',
        bindings: {
          message: { kind: 'scopeVariable', name: 'scopedMessage' },
        },
      },
    ],
  });
  eq(wired.status, 0, 'wiring/runtime: v2 block-output and scope bindings exit 0');
  const wiredLogs = logs(wired.events);
  eq(wiredLogs.at(-2)?.message, 'true',
     'wiring/runtime: downstream input resolves an earlier result slot');
  eq(wiredLogs.at(-1)?.message, 'from-scope',
     'wiring/runtime: scopeVariable binding resolves by name');
  const existsEvent = byKind(wired.events, 'fsExists').find((event) => event.status === 'ok');
  ok(existsEvent?.blockId === 'fsExists-k7m2p9',
     'wiring/events: emitted event carries the stable block id');
  ok(existsEvent?.outputs?.some((output) =>
    output.slot === 'result' && output.kind === 'boolean' && output.value === true),
  'wiring/events: emitted event carries typed result-slot data');

  const modelList = run({
    version: 2,
    roots: [{
      blockId: 'modelControl-contract',
      kind: 'modelControl',
      action: 'List',
      key: '',
      storeAs: 'models',
    }],
  });
  eq(modelList.status, 0, 'wiring/runtime: deterministic model-list contract exits 0');
  const modelListEvent = byKind(modelList.events, 'modelControl')
    .find((event) => event.status === 'ok');
  const declaredModelOutputs = new Map(
    (modelControlDef?.outputs || []).map((output) => [output.slot, output])
  );
  const emittedModelSlots = (modelListEvent?.outputs || []).map((output) => output.slot);
  ok(emittedModelSlots.includes('result') && emittedModelSlots.includes('models') &&
     (modelListEvent?.outputs || []).every((output) => {
       const declared = declaredModelOutputs.get(output.slot);
       return declared &&
         (declared.dynamic
           ? declared.supportedKinds?.includes(output.kind)
           : declared.kind === output.kind);
     }),
  'wiring/runtime: model-list emitted slots match its declared output contract');

  const operationStub = run({
    version: 2,
    roots: [{
      blockId: 'log-operation',
      kind: 'log',
      bindings: {
        message: {
          kind: 'parameterOperation',
          operationId: 'example.concat',
          variant: 'concat',
          inputs: {},
        },
      },
    }],
  });
  ok(operationStub.status !== 0,
     'wiring/operations: metadata-only parameter operation is rejected');
  ok(operationStub.events.some((event) =>
    event.data?.diagnostics?.some((diagnostic) =>
      diagnostic.code === 'unsupported_parameter_operation')),
  'wiring/operations: rejection uses unsupported_parameter_operation diagnostic');

  const missingId = run({
    version: 2,
    roots: [{ kind: 'log', message: 'invalid' }],
  });
  ok(missingId.status !== 0,
     'wiring/document: v2 block without blockId is rejected');
  ok(missingId.events.some((event) =>
    event.data?.diagnostics?.some((diagnostic) => diagnostic.code === 'missing_block_id')),
  'wiring/document: missing blockId uses missing_block_id diagnostic');

  const duplicateId = run({
    version: 2,
    roots: [{
      blockId: 'group-duplicate-test',
      kind: 'group',
      items: [
        { blockId: 'log-duplicate', kind: 'log', message: 'first' },
        { blockId: 'log-duplicate', kind: 'log', message: 'second' },
      ],
    }],
  });
  ok(duplicateId.status !== 0,
     'wiring/document: duplicate nested blockId is rejected');
  ok(duplicateId.events.some((event) =>
    event.data?.diagnostics?.some((diagnostic) => diagnostic.code === 'duplicate_block_id')),
  'wiring/document: duplicate nested ID uses duplicate_block_id diagnostic');

  const commandIdentity = run({
    version: 2,
    roots: [{
      blockId: 'command-node-k4m8q2',
      kind: 'command',
      command: {
        id: 'semantic-command-id',
        label: 'Metadata command',
        action: 'metadata',
      },
    }],
  });
  eq(commandIdentity.status, 0,
     'wiring/command: stable block and semantic command IDs coexist');
  const commandEvent = byKind(commandIdentity.events, 'command').find((event) =>
    event.status === 'ok');
  eq(commandEvent?.blockId, 'command-node-k4m8q2',
     'wiring/command: event blockId is stable graph identity');
  eq(commandEvent?.data?.id, 'semantic-command-id',
     'wiring/command: event data keeps semantic command id');

  const v1Legacy = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'x', value: 3 },
      { kind: 'log', level: 'info', message: 'x' },
    ],
  });
  eq(v1Legacy.status, 0, 'wiring/v1: documents without blockId still run');

  const missingProducer = run({
    version: 2,
    roots: [{
      blockId: 'log-missing-producer',
      kind: 'log',
      level: 'info',
      bindings: {
        message: {
          kind: 'blockOutput',
          blockId: 'does-not-exist',
          output: 'result',
        },
      },
    }],
  });
  ok(missingProducer.status !== 0,
     'wiring/binding: missing producer blockId fails the run');
  ok(missingProducer.events.some((event) =>
    event.data?.diagnostics?.some((diagnostic) =>
      diagnostic.code === 'unavailable_binding_source')),
  'wiring/binding: missing producer surfaces unavailable_binding_source');

  const nestedBinding = run({
    version: 2,
    context: { seed: 2 },
    roots: [{
      blockId: 'for-nested-binding',
      kind: 'for',
      variable: 'i',
      initial: '0',
      final: '2',
      comparator: '<',
      modifier: '+1',
      loopLimit: 4,
      items: [
        {
          blockId: 'setVariable-loop-body',
          kind: 'setVariable',
          name: 'acc',
          expression: 'seed + i',
        },
        {
          blockId: 'log-loop-body',
          kind: 'log',
          level: 'info',
          bindings: {
            message: {
              kind: 'blockOutput',
              blockId: 'setVariable-loop-body',
              output: 'result',
            },
          },
        },
      ],
    }],
  });
  eq(nestedBinding.status, 0, 'wiring/nested: blockOutput binding inside for body exits 0');
  const nestedLogs = logs(nestedBinding.events).map((event) => event.message);
  ok(nestedLogs.some((message) => message === '2' || message === '2.0'),
     'wiring/nested: first iteration resolves producer blockId');
  ok(nestedLogs.some((message) => message === '3' || message === '3.0'),
     'wiring/nested: second iteration resolves producer blockId');

  const wiringFixture = runFixture('packages/xblox/tests/xblox/wiring-block-output.xblox');
  eq(wiringFixture.status, 0, 'wiring/fixture: wiring-block-output.xblox exits 0');
  const boundLog = byKind(wiringFixture.events, 'log').find((event) =>
    event.blockId === 'log-consumer' && event.status === 'ok');
  ok(boundLog?.message?.includes('Berlin'),
     'wiring/fixture: bound log resolves setVariable output by blockId');
}

function suiteExpressions() {
  console.log('\n── expressions ──');
  const { status, events } = run({
    version: 1,
    context: { a: 6, b: 4 },
    roots: [
      { kind: 'setVariable', name: 'sum', expression: 'a + b' },
      { kind: 'setVariable', name: 'prod', expression: 'a * b' },
      { kind: 'setVariable', name: 'mixed', expression: '(a > b) * 100 + (sum == 10) * 1' },
      { kind: 'log', level: 'info', message: 'mixed' },
    ],
  });
  eq(status, 0, 'expressions: exits 0');
  ok(setVars(events).some((e) => e.data?.name === 'sum' && e.data?.value === 10), 'expressions: a+b = 10');
  ok(setVars(events).some((e) => e.data?.name === 'prod' && e.data?.value === 24), 'expressions: a*b = 24');
  eq(lastLog(events)?.message, '101', 'expressions: boolean arithmetic composes (100 + 1)');

  // PREVIOUS is global (never frame-scoped); the expression runtime binds it
  // straight from the numeric cache, not the JSON mirror. Each setVariable also
  // sets PREVIOUS, so `PREVIOUS + 1` should see the prior assignment.
  const prevRun = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'x', value: 5 },
      { kind: 'setVariable', name: 'y', expression: 'PREVIOUS + 1' },
      { kind: 'log', level: 'info', message: 'y' },
    ],
  });
  eq(prevRun.status, 0, 'expressions: PREVIOUS-in-expression exits 0');
  eq(lastLog(prevRun.events)?.message, '6', 'expressions: PREVIOUS resolves inside an expression (5 + 1)');

  const settingsPath = join(WORK, 'user-variables.settings.json');
  writeUserVariablesSettings(settingsPath);
  const userRun = run({
    version: 1,
    context: { score: 7 },
    roots: [
      { kind: 'setVariable', name: 'intPlusScore', expression: '${USER:INT_VAR} + score' },
      { kind: 'setVariable', name: 'stringMatch', expression: '"${USER:STRING_VAR}" == "string value"' },
      { kind: 'setVariable', name: 'secretMatch', expression: '"${USER:SECRET_VAR}" == "secret"' },
      { kind: 'setVariable', name: 'boolFalse', expression: '"${USER:BOOL_VAR}" == "false"' },
      { kind: 'log', level: 'info', message: 'intPlusScore + stringMatch + secretMatch + boolFalse' },
    ],
  }, { globalArgs: ['--settings', settingsPath] });
  eq(userRun.status, 0, 'expressions/user-vars: exits 0');
  const userVars = Object.fromEntries(setVars(userRun.events).map((e) => [e.data?.name, e.data?.value]));
  eq(userVars.intPlusScore, 9, 'expressions/user-vars: ${USER:INT_VAR} composes with context arithmetic');
  eq(userVars.stringMatch, 1, 'expressions/user-vars: ${USER:STRING_VAR} folds in string equality');
  eq(userVars.secretMatch, 1, 'expressions/user-vars: ${USER:SECRET_VAR} resolves from settings variables');
  eq(userVars.boolFalse, 1, 'expressions/user-vars: ${USER:BOOL_VAR} resolves boolean JSON as false');
  eq(lastLog(userRun.events)?.message, '12', 'expressions/user-vars: resolved USER values compose numerically');

  // Fast-path classifier (single binary op, no muParser): var OP var / var OP
  // literal / var CMP literal must match muParser exactly. These shapes are what
  // for/while counters and conditions use, so locking them guards the fast path.
  const fast = run({
    version: 1,
    context: { p: 7, q: 2 },
    roots: [
      { kind: 'setVariable', name: 'sub', expression: 'p - q' },        // var - var
      { kind: 'setVariable', name: 'div', expression: 'p / q' },        // var / var (3.5)
      { kind: 'setVariable', name: 'cmp', expression: 'p > q' },        // var > var → 1
      { kind: 'setVariable', name: 'eq0', expression: 'q == 3' },       // var == literal → 0
      { kind: 'setVariable', name: 'scaled', expression: 'p * 100' },   // var * literal
      { kind: 'log', level: 'info', message: 'sub + div + cmp + eq0 + scaled' },
    ],
  });
  eq(fast.status, 0, 'expressions: fast-path run exits 0');
  ok(setVars(fast.events).some((e) => e.data?.name === 'sub' && e.data?.value === 5), 'expressions: var-var subtraction (p-q=5)');
  ok(setVars(fast.events).some((e) => e.data?.name === 'div' && e.data?.value === 3.5), 'expressions: var-var division (p/q=3.5)');
  ok(setVars(fast.events).some((e) => e.data?.name === 'cmp' && e.data?.value === 1), 'expressions: var>var comparison → 1');
  ok(setVars(fast.events).some((e) => e.data?.name === 'eq0' && e.data?.value === 0), 'expressions: var==literal comparison → 0');
  ok(setVars(fast.events).some((e) => e.data?.name === 'scaled' && e.data?.value === 700), 'expressions: var*literal (p*100=700)');
  // 5 + 3.5 + 1 + 0 + 700 = 709.5
  eq(lastLog(fast.events)?.message, '709.5', 'expressions: fast-path results compose like muParser');

  // ── bespoke: muParser surface (functions, precedence, ternary) ─────────────
  const fn = run({
    version: 1,
    context: { a: 6, b: 4 },
    roots: [
      { kind: 'setVariable', name: 'lo', expression: 'min(a, b)' },
      { kind: 'setVariable', name: 'hi', expression: 'max(a, b)' },
      { kind: 'setVariable', name: 'mag', expression: 'abs(b - a)' },
      { kind: 'setVariable', name: 'root', expression: 'sqrt(a * b + 1)' },
      { kind: 'setVariable', name: 'prec', expression: '2^3 + 2 * 3' },
      { kind: 'setVariable', name: 'pick', expression: 'a > b ? 100 : 50' },
    ],
  });
  eq(fn.status, 0, 'expressions/fns: exits 0');
  const fnVars = Object.fromEntries(setVars(fn.events).map((e) => [e.data?.name, e.data?.value]));
  eq(fnVars.lo, 4, 'expressions/fns: min(a,b)');
  eq(fnVars.hi, 6, 'expressions/fns: max(a,b)');
  eq(fnVars.mag, 2, 'expressions/fns: abs(b-a)');
  eq(fnVars.root, 5, 'expressions/fns: sqrt(a*b+1) = 5');
  eq(fnVars.prec, 14, 'expressions/fns: ^ binds tighter than * (2^3 + 2*3 = 14)');
  eq(fnVars.pick, 100, 'expressions/fns: ternary cond ? a : b');

  // ── bespoke: string-equality folding (quoted literal, either side) ─────────
  const fold = run({
    version: 1,
    context: { status: 'ready', flag: true, n: 5, user: { name: 'kim' } },
    roots: [
      { kind: 'setVariable', name: 's1', expression: 'status == "ready"' },
      { kind: 'setVariable', name: 's2', expression: 'status   !=   "done"' },
      { kind: 'setVariable', name: 's3', expression: 'n == "5"' },
      { kind: 'setVariable', name: 's4', expression: 'flag == "true"' },
      { kind: 'setVariable', name: 's5', expression: "user.name == 'kim'" },
      { kind: 'setVariable', name: 's6', expression: '(status == "ready") * 10 + (user.name != "kim")' },
    ],
  });
  eq(fold.status, 0, 'expressions/strings: exits 0');
  const foldVars = Object.fromEntries(setVars(fold.events).map((e) => [e.data?.name, e.data?.value]));
  eq(foldVars.s1, 1, 'expressions/strings: status == "ready" folds to 1');
  eq(foldVars.s2, 1, 'expressions/strings: whitespace-tolerant !=');
  eq(foldVars.s3, 1, 'expressions/strings: number compares against quoted numeral ("5")');
  eq(foldVars.s4, 1, 'expressions/strings: boolean compares against "true" (display form)');
  eq(foldVars.s5, 1, 'expressions/strings: single-quoted literal + nested ref');
  eq(foldVars.s6, 10, 'expressions/strings: folded comparisons compose numerically (1*10 + 0)');

  // ── bespoke: unified references inside larger expressions ──────────────────
  const uni = run({
    version: 1,
    context: { count: 21, doc: { data: [2, 5, 8], meta: { rate: 1.5 } } },
    roots: [
      { kind: 'setVariable', name: 'u1', expression: '${count} * 2' },
      { kind: 'setVariable', name: 'u2', expression: '(doc.data[0] + doc.data[1]) * doc.meta.rate' },
      { kind: 'setVariable', name: 'u3', expression: 'doc.data.count' },
      { kind: 'setVariable', name: 'u4', expression: 'doc.data[1] > doc.data[0] ? doc.data[2] : 0' },
    ],
  });
  eq(uni.status, 0, 'expressions/refs: exits 0');
  const uniVars = Object.fromEntries(setVars(uni.events).map((e) => [e.data?.name, e.data?.value]));
  eq(uniVars.u1, 42, 'expressions/refs: ${count} unwraps inside an expression (21*2)');
  eq(uniVars.u2, 10.5, 'expressions/refs: nested refs compose with parens/precedence ((2+5)*1.5)');
  eq(uniVars.u3, 3, 'expressions/refs: .count pseudo-field (alias of .length)');
  eq(uniVars.u4, 8, 'expressions/refs: ternary over nested refs');

  // Documented trap (locked, not endorsed): an unresolved reference fails the
  // expression, so the condition falls back to the truthy-string heuristic —
  // non-empty text evaluates true. If this ever flips to fail-closed, this
  // assertion is the flag.
  const fb = run({
    version: 1,
    context: { doc: { data: [1] } },
    roots: [
      { kind: 'if', condition: 'doc.missing[0] > 1',
        consequent: [{ kind: 'setVariable', name: 'fellBack', value: 1 }] },
    ],
  });
  eq(fb.status, 0, 'expressions/fallback: exits 0');
  ok(setVars(fb.events).some((e) => e.data?.name === 'fellBack' && e.data?.value === 1),
     'expressions/fallback: unresolved ref → truthy-string fallback takes the branch [BASELINE]');
}

function suiteLoops() {
  console.log('\n── loops ──');

  // for: counter accumulates across iterations (child writes collected by parent)
  const forRun = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'counter', value: 0 },
      {
        kind: 'for', variable: 'i', initial: '0', final: '3', comparator: '<', modifier: '+1', loopLimit: 10,
        items: [{ kind: 'setVariable', name: 'counter', expression: 'counter + 1' }],
      },
      { kind: 'getVariable', name: 'counter', target: 'finalCounter' },
      { kind: 'log', level: 'info', message: 'finalCounter' },
    ],
  });
  eq(forRun.status, 0, 'loops/for: exits 0');
  eq(lastLog(forRun.events)?.message, '3', 'loops/for: body runs 3 times (0,1,2) — counter accumulates to 3');

  // while: decrement to zero
  const whileRun = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'n', value: 5 },
      { kind: 'setVariable', name: 'ticks', value: 0 },
      {
        kind: 'while', condition: 'n > 0', loopLimit: 100,
        items: [
          { kind: 'setVariable', name: 'n', expression: 'n - 1' },
          { kind: 'setVariable', name: 'ticks', expression: 'ticks + 1' },
        ],
      },
      { kind: 'log', level: 'info', message: 'ticks' },
    ],
  });
  eq(whileRun.status, 0, 'loops/while: exits 0');
  eq(lastLog(whileRun.events)?.message, '5', 'loops/while: runs until n hits 0 (5 ticks)');
}

function suitePrevious() {
  console.log('\n── previous ──');
  const isWin = process.platform === 'win32';
  const { status, events } = run({
    version: 1,
    roots: [
      {
        kind: 'shell',
        shell: isWin ? 'pwsh' : 'sh',
        command: isWin ? 'Write-Output prev-chain' : 'printf prev-chain',
        timeoutMs: 30_000,
      },
      { kind: 'log', level: 'info' },
    ],
  });
  eq(status, 0, 'previous: exits 0');
  ok(String(lastLog(events)?.message || '').includes('prev-chain'),
     'previous: shell stdout flows into PREVIOUS → log');

  // log is a transparent tap: it reads PREVIOUS but never overwrites it (the
  // handler emits its own event and never calls result()), so an upstream value
  // survives a log and reaches the next block. This is the llm → log → fs_write
  // contract, exercised here with a deterministic producer in place of llm.
  const tapFile = join(WORK, `tap-${Math.random().toString(36).slice(2)}.txt`);
  const tap = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'x', value: 'answer-from-upstream' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
      { kind: 'getVariable', name: 'PREVIOUS', target: 'afterLog' },
      { kind: 'fsWrite', path: tapFile, storeAs: 'wrote' },
    ],
  });
  eq(tap.status, 0, 'previous: log pass-through chain exits 0');
  eq(getVars(tap.events).find((e) => e.data?.target === 'afterLog')?.data?.value, 'answer-from-upstream',
     'previous: PREVIOUS survives a log untouched (log does not clobber it)');
  ok(existsSync(tapFile) && readFileSync(tapFile, 'utf8') === 'answer-from-upstream',
     'previous: producer → log → fsWrite persists the upstream value (llm → log → fs_write)');
}

function suiteContainers() {
  console.log('\n── containers (group / repurposed runScript) ──');

  // group: transparent container — children run and their vars inherit upward.
  const grp = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'base', value: 1 },
      { kind: 'group', items: [
        { kind: 'setVariable', name: 'inner', expression: 'base + 9' },
      ] },
      { kind: 'getVariable', name: 'inner', target: 'outInner' },
      { kind: 'log', level: 'info', message: 'outInner' },
    ],
  });
  eq(grp.status, 0, 'containers/group: exits 0');
  eq(lastLog(grp.events)?.message, '10', 'containers/group: child runs, var inherits up (base+9=10)');

  // runScript repurposed as a hidden group: still runs its children.
  const legacy = run({
    version: 1,
    roots: [
      { kind: 'runScript', items: [
        { kind: 'setVariable', name: 'fromLegacy', value: 7 },
      ] },
      { kind: 'getVariable', name: 'fromLegacy', target: 'outLegacy' },
      { kind: 'log', level: 'info', message: 'outLegacy' },
    ],
  });
  eq(legacy.status, 0, 'containers/runScript: exits 0 (no legacy warning)');
  eq(lastLog(legacy.events)?.message, '7', 'containers/runScript: repurposed as group, runs children');
  ok(!legacy.events.some((e) => e.kind === 'runScript' && e.status === 'warning'),
     'containers/runScript: no "legacy not executed" warning emitted');
}

function suiteScoping() {
  console.log('\n── scoping (S3: isolated loop frames) ──');

  const { status, events } = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'total', value: 0 },
      {
        kind: 'for', variable: 'i', initial: '0', final: '3', comparator: '<', modifier: '+1', loopLimit: 10,
        items: [
          { kind: 'setVariable', name: 'total', expression: 'total + i' },
          { kind: 'setVariable', name: 'loopLocal', expression: 'i * 100' },
        ],
      },
      { kind: 'getVariable', name: 'total', target: 'outTotal' },
      { kind: 'getVariable', name: 'i', target: 'outI' },
      { kind: 'getVariable', name: 'loopLocal', target: 'outLoopLocal' },
    ],
  });
  eq(status, 0, 'scoping: exits 0');

  const out = (target) => getVars(events).find((e) => e.data?.target === target)?.data?.value;

  // Assignment to an outer variable updates the parent frame — accumulation
  // is preserved (the loop body collects into `total`).
  eq(out('outTotal'), 3, 'scoping: assignment to outer var accumulates (total = 0+1+2 = 3)');

  // The loop counter and a freshly-declared body local live in the isolated
  // loop frame and are discarded on exit, so getVariable resolves null.
  eq(out('outI'), null, 'scoping: loop counter i is isolated (gone after loop)');
  eq(out('outLoopLocal'), null, 'scoping: body-local discarded after loop');
}

function suiteTransform() {
  console.log('\n── transform / validation (param resolver) ──');

  // 1) enum validation: an out-of-range enum value surfaces a non-fatal warning
  //    event listing the offending param; execution still proceeds.
  const enumRun = run({
    version: 1,
    roots: [{ kind: 'log', level: 'bogus', message: 'hi' }],
  });
  eq(enumRun.status, 0, 'transform: invalid enum is non-fatal (exits 0)');
  const enumWarn = enumRun.events.find(
    (e) => e.status === 'warning' && e.kind === 'log' &&
           JSON.stringify(e.data?.diagnostics || []).includes('level'));
  ok(!!enumWarn, 'transform: invalid enum "level" surfaces a validation diagnostic');

  // 2) range clamping: an integer above the declared max surfaces a diagnostic.
  //    `while loopLimit` has range(1,1_000_000); condition false → 0 iterations.
  const clampRun = run({
    version: 1,
    roots: [{ kind: 'while', condition: 'false', loopLimit: 5_000_000, items: [] }],
  });
  eq(clampRun.status, 0, 'transform: out-of-range int is non-fatal (exits 0)');
  const clampWarn = clampRun.events.find(
    (e) => e.status === 'warning' && e.kind === 'while' &&
           JSON.stringify(e.data?.diagnostics || []).includes('loopLimit'));
  ok(!!clampWarn, 'transform: above-max "loopLimit" surfaces a range diagnostic');

  // 3) valid inputs do not over-warn: a clean run emits no validation warnings.
  const cleanRun = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'x', value: 1 },
      { kind: 'log', level: 'info', message: 'x' },
    ],
  });
  ok(!cleanRun.events.some((e) => e.status === 'warning' && e.data?.diagnostics),
     'transform: valid inputs produce no spurious diagnostics');
}

function suiteSimulate() {
  console.log('\n── simulate (suppress side effects, keep dataflow) ──');

  const simPath = join(WORK, `sim-${Math.random().toString(36).slice(2)}.txt`);
  const doc = {
    version: 1,
    roots: [
      { kind: 'fsWrite', path: simPath, content: 'SIMULATED', storeAs: 'wrote' },
      { kind: 'setVariable', name: 'ran', value: 42 },
      { kind: 'getVariable', name: 'wrote', target: 'outWrote' },
      { kind: 'getVariable', name: 'ran', target: 'outRan' },
    ],
  };

  // Simulated: the file must NOT be written, but a predicted event fires and
  // the predicted path flows into the storeAs var; pure blocks still run.
  const sim = run(doc, { simulate: true });
  eq(sim.status, 0, 'simulate: exits 0');
  ok(!existsSync(simPath), 'simulate: side effect suppressed — file NOT written to disk');
  const simEvent = byKind(sim.events, 'fsWrite').find((e) => e.status === 'ok' && e.data?.simulated === true);
  ok(!!simEvent, 'simulate: fsWrite emits an "ok (simulated)" event');
  eq(simEvent?.data?.result, simPath, 'simulate: predicted result is the resolved path');
  const simOut = (t) => getVars(sim.events).find((e) => e.data?.target === t)?.data?.value;
  eq(simOut('outWrote'), simPath, 'simulate: predicted path flows into storeAs (PREVIOUS chain)');
  eq(simOut('outRan'), 42, 'simulate: pure setVariable still runs under simulate');

  // Contrast: the SAME doc without --simulate actually writes the file.
  const real = run(doc);
  eq(real.status, 0, 'simulate: control run (no --simulate) exits 0');
  ok(existsSync(simPath), 'simulate: control run actually writes the file (simulate was the suppressor)');
  rmSync(simPath, { force: true });
}

function suiteConstraints() {
  console.log('\n── constraints (declared param preconditions) ──');

  // 1) fsRead path is declared .must_exist(): reading a missing file is a FATAL
  //    precondition — the resolver skips the handler and surfaces an error.
  const missing = join(WORK, `nope-${Math.random().toString(36).slice(2)}.txt`);
  const r1 = run({ version: 1, roots: [{ kind: 'fsRead', path: missing, storeAs: 'c' }] });
  const errEv = r1.events.find(
    (e) => e.kind === 'fsRead' && e.status === 'error' &&
           JSON.stringify(e.data?.diagnostics || []).includes('does not exist'));
  ok(!!errEv, 'constraints: fsRead missing file → precondition error (does not exist)');
  ok(!r1.events.some((e) => e.kind === 'fsRead' && e.status === 'ok'),
     'constraints: fsRead handler skipped on failed precondition');

  // 2) the same read on a real file passes the precondition and runs cleanly.
  const real = join(WORK, `have-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(real, 'HELLO', 'utf8');
  const r2 = run({ version: 1, roots: [{ kind: 'fsRead', path: real, storeAs: 'c' }] });
  eq(r2.status, 0, 'constraints: fsRead existing file exits 0');
  ok(byKind(r2.events, 'fsRead').some((e) => e.status === 'ok'),
     'constraints: fsRead existing file runs the handler');
  ok(!r2.events.some((e) => e.kind === 'fsRead' && e.status === 'error'),
     'constraints: fsRead existing file → no precondition error');

  // 3) empty path (no PREVIOUS to fall back to) is a fatal "required but empty".
  const r3 = run({ version: 1, roots: [{ kind: 'fsRead', path: '', storeAs: 'c' }] });
  const emptyErr = r3.events.find(
    (e) => e.kind === 'fsRead' && e.status === 'error' &&
           JSON.stringify(e.data?.diagnostics || []).includes('required but empty'));
  ok(!!emptyErr, 'constraints: fsRead empty path → required-but-empty error');

  // 4) fsWrite path is .writable().create_parents(): the resolver builds a
  //    missing parent tree so a write into a non-existent directory succeeds.
  const nested = join(WORK, `nested-${Math.random().toString(36).slice(2)}`, 'deep', 'out.txt');
  const r4 = run({ version: 1, roots: [{ kind: 'fsWrite', path: nested, content: 'X', storeAs: 'w' }] });
  eq(r4.status, 0, 'constraints: fsWrite into missing dir exits 0');
  ok(existsSync(nested), 'constraints: create_parents built the parent tree and wrote the file');

  // 5) under --simulate the effectful create_parents is suppressed: no dir, no file.
  const simDir = join(WORK, `simnest-${Math.random().toString(36).slice(2)}`);
  const simNested = join(simDir, 'deep', 'out.txt');
  const r5 = run({ version: 1, roots: [{ kind: 'fsWrite', path: simNested, content: 'X' }] }, { simulate: true });
  eq(r5.status, 0, 'constraints: simulate fsWrite exits 0');
  ok(!existsSync(simNested), 'constraints: simulate suppresses the write');
  ok(!existsSync(simDir), 'constraints: simulate suppresses effectful create_parents');
}

function suiteGlobs() {
  console.log('\n── globs (feature composition: variables | globs on file_path) ──');

  // Build a deterministic tree: 2 folders + 2 files under an ENV-addressable root.
  // Using ${ENV:..} keeps the test portable (no reliance on a real Desktop) while
  // still exercising the app-level variable resolver + glob expansion + type filter.
  const root = join(WORK, `listroot-${Math.random().toString(36).slice(2)}`);
  mkdirSync(join(root, 'alpha'), { recursive: true });
  mkdirSync(join(root, 'beta'), { recursive: true });
  writeFileSync(join(root, 'gamma.txt'), 'g', 'utf8');
  writeFileSync(join(root, 'delta.log'), 'd', 'utf8');
  const env = { XBLOX_LIST_ROOT: root };

  const names = (events) => {
    const ev = byKind(events, 'fsList').find((e) => e.status === 'ok');
    return { ev, list: Array.isArray(ev?.data?.result) ? ev.data.result.map((x) => x.name).sort() : null };
  };

  // 1) glob + folders-only: `${ENV:ROOT}/*` resolves the variable, expands the
  //    glob to an array of matches, and `only:dir` keeps just the two folders.
  const g1 = run({
    version: 1,
    roots: [
      { kind: 'fsList', path: '${ENV:XBLOX_LIST_ROOT}/*', only: 'dir', storeAs: 'dirs' },
    ],
  }, { env });
  eq(g1.status, 0, 'globs: exits 0');
  const r1 = names(g1.events);
  eq(r1.list, ['alpha', 'beta'], 'globs: ${ENV}/* + only:dir → folders only (alpha, beta)');
  eq(r1.ev?.data?.count, 2, 'globs: count reflects the folders-only filter');

  // 2) same glob, only:file → just the two files (proves the type filter is real).
  const g2 = run({
    version: 1,
    roots: [{ kind: 'fsList', path: '${ENV:XBLOX_LIST_ROOT}/*', only: 'file' }],
  }, { env });
  eq(names(g2.events).list, ['delta.log', 'gamma.txt'], 'globs: only:file → files only');

  // 3) extension glob narrows within a segment: `*.txt` matches one file.
  const g3 = run({
    version: 1,
    roots: [{ kind: 'fsList', path: '${ENV:XBLOX_LIST_ROOT}/*.txt' }],
  }, { env });
  eq(names(g3.events).list, ['gamma.txt'], 'globs: *.txt narrows to a single match');

  // 4) no glob token → plain directory mode still enumerates children (4 entries).
  const g4 = run({
    version: 1,
    roots: [{ kind: 'fsList', path: '${ENV:XBLOX_LIST_ROOT}' }],
  }, { env });
  eq(names(g4.events).list, ['alpha', 'beta', 'delta.log', 'gamma.txt'],
     'globs: non-glob path falls back to directory listing');

  // 5) compose with scope + expressions: a group sets a base var, a for-loop is
  //    a no-op pass, then fsList globs the ENV root for folders. Exercises
  //    set/getVariable + container + loop frames alongside the resolver feature.
  const g5 = run({
    version: 1,
    roots: [
      { kind: 'group', items: [
        { kind: 'setVariable', name: 'wanted', expression: '1 + 1' },
      ] },
      { kind: 'for', variable: 'i', initial: '0', final: '1', comparator: '<', modifier: '+1', loopLimit: 5,
        items: [{ kind: 'setVariable', name: 'touched', expression: 'i + 10' }] },
      { kind: 'fsList', path: '${ENV:XBLOX_LIST_ROOT}/*', only: 'dir', storeAs: 'folders' },
      { kind: 'getVariable', name: 'wanted', target: 'outWanted' },
      { kind: 'getVariable', name: 'folders', target: 'outFolders' },
    ],
  }, { env });
  eq(g5.status, 0, 'globs: scoped flow exits 0');
  eq(getVars(g5.events).find((e) => e.data?.target === 'outWanted')?.data?.value, 2,
     'globs: group-set expression var survives into outer scope');
  const folders = getVars(g5.events).find((e) => e.data?.target === 'outFolders')?.data?.value;
  eq(Array.isArray(folders) ? folders.length : null, 2,
     'globs: glob result (2 folders) flows through storeAs into a variable');
}

function suiteHooks() {
  console.log('\n── hooks (app-level resolve policy: ParamResolve::Custom) ──');

  // The CLI registers a dev hook that denies any path containing the substring
  // in XBLOX_DENY_PATH_SUBSTR (see register_xblox_resolve_hooks). We use it to
  // drive the veto/filter paths deterministically — no reliance on real policy.

  // 1) scalar veto: fsRead on an existing file whose path matches the deny needle
  //    → the resolver vetoes (severity 2), the executor skips the handler.
  const denied = join(WORK, `denyme-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(denied, 'X', 'utf8');
  const r1 = run(
    { version: 1, roots: [{ kind: 'fsRead', path: denied, storeAs: 'c' }] },
    { env: { XBLOX_DENY_PATH_SUBSTR: 'denyme-' } });
  const vetoEv = r1.events.find(
    (e) => e.kind === 'fsRead' && e.status === 'error' &&
           JSON.stringify(e.data?.diagnostics || []).includes('XBLOX_DENY_PATH_SUBSTR'));
  ok(!!vetoEv, 'hooks: guarded path denied by policy → precondition error');
  ok(!r1.events.some((e) => e.kind === 'fsRead' && e.status === 'ok'),
     'hooks: vetoed block handler is skipped (never runs)');

  // 2) control: same read with no deny env → hook allows, block runs clean.
  const r2 = run({ version: 1, roots: [{ kind: 'fsRead', path: denied, storeAs: 'c' }] });
  ok(byKind(r2.events, 'fsRead').some((e) => e.status === 'ok'),
     'hooks: with no policy match the guarded read runs normally');

  // 3) glob filter: a veto on a glob match drops that single entry (non-fatal),
  //    so the listing still succeeds with the surviving folders.
  const root = join(WORK, `hookroot-${Math.random().toString(36).slice(2)}`);
  mkdirSync(join(root, 'keepdir'), { recursive: true });
  mkdirSync(join(root, 'blockdir'), { recursive: true });
  const g = run(
    { version: 1, roots: [{ kind: 'fsList', path: '${ENV:XBLOX_HOOK_ROOT}/*', only: 'dir' }] },
    { env: { XBLOX_HOOK_ROOT: root, XBLOX_DENY_PATH_SUBSTR: 'blockdir' } });
  eq(g.status, 0, 'hooks: glob listing with a filtered entry still exits 0');
  const listed = byKind(g.events, 'fsList').find((e) => e.status === 'ok');
  const gnames = Array.isArray(listed?.data?.result) ? listed.data.result.map((x) => x.name).sort() : null;
  eq(gnames, ['keepdir'], 'hooks: denied glob match filtered out, survivor kept (keepdir)');
}

function suiteIterate() {
  console.log('\n── iterate (composition: any block may carry `items` children) ──');
  const jqOn = featureEnabled('FEATURE_JQ');
  if (!jqOn)
    console.log('  · FEATURE_JQ off in dist/shared/features.json — skipping iterate Parse projections');

  // 1) NESTING PIPELINE — fs_read → Parse(.field) → fs_write.
  //    A non-iterable producer with children runs them once, in a child scope,
  //    with PREVIOUS = its result. Each child consumes its parent's output via
  //    PREVIOUS — no named variables. The whole nest's value is the last child.
  const inFile = join(WORK, `pipe-in-${Math.random().toString(36).slice(2)}.json`);
  const outFile = join(WORK, `pipe-out-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(inFile, JSON.stringify({ nested: { v: 'deep' } }), 'utf8');
  if (jqOn) {
  const pipe = run({
    version: 1,
    roots: [
      {
        kind: 'fsRead', path: inFile, encoding: 'json', storeAs: 'fileContent',
        items: [
          {
            kind: 'Parse', filter: '.nested.v', storeAs: 'picked',
            items: [
              { kind: 'fsWrite', path: outFile, content: '', storeAs: 'wrote' },
            ],
          },
        ],
      },
    ],
  });
  eq(pipe.status, 0, 'iterate/pipe: exits 0');
  eq(byKind(pipe.events, 'fsRead').filter((e) => e.status === 'ok').length, 1,
     'iterate/pipe: producer (fsRead) runs exactly once (not iterated)');
  ok(byKind(pipe.events, 'Parse').some((e) => e.status === 'ok'),
     'iterate/pipe: child Parse runs with PREVIOUS = parent result');
  ok(byKind(pipe.events, 'fsWrite').some((e) => e.status === 'ok'),
     'iterate/pipe: grandchild fsWrite runs with PREVIOUS = parsed selection');
  ok(existsSync(outFile) && readFileSync(outFile, 'utf8') === 'deep',
     'iterate/pipe: PREVIOUS chains down nesting → fs_write persists "deep"');
  }

  // 2) ITERABLE FAN-OUT — fsList (iterable) runs its `items` once per entry,
  //    binding a numeric `index`. Children logging `index` count the passes.
  const dirIn = join(WORK, `iter-${Math.random().toString(36).slice(2)}`);
  mkdirSync(dirIn, { recursive: true });
  for (const n of ['a.txt', 'b.txt', 'c.txt']) writeFileSync(join(dirIn, n), n, 'utf8');

  const fan = run({
    version: 1,
    roots: [
      {
        kind: 'fsList', path: dirIn, only: 'file', storeAs: 'entries',
        items: [{ kind: 'log', level: 'info', message: 'index' }],
      },
    ],
  });
  eq(fan.status, 0, 'iterate/fan: exits 0');
  const idxLogs = logs(fan.events).map((e) => e.message).sort();
  eq(idxLogs, ['0', '1', '2'], 'iterate/fan: children run once per entry; index = 0,1,2');

  // 3) PREVIOUS = element + projection — each pass binds PREVIOUS to the entry
  //    object; a child Parse(.name) pulls the filename, proving no pluck is
  //    needed on the iterable (the Parse block does field projection).
  if (jqOn) {
  const proj = run({
    version: 1,
    roots: [
      {
        kind: 'fsList', path: dirIn, only: 'file',
        items: [
          {
            kind: 'Parse', filter: '.name', storeAs: 'nm',
            items: [{ kind: 'log', level: 'info' }],
          },
        ],
      },
    ],
  });
  eq(proj.status, 0, 'iterate/proj: exits 0');
  eq(logs(proj.events).map((e) => e.message).sort(), ['a.txt', 'b.txt', 'c.txt'],
     'iterate/proj: PREVIOUS = entry object; child Parse(.name) projects each filename');
  }

  // 4) NO CHILDREN — a childless producer is byte-for-byte unchanged: fsList
  //    without items just lists and emits no child events.
  const bare = run({
    version: 1,
    roots: [{ kind: 'fsList', path: dirIn, only: 'file', storeAs: 'entries' }],
  });
  eq(bare.status, 0, 'iterate/bare: childless fsList exits 0');
  ok(logs(bare.events).length === 0 && !byKind(bare.events, 'Parse').length,
     'iterate/bare: no children → no child events (no behavior change)');
  const bareList = byKind(bare.events, 'fsList').find((e) => e.status === 'ok');
  eq(bareList?.data?.count, 3, 'iterate/bare: childless fsList still lists all 3 files');
}

function suiteNetwork() {
  console.log('\n── network (native BlockResult + typed outputs; file:// offline) ──');

  // 1) RAW FETCH — body flows into result/PREVIOUS+storeAs; `statusCode` is a
  //    typed *named* output (io.set(name, kind, value)) readable as a variable.
  const txt = join(WORK, `net-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(txt, 'hello-from-file', 'utf8');
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: fileUrl(txt), decode: 'raw', storeAs: 'resp' },
      { kind: 'getVariable', name: 'statusCode', target: 'code' },
      { kind: 'getVariable', name: 'resp', target: 'body' },
    ],
  });
  eq(r1.status, 0, 'network/raw: exits 0');
  const okEv = byKind(r1.events, 'network').find((e) => e.status === 'ok');
  ok(!!okEv, 'network/raw: emits an ok network event');
  eq(okEv?.data?.result, 'hello-from-file', 'network/raw: body flows into result/PREVIOUS');
  const gv = (t) => getVars(r1.events).find((e) => e.data?.target === t)?.data?.value;
  eq(gv('body'), 'hello-from-file', 'network/raw: storeAs var holds the body');
  eq(gv('code'), 0, 'network/raw: statusCode typed output is a readable variable (0 for file://)');

  // 2) JSON DECODE + dotted parse selector → the selected value is the result.
  const jsonFile = join(WORK, `net-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(jsonFile, JSON.stringify({ nested: { v: 'deep' } }), 'utf8');
  const r2 = run({
    version: 1,
    roots: [
      { kind: 'httpRequest', url: fileUrl(jsonFile), decode: 'json', parse: '.nested.v', storeAs: 'picked' },
      { kind: 'log', level: 'info' },
    ],
  });
  eq(r2.status, 0, 'network/json: exits 0');
  eq(lastLog(r2.events)?.message, 'deep', 'network/json: decode:json + parse:.nested.v selects "deep"');

  // 3) parse selector with no match → BlockResult::Error (continueOnError keeps
  //    the run alive so we can assert on the emitted error).
  const r3 = run({
    version: 1,
    roots: [{ kind: 'httpRequest', url: fileUrl(jsonFile), decode: 'json', parse: '.missing', continueOnError: true }],
  });
  ok(byKind(r3.events, 'network').some(
       (e) => e.status === 'error' && String(e.message || '').includes('no value')),
     'network/json: unmatched parse filter → error');

  // 4) FAILURE — a missing file makes the handler return BlockResult::Error; the
  //    default policy is abort-on-error, so the trailing block must NOT run.
  const missing = fileUrl(join(WORK, `does-not-exist-${Math.random().toString(36).slice(2)}.txt`));
  const r4 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: missing, decode: 'raw', retries: 0 },
      { kind: 'setVariable', name: 'after', value: 99 },
    ],
  });
  ok(byKind(r4.events, 'network').some((e) => e.status === 'error'),
     'network/error: missing file → network error event');
  ok(!setVars(r4.events).some((e) => e.data?.name === 'after'),
     'network/error: Error result aborts the list (trailing setVariable skipped)');
  ok(r4.status !== 0, 'network/error: run exits non-zero on unhandled error');

  // 5) SAME FAILURE + continueOnError → the executor's error policy lets the
  //    next block run (the other half of BlockResult::Error flow control).
  const r5 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: missing, decode: 'raw', retries: 0, continueOnError: true },
      { kind: 'setVariable', name: 'after', value: 99 },
    ],
  });
  ok(setVars(r5.events).some((e) => e.data?.name === 'after' && e.data?.value === 99),
     'network/error: continueOnError lets the trailing setVariable run');
}

function suiteXbloxRun() {
  console.log('\n── xbloxRun (nested XBlox script runner block) ──');

  const info = xbloxInfo();
  const block = (info.blocks || []).find((b) => b.kind === 'xbloxRun');
  ok(!!block, 'xbloxRun/manifest: block is registered');
  const params = new Map((block?.params || []).map((p) => [p.name, p]));
  ok((params.get('action')?.enum_values || params.get('action')?.enumValues || []).includes('Stop'),
     'xbloxRun/manifest: action exposes Run/Stop/Status lifecycle');
  ok(params.get('path')?.kind === 'file_path', 'xbloxRun/manifest: path is a file picker param');
  ok(params.get('context')?.kind === 'json_value', 'xbloxRun/manifest: context is a JSON param');
  ok(params.get('args')?.kind === 'args_list', 'xbloxRun/manifest: args is an args-list param');
  ok(params.get('context')?.ui?.control === 'xblox_context' &&
     params.get('context')?.ui?.options_path_template === 'xblox.scriptContext.{path}',
     'xbloxRun/manifest: context declares script-context lazy source');
  ok(!params.get('args')?.ui?.options_path_template,
     'xbloxRun/manifest: args stays manual instead of lazy script-context population');
  ok((params.get('logLevel')?.enum_values || params.get('logLevel')?.enumValues || []).includes('info'),
     'xbloxRun/manifest: logLevel exposes host logging levels');
  ok(params.has('loop') && params.has('loopIntervalMs') && params.has('background'),
     'xbloxRun/manifest: loop, interval, and background controls are exposed');
  ok(params.get('reload')?.kind === 'boolean',
     'xbloxRun/manifest: reload toggle is exposed for disk refresh');
  ok((block?.childLists || []).some((c) => c?.key === 'items'),
     'xbloxRun/manifest: exposes child items for follow-up blocks');

  const minimalChild = join(WORK, `xblox-run-min-${Math.random().toString(36).slice(2)}.xblox`);
  writeFileSync(minimalChild, JSON.stringify({
    version: 1,
    context: { CURRENT_FILE: '${CURRENT_FILE}', COUNT: 3 },
    roots: [
      { kind: 'setVariable', name: 'childValue', value: 'nested-ok' },
      { kind: 'getVariable', name: 'childValue', target: 'out' },
    ],
  }, null, 2));
  const scriptContext = xbloxInfoOptions(`xblox.scriptContext.${minimalChild}`);
  eq(scriptContext.status, 0, 'xbloxRun/context-options: resolver exits 0 for selected script');
  const currentFileOpt = Array.isArray(scriptContext.options)
    ? scriptContext.options.find((o) => o?.name === 'CURRENT_FILE')
    : null;
  eq(currentFileOpt?.default, '${CURRENT_FILE}', 'xbloxRun/context-options: selected script context defaults are exposed');
  const minimal = run({
    version: 1,
    roots: [
      { kind: 'xbloxRun', path: minimalChild, storeAs: 'child', logLevel: 'off' },
      { kind: 'getVariable', name: 'child.ok', target: 'childOk' },
      { kind: 'getVariable', name: 'child.lastBlockResult', target: 'childReturn' },
    ],
  });
  eq(minimal.status, 0, 'xbloxRun/minimal: parent run exits 0');
  const minimalEv = byKind(minimal.events, 'xbloxRun').find((e) => e.status === 'ok');
  eq(minimalEv?.data?.result?.lastBlockResult, 'nested-ok',
     'xbloxRun/minimal: nested script result flows into block result');
  const parentRead = (target) => getVars(minimal.events).find((e) => e.data?.target === target)?.data?.value;
  eq(parentRead('childOk'), true, 'xbloxRun/storeAs: returned JSON is stored as an object');
  eq(parentRead('childReturn'), 'nested-ok', 'xbloxRun/storeAs: nested JSON fields are addressable after storeAs');

  const cacheChild = join(WORK, `xblox-run-cache-${Math.random().toString(36).slice(2)}.xblox`);
  const cacheDoc = (value) => JSON.stringify({
    version: 1,
    roots: [{ kind: 'getVariable', name: 'value', target: 'childValue' }],
    context: { value },
  }, null, 2);
  writeFileSync(cacheChild, cacheDoc('one'));
  const cacheRun = run({
    version: 1,
    roots: [
      { kind: 'xbloxRun', path: cacheChild, storeAs: 'first' },
      { kind: 'getVariable', name: 'first.lastBlockResult', target: 'firstValue' },
      { kind: 'fsWrite', path: cacheChild, content: cacheDoc('two') },
      { kind: 'xbloxRun', path: cacheChild, storeAs: 'cached' },
      { kind: 'getVariable', name: 'cached.lastBlockResult', target: 'cachedValue' },
      { kind: 'xbloxRun', path: cacheChild, reload: true, storeAs: 'reloaded' },
      { kind: 'getVariable', name: 'reloaded.lastBlockResult', target: 'reloadedValue' },
    ],
  });
  eq(cacheRun.status, 0, 'xbloxRun/cache: parent run exits 0');
  const cacheRead = (target) => getVars(cacheRun.events).find((e) => e.data?.target === target)?.data?.value;
  eq(cacheRead('firstValue'), 'one', 'xbloxRun/cache: first child run reads disk');
  eq(cacheRead('cachedValue'), 'one', 'xbloxRun/cache: default run reuses cached child document');
  eq(cacheRead('reloadedValue'), 'two', 'xbloxRun/cache: reload=true refreshes child document from disk');

  const childBlocks = run({
    version: 1,
    roots: [{
      kind: 'xbloxRun',
      path: minimalChild,
      items: [
        { kind: 'Parse', filter: '.lastBlockResult', storeAs: 'picked' },
        { kind: 'getVariable', name: 'picked', target: 'pickedOut' },
      ],
    }],
  });
  eq(childBlocks.status, 0, 'xbloxRun/items: parent run exits 0');
  eq(getVars(childBlocks.events).find((e) => e.data?.target === 'pickedOut')?.data?.value, 'nested-ok',
     'xbloxRun/items: child blocks run with PREVIOUS = xbloxRun JSON result');

  const argsChild = join(WORK, `xblox-run-args-${Math.random().toString(36).slice(2)}.xblox`);
  writeFileSync(argsChild, JSON.stringify({
    version: 1,
    context: { NAME: 'default', COUNT: 0 },
    roots: [
      { kind: 'getVariable', name: 'NAME', target: 'seenName' },
      { kind: 'getVariable', name: 'COUNT', target: 'seenCount' },
      { kind: 'getVariable', name: 'flag', target: 'seenFlag' },
      { kind: 'getVariable', name: 'args', target: 'seenArgs' },
    ],
  }, null, 2));
  const argsRun = run({
    version: 1,
    roots: [{
      kind: 'xbloxRun',
      path: argsChild,
      context: { NAME: 'from-context', COUNT: 2 },
      args: ['--NAME', 'from-args', '--COUNT=7', '--flag'],
      storeAs: 'child',
    }],
  });
  eq(argsRun.status, 0, 'xbloxRun/args: exits 0');
  const argsResult = byKind(argsRun.events, 'xbloxRun').find((e) => e.status === 'ok')?.data?.result;
  const argEvents = argsResult?.events || [];
  const childVar = (target) => argEvents.find((e) => e.kind === 'getVariable' && e.data?.target === target)?.data?.value;
  eq(childVar('seenName'), 'from-args', 'xbloxRun/args: CLI-style args override context values');
  eq(childVar('seenCount'), 7, 'xbloxRun/args: --COUNT=7 parses as number');
  eq(childVar('seenFlag'), true, 'xbloxRun/args: bare --flag parses as boolean true');
  eq(childVar('seenArgs')?.NAME, 'from-args', 'xbloxRun/args: reserved args map is available in child context');

  const loopChild = join(WORK, `xblox-run-loop-${Math.random().toString(36).slice(2)}.xblox`);
  writeFileSync(loopChild, JSON.stringify({
    version: 1,
    context: { n: 0 },
    roots: [
      { kind: 'setVariable', name: 'n', expression: 'n + 1' },
      { kind: 'if', condition: 'n >= 3', consequent: [{ kind: 'exit', code: 0 }] },
    ],
  }, null, 2));
  const loopRun = run({
    version: 1,
    roots: [{ kind: 'xbloxRun', path: loopChild, loop: true, loopIntervalMs: 0, storeAs: 'looped' }],
  }, { timeout: 10_000 });
  eq(loopRun.status, 0, 'xbloxRun/loop: child document loop can be enabled with interval 0');
  const loopResult = byKind(loopRun.events, 'xbloxRun').find((e) => e.status === 'ok')?.data?.result;
  ok((loopResult?.eventCount || 0) >= 3,
     'xbloxRun/loop: loop runs multiple passes before child exit stops it');

  const spawnChild = join(WORK, `xblox-run-spawn-${Math.random().toString(36).slice(2)}.xblox`);
  writeFileSync(spawnChild, JSON.stringify({
    version: 1,
    roots: [{ kind: 'wait', ms: 0 }],
  }, null, 2));
  const perfStart = performance.now();
  const perfRun = run({
    version: 1,
    roots: [{ kind: 'xbloxRun', path: spawnChild, logLevel: 'off' }],
  }, { timeout: 5_000 });
  const perfElapsedMs = performance.now() - perfStart;
  console.log(`  · xbloxRun spawn smoke: elapsed=${Math.round(perfElapsedMs)}ms`);
  reportNotes.push(`xbloxRun spawn smoke: elapsed=${Math.round(perfElapsedMs)}ms`);
  eq(perfRun.status, 0, 'xbloxRun/perf: nested spawn smoke exits 0');

  const bgChild = join(WORK, `xblox-run-bg-${Math.random().toString(36).slice(2)}.xblox`);
  const bgOut = join(WORK, `xblox-run-bg-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(bgChild, JSON.stringify({
    version: 1,
    roots: [
      { kind: 'wait', ms: 50 },
      { kind: 'fsWrite', path: '${outPath}', content: 'background-ok' },
    ],
  }, null, 2));
  const bgRun = run({
    version: 1,
    roots: [
      { kind: 'xbloxRun', path: bgChild, context: { outPath: bgOut }, background: true, storeAs: 'launch' },
      { kind: 'xbloxRun', action: 'Status', path: bgChild, storeAs: 'bgStatus' },
      { kind: 'wait', ms: 1500 },
      { kind: 'fsRead', path: bgOut, storeAs: 'bgText' },
    ],
  }, { timeout: 15_000, wait: true });
  eq(bgRun.status, 0, 'xbloxRun/background: detached child can complete while parent continues');
  const bgLaunch = byKind(bgRun.events, 'xbloxRun').find((e) => e.status === 'ok');
  eq(bgLaunch?.data?.result?.background, true, 'xbloxRun/background: launch result marks the child as background');
  const bgStatus = byKind(bgRun.events, 'xbloxRun').find((e) => e.data?.result?.action === 'status');
  eq(bgStatus?.data?.result?.running, true, 'xbloxRun/status: background run is keyed by path');
  const bgRead = byKind(bgRun.events, 'fsRead').find((e) => e.status === 'ok');
  eq(bgRead?.data?.result, 'background-ok', 'xbloxRun/background: background script side effect is visible');

  const stopChild = join(WORK, `xblox-run-stop-${Math.random().toString(36).slice(2)}.xblox`);
  writeFileSync(stopChild, JSON.stringify({
    version: 1,
    roots: [
      { kind: 'wait', ms: 5000 },
    ],
  }, null, 2));
  const stopRun = run({
    version: 1,
    roots: [
      { kind: 'xbloxRun', path: stopChild, background: true, storeAs: 'launch' },
      { kind: 'xbloxRun', action: 'Status', path: stopChild, storeAs: 'statusBefore' },
      { kind: 'xbloxRun', action: 'Stop', path: stopChild, storeAs: 'stopReply' },
      { kind: 'xbloxRun', action: 'Status', path: stopChild, storeAs: 'statusAfter' },
    ],
  }, { timeout: 10_000, wait: true });
  eq(stopRun.status, 0, 'xbloxRun/stop: stop flow exits 0');
  const lifecycleEvents = byKind(stopRun.events, 'xbloxRun').map((e) => e.data?.result);
  ok(lifecycleEvents.some((r) => r?.action === 'status' && r?.running === true),
     'xbloxRun/status: sees path-keyed run before stop');
  ok(lifecycleEvents.some((r) => r?.action === 'stop' && r?.stopped === true),
     'xbloxRun/stop: cancels path-keyed background run');
  ok(lifecycleEvents.some((r) => r?.action === 'status' && r?.running === false),
     'xbloxRun/status: reports stopped after stop');
}

async function suiteIpcCom() {
  console.log('\n── ipc-com (XBlox session blocks; server/client fixture smoke) ──');
  if (skipUnlessFeature('FEATURE_COMMAND_IPC_NEXT', 'ipc-com suite')) return;
  const rep = beginReportSuite('XBlox IPC communication');

  const info = xbloxInfo();
  const blocks = Array.isArray(info.blocks) ? info.blocks : [];
  for (const kind of ['ipcSession', 'ipcSessions', 'ipcSend', 'ipcSendBatch', 'ipcBroadcast', 'ipcReceive']) {
    const block = blocks.find((b) => b && b.kind === kind);
    ok(!!block, `ipc-com/manifest: ${kind} is registered`);
    if (kind === 'ipcSessions' || kind === 'ipcReceive') {
      ok((block?.execFlags || {}).iterable === true,
         `ipc-com/manifest: ${kind} declares iterable child fan-out`);
    }
  }

  const fixtures = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox');
  const serverSrc = join(WORK, `ipc-server-${Math.random().toString(36).slice(2)}.xblox`);
  const clientSrc = join(WORK, `ipc-client-${Math.random().toString(36).slice(2)}.xblox`);
  const runTag = Math.random().toString(36).slice(2);
  const serverKey = `name:ipc-server-${runTag}`;
  const clientKey = `name:ipc-client-${runTag}`;
  const serverControlKey = xbloxFileSessionKey(serverSrc);
  const clientControlKey = xbloxFileSessionKey(clientSrc);
  const isThisRunIpcInstance = (d) => {
    const cmdline = String(d?.cmdline || '');
    return cmdline.includes(serverSrc) || cmdline.includes(clientSrc);
  };

  const writePairFixture = (fixtureName, dest) => {
    const doc = JSON.parse(readFileSync(join(fixtures, fixtureName), 'utf8'));
    doc.context = { ...(doc.context || {}), serverKey, clientKey };
    for (const block of doc.roots || []) {
      if (block?.kind === 'ipcReceive' && block.waitMs != null && block.waitMs < 1000)
        block.waitMs = 1000;
    }
    writeFileSync(dest, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
  };
  writePairFixture('ipc-server.xblox', serverSrc);
  writePairFixture('ipc-client.xblox', clientSrc);

  const server = spawnXbloxRun(serverSrc);
  const client = spawnXbloxRun(clientSrc);
  try {
    const functionalStart = performance.now();
    const liveServer = await waitFor(() => listSessions().find((d) => d.key === serverKey), { timeoutMs: 8000 });
    const liveClient = await waitFor(() => listSessions().find((d) => d.key === clientKey), { timeoutMs: 8000 });
    ok(!!liveServer, 'ipc-com/server: looping server fixture registers a session');
    ok(!!liveClient, 'ipc-com/client: looping client fixture registers a session');
    eq(liveServer?.key, serverKey, 'ipc-com/server: loop advertises declared named session');
    eq(liveClient?.key, clientKey, 'ipc-com/client: loop advertises declared named session');

    const clientVars = await waitFor(() => {
      const r = sessionCli(['vars', '--key', clientKey, '--json']);
      const vars = parseAnyJson(r.stdout);
      const replies = Array.isArray(vars?.replies) ? vars.replies : [];
      const current = vars?.CURRENT && typeof vars.CURRENT === 'object' ? [vars.CURRENT] : [];
      const acks = [...replies, ...current];
      return acks.some((m) => m?.message === 'server-ack' && m?.from === serverKey)
        ? vars
        : null;
    }, { timeoutMs: 10_000, intervalMs: 150 });
    ok(!!clientVars, 'ipc-com/roundtrip: client receives server-ack with echoed payload');

    const serverVars = await waitFor(() => {
      const r = sessionCli(['vars', '--key', serverKey, '--json']);
      const vars = parseAnyJson(r.stdout);
      const inbox = Array.isArray(vars?.inbox) ? vars.inbox : [];
      return inbox.some((m) => m?.message === 'client-ping' && m?.from === clientKey)
        ? vars
        : null;
    }, { timeoutMs: 4000, intervalMs: 150 });
    ok(!!serverVars, 'ipc-com/server: server fixture receives client-ping from client session');

    const listRun = run({
      version: 1,
      roots: [
        {
          kind: 'ipcSessions',
          storeAs: 'sessions',
          items: [{ kind: 'log', level: 'info', input: 'CURRENT', filter: '.key' }],
        },
      ],
    });
    eq(listRun.status, 0, 'ipc-com/ipcSessions: exits 0');
    ok(logs(listRun.events).some((e) => e.message === serverKey || e.message === clientKey),
       'ipc-com/ipcSessions: child blocks iterate over live session descriptors');
    reportStep(rep, 'functional server/client roundtrip', functionalStart,
      `server=${serverKey}; client=${clientKey}`);
  } finally {
    sessionCli(['stop', '--key', clientKey, '--json'], 5000);
    sessionCli(['stop', '--key', serverKey, '--json'], 5000);
    sessionCli(['stop', '--key', clientControlKey, '--json'], 5000);
    sessionCli(['stop', '--key', serverControlKey, '--json'], 5000);
    await waitFor(() => (server.exited && client.exited ? true : false), { timeoutMs: 4000, intervalMs: 100 });
    if (!client.exited) client.kill();
    if (!server.exited) server.kill();
    const cleanedRunIpc = await waitFor(() => {
      const mine = listRunIpcInstances().filter(isThisRunIpcInstance);
      return mine.length === 0 ? true : false;
    }, { timeoutMs: 4000, intervalMs: 150 });
    if (!cleanedRunIpc) {
      const staleMine = listRunIpcInstances().filter(isThisRunIpcInstance);
      ok(staleMine.length === 0,
         `ipc-com/run-ipc: no stale descriptors for server/client runs (left ${staleMine.length})`);
    } else {
      ok(true, 'ipc-com/run-ipc: server/client run-ipc descriptors are cleaned up');
    }
  }

  const throughputStart = performance.now();
  const durationMs = 3000;
  const batchSize = 256;
  const tServerSrc = join(WORK, `ipc-throughput-server-${Math.random().toString(36).slice(2)}.xblox`);
  const tClientSrc = join(WORK, `ipc-throughput-client-${Math.random().toString(36).slice(2)}.xblox`);
  const throughputTag = Math.random().toString(36).slice(2);
  const tServerKey = `name:ipc-throughput-server-${throughputTag}`;
  const tClientKey = `name:ipc-throughput-client-${throughputTag}`;
  const tServerControlKey = xbloxFileSessionKey(tServerSrc);
  const tClientControlKey = xbloxFileSessionKey(tClientSrc);
  const throughputServerDoc = {
    version: 1,
    context: {
      serverKey: tServerKey,
      clientKey: tClientKey,
      serverReceived: 0,
      serverSent: 0,
    },
    loop: { enabled: true, intervalMs: 0, reset: false },
    roots: [
      { kind: 'ipcSession', name: '${serverKey}', storeAs: 'serverSession' },
      {
        kind: 'ipcReceive',
        target: '${serverKey}',
        waitMs: 0,
        storeAs: 'serverInbox',
      },
      { kind: 'setVariable', name: 'serverReceived', expression: 'serverReceived + serverInbox.length' },
      {
        kind: 'ipcSendBatch',
        target: '${clientKey}',
        from: '${serverKey}',
        message: 'pong',
        source: 'serverInbox',
        storeAs: 'lastReplyBatch',
      },
      { kind: 'setVariable', name: 'serverSent', expression: 'serverSent + serverInbox.length' },
    ],
  };
  const throughputClientDoc = {
    version: 1,
    context: {
      serverKey: tServerKey,
      clientKey: tClientKey,
      clientSent: 0,
      clientReceived: 0,
    },
    loop: { enabled: true, intervalMs: 0, reset: false },
    roots: [
      { kind: 'ipcSession', name: '${clientKey}', storeAs: 'clientSession' },
      {
        kind: 'ipcSendBatch',
        target: '${serverKey}',
        from: '${clientKey}',
        message: 'ping',
        count: batchSize,
        storeAs: 'lastSendBatch',
      },
      { kind: 'setVariable', name: 'clientSent', expression: `clientSent + ${batchSize}` },
      {
        kind: 'ipcReceive',
        target: '${clientKey}',
        waitMs: 0,
        storeAs: 'clientInbox',
      },
      { kind: 'setVariable', name: 'clientReceived', expression: 'clientReceived + clientInbox.length' },
    ],
  };
  writeFileSync(tServerSrc, `${JSON.stringify(throughputServerDoc, null, 2)}\n`, 'utf8');
  writeFileSync(tClientSrc, `${JSON.stringify(throughputClientDoc, null, 2)}\n`, 'utf8');

  const tServer = spawnXbloxRun(tServerSrc);
  let tClient = null;
  try {
    const liveThroughputServer = await waitFor(() => listSessions().find((d) => d.key === tServerKey), { timeoutMs: 8000 });
    ok(!!liveThroughputServer, 'ipc-com/throughput: zero-interval server registers a session');
    tClient = spawnXbloxRun(tClientSrc);
    const liveThroughputClient = await waitFor(() => listSessions().find((d) => d.key === tClientKey), { timeoutMs: 8000 });
    ok(!!liveThroughputClient, 'ipc-com/throughput: zero-interval client registers a session');

    const measureStart = performance.now();
    await sleep(durationMs);
    const elapsedSec = Math.max(0.001, (performance.now() - measureStart) / 1000);
    const readVars = (key) => parseAnyJson(sessionCli(['vars', '--key', key, '--json']).stdout) || {};
    const serverVars = readVars(tServerKey);
    const clientVars = readVars(tClientKey);
    const serverReceived = Number(serverVars.serverReceived || 0);
    const serverSent = Number(serverVars.serverSent || 0);
    const clientSent = Number(clientVars.clientSent || 0);
    const clientReceived = Number(clientVars.clientReceived || 0);
    const serverRecvPerSec = serverReceived / elapsedSec;
    const roundtripPerSec = clientReceived / elapsedSec;
    const clientSendPerSec = clientSent / elapsedSec;
    const detail = [
      `window=${Math.round(elapsedSec * 1000)}ms`,
      `batch=${batchSize}`,
      `clientSent=${clientSent} (${Math.round(clientSendPerSec)}/s)`,
      `serverReceived=${serverReceived} (${Math.round(serverRecvPerSec)}/s)`,
      `serverSent=${serverSent}`,
      `clientReceived=${clientReceived} (${Math.round(roundtripPerSec)}/s)`,
    ].join('; ');

    console.log(`  · ipc throughput: ${detail}`);
    reportStep(rep, 'max throughput: loop interval 0ms server/client', throughputStart, detail);
    reportNotes.push(`IPC max throughput (0 ms loop interval): ${detail}`);
    ok(serverReceived > 0, 'ipc-com/throughput: server receives messages at 0 ms loop interval');
    ok(clientReceived > 0, 'ipc-com/throughput: client receives replies at 0 ms loop interval');
  } finally {
    sessionCli(['stop', '--key', tClientKey, '--json'], 5000);
    sessionCli(['stop', '--key', tServerKey, '--json'], 5000);
    sessionCli(['stop', '--key', tClientControlKey, '--json'], 5000);
    sessionCli(['stop', '--key', tServerControlKey, '--json'], 5000);
    await waitFor(() => (tServer.exited && (!tClient || tClient.exited) ? true : false), { timeoutMs: 4000, intervalMs: 100 });
    if (tClient && !tClient.exited) tClient.kill();
    if (!tServer.exited) tServer.kill();
  }

  endReportSuite(rep);
}

function suiteVideo() {
  console.log('\n── video (migrated; screen + device capture, hardware-tolerant) ──');
  if (skipUnlessAnyFeature(['FEATURE_VIDEO', 'FEATURE_VIDEO_DETECTION'], 'video suite')) return;

  // Probe: is the video feature compiled in? An unknown kind emits a warning
  // event ("unknown block kind skipped") instead of a real handler result.
  const probe = run({ version: 1, roots: [{ kind: 'videoListDevices', storeAs: 'cams' }] });
  if (probe.events.some((e) => e.kind === 'videoListDevices' && e.status === 'warning' &&
                               String(e.message || '').includes('unknown'))) {
    console.log('  · FEATURE_VIDEO off — skipping video suite');
    return;
  }

  // videoListDevices always resolves (array result, possibly empty w/o a camera).
  const okDev = byKind(probe.events, 'videoListDevices').find((e) => e.status === 'ok');
  ok(!!okDev, 'video/list: videoListDevices emits ok');
  const devices = Array.isArray(okDev?.data?.result) ? okDev.data.result : null;
  ok(Array.isArray(devices), 'video/list: result is an array of devices');

  // Device capture (webcam still) — gated on a real camera being present so the
  // suite stays green on headless machines. With a cam: capture → file + path.
  if (devices && devices.length > 0) {
    const shot = join(WORK, `cam-${Math.random().toString(36).slice(2)}.jpg`);
    const cap = run({
      version: 1,
      roots: [
        { kind: 'videoCapture', outputPath: shot, timeoutMs: 6000, storeAs: 'shot' },
        { kind: 'log', level: 'info' },
      ],
    });
    const okCap = byKind(cap.events, 'videoCapture').find((e) => e.status === 'ok');
    ok(!!okCap, 'video/capture: webcam videoCapture emits ok (camera present)');
    eq(okCap?.data?.result, shot, 'video/capture: result is the output path (→ PREVIOUS/storeAs)');
    ok(existsSync(shot), 'video/capture: still frame written to disk');
    eq(lastLog(cap.events)?.message, shot, 'video/capture: path flows through PREVIOUS into log');
  } else {
    console.log('  · no camera present — skipping webcam capture asserts');
  }

  // Screen input (Win32 + FEATURE_VIDEO_DETECTION). Gated on videoListScreens
  // resolving (detection feature on, real display session).
  if (process.platform === 'win32') {
    const screens = run({ version: 1, roots: [{ kind: 'videoListScreens', storeAs: 's' }] });
    const okScreens = byKind(screens.events, 'videoListScreens').find((e) => e.status === 'ok');
    if (okScreens && Array.isArray(okScreens?.data?.result?.monitors)) {
      ok(true, 'video/screens: videoListScreens returns a monitors[] array');

      const shot = join(WORK, `screen-${Math.random().toString(36).slice(2)}.jpg`);
      const cap = run({
        version: 1,
        roots: [{ kind: 'videoScreenCapture', input: 'screen:0', outputPath: shot, storeAs: 'shot' }],
      });
      const okCap = byKind(cap.events, 'videoScreenCapture').find((e) => e.status === 'ok');
      ok(!!okCap, 'video/screen: videoScreenCapture input=screen:0 emits ok');
      eq(okCap?.data?.result, shot, 'video/screen: result is the output path');
      ok(existsSync(shot), 'video/screen: screenshot written to disk');

      // Error path: empty input → BlockResult::Error; continueOnError keeps going.
      const err = run({
        version: 1,
        roots: [
          { kind: 'videoScreenCapture', input: '', continueOnError: true },
          { kind: 'setVariable', name: 'after', value: 1 },
        ],
      });
      ok(byKind(err.events, 'videoScreenCapture').some((e) => e.status === 'error'),
         'video/screen: empty input → error event (BlockResult::Error)');
      ok(setVars(err.events).some((e) => e.data?.name === 'after'),
         'video/screen: continueOnError lets the next block run');
    } else {
      console.log('  · videoListScreens unsupported here — skipping screen-input asserts');
    }
  }
}

function suiteAudio() {
  console.log('\n── audio (migrated; BlockResult + typed outputs, hardware-tolerant) ──');
  if (skipUnlessAnyFeature(['FEATURE_COMMAND_AUDIO', 'FEATURE_STT', 'FEATURE_AUDIO_DEVICES'], 'audio suite')) return;

  // Probe: is the audio (STT/TTS) feature compiled in? Unknown kinds emit a
  // "unknown block kind skipped" warning instead of a real handler result.
  const probe = run({ version: 1, roots: [{ kind: 'audioListDevices', storeAs: 'mics' }] });
  if (probe.events.some((e) => e.kind === 'audioListDevices' && e.status === 'warning' &&
                               String(e.message || '').includes('unknown'))) {
    console.log('  · FEATURE_STT off — skipping audio suite');
    return;
  }

  // audioListDevices always resolves (array result, possibly empty w/o a mic).
  const okList = byKind(probe.events, 'audioListDevices').find((e) => e.status === 'ok');
  ok(!!okList, 'audio/list: audioListDevices emits ok');
  const devices = Array.isArray(okList?.data?.result) ? okList.data.result : null;
  ok(Array.isArray(devices), 'audio/list: result is an array of capture devices');

  // audioPlay with no path → BlockResult::Error (code 2); default policy aborts
  // the list, so the trailing setVariable must NOT run.
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'audioPlay', path: '' },
      { kind: 'setVariable', name: 'after', value: 1 },
    ],
  });
  ok(byKind(r1.events, 'audioPlay').some((e) => e.status === 'error'),
     'audio/play: empty path → error event (BlockResult::Error)');
  ok(!setVars(r1.events).some((e) => e.data?.name === 'after'),
     'audio/play: Error result aborts the list (trailing setVariable skipped)');
  ok(r1.status !== 0, 'audio/play: run exits non-zero on unhandled error');

  // Same failure + continueOnError → the next block runs (other half of the
  // BlockResult::Error flow-control contract).
  const r2 = run({
    version: 1,
    roots: [
      { kind: 'audioPlay', path: '', continueOnError: true },
      { kind: 'setVariable', name: 'after', value: 1 },
    ],
  });
  ok(setVars(r2.events).some((e) => e.data?.name === 'after'),
     'audio/play: continueOnError lets the next block run');

  // audioPlay with a non-existent file → error (the in-handler exists() check).
  const missing = join(WORK, `no-such-audio-${Math.random().toString(36).slice(2)}.wav`);
  const r3 = run({
    version: 1,
    roots: [{ kind: 'audioPlay', path: missing, continueOnError: true }],
  });
  ok(byKind(r3.events, 'audioPlay').some(
       (e) => e.status === 'error' && String(e.message || '').includes('not found')),
     'audio/play: missing file → "file not found" error');

  // audioSpeak with empty text → error (code 2), checked before any provider
  // lookup so it needs no network/API key.
  const r4 = run({
    version: 1,
    roots: [{ kind: 'audioSpeak', text: '', continueOnError: true }],
  });
  ok(byKind(r4.events, 'audioSpeak').some(
       (e) => e.status === 'error' && String(e.message || '').includes('empty')),
     'audio/speak: empty text → error (no provider needed)');

  // Mic capture (audioRecord) — gated on a real capture device so the suite
  // stays green on machines without a microphone. Records a short clip → WAV.
  if (devices && devices.length > 0) {
    const wav = join(WORK, `rec-${Math.random().toString(36).slice(2)}.wav`);
    const rec = run({
      version: 1,
      roots: [
        { kind: 'audioRecord', durationMs: 300, outputPath: wav, storeAs: 'rec' },
        { kind: 'log', level: 'info' },
      ],
    });
    const okRec = byKind(rec.events, 'audioRecord').find((e) => e.status === 'ok');
    ok(!!okRec, 'audio/record: audioRecord emits ok (mic present)');
    eq(okRec?.data?.result, wav, 'audio/record: result is the output WAV path (→ PREVIOUS/storeAs)');
    ok(existsSync(wav), 'audio/record: WAV written to disk');
    eq(lastLog(rec.events)?.message, wav, 'audio/record: path flows through PREVIOUS into log');
  } else {
    console.log('  · no capture device present — skipping mic-record asserts');
  }

  // ── vibevoice TTS (compiled only when FEATURE_VIBEVOICE is on) ──
  const vvModel = join(MODELS, 'vibevoice-realtime-0.5B-q8_0.gguf');
  const vvTok   = join(MODELS, 'tokenizer.gguf');
  const vvVoice = join(MODELS, 'voice-en-Carter_man.gguf');

  if (!featureEnabled('FEATURE_VIBEVOICE')) {
    console.log('  · FEATURE_VIBEVOICE off in dist/shared/features.json — skipping vibevoice TTS');
  } else if (existsSync(vvModel) && existsSync(vvTok) && existsSync(vvVoice)) {
    // Single synthesis → outputPath saved, ok event emitted, synthMs reported.
    const vvWav = join(WORK, `vv-speak-${Math.random().toString(36).slice(2)}.wav`);
    const vvR = run({
      version: 1,
      roots: [{
        kind:       'audioSpeak',
        text:       'Hello from xblox vibevoice.',
        provider:   'vibevoice',
        model:      vvModel,
        tokenizer:  vvTok,
        voice:      vvVoice,
        outputPath: vvWav,
      }],
    }, { timeout: 180_000 });
    const vvOk = byKind(vvR.events, 'audioSpeak').find((e) => e.status === 'ok');
    ok(!!vvOk,                         'audio/speak-vv: audioSpeak emits ok');
    ok(existsSync(vvWav),              'audio/speak-vv: WAV written to outputPath');
    ok((vvOk?.data?.synthMs ?? 0) > 0, 'audio/speak-vv: synthMs > 0 (timing reported)');
    eq(vvOk?.data?.provider, 'vibevoice', 'audio/speak-vv: provider=vibevoice in event data');
    eq(vvOk?.data?.result,   vvWav,       'audio/speak-vv: result = outputPath (→ PREVIOUS)');

    // Loop × 2 with named instance: second pass reuses the loaded engine.
    // Both passes must succeed; the second should have a lower synthMs because
    // the CUDA warmup already happened on pass 1.
    const vvWav2 = join(WORK, `vv-loop-${Math.random().toString(36).slice(2)}.wav`);
    const vvLoop = run({
      version: 1,
      roots: [{
        kind: 'for', variable: 'pass', initial: '0', final: '2',
        comparator: '<', modifier: '+1', loopLimit: 10,
        items: [{
          kind:       'audioSpeak',
          text:       'Loop pass vibevoice.',
          provider:   'vibevoice',
          model:      vvModel,
          tokenizer:  vvTok,
          voice:      vvVoice,
          instance:   'vv-cached',
          outputPath: vvWav2,
        }],
      }],
    }, { timeout: 300_000 });
    const loopOks = byKind(vvLoop.events, 'audioSpeak').filter((e) => e.status === 'ok');
    eq(loopOks.length, 2, 'audio/speak-vv: loop × 2 — both passes emit ok (cached instance)');
    const [, pass2] = loopOks;
    ok((pass2?.data?.synthMs ?? 0) > 0,
       'audio/speak-vv: pass-2 reports synthMs > 0 (cached engine synthesised)');
  } else {
    console.log('  · vibevoice models not found in dist/models — skipping vibevoice TTS suite');
    console.log('    run: bash scripts/download-vibevoice.sh');
  }
}

function suiteBluetooth() {
  console.log('\n── bluetooth (Win32; devices + audio endpoints, hardware-tolerant) ──');
  if (skipUnlessFeature('FEATURE_COMMAND_BLUETOOTH', 'bluetooth suite')) return;

  // Probe: compiled in? An unknown kind emits a "unknown block kind skipped"
  // warning instead of a handler result — true on non-Win32 / FEATURE off.
  const probe = run({ version: 1, roots: [{ kind: 'bluetoothEndpoints', storeAs: 'eps' }] });
  if (probe.events.some((e) => e.kind === 'bluetoothEndpoints' && e.status === 'warning' &&
                               String(e.message || '').includes('unknown'))) {
    console.log('  · FEATURE_COMMAND_BLUETOOTH off / non-Win32 — skipping bluetooth suite');
    return;
  }

  // bluetoothEndpoints — MMDevice playback endpoints. Any Windows desktop has at
  // least one render endpoint, so this resolves to a non-trivial array.
  const okEps = byKind(probe.events, 'bluetoothEndpoints').find((e) => e.status === 'ok');
  ok(!!okEps, 'bluetooth/endpoints: bluetoothEndpoints emits ok');
  const eps = Array.isArray(okEps?.data?.result) ? okEps.data.result : null;
  ok(Array.isArray(eps), 'bluetooth/endpoints: result is an array of audio endpoints');
  ok(!eps || eps.every((e) => typeof e.id === 'string' && typeof e.name === 'string'),
     'bluetooth/endpoints: each endpoint has a string id + name');

  // bluetoothListDevices — paired/connected BT devices (array, possibly empty).
  const list = run({ version: 1, roots: [{ kind: 'bluetoothListDevices', storeAs: 'devs' }] });
  const okList = byKind(list.events, 'bluetoothListDevices').find((e) => e.status === 'ok');
  ok(!!okList, 'bluetooth/list: bluetoothListDevices emits ok');
  ok(Array.isArray(okList?.data?.result), 'bluetooth/list: result is an array of devices');

  // bluetoothConnect with empty id → BlockResult::Error (code 2), guarded BEFORE
  // any hardware/radio access. Default policy aborts → trailing block skipped.
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'bluetoothConnect', id: '' },
      { kind: 'setVariable', name: 'after', value: 1 },
    ],
  });
  ok(byKind(r1.events, 'bluetoothConnect').some((e) => e.status === 'error'),
     'bluetooth/connect: empty id → error event (BlockResult::Error)');
  ok(!setVars(r1.events).some((e) => e.data?.name === 'after'),
     'bluetooth/connect: Error result aborts the list (trailing setVariable skipped)');
  ok(r1.status !== 0, 'bluetooth/connect: run exits non-zero on unhandled error');

  // Same failure + continueOnError → the next block runs (flow-control contract).
  const r2 = run({
    version: 1,
    roots: [
      { kind: 'bluetoothConnect', id: '', continueOnError: true },
      { kind: 'setVariable', name: 'after', value: 1 },
    ],
  });
  ok(setVars(r2.events).some((e) => e.data?.name === 'after'),
     'bluetooth/connect: continueOnError lets the next block run');

  // bluetoothSetDefault with empty id → error (guarded; no hardware needed).
  const r3 = run({
    version: 1,
    roots: [{ kind: 'bluetoothSetDefault', id: '', continueOnError: true }],
  });
  ok(byKind(r3.events, 'bluetoothSetDefault').some((e) => e.status === 'error'),
     'bluetooth/set-default: empty id → error');

  // Manifest: the read-only list/endpoints blocks declare side-effect-free exec
  // flags and live under the "Bluetooth" palette group.
  const info = xbloxInfo();
  const blocks = Array.isArray(info.blocks) ? info.blocks : [];
  const epBlock = blocks.find((b) => b && b.kind === 'bluetoothEndpoints');
  if (epBlock) {
    eq(epBlock.group, 'Bluetooth', 'bluetooth/manifest: bluetoothEndpoints grouped under "Bluetooth"');
    ok((epBlock.execFlags || {}).has_side_effects === false,
       'bluetooth/manifest: bluetoothEndpoints is side-effect free');
  }
  const connectBlock = blocks.find((b) => b && b.kind === 'bluetoothConnect');
  if (connectBlock) {
    ok((connectBlock.execFlags || {}).has_side_effects === true,
       'bluetooth/manifest: bluetoothConnect is marked side-effecting');
  }
}

function suiteOcr() {
  console.log('\n── ocr (manifest + deterministic validation) ──');
  if (skipUnlessAnyFeature(['FEATURE_ONNX', 'FEATURE_LLAMA', 'FEATURE_VLM'], 'ocr suite')) return;

  const info = xbloxInfo();
  const blocks = Array.isArray(info.blocks) ? info.blocks : [];
  const ocr = blocks.find((b) => b && b.kind === 'ocrText');
  if (!ocr) {
    console.log('  · ocrText block not registered — skipping ocr suite');
    return;
  }

  eq(ocr.group, 'OCR', 'ocr/manifest: grouped under "OCR"');
  const params = Array.isArray(ocr.params) ? ocr.params : [];
  const param = (name) => params.find((p) => p && p.name === name);
  const provider = param('provider');
  eq(provider?.enum_values, ['auto', 'layout', 'table', 'text', 'vlm', 'paddle'], 'ocr/manifest: provider enum values');
  eq(provider?.default, 'auto', 'ocr/manifest: provider default = auto');
  ok(provider?.group === 'source_model', 'ocr/manifest: provider is grouped with source/model');
  ok(param('mode')?.default === 'layout', 'ocr/manifest: mode defaults to layout');
  ok(param('model')?.ui?.control === 'select' && param('model')?.ui?.options_path === 'ocr.models.vlm',
     'ocr/manifest: model uses runtime-resolved OCR VLM options');
  eq(param('backend')?.enum_values, ['auto', 'cpu', 'gpu', 'cuda', 'coreml', 'metal'], 'ocr/manifest: ONNX backend enum values');
  ok(param('backend')?.group === 'runtime' && param('threads')?.group === 'runtime',
     'ocr/manifest: backend and threads are grouped under runtime');
  ok(param('layoutModel')?.kind === 'file_path' && param('layoutConf')?.kind === 'float',
     'ocr/manifest: layout model/conf params are exposed');
  ok(param('detModel')?.kind === 'file_path' && param('recModel')?.kind === 'file_path' && param('dict')?.kind === 'file_path',
     'ocr/manifest: ONNX det/rec/dict are file_path params');
  ok(param('detModel')?.group === 'onnx_model_overrides',
     'ocr/manifest: raw ONNX paths are grouped as model overrides');
  ok(param('gpuLayers')?.kind === 'integer' && param('gpuLayers')?.min === -1,
     'ocr/manifest: VLM gpuLayers supports -1 all-layer offload');
  ok(param('json')?.kind === 'boolean' && param('json')?.default === true,
     'ocr/manifest: json output default = true');
  ok(param('markdown')?.kind === 'boolean' && param('markdown')?.default === true,
     'ocr/manifest: markdown output default = true');
  ok((ocr.execFlags || {}).supports_cancel === true && (ocr.execFlags || {}).supports_background === true,
     'ocr/manifest: exec flags are backgroundable + cancelable');
  ok((ocr.execFlags || {}).iterable === true && Array.isArray(ocr.childLists) && ocr.childLists.some((c) => c.key === 'items'),
     'ocr/manifest: OCR documents are iterable through child items');

  const missingInput = run({
    version: 1,
    roots: [{ kind: 'ocrText', input: '', continueOnError: true }],
  });
  ok(byKind(missingInput.events, 'ocrText').some((e) => e.status === 'error' &&
       String(e.message || '').includes('precondition failed') &&
       JSON.stringify(e.data?.diagnostics || []).includes('required_parameter_missing')),
     'ocr/args: empty input → required-parameter precondition (handler skipped)');

  const alias = run({
    version: 1,
    roots: [{ kind: 'ocr', input: '', continueOnError: true }],
  });
  ok(alias.events.some((e) => e.kind === 'ocr' && e.status === 'error' &&
       String(e.message || '').includes('precondition failed')),
     'ocr/alias: hidden ocr alias hits the same required-input precondition');
  ok(!alias.events.some((e) => e.status === 'running'),
     'ocr/alias: handler is not entered when the precondition fails');

  const csvPath = join(ONNX_DIR, 'ocr-reciept-1.csv');
  if (existsSync(csvPath)) {
    const csv = readFileSync(csvPath, 'utf8');
    ok(/A246/.test(csv) && /SUBTOTAL/.test(csv) && /356\.40/.test(csv),
       'ocr/artifact: ocr-reciept-1.csv has receipt number, subtotal, and total');
  } else {
    console.log('  · ocr-reciept-1.csv absent — produce it with npm run test:ocr:vl');
    reportNotes.push('OCR sidecar ocr-reciept-1.csv missing; run npm run test:ocr:vl');
  }
}

function suiteVision() {
  console.log('\n── vision (manifest + MiniCPM smoke) ──');
  if (skipUnlessAnyFeature(['FEATURE_LLAMA', 'FEATURE_VLM'], 'vision suite')) return;

  const info = xbloxInfo();
  const blocks = Array.isArray(info.blocks) ? info.blocks : [];
  const vision = blocks.find((b) => b && b.kind === 'visionAsk');
  if (!vision) {
    console.log('  · visionAsk block not registered — skipping vision suite');
    return;
  }

  eq(vision.group, 'Vision', 'vision/manifest: grouped under "Vision"');
  const params = Array.isArray(vision.params) ? vision.params : [];
  const param = (name) => params.find((p) => p && p.name === name);
  ok(param('input')?.kind === 'image_path' && Array.isArray(param('input')?.resolve) && param('input').resolve.includes('globs'),
     'vision/manifest: input is image path with glob support');
  ok(param('frames')?.kind === 'json_value' && param('frames')?.group === 'source_model',
     'vision/manifest: frames accepts additional image paths');
  ok(param('model')?.ui?.control === 'select' && param('model')?.ui?.options_path === 'vision.models.vlm',
     'vision/manifest: model uses runtime-resolved local VLM options');
  eq(param('slot')?.enum_values, ['vision', 'image', 'video'], 'vision/manifest: slot enum values');
  ok(param('prompt')?.group === 'prompt' && typeof param('prompt')?.default === 'string',
     'vision/manifest: prompt has ready default');
  ok(param('json')?.kind === 'boolean' && param('json')?.default === true,
     'vision/manifest: json output default = true');
  const normalizers = param('normalizers');
  ok(normalizers?.kind === 'flags' && normalizers?.group === 'output_normalization' && normalizers?.default === 15,
     'vision/manifest: normalizers flag mask is exposed');
  ok(Array.isArray(normalizers?.flags) && normalizers.flags.length === 4 &&
       normalizers.flags.every((flag) => typeof flag.value === 'number' && typeof flag.label === 'string'),
     'vision/manifest: normalizer flags carry value/label metadata');
  ok(param('includeRaw')?.kind === 'boolean' && param('includeRaw')?.group === 'output',
     'vision/manifest: includeRaw output debug option is exposed');
  ok(param('threads')?.group === 'runtime' && param('gpuLayers')?.group === 'runtime' && param('maxTokens')?.group === 'runtime',
     'vision/manifest: runtime params are grouped');
  ok((vision.execFlags || {}).supports_cancel === true && (vision.execFlags || {}).supports_background === true,
     'vision/manifest: exec flags are backgroundable + cancelable');
  ok(Array.isArray(vision.childLists) && vision.childLists.some((c) => c.key === 'items'),
     'vision/manifest: child items can parse/log vision output');

  const modelControl = blocks.find((b) => b && b.kind === 'modelControl');
  ok(modelControl?.group === 'AI', 'models/manifest: generic modelControl block is registered');
  for (const [kind, group] of [
    ['audioModelControl', 'Audio'],
    ['imageModelControl', 'Image'],
    ['visionModelControl', 'Vision'],
    ['ocrModelControl', 'OCR'],
    ['videoModelControl', 'Video'],
  ]) {
    const def = blocks.find((b) => b && b.kind === kind);
    ok(def?.group === group && def?.label === 'Unload Model', `models/manifest: ${kind} alias appears under ${group}`);
  }
  const listedModels = run({ version: 1, roots: [{ kind: 'modelControl', action: 'List', storeAs: 'models' }] });
  ok(listedModels.status === 0 && byKind(listedModels.events, 'modelControl').some((e) => e.status === 'ok' && Array.isArray(e.data?.result)),
     'models/list: modelControl List returns loaded model array');

  const empty = run({ version: 1, roots: [{ kind: 'visionAsk', input: '', continueOnError: true }] });
  ok(empty.events.some((e) => e.kind === 'visionAsk' && e.status === 'error' && String(e.message || '').includes('provide input')),
     'vision/args: empty input → validation error');

  const alias = run({ version: 1, roots: [{ kind: 'visionDescribe', input: '', continueOnError: true }] });
  ok(alias.events.some((e) => e.status === 'error' && String(e.message || '').includes('provide input')),
     'vision/alias: hidden visionDescribe alias dispatches to handler');

  const runRealVision = only === 'vision' || process.env.PM_XBLOX_VISION_REAL === '1';
  if (!runRealVision) {
    console.log('  · MiniCPM smoke skipped outside --only vision');
    return;
  }

  const model = join(MODELS, 'MiniCPM-V-4_5-Q4_K_M.gguf');
  const mmproj = join(MODELS, 'MiniCPM-V-4_5-mmproj-f16.gguf');
  ok(existsSync(model), 'vision/models: MiniCPM-V model is cached');
  ok(existsSync(mmproj), 'vision/models: MiniCPM-V mmproj is cached');
  if (!(existsSync(model) && existsSync(mmproj))) return;

  const image = existsSync(join(ONNX_DIR, 'xblox.png'))
    ? join(ONNX_DIR, 'xblox.png')
    : join(ONNX_DIR, 'xblox-ocr-generated-receipt.png');
  if (!existsSync(image)) writeTextPng(image);

  const r = run({
    version: 1,
    roots: [{
      kind: 'visionAsk',
      input: image,
      prompt: 'Describe the image in one short sentence.',
      model: '',
      json: true,
      normalizers: 15,
      includeRaw: false,
      threads: 8,
      gpuLayers: -1,
      ctx: 4096,
      maxTokens: 128,
      storeAs: 'vision',
      items: [
        { kind: 'Parse', filter: '.text', storeAs: 'visionText' },
      ],
    }],
  }, { timeout: 600_000 });
  const okEvent = byKind(r.events, 'visionAsk').find((e) => e.status === 'ok');
  ok(!!okEvent, 'vision/minicpm: visionAsk runs MiniCPM on image fixture');
  ok(okEvent?.data?.result?.ok === true, 'vision/minicpm: result.ok=true');
  ok(String(okEvent?.data?.result?.model || '').includes('minicpm'), 'vision/minicpm: MiniCPM model is reported');
  ok(typeof okEvent?.data?.result?.text === 'string' && okEvent.data.result.text.trim().length > 0,
     'vision/minicpm: structured result includes generated text');
  ok(okEvent?.data?.result?.normalizers?.stripThinking === true &&
       okEvent?.data?.result?.normalizers?.stripFences === true &&
       okEvent?.data?.result?.normalizers?.stripRoleLabels === true,
     'vision/minicpm: structured result reports applied normalizers');
  ok(!Object.prototype.hasOwnProperty.call(okEvent?.data?.result || {}, 'rawText'),
     'vision/minicpm: rawText is omitted unless includeRaw=true');
  ok(Array.isArray(okEvent?.data?.result?.inputs) && okEvent.data.result.inputs.length === 1,
     'vision/minicpm: structured result includes input list');
  ok(byKind(r.events, 'Parse').some((e) => e.status === 'ok' && e.data?.filter === '.text' && typeof e.data?.result === 'string'),
     'vision/minicpm: child Parse can grab generated text');
}

function suiteLlm() {
  console.log('\n── llm (manifest + args deterministic; turn/result/parse model-gated) ──');

  // ── 1) MANIFEST — the block's *declared* interface (deterministic, offline) ──
  // `xblox info --json` is the same payload the host streams to the UI, so this
  // guards the param defs / defaults / exec flags llm_blocks.cpp publishes.
  const info = xbloxInfo();
  const blocks = Array.isArray(info.blocks) ? info.blocks : [];
  const agent = blocks.find((b) => b && b.kind === 'llmAgent');

  // Whole suite is gated on the block being compiled in (LLM feature). If it is
  // not registered there is nothing to test — skip like the hardware suites.
  if (!agent) {
    console.log('  · llmAgent block not registered — skipping llm suite');
    return;
  }

  eq(agent.group, 'AI', 'llm/manifest: grouped under "AI"');
  ok(typeof agent.color === 'string' && agent.color.length > 0, 'llm/manifest: has a palette color');

  const params = Array.isArray(agent.params) ? agent.params : [];
  const param = (name) => params.find((p) => p && p.name === name);

  const prompt = param('prompt');
  ok(prompt && prompt.kind === 'prompt' && prompt.group === 'input' &&
     prompt.required === true && prompt.uses_previous === true,
     'llm/manifest: prompt is a required input that sources PREVIOUS');

  const type = param('type');
  eq(type?.enum_values, ['completion', 'responses', 'realtime'], 'llm/manifest: type enum values');
  eq(type?.default, 'completion', 'llm/manifest: type default = completion');

  const steps = param('maxSteps');
  ok(steps && steps.kind === 'integer' && steps.min === 0 && steps.max === 100,
     'llm/manifest: maxSteps is an integer clamped to [0,100]');

  const timeout = param('timeoutMs');
  ok(timeout && timeout.kind === 'duration_ms' && timeout.max === 3_600_000,
     'llm/manifest: timeoutMs is a duration capped at 1h');

  eq(param('tools')?.default, true, 'llm/manifest: tools default = true');

  const consentUi = param('consentUi');
  eq(consentUi?.default, 'win32', 'llm/manifest: consentUi default = win32 (native dialog)');
  ok(Array.isArray(consentUi?.enum_values) && consentUi.enum_values.includes('win32'),
     'llm/manifest: consentUi includes win32');

  eq(param('runner')?.default, '', 'llm/manifest: runner default empty (preset / native)');

  // Capability opt-outs — default true (inherit the chat preset), set false to
  // force the capability off for the turn regardless of preset.
  for (const cap of ['mcp', 'skills', 'planner']) {
    const p = param(cap);
    ok(p && p.kind === 'boolean' && p.default === true,
       `llm/manifest: ${cap} is a boolean opt-out defaulting to true`);
  }

  const cwd = param('cwd');
  ok(cwd && cwd.kind === 'dir_path' && Array.isArray(cwd.resolve) && cwd.resolve.includes('variables'),
     'llm/manifest: cwd is a dir_path that resolves ${vars}');

  const store = param('storeAs');
  ok(store && store.group === 'output' && store.default === 'answer',
     'llm/manifest: storeAs is an output defaulting to "answer"');

  ok(param('usage')?.group === 'output' && param('usage')?.kind === 'json_value',
     'llm/manifest: usage is a json output');
  ok(param('transcript')?.group === 'output' && param('transcript')?.kind === 'json_value',
     'llm/manifest: transcript is a json output');

  const ef = agent.execFlags || {};
  ok(ef.supports_cancel === true && ef.supports_background === true &&
     ef.has_side_effects === true && ef.is_container === false && ef.iterable === false,
     'llm/manifest: exec flags — cancelable, backgroundable, side-effecting, non-container');
  // NB: the "llm" / "agent" aliases are .hidden(), so they are intentionally
  // absent from the palette manifest — their registration is proven below by
  // hitting the same required-prompt precondition with no unknown-kind warning.

  // ── 2) ARG VALIDATION — empty prompt is a required-parameter precondition ──
  // prompt is .req(); the executor rejects empty/missing before the handler
  // runs (no running event, no provider resolve, no network). Same contract as
  // ocr/args empty input.
  for (const kind of ['llmAgent', 'llm', 'agent']) {
    const r = run({
      version: 1,
      roots: [{ kind, prompt: '', continueOnError: true }],
    });
    ok(r.events.some((e) => e.kind === kind && e.status === 'error' &&
         String(e.message || '').includes('precondition failed') &&
         JSON.stringify(e.data?.diagnostics || []).includes('required_parameter_missing')),
       `llm/args: ${kind} empty prompt → required-parameter precondition (handler skipped)`);
    ok(!r.events.some((e) => e.status === 'running'),
       `llm/args: ${kind} handler is not entered when the precondition fails`);
    ok(!r.events.some((e) => e.status === 'ok' && (e.kind === 'llm' || e.kind === kind)),
       `llm/args: ${kind} produced no answer on missing prompt`);
    eq(r.status, 2, `llm/args: ${kind} exits 2 on missing prompt`);
    ok(!r.events.some((e) => e.status === 'warning' &&
                             String(e.message || '').includes('unknown')),
       `llm/args: ${kind} is a registered block (no unknown-kind warning)`);
  }

  // prompt sources PREVIOUS: a producer sets PREVIOUS, the llm block omits prompt
  // entirely → it must clear the required-parameter precondition. tools off +
  // timeoutMs:1 keeps it cheap: any failure past the guard is a resolve/timeout.
  const prev = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'seed', value: 'PROMPT-VIA-PREVIOUS' },
      { kind: 'llmAgent', tools: false, timeoutMs: 1 },
    ],
  }, { timeout: 30_000 });
  ok(!prev.events.some((e) => e.status === 'error' &&
                              (String(e.message || '').includes('missing a prompt') ||
                               (String(e.message || '').includes('precondition failed') &&
                                JSON.stringify(e.data?.diagnostics || []).includes('required_parameter_missing')))),
     'llm/args: omitted prompt sources PREVIOUS (clears the missing-prompt guard)');

  // ── 3) RESULT + PARSE — needs a configured model + network; tolerant/skip ──
  // One combined pass keeps real calls to a minimum: tools off = a single
  // completion; json on = transcript output; storeAs "answer" feeds getVariable.
  const r = run({
    version: 1,
    roots: [
      { kind: 'llmAgent', prompt: 'Reply with only the word OK.', tools: false, json: true, storeAs: 'answer' },
      { kind: 'getVariable', name: 'answer', target: 'capturedAnswer' },
    ],
  }, { timeout: 120_000 });

  const llmEv = byKind(r.events, 'llm');
  const okEv = llmEv.find((e) => e.status === 'ok');
  const errEv = llmEv.find((e) => e.status === 'error');

  if (!okEv) {
    const why = errEv ? String(errEv.message || JSON.stringify(errEv.data?.diagnostics || []) || 'error')
              : r.status !== 0 ? `exit ${r.status}`
              : 'no llm event';
    console.log(`  · llm turn unavailable (${why}) — skipping result/parse asserts`);
    return;
  }

  // Happy path: a configured model answered. Assert the full typed output surface.
  eq(r.status, 0, 'llm/result: exits 0');
  ok(typeof okEv.data?.result === 'string' && okEv.data.result.length > 0,
     'llm/result: non-empty answer (result → PREVIOUS/storeAs)');
  ok(typeof okEv.data?.model === 'string' && okEv.data.model.length > 0,
     'llm/result: reports the resolved model as event metadata');
  ok(typeof okEv.data?.iterations === 'number' && okEv.data.iterations >= 1,
     'llm/result: reports tool-loop iteration count (>= 1)');
  ok(okEv.data?.usage != null && typeof okEv.data.usage === 'object',
     'llm/result: emits a usage aggregate (token/cost)');
  ok(okEv.data?.transcript != null,
     'llm/result: json=true emits the full transcript output');

  const captured = getVars(r.events).find((e) => e.data?.target === 'capturedAnswer');
  eq(captured?.data?.value, okEv.data.result,
     'llm/result: storeAs default "answer" holds the answer (var === result)');

  // llm → parse: the answer flows into a downstream Parse block as PREVIOUS. We
  // prompt for strict JSON and pluck a field; output is model-dependent so the
  // value assert is tolerant — the structural claim is that Parse ran on PREVIOUS.
  if (skipUnlessFeature('FEATURE_JQ', 'llm→parse jq pluck')) return;
  const piped = run({
    version: 1,
    roots: [
      { kind: 'llmAgent', prompt: 'Output only this JSON and nothing else: {"reply":"pong"}', tools: false },
      { kind: 'Parse', parser: 'jq', filter: '.reply', storeAs: 'picked' },
    ],
  }, { timeout: 120_000 });

  const pipedOk = byKind(piped.events, 'llm').some((e) => e.status === 'ok');
  if (!pipedOk) {
    console.log('  · llm→parse turn unavailable — skipping parse pipeline assert');
    return;
  }
  const parseEv = piped.events.find((e) => (e.kind === 'Parse' || e.kind === 'parse'));
  ok(!!parseEv, 'llm/parse: the answer reached a downstream Parse block (PREVIOUS chain)');
  const pickedVar = getVars(piped.events).find((e) => e.data?.target === 'picked');
  if (parseEv?.status === 'ok' && pickedVar) {
    ok(typeof pickedVar.data?.value === 'string',
       'llm/parse: jq pluck yielded a string (e.g. "pong")');
  } else {
    console.log('  · llm→parse: model output was not strict JSON — value assert skipped');
  }
}

function suiteStd() {
  console.log('\n── std (stdout / stdin / exit — pipe-friendly I/O) ──');

  // Windows text-mode stdout translates \n → \r\n; normalise for assertions.
  const lf = (s) => String(s).replace(/\r\n/g, '\n');

  // ── stdout ──────────────────────────────────────────────────────────────────
  // Explicit pipe output: the message lands on raw stdout, independent of the
  // logging system, and the event carries the resolved message.
  const out1 = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'greeting', value: 'pipe-me' },
      { kind: 'stdout', message: '${greeting}' },
    ],
  });
  eq(out1.status, 0, 'stdout: flow exits 0');
  ok(out1.stdout.split(/\r?\n/).includes('pipe-me'), 'stdout: message written to raw stdout');
  const outEv = byKind(out1.events, 'stdout').at(-1);
  eq(outEv?.data?.message, 'pipe-me', 'stdout: event carries the resolved message');

  // Production mode (no --json): stdout contains ONLY the piped payload.
  const out2 = run({
    version: 1,
    roots: [{ kind: 'stdout', message: 'clean-pipe' }],
  }, { noJson: true });
  eq(out2.status, 0, 'stdout/no-json: exits 0 (muted-event bookkeeping)');
  eq(lf(out2.stdout), 'clean-pipe\n', 'stdout/no-json: raw stdout is exactly the payload');

  // newline:false keeps the cursor on the line (payload not newline-terminated).
  const out3 = run({
    version: 1,
    roots: [
      { kind: 'stdout', message: 'a', newline: false },
      { kind: 'stdout', message: 'b', newline: false },
    ],
  }, { noJson: true });
  eq(out3.stdout, 'ab', 'stdout/no-json: newline:false concatenates payloads');

  // ── stdin ───────────────────────────────────────────────────────────────────
  // text (default): whole stream → string, trailing newline trimmed.
  const in1 = run({
    version: 1,
    roots: [
      { kind: 'stdin', storeAs: 'piped' },
      { kind: 'stdout', message: '${piped}' },
    ],
  }, { input: 'hello pipe\n', noJson: true });
  eq(in1.status, 0, 'stdin/text: exits 0');
  eq(lf(in1.stdout), 'hello pipe\n', 'stdin/text: payload round-trips stdin → stdout');

  // json: structured input feeds downstream JSON blocks (Parse/jq pluck).
  if (!skipUnlessFeature('FEATURE_JQ', 'stdin/json jq pluck')) {
  const in2 = run({
    version: 1,
    roots: [
      { kind: 'stdin', parse: 'json', storeAs: 'data' },
      { kind: 'Parse', parser: 'jq', filter: '.a.b', input: 'data', storeAs: 'picked' },
      { kind: 'stdout', message: '${picked}' },
    ],
  }, { input: '{"a":{"b":42}}\n', noJson: true });
  eq(in2.status, 0, 'stdin/json: exits 0');
  eq(in2.stdout.trim(), '42', 'stdin/json: jq plucks a field from the piped object');
  }

  // json: malformed input is a block error → non-zero exit.
  const in3 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'json' }],
  }, { input: '{nope', noJson: true });
  ok(in3.status !== 0, 'stdin/json: malformed JSON → non-zero exit');

  // number: piped value lands in the numeric store → usable by muParser.
  const in4 = run({
    version: 1,
    roots: [
      { kind: 'stdin', parse: 'number', storeAs: 'n' },
      { kind: 'setVariable', name: 'answer', expression: 'n * 6' },
      { kind: 'stdout', message: '${answer}' },
    ],
  }, { input: '7\n', noJson: true });
  eq(in4.status, 0, 'stdin/number: exits 0');
  eq(in4.stdout.trim(), '42', 'stdin/number: piped int feeds a muParser expression');

  // number: typed result kind — integer vs float (event-asserted via --json).
  const in5 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'number', storeAs: 'x' }],
  }, { input: '3.5' });
  const in5Ev = byKind(in5.events, 'stdin').find((e) => e.status === 'ok');
  eq(in5Ev?.data?.result, 3.5, 'stdin/number: float input parses as 3.5');
  const in6 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'number' }],
  }, { input: 'not-a-number' });
  ok(in6.status !== 0, 'stdin/number: non-numeric input → non-zero exit');

  // boolean: tolerant tokens (true/1/yes/on · false/0/no/off), typed result.
  const in7 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'boolean', storeAs: 'flag' }],
  }, { input: 'yes\n' });
  const in7Ev = byKind(in7.events, 'stdin').find((e) => e.status === 'ok');
  eq(in7Ev?.data?.result, true, 'stdin/boolean: "yes" parses to true');
  const in8 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'boolean' }],
  }, { input: 'OFF' });
  const in8Ev = byKind(in8.events, 'stdin').find((e) => e.status === 'ok');
  eq(in8Ev?.data?.result, false, 'stdin/boolean: "OFF" parses to false (case-insensitive)');

  // auto: JSON-scalar detection — number stays a number, prose stays a string.
  const in9 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'auto', storeAs: 'v' }],
  }, { input: '123\n' });
  const in9Ev = byKind(in9.events, 'stdin').find((e) => e.status === 'ok');
  eq(in9Ev?.data?.result, 123, 'stdin/auto: "123" detected as a number');
  const in10 = run({
    version: 1,
    roots: [{ kind: 'stdin', parse: 'auto', storeAs: 'v' }],
  }, { input: 'plain prose\n' });
  const in10Ev = byKind(in10.events, 'stdin').find((e) => e.status === 'ok');
  eq(in10Ev?.data?.result, 'plain prose', 'stdin/auto: prose falls back to string');

  // binary fence: NUL bytes are rejected (fenced off for now).
  const in11 = run({
    version: 1,
    roots: [{ kind: 'stdin' }],
  }, { input: Buffer.from([0x61, 0x00, 0x62]) });
  ok(in11.status !== 0, 'stdin/binary: NUL byte → non-zero exit (binary fenced off)');
  ok(byKind(in11.events, 'stdin').some((e) => e.status === 'error' && /binary/i.test(e.message || '')),
     'stdin/binary: error event names the binary fence');

  // line mode at EOF (no piped input): clean block error, not a hang.
  const in12 = run({
    version: 1,
    roots: [{ kind: 'stdin', mode: 'line' }],
  });
  ok(in12.status !== 0, 'stdin/line: EOF with no input → non-zero exit');

  // ── exit ────────────────────────────────────────────────────────────────────
  // exit 0: deliberate success — trailing blocks do NOT run.
  const ex1 = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'before', value: 1 },
      { kind: 'exit', code: 0 },
      { kind: 'setVariable', name: 'after', value: 2 },
    ],
  });
  eq(ex1.status, 0, 'exit: code 0 → process exits 0');
  eq(ex1.report.ok, true, 'exit: code 0 → report ok:true');
  ok(byKind(ex1.events, 'exit').filter((e) => e.status !== 'running').length === 1,
     'exit: emits one exit event');
  ok(!setVars(ex1.events).some((e) => e.data?.target === 'after'),
     'exit: trailing blocks are not executed');

  // exit N: authoritative custom code (beyond the standard 0/1/2 set).
  const ex2 = run({
    version: 1,
    roots: [{ kind: 'exit', code: 42, message: 'app-specific signal' }],
  });
  eq(ex2.status, 42, 'exit: custom code 42 becomes the process exit code');
  eq(ex2.report.exitCode, 42, 'exit: report carries exitCode 42');
  eq(ex2.report.ok, false, 'exit: non-zero code → report ok:false');

  // exit also rules the muted-event path (no --json) — the piping mode.
  const ex3 = run({
    version: 1,
    roots: [
      { kind: 'stdout', message: 'partial' },
      { kind: 'exit', code: 7 },
      { kind: 'stdout', message: 'never' },
    ],
  }, { noJson: true });
  eq(ex3.status, 7, 'exit/no-json: code 7 survives muted-event bookkeeping');
  eq(lf(ex3.stdout), 'partial\n', 'exit/no-json: output before exit is flushed, after is not');

  // exit inside a container unwinds the whole run (not just the loop body).
  const ex4 = run({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'i', value: 0 },
      {
        kind: 'while', condition: 'i < 100', loopLimit: 200,
        items: [
          { kind: 'setVariable', name: 'i', expression: 'i + 1' },
          { kind: 'if', condition: 'i == 3', consequent: [{ kind: 'exit', code: 5 }] },
        ],
      },
      { kind: 'setVariable', name: 'done', value: 1 },
    ],
  });
  eq(ex4.status, 5, 'exit: unwinds out of while/if containers with its code');
  ok(!setVars(ex4.events).some((e) => e.data?.target === 'done'),
     'exit: blocks after the unwound container do not run');

  // ── pipe: CLI → CLI ─────────────────────────────────────────────────────────
  // Two real processes joined by an OS pipe: a producer flow writes a number to
  // stdout, the consumer flow reads it from stdin, adds, and prints the sum —
  // the end-to-end pattern the std blocks exist for.
  const producerSrc = join(WORK, 'std-pipe-producer.json');
  const consumerSrc = join(WORK, 'std-pipe-consumer.json');
  writeFileSync(producerSrc, JSON.stringify({
    version: 1,
    roots: [
      { kind: 'setVariable', name: 'lhs', value: 35 },
      { kind: 'stdout', message: '${lhs}' },
    ],
  }, null, 2));
  writeFileSync(consumerSrc, JSON.stringify({
    version: 1,
    roots: [
      { kind: 'stdin', parse: 'number', storeAs: 'n' },
      { kind: 'setVariable', name: 'sum', expression: 'n + 7' },
      { kind: 'stdout', message: '${sum}' },
    ],
  }, null, 2));
  const xrun = (src) => `"${EXE}" xblox --log-level off run --no-wait --src "${src}"`;
  const piped2 = spawnSync(`${xrun(producerSrc)} | ${xrun(consumerSrc)}`, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    shell: true,
    timeout: 30_000,
  });
  eq(piped2.status, 0, 'pipe: producer | consumer chain exits 0');
  eq(lf(withoutSettingsIpcNoise(piped2.stdout || '')), '42\n',
     'pipe: stdout → OS pipe → stdin addition across two CLI processes (35 + 7 = 42)');

  // manifest: both blocks declare their interface for the host/UI palette.
  const manifest = xbloxInfo();
  const kinds = (manifest.blocks || []).map((b) => b.kind);
  ok(kinds.includes('stdin'), 'manifest: stdin block is declared');
  ok(kinds.includes('exit'), 'manifest: exit block is declared');
  const stdinDecl = (manifest.blocks || []).find((b) => b.kind === 'stdin');
  const parseDecl = (stdinDecl?.params || []).find((p) => p.name === 'parse');
  eq(parseDecl?.enum_values ?? parseDecl?.enumValues,
     ['text', 'json', 'number', 'boolean', 'auto'],
     'manifest: stdin.parse declares the primitive parse modes');
}

function suiteJq() {
  console.log('\n── jq (Parse block — libjq filter engine) ──');
  if (skipUnlessFeature('FEATURE_JQ', 'jq suite')) return;

  // Helper: run a Parse block against inline JSON data and return the result
  // value from the emitted 'Parse/ok' event.
  function parseResult(input, filter, storeAs = 'result') {
    const doc = {
      version: 1,
      context: { data: input },
      roots: [
        { kind: 'Parse', input: 'data', filter, storeAs },
      ],
    };
    const { status, events } = run(doc);
    const ev = events.find((e) => e.kind === 'Parse' && e.status === 'ok');
    return { status, result: ev?.data?.result ?? undefined, events };
  }

  // ── fast path: simple dot-path bypasses libjq ───────────────────────────
  {
    const { status, result } = parseResult({ name: 'alice', score: 42 }, '.name');
    eq(status, 0, 'jq/dotpath: exits 0');
    eq(result, 'alice', 'jq/dotpath: .name returns string field');
  }
  {
    const { result } = parseResult({ a: { b: { c: 7 } } }, '.a.b.c');
    eq(result, 7, 'jq/dotpath: .a.b.c nested field');
  }
  {
    const { result } = parseResult([10, 20, 30], '.1');
    eq(result, 20, 'jq/dotpath: array index .1');
  }

  // ── identity filter ──────────────────────────────────────────────────────
  {
    const obj = { x: 1, y: 2 };
    const { result } = parseResult(obj, '.');
    eq(JSON.stringify(result), JSON.stringify(obj), 'jq/identity: . returns input unchanged');
  }

  // ── libjq: arithmetic ────────────────────────────────────────────────────
  {
    const { status, result } = parseResult({ a: 3, b: 4 }, '.a + .b');
    eq(status, 0, 'jq/arithmetic: exits 0');
    eq(result, 7, 'jq/arithmetic: .a + .b = 7');
  }

  // ── libjq: collect array output  [ .[] | expr ] ─────────────────────────
  {
    const input = { items: [{ name: 'foo', active: true }, { name: 'bar', active: false }, { name: 'baz', active: true }] };
    const { result } = parseResult(input, '[.items[] | .name]');
    eq(JSON.stringify(result), JSON.stringify(['foo', 'bar', 'baz']), 'jq/collect: [.items[] | .name] collects all names');
  }

  // ── libjq: select() ──────────────────────────────────────────────────────
  {
    const input = { items: [{ name: 'foo', active: true }, { name: 'bar', active: false }, { name: 'baz', active: true }] };
    const { result } = parseResult(input, '[.items[] | select(.active) | .name]');
    eq(JSON.stringify(result), JSON.stringify(['foo', 'baz']), 'jq/select: select(.active) filters to active items');
  }

  // ── libjq: string interpolation ──────────────────────────────────────────
  {
    const { result } = parseResult({ name: 'world' }, '"hello, \\(.name)!"');
    eq(result, 'hello, world!', 'jq/string-interp: "hello, \\(.name)!"');
  }

  // ── libjq: type conversion ────────────────────────────────────────────────
  {
    const { result } = parseResult({ val: '42' }, '.val | tonumber');
    eq(result, 42, 'jq/tonumber: string "42" | tonumber = 42');
  }
  {
    const { result } = parseResult({ val: 99 }, '.val | tostring');
    eq(result, '99', 'jq/tostring: number 99 | tostring = "99"');
  }

  // ── libjq: keys / length ──────────────────────────────────────────────────
  {
    const { result } = parseResult({ a: 1, b: 2, c: 3 }, 'keys');
    ok(Array.isArray(result) && result.includes('a') && result.includes('b') && result.includes('c'),
       'jq/keys: keys returns sorted array of object keys');
  }
  {
    const { status, result } = parseResult([1, 2, 3, 4], 'length');
    eq(status, 0, 'jq/length: exits 0');
    eq(result, 4, 'jq/length: length of array [1,2,3,4] = 4');
  }

  // ── libjq: null/default via // ────────────────────────────────────────────
  {
    const { result } = parseResult({ x: null }, '.x // "default"');
    eq(result, 'default', 'jq/alternative: null // "default" returns "default"');
  }
  {
    const { result } = parseResult({ x: 5 }, '.x // "default"');
    eq(result, 5, 'jq/alternative: 5 // "default" returns 5');
  }

  // ── libjq: input as raw JSON string ──────────────────────────────────────
  // The Parse block accepts a string value for 'data' and parses it first.
  {
    const doc = {
      version: 1,
      context: { data: '{"z": 99}' },
      roots: [{ kind: 'Parse', input: 'data', filter: '.z', storeAs: 'result' }],
    };
    const { status, events } = run(doc);
    const ev = events.find((e) => e.kind === 'Parse' && e.status === 'ok');
    eq(status, 0, 'jq/raw-string-input: exits 0');
    eq(ev?.data?.result, 99, 'jq/raw-string-input: parses JSON string, then applies filter .z = 99');
  }

  // ── error: invalid filter ────────────────────────────────────────────────
  {
    const doc = {
      version: 1,
      context: { data: { x: 1 } },
      roots: [{ kind: 'Parse', input: 'data', filter: '!!! not valid jq', storeAs: 'result' }],
    };
    const { status, events } = run(doc);
    const errEv = events.find((e) => e.kind === 'Parse' && e.status === 'error');
    ok(errEv !== undefined, 'jq/invalid-filter: Parse emits an error event');
    ok(status !== 0, 'jq/invalid-filter: flow exits non-zero on parse error');
  }

  // ── storeAs + getVariable round-trip ─────────────────────────────────────
  {
    const doc = {
      version: 1,
      context: { payload: { score: 88 } },
      roots: [
        { kind: 'Parse', input: 'payload', filter: '.score', storeAs: 'myScore' },
        { kind: 'getVariable', name: 'myScore', target: 'out' },
        { kind: 'log', level: 'info', message: 'out' },
      ],
    };
    const { status, events } = run(doc);
    eq(status, 0, 'jq/storeAs: exits 0');
    eq(lastLog(events)?.message, '88', 'jq/storeAs: .score stored as myScore and readable via getVariable');
  }
}

function suiteFsVars() {
  console.log('\n── fs-vars (fsRead / Parse / fsWrite + PATH_* path functions) ──');

  const ASSETS = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox');
  const dataFile = join(ASSETS, 'fs-vars-data.json');

  // ── 1. fsRead + Parse (jq) + fsWrite via PATH_* ────────────────────────────
  // CURRENT_FILE is injected as a CLI arg (--CURRENT_FILE <path>), the same
  // mechanism the app uses when running via the VariableContext source_file.
  // PATH_DIR / PATH_NAME / PATH_EXT decompose it at resolution time so the
  // pipeline never needs hard-coded SRC_* variables.
  if (!skipUnlessFeature('FEATURE_JQ', 'fs-vars Parse pipeline')) {
    const outFile = join(WORK, 'fs-vars-data_modified.json');
    const doc = {
      version: 1,
      roots: [
        // 1) Read the mock JSON file whose path is CURRENT_FILE (CLI arg)
        { kind: 'fsRead', path: '${CURRENT_FILE}', encoding: 'json', storeAs: 'invoice' },
        // 2) Extract .total with a jq filter
        { kind: 'Parse', input: 'invoice', filter: '.total', storeAs: 'total' },
        // 3) Build the output path using PATH_* decomposition of CURRENT_FILE and
        //    write the parsed value (PREVIOUS = total as a string)
        {
          kind: 'fsWrite',
          path: '${PATH_DIR:CURRENT_FILE}/${PATH_NAME:CURRENT_FILE}_modified${PATH_EXT:CURRENT_FILE}',
          content: '',
          storeAs: 'wrote',
        },
      ],
    };
    // Write total into the content via a preceding setVariable so PREVIOUS holds it
    // Actually: fsWrite `content` defaults to PREVIOUS; Parse stores total → PREVIOUS = 5.69
    const r = run(doc, { extraArgs: ['--CURRENT_FILE', dataFile] });
    eq(r.status, 0, 'fs-vars/read-parse-write: exits 0');

    const readEv = byKind(r.events, 'fsRead').find((e) => e.status === 'ok');
    ok(!!readEv, 'fs-vars/read-parse-write: fsRead emits ok event');
    ok(readEv?.data?.path && samePath(readEv.data.path, dataFile),
       'fs-vars/read-parse-write: fsRead reports correct path');

    const parseEv = byKind(r.events, 'Parse').find((e) => e.status === 'ok');
    ok(!!parseEv, 'fs-vars/read-parse-write: Parse emits ok event');
    eq(parseEv?.data?.result, 5.69, 'fs-vars/read-parse-write: Parse .total = 5.69');

    const writeEv = byKind(r.events, 'fsWrite').find((e) => e.status === 'ok');
    ok(!!writeEv, 'fs-vars/read-parse-write: fsWrite emits ok event');
    // The resolved write path should be in the WORK dir (PATH_DIR of dataFile is ASSETS,
    // but we verify the _modified suffix and extension are correct)
    const writtenPath = String(writeEv?.data?.path ?? '');
    ok(writtenPath.includes('_modified') && writtenPath.endsWith('.json'),
       'fs-vars/read-parse-write: fsWrite path has _modified suffix + .json extension');
    ok(existsSync(writtenPath), 'fs-vars/read-parse-write: written file exists on disk');
  }

  // ── 2. PATH_* tokens individually ─────────────────────────────────────────
  // Use stdout (string_ message — interpolated) to emit each PATH_* token
  // and verify the expanded values in the captured output.
  {
    const testPath = join(ASSETS, 'fs-vars-data.json').replace(/\\/g, '/');
    const sep = '||';
    const doc = {
      version: 1,
      context: { f: testPath },
      roots: [
        { kind: 'stdout', message: 'DIR=${PATH_DIR:f}' },
        { kind: 'stdout', message: 'NAME=${PATH_NAME:f}' },
        { kind: 'stdout', message: 'EXT=${PATH_EXT:f}' },
        { kind: 'stdout', message: 'BASE=${PATH_BASE:f}' },
      ],
    };
    const r = run(doc, { noJson: true });
    eq(r.status, 0, 'fs-vars/path-fns: exits 0');
    const lines = r.stdout.split(/\r?\n/).filter(Boolean);
    const kv = Object.fromEntries(lines.map((l) => { const i = l.indexOf('='); return [l.slice(0, i), l.slice(i + 1)]; }));

    // PATH_DIR: parent directory — should match the ASSETS folder
    ok(samePath(kv.DIR ?? '', ASSETS),
       `fs-vars/path-fns: PATH_DIR:f matches packages/xblox/tests/xblox directory (got: ${kv.DIR})`);

    // PATH_NAME: stem without extension
    eq(kv.NAME, 'fs-vars-data', 'fs-vars/path-fns: PATH_NAME:f = "fs-vars-data"');

    // PATH_EXT: extension with dot
    eq(kv.EXT, '.json', 'fs-vars/path-fns: PATH_EXT:f = ".json"');

    // PATH_BASE: filename with extension
    eq(kv.BASE, 'fs-vars-data.json', 'fs-vars/path-fns: PATH_BASE:f = "fs-vars-data.json"');
  }

  // ── 3. PATH_ABS: relative path resolved against CWD ───────────────────────
  // fsWrite with a path using PATH_ABS lets us inspect the resolved path in
  // the event without needing a setVariable workaround.
  {
    const relPath = 'packages/xblox/tests/xblox/fs-vars-data.json';
    const outPath = join(WORK, 'path-abs-probe.txt');
    const doc = {
      version: 1,
      context: { rel: relPath },
      roots: [
        // stdout message is a string_ param — PATH_ABS expands there
        { kind: 'stdout', message: 'ABS=${PATH_ABS:rel}' },
      ],
    };
    const r = run(doc, { noJson: true });
    eq(r.status, 0, 'fs-vars/path-abs: exits 0');
    const absLine = r.stdout.split(/\r?\n/).find((l) => l.startsWith('ABS=')) ?? '';
    const absVal = absLine.slice('ABS='.length);
    ok(absVal.length > 0 && !absVal.includes('${'),
       'fs-vars/path-abs: PATH_ABS:rel resolves (non-empty, no literal token)');
    ok(absVal.includes('fs-vars-data.json'),
       `fs-vars/path-abs: PATH_ABS:rel includes the filename (got: ${absVal})`);
  }

  // ── 4. Deep resolution: context var holding ${KNOWNFOLDER:TEMP}/... ───────
  // A context variable contains another ${...} token; since the path is used
  // in an image_path / file_path parameter (ParamResolve::Deep), the nested
  // KNOWNFOLDER token is expanded in a second pass.
  {
    const doc = {
      version: 1,
      context: { scratch: '${KNOWNFOLDER:TEMP}/xblox-deep-test.json' },
      roots: [
        // Write a temp file so fsRead can read it back (we don't need the read
        // to succeed — just assert the path was deeply resolved)
        { kind: 'fsWrite', path: '${scratch}', content: '{"ok":1}', storeAs: 'wrote' },
      ],
    };
    const r = run(doc);
    eq(r.status, 0, 'fs-vars/deep: exits 0');
    const writeEv = byKind(r.events, 'fsWrite').find((e) => e.status === 'ok');
    ok(!!writeEv, 'fs-vars/deep: fsWrite ok — deep path resolved');
    const writtenPath = String(writeEv?.data?.path ?? '');
    ok(!writtenPath.includes('${') && writtenPath.includes('xblox-deep-test.json'),
       'fs-vars/deep: fsWrite path has no unresolved ${…} tokens (KNOWNFOLDER expanded)');
    // Clean up
    try { if (existsSync(writtenPath)) rmSync(writtenPath); } catch { /* ignore */ }
  }

  // ── 5. PATH_* on missing variable → literal token (no crash) ──────────────
  {
    const doc = {
      version: 1,
      roots: [
        { kind: 'setVariable', name: 'result', value: '${PATH_NAME:noSuchVar}' },
      ],
    };
    const r = run(doc);
    eq(r.status, 0, 'fs-vars/path-missing: exits 0 (graceful fallback)');
    const v = setVars(r.events).find((e) => e.data?.name === 'result');
    eq(v?.data?.value, '${PATH_NAME:noSuchVar}',
       'fs-vars/path-missing: unresolvable PATH_NAME stays as literal token');
  }
}

function suiteRefs() {
  console.log('\n── refs (unified reference grammar — paths in expressions + templates) ──');
  const lf = (s) => String(s).replace(/\r\n/g, '\n');
  const ctx = {
    doc: { data: [2, 5, 8], meta: { rate: 1.5 } },
    status: 'ready',
    user: { name: 'kim' },
  };

  // Expression-side nested access: dot, bracket, and mixed spellings are one
  // grammar (`doc.data[0]` ≡ `doc.data.0`), arrays expose `.length`.
  const ex = run({
    version: 1,
    context: ctx,
    roots: [
      { kind: 'setVariable', name: 'first', expression: 'doc.data[0]' },
      { kind: 'setVariable', name: 'second', expression: 'doc.data.1' },
      { kind: 'setVariable', name: 'calc', expression: 'doc.data[0] + doc.meta.rate * 2' },
      { kind: 'setVariable', name: 'len', expression: 'doc.data.length' },
    ],
  });
  eq(ex.status, 0, 'refs/expr: exits 0');
  ok(setVars(ex.events).some((e) => e.data?.name === 'first' && e.data?.value === 2),
     'refs/expr: doc.data[0] resolves through bracket indexing');
  ok(setVars(ex.events).some((e) => e.data?.name === 'second' && e.data?.value === 5),
     'refs/expr: doc.data.1 — dot-index spelling is equivalent');
  ok(setVars(ex.events).some((e) => e.data?.name === 'calc' && e.data?.value === 5),
     'refs/expr: nested refs compose with muParser arithmetic (2 + 1.5*2)');
  ok(setVars(ex.events).some((e) => e.data?.name === 'len' && e.data?.value === 3),
     'refs/expr: array length pseudo-field (doc.data.length = 3)');

  // ${ref} unwraps inside expression fields — template and expression syntax
  // mean the same thing.
  const un = run({
    version: 1,
    context: ctx,
    roots: [
      { kind: 'setVariable', name: 'viaTemplate', expression: '${doc.data[2]} - ${doc.data[0]}' },
    ],
  });
  eq(un.status, 0, 'refs/unwrap: exits 0');
  ok(setVars(un.events).some((e) => e.data?.name === 'viaTemplate' && e.data?.value === 6),
     'refs/unwrap: ${doc.data[2]} - ${doc.data[0]} = 6 (interpolation syntax tolerated)');

  // String equality folds against the context — quoted literal on either side.
  const cond = run({
    version: 1,
    context: ctx,
    roots: [
      { kind: 'if', condition: 'status == "ready"',
        consequent: [{ kind: 'setVariable', name: 'a', value: 1 }] },
      { kind: 'if', condition: 'user.name != "kim"',
        consequent: [{ kind: 'setVariable', name: 'b', value: 1 }] },
      { kind: 'else', items: [{ kind: 'setVariable', name: 'b', value: 2 }] },
      { kind: 'if', condition: '"kim" == user.name',
        consequent: [{ kind: 'setVariable', name: 'c', value: 1 }] },
      { kind: 'if', condition: 'status == "ready" && doc.data[0] == 2',
        consequent: [{ kind: 'setVariable', name: 'd', value: 1 }] },
    ],
  });
  eq(cond.status, 0, 'refs/strings: exits 0');
  const condVars = Object.fromEntries(setVars(cond.events).map((e) => [e.data?.name, e.data?.value]));
  eq(condVars.a, 1, 'refs/strings: status == "ready" is true');
  eq(condVars.b, 2, 'refs/strings: user.name != "kim" takes the else branch');
  eq(condVars.c, 1, 'refs/strings: literal-first form ("kim" == user.name) works');
  eq(condVars.d, 1, 'refs/strings: string fold composes with numeric refs (&&)');

  // PREVIOUS carries structures: nested refs reach into the last block result.
  const prev = run({
    version: 1,
    roots: [
      { kind: 'stdin', parse: 'json', storeAs: 'doc2' },
      { kind: 'if', condition: 'PREVIOUS.data[0] == 2',
        consequent: [{ kind: 'setVariable', name: 'hit', value: 1 }] },
      { kind: 'setVariable', name: 'sum', expression: 'doc2.data[0] + doc2.data[1]' },
    ],
  }, { input: '{"data":[2,7]}\n' });
  eq(prev.status, 0, 'refs/previous: exits 0');
  const prevVars = Object.fromEntries(setVars(prev.events).map((e) => [e.data?.name, e.data?.value]));
  eq(prevVars.hit, 1, 'refs/previous: PREVIOUS.data[0] == 2 reaches into the piped result');
  eq(prevVars.sum, 9, 'refs/previous: stored object stays addressable (doc2.data[0] + doc2.data[1])');

  // Template-side: ${path} tokens with dots/brackets interpolate from the
  // context; unresolved paths degrade to the bare key text (no abort).
  const tpl = run({
    version: 1,
    context: ctx,
    roots: [
      { kind: 'stdout', message: 'first=${doc.data[0]} rate=${doc.meta.rate} name=${user.name}' },
      { kind: 'stdout', message: 'missing=[${doc.nope[3]}]' },
    ],
  }, { noJson: true });
  eq(tpl.status, 0, 'refs/template: exits 0');
  eq(lf(tpl.stdout), 'first=2 rate=1.5 name=kim\nmissing=[doc.nope[3]]\n',
     'refs/template: nested ${path} interpolation (unresolved degrades to key text)');
}

// ── main ──────────────────────────────────────────────────────────────────────
const onlyIdx = process.argv.indexOf('--only');
const only = onlyIdx >= 0 ? process.argv[onlyIdx + 1] : null;

const suites = {
  engine: suiteEngine,
  variables: suiteVariables,
  wiring: suiteWiringContract,
  expressions: suiteExpressions,
  loops: suiteLoops,
  previous: suitePrevious,
  containers: suiteContainers,
  scoping: suiteScoping,
  transform: suiteTransform,
  simulate: suiteSimulate,
  constraints: suiteConstraints,
  globs: suiteGlobs,
  hooks: suiteHooks,
  iterate: suiteIterate,
  network: suiteNetwork,
  xbloxRun: suiteXbloxRun,
  'ipc-com': suiteIpcCom,
  video: suiteVideo,
  audio: suiteAudio,
  bluetooth: suiteBluetooth,
  ocr: suiteOcr,
  vision: suiteVision,
  llm: suiteLlm,
  std: suiteStd,
  jq: suiteJq,
  fsVars: suiteFsVars,
  refs: suiteRefs,
};

console.log(`xblox-next harness · exe=${EXE}`);
console.log(`xblox-next harness · models=${MODELS}`);
if (!existsSync(EXE)) {
  console.error(`\nCLI not found at ${EXE} — run "npm run build:cpp" first.`);
  process.exit(2);
}

try {
  if (only) {
    if (!suites[only]) {
      console.error(`unknown suite "${only}". choices: ${Object.keys(suites).join(', ')}`);
      process.exit(2);
    }
    await suites[only]();
  } else {
    for (const fn of Object.values(suites)) await fn();
  }
} finally {
  if (process.env.PM_KEEP_XBLOX_TEST_TMP !== '1') {
    try { rmSync(WORK, { recursive: true, force: true }); } catch { /* ignore */ }
  }
  writeNextReport();
}

console.log(`\n── result ── ${passed} passed, ${failed} failed`);
if (failed > 0) {
  console.log('failures:');
  for (const f of failures) console.log(`  - ${f}`);
  process.exitCode = 1;
}
