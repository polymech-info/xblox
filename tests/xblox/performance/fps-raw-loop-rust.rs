// Raw Rust comparison for fps-raw-loop.xblox.
//
// This intentionally avoids expression math: one loop iteration writes a
// constant context value and counts the same two successful events the C++ raw
// benchmark counts for while-body setVariable execution.
//
// Build/run:
//   rustc -O tests/xblox/fps-raw-loop-rust.rs -o tests/xblox/fps-raw-loop-rust.exe
//   tests/xblox/fps-raw-loop-rust.exe 6

use std::collections::HashMap;
use std::env;
use std::time::{Duration, Instant};

const CHUNK_SIZE: usize = 1000;

struct Runtime {
    context: HashMap<&'static str, f64>,
    event_count: u64,
}

impl Runtime {
    fn new() -> Self {
        let mut context = HashMap::new();
        context.insert("noop", 0.0);
        Self { context, event_count: 0 }
    }

    fn set_var(&mut self, name: &'static str, value: f64) {
        self.context.insert(name, value);
        self.event_count += 2;
    }
}

fn main() {
    let duration = env::args()
        .nth(1)
        .and_then(|arg| arg.parse::<f64>().ok())
        .filter(|seconds| *seconds > 0.0)
        .map(Duration::from_secs_f64)
        .unwrap_or_else(|| Duration::from_secs(6));

    let started = Instant::now();
    let mut runtime = Runtime::new();

    while started.elapsed() < duration {
        for _ in 0..CHUNK_SIZE {
            runtime.set_var("noop", 1.0);
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    println!(
        "[rust-raw] elapsed={:.3} events={} iterationsPerSec={:.1} eventsPerSec={:.1}",
        elapsed,
        runtime.event_count,
        (runtime.event_count as f64 / 2.0) / elapsed,
        runtime.event_count as f64 / elapsed
    );
}
