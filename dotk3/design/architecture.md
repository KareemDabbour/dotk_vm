# DotK 3 Architecture

## Execution Backend

The frontend owns a source-spanned AST for parsing, checking and editor metadata.
bytecode.cpp lowers supported expressions/statements to immutable numeric
instructions with source nodes, predecoded numeric constants and lexical slot
indices. runtime_bytecode.cpp executes these using reusable frame-owned local and
operand vectors. Returns and loop transfers are ordinary VM control flow, not C++
exception unwinding. Native/type/operator boundaries reuse checked runtime helpers.

Execution is bytecode-only; optimizationLevel defaults2. O0 is baseline
bytecode, O1 specializes checked Int arithmetic/comparisons, and O2 additionally
uses per-frame binding caches invalidated by declaration epochs. No level removes
language checks or source/step information. The CLI exposes -O0/-O1/-O2 and
--engine bytecode; C SDK config layout is unchanged and uses runtime defaults.

The AST expression evaluator and statement walker have been removed. Eligible
noncapturing functions/programs use slot bytecode; other bodies use compiled lexical
control streams whose expressions and leaf operations execute the same operand VM.
Functions cache immutable control streams; call/suspension state is separate.
Methods, closures, defaults/rest, generators, loops, handlers/defer, slices/matches,
interpolation and assignments all execute bytecode. Declaration instructions use
checked type/import helpers, not control/expression interpretation. The AST remains
only as source/type/declaration metadata. The debugger also observes bytecode
checkpoints. Optimization-level tests cover contracts, evaluation order, captured
scope identity, cleanup/finally transfers and rooted values; there is no JIT.

Only active bytecode frames are traced. Frame local/operand Values and nominal
contracts remain GC roots across native callbacks and collection; inactive pooled
frames clear Values/contracts but retain storage capacity. Ordinary calls balance
scratch roots, and parsed declaration-free blocks avoid unnecessary environments.
Closure-bearing scopes retain distinct heap identity. Adaptive collection occurs
at rooted statement/instruction safe points, not arbitrary allocation; provider/SDK
host temporaries therefore retain their explicit-rooting contract.

Immediate member calls lower to ResolveMethod followed by argument bytecode and
InvokeMethod. Resolution selects the callable, receiver and declaring owner before
arguments run, without allocating a BoundMethod. Pending selections are traced
with active operand frames, including across nested calls and argument-triggered
GC. Field-valued callables remain plain values; visibility, initialization,
overrides and super use the same member resolver as detached access. Extracting a
method value still allocates a BoundMethod that retains its receiver/function.

Native collection, String and Bytes methods use shared, traced descriptors.
Each descriptor points to a Runtime-owned NativeMethod definition: receiver kind,
name/result metadata, arity range, construction policy and a receiver-aware function
pointer. native_methods.cpp validates that boundary and invokes the pointer; it
does not decode a method name or AST to choose an implementation. Descriptor
definitions have stable addresses across later registrations. AST signatures are
adapters for existing override/reflection metadata, not executable native bodies.

native_list_methods.cpp, native_map_methods.cpp, native_set_methods.cpp and
native_text_methods.cpp own their registration tables and bodies. Shared semantic
helpers preserve contracts, snapshots and collection construction. Related algorithm
variants are compile-time specializations, not runtime name-switch branches.
Registration checks implementation coverage against the shared core.hpp checker/LSP
catalogues. Adding a built-in method changes its signature, owning table/body and
tests, not VM dispatch. Hosts can also use Runtime::registerNativeMethod without
editing the builtin catalogue; this does not dynamically teach the LSP new methods.

No native closure is manufactured on access and no wrapper recursively forwards a
call. Profiling records one canonical native activation, such as native:List.length.
Result collections, callback snapshots and script method environments retain their
normal allocations and contract checks. See [native-integration.md](native-integration.md#native-method-registration).

## Runtime Services

Bytecode-only migration verification: runtime, parser/checker, profiler, debugger,
generator/task, embedding/thread, editor and LSP tests pass in release and UBSan.
The current Pac-Man model has an added fourth ghost at tile29; existing three-ghost
power-pellet and browser-state expectations fail in test-cli/test-network on both
the frozen pre-migration binary and current VM. That application edit is preserved;
these two groups are not reported as passing full app parity.

profiler.hpp/profiler.cpp own source CPU samples, activation/native/phase timings,
allocation/GC telemetry and Speedscope/Perfetto exports. Config.profiler is null by
default. Runtime frame/safe-point/native/collection hooks do not install debugger
callbacks or select the AST backend. The bytecode loop has profiled/unprofiled
template instantiations of shared operation code; verbose mode alone counts every
opcode. Samples/events/sites are bounded and explicit dropped counters identify
truncation. These are source profiling and VM telemetry, not C++ stack sampling.
CLI simple/verbose views and interpretation are in [profiling.md](profiling.md).

Core installation is isolated in core_module.cpp. Runtime modules expose readonly
System/Reflection/Collections/Text/Math/Tasks bindings without implicit flat native aliases.
core.hpp is the shared signature/member catalogue for checker and editor discovery.
Qualified host registration constructs explicit module paths rather than exposing
all host functionality globally. Reflection returns copied names/type previews.

semantic.cpp owns bounded, frontend-only member resolution. Temporary views use
weak owner links and request-scoped import metadata; imported ASTs expose exports
without executing them. LSP completion/hover use that resolver, including partial
dot recovery. The CLI uses system libedit only for terminal input/history; no
terminal driver is linked into the runtime library or language server.

Collection contracts include map keys/values. Runtime validation stages attachments
to all mutable nested collections, checking existing/staged aliases, then commits
only after validation succeeds. GC follows key and value type references. Map
reads/writes enforce pinned contracts; actual primitive key kinds are validated.
Generic argument parsing is shared between checker/runtime, and LSP diagnostics
reuse checker literal/index/append rules without evaluating code.

Runtime annotation contracts retain resolved nominal record/class/enum objects and
optional element contracts. Bindings and lists mark those type references during
GC. Module-qualified names resolve through namespace bindings; enum patterns
compare constructor/type identities instead of display strings. Function/record
parameter annotations resolve in the defining environment, not the caller.

Classes use distinct ClassType/Instance objects. Class AST parameters retain
field mutability and drive generated field constructors; child function ASTs
are methods. Instance bindings pin their contracts and permit writes only to var
fields. BoundMethod.parent retains the receiver and BoundMethod.origin retains
the function; existing tracing follows both, including collectible cycles. Calls
bind readonly self in the function environment before parameter defaults. No
inheritance, initializer hook, static dispatch or trait implementation is implied.
TraitType declarations own abstract signature ASTs; explicit impl validation resolves
signature contracts in the trait/method defining environments. Class trait vectors
retain nominal identities and are traced by GC. New methods/conformance are installed
only after validation, reusing BoundMethod dispatch. Collections.Sized is a shared
core signature: builtin collection kinds satisfy it, and Collections.len delegates
to a conforming class's length method. No structural duck typing or default-method
dispatch is implied. Checker/LSP keep implementation metadata outside immutable ASTs.

- syntax.hpp: immutable source ownership, tokens, spans and AST representation.
- lexer.cpp: new grammar tokenization; no import of legacy headers.
- parser.cpp: Pratt expressions and statement parsing with precise diagnostics.
- runtime.hpp/runtime.cpp: tagged values, lexical environments, checked semantics,
    object ownership, native functions and module state. Lazy generators retain
    scopes, iterators, captured defers, and try/catch/finally continuations. GC marks
    their saved environments, deferred values, and pending thrown payloads.
- main.cpp: CLI adapters only; errors are library results/exceptions internally.
- tests/: conformance tests owned by this version.
- include/: public embedding interface, separate from both legacy native ABIs.

The backend representation and feature status evolve through tested Tasks. Avoid
adding a version flag to legacy code or promising binary compatibility with it.

Generator loop transfers carry destination scope/iterator depths. A central
unwinder dispatches catches and finalizers before applying a transfer. Explicit
close suppresses cleanup yields; interruption and execution limits cannot be
swallowed by script catches. Unwinding has a bounded script-step reserve, not a
guarantee against allocation failures or blocking native callbacks. GC and VM
destruction do not run user code.

Managed Task objects exclusively own generators and form an acyclic parent tree.
The Runtime owns an active-task root set and FIFO ready queue; there is one
executor and an ambient current-task pointer. Task exit closes children before
generator-level cleanup, and child failures cancel their ancestry. Terminal
tasks detach from scheduling/parent ownership but preserve their latest yield
and failure payload for inspection. The collector marks task ownership edges.

Separate embedded VMs may execute on separate host threads. Shared heaps and
native resources are not implied by that isolation. Native library and process
boundaries are detailed in [native-integration.md](native-integration.md).

Debugger execution checkpoints are opt-in and per-VM. A scoped runtime frame
stack roots live environments while emitting copied, source-owning snapshots;
generators have persistent frame IDs and tasks have independent IDs. The debugger
controller uses frame presence and live generator identities for next/finish,
not native call depth. CLI commands inspect snapshots without evaluating script.
Bounded formatting traverses only managed data, never getters/native callbacks.
The optional owned-source resolver preserves module file identity for breakpoints.
Task inspection copies bounded active-scheduler metadata and parent identities
into snapshots. Console layout and source-context rendering remain in the
debugger adapter rather than the evaluator. Module/source files use `.k3`.

Static typing: src/types.{hpp,cpp} intern types and implement subtyping (unions,
wildcards, Int<:Float, erased generics); src/typer.{hpp,cpp} is the single checker.
It parses the core library from src/prelude.hpp (bodiless stub syntax), resolves
the user-module graph through a loader, infers locals/lambdas bidirectionally,
narrows nullable flow, and annotates the AST (resolved types, Int->Float widening,
numeric operand kinds, native call targets). Every run, REPL submission (via the
stateful TypeSession), --check and LSP document goes through it before execution;
checked programs run with Config::enforceContracts=false so annotations are erased
and the compiler emits slot-only frames, copy-captured closures, typed Float
opcodes and CallNative/CallMethod for statically resolved core calls. The runtime has
no annotation contracts; only `is`/`as` keep a minimal erased type test. The embedding
API owns a TypeSession per VM: dk3_eval type-checks, natives declare signatures.
The declarative optional .k3 syntax contribution is independent of the LSP client.
The editor contribution now includes an optional languageclient adapter. A separate
frontend-only `dotk3-lsp` executable owns versioned document snapshots and speaks
stdio JSON-RPC using nlohmann/json. It maps byte spans to UTF-16 diagnostic/outline
ranges without constructing a VM. The client launches only in trusted workspaces;
language assets remain available without server execution. No module index or
semantic completion/navigation is advertised by this initial server.