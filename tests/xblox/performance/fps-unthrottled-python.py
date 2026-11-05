#!/usr/bin/env python3
"""Python comparison for fps-unthrottled.xblox.

This intentionally simulates XBlox-ish overhead: block dispatch, dict context,
expression helpers, and event recording. Current XBlox reference observed locally:
~4500 iterations/sec.
"""

from __future__ import annotations

import signal
import time

REPORT_EVERY_MS = 5000
CHUNK_SIZE = 1000

running = True
context: dict[str, float] = {
    "frames": 0.0,
    "fps": 0.0,
    "lastReportMs": 0.0,
}
events: list[dict[str, object]] = []


def now_ms() -> float:
    return time.perf_counter() * 1000.0


context["lastReportMs"] = now_ms()


def stop(_signum: int, _frame: object) -> None:
    global running
    running = False


signal.signal(signal.SIGINT, stop)
if hasattr(signal, "SIGBREAK"):
    signal.signal(signal.SIGBREAK, stop)


def emit(kind: str, status: str, message: str = "", data: object | None = None) -> None:
    events.append({"kind": kind, "status": status, "message": message, "data": data})


def set_var(name: str, value: float) -> None:
    context[name] = value
    emit("setVariable", "ok", "variable set", {"name": name, "value": value})


def eval_expr(expr: str) -> float:
    if expr == "frames + 1":
        return context["frames"] + 1
    if expr == "nowMs - lastReportMs >= 5000":
        return 1.0 if now_ms() - context["lastReportMs"] >= REPORT_EVERY_MS else 0.0
    if expr == "frames * 1000 / (nowMs - lastReportMs)":
        return context["frames"] * 1000.0 / (now_ms() - context["lastReportMs"])
    if expr == "nowMs":
        return now_ms()
    if expr in context:
        return context[expr]
    return float(expr)


def run_set_variable(name: str, expression: str) -> None:
    set_var(name, eval_expr(expression))


def run_if(condition: str, consequent) -> None:
    emit("if", "running")
    if abs(eval_expr(condition)) > 1e-12:
        for block in consequent:
            block()
    emit("if", "ok", "if complete")


def run_log(level: str, expression: str) -> None:
    message = f"{eval_expr(expression):.1f}"
    print(f"[python-fps] {message}", flush=True)
    emit("log", "ok", message, {"level": level, "message": message})


def run_frame() -> None:
    run_set_variable("frames", "frames + 1")
    run_if(
        "nowMs - lastReportMs >= 5000",
        [
            lambda: run_set_variable("fps", "frames * 1000 / (nowMs - lastReportMs)"),
            lambda: run_log("info", "fps"),
            lambda: set_var("frames", 0.0),
            lambda: run_set_variable("lastReportMs", "nowMs"),
        ],
    )


print("[python-fps] XBlox-ish runtime simulation running; press Ctrl+C to stop", flush=True)
while running:
    for _ in range(CHUNK_SIZE):
        if not running:
            break
        run_frame()

print(f"[python-fps] stopped events={len(events)}", flush=True)
