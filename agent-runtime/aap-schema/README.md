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

## Generated Core Types

`stable/v0.1/core.schema.json` is also the single input for the partial OpenSpec
`3.10` generation slice. The deterministic generator writes checked-in outputs
for all three current consumers:

- Rust: `../crates/aegisy-aap/src/generated_core.rs`;
- TypeScript declarations/runtime: `generated/typescript/core_types.d.ts` and
  `generated/typescript/core_types.mjs`;
- Qt/C++: `generated/cpp/aap_core_types_generated.h` and
  `generated/cpp/aap_core_types_generated.cpp`.

Generated decoders reject unknown fields and expose definition-level validation;
they are not permissive DTOs. The generator audits the complete Schema AST and
fails on an unknown keyword, unsupported dialect or semantic combination instead
of silently emitting a weaker type. It also rejects non-injective generated type,
field, enum-variant, or union-variant names and rejects property-bearing open
objects that the generated DTOs cannot represent faithfully. JSON-safe integer
and Unicode-scalar string handling is shared across the three runtimes. Recursive
Item `data` additionally fails closed beyond 16 levels or 4,096 aggregate values,
matching the typed Runtime/Qt boundary that JSON Schema alone cannot express.

`fixtures/aap-core-domains.fixture-map.json` maps the complete positive fixture
catalog to definitions and binds its canonical JSON bytes. The generated Rust,
TypeScript, and Qt/C++ codecs must reproduce the same byte count and SHA-256.
`fixtures/aap-core-generated-corpus.json` is a separate shared 43-case accept/
reject corpus. Materialization stores each candidate as raw `value_json`; Rust,
TypeScript, and Qt/C++ parse those same bytes independently, and parse failure is a
rejection. The corpus includes lone-surrogate string and object-key boundaries.
All three validators must make the same decision for every case and reproduce its
reviewed decision-list SHA-256; a matching positive fixture hash alone is
insufficient.

Run the generator and focused cross-language gate from the repository root:

```sh
node agent-runtime/aap-schema/scripts/generate-core-types.mjs
node agent-runtime/aap-schema/scripts/test-core-generator-inputs.mjs
node agent-runtime/aap-schema/scripts/generate-core-types.mjs --check
cmake --build build --target AegisyAapGeneratedTypesTest -j4
ctest --test-dir build -R '^aap_generated_types$' --output-on-failure
cargo package --manifest-path agent-runtime/Cargo.toml \
  -p aegisy-aap --allow-dirty
```

The CTest gate materializes the shared corpus in the build directory and compares
the independent Rust, TypeScript, and Qt/C++ results. The runner consumes paths
through platform-native argument APIs so a Windows Unicode checkout is part of the
required clean-runner evidence. `.gitattributes` fixes Schema, fixture, generator,
generated-output, and gate source files to LF so a Windows checkout cannot change
the reviewed bytes. `BUILD_TESTING=ON` requires both Node.js and Cargo; a production
configuration with testing disabled keeps its existing dependency behavior.

This slice generates only `core.schema.json` domain definitions. Transport
request/response/notification generation from `aap.schema.json`, migration of all
hand-written Rust/Qt consumers, and clean Windows execution remain required before
OpenSpec `3.10` can be checked.

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
