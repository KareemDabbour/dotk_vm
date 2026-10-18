# DotK 3 Experimental Alpha

This is a siloed language implementation, not a DotK 2 mode or a finished rewrite.
It does not compile, include, or link the parent VM. Existing `.k` programs and
the parent Makefile are unchanged. New programs use `.k3`. Examples, module
resolution, tests and documentation use this extension consistently.

The frontend/runtime are C++17; the embedding header is C-compatible. The frontend
retains owned source-spanned AST metadata, but execution is bytecode-only. Scripts,
REPL submissions, methods, closures, defaults, handlers and generators all execute
compiled instructions. Slot bytecode and lexical control bytecode share the same
checked runtime and expression VM; there is no AST execution engine or fallback.
There is no JIT.

## Build and Run

```bash
make -C dotk3 CXX=clang++ CC=clang all test
dotk3/build/release/dotk3 dotk3/examples/core.k3
dotk3/build/release/dotk3 dotk3/examples/data.k3
dotk3/build/release/dotk3 dotk3/examples/generators.k3
dotk3/build/release/dotk3 dotk3/examples/modules.k3
dotk3/build/release/dotk3 --check dotk3/examples/core.k3
dotk3/build/release/dotk3 --ast dotk3/examples/data.k3
make -C dotk3 CXX=clang++ CC=clang MODE=stress all test
```

Clang and GCC are supported toolchain targets; this batch was verified with
Clang on macOS. The CLI now links libedit for terminal editing/history (provided
by the macOS SDK; Linux needs libedit development headers/library). The embedding
library and LSP do not link libedit. The Windows CLI currently falls back to plain
line input. No CMake is required. `MODE=stress` currently
means unoptimized UBSan, not collect-on-every-allocation or ASan. The C fixture
links against `build/<mode>/libdotk3.a` without the CLI entrypoint.

## Static Typing

DotK 3 is statically typed with local inference. Every program, module, REPL
submission and LSP document is type-checked before it runs; a program with type
errors never executes. Annotations are erased at run time (`Int` arithmetic and
`Float` arithmetic use typed instructions and slot-only frames), so there are no
run-time contracts for checked code.

```text
fn first<T>(items: List<T>) -> T? { return items.find(fn(item) => true) }
type Pair<T> = Map<String, T>
enum Shape { Circle(Float), Empty }              // payload-less variants are values
class Stack<T extends Object> { var items: List<T> = [] }

let xs = [1, 2, 3]                                // List<Int>, inferred
let ys: List<Float> = [1, 2]                      // Int widens to Float
let n: Int? = null
if n != null { System.print(n + 1) }              // flow narrowing
let boxed = Json.parse("[1]") as List<Object?>    // checked cast (erased generics warn)
System.print(xs[0]?.toString() ?? "none")         // ?. and ?? on nullable values
```

- Top type is `Object` (`Object?` with null). Unions `A | B`, `T?`, enums and
  `type` aliases are supported; so are `fn(A) -> R` function types, `<T>` generics
  with `T extends Bound`, and wildcards `? extends T` / `? super T`. Generics are
  erased and invariant; `Int` widens to `Float` implicitly.
- `is` narrows, `as` is a checked cast (`Error.Kind.Type` on failure), `x!` asserts
  non-null, and `x?.m()` short-circuits on null.
- Named function parameters and fields need annotations. Locals, lambda
  parameters, lambda results and non-recursive function results are inferred,
  using the expected type where there is one.
- Closures capture by value, so captured locals must be effectively final.
- Collections remain subclassable (`class Inventory extends List<String>`).
- `String + value` concatenates by calling the value's `toString`.
- `Json.parse` and `Reflection.binding` return `Object?`; use `as` to narrow.
- `--no-check` reports type errors as warnings and runs the program anyway, without
  any type information; ill-typed code fails with ordinary run-time errors.
- The embedding C API type-checks `dk3_eval` sources too. Natives declare their
  signature as a declaration tail, e.g. `dk3_define_native(vm, "host.add", "(a: Int, b: Int) -> Int", fn, ud)`
  or the `signature` field of `DK3NativeExport` (`DK3_MODULE_ABI_VERSION` 2).
- There are no run-time annotation checks: later sections that talk about
  "contracts" describe what is now a compile-time error. Values a host inserts
  into script collections are not re-validated.

## New Basics

### Execution and Optimization

```bash
dotk3/build/release/dotk3 -O2 program.k3
dotk3/build/release/dotk3 -O0 program.k3
dotk3/build/release/dotk3 --engine bytecode program.k3
```

Bytecode is the main/default engine; no engine flag is normally needed. `-O0`
selects baseline bytecode, `-O1` adds checked integer arithmetic/comparison
specialization, and default `-O2` additionally caches lexical/global binding lookup
with invalidation when declarations change. These levels preserve overflow, strict
Bool, mutation/type checks, evaluation order, source spans and execution budgets.
They control script-bytecode optimization, separately from the C++ build's flags.
There is no fake/aggressive `-O3` that removes safety checks.

The AST is only frontend/source/type metadata, not an interpreter. `--engine ast`
is rejected. Closures, methods/constructors, defaults/rest parameters, iterable and
C-style for, handlers/defers, generators, slices, matches, interpolation and compound
assignments lower to dedicated bytecode operations. Lexical frames retain captured
environments and cleanup/handler continuations; eligible noncapturing bodies use
slot frames. Debugger checkpoints work on bytecode source metadata as well.

Both iterable and regular C-style loops support break/continue:

```text
for (var index = 0; index < 10; index++) {
  if index == 2 { continue }
  if index == 7 { break }
  System.print(index)
}
for item in [1, 2, 3] { System.print(item) }
```

C-style initializer/condition/increment clauses are optional; an omitted condition
is true. Initializer bindings are loop-scoped. Continue executes the increment,
then retests; break skips the increment. Postfix ++/-- are update statements (not
value-producing expressions); +=/-= also work. Finally/defer cleanup is preserved
across loop transfers, including suspended generators.

Compiled calls use reusable frame-owned slots/operand stacks and direct return/jump
instructions rather than per-call heap environments and normal-return exceptions.
GC traces frame values/contracts. Allocation pressure now requests collection at
safe statement/instruction boundaries; `GC.collect()` still requests one explicitly, but
long-running allocation loops no longer require hand-inserted checkpoints. Object
limits still apply and external/native payloads remain trusted. Allocation itself
does not collect unrooted host temporaries.

See [the performance report](../benchmarks/results/v3-bytecode-report.md) for
same-source before/after measurements and remaining coverage/costs.

```text
let immutable = 42
var mutable = 0
mutable += 1

let values: List<Int> = [1, 2, 3]
let mapping = {"answer": immutable}
let pair = (1, "one")
System.print(values[::-1])

fn add(first: Int, second: Int = 2) -> Int {
    return first + second
}
System.print(add(second: 3, first: 1))
```

- `let` prevents rebinding, not mutation of a referenced list/map. `var` permits
  rebinding. Assignment never declares a name. Duplicate same-scope declarations
  fail; nested scopes can shadow outer bindings.
- Int is checked signed 64-bit, including exact literals above 2^53. Float is
  double. `+`, `-`, `*`, and negation check integer overflow. `/` is floating
  division; `//` and `%` are floor division/modulo (Int results for Int operands,
  Float results with any Float operand; `%` takes the divisor's sign). `**` is
  right-associative exponentiation that binds tighter than unary minus (`-2 ** 2`
  is `-4`); Int ** Int needs a non-negative exponent and checks overflow, any Float
  operand uses `pow` and rejects nonfinite results. Potentially lossy mixed
  arithmetic/order comparisons require explicit `Math.toFloat(...)`. Mixed numeric
  equality compares exactly representable values without rounding an Int first.
- Conditions/logical operands require Bool. Use `value != null`, not truthiness.
  `??` provides null fallback. Concatenation requires matching String/Bytes/List
  types; other values require explicit `Text.str(...)`.
- List/map values are mutable; tuples and record fields are non-reassignable.
  List/map equality is identity-based; tuple/record/enum equality is structural.
  Record fields can reference mutable objects, so this is not deep immutability.
- Scalar negative indexes use length+index. Slices are `start:end:step`,
  end-exclusive, with distinct omitted bounds and Python-style negative steps.
  Slice expressions and call arguments evaluate left-to-right. Zero steps fail.
- String is validated UTF-8 and indexes code points, not bytes or graphemes.
  Bytes is distinct, such as `b"a\0b"`; byte indexing returns Int. Use
  `encode`/`decode`. Unicode normalization and grapheme segmentation are deferred.
- Named calls use `name: value`; duplicate positional/named binding fails.
  Defaults are evaluated each call and can use earlier parameters. `*rest`
  receives a list. Parameters are immutable bindings.

## Object, Interpolation and IO Foundations

Not every value is a heap object: Int/Float/Bool remain unboxed variant values.
Object is the common logical non-null annotation and open class root, exported
bare and as Object. User classes and collection classes inherit the root;
primitives/records/resources may satisfy Object without boxing or losing their
existing equality semantics. `Object?` accepts null and is the top type; there is
no `Any`. Qualified/imported Object aliases have the same contract.

```text
let name: String = "Ada"
let score: Object = 15
System.print(f"Hello {name}: {score + 2}; braces {{score}}")
```

F-prefixed single/double/triple strings accept expression replacements and escaped
{{/}} literal braces. Replacements parse as ordinary expressions, evaluate once
left-to-right in the current scope, and use toString conversion with normal GC/budget
rules. Missing names, empty/unclosed replacements and extra statements fail. This
version does not implement Python format specifications, !r/!s flags or arbitrary
template execution at runtime; placeholder diagnostics identify the containing
literal. Source/editor checking never evaluates replacements.

Error is also exported bare for Error.Kind. Caught runtime errors carry nominal
enum values such as Error.Kind.Index, Type, IO, Access, Capability, Json, Native,
Application, Limit and Cancelled. Compare error.kind to an enum value, not a String.
Categories retain readable lowercase display. Error(Error.Kind.Application, message)
constructs a user error. Legacy String kinds are temporarily normalized to known
enum categories; unknown custom strings normalize to Application. Runtime limits
and cancellation retain their existing uncatchable behavior.

IO.write(String|Bytes) writes without an implicit newline through the host output
callback. IO.readLine() returns String? (null at EOF) through an explicit host
callback with a 64-KiB default input bound. IO.Files aliases the same readonly Files
capability, not a new grant. Time.monotonicMillis() and Time.sleep(Int) are
callback-based timing services, denied in embedding defaults; delay is bounded by
the host (default maximum 1000 ms). The CLI supplies line input, monotonic clock,
bounded delay and flushing output adapters. Source loaded from stdin consumes that
stream before execution; use a script file or -e for interactive line input.

Line reads/delays can block and are not cooperative waits or wall-clock preemption.
Raw/nonblocking terminal keys, bounded TCP and a small HTTP/1.1 module now work as
described below. Interactive programs need not copy the old app architecture.
See [examples/foundations.k3](examples/foundations.k3); pass an argument to enable
its optional line-input demonstration.

## User Input

Simple line input works without raw mode:

```text
IO.write("Name: ")
let name: String? = IO.readLine()
System.print(f"Hello {name ?? 'Guest'}")
```

For an interactive event loop, IO.Terminal.open/close control one host terminal
session. IO.Terminal.readKey(timeoutMs = 0) returns an Int byte or null on timeout;
zero polls without waiting. A positive timeout waits up to the host limit (default
1000 ms), not forever. EOF/errors are structured IO errors, not timeouts. Keys are
bytes, so arrow/UTF-8 sequences require application decoding. isOpen reports state;
open/close are idempotent. Line input is rejected while raw mode is active.

```text
IO.Terminal.open()
defer IO.Terminal.close()
let key: Int? = IO.Terminal.readKey(0)
if key != null { System.print(f"key={key}") }
Tasks.pump(1)
```

Polling between task turns gives async-style interaction without a blocking read;
it does not introduce await/futures or background input threads. Native timed reads
still occupy that VM until they return. Open is denied unless both terminal callbacks
are installed. The POSIX CLI enables a termios/poll adapter only on real terminal
stdin, not piped source, debugger mode or Windows. Non-TTY embedding hosts may
provide their own callbacks; default VMs remain denied.

Use defer close for script cleanup. The CLI additionally restores termios before a
REPL prompt (including error recovery), on normal shutdown/reset, and via adapter
RAII as fallback. Raw mode disables signal-key handling so Ctrl-C is byte3; programs
should handle it as a quit request. This does not promise recovery from SIGKILL,
abrupt process termination or arbitrary signal handlers. Cursor/display state is
the application's responsibility; callbacks/destruction require host synchronization.
Try [examples/input_loop.k3](examples/input_loop.k3), which polls while cooperative
work runs, then cancels work and restores input on q/Ctrl-C:

```bash
dotk3/build/release/dotk3 dotk3/examples/input_loop.k3 interactive
```

## Tetris

```bash
dotk3/build/release/dotk3 dotk3/examples/tetris.k3 play
```

[examples/tetris_model.k3](examples/tetris_model.k3) exports the board, collision,
clockwise rotation with simple horizontal kicks, piece locking, line compaction,
scoring and spawn/game-over model. [examples/tetris.k3](examples/tetris.k3) renders
colored terminal cells, a next-piece preview and timed gravity. Use a/d to move,
w to rotate, s to lower, Space to drop, p to pause, r to restart after game-over,
and q/Ctrl-C to quit. Raw input/cursor state is restored on normal quit/error.
This reuses the legacy game's rules; it does not claim SRS, seven-bag, hold or
full modern Tetris guideline behavior. No argument runs a deterministic smoke drop.

The CLI disables the step limit by default so games/services and other long-lived
scripts do not need an opt-out. Prefix `--steps N` opts into a finite execution
budget; `--steps 0` also explicitly selects the default. Embedding Config/C SDK
defaults retain their million-step host budget and can choose zero separately.
This does not disable the object budget or make native calls preemptible. Examples collect
between frames/requests/loop turns; arbitrary scripts do not automatically collect
on every allocation. Limits and native capabilities are not a hostile-code sandbox.

## TCP and HTTP

Sockets and IO.Sockets are the same readonly namespace. No networking is granted
by default. Prefix CLI arguments with `--net` to enable a POSIX numeric-IPv4 TCP
adapter; it can be combined with `--steps` and `--files-root`. DNS, IPv6, UDP, TLS,
background networking threads and task-level await are not implemented.

| Operation | Result |
| --- | --- |
| `Sockets.listen(host: String, port: Int)` | Listener Resource; port0 chooses an ephemeral port. |
| `Sockets.connect(host, port, timeoutMs = 0)` | Connected Resource or IO error. |
| `Sockets.accept(listener, timeoutMs = 0)` | Resource or null on timeout. |
| `Sockets.receive(socket, maximum, timeoutMs = 0)` | Bytes, null on timeout, empty Bytes at EOF. |
| `Sockets.send(socket, String\|Bytes, timeoutMs = 0)` | Whole-write byte count; failure closes the socket. |
| `Sockets.port(socket)` | Local bound port, not the remote peer port. |
| `Sockets.close(socket)` | Idempotent Null-returning close. |

Zero waits poll; positive waits are bounded by the host (default1000ms). A default
transfer is limited to 1MiB. TCP can split/coalesce messages: one receive is not one
message. Partial-send side effects cannot be rolled back; a failed write closes
the connection. The CLI uses nonblocking descriptors, bounded poll deadlines and
SIGPIPE suppression. It retains at most256 open sockets independently of wrapper
GC, closes them at CLI shutdown and reclaims closed registry entries on adoption.
Use defer close instead of depending on GC. C++ Config providers own their domain,
shutdown/thread synchronization and policy; the C SDK config ABI is unchanged and
does not yet expose a network-provider grant API.

Http and IO.Http use the pinned picohttpparser library for syntax plus strict
bounded framing. These are socket-based exchange helpers, not URL/TLS clients:

```text
let peer = Sockets.connect("127.0.0.1", 8080, 1000)
defer Sockets.close(peer)
Http.writeRequest(peer, "GET", "/health")
let response = Http.readResponse(peer)
if response != null { System.print(response["status"]); IO.write(response["body"]) }
```

- `readRequest(socket, timeoutMs = 1000) -> Map?` has method, target, headers and body.
- `readResponse(socket, timeoutMs = 1000) -> Map?` has status, reason, headers and body.
- Header keys are lowercase; header values are validated Strings. Body is Bytes,
  so binary/UTF-8 bodies use exact byte Content-Length, not code-point length.
- `writeRequest(socket, method, target, body = "", timeoutMs = 1000) -> Int` writes
  an origin-form HTTP/1.1 request with Host: localhost and Connection: close.
  Supported outgoing methods are GET/POST/PUT/PATCH/DELETE/OPTIONS.
- `writeResponse(socket, status, body, contentType = "text/plain; charset=utf-8",
  timeoutMs = 1000) -> Int` writes a bounded response with Connection: close.
- Null reads mean no bytes before timeout/EOF. A partial message followed by EOF
  or timeout is an IO error and closes the connection. Use Sockets for the precise
  timeout-versus-EOF distinction. Each HTTP read has one overall deadline.
- One exchange per connection only. Header limit16KiB/64 entries, wire-message
  limit1MiB by default. HTTP/1.0, interim1xx responses, duplicate/folded headers,
  LF-only framing, chunked/Transfer-Encoding, Expect, ambiguous lengths and
  buffered pipelining/trailing bytes are rejected. Responses require Content-Length
  except empty204/304; writers reject bodies for those statuses. There is no custom
  header API, keep-alive, compression, redirects, cookies or automatic retry.
  Peer-controlled framing/size violations are recoverable IO errors, not fatal VM
  allocation/step-limit failures, so the example service can reject and continue.

Run the API service and a client in separate terminals:

```bash
dotk3/build/release/dotk3 --net dotk3/examples/webserver.k3 serve 8080
dotk3/build/release/dotk3 --net dotk3/examples/http_client.k3 127.0.0.1 8080 /health
```

The service binds loopback and serves GET /, JSON GET /health, binary POST /echo
and404 otherwise. It handles connections sequentially with bounded request IO;
it is a working API example, not a concurrent production web framework/static-file
site. An optional request count after the port makes a finite run for tests.

## Requests (HTTP/HTTPS client)

`Requests` queries web APIs by URL. Like sockets it is denied by default and
enabled with `--net`; the CLI backs it with libcurl (link with `-lcurl`), so it
supports HTTPS, DNS, redirects, gzip and chunked responses.

```text
let response = Requests.get("https://api.github.com/repos/python/cpython/issues", {
    "params": {"per_page": 2, "state": "open"},
    "headers": {"Accept": "application/json"}
})
if response["ok"] { System.print(Requests.json(response)[0]["title"]) }
let created = Requests.post("https://httpbin.org/post", {"json": {"name": "Ada"}})
```

| Function | Notes |
| --- | --- |
| `Requests.get/head/delete/post/put/patch(url, options = {})` | Method helpers. |
| `Requests.request(method, url, options = {})` | GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS. |
| `Requests.json(response)` | Parses `response["body"]` with `Json.parse`. |

Options (a Map; unknown keys are an error): `headers` (Map of String), `params`
(Map of String/Int/Float/Bool or List of them, percent-encoded onto the URL),
`body` (String or Bytes), `json` (any JSON value; sets Content-Type unless given),
`timeoutMs` (default 10000, host maximum 30000) and `redirects` (Bool or 0..10,
default 5).

The response is a Map with `status`, `ok` (2xx), `reason` (empty for HTTP/2),
`url` (after redirects), `headers` (lowercase keys; repeated headers joined with
`, `) and `body` (String, or Bytes when the payload is not valid UTF-8). Non-2xx
statuses return normally; connection, TLS, timeout and protocol failures throw an
`Error.Kind.IO` error and oversized responses throw `Error.Kind.Limit` (default
limit 1MiB). Only http/https URLs are allowed, including across redirects, and
Content-Length/Transfer-Encoding cannot be set by scripts. Calls block the VM and
are not preemptible. Embedders provide `Config::httpRequest`; without it the
module throws a Capability error. [examples/requests.k3](examples/requests.k3) is a
small command-line client.

## Chat

```bash
dotk3/build/release/dotk3 --net dotk3/examples/chat_server.k3 serve 9090
dotk3/build/release/dotk3 --net dotk3/examples/chat_client.k3 connect 127.0.0.1 9090
dotk3/build/release/dotk3 --net dotk3/examples/chat_client.k3 send 127.0.0.1 9090 hello
```

The loopback server polls up to32 peers, accepts new clients while reading/broadcasting
and removes failed/disconnected clients without taking down the remaining peers.
UTF-8 is decoded only after complete LF/CRLF-framed lines, with an8KiB pending
buffer and2KiB message limit. `/nick NAME` changes the label and `/quit` disconnects.
Control bytes are rejected. The raw/polled terminal client displays incoming chat
while composing ASCII input, with Enter, Backspace and Ctrl-C; incoming chat and
the one-shot sender support UTF-8. This is bounded cooperative polling, not await,
authentication, encryption, persistence, rate limiting or an internet-ready service.
An optional message count after the server port gives finite test runs. With no
arguments, network examples print usage and do not grant/open networking.

## Pac-Man and Browser UI

```bash
dotk3/build/release/dotk3 dotk3/examples/pacman.k3 play
dotk3/build/release/dotk3 --net --files-root dotk3/examples/browser_ui dotk3/examples/browser_server.k3 serve 8081
```

Open http://127.0.0.1:8081/ for the browser arcade. This is a **separate server**
from the API-only webserver example; that original example remains unchanged.
The UI serves an allowlisted HTML/CSS/JS/icon/font bundle through the readonly
Files capability. No Node, CDN or frontend build is needed to run the browser app.
Assets are loaded at startup; restart the server after changing them. The port can
be0 for a free ephemeral port, printed at readiness. A final optional request count
produces a finite run for process tests.

[examples/pacman_model.k3](examples/pacman_model.k3) ports the legacy maze and its
cached-neighbor/BFS navigation into a reusable DotK 3 model. Buffered turns,
pellets/power pellets, chasing/random ghosts, edible-ghost scores, cooldowns,
lives/respawn, pause, win/game-over, restart and next mazes are authoritative in
DotK 3, not JavaScript. This is a playable maze-chase implementation, not an exact
original arcade ROM/AI/timing/tunnel emulation. Best score is session-local.

The terminal app uses arrows/WASD, Space to start, p to pause, r to restart,
n for the next cleared maze and q/Ctrl-C to quit, with a21-row player-following
view and normal raw/cursor cleanup. With no arguments it runs a deterministic smoke.
The browser renders the actual model on canvas with keyboard/swipe/direction
controls, pause/resume/restart, optional sound, score/lives/pellets/level HUD,
connection recovery and proportional desktop/mobile layout. Switching away
pauses active play. Browser assets use local pinned Lucide and Oxanium assets.

GET /api/state returns the model snapshot; POST /api/action accepts JSON
`{"action":"right"}` (or left/up/down/start/pause/restart/next). Invalid JSON/actions
return400 without killing the service. All browser clients share one local session;
this is not multiplayer isolation/authentication. Bounded sequential HTTP handling
and cooperative130ms ticks are intended for a local demo, not a production game
backend or hard real-time scheduler. The service binds loopback only.

## Conversion and Operator Hooks

Classes and records can define a public synchronous `fn toString() -> String`.
Direct calls work normally; Text.str(value), System.print arguments and REPL result
echo use the hook when present. Inherited methods work, including private state
read inside the declaring method. Returned values must be actual Strings; recursive
conversion of the same object fails rather than recursing forever. Hook calls retain
GC roots and use ordinary budgets/errors. Undocumented/no-hook values keep their
structural formatting. Nested container formatting remains structural rather than
implicitly invoking hooks on every child.

Debugger snapshots, reflection previews and error diagnostics never invoke toString.
Script printing can have method side effects, but failed conversion does not emit
the final print buffer; hook side effects are not rolled back. Json serialization
does not use this hook or become a custom-record codec.

Named methods opt into operator dispatch without new declaration syntax:

| Operator | Method |
| --- | --- |
| `+`, `-`, `*`, `/`, `//`, `%`, `**` | `__add__`, `__sub__`, `__mul__`, `__div__`, `__floordiv__`, `__mod__`, `__pow__` |
| `<`, `<=`, `>`, `>=` | `__lt__`, `__le__`, `__gt__`, `__ge__` |
| Unary `-`, unary `+` | `__neg__`, `__pos__` |
| `value[key]`, `value[key] = item` | `__getitem__`, `__setitem__` |

```text
record Vector {
  x: Int
  fn __add__(other: Vector) -> Vector { return Vector(self.x + other.x) }
  fn __mul__(amount: Int) -> Vector { return Vector(self.x * amount) }
  fn toString() -> String { return "Vector(" + Text.str(self.x) + ")" }
}
var value = Vector(2) + Vector(3)
value *= 2
System.print(value)
```

Binary hooks receive the right operand and dispatch only from the left operand's
class/record; unary hooks receive no arguments. Index getters receive key; setters
receive key/value and must return Null. Indexed compound assignments evaluate
receiver/key once, get the old value, compute the result and invoke the setter.
They must be public synchronous
instance methods of the expected arity, without rest parameters. Comparison results
must be actual Bool. Normal method parameter/return contracts, inheritance, GC and
visibility still apply. Compound assignments use the corresponding binary hook,
then enforce the destination contract. Existing primitive operators remain unchanged.

The checker validates known hook signatures/operands/results, and the LSP discovers
declared result members without running methods. No reflected/right-hand fallback,
equality/hash/truthiness overload, or automatic String concatenation is
introduced. Equality and collection membership retain existing identity/structural
rules. See [examples/operators.k3](examples/operators.k3).

## Anonymous Functions

Anonymous functions use `fn` without a name, with a block or expression body:

```text
let add = fn(first: Int, second: Int = 2) -> Int {
  return first + second
}
let double: fn(Int) -> Int = fn(value: Int) -> Int => value * 2
System.print(add(second: 3, first: 1), double(4))
fn(value: Int) -> Null { System.print(value) }(7)
```

They capture by value: a captured local must be effectively final (use a class or
list for shared mutable state). They support ordinary annotations/defaults/named/rest
arguments, and may yield as generators. Captured values remain GC-rooted when a
closure escapes its defining function. Leading block-body docstrings are available
through Reflection.doc. Standalone function expressions and immediate calls work
in scripts and the REPL; `=>` returns its expression without an explicit return.

Function types are written `fn(A, B) -> R`; the typer checks every call and
assignment against them. LSP hover shows known anonymous signatures; typed result/member discovery and
parameter scopes work inside block/arrow bodies without leaking past the function.
There is no alternate bare `(x) => ...` syntax. See
[examples/anonymous_functions.k3](examples/anonymous_functions.k3).

## Core Services

Core functions are no longer free-floating globals. The built-in modules are
explicit readonly namespaces, and old flat aliases are intentionally removed:

| Namespace | Members |
| --- | --- |
| `System` | `print`, `args`, `platform`, `env` |
| `GC` | `collect`, `stats` |
| `Time` | `unixMillis`, `monotonicMillis`, `sleep`, `make`, `parts`, `format`, `parse` |
| `Path` | `join`, `normalize`, `name`, `stem`, `extension`, `parent`, `isAbsolute`, `parts` |
| `Encoding` | `toHex`, `fromHex`, `toBase64`, `fromBase64`, `urlEncode`, `urlDecode` |
| `Hash` | `sha256`, `crc32` |
| `IO` | `write`, `readLine`, `Files`, `Terminal` |
| `Reflection` | `type`, `members`, `binding`, `bindings`, `doc` |
| `Collections` | `len`, `range`, `List`, `Map`, `Tuple`, `Set`, `Sized` |
| `Text` | `str`, `encode`, `decode`, `parseInt`, `tryParseInt`, `parseFloat`, `tryParseFloat`, `parseBool` |
| `Math` | `toInt`, `toFloat`, `PI`, `TAU`, `E`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `toRadians`, `toDegrees`, `sqrt`, `exp`, `log`, `log2`, `log10`, `pow`, `hypot`, `floor`, `ceil`, `round` |
| `Tasks` | `spawn`, `pump`, `cancel`, `close` |
| `Files` | `readText`, `readBytes`, `exists`, `size` |
| `Json` | `parse`, `stringify` |
| `Random` | `seed`, `float`, `int`, `choice`, `shuffle`, `uuid` |

`Object` and `Error` are plain globals, not `System` members.

Use `GC.collect()`, `Reflection.members(point)`, and `System.print(value)`.
`Reflection.binding("point.x")` reads a session binding/dotted field path without
calling functions; `Reflection.bindings()` lists the current script/session's
bindings, not caller-local stack frames. Modules and their members are not
reassignable. Hosts can register `host.add` with the existing native registration
API; explicit host-created global names remain a host policy, not core aliases.
Type annotations such as Int/Float/Error are still type names, not global native
conversion functions. See [examples/core_services.k3](examples/core_services.k3).

Explicit imports opt into top-level immutable aliases; there are no wildcard
imports or implicit flat function aliases. For a script beside model_types.k3
(or evaluation from the examples directory):

```text
from System import print, gc
from Reflection import members as fields
from "model_types.k3" import Point
print(fields(Point(3)))
```

The same `from` syntax works for exported declarations in user modules. Imported
names are validated before any aliases are installed; duplicate/missing aliases
fail. `import System as sys` is also supported.

`List()` and `Map()` are callable built-in type constructors, also available as
`Collections.List` and `Collections.Map`. `List(iterable)` materializes an iterable;
`Map(existingMap)` copies a map. Copies are shallow, independent containers and
preserve contracts when copying already-typed lists/maps. Constructors accept
zero or one positional argument, not named arguments.

```text
let names = List(("Ada", "Grace"))
let scores = Map({"Ada": 10})
System.print(names.length(), scores.length())
```

List, Tuple, Map, String and Bytes share `.length() -> Int`. String length counts
Unicode code points, Bytes counts bytes, and Map counts entries. `Collections.len`
remains available, but is not required for member-style use. These built-ins
satisfy `Collections.Sized`, which custom classes can implement explicitly. See
[examples/language_api.k3](examples/language_api.k3).

## Declaration Documentation

Functions, records, classes, traits and enums accept a leading string literal as documentation.
Triple single/double quotes allow multiline strings, including docstrings:

```text
record Person {
  """An immutable person record."""
  name: String
}
fn name(person: Person) -> String {
  """Return the person's display name."""
  return person.name
}
System.print(Reflection.doc(Person), Reflection.doc(name))
```

Documentation is AST metadata, not an executed declaration-body expression.
`Reflection.doc` also accepts record/class instances, bound methods and enum values; undocumented
values return an empty String. Text is retained as written, without dedenting.
The LSP shows declaration signatures/docs and record field/schema docs, including
bounded exported metadata from direct imported modules. This is static discovery,
not execution-based reflection. Javadoc tags, parameter/field doc comments and
generated documentation are not implemented.

## Data and Errors

```text
record Vec2 { x: Float, y: Float }
enum Event { Move(Vec2), Quit }

fn destination(event: Event) -> Vec2? {
    return match event {
        Event.Move(position) => position,
        Event.Quit => null,
    }
}

try {
    throw Error("application", "Could not complete the operation")
} catch error {
    System.print(error.kind, error.message)
} finally {
    System.print("finished")
}
```

Built-in runtime failures become Error records with kind/message/file/line/column.
User `throw` currently also accepts arbitrary payloads, so catches must account
for their type. `defer call(...)` captures the callable/arguments when registered
and runs LIFO on ordinary return/error. Limits and interruption are not catchable
script failures. Ordinary-function cleanup still uses the remaining execution
budget; generator cleanup has the bounded reserve described below. Complete
cancellation-safe resource regions need further work.

Records support named arguments, defaults using earlier fields, immutable fields,
and structural equality. Mutable collections in fields are still mutable; defaults
are evaluated afresh per construction. Enums use qualified variant patterns.
The checker now validates known record/enum constructors and payload patterns,
preserves typed fields/payload bindings, and reports missing variants in matches
whose enum type is known locally. Duplicate/unreachable arms and immutable field
writes are diagnosed. Dynamic and module-derived values remain runtime-checked.
See [examples/records_enums.k3](examples/records_enums.k3).

`match` also tests run-time types with `name: Type` arms. The name is bound to the
subject, narrowed to that type; `_` ignores it, and `_ =>` is the catch-all. Types
are checked in order, and the checker rejects arms that can never match and
matches that do not cover every type of a union (use `_`, or an `Object` arm for
an `Object?` subject). Generic arguments are erased at run time, so write
`List<?>` rather than `List<Int>`. Enum variant arms and type arms cannot be mixed.

```dotk3
fn describe(value: Object?) -> String {
    return match value {
        n: Int => f"int {n + 1}",
        s: String => f"string of {s.length()}",
        items: List<?> => f"list of {items.length()}",
        _: Null => "nothing",
        _ => "something else"
    }
}
```

Module-qualified annotations now work at runtime, including ordinary function
parameters/returns and nullable/list/tuple element contracts:

```text
import model_types as model
let point: model.Point = model.Point(3)
let events: List<model.Event> = [model.Event.Move(point)]
```

Record/enum checks use declaration identity, not only the short display name.
Two separately loaded modules exporting `Point` are different types. Contracts
are resolved at binding/call boundaries; bindings and typed lists retain their
resolved type identity, so rebinding a type alias cannot change an existing
contract. GC traces contract type references. Enum patterns such as
`model.Event.Move(point)` also resolve constructors by identity. Single-element
`List<T>`/`Tuple<T>` and `Map<K,V>` contracts are supported; custom generic types
are rejected rather than silently treated as enforced. This is not complete
generic typing or cross-module static inference. See
[examples/qualified_types.k3](examples/qualified_types.k3).

Typed maps validate existing entries, indexed reads, insertions and replacements.
Keys must be primitive values; key annotations support Int, Float, Bool,
Null, String and Bytes (including nullable forms). Values can have nested collection
or qualified record/enum contracts. Aliases cannot attach conflicting contracts.
Validation stages collection annotations and commits them only when the entire
type check succeeds, so a failed nested check does not partly constrain a previously
untyped collection. This does not roll back script side effects or whole statements.
See [examples/typed_maps.k3](examples/typed_maps.k3).

Maps expose member-style operations in addition to indexing and `.length()`:

```text
let data: Map<String, Int?> = {"present": null, "number": 3}
System.print(data.contains("present"), data.contains("missing"))
let before = data.values()
System.print(data.remove("present"), data.remove("present"))
System.print(data.keys(), before)
```

`contains(key) -> Bool` distinguishes presence from a null value. `remove(key) -> Bool`
reports whether an entry was removed; absent keys return false. Both validate
primitive/typed keys before inspecting or mutating entries. Failed validation leaves
the map unchanged, and removal preserves remaining insertion order and contracts.

`keys() -> List<K>` and `values() -> List<V>` return eager, independent list snapshots
in insertion order. Snapshots preserve pinned contracts even for empty maps and
keep managed entries/nominal types alive across GC. Copies are shallow: referenced
objects remain shared, but later map insertion/removal does not resize an existing
snapshot. Calls are positional-only (contains/remove one key, keys/values none).
Current membership/removal use linear scans and snapshots are budget-bounded, not
lazy views. Reflection and LSP share a member catalogue; typed snapshot indexes
retain known element discovery. See [examples/map_methods.k3](examples/map_methods.k3).

### Conditional Map Updates

Maps also expose five positional-only update operations:

| Method | Behavior |
| --- | --- |
| `compute(key, callback)` | Calls callback(key, previous-or-null), then stores its non-null result or removes the key |
| `computeIfAbsent(key, callback)` | Calls callback(key) only when absent or mapped to null; stores only non-null results |
| `computeIfPresent(key, callback)` | Calls callback(key, previous) only for a non-null existing value; null result removes the key |
| `putIfAbsent(key, value)` | Stores value when absent/null; returns the previous non-null value or null |
| `merge(key, value, callback)` | Stores a non-null incoming value when absent/null; otherwise callback(previous, incoming) supplies the replacement or null removal |

```text
let counts: Map<String, Int> = {}
counts.computeIfAbsent("visits", fn(key: String) -> Int => 0)
counts.compute("visits", fn(key: String, previous: Int?) -> Int => (previous ?? 0) + 1)
counts.merge("visits", 2, fn(previous: Int, added: Int) -> Int => previous + added)
```

These follow familiar Java-style null semantics, unlike contains which tests actual
key presence. A null result of computeIfAbsent does not erase an existing null
entry; compute/computeIfPresent/merge use null as removal, not as a stored value.
Index assignment can still store null when V allows it. Results are V? because
absence/removal is possible even for a non-null value contract.

Key/value contracts and known callback signatures are checked. Compute sees V?,
present/merge callbacks see non-null V, and merge rejects null incoming values.
putIfAbsent validates its supplied value even when an existing value wins. No entry
iterator survives a callback: prior values are rooted and entries are rescanned
after script mutation/GC. Callback errors or invalid results do not apply the final
update; callback side effects, including changes to this Map, are not rolled back.
Scans consume budgets; methods are neither thread-atomic nor a concurrency API.
See [examples/map_methods.k3](examples/map_methods.k3).

### List Mutation

```text
let values: List<Int> = [1, 3]
values.insert(1, 2)
let removed: Int = values.removeAt(-1)
values.extend((4, 5))
System.print(removed, values)
```

`insert(index, value) -> Null` accepts normalized indexes from zero through length;
negative indexes use length+index and insert before that existing position.
Indexes outside that interval fail rather than clamp. `removeAt(index) -> T`
returns the removed value, including null when stored, and requires an existing
index; empty/out-of-range removals fail. Lists retain their element contracts
after removal. Both operations preserve order and validate before mutation.

`extend(iterable) -> Null` stages all yielded values, validates the entire batch,
then appends. List/Tuple/String/Bytes/Generator sources are supported (maps are not
iterable). Strings yield code points and Bytes yield Int bytes. Self-extension
copies the original list once. Type/iteration/budget failures do not append a
partial batch or partly constrain nested elements. Generator side effects,
consumed yields and user mutation of the destination are not rolled back; generator
cleanup/ownership follows the existing lifecycle rules.

Methods are positional-only and share reflection/checker/LSP signatures. Indexed
removal retains known element return types in the editor; detached methods and
removed objects stay GC-safe through host handles. Extension is eager and uses
temporary storage/budgets; array insertion/removal shifts items and is not a
constant-time operation. See [examples/list_methods.k3](examples/list_methods.k3).

### Collection Queries and Reset

Lists support `contains(value) -> Bool` and `indexOf(value) -> Int`. Searches use
existing equality (structural records, identity-based classes/collections),
validate the query against any element contract, and consume scan budgets.
`indexOf` returns the first zero-based match or -1; null can be searched when
the contract permits it.

`List.clear()` and `Map.clear()` return Null and empty the existing container,
preserving its pinned contracts. Aliases observe the clearing; prior copies and
snapshot lists are unchanged. Repeated clearing is harmless. `List.reverse()`
returns Null, mutates the existing list and preserves element contracts; its
script budget is checked before rearrangement, so a budget failure does not
publish partial reversal.

`Map.get(key) -> V?` validates primitive/typed keys and returns null if absent,
without inserting a key. Stored null and absence have the same get result; use
contains to distinguish them. Indexing still throws on missing keys. Nullable
return types are retained in checker/LSP; validate absence before dereferencing.
Returned objects and snapshots remain rooted normally after source clearing.

All methods are positional-only. Searches remain linear and container clearing/
rearrangement are native array operations, not hard wall-clock-preemptible work.
See [examples/collection_queries.k3](examples/collection_queries.k3).

### Callback Collection Operations

Lists now provide seven positional-only operations:

| Method | Result |
| --- | --- |
| `map(callback)` | New List of callback results |
| `filter(predicate)` | New List of matching original values |
| `reduce(callback, initial)` | Left fold, passing accumulator then item |
| `any(predicate)` / `all(predicate)` | Bool, short-circuiting |
| `find(predicate)` | First matching value or null |
| `findIndex(predicate)` | First matching zero-based position or -1 |

```text
let values: List<Int> = [1, 2, 3, 4]
let doubled: List<Int> = values.map(fn(value: Int) -> Int => value * 2)
let even = values.filter(fn(value: Int) -> Bool => value % 2 == 0)
let total = even.reduce(fn(sum: Int, value: Int) -> Int => sum + value, 0)
```

Callbacks receive one item (reduce receives accumulator, item); predicates must
return actual Bool, not truthy values or a generator. Bound methods, closures and
native functions work. Empty any/all return false/true; empty reduce returns its
explicit initial value without calling the function. Find returning null does
not distinguish a matched null from absence; use findIndex when that matters.

Every call first takes a budgeted shallow snapshot. Clearing/appending/shuffling
the source inside a callback does not alter traversal, but mutation of referenced
objects remains visible. Snapshots, callbacks, results and accumulators stay rooted
across callback GC. Results are published only after success; callback effects are
not rolled back. Even short-circuit queries pay the full snapshot cost.

Filter preserves the source's pinned element contract, including empty results.
Map creates an unpinned result until an explicit annotation attaches a contract;
declared callback returns are checked as usual. The checker validates known literal,
named and bound-method callback signatures, preserves nullable finds/accumulators
and infers declared mapped element types. Dynamic callbacks stay runtime-checked.
The LSP shares signatures/result discovery without running callbacks.

### Seeded Randomness

```text
Random.seed(42)
let die: Int = Random.int(1, 7)
let direction: String = Random.choice(["north", "south", "east", "west"])
Random.shuffle(values)
let bonus: Bool = Random.float() < 0.25
```

Random uses a separate standard mt19937_64 engine per VM, initially seeded with 0.
Call seed(Int) explicitly to choose a reproducible run. Negative seeds are mapped
modulo 2^64. This is deterministic pseudo-randomness, not cryptography or OS entropy.
Draw sequences are reproducible within the same runtime/build; distribution and
shuffle algorithms are not promised as a cross-platform/version save-file format.

Int(stop) selects from [0, stop); int(start, stop) from [start, stop). Bounds must
be Int and define a nonempty increasing interval, including wide signed ranges.
Float returns a 53-bit fraction in [0, 1). Choice accepts nonempty List/Tuple/String/
Bytes; Strings select Unicode code points and Bytes produce Int. Maps/generators
are not accepted. List/Tuple choices retain existing object references, not deep
copies; String choices produce one-code-point Strings.

Shuffle mutates a List in place, preserves contracts/aliases and returns Null.
It preflights script budgets before mutation or consuming draws; empty/singleton
lists are valid. All functions are positional-only, explicitly importable, and
typed in the checker/LSP. Native array work is not wall-clock-preemptible.
Try [examples/procedural_loot.k3](examples/procedural_loot.k3), which exercises all
twelve new List/Random operations without graphics or database dependencies:

```bash
dotk3/build/release/dotk3 dotk3/examples/procedural_loot.k3
```

### String Methods

String values expose contains/startsWith/endsWith/find/split/join in addition to
length. Search/prefix/suffix arguments are Strings, never implicitly converted
from Bytes or other values. Matching is exact and case-sensitive, without Unicode
normalization. Empty needles/prefixes/suffixes match. `find(needle) -> Int` returns
the first Unicode code-point index or -1, not a UTF-8 byte offset.

`split(separator) -> List<String>` requires a nonempty String separator, preserving
leading/trailing/repeated empty fields. Empty input produces one empty field;
an absent separator produces one field containing the source. Result lists retain
String element contracts across GC and mutation.

`separator.join(iterable) -> String` accepts List/Tuple/String/Generator sources
whose elements are Strings. String sources iterate by code point. Empty input
produces an empty String; empty separators concatenate without gaps. Joining is
eager, budget-checked and publishes no partial result on type/iteration failure.
Generator consumption, side effects and existing cleanup rules are not rolled back.

All methods are positional-only. Script budgets preflight search/split byte work
and account for joined elements/bytes; native string operations are not wall-clock
preemptible. Grapheme indexing, normalization, regex/default-whitespace splitting,
and automatic conversion remain pending. See
[examples/string_methods.k3](examples/string_methods.k3).

## ASCII OBJ Viewer

[examples/obj.k3](examples/obj.k3) parses Wavefront `.obj` files (`v` and `f` lines, with `v/t/n`
entries, negative indices and polygon fans) and renders them as shaded, depth-buffered
ASCII from a field-of-view camera. [examples/obj_viewer.k3](examples/obj_viewer.k3) adds
the controls and needs a terminal:

```bash
dotk3 --files-root . examples/obj_viewer.k3 examples/models/cow.obj 80 30
```

W/S move, A/D strafe, Q/E go down/up, J/L or the arrow keys turn, I/K look up/down,
`+`/`-` change the field of view, Space starts or stops the model's rotation, and X quits.

## Read-Only Files

Files is an always-discoverable readonly namespace, but actual access is disabled
unless the host grants callbacks. The CLI grants regular-file reads explicitly:

```bash
dotk3/build/release/dotk3 --files-root dotk3/examples dotk3/examples/files.k3
dotk3/build/release/dotk3 --files-root . -e 'System.print(Files.size("README.md"))'
```

The root option precedes ordinary CLI arguments. Relative paths are resolved
against that root, not the process/script directory; absolute/parent paths and
resolved symlink escapes are rejected. `readText(path) -> String` validates UTF-8;
`readBytes(path) -> Bytes` preserves binary/NUL bytes. `exists(path) -> Bool`
reports regular-file presence, and `size(path) -> Int` reports bytes or an error
for a missing/nonregular file. Reads default to 4 MiB (inclusive); metadata may
report larger sizes without reading. Empty files are valid.

Calls require one positional, nonempty, NUL-free String path. Access denial is
`Error.kind == "capability"`, host/path/missing-file failures are `io`, invalid
UTF-8 retains the existing `value` category, and oversize reads are noncatchable
`limit` failures. Errors point at the script call. Explicit `from Files import
readText` works; checker/AST/LSP operations never invoke the provider. REPL reset
retains the configured root.

Embedding defaults grant no access. C++ Config accepts readFile/fileSize callbacks;
C hosts use separately versioned DK3Files/dk3_set_files without changing DK3Config.
Hosts own path policies, callbacks/userdata and shared-buffer synchronization.
Callbacks cannot reenter the same VM; null provider revokes access without
invalidating existing results. Details are in
[design/native-integration.md](design/native-integration.md).

This is not a hostile-code sandbox: filesystem checks/open have a TOCTOU race,
native I/O may block and is not preempted by script budgets, and existing script/
module loading policies are unchanged. Read limits bound result bytes, not total
process memory. Async I/O is not implemented. See [examples/files.k3](examples/files.k3).

### File Handles and Writes

`--files-root DIR` also enables handle-based reads and directory queries; adding
`--files-write` allows changes below the same root. Without it every mutating
operation fails with `io`; with no root at all they fail with `capability`.

```dotk3
let file = Files.open("log.txt", "w")   # modes: r, w, a, r+, w+, a+
Files.write(file, "first\nsecond\n")
Files.close(file)                      # idempotent; unclosed files close when collected

let input = Files.open("log.txt")
System.print(Files.readLine(input))    # String? (null at EOF; \n and \r\n removed)
System.print(Files.read(input, 4))     # Bytes (empty at EOF)
Files.seek(input, 0)                   # origin: "start" (default), "current", "end"
System.print(Files.tell(input))        # Int position
```

Whole-file helpers: `writeText(path, data)`, `append(path, data)`. Directory
operations: `list(path) -> List<String>` (sorted), `isDirectory`, `mkdir` (creates
parents), `remove` (files or empty directories) and `rename` (refuses to overwrite).
A handle is a `Resource`; using a closed one is a `state` error. The same sandbox
rules as `readText` apply to every path. C++ hosts supply `Config::fileSystem`.

```bash
dotk3 --files-root . --files-write script.k3
```

## System, GC, Reflection and Collections

- `System.platform() -> String` (`macos`, `linux`, `windows`) and
  `System.env(name) -> String?`. Environment access needs `--env` (otherwise
  `capability`).
- `GC.collect()` requests a collection at the next safe point; `GC.stats() ->
  Map<String, Int>` reports `objects`, `collections`, `nextCollection`, `objectLimit`
  and `steps`.
- `Reflection.has(value, name)`, `Reflection.get(value, name)`,
  `Reflection.isInstance(value, type)` (classes, records, traits, enums, built-in
  collections) and `Reflection.id(value)` join `type/members/binding/doc`.
- `Collections.sorted(items, compare = null)`, `reversed`, `min`, `max`, `enumerate(items,
  start = 0)` and `zip(a, b)` accept any iterable. Numbers and Strings order
  naturally; `compare(a, b)` returns a negative Int for "a first". Sorting is stable.
  `enumerate`/`zip` return a `List` of `Tuple` pairs.

## JSON Interchange

```text
let document = Json.parse("""{"count":9007199254740993,"tags":["active",null]}""") as Map<String, Object?>
(document["tags"] as List<Object?>).append("verified")
System.print(Json.stringify(document))
```

Json.parse accepts one UTF-8 String and produces Null/Bool/Int/Float/String/List/
Map values. Integer tokens must fit signed Int64 and remain exact beyond 2^53;
overflowing integers are rejected, not rounded into Float. Decimal/exponent tokens
use finite Float64. Objects use String keys and reject duplicate decoded keys.
Invalid syntax, Unicode escapes and trailing data produce structured json errors.
Parsed shapes are dynamic; annotations attach existing runtime contracts.

Json.stringify accepts primitive JSON values, Lists/Tuples (arrays), and Maps with
actual String keys. It rejects cycles, nonfinite floats, Bytes, records/classes,
functions/resources and non-String keys; convert explicitly before encoding.
Repeated noncyclic references are allowed. Maps retain insertion order and output
is compact. Embedded null characters are escaped normally. Both calls are
positional-only and expose shared checker/LSP signatures without editor execution.

The implementation reuses pinned nlohmann/json 3.12.0: public SAX parsing converts
directly into rooted runtime objects. Nesting is capped at 64 containers and input/
returned output at 4 MiB. Conversion consumes step budgets; serialization also uses
a conservative preflight byte estimate that may reject below the returned-byte cap.
Native lexing/dumping and transient allocations are not a wall-clock/memory sandbox.
The existing vendored header is now also linked into the runtime library; its pinned
hash/version are unchanged. See [examples/json.k3](examples/json.k3).

## Modules and Generators

```text
import library as library
System.print(library.answer)

fn stream() {
    yield null
    for value in Collections.range(3) { yield value }
}
for value in stream() { System.print(value) }
```

Modules expose only `export` declarations and have separate lexical scopes based
on the prelude, not the caller's globals. CLI resolution reads `.k3` files below
the script directory; this policy is not a symlink-hardened sandbox. Hosts supply
their own resolver. Empty-module resolution and explicit import-cycle diagnostics
need refinement. Namespace fields cannot be reassigned.

Generators are lazy. Exhaustion is a separate boolean, so yielded null is valid.
Generator bodies support bindings, expressions, nested blocks, if/while/for,
yield, return, break/continue, defer, and try/catch/finally. Deferred calls retain
their captured callable/arguments through suspension and GC. Scope exits unwind
defers LIFO, and transfers across try regions run finally. Normal iteration can
yield inside try, catch, or finally; pending errors survive those suspensions.
The first ordinary error wins over later cleanup errors; uncatchable failures
take precedence over catchable errors. A cleanup failure overrides a pending
return or loop transfer. Failed generators become terminal.

Use `Tasks.close(generator)` or the C API `dk3_close` when abandoning a suspended
generator. Close is idempotent, does not enter an unstarted body, and drains
active finalizers without delivering their yields. A cleanup error is reported
once; subsequent resume is exhausted. Closing/resuming a running generator is
rejected. Breaking an external iteration does not implicitly close its source;
explicitly close it when it is no longer needed. GC, handle release, and VM
destruction do not run generator defers/finally callbacks. Separately registered
opaque resource disposal is explicit or performed by owner-thread VM shutdown.

Close and uncatchable-error unwinding use a separate 10,000-step script budget
and bypass the interruption callback so cleanup can run. Deferred-call cleanup
also has a 10,000-step reserve per scope, shared with an enclosing unwind.
Cleanup is best effort once this reserve or the object limit is exhausted.
Native callbacks remain non-preemptible. This is not a production resource or
sandbox guarantee. Sending values, async/await, native wait integration, and
production cancellation-safe scheduling remain pending. `range` is currently an eager,
budget-bounded list, not a lazy range object.

## Cooperative Tasks

```text
let job = Tasks.spawn(stream())
while not job.done { Tasks.pump(1) }
System.print(job.status, job.has_value, job.value)
```

Spawn accepts a fresh generator and transfers its execution to a per-VM FIFO
scheduler. Tasks expose status/done/latest-yield/error; pump returns executed
turns. Children spawned during task execution belong to that task. Parent exit
cancels unfinished descendants before parent-level cleanup, and uncaught child
failure propagates through its ancestry. This is cancel-on-exit, not implicit
join. `Tasks.cancel(task)` is idempotent, with safe-point requests for running Tasks.
Nesting is limited to 128 levels; teardown shares the bounded cleanup reserve.
Active tasks stay rooted independently of handles. Explicitly cancel unwanted
root tasks before destruction; releasing a handle does not cancel a task.

Tasks are not threads. Native callbacks can still block a VM. Independent VMs
can execute on separate host threads; worker channels, processes, await/I/O
scheduling, and raylib/database bindings are not built in yet. See
[the integration guide](design/native-integration.md) for thread/process ownership,
render-thread requirements, database transactions, and the resource ABI.
Run the example with `dotk3/build/release/dotk3 dotk3/examples/tasks.k3`.

## Profiling

Source CPU profiling and VM telemetry are independent options and can be combined.
They do not enable debugger mode or change the selected bytecode engine/-O level.
Reports go to stderr; program output stays on stdout.

```bash
dotk3/build/release/dotk3 --profile examples/program.k3
dotk3/build/release/dotk3 --profile-vm simple examples/program.k3
dotk3/build/release/dotk3 --profile-vm verbose examples/program.k3
dotk3/build/release/dotk3 --profile --profile-output source.speedscope.json examples/program.k3
dotk3/build/release/dotk3 --profile --profile-vm verbose --profile-trace vm.trace.json examples/program.k3
```

`--profile` reports safe-point-sampled CPU self/inclusive hotspots by function and
source location, recorded activation counts, native wall/CPU durations and managed
allocation sites. `--profile-vm simple` gives wall/CPU time, parse/compile/execute
phases, actual bytecode/fallback call counts, allocation/live-object metrics and GC
pause p50/p95/max. `verbose` adds readable opcode counts, specialization/cache
statistics, allocation kinds/sites and native-call detail. Use
`--profile-interval-ms 1..1000` to change the default1ms CPU sample interval.

Speedscope JSON opens in Speedscope; `--profile-trace` exports Perfetto/Chrome trace
events and requires verbose VM mode. Native callbacks and GC appear separately;
wall-time phase totals overlap and are not additive. Memory bytes are estimated
Object-base plus logical text payload, not RSS or exact container/native memory.
Sampling is cooperative, not an OS-level asynchronous/native-stack profiler.
For C++ symbol hotspots, use Instruments Time Profiler/Allocations on macOS.

Buffers are bounded with reported dropped counts. Profiling is opt-in and can
perturb timings, particularly verbose per-opcode tracking and call-heavy scripts.
Output is written on normal/handled-error completion, not guaranteed after process
signals/forced termination. [Profiling details and overhead](design/profiling.md)
describe interpretation, exports, bounds and verification.

## CLI and Shell

Failures use source-first diagnostics: a named error category/message, file/line/
column, the complete failing source line with a token marker and one neighboring
line on each side when available. Runtime failures add a failure-first call stack
with function/method/module/generator names and caller locations, without needing
--debug. Imported source is retained as the executed snapshot, not reread after
failure; stack metadata owns names/source spans rather than runtime values.
Cleanup does not overwrite the original captured trace. Thrown values and generator/
task failures preserve their traces across host adapters; a later unrelated failure
gets a fresh trace. Capture is best effort if native allocation itself fails.

CLI colors are automatic when stderr is a terminal: red error/marker, cyan location/
stack heading and subdued gutters/locations. Redirected stderr and embedding
diagnostic strings are plain. Set `NO_COLOR` or `TERM=dumb` to disable terminal
colors. Tabs expand to four-column stops in excerpts, CRLF is normalized and UTF-8
code points do not consume one marker column per byte; grapheme/wide-character
terminal measurement is not a complete Unicode layout engine. Long source lines
are not truncated. Token underlines are bounded to80 source bytes.

Run the focused diagnostic regressions independently of optional app examples:

```bash
make -C dotk3 CXX=clang++ CC=clang test-diagnostics
make -C dotk3 CXX=clang++ CC=clang MODE=stress test-diagnostics
```

```bash
dotk3/build/release/dotk3
dotk3/build/release/dotk3 --repl
dotk3/build/release/dotk3 -i dotk3/examples/core.k3
dotk3/build/release/dotk3 -e 'System.print(System.args)' first second
printf 'System.print(42)\n' | dotk3/build/release/dotk3
```

A terminal with no arguments opens the persistent multiline shell; piped input
runs as a batch. The shell echoes the final non-null expression, keeps state
after errors, and has `:help`, `:reset`, `:quit`, `:load FILE.k3`, `:ast FILE.k3`,
and `:check PATH [PATH...]`. Quote file paths containing spaces. `:load` runs the
file in the existing session; check/AST commands do not execute it. They recover
after errors without destroying the session. Normal exit/reset explicitly
cancels remaining Tasks. `System.args` is an immutable tuple of user arguments. Help,
version, script arguments, explicit stdin (`-`), and inline `-e` are implemented.
Terminal REPL input supports left/right editing and up/down in-session history
through libedit, limited to 500 physical input lines. History is not written to
disk by default, and reset keeps the current session's editing history. Piped
input remains plain and deterministic. REPL tab completion, persistent history,
and guaranteed Ctrl-C cleanup remain pending.

To inspect a live record, run `Reflection.members(point)` or `:members point`.
The shell command prints field names/types and bounded values; dotted paths such
as `:members module.value` are supported, not arbitrary expressions or calls.
Inspecting a record type shows its constructor fields rather than instance values.
List, task, enum namespace and module members are also discoverable.

The source debugger supports file/line breakpoints, step/next/finish, stack/frame
selection, and bounded readonly locals/globals. Generator frames retain identity
across yields, and task IDs are shown while running cooperative Tasks. Start it:

```bash
dotk3/build/release/dotk3 --debug dotk3/examples/debugger.k3
```

Stops show nearby source lines with `=>` marking the active line. Variables,
stack frames and breakpoints use aligned tables; long variable values wrap with
aligned continuations. At a stop, use `help`, `break 4`, `continue`, `next`, `bt`,
`locals`, `tasks`, or `print sum`. The read-only task table includes parent IDs,
state, generator frame and location, with the executing task marked.
Breakpoint management includes `breakpoints`, `disable`, `enable`, and `delete`.
`--ast` remains a syntax dump, not a debugger. Detailed scope and limits are in
[the CLI/debugger guide](design/cli-debugger.md).

## Checker and Embedding

```bash
dotk3/build/release/dotk3 --check dotk3/examples
dotk3/build/release/dotk3 --check first.k3 second.k3
```

`--check` accepts files, directories and stdin (`-`), and never executes code.
Directory traversal checks `.k3` files in sorted order, deduplicates normalized
paths, reports errors across files, and prints a pass count. An empty directory
fails with usage status 64; language errors return 65 and host/file errors 70.
Every run, `--check`, REPL submission and LSP document goes through the static
typer (see Static Typing); imported modules are type-checked with the importing
program, and warnings are printed only by `--check`. `--ast` prints the
source-spanned tree. The LSP publishes the typer's errors and warnings on unsaved
edits; completion and hover use the same checker (types, visibility, inheritance and
imports), including partial `receiver.` input.
There is no formatter or general semantic navigation yet.

Optional `.k3` VS Code syntax/configuration and LSP client support live under
[editor/README.md](editor/README.md). They highlight records/enums/functions,
types and literals, provide comment/bracket behavior, and connect to the native
language server for live parser/checker diagnostics and document outlines.
They do not install themselves or replace the CLI checker. Setup, trust/server
settings and test limitations are documented in the editor guide.

```bash
make -C dotk3 CXX=clang++ CC=clang lsp
```

The server is `build/<mode>/dotk3-lsp`: stdio JSON-RPC, full-document updates,
UTF-16 positions, versioned diagnostics, record/enum/function/binding symbols,
member hover and dot-completion for known records/core modules/enums/typed indexes.
Declaration hover includes custom-type/function signatures and docstrings;
explicit import aliases and collection `.length()` participate in member discovery.
It uses frontend code only, never runs scripts, and retains the checker's current
per-file/dynamic-type limits. Member discovery can read bounded exported metadata
from direct sibling imports and prefers unsaved open modules, without executing
them. Full inference, navigation, non-member completion, formatting and incremental
parsing remain pending. See [design/lsp.md](design/lsp.md).
The pinned JSON header is shared by the LSP and runtime JSON library. Tests use
Ruby/Node; optional client setup uses npm.

The public header is [include/dotk3.h](include/dotk3.h). It provides independent
instances, output/resolver hooks, evaluation, persistent rooted values, positional
function calls, Int/String getters, contextual native registration and lazy
resume/explicit generator close, task spawn/pump/cancel and task state/value inspection.
`available == 1` plus a null-valued handle is a yielded null;
`available == 0` means completion. Native callback userdata belongs to the host.
Public API reentry on the same instance returns BUSY. Hosts must externally
synchronize destruction and keep callbacks/handles within their owner lifetime.
Handles released while the VM lives are rejected; they are invalid after destroy.
The API is experimental and does not promise binary compatibility with DotK 2.

Object limits count managed objects, not total bytes or process memory. AST step
limits do not preempt parsing or native callbacks. OOM poisons the instance.
There is no hostile-code sandbox, general native capability library, dynamic
binary-module loader or production scheduler. Collection/resource inspection and
versioned contextual module registration are implemented below.

The C API now creates/reads Null, Bool, finite Float and raw Bytes in addition to
Int/String, and dk3_get_type reports actual runtime categories (not nominal names).
Getters are strict: no Int/Float coercion or String/Bytes reinterpretation. Null
buffer with zero length is accepted for String/Bytes; nonempty null buffers fail.
Borrowed getter buffers remain valid while the immutable value handle is alive;
copy before releasing the handle or destroying the VM. Stale/foreign handles fail.

Native callbacks have Bool/Float/String/Bytes getters and Null/Bool/Float/Bytes
returns. Argument byte spans are borrowed for callback duration; returns copy
buffers immediately. dk3_native_return_value reuses a live same-VM handle without
transferring ownership (released/foreign handles fail). Ordinary public API reentry
still returns BUSY; callback-specific helpers operate within the active call.
Host APIs now construct Lists/Tuples/Maps, extract owned child handles and inspect
sizes. List append/set and Map set/remove enforce pinned contracts; Tuples remain
immutable. Sequence indexes are size_t positions without negative indexing. Map
get returns found=0/no handle when absent, or found=1/a handle even for null.
Entry traversal follows current insertion order and live indexes can shift on
mutation. Same-owner child handles survive parent release/GC independently.

Callback-local dk3_native_argument/make/list/tuple/map/value/sequence/release helpers
return owned handles, not temporary borrows. Release each with dk3_native_release
during a callback or dk3_release afterwards. DK3Call is borrowed for the current
callback on its executing thread and must not escape. Public calls remain BUSY;
helpers reuse checked operations without script execution. Managed return values
receive a temporary root, permitting builder release and subsequent allocation.

```bash
make -C dotk3 CXX=clang++ CC=clang example-native
```

The public C adapter builds nested maps/lists and exchanges typed data with script
and JSON, then registers a Counter module backed by opaque resources. See
[examples/structured_host.c](examples/structured_host.c).
Native work remains trusted, not preemptible/sandboxed. Richer iterators, dynamic
loading, manifest-based editor signatures and SQLite/raylib bindings remain follow-up.

### Opaque Resources and Native Modules

Initialize `DK3Resource` with `dk3_resource_init`, fill a nonempty UTF-8 type_name,
nonnull payload and dispose callback, and call `dk3_resource`. Only OK transfers
payload cleanup responsibility to the VM. Each resource remembers its creation
thread. `dk3_resource_get` checks actual kind, exact tag, live state and thread;
the payload is borrowed until close. Released/foreign/kind/tag errors are INVALID,
closed access is ERROR, and live wrong-thread access/close is BUSY.

`dk3_resource_close` is idempotent; `dk3_resource_closed` reads lifecycle state.
An optional parent must be a live same-VM resource on the current thread. Closing
a parent with open children returns BUSY. GC and handle release never dispose a
payload: forgotten open state stays registered until explicit close or shutdown.
The number of open resources is separately capped by DK3Config.object_limit;
closing resources recovers this quota, but managed objects still need collection.

`dk3_close_resources` and `dk3_destroy` preflight all open resource threads, then
dispose in reverse creation order, children before parents. Wrong-thread shutdown
returns BUSY without closing anything and leaves the VM alive. Close remaining
resources on their creation threads before destroying a multi-thread-used VM.
These cleanup operations remain available after OOM poisoning. Dispose callbacks
must release the payload, must not throw, and cannot reenter the VM or use a native
call context. This is host cleanup, not arbitrary GC callbacks or script cleanup.

Callback-local resource/create/get/closed/close helpers follow the existing owned
handle rules. Scripts can annotate the nonconstructible `Resource` category and
inspect readonly `.closed` and `.type_name`; concrete tag checks are enforced by
host adapters, not custom nominal language types. Reflection and frontend member
discovery expose these metadata fields without executing host code.

Initialize `DK3NativeModule` with `dk3_module_init`, set its name/docs and an array
of `DK3NativeExport` contextual callbacks (each with a type signature), then `dk3_register_module`. Names must
be single ASCII identifiers, not keywords; existing modules/global/core names and
duplicate exports are rejected. Registration stages the entire readonly namespace
before publication. After an object-limit failure, collect abandoned staged objects
before retrying. Imports/aliases and Reflection.doc/members work without a source
resolver. Names/docs are copied; callback code and userdata must outlive the VM.
Module callbacks are positional-only; `dk3_define_native` takes a signature too.
This is a versioned registration ABI, not a full editor manifest.

### Writing a Native Module in C

[examples/native/stats_module.c](examples/native/stats_module.c) is a complete
module (Int/String/List arguments, a Map result, a resource with a dispose
callback, and errors). A plugin only includes `dotk3.h` and exports one function:

```c
const DK3NativeModule *dk3_module_entry(void);   /* returns name + DK3NativeExport table */
```

Each export is a name, a declaration tail the type checker uses
(`"(a: Int, b: Int) -> Int"`), a `DK3Native` callback and optional docs. Inside a
callback read arguments with `dk3_native_int/float/string/...`, return with
`dk3_native_return_*`, and fail with `dk3_native_error` (a catchable `Error` of
kind `native`). Release every `DK3Value` obtained from `dk3_native_argument`,
`dk3_native_sequence_get`, `dk3_native_make_*` etc. with `dk3_native_release`.

```bash
make example-plugin     # builds the library, copies it beside examples/native/stats_demo.k3, runs the demo
```

Scripts load a plugin with a plain `import Stats` (or `from Stats import gcd`): when
no `Stats.k3` exists, the directory of the running script is searched for
`Stats.<ext>`, `libStats.<ext>` or `stats_module.<ext>` (`.dylib` on macOS, `.so` on
Linux), and the module name it exports must match the import. Imported `.k3` files
may import native modules the same way. Automatic discovery applies to script files
only; `-e`, stdin, the REPL and `--check` need `dotk3 --native PATH` (repeatable,
any location), which also makes the signatures visible to the checker and REPL.

The language server finds libraries the same way for open files: it calls the
library's `dk3_module_entry` to read the export table (no export runs), so
diagnostics, hover and completion know `Stats`. That executes the library's load
code; start `dotk3-lsp --no-native` to disable it. Rebuilt libraries are picked up
after restarting the server.

The library does not link DotK 3; the `dotk3` executable provides the `dk3_*`
symbols. Build your own with `cc -fPIC -shared -I include mod.c -o mod.dylib
-undefined dynamic_lookup` on macOS or `cc -fPIC -shared -I include mod.c -o mod.so`
on Linux. Resources are disposed when the CLI exits or the REPL is reset. Loading
runs native code with the full privileges of the process, so a library sitting
next to a script is trusted exactly like the script itself.

To embed DotK 3 in a C program instead, call `dk3_register_module` as in
[examples/structured_host.c](examples/structured_host.c).

## Coverage and Next Work

| Area | This alpha | Remaining |
| --- | --- | --- |
| Independent build/frontend | Implemented and tested | Packaging, parser-depth hardening |
| Binding/numeric/boolean basics | Implemented and tested | Expanded fuzzing and standard numeric library |
| Collections/Unicode | Typed core, callback transformations/folds/queries and known result tooling | Broader generic inference, graphemes and iterator protocols |
| Procedural randomness | Seeded per-VM integers/fractions/choice and contract-preserving shuffle | Additional distributions and explicit entropy capability |
| Functions/defaults/closures | Implemented and tested | Destructuring and unpacked calls |
| Records/enums/match | Runtime core, qualified nominal contracts/patterns and known local static checks | Cross-module static interfaces, broader inference and interfaces |
| Classes | Typed constructors/fields, lexical visibility, statics, open inheritance/super and native collection bases | Advanced variance/generics and broader inference |
| Traits | Nominal contracts, default methods, class/record implementations and Sized conformance | Generics, associated types, trait inheritance and variance |
| Errors/defer/modules | Core implemented | Robust cycles/empty modules, resource libraries and cancellation-safe cleanup |
| Lazy generators | Cleanup, handlers, loop transfers and explicit close tested | Send protocol and richer iteration |
| Cooperative tasks | Parent ownership, FIFO pump, cancellation and failure propagation tested | Await, timers/I/O, native completions and worker channels |
| Optional typing | Runtime checks plus basic checker | Full inference/null-flow, type aliases and soundness work |
| C embedding | Scalar/owned collections, checked callbacks, affine parent-aware resources and versioned atomic modules | SQLite/raylib bindings, richer iterators and dynamic loading |
| JSON interchange | Exact Int64 SAX parse, bounded compact encode, duplicate/cycle/type checks | Custom conversions, streaming and schema tooling |
| CLI | Core shell, member inspection, libedit arrows/history and project checking tested | Persistent history/tab completion, test/profiler parity |
| Source debugger | Breakpoints, stepping, source context, frame/variable/task tables and GC-safe previews tested | Exception stops, richer inspectors, C debugger API, watch expressions and DAP |
| Language server/editor | Diagnostics, outlines, record/core hover and dot-completion tested | General navigation/completion, incremental parsing, full module indexing and formatter |
| Tooling/backend | Spans, diagnostics, project checks | Shared rich opcode metadata and complete bytecode backend |

### Classes and Traits

Class core is implemented with explicit field mutability and instance methods:

```text
class Counter {
  var value: Int = 0
  let label: String = "counter"
  fn add(amount: Int = 1) -> Int {
    self.value += amount
    return self.value
  }
}
let counter = Counter(value: 2)
let increment: fn(Int) -> Int = counter.add
GC.collect()
System.print(increment(3))
```

Construction uses declared fields in order, with positional/named arguments and
fresh defaults just like records. Earlier fields are available to defaults;
there is no implicit `self` in field defaults. `let`
fields cannot be rebound; `var` fields enforce pinned type contracts on writes.
New fields cannot be added, and methods cannot be replaced through an instance.
Fields/methods are public by default. Explicit public/protected/private modifiers
now apply to classes; advanced class generics remain follow-up. Constructors,
static members, inheritance and native collection bases share the same class model.

Collections.List/Map/Tuple/Set are real open class descriptors, also exported as bare
constructors and available through explicit import aliases. Reflection.type reports
Type for these descriptors. Literals and host-created collections retain descriptor
identity; derived collections retain a nominal class identity and native storage.
There is no imposed final/sealed/frozen policy. List/Map/Set stay mutable; existing
Tuple element immutability is preserved, including subclasses.

### Class Visibility

```text
class Counter {
  private var value: Int = 0
  protected let step: Int = 1
  public fn increment() -> Int { self.value += self.step; return self.value }
}
class FastCounter extends Counter {
  public fn stepSize() -> Int { return self.step }
}
```

Put the access modifier before static/fn/let/var/constructor. Public is the default
and preserves existing programs. Private admits lexical code defined for the
declaring class; protected also admits subclasses. Reads and writes, methods,
statics, super and constructors use the same runtime access rule. Protected access
uses the class relationship, not a Java-specific receiver restriction.

Class methods, constructor/default code, closures and generators retain their
declaring class access across calls and GC. External impl methods are installed
as target-class methods with their own implementation lexical scope. Trait default
bodies retain trait scope and cannot directly access private implementation state.
Unrelated callbacks do not inherit their caller's privileges. Authorized code may
return a private callable/closure as explicit delegation; possession permits use.

Overrides cannot narrow visibility or replace a private base method. Trait
implementations must be public instance methods. Known accesses are diagnosed by
the checker; dynamic/native/imported unknown values are runtime-authoritative.
Completion/hover omit inaccessible members, including inherited statics/fields;
script reflection filters names/values by current access. Ordinary stringification
never exposes private/protected field values. Records keep public structural data
semantics and do not accept these modifiers. No finality/freezing policy is added.

This is language encapsulation, not protection from trusted embedding/native code
or a hostile-code sandbox. See [examples/encapsulation.k3](examples/encapsulation.k3).

### Custom Constructors

```text
class Player {
  let name: String
  var level: Int = 1
  constructor(name: String, level: Int = 1) -> Null {
    self.name = name
    self.level = level
  }
}
let player = Player("Ada", level: 5)
```

Constructor parameters support ordinary types/defaults/named/rest arguments and
define the class call signature, independently of fields. Parameterized fn init
is also a custom constructor; explicit constructor() supports zero-argument custom
initialization without changing legacy fn init() hook semantics. A class can declare
only one initializer; inherited constructors and explicit super.init(...) work.
Constructors remain synchronous and return Null; they need not preserve the base
constructor's argument signature.

Field defaults run first in their declaring lexical scope. Fields without defaults
start uninitialized; reading them fails. A let field without a default can be written
once during construction, and every required field must be initialized before return.
Defaulted let fields are already initialized and cannot be overwritten. Type checks
apply on every write. Missing/duplicate writes, invalid arguments and constructor
errors abort publication of the result; script side effects or escaped references
are not rolled back. Without a custom constructor, generated field-based construction
and existing zero-argument init hooks continue to work.

### Collection Subclasses and Set

```text
class Inventory extends Collections.List {
  let owner: String
  constructor(owner: String, initial: List<String> = []) -> Null {
    super.init(initial)
    self.owner = owner
  }
  fn append(value: String) -> Null { super.append(value) }
}
let inventory: List<String> = Inventory("Ada", ["compass"])
let tags: Set<String> = Set(["tool", "tool", "light"])
tags.add("travel")
System.print(tags.values())
```

Collection subclasses share one native storage object with indexing/iteration,
existing methods/operators, typed aliases, Json-compatible List/Map conversion and
the host API. Fields and overrides are ordinary class members; super calls the
native base method. Native init initializes storage only during construction. With
no custom init, a collection subclass accepts the base's optional source; its own
fields must have defaults. Native override arity/result categories are checked and
element/key contracts remain runtime-authoritative. Intrinsic indexing uses physical
storage bounds; Collections.len respects an overridden length method.

Qualified annotations such as Collections.List<Int> and imported constructor aliases
resolve to the same collection contract. Mutations preserve invariant contracts.
Copies/slices/snapshots return base collections, not implicitly reconstructed subclasses.
Tuple(iterable) constructs immutable element storage; subclasses can still have their
own typed fields. Class methods/static fields/subtype identity are GC-traced and
reflected without exposing native pointers.

Set<T> preserves insertion order, suppresses duplicates using normal equality,
and supports add/remove/contains/clear, values snapshots, union/intersection/difference
and isSubsetOf. Add/remove return whether membership changed. Algebra returns new
base Sets preserving the left set's element contract; values returns a typed List.
Sets iterate but are not position-indexed. Membership is currently linear and algebra
can be quadratic; no hash-backend performance claim is made. Mutable collection
equality remains identity-based. Json does not implicitly serialize Sets.
See [examples/collection_classes.k3](examples/collection_classes.k3).

### Static Members and Open Inheritance

```text
class Actor {
  static var created: Int = 0
  static let category: String = "actor"
  let name: String
  fn init() -> Null { Actor.created += 1 }
  fn describe() -> String { return self.name }
}
class Runner extends Actor {
  let speed: Int = 2
  static fn create(name: String) -> Runner { return Runner(name) }
  fn describe() -> String { return super.describe() + " / runner" }
}
let actor: Actor = Runner.create("Ada")
System.print(actor.describe(), Runner.created)
```

Static let/var fields require typed initializers, evaluated once at class definition
in source order. Static functions are installed first and have normal lexical
scope/defaults/named arguments, but no implicit self/super. Access statics through
the class, not an instance. Static var writes retain contracts; static let/function
bindings cannot be reassigned. Inherited statics refer to the original owner's
storage rather than copying mutable state. Detached static functions retain their
class state across GC. Static functions do not satisfy instance trait signatures.

Extends accepts one previously defined user or built-in collection class, optionally
module-qualified; records cannot be bases. Classes are open by default, with no
final/sealed/frozen modifier. Constructor fields are ordered base-to-derived with
fresh defaults, resolved in each declaring class's lexical/module scope. Inherited
fields cannot be redeclared. Derived instances satisfy base annotations and inherit
trait conformance. List/Map contracts remain invariant, even for related classes.

Methods dispatch from the actual instance type. Overrides preserve parameter names,
types, rest/default presence, return contracts and generator status; changing static
vs instance kind is rejected. Super.method calls the implementation above the class
that owns the selected method, on the same derived receiver. The most-derived init
hook runs once; if no override exists, the inherited hook runs. An overriding init
calls super.init explicitly when parent initialization is needed, not automatically
twice. Legacy hooks are zero-argument; custom constructors are synchronous and
Null-returning. Super is a method
proxy, not a second constructor. Super.field accesses the same instance's inherited
field with its original type/mutability contract, not a separate parent object.

Inheritance is capped at 128 edges. Fields/methods/static/super and base identities
are rooted for GC; reflection and bounded frontend metadata include inherited
members without executing code. Unknown imported interfaces retain conservative
checker limits. Multiple class inheritance, advanced covariance/class generics,
complete module inference and advanced class generics remain
follow-up. See [examples/inheritance.k3](examples/inheritance.k3).

A class may define `fn init() -> Null { ... }`. This synchronous zero-argument hook
runs once automatically after field construction, with readonly `self` and typed
mutable fields available. Omitting its return annotation is allowed, but any
non-null return is rejected. Yields are rejected; parameterized custom constructors
use the separate constructor path described above.
GC and ordinary defer/throw behavior work inside the hook. An initializer error
fails construction; script side effects or explicitly escaped references are not
rolled back. The ordinary field constructor signature stays unchanged.

Methods receive implicit readonly `self`; do not declare a `self` parameter.
Detached methods retain the receiver and lexical environment across GC, host
handles, closures and generator suspension. Ordinary debugger frames use
`Class.method` and expose `self` without executing getters. Reflection lists
declared fields/methods and docs; LSP supports class outlines, method signatures,
`self.`/instance completion and direct imported metadata without executing code.
Class equality is identity-based, unlike structural records. Records remain
immutable data values; mutable collections in their fields are still mutable.
See [examples/classes.k3](examples/classes.k3) for composition with a record.
Records can define methods and `implements` clauses too, using the same bound
dispatch, including trait defaults. Record fields remain immutable and equality
compares data fields structurally, not methods. Referenced collections may still
be mutated. A record method named `init` is an ordinary method, not a constructor
hook. See [examples/object_lifecycle.k3](examples/object_lifecycle.k3).

### Trait Intersections

Annotations can require several traits simultaneously:

```text
from Collections import Sized
trait Named { fn name() -> String }
fn inspect(value: Sized & Named) -> String {
  return value.name() + " / " + Text.str(value.length())
}
let values: List<Sized & Named> = []
let optional: (Sized & Named)? = null
```

The value must implement every component; implementing just Sized is insufficient.
Use this in bindings, fields, parameters, returns and collection elements. Grouped
nullable intersections accept null as a whole. Components themselves must be
non-null traits; primitive/class intersections and unions are not supported.
Reversed order and repeated identities do not change runtime contracts. Conflicting
method signatures are rejected, and inspection/completion combines both traits'
members without execution. Resolved identities are pinned and GC-traced normally.
Imported unknown interfaces retain existing conservative static-checking limits;
runtime validation is authoritative. See [examples/traits.k3](examples/traits.k3).

### Trait Contracts

```text
from Collections import Sized
class Bag implements Sized {
  let items: List<Int> = []
  fn length() -> Int { return self.items.length() }
}
fn size(value: Sized) -> Int { return value.length() }
System.print(size(Bag([1, 2])), size("abc"))
```

Define a custom contract with `trait Readable { fn read() -> String }` and
declare class conformance with `class Bag implements Sized, Readable { ... }`.
Define all required methods directly inside the class, like Java interfaces.
The clause accepts multiple qualified trait names and uses the same signature
validator as external `impl` blocks. Invalid class conformance withdraws the new
class binding, so a failed declaration does not publish a partial class.
Existing bindings are preserved. Traits must be available when the class is declared.

External `impl Readable for ClassName { ... }` remains supported as an opt-in
extension form; it is not required for class-local implementations. Conformance is nominal:
matching method names alone is not enough. Trait identities remain distinct
across modules and are pinned in field/container/argument/return annotations.
Classes may implement multiple traits. An empty implementation block validates
matching existing methods; nonempty blocks add only the trait's required methods.
Existing methods cannot be replaced and fields cannot be shadowed.

Trait signatures require explicit parameter/return types, no explicit `self`,
parameter defaults or rest arguments. A method body supplies a default implementation;
abstract signatures have no body. Implementations use implicit readonly `self`;
parameter names, arity and resolved types must match, preserving named calls.
Generator methods do not satisfy these synchronous contracts. Implementation
validation completes before methods/conformance are installed; this is validation
atomicity, not recovery from allocation failures or rollback of script side effects.

List/Tuple/Map/String/Bytes satisfy `Collections.Sized` automatically. For custom
implementations, both `.length()` and `Collections.len(value)` call the checked
class method. User-defined trait methods reuse bound-method dispatch and GC roots.
Reflection exposes trait docs/member names; the LSP has trait/implementation
outlines, signatures, self/typed member completion and live mismatch diagnostics.
Qualified/dynamic checker cases remain conservative; runtime identity checks are
authoritative. Discovery is bounded metadata inspection, not execution.

Default trait methods run in their trait's lexical environment, with readonly
`self` exposing the trait contract. Explicit methods on the class/record take
precedence. Competing defaults require an explicit method instead of silently
choosing by trait order. Defaults are synchronous and may carry docstrings;
`init` is reserved for class lifecycle and cannot be a trait method.

Generic traits, associated types, trait inheritance, variance,
builtin extension implementations and general iterator traits are not
implemented. See [examples/traits.k3](examples/traits.k3) and
[design/plan.md](design/plan.md).

Fourteen test groups cover lexer, parser, runtime, profiling, generator lifecycle, tasks,
checker, C embedding, independent-VM thread overlap, CLI subprocess behavior,
real loopback web/chat journeys, source-debugger conformance, editor/client
validation and native LSP protocol. Terminal tests include Tetris/chat PTYs and
deterministic collision/rotation/drop/line-clear/game-over checks. `test-network`
and `test-cli` rerun these app slices independently. Pac-Man model/PTY and browser
asset/action/error-isolation flows are included. Live Playwright checks separately
verify canvas pixels, movement, ghost movement, restart, pause/resume and small/wide
viewport layouts; they are not yet a headless-browser dependency of make test.
Both release and UBSan runs pass. This is a substantial independent core, not
all of the proposed language overhaul. Design requirements and batch coverage
are under [design](design); do not mistake those requirements for implemented
features. No parent source, test, build, or binary is changed by this project.