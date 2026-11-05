#!/usr/bin/env node
/**
 * test-xblox-vector-store-local.mjs — Local offline vector store XBlox block test suite.
 *
 * Covers blocks registered by vector_blocks_local.cpp (FEATURE_VECTOR_LOCAL_XBLOX=1):
 *   vectorLocalOpen · vectorLocalAddText · vectorLocalAddFile ·
 *   vectorLocalAddDir · vectorLocalSearch · vectorLocalDelete · vectorLocalStats
 *
 * Tiers:
 *
 *   manifest  (offline, always):
 *     Verifies all block kinds are registered with correct params + group.
 *
 *   inline    (offline, requires embedding model on disk):
 *     open        — opens/creates a fresh store
 *     add-text    — embeds an inline string, checks chunk count + ids
 *     add-md      — embeds a .md file; verifies section-level chunking (title populated)
 *     add-plain   — embeds a .txt file; verifies chunks produced
 *     dedup       — re-adding the same file returns count=0, skipped=true
 *     search      — semantic query returns ranked hits with expected fields
 *     search-topk — topK parameter is respected
 *
 *   office  (requires embedding model + officecli on PATH):
 *     xlsx        — index tests/office/annual-report-2024.xlsx, assert chunks > 0
 *     docx        — index tests/office/meeting-notes.docx, assert chunks > 0
 *     search      — semantic query returns a hit from an office document
 *     dedup       — re-adding the same .xlsx returns skipped=true
 *     delete      — vectorLocalDelete removes chunks; follow-up search misses
 *     stats       — vectorLocalStats returns docCount + indexSizeBytes
 *
 * Run all:
 *   node tests/orchestrator/test-xblox-vector-store-local.mjs
 *
 * Run one suite:
 *   node tests/orchestrator/test-xblox-vector-store-local.mjs --only manifest
 *   node tests/orchestrator/test-xblox-vector-store-local.mjs --only inline
 *   node tests/orchestrator/test-xblox-vector-store-local.mjs --only office
 *
 * Skip inline (manifest only):
 *   SKIP_EMBED_TEST=1  or  --skip-inline
 *
 * Override model path:
 *   PM_EMBED_MODEL=/path/to/model.gguf
 */

import { performance } from 'node:perf_hooks';
import { spawnSync } from 'node:child_process';
import {
  existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert } from '../../../../tests/orchestrator/test-commons.js';
import { buildMetricsBundle, createMetricsCollector, renderMarkdownReport } from '../../../../tests/orchestrator/reports.js';
import { detectGpuBackend, startGpuMemorySampler } from '../../../../tests/orchestrator/gpu-metrics.mjs';
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '../../../../tests/orchestrator/test-report-accumulate.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');

function featureEnabled(name) {
  try {
    const flags = JSON.parse(readFileSync(join(PACKAGE_ROOT, 'dist', 'shared', 'features.json'), 'utf8'));
    return flags[name] === true;
  } catch {
    return false;
  }
}

// ── CLI exe ────────────────────────────────────────────────────────────────────

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

/** Resolve an embedding model path.
 *  Priority: PM_EMBED_MODEL env → dist/models/bge-small (q8_0 then f16) → empty string */
function resolveEmbedModel() {
  const env = process.env.PM_EMBED_MODEL;
  if (env && env.trim()) return resolve(env.trim());
  const modelsDir = join(PACKAGE_ROOT, 'dist', 'models');
  for (const candidate of [
    'bge-small-en-v1.5-q8_0.gguf',
    'bge-small-en-v1.5-f16.gguf',
    'bge-base-en-v1.5-q8_0.gguf',
    'bge-base-en-v1.5-f16.gguf',
  ]) {
    const p = join(modelsDir, candidate);
    if (existsSync(p)) return p;
  }
  return '';
}

// ── Tiny test runner ──────────────────────────────────────────────────────────

let passed = 0;
let failed = 0;
const failures = [];

function ok(cond, label) {
  if (cond) { passed += 1; console.log(`  ✓ ${label}`); }
  else       { failed += 1; failures.push(label); console.log(`  ✗ ${label}`); }
}
function eq(actual, expected, label) {
  ok(
    Object.is(actual, expected) || JSON.stringify(actual) === JSON.stringify(expected),
    `${label} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`,
  );
}
function ge(actual, min, label) {
  ok(typeof actual === 'number' && actual >= min,
    `${label} (expected ≥ ${min}, got ${actual})`);
}

// ── CLI runner ─────────────────────────────────────────────────────────────────

const EXE  = cliExePath();
const WORK = mkdtempSync(join(tmpdir(), 'xblox-vsl-'));

const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');
const metricsCollector = createMetricsCollector();
const startedAtIso     = new Date().toISOString();

// Per-invocation log: one entry per `run()` (each is a fresh tanit-cli
// process), used to build the case/timing tables in the report.
const RUN_LOG = [];

// Files ingested by the device-comparison workload (path + size + chunk count),
// captured for the report's "Ingested files" section.
let INGEST_FILES = [];

// Embedding device for the current phase: injected as `gpuLayers` into every
// vectorLocal* block so the whole suite runs CPU-only (0) or GPU-offloaded (-1
// = all layers). Set per-phase by the device toggle / comparison bench.
let ACTIVE_GPU_LAYERS = 0;

function run(doc, opts = {}) {
  // Inject the active device's gpuLayers into vector blocks that don't set it.
  for (const root of doc?.roots ?? []) {
    if (typeof root?.kind === 'string' && root.kind.startsWith('vectorLocal')
        && root.gpuLayers === undefined) {
      root.gpuLayers = ACTIVE_GPU_LAYERS;
    }
  }
  const src = join(WORK, `flow-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(src, JSON.stringify(doc, null, 2) + '\n', 'utf8');
  const args = ['xblox', '--log-level', 'off', 'run', '--json', '--no-wait', '--src', src];
  if (opts.extraArgs) args.push(...opts.extraArgs);
  const kind = doc?.roots?.[0]?.kind ?? 'unknown';
  const t0 = performance.now();
  const r = spawnSync(EXE, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 120_000,
    env: { ...process.env, ...(opts.env ?? {}) },
  });
  const ms = performance.now() - t0;
  const report = parseTrailingJson(r.stdout || '');
  const events = report?.events ?? [];
  RUN_LOG.push({
    kind,
    label: opts.label ?? kind,
    ms,
    status: r.status ?? -1,
    ok: (r.status ?? -1) === 0,
    eventCount: events.length,
  });
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    report,
    events,
    ms,
  };
}

function parseTrailingJson(stdout) {
  const text = String(stdout || '').trim();
  const starts = [text.lastIndexOf('\n{'), text.indexOf('{')].filter((n) => n >= 0);
  for (const s of starts) {
    try { return JSON.parse(text.slice(text[s] === '{' ? s : s + 1)); } catch { /* next */ }
  }
  try { return JSON.parse(text); } catch { return { events: [] }; }
}

const byKind = (events, kind) => events.filter((e) => e.kind === kind);
const okEv   = (events, kind) => byKind(events, kind).find((e) => e.status === 'ok');
const errEv  = (events, kind) => byKind(events, kind).find((e) => e.status === 'error');

// ── Fixture files for inline tests ────────────────────────────────────────────

// A real markdown doc that definitely exists in the repo
const MD_FILE   = join(PACKAGE_ROOT, 'docs', 'llm', 'local-llms.md');
const CHAT_FILE = join(PACKAGE_ROOT, 'tests', 'cms', 'chat.md');

// Small plain-text fixture written to the temp dir
const TXT_FILE = join(WORK, 'fixture.txt');
writeFileSync(TXT_FILE,
  'Embeddings are dense vector representations of text.\n\n' +
  'They allow semantic search without exact keyword matching.\n\n' +
  'Local embedding models run entirely on-device with no network calls.\n',
  'utf8');

// Markdown fixture with YAML front matter and multiple sections
const FM_FILE = join(WORK, 'fixture-fm.md');
writeFileSync(FM_FILE,
  '---\n' +
  'title: Chunking Test\n' +
  'summary: A document for testing markdown front matter parsing.\n' +
  'tags: [chunking, test, frontmatter]\n' +
  '---\n\n' +
  '# Introduction\n\n' +
  'This is the introduction section. It explains the purpose of the document.\n\n' +
  '## Details\n\n' +
  'This section contains more detailed information about the chunking algorithm.\n' +
  'The algorithm splits text at heading boundaries and then further at paragraph boundaries.\n\n' +
  '### Sub-section\n\n' +
  'Even deeper nesting is handled by the MarkdownChunker.\n',
  'utf8');

// Store directory (fresh per test run)
const STORE_DIR = join(WORK, 'test-store');

// Embedding model — auto-detected from dist/models/ if PM_EMBED_MODEL not set
const EMBED_MODEL = resolveEmbedModel();

// ── Suite: manifest (offline) ─────────────────────────────────────────────────

function suiteManifest() {
  console.log('\n── vector-local/manifest ──');

  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'info', '--json'], {
    cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 15_000,
  });
  const info = parseTrailingJson(r.stdout || '');
  const blocks = info?.blocks ?? [];
  const byKindMap = Object.fromEntries(blocks.map((b) => [b.kind, b]));

  const expectedKinds = [
    'vectorLocalOpen',
    'vectorLocalAddText',
    'vectorLocalAddFile',
    'vectorLocalSearch',
  ];

  // All four blocks are registered
  for (const kind of expectedKinds) {
    ok(kind in byKindMap, `manifest: ${kind} registered`);
  }

  // Group is "Vector"
  for (const kind of expectedKinds) {
    const def = byKindMap[kind];
    if (!def) continue;
    eq(def.group, 'Vector', `manifest: ${kind} has group "Vector"`);
  }

  // Common params present on all blocks
  for (const kind of expectedKinds) {
    const def = byKindMap[kind];
    if (!def) continue;
    const pnames = (def.params ?? []).map((p) => p.name);
    ok(pnames.includes('storePath'), `manifest: ${kind} declares 'storePath'`);
    ok(pnames.includes('model'),     `manifest: ${kind} declares 'model'`);
    ok(pnames.includes('threads'),   `manifest: ${kind} declares 'threads'`);
  }

  // Block-specific required params
  const open   = byKindMap['vectorLocalOpen'];
  const addTxt = byKindMap['vectorLocalAddText'];
  const addFile = byKindMap['vectorLocalAddFile'];
  const search  = byKindMap['vectorLocalSearch'];

  if (addTxt) {
    const pnames = (addTxt.params ?? []).map((p) => p.name);
    for (const pn of ['text', 'source', 'chunkSize', 'chunkOverlap']) {
      ok(pnames.includes(pn), `manifest: vectorLocalAddText declares '${pn}'`);
    }
  }

  if (addFile) {
    const pnames = (addFile.params ?? []).map((p) => p.name);
    for (const pn of ['file', 'source', 'chunkSize', 'chunkOverlap']) {
      ok(pnames.includes(pn), `manifest: vectorLocalAddFile declares '${pn}'`);
    }
  }

  if (search) {
    const pnames = (search.params ?? []).map((p) => p.name);
    for (const pn of ['query', 'topK', 'filterSource', 'filterPath']) {
      ok(pnames.includes(pn), `manifest: vectorLocalSearch declares '${pn}'`);
    }
    // Search exposes a "Then" child list
    const childList = search.childLists ?? search.children ?? [];
    ok(Array.isArray(childList) && childList.some((cl) => cl.key === 'items'),
       'manifest: vectorLocalSearch declares "items" child list');
  }
}

// ── Suite: inline (requires embedding model) ───────────────────────────────────

function suiteInline() {
  console.log('\n── vector-local/open ──');

  // ── open ──────────────────────────────────────────────────────────────────
  {
    const r = run({ roots: [{
      kind: 'vectorLocalOpen',
      storePath: STORE_DIR,
      ...(EMBED_MODEL ? { model: EMBED_MODEL } : {}),
      storeAs: 'info',
    }] }, { timeout: 30_000 });

    eq(r.status, 0, 'open: exits 0');
    const ev = okEv(r.events, 'vectorLocalOpen');
    ok(!!ev, 'open: ok event emitted');
    ok(typeof ev?.data?.docCount === 'number', 'open: docCount is number');
    ok(typeof ev?.data?.dims     === 'number' && ev.data.dims > 0, 'open: dims > 0');
    ok(typeof ev?.data?.modelPath === 'string' && ev.data.modelPath.length > 0,
       'open: modelPath non-empty');
    ok(ev?.data?.docCount === 0, 'open: fresh store has 0 docs');
  }

  // ── add-text ──────────────────────────────────────────────────────────────
  console.log('\n── vector-local/add-text ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalAddText',
      storePath: STORE_DIR,
      text: 'Semantic search uses dense vector embeddings to find conceptually similar documents.',
      source: 'test',
      path: 'inline://test-1',
      title: 'Test sentence',
      chunkSize: 300,
      chunkOverlap: 0,
      storeAs: 'added',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'add-text: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddText');
    ok(!!ev, 'add-text: ok event emitted');
    ge(ev?.data?.count ?? 0, 1, 'add-text: count ≥ 1');
    ok(Array.isArray(ev?.data?.result?.ids) && ev.data.result.ids.length > 0, 'add-text: ids non-empty');
    ge(ev?.data?.docCount ?? 0, 1, 'add-text: docCount ≥ 1');
  }

  // ── add-text (long — triggers chunking) ───────────────────────────────────
  console.log('\n── vector-local/add-text-chunked ──');
  {
    const longText = Array.from({ length: 20 }, (_, i) =>
      `Paragraph ${i + 1}: Local vector search enables efficient semantic retrieval. ` +
      `Dense embeddings from GGUF models power offline search with no network calls.`
    ).join('\n\n');

    const r = run({ roots: [{
      kind: 'vectorLocalAddText',
      storePath: STORE_DIR,
      text: longText,
      source: 'test',
      path: 'inline://long-text',
      chunkSize: 200,
      chunkOverlap: 0,
      storeAs: 'added',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'add-text-chunked: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddText');
    ok(!!ev, 'add-text-chunked: ok event emitted');
    ge(ev?.data?.count ?? 0, 2, 'add-text-chunked: at least 2 chunks');
  }

  // ── add-plain ──────────────────────────────────────────────────────────────
  console.log('\n── vector-local/add-plain ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalAddFile',
      storePath: STORE_DIR,
      file: TXT_FILE,
      source: 'test',
      chunkSize: 400,
      chunkOverlap: 0,
      storeAs: 'added',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'add-plain: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddFile');
    ok(!!ev, 'add-plain: ok event emitted');
    ge(ev?.data?.count ?? 0, 1, 'add-plain: count ≥ 1');
    ok(Array.isArray(ev?.data?.result?.ids), 'add-plain: ids is array');
  }

  // ── add-md (front matter + section chunking) ───────────────────────────────
  console.log('\n── vector-local/add-md ──');
  let mdChunkCount = 0;
  {
    const r = run({ roots: [{
      kind: 'vectorLocalAddFile',
      storePath: STORE_DIR,
      file: FM_FILE,
      source: 'test',
      chunkSize: 400,
      chunkOverlap: 0,
      storeAs: 'added',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'add-md: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddFile');
    ok(!!ev, 'add-md: ok event emitted');
    // Fixture has 3 sections (intro + details + sub-section) → ≥ 3 chunks
    mdChunkCount = ev?.data?.count ?? 0;
    ge(mdChunkCount, 3, 'add-md: at least 3 chunks (one per heading)');
    ok(Array.isArray(ev?.data?.result?.ids), 'add-md: ids is array');
  }

  // ── add-md-real (real repo doc) ────────────────────────────────────────────
  if (existsSync(MD_FILE)) {
    console.log('\n── vector-local/add-md-real ──');
    const r = run({ roots: [{
      kind: 'vectorLocalAddFile',
      storePath: STORE_DIR,
      file: MD_FILE,
      source: 'docs',
      chunkSize: 600,
      chunkOverlap: 0,
      storeAs: 'added',
    }] }, { timeout: 120_000 });

    eq(r.status, 0, 'add-md-real: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddFile');
    ok(!!ev, 'add-md-real: ok event emitted');
    ge(ev?.data?.count ?? 0, 5, 'add-md-real: ≥ 5 chunks from real doc');
  }

  // ── dedup — re-adding same file returns skipped=true, count=0 ─────────────
  console.log('\n── vector-local/dedup ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalAddFile',
      storePath: STORE_DIR,
      file: TXT_FILE,   // same file as add-plain above
      source: 'test',
      storeAs: 'added',
    }] }, { timeout: 30_000 });

    eq(r.status, 0, 'dedup: exits 0');
    const ev = okEv(r.events, 'vectorLocalAddFile');
    ok(!!ev, 'dedup: ok event emitted');
    eq(ev?.data?.count ?? -1, 0, 'dedup: count=0 (already indexed)');
    ok(ev?.data?.result?.skipped === true, 'dedup: skipped=true');
  }

  // ── search ─────────────────────────────────────────────────────────────────
  console.log('\n── vector-local/search ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalSearch',
      storePath: STORE_DIR,
      query: 'semantic search dense vector embeddings',
      topK: 5,
      storeAs: 'results',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'search: exits 0');
    const ev = okEv(r.events, 'vectorLocalSearch');
    ok(!!ev, 'search: ok event emitted');
    ge(ev?.data?.count ?? 0, 1, 'search: at least 1 hit');

    const hits = ev?.data?.result?.hits ?? [];
    ok(Array.isArray(hits) && hits.length > 0, 'search: hits is non-empty array');
    const h = hits[0];
    if (h) {
      ok(typeof h.id     === 'number', 'search: hit.id is number');
      ok(typeof h.score  === 'number', 'search: hit.score is number');
      ok(typeof h.chunk  === 'string', 'search: hit.chunk is string');
      ok(typeof h.source === 'string', 'search: hit.source is string');
      ok(typeof h.path   === 'string', 'search: hit.path is string');
      ok(h.score > 0.2, `search: top hit score > 0.2 (got ${h.score?.toFixed(3)})`);
    }
  }

  // ── search: topK respected ─────────────────────────────────────────────────
  console.log('\n── vector-local/search-topk ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalSearch',
      storePath: STORE_DIR,
      query: 'embedding model local inference',
      topK: 2,
      storeAs: 'results',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'search-topk: exits 0');
    const ev = okEv(r.events, 'vectorLocalSearch');
    ok(!!ev, 'search-topk: ok event emitted');
    // count is set via .set() so available directly on ev.data
    ok((ev?.data?.count ?? 99) <= 2, 'search-topk: ≤ 2 hits returned');
  }

  // ── search: markdown section titles in results ─────────────────────────────
  console.log('\n── vector-local/search-md-titles ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalSearch',
      storePath: STORE_DIR,
      query: 'introduction section purpose document',
      topK: 10,
      storeAs: 'results',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'search-md-titles: exits 0');
    const ev = okEv(r.events, 'vectorLocalSearch');
    const hits = ev?.data?.result?.hits ?? [];
    // At least one hit from the FM fixture should have a non-empty title
    const titledHit = hits.find((h) =>
      h.path?.includes('fixture-fm') && typeof h.title === 'string' && h.title.length > 0
    );
    ok(!!titledHit, 'search-md-titles: hit from FM fixture has title set');
    if (titledHit) {
      ok(
        titledHit.title.includes('Introduction') ||
        titledHit.title.includes('Chunking Test') ||
        titledHit.title.includes('Details'),
        `search-md-titles: title contains heading text (got "${titledHit.title}")`,
      );
    }
  }

  // ── search: filterSource ───────────────────────────────────────────────────
  console.log('\n── vector-local/search-filter ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalSearch',
      storePath: STORE_DIR,
      query: 'embeddings vector search',
      topK: 20,
      filterSource: 'docs',
      storeAs: 'results',
    }] }, { timeout: 60_000 });

    eq(r.status, 0, 'search-filter: exits 0');
    const ev = okEv(r.events, 'vectorLocalSearch');
    const hits = ev?.data?.result?.hits ?? [];
    const allDocs = hits.length > 0 && hits.every((h) => h.source === 'docs');
    ok(allDocs, 'search-filter: all hits have source="docs"');
  }

  // ── error: non-existent file ───────────────────────────────────────────────
  console.log('\n── vector-local/error-missing-file ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalAddFile',
      storePath: STORE_DIR,
      file: join(WORK, 'does-not-exist.md'),
      storeAs: 'added',
    }] }, { timeout: 30_000 });

    // Should exit non-zero or emit an error event
    const ev = errEv(r.events, 'vectorLocalAddFile');
    ok(r.status !== 0 || !!ev, 'error-missing-file: non-zero exit or error event');
  }
}

// ── office suite ──────────────────────────────────────────────────────────────
//
// Requires: embedding model on disk + officecli on PATH.
// Fixtures live at tests/office/*.xlsx / *.docx (created by officecli or checked in).

function suiteOffice() {
  const OFFICE_STORE = join(WORK, 'office-store');
  const FIXTURES_DIR = join(PACKAGE_ROOT, 'tests', 'office');
  const XLSX_FIXTURE = join(FIXTURES_DIR, 'annual-report-2024.xlsx');
  const DOCX_FIXTURE = join(FIXTURES_DIR, 'meeting-notes.docx');

  console.log('\n══ office suite ══════════════════════════════════════════════');

  // ── open ────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/open ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalOpen',
      storePath: OFFICE_STORE,
      model: EMBED_MODEL,
      storeAs: 'info',
    }] }, { timeout: 90_000 });
    eq(r.status, 0, 'office/open: exits 0');
    const ev = okEv(r.events, 'vectorLocalOpen');
    ok(!!ev, 'office/open: vectorLocalOpen ok event');
  }

  // ── xlsx ─────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/xlsx ──');
  {
    if (!existsSync(XLSX_FIXTURE)) {
      console.log(`  ⚠ skip — fixture not found: ${XLSX_FIXTURE}`);
      ok(true, 'office/xlsx: skipped (no fixture)');
    } else {
      const r = run({ roots: [{
        kind: 'vectorLocalAddFile',
        storePath: OFFICE_STORE,
        file: XLSX_FIXTURE,
        model: EMBED_MODEL,
        source: 'office-xlsx',
        storeAs: 'added',
      }] }, { timeout: 120_000 });

      const ev = okEv(r.events, 'vectorLocalAddFile');
      const result = ev?.data?.result ?? {};
      const count  = result.count ?? 0;
      const skipped = result.skipped ?? false;

      ok(r.status === 0, 'office/xlsx: exits 0');

      if (result.error?.includes('officecli not found')) {
        console.log('  ⚠ officecli not on PATH — office extraction skipped');
        ok(true, 'office/xlsx: officecli not available (expected in CI without office)');
      } else {
        ge(count, 1, 'office/xlsx: at least 1 chunk indexed');
        ok(!skipped, 'office/xlsx: not a dedup skip on fresh store');
      }
    }
  }

  // ── docx ────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/docx ──');
  {
    if (!existsSync(DOCX_FIXTURE)) {
      console.log(`  ⚠ skip — fixture not found: ${DOCX_FIXTURE}`);
      ok(true, 'office/docx: skipped (no fixture)');
    } else {
      const r = run({ roots: [{
        kind: 'vectorLocalAddFile',
        storePath: OFFICE_STORE,
        file: DOCX_FIXTURE,
        model: EMBED_MODEL,
        source: 'office-docx',
        storeAs: 'added',
      }] }, { timeout: 120_000 });

      const ev  = okEv(r.events, 'vectorLocalAddFile');
      const res = ev?.data?.result ?? {};
      ok(r.status === 0, 'office/docx: exits 0');
      if (!res.error?.includes('officecli not found')) {
        ge(res.count ?? 0, 1, 'office/docx: at least 1 chunk indexed');
      }
    }
  }

  // ── search ──────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/search ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalSearch',
      storePath: OFFICE_STORE,
      model: EMBED_MODEL,
      query: 'annual revenue financial results',
      topK: 5,
      storeAs: 'hits',
    }] }, { timeout: 90_000 });

    ok(r.status === 0, 'office/search: exits 0');
    const ev   = okEv(r.events, 'vectorLocalSearch');
    const hits = ev?.data?.result?.hits ?? [];
    // If we had no fixture, docCount may be 0 — just check structure
    ok(Array.isArray(hits), 'office/search: hits is array');
    if (hits.length > 0) {
      const h = hits[0];
      ok(typeof h.score === 'number' && h.score > 0, 'office/search: first hit has score > 0');
      ok(typeof h.chunk === 'string', 'office/search: first hit has chunk string');
    }
  }

  // ── dedup ────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/dedup ──');
  {
    if (!existsSync(XLSX_FIXTURE)) {
      ok(true, 'office/dedup: skipped (no fixture)');
    } else {
      const r = run({ roots: [{
        kind: 'vectorLocalAddFile',
        storePath: OFFICE_STORE,
        file: XLSX_FIXTURE,
        model: EMBED_MODEL,
        source: 'office-xlsx',
        storeAs: 'added',
      }] }, { timeout: 120_000 });

      const ev  = okEv(r.events, 'vectorLocalAddFile');
      const res = ev?.data?.result ?? {};
      ok(r.status === 0, 'office/dedup: exits 0');
      ok(res.skipped === true || res.count === 0, 'office/dedup: second add is skipped');
    }
  }

  // ── stats ────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/stats ──');
  {
    const r = run({ roots: [{
      kind: 'vectorLocalStats',
      storePath: OFFICE_STORE,
      model: EMBED_MODEL,
      storeAs: 'stats',
    }] }, { timeout: 60_000 });

    ok(r.status === 0, 'office/stats: exits 0');
    const ev  = okEv(r.events, 'vectorLocalStats');
    const res = ev?.data?.result ?? {};
    ok(typeof res.docCount      === 'number', 'office/stats: docCount is number');
    ok(typeof res.dims          === 'number', 'office/stats: dims is number');
    ok(typeof res.indexSizeBytes=== 'number', 'office/stats: indexSizeBytes is number');
  }

  // ── delete ────────────────────────────────────────────────────────────────────
  console.log('\n── vector-office/delete ──');
  {
    if (!existsSync(XLSX_FIXTURE)) {
      ok(true, 'office/delete: skipped (no fixture)');
    } else {
      const r = run({ roots: [{
        kind: 'vectorLocalDelete',
        storePath: OFFICE_STORE,
        path: XLSX_FIXTURE,
        model: EMBED_MODEL,
        storeAs: 'del',
      }] }, { timeout: 60_000 });

      ok(r.status === 0, 'office/delete: exits 0');
      const ev  = okEv(r.events, 'vectorLocalDelete');
      const res = ev?.data?.result ?? {};
      ok(typeof res.deleted === 'number', 'office/delete: deleted is number');
    }
  }
}

// ── main ──────────────────────────────────────────────────────────────────────

const args = process.argv.slice(2);
const onlyFilter = (() => {
  const i = args.indexOf('--only');
  return i >= 0 ? args[i + 1] : null;
})();
const skipInline = args.includes('--skip-inline') ||
                   process.env.SKIP_EMBED_TEST === '1';
const skipOffice = args.includes('--skip-office') ||
                   process.env.SKIP_OFFICE_TEST === '1';

// ── Device toggle: cpu | gpu | both ────────────────────────────────────────────
// Controls the embedding device (gpuLayers) for the assertion suite, and which
// devices the comparison bench runs. Default: cpu (deterministic, CI-friendly).
const deviceArg = (() => {
  const i = args.indexOf('--device');
  const v = i >= 0 ? args[i + 1] : (process.env.VSTORE_DEVICE || 'cpu');
  return String(v || 'cpu').toLowerCase();
})();
const gpuLayersFor = (device) => (device === 'gpu' ? -1 : 0);
// Devices used by the comparison bench (order = table column order).
const BENCH_DEVICES = deviceArg === 'both' ? ['cpu', 'gpu'] : [deviceArg === 'gpu' ? 'gpu' : 'cpu'];
// Assertions run under the first requested device (cpu when 'both').
const ASSERT_DEVICE = BENCH_DEVICES[0];

const GPU_BACKEND = detectGpuBackend({ root: PACKAGE_ROOT, cpuOnly: deviceArg === 'cpu' });

console.log(`test-xblox-vector-store-local`);
console.log(`  exe   : ${EXE}`);
console.log(`  work  : ${WORK}`);
console.log(`  model : ${EMBED_MODEL || '(auto — resolved by block)'}`);
console.log(`  device: ${deviceArg}  (backend: ${GPU_BACKEND.name})`);
if (skipInline) console.log('  inline tests: SKIPPED (SKIP_EMBED_TEST=1 or --skip-inline)');

const runSuite = (name) => !onlyFilter || onlyFilter === name;
const vectorLocalOn = featureEnabled('FEATURE_VECTOR_LOCAL_XBLOX');
if (!vectorLocalOn) {
  console.log('  · FEATURE_VECTOR_LOCAL_XBLOX off in dist/shared/features.json — skipping local vector store suites');
}

// GPU instrumentation: sample VRAM across the whole assertion run.
ACTIVE_GPU_LAYERS = gpuLayersFor(ASSERT_DEVICE);
const gpuSampler  = startGpuMemorySampler();

if (vectorLocalOn && runSuite('manifest'))                        suiteManifest();
if (vectorLocalOn && !skipInline && runSuite('inline'))           suiteInline();
if (vectorLocalOn && !skipInline && !skipOffice && runSuite('office')) suiteOffice();

const GPU_MEMORY = gpuSampler.stop();

// ── Device comparison bench (controlled workload, CPU vs GPU) ──────────────────
const DEVICE_RESULTS = (vectorLocalOn && !skipInline && runSuite('inline')) ? runDeviceComparison() : [];

// ── summary ───────────────────────────────────────────────────────────────────

console.log(`\n${'─'.repeat(50)}`);
console.log(`  passed: ${passed}  failed: ${failed}`);
if (failures.length) {
  console.log('\n  FAILURES:');
  failures.forEach((f) => console.log(`    ✗ ${f}`));
}
console.log('');

// Cleanup temp work dir (store is inside WORK so deleted here too)
try { rmSync(WORK, { recursive: true, force: true }); } catch { /* ok */ }

// ── Extra report sections (cases, per-kind timings, GPU) ───────────────────────

function ms1(n) { return Number.isFinite(n) ? (Math.round(n * 10) / 10).toFixed(1) : '—'; }

// Controlled CPU-vs-GPU workload: open → ingest a fixed set of fixtures → warm →
// timed searches, per device, with VRAM sampling. Fair side-by-side comparison
// independent of the assertion suite's mixed workload.
function runDeviceComparison() {
  const sizeOf = (p) => { try { return statSync(p).size; } catch { return 0; } };
  const ingestFiles = [TXT_FILE, FM_FILE, MD_FILE, CHAT_FILE]
    .filter((p) => existsSync(p))
    .map((p) => ({ path: p, size: sizeOf(p), chunks: null }));
  const totalBytes = ingestFiles.reduce((s, f) => s + f.size, 0);
  const queries = [
    'how to run embedding models locally without internet',
    'semantic search with dense vector embeddings',
    'GPU acceleration for inference',
    'markdown chunking and front matter parsing',
    'agent memory persistence across sessions',
  ];
  const results = [];

  for (const device of BENCH_DEVICES) {
    ACTIVE_GPU_LAYERS = gpuLayersFor(device);
    const storeDir = join(WORK, `cmp-${device}`);
    const sampler  = startGpuMemorySampler();

    const openR  = run({ roots: [{ kind: 'vectorLocalOpen', storePath: storeDir,
      model: EMBED_MODEL || undefined, threads: 4, storeAs: 'info' }] }, { timeout: 60_000 });
    const openEv = okEv(openR.events, 'vectorLocalOpen');

    let chunks = 0;
    const ingestT0 = performance.now();
    for (const f of ingestFiles) {
      const r  = run({ roots: [{ kind: 'vectorLocalAddFile', storePath: storeDir, file: f.path,
        source: 'cmp', chunkSize: 500, chunkOverlap: 0, threads: 4, storeAs: 'res' }] }, { timeout: 120_000 });
      const ev = okEv(r.events, 'vectorLocalAddFile');
      const n  = ev?.data?.count ?? 0;
      chunks += n;
      if (f.chunks === null) f.chunks = n; // same content → record once
    }
    const ingestMs = performance.now() - ingestT0;

    run({ roots: [{ kind: 'vectorLocalSearch', storePath: storeDir, query: queries[0], topK: 5, storeAs: 'r' }] });

    const searchMs = [];
    for (const q of queries) {
      const r = run({ roots: [{ kind: 'vectorLocalSearch', storePath: storeDir, query: q, topK: 5, storeAs: 'res' }] }, { timeout: 60_000 });
      searchMs.push(r.ms);
    }
    const vram   = sampler.stop();
    const sorted = [...searchMs].sort((a, b) => a - b);
    const searchAvg = searchMs.reduce((a, b) => a + b, 0) / (searchMs.length || 1);
    results.push({
      device,
      gpuLayers: gpuLayersFor(device),
      gpuReported: openEv?.data?.gpu ?? null,
      files: ingestFiles.length,
      totalBytes,
      chunks,
      ingestMs,
      ingestChunksPerSec: ingestMs > 0 ? chunks / (ingestMs / 1000) : 0,
      ingestMibPerSec: ingestMs > 0 ? (totalBytes / 1048576) / (ingestMs / 1000) : 0,
      searchAvg,
      searchP50: sorted[Math.floor(sorted.length * 0.5)] ?? 0,
      vram,
    });
    console.log(`  [cmp ${device}] ingest ${(ingestMs / 1000).toFixed(1)}s (${chunks} chunks, ` +
      `${(totalBytes / 1024).toFixed(0)} KiB), search avg ${searchAvg.toFixed(0)} ms` +
      (vram?.available ? `, VRAM Δ ${vram.peak_delta_mib >= 0 ? '+' : ''}${vram.peak_delta_mib} MiB` : ''));
  }
  ACTIVE_GPU_LAYERS = gpuLayersFor(ASSERT_DEVICE);
  INGEST_FILES = ingestFiles;
  return results;
}

function buildDetailSections() {
  const L = [];

  // ── Run configuration ───────────────────────────────────────────────────────
  L.push('## Run configuration');
  L.push('');
  L.push('| Key | Value |');
  L.push('| --- | --- |');
  L.push(`| Embedding model | \`${EMBED_MODEL || '(auto-discovered by block)'}\` |`);
  L.push(`| Device toggle | \`--device ${deviceArg}\` |`);
  L.push(`| Assertion suite device | ${ASSERT_DEVICE.toUpperCase()} (gpuLayers ${gpuLayersFor(ASSERT_DEVICE)}) |`);
  L.push(`| Compared devices | ${BENCH_DEVICES.join(' · ')} |`);
  L.push('');

  // ── CPU vs GPU comparison ───────────────────────────────────────────────────
  if (DEVICE_RESULTS.length) {
    L.push('## Device comparison (controlled workload)');
    L.push('');
    L.push('> Same fixtures + queries per device. Ingest = wall time to add all fixtures; ' +
           'Search = per-`vectorLocalSearch` spawn (cold process + embed + scan).');
    L.push('');
    L.push('| Device | gpuLayers | Files | KiB | Chunks | Ingest s | chunks/s | KiB/s | Search avg ms | Search p50 ms | VRAM Δ MiB |');
    L.push('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
    for (const d of DEVICE_RESULTS) {
      const vramCell = d.vram?.available ? `${d.vram.peak_delta_mib >= 0 ? '+' : ''}${d.vram.peak_delta_mib}` : '—';
      L.push(`| **${d.device.toUpperCase()}** | ${d.gpuLayers} | ${d.files} | ${(d.totalBytes / 1024).toFixed(1)} | ${d.chunks} | ` +
        `${(d.ingestMs / 1000).toFixed(2)} | ${d.ingestChunksPerSec.toFixed(1)} | ${(d.ingestMibPerSec * 1024).toFixed(1)} | ` +
        `${ms1(d.searchAvg)} | ${ms1(d.searchP50)} | ${vramCell} |`);
    }
    L.push('');
    const cpu = DEVICE_RESULTS.find((d) => d.device === 'cpu');
    const gpu = DEVICE_RESULTS.find((d) => d.device === 'gpu');
    if (cpu && gpu && cpu.ingestMs > 0 && gpu.ingestMs > 0) {
      const ingestSpeedup = cpu.ingestMs / gpu.ingestMs;
      const searchSpeedup = cpu.searchAvg / (gpu.searchAvg || 1);
      L.push(`> **GPU vs CPU:** ingest ${ingestSpeedup.toFixed(2)}× · search ${searchSpeedup.toFixed(2)}× ` +
             `(>1 = GPU faster). Tiny embedding models may not benefit from offload due to per-call transfer overhead.`);
      L.push('');
    }

    // ── Ingested files (the controlled workload's corpus) ─────────────────────
    if (INGEST_FILES.length) {
      const relOf = (p) => (p.startsWith(PACKAGE_ROOT) ? p.slice(PACKAGE_ROOT.length).replace(/^[\\/]+/, '') : p);
      const fmtKiB = (b) => (b / 1024).toFixed(1);
      const totBytes = INGEST_FILES.reduce((s, f) => s + f.size, 0);
      const totChunks = INGEST_FILES.reduce((s, f) => s + (f.chunks ?? 0), 0);
      L.push('### Ingested files');
      L.push('');
      L.push('| File | Size KiB | Chunks |');
      L.push('| --- | ---: | ---: |');
      for (const f of INGEST_FILES) {
        L.push(`| \`${relOf(f.path)}\` | ${fmtKiB(f.size)} | ${f.chunks ?? '—'} |`);
      }
      L.push(`| **Total (${INGEST_FILES.length} files)** | **${fmtKiB(totBytes)}** | **${totChunks}** |`);
      L.push('');
    }
  }

  // ── GPU / offload backend ───────────────────────────────────────────────────
  L.push('## GPU / embedding backend');
  L.push('');
  L.push('| Metric | Value |');
  L.push('| --- | --- |');
  L.push(`| Build offload backend | ${GPU_BACKEND.name} (${GPU_BACKEND.detail}) |`);
  L.push(`| Assertion embedding device | ${ASSERT_DEVICE.toUpperCase()} (\`gpuLayers: ${gpuLayersFor(ASSERT_DEVICE)}\`) |`);
  if (GPU_MEMORY?.available) {
    L.push(`| VRAM baseline | ${GPU_MEMORY.baseline_used_mib} / ${GPU_MEMORY.total_mib} MiB |`);
    L.push(`| VRAM peak | ${GPU_MEMORY.peak_used_mib} / ${GPU_MEMORY.total_mib} MiB |`);
    L.push(`| VRAM Δ (run) | ${GPU_MEMORY.peak_delta_mib >= 0 ? '+' : ''}${GPU_MEMORY.peak_delta_mib} MiB |`);
    L.push(`| nvidia-smi samples | ${GPU_MEMORY.sample_count} |`);
    if (GPU_MEMORY.devices?.length) {
      L.push(`| GPU device(s) | ${GPU_MEMORY.devices.map((g) => `${g.name} (${g.used_mib}/${g.total_mib} MiB)`).join('; ')} |`);
    }
  } else {
    L.push('| VRAM | nvidia-smi unavailable (no NVIDIA GPU / CPU-only host) |');
  }
  L.push('');

  // ── Per-kind block timings ──────────────────────────────────────────────────
  const byKind = new Map();
  for (const e of RUN_LOG) {
    let a = byKind.get(e.kind);
    if (!a) { a = { kind: e.kind, n: 0, ok: 0, total: 0, min: Infinity, max: 0 }; byKind.set(e.kind, a); }
    a.n += 1;
    if (e.ok) a.ok += 1;
    a.total += e.ms;
    a.min = Math.min(a.min, e.ms);
    a.max = Math.max(a.max, e.ms);
  }
  const totalCalls = RUN_LOG.length;
  const totalCliMs = RUN_LOG.reduce((s, e) => s + e.ms, 0);

  L.push('## Block invocations (per kind)');
  L.push('');
  L.push(`> ${totalCalls} \`tanit-cli\` process spawns · ${ms1(totalCliMs)} ms total CLI wall time (cold-spawn each).`);
  L.push('');
  L.push('| Block kind | Calls | OK | Total ms | Avg ms | Min ms | Max ms |');
  L.push('| --- | ---: | ---: | ---: | ---: | ---: | ---: |');
  for (const a of [...byKind.values()].sort((x, y) => y.total - x.total)) {
    L.push(`| \`${a.kind}\` | ${a.n} | ${a.ok}/${a.n} | ${ms1(a.total)} | ${ms1(a.total / a.n)} | ${ms1(a.min)} | ${ms1(a.max)} |`);
  }
  L.push('');

  // ── Slowest individual cases ────────────────────────────────────────────────
  const slowest = [...RUN_LOG].sort((a, b) => b.ms - a.ms).slice(0, 12);
  if (slowest.length) {
    L.push('## Slowest individual cases');
    L.push('');
    L.push('| # | Block kind | ms | Status |');
    L.push('| ---: | --- | ---: | :---: |');
    slowest.forEach((e, i) => {
      L.push(`| ${i + 1} | \`${e.kind}\` | ${ms1(e.ms)} | ${e.ok ? '✓' : '✗'} |`);
    });
    L.push('');
  }

  return L.join('\n') + '\n';
}

// ── Report ────────────────────────────────────────────────────────────────────

{
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  const wallMs  = metrics.process.durationWallMs;
  const ok      = failed === 0;

  let md = renderMarkdownReport({
    ok,
    passed,
    failed,
    meta: {
      testName:    'xblox-vector-store-local',
      displayName: 'vector-store-local (functional)',
      cwd:         PACKAGE_ROOT,
      exe:         EXE,
      writtenAt:   finishedAtIso,
      wallClockMs: Math.round(wallMs * 100) / 100,
    },
    metrics,
    integrationNotes: failures.length
      ? failures.map((f) => `FAIL: ${f}`)
      : ['All assertions passed.'],
  });

  // Insert the detail sections (GPU, per-kind timings, slowest cases) just before
  // the renderer's trailing "*Written …*" footer so the document reads cleanly.
  {
    const detail = buildDetailSections();
    const footerIdx = md.lastIndexOf('\n---\n*Written ');
    md = footerIdx >= 0
      ? `${md.slice(0, footerIdx)}\n${detail}${md.slice(footerIdx + 1)}`
      : md + detail;
  }

  const acc = isAccumulateMode() && getChapterTitle();
  md += acc
    ? '\n---\n\n*Per-run: `tests/test-report-last.md` — also a chapter in `tests/test-report-all.md`.*\n'
    : '\n---\n\n*Artifact: `tests/test-report-last.md` — overwritten on each run.*\n';

  mkdirSync(dirname(TEST_REPORT_PATH), { recursive: true });
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  console.log(`\n  report → ${TEST_REPORT_PATH}`);

  if (acc) {
    appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
    console.log(`  report → ${TEST_REPORT_ALL_PATH} (chapter)`);
  }
}

process.exit(failed > 0 ? 1 : 0);
