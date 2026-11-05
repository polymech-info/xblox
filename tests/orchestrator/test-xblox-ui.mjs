// test-xblox-ui.mjs — full win32 → WebView2 round trip for the xblox viewer.
//
// Launches the GUI exactly the way a user does:
//
//   tanit --ui-preset viewer --src packages/xblox/tests/xblox/language.xblox
//
// then drives/inspects it cross-process through the assistant CLI
// (`assistant app-inspect dump`, `assistant app-use batch`) — i.e. through
// Windows UI Automation, NOT through any in-process hook. This verifies the
// whole selector contract end to end:
//
//   React tree rows  →  HTML id/aria  →  Chromium UIA bridge  →  win32 UIA
//   →  app_inspect/app_use (AutomationId scoring, Value pattern)  →  this test
//
// Phases:
//   1. ready    — wait until an element with AutomationId "xblox-tree" exists
//                 for the UI pid (this is the "xblox viewer is ready" signal).
//   2. inspect  — assert block rows from language.xblox surface with stable
//                 AutomationIds (xblox:<path>), structured Names
//                 ("<kind>: <label>"), TreeItem control type, SelectionItem
//                 pattern, and visible children (xblox:2/consequent/0).
//   3. write    — UIA Value-pattern round trip: set-value on the inline
//                 condition editor (xblox:2:condition) and assert the ROW
//                 Name re-renders ("if: <new condition>") — proof the value
//                 went through React state, not just the DOM attribute.
//   4. input    — real mouse click on a row via click-element (selector →
//                 center coords → SendInput) and assert the row takes focus
//                 (roving tabindex). Disable with PM_XBLOX_UI_INPUT=0.
//
// Mode: launch-only, on the isolated tests/ui profile (--config-dir), like
// test-ui-stress launch mode. If tanit.exe is missing or the instance
// never registers (headless CI), the suite SKIPS unless PM_XBLOX_UI_REQUIRE=1.
//
// A UIA dump sample is saved to packages/xblox/tests/xblox-ui-dump-sample.json for selector
// debugging (same idea as tests/ui-stress-dump-sample.json).
//
// Run: npm run test:xblox-ui
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
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
const FIXTURE = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox', 'language.xblox');
const DUMP_SAMPLE = join(PACKAGE_ROOT, 'tests', 'xblox-ui-dump-sample.json');

// ── exe resolution (same convention as the other orchestrator suites) ────────
function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

function uiExePath() {
  const env = process.env.PM_IMAGE || process.env.MEDIA_IMG_UI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit.exe' : 'tanit';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

// ── report scaffolding ────────────────────────────────────────────────────────
function createTestReport() {
  return {
    meta: {}, notes: [], suites: [], _cur: null,
    beginSuite(name) { if (this._cur) this.endSuite(); this._cur = { name, steps: [], t0: performance.now() }; },
    endSuite() { if (!this._cur) return; this._cur.totalMs = performance.now() - this._cur.t0; this.suites.push(this._cur); this._cur = null; },
    step(label, ms, detail = '') { if (!this._cur) return; this._cur.steps.push({ label, ms: Math.round(ms * 100) / 100, detail: detail || '' }); },
    note(s) { this.notes.push(String(s)); },
    finalize(stats, wallMs, exe, extra = {}) {
      if (this._cur) this.endSuite();
      this.meta = {
        generatedAt: new Date().toISOString(), node: process.version, argv: process.argv.slice(2),
        exe, passed: stats.passed, failed: stats.failed, wallClockMs: Math.round(wallMs * 100) / 100, ...extra,
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
    ok, passed: rep.meta.passed, failed,
    abortReason: rep.meta.abortReason, uncaughtError: rep.meta.uncaughtError, error: rep.meta.uncaughtError,
    meta: {
      cwd: process.cwd(), displayName: 'xblox-ui', testName: 'xblox-ui', writtenAt: finishedAtIso,
      generatedAt: rep.meta.generatedAt, exe: rep.meta.exe,
      argv: Array.isArray(rep.meta.argv) ? rep.meta.argv.join(' ') : String(rep.meta.argv ?? ''),
      wallClockMs: rep.meta.wallClockMs,
    },
    metrics, images: [], mediaSuites: rep.suites, integrationNotes: rep.notes,
  });
  const acc = isAccumulateMode() && getChapterTitle();
  md += acc
    ? '\n---\n\n*Per-run: `tests/test-report-last.md` — this run is also a chapter in `tests/test-report-all.md`.*\n'
    : '\n---\n\n*Artifact: `tests/test-report-last.md` — overwritten on each test run.*\n';
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  if (acc) {
    appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
    console.log(`  (aggregated report chapter: ${TEST_REPORT_ALL_PATH})`);
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(fn, { timeoutMs = 8000, intervalMs = 200 } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() >= deadline) return null;
    await sleep(intervalMs);
  }
}

function parseJson(text) {
  const s = String(text || '').trim();
  if (!s) return null;
  try { return JSON.parse(s); } catch {
    const i = Math.min(...[s.indexOf('{'), s.indexOf('[')].filter((n) => n >= 0));
    if (Number.isFinite(i)) { try { return JSON.parse(s.slice(i)); } catch { /* fall */ } }
    return null;
  }
}

// ── CLI plumbing ──────────────────────────────────────────────────────────────
function baseArgs(configDir) {
  const a = ['--log-level', 'off'];
  if (configDir) a.push('--config-dir', configDir);
  return a;
}

function runCli(exe, argv, { configDir, timeout = 20_000 } = {}) {
  const t0 = performance.now();
  const r = spawnSync(exe, [...baseArgs(configDir), ...argv], { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout });
  return { status: r.status ?? -1, stdout: r.stdout || '', stderr: r.stderr || '', ms: performance.now() - t0 };
}

// The registry can hold stale entries from crashed runs (orphaned=true,
// "pid not running") — attaching to one of those was this suite's original
// failure mode, so live instances only.
function findUi(exe, configDir) {
  const r = runCli(exe, ['run-ipc', '--json', 'list'], { configDir });
  const arr = parseJson(r.stdout);
  return (Array.isArray(arr) ? arr : []).find((d) => d.kind === 'ui' && !d.orphaned) || null;
}

// `assistant app-inspect dump --pid <pid> --json` → { ok, windows:[{elements}] }.
function assistantDump(exe, pid) {
  const r = runCli(exe, ['assistant', 'app-inspect', 'dump', '--pid', String(pid), '--json', '--limit', '2000'], { timeout: 30_000 });
  return parseJson(r.stdout);
}

// Flatten every UIA element across the pid's windows.
function dumpElements(exe, pid) {
  const d = assistantDump(exe, pid);
  const out = [];
  for (const w of (d?.windows || [])) for (const e of (w.elements || [])) out.push(e);
  return out;
}

const byAid = (elements, aid) => elements.find((e) => e.automationId === aid) || null;

// Run a JSON batch program through `assistant app-use batch`.
function assistantBatch(exe, steps, scratchDir, { timeout = 90_000 } = {}) {
  const file = join(scratchDir, `batch-${Date.now()}-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(file, JSON.stringify({ steps }, null, 2), 'utf8');
  const r = runCli(exe, ['assistant', 'app-use', 'batch', '--file', file, '--json', '--continue-on-error'], { timeout });
  try { rmSync(file, { force: true }); } catch { /* */ }
  return parseJson(r.stdout);
}

// wait-element matches fuzzily (any non-empty AutomationId scores 10), so we
// require a contains-or-exact AutomationId match (≥300) and double-check the
// returned element. Returns the element JSON or null.
function batchWaitElement(exe, pid, automationId, scratchDir, { timeoutMs = 20_000, exact = true } = {}) {
  const rep = assistantBatch(exe, [{
    action: 'wait-element', pid, automation_id: automationId,
    minScore: 300, timeoutMs, intervalMs: 500,
  }], scratchDir, { timeout: timeoutMs + 30_000 });
  const el = rep?.steps?.[0]?.data?.element || null;
  if (!el) return null;
  if (exact && el.automationId !== automationId) return null;
  return el;
}

// ── main ─────────────────────────────────────────────────────────────────────
const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const rep = createTestReport();
const t0 = performance.now();
const exe = cliExePath();
const ui = uiExePath();

let launched = null;
let scratchDir = null;

try {
  if (!existsSync(exe)) throw new Error(`tanit-cli not found at ${exe}`);
  if (!existsSync(FIXTURE)) throw new Error(`fixture not found: ${FIXTURE}`);

  console.log('\n🪟 XBlox UI round trip (win32 → WebView2 → UIA)\n');
  console.log(`   cli: ${exe}`);
  console.log(`   ui:  ${ui}`);
  console.log(`   src: ${FIXTURE}\n`);

  scratchDir = mkdtempSync(join(tmpdir(), 'pm-xbloxui-'));
  const requireUi = process.env.PM_XBLOX_UI_REQUIRE === '1';
  const configDir = (process.env.PM_XBLOX_UI_CONFIG_DIR || '').trim() || join(PACKAGE_ROOT, 'tests', 'ui');
  mkdirSync(configDir, { recursive: true });

  // ── launch: the exact command line from the bug-report workflow ────────────
  rep.beginSuite('xblox-ui: launch viewer');
  let pid = null;
  let id = null;
  if (process.platform === 'win32' && existsSync(ui)) {
    const args = [...baseArgs(configDir), '--ui-preset', 'viewer', '--src', FIXTURE];
    console.log(`   launching: pm-image ${args.join(' ')}`);
    const child = spawn(ui, args, { cwd: PACKAGE_ROOT, stdio: 'ignore', detached: false });
    child.on('error', () => {});
    launched = { child };
    const found = await waitFor(() => findUi(exe, configDir), { timeoutMs: 30_000, intervalMs: 400 });
    id = found?.id || null;
    pid = found?.pid || null;
  }
  rep.step('launch', 0, pid ? `ui=${id} pid=${pid}` : 'no UI');
  rep.endSuite();

  if (!pid) {
    const note = existsSync(ui)
      ? 'launched tanit.exe but no `ui` instance registered within 30s (headless CI, or a single-instance UI absorbed the launch)'
      : 'tanit.exe not found in dist';
    console.log(`\n   ⏭  SKIP: ${note}\n`);
    rep.note('SKIPPED: ' + note);
    if (requireUi) stats.assert(false, 'xblox-ui: a UI was available (PM_XBLOX_UI_REQUIRE=1)');
    else stats.assert(true, `xblox-ui: skipped — ${note}`);
  } else {
    // ── phase 1: viewer ready ─────────────────────────────────────────────────
    // The xblox webview is "ready" exactly when the tree container's HTML id
    // surfaces as a UIA AutomationId for this pid. Cold start includes WebView2
    // env + navigation + React mount, so the window is generous.
    rep.beginSuite('xblox-ui: viewer ready (AutomationId xblox-tree)');
    {
      const tRdy = performance.now();
      const tree = await waitFor(
        () => batchWaitElement(exe, pid, 'xblox-tree', scratchDir, { timeoutMs: 15_000 }),
        { timeoutMs: 60_000, intervalMs: 250 },
      );
      const ms = performance.now() - tRdy;
      rep.step('wait xblox-tree', ms, tree ? `controlType=${tree.controlType}` : 'TIMEOUT');
      stats.assert(!!tree, `xblox-ui: viewer ready — AutomationId "xblox-tree" visible via UIA (${Math.round(ms)} ms)`);
      if (tree) stats.assert(/tree/i.test(tree.controlType || ''), `xblox-ui: container surfaces as a Tree control (got ${tree?.controlType})`);
    }
    rep.endSuite();

    // ── phase 2: structural inspection (one dump, assert in node) ────────────
    rep.beginSuite('xblox-ui: rows surface with stable selectors');
    {
      const tIns = performance.now();
      // Re-dump until the tree materialises. Chromium populates its UIA tree
      // lazily and incrementally after the first accessibility query, so an
      // early dump can contain a row but not yet its inline Edit child — gate
      // on both a row and an inline editor before asserting.
      let els = [];
      await waitFor(() => {
        els = dumpElements(exe, pid);
        return byAid(els, 'xblox:0') && byAid(els, 'xblox:2:condition');
      }, { timeoutMs: 30_000, intervalMs: 800 });
      try { writeFileSync(DUMP_SAMPLE, JSON.stringify(assistantDump(exe, pid), null, 2), 'utf8'); } catch { /* */ }
      rep.note('saved UIA dump sample → packages/xblox/tests/xblox-ui-dump-sample.json');

      const row0 = byAid(els, 'xblox:0');
      stats.assert(!!row0, 'xblox-ui: root row xblox:0 has a UIA AutomationId');
      if (row0) {
        stats.assert(/^stdout: /.test(row0.name || ''), `xblox-ui: row Name is structured "<kind>: <label>" (got "${row0.name}")`);
        stats.assert(/treeitem/i.test(row0.controlType || ''), `xblox-ui: row is a TreeItem (got ${row0.controlType})`);
        stats.assert(row0.patterns?.selectionItem === true, 'xblox-ui: row exposes the SelectionItem pattern');
      }

      const ifRow = byAid(els, 'xblox:2');
      stats.assert(!!ifRow && /^if: score >= 9/.test(ifRow.name || ''), `xblox-ui: if-row xblox:2 named from its condition (got "${ifRow?.name}")`);

      // Path "2" is in the builder's default open set → its consequent child
      // must already be a UIA element, proving hierarchy reaches win32.
      const child = byAid(els, 'xblox:2/consequent/0');
      stats.assert(!!child, 'xblox-ui: nested row xblox:2/consequent/0 reachable (children surface through UIA)');

      // Inline editor controls are individually addressable for set-value.
      const cond = byAid(els, 'xblox:2:condition');
      stats.assert(!!cond, 'xblox-ui: inline condition editor xblox:2:condition has an AutomationId');
      stats.assert(cond?.patterns?.value === true, 'xblox-ui: condition editor exposes the Value pattern');
      stats.assert((cond?.value || '') === 'score >= 9', `xblox-ui: condition editor Value readable (got "${cond?.value}")`);

      // Only rows inside the viewport survive the dump's offscreen filter
      // (IsOffscreen), so the count is bounded by window height, not by the
      // document — a dozen visible rows proves the tree rendered.
      const rowAids = els.filter((e) => /^xblox:\d/.test(e.automationId || '') && /treeitem/i.test(e.controlType || ''));
      rep.step('inspect', performance.now() - tIns, `${rowAids.length} visible tree rows with xblox:<path> ids`);
      stats.assert(rowAids.length >= 10, `xblox-ui: language.xblox renders a tree (${rowAids.length} visible addressable rows)`);
    }
    rep.endSuite();

    // ── phase 3: UIA Value-pattern WRITE round trip ───────────────────────────
    // set-value targets the input by exact AutomationId; Chromium routes the
    // Value pattern through the renderer as user input, so React state updates
    // and the ROW's accessible name (blockLabel = condition) must re-render.
    rep.beginSuite('xblox-ui: set-value round trip (UIA → React → UIA)');
    {
      const tSet = performance.now();
      const newCond = 'score >= 99';
      const setRep = assistantBatch(exe, [{
        action: 'set-value', pid, automationId: 'xblox:2:condition', value: newCond,
      }], scratchDir);
      const setOk = setRep?.steps?.[0]?.ok === true;
      stats.assert(setOk, `xblox-ui: set-value accepted (${setRep?.steps?.[0]?.error || 'ok'})`);

      let renamed = null;
      if (setOk) {
        renamed = await waitFor(() => {
          const el = batchWaitElement(exe, pid, 'xblox:2', scratchDir, { timeoutMs: 4_000 });
          return el && el.name === `if: ${newCond}` ? el : null;
        }, { timeoutMs: 15_000, intervalMs: 300 });
      }
      rep.step('set-value + rename', performance.now() - tSet, renamed ? `row name → "${renamed.name}"` : 'row name did not update');
      stats.assert(!!renamed, 'xblox-ui: row Name re-rendered from React state after UIA set-value');
    }
    rep.endSuite();

    // ── phase 4: real input — click a row, it takes focus (roving tabindex) ──
    if (process.env.PM_XBLOX_UI_INPUT === '0') {
      rep.note('input phase disabled (PM_XBLOX_UI_INPUT=0)');
    } else {
      rep.beginSuite('xblox-ui: click-element focuses the row');
      const tClk = performance.now();
      // stdout rows have no inline input at their center, so focus lands on the
      // row div itself (tabindex). find-app+activate first: click-element sends
      // REAL mouse input, the window must be foreground.
      const clickRep = assistantBatch(exe, [
        { action: 'find-app', pid },
        { action: 'activate', pid },
        { action: 'click-element', pid, automation_id: 'xblox:0', minScore: 300, timeoutMs: 10_000 },
      ], scratchDir);
      const clicked = clickRep?.steps?.[2]?.ok === true;
      stats.assert(clicked, `xblox-ui: click-element on xblox:0 (${clickRep?.steps?.[2]?.error || 'ok'})`);

      let focused = null;
      if (clicked) {
        focused = await waitFor(() => {
          const el = batchWaitElement(exe, pid, 'xblox:0', scratchDir, { timeoutMs: 4_000 });
          return el?.focused ? el : null;
        }, { timeoutMs: 10_000, intervalMs: 300 });
      }
      rep.step('click + focus', performance.now() - tClk, focused ? 'row focused' : 'row not focused');
      stats.assert(!!focused, 'xblox-ui: clicked row reports UIA focus (keyboard-focusable rows)');
      rep.endSuite();
    }
  }
} catch (e) {
  console.error(e);
  rep.meta.abortReason = 'uncaught exception';
  rep.meta.uncaughtError = e?.stack || String(e);
} finally {
  if (launched?.child) {
    try { launched.child.kill('SIGKILL'); } catch { /* */ }
    if (process.platform === 'win32' && launched.child.pid) {
      try { spawnSync('taskkill', ['/PID', String(launched.child.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { /* */ }
    }
  }
  if (scratchDir) { try { rmSync(scratchDir, { recursive: true, force: true }); } catch { /* */ } }

  rep.finalize(stats, performance.now() - t0, exe);
  writeTestReportFile(rep, metricsCollector, startedAtIso);

  console.log(`\n${stats.failed === 0 ? '✅' : '❌'} xblox-ui: ${stats.passed} passed, ${stats.failed} failed\n`);
  process.exit(stats.failed === 0 ? 0 : 1);
}
