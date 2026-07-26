# ADR 0005: Local Model Provider Policy

- Status: Proposed
- Accountable owner: Product
- Consulted owners: Model Runtime, Product Security, Privacy, Support
- Due gate: An accepted dedicated local-provider OpenSpec before implementation;
  OpenSpec `10.11` is only a supplemental provider-contract test gate

## Context

Local endpoints can improve privacy and offline availability, but they may expose
unverified model identities, tools, context limits, retention behavior, or network
reachability. Routing them through Aegisy cloud policy would imply control and
telemetry that may not exist; treating them as ordinary signed catalog models would
fabricate authority.

## Proposed Decision

Offer local models, if approved, as an explicitly unmanaged offline provider profile.
The profile must be opt-in, localhost-only by default, visibly unverified, excluded
from Aegisy entitlement/billing claims, and unable to inherit cloud credentials.

Initial local profiles are Chat/read-only only. Work tools, images, structured
patches, network access, background jobs, and model switching stay unavailable until
the exact local runtime proves each capability and passes the normal permission,
sandbox, approval, recovery, and release gates.

## Required Evidence

- Endpoint-origin policy, loopback/Unix-socket binding, redirect denial, DNS rebinding
  defense, authentication handling, request bounds, and content-free diagnostics.
- Runtime/model identity and version observation without trusting caller labels.
- Capability, context-limit, tokenizer, tool-call, streaming, cancellation, and
  restart fixtures for every supported local runtime version.
- Clear UI and export semantics distinguishing unmanaged local data from Aegisy cloud
  data and from signed catalog recommendations.

## Consequences

No automatic fallback from Aegisy cloud to a local model is allowed. Local endpoints
cannot receive Aegisy login/provider credentials and cannot claim zero-data-retention
or exact capability values without evidence. Remote LAN endpoints require a separate
network/security decision and are not covered by this ADR.
