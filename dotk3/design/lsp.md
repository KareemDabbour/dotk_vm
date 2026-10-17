# Initial Language Server

`dotk3-lsp` is a separate stdio LSP executable linking only lexer, parser and typer
code (diagnostics, completion and hover all come from the typer). It never creates a Runtime or executes source. The transport uses
JSON-RPC Content-Length framing and the pinned nlohmann/json parser. Protocol
messages are the only stdout output; fatal transport errors go to stderr.

## Implemented

- Initialize/shutdown/exit and protocol error responses.
- Open/close/save notifications and full-document change synchronization.
- Owned URI/text/version snapshots; stale changes are ignored.
- Parser and typer diagnostics (errors and warnings, imports resolved from open documents first) for unsaved buffers, cleared after fixes/close.
- Synchronized typed-map/list diagnostics: literal/default/return values, key/index
	types and typed list append arguments where their contracts are known.
- UTF-16 positions, including non-BMP Unicode and CRLF line endings.
- Top-level document symbols for records, enums/variants, functions and bindings.
- Member hover and dot-completion for core namespaces, records, enums, typed
	collection results and annotated function parameters, including partial dots.
- Custom record/enum/function declaration signatures and docstring hover; record
	field hover includes schema documentation. Explicit core/imported aliases and
	List/Map constructors plus collection `.length()` use the same metadata path.
- Class/field/method outlines, declaration/method docs and signatures, instance/self
	completion, annotated method return discovery and direct imported class aliases.
	Readonly fields and known class argument/return checks share CLI diagnostics.
- Trait/implementation outlines, signature/doc hover, trait-typed/core Sized member
	discovery and self/added method completion inside implementations. Local signature
	and conformance diagnostics use the same checker as the CLI.
- Java-style class-local implements clauses in hover and outline details, including
	multiline/multiple headers, with live missing/mismatched method diagnostics.
- Initializer hook diagnostics, trait-default body checks/self and inherited
	default completion, and immutable record field/method outlines and discovery.
- Shared map contains/remove/keys/values signatures, primitive/typed key and arity
	diagnostics, snapshot List<K>/List<V> results and known indexed element discovery.
- Shared list insert/removeAt/extend signatures, index/element/arity checks,
	known removed-element discovery and live extension error clearing.
- List contains/indexOf/clear/reverse and Map clear/get signatures; nullable get
	results and edit/fix diagnostics share the nonexecuting checker.
- Anonymous literal/binding signature hover, typed result discovery and nested
	block/arrow parameter scopes, with live known-call diagnostics and scope limits.
- Binding hover shows let/var, name, declared or inferred type, export status and
	short single-line literal initializers, retaining callable signatures and docs.
	Declaration sites, references, shadowed locals and exported module members use
	the same source metadata. Native import aliases show their origin/signature.
	Known primitive expressions, small homogeneous containers, nullable fallback,
	constructors and generator calls have conservative initializer type discovery.
- String literal/binding methods, strict argument checks, typed split element
	discovery and known iterable String element validation for join.
- Files namespace/import signatures, String path checks and typed results without
	invoking the runtime/provider or granting filesystem access.
- Json parse/stringify signatures and typed serialized results without executing
	JSON conversion; parsed values remain conservative Any until annotated.
- Optional VS Code client with trust gating, native-server path and restart.
- Visible active-editor connection/language-mode status and an unsaved diagnostic
	probe command. DotK 3 untitled buffers are synchronized alongside file documents.

Only these capabilities are advertised. Symbols are available for successfully
parsed documents even when a checker error exists. Syntax-invalid documents
currently have no outline. Each document gets the first frontend error, not all
possible errors; project checks and LSP do not resolve module export interfaces.

Full synchronization is deliberate: incremental change ranges are not supported
and are rejected. Requests are synchronous; cancellation notifications do not
preempt an in-flight frontend check. Documents must be opened before querying;
the server does not crawl a workspace index. Member discovery may read direct
relative import metadata from file-backed documents. It rejects absolute/parent
paths, prefers unsaved open modules, caps reads at 32 per request and 4 MiB each,
and only exposes exported declarations. This is not symlink-hardened sandboxing.
Untitled imported-file discovery and general module interface checking remain
limited. Temporary views retain source ownership, not runtime objects or pointers.

## Bounds and Limits

Headers are limited to 8 KiB, framed messages to 16 MiB, individual documents to
4 MiB, open documents to 64, and retained source text to 32 MiB. Text limits do
not bound AST/token memory or parser stack depth. The checker/parser are not
wall-clock preemptible. These are conservative transport limits, not a sandbox.
Malformed JSON with valid framing receives Parse error; invalid/truncated framing
terminates the process because resynchronizing arbitrary bytes is unsafe.
Unexpected input EOF also returns a nonzero status and stderr diagnostic; only
the requested shutdown/exit handshake returns success. The client classifies
expected zero-code restart/deactivation exits as informational rather than hiding
unexpected exits or nonzero failures.

General completion, definition/reference navigation, signature help, rename,
formatting, incremental parsing, module-aware symbols and full inference are
follow-up work. LSP is not the source debugger/DAP protocol.

Binding details describe source declarations/initializers, not current runtime
values or flow-sensitive reassignment. Explicit annotations (including Any and
nullable types) remain authoritative; unknown dynamic results are labeled unknown.
Literal previews are limited to 80 source bytes and never truncate UTF-8 values.
Container inference examines at most64 initializer children with the existing
resolution depth bound. Hover never runs initializers, callbacks, IO or toString.

## Verification

Ruby protocol tests drive the actual server. Node tests validate client activation
using mocked VS Code APIs and exercise real vscode-jsonrpc stream interoperability.
Tests cover lifecycle, stale versions, clearing diagnostics, Unicode/CRLF positions,
record/enum outlines, invalid JSON, unsupported requests and oversized frame declarations.
Member tests cover local/parameter/imported records, unsaved module overrides,
typed map values, enum variants, core signatures, tasks and partial-dot recovery.
Docstring tests cover declaration and field hover, explicit import aliases and
maintained-client transport. Discovery never executes user source; general runtime
reflection and complete module interfaces are absent. Generic/inherited
trait inference is not implemented. Class
tests cover self/returned/imported instance discovery, readonly diagnostic clearing,
method docs/outlines and actual client completion.
Binding protocol tests cover declarations/references, mutability, scalar/container
inference, native imports, nullable/Any contracts, shadowing, aliases, exported
visibility, generator/callable results and initializers that must not execute.
This does not claim a fully automated VS Code editor-host integration test.

Dependency self-check details are in [lsp-dependencies.md](lsp-dependencies.md).
The npm advisory scan is current to this batch; the vendored C++ review was a
best-effort fallback, not a security certification.