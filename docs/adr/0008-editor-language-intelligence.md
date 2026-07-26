# ADR 0008: Editor Language Intelligence

- Status: Accepted
- Accountable owner: Editor Platform
- Consulted owners: Runtime Integrations, Product Security, Performance
- Recheck gate: Any supported-language or execution-policy expansion

## Context

The first editor milestone needed useful code navigation without allowing project
configuration, build scripts, plugins, or language servers to become hidden execution
authority. Monaco syntax plus observed build diagnostics can provide a safe baseline;
full LSP behavior varies by installed server and project trust.

## Decision

The baseline editor requires Monaco syntax/edit/save plus structured observed build
diagnostics. LSP is an optional, read-only enhancement, not a blocker for opening or
editing a project. The sidecar owns every language-server process and normalizes AAP
definition, reference, diagnostic, lifecycle, and status results; Qt never receives
native LSP messages.

Language servers are external, fixed-name or path-policy-discovered processes. Their
environment, roots, messages, paths, frames, results, and timeouts stay bounded.
`workspace/applyEdit` is always denied. Unsafe project discovery, build scripts,
dependency fetching, proc macros, project code, and arbitrary configuration remain
off unless a future sandbox/permission decision explicitly enables them.

## Consequences

Missing or failed servers render an explicit unavailable state while Monaco and
build diagnostics continue. Aegisy does not download servers automatically or claim
full semantic precision for restricted Rust/project modes. Adding a new language or
enabling trusted project configuration requires installed-version fixtures, security
review, performance bounds, and macOS/Windows evidence.
