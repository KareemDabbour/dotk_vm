# DotK 3 Implementation Plan

## Architecture

Owned Source -> Tokens -> source-spanned AST -> optional lexical/basic checking
-> bytecode-only execution. Slot and lexical control instructions cover normal
functions, methods/closures, structured handlers, loops and suspended generators;
the AST expression evaluator/statement walker and engine option are removed.
Default O2 uses checked specialization and invalidation-safe binding caches;
frame slots/rooting and adaptive safe-point GC are measured and parity-tested.
This is not the DotK 2 VM or a claim of a completed all-language/JIT pipeline.

Internals use C++17 for ownership, AST variants, and checked arithmetic. Public
embedding must not expose C++ types. Heap objects and captured environments need
tracing collection; reference counting alone cannot collect closure cycles.

## Tasks

| Task | Requirements | Acceptance |
| --- | --- | --- |
| T001 Independent build and lexer | REQ-001, REQ-006, REQ-013 | Newline/token/source-span/escape tests; no parent modifications. |
| T002 AST frontend and core runtime | REQ-002..REQ-007 | Binding, arithmetic, boolean, collection, Unicode, closure and call tests. |
| T010 Typed collection contracts | REQ-005, REQ-010, REQ-018 | Map key/value read/write/alias/GC contracts, atomic nested attachment, static known collection checks and synchronized live LSP errors. |
| T011 Modular core and member discovery | REQ-013, REQ-016, REQ-018, REQ-019 | Readonly core namespaces/shared catalogue, reflection and qualified host registration; record/core dot-completion/hover; libedit terminal editing/history tests. |
| T012 Language API and documentation | REQ-005, REQ-006, REQ-009, REQ-012, REQ-018, REQ-019, REQ-020 | Capitalized namespaces, explicit imports, record spelling, List/Map construction/copy, shared length, triple docstrings and custom declaration/import hover; runtime/checker/LSP/CLI/client tests. |
| T013 Mutable classes | REQ-009, REQ-010, REQ-013, REQ-017, REQ-018 | Class state/field constructors/self/init implemented; static/inheritance extensions ship in T032/T033, custom constructor arguments remain follow-up. |
| T014 User-defined traits | REQ-005, REQ-009, REQ-010, REQ-018, REQ-021 | Nominal synchronous contracts, class/record impl/defaults, Sized bridge and source-only editor metadata; generic/inherited traits remain follow-up. |
| T015 Lifecycle/default/record composition | REQ-022 | Three sequential verified increments: class init hooks, trait defaults/override conflicts and immutable record methods; combined host/GC/checker/LSP/example contracts. |
| T016 Map membership/removal/snapshots | REQ-023 | Three sequential verified increments: contains, remove and contract-preserving keys/values snapshots; shared checker/reflection/LSP metadata and host/GC/example tests. |
| T017 List insertion/removal/extension | REQ-024 | Three sequential verified increments: strict insert, typed removeAt and staged iterable extend; atomic contracts/budgets and shared tooling/host ownership tests. |
| T018 Collection queries/reset/lookup | REQ-025 | Six focused-validated increments: List contains/indexOf/clear/reverse and Map clear/get, with equality/contracts/budgets/nullable typing and host/editor gates. |
| T019 Anonymous callable tooling | REQ-026 | Standalone/direct fn literals, immutable-signature checking, block/arrow parameter scopes, result/signature discovery and CLI/real-client coverage. |
| T020 String search/split/join | REQ-027 | Six sequential increments: membership/prefix/suffix/code-point find/typed split/strict join; shared tooling, literal inference and budget/GC/host tests. |
| T021 Read-only Files capability | REQ-028 | Disabled default runtime callbacks, typed module, rooted opt-in CLI, versioned C grant/revoke API and end-to-end path/limit/UTF-8/host/editor tests. |
| T022 C scalar/native interoperability | REQ-029 | Strict scalar/type/binary handle API, callback getters/returns and same-owner value returns; GC/released/foreign/BUSY/finite boundaries tested. |
| T023 JSON interchange | REQ-030 | Pinned SAX parse/exact integers and bounded compact encode with cycle/duplicate/type/depth limits; Files/C handles/checker/LSP/client workflows. |
| T024 Owned host collection API | REQ-031 | List/Tuple/Map builders, owned traversal, checked mutation and kind/owner/contract/GC boundaries. |
| T025 Callback structured adapters | REQ-032 | Active-context aggregate helpers, public BUSY and temporary result roots with C adapter/concurrency tests. |
| T026 Opaque resource lifecycle | REQ-033 | Versioned descriptors, owner/thread/tag/parent checks, explicit close, bounded registrations, preflight shutdown and readonly metadata/tooling tests. |
| T027 Contextual module registration | REQ-034 | Atomic versioned readonly exports, imports/docs, invalid/collision/limit rollback and public resource module example. |
| T028 Callback collection workflows | REQ-035 | Seven snapshot-based transformations/folds/queries, GC/type/budget/failure behavior and signature/result editor tests. |
| T029 Procedural Random service | REQ-036 | Five seeded per-VM services, strict intervals/UTF-8 choice/atomic shuffle preflight and replay/editor/example tests. |
| T030 Conditional Map updates | REQ-037 | Five compute/conditional operations, typed callbacks/null semantics, mutation/GC/error safety and editor/example tests. |
| T031 Trait intersections | REQ-038 | Grouped conjunction/nullable syntax, pinned all-trait contracts, conflicts/order equivalence and checker/LSP member union tests. |
| T032 Shared class statics | REQ-039 | Typed class storage/functions, access categories, GC/reflection/editor checks and factory example. |
| T033 Open single inheritance | REQ-040 | Base fields/subtypes/traits, exact overrides, super/init, lexical/module/host GC and editor/example checks. |
| T034 Custom construction | REQ-041 | Explicit/parameterized init signatures, one-write required fields, super chaining and legacy compatibility. |
| T035 Native collection classes | REQ-042 | Real open descriptors, one-storage subclasses, alias/contracts/GC/host/reflection/editor integration. |
| T036 Typed Set | REQ-043 | Membership/mutation/ordered snapshots/algebra/subset/iteration/subclass and typed editor tests. |
| T037 Lexical class visibility | REQ-044 | Private/protected/public runtime/checker/override/trait/GC/reflection/display/editor and host tests. |
| T038 Conversion and operator hooks | REQ-045 | Rooted toString/print/REPL conversion, safe inspection and typed named binary/unary/compound hooks. |
| T039 Indexed hooks | REQ-046 | Typed getter/setter/compound single evaluation and result checks. |
| T040 F-string interpolation | REQ-047 | Expression parsing, escaped braces, evaluation order/GC and editor checks. |
| T041 Logical Object root | REQ-048 | Open root, non-null annotations/aliases and preserved value representation. |
| T042 Enum Error kinds | REQ-049 | Caught/user nominal categories, nested metadata and legacy constructor bridge. |
| T043 IO/timing foundation | REQ-050 | Host/CLI input/output/clock/delay bounds and explicit porting assessment. |
| T044 Nonblocking user input | REQ-051 | Terminal callback/session bounds, POSIX raw/poll restoration, task progress and real PTY/editor checks. |
| T045 Playable terminal Tetris | REQ-052 | Deterministic rules, raw PTY controls, pause/drop/gravity/preview/exit and explicit sustained-run budget. |
| T046 Bounded TCP capability | REQ-053 | Default denial, mock host contracts, real loopback timeout/EOF/binary/lifecycle and safe SDK resource isolation. |
| T047 HTTP and chat apps | REQ-054 | Pinned parser, strict fragmented/binary/invalid HTTP, working web/client/multi-peer chat and interactive PTY journeys. |
| T048 Unbounded CLI default | REQ-055 | Past-ceiling long-run test plus explicit-budget and invalid-option checks; embedding contracts preserved. |
| T049 Shared Pac-Man model/app | REQ-056 | Legacy maze/BFS port, rules/state/model checks and actual terminal controls/cleanup. |
| T050 Separate browser arcade | REQ-057 | Allowlisted local assets, authoritative game endpoints, process/error/asset tests and browser pixel/control/layout checks. |
| T003 Modules, errors and data modeling | REQ-009, REQ-011, REQ-012 | Runtime records/enums plus known constructor/field/payload checks and static local match coverage; export isolation, throw and cleanup tests. |
| T004 Lazy execution and lifecycle | REQ-008, REQ-011, REQ-015 | Null-yield, suspended handlers, loop transfers, explicit close, parent-owned FIFO tasks, cancellation and failure propagation tests; await/I/O waits remain pending. |
| T005 Embedding and tooling | REQ-010, REQ-013, REQ-014 | Isolated hosts, expanded data checking and .k3 editor asset validation; formatter/full inference remain pending. |
| T006 Documentation and release gates | All | Feature-status matrix, conformance suite, UBSan, CLI examples, batch report. |
| T007 CLI and interactive shell | REQ-013, REQ-016 | Core shell plus multi-file/directory checking, aggregate diagnostics, no-execution tests. History/completion and version 2 workflow parity follow. |
| T008 Source debugger | REQ-013, REQ-017 | Core source breakpoints, stepping, frames/readonly locals, task-aware identities and GC-safe copied snapshots implemented; richer inspectors, exception stops and DAP follow. |
| T009 Language server | REQ-010, REQ-013, REQ-018 | Frontend-only stdio protocol, unsaved diagnostics, UTF-16/version synchronization, outlines and optional VS Code client tests. Richer semantic capabilities follow. |

## Implementation Steps

P1.1 through P1.6 retain the existing T001 through T006 core/runtime milestones.
Their actual alpha coverage remains in the README and batch report.

### P2.1 CLI Core

- [ ] T007 [Plan:2.1] Maintain the core shell in `dotk3/src/main.cpp` and its executable contract in `dotk3/tests/cli.sh`; core functionality is shipped, remaining parity work stays open.

### P2.2 Debugger Core

- [x] T008 [Plan:2.2] Implemented per-VM execution hooks in `dotk3/src/runtime.cpp`, controller/console in `dotk3/src/debugger.cpp`, and runtime stepping/inspection contracts in `dotk3/tests/debugger.cpp`. Follow-up debugger capabilities remain open.

### P2.3 Language Server Core

- [x] T009 [Plan:2.3] Implemented `dotk3/src/lsp_main.cpp`, stdio protocol tests in `dotk3/tests/lsp.rb`, and optional client/transport tests under `dotk3/editor` and `dotk3/tests`. Diagnostics and outlines ship; completion/navigation/inference remain pending.

### P2.4 Typed Collection Contracts

- [x] T010 [Plan:2.4] Implemented typed map key/value and atomic nested collection validation in `dotk3/src/runtime.cpp`, checker literal/index/append diagnostics in `dotk3/src/checker.cpp`, and synchronized live LSP checks in `dotk3/tests/lsp.rb`.

### P2.5 Modular Core and Member Tooling

- [x] T011 [Plan:2.5] Implemented `dotk3/src/core_module.cpp` with catalogue `dotk3/src/core.hpp`, frontend resolver `dotk3/src/semantic.cpp`, runtime reflection and `:members`, and libedit-backed shell input. Conformance covers namespaces, members, imported metadata and real terminal editing/history.

### P2.6 Language API and Documentation

- [x] T012 [Plan:2.6] Capitalized the core services, added explicit import aliases, replaced struct with record, introduced List/Map constructors and shared length methods, and retained declaration docs for runtime reflection and custom-type LSP hover. Runtime/parser/checker/protocol/client/example contracts cover this increment.

### P2.7 Mutable Class Core

- [x] T013 [Plan:2.7] Implemented typed let/var state, generated field constructors, implicit self and bound methods with checker/GC/debugger/LSP metadata. Tests cover collectible method/receiver cycles, detached C callbacks, nominal module identity and source-only editor discovery. Keep records immutable; initializer hooks and inheritance/static dispatch remain outside this core increment.

### P2.8 User-Defined Trait Core

- [x] T014 [Plan:2.8] Implemented nominal trait signatures, class/record impl validation, defaults, pinned GC conformance and Collections.Sized for builtins/custom values. Checker/LSP cover signatures, readonly methods, defaults/self and imported metadata. Generic/inherited traits remain outside this core.
- [x] T014 [Plan:2.8] Added class-local implements clauses with body methods, shared validation, failed-class publication rollback and synchronized header hover/outlines. External impl remains supported; multiline/multiple/qualified names and host callback lifetimes are tested.

### P2.9 Lifecycle and Method Composition

- [x] T015 [Plan:2.9] Verified class post-field init hooks, synchronous trait defaults with explicit override/conflict policy, and immutable record methods/trait conformance in three consecutive increments. Combined C handles, GC, checker and LSP tests plus object_lifecycle.k3 validate the interactions. Inheritance/static extensions now ship in T032/T033; custom argument constructors and generic traits remain open.

### P2.10 Map Method Ergonomics

- [x] T016 [Plan:2.10] Implemented contains, remove and typed keys/values snapshots in three focused-validated increments. Shared catalogue synchronizes runtime inspection, checker signatures and LSP discovery; nominal/empty/shallow snapshot contracts and source-removal C handle lifetimes tested. Linear storage remains; no lazy views or hash-backend performance claim.

### P2.11 List Method Ergonomics

- [x] T017 [Plan:2.11] Implemented list insert/removeAt/extend in three focused-validated increments. Shared catalogue/type results drive checker/reflection/LSP; tested self-extension, generator failures/budgets, nested atomic contracts and removed nominal host callback lifetimes. Eager staging/array shifts remain explicit tradeoffs.

### P2.12 Collection Queries and Reset

- [x] T018 [Plan:2.12] Implemented six consecutive increments: List membership, first-match index, clearing, reversal, Map clearing and nullable lookup. Verified shared metadata, budget preflight, pinned contracts, snapshot/alias semantics and returned-object C handle lifetime. Linear/native work tradeoffs remain documented.

### P2.13 Anonymous Callable Tooling

- [x] T019 [Plan:2.13] Preserved existing lexical anonymous runtime behavior and fixed standalone expression routing, known immutable/direct call checking, source-bounded editor parameter scopes and signature/result discovery. CLI/REPL/runtime/parser/checker/LSP/client tests cover block/arrow/default/rest/generator/docs/GC behavior. Dynamic signature inference stays conservative.

### P2.14 String Method Ergonomics

- [x] T020 [Plan:2.14] Implemented six focused-validated String methods with exact UTF-8 semantics, code-point find, empty-preserving split and strict generator-aware join. Shared catalogue/typed results support literal/member editor discovery. Literal token contents no longer impersonate grammar operators/keywords. Unicode normalization/graphemes/regex and conversions remain follow-up.

### P2.15 Read-Only Files Capability

- [x] T021 [Plan:2.15] Implemented one vertical Files milestone across runtime callbacks, readonly service catalogue, CLI root option/policy/bounded reads, versioned C providers/revocation, checker/LSP and scripts. Existing config ABI preserved; native blocking/TOCTOU limits documented. Writes/listing/open resources and async remain follow-up.

### P2.16 C Scalar and Native Interoperability

- [x] T022 [Plan:2.16] Added strict scalar/type/binary interoperability. Public API remains locked and owner-rooted; collection traversal ships in T024/T025 and resources in T026.

### P2.17 JSON Interchange

- [x] T023 [Plan:2.17] Reused pinned nlohmann/json for public SAX conversion with exact Int64, strict duplicates, depth/byte/step limits and rooted runtime objects; bounded stringify rejects cycles/unsupported/nonfinite data. Files/host/editor pipelines verified, without source execution in LSP.

### P2.18 Owned Host Collection API

- [x] T024 [Plan:2.18] Implemented List/Tuple/Map builders/traversal and checked mutations. Verified tuple immutability, presence/null, typed/range/foreign/released checks and independent child roots. Handle publication completes only after root/storage succeeds.

### P2.19 Callback Structured Adapters

- [x] T025 [Plan:2.19] Implemented current-thread callback helpers reusing checked APIs and temporary result roots. Nested adapter/C example exchanges typed script/JSON data; public API remains BUSY and wrong-thread helpers fail before access/allocation.

### P2.20 Opaque Resource Lifecycle

- [x] T026 [Plan:2.20] Implemented versioned affine resources with copied tags, parent dependencies, idempotent explicit close, bounded forgotten open state and reverse preflight shutdown. Runtime/checker/reflection/LSP expose readonly Resource metadata. Wrong-thread, GC, released/foreign, quota and dependency behavior tested; no GC callbacks or disposal queues.

### P2.21 Contextual Native Module Registration

- [x] T027 [Plan:2.21] Implemented versioned atomic registration of readonly documented contextual exports. Imports/aliases use existing namespaces; invalid/collision/duplicate/limit failures publish no partial module. Explicit GC permits allocation-failure retry. Public C Counter example covers rooted resource returns, lifecycle access and explicit close. Dynamic loaders/manifests/drivers remain pending.

### P2.22 Callback Collection Workflows

- [x] T028 [Plan:2.22] Implemented seven List callback operations with stable budgeted snapshots, rooted managed results/accumulators, Bool predicates, short-circuit searches and explicit fold seeds. Filter contracts and known callback signatures/results flow through checker/LSP; empty/null/GC/error/mutation/host cases tested.

### P2.23 Procedural Randomness

- [x] T029 [Plan:2.23] Implemented seed/float/int/choice/shuffle using standard per-VM mt19937_64 state, half-open intervals, UTF-8 choice and preflight in-place shuffle. Same-build replay, isolation, contracts, wide ranges and editor imports/results tested. procedural_loot.k3 exercises both milestones; no implicit entropy, cryptographic or portable-stream claim.

### P2.24 Conditional Map Updates

- [x] T030 [Plan:2.24] Implemented compute/if-absent/if-present/put-if-absent/merge with documented Java-like null semantics and source-rescanning callbacks. Runtime/checker/shared result metadata/LSP/example tests validate arity, key/value/return contracts, GC and nontransactional callback effects.

### P2.25 Trait Intersection Annotations

- [x] T031 [Plan:2.25] Implemented all-trait annotations, optional whole groups, order/dedup contract identity, GC edges and incompatible signature rejection. Known static conformance/member calls and nonexecuting editor discovery combine traits without mutating declarations. Classes, unions, generic traits and full module inference remain outside the increment.

### P2.26 Shared Class Statics

- [x] T032 [Plan:2.26] Implemented static functions and initialized typed let/var class storage, category-specific access, inherited owner sharing and GC-safe detached factories. Checker/member discovery/reflection distinguish class and instance members; no implicit self/super or static trait implementation.

### P2.27 Open Single Inheritance

- [x] T033 [Plan:2.27] Implemented extends with inherited base-first fields, lexical/module defaults/contracts, derived-to-base checks, inherited traits and exact overrides. Super dispatch begins at the selected method owner and supports explicit parent init; GC, host callbacks, hierarchy limits and editor discovery tested. examples/inheritance.k3 combines both milestones.

### P2.28 Custom Construction

- [x] T034 [Plan:2.28] Implemented explicit constructor and parameterized init argument signatures, field defaults/one-write let initialization, read-before-init/incomplete failures and inherited super chaining. Generated field constructors and old init hooks remain compatible; checker/LSP/runtime tests cover the boundary.

### P2.29 Native Collection Classes

- [x] T035 [Plan:2.29] Replaced List/Map factories with real open collection descriptors and added Tuple/Set types. Raw and derived values preserve native storage, descriptor/nominal identity and pinned contracts. Fields/overrides/native super-init/qualified aliases, reflection, GC, host APIs and editor member discovery are integrated without sealed/frozen policy.

### P2.30 Typed Set

- [x] T036 [Plan:2.30] Implemented ordered deduplicating Set with typed mutations, snapshots, algebra, subset checks, iteration and subclassing. Runtime/host category/checker/LSP tests and collection_classes.k3 verify the complete core class/collection workflow. Linear storage, base-copy policy and JSON exclusion are explicit.

### P2.31 Lexical Class Visibility

- [x] T037 [Plan:2.31] Implemented explicit public/protected/private class members and constructor/static access, declaring-class lexical scopes and GC metadata. Unrelated callbacks do not gain privileges; closures/generators/defaults/impl methods retain their own scopes. Override/trait rules, filtered reflection/printing, checker/LSP and actual client tests plus encapsulation.k3 cover the milestone.

### P2.32 Conversion and Operator Hooks

- [x] T038 [Plan:2.32] Implemented public synchronous toString for script conversions with recursion/GC/result checks, keeping debugger/reflection/error formatting nonexecuting. Explicit named arithmetic/comparison/unary hooks use left dispatch and normal method contracts; compound assignments and declared editor result discovery are tested with operators.k3. Equality/reflected/index/hash/truthiness protocols remain separate work.

### P2.33 Indexed Hooks

- [x] T039 [Plan:2.33] Implemented __getitem__/__setitem__ with typed operands/results, Null setter enforcement and single receiver/key evaluation for compound assignment. Existing native indexing remains fallback.

### P2.34 F-String Interpolation

- [x] T040 [Plan:2.34] Implemented f-prefixed strings with parsed scoped replacements, escaped braces, rooted conversion/evaluation order and malformed/name/editor tests; full Python format specs remain outside this increment.

### P2.35 Logical Object Root

- [x] T041 [Plan:2.35] Implemented Object/System.Object aliases, open class root and universal non-null value contracts without primitive boxing/equality changes. Object? handles null.

### P2.36 Typed Error Categories

- [x] T042 [Plan:2.36] Implemented nested Error.Kind enum values in caught/user errors with stable nominal identity, readable category display and temporary legacy String normalization. Unknown legacy categories become Application.

### P2.37 IO and Timing Foundation

- [x] T043 [Plan:2.37] Implemented callback-gated IO.write/readLine, IO.Files alias, monotonic clock/delay and bounded CLI adapters. Host default denial, EOF/input bounds and live metadata are verified. Practical game/network apps follow in T044 through T047 without copying the old architectures.

### P2.38 Nonblocking User Input

- [x] T044 [Plan:2.38] Implemented explicit terminal sessions and bounded/nonblocking byte reads through host callbacks and CLI termios/poll. PTY tests verify keys without Enter, timeout, restored line input and REPL error recovery; mock tests show task progress during empty polls. input_loop.k3 demonstrates usable cooperative input without copying old apps or claiming task-level await.

### P2.39 Playable Terminal Tetris

- [x] T045 [Plan:2.39] Added reusable Tetris model and raw/polled colored terminal app, deterministic collision/rotation/drop/clear/spawn tests and interactive pause/movement/rotation/drop/quit PTY checks. Explicit --steps0 and per-frame GC support trusted sustained play; no SRS/hold/seven-bag claim.

### P2.40 Bounded TCP Capability

- [x] T046 [Plan:2.40] Added per-runtime socket providers, readonly metadata/catalogue/checker/LSP, opt-in POSIX numeric IPv4 CLI and a bounded independent open registry. Host/null/publication/partial-send/foreign/closed/bounds behavior and real timeout/EOF/binary exchanges are tested. Polymorphic ResourceInfo retains safe SDK resource casts and unchanged C ABI.

### P2.41 HTTP and Chat Apps

- [x] T047 [Plan:2.41] Integrated unchanged pinned picohttpparser with strict bounded single-exchange framing and request/response helpers. Working loopback webserver/client and polled multi-client chat use Bytes-safe framing, bounded messages and explicit cleanup. Real process/readiness/binary/rejection/disconnect/UTF-8 and terminal-client flows are part of canonical test-network.

## Testing Strategy

- Required journeys: Tetris bounds/rotation/locking/line clears/game-over and real controls; denied/enabled socket lifecycle, timeout versus EOF; fragmented and large binary HTTP request/response; malformed/truncated/timed-out HTTP rejection; two chat clients, UTF-8 split frames, oversize isolation, disconnect and raw editing/exit.
- Unit layer: existing C++ runtime/checker/C embedding suites plus deterministic game/line checks in the existing Ruby terminal suite. MockSocket exercises host-return, GC/publication and framing boundaries that TCP may coalesce nondeterministically.
- Integration/runtime layer: canonical `make -C dotk3 CXX=clang++ CC=clang all test example-native`, then MODE=stress. New test-network launches real CLI services, waits for explicit stdout readiness, uses ephemeral loopback ports and Ruby standard-library Socket/Net::HTTP/JSON clients plus actual DotK clients, checks responses/process exit codes and cleans up children. PTY assertions verify a nonblank colored board, controls and restored cursor/input state.
- Environment verified: macOS/Clang, Ruby2.6.10, Node16.20.2 and a reachable Docker daemon. No DB/MQ/external API is involved; native loopback is the intended infrastructure, not a mocked container fallback. Existing CLI/PTY/editor/native tests are reused; new process journeys are wired into make test rather than manual-only snippets.
- Startup success is not the sole criterion: tests verify endpoint statuses/bodies/byte counts, peer broadcasts/isolation, bounded rejection paths and clean finite shutdown. Release and UBSan are required gates; every example is checked and run in its safe default mode, with app modes exercised separately.
- Browser layer: a separate browser_server.k3 now delivers the Pac-Man canvas/HUD, while the API-only example is unchanged. Canonical network tests verify all local assets, state/actions/pause/restart and malformed-input isolation. Live Playwright checks verify player/ghost movement, pause/resume/restart, loaded font/icons, nonblank canvas pixels, proportional maze geometry and small/wide desktop/mobile layouts. Browser-tool tests are recorded separately rather than claiming an installed headless-browser test dependency.
- Gaps/limits: full visual VS Code E2E, ASan (known local pre-main startup failure), Windows sockets, internet/TLS/DNS and production load/security guarantees are not claimed. The C parser advisory review uses model fallback because the available CVE tool accepts only Maven/NuGet/npm; the new npm-distributed icon/font coordinates were assessed with that tool and had no known findings.

### P2.42 CLI Execution Policy

- [x] T048 [Plan:2.42] Changed the CLI default from million-step guard to unlimited execution, keeping explicit --steps N and host/embedding defaults. A200000-iteration collected run crosses the old ceiling; explicit100-step runaway and invalid-negative option tests preserve guard behavior.

### P2.43 Shared Pac-Man

- [x] T049 [Plan:2.43] Ported the legacy rectangular maze/cached-neighbor/BFS navigation to a DotK model shared by raw terminal and browser apps. Buffered turns, legal ghost choices, normal/power/ghost scores, cooldown/lives/respawn, stopped/pause/win/game-over/restart/next state are tested. Terminal PTY exercises arrows, movement, pause/restart and cursor cleanup without a step opt-out.

### P2.44 Separate Browser Arcade

- [x] T050 [Plan:2.44] Added a separate loopback static/JSON server and responsive canvas/HUD/icon/touch/keyboard/sound frontend using local pinned assets. Existing API server remains distinct. Authoritative DotK model endpoints/assets/error isolation are canonical process tests; actual browser controls, pixels and viewport geometry are verified. One shared local session and sequential bounded HTTP remain explicit limits.

Core class/collection integration, visibility, input and explicit operator hooks now ship. Advanced user class
generics/variance, optimized storage and full cross-module inference remain language
extensions, not claims of a finished production type system.

Schedule: Prioritize practical language/CLI features for small projects. SQLite/raylib
bindings are deferred at the user's request, not prerequisites for useful programs.
CLI polish can follow debugger command ergonomics. Do not postpone every CLI
improvement until the full bytecode backend exists, and do not label `--ast` as
a debugger. See [cli-debugger.md](cli-debugger.md) for the scoped increments.

## Requirement Mapping

| Requirement | Plan Items | Tasks | Evidence |
| --- | --- | --- | --- |
| REQ-001 | P1.1, P1.6 | T001, T006 | Makefile and independent test/build gates |
| REQ-002 | P1.2 | T002 | Runtime binding tests |
| REQ-003 | P1.2 | T002 | Runtime numeric tests |
| REQ-004 | P1.2 | T002 | Runtime condition tests |
| REQ-005 | P1.2, P2.4, P2.6, P2.8 | T002, T010, T012, T014 | Collections/contracts/constructors/length and Sized trait tests |
| REQ-006 | P1.1, P1.2, P2.6 | T001, T002, T012 | Lexer UTF-8, triple strings and runtime text tests |
| REQ-007 | P1.2 | T002 | Functions/defaults/closure tests |
| REQ-008 | P1.4 | T004 | Generator lifecycle tests |
| REQ-009 | P1.3, P2.6, P2.7, P2.8 | T003, T012, T013, T014 | Record/enum/match/class/trait core tests |
| REQ-010 | P1.5, P2.3, P2.4, P2.7, P2.8 | T005, T009, T010, T013, T014 | Checker/runtime/class/trait/collection/live diagnostics |
| REQ-011 | P1.3, P1.4 | T003, T004 | Error/defer/finally tests |
| REQ-012 | P1.3, P2.6 | T003, T012 | Module isolation and explicit alias tests |
| REQ-013 | P1.1, P1.5, P2.1, P2.2, P2.3, P2.5, P2.7 | T001, T005, T007, T008, T009, T011, T013 | Spans/check/CLI/debugger/LSP/class/member tests; rich opcode metadata pending |
| REQ-014 | P1.5 | T005 | C embedding and thread-overlap tests |
| REQ-015 | P1.4 | T004 | Cooperative task tests |
| REQ-016 | P2.1, P2.5 | T007, T011 | CLI subprocess and real terminal editing/history tests; parity pending |
| REQ-017 | P2.2, P2.7 | T008, T013 | Debugger runtime/controller/CLI and GC-safe method/self inspection |
| REQ-018 | P2.3, P2.4, P2.5, P2.6, P2.7, P2.8 | T009, T010, T011, T012, T013, T014 | LSP protocol/client/custom/class/trait docs and members; full inference pending |
| REQ-019 | P2.5, P2.6 | T011, T012 | Capitalized namespaces and explicit alias tests, member protocol and terminal tests |
| REQ-020 | P2.6 | T012 | Parser/runtime/checker/CLI/LSP/client doc, constructor and length tests plus language_api example |
| REQ-021 | P2.8 | T014 | Parser/runtime/checker/embedding/debugger/LSP/client trait tests and traits example |
| REQ-022 | P2.9 | T015 | Init/default/record parser/runtime/checker/LSP/C handle tests and object_lifecycle example |
| REQ-023 | P2.10 | T016 | Runtime/checker/LSP/C snapshot ownership tests and map_methods example |
| REQ-024 | P2.11 | T017 | Runtime/checker/LSP/C insertion/removal/extension tests and list_methods example |
| REQ-025 | P2.12 | T018 | Runtime/checker/LSP/C search/reset/get tests and collection_queries example |
| REQ-026 | P2.13 | T019 | Anonymous parser/runtime/checker/CLI/LSP/client tests and anonymous_functions example |
| REQ-027 | P2.14 | T020 | String parser/runtime/checker/LSP/C getter tests and string_methods example |
| REQ-028 | P2.15 | T021 | Runtime/C capability and CLI rooted/path/limit tests plus nonexecuting LSP/client tests |
| REQ-029 | P2.16 | T022 | C scalar/native roundtrip/type/owner/GC/concurrency tests |
| REQ-030 | P2.17 | T023 | JSON runtime/CLI/C handle/checker/LSP/client tests and json example |
| REQ-031 | P2.18 | T024 | Owned collection contract/range/owner/GC tests |
| REQ-032 | P2.19 | T025 | Callback/context/result root tests and structured_host example |
| REQ-033 | P2.20 | T026 | Resource lifecycle/dependency/quota/thread/metadata C/checker/LSP tests |
| REQ-034 | P2.21 | T027 | Atomic module import/docs/collision/limit rollback and C Counter example |
| REQ-035 | P2.22 | T028 | Callback runtime/checker/GC/host/editor tests and procedural loot example |
| REQ-036 | P2.23 | T029 | Random boundary/replay/isolation/budget/editor tests and procedural loot example |
| REQ-037 | P2.24 | T030 | Map runtime/checker/GC/editor tests and updated map_methods example |
| REQ-038 | P2.25 | T031 | Parser/runtime/checker/conflict/GC/LSP tests and updated traits example |
| REQ-039 | P2.26 | T032 | Static field/function runtime/checker/reflection/LSP and factory tests |
| REQ-040 | P2.27 | T033 | Inheritance/super/subtype/module/host/GC/editor tests and inheritance example |
| REQ-041 | P2.28 | T034 | Constructor argument/field/super/legacy runtime/checker/editor tests |
| REQ-042 | P2.29 | T035 | Collection subtype/storage/contract/alias/GC/host/editor integration tests |
| REQ-043 | P2.30 | T036 | Set mutation/algebra/iteration/type/editor tests and collection_classes example |
| REQ-044 | P2.31 | T037 | Visibility runtime/checker/parser/GC/reflection/LSP/client/host tests and encapsulation example |
| REQ-045 | P2.32 | T038 | Conversion/operator runtime/checker/debugger/CLI/LSP tests and vector example |
| REQ-046 | P2.33 | T039 | Getter/setter/compound runtime/checker tests |
| REQ-047 | P2.34 | T040 | Lexer/parser/runtime/checker/LSP interpolation tests |
| REQ-048 | P2.35 | T041 | Root/alias/primitive/null runtime/checker tests |
| REQ-049 | P2.36 | T042 | Error category runtime/generator/task/LSP tests |
| REQ-050 | P2.37 | T043 | Host/CLI IO/timing and foundations example |
| REQ-051 | P2.38 | T044 | Terminal mock/PTY/recovery/checker/LSP and input_loop example |
| REQ-052 | P2.39 | T045 | Deterministic Tetris model and real terminal controls/render/quit |
| REQ-053 | P2.40 | T046 | Mock/real socket lifecycle, type/bounds/denial/EOF and embedding regressions |
| REQ-054 | P2.41 | T047 | Real HTTP/web/chat/binary/UTF-8/disconnect and interactive process journeys |
| REQ-055 | P2.42 | T048 | CLI long-run/default and explicit budget/invalid-option subprocess checks |
| REQ-056 | P2.43 | T049 | Pac-Man model rules, legality, scoring/states and raw terminal PTY |
| REQ-057 | P2.44 | T050 | Browser asset/action/error process tests and live canvas/control/layout checks |

T012 also extends REQ-005/006/009/012/018/019; T013 ships a scoped class core,
and T014 ships a scoped nominal trait core without claiming the full trait system complete.
Tasks are sequential where frontend/runtime contracts change. Optional tools,
additional network protocols, full static inference, grapheme segmentation, and advanced
structured concurrency are explicit follow-up work, not implicit claims of a
finished language. Actual implemented status belongs in the README and report.