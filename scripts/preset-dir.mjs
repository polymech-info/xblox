import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";

export function presetBuildDir(presetName) {
  const presetsFile = resolve(process.cwd(), "CMakePresets.json");
  if (existsSync(presetsFile)) {
    try {
      const presets = JSON.parse(readFileSync(presetsFile, "utf8"));
      const all = [
        ...(presets.buildPresets || []),
        ...(presets.configurePresets || []),
      ];
      const found = all.find((p) => p.name === presetName);
      if (found?.binaryDir) {
        return found.binaryDir
          .split("${sourceDir}").join(process.cwd())
          .split("${presetName}").join(presetName);
      }
    } catch {
      /* fall through */
    }
  }
  return resolve(process.cwd(), "build", presetName);
}
