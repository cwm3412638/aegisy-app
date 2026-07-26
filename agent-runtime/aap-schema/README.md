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

## Promotion

An experimental contract can move to `stable/` only through a reviewed OpenSpec
change that defines compatibility, authority, recovery, fixtures, generated
types, Runtime enforcement, and Qt fail-closed behavior. Promotion copies a
reviewed contract into a new stable wire version; it does not reinterpret an
existing experimental URI as stable.

Run the package gate with:

```sh
cargo test --manifest-path ../Cargo.toml -p aegisy-agentd --test schema_package
```
