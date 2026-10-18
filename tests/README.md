# DotK test files

Each `.k` test should include an expected-output block:

# EXPECTED
# first output line
# second output line
# END_EXPECTED

## Commands

Runnable walkthroughs for the runtime updates are in
[../examples/updates/README.md](../examples/updates/README.md). Their DotK
script uses the same expected-output convention as the tests; `make examples`
builds and runs the separate C walkthrough.

- Single test:
  - `./dotk.out --test tests/core/01_basic_arithmetic.k`
- Entire suite (recursive):
  - `./dotk.out --test-all`
- Entire suite, stop on first failure:
  - `./dotk.out --test-all --stop-on-fail`

## GC and Sanitizers

Build and run with collection on every growing allocation and UBSan:

```bash
make test-stress exec=benchmarks/bin/dotk-stress.out
```

For focused checks:

```bash
./benchmarks/bin/dotk-stress.out --test-all tests/collections --stop-on-fail
./benchmarks/bin/dotk-stress.out --test-all tests/gc --stop-on-fail
```

`make debug` enables ASan and UBSan. Stress builds are for correctness, not
performance comparisons. Catch expected runtime errors with `try`/`catch`
and check their messages using the normal expected-output block. The test
runner does not impose a timeout; use a process deadline for hang probes.

On macOS, tests using timed `Thread.join()` currently fail because that
operation is unsupported. Coroutine selector tests may also depend on which
ready channel is selected first. Report these separately from GC diagnostics.
If ASan stalls before `main`, verify the compiler runtime's OS compatibility;
a stalled sanitizer initialization is not a passing test.

## Modules

Embedding has a separate public-API host fixture:

```bash
make test-embedding
make MODE=stress test-embedding
```

It covers C/C++ linking, host output and diagnostics, signal preservation,
reentrancy/thread rejection, GC-rooted handles, owned source buffers,
compile-once reuse, isolated live VMs/imports, cross-owner rejection, contextual
native callbacks, cooperative task fairness/cancellation/errors, per-VM profiling,
budgets/interruption, repeated lifecycle, and allocation-failure cleanup. The CLI
`.k` runner does not execute this C test.

Tests are grouped by top-level folder under `tests` when printing results.

Examples:

- `tests/core/...` -> `CORE`
- `tests/kwargs/...` -> `KWARGS`
- `tests/classes/...` -> `CLASSES`

Current modules include:

- `core`
- `control`
- `functions`
- `collections`
- `classes`
- `strings`
- `operators`
- `kwargs`
- `gc`
- `modulesys`

## Example failing test

If a file prints `2` but expected says `3`, it fails:

```k
print(1 + 1)

# EXPECTED
# 3
# END_EXPECTED
```

The runner will print `[FAIL]`, plus EXPECTED vs ACTUAL output.
