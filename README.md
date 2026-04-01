# DotK 2.0

DotK is a bytecode virtual machine and language runtime written in C. The project is inspired by the Clox VM from *Crafting Interpreters*, then extended with modules, testing tooling, optimizer switches, and native interop.

## Highlights

- Interactive REPL and `.k` script execution
- Bytecode and execution-stack debug output flags
- Built-in importable native modules (`primitives`, `core`, `file`, `sql`, `io`, `http`)
- Dynamic loading of external shared-object modules (`.so`)
- Built-in test runner for single tests and full-suite recursive runs
- Optional benchmark workflow for baseline/candidate comparisons

## Requirements

The default Linux build links against:

- `gcc`
- `make`
- `libm`
- `libsqlite3`
- `libX11`
- `libdl`
- `pthread`

On Debian/Ubuntu, for example:

```bash
sudo apt install build-essential libsqlite3-dev libx11-dev
```

## Build

From the repository root:

```bash
make          # release build (default)
make debug    # ASAN/debug build
make clean
```

Binary output:

- `./dotk.out`

Optional install target:

```bash
sudo make install
```

This installs the executable as `dotk` in `/usr/local/bin`.

## Quick Start

```bash
./dotk.out                  # REPL
./dotk.out file.k           # run a script
./dotk.out -i file.k        # run script, then drop into REPL
./dotk.out --help           # show CLI options
```

Anything after the last flag/script is passed to the DotK program as argv-style arguments.

## Language Guide

DotK syntax is C/JS-like with dynamic typing, first-class functions, classes, modules, and built-in collection types.

### Comments

- `#` starts a line comment
- `~ ... ~` can be used as a block comment delimiter

### Variables and Primitive Values

```js
var x = 10
var y = 2.5
var ok = true
var n = null
var s = "hello"
```

Semicolons are optional (you can use them, but they are not required).

### `const` vs `var` vs Implicit Declaration

`var` declares a mutable variable:

```js
var count = 1
count = count + 1
```

`const` declares a non-reassignable binding:

```js
const limit = 9
print(limit)

try {
	limit = 10
} catch (e) {
	print(e.message) # Cannot assign to constant 'limit'.
}
```

Implicit declaration (assignment without `var`/`const`) is currently supported and behaves as an implicit global:

```js
a = 1
{
	b = 5
	b += 2
}
print(a) # 1
print(b) # 7 (still visible globally)
```

Prefer explicit `var`/`const` in new code to avoid accidental globals.

### Collections

List literal:

```js
var xs = {1, 2, 3}
print(xs[0])
print(xs[-1])
print(xs[1:])
```

List comprehensions:

```js
var doubled = {for x from {1, 2, 3, 4} => x * 2}
var evensSquared = {for x from {1, 2, 3, 4, 5, 6} if x % 2 == 0 => x * x}
print(doubled)      # [2,4,6,8]
print(evensSquared) # [4,16,36]
```

Map literal:

```js
var m = .{"a"::1, "b"::2}
print(m["a"])
m["c"] = 3
```

### Slices and Indexing

DotK supports negative indexing and Python-like slice ranges on indexable types:

```js
var xs = {10, 20, 30, 40, 50}
print(xs[-1])                # 50
print(",".join(xs[1:4]))     # 20,30,40
print(",".join(xs[:3]))      # 10,20,30
print(",".join(xs[2:]))      # 30,40,50

var base = "abcdef"
print(base[1:4])             # bcd
```

DotK also supports a step component using `@step` in the slice expression:

```js
var xs = {0, 1, 2, 3, 4, 5}
print(xs[:@2])               # every 2nd element: 0,2,4
print(xs[1:@2])              # start at index 1, step 2: 1,3,5
print(xs[:@-1])              # reverse traversal
```

You can also build a reusable slice spec and apply it later:

```js
var keySlice = 1:-2@1
print("hello"[keySlice])
```

### Control Flow

```js
if (x > 0) {
	print("positive")
} else {
	print("non-positive")
}

var i = 0
while (i < 3) {
	print(i)
	i = i + 1
}

for (var j = 0; j < 3; j += 1) {
	print(j)
}

for item in {"a", "b", "c"} {
	print(item)
}
```

`for x in iterable` uses the iterator protocol:

- call `iterable._iter_()` once to get an iterator object
- repeatedly call `iterator._next_()` until it returns `null`

Custom classes can participate by overriding `_iter_` and `_next_` (or `__iter__` and `__next__`).

### Functions and Closures

Named function:

```js
fn add(a, b) {
	return a + b
}
```

Anonymous function (block form):

```js
var f = fn(x) {
	return x * 2
}
```

Arrow-style function expression:

```js
var ys = {1, 2, 3}.map(fn(n) => n * 2)
```

Variadic function parameters are supported:

```js
fn sum(a, b, *rest) {
	var s = a + b
	var i = 0
	while (i < len(rest)) {
		s = s + rest[i]
		i = i + 1
	}
	return s
}
```

Lazy generators via `yield` are supported:

```js
fn evens(n) {
	for (var i = 0; i < n; i += 1) {
		yield i * 2
	}
}

var g = evens(4)
print(g._next_()) # 0
print(g._next_()) # 2

for x in evens(4) {
	print(x)
}
```

Functions that contain `yield` return a generator object. A generator implements `_iter_()` and `_next_()`, and `_next_()` returns `null` when iteration is complete.

### Keyword Arguments (Kwargs)

Keyword arguments are passed with `@name=value` and can reorder/fill named parameters:

```js
fn f(a, b, c) {
	print(a)
	print(b)
	print(c)
}

f(10, @c=30, @b=20)
# 10, 20, 30
```

Kwargs also work with variadics; extra positional values still flow into `*rest`:

```js
fn g(a, b, *rest) {
	print(a)
	print(b)
	print(len(rest))
}

g(100, 200, 1, 2, 3, @b=20, @a=10)
# a=10, b=20, len(rest)=5
```

Native functions can also receive kwargs packs/maps:

```js
print(len(@x=1, @y=2))       # 2
print(len(@x=1, @y=2, @z=3)) # 3
```

### Classes and Inheritance

```js
class A {
	init(x) {
		this.x = x
	}

	get() {
		return this.x
	}
}

class B<A> {
	init(x, y) {
		super.init(x)
		this.y = y
	}

	sum() {
		return this.x + this.y
	}
}

var b = B(2, 5)
print(b.get())
print(b.sum())
```

### Operator Overloading

Custom classes can override operators through method hooks:

- comparison: `_eq_`, `_lt_`, `_gt_`
- indexing: `_get_`, `_set_`
- size/hash/string: `_size_`, `_hash_`, `toStr`

Example:

```js
class Box {
	init(v) {
		this.v = v
	}

	_eq_(other) {
		return this.v == other.v
	}

	_lt_(other) {
		return this.v < other.v
	}

	_gt_(other) {
		return this.v > other.v
	}
}

var a = Box(10)
var b = Box(10)
var c = Box(20)
print(a == b)
print(a < c)
print(c > b)
```

Python-style dunder aliases are also supported:

- `toStr` ↔ `__str__`
- `_eq_` ↔ `__eq__`
- `_lt_` ↔ `__lt__`
- `_gt_` ↔ `__gt__`
- `_get_` ↔ `__getitem__`
- `_set_` ↔ `__setitem__`
- `_size_` ↔ `__len__`
- `_hash_` ↔ `__hash__`

### Error Handling

```js
try {
	var xs = {1}
	print(xs[5])
} catch (e) {
	print(type(e))
	print(e.message)
}
```

### Strings and Interpolation

```js
var name = "DotK"
var n = 42

print(f"{name}:{n}")
print("hello ${}!".f(name))
```

### Prefixed Strings and Numbers

DotK supports prefixed string literals for byte-oriented/text-decoding workflows:

```js
var a = b"ABC"
var h = x"41 42 43"
var n = bin"01000001 01000010 01000011"

print(a)                  # ABC
print(h)                  # ABC
print(n)                  # ABC
print(a == h && h == n)   # true
```

- `b"..."`: byte-style literal
- `x"..."`: hex-encoded byte sequence
- `bin"..."`: binary-encoded byte sequence

Prefixed numeric literals are also supported:

```py
print(0xFF)                          # 255
print(0b101010)                      # 42
print(0y255)                         # 255
print((0x0F | 0b00110000) == 63)     # true
```

- `0x...`: hexadecimal
- `0b...`: binary
- `0y...`: explicit decimal form

### Modules and Imports

Import full module (namespace):

```py
import modules.mathx as mathx
print(mathx.PI)
print(mathx.add(3, 4))
```

Bare identifier imports default to aliasing the module namespace:

```py
import stdlib
var xs = stdlib.LinkedList()
```

This is equivalent to:

```py
import stdlib as stdlib
```

Import specific exports:

```py
from modules.mathx import add, PI
print(add(2, 3))
print(PI)
```

Import all exports from a module:

```py
from modules.mathx import *
print(PI)
print(add(10, 5))
```

Path/string imports are still supported:

```py
import "modules/mathx" as mathx
from "modules/mathx.k" import add
```

Authoring a module (declaration + export list):

```py
module mathx;

fn add(a, b) {
	return a + b;
}

var PI = 3;

export add, PI;
```

Export constants directly:

```py
module constx;

export const ANSWER = 42;
```

Constness is preserved through imports:

```py
from constx import ANSWER
# ANSWER = 99  # runtime error: constant assignment
```

Namespace access is object-style (`mod.name`). Map key access (`map.key`) is not used for module namespaces.

### Built-in Functionality

Out of the box, DotK provides:

- core utility built-ins (e.g., `len`, `int`, `type`, process helpers)
- object model primitives (`Object`, `String`, `List`, `HashMap`, `StringBuilder`, etc.)
- built-in modules for file I/O, SQL, and HTTP (`file` is preloaded; others are import-on-demand)
- native module loading via `import` or `loadLib(path)`

### `lang.k` Standard Helpers

The repository includes a standard helper file at [lang.k](lang.k) that is commonly imported from scripts:

```py
from "lang" import min, max, abs, with, __hierarchy__, Ansi
```

Notable helpers in [lang.k](lang.k):

- `min(a, b)`, `max(a, b)`, `abs(a)` numeric helpers
- `with(closable, f)` utility that executes `f`, then attempts `closable.close()`
- `__hierarchy__(ClassObj)` inspects class inheritance chain
- `Ansi` class constants for terminal colors/styles

For concrete language examples, browse the test suite in [tests](tests), especially [tests/core](tests/core), [tests/functions](tests/functions), [tests/classes](tests/classes), [tests/modulesys](tests/modulesys), and [tests/collections](tests/collections).

## CLI Flags

- `-i` run script, then start interactive shell
- `--pbytecode` print compiled bytecode
- `--pexec` print VM execution stack trace output
- `--debug` start interactive VM debugger
- `-O0`, `-O1`, `-O2` set optimization level (`-O1` default)
- `--opt <n>` or `--opt=<n>` set optimization level
- `--test <file.k>` run one test file against its expected block
- `--test-all [dir]` run all `.k` tests recursively (default: `tests`)
- `--stop-on-fail` stop at first failure (with `--test-all`)

## CLI Debugger (`--debug`)

Start a program in interactive debugger mode:

```bash
./dotk.out --debug your_file.k
```

When execution pauses, DotK shows the current file/line/column and opens a `(dbg)` prompt.

Typical workflow:

```text
(dbg) b 42
(dbg) c
... breakpoint hit ...
(dbg) locals
(dbg) v myVar
(dbg) bt
(dbg) s
(dbg) c
```

Core debugger commands:

- `c` / `continue`: continue execution
- `s` / `step`, `n` / `next`: execute one instruction
- `b <line>`: add line breakpoint (any file)
- `b <file>:<line>`: add file-specific line breakpoint
- `bo <offset>`: add bytecode offset breakpoint
- `bl` / `breaks`: list breakpoints
- `del <id>`: delete breakpoint by id
- `clear`: clear all breakpoints
- `p` / `pos`: print current position
- `l` / `line`: print current source line
- `dis [n]`: disassemble current instruction(s)
- `stack`: print stack grouped by frame
- `locals` / `loc`: list locals
- `locals <name|index>`, `v <name|index>`, `inspect <name|index>`: inspect one local
- `watch` / `w`: list watches
- `watch <name|index>`: add watch
- `unwatch <id|expr>`: remove watch
- `clearwatch`: remove all watches
- `bt`: backtrace
- `h` / `help`: show help
- `q` / `quit`: abort execution

You can also pre-seed debugger commands non-interactively with `DOTK_DEBUG_CMDS` (semicolon-separated):

```bash
DOTK_DEBUG_CMDS="b 12;c;locals;bt;q" ./dotk.out --debug your_file.k
```

## Testing

Run tests with:

```bash
make test
make test-fast
./dotk.out --test tests/core/01_basic_arithmetic.k
./dotk.out --test-all tests
```

Each test file should include an expected-output block:

```py
# EXPECTED
# first output line
# second output line
# END_EXPECTED
```

## Built-in Modules

Registered built-in modules:

- `primitives`
- `core`
- `file`
- `sql`
- `io`
- `http`

Default startup behavior:

- `primitives`, `core`, and `file` are preloaded
- `sql`, `io`, and `http` are loaded when imported

Example imports:

```py
import io
import http
from sql import SQL
```

## External Native Modules (`.so`)

DotK resolves shared modules from:

- `./`
- `./modules/`
- `/usr/local/lib/dotk/`
- `/usr/lib/dotk/`

So both forms work:

```py
import "json_module"
import "json_module.so"
```

Build helper targets in this repo:

```bash
make json-module
make raylib-module
make sdl-module
make x11-module
```

Native module API entrypoint:

- preferred: `bool dotk_init_module(const DotKNativeApi* api)`
- legacy fallback: `void init(DefineNativeFn, DefineNativeClassFn)`

Reference: [src/include/native_api.h](src/include/native_api.h).

## Benchmarks

Available make targets:

```bash
make save-baseline
make save-candidate
make bench
make bench-long
make bench-legacy
make bench-nan
```

Workloads are listed in [benchmarks/workloads.txt](benchmarks/workloads.txt).

## Project Layout

- [src](src): VM, compiler, runtime, and built-in module implementation
- [src/include](src/include): internal headers and native API surface
- [modules](modules): example external native modules (`.so` builds)
- [tests](tests): language and runtime test suite
- [benchmarks](benchmarks): workload definitions and binary snapshots
- [dotk-syntax-highlighting](dotk-syntax-highlighting): VS Code syntax grammar package
- root `*.k` files: examples/demos (e.g., HTTP, sockets, graphics, games)

## Notes

- This repository includes graphics-related examples and module targets (`X11`, `raylib`, `SDL2`) that may need additional system packages.
- The main interpreter executable is built from the default make target and emitted as `dotk.out`.