/**
 * Capture a usable MSVC + Windows SDK environment for Ninja builds.
 * Shared by cmake.mjs and ninja.mjs.
 *
 * Do not trust VSCMD_* alone. Shells often keep those markers after a broken
 * or partial vcvars, which skips injection and breaks configure
 * ("No CMAKE_CXX_COMPILER could be found").
 */
import { existsSync } from "node:fs";
import { spawnSync } from "node:child_process";

function includeLooksHealthy(include) {
  if (!include || include.length < 200) return false;
  const hasMsvc = /MSVC/i.test(include) || /Microsoft Visual Studio/i.test(include);
  const hasSdk = /Windows Kits/i.test(include) || /ucrt/i.test(include);
  return hasMsvc && hasSdk;
}

function clOnPath(env) {
  const r = spawnSync("where.exe", ["cl"], {
    encoding: "utf8",
    env,
    windowsHide: true,
  });
  return r.status === 0 && Boolean(r.stdout?.trim());
}

function findVcvarsall() {
  const vswhere =
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
  if (!existsSync(vswhere)) return null;

  const attempts = [
    [
      "-latest",
      "-products",
      "*",
      "-requires",
      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
      "-property",
      "installationPath",
    ],
    ["-latest", "-products", "*", "-property", "installationPath"],
    ["-latest", "-property", "installationPath"],
  ];

  for (const args of attempts) {
    const vsFind = spawnSync(vswhere, args, { encoding: "utf8" });
    const root = (vsFind.stdout || "").trim().split(/\r?\n/).filter(Boolean)[0];
    if (!root) continue;
    const vcvarsall = `${root}\\VC\\Auxiliary\\Build\\vcvarsall.bat`;
    if (existsSync(vcvarsall)) return vcvarsall;
  }

  const fallback =
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat";
  return existsSync(fallback) ? fallback : null;
}

/** @returns {NodeJS.ProcessEnv | null} env to pass to spawn, or null if the current env is fine */
export function getVsEnv() {
  if (process.platform !== "win32") return null;

  if (includeLooksHealthy(process.env.INCLUDE) && clOnPath(process.env)) {
    return null;
  }

  const vcvarsall = findVcvarsall();
  if (!vcvarsall) return null;

  const dump = spawnSync(`"${vcvarsall}" amd64 > nul 2>&1 && set`, [], {
    shell: true,
    encoding: "utf8",
  });
  if (dump.status !== 0 || !dump.stdout) return null;

  const env = { ...process.env };
  for (const line of dump.stdout.split(/\r?\n/)) {
    const idx = line.indexOf("=");
    if (idx > 0) env[line.slice(0, idx)] = line.slice(idx + 1);
  }

  if (!includeLooksHealthy(env.INCLUDE) || !clOnPath(env)) {
    console.error(
      "[vs-env] vcvarsall ran but INCLUDE/cl still unusable — check the Visual Studio install"
    );
    return null;
  }
  return env;
}
