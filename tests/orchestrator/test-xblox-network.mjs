#!/usr/bin/env node
/**
 * test-xblox-network.mjs — xblox network-block test suite.
 *
 * Covers the features added in the network-blocks refactor:
 *   auth        Bearer / Basic / API-key header injection (verified via echo server or
 *               by inspecting the block event metadata — auth tokens flow through params)
 *   insecure    SSL verification flags accepted without crashing (file:// offline)
 *   downloadAs  Response body saved to file; PREVIOUS = saved path
 *   cache       ETag/TTL disk cache — second request served from cache
 *   proxy       Proxy param accepted (offline: just verifies the param is wired)
 *   retry       Exponential back-off — attempt count surfaced in event metadata
 *   mqtt        In-process MQTT broker + client round-trip + child message flow
 *   iterable    fetch + decode:json + JSON-array → child items fan over each element
 *   error flow  BlockResult::Error + continueOnError
 *   mcp         mcpCall block + `mcp client` CLI (Tier 1 local stdio; gated on server)
 *   mcp-office  officecli sidecar MCP (tests/office fixtures; view/query verbs)
 *   mcp-router  tanit router HTTP MCP (roaming profile + ZITADEL login; network gated)
 *   mcp-ui      block_params_ui routes (mcp.servers / mcp.tools.* / schema)
 *   mcp-xblox-local  committed network-mcp-local.xblox (--config-dir packages/xblox/tests/xblox)
 *   mcp-xblox-live   committed network-mcp-live.xblox (roaming tanit; gated)
 *
 * All HTTP tests use file:// URLs so no network access is required.
 * MCP tests spawn a local stdio server (code-mcp exe or npx server-memory) and
 * use `--config-dir` with an isolated fixture copy.
 *
 * Run:
 *   node tests/orchestrator/test-xblox-network.mjs
 *   node tests/orchestrator/test-xblox-network.mjs --only cache
 *   node tests/orchestrator/test-xblox-network.mjs --only mcp
 *   node tests/orchestrator/test-xblox-network.mjs --only mcp-client
 *
 *   node tests/orchestrator/test-xblox-network.mjs --only mcp-office
 *
 * Suites: raw · json · downloadAs · iterable · auth · insecure · retry ·
 *         cache · proxy · mqtt · error · mcp · mcp-manifest · mcp-client ·
 *         mcp-block · mcp-ui · mcp-xblox-local · mcp-xblox-live · mcp-live · mcp-office · mcp-office-client · mcp-office-block
 *         mcp-router · mcp-router-client · mcp-router-block · mcp-router-auth-failure
 */

import { spawnSync } from 'node:child_process';
import {
  existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, unlinkSync, writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');

// ── CLI exe location (same logic as test-xblox-next.mjs) ─────────────────────

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

// ── Tiny test runner ──────────────────────────────────────────────────────────

let passed = 0;
let failed = 0;
const failures = [];

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
  ok(
    Object.is(actual, expected) || JSON.stringify(actual) === JSON.stringify(expected),
    `${label} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`,
  );
}

// ── CLI driver ────────────────────────────────────────────────────────────────

const EXE = cliExePath();
const WORK = mkdtempSync(join(tmpdir(), 'xblox-net-'));
// Rich native block manifest from `xblox info --json` exceeds Node's 1 MiB default.
const CLI_MAX_BUFFER = 16 * 1024 * 1024;

function xbloxInfo(opts = {}) {
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'info', '--json'], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 15_000,
    maxBuffer: CLI_MAX_BUFFER,
  });
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    json: parseTrailingJson(r.stdout || ''),
  };
}

function run(doc, opts = {}) {
  const src = join(WORK, `flow-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(src, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
  const args = ['xblox', '--log-level', 'trace', 'run'];
  if (!opts.noJson) args.push('--json');
  args.push('--no-wait', '--src', src);
  if (opts.simulate) args.push('--simulate');
  if (opts.extraArgs) args.push(...opts.extraArgs);
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 30_000,
    maxBuffer: CLI_MAX_BUFFER,
    ...(opts.env ? { env: { ...process.env, ...opts.env } } : {}),
  });
  const report = parseTrailingJson(r.stdout || '');
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    report,
    events: report.events || [],
  };
}

function parseTrailingJson(stdout) {
  const text = String(stdout || '').trim();
  const candidates = [text.lastIndexOf('\n{'), text.indexOf('{')].filter((n) => n >= 0);
  for (const start of candidates) {
    try { return JSON.parse(text.slice(text[start] === '{' ? start : start + 1)); } catch { /* next */ }
  }
  try { return JSON.parse(text); } catch { return { events: [], ok: false }; }
}

function runCli(args, opts = {}) {
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 60_000,
    maxBuffer: opts.maxBuffer ?? CLI_MAX_BUFFER,
    ...(opts.env ? { env: { ...process.env, ...opts.env } } : {}),
  });
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    json: parseTrailingJson(r.stdout || ''),
  };
}

function runXbloxFile(srcPath, opts = {}) {
  const args = [...(opts.configArgs || []), 'xblox', '--log-level', 'off', 'run', '--json', '--no-wait', '--src', srcPath];
  if (opts.simulate) args.push('--simulate');
  if (opts.cliContext) {
    for (const [key, value] of Object.entries(opts.cliContext)) {
      if (value == null || value === '') continue;
      args.push(`--${key}`, String(value));
    }
  }
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 300_000,
    maxBuffer: CLI_MAX_BUFFER,
    ...(opts.env ? { env: { ...process.env, ...opts.env } } : {}),
  });
  const report = parseTrailingJson(r.stdout || '');
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    report,
    events: report.events || [],
  };
}

// file:// URLs that curl handles the same as HTTP (offline, deterministic).
function fileUrl(p) {
  const norm = String(p).replace(/\\/g, '/');
  return process.platform === 'win32' ? `file:///${norm}` : `file://${norm}`;
}

// Event helpers
const byKind = (events, kind) => events.filter((e) => e.kind === kind);
const setVars = (events) => byKind(events, 'setVariable');
const getVars = (events) => byKind(events, 'getVariable');
const logs = (events) => byKind(events, 'log').filter((e) => e.status === 'ok');
const lastLog = (events) => logs(events).at(-1);

// ── Suite: raw fetch ──────────────────────────────────────────────────────────

function suiteRaw() {
  console.log('\n── network/raw ──');

  const txt = join(WORK, `raw-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(txt, 'hello-from-file', 'utf8');

  // Body flows into PREVIOUS via storeAs
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: fileUrl(txt), decode: 'raw', storeAs: 'resp' },
      { kind: 'getVariable', name: 'statusCode', target: 'code' },
      { kind: 'getVariable', name: 'resp', target: 'body' },
    ],
  });
  eq(r1.status, 0, 'raw: exits 0');
  const okEv = byKind(r1.events, 'network').find((e) => e.status === 'ok');
  ok(!!okEv, 'raw: emits ok network event');
  eq(okEv?.data?.result, 'hello-from-file', 'raw: body in event result');
  const gv = (t) => getVars(r1.events).find((e) => e.data?.target === t)?.data?.value;
  eq(gv('body'), 'hello-from-file', 'raw: storeAs var holds the body');
  eq(gv('code'), 0, 'raw: statusCode is 0 for file:// (curl returns 0)');

  // fromCache flag present in event data (false on first request)
  ok(okEv?.data?.fromCache === false || okEv?.data?.fromCache === undefined,
     'raw: fromCache is false on uncached request');
}

// ── Suite: JSON decode + dot-path parse ───────────────────────────────────────

function suiteJson() {
  console.log('\n── network/json ──');

  const jf = join(WORK, `json-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(jf, JSON.stringify({ nested: { v: 'deep' }, items: [1, 2, 3] }), 'utf8');

  // decode:json + parse selector
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'httpRequest', url: fileUrl(jf), decode: 'json', parse: '.nested.v', storeAs: 'picked' },
      { kind: 'log', level: 'info' },
    ],
  });
  eq(r1.status, 0, 'json: exits 0');
  eq(lastLog(r1.events)?.message, 'deep', 'json: parse:.nested.v selects "deep"');

  // Unmatched parse selector → error
  const r2 = run({
    version: 1,
    roots: [{ kind: 'httpRequest', url: fileUrl(jf), decode: 'json', parse: '.missing', continueOnError: true }],
  });
  ok(byKind(r2.events, 'network').some(
       (e) => e.status === 'error' && String(e.message || '').includes('no value')),
     'json: unmatched parse filter → error event');

  // Whole decoded array as result (no parse)
  const arrayFile = join(WORK, `arr-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(arrayFile, JSON.stringify([10, 20, 30]), 'utf8');
  const r3 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: fileUrl(arrayFile), decode: 'json', storeAs: 'arr' },
      { kind: 'getVariable', name: 'arr', target: 'out' },
    ],
  });
  eq(r3.status, 0, 'json/array: exits 0');
  eq(getVars(r3.events).find((e) => e.data?.target === 'out')?.data?.value, [10, 20, 30],
     'json/array: full array stored in variable');
}

// ── Suite: downloadAs ─────────────────────────────────────────────────────────

function suiteDownloadAs() {
  console.log('\n── network/downloadAs ──');

  const src = join(WORK, `dl-src-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(src, 'download-content', 'utf8');

  const dest = join(WORK, `dl-dest-${Math.random().toString(36).slice(2)}.txt`);

  // downloadAs saves the body to a file; PREVIOUS = the saved path
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: fileUrl(src), downloadAs: dest, storeAs: 'dlpath' },
      { kind: 'getVariable', name: 'dlpath', target: 'path' },
    ],
  });
  eq(r1.status, 0, 'downloadAs: exits 0');
  ok(existsSync(dest), 'downloadAs: file exists at destination path');
  eq(readFileSync(dest, 'utf8'), 'download-content', 'downloadAs: file content matches source');

  const okEv = byKind(r1.events, 'network').find((e) => e.status === 'ok');
  ok(!!okEv, 'downloadAs: emits ok network event');
  ok(typeof okEv?.data?.savedPath === 'string' && okEv.data.savedPath.length > 0,
     'downloadAs: event contains savedPath');
  // result (PREVIOUS) is the saved path
  ok(typeof okEv?.data?.result === 'string' && okEv.data.result.length > 0,
     'downloadAs: result in event is the saved path');

  // Chain: downloadAs → the path flows as PREVIOUS into a downstream log
  const dest2 = join(WORK, `dl-dest2-${Math.random().toString(36).slice(2)}.bin`);
  const r2 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: fileUrl(src), downloadAs: dest2, storeAs: 'p' },
      { kind: 'log', level: 'info' },
    ],
  });
  eq(r2.status, 0, 'downloadAs/chain: exits 0');
  ok(lastLog(r2.events)?.message === dest2, 'downloadAs/chain: saved path flows as PREVIOUS into log');
}

// ── Suite: fetch as iterable container ───────────────────────────────────────

function suiteIterable() {
  console.log('\n── network/iterable ──');

  // JSON array response → child blocks iterate once per element
  const af = join(WORK, `iter-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(af, JSON.stringify([
    { name: 'alpha', value: 1 },
    { name: 'beta',  value: 2 },
    { name: 'gamma', value: 3 },
  ]), 'utf8');

  const r1 = run({
    version: 1,
    roots: [{
      kind: 'fetch',
      url: fileUrl(af),
      decode: 'json',
      storeAs: 'items',
      items: [
        // Each iteration: PREVIOUS = element, index = loop counter
        { kind: 'setVariable', name: 'elem_${index}', expression: 'PREVIOUS.name' },
      ],
    }],
  });
  eq(r1.status, 0, 'iterable: exits 0');
  const svs = setVars(r1.events);
  // Should have set elem_0, elem_1, elem_2
  ok(svs.some((e) => e.data?.name === 'elem_0'), 'iterable: elem_0 set (index 0)');
  ok(svs.some((e) => e.data?.name === 'elem_1'), 'iterable: elem_1 set (index 1)');
  ok(svs.some((e) => e.data?.name === 'elem_2'), 'iterable: elem_2 set (index 2)');
  eq(svs.find((e) => e.data?.name === 'elem_0')?.data?.value, 'alpha',
     'iterable: elem_0 = "alpha"');
  eq(svs.find((e) => e.data?.name === 'elem_2')?.data?.value, 'gamma',
     'iterable: elem_2 = "gamma"');

  // Non-array result → child block runs once (normal container behaviour)
  const sf = join(WORK, `scalar-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(sf, JSON.stringify({ single: true }), 'utf8');
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'fetch',
      url: fileUrl(sf),
      decode: 'json',
      items: [{ kind: 'setVariable', name: 'singleRan', value: 1 }],
    }],
  });
  eq(r2.status, 0, 'iterable/scalar: exits 0');
  ok(setVars(r2.events).some((e) => e.data?.name === 'singleRan'),
     'iterable/scalar: child block runs once on non-array result');

  // fetch as iterable — info block registered; xblox info should declare iterable:true
  // (offline manifest check — no network needed)
  const info = xbloxInfo().json;
  const fetchDef = (info?.blocks || []).find((b) => b.kind === 'fetch');
  ok(fetchDef?.execFlags?.iterable === true,
     'iterable/manifest: fetch block declares iterable:true in xblox info');
  const childList = fetchDef?.childLists ?? fetchDef?.children ?? [];
  ok(Array.isArray(childList) && childList.some((cl) => cl.key === 'items'),
     'iterable/manifest: fetch block declares items child list');
}

// ── Suite: auth params (param-wiring verification) ────────────────────────────
//
// Since we can't run a real HTTP server offline, we verify that:
//   a) the block accepts auth params without error on a file:// request
//   b) the block descriptor declares the auth params via xblox info
//   c) Bearer / Basic / ApiKey all parse and the block still completes OK

function suiteAuth() {
  console.log('\n── network/auth ──');

  const txt = join(WORK, `auth-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(txt, 'auth-body', 'utf8');
  const url = fileUrl(txt);

  // Bearer: block should not error on a file:// request (auth headers ignored by curl for file://)
  const r1 = run({
    version: 1,
    roots: [{ kind: 'fetch', url, auth: 'bearer', authToken: 'my-token', storeAs: 'r' }],
  });
  eq(r1.status, 0, 'auth/bearer: exits 0 with bearer param on file://');
  ok(byKind(r1.events, 'network').some((e) => e.status === 'ok'), 'auth/bearer: ok event emitted');

  // Basic
  const r2 = run({
    version: 1,
    roots: [{ kind: 'fetch', url, auth: 'basic', authUser: 'alice', authPass: 'secret', storeAs: 'r' }],
  });
  eq(r2.status, 0, 'auth/basic: exits 0 with basic param on file://');

  // ApiKey (custom header)
  const r3 = run({
    version: 1,
    roots: [{ kind: 'fetch', url, auth: 'apikey', authToken: 'k123', authHeader: 'X-Custom-Key', storeAs: 'r' }],
  });
  eq(r3.status, 0, 'auth/apikey: exits 0 with apikey param on file://');

  // Verify params declared in manifest
  const info = xbloxInfo().json;
  const fetchDef = (info?.blocks || []).find((b) => b.kind === 'fetch');
  const pnames = (fetchDef?.params || []).map((p) => p.name);
  ok(pnames.includes('auth'),      'auth/manifest: fetch declares "auth" param');
  ok(pnames.includes('authToken'), 'auth/manifest: fetch declares "authToken" param');
  ok(pnames.includes('authUser'),  'auth/manifest: fetch declares "authUser" param');
  ok(pnames.includes('authPass'),  'auth/manifest: fetch declares "authPass" param');
  ok(pnames.includes('authHeader'),'auth/manifest: fetch declares "authHeader" param');
}

// ── Suite: insecure / SSL flags ───────────────────────────────────────────────

function suiteInsecure() {
  console.log('\n── network/insecure ──');

  const txt = join(WORK, `ins-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(txt, 'ssl-skipped', 'utf8');

  // insecure:true should be accepted and not break file:// requests
  const r1 = run({
    version: 1,
    roots: [{ kind: 'fetch', url: fileUrl(txt), insecure: true, storeAs: 'r' }],
  });
  eq(r1.status, 0, 'insecure: exits 0 with insecure:true on file://');
  ok(byKind(r1.events, 'network').some((e) => e.status === 'ok'), 'insecure: ok event emitted');

  // verifyPeer:false + verifyHost:false (explicit flags)
  const r2 = run({
    version: 1,
    roots: [{ kind: 'fetch', url: fileUrl(txt), verifyPeer: false, verifyHost: false, storeAs: 'r' }],
  });
  eq(r2.status, 0, 'insecure/flags: exits 0 with verifyPeer:false verifyHost:false');

  // Manifest: fetch declares insecure param
  const info = xbloxInfo().json;
  const fetchDef = (info?.blocks || []).find((b) => b.kind === 'fetch');
  const pnames = (fetchDef?.params || []).map((p) => p.name);
  ok(pnames.includes('insecure'),   'insecure/manifest: fetch declares "insecure" param');
  ok(pnames.includes('verifyPeer'), 'insecure/manifest: fetch declares "verifyPeer" param');
  ok(pnames.includes('verifyHost'), 'insecure/manifest: fetch declares "verifyHost" param');
}

// ── Suite: retry metadata ─────────────────────────────────────────────────────
//
// Verify that the `attempts` counter in the error/ok event metadata reflects
// the actual retry count. Use retries:0 for quick tests; the error event's
// data.attempts should equal 1 (one attempt, no retries).

function suiteRetry() {
  console.log('\n── network/retry ──');

  const missing = fileUrl(join(WORK, `missing-${Math.random().toString(36).slice(2)}.txt`));

  // retries:0 → exactly 1 attempt
  const r1 = run({
    version: 1,
    roots: [{ kind: 'fetch', url: missing, decode: 'raw', retries: 0, continueOnError: true }],
  });
  const errEv1 = byKind(r1.events, 'network').find((e) => e.status === 'error');
  ok(!!errEv1, 'retry: retries:0 → error event emitted');
  eq(errEv1?.data?.attempts, 1, 'retry: retries:0 → attempts=1 in error event');

  // retries:2 → up to 3 attempts (1 + 2 retries) on a curl error
  // retryDelayMs:0 to avoid slowing the test suite
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'fetch', url: missing, decode: 'raw',
      retries: 2, retryDelayMs: 0, continueOnError: true,
    }],
  }, { timeout: 15_000 });
  const errEv2 = byKind(r2.events, 'network').find((e) => e.status === 'error');
  ok(!!errEv2, 'retry: retries:2 → error event emitted');
  eq(errEv2?.data?.attempts, 3, 'retry: retries:2 → attempts=3 in error event');
}

// ── Suite: disk cache ────────────────────────────────────────────────────────
//
// Write a file, fetch it with cache:true, confirm ok.
// On the second fetch, fromCache should be true (body hit, TTL not expired).
// Then test force-expire: set cacheTtlMs:0 + force_revalidate to ensure the
// second request goes to the server (file:// always returns 200 + body, so the
// cache is refreshed and fromCache stays false on a full re-download).

function suiteCache() {
  console.log('\n── network/cache ──');

  const cacheDir = join(WORK, `cache-${Math.random().toString(36).slice(2)}`);
  mkdirSync(cacheDir, { recursive: true });

  const src = join(WORK, `csrc-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(src, 'cached-content', 'utf8');
  const url = fileUrl(src);

  // First fetch: miss → download + store
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'fetch', url,
      cache: true, cacheDir, cacheTtlMs: 60000,
      storeAs: 'r',
    }],
  });
  eq(r1.status, 0, 'cache/first: exits 0');
  const ev1 = byKind(r1.events, 'network').find((e) => e.status === 'ok');
  ok(!!ev1, 'cache/first: ok event');
  eq(ev1?.data?.fromCache, false, 'cache/first: fromCache=false on first fetch');
  eq(ev1?.data?.result, 'cached-content', 'cache/first: body is correct');

  // Second fetch: hit → served from cache
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'fetch', url,
      cache: true, cacheDir, cacheTtlMs: 60000,
      storeAs: 'r',
    }],
  });
  eq(r2.status, 0, 'cache/second: exits 0');
  const ev2 = byKind(r2.events, 'network').find((e) => e.status === 'ok');
  ok(!!ev2, 'cache/second: ok event');
  eq(ev2?.data?.fromCache, true, 'cache/second: fromCache=true on cache hit');
  eq(ev2?.data?.result, 'cached-content', 'cache/second: cached body is correct');

  // TTL=0 → cache entry always considered expired; network is consulted again
  const r3 = run({
    version: 1,
    roots: [{
      kind: 'fetch', url,
      cache: true, cacheDir, cacheTtlMs: 0,
      storeAs: 'r',
    }],
  });
  eq(r3.status, 0, 'cache/expired: exits 0 with TTL=0');
  const ev3 = byKind(r3.events, 'network').find((e) => e.status === 'ok');
  ok(!!ev3, 'cache/expired: ok event');
  // With TTL=0 the cached entry is always stale; ETag/Last-Modified revalidation
  // fires. file:// returns 200 (not 304), so fromCache will be false.
  eq(ev3?.data?.fromCache, false, 'cache/expired: fromCache=false after TTL expiry');

  // Manifest: fetch declares cache params
  const info = xbloxInfo().json;
  const fetchDef = (info?.blocks || []).find((b) => b.kind === 'fetch');
  const pnames = (fetchDef?.params || []).map((p) => p.name);
  ok(pnames.includes('cache'),       'cache/manifest: fetch declares "cache" param');
  ok(pnames.includes('cacheDir'),    'cache/manifest: fetch declares "cacheDir" param');
  ok(pnames.includes('cacheTtlMs'), 'cache/manifest: fetch declares "cacheTtlMs" param');
}

// ── Suite: proxy params ───────────────────────────────────────────────────────
//
// We can't test a real proxy offline, but we can verify:
//   a) proxy param is accepted without breaking file:// requests
//   b) the manifest declares proxy / proxyUser / proxyPass

function suiteProxy() {
  console.log('\n── network/proxy ──');

  const txt = join(WORK, `prx-${Math.random().toString(36).slice(2)}.txt`);
  writeFileSync(txt, 'proxy-test', 'utf8');

  // Supply an unreachable proxy — curl should still resolve file:// directly
  // (CURLOPT_PROXY is set but file:// bypasses it).
  const r1 = run({
    version: 1,
    roots: [{ kind: 'fetch', url: fileUrl(txt), proxy: 'http://127.0.0.1:19999', storeAs: 'r' }],
  });
  // file:// ignores the proxy setting, so this should still succeed.
  eq(r1.status, 0, 'proxy: exits 0 (file:// ignores proxy)');
  ok(byKind(r1.events, 'network').some((e) => e.status === 'ok'), 'proxy: ok event emitted');

  const info = xbloxInfo().json;
  const fetchDef = (info?.blocks || []).find((b) => b.kind === 'fetch');
  const pnames = (fetchDef?.params || []).map((p) => p.name);
  ok(pnames.includes('proxy'),     'proxy/manifest: fetch declares "proxy" param');
  ok(pnames.includes('proxyUser'), 'proxy/manifest: fetch declares "proxyUser" param');
  ok(pnames.includes('proxyPass'), 'proxy/manifest: fetch declares "proxyPass" param');
}

// ── Suite: MQTT server/client + child message flow ────────────────────────────

function suiteMqtt() {
  console.log('\n── network/mqtt ──');

  const port = 18_884 + Math.floor(Math.random() * 1000);
  const payload = `mqtt-payload-${Math.random().toString(36).slice(2)}`;

  const r1 = run({
    version: 1,
    roots: [
      {
        kind: 'mqttServer',
        action: 'Start',
        instance: 'orch',
        bind: '127.0.0.1',
        port,
        storeAs: 'mqttServer',
      },
      {
        kind: 'mqttClient',
        action: 'RoundTrip',
        host: '127.0.0.1',
        port,
        topic: 'xblox/orchestrator',
        payload,
        timeoutMs: 5000,
        storeAs: 'mqtt',
        items: [
          // Each received MQTT message is exposed as PREVIOUS={topic,payload,host,port}.
          { kind: 'Parse', parser: 'jq', filter: '.payload', storeAs: 'mqttPayload' },
          { kind: 'log', level: 'info', message: 'mqttPayload' },
        ],
      },
      {
        kind: 'mqttServer',
        action: 'Status',
        instance: 'orch',
        storeAs: 'mqttServerStatus',
      },
      {
        kind: 'mqttServer',
        action: 'Stop',
        instance: 'orch',
      },
    ],
  }, { timeout: 20_000 });

  eq(r1.status, 0, 'mqtt: exits 0');
  ok(byKind(r1.events, 'mqttServer').some((e) => e.status === 'ok' && String(e.message || '').includes('started')),
     'mqtt: broker starts');
  ok(byKind(r1.events, 'mqttClient').some((e) => e.status === 'ok'),
     'mqtt: client round-trip emits ok event');
  eq(lastLog(r1.events)?.message, payload,
     'mqtt: child Parse + log sees received message payload via PREVIOUS');

  const clientEv = byKind(r1.events, 'mqttClient').find((e) => e.status === 'ok');
  eq(clientEv?.data?.result?.messages?.[0]?.topic, 'xblox/orchestrator',
     'mqtt: event result includes received topic');
  eq(clientEv?.data?.result?.messages?.[0]?.payload, payload,
     'mqtt: event result includes received payload');

  const statusEv = byKind(r1.events, 'mqttServer').find((e) => e.data?.result?.publishes >= 1);
  ok(!!statusEv, 'mqtt: broker status includes publish count');
  ok(byKind(r1.events, 'mqttServer').some((e) => String(e.message || '').includes('stopped')),
     'mqtt: broker stops');

  const info = xbloxInfo().json;
  const serverDef = (info?.blocks || []).find((b) => b.kind === 'mqttServer');
  const clientDef = (info?.blocks || []).find((b) => b.kind === 'mqttClient');
  ok(!!serverDef, 'mqtt/manifest: mqttServer block registered');
  ok(!!clientDef, 'mqtt/manifest: mqttClient block registered');
  const childList = clientDef?.childLists ?? clientDef?.children ?? [];
  ok(Array.isArray(childList) && childList.some((cl) => cl.key === 'items'),
     'mqtt/manifest: mqttClient declares items child list');
}

// ── Suite: error paths ────────────────────────────────────────────────────────

function suiteError() {
  console.log('\n── network/error ──');

  const missing = fileUrl(join(WORK, `err-${Math.random().toString(36).slice(2)}.txt`));

  // Missing file → network error event; trailing block NOT run (abort-on-error default)
  const r1 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: missing, retries: 0 },
      { kind: 'setVariable', name: 'after', value: 99 },
    ],
  });
  ok(byKind(r1.events, 'network').some((e) => e.status === 'error'),
     'error: missing file → error event');
  ok(!setVars(r1.events).some((e) => e.data?.name === 'after'),
     'error: Error result aborts the chain');
  ok(r1.status !== 0, 'error: run exits non-zero on unhandled error');

  // continueOnError → trailing block runs
  const r2 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: missing, retries: 0, continueOnError: true },
      { kind: 'setVariable', name: 'after', value: 99 },
    ],
  });
  ok(setVars(r2.events).some((e) => e.data?.name === 'after' && e.data?.value === 99),
     'error/continueOnError: trailing setVariable runs after fetch error');

  // Empty URL → immediate error
  const r3 = run({
    version: 1,
    roots: [{ kind: 'fetch', url: '', retries: 0, continueOnError: true }],
  });
  ok(
    byKind(r3.events, 'network').some((e) => e.status === 'error')
    || byKind(r3.events, 'fetch').some((e) => e.status === 'error'),
    'error/emptyUrl: empty url → error event',
  );

  // statusCode typed output is readable even on error
  const r4 = run({
    version: 1,
    roots: [
      { kind: 'fetch', url: missing, retries: 0, continueOnError: true },
      { kind: 'getVariable', name: 'statusCode', target: 'code' },
    ],
  });
  const codeVal = getVars(r4.events).find((e) => e.data?.target === 'code')?.data?.value;
  ok(typeof codeVal === 'number', 'error/statusCode: statusCode output is a number on error');
}

// ── Suite: MCP (Tier 1 local stdio) ───────────────────────────────────────────
//
// Uses packages/xblox/tests/xblox/mcp.json via --config-dir. code-mcp command resolves through
// dist/.../bin sidecars (see app_exe_directory.cpp / resolve_third_party_executable).
//
// Grouped sub-suites:
//   mcp/manifest  offline xblox info assertions
//   mcp/client    Phase 7 CLI introspection (--config-dir packages/xblox/tests/xblox)
//   mcp/ui        block_params_ui routes (mcp.servers / mcp.tools.* / schema)
//   mcp/xblox-local  committed packages/xblox/tests/xblox/network-mcp-local.xblox (--config-dir)
//   mcp/block     inline mcpCall blocks (index/search)
//   mcp/live      optional user-default config (PM_MCP_LIVE=1, no --config-dir)

const MCP_CONFIG_DIR = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox');
const MCP_CONFIG_FILE = join(MCP_CONFIG_DIR, 'mcp.json');
const MCP_XBLOX_LOCAL = join(MCP_CONFIG_DIR, 'network-mcp-local.xblox');
const MCP_XBLOX_LIVE = join(MCP_CONFIG_DIR, 'network-mcp-live.xblox');
const MCP_SERVER = 'code-mcp';
const MCP_SIDECAR = 'codebase-memory-mcp.exe';

let mcpCtxCache = null;

function skipMcpLine(reason) {
  console.log(`  ⊘ skipped: ${reason}`);
}

function exeSidecarPath(name) {
  return join(dirname(EXE), 'bin', name);
}

function posixPath(p) {
  return String(p).replace(/\\/g, '/');
}

function codebaseMemoryProjectName(repoPath) {
  return posixPath(repoPath).replace(/\/$/, '').replace(':/', '-').replace(/\//g, '-');
}

function mcpResultPayload(value) {
  if (!value || typeof value !== 'object') return null;
  if (Array.isArray(value.content)) {
    for (const block of value.content) {
      if (block?.type === 'text' && typeof block.text === 'string') {
        try { return JSON.parse(block.text); } catch { return block.text; }
      }
    }
  }
  return value;
}

function searchHasHits(payload) {
  const body = mcpResultPayload(payload);
  if (!body || typeof body !== 'object') return false;
  if (Array.isArray(body.results) && body.results.length > 0) return true;
  if (Array.isArray(body.semantic_results) && body.semantic_results.length > 0) return true;
  if (typeof body.total === 'number' && body.total > 0) return true;
  if (typeof body.total_results === 'number' && body.total_results > 0) return true;
  return false;
}

function prepareMcpFixture() {
  if (mcpCtxCache) return mcpCtxCache;

  if (!existsSync(MCP_CONFIG_FILE)) {
    mcpCtxCache = { skip: true, reason: `missing ${MCP_CONFIG_FILE}` };
    return mcpCtxCache;
  }

  const sidecar = exeSidecarPath(MCP_SIDECAR);
  if (!existsSync(sidecar)) {
    mcpCtxCache = {
      skip: true,
      reason: `${MCP_SIDECAR} not found in ${join(dirname(EXE), 'bin')} (build sidecars first)`,
    };
    return mcpCtxCache;
  }

  const configArgs = ['--config-dir', MCP_CONFIG_DIR];

  const listRes = runCli([...configArgs, 'mcp', '--json', 'client', 'list'], {
    timeout: 30_000,
  });
  if (listRes.status !== 0 || !listRes.json?.ok) {
    mcpCtxCache = {
      skip: true,
      reason: `packages/xblox/tests/xblox/mcp.json not loadable (${listRes.json?.error || listRes.stderr.trim() || 'list failed'})`,
    };
    return mcpCtxCache;
  }

  const toolsRes = runCli([
    ...configArgs, 'mcp', '--json', 'client', 'tools', '--server', MCP_SERVER,
  ], { timeout: 180_000 });
  const tools = toolsRes.json?.tools;
  const hasIndex = Array.isArray(tools) && tools.some((t) => t.name === 'index_repository');
  const hasSearch = Array.isArray(tools) && tools.some((t) => t.name === 'search_graph');
  if (toolsRes.status !== 0 || !toolsRes.json?.tools_list_ok || !hasIndex || !hasSearch) {
    mcpCtxCache = {
      skip: true,
      reason: `MCP server "${MCP_SERVER}" unavailable or missing index/search tools (${toolsRes.json?.error || toolsRes.stderr.trim() || 'tools/list failed'})`,
    };
    return mcpCtxCache;
  }

  mcpCtxCache = {
    skip: false,
    configDir: MCP_CONFIG_DIR,
    serverName: MCP_SERVER,
    schemaTool: 'index_repository',
    searchTool: 'search_graph',
    indexTool: 'index_repository',
    configArgs,
    docsPath: posixPath(join(PACKAGE_ROOT, 'docs')),
    srcPath: posixPath(join(PACKAGE_ROOT, 'src')),
    sidecar,
  };
  return mcpCtxCache;
}

function mcpClientCli(ctx, verbArgs, opts = {}) {
  return runCli([...ctx.configArgs, 'mcp', '--json', 'client', ...verbArgs], {
    timeout: 180_000,
    ...opts,
  });
}

function suiteMcpManifest() {
  console.log('\n  ── mcp/manifest ──');

  const info = xbloxInfo();
  eq(info.status, 0, 'mcp/manifest: xblox info exits 0');
  const mcpCall = (info.json?.blocks || []).find((b) => b.kind === 'mcpCall');
  ok(!!mcpCall, 'mcp/manifest: mcpCall block registered');
  const pnames = (mcpCall?.params || []).map((p) => p.name);
  for (const name of ['server', 'tool', 'arguments', 'timeoutMs', 'storeAs']) {
    ok(pnames.includes(name), `mcp/manifest: mcpCall declares "${name}" param`);
  }
}

function suiteMcpClient(ctx) {
  console.log('\n  ── mcp/client ──');

  const list = mcpClientCli(ctx, ['list']);
  eq(list.status, 0, 'mcp/client/list: exits 0');
  ok(list.json?.ok === true, 'mcp/client/list: ok=true');
  ok((list.json?.servers || []).some((s) => s.name === ctx.serverName),
     `mcp/client/list: includes fixture server "${ctx.serverName}"`);

  const tools = mcpClientCli(ctx, ['tools', '--server', ctx.serverName]);
  eq(tools.status, 0, 'mcp/client/tools: exits 0');
  ok(tools.json?.tools_list_ok === true, 'mcp/client/tools: tools_list_ok');
  ok((tools.json?.tools || []).length >= 1, 'mcp/client/tools: returns ≥1 tool');

  const schema = mcpClientCli(ctx, ['schema', '--server', ctx.serverName, '--tool', ctx.schemaTool]);
  eq(schema.status, 0, 'mcp/client/schema: exits 0');
  ok(schema.json?.ok === true, 'mcp/client/schema: ok=true');
  ok(schema.json?.inputSchema?.type === 'object',
     'mcp/client/schema: inputSchema is an object schema');
  ok(Array.isArray(schema.json?.inputSchema?.required)
     && schema.json.inputSchema.required.includes('repo_path'),
     'mcp/client/schema: index_repository requires repo_path');
}

function mcpCallCli(ctx, tool, args) {
  return runCli([
    ...ctx.configArgs,
    'mcp', '--json', 'client', 'call',
    '--server', ctx.serverName,
    '--tool', tool,
    '--args', JSON.stringify(args),
  ], { timeout: 300_000 });
}

function mcpCallBlock(ctx, tool, args, storeAs = 'mcpResult') {
  return run({
    version: 1,
    roots: [
      {
        kind: 'mcpCall',
        server: ctx.serverName,
        tool,
        arguments: args,
        timeoutMs: 300_000,
        storeAs,
      },
      { kind: 'getVariable', name: storeAs, target: 'out' },
    ],
  }, { extraArgs: ctx.configArgs, timeout: 300_000 });
}

function tryIndexAndSearch(ctx, repoPath, label, query = 'mcp') {
  const project = codebaseMemoryProjectName(repoPath);
  const indexArgs = { repo_path: repoPath, mode: 'fast' };

  const rIndex = mcpCallBlock(ctx, ctx.indexTool, indexArgs, 'mcpIndex');
  const indexOk = rIndex.status === 0
    && byKind(rIndex.events, 'mcpCall').some((e) => e.status === 'ok');
  if (!indexOk) {
    ok(false, `${label}/index: mcpCall index_repository ok`);
    return { ok: false, project };
  }
  ok(true, `${label}/index: mcpCall index_repository ok`);

  const rSearch = mcpCallBlock(ctx, ctx.searchTool, {
    project,
    query,
    limit: 10,
  }, 'mcpSearch');
  const searchEv = byKind(rSearch.events, 'mcpCall').find((e) => e.status === 'ok');
  const outVar = getVars(rSearch.events).find((e) => e.data?.target === 'out');
  const hits = searchHasHits(outVar?.data?.value) || searchHasHits(searchEv?.data?.result);
  if (!searchEv || rSearch.status !== 0) {
    ok(false, `${label}/search: mcpCall search_graph ok`);
    return { ok: false, project };
  }
  ok(true, `${label}/search: mcpCall search_graph ok`);
  if (!hits) return { ok: false, project };
  ok(true, `${label}/search: search_graph returned hits for query "${query}"`);
  return { ok: true, project };
}

function suiteMcpBlock(ctx) {
  console.log('\n  ── mcp/block ──');

  let winner = tryIndexAndSearch(ctx, ctx.docsPath, 'mcp/block/docs', 'xblox');
  if (!winner.ok) {
    console.log('  · docs index/search failed — trying ./src');
    winner = tryIndexAndSearch(ctx, ctx.srcPath, 'mcp/block/src', 'mcp');
  }
  ok(winner.ok, 'mcp/block: index + search succeeded on docs or src');

  const cliCall = mcpCallCli(ctx, ctx.searchTool, {
    project: winner.project,
    query: 'mcp',
    limit: 5,
  });
  eq(cliCall.status, 0, 'mcp/block/cli-call: exits 0');
  ok(cliCall.json?.ok === true, 'mcp/block/cli-call: ok=true');
  ok(searchHasHits(cliCall.json?.result), 'mcp/block/cli-call: search result has hits');

  const r2 = run({
    version: 1,
    roots: [{
      kind: 'mcpCall',
      server: ctx.serverName,
      tool: '__xblox_missing_tool__',
      continueOnError: true,
    }],
  }, { extraArgs: ctx.configArgs, timeout: 180_000 });
  ok(byKind(r2.events, 'mcpCall').some((e) => e.status === 'error'),
     'mcp/block/error: missing tool → error event');

  const r3 = run({
    version: 1,
    roots: [{
      kind: 'mcpCall',
      server: ctx.serverName,
      tool: ctx.searchTool,
      storeAs: 'simResult',
    }],
  }, { simulate: true, extraArgs: ctx.configArgs, timeout: 30_000 });
  eq(r3.status, 0, 'mcp/block/simulate: exits 0');
  const simEv = byKind(r3.events, 'mcpCall').find((e) => e.status === 'ok');
  ok(!!simEv, 'mcp/block/simulate: emits ok mcpCall event');
  ok(simEv?.data?.simulated === true || String(simEv?.message || '').includes('simulated'),
     'mcp/block/simulate: event marked simulated');
}

function suiteMcpLive() {
  console.log('\n  ── mcp/live ──');

  if (process.env.PM_MCP_LIVE !== '1') {
    skipMcpLine('set PM_MCP_LIVE=1 to exercise the default profile mcp.json (optional)');
    return;
  }

  const list = runCli(['mcp', '--json', 'client', 'list'], { timeout: 30_000 });
  eq(list.status, 0, 'mcp/live/list: exits 0');
  ok(list.json?.ok === true, 'mcp/live/list: ok=true');
  ok(Array.isArray(list.json?.servers), 'mcp/live/list: servers array present');
}

function parseStdoutJson(stdout) {
  const text = String(stdout || '').trim();
  try { return JSON.parse(text); } catch { return parseTrailingJson(stdout); }
}

function sortedNames(rows, field = 'value') {
  return (Array.isArray(rows) ? rows : [])
    .map((row) => row?.[field])
    .filter((name) => typeof name === 'string' && name.length > 0)
    .sort();
}

function suiteMcpUi(ctx) {
  console.log('\n  ── mcp/ui ──');

  // Gold source: same cached probe path as `mcp client` (Phase 7).
  const clientList = mcpClientCli(ctx, ['list']);
  eq(clientList.status, 0, 'mcp/ui/preflight/client-list: exits 0');
  ok(clientList.json?.ok === true, 'mcp/ui/preflight/client-list: ok=true');

  const clientTools = mcpClientCli(ctx, ['tools', '--server', ctx.serverName]);
  eq(clientTools.status, 0, 'mcp/ui/preflight/client-tools: exits 0');
  ok(clientTools.json?.tools_list_ok === true, 'mcp/ui/preflight/client-tools: tools_list_ok');
  ok(clientTools.json?.handshake_ok === true, 'mcp/ui/preflight/client-tools: handshake_ok');
  const clientToolNames = sortedNames(clientTools.json?.tools, 'name');
  ok(clientToolNames.length >= 2,
     `mcp/ui/preflight/client-tools: catalog has ≥2 tools (got ${clientToolNames.length})`);
  ok(clientToolNames.includes(ctx.searchTool), 'mcp/ui/preflight/client-tools: includes search_graph');
  ok(clientToolNames.includes(ctx.indexTool), 'mcp/ui/preflight/client-tools: includes index_repository');

  const clientSearchSchema = mcpClientCli(ctx, [
    'schema', '--server', ctx.serverName, '--tool', ctx.searchTool,
  ]);
  eq(clientSearchSchema.status, 0, 'mcp/ui/preflight/client-schema-search: exits 0');
  ok(clientSearchSchema.json?.ok === true, 'mcp/ui/preflight/client-schema-search: ok=true');

  const clientIndexSchema = mcpClientCli(ctx, [
    'schema', '--server', ctx.serverName, '--tool', ctx.schemaTool,
  ]);
  eq(clientIndexSchema.status, 0, 'mcp/ui/preflight/client-schema-index: exits 0');
  ok(clientIndexSchema.json?.ok === true, 'mcp/ui/preflight/client-schema-index: ok=true');

  const enabledClientNames = (clientList.json?.servers || [])
    .filter((s) => s.enabled !== false)
    .map((s) => s.name)
    .filter((name) => typeof name === 'string')
    .sort();

  const servers = runCli([...ctx.configArgs, 'xblox', 'info', 'options', '--path', 'mcp.servers']);
  eq(servers.status, 0, 'mcp/ui/servers: exits 0');
  const serverOpts = parseStdoutJson(servers.stdout);
  const uiServerNames = sortedNames(serverOpts).filter((name) => name !== '__custom__');
  ok(uiServerNames.length >= 2,
     `mcp/ui/servers: lists ≥2 probed servers (got ${uiServerNames.length})`);
  ok(uiServerNames.includes(ctx.serverName),
     `mcp/ui/servers: includes fixture server "${ctx.serverName}"`);
  ok(uiServerNames.every((name) => enabledClientNames.includes(name)),
     'mcp/ui/servers: every UI server is enabled in mcp.json');
  ok(enabledClientNames.includes(ctx.serverName),
     'mcp/ui/servers: fixture server enabled in client list');

  const tools = runCli([
    ...ctx.configArgs, 'xblox', 'info', 'options', '--path', `mcp.tools.${ctx.serverName}`,
  ]);
  eq(tools.status, 0, 'mcp/ui/tools: exits 0');
  const toolOpts = parseStdoutJson(tools.stdout);
  const uiToolNames = sortedNames(toolOpts);
  eq(uiToolNames.length, clientToolNames.length,
     `mcp/ui/tools: count matches client catalog (${uiToolNames.length} vs ${clientToolNames.length})`);
  eq(uiToolNames, clientToolNames, 'mcp/ui/tools: names match client catalog');
  for (const ct of clientTools.json.tools) {
    const uiRow = toolOpts.find((o) => o.value === ct.name);
    ok(!!uiRow, `mcp/ui/tools: row present for ${ct.name}`);
    if (typeof ct.description === 'string' && ct.description.length > 0) {
      eq(uiRow.description, ct.description, `mcp/ui/tools: description for ${ct.name}`);
    }
  }

  const schema = runCli([
    ...ctx.configArgs, 'xblox', 'info', 'schema',
    '--schema-path', 'mcp.tools',
    '--provider', ctx.serverName,
    '--model', ctx.searchTool,
  ]);
  eq(schema.status, 0, 'mcp/ui/schema-search: exits 0');
  const schemaDoc = parseStdoutJson(schema.stdout);
  ok(schemaDoc?.block === 'mcpCall', 'mcp/ui/schema-search: block=mcpCall');
  eq(schemaDoc?.provider, ctx.serverName, 'mcp/ui/schema-search: provider=server');
  eq(schemaDoc?.model, ctx.searchTool, 'mcp/ui/schema-search: model=tool');
  ok(schemaDoc?.value_field === 'arguments', 'mcp/ui/schema-search: value_field=arguments');
  ok(schemaDoc?.schema?.type === 'object', 'mcp/ui/schema-search: schema is object');
  eq(
    JSON.stringify(schemaDoc?.schema?.properties ?? null),
    JSON.stringify(clientSearchSchema.json?.inputSchema?.properties ?? null),
    'mcp/ui/schema-search: properties match client schema',
  );
  eq(
    JSON.stringify(schemaDoc?.schema?.required ?? []),
    JSON.stringify(clientSearchSchema.json?.inputSchema?.required ?? []),
    'mcp/ui/schema-search: required match client schema',
  );
  ok(schemaDoc?.schema?.properties?.project != null,
     'mcp/ui/schema-search: search_graph exposes project property');
  ok(Array.isArray(schemaDoc?.schema?.required)
     && schemaDoc.schema.required.includes('project'),
     'mcp/ui/schema-search: project is required');

  const indexSchema = runCli([
    ...ctx.configArgs, 'xblox', 'info', 'schema',
    '--schema-path', 'mcp.tools',
    '--provider', ctx.serverName,
    '--model', ctx.schemaTool,
  ]);
  eq(indexSchema.status, 0, 'mcp/ui/schema-index: exits 0');
  const indexDoc = parseStdoutJson(indexSchema.stdout);
  eq(
    JSON.stringify(indexDoc?.schema?.properties ?? null),
    JSON.stringify(clientIndexSchema.json?.inputSchema?.properties ?? null),
    'mcp/ui/schema-index: properties match client schema',
  );
  ok(Array.isArray(indexDoc?.schema?.required)
     && indexDoc.schema.required.includes('repo_path'),
     'mcp/ui/schema-index: repo_path is required');

  const unknownTools = runCli([
    ...ctx.configArgs, 'xblox', 'info', 'options', '--path', 'mcp.tools.__no_such_server__',
  ]);
  eq(unknownTools.status, 1, 'mcp/ui/tools-unknown-server: exits 1 when catalog empty');
  const unknownOpts = parseStdoutJson(unknownTools.stdout);
  ok(Array.isArray(unknownOpts) && unknownOpts.length === 0,
     'mcp/ui/tools-unknown-server: returns empty array');
}

function tryRunMcpXbloxLocal(ctx, repoPath, label, query) {
  const project = codebaseMemoryProjectName(repoPath);
  const r = runXbloxFile(MCP_XBLOX_LOCAL, {
    configArgs: ctx.configArgs,
    timeout: 300_000,
    cliContext: {
      server: ctx.serverName,
      repoPath,
      project,
      query,
      searchLimit: 10,
    },
  });
  const indexEv = byKind(r.events, 'mcpCall').find((e) => e.data?.tool === 'index_repository');
  const searchEv = byKind(r.events, 'mcpCall').find((e) => e.data?.tool === 'search_graph');
  const searchVar = getVars(r.events).find((e) => e.data?.target === 'searchOut');
  const indexOk = r.status === 0 && indexEv?.status === 'ok';
  const searchOk = r.status === 0 && searchEv?.status === 'ok';
  const hits = searchHasHits(searchVar?.data?.value) || searchHasHits(searchEv?.data?.result);
  if (!indexOk || !searchOk || !hits) {
    console.log(`  · ${label} xblox run did not succeed (status=${r.status})`);
    return { ok: false, project };
  }
  ok(true, `${label}/xblox: index_repository ok`);
  ok(true, `${label}/xblox: search_graph ok`);
  ok(true, `${label}/xblox: search_graph returned hits for query "${query}"`);
  ok(logs(r.events).some((e) => String(e.message || '').includes('mcp search stored')),
     `${label}/xblox: completion log emitted`);
  return { ok: true, project };
}

function suiteMcpXbloxLocal(ctx) {
  console.log('\n  ── mcp/xblox-local ──');
  ok(existsSync(MCP_XBLOX_LOCAL), 'mcp/xblox-local: fixture file exists');

  let winner = tryRunMcpXbloxLocal(ctx, ctx.docsPath, 'mcp/xblox-local/docs', 'xblox');
  if (!winner.ok) {
    console.log('  · docs run failed — trying ./src');
    winner = tryRunMcpXbloxLocal(ctx, ctx.srcPath, 'mcp/xblox-local/src', 'mcp');
  }
  ok(winner.ok, 'mcp/xblox-local: committed doc succeeded on docs or src');

  const sim = runXbloxFile(MCP_XBLOX_LOCAL, {
    configArgs: ctx.configArgs,
    simulate: true,
    timeout: 60_000,
    cliContext: {
      server: ctx.serverName,
      repoPath: ctx.srcPath,
      project: codebaseMemoryProjectName(ctx.srcPath),
      query: 'mcp',
    },
  });
  eq(sim.status, 0, 'mcp/xblox-local/simulate: exits 0');
  const simEvs = byKind(sim.events, 'mcpCall').filter((e) => e.status === 'ok');
  ok(simEvs.length >= 2, 'mcp/xblox-local/simulate: index + search mcpCall events');
  ok(simEvs.every((e) => e.data?.simulated === true || String(e.message || '').includes('simulated')),
     'mcp/xblox-local/simulate: events marked simulated');
}

function suiteMcpXbloxLive() {
  console.log('\n── mcp-xblox-live ──');
  ok(existsSync(MCP_XBLOX_LIVE), 'mcp/xblox-live: fixture file exists');

  const ctx = prepareMcpRouterFixture();
  if (ctx.skip) {
    skipMcpLine(ctx.reason);
    return;
  }

  console.log(`  (profile: ${ctx.mcpJsonPath}, tool: ${ctx.toolName})`);
  const r = runXbloxFile(MCP_XBLOX_LIVE, {
    timeout: 300_000,
    cliContext: {
      server: ctx.serverName,
      tool: ctx.toolName,
    },
  });
  eq(r.status, 0, 'mcp/xblox-live: exits 0');
  ok(r.report?.ok === true, 'mcp/xblox-live: report ok=true');
  const callEv = byKind(r.events, 'mcpCall').find((e) => e.data?.tool === ctx.toolName);
  ok(callEv?.status === 'ok', 'mcp/xblox-live: mcpCall event ok');
  ok(callEv?.data?.result != null, 'mcp/xblox-live: result present');
  const parseEv = byKind(r.events, 'Parse').find((e) => e.status === 'ok' && e.data?.input === 'mcpLiveResult');
  ok(!!parseEv, 'mcp/xblox-live: Parse summarizes mcp content');
  ok(Number(parseEv?.data?.result?.count) >= 1, 'mcp/xblox-live: parsed count >= 1');
  const summaryLog = logs(r.events).find((e) => String(e.message || '').includes('count'));
  ok(!!summaryLog, 'mcp/xblox-live: log emits parsed summary');
  ok(logs(r.events).some((e) => String(e.message || '').includes('Afrikaans')
     || String(e.message || '').includes('Arabic')),
     'mcp/xblox-live: log sample includes a known language');
}

function suiteMcp(group = null) {
  console.log('\n── mcp ──');

  if (group === 'manifest') {
    suiteMcpManifest();
    return;
  }
  if (group === 'live') {
    suiteMcpLive();
    return;
  }

  const runGroup = (name, fn) => {
    if (group && group !== name) return;
    fn();
  };

  runGroup('manifest', suiteMcpManifest);

  const ctx = prepareMcpFixture();
  if (ctx.skip) {
    if (!group || group === 'client' || group === 'block') {
      skipMcpLine(ctx.reason);
    }
    runGroup('live', suiteMcpLive);
    return;
  }

  console.log(`  (config: packages/xblox/tests/xblox/mcp.json, sidecar: ${ctx.sidecar})`);
  runGroup('client', () => suiteMcpClient(ctx));
  runGroup('ui', () => suiteMcpUi(ctx));
  runGroup('xblox-local', () => suiteMcpXbloxLocal(ctx));
  runGroup('block', () => suiteMcpBlock(ctx));
  runGroup('live', suiteMcpLive);
}

// ── Suite: MCP officecli (Tier 1 local stdio, tests/office fixtures) ───────────
//
// officecli sidecar: `officecli mcp` stdio server, single `officecli` tool wrapping
// document verbs (view, query, get, set, …). Fixtures under tests/office/.

const OFFICE_SERVER = 'officecli';
const OFFICE_TOOL = 'officecli';
const OFFICE_SIDECAR = 'officecli.exe';
const OFFICE_FIXTURE_DIR = join(PACKAGE_ROOT, 'tests', 'office');
const OFFICE_CATALOG_FILE = join(OFFICE_FIXTURE_DIR, 'product-catalog.xlsx');
const OFFICE_SPECS_FILE = join(OFFICE_FIXTURE_DIR, 'specs.xlsx');
const OFFICE_VERBS = [
  'create', 'view', 'get', 'query', 'set', 'add', 'remove', 'move',
  'swap', 'validate', 'batch', 'raw', 'help', 'load_skill',
];

let mcpOfficeCtxCache = null;

function mcpResultText(value) {
  const body = mcpResultPayload(value);
  return typeof body === 'string' ? body : null;
}

function officeQueryHasMatches(payload) {
  const body = mcpResultPayload(payload);
  if (!body || typeof body !== 'object') return false;
  if (typeof body.matches === 'number' && body.matches > 0) return true;
  return Array.isArray(body.results) && body.results.length > 0;
}

function prepareMcpOfficeFixture() {
  if (mcpOfficeCtxCache) return mcpOfficeCtxCache;

  if (!existsSync(MCP_CONFIG_FILE)) {
    mcpOfficeCtxCache = { skip: true, reason: `missing ${MCP_CONFIG_FILE}` };
    return mcpOfficeCtxCache;
  }

  const sidecar = exeSidecarPath(OFFICE_SIDECAR);
  if (!existsSync(sidecar)) {
    mcpOfficeCtxCache = {
      skip: true,
      reason: `${OFFICE_SIDECAR} not found in ${join(dirname(EXE), 'bin')} (build sidecars first)`,
    };
    return mcpOfficeCtxCache;
  }

  if (!existsSync(OFFICE_CATALOG_FILE) || !existsSync(OFFICE_SPECS_FILE)) {
    mcpOfficeCtxCache = {
      skip: true,
      reason: `missing tests/office fixtures (${OFFICE_CATALOG_FILE} or ${OFFICE_SPECS_FILE})`,
    };
    return mcpOfficeCtxCache;
  }

  const configArgs = ['--config-dir', MCP_CONFIG_DIR];

  const listRes = runCli([...configArgs, 'mcp', '--json', 'client', 'list'], {
    timeout: 30_000,
  });
  if (listRes.status !== 0 || !listRes.json?.ok) {
    mcpOfficeCtxCache = {
      skip: true,
      reason: `officecli: mcp.json not loadable (${listRes.json?.error || listRes.stderr.trim() || 'list failed'})`,
    };
    return mcpOfficeCtxCache;
  }
  if (!(listRes.json?.servers || []).some((s) => s.name === OFFICE_SERVER)) {
    mcpOfficeCtxCache = {
      skip: true,
      reason: `officecli: "${OFFICE_SERVER}" missing from mcp client list`,
    };
    return mcpOfficeCtxCache;
  }

  const toolsRes = runCli([
    ...configArgs, 'mcp', '--json', 'client', 'tools', '--server', OFFICE_SERVER,
  ], { timeout: 120_000 });
  const tool = (toolsRes.json?.tools || []).find((t) => t.name === OFFICE_TOOL);
  if (toolsRes.status !== 0 || !toolsRes.json?.tools_list_ok || !tool) {
    mcpOfficeCtxCache = {
      skip: true,
      reason: `officecli MCP unavailable (${toolsRes.json?.error || toolsRes.stderr.trim() || 'tools/list failed'})`,
    };
    return mcpOfficeCtxCache;
  }

  mcpOfficeCtxCache = {
    skip: false,
    configDir: MCP_CONFIG_DIR,
    serverName: OFFICE_SERVER,
    toolName: OFFICE_TOOL,
    configArgs,
    catalogPath: posixPath(OFFICE_CATALOG_FILE),
    specsPath: posixPath(OFFICE_SPECS_FILE),
    sidecar,
  };
  return mcpOfficeCtxCache;
}

function suiteMcpOfficeClient(ctx) {
  console.log('\n  ── mcp-office/client ──');

  const list = mcpClientCli(ctx, ['list']);
  eq(list.status, 0, 'mcp-office/client/list: exits 0');
  ok(list.json?.ok === true, 'mcp-office/client/list: ok=true');
  ok((list.json?.servers || []).some((s) => s.name === OFFICE_SERVER),
     'mcp-office/client/list: includes officecli server');
  ok((list.json?.servers || []).find((s) => s.name === OFFICE_SERVER)?.transport === 'stdio',
     'mcp-office/client/list: officecli transport is stdio');

  const tools = mcpClientCli(ctx, ['tools', '--server', OFFICE_SERVER]);
  eq(tools.status, 0, 'mcp-office/client/tools: exits 0');
  ok(tools.json?.tools_list_ok === true, 'mcp-office/client/tools: tools_list_ok');
  const tool = (tools.json?.tools || []).find((t) => t.name === OFFICE_TOOL);
  ok(!!tool, 'mcp-office/client/tools: exposes officecli tool');
  ok(String(tool?.description || '').includes('view'),
     'mcp-office/client/tools: description mentions view verb');

  const schema = mcpClientCli(ctx, ['schema', '--server', OFFICE_SERVER, '--tool', OFFICE_TOOL]);
  eq(schema.status, 0, 'mcp-office/client/schema: exits 0');
  ok(schema.json?.ok === true, 'mcp-office/client/schema: ok=true');
  const commandEnum = schema.json?.inputSchema?.properties?.command?.enum;
  ok(Array.isArray(commandEnum), 'mcp-office/client/schema: command enum present');
  for (const verb of OFFICE_VERBS) {
    ok(commandEnum.includes(verb), `mcp-office/client/schema: command enum includes "${verb}"`);
  }
  ok((schema.json?.inputSchema?.required || []).includes('command'),
     'mcp-office/client/schema: command is required');
}

function suiteMcpOfficeBlock(ctx) {
  console.log('\n  ── mcp-office/block ──');

  const viewArgs = { command: 'view', file: ctx.specsPath, mode: 'stats' };
  const rView = mcpCallBlock(ctx, OFFICE_TOOL, viewArgs, 'officeView');
  const viewEv = byKind(rView.events, 'mcpCall').find((e) => e.status === 'ok');
  const viewText = mcpResultText(viewEv?.data?.result)
    || mcpResultText(getVars(rView.events).find((e) => e.data?.target === 'out')?.data?.value);
  eq(rView.status, 0, 'mcp-office/block/view: exits 0');
  ok(!!viewEv, 'mcp-office/block/view: ok mcpCall event');
  ok(typeof viewText === 'string' && /Sheets:/i.test(viewText),
     'mcp-office/block/view: stats output mentions sheets');

  const queryArgs = { command: 'query', file: ctx.catalogPath, selector: 'sheet' };
  const rQuery = mcpCallBlock(ctx, OFFICE_TOOL, queryArgs, 'officeQuery');
  const queryEv = byKind(rQuery.events, 'mcpCall').find((e) => e.status === 'ok');
  const queryPayload = queryEv?.data?.result
    || getVars(rQuery.events).find((e) => e.data?.target === 'out')?.data?.value;
  eq(rQuery.status, 0, 'mcp-office/block/query: exits 0');
  ok(!!queryEv, 'mcp-office/block/query: ok mcpCall event');
  ok(officeQueryHasMatches(queryPayload),
     'mcp-office/block/query: product-catalog sheets returned');

  const cliCall = mcpCallCli(ctx, OFFICE_TOOL, {
    command: 'get',
    file: ctx.specsPath,
    path: '/',
    depth: 1,
  });
  eq(cliCall.status, 0, 'mcp-office/block/cli-get: exits 0');
  ok(cliCall.json?.ok === true, 'mcp-office/block/cli-get: ok=true');
  const getBody = mcpResultPayload(cliCall.json?.result);
  ok(getBody != null, 'mcp-office/block/cli-get: parsed result body');
}

function suiteMcpOffice(group = null) {
  console.log('\n── mcp-office ──');

  const runGroup = (name, fn) => {
    if (group && group !== name) return;
    fn();
  };

  const ctx = prepareMcpOfficeFixture();
  if (ctx.skip) {
    skipMcpLine(ctx.reason);
    return;
  }

  console.log(`  (config: packages/xblox/tests/xblox/mcp.json, fixture: tests/office, sidecar: ${ctx.sidecar})`);
  runGroup('client', () => suiteMcpOfficeClient(ctx));
  runGroup('block', () => suiteMcpOfficeBlock(ctx));
}

// ── Suite: MCP router auth (Tier 2 — tanit remote HTTP) ───────────────────────
//
// Uses the user's default roaming profile (no --config-dir). ZITADEL bearer and
// mcp.json must live in the same config dir as `tanit-cli login` / `status`.
// Router auth is injected in create_mcp_session via pm_zitadel_oauth_read_access_token.
// Gated on live ZITADEL login (`status --json` → tanit.logged_in).

const ROUTER_SERVER = 'tanit';

let mcpRouterCtxCache = null;

function tanitRouterCredentialsAvailable() {
  const status = runCli(['status', '--json'], { timeout: 30_000 });
  if (status.json?.tanit?.logged_in === true)
    return true;
  if (status.json?.tanit?.ok === true)
    return true;
  return false;
}

function roamingMcpConfigPath() {
  const list = runCli(['mcp', '--json', 'client', 'list'], { timeout: 30_000 });
  return list.json?.mcp_json_path || null;
}

function prepareMcpRouterFixture() {
  if (mcpRouterCtxCache) return mcpRouterCtxCache;

  const mcpPath = roamingMcpConfigPath();
  if (!mcpPath || !existsSync(mcpPath)) {
    mcpRouterCtxCache = {
      skip: true,
      reason: 'roaming mcp.json not found (run without --config-dir; check profile)',
    };
    return mcpRouterCtxCache;
  }

  const cfg = JSON.parse(readFileSync(mcpPath, 'utf8'));
  const routerServer = cfg?.mcpServers?.[ROUTER_SERVER];
  if (!routerServer?.url) {
    mcpRouterCtxCache = {
      skip: true,
      reason: `roaming mcp.json missing enabled ${ROUTER_SERVER} server`,
    };
    return mcpRouterCtxCache;
  }
  if (routerServer.enabled === false) {
    mcpRouterCtxCache = { skip: true, reason: `${ROUTER_SERVER} server disabled in roaming mcp.json` };
    return mcpRouterCtxCache;
  }
  if (routerServer.headers && Object.keys(routerServer.headers).length > 0) {
    mcpRouterCtxCache = {
      skip: true,
      reason: `${ROUTER_SERVER} still has literal headers in roaming mcp.json (remove x-litellm-api-key to test injected auth)`,
    };
    return mcpRouterCtxCache;
  }

  const forceLive = process.env.PM_MCP_ROUTER_LIVE === '1';
  if (!forceLive && !tanitRouterCredentialsAvailable()) {
    mcpRouterCtxCache = {
      skip: true,
      reason: 'no tanit router credentials (run `tanit-cli login`; or PM_MCP_ROUTER_LIVE=1)',
    };
    return mcpRouterCtxCache;
  }

  const configArgs = [];
  const cachePath = join(dirname(mcpPath), 'mcp-tools-cache.json');
  if (existsSync(cachePath))
    unlinkSync(cachePath);

  const toolsRes = runCli([
    'mcp', '--json', 'client', 'tools', '--probe', '--server', ROUTER_SERVER,
  ], { timeout: 180_000 });

  const tools = toolsRes.json?.tools;
  const toolName = Array.isArray(tools) && tools[0]?.name ? tools[0].name : null;
  if (toolsRes.status !== 0 || !toolsRes.json?.tools_list_ok || !toolName) {
    const errText = toolsRes.json?.error
      || toolsRes.json?.handshake_error
      || toolsRes.json?.skip_reason
      || toolsRes.json?.probe_note
      || toolsRes.stderr.trim()
      || 'tools/list failed';
    mcpRouterCtxCache = {
      skip: true,
      reason: `tanit router MCP unavailable (${errText})`,
    };
    return mcpRouterCtxCache;
  }

  mcpRouterCtxCache = {
    skip: false,
    serverName: ROUTER_SERVER,
    toolName,
    configArgs,
    mcpJsonPath: mcpPath,
    routerUrl: routerServer.url,
  };
  return mcpRouterCtxCache;
}

function suiteMcpRouterClient(ctx) {
  console.log('\n  ── mcp-router/client ──');

  const list = mcpClientCli(ctx, ['list']);
  eq(list.status, 0, 'mcp-router/client/list: exits 0');
  const router = (list.json?.servers || []).find((s) => s.name === ROUTER_SERVER);
  ok(!!router, 'mcp-router/client/list: includes tanit server');
  ok(router?.transport === 'http', 'mcp-router/client/list: tanit transport is http');

  const tools = mcpClientCli(ctx, ['tools', '--probe', '--server', ROUTER_SERVER]);
  eq(tools.status, 0, 'mcp-router/client/tools: exits 0');
  ok(tools.json?.tools_list_ok === true, 'mcp-router/client/tools: tools_list_ok');
  ok((tools.json?.tools || []).length >= 1, 'mcp-router/client/tools: remote catalog returned');

  const schema = mcpClientCli(ctx, ['schema', '--server', ROUTER_SERVER, '--tool', ctx.toolName]);
  eq(schema.status, 0, 'mcp-router/client/schema: exits 0');
  ok(schema.json?.ok === true, 'mcp-router/client/schema: ok=true');
  ok(schema.json?.inputSchema?.type === 'object',
     'mcp-router/client/schema: inputSchema is an object');
}

function suiteMcpRouterBlock(ctx) {
  console.log('\n  ── mcp-router/block ──');

  const rCall = mcpCallBlock(ctx, ctx.toolName, {}, 'routerResult');
  const okEv = byKind(rCall.events, 'mcpCall').find((e) => e.status === 'ok');
  eq(rCall.status, 0, 'mcp-router/block/call: exits 0');
  ok(!!okEv, 'mcp-router/block/call: ok mcpCall event');
  ok(okEv?.data?.result != null, 'mcp-router/block/call: result present');

  const cliCall = mcpCallCli(ctx, ctx.toolName, {});
  eq(cliCall.status, 0, 'mcp-router/block/cli-call: exits 0');
  ok(cliCall.json?.ok === true, 'mcp-router/block/cli-call: ok=true');
  ok(cliCall.json?.result != null, 'mcp-router/block/cli-call: result present');
}

function suiteMcpRouterAuthFailure() {
  console.log('\n  ── mcp-router/auth-failure ──');

  if (tanitRouterCredentialsAvailable()) {
    skipMcpLine('credentials present — skip unauthenticated router probe');
    return;
  }

  const mcpPath = roamingMcpConfigPath();
  if (!mcpPath || !existsSync(mcpPath)) {
    skipMcpLine('roaming mcp.json not found (run without --config-dir; check profile)');
    return;
  }

  const cfg = JSON.parse(readFileSync(mcpPath, 'utf8'));
  const routerServer = cfg?.mcpServers?.[ROUTER_SERVER];
  if (!routerServer?.url || routerServer.enabled === false) {
    skipMcpLine(`${ROUTER_SERVER} server disabled or missing in roaming mcp.json`);
    return;
  }

  const tools = runCli([
    'mcp', '--json', 'client', 'tools', '--server', ROUTER_SERVER,
  ], { timeout: 60_000 });
  ok(tools.status !== 0 || tools.json?.tools_list_ok !== true,
     'mcp-router/auth-failure: tools/list fails without credentials');
  const errText = tools.json?.error || tools.stderr || tools.json?.probe_note || '';
  ok(/auth|login|api_key|401|403|unauthorized/i.test(String(errText))
     || tools.json?.handshake_ok === false
     || tools.json?.ok === false,
     'mcp-router/auth-failure: surfaces auth/handshake error (not a crash)');
}

function suiteMcpRouter(group = null) {
  console.log('\n── mcp-router ──');

  const runGroup = (name, fn) => {
    if (group && group !== name) return;
    fn();
  };

  runGroup('auth-failure', suiteMcpRouterAuthFailure);

  const ctx = prepareMcpRouterFixture();
  if (ctx.skip) {
    if (!group || group === 'client' || group === 'block') {
      skipMcpLine(ctx.reason);
    }
    return;
  }

  console.log(`  (profile: ${ctx.mcpJsonPath}, router: ${ctx.routerUrl}, tool: ${ctx.toolName})`);
  runGroup('client', () => suiteMcpRouterClient(ctx));
  runGroup('block', () => suiteMcpRouterBlock(ctx));
}

// ── main ──────────────────────────────────────────────────────────────────────

const onlyIdx = process.argv.indexOf('--only');
const only = onlyIdx >= 0 ? process.argv[onlyIdx + 1] : null;

const suites = {
  raw:        suiteRaw,
  json:       suiteJson,
  downloadAs: suiteDownloadAs,
  iterable:   suiteIterable,
  auth:       suiteAuth,
  insecure:   suiteInsecure,
  retry:      suiteRetry,
  cache:      suiteCache,
  proxy:      suiteProxy,
  mqtt:       suiteMqtt,
  error:      suiteError,
  mcp:        () => suiteMcp(),
  'mcp-manifest': () => suiteMcp('manifest'),
  'mcp-client':   () => suiteMcp('client'),
  'mcp-block':    () => suiteMcp('block'),
  'mcp-ui':       () => suiteMcp('ui'),
  'mcp-xblox-local': () => suiteMcp('xblox-local'),
  'mcp-xblox-live':  () => suiteMcpXbloxLive(),
  'mcp-live':     () => suiteMcp('live'),
  'mcp-office':        () => suiteMcpOffice(),
  'mcp-office-client': () => suiteMcpOffice('client'),
  'mcp-office-block':  () => suiteMcpOffice('block'),
  'mcp-router':              () => suiteMcpRouter(),
  'mcp-router-client':       () => suiteMcpRouter('client'),
  'mcp-router-block':        () => suiteMcpRouter('block'),
  'mcp-router-auth-failure': () => suiteMcpRouter('auth-failure'),
};

console.log(`xblox-network test suite · exe=${EXE}`);
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
    suites[only]();
  } else {
    for (const fn of Object.values(suites)) fn();
  }
} finally {
  if (process.env.PM_KEEP_XBLOX_TEST_TMP !== '1') {
    try { rmSync(WORK, { recursive: true, force: true }); } catch { /* ignore */ }
  }
}

console.log(`\n── result ── ${passed} passed, ${failed} failed`);
if (failed > 0) {
  console.log('failures:');
  for (const f of failures) console.log(`  - ${f}`);
  process.exitCode = 1;
}
