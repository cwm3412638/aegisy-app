# ADR 0003: ACP Extension Policy

- Status: Accepted
- Accountable owner: Protocol Working Group
- Consulted owners: Runtime Integrations, Desktop Platform, Product Security
- Due gates: Recheck at `8.1`; each extension requires review under `8.4`

## Context

ACP may not expose every Aegisy concept for structured diffs, Git graph, model
capabilities, child sessions, or background jobs. Vendor-specific assumptions would
make one implementation the accidental protocol and weaken truthful capability UI.

## Decision

This decision is an extension guardrail, not approval for any current extension.

Use standard, version-pinned ACP methods first. Translate ACP into stable AAP inside
the sidecar; Qt never consumes raw ACP. A missing ACP feature remains unavailable and
is represented through capability/degradation metadata.

Create an Aegisy ACP extension only when:

1. Two independent agent/runtime implementations demonstrate the same product need.
2. No standard ACP method or composable AAP projection can represent it truthfully.
3. The extension is namespaced, versioned, bounded, redaction-reviewed, optional,
   capability negotiated, and covered by positive and negative fixtures.
4. Standard-only peers continue to work without fabricated fallbacks.
5. Upstream proposal suitability is recorded before a permanent Aegisy dialect is
   accepted.

## Consequences

Kimi or another first ACP fixture cannot define Aegisy's de facto wire semantics.
No extension can carry credentials, grant permission, infer approval, or enable child
or background execution without the corresponding AAP security and release gates.
Every accepted extension requires its own compatibility and rollback entry.
