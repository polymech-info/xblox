#!/usr/bin/env node
/**
 * XBlox CLI tests: block-tree execution and custom-command dispatch.
 *
 * Invoked via:
 *   npm run test:xblox
 */

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
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
const XBLOX_FIXTURE_DIR = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox');

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
      displayName: 'xblox',
      testName: 'xblox',
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

function writeBlocksFile(path, doc) {
  writeFileSync(path, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
}

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

function runXblox(exe, src, extraArgs = [], opts = {}) {
  const t0 = performance.now();
  const r = spawnSync(exe, [...(opts.globalArgs || []), 'xblox', '--log-level', 'off', 'run', '--json', '--no-wait', '--src', src, ...extraArgs], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
  });
  return {
    status: r.status ?? -1,
    stdout: r.stdout || '',
    stderr: r.stderr || '',
    ms: performance.now() - t0,
  };
}

function parseTrailingJson(stdout) {
  const text = String(stdout || '').trim();
  const starts = [text.lastIndexOf('\n{'), text.indexOf('{')].filter((n) => n >= 0);
  for (const start of starts) {
    try {
      return JSON.parse(text.slice(text[start] === '{' ? start : start + 1));
    } catch {
      /* try next candidate */
    }
  }
  return JSON.parse(text);
}

function commandEvents(report) {
  return (report.events || []).filter((event) => event.kind === 'command');
}

function actionEvents(report, action) {
  return commandEvents(report).filter((event) => event.data?.action === action);
}

function eventsByKind(report, kind) {
  return (report.events || []).filter((event) => event.kind === kind);
}

// Command blocks carry their payload inline (kind:"command" + command:{…});
// the legacy runScript/host.runCustomCommand script convention is deprecated
// (runScript degrades to a plain group and runs no script).
function flowBlocksFile() {
  return {
    version: 1,
    context: {
      selection: { files: ['demo/input.png'] },
      mode: 'capture',
      hasQueuedWork: true,
    },
    roots: [
      {
        kind: 'command',
        command: { id: 'metadata-noop', label: 'Metadata Noop' },
      },
      {
        kind: 'if',
        condition: 'selectionCount > 0',
        consequent: [
          {
            kind: 'command',
            command: { id: 'app-chat', label: 'Chat Noop', appCommand: 'chat' },
          },
        ],
      },
      {
        kind: 'else',
        items: [
          {
            kind: 'command',
            command: { id: 'bad-branch', label: 'Bad Branch', appCommand: 'bad' },
          },
        ],
      },
      {
        kind: 'if',
        condition: '1 == 2',
        consequent: [
          {
            kind: 'command',
            command: { id: 'bad-false', label: 'Bad False', appCommand: 'bad' },
          },
        ],
      },
      {
        kind: 'else',
        items: [
          {
            kind: 'command',
            command: { id: 'settings-ribbon', label: 'Settings Noop', ribbonCommand: 'settings' },
          },
        ],
      },
      {
        kind: 'switch',
        variable: 'mode',
        items: [
          {
            kind: 'case',
            comparator: '===',
            expression: "'capture'",
            consequent: [
              {
                kind: 'command',
                command: { id: 'capture-url', label: 'Capture Url Noop', url: 'https://polymech.info' },
              },
            ],
          },
          {
            kind: 'switchDefault',
            consequent: [
              {
                kind: 'command',
                command: { id: 'default-path', label: 'Default Path Noop', path: '.' },
              },
            ],
          },
        ],
      },
      {
        kind: 'while',
        condition: 'hasQueuedWork',
        loopLimit: 3,
        items: [
          { kind: 'wait', ms: 5 },
          {
            kind: 'command',
            command: { id: 'while-meta', label: 'While Metadata' },
          },
          { kind: 'break' },
        ],
      },
    ],
  };
}

function realCommandBlocksFile(workDir) {
  const isWin = process.platform === 'win32';
  const externalCommand = isWin
    ? { mode: 'argv', command: 'cmd.exe', args: ['/c', 'dir', '/b'], cwd: workDir }
    : { mode: 'argv', command: 'ls', args: ['-1'], cwd: workDir };
  return {
    version: 1,
    roots: [
      {
        kind: 'command',
        command: {
          id: 'list-dir',
          label: isWin ? 'Dir' : 'Ls',
          externalCommand,
        },
      },
    ],
  };
}

function expressionBlocksFile() {
  return {
    version: 1,
    context: {
      selection: { files: ['demo/input.png', 'demo/input-2.png'] },
      score: 7,
      modeNum: 2,
      hasQueuedWork: true,
    },
    roots: [
      {
        kind: 'if',
        condition: 'score * 2 >= 14',
        consequent: [
          {
            kind: 'command',
            command: { id: 'expr-if-true', label: 'Expression If', appCommand: 'chat' },
          },
        ],
      },
      {
        kind: 'else',
        items: [
          {
            kind: 'command',
            command: { id: 'expr-if-bad-alternate', label: 'Bad Alternate', appCommand: 'bad' },
          },
        ],
      },
      {
        kind: 'if',
        condition: 'selectionCount == 0',
        consequent: [
          {
            kind: 'command',
            command: { id: 'expr-selection-bad', label: 'Bad Selection', appCommand: 'bad' },
          },
        ],
      },
      {
        kind: 'elseIf',
        condition: 'selectionCount == 2',
        items: [
          {
            kind: 'command',
            command: { id: 'expr-elseif', label: 'Expression Else If', ribbonCommand: 'settings' },
          },
        ],
      },
      {
        kind: 'else',
        items: [
          {
            kind: 'command',
            command: { id: 'expr-alternate-bad', label: 'Bad Alternate', appCommand: 'bad' },
          },
        ],
      },
      {
        kind: 'switch',
        variable: 'modeNum + 1',
        items: [
          {
            kind: 'case',
            comparator: '===',
            expression: '2',
            consequent: [
              {
                kind: 'command',
                command: { id: 'expr-switch-bad', label: 'Bad Switch', appCommand: 'bad' },
              },
            ],
          },
          {
            kind: 'case',
            comparator: '>=',
            expression: '3',
            consequent: [
              {
                kind: 'command',
                command: { id: 'expr-switch-numeric', label: 'Expression Switch', url: 'https://polymech.info' },
              },
            ],
          },
          {
            kind: 'switchDefault',
            consequent: [
              {
                kind: 'command',
                command: { id: 'expr-switch-default-bad', label: 'Bad Default', path: '.' },
              },
            ],
          },
        ],
      },
      {
        kind: 'while',
        condition: 'hasQueuedWork * (score > 6)',
        loopLimit: 3,
        items: [
          {
            kind: 'command',
            command: { id: 'expr-while', label: 'Expression While' },
          },
          { kind: 'break' },
        ],
      },
      { kind: 'setVariable', name: 'userIntPlusScore', expression: '${USER:INT_VAR} + score' },
      { kind: 'setVariable', name: 'userStringMatch', expression: '"${USER:STRING_VAR}" == "string value"' },
      { kind: 'setVariable', name: 'userSecretMatch', expression: '"${USER:SECRET_VAR}" == "secret"' },
      { kind: 'setVariable', name: 'userBoolFalse', expression: '"${USER:BOOL_VAR}" == "false"' },
      {
        kind: 'if',
        condition: 'userIntPlusScore == 9',
        consequent: [
          {
            kind: 'command',
            command: { id: 'expr-user-int', label: 'User Int Expression', appCommand: 'chat' },
          },
        ],
      },
      {
        kind: 'if',
        condition: 'userStringMatch * userSecretMatch * userBoolFalse',
        consequent: [
          {
            kind: 'command',
            command: { id: 'expr-user-values', label: 'User Variable Expressions', appCommand: 'chat' },
          },
        ],
      },
    ],
  };
}

function contextBlocksFile() {
  return {
    version: 1,
    context: {
      seed: 3,
    },
    roots: [
      { kind: 'setVariable', name: 'score', expression: 'seed + 4' },
      { kind: 'getVariable', name: 'score', target: 'copiedScore' },
      { kind: 'setVariable', name: 'counter', value: 0 },
      {
        kind: 'for',
        variable: 'j',
        initial: '0',
        final: '3',
        comparator: '<',
        modifier: '+1',
        loopLimit: 10,
        items: [
          { kind: 'setVariable', name: 'counter', expression: 'counter + 1' },
        ],
      },
      { kind: 'getVariable', name: 'counter', target: 'copiedCounter' },
      { kind: 'log', level: 'info', message: 'copiedScore + copiedCounter' },
      {
        kind: 'if',
        condition: '(copiedScore == 7) * (copiedCounter == 3)',
        consequent: [
          {
            kind: 'command',
            command: { id: 'context-if', label: 'Context If', appCommand: 'chat' },
          },
        ],
      },
      {
        kind: 'else',
        items: [
          {
            kind: 'command',
            command: { id: 'context-bad-alternate', label: 'Bad Context', appCommand: 'bad' },
          },
        ],
      },
      { kind: 'setVariable', name: 'nested.total', expression: 'copiedScore * 2' },
      {
        kind: 'switch',
        variable: 'nested.total',
        items: [
          {
            kind: 'case',
            comparator: '>=',
            expression: '14',
            consequent: [
              {
                kind: 'command',
                command: { id: 'context-switch', label: 'Context Switch', url: 'https://polymech.info' },
              },
            ],
          },
          {
            kind: 'switchDefault',
            consequent: [
              {
                kind: 'command',
                command: { id: 'context-default-bad', label: 'Bad Default', path: '.' },
              },
            ],
          },
        ],
      },
    ],
  };
}

function runFlagsAbortBlocksFile() {
  return {
    version: 1,
    roots: [
      { kind: 'setVariable', value: 1 },
      { kind: 'setVariable', name: 'afterAbort', value: 1 },
    ],
  };
}

function runFlagsContinueBlocksFile() {
  return {
    version: 1,
    roots: [
      { kind: 'setVariable', value: 1, continueOnError: true },
      { kind: 'setVariable', name: 'afterContinue', value: 1 },
    ],
  };
}

function shellBlocksFile() {
  const isWin = process.platform === 'win32';
  return {
    version: 1,
    roots: [
      {
        kind: 'shell',
        shell: isWin ? 'pwsh' : 'sh',
        command: isWin ? 'Write-Output xblox-shell' : 'printf xblox-shell',
        timeoutMs: 30_000,
        storeAs: 'shellOut',
      },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
}

async function suiteXbloxExpressions(rep, assert, exe, workDir) {
  console.log('\n── XBlox: muParser expressions ──\n');
  rep.beginSuite('XBlox muParser expressions');

  const expressionsPath = join(workDir, 'expressions.blocks.json');
  const settingsPath = join(workDir, 'user-variables.settings.json');
  writeBlocksFile(expressionsPath, expressionBlocksFile());
  writeUserVariablesSettings(settingsPath);
  const r = runXblox(exe, expressionsPath, [], { globalArgs: ['--settings', settingsPath] });
  const report = parseTrailingJson(r.stdout);
  rep.step('xblox expression blocks', r.ms, `exit=${r.status}; events=${report.eventCount}`);

  assert(r.status === 0, `xblox expressions: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
  assert(report.ok === true, 'xblox expressions: report ok=true');
  assert(actionEvents(report, 'app').some((event) => event.data?.id === 'expr-if-true'), 'xblox expressions: arithmetic if condition runs consequent');
  assert(actionEvents(report, 'ribbon').some((event) => event.data?.id === 'expr-elseif'), 'xblox expressions: selectionCount else-if condition runs');
  assert(actionEvents(report, 'url').some((event) => event.data?.id === 'expr-switch-numeric'), 'xblox expressions: numeric switch comparison runs matching case');
  assert(actionEvents(report, 'metadata').some((event) => event.data?.id === 'expr-while'), 'xblox expressions: while condition runs metadata command');
  assert(actionEvents(report, 'app').some((event) => event.data?.id === 'expr-user-int'), 'xblox expressions: USER int resolves inside arithmetic condition');
  assert(actionEvents(report, 'app').some((event) => event.data?.id === 'expr-user-values'), 'xblox expressions: USER string/secret/bool values resolve inside expressions');
  assert(!commandEvents(report).some((event) => String(event.data?.id || '').includes('bad')), 'xblox expressions: non-matching branches do not run bad commands');
  assert((report.events || []).some((event) => event.kind === 'break' && event.status === 'ok'), 'xblox expressions: while reaches break');

  rep.endSuite();
}

async function suiteXbloxContext(rep, assert, exe, workDir) {
  console.log('\n── XBlox: context scope variables ──\n');
  rep.beginSuite('XBlox context scope variables');

  const contextPath = join(workDir, 'context.blocks.json');
  writeBlocksFile(contextPath, contextBlocksFile());
  const r = runXblox(exe, contextPath);
  const report = parseTrailingJson(r.stdout);
  rep.step('xblox context blocks', r.ms, `exit=${r.status}; events=${report.eventCount}`);

  const setEvents = (report.events || []).filter((event) => event.kind === 'setVariable');
  const getEvents = (report.events || []).filter((event) => event.kind === 'getVariable');
  assert(r.status === 0, `xblox context: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
  assert(report.ok === true, 'xblox context: report ok=true');
  assert(setEvents.some((event) => event.data?.name === 'score' && event.data?.value === 7), 'xblox context: setVariable writes evaluated score');
  assert(getEvents.some((event) => event.data?.name === 'score' && event.data?.target === 'copiedScore' && event.data?.value === 7), 'xblox context: getVariable reads and copies score');
  assert(getEvents.some((event) => event.data?.name === 'counter' && event.data?.target === 'copiedCounter' && event.data?.value === 3), 'xblox context: for loop updates counter three times');
  assert((report.events || []).some((event) => event.kind === 'log' && event.data?.level === 'info' && event.data?.message === '10'), 'xblox context: log block evaluates message expression');
  assert(actionEvents(report, 'app').some((event) => event.data?.id === 'context-if'), 'xblox context: copied variables are available to if condition');
  assert(actionEvents(report, 'url').some((event) => event.data?.id === 'context-switch'), 'xblox context: nested variable is available to switch condition');
  assert(!commandEvents(report).some((event) => String(event.data?.id || '').includes('bad')), 'xblox context: non-matching context branches do not run');

  rep.endSuite();
}

async function suiteXbloxRunFlags(rep, assert, exe, workDir) {
  console.log('\n── XBlox: run flags ──\n');
  rep.beginSuite('XBlox run flags');

  const abortPath = join(workDir, 'run-flags-abort.blocks.json');
  writeBlocksFile(abortPath, runFlagsAbortBlocksFile());
  {
    const r = runXblox(exe, abortPath);
    const report = parseTrailingJson(r.stdout);
    const setEvents = (report.events || []).filter((event) => event.kind === 'setVariable');
    rep.step('xblox run flags abort', r.ms, `exit=${r.status}; events=${report.eventCount}`);
    assert(r.status !== 0, `xblox run flags: abort case exits non-zero (got ${r.status})`);
    assert(report.ok === false, 'xblox run flags: abort case report ok=false');
    assert(setEvents.some((event) => event.status === 'error' && String(event.message || '').includes('missing name')), 'xblox run flags: failing block emits error');
    assert(!setEvents.some((event) => event.data?.name === 'afterAbort'), 'xblox run flags: default abort skips following sibling');
  }

  const continuePath = join(workDir, 'run-flags-continue.blocks.json');
  writeBlocksFile(continuePath, runFlagsContinueBlocksFile());
  {
    const r = runXblox(exe, continuePath);
    const report = parseTrailingJson(r.stdout);
    const setEvents = (report.events || []).filter((event) => event.kind === 'setVariable');
    rep.step('xblox run flags continue', r.ms, `exit=${r.status}; events=${report.eventCount}`);
    assert(r.status !== 0, `xblox run flags: continue case still reports failure (got ${r.status})`);
    assert(report.ok === false, 'xblox run flags: continue case report ok=false');
    assert(setEvents.some((event) => event.data?.name === 'afterContinue' && event.data?.value === 1), 'xblox run flags: continueOnError allows following sibling');
  }

  rep.endSuite();
}

async function suiteXbloxShell(rep, assert, exe, workDir) {
  console.log('\n── XBlox: shell blocks ──\n');
  rep.beginSuite('XBlox shell blocks');

  const shellPath = join(workDir, 'shell.blocks.json');
  writeBlocksFile(shellPath, shellBlocksFile());
  const r = runXblox(exe, shellPath);
  const report = parseTrailingJson(r.stdout);
  rep.step('xblox shell block', r.ms, `exit=${r.status}; events=${report.eventCount}`);

  const shellEvent = eventsByKind(report, 'shell').find((event) => event.status === 'ok');
  const logEvent = eventsByKind(report, 'log').at(-1);
  assert(r.status === 0, `xblox shell: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
  assert(report.ok === true, 'xblox shell: report ok=true');
  assert(shellEvent?.type === 'shell', 'xblox shell: emits shell typed event');
  assert(String(shellEvent?.data?.stdout || '').includes('xblox-shell'), 'xblox shell: captures stdout');
  assert(shellEvent?.data?.result === shellEvent?.data?.stdout, 'xblox shell: result mirrors stdout for storeAs');
  assert(String(logEvent?.message || '').includes('xblox-shell'), 'xblox shell: stdout flows into PREVIOUS');

  rep.endSuite();
}

async function suiteXbloxNetwork(rep, assert, exe) {
  console.log('\n── XBlox: network blocks ──\n');
  rep.beginSuite('XBlox network blocks');

  const networkPath = join(XBLOX_FIXTURE_DIR, 'network.xblox');
  const r = runXblox(exe, networkPath);
  const report = parseTrailingJson(r.stdout);
  rep.step('xblox network fixture', r.ms, `exit=${r.status}; events=${report.eventCount}`);

  const networkEvent = eventsByKind(report, 'network')[0];
  const parseEvent = eventsByKind(report, 'Parse').find((event) => event.status === 'ok');
  const logEvent = eventsByKind(report, 'log').at(-1);

  assert(r.status === 0, `xblox network: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
  assert(report.ok === true, 'xblox network: report ok=true');
  assert(networkEvent?.status === 'ok' && networkEvent.type === 'network', 'xblox network: fetch event succeeds with network type');
  assert(networkEvent?.data?.statusCode === 200, 'xblox network: fetch status code is 200');
  assert(typeof networkEvent?.data?.result === 'string' && networkEvent.data.result.includes('Cassandra - 65cm'), 'xblox network: response body contains Cassandra page');
  assert(parseEvent?.status === 'ok' && parseEvent.data?.result === 'Cassandra - 65cm', 'xblox network: Parse jq transforms PREVIOUS into page title');
  assert(!eventsByKind(report, 'setVariable').some((event) => event.data?.name === 'cassandra'), 'xblox network: generic storeAs replaces setVariable handoff');
  assert(logEvent?.message === 'Cassandra - 65cm', 'xblox network: log PREVIOUS prints parsed title');

  console.log(`\nnetwork log PREVIOUS:\n${logEvent.message}\n`);

  rep.endSuite();
}

async function suiteXbloxFs(rep, assert, exe, workDir) {
  console.log('\n── XBlox: filesystem blocks ──\n');
  rep.beginSuite('XBlox filesystem blocks');

  const tmpDir = join(workDir, 'fs-test');
  const txtFile = join(tmpDir, 'hello.txt');
  const jsonFile = join(tmpDir, 'data.json');
  const subDir = join(tmpDir, 'sub');

  // ── fsMkdir ──────────────────────────────────────────────────────────────
  const mkdirFlow = {
    version: 1,
    roots: [
      { kind: 'fsMkdir', path: tmpDir, storeAs: 'createdDir' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  {
    const p = join(workDir, 'fs-mkdir.json');
    writeBlocksFile(p, mkdirFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsMkdir', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMkdir: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsMkdir').find((e) => e.status === 'ok'), 'fsMkdir: event ok');
    assert(existsSync(tmpDir), 'fsMkdir: directory created on disk');
  }

  // ── fsMkdir: ${…} + existing + nested parents ────────────────────────────
  {
    const nested = join(workDir, 'fs-mkdir-var', 'child');
    const mkdirVarFlow = {
      version: 1,
      context: { mkdirDst: nested },
      roots: [
        { kind: 'fsMkdir', path: '${mkdirDst}', storeAs: 'nestedDir' },
        { kind: 'fsMkdir', path: '${mkdirDst}' },
        { kind: 'fsMkdir', path: '${mkdirDst}', dryRun: true },
      ],
    };
    const p = join(workDir, 'fs-mkdir-var.json');
    writeBlocksFile(p, mkdirVarFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsMkdir variables', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMkdir variables: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    const evs = eventsByKind(report, 'fsMkdir').filter((e) => e.status === 'ok');
    assert(evs.length === 3, `fsMkdir variables: 3 ok events (got ${evs.length})`);
    assert(existsSync(nested), 'fsMkdir variables: nested directory created');
    const destNorm = String(evs[0]?.data?.path || evs[0]?.data?.result || '').replace(/\\/g, '/');
    const expectNorm = nested.replace(/\\/g, '/');
    assert(
      destNorm.toLowerCase() === expectNorm.toLowerCase()
        || destNorm.toLowerCase().endsWith('fs-mkdir-var/child'),
      `fsMkdir variables: dest resolved (got "${destNorm}")`,
    );
  }

  // ── fsWrite ──────────────────────────────────────────────────────────────
  const writeFlow = {
    version: 1,
    roots: [
      { kind: 'fsWrite', path: txtFile, content: 'xblox-fs-hello', storeAs: 'writtenPath' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  {
    const p = join(workDir, 'fs-write.json');
    writeBlocksFile(p, writeFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsWrite', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsWrite: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsWrite').find((e) => e.status === 'ok'), 'fsWrite: event ok');
    assert(existsSync(txtFile), 'fsWrite: file exists on disk');
    assert(readFileSync(txtFile, 'utf8') === 'xblox-fs-hello', 'fsWrite: file content matches');
  }

  // ── fsRead ───────────────────────────────────────────────────────────────
  const readFlow = {
    version: 1,
    roots: [
      { kind: 'fsRead', path: txtFile, encoding: 'text', storeAs: 'fileText' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  {
    const p = join(workDir, 'fs-read.json');
    writeBlocksFile(p, readFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsRead', r.ms, `exit=${r.status}`);
    const readEvt = eventsByKind(report, 'fsRead').find((e) => e.status === 'ok');
    assert(r.status === 0, `fsRead: exits 0 (got ${r.status})`);
    assert(readEvt, 'fsRead: event ok');
    assert(readEvt?.data?.result === 'xblox-fs-hello', 'fsRead: result matches written content');
    assert(eventsByKind(report, 'log').find((e) => e.status === 'ok')?.message === 'xblox-fs-hello', 'fsRead: flows into PREVIOUS → log');
  }

  // ── fsHash ───────────────────────────────────────────────────────────────
  const hashFlow = {
    version: 1,
    roots: [
      { kind: 'fsHash', path: txtFile, storeAs: 'fileHash' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
      { kind: 'fsHash', path: txtFile, prefixLength: 12, storeAs: 'fileHashShort' },
    ],
  };
  {
    const expected = createHash('sha256').update('xblox-fs-hello').digest('hex');
    const p = join(workDir, 'fs-hash.json');
    writeBlocksFile(p, hashFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsHash', r.ms, `exit=${r.status}`);
    const hashEvents = eventsByKind(report, 'fsHash').filter((e) => e.status === 'ok');
    const hashEvt = hashEvents[0];
    const shortHashEvt = hashEvents[1];
    assert(r.status === 0, `fsHash: exits 0 (got ${r.status})`);
    assert(hashEvt, 'fsHash: event ok');
    assert(hashEvt?.data?.result === expected, 'fsHash: digest matches Node sha256');
    assert(shortHashEvt?.data?.result === expected.slice(0, 12), 'fsHash: prefixLength returns digest prefix');
    assert(shortHashEvt?.data?.sha256 === expected, 'fsHash: full sha256 remains available in event metadata');
    assert(eventsByKind(report, 'log').find((e) => e.status === 'ok')?.message === expected, 'fsHash: flows into PREVIOUS → log');
  }

  // ── fsRead JSON ──────────────────────────────────────────────────────────
  writeFileSync(jsonFile, JSON.stringify({ score: 42, tag: 'xblox' }), 'utf8');
  const readJsonFlow = {
    version: 1,
    roots: [
      { kind: 'fsRead', path: jsonFile, encoding: 'json', storeAs: 'parsed' },
    ],
  };
  {
    const p = join(workDir, 'fs-read-json.json');
    writeBlocksFile(p, readJsonFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsRead json', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsRead json: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsRead').find((e) => e.status === 'ok')?.data?.result?.score === 42, 'fsRead json: parses JSON and exposes fields');
  }

  // ── fsExists ─────────────────────────────────────────────────────────────
  const existsFlow = {
    version: 1,
    roots: [
      { kind: 'fsExists', path: txtFile, storeAs: 'exists' },
    ],
  };
  {
    const p = join(workDir, 'fs-exists.json');
    writeBlocksFile(p, existsFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsExists', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsExists: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsExists').find((e) => e.status === 'ok')?.data?.exists === true, 'fsExists: returns true for existing file');
  }

  // ── fsList ───────────────────────────────────────────────────────────────
  const listFlow = {
    version: 1,
    roots: [
      { kind: 'fsList', path: tmpDir, storeAs: 'entries' },
    ],
  };
  {
    const p = join(workDir, 'fs-list.json');
    writeBlocksFile(p, listFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsList', r.ms, `exit=${r.status}`);
    const listEvt = eventsByKind(report, 'fsList').find((e) => e.status === 'ok');
    assert(r.status === 0, `fsList: exits 0 (got ${r.status})`);
    assert(listEvt, 'fsList: event ok');
    const entries = listEvt?.data?.result || [];
    assert(Array.isArray(entries), 'fsList: result is array');
    assert(entries.some((e) => e.name === 'hello.txt'), 'fsList: hello.txt appears in listing');
  }

  // ── fsWrite append ───────────────────────────────────────────────────────
  const appendFlow = {
    version: 1,
    roots: [
      { kind: 'fsWrite', path: txtFile, content: '\nappended', append: true },
    ],
  };
  {
    const p = join(workDir, 'fs-append.json');
    writeBlocksFile(p, appendFlow);
    const r = runXblox(exe, p);
    rep.step('fsWrite append', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsWrite append: exits 0 (got ${r.status})`);
    assert(readFileSync(txtFile, 'utf8').includes('appended'), 'fsWrite append: content appended on disk');
  }

  // ── fsDelete ─────────────────────────────────────────────────────────────
  const deleteFlow = {
    version: 1,
    roots: [
      { kind: 'fsDelete', path: txtFile, storeAs: 'deleted' },
    ],
  };
  {
    const p = join(workDir, 'fs-delete.json');
    writeBlocksFile(p, deleteFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsDelete', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsDelete: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsDelete').find((e) => e.status === 'ok')?.data?.deleted === true, 'fsDelete: deleted=true in event');
    assert(!existsSync(txtFile), 'fsDelete: file removed from disk');
  }

  // ── PREV chain: write → read via PREVIOUS ────────────────────────────────
  const prevChainFlow = {
    version: 1,
    roots: [
      { kind: 'fsWrite', path: join(tmpDir, 'chain.txt'), content: 'chained', storeAs: '' },
      // storeAs empty → path goes to PREVIOUS; fsRead picks it up
      { kind: 'fsRead', storeAs: 'chainContent' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  {
    const p = join(workDir, 'fs-prev-chain.json');
    writeBlocksFile(p, prevChainFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fs PREVIOUS chain', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fs PREVIOUS chain: exits 0 (got ${r.status})`);
    const logEvt = eventsByKind(report, 'log').find((e) => e.status === 'ok' && e.message === 'chained');
    assert(logEvt, 'fs PREVIOUS chain: content flows write→read→log via PREVIOUS');
  }

  // ── Variable interpolation: ${CWD} and ${YYYY} in path/content ───────────
  {
    const varFlow = {
      version: 1,
      roots: [
        // Write a file whose path uses ${CWD} and whose content uses ${YYYY}
        { kind: 'fsWrite', path: `${tmpDir}/var-test.txt`, content: 'year=${YYYY}', storeAs: '' },
        { kind: 'fsRead',  path: `${tmpDir}/var-test.txt`, storeAs: 'varContent' },
        { kind: 'log',     level: 'info', message: 'PREVIOUS' },
      ],
    };
    const p = join(workDir, 'fs-vars.json');
    writeBlocksFile(p, varFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fs variable interpolation', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fs variable interpolation: exits 0 (got ${r.status})`);
    const content = eventsByKind(report, 'fsRead').find((e) => e.status === 'ok')?.data?.result || '';
    // ${YYYY} should be resolved to a 4-digit year
    assert(/^year=\d{4}$/.test(String(content)), `fs variable interpolation: \${YYYY} expanded in content (got "${content}")`);
  }

  // ── fsCopy: src/dst ${…} via transfer lib (dry-run) ─────────────────────
  {
    const copySrc = join(tmpDir, 'copy-src.txt');
    const copyDest = join(tmpDir, 'copy-out');
    writeFileSync(copySrc, 'copy-me', 'utf8');
    mkdirSync(copyDest, { recursive: true });
    const copyFlow = {
      version: 1,
      context: { copySrc, copyDst: copyDest },
      roots: [
        { kind: 'fsCopy', src: '${copySrc}', dst: '${copyDst}', dryRun: true, storeAs: 'copiedDest' },
      ],
    };
    const p = join(workDir, 'fs-copy-vars.json');
    writeBlocksFile(p, copyFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsCopy variables', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsCopy variables: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    const ev = eventsByKind(report, 'fsCopy').find((e) => e.status === 'ok');
    assert(ev, 'fsCopy variables: event ok');
    const destNorm = String(ev?.data?.dest || ev?.data?.result || '').replace(/\\/g, '/');
    const expectNorm = copyDest.replace(/\\/g, '/');
    assert(
      destNorm.toLowerCase() === expectNorm.toLowerCase()
        || destNorm.toLowerCase().endsWith(expectNorm.split('/').pop().toLowerCase()),
      `fsCopy variables: dest resolved (got "${destNorm}")`,
    );
  }

  // ── fsMove: copy then delete src ─────────────────────────────────────────
  {
    const moveSrc = join(tmpDir, 'move-src.txt');
    const moveDest = join(tmpDir, 'move-out');
    writeFileSync(moveSrc, 'move-me', 'utf8');
    mkdirSync(moveDest, { recursive: true });
    const moveFlow = {
      version: 1,
      context: { moveSrc, moveDst: moveDest },
      roots: [
        { kind: 'fsMove', src: '${moveSrc}', dst: '${moveDst}' },
      ],
    };
    const p = join(workDir, 'fs-move.json');
    writeBlocksFile(p, moveFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsMove', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMove: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    assert(eventsByKind(report, 'fsMove').find((e) => e.status === 'ok'), 'fsMove: event ok');
    assert(existsSync(join(moveDest, 'move-src.txt')), 'fsMove: dest file exists');
    assert(readFileSync(join(moveDest, 'move-src.txt'), 'utf8') === 'move-me', 'fsMove: dest contents');
    assert(!existsSync(moveSrc), 'fsMove: source removed');
  }

  // ── fsMove overwrite / skip / leftover include ───────────────────────────
  {
    const owSrc = join(tmpDir, 'ow-src.txt');
    const owDest = join(tmpDir, 'ow-out');
    writeFileSync(owSrc, 'ow-new', 'utf8');
    mkdirSync(owDest, { recursive: true });
    writeFileSync(join(owDest, 'ow-src.txt'), 'ow-old', 'utf8');
    const owFlow = {
      version: 1,
      context: { owSrc, owDst: owDest },
      roots: [
        { kind: 'fsMove', src: '${owSrc}', dst: '${owDst}', conflict: 'overwrite' },
      ],
    };
    const p = join(workDir, 'fs-move-overwrite.json');
    writeBlocksFile(p, owFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsMove overwrite', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMove overwrite: exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    assert(readFileSync(join(owDest, 'ow-src.txt'), 'utf8') === 'ow-new', 'fsMove overwrite: dest replaced');
    assert(!existsSync(owSrc), 'fsMove overwrite: source removed');
  }

  {
    const skipSrc = join(tmpDir, 'skip-src.txt');
    const skipDest = join(tmpDir, 'skip-out');
    writeFileSync(skipSrc, 'skip-new', 'utf8');
    mkdirSync(skipDest, { recursive: true });
    writeFileSync(join(skipDest, 'skip-src.txt'), 'skip-old', 'utf8');
    const skipFlow = {
      version: 1,
      context: { skipSrc, skipDst: skipDest },
      roots: [
        { kind: 'fsMove', src: '${skipSrc}', dst: '${skipDst}', conflict: 'skip' },
      ],
    };
    const p = join(workDir, 'fs-move-skip.json');
    writeBlocksFile(p, skipFlow);
    const r = runXblox(exe, p);
    const report = parseTrailingJson(r.stdout);
    rep.step('fsMove skip', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMove skip: exits 0 (got ${r.status})`);
    assert(eventsByKind(report, 'fsMove').find((e) => e.status === 'ok'), 'fsMove skip: event ok');
    assert(readFileSync(join(skipDest, 'skip-src.txt'), 'utf8') === 'skip-old', 'fsMove skip: dest unchanged');
    assert(existsSync(skipSrc), 'fsMove skip: source left');
  }

  {
    const fold = join(tmpDir, 'fs-move-filter');
    const foldDest = join(tmpDir, 'fs-move-filter-out');
    mkdirSync(fold, { recursive: true });
    mkdirSync(foldDest, { recursive: true });
    writeFileSync(join(fold, 'take.txt'), 'yes', 'utf8');
    writeFileSync(join(fold, 'keep.log'), 'no', 'utf8');
    const filterFlow = {
      version: 1,
      context: { foldSrc: fold, foldDst: foldDest },
      roots: [
        { kind: 'fsMove', src: '${foldSrc}', dst: '${foldDst}', include: '*.txt' },
      ],
    };
    const p = join(workDir, 'fs-move-include.json');
    writeBlocksFile(p, filterFlow);
    const r = runXblox(exe, p);
    rep.step('fsMove include leftover', r.ms, `exit=${r.status}`);
    assert(r.status === 0, `fsMove include leftover: exits 0 (got ${r.status})`);
    assert(existsSync(join(foldDest, 'fs-move-filter', 'take.txt')), 'fsMove include: dest has take.txt');
    assert(!existsSync(join(foldDest, 'fs-move-filter', 'keep.log')), 'fsMove include: dest has no keep.log');
    assert(!existsSync(join(fold, 'take.txt')), 'fsMove include: matched source gone');
    assert(existsSync(join(fold, 'keep.log')), 'fsMove include: unmatched source left');
    assert(existsSync(fold), 'fsMove include: leftover source dir stays');
  }

  rep.endSuite();
}

async function suiteXbloxAudio(rep, assert, exe) {
  console.log('\n── XBlox: audio blocks ──\n');
  rep.beginSuite('XBlox audio blocks');

  // audioListDevices — expect at least one mic.
  const listFlow = {
    version: 1,
    roots: [
      { kind: 'audioListDevices', storeAs: 'mics' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  const tmpFile = join(tmpdir(), `xblox-audio-list-${Date.now()}.json`);
  writeBlocksFile(tmpFile, listFlow);
  const r = runXblox(exe, tmpFile);
  const report = parseTrailingJson(r.stdout);
  rep.step('audioListDevices', r.ms, `exit=${r.status}`);

  try { rmSync(tmpFile); } catch { /* */ }

  const evt = eventsByKind(report, 'audioListDevices').find((e) => e.status === 'ok');

  if (evt === undefined) {
    // audioListDevices not compiled in (FEATURE_STT off) — skip gracefully.
    console.log('  audioListDevices not available in this build — skipping audio suite');
    rep.endSuite();
    return;
  }

  assert(r.status === 0, `audioListDevices: exits 0 (got ${r.status})`);
  assert(evt.status === 'ok', 'audioListDevices: event ok');
  const mics = evt.data?.result || [];
  assert(Array.isArray(mics), 'audioListDevices: result is array');
  assert(mics.length >= 1, `audioListDevices: at least one microphone found (got ${mics.length})`);
  console.log(`  microphones found: ${mics.map((m) => m.name).join(', ')}`);

  rep.endSuite();
}

async function suiteXbloxVideo(rep, assert, exe) {
  console.log('\n── XBlox: video blocks ──\n');
  rep.beginSuite('XBlox video blocks');

  // videoListDevices — webcam not connected, expect 0 or more devices (no assert on count).
  const listFlow = {
    version: 1,
    roots: [
      { kind: 'videoListDevices', storeAs: 'cameras' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  const tmpFile = join(tmpdir(), `xblox-video-list-${Date.now()}.json`);
  writeBlocksFile(tmpFile, listFlow);
  const r = runXblox(exe, tmpFile);
  const report = parseTrailingJson(r.stdout);
  rep.step('videoListDevices', r.ms, `exit=${r.status}`);

  try { rmSync(tmpFile); } catch { /* */ }

  const evt = eventsByKind(report, 'videoListDevices').find((e) => e.status === 'ok');

  if (evt === undefined) {
    console.log('  videoListDevices not available in this build (FEATURE_VIDEO off) — skipping video suite');
    rep.endSuite();
    return;
  }

  assert(r.status === 0, `videoListDevices: exits 0 (got ${r.status})`);
  assert(evt.status === 'ok', 'videoListDevices: event ok');
  const cameras = evt.data?.result || [];
  assert(Array.isArray(cameras), 'videoListDevices: result is array');

  if (cameras.length === 0) {
    console.log('  no camera connected — videoCapture smoke test skipped');
    rep.endSuite();
    return;
  }

  console.log(`  cameras found: ${cameras.map((c) => c.name).join(', ')}`);

  // If a camera exists, attempt a capture (soft failure — webcam may be busy).
  const captureFlow = {
    version: 1,
    roots: [
      { kind: 'videoCapture', device: '', timeoutMs: 6000, storeAs: 'frame' },
      { kind: 'log', level: 'info', message: 'PREVIOUS' },
    ],
  };
  const capFile = join(tmpdir(), `xblox-video-cap-${Date.now()}.json`);
  writeBlocksFile(capFile, captureFlow);
  const rc = runXblox(exe, capFile);
  const rc_report = parseTrailingJson(rc.stdout);
  rep.step('videoCapture', rc.ms, `exit=${rc.status}`);
  try { rmSync(capFile); } catch { /* */ }

  const capEvt = eventsByKind(rc_report, 'videoCapture').find((e) => e.status === 'ok' || e.status === 'error');
  if (capEvt?.status === 'ok') {
    console.log(`  captured frame: ${capEvt.data?.path}`);
    assert(typeof capEvt.data?.path === 'string' && capEvt.data.path.length > 0,
           'videoCapture: path is a non-empty string');
  } else {
    console.log(`  videoCapture soft failure (device busy?): ${capEvt?.message}`);
  }

  rep.endSuite();
}

async function suiteXblox(rep, assert, exe, workDir) {
  console.log('\n── XBlox: blocks + command execution ──\n');
  rep.beginSuite('XBlox block flow and commands');

  const flowPath = join(workDir, 'flow.blocks.json');
  writeBlocksFile(flowPath, flowBlocksFile());
  {
    const r = runXblox(exe, flowPath);
    const report = parseTrailingJson(r.stdout);
    rep.step('xblox flow blocks', r.ms, `exit=${r.status}; events=${report.eventCount}`);
    assert(r.status === 0, `xblox: flow exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    assert(report.ok === true, 'xblox: flow report ok=true');
    assert(actionEvents(report, 'metadata').length >= 2, 'xblox: metadata noop commands are reported');
    assert(actionEvents(report, 'app').length === 1, 'xblox: true if branch runs app noop once');
    assert(actionEvents(report, 'ribbon').length === 1, 'xblox: false if branch runs alternate ribbon noop');
    assert(actionEvents(report, 'url').length === 1, 'xblox: switch capture case runs url noop');
    assert(actionEvents(report, 'path').length === 0, 'xblox: switch default does not run for capture mode');
    assert((report.events || []).some((event) => event.kind === 'break' && event.status === 'ok'), 'xblox: while block reaches break');
  }

  const realPath = join(workDir, 'real.blocks.json');
  const markerFile = join(workDir, 'xblox-real-file.txt');
  writeFileSync(markerFile, 'listed-by-xblox\n', 'utf8');
  writeBlocksFile(realPath, realCommandBlocksFile(workDir));
  {
    const r = runXblox(exe, realPath);
    const report = parseTrailingJson(r.stdout);
    rep.step('xblox real external dir/ls', r.ms, `exit=${r.status}; stdout=${r.stdout.trim().slice(0, 160)}`);
    assert(r.status === 0, `xblox: real external command exits 0 (got ${r.status}); stderr: ${r.stderr.slice(0, 300)}`);
    assert(r.stdout.includes('xblox-real-file.txt'), 'xblox: dir/ls external command lists fixture file');
    assert(report.ok === true, 'xblox: real external command report ok=true');
    const externalEvents = actionEvents(report, 'external');
    const externalEvent = externalEvents[0];
    assert(externalEvents.length === 1, 'xblox: real external command event is external');
    assert(externalEvent?.type === 'command' && externalEvent?.errorCode === undefined, 'xblox: command event has structured type/error fields');
    assert(Array.isArray(externalEvent?.stdout) && externalEvent.stdout.includes('xblox-real-file.txt'), 'xblox: command stdout is captured as lines');
    assert(Array.isArray(externalEvent?.stderr), 'xblox: command stderr is captured as lines');
  }

  rep.endSuite();
}

const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const rep = createTestReport();
const t0 = performance.now();
const exe = cliExePath();
const workDir = mkdtempSync(join(tmpdir(), 'pm-xblox-test-'));
const expressionsOnly = process.argv.includes('--expressions-only');
const contextOnly = process.argv.includes('--context-only');
const networkOnly = process.argv.includes('--network-only');
const fsOnly = process.argv.includes('--fs-only');
const audioOnly = process.argv.includes('--audio-only');
const videoOnly = process.argv.includes('--video-only');

try {
  mkdirSync(workDir, { recursive: true });
  if (expressionsOnly) {
    await suiteXbloxExpressions(rep, stats.assert, exe, workDir);
  } else if (contextOnly) {
    await suiteXbloxContext(rep, stats.assert, exe, workDir);
  } else if (networkOnly) {
    await suiteXbloxNetwork(rep, stats.assert, exe);
  } else if (fsOnly) {
    await suiteXbloxFs(rep, stats.assert, exe, workDir);
  } else if (audioOnly) {
    await suiteXbloxAudio(rep, stats.assert, exe);
  } else if (videoOnly) {
    await suiteXbloxVideo(rep, stats.assert, exe);
  } else {
    await suiteXblox(rep, stats.assert, exe, workDir);
    await suiteXbloxExpressions(rep, stats.assert, exe, workDir);
    await suiteXbloxContext(rep, stats.assert, exe, workDir);
    await suiteXbloxRunFlags(rep, stats.assert, exe, workDir);
    await suiteXbloxShell(rep, stats.assert, exe, workDir);
    await suiteXbloxFs(rep, stats.assert, exe, workDir);
    await suiteXbloxAudio(rep, stats.assert, exe);
    await suiteXbloxVideo(rep, stats.assert, exe);
  }
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

if (stats.failed > 0 || rep.meta.abortReason) {
  process.exitCode = 1;
}
