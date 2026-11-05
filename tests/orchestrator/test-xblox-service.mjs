#!/usr/bin/env node
/**
 * test-xblox-service.mjs — XBlox service-block test suite.
 *
 * Covers the service blocks registered by service_blocks.cpp
 * (FEATURE_SERVICE_XBLOX=1).
 *
 * Two tiers of tests:
 *
 *   Offline (always run):
 *     manifest  — `xblox info` declares all expected service block kinds and
 *                 their required params + groups.
 *
 *   Live (requires SERVER_URL + valid auth from zitadel-oauth.json):
 *     images    — serviceImagesUpload: POST /api/images
 *     files     — serviceFilesUpload / serviceFilesList / serviceFilesRead /
 *                 serviceFilesRemove
 *     pages     — servicePagesCreate / servicePagesList / servicePagesGet /
 *                 servicePagesRemove
 *     posts     — servicePostsCreate / servicePostsList / servicePostsGet /
 *                 servicePostsRemove (uses test-service.mjs fixture images)
 *     search    — serviceSearch (GET /api/search) +
 *                 serviceFilesSearch (GET /api/vfs/search/{mount})
 *
 * Run:
 *   node tests/orchestrator/test-xblox-service.mjs
 *   node tests/orchestrator/test-xblox-service.mjs --only manifest
 *   node tests/orchestrator/test-xblox-service.mjs --only files
 *   node tests/orchestrator/test-xblox-service.mjs --skip-live
 *
 * Skip live tests:
 *   SKIP_SERVICE_TEST=1  or  --skip-live
 *
 * Fixtures:
 *   tests/assets/service/upload-a.png  (created by build-fixtures.mjs)
 *   tests/assets/service/upload-b.png
 */

import { spawnSync } from 'node:child_process';
import {
  existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');

// ── CLI exe location (mirrors test-xblox-network.mjs) ────────────────────────

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

// ── Fixture images (same as test-service.mjs) ─────────────────────────────────

const SERVICE_DIR = join(PACKAGE_ROOT, 'tests', 'assets', 'service');
const BUILD_FIXTURES = join(PACKAGE_ROOT, 'tests', 'assets', 'build-fixtures.mjs');

function ensureFixtures() {
  const a = join(SERVICE_DIR, 'upload-a.png');
  const b = join(SERVICE_DIR, 'upload-b.png');
  if (existsSync(a) && existsSync(b)) return true;
  console.log('[test-xblox-service] building fixture PNGs via build-fixtures.mjs …');
  const r = spawnSync(process.execPath, [BUILD_FIXTURES], {
    encoding: 'utf8', cwd: PACKAGE_ROOT, stdio: 'inherit',
  });
  return r.status === 0 && existsSync(a) && existsSync(b);
}

// ── Env / auth ────────────────────────────────────────────────────────────────

function loadEnv() {
  const envPath = join(PACKAGE_ROOT, '.env');
  if (!existsSync(envPath)) return;
  for (const raw of readFileSync(envPath, 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const k = line.slice(0, eq).trim();
    let v = line.slice(eq + 1).trim();
    const h = v.indexOf('#');
    if (h >= 0 && v[0] !== '"' && v[0] !== "'") v = v.slice(0, h).trimEnd();
    if ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'"))) v = v.slice(1, -1);
    if (!process.env[k]) process.env[k] = v;
  }
}

// ── Tiny test runner ──────────────────────────────────────────────────────────

let passed = 0;
let failed = 0;
const failures = [];

function ok(cond, label) {
  if (cond) { passed += 1; console.log(`  ✓ ${label}`); }
  else { failed += 1; failures.push(label); console.log(`  ✗ ${label}`); }
}
function eq(actual, expected, label) {
  ok(
    Object.is(actual, expected) || JSON.stringify(actual) === JSON.stringify(expected),
    `${label} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`,
  );
}

// ── CLI driver ────────────────────────────────────────────────────────────────

const EXE = cliExePath();
const WORK = mkdtempSync(join(tmpdir(), 'xblox-svc-'));

function run(doc, opts = {}) {
  const src = join(WORK, `flow-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(src, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
  const args = ['xblox', '--log-level', 'off', 'run'];
  if (!opts.noJson) args.push('--json');
  args.push('--no-wait', '--src', src);
  if (opts.extraArgs) args.push(...opts.extraArgs);
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 60_000,
    env: { ...process.env, ...(opts.env ?? {}) },
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

// Event helpers
const byKind = (events, kind) => events.filter((e) => e.kind === kind);

// ── Suite: manifest (offline) ─────────────────────────────────────────────────
//
// Verify that all service blocks are registered and their key params declared.

function suiteManifest() {
  console.log('\n── service/manifest ──');

  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'info', '--json'], {
    cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 15_000,
  });
  const info = parseTrailingJson(r.stdout || '');
  const blocks = info?.blocks ?? [];
  const byKindMap = Object.fromEntries(blocks.map((b) => [b.kind, b]));

  const expectedKinds = [
    'serviceImagesUpload',
    'serviceFilesUpload', 'serviceFilesList', 'serviceFilesRead', 'serviceFilesRemove',
    'servicePagesCreate', 'servicePagesList', 'servicePagesGet', 'servicePagesRemove',
    'servicePicturesList', 'servicePicturesGet', 'servicePicturesRemove',
    'servicePostsCreate', 'servicePostsList', 'servicePostsGet', 'servicePostsRemove',
    'serviceSearch', 'serviceFilesSearch',
  ];

  for (const kind of expectedKinds) {
    ok(kind in byKindMap, `manifest: ${kind} registered`);
  }

  // Auth params present on each block
  const authParams = ['serverUrl', 'auth', 'bearerToken'];
  for (const kind of expectedKinds) {
    const def = byKindMap[kind];
    if (!def) continue;
    const pnames = (def.params ?? []).map((p) => p.name);
    for (const ap of authParams) {
      ok(pnames.includes(ap), `manifest: ${kind} declares '${ap}' param`);
    }
  }

  // Group should be "Service"
  for (const kind of expectedKinds) {
    const def = byKindMap[kind];
    if (!def) continue;
    eq(def.group, 'Service', `manifest: ${kind} has group "Service"`);
  }

  // List / search blocks that expose a "Then" child list
  const childListKinds = [
    'serviceFilesList', 'servicePagesList', 'servicePicturesList', 'servicePostsList',
    'serviceSearch', 'serviceFilesSearch',
  ];
  for (const kind of childListKinds) {
    const def = byKindMap[kind];
    if (!def) continue;
    const childList = def.childLists ?? def.children ?? [];
    ok(Array.isArray(childList) && childList.some((cl) => cl.key === 'items'),
       `manifest: ${kind} declares 'items' child list`);
  }

  // serviceSearch declares required search params
  const srchDef = byKindMap['serviceSearch'];
  if (srchDef) {
    const pnames = (srchDef.params ?? []).map((p) => p.name);
    for (const pn of ['q', 'type', 'limit', 'visibilityFilter']) {
      ok(pnames.includes(pn), `manifest: serviceSearch declares '${pn}' param`);
    }
  }

  // serviceFilesSearch declares required search params
  const fsDef = byKindMap['serviceFilesSearch'];
  if (fsDef) {
    const pnames = (fsDef.params ?? []).map((p) => p.name);
    for (const pn of ['q', 'mount', 'path', 'type', 'maxResults', 'fts']) {
      ok(pnames.includes(pn), `manifest: serviceFilesSearch declares '${pn}' param`);
    }
  }
}

// ── Suite: images (live) ──────────────────────────────────────────────────────

function suiteImages(serverUrl) {
  console.log('\n── service/images (live) ──');

  if (!ensureFixtures()) {
    ok(false, 'images: fixture PNGs present');
    return;
  }
  const fileA = join(SERVICE_DIR, 'upload-a.png');
  const fileB = join(SERVICE_DIR, 'upload-b.png');

  const r = run({
    version: 1,
    roots: [{
      kind: 'serviceImagesUpload',
      serverUrl,
      auth: 'auto',
      files: [fileA, fileB],
      storeAs: 'results',
    }],
  }, { timeout: 120_000 });

  eq(r.status, 0, 'images: exits 0');
  const ev = byKind(r.events, 'serviceImagesUpload').find((e) => e.status === 'ok');
  ok(!!ev, 'images: ok event emitted');
  ok(Array.isArray(ev?.data?.result) && ev.data.result.length === 2,
     'images: result has 2 entries');
  ok(ev?.data?.result?.every((e) => e.ok), 'images: all uploads ok');
  ok(ev?.data?.result?.every((e) => typeof e.data?.url === 'string'),
     'images: each result has url');
}

// ── Suite: files (live) ───────────────────────────────────────────────────────

function suiteFiles(serverUrl) {
  console.log('\n── service/files (live) ──');

  const tmpFile = join(WORK, `xblox-svc-test-${Date.now()}.txt`);
  writeFileSync(tmpFile, 'xblox-service-test-content', 'utf8');
  const remotePath = `xblox-tests/test-${Date.now()}.txt`;

  // Upload
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesUpload',
      serverUrl,
      auth: 'auto',
      file: tmpFile,
      mount: 'home',
      remotePath,
      storeAs: 'uploaded',
    }],
  }, { timeout: 60_000 });

  eq(r1.status, 0, 'files/upload: exits 0');
  const upEv = byKind(r1.events, 'serviceFilesUpload').find((e) => e.status === 'ok');
  ok(!!upEv, 'files/upload: ok event emitted');

  // List directory
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesList',
      serverUrl,
      auth: 'auto',
      mount: 'home',
      path: 'xblox-tests',
      storeAs: 'ls',
    }],
  }, { timeout: 30_000 });

  eq(r2.status, 0, 'files/list: exits 0');
  const lsEv = byKind(r2.events, 'serviceFilesList').find((e) => e.status === 'ok');
  ok(!!lsEv, 'files/list: ok event emitted');

  // Remove the uploaded file
  const r3 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesRemove',
      serverUrl,
      auth: 'auto',
      mount: 'home',
      path: remotePath,
      storeAs: 'removed',
    }],
  }, { timeout: 30_000 });

  eq(r3.status, 0, 'files/remove: exits 0');
  const rmEv = byKind(r3.events, 'serviceFilesRemove').find((e) => e.status === 'ok');
  ok(!!rmEv, 'files/remove: ok event emitted');
}

// ── Suite: pages (live) ───────────────────────────────────────────────────────

function suitePages(serverUrl) {
  console.log('\n── service/pages (live) ──');

  const slug = `xblox-test-${Date.now()}`;
  const title = 'XBlox Service Test Page';

  // Create
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'servicePagesCreate',
      serverUrl,
      auth: 'auto',
      slug,
      title,
      content: '# XBlox Test\n\nCreated by test-xblox-service.mjs.',
      tags: 'xblox,test',
      visibility: 'private',
      storeAs: 'page',
    }],
  }, { timeout: 60_000 });

  eq(r1.status, 0, 'pages/create: exits 0');
  const createEv = byKind(r1.events, 'servicePagesCreate').find((e) => e.status === 'ok');
  ok(!!createEv, 'pages/create: ok event emitted');

  const pageData = createEv?.data?.result;
  const pageId = pageData?.id ?? pageData?.page_id ?? pageData?.slug ?? slug;
  ok(typeof pageId === 'string' && pageId.length > 0, 'pages/create: result has page id/slug');

  // List
  const r2 = run({
    version: 1,
    roots: [{ kind: 'servicePagesList', serverUrl, auth: 'auto', limit: 5, storeAs: 'pages' }],
  }, { timeout: 30_000 });
  eq(r2.status, 0, 'pages/list: exits 0');
  ok(byKind(r2.events, 'servicePagesList').some((e) => e.status === 'ok'), 'pages/list: ok event');

  // Get by slug
  const r3 = run({
    version: 1,
    roots: [{ kind: 'servicePagesGet', serverUrl, auth: 'auto', pageId: slug, storeAs: 'page' }],
  }, { timeout: 30_000 });
  eq(r3.status, 0, 'pages/get: exits 0');
  ok(byKind(r3.events, 'servicePagesGet').some((e) => e.status === 'ok'), 'pages/get: ok event');

  // Remove
  const r4 = run({
    version: 1,
    roots: [{ kind: 'servicePagesRemove', serverUrl, auth: 'auto', pageId: slug, storeAs: 'removed' }],
  }, { timeout: 30_000 });
  eq(r4.status, 0, 'pages/remove: exits 0');
  ok(byKind(r4.events, 'servicePagesRemove').some((e) => e.status === 'ok'), 'pages/remove: ok event');
}

// ── Suite: posts (live) ───────────────────────────────────────────────────────

function suitePosts(serverUrl) {
  console.log('\n── service/posts (live) ──');

  if (!ensureFixtures()) {
    ok(false, 'posts: fixture PNGs present');
    return;
  }
  const fileA = join(SERVICE_DIR, 'upload-a.png');

  // Create post with image
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'servicePostsCreate',
      serverUrl,
      auth: 'auto',
      title: 'XBlox Service Test Post',
      description: 'Created by test-xblox-service.mjs.',
      visibility: 'private',
      file: fileA,
      storeAs: 'post',
    }],
  }, { timeout: 120_000 });

  eq(r1.status, 0, 'posts/create: exits 0');
  const createEv = byKind(r1.events, 'servicePostsCreate').find((e) => e.status === 'ok');
  ok(!!createEv, 'posts/create: ok event emitted');
  const postId = createEv?.data?.result?.postId ?? createEv?.data?.postId;
  ok(typeof postId === 'string' && postId.length > 0, 'posts/create: result has postId');

  // List
  const r2 = run({
    version: 1,
    roots: [{ kind: 'servicePostsList', serverUrl, auth: 'auto', limit: 5, storeAs: 'posts' }],
  }, { timeout: 30_000 });
  eq(r2.status, 0, 'posts/list: exits 0');
  ok(byKind(r2.events, 'servicePostsList').some((e) => e.status === 'ok'), 'posts/list: ok event');

  if (!postId) return;

  // Get
  const r3 = run({
    version: 1,
    roots: [{ kind: 'servicePostsGet', serverUrl, auth: 'auto', postId, storeAs: 'post' }],
  }, { timeout: 30_000 });
  eq(r3.status, 0, 'posts/get: exits 0');
  ok(byKind(r3.events, 'servicePostsGet').some((e) => e.status === 'ok'), 'posts/get: ok event');

  // Remove
  const r4 = run({
    version: 1,
    roots: [{ kind: 'servicePostsRemove', serverUrl, auth: 'auto', postId, storeAs: 'removed' }],
  }, { timeout: 30_000 });
  eq(r4.status, 0, 'posts/remove: exits 0');
  ok(byKind(r4.events, 'servicePostsRemove').some((e) => e.status === 'ok'), 'posts/remove: ok event');
}

// ── Suite: search (live) ──────────────────────────────────────────────────────
//
// Tests serviceSearch (unified) and serviceFilesSearch (VFS).
//
// serviceSearch: no specific content is required — we only verify the response
// shape and that the block exits cleanly.  We do a broad "all" search with a
// short, common query term so at least one result is likely, but we don't
// assert a non-zero count because fresh instances may have empty indexes.
//
// serviceFilesSearch: queries the "home" mount for an empty string — the
// server will reject it (min 2 chars) so we verify the error path — then
// queries for a safe 2-char string to exercise the happy path.

function suiteSearch(serverUrl) {
  console.log('\n── service/search (live) ──');

  // ── serviceSearch: all types, broad term ──────────────────────────────────
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'serviceSearch',
      serverUrl,
      auth: 'auto',
      q: 'test',
      type: 'all',
      limit: 10,
      storeAs: 'results',
    }],
  }, { timeout: 30_000 });

  eq(r1.status, 0, 'search/all: exits 0');
  const srchEv = byKind(r1.events, 'serviceSearch').find((e) => e.status === 'ok');
  ok(!!srchEv, 'search/all: ok event emitted');
  // result is a JSON array (may be empty on a fresh instance)
  ok(Array.isArray(srchEv?.data?.result) || typeof srchEv?.data?.result === 'string',
     'search/all: result is array or json string');

  // ── serviceSearch: type filter — files only ────────────────────────────────
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'serviceSearch',
      serverUrl,
      auth: 'auto',
      q: 'test',
      type: 'files',
      limit: 5,
      storeAs: 'fileResults',
    }],
  }, { timeout: 30_000 });

  eq(r2.status, 0, 'search/files-type: exits 0');
  ok(byKind(r2.events, 'serviceSearch').some((e) => e.status === 'ok'),
     'search/files-type: ok event emitted');

  // ── serviceSearch: empty q → error event ──────────────────────────────────
  const r3 = run({
    version: 1,
    roots: [{
      kind: 'serviceSearch',
      serverUrl,
      auth: 'auto',
      q: '',
      storeAs: 'r',
      continueOnError: true,
    }],
  });
  ok(byKind(r3.events, 'serviceSearch').some((e) => e.status === 'error'),
     'search/empty-q: empty q → error event');

  // ── serviceFilesSearch: happy path (home mount, 2-char query) ─────────────
  //
  // The server rejects queries <2 chars and queries containing / or ..; we
  // use a safe 2-char term. Not all mounts are searchable, so we only verify
  // the event shape, not that results > 0 (the index may be empty).
  const r4 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesSearch',
      serverUrl,
      auth: 'auto',
      q: 'te',
      mount: 'home',
      type: 'all',
      maxResults: 50,
      fts: 0,
      storeAs: 'searchResult',
    }],
  }, { timeout: 30_000 });

  // Server may return 404 if the mount doesn't exist, or 200 with results.
  // Either way the block should exit 0 on 200 or report an error event.
  const fsEv4ok  = byKind(r4.events, 'serviceFilesSearch').find((e) => e.status === 'ok');
  const fsEv4err = byKind(r4.events, 'serviceFilesSearch').find((e) => e.status === 'error');
  ok(!!(fsEv4ok || fsEv4err), 'search/vfs: ok or error event emitted');
  if (fsEv4ok) {
    const res = fsEv4ok.data?.result;
    ok(res && typeof res === 'object' && !Array.isArray(res),
       'search/vfs: result is an object {results,total,truncated}');
  }

  // ── serviceFilesSearch: FTS flag ───────────────────────────────────────────
  //
  // Fresh uploads may not appear in the FTS index; we only verify the block
  // runs and emits an event.  We use a known-existing filename fragment "pm"
  // (safe, ≥2 chars).
  const r5 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesSearch',
      serverUrl,
      auth: 'auto',
      q: 'pm',
      mount: 'home',
      maxResults: 20,
      fts: 1,
      storeAs: 'ftsResult',
    }],
  }, { timeout: 30_000 });

  const fsEv5 = byKind(r5.events, 'serviceFilesSearch');
  ok(fsEv5.some((e) => e.status === 'ok' || e.status === 'error'),
     'search/vfs-fts: event emitted (ok or error)');
  const okFts = fsEv5.find((e) => e.status === 'ok');
  if (okFts) {
    console.log(`    ↳ FTS returned total=${okFts.data?.result?.total ?? '?'} ` +
                `(index may lag fresh uploads)`);
  }
}

// ── Suite: auth error (offline) ────────────────────────────────────────────────
//
// Block with explicit bad credentials → error event (no network needed if server is unreachable,
// but we still exercise the block mechanics).

function suiteAuthError() {
  console.log('\n── service/auth-error (offline mechanics) ──');

  // Missing serverUrl → error before any network call
  const r1 = run({
    version: 1,
    roots: [{
      kind: 'serviceImagesUpload',
      serverUrl: '',
      auth: 'bearer',
      bearerToken: 'fake',
      file: join(WORK, 'nonexistent.png'),
      storeAs: 'r',
      continueOnError: true,
    }],
  });
  ok(byKind(r1.events, 'serviceImagesUpload').some((e) => e.status === 'error'),
     'auth-error: empty serverUrl → error event');

  // auth:bearer + empty bearerToken → error
  const r2 = run({
    version: 1,
    roots: [{
      kind: 'servicePagesList',
      serverUrl: 'https://127.0.0.1:19999',
      auth: 'bearer',
      bearerToken: '',
      storeAs: 'r',
      continueOnError: true,
    }],
  });
  ok(byKind(r2.events, 'servicePagesList').some((e) => e.status === 'error'),
     'auth-error: auth:bearer + empty bearerToken → error event');

  // Required param missing: serviceFilesUpload with no file → error
  const r3 = run({
    version: 1,
    roots: [{
      kind: 'serviceFilesUpload',
      serverUrl: 'https://example.com',
      auth: 'bearer',
      bearerToken: 'fake-token',
      file: '',
      storeAs: 'r',
      continueOnError: true,
    }],
  });
  ok(byKind(r3.events, 'serviceFilesUpload').some((e) => e.status === 'error'),
     'auth-error: serviceFilesUpload with empty file → error event');
}

// ── main ──────────────────────────────────────────────────────────────────────

loadEnv();

const skipLive = process.argv.includes('--skip-live') || process.env.SKIP_SERVICE_TEST === '1';
const onlyIdx  = process.argv.indexOf('--only');
const only     = onlyIdx >= 0 ? process.argv[onlyIdx + 1] : null;

const serverUrl = (process.env.SERVER_URL || '').trim();
const hasLive   = !skipLive && serverUrl.length > 0;

const allSuites = {
  manifest:  suiteManifest,
  authError: suiteAuthError,
  ...(hasLive ? {
    images: () => suiteImages(serverUrl),
    files:  () => suiteFiles(serverUrl),
    pages:  () => suitePages(serverUrl),
    posts:  () => suitePosts(serverUrl),
    search: () => suiteSearch(serverUrl),
  } : {}),
};

console.log(`xblox-service test suite · exe=${EXE}`);
console.log(`  live tests: ${hasLive ? `enabled (SERVER_URL=${serverUrl})` : 'skipped (no SERVER_URL or --skip-live)'}`);

if (!existsSync(EXE)) {
  console.error(`\nCLI not found at ${EXE} — run "npm run build:cpp" first.`);
  process.exit(2);
}

try {
  if (only) {
    if (!allSuites[only]) {
      const choices = Object.keys(allSuites).join(', ');
      console.error(`unknown suite "${only}". choices: ${choices}`);
      process.exit(2);
    }
    allSuites[only]();
  } else {
    for (const fn of Object.values(allSuites)) fn();
  }
} finally {
  if (process.env.PM_KEEP_XBLOX_TEST_TMP !== '1') {
    try { rmSync(WORK, { recursive: true, force: true }); } catch { /* ignore */ }
  }
}

console.log(`\n── result ── ${passed} passed, ${failed} failed`);
if (!hasLive) {
  console.log('  (live suites skipped — set SERVER_URL to enable)');
}
if (failed > 0) {
  console.log('failures:');
  for (const f of failures) console.log(`  - ${f}`);
  process.exitCode = 1;
}
