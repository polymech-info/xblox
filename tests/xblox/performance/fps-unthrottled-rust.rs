// Rust comparison for fps-unthrottled.xblox.
// This intentionally simulates XBlox-ish overhead: block dispatch, HashMap context,
// expression helpers, and event recording. Current XBlox reference observed locally:
// ~4500 iterations/sec.
//
// Build/run:
//   rustc -O tests/xblox/fps-unthrottled-rust.rs -o tests/xblox/fps-unthrottled-rust.exe
//   tests/xblox/fps-unthrottled-rust.exe
//   tests/xblox/fps-unthrottled-rust.exe 6
// The optional numeric argument exits after that many seconds for smoke tests.

use std::collections::HashMap;
use std::env;
use std::time::{Duration, Instant};

const REPORT_EVERY_MS: f64 = 5000.0;
const CHUNK_SIZE: usize = 1000;

#[derive(Clone)]
struct Event {
    kind: &'static str,
    status: &'static str,
}

struct Runtime {
    started: Instant,
    context: HashMap<&'static str, f64>,
    events: Vec<Event>,
}

impl Runtime {
    fn new() -> Self {
        let mut context = HashMap::new();
        context.insert("frames", 0.0);
        context.insert("fps", 0.0);
        context.insert("lastReportMs", 0.0);
        let mut runtime = Self { started: Instant::now(), context, events: Vec::new() };
        runtime.set_var("lastReportMs", runtime.now_ms());
        runtime
    }

    fn now_ms(&self) -> f64 {
        self.started.elapsed().as_secs_f64() * 1000.0
    }

    fn emit(&mut self, kind: &'static str, status: &'static str) {
        self.events.push(Event { kind, status });
    }

    fn get(&self, name: &'static str) -> f64 {
        *self.context.get(name).unwrap_or(&0.0)
    }

    fn set_var(&mut self, name: &'static str, value: f64) {
        self.context.insert(name, value);
        self.emit("setVariable", "ok");
    }

    fn eval_expr(&self, expr: &'static str) -> f64 {
        match expr {
            "frames + 1" => self.get("frames") + 1.0,
            "nowMs - lastReportMs >= 5000" => {
                if self.now_ms() - self.get("lastReportMs") >= REPORT_EVERY_MS { 1.0 } else { 0.0 }
            }
            "frames * 1000 / (nowMs - lastReportMs)" => self.get("frames") * 1000.0 / (self.now_ms() - self.get("lastReportMs")),
            "nowMs" => self.now_ms(),
            "fps" => self.get("fps"),
            _ => expr.parse::<f64>().unwrap_or(0.0),
        }
    }

    fn run_set_variable(&mut self, name: &'static str, expression: &'static str) {
        let value = self.eval_expr(expression);
        self.set_var(name, value);
    }

    fn run_if(&mut self, condition: &'static str) {
        self.emit("if", "running");
        if self.eval_expr(condition).abs() > 1e-12 {
            self.run_set_variable("fps", "frames * 1000 / (nowMs - lastReportMs)");
            println!("[rust-fps] {:.1}", self.eval_expr("fps"));
            self.emit("log", "ok");
            self.set_var("frames", 0.0);
            self.run_set_variable("lastReportMs", "nowMs");
        }
        self.emit("if", "ok");
    }

    fn run_frame(&mut self) {
        self.run_set_variable("frames", "frames + 1");
        self.run_if("nowMs - lastReportMs >= 5000");
    }

    fn event_summary(&self) -> (usize, usize) {
        let ok_events = self.events.iter().filter(|event| event.status == "ok").count();
        let log_events = self.events.iter().filter(|event| event.kind == "log").count();
        (ok_events, log_events)
    }
}

fn main() {
    let max_duration = env::args()
        .nth(1)
        .and_then(|arg| arg.parse::<f64>().ok())
        .filter(|seconds| *seconds > 0.0)
        .map(Duration::from_secs_f64);

    println!("[rust-fps] XBlox-ish runtime simulation running; press Ctrl+C to stop");

    let mut runtime = Runtime::new();
    loop {
        for _ in 0..CHUNK_SIZE {
            runtime.run_frame();
        }
        if let Some(duration) = max_duration {
            if runtime.started.elapsed() >= duration {
                let (ok_events, log_events) = runtime.event_summary();
                println!(
                    "[rust-fps] stopped events={} okEvents={} logEvents={}",
                    runtime.events.len(),
                    ok_events,
                    log_events
                );
                break;
            }
        }
    }
}
