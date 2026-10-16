# Concurrency and Native Libraries

Only features explicitly described as current are implemented. DotK 2 native
modules cannot be loaded unchanged: they use a different object and native ABI.

## Execution Boundary

One VM has one executor and owns its heap, globals, modules, tasks, and handles.
Cooperative tasks share objects but never execute simultaneously. One pump turn
resumes one generator until yield, completion, or failure. There is no implicit
background thread, timer, socket polling, or wall-clock preemption. Native calls
and parsing are not preempted by script budgets.

Current operations are `Tasks.spawn(generator)`, `Tasks.pump(turns)`, and `Tasks.cancel(task)`.
Spawn requires a fresh unowned generator. Pump uses FIFO ordering, requeues
yielded tasks, and returns executed turns, not completed Tasks. Readonly task
members are `status`, `done`, `has_value`, `value`, and `error`. Value is the
latest yield, not accumulated output or a return value. Has-value distinguishes
an unstarted task from yielded null. Task return results are not implemented.

Tasks spawned by a running task are its children. Parent completion, return,
uncaught failure, or cancellation closes unfinished descendants before
parent-level generator cleanup, in reverse creation order. This is cancel-on-exit,
not implicit join: keep a parent alive until children finish if all their work is
required. Uncaught child failure cancels its ancestry and siblings, marks ancestors
failed, and escapes pump. Handled in-task errors are not task failures. Unrelated
root tasks stay independent. Earlier turns are not rolled back on later failure.

Cancelling a running task or its active ancestor requests cancellation at the
next script safe point, not forceful thread termination. Suspended cancellation
is synchronous. Subtree teardown shares the bounded generator cleanup reserve.
Nesting is limited to 128 levels. Spawn during cleanup/exit and recursive pumping
are rejected. Active tasks remain rooted after handle release. Explicitly cancel
unwanted root tasks before shutdown: GC/release/destruction do not run generator callbacks.
Opaque resource disposal is a separate explicit owner-thread shutdown operation.

The C API exposes `dk3_spawn`, `dk3_pump`, `dk3_cancel`, `dk3_task_state`, and
`dk3_task_value`. Task-value availability means a latest yield is present, even
after the task ends. Hosts can pump turns between GUI/event-loop operations.
The per-VM API lock prevents host cancellation from interrupting an in-flight
native call from a competing thread.

## Native Method Registration

The internal C++ Runtime exposes receiver-aware registration independently of VM
dispatch. This is not a new C SDK ABI. NativeMethod owns name/result metadata, a
receiver kind, minimum/maximum positional arity, a function pointer and an optional
construction-only policy. Runtime copies the definition into stable per-VM storage;
the caller's descriptor may go out of scope. Callback code must outlive the VM.
Callbacks receive the receiver explicitly instead of capturing managed pointers.

```cpp
runtime.registerNativeMethod({dk3::Kind::List, "count", "Int", 0, 0,
	[](dk3::Runtime &, dk3::Object *receiver, const auto &, const dk3::Span &) -> dk3::Value {
		return static_cast<int64_t>(receiver->items.size());
	}});
```

Both immediate `values.count()` and detached `let count = values.count; count()`
use the same descriptor; subclasses inherit it through normal class lookup. The
generic call boundary validates receiver kind, construction state, positional names
and arity. Bodies remain responsible for operation-specific types/contracts and
use checked Runtime operations for mutation. Receiver, callable and arguments are
rooted for the call; longer-lived managed values still require handles. Native code
remains trusted owner-thread code, not a sandbox or a stable C++ binary interface.

Builtins register through type-owned tables in native_list_methods.cpp,
native_map_methods.cpp, native_set_methods.cpp and native_text_methods.cpp. Their
signatures come from the existing core.hpp checker/LSP catalogue. Installation
rejects incomplete tables, uncatalogued names, duplicate registrations, missing
callbacks, empty names and invalid arity ranges. Add a builtin signature and its
owner-table callback, then tests; no VM method-name switch needs updating.
Host-only registrations do not automatically extend frontend/LSP catalogues.

## Threads

Current embedding permits independent VMs on separate host threads, without a
global VM execution lock. Same-instance reentry returns BUSY. The two-thread
embedding test requires native callbacks to overlap, not merely eventually finish.
It is not a throughput benchmark or comprehensive race-detector result.

Use one VM per worker and exchange copied data. Never transfer DK3Value handles,
managed Object pointers, closures, or native connection pointers into another VM.
Hosts can copy a String result, create a String in another VM, and call a receiving
function. Copy borrowed strings before releasing their values or destroying the
VM. A standard worker/channel library is not implemented yet.

Hosts must synchronize shared callback userdata, output, external libraries,
handle lifetime, and destruction. Isolation does not make raylib or a database
connection thread-safe. Keep the internal C++ Runtime on one executor; use the
C boundary for host concurrency. Do not concurrently release an in-use handle.

## Processes

For stronger isolation, launch independent host/runtime processes and exchange
serialized data over pipes, sockets, or a host-defined transport. Connections,
textures, closures, and VM handles stay process-local. Prefer spawn/exec over
copying a live multi-threaded VM with fork. Process termination does not promise
script cleanup or transaction rollback.

The alpha has no process API, IPC codec, process pool, or script worker library.
These are host integrations today, not existing DotK 3 language features.

## Raylib

The intended binding is a registered native module or host adapter. Keep window,
graphics-context, input, and drawing operations on the main/render thread,
particularly on macOS. Independent workers can prepare assets/simulation data,
then send copied commands to the render VM. GPU upload stays on the context owner.
Pump turns between frames, but turns do not guarantee a frame-time budget.

Textures, windows, shaders, and audio need typed opaque resource wrappers with
owner/thread checks, use-after-close validation, and idempotent release. Release
textures before the graphics conText. Explicit close/defer should be authoritative;
GC must not arbitrarily execute GPU operations on another thread.

No DotK 3 raylib binding ships today. Owned aggregates, thread-affine resources and
versioned module registration now provide the adapter foundation. Context setup,
actual rendering bindings and packaging still need implementation; the existing
legacy module must be ported.

## Databases

Use an adapter around SQLite or another native client, not reimplemented SQL.
Give each connection a VM/worker owner. Use separate connections or a serialized
database actor, rather than sharing pointers between worker heaps. Transaction
and locking rules still apply across connections; check the library's compiled
thread-safety mode.

Default to prepared statements and parameter binding. Provide typed rows,
structured errors, explicit statement/connection close, and a transaction guard
that rolls back unless committed. Close statements before connections. End child
tasks before closing their parent's connection. Parent-level cleanup ordering
does not make an arbitrary inner lexical resource scope safe automatically.

A synchronous query blocks that VM. Use native timeouts/library-specific
interruption, or do blocking work outside the VM on a database worker and return
copied results through a future completion queue. Script cancellation cannot
stop an already-running SQL call by itself. Await/native completion integration
is not implemented yet.

No database driver ships in this alpha. Contextual collection callbacks, opaque
parent-aware resources and native module registration now support driver adapters.
Prepared statements, parameter conversion, transactions and interruption still
need a real SQLite binding. Task cancellation alone is not transaction safety.

## Read-Only File Capability

Files.readText/readBytes/exists/size are implemented through per-Runtime callbacks,
disabled by default. No runtime or embedding library opens OS files implicitly.
The CLI's --files-root DIR adapter reads regular files with bounded chunked I/O,
canonical root/relative path checks and resolved symlink containment. This is not
race-proof: checks/open can race filesystem mutation, and native I/O is blocking.
It does not change script/module loading or create a filesystem sandbox.

C hosts initialize DK3Files with dk3_files_init, provide read/info callbacks and
call dk3_set_files(vm, &files). abi_version must be DK3_FILES_ABI_VERSION and size
must cover the struct. The existing DK3Config layout is unchanged. max_bytes
defaults to 4 MiB; zero permits only empty reads. Either callback may be omitted
to deny that operation. dk3_set_files(vm, NULL) revokes both capabilities.

Read callbacks receive path and maximum bytes and return DK3_OK plus a borrowed
byte span, copied immediately before any script/native operation. Keep the buffer
valid until the enclosing API call returns (or the next provider call), and
synchronize it across independent VMs. NULL with zero length is valid; NULL with
nonzero length or length above the limit is rejected before copying. Metadata
returns existence 0/1 and uint64 byte size; absent ignores size. Return DK3_ERROR
for provider failure or DK3_LIMIT for a limit failure. Host exceptions do not cross
the C ABI. Script errors retain the calling source span.

Same-instance reentry/revocation/collect from a provider returns DK3_BUSY. Userdata
and callback lifetimes belong to the host and must outlive installation. Revocation
does not invalidate existing String/Bytes handles. C providers may implement virtual
files; they must enforce their own authorization/path policy. Text validates UTF-8,
binary reads preserve bytes, exists may return false, and size missing/overflow
fails. All providers remain trusted host code, not sandboxed callbacks.

## Scalar and Value Interoperability

Host handles support creation/readback for Null/Bool/Int/Float/String/Bytes and
dk3_get_type categories for managed values. Float creators/returns reject NaN and
infinity; getters are strict kinds, not conversions or nominal-name comparisons.
String/Bytes preserve embedded NUL with explicit lengths. Zero-length NULL buffers
are accepted. Handle buffers remain borrowed until release/VM destruction.

Native callback getters cover Int/Bool/Float/String/Bytes; corresponding return
helpers include Null and binary output. Borrow arguments only during the callback,
and keep output bytes valid until the helper returns (they are copied immediately).
dk3_native_return_value accepts an existing live same-owner DK3Value; it copies
the managed reference and does not transfer/release the host handle. Foreign or
released handles fail. Public API calls inside a callback remain BUSY.

## Owned Collection Interoperability

dk3_list/tuple copy live same-owner references into managed containers; dk3_map
creates an empty Map. sequence_size/get work for Lists/Tuples and get returns an
owned child handle. list_append/set enforce pinned contracts; Tuples are immutable.
map_size/get/set/remove/entry preserve primitive/pinned keys and values. Missing get
is found=0/NULL output; stored null is found=1 with a real handle. Entry extraction
returns two owned handles into distinct output slots in live insertion order.
Indexes may shift after mutation; copy entries for stable traversal. Wrong kind/
range/owner/released handles return INVALID, runtime contract errors return ERROR
without applying the mutation. Public API locking is unchanged.

Callback-local helpers require the current thread's active DK3Call and do not
invoke scripts/getters or resume generators. Argument/get/entry and constructor
handles are owned and require release: dk3_native_release during a callback,
dk3_release afterwards. The DK3Call context must not escape callback duration.
Other VM/outer callback/other thread contexts are invalid; public reentry remains
BUSY. Scalar helpers check context before managed allocation/access too.

dk3_native_return_value temporarily roots the result independently of builder
release and later allocations; the root is released on success/error. Child handles
independently root objects after parent release, but never outlive VM destruction.
example-native builds examples/structured_host.c, exchanging typed script/JSON
values without raw Object pointers. The resource/module ABI below is separate from
this aggregate plumbing; neither provides native wall-clock preemption.

## Opaque Resource ABI

DK3Resource is size/version checked (DK3_RESOURCE_ABI_VERSION=1), with a copied
nonempty UTF-8 tag, nonnull payload, dispose callback/userdata and optional parent
handle. Only successful creation transfers disposal responsibility; every returned
handle still requires release. Payload/tag and callback userdata are trusted host
inputs, not authorization tokens. No raw managed Object is exposed.

Each resource owns a distinct state with its creation thread. Get requires its
exact tag, live handle and open state; it returns a borrowed pointer valid until
close. Wrong type/tag/owner/released input returns INVALID, closed get ERROR and
live wrong-thread get/close BUSY. State queries and closed-idempotent close need
no owner-thread execution. Callback-local helpers require the active current call
as well as the resource owner. Public same-VM callback reentry remains BUSY.

Optional parents must be open, same-VM, current-thread resources. Live children
block explicit parent close. Parent relationships cannot be changed and form no
cycles; state survives parent handle release/managed GC. Close marks the state
closed and clears the payload before invoking disposal exactly once. Alias handles
observe the same state. dispose must complete cleanup and must not throw. Defensive
exception handling reports ERROR without retrying an already-started disposal;
external side effects cannot be rolled back.

Resource wrapper collection never executes disposal. Open registrations remain
until close/shutdown and are separately capped by DK3Config.object_limit, including
forgotten wrappers. Closed records are pruned on subsequent creation; their managed
aliases may remain alive. Resource payload bytes are host-controlled, not covered
by the managed object or step budgets. Explicit dk3_collect reclaims unreachable
managed objects; release/close alone do not promise that reclamation.

dk3_close_resources and dk3_destroy take the ordinary VM lock and preflight all
remaining owner threads before any disposal. Wrong-thread shutdown returns BUSY,
leaving every resource and the VM alive. Dispose in reverse creation order after
preflight, so children precede parents. For resources created on different host
threads, explicitly close them on those threads before final shutdown. Cleanup
is available even on an OOM-poisoned VM. Disposers receive no active DK3Call; API
reentry is BUSY and callback-local helpers INVALID. Synchronize userdata and all
destruction externally. There is no disposal queue or automatic thread migration.

Resource is a runtime/checker annotation category, not a constructor or custom
nominal resource schema. Scripts read immutable closed/type_name fields; reflection,
debugger and LSP metadata inspection do not access pointers or execute callbacks.
Adapters validate their concrete tags before native use. Explicit adapter close
can be captured with defer; task cancellation does not by itself guarantee safe
lexical resource regions or transactions.

## Versioned Native Module Registration

DK3NativeModule and DK3NativeExport use DK3_MODULE_ABI_VERSION=1. Initialize with
dk3_module_init, supply one module name, optional docs and contextual named exports,
then dk3_register_module. Names are single ASCII identifiers, excluding language
keywords. Duplicate exports and existing global/core/module names fail without
publishing anything. Header version/size, null pointers and callback presence are
validated. Native docs must be UTF-8.

Build the namespace privately under a temporary GC root, then publish a readonly
prelude binding once. Object-limit failure releases that root and leaves no binding;
explicit collection can reclaim staged objects before a smaller retry. Native
imports use existing import/from/alias rules, need no source resolver, and expose
copied docs via Reflection.doc and exports via Reflection.members. Functions use
the existing current-thread contextual dispatch and reject named arguments whose
labels this ABI does not describe. Legacy define_native behavior is preserved.

Descriptor arrays, names and docs may be released after registration. Function
code and userdata must remain valid until VM destruction, including disposal of
registered resources before unloading the host library. No unregister, binary
loader, manifest, native ABI negotiation beyond the versioned descriptor, or static
native signature inference ships yet. examples/structured_host.c demonstrates
typed aggregate and Counter resource adapters using only the public C API.

## Next Native Milestone

- Small prepared-statement SQLite binding using contextual module/resource APIs.
- Thread-affine raylib binding with graphics-context parent ownership.
- Native module packaging/loading and editor signature manifests.
- Owner-thread disposal queues if integrations need asynchronous shutdown.
- Native completion queues and cooperative wait/await integration.