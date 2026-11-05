#!/usr/bin/env node
/**
 * Ninja wrapper that injects the MSVC environment (same as cmake.mjs).
 * Skips cmake's check-build-system step, which regenerates build.ninja
 * even when no CMake inputs changed.
 *
 *   node scripts/ninja.mjs --preset dev --target pm-audio
 *   node scripts/ninja.mjs --preset dev -- -j 8
 */
import { existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { cpus } from "node:os";
import { getVsEnv } from "./vs-env.mjs";
import { presetBuildDir } from "./preset-dir.mjs";

function ninjaHasFlag(args, flag) {
  return args.some((a) => a === flag || (a.startsWith(flag) && /^[\d.]/.test(a.slice(flag.length))));
}

function withDefaultNinjaLimits(args) {
  const jobs = Math.max(1, (cpus()?.length || 8) - 2);
  const extra = [];
  if (!ninjaHasFlag(args, "-j")) extra.push("-j", String(jobs));
  if (!ninjaHasFlag(args, "-l")) extra.push("-l", String(jobs));
  return extra.length ? [...extra, ...args] : args;
}

const args = process.argv.slice(2);
const presetIdx = args.indexOf("--preset");
const preset = presetIdx >= 0 ? args[presetIdx + 1] : "dev";

const targetIdx = args.indexOf("--target");
const target = targetIdx >= 0 ? args[targetIdx + 1] : null;

const sepIdx = args.indexOf("--");
const ninjaArgs = withDefaultNinjaLimits([
  ...(sepIdx >= 0 ? args.slice(sepIdx + 1) : []),
  ...(target ? [target] : []),
]);

const buildDir = presetBuildDir(preset);
if (!existsSync(buildDir)) {
  console.error(`[ninja] build dir not found: ${buildDir}`);
  console.error("[ninja] Run npm run configure first");
  process.exit(1);
}

spawnSync(process.execPath, ["scripts/ninja-clean.mjs", "--preset", preset, "--pre"], {
  cwd: process.cwd(),
  stdio: "inherit",
});

const vsEnv = getVsEnv();
console.log(`[ninja] ${["ninja", ...ninjaArgs].join(" ")}`);
const r = spawnSync("ninja", ninjaArgs, {
  cwd: buildDir,
  stdio: "inherit",
  ...(vsEnv ? { env: vsEnv } : {}),
});
if (r.error) throw r.error;
if ((r.status ?? 1) === 0) {
  spawnSync(process.execPath, ["scripts/ninja-clean.mjs", "--preset", preset, "--post"], {
    cwd: process.cwd(),
    stdio: "inherit",
  });
}
process.exit(r.status ?? 1);
