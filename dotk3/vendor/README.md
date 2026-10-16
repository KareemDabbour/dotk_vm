# Vendored Dependencies

The LSP server uses the upstream single-header nlohmann/json library, version
3.12.0, from its GitHub release. The core Json module shares it. The upstream
header is unmodified and retains its own license.

Source: https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`

The server uses this parser for JSON-RPC data; it does not parse JSON by splitting
strings. Message/document bounds do not constitute a hostile-code sandbox.

## HTTP Parser

The core Http module links unchanged picohttpparser C/header sources from h2o at
revision `465a7ff09fbd3432fe56c673451f5460154d1f07`. Upstream source retains its
own license; no network fetch is required by builds. It parses request/status
lines and headers; DotK adds a restricted Content-Length/close framing policy.

- Source: https://github.com/h2o/picohttpparser/tree/465a7ff09fbd3432fe56c673451f5460154d1f07
- picohttpparser.h SHA-256: `1fc9074dd12418b2b91e55ef3a8279bccf8d3577b790dea6f2f908ab2a157f1c`
- picohttpparser.c SHA-256: `6480689711996bc9a219e26df27ac32ebbb7dce2f24a63a316d041b08f646199`

Native C vendoring is outside the available CVE tool's Maven/NuGet/npm schema.
The [review](../design/network-cve/cve-fix-summary.md) is an explicitly best-effort
model-knowledge fallback with no known findings, not a live security certification.

## Browser Arcade Assets

The separate browser server serves unchanged local assets, downloaded once from
the pinned npm distributions. Upstream licenses accompany them in examples/browser_ui.

- `lucide@1.55.0`: https://cdn.jsdelivr.net/npm/lucide@1.55.0/dist/umd/lucide.min.js
- Lucide SHA-256: `48b3b5d82216bbe1d0c49dea480ed38fca8420c275c98f20505e3f9556f6b1d1`
- `@fontsource-variable/oxanium@5.3.0`: https://cdn.jsdelivr.net/npm/@fontsource-variable/oxanium@5.3.0/files/oxanium-latin-wght-normal.woff2
- Font SHA-256: `3aa555c528430aab19bb2693dc206bab557767d3082dba66400784c9cc90cfbb`

The available GitHub advisory tool reported no known findings for both coordinates;
the artifact is [cve-report-browser-assets.json](../design/cve-report-browser-assets.json).
That is dependency assessment, not a production security certification.