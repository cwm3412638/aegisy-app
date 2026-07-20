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
read-only Agent mutation, metadata-only provider thread items, blocked provider
delete/compact, and the release gate for background/multi-agent autonomy. The
autonomy entries use `availability: not-advertised`, `stable_enabled: false`,
`override_available: false`, and bounded `missing_gates`; there is no hidden
experimental switch. A client must fail closed when the schema is unknown or the
request fails.

```jsonl
{"jsonrpc":"2.0","id":"6","method":"runtime/degradations","params":{}}
{"jsonrpc":"2.0","id":"6","result":{"schema_version":"runtime-degradations/0.1","degradations":[{"feature":"agent-mutation","state":"disabled","scope":"runtime"},{"feature":"provider-thread-item-content","state":"metadata-only","scope":"provider"},{"feature":"provider-thread-compact","state":"blocked","scope":"provider"},{"feature":"background-jobs","state":"disabled","availability":"not-advertised","stable_enabled":false,"override_available":false,"scope":"runtime","missing_gates":["21.2","21.6","21.8","21.9","20.9"]},{"feature":"multi-agent","state":"disabled","availability":"not-advertised","stable_enabled":false,"override_available":false,"scope":"runtime","missing_gates":["18.3","21.3","21.4","21.5","21.6","21.10"]},{"feature":"unattended-writes","state":"disabled","availability":"not-advertised","stable_enabled":false,"override_available":false,"scope":"runtime","missing_gates":["15.3","16.7","18.3","18.4","18.5","18.6"]}]}}
```

No degradation response grants write, command, Hook, network, deletion, or
compaction authority. The Qt host displays the state and does not expose an
action that would simulate a blocked provider operation.

## Structured Plan Boundary

The sidecar contains an internal `structured-plan/0.1` data contract for the
future plan/child-task flow. It validates bounded stable step IDs, statuses,
owners, dependency references and cycles, content-free SHA-256 evidence, and
completion evidence. A reviewed plan or evidence revision marks affected steps
`stale` without silently resetting their status. The contract is not currently
exposed through AAP, persisted as a job, or connected to an executor; clients
must not infer plan authority or write permission from Codex `turn/plan/updated`
notifications.

The related internal `child-task/0.1` request contract binds a parent session and
turn to a bounded goal, content-addressed context identities, project/root and
`read_only` or `dedicated_worktree` isolation, tool list, model profile,
permission/network policy, token/cost/time/turn/tool/concurrency budgets, and an
expected `child-handoff/0.1` result shape. Validation does not create a child,
grant permissions, allocate a worktree, or start a provider. Parent/child
lineage, approval, scheduling, and execution remain unavailable.

The internal `child-task-state/0.1` state machine keeps parent/child identities,
generation, transactional status updates, cancellation request/rejection/
acknowledgement, and a bounded handoff descriptor. Cancellation is not
completion: a child may finish first and the state records that race explicitly.
Generation exhaustion and invalid updates fail without leaving partial state.
Handoffs contain only content references, source revision, counts, and truncation
metadata. This state is not persisted or exposed through AAP yet.

The internal `child-worktree-admission/0.1` gate connects a validated child-task
request and runnable child lifecycle to the existing live dedicated-worktree
descriptor and health verifier. A write request is rejected before admission if
it uses shared isolation, lacks exact parent/child ownership and base revision,
reuses another child's worktree, or finds missing, unlocked, prunable, dirty,
conflicted, or otherwise unhealthy state. Its non-serializable workspace proof
and content-free receipt set `permission_granted:false` and
`execution_available:false`; they cannot replace permission, approval, sandbox,
budget, recovery, or per-tool path checks. No AAP method exposes this gate.

The internal `child-runtime-budget/0.1` ledger is the first runtime budget
enforcement primitive. Model work reserves non-zero token and cost ceilings before
admission, then settles authoritative or estimated usage. If provider usage is
unknown, the ledger charges the full reservation rather than zero. Wall-clock,
turn, tool-call, active-concurrency, and policy-bound network-request limits fail
before new matching work is admitted; existing reservations may still settle or
finish so exhaustion cannot strand an operation. Every transactional update
returns a content-free snapshot with limits, used/reserved/remaining amounts,
usage-source counts, and warning/saturated/exhausted dimensions. Snapshots set
`permission_granted:false` and `execution_available:false`. Provider usage and a
Runtime-owned monotonic clock, persistence, scheduling, cancellation/refund policy,
and AAP/Qt budget events remain unavailable.

The internal `unified-execution-plan/0.1` invariant uses one fixed ordered stage
list for interactive, child, and background modes: identity, reconciliation,
permission, approval, workspace, budget, sandbox, recovery, durable job,
notification, release, dispatch, observation, and handoff. Mode selects required
gates and terminal evidence but cannot inject child/job/unattended bindings into a
different mode. The current Codex read-only interactive `turn/start` validates this
plan immediately before adapter dispatch. Child and background plans require their
budget, workspace, durable job, notification, and release evidence and remain
blocked/unadvertised. Plan output is content-free and always sets
`permission_granted:false` and `execution_available:false`; it is an invariant, not
an authorization or executor ticket. Typed proof composition, generic dispatch,
durable lifecycle, AAP/Qt mode status, and cross-platform evidence remain open.

Internal `background-job-request/0.1` and `background-job-state/0.1` contracts
define the future durable queue record without enabling it. A request binds the
project/session/root, unified plan identity, optional child, idempotency identity,
manual or one-shot schedule, bounded attempts/backoff, and optional safe retry
boundary. The transactional lifecycle separates pause request/acknowledgement,
waiting approval, cancel request/acknowledgement, terminal attempt evidence, and
bounded result references. Completion may win a cancellation race. Restart never
infers success: active work becomes interrupted, queued/paused/waiting states are
preserved, and retry is only eligible when the exact request pre-bound a safe
boundary and has capacity. Decisions always set `automatic_retry:false` and
`automatic_approval:false`; pause/resume cannot bypass schedule or retry backoff.
Workbench schema v11 persists exact canonical request/state JSON, hashes, schedule
metadata, generation, cancellation, attempts, and recovery ordering. Creation and
generation-CAS updates commit with typed `background-job.*` session events; identical
retries are idempotent, stale writers fail, event failure rolls back the projection,
and startup performs a bounded integrity scan before the store becomes writable.
Active jobs protect session deletion/retention. Schema v11 additionally persists one
optional `background-job-scheduler-lease/0.1` per job. Its canonical JSON and
redundant metadata bind the exact job/request/state generation, scheduler owner,
lease generation, bounded acquire/renew/expiry times, optional verified process
registration/process identities, terminal reason, and fixed false dispatch/takeover
authority. Acquire, renew, state-rebind, process-bind, release, and expiry use
generation CAS and typed `background-job.lease-*` events in the same transaction.
Active leases protect deletion even for a terminal job. A stale lease can be marked
expired without adopting the newer job state. Schema v10 upgrades through the normal
WAL-consistent backup and startup revalidates every bounded lease row.

Internal `background-job-scheduler/0.2` is the scheduler recovery boundary,
but it owns only a content-free inspection snapshot. It loads one complete bounded
recovery set from the verified store and binds it to a scheduler owner identity and
generation. Entries classify schedule wait, admission review, paused, approval wait,
retry review, terminal review, monitor-owned-process, or manual reconciliation. Lease
states distinguish missing, current, expired, released, stale job state, and owner
mismatch. Process ownership separately distinguishes missing lease/registration,
unavailable or non-running observation, mismatch, and exact current ownership. Each
entry fixes dispatch, automatic retry/approval, and automatic takeover to false.
Without the internal Runtime process registry, snapshots continue to report
`process_observation_available:false` and active state remains manual review.

Internal `background-job-process-observation/0.1` consumes an actual Runtime-owned
`std::process::Child`; it has no PID registration API and serialized evidence includes
no PID, command, path, environment, or output. Evidence binds scheduler owner,
project/session/root/job, exact request/state identities and generation, attempt,
opaque process-registration/process identities, registration time, and observation time. States are
`owned_running`, `owned_exited`, `absent`, `inaccessible`, `mismatched`, and
`unknown`. Only a current durable lease whose persisted process-registration and
process identities both match an exact `owned_running` observation becomes
monitor-only. Every other active result
requires manual reconciliation, and `owned_exited` additionally
requires a terminal job event; process exit never implies completion. Pending
cancellation separately requires acknowledgement. Truncation, invalid time/limit,
observation failure, or store failure leaves the prior scheduler snapshot unchanged.
The registry is in-memory. Persisted lease/process hashes cannot recreate its Child
handle after restart, so restart remains manual rather than automatically adopting a
PID. No AAP method, Qt control, scheduler loop, notification, automatic lease
acquisition/renewal, recovery mutation, or dispatch path consumes these records yet.

Internal `background-job-recovery-decision/0.1` turns one validated scheduler entry
into a content-free audit decision. It binds exact job status/cancellation and
generation, scheduler owner/generation/snapshot/entry, lease evidence, optional
process-observation identity, bounded blocker codes/hash/count, and
observation/record times.
`WorkbenchStore` rechecks the current durable job and lease before and under the
write transaction, then appends an idempotent
`background-job.recovery-reviewed` session event. At most 10,000 decision events are
accepted and all are semantically revalidated on store open; event failure rolls back
the sequence allocation and hash-consistent semantic tampering fails startup. Every
decision fixes retry, approval, takeover, dispatch, and mutation authority to false.
The journal does not transition a job, renew/release a lease, signal a process,
notify a user, or authorize a later recovery action. Automatic decision production or
consumption and AAP/Qt inspection remain unavailable.

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

## Context Budget

Every prepared `turn/start` response also carries a content-free
`context-budget/0.1` plan. Current inputs are explicit context items and
auto-discovered instructions. Instruction precedence ranks and `pinned`
priority determine deterministic allocation order; the rendered context text
keeps the caller/context order. The allocator enforces a 64 KiB total and 16
KiB per-item hard bound and reports requested/allocated bytes, class, priority
score, inclusion, and exclusion reason without source content. This is a partial
17.4 foundation; task state, recent turns, tools, search, repository maps,
tokenizer authority, and provider context windows are not yet budget consumers.

## Context Inspector

Capability `turn.context.inspect` exposes the read-only
`turn/context/inspect` preflight. It uses the same session-root validation,
explicit selected-pin resolution, instruction discovery, stale checks, and
`context-budget/0.1` allocator as `turn/start`, but never calls a model or
persists history. The versioned
`context-inspector/0.1` response contains only manifest/budget metadata and
explicit `content_included:false`, `model_started:false`, and `persisted:false`
flags. Source and instruction bodies are never returned. The Qt Work composer
exposes a preflight action for an existing session and renders only the
source/type/trust/size/status/reason metadata table; unchecked client context is
sent as an explicit exclusion marker.

## Instruction Discovery

Capability `workspace.instructions.discovery` exposes the read-only
`workspace/instructions` method. The request binds a registered project and
optional root, and may provide an existing root-relative `target_path` so only
the project-to-target ancestor chain is applicable. Managed and user sources
come only from the sidecar's path-only environment configuration:
`AEGISY_MANAGED_INSTRUCTIONS_DIR` and `AEGISY_USER_INSTRUCTIONS_DIR`.
Callers cannot supply arbitrary host paths.

The versioned response is `instruction-discovery/0.1`. Entries are sorted in
`merge_order:"weakest-first"` order using the explicit precedence
`managed > user > nested (closer depth wins) > project`. Each entry is bounded
and carries scope, logical relative path, depth, precedence rank/reason,
`untrusted-data` trust, byte/token estimates, SHA-256 content identity,
revision, freshness, inclusion, and rejection state. Content is omitted unless
`include_content:true`; even then it is bounded untrusted data and has no
permission, command, Hook, or network effect.

The discovery boundary rejects symlink components, sensitive and built-in
ignored paths, Git-ignored project files, case-collision names, invalid UTF-8,
control characters, secret-shaped content, and bounded file/count/byte
overflows. A file is reread for metadata after capture; a changed revision is
reported as `stale` and is not included. Work-mode `turn/start` automatically
appends up to eight valid discovered instructions after explicit user context,
using the same bounded context preparation and manifest. Secondary-root
attachments do not select the primary-root instruction chain. Rejected or
budget-excluded instructions remain visible through the explicit discovery
method; task `17.2` remains partial until durable configuration, complete
exclusion/budget reporting, inspection, and managed-policy integration are
complete.

```jsonl
{"jsonrpc":"2.0","id":"21","method":"workspace/instructions","params":{"project_id":"project-1","root_id":"root-1","target_path":"src/main.rs","include_content":false}}
{"jsonrpc":"2.0","id":"21","result":{"schema_version":"instruction-discovery/0.1","project_id":"project-1","root_id":"root-1","merge_order":"weakest-first","precedence":"managed > user > nested (closer depth wins) > project","content_trust":"untrusted-data","authority_effect":"none; instructions cannot grant permissions, execute commands, enable hooks, or authorize network","entries":[{"scope":"nested","source":"project","relative_path":"nested/src/AGENTS.md","depth":1,"precedence_rank":401,"precedence_reason":"nested-project-depth","trust":"untrusted-data","bytes":128,"token_estimate":32,"content_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","revision":"instruction-revision:128:1","freshness":"fresh","included":true,"content_state":"available"}],"included_files":1,"included_bytes":128,"truncated":false,"truncation_reasons":[],"rejection_reasons":[]}}
```

## Pinned Context Store

Capability `workspace.pinned-context.store` indicates that the private
`pinned-context-store/0.1` is available. The companion
`workspace.pinned-context.manage` capability indicates that the AAP metadata
management methods are exposed. The store accepts only the validated
`pinned-context/0.1` descriptor set: files, selections, images, diagnostics,
terminal excerpts, Git commits/diffs, artifacts, and child handoffs are
represented by bounded references, hashes, sizes, freshness, priority, and
secret-free metadata. It never persists source or instruction bodies.

`workspace/pinned-context/list` is project-scoped and returns the current set
identity plus descriptors, or an explicit empty/non-persisted result. It is
read-only and remains available during session recovery, deletion, or an
operation reconciliation gate. `workspace/pinned-context/save` requires the
request project to match the set project, revalidates every optional root and
session binding, and can receive `expected_set_identity` for compare-and-swap
protection against an older UI view. Identical sets are idempotent; updates
publish a new immutable object and retain the old one. Both responses state
`content_bodies_included:false` and never return a body field.

`workspace/pinned-context/remove` accepts `project_id`, `item_id`, and the same
optional `expected_set_identity`. It is an explicit unpin operation and uses
the exact set persistence path, so a stale UI cannot remove an item from a
newer set. Removing the final item writes a valid empty set with a new identity;
the next list therefore distinguishes an intentional empty set from a project
that has never stored pins.

Runtime-owned source changes use the additive
`pinned-context-source-invalidation/0.1` result embedded in successful
`workspace/watch/poll`, `workspace/save-user-text`, workspace diagnostic,
`terminal/restart-user`, and `terminal/remove-user` responses. Matching
file/selection references, diagnostic `metadata.path`, or terminal
`metadata.terminal_id` entries are changed from `fresh` to `stale` by publishing
a new immutable set through the existing pointer journal and metadata-only
project event. The result reports only `state`, `stale_count`, durability, old/new
set identities, or a bounded error code; it never returns paths or source bodies.
`failed` and `unavailable` are not interpreted as freshness. Context inspection
and turn start still reread the authoritative source and fail closed on identity
or authority loss. Qt accepts the returned `set_identity` only when the schema,
both hash formats, and `previous_set_identity` match its current view; any
ambiguity triggers a full `workspace/pinned-context/list` reload before another
CAS mutation.

Every save/remove compares the previous and next complete image-reference set.
If the final descriptor for a session-owned image is removed, the response event
reports additive `released_blob_reference_count`; the project event and release
commit in one SQLite transaction. Duplicate pins for the same session/reference
keep the Blob active until the final descriptor disappears. Failure rolls back all
releases, while success retains released content for at least 24 hours. This release
rule applies only to imported image references; command Artifacts retain their
independent Session-history lifecycle. Explicitly importing the exact same image into
the same Session later reactivates the identical released reference transactionally;
changed ownership, content, reference, media, or metadata cannot reuse that identity.
Release-bearing project events persist only a `pinned-context-release:sha256:` batch
identity and bounded count. The hash binds the release inputs and final set without
placing reference IDs, paths, or content in the event, and prevents an unrelated older
event for the same empty set from satisfying a later release.

`artifact/read-command-output` remains session-scoped and returns the validated
text Artifact together with an additive `session_id` binding field. Clients must
not infer ownership from the timeline item or request correlation ID. The Qt
read-only Artifact dialog uses that field for an explicit `固定完整输出` action:
only a matching active project-bound Work session may submit a metadata-only
`artifact` descriptor through `workspace/pinned-context/save`, with
`source:"command-output"`, the exact `command-output:sha256:` reference,
`content_hash`, retained UTF-8 byte count, priority `700`, and
`metadata.item_id`. The action is user initiated, never implicit model context,
and is disabled for cross-session, recovery/deletion/reconciliation-blocked,
busy-mutation, invalid-identity, or unsupported-media states.

Capability `turn.context.pinned-selected` indicates that `turn/start` and
`turn/context/inspect` accept `pinned_context_set_identity` together with an
explicit `pinned_context_ids` list. The runtime never sends all persisted pins
implicitly. A non-empty selection requires the exact current set identity,
contains at most 16 unique IDs, and is revalidated against the current project,
session, and registered root. The first assembly phase accepts `file`, `selection`,
project/root-bound `diagnostic`, session-owned `terminal_excerpt`, primary-root
`git_commit`/`git_diff`, session-owned `artifact`, session-owned `image`, and the
read-only `child_handoff` foundation. A child handoff must use an
`artifact:sha256:` reference for a text Artifact Blob owned by the current parent
session, with `child-handoff/0.1` metadata naming a distinct same-project source
session and bounded handoff ID. Assembly rereads and validates UTF-8, owner,
media type, byte count, and SHA-256; invalid or missing authority fails closed.
Child-task creation, lineage handoff persistence, approvals, and multi-agent
execution remain outside this foundation. Artifact assembly accepts only validated `command-output:sha256:`
text references and rechecks UTF-8, byte count, and SHA-256 before adding content.
The durable command-output path is reloaded through session-scoped Blob ownership
after Runtime restart; inspection remains metadata-only while turn assembly may
include the verified text. Diagnostic assembly accepts only normalized
`diagnostic-raw:sha256:` content from the authoritative DiagnosticStore under the
bound project/root and rechecks media type, reference, SHA-256, and byte count.
That store is memory-only: Runtime restart or eviction makes the pin unavailable,
and the persisted descriptor never substitutes for the missing body. Inspection
still returns metadata only. A terminal descriptor uses
`terminal-excerpt:<terminal>:<generation>:<start>:<end>` and is valid only while the
same Runtime-owned PTY capture retains that exact session/generation/range. Assembly
applies the same terminal normalization and verifies SHA-256 and byte count; Runtime
or terminal restart, removal, generation change, or capture eviction fails closed.
The metadata-only descriptor supplies a root-relative
path, expected raw-byte
SHA-256/revision, freshness, and priority. A selection additionally carries
bounded 1-based Unicode scalar `line`, `column`, `end_line`, and `end_column`
metadata. Both inspect and start use the normal authoritative resolver: it rereads
the file, reapplies ignore/sensitive/symlink/root policy, hashes the raw bytes,
marks a hash or
revision change `stale`, and only then extracts the selection range. Only
normalized UTF-8 text enters the bounded untrusted context envelope, and the
inspector still returns metadata only.

Capabilities `workspace.image.import-user` and `workspace.image.preview` are
advertised only with the durable Workbench store. `workspace/image/import-user`
is an explicit user operation bound to a current project Work session and
registered root. The request carries a basename, declared media type, and
standard Base64 bytes. Encoded size is rejected before decode; decoded content
is limited to 8 MiB and must independently decode as PNG, JPEG, or WebP with
edges no larger than 8192 pixels, at most 40 million pixels, and a 192 MiB decode
allocation ceiling. The response contains `image:sha256:` identity, bytes, media
type, width, and height, never the body or original local path.

`workspace/image/read` is read-only and requires the exact owning session and
`image:sha256:` reference. It revalidates Blob scope, bytes, SHA-256, media type,
dimensions, and decode limits, then returns only a PNG thumbnail bounded to
320x180 plus original metadata. A saved image descriptor must bind that exact
session/project/root/reference/hash/byte/media/dimension tuple. Inspect/start
rereads and decodes the Blob again; inspection exposes only manifest/budget
metadata. An included image reserves a conservative all-or-nothing 16 KiB budget
entry (4096 estimated tokens), while the binary body stays outside prompt text.
For Codex 0.144.5 the runtime creates a verified private hard link with a safe
format extension and sends it as a `localImage` input. The link is dropped after
the turn, and safely named crash leftovers are removed at Blob-store startup.
Neither the temporary path nor image bytes enter durable session history.

```jsonl
{"jsonrpc":"2.0","id":"30","method":"workspace/pinned-context/list","params":{"project_id":"project-1"}}
{"jsonrpc":"2.0","id":"30","result":{"schema_version":"pinned-context/0.1","project_id":"project-1","set_identity":null,"items":[],"persisted":false,"content_bodies_included":false}}
{"jsonrpc":"2.0","id":"31","method":"workspace/pinned-context/save","params":{"project_id":"project-1","set":{"schema_version":"pinned-context/0.1","project_id":"project-1","items":[{"id":"pin-file","project_id":"project-1","root_id":"root-1","kind":"file","source":"file-tree","label":"src/main.rs","reference":"src/main.rs","content_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","bytes":32,"freshness":"fresh","priority":850,"metadata":{}}]},"expected_set_identity":null}}
{"jsonrpc":"2.0","id":"31","result":{"schema_version":"pinned-context/0.1","project_id":"project-1","set_identity":"pinned-context:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","items":[{"id":"pin-file","project_id":"project-1","root_id":"root-1","kind":"file","source":"file-tree","label":"src/main.rs","reference":"src/main.rs","content_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","bytes":32,"freshness":"fresh","priority":850,"metadata":{}}],"persisted":true,"content_bodies_included":false}}
{"jsonrpc":"2.0","id":"32","method":"turn/context/inspect","params":{"session_id":"session-1","pinned_context_set_identity":"pinned-context:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","pinned_context_ids":["pin-file"]}}
{"jsonrpc":"2.0","id":"33","method":"workspace/image/import-user","params":{"session_id":"session-1","root_id":"root-1","label":"layout.png","media_type":"image/png","data_base64":"<bounded-base64>"}}
{"jsonrpc":"2.0","id":"33","result":{"schema_version":"pinned-image/0.1","project_id":"project-1","session_id":"session-1","root_id":"root-1","label":"layout.png","reference":"image:sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","content_hash":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","bytes":1024,"media_type":"image/png","width":640,"height":480,"preview_available":true,"content_included":false}}
{"jsonrpc":"2.0","id":"34","method":"workspace/image/read","params":{"session_id":"session-1","reference":"image:sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}}
```

The project pointer, immutable object, and Workbench SQLite event are separate
publications. A successful save appends a metadata-only
`project.pinned-context-updated/0.1` event after object publication; a failed
event append returns an incomplete-save error, and a failed pointer replacement
may leave a preserved unreferenced object. A private
`pinned-context-publication/0.1` journal records the previous/next set and object
identities before pointer replacement and is removed only after the event and Blob
release transaction succeeds. Runtime startup validates the journal against the
pointer, immutable objects, and latest project event: it cleans an unchanged
previous pointer, replays an uncommitted forward event/release, or cleans an already
committed event without appending a duplicate. Tamper or ambiguous state disables
the pin capability and preserves data. This does not claim cross-resource
atomicity. Image Blob release is atomic with the SQLite event, not with the earlier
external object/pointer publication; database failure therefore preserves the active
Blob and is recovered from the journal on the next startup. When a
descriptor uses a standard `*:sha256:` Blob reference, save
checks active SQLite metadata for exact project/session ownership, hash, and
byte count without reading Blob bytes or updating access time. File, selection,
validated artifact, diagnostic, and terminal pins now have explicit selected turn
assembly and authoritative reread or identity validation.
Qt loads project pins into the composer and exposes CAS-protected file and
clean, conflict-free editor-selection pin creation, per-turn inclusion, deterministic
order changes, and unpin without turning persisted pins into implicit model context.
The Structure diagnostics surface exposes explicit diagnostic pin/unpin after a
fresh `workspace/diagnostics/raw` read validates project/root, media type,
reference, SHA-256, and UTF-8 byte count.
The Terminal context menu separates transient `添加选中内容` from persistent
`固定最近输出`. Read-only `terminal/excerpt/read` returns at most 16 KiB of the
retained tail as `terminal-excerpt/0.1`, after pinned ANSI/OSC stripping and control
normalization, with absolute offsets/generation/hash. Qt validates it and persists
only the descriptor through CAS.

Capability `workspace.git-context.read-only` exposes the explicit user-preparation
method `workspace/git/context/read`. `git_commit` requires one complete lowercase
40-character OID and returns filtered deterministic commit detail under
`git-commit:<oid>`. `git_diff` accepts only `worktree`, `staged`, or `commit`; commit
scope also requires a complete OID and uses `git-diff:commit:<oid>`, while mutable
scopes use `git-diff:worktree` or `git-diff:staged`. Results are normalized text,
bounded to 16 KiB with explicit truncation, and return both the bounded
`content_hash` and complete normalized `source_hash`/`source_bytes`. Qt validates
the complete response before persisting only a metadata descriptor. Assembly
re-runs the same filtered query and checks reference, bounded content, complete
source identity, bytes, and truncation. Worktree/staged changes therefore invalidate
their pins; commit and commit-diff pins survive unrelated worktree changes but fail
if the exact Git object is no longer available. No Git mutation is added.

Child-task production, lineage handoff persistence, full cross-resource atomicity,
and Windows runtime evidence remain open. Startup also runs the bounded
`pinned-context-object-gc/0.1` sweep after publication compensation: it protects
pointer/journal objects, applies a 24-hour grace period, rechecks hash/schema and
file metadata, and preserves uncertain entries.
Qt watch/save callbacks mark loaded file, selection, and diagnostic pins stale
locally, while terminal restart/removal marks matching terminal-excerpt pins stale.
The sidecar independently persists the same source invalidation, so a later list or
Runtime restart retains the stale descriptor. Runtime inspect/start remains
authoritative and fail-closed. Agent/Codex remains read-only.

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
