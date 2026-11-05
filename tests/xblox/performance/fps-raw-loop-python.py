#!/usr/bin/env python3
"""Raw Python comparison for fps-raw-loop.xblox.

This intentionally avoids expression math: one loop iteration writes a constant
context value and counts the same two successful events the C++ raw benchmark
counts for while-body setVariable execution.
"""

from __future__ import annotations

import signal
import sys
import time

CHUNK_SIZE = 1000

running = True
context: dict[str, float] = {"noop": 0.0}
event_count = 0


def stop(_signum: int, _frame: object) -> None:
    global running
    running = False


signal.signal(signal.SIGINT, stop)
if hasattr(signal, "SIGBREAK"):
    signal.signal(signal.SIGBREAK, stop)


def set_var(name: str, value: float) -> None:
    global event_count
    context[name] = value
    event_count += 2


duration = float(sys.argv[1]) if len(sys.argv) > 1 else None
started = time.perf_counter()
while running:
    for _ in range(CHUNK_SIZE):
        set_var("noop", 1.0)
    if duration is not None and time.perf_counter() - started >= duration:
        break

elapsed = time.perf_counter() - started
print(f"[python-raw] elapsed={elapsed:.3f} events={event_count} iterationsPerSec={(event_count / 2) / elapsed:.1f} eventsPerSec={event_count / elapsed:.1f}", flush=True)
