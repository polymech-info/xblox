#!/usr/bin/env node
// Node.js comparison for tests/xblox/fps-unthrottled.xblox, intentionally
// simulating XBlox-ish block dispatch, object context, expression helpers, and event recording.
// Current XBlox reference observed locally: ~4500 iterations/sec.

const reportEveryMs = 5000;
const chunkSize = 1000;

let running = true;
const context = {
  frames: 0,
  fps: 0,
  lastReportMs: nowMs(),
};
const events = [];

process.on("SIGINT", () => {
  running = false;
});

process.on("SIGBREAK", () => {
  running = false;
});

function nowMs() {
  return Number(process.hrtime.bigint() / 1000000n);
}

function emit(kind, status, message, data) {
  events.push({ kind, status, message, data });
}

function getVar(name) {
  return context[name];
}

function setVar(name, value) {
  context[name] = value;
  emit("setVariable", "ok", "variable set", { name, value });
}

function evalExpression(name) {
  if (name === "frames + 1") return getVar("frames") + 1;
  if (name === "nowMs - lastReportMs >= 5000") return nowMs() - getVar("lastReportMs") >= reportEveryMs ? 1 : 0;
  if (name === "frames * 1000 / (nowMs - lastReportMs)") return (getVar("frames") * 1000) / (nowMs() - getVar("lastReportMs"));
  if (name === "nowMs") return nowMs();
  if (name in context) return Number(getVar(name));
  return Number(name) || 0;
}

function runSetVariable(name, expression) {
  setVar(name, evalExpression(expression));
}

function runIf(condition, consequent) {
  emit("if", "running", "");
  if (Math.abs(evalExpression(condition)) > 1e-12) {
    for (const block of consequent) block();
  }
  emit("if", "ok", "if complete");
}

function runLog(level, expression) {
  const message = String(evalExpression(expression));
  console.log(`[node-fps] ${message}`);
  emit("log", "ok", message, { level, message });
}

function runFrame() {
  runSetVariable("frames", "frames + 1");
  runIf("nowMs - lastReportMs >= 5000", [
    () => runSetVariable("fps", "frames * 1000 / (nowMs - lastReportMs)"),
    () => runLog("info", "fps"),
    () => setVar("frames", 0),
    () => runSetVariable("lastReportMs", "nowMs"),
  ]);
}

function tick() {
  for (let i = 0; i < chunkSize && running; i += 1) {
    runFrame();
  }

  if (running) {
    setImmediate(tick);
  } else {
    console.log(`[node-fps] stopped events=${events.length}`);
  }
}

console.log("[node-fps] XBlox-ish runtime simulation running; press Ctrl+C to stop");
setImmediate(tick);
