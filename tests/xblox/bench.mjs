#!/usr/bin/env node
/**
 * XBlox benchmark — throughput (fps/iters) + peak RSS.
 *
 *   npm run bench:xblox
 *   npm run bench:xblox -- --seconds 5 --append
 *   npm run bench:xblox -- --only language,expressions
 *
 * Four suites
 *   fps-raw-loop    — tightest interpreted loop floor (inner while, existing)
 *   fps-unthrottled — compiled scalar-slot engine (inner while, existing)
 *   language        — language.xblox full-pass: if/elseIf/switch/while/break
 *   expressions     — expressions.xblox full-pass: muparser/jq/templates
 *
 * Logging is suppressed (--log-level off, child stdout captured).
 * Peak RSS is polled from the child process every 50 ms.
 */

import { spawn }                                              from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync,
         writeFileSync, appendFileSync }                      from 'node:fs';
import { tmpdir, cpus, hostname, platform }                   from 'node:os';
import { dirname, join, resolve }                             from 'node:path';
import { fileURLToPath }                                      from 'node:url';

const __dirname    = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, '..', '..', '..', '..');
const HERE         = __dirname;

// ── CLI args ──────────────────────────────────────────────────────────────────

function arg(name, fallback) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}
const TARGET_SECONDS = Number(arg('seconds', '3'));
const APPEND         = process.argv.includes('--append');
const ONLY_FILTER    = arg('only', '').split(',').map(s => s.trim()).filter(Boolean);
const INT_MAX        = 2_147_483_647;

// Safety-net timeout: kill child if it doesn't exit within this many seconds.
// Prevents hangs from misconfigured benchmarks.
const CHILD_TIMEOUT_S = TARGET_SECONDS * 8 + 30;

// ── exe resolution ────────────────────────────────────────────────────────────

function cliExePath() {
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env?.trim()) return resolve(env.trim());
  const name  = platform() === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const win64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  return existsSync(win64) ? win64 : resolve(PACKAGE_ROOT, 'dist', name);
}
const EXE  = cliExePath();
const WORK = mkdtempSync(join(tmpdir(), 'xblox-bench-'));

// ── document helpers ──────────────────────────────────────────────────────────

/** Set loopLimit on every existing while/for node (fps-* inner-loop benchmarks). */
function withLoopLimit(doc, limit) {
  const clone = JSON.parse(JSON.stringify(doc));
  const walk  = (node) => {
    if (Array.isArray(node)) { node.forEach(walk); return; }
    if (node && typeof node === 'object') {
      if (node.kind === 'while' || node.kind === 'for') node.loopLimit = limit;
      for (const v of Object.values(node)) walk(v);
    }
  };
  walk(clone.roots ?? clone);
  return clone;
}

// ── peak RSS sampler ──────────────────────────────────────────────────────────

function sampleRssKb(pid) {
  return new Promise((res) => {
    if (platform() === 'linux') {
      try {
        const s = readFileSync(`/proc/${pid}/status`, 'utf8');
        const m = s.match(/VmRSS:\s+(\d+)/);
        return res(m ? Number(m[1]) : 0);
      } catch { return res(0); }
    }
    let out = '';
    const [cmd, cargs] = platform() === 'win32'
      ? ['wmic', ['process', 'where', `processid=${pid}`, 'get', 'WorkingSetSize', '/format:value']]
      : ['ps',   ['-o', 'rss=', '-p', String(pid)]];

    const child = spawn(cmd, cargs, { stdio: ['ignore', 'pipe', 'ignore'] });
    child.stdout.on('data', (d) => { out += d; });
    child.on('close', () => {
      if (platform() === 'win32') {
        const m = out.match(/WorkingSetSize=(\d+)/);
        res(m ? Math.round(Number(m[1]) / 1024) : 0);
      } else {
        res(Number(out.trim()) || 0);
      }
    });
    child.on('error', () => res(0));
  });
}

// ── timed run ─────────────────────────────────────────────────────────────────
//
// loopLimit=0  → single-pass (no loop flags)
// loopLimit>0  → --loop --loop-interval-ms 0 --loop-limit N  (doc-level loop)
// loopLimit<0  → --loop --loop-interval-ms 0  with inner loopLimit embedded
//                in the doc (fps-* suites pass 0, so this branch unused)

function timedRun(doc, loopLimit = 0) {
  const src  = join(WORK, `b-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(src, JSON.stringify(doc), 'utf8');

  const args = [
    'xblox', '--log-level', 'off', 'run',
    '--src', src, '--event-mode', 'count',
  ];
  if (loopLimit > 0) {
    // Doc-loop suite: runtime counter, no variable pollution, before yield.
    // --quiet suppresses stdout block writes (message still resolved + event emitted)
    // so the pipe never fills and numbers reflect pure execution cost.
    args.push('--loop', '--loop-interval-ms', '0', '--loop-limit', String(loopLimit), '--quiet');
  }

  return new Promise((resolve, reject) => {
    const t0    = process.hrtime.bigint();
    const child = spawn(EXE, args, { cwd: PACKAGE_ROOT, stdio: 'pipe' });

    // Safety-net kill (prevents hangs from misconfigured runs).
    const watchdog = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new Error(`child timed out after ${CHILD_TIMEOUT_S}s`));
    }, CHILD_TIMEOUT_S * 1000);

    let stderr = '';
    child.stderr.on('data', (d) => { stderr += d; });
    child.stdout.resume(); // drain — prevents pipe-buffer deadlock when doc has stdout blocks

    let peakKb = 0;
    const poll = setInterval(async () => {
      if (!child.pid) return;
      const kb = await sampleRssKb(child.pid);
      if (kb > peakKb) peakKb = kb;
    }, 50);

    child.on('close', (code) => {
      clearTimeout(watchdog);
      clearInterval(poll);
      const elapsed = Number(process.hrtime.bigint() - t0) / 1e9;
      if (code !== 0 && code !== null)
        reject(new Error(`exit ${code}: ${stderr.slice(0, 300)}`));
      else
        resolve({ elapsed, peakKb });
    });
    child.on('error', (err) => { clearTimeout(watchdog); clearInterval(poll); reject(err); });
  });
}

// ── measurement ───────────────────────────────────────────────────────────────

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
}

/**
 * @param {string}  name
 * @param {(n: number) => [doc: object, loopLimit: number]} makeRun
 *   Inner-loop suites:   loopLimit=0  (embedded in doc via withLoopLimit)
 *   Doc-loop suites:     loopLimit=n  (passed as --loop-limit n)
 * @param {number}  calibN  Calibration iteration count. Use a small value
 *   (~200) for doc-loop suites where each pass takes milliseconds; use
 *   200_000 for inner tight-loop suites where each iteration is nanoseconds.
 */
async function measure(name, makeRun, calibN) {
  // Startup floor: median of 3 single-iteration runs.
  const startupSamples = [];
  for (let i = 0; i < 3; i++) {
    const [doc, lim] = makeRun(1);
    const { elapsed } = await timedRun(doc, lim);
    startupSamples.push(elapsed);
  }
  const startup = median(startupSamples);

  // Calibration: estimate per-iteration rate.
  const [calibDoc, calibLim] = makeRun(calibN);
  const { elapsed: calibRaw }  = await timedRun(calibDoc, calibLim);
  const calibRate = calibN / Math.max(calibRaw - startup, 1e-6);

  // Pick N for ~TARGET_SECONDS, run once for final numbers.
  const N = Math.min(INT_MAX, Math.max(calibN, Math.round(calibRate * TARGET_SECONDS)));
  const [doc, lim] = makeRun(N);
  const { elapsed: rawT, peakKb } = await timedRun(doc, lim);
  const t = Math.max(rawT - startup, 1e-9);

  return { name, N, seconds: t, startup, rate: N / t, peakKb };
}

// ── benchmark registry ────────────────────────────────────────────────────────

const BENCHMARKS = [
  {
    // Inner-loop suites: loopLimit embedded in doc (withLoopLimit), no CLI loop flags.
    // calibN=200_000 because each inner iteration is nanoseconds.
    name:    'fps-raw-loop',
    note:    'while(1){ setVariable=const } — interpreted tight-loop floor',
    calibN:  200_000,
    makeRun: (lim) => [withLoopLimit(JSON.parse(readFileSync(join(HERE, 'performance/fps-raw-loop.xblox'), 'utf8')), lim), 0],
  },
  {
    name:    'fps-unthrottled',
    note:    'while(1){ expr; if(cond) } — compiled scalar-slot engine',
    calibN:  200_000,
    makeRun: (lim) => [withLoopLimit(JSON.parse(readFileSync(join(HERE, 'performance/fps-unthrottled.xblox'), 'utf8')), lim), 0],
  },
  {
    // Doc-loop suites: doc is unmodified, loopLimit passed via --loop-limit.
    // The C++ runtime counts passes before the inter-pass yield — precise,
    // no script-variable pollution, scope persists (reset:false via CLI).
    // calibN=200 because each full document pass takes milliseconds.
    name:    'language',
    note:    'language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet)',
    calibN:  1_000,
    makeRun: (lim) => [JSON.parse(readFileSync(join(HERE, 'performance/language.xblox'), 'utf8')), lim],
  },
  {
    name:    'expressions',
    note:    'expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout)',
    calibN:  200,
    makeRun: (lim) => [JSON.parse(readFileSync(join(HERE, 'performance/expressions.xblox'), 'utf8')), lim],
  },
  {
    name:    'expressions-bench',
    note:    'expressions-bench.xblox — same computation, no stdout (pure expr cost)',
    calibN:  50,
    makeRun: (lim) => [JSON.parse(readFileSync(join(HERE, 'performance/expressions-bench.xblox'), 'utf8')), lim],
  },
];

// ── run ───────────────────────────────────────────────────────────────────────

function fmtN(n)  { return Math.round(n).toLocaleString('en-US'); }
function fmtMb(kb) { return kb > 0 ? `${(kb / 1024).toFixed(1)} MB` : '—'; }

const active = ONLY_FILTER.length
  ? BENCHMARKS.filter((b) => ONLY_FILTER.includes(b.name))
  : BENCHMARKS;

console.log(`xblox bench | exe=${EXE}`);
console.log(`target~${TARGET_SECONDS}s/case | ${cpus()[0]?.model?.trim() || 'cpu?'} | ${hostname()}\n`);

const results = [];
for (const b of active) {
  process.stdout.write(`  ${b.name} ... `);
  try {
    const r = await measure(b.name, b.makeRun, b.calibN);
    results.push(r);
    console.log(
      `${fmtN(r.rate).padStart(12)} iters/sec` +
      `  peak ${fmtMb(r.peakKb).padStart(9)}` +
      `  (N=${fmtN(r.N)}, ${r.seconds.toFixed(2)}s, startup ${(r.startup * 1000).toFixed(0)}ms)`,
    );
  } catch (e) {
    console.log(`FAILED — ${e.message}`);
  }
}

// ── markdown table ────────────────────────────────────────────────────────────

const date = new Date().toISOString().slice(0, 10);
const cpu  = (cpus()[0]?.model || 'cpu?').trim();
const lines = [
  '',
  `### ${date} | ${cpu}`,
  '',
  '| benchmark | iters/sec | peak MB | note |',
  '|---|---:|---:|---|',
  ...results.map((r) => {
    const note = BENCHMARKS.find((b) => b.name === r.name)?.note ?? '';
    return `| \`${r.name}\` | ${fmtN(r.rate)} | ${fmtMb(r.peakKb)} | ${note} |`;
  }),
  '',
];
console.log(lines.join('\n'));

if (APPEND) {
  const out    = join(HERE, 'bench-results.md');
  const header = existsSync(out) ? '' : '# XBlox bench results\n';
  appendFileSync(out, header + lines.join('\n') + '\n', 'utf8');
  console.log(`appended → ${out}`);
}
