# DotK test files

Each `.k` test should include an expected-output block:

# EXPECTED
# first output line
# second output line
# END_EXPECTED

## Commands

- Single test:
  - `./dotk.out --test tests/core/01_basic_arithmetic.k`
- Entire suite (recursive):
  - `./dotk.out --test-all`
- Entire suite, stop on first failure:
  - `./dotk.out --test-all --stop-on-fail`

## Modules

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
