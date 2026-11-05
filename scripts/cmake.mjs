#!/usr/bin/env node
import { existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { getVsEnv } from "./vs-env.mjs";

const exe =
  process.env.CMAKE ||
  (process.platform === "linux" && existsSync("/snap/bin/cmake")
    ? "/snap/bin/cmake"
    : "cmake");

// On Windows with Ninja, MSVC env vars (LIB, INCLUDE, PATH) must be present.
// vs-env.mjs injects them when the shell is missing a healthy vcvars.
const vsEnv = getVsEnv();
const r = spawnSync(exe, process.argv.slice(2), {
  stdio: "inherit",
  ...(vsEnv ? { env: vsEnv } : {}),
});
if (r.error) throw r.error;
process.exit(r.status ?? 1);
