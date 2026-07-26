# AAP And Adapter Contributor Guide

Status: internal contributor policy
Current stable wire version: `AAP 0.1`
Current Codex adapter pin: `codex-cli 0.144.5`

This guide describes how to change the Aegisy Agent Protocol (AAP), the Rust
`aegisy-agentd` runtime, the Qt client, and the Codex adapter without silently
changing authority or compatibility. The wire schemas and deterministic
fixtures are the source of truth. `docs/AAP-PROTOCOL-GUIDE.md` explains the
current wire behavior; `docs/CODEX-ADAPTER-UPGRADE.md` is the release procedure
for changing the pinned provider adapter.

## Repository Map

- `agent-runtime/aap-schema/stable/v0.1/aap.schema.json`: checked-in stable JSON
  Schema and method/capability vocabulary.
- `agent-runtime/aap-schema/fixtures/`: redacted JSONL protocol evidence and
  generated Codex schema material. Fixtures must remain deterministic.
- `agent-runtime/crates/aegisy-aap/src/lib.rs`: Rust wire types and strict
  cross-language identity/validation helpers.
- `agent-runtime/crates/aegisy-agentd/src/`: Runtime dispatch, Store producers,
  adapter boundary, and recovery logic. The adapter is not a second authority.
- `include/agent_runtime_client.h`, `src/agent_runtime_client.cpp`: Qt AAP
  transport, request correlation, capability gating, and response validation.
- `include/agent_workbench_widget.h`, `src/agent_workbench_widget.cpp`: Qt
  projection and user-visible state. It cannot invent Runtime capabilities.
- `tests/agent_runtime_environment_test.cpp` and
  `tests/agent_workbench_render_test.cpp`: deterministic Qt contract/render
  coverage.

## Schema Rules

1. Stable changes are additive only within an existing version. A required-field,
   identity, enum, error-shape, authority, or ordering change requires a new
   schema version and a migration/replay decision.
2. Experimental fields never appear in the stable declaration and never become
   an implicit fallback. A capability must be declared by the client, negotiated
   by Runtime, mapped to a method, and checked again at dispatch.
3. Every request/response/notification has an object `params` shape with strict
   unknown-field rejection. IDs are bounded, non-null graphical strings for
   successful responses. Keep the 4 MiB frame limit exact in both directions.
4. Content-free metadata must remain separate from authority. A status, health
   result, degradation report, model profile, Timeline anchor, or acknowledgement
   never grants permission to write, execute, approve, route, or dispatch.
5. Durable identities use the domain-separated canonical byte rules already
   documented in the protocol guide. Never hash a transport wrapper, raw secret,
   or an unbounded provider body.

When adding a method, update all of the following in one change:

- stable Schema and Rust types;
- Runtime capability registry, required-capability map, handler, and recovery
  mode behavior;
- Qt request/response correlation and strict validator;
- protocol and Qt fixtures/tests;
- `docs/AAP-PROTOCOL-GUIDE.md` and the OpenSpec design/spec/verification files;
- `PROJECT-MEMORY.md` when the architecture, security boundary, or release gate
  changes.

## Runtime And Qt Boundary

The Rust Runtime owns provider processes, SQLite transactions, Timeline ordering,
durable identity, and fail-closed recovery. Qt owns presentation, user gesture
state, local editor saves inside the canonical project root, and request
correlation. Qt must not infer a missing response, reuse a prior generation, or
turn an unavailable capability into a simulated control.

The current Codex adapter is read-only. Runtime policy denials, Provider
`declined` Tool state, and genuine user Approval are different facts. Do not add
an Approval, file-write, Git, background-job, or unattended-write producer until
its permission, sandbox, approval, durable consumption, recovery, and release
gates exist. Durable Turn-start acknowledgement records only metadata and
Timeline anchors; it does not make the Agent writable.

For a new Runtime method, verify the following lifecycle:

1. Initialize advertises the capability only when the backend and Store are
   healthy enough to provide it.
2. The exact `initialized` notification is consumed before business dispatch.
3. The handler validates session/project ownership, recovery/deletion state,
   generation, cursor/anchor, and request identity before any Store write.
4. Every durable projection, internal event, and public Timeline event that form
   one fact commit in one transaction, or none commit.
5. Qt publishes a result only after complete private validation. On drift or
   ambiguous ownership, freeze the affected Session and preserve confirmed state.

## Fixture Workflow

Fixtures are evidence, not examples that may drift. Keep each JSONL line one
complete frame and use stable bounded IDs. Remove prompts, source bodies, paths,
provider responses, credentials, authorization headers, user identifiers, and
opaque vendor content unless the contract explicitly tests their rejection.
Use obvious redacted sentinels only when a test proves they never reach a
response, Timeline Item, Trace, log, or Store.

For a new or changed fixture:

1. Add the smallest redacted lifecycle that proves the contract, including the
   failure/reconnect path and the terminal state.
2. Add schema parsing and Rust protocol assertions. If Qt consumes the shape,
   add response-generation, stale-generation, malformed-response, and render
   coverage.
3. Assert deterministic identities, bounded pages, authority flags, and absence
   of forbidden content. Do not assert only process exit status.
4. Run the focused test first, then the full local gates:

   ```sh
   cargo fmt --all --manifest-path agent-runtime/Cargo.toml -- --check
   cargo test --workspace --manifest-path agent-runtime/Cargo.toml
   cargo clippy --workspace --all-targets \
     --manifest-path agent-runtime/Cargo.toml -- -D warnings
   jq empty agent-runtime/aap-schema/stable/v0.1/aap.schema.json
   openspec validate build-aegisy-agent-workbench --strict
   git diff --check
   cmake --build build -j4
   ctest --test-dir build --output-on-failure
   ```

5. Record the exact command and result in `verification.md` and summarize the
   durable conclusion in `PROJECT-MEMORY.md`. A skipped or ignored live fixture
   remains an explicit evidence gap.

## Versioning And Adapter Changes

AAP versioning and vendor versioning are independent. A provider upgrade must
not silently change the AAP projection. For every Codex pin change:

- record the exact CLI/App Server version and binary/schema hashes;
- regenerate the checked-in vendor schema from that binary and review the diff;
- update the compatibility/degradation matrix and redacted lifecycle fixtures;
- preserve the read-only permission and environment redaction contract;
- run the macOS and Windows pinned-binary suites before promotion;
- retain the prior signed sidecar, adapter, schema, and hash set as one rollback
  slot.

Only transient startup transport/EOF/read/write/timeout failures may retry. Version
mismatch, protocol rejection, malformed provider data, and uncertain mutation
ownership must fail closed. A rollback restores the complete prior artifact set;
it is not a database downgrade and must not rewrite session history. See
`docs/CODEX-ADAPTER-UPGRADE.md` for the full procedure.

## Release Evidence

The local macOS gate proves schema, Rust, Qt, and deterministic fixture behavior.
It does not prove Windows behavior. A release candidate must additionally attach
redacted Windows runner evidence for:

- named-pipe/ACL or packaging transport facts and reconnect generation handling;
- TLS initialization and signed installer install/upgrade/rollback;
- ConPTY Unicode, resize, Ctrl+C, exit status, and process-tree cleanup;
- long paths, Git, high-DPI, IME, accessibility, antivirus, and renderer restart.

Do not check an OpenSpec task or update memory to claim these facts from a macOS
build. Keep the task open until the required runner artifact and command output
are available.

## Review Checklist

- [ ] Schema version, capability, method map, and strict validators agree.
- [ ] Rust and Qt reject unknown fields, stale generations, cross-session data,
      forged identities, and oversized frames.
- [ ] Store transaction and public Timeline ordering are atomic.
- [ ] Recovery, retry, timeout, disconnect, and ambiguous ownership preserve
      confirmed state and do not redispatch.
- [ ] Fixtures contain no secrets or unbounded provider content.
- [ ] Authority fields remain explicitly false unless a reviewed gate grants it.
- [ ] Focused and complete local gates pass; external platform gates are attached
      or remain clearly open.
- [ ] `PROJECT-MEMORY.md`, OpenSpec verification, and upgrade documentation are
      synchronized with the actual evidence.
