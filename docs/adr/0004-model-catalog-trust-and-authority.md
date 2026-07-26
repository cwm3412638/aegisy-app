# ADR 0004: Model Catalog Trust and Field Authority

- Status: Provisional
- Accountable owner: Model Control Plane
- Consulted owners: Product Security, Runtime Integrations, Billing, Desktop Platform
- Due gates: OpenSpec `9.1` through `9.4`, `9.7`, `9.10`, and `10.1`; before model
  selection, routing, or token issue

## Context

Internal contracts already define strict model catalog, Ed25519 key-ring rotation,
root-anchored trust storage, freshness states, compatibility metadata, and field
authority. No production root, authenticated publication endpoint, desktop refresh,
or admin publisher exists. The current desktop projection is intentionally offline
and cannot select a model.

## Provisional Decision

Use signed, canonical JSON bundles with an externally anchored Ed25519 root. Rotate
catalog signing keys through a monotonic signed key ring that preserves history and
revocation. The desktop host owns authenticated HTTPS refresh; credentials never
enter the sidecar. Cache states are exactly fresh, stale, expired, invalid, offline,
or empty, and only fresh, signature-validated data may participate in selection.

Every value carries one authority class: upstream-authoritative, Aegisy-configured,
evaluation-derived, estimated, observed, stale, or unknown. Unknown/estimated values
cannot appear exact, and absence never means capability support.

## Required Production Evidence

- Production trust root custody, rotation/revocation ceremony, publisher access
  control, rollback protection, audit record, and emergency recovery.
- Authenticated conditional GET endpoint with bounded identity encoding and no
  redirects, credential forwarding, or response-body logging.
- Admin validation for alias, context, protocol, price, capability, entitlement, and
  compatibility contradictions.
- Fresh/stale/expired/offline/invalid desktop fixtures plus macOS/Windows restart and
  clock-regression evidence.

## Consequences

The current offline projection remains non-selecting. Local configuration, UI text,
or a provider response cannot mark a catalog signature validated. Catalog trust does
not issue a model token or authorize a Turn; those are separate scoped gates.
