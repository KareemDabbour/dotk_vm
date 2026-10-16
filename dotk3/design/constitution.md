# DotK 3 Principles

- This tree is independent of DotK 2. Do not alter parent sources or builds.
- Prefer a small explicit language over compatibility aliases and implicit behavior.
- Every shipped semantic rule must have an executable conformance test.
- Preserve fn, lexical closures, GC, modules, generators, and host embedding as concepts, not their old implementation details.
- Diagnostics own source text and report file, line, column, and an excerpt.
- Checked integers, strict booleans, and explicit text/bytes are foundational.
- No feature is advertised as implemented until its acceptance tests pass.
- No claims of hostile-code sandboxing or general performance wins.
- CLI policy and host capabilities remain separate.