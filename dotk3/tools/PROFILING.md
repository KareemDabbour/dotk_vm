# dotk3 Profiler Guide

## Enabling profiling

| Flag | What it records |
|---|---|
| `--profile` | Safe-point sampled CPU profile of your dotk3 source code |
| `--profile-vm simple` | VM-level counters: allocations, GC, phase timings, call counts |
| `--profile-vm verbose` | Everything in `simple` plus opcode counts, binding cache, arithmetic paths, and a timeline trace |
| `--profile-output FILE` | Write source CPU profile as [Speedscope](https://www.speedscope.app) JSON |
| `--profile-trace FILE` | Write VM timeline as [Perfetto](https://ui.perfetto.dev) JSON (requires `--profile-vm verbose`) |
| `--profile-interval-ms N` | Sampling interval in ms (default 1 ms, range 1–1000) |
| `-O0 / -O1 / -O2` | Optimization level (default `-O2`); lower levels may show more granular profiles |

Examples:

```sh
# Source + VM verbose, open flame graph in Speedscope
dotk3 --profile --profile-vm verbose --profile-output cpu.json examples/game_of_life.k3
# Then open cpu.json at https://www.speedscope.app

# Visualize the full text report with profile_view.py
dotk3 --profile --profile-vm verbose myprogram.k3 2>report.txt && python3 tools/profile_view.py report.txt

# Timeline trace for Perfetto
dotk3 --profile-vm verbose --profile-trace trace.json myprogram.k3
# Then open trace.json at https://ui.perfetto.dev
```

---

## Text report sections

The text report is printed to **stderr** at the end of a profiled run.

### Source CPU profile

```
DotK source CPU profile (safe-point sampled; not debugger mode)
Samples: 42, attributed CPU: 210.000 ms
  Self ms  Inclusive ms  Function / source
  100.000       150.000  myFn  examples/game_of_life.k3:30:5
```

- **Safe-point sampled** — samples are taken at `SafePoint` bytecode instructions, not on a timer thread. This means sample counts are proportional to CPU work but not clock-exact.
- **Self ms** — CPU time spent directly inside this function (not in callees).
- **Inclusive ms** — CPU time including all callees.
- Sorting by **self ms** is the best way to find hot spots.
- Only the top 20 frames are shown. Export to Speedscope (`--profile-output`) for the full flame graph.

### VM profile

```
DotK VM profile (verbose)
Engine: bytecode-only; optimization O2
Wall: 500.000 ms; CPU: 400.000 ms
Execution: 1000 slot-bytecode calls, 200 lexical-bytecode calls
```

- **Wall** — real (wall-clock) elapsed time of the entire run.
- **CPU** — thread CPU time (more stable than wall for comparing code changes).
- **Slot-bytecode calls** — calls dispatched through the fast pre-compiled bytecode slot (common case).
- **Lexical-bytecode calls** — calls that had to look up the function by name at runtime (slower; usually closures or late-bound calls).

### Phase timings

```
  Parse:      10.000 ms wall,  9.000 ms CPU (1)
  Compile:     5.000 ms wall,  4.000 ms CPU (1)
  Initialize: 20.000 ms wall, 18.000 ms CPU (1)
  Execute:   460.000 ms wall,368.000 ms CPU (1)
  GC:          5.000 ms wall,  4.000 ms CPU (5)
```

- **Parse** — lexing + parsing source text into an AST.
- **Compile** — type-checking and lowering the AST to bytecode.
- **Initialize** — running the prelude and module initializers before `main`/top-level.
- **Execute** — actual program execution time.
- **GC** — total time spent in garbage collection pauses (also listed separately below).

### Allocations & GC

```
Allocations: 5000 objects; estimated base/payload bytes: 250000; peak live objects: 1000; live: 200
GC: 5 collections; 800 reclaimed; total 2.000 ms; pauses p50/p95/max 0.400/0.800/1.200 ms
```

- **Estimated bytes** — `sizeof(Object)` × count + string/list payload sizes. Not RSS or actual heap size.
- **Peak live objects** — high-water mark of simultaneously live objects.
- **Reclaimed** — objects freed across all collections.
- **Pauses p50/p95/max** — GC stop-the-world pause times; p95 and max are the most important for latency.

### Operand stack & frame peak

```
Frames peak: 50; operand values peak: 200
```

- **Frames peak** — deepest call stack seen. High values may indicate unbounded recursion.
- **Operand values peak** — deepest bytecode operand stack. Relevant for large expression trees.

### Binding cache (verbose only)

```
Binding cache: 5000 hits, 100 misses, 50 invalidations
```

The VM caches resolved global/module bindings to avoid repeated environment walks.

- **Hits** — cache served the lookup directly.
- **Misses** — cache miss; environment walked.
- **Invalidations** — cache cleared due to a new binding being defined (e.g. module re-import). A high invalidation count can indicate repeated module loading.

### Arithmetic paths (verbose only)

```
Arithmetic paths: 2000 checked integer specializations, 500 float specializations, 100 generic operations
```

The VM specialises `+`, `-`, `*`, etc. for `Int` and `Float` to avoid boxing.

- **Checked integer specializations** — fast path: checked overflow arithmetic.
- **Float specializations** — fast IEEE-754 path.
- **Generic operations** — slow path (mixed types, strings, etc.). A high generic count relative to the total is a candidate for optimization.

### Opcodes (verbose only)

```
Opcodes:
  Tick: 50000
  LoadLocal: 20000
  ...
```

Raw count of each bytecode instruction executed. Useful for understanding where the VM is spending time at the lowest level. Key opcodes:

| Opcode | Meaning |
|---|---|
| `Tick` | Step-limit / profiler checkpoint; appears at the top of loops and function entries |
| `LoadLocal` / `StoreLocal` | Read/write a local variable (fast) |
| `LoadName` / `StoreName` | Read/write a binding by name in the environment chain (slower) |
| `Call` / `CallMethod` | Function/method call |
| `Binary` | Generic binary operator (add, compare, …) |
| `Member` | Property access |
| `IterInit` / `IterNext` | For-loop iteration setup/advance |

### Allocation kinds (verbose only)

```
Allocation kinds:
  List: 1000 objects, estimated 50000 bytes
  Instance: 200 objects, estimated 20000 bytes
```

Counts allocations by object type. High `List` counts can mean hot loops are allocating lists (e.g. old-style `Collections.range`). High `Instance` counts mean many class instances are being created.

### Function/native activations

```
Function/native activations (recorded exact counts; generators count resumes):
  countNeighbors: 100000
  Collections.range: 2400
```

Exact call counts (not sampled). Generators count each `resume`, not just the initial call. Use this to find unexpectedly hot functions.

### Native calls

```
Native calls (wall includes blocking; CPU is separate):
  Collections.range: 2400; 0.050 ms wall, 0.040 ms CPU
  Text.split: 100; 1.200 ms wall, 1.100 ms CPU
```

Wall time for natives includes any blocking I/O. CPU is the thread CPU time only. Use this to find natives that are doing heavy work or blocking.

### Allocation sites

```
Allocation sites (estimated, not RSS or exact container capacities):
  examples/game_of_life.k3:24:9: 2400 objects
```

The source location (`file:line:col`) responsible for the most object allocations. Cross-reference with allocation kinds to understand what is being allocated.

### Profiler bounds

```
Profiler bounds: dropped samples/frames/events/sites 0/0/0/0
```

If any of these are non-zero, the profiler hit its internal limits and the data is **incomplete**. Shorten your workload, increase `-O` level (to reduce allocations), or use `--profile-interval-ms` to reduce sample frequency.

---

## Visualization tools

| Tool | How to use |
|---|---|
| `tools/profile_view.py` | Parses the text report and opens a self-contained HTML dashboard |
| [Speedscope](https://www.speedscope.app) | Open the file from `--profile-output` for an interactive flame graph |
| [Perfetto](https://ui.perfetto.dev) | Open the file from `--profile-trace` for an event timeline |

```sh
# HTML dashboard from text report
dotk3 --profile --profile-vm verbose myprogram.k3 2>report.txt
python3 tools/profile_view.py report.txt               # auto-opens browser
python3 tools/profile_view.py report.txt -o out.html   # write to specific file
python3 tools/profile_view.py report.txt --json        # print parsed JSON

# Pipe directly
dotk3 --profile-vm verbose myprogram.k3 2>&1 >/dev/null | python3 tools/profile_view.py
```

---

## Tips

- Use `-O0` or `-O1` when profiling to see more granular call sites; `-O2` inlines aggressively.
- Avoid `--debug` when profiling; the debugger disables the fast execution path.
- `Collections.range` returns a lazy `Range` iterator — no allocation per element — so large ranges are cheap.
- High `LoadName`/`StoreName` opcode counts relative to `LoadLocal`/`StoreLocal` suggests the code could benefit from introducing local variables for frequently accessed globals/module members.
- If GC p95/max pauses are high, reducing allocations in hot loops (avoid building lists inside loops) is the most effective fix.
