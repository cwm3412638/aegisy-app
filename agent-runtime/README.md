# Aegisy Agent Runtime

This workspace contains the executable Aegisy Agent Protocol (AAP) runtime and
the first Codex App Server adapter. The default runtime launches the installed
Codex CLI and maps its thread, turn, and agent-message events into stable AAP
events.

The adapter is pinned to Codex CLI/App Server `0.144.5`. The generated v2
protocol schema used by the adapter is checked in at
`aap-schema/codex-app-server-0.144.5/v2.schemas.json`; a different installed
version is rejected before the app-server process is launched.

The version compatibility matrix, upgrade gates, emergency pin, and rollback
procedure are documented in [`docs/CODEX-ADAPTER-UPGRADE.md`](../docs/CODEX-ADAPTER-UPGRADE.md).

## Run

```sh
cargo run -p aegisy-agentd
```

Messages are JSON-RPC 2.0 objects separated by newlines on stdin/stdout. The
`initialize` request and `initialized` notification must complete before project
or session methods are accepted.

## Supported preview methods

- `initialize`
- `initialized`
- `project/open`
- `project/trust-review`
- `project/trust-acknowledge` (user review record only; grants no execution authority)
- `session/start`
- `turn/start`
- `turn/steer` (Codex only; same-turn, identity-scoped)
- `session/search` (bounded local Session metadata and approved transcript fields)
- `session/read`
- `runtime/health`
- `runtime/degradations`
- `workspace/list`
- `workspace/read`
- `shutdown`

Both Chat and Work currently force Codex into `read-only` sandbox mode with an
approval policy of `never`. Chat uses an isolated empty temporary workspace;
Work binds the selected project root so the Agent can inspect project context.
Neither mode can approve file writes or mutating commands in this milestone.

Codex provider state is exposed only through bounded read-only projections:
`session/provider-list` lists thread metadata with an optional project root and
cursor, and `session/provider-read` reads one thread's metadata plus bounded turn
metadata when requested. Provider item contents, raw rollout fields, delete, and
compact operations are intentionally omitted until their AAP review and recovery
contracts are complete.

Clients can query `runtime/degradations` to render disabled or metadata-only
features explicitly. This is authoritative capability state, not a request to
silently fall back to a mutating or provider-opaque implementation.

Codex `runtime/health` includes only process state, PID/exit metadata, restart
recommendation, and a content-free stderr summary: observed bytes, newline count,
redacted-line count, and the last stable class (`fatal`, `error`, `warning`,
`timeout`, or `info`). Stderr text is drained and discarded after classification;
credentials, prompts, paths, and raw diagnostics never cross AAP.

Codex startup waits at most 15 seconds for the initialize response. Only transient
transport, EOF, read/write, or timeout failures are retried, with a maximum of
three bounded attempts and a short fixed backoff. Version mismatches and protocol
rejections fail immediately. `runtime/restart` replaces an exited or unavailable
adapter while preserving session bindings; it refuses to run during an active turn
or user terminal. A later exit is therefore recoverable through an explicit,
reviewable runtime action rather than an implicit process loop. Qt polls this
health state and exposes the guarded restart action when recovery is required.

Project trust review returns instruction and Hook paths plus bounded content
identities, never their bodies. `project/trust-acknowledge` re-scans the registered
root and persists the exact reviewed hash in the project event stream. A root,
instruction, or Hook change invalidates the acknowledgement. This is only evidence
that the user reviewed the snapshot; Agent writes, commands, Hooks, and network
remain unavailable until their separate policy, approval, sandbox, and recovery
gates are complete.

Structured Codex command Items include the value-free parent environment identity
and a hashed `codex-child-environment/0.1` launch contract. The contract binds
`env_clear`, allowlisted platform inheritance, credential/proxy redaction, loader
injection denial, and execution-control denial. Codex command metadata does not
report mutations made by an individual child process, so the runtime exposes that
limitation explicitly instead of claiming child-level observation.

Read-only Codex turns also translate schema-defined token usage, plan, and
unified-diff notifications into bounded AAP timeline events. These projections
do not authorize writes. With the Workbench data root configured, each update is
also appended as a unique durable timeline Item (at most 32 updates per kind per
turn); after the cap, one content-free truncation marker is emitted. Standalone
runtimes without durable storage retain the bounded in-memory projection.

While a Codex turn is active, `turn/steer` accepts a bounded follow-up input
only when its `session_id` and `turn_id` exactly match the active turn. The
request crosses the same out-of-band control path as cancellation, so it stays
reachable when the normal AAP request queue is saturated. Aegisy sends the
schema-defined `turn/steer` request with `expectedTurnId`; the immediate AAP
response means the input was queued, while `turn.steering-acknowledged` or
`turn.steering-failed` is authoritative for the provider result. Steering is
still read-only and cannot grant file, command, or permission mutation.

The redacted request sequence in
`aap-schema/fixtures/codex-thread-lifecycle.jsonl` is the checked-in lifecycle
fixture used by protocol tests. It contains no provider credentials, rollout
content, or real user identifiers.

`aap-schema/fixtures/codex-turn-metadata.jsonl` provides schema-aligned
token-usage, plan, and diff notification inputs for adapter replay tests.

`aap-schema/fixtures/codex-recovery.jsonl` records bounded partial output,
transport failure, restart health, provider metadata, and compaction request
and approval-denial shapes. Real stdio fixtures verify that an EOF is classified
as retryable transport failure, the same session binding can complete after
restart, provider failures remain non-retryable, and approval requests are
always declined without granting execution authority.

Workspace browsing uses project-relative paths only. The sidecar rejects path
traversal, absolute paths, symlinks, sensitive credential filenames, binary
content, and text files larger than the negotiated preview limit. Generated and
runtime-owned directories are omitted from directory listings.

Set `AEGISY_AGENT_BACKEND=preview` to run the deterministic echo backend used by
protocol and UI rendering tests.

## Portable sessions

The runtime exposes preview/commit pairs for redacted session export and import:
`session/export/preview`, `session/export`, `session/import/preview`, and
`session/import`. The versioned package contract, limits, exclusions, collision
strategies, and continuation boundary are documented in
[`docs/PORTABLE-SESSION-FORMAT.md`](../docs/PORTABLE-SESSION-FORMAT.md).
