#!/usr/bin/env node
/**
 * Pre/post helper for .ninja_deps.
 *
 * A Ctrl-C mid-build can leave .ninja_deps truncated. The next ninja then
 * prints "premature end of file; recovering" and re-reads every dep file.
 *
 *   --pre  : restore a healthy backup, or delete a corrupt log.
 *   --post : copy a healthy log to .ninja_deps.bak after a successful build.
 */
import { readFileSync, copyFileSync, rmSync, existsSync, statSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { join } from "node:path";
import { presetBuildDir } from "./preset-dir.mjs";

const MAGIC = "# ninjadeps\n";

function isHealthy(file) {
  if (!existsSync(file)) return false;
  const size = statSync(file).size;
  if (size < MAGIC.length + 4) return false;
  const header = readFileSync(file, { encoding: "ascii", flag: "r" }).slice(0, MAGIC.length);
  return header === MAGIC;
}

function sizeOf(file) {
  return existsSync(file) ? statSync(file).size : 0;
}

function ninjaAcceptsDeps(buildDir) {
  const r = spawnSync("ninja", ["-C", buildDir, "-t", "recompact"], {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  });
  const output = `${r.stdout ?? ""}${r.stderr ?? ""}`;
  return r.status === 0 && !/premature end of file|bad deps log signature|deps log/i.test(output);
}

const args = process.argv.slice(2);
const mode = args.includes("--post") ? "post" : "pre";
const presetIdx = args.indexOf("--preset");
const preset = presetIdx >= 0 ? args[presetIdx + 1] : "dev";
const buildDir = presetBuildDir(preset);
const deps = join(buildDir, ".ninja_deps");
const backup = join(buildDir, ".ninja_deps.bak");

if (!existsSync(buildDir)) process.exit(0);

if (mode === "pre") {
  const depsHealthy = isHealthy(deps);
  const backupHealthy = isHealthy(backup);
  const depsLooksTruncated = depsHealthy && backupHealthy && sizeOf(deps) < sizeOf(backup);

  if (!depsHealthy || depsLooksTruncated) {
    if (backupHealthy) {
      console.log("[ninja-clean] restoring .ninja_deps from backup");
      try { rmSync(deps, { force: true }); } catch { /* retry below */ }
      copyFileSync(backup, deps);
    } else if (existsSync(deps)) {
      const sz = statSync(deps).size;
      console.log(`[ninja-clean] removing corrupt .ninja_deps (${sz} bytes)`);
      rmSync(deps, { force: true });
    }
  }

  if (existsSync(deps) && !ninjaAcceptsDeps(buildDir)) {
    console.log("[ninja-clean] removing corrupt .ninja_deps and backup");
    try { rmSync(deps, { force: true }); } catch { /* gone */ }
    try { rmSync(backup, { force: true }); } catch { /* gone */ }
  }
} else if (isHealthy(deps)) {
  try {
    copyFileSync(deps, backup);
  } catch {
    /* non-fatal */
  }
}
