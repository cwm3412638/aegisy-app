# Aegisy Agent Protocol Guide

Version: `AAP 0.1`  
Audience: Qt host, `aegisy-agentd`, and future Codex/ACP adapters.

This is the internal wire guide for the currently shipped read-only workbench.
The normative schemas and limits live in `agent-runtime/aap-schema`; the JSONL
fixtures under `agent-runtime/aap-schema/fixtures` are deterministic replay
evidence. AAP is JSON-RPC 2.0 over newline-delimited stdio. Each request and
response is one complete JSON object; a notification has no `id`.

## Connection

The client must complete `initialize` before sending project, session, turn,
workspace, terminal, or storage requests. The runtime returns a stable protocol
version, runtime/backend identity, negotiated capabilities, and permission
profile. The client then sends `initialized`.

```jsonl
{"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocol_version":"0.1","client":{"name":"aegisy-client","version":"fixture"}}}
{"jsonrpc":"2.0","id":"1","result":{"protocol_version":"0.1","runtime":{"name":"aegisy-agentd","version":"0.1.0"},"backend":{"adapter":"preview","status":"ready","version":"0.1.0"},"capabilities":["runtime.preview","runtime.health","runtime.degradations","permission.read-only"]}}
{"jsonrpc":"2.0","method":"initialized"}
```

An unsupported protocol version fails before a session is loaded. Clients must
not infer a missing capability from a successful initialize; they must gate the
dependent action on the explicit capability/degradation state.

## Session And Turn

Session identity is the boundary for history, environment identity, project
roots, terminals, artifacts, and provider continuation. Work sessions require a
project. The current Codex adapter uses `permission_profile: read-only`.

```jsonl
{"jsonrpc":"2.0","id":"2","method":"session/start","params":{"mode":"chat","title":"AAP example"}}
{"jsonrpc":"2.0","id":"2","result":{"session":{"id":"session-1","mode":"chat","title":"AAP example"},"runtime":{"adapter":"preview","version":"0.1.0","provider":"local","model":"deterministic-echo","permission_profile":"read-only"},"environment":{"environment_id":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","identity":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}}
{"jsonrpc":"2.0","id":"3","method":"turn/start","params":{"session_id":"session-1","input":"Inspect the project","idempotency_key":"turn-key-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.started","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"item.delta","session_id":"session-1","turn_id":"turn-1","item":{"id":"item-1","kind":"message","role":"agent","state":"delta","content":"Project summary"}}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.completed","session_id":"session-1","turn_id":"turn-1"}}
```

Every item event carries the same session and turn identity until the terminal
turn event. A turn has exactly one terminal state: `completed`, `interrupted`,
or `failed`. Reusing an idempotency key with different input is an error; retrying
the same request returns the original turn identity.

## Structured Errors And Cancellation

Error messages are diagnostics, not a stable UI contract. Clients should use the
numeric code plus bounded `runtime-error/0.1` data when present. Provider,
transport, timeout, persistence, and adapter classes never include credentials,
prompts, response bodies, or raw provider rollout text in AAP timeline data.

```jsonl
{"jsonrpc":"2.0","id":"4","method":"turn/cancel","params":{"session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","id":"4","result":{"state":"cancellation-requested"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.cancellation-acknowledged","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.interrupted","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","id":"5","error":{"code":-32110,"message":"provider request failed: [REDACTED]","data":{"schema_version":"runtime-error/0.1","class":"provider","retryable":false}}}
```

Cancellation acknowledgement means the provider accepted the interrupt; it
does not claim the turn is already terminal. Completion may win a cancellation
race. Cancellation is identity-scoped and remains available through the bounded
out-of-band control path when normal dispatch is saturated.

The stable `runtime-error/0.1` class set is `protocol`, `provider`, `adapter`,
`transport`, `timeout`, `sandbox`, `policy`, `tool`, `storage`, `workspace`,
`git`, and `budget`. `retryable` is conservative: transport, timeout, and
explicitly transient provider failures may be retryable; policy, sandbox,
workspace, Git, storage, tool, budget, protocol, and adapter failures are not
automatically retried. `persistence` remains accepted as a legacy display alias
for `storage` in older clients.

## Capability Degradation

`runtime/degradations` is a versioned, content-free explanation for features that
are unavailable, metadata-only, or blocked. The current Codex adapter reports
read-only Agent mutation, metadata-only provider thread items, and blocked
provider delete/compact. A client must fail closed when the schema is unknown or
the request fails.

```jsonl
{"jsonrpc":"2.0","id":"6","method":"runtime/degradations","params":{}}
{"jsonrpc":"2.0","id":"6","result":{"schema_version":"runtime-degradations/0.1","degradations":[{"feature":"agent-mutation","state":"disabled","scope":"runtime"},{"feature":"provider-thread-item-content","state":"metadata-only","scope":"provider"},{"feature":"provider-thread-compact","state":"blocked","scope":"provider"}]}}
```

No degradation response grants write, command, Hook, network, deletion, or
compaction authority. The Qt host displays the state and does not expose an
action that would simulate a blocked provider operation.

## Replay And Reconnect

The durable session stream is ordered by a session-local sequence. After a
transport loss, the host reads the newest page and then requests older pages with
the strict opaque cursor returned by the runtime. A cursor is exclusive, so a
newer live item cannot duplicate an already rendered item.

```jsonl
{"jsonrpc":"2.0","id":"7","method":"session/read","params":{"session_id":"session-1","limit":100}}
{"jsonrpc":"2.0","id":"7","result":{"session":{"id":"session-1","mode":"chat"},"items":[],"history_page":{"limit":100,"first_sequence":null,"last_sequence":null,"latest_sequence":0,"has_older":false,"older_cursor":null},"runtime":{"adapter":"durable-store-replay","permission_profile":"read-only","replayed":true},"environment":{"replayed":true,"available":false}}}
```

If the requested history is no longer retained, the runtime returns a bounded
snapshot and identifies the replay boundary/gap. The host must not silently
restart from sequence zero or fabricate provider continuation. A resumed Codex
binding is valid only when its pinned adapter version and opaque provider thread
identity still match; otherwise the user must fork a portable, provider-neutral
session.

## Turn Context Manifest

Capability `turn.context.manifest` adds a content-free manifest to the
`turn/start` response whenever structured context is prepared. The manifest is
`context-manifest/0.1`; each included entry carries the client item ID, kind,
source, `pinned` priority, `untrusted-data` trust, a `sha256:` content identity,
conservative token estimate, freshness, inclusion reason, and an `included` flag.
File attachments are reread and validated against the registered roots before
their hash is calculated, so a stale revision is labelled `stale`. The manifest
does not contain attachment text, and bounded/truncated context is explicitly
marked rather than silently treated as complete.

```jsonl
{"jsonrpc":"2.0","id":"20","method":"turn/start","params":{"session_id":"session-1","input":"Review this file","idempotency_key":"turn-1","context":[{"id":"context-file","kind":"file","label":"main.rs","origin":"file-tree","path":"main.rs","revision":"content:old"}]}}
{"jsonrpc":"2.0","id":"20","result":{"turn":{"id":"turn-1","state":"started"},"context":{"item_count":1,"bytes":96,"truncated":false,"manifest":{"schema_version":"context-manifest/0.1","entries":[{"id":"context-file","kind":"file","source":"file-tree","priority":"pinned","trust":"untrusted-data","content_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","token_size":12,"freshness":"stale","inclusion_reason":"user-selected","included":true}],"estimated_tokens":12,"truncated":false}}}}
```

This is metadata for later context budgeting and inspection, not an authority
grant. It does not persist source content or activate compaction.

## Operation Reconciliation

`operation/reconcile` accepts only content-free event, process, workspace, and Git
evidence. The runtime derives a content-hashed review result and appends it as an
`operation.reconciled/0.1` event in the session stream when durable storage is
available. An unknown or blocked result prevents later session-bound mutations
until a newer reconciliation for the same operation proves a stable terminal
state. Reconciliation retries with identical evidence are idempotent; different
evidence receives a new review event. The runtime never infers success, probes the
host on behalf of the caller, or executes recovery actions through this method.

Capability `operation.reconciliation.probe` exposes the separate read-only
`operation/probe` method. It resolves a registered project root through the
session, hashes bounded visible workspace metadata, reads the existing Git
status query, and observes only runtime-owned turn or terminal state. A probe
returns state labels plus optional snapshot hashes; it never returns source
content, command output, arbitrary paths, or caller-selected PIDs. A supplied
event state remains caller-labelled; when omitted, a durable Runtime may derive
only the latest registered `turn.*` state from the session event stream. The
probe result must still be sent through `operation/reconcile` for durable review
and mutation gating.

Capability `operation.reconciliation.status` exposes a content-free status read
that remains available while a session is blocked. It reports the current
review summary and `recovery_action_available:false`; it never clears the gate
or exposes a recovery mutation.

With durable storage, Runtime startup scans the latest event for each
session/operation pair, validates the event identity and evidence hash, and
preloads the bounded reconciliation cache. The event store remains authoritative
for request-time gating; a failed or over-limit startup scan must not be treated
as evidence that an operation is safe.

```jsonl
{"jsonrpc":"2.0","id":"10","method":"operation/reconcile","params":{"operation_id":"operation-1","session_id":"session-1","kind":"turn","evidence":{"event":"none","process":"not-observed","workspace":{"state":"not-required"},"git":{"state":"not-required"}}}}
{"jsonrpc":"2.0","id":"10","result":{"schema_version":"operation-reconciliation-result/0.1","reconciliation":{"state":"unknown","writes_blocked":true,"decision":"explicit-review-required"},"durable":true,"event_sequence":8}}
{"jsonrpc":"2.0","id":"11","method":"operation/probe","params":{"operation_id":"operation-1","session_id":"session-1","kind":"workspace-edit","event":"none","root_id":"root-1"}}
{"jsonrpc":"2.0","id":"11","result":{"schema_version":"operation-reconciliation-probe/0.1","evidence":{"event":"none","process":"not-observed","workspace":{"state":"not-observed"},"git":{"state":"not-required"}},"workspace_snapshot_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","event_source":"caller-supplied","process_source":"not-required"}}
{"jsonrpc":"2.0","id":"12","method":"operation/status","params":{"session_id":"session-1"}}
{"jsonrpc":"2.0","id":"12","result":{"schema_version":"operation-reconciliation-status/0.1","session_id":"session-1","blocked":true,"operation":{"state":"unknown","writes_blocked":true},"recovery_action_available":false}}
```

## Compaction Checkpoint Review

Capability `session.compaction.checkpoint-review` is advertised only when the
Workbench event store and compaction checkpoint store both opened successfully.
The current AAP surface creates and reads a manual review checkpoint; it does not
activate compacted context, delete history, or call a provider compact method.

```jsonl
{"jsonrpc":"2.0","id":"8","method":"session/compaction/checkpoint/create","params":{"session_id":"session-1","checkpoint_id":"checkpoint-1","preservation_instructions":"Preserve unresolved work","summary":{"decisions":["Keep history authoritative"],"unresolved_tasks":["Review the UI"],"changed_files":[],"commands":[],"tests":[],"failures":[],"next_actions":["Continue implementation"]}}}
{"jsonrpc":"2.0","id":"8","result":{"schema_version":"session-compaction-checkpoint-create-result/0.1","review":{"schema_version":"session-compaction/0.1","checkpoint_id":"checkpoint-1","session_id":"session-1","through_sequence":7,"source_context_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","review_id":"compaction-review:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","state":"review-required"},"descriptor":{"schema_version":"session-compaction-checkpoint-store/0.1","state":"review-persisted","original_event_history_authoritative":true},"event_sequence":8,"idempotent_replay":false,"activation_available":false,"provider_compact_invoked":false,"original_event_history_authoritative":true}}
{"jsonrpc":"2.0","id":"9","method":"session/compaction/checkpoint/read","params":{"session_id":"session-1","checkpoint_id":"checkpoint-1"}}
{"jsonrpc":"2.0","id":"10","method":"session/compaction/checkpoint/revise","params":{"session_id":"session-1","source_checkpoint_id":"checkpoint-1","source_review_id":"compaction-review:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","checkpoint_id":"checkpoint-2","preservation_instructions":"Preserve unresolved work","summary":{"decisions":["Keep history authoritative"],"unresolved_tasks":["Review the edited summary"],"changed_files":[],"commands":[],"tests":[],"failures":[],"next_actions":["Continue implementation"]}}}
```

Create derives `through_sequence` and `source_context_hash` from the complete,
verified session event stream; clients do not supply either value. It is blocked
while the session has an active turn. An identical retry returns the original
review and event, while reusing a checkpoint ID with different preservation or
summary content is a conflict. `revise` requires the exact source Review ID and a
new checkpoint ID; it never overwrites the source object, records a metadata-only
`supersedes` descriptor in the new event, and an identical retry returns the same
revision event. Read succeeds only when the content-addressed object and matching
metadata-only session event both validate after restart. Event replay rejects a
revision whose source checkpoint event is missing or whose lineage descriptor does
not match an earlier validated checkpoint.

The summary and preservation instructions are bounded and secret-shape checked,
but remain user-provided manual review data at this milestone. Qt can edit a read
review into a new immutable revision, but revisions are not added to model context
automatically. Provider `thread/compact/start`, activation, automatic thresholds,
model-generated summaries, and cross-resource failure compensation remain
unavailable.

## Security Boundary

- AAP never carries desktop login tokens, API keys, authenticated proxy values,
  raw environment values, prompts, source content, or provider response bodies
  unless a separately reviewed, bounded content reference explicitly requires it.
- The current Agent/Codex profile is read-only. User editor saves and user
  terminals are separate, explicit operations scoped to the opened project.
- The Qt UI consumes AAP state and does not parse vendor Codex events directly.
- New mutation/provider methods require a schema version, capability/degradation
  entry, redacted fixture, failure/reconnect behavior, persistence implications,
  and matching Qt/sidecar tests before they can be exposed. The current manual
  compaction review methods are an explicitly gated read-only exception: they
  have sidecar/persistence protocol coverage, advertise a capability only when
  both stores are healthy, and expose no activation or provider mutation.

## Verification

Use the checked-in fixtures as JSONL, not as prose snapshots. The Rust protocol
tests parse every line and scan for credential-shaped content. Changes to the
pinned Codex schema must update the adapter compatibility runbook and regenerate
the affected fixtures before release.
