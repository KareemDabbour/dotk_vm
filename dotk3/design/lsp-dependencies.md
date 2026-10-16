# LSP Dependency Self-Check

The JSON library is pinned to nlohmann/json 3.12.0 with upstream URL and SHA-256
in the vendor README. The runtime JSON library now shares this same unchanged
header/version/hash with the LSP; no new dependency was downloaded. The shared
advisory tool does not accept the C++ ecosystem;
the header review therefore used best-effort model knowledge. No known findings
were identified in that fallback review. This is not a current-database security
certification and may miss recent advisories.

Resolved editor dependencies are pinned by package-lock.json. npm audit and the
shared npm GitHub-advisory assessment returned no known findings on 2026-10-11.
The report is cve-report-lsp-npm.json. This is an implementation self-check, not
an independent security gate or a claim about protocol/parser hardening.

Installation used the public npm registry and an empty user configuration because
the host's existing npm authentication was invalid. Host credentials/settings were
not read, changed or replaced. Dependency install scripts were disabled.