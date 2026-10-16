# DotK 3 Editor and LSP

Optional VS Code language support for `.k3` files. It is separate
from the legacy `.k` extension and does not change the user's installed extensions.

Build the native server, install the client dependencies, then preview in an
Extension Development Host from the repository root:

```sh
make -C dotk3 CXX=clang++ CC=clang lsp
npm --prefix dotk3/editor ci --ignore-scripts --registry=https://registry.npmjs.org --userconfig=/dev/null
code --extensionDevelopmentPath="$PWD/dotk3/editor" "$PWD"
```

The explicit public registry/empty user config avoids dependence on private npm
credentials; it does not modify user settings. Trust the development workspace
and open a `.k3` document. The client detects a release `dotk3-lsp` in the source
tree/workspace. For another build/location, set the absolute `dotk3.serverPath`
setting and run **DotK 3: Restart Language Server**. Untrusted workspaces receive
syntax support only; no server executable is launched.

## What Changes in the Editor

The status bar shows **DotK 3: Running** for an attached DotK 3 file. Click it or
run **DotK 3: Show Language Server Status** to inspect the active language mode and
diagnostic count, open Problems/Outline, or launch a safe test. If a `.k3` file has
another language mode, the indicator says **DotK 3: Language Mode** instead.
Choose **DotK 3** in the editor's language selector; no user association settings
are changed automatically.

Run **DotK 3: Test Editor Integration**. It opens an unsaved DotK 3 buffer with an
intentional Int/String type error, waits for the diagnostic, and reports whether
the editor received it. The sample is neither saved nor executed. Replace the
string with `1` to see the squiggle/Problems entry clear. File and untitled
DotK 3 documents are attached to the same language server.

For valid source, zero diagnostics is normal: open **Outline** to see declarations,
or use **Go to Symbol in Editor**. Syntax colors come from the grammar and theme,
not a server-start notification. Member hover and dot-completion are implemented;
general completion/navigation are still pending. Type `Reflection.` or `point.`
for suggestions; hovering a known record field shows its declared type and any
record documentation. Custom record/enum/function declaration hover includes
signatures and leading docstrings. Explicit import aliases and collection
`.length()` members are supported, including bounded direct-module metadata.
Classes have outlines with fields/methods, method signatures/docs and completion
for instances, implicit `self` and annotated method results. Class readonly/type
diagnostics use the shared checker; imported metadata remains bounded/conservative.
Traits and implementation blocks have outlines and typed method signatures;
Class-local `implements` clauses are highlighted and displayed in class hover
and outline details, including multiline/multiple trait headers.
The shared checker diagnoses invalid init hooks, default-body returns/conflicts
and immutable record writes. Records expose fields/methods in outlines and
completion; inherited defaults and trait-body self are discoverable without execution.
Map contains/remove/keys/values have shared signatures and completion. Typed
snapshot element contracts feed index/member discovery and append diagnostics.
List append/insert/removeAt/extend also share signatures. Live index/element/arity
errors use the checker, and typed removeAt results preserve known member discovery.
List search/clear/reverse and Map clear/get share the same catalogue. Map get
results preserve V? and diagnose incompatible nonnullable assignments.
Anonymous literals and Function-annotated bindings expose known signatures and
typed results. Block/arrow body parameters participate in member completion;
call diagnostics clear on valid edits and parameter scopes end with the function.
String literals/bindings expose shared method signatures; split results retain
List[String] element discovery, and join arguments use known source-element checks.
Files namespace/import aliases expose path/arity diagnostics and String/Bytes/Bool/
Int results. Discovery/checking never invokes file providers or grants file access.
Json parse/stringify provide readonly completion/import signatures. parse input
is checked as String and stringify returns String; parsed shapes stay dynamic.
The language server does not parse or serialize JSON source expressions at runtime.
trait-annotated values, Sized aliases and impl self/added methods participate in
completion. Signature mismatch diagnostics clear after valid edits. Imported
exported classes can expose direct-module implementation methods without execution.
These features appear in the **Extension Development Host** where this extension
is loaded, not automatically in the original VS Code window. Reload that host
after updating extension code. The integration test reports a timeout if document
diagnostics are not received; inspect language mode and server Output in that case.

The assets contribute syntax highlighting, comments, bracket pairing and basic
brace indentation. Records, enum variants, types, named functions, control flow,
capitalized core namespaces, numbers and string/byte escapes use standard TextMate
scopes and the current theme. Triple-quoted multiline docstrings are highlighted.
The native LSP publishes parser/checker diagnostics for open documents, including
unsaved full-text changes, with UTF-16 positions and stale-version protection.
It clears diagnostics on fix/close. Outline/document symbols include top-level
records, enums/variants, named functions and bindings. The server does not execute
user code or instantiate a VM. It shares the CLI checker's conservative limits.
Member discovery resolves local/annotated records, function parameters, typed
indexes, core namespaces and enum variants. Direct local import metadata is read
without executing source and unsaved modules take precedence. This is bounded
best-effort member inference, not a complete module/type index. Go-to-definition,
renaming, formatting, full inference, semantic tokens and a debug adapter remain
pending. Restart the language server after rebuilding it.

Run checks in the terminal:

```sh
dotk3/build/release/dotk3 --check dotk3/examples
make -C dotk3 test-editor
make -C dotk3 test-lsp
npm --prefix dotk3/editor run build
npm --prefix dotk3/editor test
```

The asset tests validate JSON/regex syntax and mocked VS Code activation, trust,
path and restart behavior. Native protocol tests exercise live documents, Unicode,
version/lifecycle and error handling. The npm test additionally uses the maintained
JSON-RPC transport against the actual server. The complete VS Code Extension
Development Host interaction has not been automated/visually verified here.

The native builds need no npm packages. LSP JSON parsing uses a pinned vendored
header; tests use Ruby/Node. The optional client uses vscode-languageclient with
a committed npm lockfile. Packaging/marketplace publication are not included.

## Process Exit Messages

The upstream language client logs executable exits as errors even when a normal
restart/stop completes with code 0. The DotK 3 adapter now reports expected
zero-code exits as informational, while unexpected exits and nonzero codes remain
errors. The output channel also shows the exact server executable being launched.
Unexpected stdin EOF is reported by the native server as exit 1 with a reason;
successful `shutdown`/`exit` still returns 0.

After changing extension code, reload the Extension Development Host window using
**Developer: Reload Window**, then run **DotK 3: Restart Language Server**. Check
the **DotK 3 Language Server** output channel for the selected binary and any
preceding protocol/launch diagnostic. A zero-code line alone does not prove that
the server crashed.