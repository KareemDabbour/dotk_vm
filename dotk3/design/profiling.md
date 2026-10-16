# Source and VM Profiling

Both profilers share bounded per-session storage and timestamps. They observe the
existing executor without installing debug callbacks, spawning sampler threads or
reading a VM from another thread. `-O0/-O1/-O2` and backend selection are retained.
The profiler-off bytecode loop is a compile-time uninstrumented instantiation of
the same operation implementation, avoiding telemetry checks on every opcode.

## Commands

```sh
dotk3/build/release/dotk3 --profile program.k3
dotk3/build/release/dotk3 --profile-vm simple program.k3
dotk3/build/release/dotk3 --profile-vm verbose program.k3
dotk3/build/release/dotk3 --profile --profile-output source.speedscope.json program.k3
dotk3/build/release/dotk3 --profile --profile-vm verbose --profile-trace vm.trace.json program.k3
dotk3/build/release/dotk3 --profile --profile-interval-ms 5 -O1 program.k3
```

Options precede the script/eval/repl arguments. `--profile-output` enables source
profiling and writes Speedscope JSON. `--profile-trace` requires verbose VM mode
and writes Perfetto/Chrome complete-duration events; two exports must have distinct
paths. The CLI reports failed writes and preserves a script's existing error exit
code. File writes are explicit CLI requests, not a filesystem grant to script code.
Profiling with `--debug`, `--check` or `--ast` is rejected to avoid misleading runs.
The REPL accumulates data across submissions/resets until normal shell completion.

## Source View

`--profile` attributes sampled CPU to logical function/source stacks. It reports
self CPU, inclusive CPU, source file/line/column, task identities, recorded exact
activation counts and managed allocation sites. Generators count each resume as
an activation; their suspended wall time is not charged as executing CPU. The
report uses the top20 hotspots/sites rather than dumping user values or invoking
toString/getters. Exported stacks preserve caller/callee trees and task labels.

The sampler checks the CPU clock periodically at VM safe points (every64 checkpoint
opportunities) and takes a sample after the selected CPU interval. Native/phase
boundaries flush pending attribution as well. This is cooperative safe-point
sampling, not random signal-based sampling: short functions and long operations
between safe points may be biased. It does not unwind C++ or sample an opaque
native function's internal instructions. Native callbacks have separate measured
wall/CPU durations, so blocking waits are not mislabeled as script CPU execution.
On platforms with CLOCK_THREAD_CPUTIME_ID the clock is the VM thread; other builds
explicitly label the process-CPU fallback. Profile CPU includes profiler overhead.

## VM Views

Simple mode reports:

- Session wall and CPU clocks, optimization level and executed slot-bytecode versus
  lexical-bytecode program/call counts. Both execute compiled instructions; there
  is no AST engine or execution fallback.
- Managed Object allocations, estimated bytes, current/peak live object counts,
  peak logical frames and operand-stack values.
- Parse, compile, initialize, execute and GC phase totals.
- GC collection count, reclaimed objects, total collection time and pause
  p50/p95/max. These measure the actual synchronous collector, not a concurrent GC.

Verbose adds readable opcode counts, checked integer/generic arithmetic paths,
binding-cache hits/misses/invalidations, allocation-kind/site breakdowns, native
call counts/times and bounded timeline events. Counts are useful for explaining
execution mix; opcode counts are not native CPU time per opcode. This is VM
telemetry, not a replacement for Instruments/perf native symbol profiling.

Execute includes nested compile/native/GC work; native calls such as Tasks.pump
may include script execution. Do not sum overlapping inclusive phase durations.
Session wall time can include REPL waiting. Perfetto tids represent logical task
lanes on one executor, not multiple simultaneously running OS threads.

Allocation counts track managed Object creation. Byte estimates include sizeof
Object plus logical String/Bytes payload, not exact vector/map capacities, bytecode
or parser allocations, allocator overhead, RSS or foreign-resource payloads. Native
Instruments Allocations/OS process tools remain appropriate for total memory.

## Bounds and Failure Behavior

C++ ProfileOptions defaults cap samples/events at100000 and unique frame/site
records at16384; labels are shortened without splitting UTF-8. Function/phase
tables are capped too. Overflow is explicit in dropped counters and an overflow
frame rather than unbounded recording. GC p50/p95 use retained bounded pause
samples; max and total continue updating. Reports/exports are not a sandbox.

Use a fresh Profiler for each independent VM/thread; the CLI owns one session.
The C SDK ABI is unchanged; C++ hosts may set Config.profiler. Clock-controlled
tests use an injected clock. Cleanup-time recording failures are logged and produce
an incomplete report instead of terminating an in-flight script exception. Such
exports fail explicitly rather than presenting incomplete data as complete.

The CLI emits reports after normal execution, caught language errors or normal
REPL/game quit. SIGINT/SIGKILL/crash recovery and periodic on-disk snapshots are not
implemented, so prefer a finite workload or the application's graceful quit path.

## Overhead Evidence

A local Apple M1 Pro/O2 call-heavy check executes200000 trivial function calls,
with two warmups and five alternating-order whole-process samples per mode. It
includes profiler report formatting/capture and is not a universal overhead bound.

| Mode | Median ms | Relative to off |
| --- | ---: | ---: |
| Off | 75.28 | 1.00x |
| Source | 116.15 | 1.54x |
| VM simple | 86.50 | 1.15x |
| VM verbose | 238.65 | 3.17x |

The separate final seven-sample/three-memory before/after check against the frozen
optimized pre-profiler binary measured16.97ms before versus16.49ms after on the
existing25000-call workload, within local process-timing noise; peak RSS2.41 versus
2.47MiB. This is evidence against a large disabled-mode regression, not a zero-cost
guarantee. Code size increases because export/telemetry and two loop instantiations
are linked. Start with simple mode; use verbose when detailed counts justify cost.

- [Enabled mode samples](../../benchmarks/results/v3-profiler-modes.json)
- [Disabled-mode comparison](../../benchmarks/results/v3-profiler-disabled.json)

## Native Dispatch Evidence

Immediate methods use rooted ResolveMethod/InvokeMethod selections; detached
methods bind once, and both invoke the same shared native descriptor directly.
Descriptors now carry callbacks installed by type-owned registration tables; there
is no name-switched native body dispatcher or AST-dependent native invocation.
Profiles therefore show one native:List.length activation rather than nested
native:List.length/native:length wrappers. This removes dispatch allocation and
duplicate instrumentation, not the work of the underlying primitive.

Same-source Apple M1 Pro/Clang17 O2 results against the executable frozen before
the native-dispatch change: two warmups, seven alternating timing runs and three
separate memory runs per binary. Timings are unprofiled whole-process medians,
including initialization and compilation; startup is not subtracted.

| Workload | Before ms | After ms | Speedup | Peak RSS MiB Before/After |
| --- | ---: | ---: | ---: | ---: |
| 200000 immediate List.length calls | 126.46 | 59.78 | 2.12x | 5.70 / 2.53 |
| 450000 mixed native calls across six kinds | 267.60 | 92.30 | 2.90x | 5.94 / 2.67 |
| 200000 detached List.length calls | 97.94 | 59.30 | 1.65x | 5.64 / 2.55 |
| Ordinary function control | 68.40 | 70.15 | 0.98x | 2.48 / 2.53 |

Warm-cache startup measured7.67/7.29ms. These are local workload results, not a
general VM speedup: the ordinary-function control was3% slower in this run.
Peak physical footprint for immediate length fell4.50 to1.34MiB and mixed native
calls4.73 to1.47MiB. The stripped executable fell1279600 to1274864 bytes. Memory
high-water marks are not total allocation counts or live heap measurements.

Profiler tests separately verify zero managed allocations after setup in5400-call
scalar native loops at every optimization level, one canonical native activation
per call, and one BoundMethod allocation per detached extraction with no Native
allocation. Result-producing methods still allocate their results normally.

- [Workloads and checksums](../../benchmarks/v3_native_methods.json)
- [Callback registration timing/memory/binary evidence](../../benchmarks/results/v3-native-callbacks.json)
- [Earlier name-dispatch capture](../../benchmarks/results/v3-native-methods.json)

```sh
python3 tools/bench_compare.py --paired \
  benchmarks/bin/dotkv3-before-native-dispatch.out dotk3/build/release/dotk3 \
  benchmarks/v3_native_methods.json --baseline-source dotkv3 \
  --profile native-callback-registration --iterations 7 --warmups 2 --memory-iterations 3 \
  --output benchmarks/results/v3-native-callbacks.json
```

The helper's historical dotkv2/dotkv3 labels mean before/after in this report.

## Verification

`make -C dotk3 test-profiler` runs deterministic clock tests plus real CLI tests.
They validate CPU-versus-wall/suspension behavior, sample stacks/weights, bounded
overflow, GC/allocations, engine/optimization preservation, source/simple/verbose
modes, Speedscope/Perfetto JSON, malformed flags, export failures, original script
errors and disabled output. It is part of canonical `make test`. Twelve core groups,
the native host example, CLI diagnostics and maintained LSP stream transport pass
in release and UBSan. Runtime extension tests additionally cover registration,
descriptor lifetime, metadata-independent invocation, inherited/detached calls and
strict construction/error policy. The preserved four-ghost Pac-Man edit still
conflicts with older optional app fixtures; full app parity is not claimed.

Reproduce the mode-cost experiment with:

```sh
ruby dotk3/tests/profiler.rb dotk3/build/release/dotk3 --benchmark benchmarks/results/v3-profiler-modes.json
```