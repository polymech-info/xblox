#!/usr/bin/env node
// Raw Node.js comparison for fps-raw-loop.xblox.
//
// This intentionally avoids expression math: one loop iteration writes a
// constant context value and counts the same two successful events the C++ raw
// benchmark counts for while-body setVariable execution.

const chunkSize = 1000;
const durationSeconds = Number(process.argv[2] ?? "6");
const durationMs = Number.isFinite(durationSeconds) && durationSeconds > 0 ? durationSeconds * 1000 : 6000;

const context = { noop: 0 };
let eventCount = 0;
let running = true;

process.on("SIGINT", () => {
  running = false;
});

process.on("SIGBREAK", () => {
  running = false;
});

function nowMs() {
  return Number(process.hrtime.bigint() / 1000000n);
}

function setVar(name, value) {
  context[name] = value;
  eventCount += 2;
}

const started = nowMs();
while (running && nowMs() - started < durationMs) {
  for (let i = 0; i < chunkSize; i += 1) {
    setVar("noop", 1);
  }
}

const elapsedSeconds = (nowMs() - started) / 1000;
console.log(
  `[node-raw] elapsed=${elapsedSeconds.toFixed(3)} events=${eventCount} iterationsPerSec=${((eventCount / 2) / elapsedSeconds).toFixed(1)} eventsPerSec=${(eventCount / elapsedSeconds).toFixed(1)}`,
);
