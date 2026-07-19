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
- `session/compaction/checkpoint/create`, `read`, and immutable `revise` (manual
  review only; activation and provider compact remain unavailable)
- `turn/steer` (Codex only; same-turn, identity-scoped)
- `session/search` (bounded local Session metadata and approved transcript fields)
- `operation/probe` (session-bound, read-only workspace/Git/runtime evidence probe)
- `operation/status` (content-free session reconciliation gate status)
- `operation/reconcile` (content-free evidence review with durable session event)
- `session/read`
- `runtime/health`
- `runtime/degradations`
- `workspace/list`
- `workspace/read`
- `workspace/instructions` (deterministic, bounded, untrusted instruction discovery)
- `workspace/pinned-context/list`, `save`, and `remove` (durable,
  metadata-only pinned-context descriptors with project/root/session validation)
- `turn/context/inspect` (read-only preflight; no model call or persistence)
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

When structured context is attached to `turn/start`, capability
`turn.context.manifest` adds a `context-manifest/0.1` object to the immediate
response. It contains only bounded source/kind, priority, trust, SHA-256,
conservative token, freshness, inclusion, and truncation metadata; attachment
text is never copied into the manifest.

The same response includes a content-free `context-budget/0.1` plan for the
current explicit context and auto-discovered instructions. Instruction
precedence ranks and pinned priority receive deterministic allocation order,
while rendered text keeps caller order. The plan enforces a 64 KiB total and
16 KiB per-item hard bound and reports requested/allocated bytes, class,
priority score, inclusion, and exclusion reason. Task-state, recent-turn,
tool-result, search, repository-map, tokenizer, and provider-window consumers
remain future budget classes.

`turn/context/inspect` runs the same bounded context preparation for a session
without starting a turn or writing history. It returns only the
`context-inspector/0.1` manifest/budget and explicit
`content_included:false`, `model_started:false`, and `persisted:false` flags; instruction and source bodies
never cross this response. The Qt Work composer exposes a search-icon preflight
action for an existing session and renders a read-only metadata table; it does
not preview source or instruction bodies.

`workspace/instructions` returns `instruction-discovery/0.1` metadata for the
registered project root and optional target path. It merges applicable sources
in weakest-first order with managed > user > closer nested > project precedence.
Managed and user roots are path-only environment configuration, not request
parameters. Content requires an explicit `include_content:true` and remains
bounded `untrusted-data`; it cannot authorize permissions, commands, Hooks, or
network. Secret-shaped/control-character content, sensitive/ignored/Git-ignored
paths, symlinks, case collisions, stale reads, and limit overflows are rejected
or reported without returning the body. Work-mode `turn/start` automatically
appends up to eight valid discovered instructions after explicit user context,
using the same bounded context manifest; secondary-root attachments do not
select the primary-root instruction chain. The explicit discovery response
remains the source for rejected/budget-excluded diagnostics, and this is still
a partial foundation rather than a complete managed-policy integration.

The internal `pinned-context/0.1` contract validates content-free descriptors for
files, selections, images, diagnostics, terminal excerpts, Git commits/diffs,
artifacts, and child handoffs. It binds project/session/root identity, source,
reference, hash, size, freshness, priority, and bounded secret-free metadata. The
descriptor contract alone does not add pins to model context; the separate store
and AAP methods provide metadata persistence only.

`pinned-context-store/0.1` adds private project-external persistence for exact
descriptor sets. Immutable content-addressed objects and atomically replaced
project pointers are hash/schema/project revalidated after reopen; tampered
current authority blocks updates. AAP advertises
`workspace.pinned-context.store` and `workspace.pinned-context.manage` when this
store opens successfully. `workspace/pinned-context/list` reads the current
project set and `workspace/pinned-context/save` validates project/root/session
bindings and supports optional `expected_set_identity` compare-and-swap
protection; `workspace/pinned-context/remove` removes one item through the same
CAS path. Removing the last item persists an explicit empty set identity.
Responses contain only descriptors and
`content_bodies_included:false`; the store never contains source bodies. It is
not atomically bound to the Workbench database: a successful AAP save publishes
the external object/pointer before appending a separate metadata-only project
event, and a failed database transaction is reported as an incomplete save. A
`pinned-context-publication/0.1` journal records the previous and next immutable
identities before pointer replacement and is removed only after the SQLite event
and Blob-release transaction succeeds. Runtime startup validates journal, pointer,
object, and latest-event identities; it replays only unambiguous missing
event/release work, cleans an already-committed journal without duplicating the
event, and disables pinned-context capabilities on tamper or ambiguity while
preserving data. For
standard `*:sha256:` Blob references, save performs a read-only SQLite metadata
check for active project/session ownership plus exact hash/byte identity; it
does not read Blob bytes or update access timestamps. It does not reread or
invalidate source references. After startup publication compensation, the store
runs `pinned-context-object-gc/0.1` with a 24-hour orphan grace period; current
pointers and pending journals are protected, object hash/schema integrity is
rechecked before deletion, and unknown, corrupt, recent, future-dated, or changed
entries are retained and reported.

Every full-set save compares the previous and next image descriptors. Removing
the final pin for one session-owned image releases its durable Blob reference in
the same SQLite transaction as the project event; duplicate pins sharing that
reference keep it active until the last removal. Transaction failure rolls back
all releases, and success extends retention by at least 24 hours. An explicit later
write with the exact same content, owner, reference, and metadata reactivates the
released reference transactionally; any identity difference remains an error. Release
events carry only a hashed batch identity and count, allowing repeated transitions to
the same empty set without persisting reference IDs or content. The external
pin pointer is still published first, so cross-resource atomicity and automatic
source invalidation remain open.

Capabilities `workspace.image.import-user` and `workspace.image.preview` add the
session/project-scoped image authority used by pinned context. Import accepts
only an explicit user-labelled Base64 payload, rejects encoded input above the
8 MiB decoded limit before allocation, and independently decodes PNG, JPEG, or
WebP under 8192-pixel-edge, 40-million-pixel, and 192 MiB allocation limits.
Only `image:sha256:` identity plus media type and dimensions enter the descriptor.
Preview returns a bounded PNG thumbnail and never the original image body.
Selected image pins reserve a conservative 16 KiB/4096-token budget entry and
are passed to pinned Codex 0.144.5 as `localImage` inputs through verified private
temporary hard links. Normal turn completion removes the links; runtime startup
removes only safely named crash leftovers and preserves unknown entries. Image
paths and bodies are not stored in turn history or returned by context inspection.
Final unpin releases the active reference through the event transaction. Cross-resource
atomicity, automatic source invalidation, and pin/Blob GC lifecycle remain open under
OpenSpec task 17.3.

`operation/probe` is a read-only evidence collector for the reconciliation
workflow. It resolves only registered project roots through the Work session,
hashes bounded workspace metadata and read-only Git status, and observes only
runtime-owned turn or terminal state. It returns hashes and state labels, never
file contents, command output, arbitrary host paths, or caller-selected PIDs.
When an event state is supplied it remains explicitly caller-labelled; when it is
omitted, a durable Runtime may derive only the latest registered `turn.*` state
from the session event stream. The probe does not infer success for other event
families, approve, mutate, or recover an operation.

When the Workbench store is available, Runtime startup preloads the latest
validated reconciliation record for each session/operation pair. The SQLite
event stream remains authoritative and per-request checks still fail closed if
the startup cache is unavailable or a record cannot be validated.

`operation/status` exposes the current content-free blocked review for a session
and explicitly reports that no recovery action is available. It is safe to call
while a session is blocked and never returns operation content or unlocks a
mutation by itself.

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
