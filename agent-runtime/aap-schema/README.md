# Aegisy Agent Protocol Schema Package

This private package is the repository source of truth for versioned AAP wire
schemas and deterministic protocol fixtures. Its package version is independent
from both the AAP wire version and provider-adapter versions.

## Namespaces

- `stable/` contains wire contracts that are additive-only within one version.
  Removing a field, adding a required field, changing authority, or changing an
  identity/order rule requires a new wire version and an explicit migration and
  replay decision.
- `experimental/` is a separate, compatibility-free namespace. It is currently
  empty and unavailable on the wire. The directory and registry reserve package
  structure only; they do not advertise or negotiate an experimental capability.
- `fixtures/` contains bounded, redacted protocol evidence. Fixtures are not a
  second schema source and must not contain credentials, user identifiers,
  prompts, source bodies, or provider response bodies.

Each namespace owns a `namespace.json` registry. Registry paths are relative to
their namespace directory, must resolve to ordinary files inside this package,
and must agree with each JSON Schema `$id`. Stable schemas must never reference
the experimental namespace.

## Stable Components

AAP `0.1` registers two additive components:

- `stable/v0.1/aap.schema.json` owns JSON-RPC transport envelopes, handshake,
  Timeline events, and the stable method/capability vocabulary.
- `stable/v0.1/core.schema.json` is a reusable `$defs` library for Project,
  Session, Turn, Item, Runtime projections, Workspace, the non-authorizing
  Approval acknowledgement, Error, Usage, command-output Artifact reads, and
  negotiated Capability sets.

The core file is not one aggregate wire message. Method-specific responses use
individual definitions or strict projections of them. The
`fixtures/aap-core-domains.json` document is a named fixture catalog and is
validated definition by definition. Runtime capture tests additionally validate
real `project/open`, `project/root-list`, `project/list`, `session/start/list/search/read`,
`turn/start`, and Timeline Item output so a self-consistent fixture cannot hide
producer drift.

The package gate treats the current public core enum values and security-relevant
bounds as the stable `0.1` compatibility baseline. Future enum values may be added,
but removing a baseline value or drifting a shared Runtime, Backend, live Item, or
history Item boundary fails before release.

Artifact and Capability definitions describe the current domain-specific
command-output read and handshake capability-set shapes; they do not create a
generic artifact method or a single-capability wire object. Approval supports
only the existing metadata-only acknowledgement with all user-decision,
approval, mutation, and execution authority fixed false. AAP `0.1`
`experimental` must remain empty.

JSON Schema `maxLength` counts Unicode characters. Normative UTF-8 byte limits,
aggregate tree limits, cross-field identities, timestamps, token totals, and
Workspace Git invariants remain independently enforced by typed Rust and Qt
validators and their boundary tests.

## Promotion

An experimental contract can move to `stable/` only through a reviewed OpenSpec
change that defines compatibility, authority, recovery, fixtures, generated
types, Runtime enforcement, and Qt fail-closed behavior. Promotion copies a
reviewed contract into a new stable wire version; it does not reinterpret an
existing experimental URI as stable.

Run the package gate with:

```sh
jq empty stable/namespace.json stable/v0.1/aap.schema.json \
  stable/v0.1/core.schema.json fixtures/aap-core-domains.json
cargo test --manifest-path ../Cargo.toml -p aegisy-agentd --test schema_package
cargo test --manifest-path ../Cargo.toml -p aegisy-agentd --test core_schema_runtime
```
