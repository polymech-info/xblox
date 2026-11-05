#!/usr/bin/env node
/**
 * test-xblox-vector-store-local-bench.mjs
 *
 * Multi-model embedding quality + speed benchmark for the local offline vector store.
 *
 * Detects all available GGUF models in dist/models/ and runs each through the
 * same pipeline, producing a side-by-side comparison table in the report.
 *
 * Phases (per model):
 *   1. ingest   — add all docs/**\/*.md into a per-model store, timed
 *   2. warm-up  — 3 throwaway queries to ensure the model is hot
 *   3. semantic — curated queries that should work; reports cosine scores
 *   4. edge     — adversarial queries that expose limitations
 *   5. report   — comparison table + per-model detail sections
 *
 * Speed unit: ms/query — wall time of one vectorLocalSearch spawnSync call.
 *   Includes: process startup + query embedding + brute-force cosine scan.
 *   Warm-up queries are discarded; only post-warm timings count.
 *
 * Run:
 *   npm run test:xblox:vector-store-local:bench
 *   node tests/orchestrator/test-xblox-vector-store-local-bench.mjs
 *   node ... --models all                   # all detected models (default)
 *   node ... --models small-q8              # slug match (bge-small-en-v1.5-q8_0)
 *   node ... --models small-q8,base-q8      # comma-separated slugs
 *   node ... --skip-ingest                  # reuse existing stores (same model set)
 *   node ... --only semantic                # skip edge-case queries
 *   node ... --only edge                    # skip semantic queries
 *
 * Output:
 *   tests/test-report-last.md              (always overwritten)
 *   tests/test-report-vector-store-bench.md (dedicated, always overwritten)
 *   tests/test-report-all.md               (appended when MEDIA_CPP_TEST_REPORT_ACCUMULATE=1)
 */

import { performance } from 'node:perf_hooks';
import { spawnSync } from 'node:child_process';
import {
  existsSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync,
} from 'node:fs';
import { join, resolve, dirname, basename, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { buildMetricsBundle, createMetricsCollector } from '../../../../tests/orchestrator/reports.js';
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '../../../../tests/orchestrator/test-report-accumulate.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT     = resolve(__dirname, '..', '..', '..', '..');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');
const BENCH_STORE_ROOT = join(PACKAGE_ROOT, 'out', 'bench-stores');
const DOCS_DIR         = join(PACKAGE_ROOT, 'docs');
const MODELS_DIR       = join(PACKAGE_ROOT, 'dist', 'models');

// ── CLI exe ───────────────────────────────────────────────────────────────────

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env?.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

const EXE = cliExePath();

// ── Model discovery ───────────────────────────────────────────────────────────
//
// Known-good catalog (prefer these; in order small→large, q8→f16)
const CATALOG = [
  { slug: 'small-q8',  file: 'bge-small-en-v1.5-q8_0.gguf', dims: 384, label: 'BGE-small q8_0' },
  { slug: 'small-f16', file: 'bge-small-en-v1.5-f16.gguf',   dims: 384, label: 'BGE-small f16'  },
  { slug: 'base-q8',   file: 'bge-base-en-v1.5-q8_0.gguf',   dims: 768, label: 'BGE-base q8_0'  },
  { slug: 'base-f16',  file: 'bge-base-en-v1.5-f16.gguf',    dims: 768, label: 'BGE-base f16'   },
];

/**
 * Returns an array of { slug, label, path, sizeMb } for all models that should
 * be benchmarked, based on argv and PM_EMBED_MODEL.
 *
 * Slug matching priority:
 *   1. Catalog entries present in dist/models/   (slugs: small-q8, small-f16, base-q8, base-f16)
 *   2. PM_EMBED_MODEL env var (slug derived from basename, also matches 'custom' / 'env')
 *   3. Any *.gguf in dist/models/                (slug = filename without .gguf)
 *   4. Absolute path passed directly as a slug
 */
function discoverModels(requestedSlugs) {
  const env = process.env.PM_EMBED_MODEL?.trim();
  const wantAll = !requestedSlugs || requestedSlugs === 'all';
  const wanted  = wantAll ? null : requestedSlugs.split(',').map((s) => s.trim().toLowerCase());

  // ── 1. Catalog models present on disk ──────────────────────────────────────
  const catalog = CATALOG
    .map((c) => ({ ...c, path: join(MODELS_DIR, c.file) }))
    .filter((c) => existsSync(c.path))
    .map((c) => ({ ...c, sizeMb: fileMb(c.path) }));

  // ── 2. PM_EMBED_MODEL env var ──────────────────────────────────────────────
  // Matchable as: 'custom', 'env', or any prefix of its basename (e.g. 'all-MiniLM')
  let envModel = null;
  if (env && existsSync(env)) {
    const nameSlug = basename(env, extname(env)).toLowerCase();
    envModel = { slug: nameSlug, label: basename(env, extname(env)), path: env, sizeMb: fileMb(env) };
  }

  // ── 3. Any other *.gguf in dist/models/ (fallback) ────────────────────────
  const extra = [];
  if (existsSync(MODELS_DIR)) {
    for (const f of readdirSync(MODELS_DIR)) {
      if (!f.endsWith('.gguf')) continue;
      const p = join(MODELS_DIR, f);
      // Skip files already in catalog
      if (catalog.some((c) => c.path === p)) continue;
      extra.push({ slug: basename(f, '.gguf').toLowerCase(), label: basename(f, '.gguf'), path: p, sizeMb: fileMb(p) });
    }
  }

  // ── 4. Absolute / relative path passed as slug ────────────────────────────
  const pathModels = [];
  if (wanted) {
    for (const w of wanted) {
      const abs = resolve(w);
      if (existsSync(abs) && abs.endsWith('.gguf')) {
        pathModels.push({ slug: slugify(abs), label: basename(abs, '.gguf'), path: abs, sizeMb: fileMb(abs) });
      }
    }
  }

  // ── Assemble + filter ──────────────────────────────────────────────────────
  const all = [...pathModels, ...catalog, ...(envModel ? [envModel] : []), ...extra];
  // Deduplicate by resolved path
  const seen = new Set();
  const deduped = all.filter((m) => { const k = m.path; return seen.has(k) ? false : (seen.add(k), true); });

  if (wantAll) return deduped;

  // Match each wanted slug against: slug, label-lowercase, basename-lowercase
  const matched = [];
  const missingInCatalog = [];

  for (const w of wanted) {
    const hit = deduped.find(
      (m) => m.slug === w ||
             m.label.toLowerCase() === w ||
             basename(m.path).toLowerCase() === w ||
             basename(m.path, '.gguf').toLowerCase() === w
    );
    if (hit) {
      matched.push(hit);
    } else {
      // See if it's a known catalog slug (helps error message)
      const catalogEntry = CATALOG.find((c) => c.slug === w);
      if (catalogEntry) {
        missingInCatalog.push({ ...catalogEntry, path: join(MODELS_DIR, catalogEntry.file) });
      }
    }
  }

  // Report missing catalog models with exact paths
  if (missingInCatalog.length) {
    console.error('\nModels requested but not found on disk:');
    for (const m of missingInCatalog) {
      console.error(`  ${m.slug.padEnd(12)}  expected: ${m.path}`);
    }
    console.error('\nDownload with:');
    console.error('  npm run download:bge-models:win       # Windows');
    console.error('  npm run download:bge-models           # Linux/macOS');
    if (envModel) {
      console.error(`\nFound via PM_EMBED_MODEL: ${envModel.path}`);
      console.error('  Run with --models all to use it, or without --models to use all available.');
    }
    if (matched.length === 0) {
      process.exit(1);
    }
    console.error(`\nProceeding with ${matched.length} matched model(s).\n`);
  }

  return matched;
}

function fileMb(p) {
  try { return Math.round(statSync(p).size / 1e6 * 10) / 10; } catch { return 0; }
}

function slugify(p) {
  return basename(p, extname(p)).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 48);
}

// ── xblox runner ─────────────────────────────────────────────────────────────

function runBlock(doc, { timeout = 180_000 } = {}) {
  const tmp = join(PACKAGE_ROOT, 'out', `_bench_${Math.random().toString(36).slice(2)}.json`);
  mkdirSync(dirname(tmp), { recursive: true });
  writeFileSync(tmp, JSON.stringify(doc, null, 2), 'utf8');
  const t0 = performance.now();
  const r = spawnSync(EXE, ['xblox', '--log-level', 'off', 'run', '--json', '--no-wait', '--src', tmp], {
    cwd: PACKAGE_ROOT, encoding: 'utf8', timeout,
  });
  const ms = performance.now() - t0;
  try { rmSync(tmp); } catch { /* ok */ }
  const report = parseJson(r.stdout || '');
  return { status: r.status ?? -1, events: report?.events ?? [], ms };
}

function parseJson(txt) {
  const s = (txt || '').trim();
  if (!s) return null;
  const lastNl = s.lastIndexOf('\n{');
  if (lastNl >= 0) { try { return JSON.parse(s.slice(lastNl + 1)); } catch { /* next */ } }
  const first = s.indexOf('{');
  if (first >= 0) { try { return JSON.parse(s.slice(first)); } catch { /* next */ } }
  try { return JSON.parse(s); } catch { return null; }
}

function okEv(events, kind) {
  return events.find((e) => e.kind === kind && e.status === 'ok');
}

// ── Collect docs ──────────────────────────────────────────────────────────────

function collectMdFiles(root) {
  const out = [];
  function walk(dir) {
    for (const ent of readdirSync(dir, { withFileTypes: true })) {
      const full = join(dir, ent.name);
      if (ent.isDirectory()) { walk(full); continue; }
      if (ent.isFile() && ent.name.endsWith('.md')) {
        const sz = statSync(full).size;
        if (sz > 0 && sz <= 1_048_576) out.push(full);
      }
    }
  }
  walk(root);
  return out;
}

// ── Query suite ───────────────────────────────────────────────────────────────

const QUERIES = [
  // ── Semantic — should work well ───────────────────────────────────────────
  { label: 'local LLM embedding',      query: 'how to run embedding models locally without internet', expect: ['llm','embed','local','gguf','model'] },
  { label: 'GPU acceleration CUDA',    query: 'CUDA GPU acceleration for inference', expect: ['cuda','gpu','onnx','vulkan'] },
  { label: 'xblox scripting blocks',   query: 'scripting blocks execution flow', expect: ['xblox','block','flow','run'] },
  { label: 'agent memory persistence', query: 'how does the agent remember previous conversations', expect: ['memory','agent','persist','store'] },
  { label: 'markdown rendering',       query: 'render markdown to HTML in the viewer', expect: ['markdown','md4c','html','viewer','render'] },
  { label: 'MCP client tools',         query: 'model context protocol client tool calls', expect: ['mcp','tool','client'] },
  { label: 'audio speech STT',         query: 'transcribe microphone audio speech to text', expect: ['audio','whisper','stt','speech','transcri'] },
  { label: 'image ONNX classify',      query: 'classify images using ONNX runtime model', expect: ['onnx','classif','image','model'] },
  { label: 'WebView2 Win32 UI',        query: 'embed web view inside native Win32 window', expect: ['webview','win32','ui','window'] },
  { label: 'video YOLO detection',     query: 'detect objects in video frames with YOLO', expect: ['detect','video','yolo','object'] },

  // ── Edge — probing limitations ────────────────────────────────────────────
  { label: 'negation (slow/bad)',   edge: true, note: 'Embeddings ignore negation — polarity-blind', query: 'slow performance bad latency', expect: ['perform','latency','speed','optim'] },
  { label: 'single token',         edge: true, note: 'Ambiguous — RAM vs agent memory vs VStore', query: 'memory', expect: [] },
  { label: 'OOC — quantum',        edge: true, note: 'Completely out-of-corpus; scores should be low', query: 'quantum computing entanglement superposition', expect: [], expectLowScore: true },
  { label: 'synonym gap (pics)',    edge: true, note: '"pictures" ↔ "images" synonym coverage test', query: 'gallery of pictures thumbnails', expect: ['image','picture','gallery','thumb'] },
  { label: 'code snippet (CMake)', edge: true, note: 'Code fragment query vs prose corpus', query: 'target_link_libraries pm-media PRIVATE llama', expect: ['cmake','link','llama','media'] },
  { label: 'paraphrase (British)', edge: true, note: 'Non-standard spelling + unusual phrasing', query: 'catalogue documents on disk for later retrieval', expect: ['index','file','store','embed','search'] },
  { label: 'very long query',      edge: true, note: 'Long query — does extra context hurt retrieval?', query: 'I want to understand how to configure a local language model for embedding, running fully offline without any cloud API, using a GGUF quantized checkpoint, perhaps all-MiniLM or nomic-embed, producing 384-dimensional float vectors that I can then store in a flat binary index and search with cosine similarity. What are the configuration options and which models are supported?', expect: ['embed','model','gguf','local','vector'] },
  { label: 'OOD — Bluetooth',      edge: true, note: 'Out-of-domain niche topic; no expected match', query: 'Bluetooth serial port pairing device discovery', expect: [] },
  { label: 'numeric only',         edge: true, note: 'Pure numbers — embedding dimension sizes', query: '384 512 768 1024 1536', expect: [] },
  { label: 'punctuation only',     edge: true, note: 'Degenerate query — tokenisation edge case', query: '--- *** /// ...', expect: [] },
];

const TOPK        = 5;
const WARMUP_COUNT = 3;

// ── Per-model benchmark ───────────────────────────────────────────────────────

function computeStats(arr) {
  if (!arr.length) return { min: 0, avg: 0, p50: 0, p95: 0, max: 0 };
  const s = [...arr].sort((a, b) => a - b);
  const avg = s.reduce((a, b) => a + b, 0) / s.length;
  return {
    min: s[0], avg, p50: s[Math.floor(s.length * 0.5)],
    p95: s[Math.floor(s.length * 0.95)], max: s[s.length - 1],
  };
}

function queryRelevanceScore(results) {
  // Percentage of results with expects that hit ≥ 50% keywords in top-3
  const withExpect = results.filter((r) => r.expect?.length);
  if (!withExpect.length) return null;
  const passing = withExpect.filter((r) => {
    const text = r.hits.slice(0, 3)
      .map((h) => `${h.path ?? ''} ${h.title ?? ''} ${h.chunk ?? ''}`.toLowerCase()).join(' ');
    const matched = r.expect.filter((kw) => text.includes(kw.toLowerCase()));
    return matched.length / r.expect.length >= 0.5;
  });
  return Math.round(passing.length / withExpect.length * 100);
}

async function benchmarkModel(model, mdFiles, skipIngest, onlyFilter) {
  const storeDir = join(BENCH_STORE_ROOT, slugify(model.path));
  console.log(`\n${'═'.repeat(60)}`);
  console.log(`  Model : ${model.label}  (${model.sizeMb} MB)`);
  console.log(`  Path  : ${model.path}`);
  console.log(`  Store : ${storeDir}`);
  console.log('');

  let dims       = model.dims ?? 0;
  let chunksTotal = 0;
  let ingestMs   = 0;
  let actualModelPath = model.path;

  // ── Ingest ───────────────────────────────────────────────────────────────
  if (!skipIngest) {
    try { rmSync(storeDir, { recursive: true, force: true }); } catch { /* ok */ }
    mkdirSync(storeDir, { recursive: true });

    const openR = runBlock({ roots: [{ kind: 'vectorLocalOpen', storePath: storeDir, model: model.path, threads: 4, storeAs: 'info' }] }, { timeout: 30_000 });
    const openEv = okEv(openR.events, 'vectorLocalOpen');
    actualModelPath = openEv?.data?.modelPath ?? model.path;
    dims = openEv?.data?.dims ?? dims;
    console.log(`  [1/4] Ingesting ${mdFiles.length} files …`);

    const ingestT0 = performance.now();
    let filesAdded = 0;
    for (let i = 0; i < mdFiles.length; i++) {
      if (i % 25 === 0) process.stdout.write(`    ${i + 1}/${mdFiles.length}\r`);
      const r = runBlock({ roots: [{ kind: 'vectorLocalAddFile', storePath: storeDir, file: mdFiles[i], source: 'docs', chunkSize: 500, chunkOverlap: 0, threads: 4, storeAs: 'res' }] }, { timeout: 120_000 });
      const ev = okEv(r.events, 'vectorLocalAddFile');
      if (ev) { chunksTotal += ev.data?.count ?? 0; filesAdded++; }
    }
    ingestMs = performance.now() - ingestT0;
    console.log(`    Added ${filesAdded}/${mdFiles.length} → ${chunksTotal} chunks in ${(ingestMs / 1000).toFixed(1)}s`);
  } else {
    const openR = runBlock({ roots: [{ kind: 'vectorLocalOpen', storePath: storeDir, model: model.path, storeAs: 'info' }] }, { timeout: 30_000 });
    const ev = okEv(openR.events, 'vectorLocalOpen');
    actualModelPath = ev?.data?.modelPath ?? model.path;
    dims       = ev?.data?.dims ?? dims;
    chunksTotal = ev?.data?.docCount ?? 0;
    console.log(`  [1/4] Skipped (--skip-ingest). Existing store: ${chunksTotal} docs`);
  }

  // ── Warm-up ───────────────────────────────────────────────────────────────
  console.log('  [2/4] Warming up …');
  const wT0 = performance.now();
  for (let i = 0; i < WARMUP_COUNT; i++) {
    runBlock({ roots: [{ kind: 'vectorLocalSearch', storePath: storeDir, query: 'local language model embedding vector search', topK: 3, storeAs: 'r' }] });
  }
  const warmupMs = performance.now() - wT0;
  console.log(`    Done (${Math.round(warmupMs)} ms for ${WARMUP_COUNT} queries)`);

  // ── Queries ───────────────────────────────────────────────────────────────
  function runSuite(queries, tag) {
    console.log(`  [${tag}] ${queries.length} queries …`);
    return queries.map((q) => {
      const r = runBlock({ roots: [{ kind: 'vectorLocalSearch', storePath: storeDir, query: q.query, topK: TOPK, storeAs: 'res' }] }, { timeout: 60_000 });
      const ev  = okEv(r.events, 'vectorLocalSearch');
      const hits = ev?.data?.result?.hits ?? [];
      const top = hits[0];
      console.log(`    ${q.label.padEnd(36)} ${r.ms.toFixed(0).padStart(5)} ms  top=${top ? top.score.toFixed(3) : ' — '}`);
      return { ...q, hits, ms: r.ms };
    });
  }

  const semanticResults = (onlyFilter !== 'edge')
    ? runSuite(QUERIES.filter((q) => !q.edge), '3/4')
    : [];
  const edgeResults = (onlyFilter !== 'semantic')
    ? runSuite(QUERIES.filter((q) => q.edge), '4/4')
    : [];

  const allMs   = [...semanticResults, ...edgeResults].map((r) => r.ms);
  const speed   = computeStats(allMs);
  const quality = queryRelevanceScore(semanticResults);

  return {
    model,
    actualModelPath,
    dims,
    chunksTotal,
    ingestMs,
    warmupMs,
    ingestChunksPerSec: ingestMs > 0 ? chunksTotal / (ingestMs / 1000) : 0,
    semanticResults,
    edgeResults,
    speed,
    quality,
  };
}

// ── Markdown report ───────────────────────────────────────────────────────────

function esc(s) {
  return String(s ?? '').replace(/\|/g, '\\|').replace(/\r?\n/g, ' ').slice(0, 120);
}
function scoreBar(s) {
  const n = Math.max(0, Math.min(1, Number(s) || 0));
  const f = Math.round(n * 10);
  return '█'.repeat(f) + '░'.repeat(10 - f) + ` ${n.toFixed(3)}`;
}
function ms1(n) { return Number.isFinite(n) ? n.toFixed(1) : '—'; }

function buildReport(runs, allDocFiles, metrics, startedAt, finishedAt) {
  const lines = [];

  lines.push('# Vector Store Local — Multi-Model Benchmark');
  lines.push('');
  lines.push(`*${runs.length} model(s) · ${allDocFiles.length} docs · ${TOPK}-NN cosine · ${new Date(finishedAt).toUTCString()}*`);
  lines.push('');

  // ── Host ──────────────────────────────────────────────────────────────────
  const host = metrics?.host ?? {};
  if (host.cpuModel) {
    lines.push(`> **Host:** ${host.cpuCount}× ${host.cpuModel} · ${(host.totalMemBytes / 1e9).toFixed(0)} GB RAM · ${host.platform}`);
    lines.push('');
  }

  // ── Side-by-side comparison table ────────────────────────────────────────
  lines.push('## Model Comparison');
  lines.push('');
  lines.push('> Speed = wall-time ms per `vectorLocalSearch` call (post-warm-up, includes process startup + embed + scan).');
  lines.push('> Quality = % of semantic queries where expected keywords appear in top-3 results.');
  lines.push('');
  lines.push('| Model | Dims | Size MB | Chunks | Ingest chunks/s | Avg ms | p50 ms | p95 ms | Quality |');
  lines.push('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
  for (const r of runs) {
    const q = r.quality != null ? `${r.quality}%` : '—';
    lines.push(
      `| **${esc(r.model.label)}** ` +
      `| ${r.dims} ` +
      `| ${r.model.sizeMb} ` +
      `| ${r.chunksTotal} ` +
      `| ${r.ingestChunksPerSec.toFixed(1)} ` +
      `| ${ms1(r.speed.avg)} ` +
      `| ${ms1(r.speed.p50)} ` +
      `| ${ms1(r.speed.p95)} ` +
      `| ${q} |`
    );
  }
  lines.push('');

  // ── Per-query score comparison (semantic) ─────────────────────────────────
  if (runs.some((r) => r.semanticResults.length > 0)) {
    lines.push('## Per-Query Score Comparison (Semantic)');
    lines.push('');
    lines.push('Top-1 cosine similarity per model per query. Higher = better match.');
    lines.push('');
    const modelHeaders = runs.map((r) => r.model.label).join(' | ');
    lines.push(`| Query | ${modelHeaders} |`);
    lines.push(`| --- | ${'---: | '.repeat(runs.length)}`);

    const semanticQs = QUERIES.filter((q) => !q.edge);
    for (const q of semanticQs) {
      const cols = runs.map((r) => {
        const res = r.semanticResults.find((sr) => sr.label === q.label);
        const score = res?.hits[0]?.score;
        return score != null ? score.toFixed(3) : '—';
      }).join(' | ');
      lines.push(`| ${esc(q.label)} | ${cols} |`);
    }
    lines.push('');

    // Per-query speed comparison
    lines.push('## Per-Query Speed Comparison (ms)');
    lines.push('');
    lines.push(`| Query | ${modelHeaders} |`);
    lines.push(`| --- | ${'---: | '.repeat(runs.length)}`);
    for (const q of QUERIES) {
      const cols = runs.map((r) => {
        const res = [...r.semanticResults, ...r.edgeResults].find((sr) => sr.label === q.label);
        return res ? res.ms.toFixed(0) : '—';
      }).join(' | ');
      const tag = q.edge ? `*${esc(q.label)}*` : esc(q.label);
      lines.push(`| ${tag} | ${cols} |`);
    }
    lines.push('');
    lines.push('*Italic = edge-case query.*');
    lines.push('');
  }

  // ── Per-model detail sections ─────────────────────────────────────────────
  for (const r of runs) {
    lines.push(`## ${r.model.label}`);
    lines.push('');
    lines.push('| Key | Value |');
    lines.push('| --- | --- |');
    lines.push(`| Model path | \`${esc(r.actualModelPath)}\` |`);
    lines.push(`| Dims | ${r.dims} |`);
    lines.push(`| Size | ${r.model.sizeMb} MB |`);
    lines.push(`| Chunks indexed | ${r.chunksTotal} |`);
    lines.push(`| Ingest time | ${(r.ingestMs / 1000).toFixed(1)} s (${r.ingestChunksPerSec.toFixed(1)} chunks/s) |`);
    lines.push(`| Warm-up | ${Math.round(r.warmupMs)} ms (${WARMUP_COUNT} queries) |`);
    lines.push(`| Speed avg | **${ms1(r.speed.avg)} ms/query** (p50 ${ms1(r.speed.p50)}, p95 ${ms1(r.speed.p95)}) |`);
    lines.push(`| Semantic quality | ${r.quality != null ? `**${r.quality}%**` : '—'} |`);
    lines.push('');

    if (r.semanticResults.length) {
      lines.push('### Semantic Queries');
      lines.push('');
      for (const q of r.semanticResults) {
        const topText = q.hits.slice(0, 3)
          .map((h) => `${h.path ?? ''} ${h.title ?? ''} ${h.chunk ?? ''}`.toLowerCase()).join(' ');
        const verdict = q.expect?.length
          ? (() => {
              const m = q.expect.filter((kw) => topText.includes(kw.toLowerCase()));
              return m.length / q.expect.length >= 0.5 ? '✅' : '⚠️';
            })()
          : '';

        lines.push(`**${verdict} ${esc(q.label)}** · \`${esc(q.query.slice(0, 80))}\` · **${q.ms.toFixed(0)} ms**`);
        lines.push('');
        if (q.hits.length === 0) {
          lines.push('*(no results)*');
        } else {
          lines.push('| # | Score | Title | File |');
          lines.push('| ---: | --- | --- | --- |');
          for (let i = 0; i < q.hits.length; i++) {
            const h = q.hits[i];
            lines.push(`| ${i + 1} | ${scoreBar(h.score)} | ${esc(h.title || '—')} | \`${basename(h.path ?? '')}\` |`);
          }
        }
        lines.push('');
      }
    }

    if (r.edgeResults.length) {
      lines.push('### Edge Cases');
      lines.push('');
      for (const q of r.edgeResults) {
        const topScore = q.hits[0]?.score ?? 0;
        const lowOk = q.expectLowScore
          ? (topScore < 0.3 ? '✅ low score OK' : `⚠️ high score ${topScore.toFixed(3)}`)
          : `top: ${topScore.toFixed(3)}`;
        lines.push(`**${esc(q.label)}** · ${lowOk} · **${q.ms.toFixed(0)} ms**`);
        if (q.note) lines.push(`  *${esc(q.note)}*`);
        lines.push('');
        for (let i = 0; i < Math.min(2, q.hits.length); i++) {
          const h = q.hits[i];
          lines.push(`  ${i + 1}. ${scoreBar(h.score)} \`${basename(h.path ?? '')}\` — ${esc((h.chunk ?? '').slice(0, 70))}`);
        }
        lines.push('');
      }
    }
  }

  // ── Speed summary table ───────────────────────────────────────────────────
  lines.push('## Speed Summary');
  lines.push('');
  lines.push('Wall time per `vectorLocalSearch` call. Includes cold process spawn + embed + scan.');
  lines.push('');
  lines.push('| Model | min ms | avg ms | p50 ms | p95 ms | max ms | queries |');
  lines.push('| --- | ---: | ---: | ---: | ---: | ---: | ---: |');
  for (const r of runs) {
    const s = r.speed;
    const n = r.semanticResults.length + r.edgeResults.length;
    lines.push(`| ${esc(r.model.label)} | ${ms1(s.min)} | **${ms1(s.avg)}** | ${ms1(s.p50)} | ${ms1(s.p95)} | ${ms1(s.max)} | ${n} |`);
  }
  lines.push('');
  lines.push('> In-process cached scenario (no spawn): expected **< 10 ms/query** for ≤ 6000 chunks (brute-force 384-dim cosine).');
  lines.push('');

  lines.push('---');
  lines.push(`*Generated ${finishedAt} · Node ${process.version}*`);
  lines.push('');
  return lines.join('\n');
}

// ── main ──────────────────────────────────────────────────────────────────────

const argv         = process.argv.slice(2);
const skipIngest   = argv.includes('--skip-ingest');
const onlyFilter   = argv[argv.indexOf('--only') + 1] ?? null;
const modelsArg    = argv[argv.indexOf('--models') + 1] ?? 'all';

const collector  = createMetricsCollector();
const startedAt  = new Date().toISOString();

const models = discoverModels(modelsArg);

console.log('vector-store-local-bench (multi-model)');
console.log(`  exe      : ${EXE}`);
console.log(`  docs     : ${DOCS_DIR}`);
console.log(`  stores   : ${BENCH_STORE_ROOT}`);
console.log(`  models   : ${models.length}`);
for (const m of models) console.log(`    • ${m.label.padEnd(20)} ${m.path}`);

if (models.length === 0) {
  console.error('\nNo models found. Run first:');
  console.error('  npm run download:bge-models:win');
  console.error('  — or set PM_EMBED_MODEL=/path/to/model.gguf\n');
  process.exit(1);
}

mkdirSync(BENCH_STORE_ROOT, { recursive: true });

const mdFiles = collectMdFiles(DOCS_DIR);
console.log(`\n  docs: ${mdFiles.length} markdown files`);

// Run each model sequentially
const runs = [];
for (const model of models) {
  const result = await benchmarkModel(model, mdFiles, skipIngest, onlyFilter);
  runs.push(result);
}

// ── Write report ──────────────────────────────────────────────────────────────

console.log('\n[report] Writing …');

const finishedAt  = new Date().toISOString();
const metrics     = buildMetricsBundle(collector, startedAt, finishedAt);
const md          = buildReport(runs, mdFiles, metrics, startedAt, finishedAt);

mkdirSync(dirname(TEST_REPORT_PATH), { recursive: true });
writeFileSync(TEST_REPORT_PATH, md, 'utf8');
console.log(`  → ${TEST_REPORT_PATH}`);

const dedicatedPath = join(PACKAGE_ROOT, 'tests', 'test-report-vector-store-bench.md');
writeFileSync(dedicatedPath, md, 'utf8');
console.log(`  → ${dedicatedPath}`);

if (isAccumulateMode() && getChapterTitle()) {
  appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
  console.log(`  → ${TEST_REPORT_ALL_PATH} (chapter)`);
}

console.log('\nDone.\n');
