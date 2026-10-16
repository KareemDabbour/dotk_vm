# CLI and Debugger Milestones

The CLI is a first-class development surface, not a wrapper to leave until the
end of the language rewrite. This work stays in DotK 3 and does not reuse the
version 2 VM or promise old syntax/flag compatibility.

## CLI Core: Implemented

- No arguments on a terminal opens a persistent shell; piped input runs a batch.
- `--repl` forces the shell, including in a pipe, without non-terminal prompts.
- `FILE.k3 [args...]`, `- [args...]`, and `-e SOURCE [args...]` run scripts,
  stdin, and inline code. `--eval` aliases `-e`; `--` permits dash-prefixed filenames.
- `-i FILE.k3 [args...]` runs a script then keeps its VM alive in the shell.
- `args` is a predefined immutable tuple of user arguments, excluding the
  executable/script name. Embedded VMs default to an empty tuple.
- `--help`, `-h`, `--version`, `--check PATH [PATH...]`, and `--ast` are available.
- Shell commands are `:help`, `:reset`, `:quit`/`:q`, `:load FILE.k3`, `:ast FILE.k3`,
  `:check PATH [PATH...]`, and `:members NAME[.MEMBER...]`. Double-quote paths containing spaces.
- On macOS/Linux, libedit provides arrow-key cursor movement and up/down recall,
  with at most 500 lines of in-session history. Nothing is written to a history
  file by default. Piped input and the Windows fallback remain plain line input.

Multiline completion comes from parser EOF diagnostics, not counting braces in
comments/strings. The shell evaluates each complete submission once and echoes
its final non-null expression. Functions, bindings, closures and module state
persist. Syntax/runtime errors are reported without discarding the session;
runtime side effects are not rolled back. An unfinished submission at EOF is
an error. Ordinary strings still cannot cross physical lines; put an optional
`else` on the closing-brace line or submit the whole if/else together.

File imports resolve relative to the script directory. Shell/inline/stdin imports
resolve from the current directory; a script-followed shell keeps that script's
base. This is not a symlink-hardened sandbox. Reset clears globals and module cache.
`:load` parses/runs a file in the same VM with its filename retained in diagnostics.
It does not change the session's module-resolution base or directory; use `-i`
when the file's own directory should establish the initial import base. Re-loading
declarations can report duplicate bindings, and failures do not roll back earlier
side effects. `:check` treats files as independent units rather than inspecting
the session's globals. Shell file commands reject `-` to avoid consuming the
command input stream; command-line stdin modes remain supported. Malformed
quoted paths are rejected before file execution.

The CLI explicitly cancels outstanding tasks on reset and normal exit before
destroying its VM. Cleanup is bounded and can fail; reset reports that failure
then creates a fresh VM. OS termination, crashes and Ctrl-C are not currently
guaranteed cleanup paths. Native calls can still block the shell.

Exit codes retain this alpha's policy: 64 invalid usage, 65 language/incomplete
input or shutdown failure, 70 host/file errors, 0 successful operation. Recovered
shell errors do not make a later normal quit fail. This is not exact version 2
compile/runtime exit-code parity.

## CLI Polish: Planned

Project checks are implemented: file/directory/stdin inputs, deterministic `.k3`
traversal, normalized-path deduplication, per-file diagnostics and pass counts.
They do not execute code or resolve module interfaces. Empty scans are errors.
Known local record fields/constructors, enum payloads and exhaustive matches are
checked; dynamic cases remain runtime-authoritative. Optional editor syntax and
bracket/comment assets are separate from LSP/semantic diagnostics.

libedit now handles line editing/history; persistent history, tab completion and
interrupt cleanup are follow-ups rather than a custom raw-mode driver.
Inventory version 2's test runner, profiler, tracing and script-then-shell flags
before deciding which to port; do not expose accepted-but-ignored switches.

## Debugger Core: Implemented

REQ-017 now has runtime hooks and an opt-in CLI debugging session. AST printing
and runtime diagnostics remain separate. Implemented operations are:

- Source file/line breakpoints with enable/disable and list operations.
- Continue, step into, step over, and step out.
- Call stack, frame selection, and readonly local/global inspection.
- Source context with active-line marker, generator/task identity and aligned inspection tables.
- Active-task inspection including parent IDs, state, generator frame and location.
- Copied, bounded previews that can survive collection and VM destruction.

Run `dotk3 --debug FILE.k3 [args...]`. Commands are `break`/`b [file:]line`,
`breakpoints`, `delete id`, `enable id`, `disable id`, `continue`/`c`, `step`/`s`,
`next`/`n`, `finish`/`f`, `bt`/`where`, `frame index`, `locals`, `globals`,
`tasks`, `print`/`p name`, `list`, `help`, and `quit`/`q`. Frame selection changes inspection,
not the executing frame. Stepping controls the current active frame.

The initial executable checkpoint stops automatically. Breakpoint locations are
normalized file/line pairs; lines without a runtime checkpoint will not stop.
Function-entry and return statements are distinct checkpoints, including when
they share a line. Native callbacks are treated as step-over operations. Stdin
cannot simultaneously supply the script and debugger commands, so `--debug -`
is rejected. Commands can be piped, and debugger-input EOF requests quit.

Inspection previews at most 64 bindings per frame/globals, eight list/tuple
elements, four map entries, two nested container levels, and bounded strings.
Records, variants, functions, tasks and native resources currently have type
previews rather than expandable member inspectors. `print` looks up a copied
binding: expressions, member access and function calls are not evaluated.
Locals include visible captured bindings with nearest-scope shadowing.

The console shows two neighboring lines on either side of the stop, clipping
individual source lines at a UTF-8 boundary. Variable previews use aligned rows
and continuation indentation instead of spilling long values into the next row.
Stack/frame selection and breakpoint lists have their own headings/tables.
Help is grouped into execution, breakpoints and inspection commands.

`tasks` is a copied read-only view of up to 128 active scheduler entries, sorted
by task ID. It includes parent, state, frame and next/current source location,
with `=>` marking the executing task. Finished tasks are no longer in the active
queue. This view neither switches execution nor pumps/cancels tasks while paused.
Snapshots own task source spans and do not expose live task pointers.

Quit requests cooperative unwinding and suppresses further stops. It is not
forceful native interruption. Separate shutdown failures are still reported.
Step-over/out waits for a suspended generator to resume or end instead of using
ordinary native call depth. Other tasks may run while that generator is suspended;
their breakpoints can still stop execution.

The internal C++ `Config::debug` observer receives a `DebugSnapshot` independent
of the CLI. Execution hooks and counters are per-VM. Checkpoints observe
statements, script-call entry, condition iterations and yield/return statements
in both AST evaluation and lowered generator control flow. A shared statement
execution path prevents duplicate AST/lowered Execute notifications.

Snapshots own source spans and copied formatted values, not managed heap pointers.
Live execution environments/generators are GC roots during the callback. Execution
and mutation calls through Runtime reject debugger reentry; collection is allowed.
The existing C API's same-instance reentry remains BUSY. Formatting does not call
getters, script expressions or native callbacks. There is no shared debugger state.
`Config::resolveSource` can supply owned file metadata; the CLI uses it so imported
module breakpoints identify their actual paths, while the older text resolver
remains supported.

Generator identities persist through suspension even when native call depth
returns to zero. Pausing one VM does not pause other worker VMs. It is not a
C/C++ debugger and cannot preempt a blocked native call.

Acceptance tests cover nested calls and real next/finish stop sequences, repeated
loop lines, shadowing, captured closures through GC, generator resume, task switches,
defer/finally and handled errors, callback reentry, disabled execution, VM isolation,
module frames, owned snapshots, preview limits, and CLI commands/quit cleanup.

Pause-on-exception, watch evaluation/mutation, expandable objects, a public C
debugger adapter, interrupt-to-pause, remote protocols, VS Code DAP integration,
and a graphical debugger are later increments. CLI line editing remains pending.