# XBlox bench results (append-only)

### 2026-06-01 | AMD Ryzen 7 3700X 8-Core Processor | RelWithDebInfo

| benchmark | iters/sec | note |
|---|---:|---|
| `fps-raw-loop` | 612,117,201 | while(1){ setVariable=const } - pure loop floor, no expr/json/muparser (interpreted tight loop) |
| `fps-unthrottled` | 9,511,502 | while(1){ setVariable expr; if(cond){...} } - multi-block body (compiled scalar-slot engine) |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor | RelWithDebInfo

| benchmark | iters/sec | note |
|---|---:|---|
| `fps-raw-loop` | 612,128,833 | while(1){ setVariable=const } - pure loop floor, no expr/json/muparser (interpreted tight loop) |
| `fps-unthrottled` | 11,585,472 | while(1){ setVariable expr; if(cond){...} } - multi-block body (compiled scalar-slot engine) |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 590,773,657 | 20.9 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 11,118,748 | 19.6 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 668 | 22.4 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator |
| `expressions` | 46 | 23.8 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 605,196,679 | 20.9 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 11,232,327 | 19.6 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 722 | 22.6 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (with stdout) |
| `expressions` | 47 | 23.8 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `expressions` | 46 | 24.0 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 605,110,931 | 20.9 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 11,034,838 | 19.7 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 689 | 22.4 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (with stdout) |
| `language-bench` | 2,799 | 23.1 MB | language-bench.xblox — same flow, no stdout (pure control-flow cost) |
| `expressions` | 43 | 24.0 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 603,774,245 | 20.9 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `language` | 561 | 22.6 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 31 | 23.9 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 552,942,649 | — | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 7,983,884 | 19.6 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 527 | 22.3 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 29 | 24.0 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `expressions-bench` | 41 | 23.5 MB | expressions-bench.xblox — same computation, no stdout (pure expr cost) |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor | jq filter-cache + no-fs-ops

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 597,032,254 | 21.6 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 10,353,718 | 20.9 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 804 | 23.2 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 100 | 24.3 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout) |
| `expressions-bench` | 1,177 | 22.3 MB | expressions-bench.xblox — pure expr cost, no stdout, no fs (28.7× vs baseline 41) |


### 2026-06-18 | AMD Ryzen 7 3700X 8-Core Processor | + zero-copy nlohmann↔jv converters

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 607,792,357 | 20.8 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 10,966,988 | 19.6 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 846 | 22.4 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 107 | 23.1 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout) |
| `expressions-bench` | 1,282 | 22.2 MB | expressions-bench.xblox — pure expr cost, no stdout, no fs (+8.9% over filter-cache alone) |


### 2026-06-19 | AMD Ryzen 7 3700X 8-Core Processor | + muParser bytecode-reuse (skip recompile on unrelated scope push/pop)

Profiling (2,000 passes) showed `mu bind+Eval` was 69% of measured time at 11.4 µs/eval —
muParser was recompiling bytecode on nearly every eval because each if/while `ScopeGuard`
push/pop bumped the global structural sequence, invalidating every cached expression.
Fix: when the sequence moves, re-derive the expression's name→address bindings (cheap
pointer walk) and only force a SetExpr/DefineVar/ClearVar recompile if an address actually
changed. Result: `mu bind+Eval` 320 µs → 73 µs per pass (4.4×), 61% bytecode reuse.

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `language` | 867 | 22.7 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 112 | 23.2 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout) |
| `expressions-bench` | 2,004 | 22.0 MB | expressions-bench.xblox — pure expr cost (+56% over bytecode-reuse; 1,282 → 2,004) |


### 2026-07-10 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 63,727 | 633.5 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 32,544 | 944.4 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 101 | 24.9 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 33 | 26.9 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout) |
| `expressions-bench` | 1,395 | 25.3 MB | expressions-bench.xblox — same computation, no stdout (pure expr cost) |


### 2026-07-12 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 41,125 | 1072.5 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 24,398 | 1606.8 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `language` | 99 | 25.2 MB | language.xblox — if/elseIf/else/switch/while/for/break/iterator (--quiet) |
| `expressions` | 31 | 29.3 MB | expressions.xblox — muparser/jq/PREVIOUS/templates/nowMs (with stdout) |
| `expressions-bench` | 1,408 | 25.6 MB | expressions-bench.xblox — same computation, no stdout (pure expr cost) |


### 2026-07-29 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 39,338 | 1078.9 MB | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 22,185 | 1613.8 MB | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `expressions-bench` | 1,036 | 32.6 MB | expressions-bench.xblox — same computation, no stdout (pure expr cost) |


### 2026-09-20 | AMD Ryzen 7 3700X 8-Core Processor

| benchmark | iters/sec | peak MB | note |
|---|---:|---:|---|
| `fps-raw-loop` | 20,427 | — | while(1){ setVariable=const } — interpreted tight-loop floor |
| `fps-unthrottled` | 8,627 | — | while(1){ expr; if(cond) } — compiled scalar-slot engine |
| `expressions-bench` | 268 | — | expressions-bench.xblox — same computation, no stdout (pure expr cost) |

