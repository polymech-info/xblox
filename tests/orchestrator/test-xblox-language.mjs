#!/usr/bin/env node
/**
 * XBlox language (control-flow) tests.
 *
 * Companion to docs/xblox/language.md. CLI-first: each fixture is a temp
 * .xblox file run through `tanit-cli xblox run`; branch decisions are
 * observed via MARK:<name> stdout lines.
 *
 * Covers the sibling-chain model (D1), movable case containers (D2), and
 * break scoping (D3):
 *   - if / elseIf / else as sibling containers, first match wins
 *   - orphan elseIf / else / case are warnings, never fatal
 *   - switch case/default matching, no fallthrough
 *   - break exits the nearest loop only; execution continues after it
 *
 *   npm run test:xblox-language
 *
 * Live service-pages share fixture (requires SERVER_URL + login):
 *   SKIP_SERVICE_TEST=1 skips service-pages share only.
 *   Uses stable slug share-smoke (filename); second run asserts create upsert + if-newer skip.
 */
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');
const SERVICE_PAGES_XBLOX = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox', 'service-pages.xblox');
const SHARE_PAGE_SLUG = 'share-smoke';
const SHARE_SMOKE_MD = join(PACKAGE_ROOT, 'tests', 'service', 'share-smoke.md');
const SHARE_SMOKE_PNG = join(PACKAGE_ROOT, 'tests', 'service', 'share-smoke.png');
const BUILD_FIXTURES = join(PACKAGE_ROOT, 'tests', 'assets', 'build-fixtures.mjs');
// `xblox info --json` is now ~1.7MB (v2 manifests). Node spawnSync defaults to 1MB.
const CLI_MAX_BUFFER = 16 * 1024 * 1024;

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

const stdout = (mark) => ({ kind: 'stdout', message: `MARK:${mark}` });

function countMark(output, mark) {
  // Exact-token match: "MARK:ELSE" must not count "MARK:ELSEIF-B".
  const re = new RegExp(`MARK:${mark.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?![\\w-])`);
  return output.split('\n').filter((line) => re.test(line)).length;
}

/** Run a blocks-file document; returns { out, code }. */
function runDoc(exe, work, name, doc) {
  const src = join(work, `${name}.xblox`);
  writeFileSync(src, JSON.stringify({ version: 1, context: {}, ...doc }, null, 2));
  const r = spawnSync(exe, ['--log-level', 'warn', 'xblox', 'run', '--src', src], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 30_000,
    maxBuffer: CLI_MAX_BUFFER,
  });
  return { out: `${r.stdout ?? ''}\n${r.stderr ?? ''}`, code: r.status };
}

function parseTrailingJson(stdout) {
  const text = String(stdout || '').trim();
  const candidates = [text.lastIndexOf('\n{'), text.indexOf('{')].filter((n) => n >= 0);
  for (const start of candidates) {
    try { return JSON.parse(text.slice(text[start] === '{' ? start : start + 1)); } catch { /* next */ }
  }
  try { return JSON.parse(text); } catch { return { events: [], ok: false }; }
}

function runXbloxSrc(exe, src, opts = {}) {
  const args = ['--log-level', opts.logLevel ?? 'warn', 'xblox', 'run', '--src', src];
  if (opts.json) args.push('--json', '--no-wait');
  const r = spawnSync(exe, args, {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: opts.timeout ?? 120_000,
    maxBuffer: CLI_MAX_BUFFER,
    env: { ...process.env, ...(opts.env ?? {}) },
  });
  return {
    code: r.status ?? -1,
    out: `${r.stdout ?? ''}\n${r.stderr ?? ''}`,
    report: parseTrailingJson(r.stdout || ''),
  };
}

function ensureShareSmokeFixtures() {
  if (existsSync(SHARE_SMOKE_MD) && existsSync(SHARE_SMOKE_PNG)) return true;
  const r = spawnSync(process.execPath, [BUILD_FIXTURES], {
    encoding: 'utf8',
    cwd: PACKAGE_ROOT,
    stdio: 'inherit',
  });
  return r.status === 0 && existsSync(SHARE_SMOKE_MD) && existsSync(SHARE_SMOKE_PNG);
}

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

function manifestTests(exe) {
  const r = spawnSync(exe, ['--log-level', 'off', 'xblox', 'info', '--json'], {
    cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER,
  });
  let info = {};
  try { info = JSON.parse(r.stdout || '{}'); } catch { /* asserted below */ }
  const kinds = new Set(Array.isArray(info.blocks) ? info.blocks.map((b) => b.kind) : []);
  for (const kind of ['if', 'elseIf', 'else', 'case', 'switchDefault', 'break', 'iterator']) {
    ok(kinds.has(kind), `manifest registers ${kind}`);
  }
  const ifBlock = (info.blocks ?? []).find((b) => b.kind === 'if');
  ok(!(ifBlock?.childLists ?? []).some((l) => l.key === 'alternate'),
    'if no longer declares an alternate child list');
  const iterator = (info.blocks ?? []).find((b) => b.kind === 'iterator');
  ok(iterator?.execFlags?.iterable === true && (iterator?.childLists ?? []).some((l) => l.key === 'items'),
    'iterator declares iterable child items');
  for (const kind of ['log', 'stdout']) {
    const block = (info.blocks ?? []).find((b) => b.kind === kind);
    const params = new Set((block?.params ?? []).map((p) => p.name));
    ok(params.has('input') && params.has('filter') && params.has('format'),
      `${kind} exposes jq query parameters`);
  }
}

function languageTests(exe, work) {
  // ── if / elseIf / else sibling chains ──────────────────────────────────────
  {
    const { out, code } = runDoc(exe, work, 'if-then', {
      roots: [
        { kind: 'if', condition: '1 == 1', consequent: [stdout('THEN')] },
        { kind: 'else', items: [stdout('ELSE')] },
      ],
    });
    ok(code === 0 && countMark(out, 'THEN') === 1 && countMark(out, 'ELSE') === 0,
      'if true: runs consequent, sibling else skipped', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'if-else', {
      roots: [
        { kind: 'if', condition: '1 == 2', consequent: [stdout('THEN')] },
        { kind: 'else', items: [stdout('ELSE')] },
      ],
    });
    ok(code === 0 && countMark(out, 'ELSE') === 1 && countMark(out, 'THEN') === 0,
      'if false: sibling else runs', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'elseif-first-match', {
      roots: [
        { kind: 'if', condition: '1 == 2', consequent: [stdout('THEN')] },
        { kind: 'elseIf', condition: '1 == 3', items: [stdout('ELSEIF-A')] },
        { kind: 'elseIf', condition: '1 == 1', items: [stdout('ELSEIF-B')] },
        { kind: 'else', items: [stdout('ELSE')] },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'ELSEIF-B') === 1 && countMark(out, 'AFTER') === 1
      && countMark(out, 'THEN') === 0 && countMark(out, 'ELSEIF-A') === 0 && countMark(out, 'ELSE') === 0,
      'elseIf chain: first matching branch wins', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'chain-skips-after-match', {
      roots: [
        { kind: 'if', condition: '1 == 1', consequent: [stdout('THEN')] },
        { kind: 'elseIf', condition: '1 == 1', items: [stdout('ELSEIF')] },
        { kind: 'else', items: [stdout('ELSE')] },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'THEN') === 1 && countMark(out, 'AFTER') === 1
      && countMark(out, 'ELSEIF') === 0 && countMark(out, 'ELSE') === 0,
      'chain skips all branches after if matched', out.slice(-400));
  }
  {
    // A non-chain block between if and else terminates the chain → orphan else.
    const { out, code } = runDoc(exe, work, 'chain-interrupted', {
      roots: [
        { kind: 'if', condition: '1 == 2', consequent: [stdout('THEN')] },
        stdout('BETWEEN'),
        { kind: 'else', items: [stdout('ELSE')] },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'BETWEEN') === 1 && countMark(out, 'AFTER') === 1
      && countMark(out, 'ELSE') === 0,
      'non-chain sibling terminates the chain (else becomes orphan)', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'orphan-else', {
      roots: [stdout('BEFORE'), { kind: 'else', items: [stdout('ELSE')] }, stdout('AFTER')],
    });
    ok(code === 0 && countMark(out, 'BEFORE') === 1 && countMark(out, 'AFTER') === 1
      && countMark(out, 'ELSE') === 0,
      'orphan else is a warning, not fatal', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'orphan-elseif', {
      roots: [{ kind: 'elseIf', condition: '1 == 1', items: [stdout('ELSEIF')] }, stdout('AFTER')],
    });
    ok(code === 0 && countMark(out, 'ELSEIF') === 0 && countMark(out, 'AFTER') === 1,
      'orphan elseIf is a warning, not fatal', out.slice(-400));
  }
  {
    // else consumes the chain: a second else is an orphan.
    const { out, code } = runDoc(exe, work, 'double-else', {
      roots: [
        { kind: 'if', condition: '1 == 2', consequent: [stdout('THEN')] },
        { kind: 'else', items: [stdout('ELSE-1')] },
        { kind: 'else', items: [stdout('ELSE-2')] },
      ],
    });
    ok(code === 0 && countMark(out, 'ELSE-1') === 1 && countMark(out, 'ELSE-2') === 0,
      'else consumes the chain; second else is an orphan', out.slice(-400));
  }
  {
    // Chains are per-list: a nested chain inside a consequent works independently.
    const { out, code } = runDoc(exe, work, 'nested-chain', {
      context: { recording: false },
      roots: [
        {
          kind: 'if', condition: '1 == 1',
          consequent: [
            { kind: 'if', condition: 'recording', consequent: [stdout('STOP')] },
            { kind: 'else', items: [stdout('START')] },
          ],
        },
      ],
    });
    ok(code === 0 && countMark(out, 'START') === 1 && countMark(out, 'STOP') === 0,
      'nested sibling chain inside a consequent works', out.slice(-400));
  }

  // ── switch / case / default ────────────────────────────────────────────────
  {
    const { out, code } = runDoc(exe, work, 'switch-case', {
      context: { mode: 'b' },
      roots: [{
        kind: 'switch', variable: 'mode',
        items: [
          { kind: 'case', comparator: '===', expression: '"a"', consequent: [stdout('CASE-A')] },
          { kind: 'case', comparator: '===', expression: '"b"', consequent: [stdout('CASE-B')] },
          { kind: 'switchDefault', consequent: [stdout('DEFAULT')] },
        ],
      }],
    });
    ok(code === 0 && countMark(out, 'CASE-B') === 1
      && countMark(out, 'CASE-A') === 0 && countMark(out, 'DEFAULT') === 0,
      'switch: matching case runs, no fallthrough', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'switch-default', {
      context: { mode: 'zzz' },
      roots: [{
        kind: 'switch', variable: 'mode',
        items: [
          { kind: 'case', comparator: '===', expression: '"a"', consequent: [stdout('CASE-A')] },
          { kind: 'switchDefault', consequent: [stdout('DEFAULT')] },
        ],
      }],
    });
    ok(code === 0 && countMark(out, 'DEFAULT') === 1 && countMark(out, 'CASE-A') === 0,
      'switch: unmatched value runs switchDefault', out.slice(-400));
  }
  {
    // A case dragged outside a switch is an orphan no-op, not an executor.
    const { out, code } = runDoc(exe, work, 'orphan-case', {
      roots: [
        { kind: 'case', comparator: '===', expression: '"x"', consequent: [stdout('CASE')] },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'CASE') === 0 && countMark(out, 'AFTER') === 1,
      'orphan case outside switch is a warning, body skipped', out.slice(-400));
  }

  // ── break scoping ──────────────────────────────────────────────────────────
  {
    const { out, code } = runDoc(exe, work, 'while-break', {
      roots: [
        { kind: 'while', condition: '1 == 1', loopLimit: 10, items: [stdout('TICK'), { kind: 'break' }] },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'TICK') === 1 && countMark(out, 'AFTER') === 1,
      'while: break exits the loop only; execution continues', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'for-break', {
      roots: [
        {
          kind: 'for', variable: 'i', initial: '0', final: '5', comparator: '<', modifier: '+1',
          items: [stdout('TICK'), { kind: 'break' }],
        },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'TICK') === 1 && countMark(out, 'AFTER') === 1,
      'for: break exits the loop only; execution continues', out.slice(-400));
  }
  {
    // break inside the inner loop only ends the inner loop pass.
    const { out, code } = runDoc(exe, work, 'nested-break', {
      roots: [
        {
          kind: 'for', variable: 'i', initial: '0', final: '2', comparator: '<', modifier: '+1',
          items: [
            stdout('OUTER'),
            { kind: 'while', condition: '1 == 1', loopLimit: 10, items: [stdout('INNER'), { kind: 'break' }] },
          ],
        },
        stdout('AFTER'),
      ],
    });
    ok(code === 0 && countMark(out, 'OUTER') === 2 && countMark(out, 'INNER') === 2
      && countMark(out, 'AFTER') === 1,
      'nested loops: break exits the nearest loop only', out.slice(-400));
  }
  {
    const { out, code } = runDoc(exe, work, 'orphan-break', {
      roots: [stdout('BEFORE'), { kind: 'break' }, stdout('AFTER')],
    });
    ok(code === 0 && countMark(out, 'BEFORE') === 1 && countMark(out, 'AFTER') === 1,
      'break outside a loop is a warning; execution continues', out.slice(-400));
  }

  // ── iterator over arbitrary scope/context values ───────────────────────────
  {
    const { out, code } = runDoc(exe, work, 'iterator-context-values', {
      context: {
        nums: [1, 2, 3],
        labels: { a: 'red', b: 'blue' },
        single: 'solo',
      },
      roots: [
        {
          kind: 'iterator', input: 'nums', filter: '.',
          items: [stdout('ARR-${CURRENT_INDEX}-${CURRENT}')],
        },
        {
          kind: 'iterator', input: 'labels', filter: '.',
          items: [
            { kind: 'Parse', input: 'CURRENT', filter: '.key', storeAs: 'entryKey' },
            stdout('OBJ-${entryKey}'),
          ],
        },
        {
          kind: 'iterator', input: 'labels', filter: '.', mode: 'objectValues',
          items: [stdout('VAL-${CURRENT}')],
        },
        {
          kind: 'iterator', input: 'single', filter: '.',
          items: [stdout('SCALAR-${CURRENT_INDEX}-${CURRENT}')],
        },
      ],
    });
    ok(code === 0
      && countMark(out, 'ARR-0-1') === 1 && countMark(out, 'ARR-1-2') === 1 && countMark(out, 'ARR-2-3') === 1
      && countMark(out, 'OBJ-a') === 1 && countMark(out, 'OBJ-b') === 1
      && countMark(out, 'VAL-red') === 1 && countMark(out, 'VAL-blue') === 1
      && countMark(out, 'SCALAR-0-solo') === 1,
      'iterator fans out CURRENT/CURRENT_INDEX for arrays, objects, values, and scalars', out.slice(-800));
  }
  {
    const { out, code } = runDoc(exe, work, 'log-stdout-query', {
      context: { labels: { a: 'MARK:STDOUT-red', b: 'MARK:LOG-blue' } },
      roots: [
        { kind: 'stdout', input: 'labels', filter: '.a', message: 'ignored', newline: true },
        { kind: 'log', level: 'warn', input: 'labels', filter: '.b', message: 'ignored' },
      ],
    });
    ok(code === 0 && countMark(out, 'STDOUT-red') === 1 && countMark(out, 'LOG-blue') === 1 && !out.includes('ignored'),
      'stdout/log can render jq-filtered context values', out.slice(-800));
  }
}

// ── --md pseudo-code markdown (xblox_markdown.cpp) ──────────────────────────
// Renders the shared showcase fixture as markdown without running it.
function markdownTests(exe, work) {
  const fixture = join(PACKAGE_ROOT, 'packages', 'xblox', 'tests', 'xblox', 'language.xblox');
  const run = (extraArgs = []) => spawnSync(
    exe, ['--log-level', 'off', 'xblox', 'run', '--src', fixture, '--md', ...extraArgs],
    { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER },
  );

  {
    const r = run();
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0, `--md exits 0 (got ${r.status})`, (r.stderr ?? '').slice(-300));
    ok(lines[0] === '# language.xblox', '--md: H1 title is the source file name', lines[0]);
    ok(out.includes('## Context'), '--md: context section present');
    ok(lines.includes('- score: `7`') && lines.includes('- mode: `"beta"`'),
      '--md: context variables render as bullets', out.slice(0, 300));
    ok(lines.includes('- user: `{"name":"kim"}`'),
      '--md: nested context object renders compact', out.slice(0, 300));
    ok(out.includes('## Script'), '--md: script section present');
    ok(!lines.some((l) => l.includes('print ')),
      '--md: stdout blocks are filtered by default', out);
    ok(lines.includes('- if `score >= 9`') && lines.includes('- else if `score >= 6`')
      && lines.includes('- else'),
      '--md: if / else if / else chain renders as pseudo code', out.slice(0, 1200));
    ok(lines.includes('- switch `mode`') && lines.includes('  - case `"beta"`')
      && lines.includes('  - default'),
      '--md: switch / case / default nest under the switch', out);
    ok(lines.includes('- while `n < 10`') && lines.includes('- for `i = 0; i < 2; +1`'),
      '--md: while and for loops render', out);
    ok(lines.includes('- set `n = 0`') && lines.includes('  - set `n = n + 1`'),
      '--md: setVariable renders value and expression forms', out);
  }
  {
    const r = run(['--md-filter', 'none']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0 && lines.some((l) => l.startsWith('- print `"── xblox language showcase')),
      '--md-filter none: stdout blocks are included', out.slice(0, 600));
    ok(lines.some((l) => l.startsWith('  - print `"  score=${score}')),
      '--md-filter none: children indent one bullet level below their container', out);
    // Non-execution proof: templates stay uninterpolated and nothing printed raw.
    ok(out.includes('${score}') && !out.includes('score=7'),
      '--md does not run the document (no interpolation/output)', out);
  }
  {
    const r = run(['--md-filter', 'stdout,break']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0 && !lines.some((l) => l.includes('print ') || l.trim().endsWith('break')),
      '--md-filter: comma-separated kinds are all excluded', out);
    ok(lines.includes('- while `n < 10`'),
      '--md-filter: unfiltered kinds still render', out);
  }
  {
    const r = run(['--md-numbered', '--md-filter', 'none']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0 && lines.includes('1. print `"── xblox language showcase ───────────..."`'),
      '--md-numbered: numeric bullets at the root level', out.slice(0, 600));
    ok(lines.some((l) => /^ {3}1\. print /.test(l)),
      '--md-numbered: children use per-level numbering', out);
  }
  {
    // Default filter + numbering: filtered blocks don't consume numbers.
    const r = run(['--md-numbered']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0 && lines.includes('1. if `score >= 9`'),
      '--md-numbered: numbering skips filtered blocks', out.slice(0, 600));
  }
  {
    // Non-language blocks: <group>/<kind> prefix, -> storeAs suffix, loop note.
    const src = join(work, 'md-groups.xblox');
    writeFileSync(src, JSON.stringify({
      version: 1,
      loop: { enabled: true, intervalMs: 50, reset: false },
      roots: [
        { kind: 'keyEvent', keys: 'Shift+F12', storeAs: 'f9' },
        { kind: 'audioRecordStop', session: '${recordSession}', storeAs: 'wavPath' },
        { kind: 'fsWrite', path: '${journal}', append: true, content: 'x', storeAs: 'journalPath' },
      ],
    }, null, 2));
    const r = spawnSync(
      exe, ['--log-level', 'off', 'xblox', 'run', '--src', src, '--md'],
      { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER },
    );
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0 && lines.includes('> loops every 50ms; state persists across passes'),
      '--md: document loop settings surface under the title', out.slice(0, 400));
    ok(lines.includes('- input/keyEvent `Shift+F12` -> f9'),
      '--md: non-language blocks get a <group>/<kind> prefix', out);
    ok(lines.includes('- audio/audioRecordStop `${recordSession}` -> wavPath'),
      '--md: storeAs renders as -> suffix', out);
    ok(lines.includes('- files/fsWrite `${journal}` (append) -> journalPath'),
      '--md: append flag is annotated', out);
  }
  {
    // --mermaid: fenced flowchart replaces the bullet tree.
    const r = run(['--mermaid']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/);
    ok(r.status === 0, '--mermaid exits 0', (r.stderr ?? '').slice(-300));
    ok(lines.includes('```mermaid') && lines.includes('graph TD'),
      '--mermaid: fenced mermaid block, vertical by default', out.slice(0, 600));
    ok(out.includes('## Context') && lines.includes('- score: `7`'),
      '--mermaid: context variables stay as markdown bullets above the diagram', out.slice(0, 400));
    ok(lines.some((l) => l.includes('{"score >= 9"}')),
      '--mermaid: conditions render as decision diamonds', out);
    ok(lines.some((l) => l.includes('-->|yes|')) && lines.some((l) => l.includes('-->|no|')),
      '--mermaid: if branches get yes/no edge labels', out);
    ok(lines.some((l) => l.includes("-->|'beta'|")) && lines.some((l) => l.includes('-->|default|')),
      '--mermaid: switch cases label the outgoing edges', out);
    ok(lines.some((l) => l.includes('-->|do|')) && lines.some((l) => l.includes('-->|done|')),
      '--mermaid: loops get do/done edges', out);
    // Loop back-edge: the while diamond has both an incoming edge and a
    // back-edge from the body, i.e. at least two edges target it.
    const whileId = lines.map((l) => /^ {2}(B\d+)\{"while n < 10"\}/.exec(l)).find(Boolean)?.[1];
    const edgesIn = lines.filter((l) => l.includes('-->') && l.trimEnd().endsWith(` ${whileId}`));
    ok(whileId && edgesIn.length >= 2,
      '--mermaid: loop body wires back to the loop diamond', out);
    ok(!lines.some((l) => l.includes('print')),
      '--mermaid: --md-filter default still applies (stdout filtered)', out);
  }
  {
    const r = run(['--mermaid', '--mermaid-direction', 'LR']);
    ok(r.status === 0 && (r.stdout ?? '').includes('graph LR'),
      '--mermaid-direction LR: horizontal layout', (r.stdout ?? '').slice(0, 400));
    const bad = run(['--mermaid', '--mermaid-direction', 'XX']);
    ok(bad.status !== 0, '--mermaid-direction rejects unknown values');
  }
  {
    // Colouring: default "both" emits semantic linkStyles + group classDefs.
    const both = (run(['--mermaid']).stdout ?? '');
    ok(both.includes('stroke:#9ece6a') && both.includes('stroke:#f7768e')
      && both.includes('linkStyle default stroke:#565f89'),
      '--mermaid-color both (default): yes/no/default link tints', both.slice(-800));
    ok(both.includes('classDef flow stroke:#bb9af7'),
      '--mermaid-color both (default): flow diamonds get a classDef', both.slice(-800));
    const edges = (run(['--mermaid', '--mermaid-color', 'edges']).stdout ?? '');
    ok(edges.includes('linkStyle') && !edges.includes('classDef'),
      '--mermaid-color edges: linkStyles only', edges.slice(-800));
    const groups = (run(['--mermaid', '--mermaid-color', 'groups']).stdout ?? '');
    ok(!groups.includes('linkStyle') && groups.includes('classDef flow'),
      '--mermaid-color groups: classDefs only', groups.slice(-800));
    const none = (run(['--mermaid', '--mermaid-color', 'none']).stdout ?? '');
    ok(!none.includes('linkStyle') && !none.includes('classDef'),
      '--mermaid-color none: renderer theme untouched', none.slice(-800));
    const bad = run(['--mermaid', '--mermaid-color', 'rainbow']);
    ok(bad.status !== 0, '--mermaid-color rejects unknown values');
  }
  {
    const noMd = spawnSync(
      exe, ['--log-level', 'off', 'xblox', 'run', '--src', fixture, '--mermaid'],
      { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER },
    );
    ok(noMd.status !== 0, '--mermaid without --md is rejected', (noMd.stderr ?? '').slice(-300));
  }
  {
    // --mermaid-type sequence: chains/loops as fragments, no flow syntax.
    const r = run(['--mermaid', '--mermaid-type', 'sequence']);
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/).map((l) => l.trim());
    ok(r.status === 0 && out.includes('sequenceDiagram') && !out.includes('graph TD'),
      '--mermaid-type sequence: emits a sequenceDiagram', out.slice(0, 400));
    ok(lines.includes('participant Script'),
      '--mermaid-type sequence: Script participant always declared', out);
    ok(lines.includes('alt score >= 9') && lines.includes('else score >= 6')
      && lines.includes('else') && lines.includes('end'),
      '--mermaid-type sequence: if chain renders as alt/else fragments', out);
    ok(lines.includes('alt mode == "alpha"') && lines.includes('else default'),
      '--mermaid-type sequence: switch renders as alt fragments per case', out);
    ok(lines.includes('loop while n < 10') && lines.includes('loop for i = 0, i < 2, +1'),
      '--mermaid-type sequence: loops render as loop fragments', out);
    ok(lines.includes('Note over Script: break'),
      '--mermaid-type sequence: break renders as a note', out);
    const bad = run(['--mermaid', '--mermaid-type', 'gantt']);
    ok(bad.status !== 0, '--mermaid-type rejects unknown values');
  }
  {
    // Sequence with grouped blocks: participants, return arrows, doc loop.
    const src = join(work, 'md-groups.xblox'); // written by an earlier test block
    const r = spawnSync(
      exe, ['--log-level', 'off', 'xblox', 'run', '--src', src,
        '--md', '--mermaid', '--mermaid-type', 'sequence'],
      { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER },
    );
    const out = r.stdout ?? '';
    const lines = out.split(/\r?\n/).map((l) => l.trim());
    ok(r.status === 0 && lines.includes('participant Input')
      && lines.includes('participant Audio') && lines.includes('participant Files'),
      'sequence: registry groups become participants in first-use order', out);
    ok(lines.includes('Script->>Audio: audioRecordStop')
      && lines.includes('Audio-->>Script: wavPath'),
      'sequence: grouped blocks message their participant, storeAs returns dashed', out);
    ok(lines.includes('loop every 50ms'),
      'sequence: document loop wraps the diagram in a loop fragment', out);
  }
  {
    // --expand-parameters: full pseudo line in node labels vs compact default.
    const src = join(work, 'md-groups.xblox'); // written by the previous test block
    const args = ['--log-level', 'off', 'xblox', 'run', '--src', src, '--md', '--mermaid'];
    const compact = spawnSync(exe, args, { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER });
    ok(compact.status === 0 && compact.stdout.includes('["input/keyEvent -> f9"]')
      && !compact.stdout.includes('Shift+F12'),
      '--mermaid: compact node labels by default (group/kind -> storeAs)', compact.stdout);
    const expanded = spawnSync(exe, [...args, '--expand-parameters'],
      { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 30_000, maxBuffer: CLI_MAX_BUFFER });
    ok(expanded.status === 0 && expanded.stdout.includes('["input/keyEvent Shift+F12 -> f9"]'),
      '--expand-parameters: parameters appear in node labels', expanded.stdout);
    ok(expanded.stdout.includes('["files/fsWrite ${journal} (append) -> journalPath"]'),
      '--expand-parameters: append/store annotations carry over, backticks stripped', expanded.stdout);
    ok(/classDef input stroke:#[0-9a-f]{6}/.test(compact.stdout)
      && /\n {2}class B\d+(,B\d+)* audio\r?\n/.test(compact.stdout),
      '--mermaid: registry groups become node classDefs with stable assignments', compact.stdout);
  }
}

function servicePagesCreateResult(report) {
  const createEv = (report.events || []).find((e) => e.kind === 'servicePagesCreate' && e.status === 'ok');
  return createEv?.data?.result ?? null;
}

function servicePagesStdoutMarks(report) {
  return (report.events || [])
    .filter((e) => e.kind === 'stdout' && e.status === 'ok')
    .map((e) => String(e.message || e.data?.message || ''));
}

function servicePagesShareTests(exe) {
  loadEnv();
  const skipLive = process.argv.includes('--skip-live') || process.env.SKIP_SERVICE_TEST === '1';
  const serverUrl = (process.env.SERVER_URL || '').trim();
  if (skipLive || !serverUrl) {
    ok(true, 'service-pages share: skipped (SKIP_SERVICE_TEST or missing SERVER_URL)');
    return;
  }

  ok(existsSync(SERVICE_PAGES_XBLOX), 'service-pages.xblox fixture present');
  ok(ensureShareSmokeFixtures(), 'share-smoke.md + share-smoke.png fixtures present');

  const runShare = () => runXbloxSrc(exe, SERVICE_PAGES_XBLOX, { json: true, logLevel: 'warn' });

  const first = runShare();
  const marks1 = servicePagesStdoutMarks(first.report);
  ok(first.code === 0, `service-pages share fixture exits 0 (got ${first.code})`, first.out.slice(-800));
  ok(marks1.some((m) => m.includes('MARK:PAGE_CREATED')),
    'service-pages share: MARK:PAGE_CREATED', first.out.slice(-800));
  ok(marks1.some((m) => m.includes('MARK:UPLOADS-1')),
    'service-pages share: one image uploaded', first.out.slice(-800));

  const result1 = servicePagesCreateResult(first.report);
  ok(!!result1, 'service-pages share: servicePagesCreate ok event');
  ok(result1?.slug === SHARE_PAGE_SLUG, `service-pages share: slug matches ${SHARE_PAGE_SLUG}`);
  const pageId = result1?.page_id ?? result1?.id;
  ok(typeof pageId === 'string' && pageId.length > 0, 'service-pages share: page_id present');
  ok(Array.isArray(result1?.uploaded_images) && result1.uploaded_images.length === 1,
    'service-pages share: uploaded_images has one entry');
  ok(result1?.visible === false, 'service-pages share: page hidden');
  ok(result1?.uploaded_images?.[0]?.public_ref?.includes('./'), 'service-pages share: image public_ref rewritten');
  const uploadFolder = result1.uploaded_images[0]?.public_ref?.match(/\.\/([^/]+)\//)?.[1];
  ok(typeof uploadFolder === 'string' && uploadFolder.length > 0, 'service-pages share: upload folder parsed');

  const second = runShare();
  ok(second.code === 0, `service-pages share upsert exits 0 (got ${second.code})`, second.out.slice(-800));
  const result2 = servicePagesCreateResult(second.report);
  ok(!!result2, 'service-pages share upsert: servicePagesCreate ok event');
  ok((result2?.page_id ?? result2?.id) === pageId, 'service-pages share upsert: same page_id');
  ok(result2?.slug === SHARE_PAGE_SLUG, 'service-pages share upsert: slug unchanged');
  ok(Array.isArray(result2?.uploaded_images) && result2.uploaded_images.length === 1,
    'service-pages share upsert: uploaded_images has one entry');
  ok(result2.uploaded_images[0]?.skipped === true, 'service-pages share upsert: image skipped (if-newer)');
  ok(
    result2.uploaded_images[0]?.public_ref?.includes(`./${uploadFolder}/`),
    'service-pages share upsert: same upload folder',
  );

  console.log(`  ↳ stable slug=${SHARE_PAGE_SLUG} page_id=${pageId} folder=${uploadFolder} (no cleanup)`);
}

function main() {
  const exe = cliExePath();
  console.log(`XBlox language tests: ${exe}`);
  const work = mkdtempSync(join(tmpdir(), 'xblox-language-'));
  try {
    manifestTests(exe);
    languageTests(exe, work);
    markdownTests(exe, work);
    servicePagesShareTests(exe);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
  console.log(`\nXBlox language tests complete: passed=${passed} failed=${failed}`);
  process.exit(failed ? 1 : 0);
}

main();
