# XBlox loop-throughput benchmarks.
#
# Preferred: deterministic, auto-capturing harness (bounded loopLimit + wall-clock
# timing, startup subtracted, N auto-calibrated to ~3s/case). Appends a dated,
# machine-tagged table to packages/xblox/tests/xblox/bench-results.md:
#
#   npm run bench:xblox -- --seconds 3 --append
#
# The benchmarks run with events DISABLED, so they hit the compiled scalar-slot
# engine (the real fire-and-forget path). Correctness of both the compiled and
# interpreted engines is covered by:  npm run test:xblox-next
#
# ── manual single runs (infinite; Ctrl+C to stop, fps logged every 5s) ───────
tanit-cli.exe xblox --log-level info run --src packages\xblox\tests\xblox\while-count.xblox
tanit-cli.exe xblox --log-level info run --src packages\xblox\tests\xblox\performance\fps-unthrottled.xblox
tanit-cli.exe xblox --log-level info run --src packages\xblox\tests\xblox\performance\fps-raw-loop.xblox

# ── cross-language reference simulations (NOT the real engine; rough ceilings) ─
node packages\xblox\tests\xblox\performance\fps-unthrottled-node.mjs
node packages\xblox\tests\xblox\performance\fps-raw-loop-node.mjs 6
python packages\xblox\tests\xblox\performance\fps-unthrottled-python.py
python packages\xblox\tests\xblox\performance\fps-raw-loop-python.py 6
rustc -O packages\xblox\tests\xblox\performance\fps-unthrottled-rust.rs -o packages\xblox\tests\xblox\performance\fps-unthrottled-rust.exe && packages\xblox\tests\xblox\performance\fps-unthrottled-rust.exe || echo rustc not available
rustc -O packages\xblox\tests\xblox\performance\fps-raw-loop-rust.rs -o packages\xblox\tests\xblox\performance\fps-raw-loop-rust.exe && packages\xblox\tests\xblox\performance\fps-raw-loop-rust.exe 6 || echo rustc not available

# ── latest cpp numbers (iters/sec) — see bench-results.md for the full log ────
#
# 2026-06-01 · AMD Ryzen 7 3700X · RelWithDebInfo (npm run build:cpp):
#   fps-raw-loop      ~602,000,000   interpreted tight loop (no expr/json/muparser)
#   fps-unthrottled     ~9,380,000   compiled scalar-slot engine (setVariable+if body)
#
# History of fps-unthrottled (this benchmark, frames/sec ≈ iters/sec):
#   ~9,380,000  2026-06-01  compiled RPN engine re-enabled + generalised
#     ~151,000  2026-06-01  REGRESSED: compiled engine orphaned; per-iter resolve pipeline
#       422,603  (baseline) pre NH_Parameter/BlockIO/resolve-pipeline overhaul
#
# Old cross-language baselines (simulations): python 456895.9 · rust 6137224.6
