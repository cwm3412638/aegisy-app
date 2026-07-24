# Aegisy Agent Protocol Guide

Version: `AAP 0.1`  
Audience: Qt host, `aegisy-agentd`, and future Codex/ACP adapters.

This is the internal wire guide for the currently shipped read-only workbench.
The normative schemas and limits live in `agent-runtime/aap-schema`; the JSONL
fixtures under `agent-runtime/aap-schema/fixtures` are deterministic replay
evidence. AAP is JSON-RPC 2.0 over newline-delimited stdio. Each request and
response is one complete JSON object; a notification has no `id`.

## Connection

The connection uses a two-stage handshake. Before it completes, `initialize` is
the only request the client may send. The request declares a numeric protocol
range and preference, bounded client identity, client platform, stable and
experimental capability sets, frame limit, and the observed transport-security
state. The runtime returns the selected protocol, its own identity and platform,
backend availability, the capability intersection, the effective limit, and the
same truthful transport-security state.

```jsonl
{"jsonrpc":"2.0","id":"initialize-compatible","method":"initialize","params":{"protocol":{"minimum":"0.1","maximum":"0.1","preferred":"0.1"},"client":{"name":"aegisy-client","version":"0.1.0"},"platform":{"os":"macos","architecture":"arm64"},"capabilities":{"stable":["permission.read-only","runtime.health","runtime.preview"],"experimental":[]},"limits":{"max_frame_bytes":4194304},"transport_security":{"transport":"stdio","local":true,"authenticated":false,"encrypted":false,"peer_verified":false}}}
{"jsonrpc":"2.0","id":"initialize-compatible","result":{"protocol":{"minimum":"0.1","maximum":"0.1","selected":"0.1","upgrade_direction":"none"},"runtime":{"name":"aegisy-agentd","version":"0.1.0"},"platform":{"os":"macos","architecture":"arm64"},"backend":{"adapter":"preview","status":"ready","version":"0.1.0"},"capabilities":{"stable":["permission.read-only","runtime.health","runtime.preview"],"experimental":[]},"limits":{"max_frame_bytes":4194304},"transport_security":{"transport":"stdio","local":true,"authenticated":false,"encrypted":false,"peer_verified":false}}}
{"jsonrpc":"2.0","method":"initialized","params":{}}
```

`backend.status: ready` describes Adapter availability; it does not complete the
protocol handshake and does not grant execution authority. After validating the
initialize response, the client sends the exact `initialized` notification above.
The runtime accepts business requests only after consuming that notification.
Duplicate initialize, initialized-before-initialize, initialized with an `id`, or
initialized with non-empty/missing params fails closed.

AAP `0.1` requires a non-empty stable declaration and an empty experimental set.
The response contains only stable capabilities supported by both peers; unknown
client capabilities are omitted. A ready or read-only-recovery backend also
requires its exact backend marker and `permission.read-only`. Every business
method is independently mapped to required capabilities: Qt does not send a
method whose requirements were not negotiated, and the runtime repeats the gate,
including for out-of-band cancellation, steering, and terminal stop. A successful
initialize response or degradation report is never a substitute for this gate.

Protocol versions are compared as numeric `major.minor` pairs, not strings. A
non-overlapping range fails before project or session state is loaded and returns
bounded `initialize-error/0.1` data. If the client maximum is below the runtime
minimum, `upgrade_direction` is `client`; if the client minimum is above the
runtime maximum, it is `runtime`:

```jsonl
{"jsonrpc":"2.0","id":"initialize-incompatible","error":{"code":-32003,"message":"AAP protocol ranges do not overlap","data":{"schema_version":"initialize-error/0.1","reason":"protocol-range-not-overlapping","client":{"minimum":"0.2","maximum":"0.3"},"runtime":{"minimum":"0.1","maximum":"0.1"},"upgrade_direction":"runtime"}}}
```

One newline-delimited JSON frame is capped at exactly 4 MiB in both directions
for AAP `0.1`. Each peer must enforce the fixed `max_frame_bytes` before writing
and while reading; an oversized
frame is drained or rejected without unbounded allocation or body diagnostics.
Negotiated inline payload sizes, chunking, and authenticated content references
remain OpenSpec task `3.8`; they must not be inferred from this fixed frame bound.
Current Qt-to-sidecar AAP uses child-process stdio. It is local but is not
authenticated, encrypted, or peer-verified; the five transport-security fields
must never claim otherwise. Authenticated Unix-socket and Windows named-pipe IPC
remain the target of OpenSpec tasks `4.2` through `4.4`.

Transport loss, runtime exit, handshake rejection, or malformed protocol input
clears readiness, negotiated capabilities, and the negotiated limit. No cached
capability may authorize a request after disconnect; reconnect starts a fresh
two-stage handshake.

## Session And Turn

Session identity is the boundary for history, environment identity, project
roots, terminals, artifacts, and provider continuation. Work sessions require a
project. The current Codex adapter uses `permission_profile: read-only`.

```jsonl
{"jsonrpc":"2.0","id":"2","method":"session/start","params":{"mode":"chat","title":"AAP example"}}
{"jsonrpc":"2.0","id":"2","result":{"session":{"id":"session-1","mode":"chat","title":"AAP example"},"runtime":{"adapter":"preview","version":"0.1.0","provider":"local","model":"deterministic-echo","permission_profile":"read-only"},"environment":{"environment_id":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","identity":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}}
{"jsonrpc":"2.0","id":"3","method":"turn/start","params":{"session_id":"session-1","input":"Inspect the project","idempotency_key":"turn-key-1"}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:f305276603e639d98638df9dfc805247f46cda7c3e68abac647f52335996fb24","sequence":1,"timestamp_ms":1730000000000,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"turn.started","item":null,"item_update":null}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:fc245999261fadedb20f05abf2846b0ddd3fcd03b1dfcead7130fd05e55a3b9a","sequence":2,"timestamp_ms":1730000000001,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"item.started","item":{"id":"item-1","kind":"message","role":"agent","state":"started","content":""},"item_update":{"revision":1,"content_mode":"snapshot-replacement"}}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:d70dbd6c228cc8308796f50d4add55627491625139999fcd627e2ea7d0e1e84a","sequence":3,"timestamp_ms":1730000000002,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"item.delta","item":{"id":"item-1","kind":"message","role":"agent","state":"delta","content":"Project summary"},"item_update":{"revision":2,"content_mode":"snapshot-replacement"}}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:29638b7362125d3dcc1e179a651ac0879df8993520c435820f3109369d0f7731","sequence":4,"timestamp_ms":1730000000003,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"item.completed","item":{"id":"item-1","kind":"message","role":"agent","state":"completed","content":"Project summary"},"item_update":{"revision":3,"content_mode":"snapshot-replacement"}}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:f79be436b3316726db313dd4feadf4bcb5d1daedfde2a8d3276c8487d92da86b","sequence":5,"timestamp_ms":1730000000004,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"completed","event":"turn.completed","item":null,"item_update":null}}
```

Every event uses `timeline-event/0.1`. `sequence` is contiguous per Session in
the current Runtime live stream and every numeric cursor stays at or below
`9007199254740991`. `timestamp_ms` is the Runtime observation time and never
moves backwards within that Session; provider time remains separate Item
metadata. `correlation_id` equals `turn_id`. `event_id` is a domain-separated
SHA-256 identity over the immutable event fields other than the ID itself.

The Event ID input is byte-exact. First encode one compact UTF-8 JSON object with
fields in this order: `schema_version`, `sequence`, `timestamp_ms`,
`correlation_id`, `session_id`, `turn_id`, `turn_state`, `event`, `item`, and
`item_update`. An Item uses `id`, `kind`, `role`, `state`, `content`, then optional
`data`; an Item update uses `revision`, then `content_mode`. Present `null` Item
and Item-update fields remain literal `null`, while absent optional Item `data` is
omitted. Item-data object keys are sorted by ascending UTF-8 bytes at every level;
array order is semantic and retained. Strings use compact JSON escaping, emit
non-control Unicode scalars as UTF-8 without normalization, and reject invalid
Unicode scalar sequences. Mathematical integers are normalized to their shortest
base-10 integer form with no exponent, decimal point, leading plus, or redundant
leading zero; every representation of zero, including negative zero, becomes `0`.

Let `material` be those exact UTF-8 bytes and `uint64be` an unsigned eight-byte
big-endian length. The digest input is
`"aegisy-timeline-event/0.1\0" || uint64be(len(material)) || material`, where
`\0` is one NUL byte. `event_id` is `event:sha256:` followed by the digest's 64
lowercase hexadecimal characters. No transport wrapper or `event_id` bytes enter
the digest.

An Item update carries a contiguous per-Item revision. `snapshot-replacement`
means `item.content` is the complete current bounded snapshot, so clients replace
rather than append. Streaming Items follow `started -> delta* -> completed`;
an atomic Item may emit only `completed` at revision one. Session, Turn, Item,
kind, and role identity cannot drift. A Turn has exactly one terminal state:
`completed`, `interrupted`, or `failed`; `turn.persistence-failed` is not a valid
fourth terminal. Reusing an idempotency key with different input is an error;
retrying the same request returns the original turn identity.

To keep Event IDs byte-identical across Rust and Qt, optional Item `data` is a
canonical JSON tree with at most 16 levels and 4,096 total values. Objects contain
at most 128 properties whose names are 1-128 byte ASCII graphical strings, arrays
at most 4,096 entries, and every number must have an exact mathematical integer
value in `[-9007199254740991, 9007199254740991]`. Non-integral or non-finite
values, unsafe property names, and values outside that range are rejected before
sequence allocation. The JSON Schema enforces value types and per-container
bounds; Rust and Qt typed validators additionally enforce the aggregate depth and
node limits.

Structurally valid unknown events are safe only when they are itemless, running,
and bound to the active Turn. An older client advances its cursor and records a
bounded content-free diagnostic without projecting UI state. An unknown event
cannot carry an Item or create authority.

The dedicated public Event Journal, not the internal Workbench projection-event
stream, owns durable reconnect sequence authority. A fixed-watermark catch-up page
uses the following AAP `0.1` data contract. Runtime advertises
`timeline.replay.fixed-watermark` only when its durable Workbench Store is healthy;
`timeline/sync` otherwise remains unavailable. The Qt client declares and gates the
same capability instead of attempting an unnegotiated fallback.

```jsonl
{"jsonrpc":"2.0","id":"timeline-sync-empty","method":"timeline/sync","params":{"session_id":"session-empty","after":{"sequence":0,"event_id":null},"watermark":null,"limit":200}}
{"jsonrpc":"2.0","id":"timeline-sync-empty","result":{"schema_version":"timeline-sync-page/0.1","session_id":"session-empty","after":{"sequence":0,"event_id":null},"watermark":{"sequence":0,"event_id":null},"events":[],"next_after":null,"complete":true}}
```

If the requested anchor is older than the durable retention floor, Runtime returns
the distinct structured error below. This response does not contain a Session
snapshot, does not expose the internal Sequencer checkpoint, and does not authorize
the client to resume from `retained_floor` while silently discarding older visible
state. Until `timeline.snapshot.current` is negotiated, the affected Session remains
frozen while unrelated Sessions can continue.

```jsonl
{"jsonrpc":"2.0","id":"timeline-gap-1","error":{"code":-32148,"message":"requested Timeline history is no longer retained","data":{"schema_version":"timeline-retention-gap/0.1","reason":"requested-anchor-not-retained","session_id":"session-1","requested_after":{"sequence":0,"event_id":null},"requested_watermark":null,"retained_floor":{"sequence":2,"event_id":"event:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"head":{"sequence":3,"event_id":"event:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"snapshot_required":true,"snapshot_available":false,"snapshot_capability":"timeline.snapshot.current","snapshot_method":"timeline/snapshot","event_history_complete":false,"replay_from_floor_allowed":false}}}
```

Only a genuine pre-floor request uses `-32148`. A malformed, substituted, or
same-sequence wrong Event ID in retained history remains anchor/watermark drift and
uses the ordinary sync failure path. This prevents a forged anchor from being
misrepresented as normal retention recovery.

Every anchor is the exact pair `{sequence,event_id}`. Sequence zero is represented
only as `{0,null}`. Every positive sequence requires the exact 77-byte lowercase
`event:sha256:` identity of that event; a null, uppercase, malformed, or substituted
ID is invalid. All sequences are JSON-safe mathematical integers. The request
contains exactly `session_id`, `after`, nullable `watermark`, and `limit`; the limit
is 1 through 200.

On the first request, `watermark:null` asks Runtime to capture the Session Journal
head once. The response always returns that fixed anchor. Every continuation repeats
it verbatim, so events appended while paging cannot move the target or make catch-up
chase a live head. The response schema is `timeline-sync-page/0.1` and repeats the
same Session and `after`, then returns only contiguous same-Session events after that
anchor and at or below the watermark. Runtime also keeps the serialized page below
the 4 MiB AAP frame budget; the 200-event count is not permission to exceed it.

An incomplete non-empty page sets `next_after` to the exact sequence/Event-ID pair
of its final event and `complete:false`. A complete page reaches the fixed watermark,
sets `next_after:null`, and uses `complete:true`; an empty Journal therefore completes
at `{0,null}`. Schema validation closes every object and enforces local bounds. Typed
Rust and Qt validation additionally enforces request/response identity, anchor
ordering, event continuity, Event-ID agreement, final-anchor semantics, and the
requested page limit before any Timeline projection changes.

Runtime sequencing is a prepare/commit boundary. `prepare` clones the Session
lifecycle state and returns the candidate envelope without advancing sequence,
timestamp, Turn, or Item state. Runtime serializes the notification and durably
appends the envelope before `commit` verifies the exact sequence/Event-ID baseline
and installs that candidate. A stale prepared ticket is rejected rather than
overwriting newer Session state. Dropping a prepared event after serialization or
persistence failure therefore advances no in-memory lifecycle cursor.

For durable Codex Turn creation, sanitized Item append, Item plus command Artifact,
and normal completed/failed/interrupted Turn Trace terminal paths, the Workbench
projection event, affected projection rows, Blob reference when present, and public
Journal append commit in one SQLite transaction. A public Item must exactly equal the
Store's sanitized persisted Item projection, including Session/Turn binding, kind,
role, state, content, non-null data, and projection timestamp; Runtime constructs the
live/replay Item from that same sanitized preview. A mismatch or Journal failure
rolls back the projection transaction and leaves the prepared sequencer state
uncommitted. Streaming and control-only Timeline events that have no durable Item or
Turn projection append only to the public Journal before sequencer commit.

Qt owns an independent recovery state for each Session. A live sequence gap freezes
projection for only that Session, queues bounded later live events, and starts
fixed-watermark sync from the last confirmed sequence/Event-ID anchor. Every replay
page is validated into a private candidate; incomplete pages remain invisible. Only
after the final page reaches the unchanged watermark does Qt publish the complete
candidate once, render the staged events, and drain the queued live events through
normal validation. Anchor drift, malformed pages, request failure, capability loss,
or queue/batch overflow preserves the last confirmed projection and freezes the
affected Session.

The Preview backend still persists its Turn/Item projections before journaling its
six-event synthetic Timeline and is not covered by the atomic producer boundary
above. Adapter and persistence compensation paths can also durably finish a Turn
Trace before appending the public terminal/Error event in a later transaction; those
fallbacks remain outside the atomic producer boundary. Snapshot, structured
retention-gap recovery, live subscription, heartbeat, complete reconnect
orchestration, explicit acknowledgement, and Windows recovery evidence also remain
later parts of OpenSpec `3.5`; keep that task incomplete.

## Structured Errors And Cancellation

Error messages are diagnostics, not a stable UI contract. Clients should use the
numeric code plus bounded `runtime-error/0.1` data when present. Provider,
transport, timeout, persistence, and adapter classes never include credentials,
prompts, response bodies, or raw provider rollout text in AAP timeline data.

```jsonl
{"jsonrpc":"2.0","id":"4","method":"turn/cancel","params":{"session_id":"session-2","turn_id":"turn-2"}}
{"jsonrpc":"2.0","id":"4","result":{"state":"cancellation-requested"}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:72ea0537d20013e275ce8545ee08e65a783deeef0fec69ad011ff823679217e9","sequence":1,"timestamp_ms":1730000000099,"correlation_id":"turn-2","session_id":"session-2","turn_id":"turn-2","turn_state":"running","event":"turn.started","item":null,"item_update":null}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:fd7d6120bc7ba2ffff60562cd172a6a4d872781ad638953f89be27e7a5837e4f","sequence":2,"timestamp_ms":1730000000100,"correlation_id":"turn-2","session_id":"session-2","turn_id":"turn-2","turn_state":"running","event":"turn.cancellation-acknowledged","item":null,"item_update":null}}
{"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:8b416a06c09ed140775e75d12d36a333a68e863e4565ffea33a25c681695eae2","sequence":3,"timestamp_ms":1730000000101,"correlation_id":"turn-2","session_id":"session-2","turn_id":"turn-2","turn_state":"interrupted","event":"turn.interrupted","item":null,"item_update":null}}
{"jsonrpc":"2.0","id":"5","error":{"code":-32110,"message":"provider request failed: [REDACTED]","data":{"schema_version":"runtime-error/0.1","class":"provider","retryable":false,"provider_error":{"schema_version":"provider-error/0.1","source":"codex-app-server","kind":"unauthorized","class":"provider","http_status":401,"retryable":false,"response_body_included":false,"credentials_included":false}}}}
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

Provider metadata is intentionally content-free. The `provider-error/0.1`
object may preserve only the stable kind/class, an optional HTTP status, and a
retryability decision. It must set `response_body_included:false` and
`credentials_included:false`; a provider response body or dynamic provider
message is never copied into an AAP Timeline item. The local gateway uses the
same contract for 4xx/5xx, rate-limit, connection, and SSE-disconnect events
and removes upstream `x-aegisy-error-*` headers before forwarding a successful
response.

Pinned Codex `error` notifications are observations, not terminal events. Aegisy
emits `turn.error-observed` with a fixed display message, stable content-free
classification, `will_retry`, and `terminal:false`; it does not copy the dynamic
Codex message. A Turn ends only after a schema-valid `turn/completed` notification
whose exact Turn ID has status `completed`, `failed`, or `interrupted`. Missing,
mismatched, `inProgress`, or unknown terminal state is a protocol failure. A
non-terminal error followed by EOF is therefore terminated by the transport
failure path rather than by the observation itself.

Usage accounting is separately described by the internal
`usage-authority/0.1` contract. Token, context, cost, and reasoning metrics
carry one of `observed`, `catalog-derived`, `estimated`, `stale`, or `unknown`
evidence labels. This metadata is validation-only and grants no model,
routing, billing, token, or turn authority; unknown values have no numeric
payload and stale values cannot remain authoritative.

Codex `thread/tokenUsage/updated` events keep their existing bounded numeric
projection and add an `authority` member containing this report. Provider token
counts, the reported context window, and reasoning token counts are labelled
`observed`; cost remains `unknown`. If a provider total does not reconcile with
its component semantics, Aegisy preserves the original bounded total but does
not silently copy it into the authoritative field.

The sidecar also contains internal `context-threshold/0.1` and
`turn-trace/0.6` validation contracts. Neither is currently an AAP capability.
Codex usage Timeline metadata may include a `compaction_threshold` decision
computed from provider-observed last-input/context-window values. Runtime can
restore its hysteresis latch from complete bounded usage history and projects a
content-free Session summary; `automatic_compaction_authority` is always false.
The Workbench Store atomically persists validated terminal traces and strictly
replays inner `turn-trace/0.1` through `turn-trace/0.6` without rewriting legacy
events. Existing `0.1` through `0.5` records retain their version-specific
compatibility and fixed legacy identities; cross-version fields, unknown fields,
and future `0.7+` versions fail closed. The outer durable
envelope remains `turn.trace.recorded/0.1`, and the Workbench Store remains SQLite
schema v14. Turn Trace itself required no schema migration; v14 was introduced
later for the authority-free model-profile projection. The pinned Codex Runtime
produces `0.6` traces for completed, failed, and interrupted Turns.
One immutable Intent identifies Chat conversation or current Work read-only
inspection. Completed terminal metadata binds that Intent and independently marks
Workspace change, Git change, and verification as not-applicable, applicable,
unknown, or observed. Chat uses three not-applicable domains; Work has no mutation
authority, so Workspace/Git change are not applicable and unobserved verification
is unknown. `completed` means only that the provider Turn lifecycle ended normally;
it is not proof of file changes, Git state, or passing tests. Failed/interrupted
traces carry no completion domains. Runtime and Session model-binding identities,
prepared Context counts/hashes, stable Error classification, and terminal evidence
remain metadata-only. Admission, direct read, and replay require the Trace project,
environment, and current Intent Chat/Work mode to match the durable Session binding;
legacy `0.1` traces retain their mode-less Intent semantics but still bind Session,
Turn, project, and environment. A mismatch fails closed rather than reclassifying
completion. Trace
versions `0.3` through `0.6` may additionally contain one `usage-report` event. It embeds the validated
`usage-authority/0.1` report as the latest successfully validated and persisted
Codex Provider-thread snapshot, with `scope: provider-thread`,
`accounting: absolute-snapshot`,
and both Attempt and Retry attribution explicitly `unavailable`. Provider thread
totals are not an Aegisy Turn Attempt total, so snapshots are never summed and no
Attempt or Retry identity is manufactured.

Runtime first clone-preflights the candidate Trace with the Usage Item identity and
authority report, then commits the ordinary Usage Timeline Item, and replaces the
authoritative accumulator only after that commit succeeds. At terminal
finalization it emits at most the latest retained snapshot, binds the report's
deterministic metadata identity, observation time, and exact persisted Usage Item,
and places it before any Error and the terminal event. Completed, failed, and
interrupted traces retain a snapshot when one was successfully validated, observed,
and persisted. Multiple notifications replace the retained absolute snapshot; they
do not add to it. A Turn with no valid, successfully persisted Usage snapshot has no
`usage-report` event; it does not receive an `unknown` placeholder. Store admission,
direct read, and projection replay inspect the complete bounded Session Item prefix
through that Turn's final Usage Item. They start from the single Session initial
state `NoAction`, replay every prior Usage transition in sequence, revalidate each
current Item's Session/Turn/kind/role/state, payload hash, raw Provider snapshot
shape, reconstructed authority report, and context-threshold transition, then
require the Trace to bind the final valid authority snapshot. The scan uses the same
100,000-all-Item uncertainty boundary as Runtime restoration and never reads later
Turns when validating an older Trace. A genuinely malformed Provider notification
that cannot form an authority report remains ordinary Timeline metadata and is not
a Trace Usage candidate, but advances the review latch to conservative
`PreviewRequired` consistently live, in Store replay, and after restart. Valid raw
Usage with a removed authority report is rejected as a semantic downgrade rather
than treated as malformed input. Initial-state and cross-Turn hysteresis substitution
therefore fail closed even when Item and Event hashes are recomputed.

Trace versions `0.4` through `0.6` also record a content-free Tool lifecycle for
observed Codex command Items. A Started Tool binds the closed provider status/source, provider
timestamp, action identity, and stable input identity while explicitly declaring that
no Timeline Item has been persisted. Stable input identity is computed only from a
fixed, compile-time-known projection of command, cwd, action type, and the recognized
typed action fields. Unknown Provider keys and values never enter that identity.
Object keys are canonicalized, while action-array order remains semantic.

The adapter separately compares an opaque memory-only SHA-256 fingerprint of the
complete untruncated Provider command/actions/cwd, including unknown fields, before
accepting a terminal notification. That fingerprint is neither serialized nor
persisted. A terminal completed, failed, or declined observation is first constructed
from the exact would-be sanitized persisted Item returned by Store preview and is
preflighted on an accumulator clone. The command Item and any Artifact/Blob reference
then commit, and only a successful commit replaces the authoritative accumulator. The
Tool binds a Session/Turn/Item domain-separated Item identity, never the raw Provider
Item ID, plus the complete sanitized Item payload SHA-256, output identity, duration,
exit status, and terminal timestamp. Neither command, cwd/path, action content, output,
process ID, Provider body, raw Item ID, nor raw fingerprint enters the Trace. Invalid
status/source changes, input drift,
duplicate or missing lifecycle events, reverse time, duration outside the observed
interval, and contradictory exit status fail closed. Completed and cancelled Turns
cannot retain an unterminated Started Tool; failed and interrupted Turns may retain it
without fabricating terminal authority. Provider `declined` is a Tool observation.

Trace versions `0.5` and `0.6` contain exactly one content-free Runtime approval-policy
observation before Model or Context metadata. For the pinned Codex adapter it binds
the exact Runtime, `codex-app-server` adapter, `0.144.5` Runtime version, durable
adapter version `codex-cli 0.144.5`, fixed producing Runtime identity
`aegisy-agentd:0.1.0`, Provider-thread identity,
configured/effective policy identities,
`approvalPolicy=never`, read-only sandbox, and read-only permission profile. Store
admission, direct read, projection replay, and startup quarantine independently
recompute this binding from the durable Session Runtime row. The event always has
`decision_attribution=no-user-decision`, `user_decision_observed=false`, and
`execution_authority=false`: it describes observed Runtime policy and is not an
Approval request, decision, denial, or grant.
The fixed producing Runtime identity keeps existing `0.5` records replayable across
future binary upgrades; a new producer version must explicitly revise the Trace
contract or add a reviewed compatibility entry. Both `turn-trace/0.5` and `0.6`
reject every `Approval` payload until a durable approval-authority producer and
ledger binding exist.

Trace version `0.6` adds a distinct content-free `RuntimeDenial` observation for an
actual approval request bound to the active Codex Turn. The recognized request
classes and fixed local responses are:

- `item/commandExecution/requestApproval` -> `{"decision":"decline"}`.
- `item/fileChange/requestApproval` -> `{"decision":"decline"}`.
- `item/permissions/requestApproval` -> `{"permissions":{},"scope":"turn"}`.

The permissions response is an empty permission grant interpreted by Runtime as a
denial; it is not a literal `decision: decline` response. The adapter requires a
valid bounded request ID, the exact active Provider thread and Turn, a non-empty
bounded Item ID, and a non-negative `startedAtMs`. A mismatch receives JSON-RPC
`-32602`, fails the Turn, and creates neither `RuntimeDenial` nor `Approval`.
Approval requests received outside an active bound Turn are likewise errors rather
than denial observations.

The checked-in generated Codex `0.144.5` schema does not define these three
`ServerRequest` shapes, so this stage does not claim generated-schema validation for
them. The current evidence is the same-version App Server protocol source together
with deterministic real-stdio fixtures that match each request and response shape.
Any Codex pin upgrade must re-review those sources and fixtures before changing the
adapter contract.

The Provider request identity is a SHA-256 hash of the complete bounded request
message. The durable request identity additionally binds the Trace, Provider-thread
and policy-authority identities, and `request_kind`; changing a command request into
a file or permissions request cannot preserve identity. Runtime preflights that
identity, duplicate detection, the 128-observation ceiling, delivery order, and the
exact 72 KiB durable reservation into a non-serializable prepared ticket. Only then
does the adapter write and flush the fixed response. Runtime commits the prepared
observation after that flush succeeds, using preallocated storage and a monotonic
timestamp. A failed write or abandoned ticket records no denial; the normal
fail-closed adapter/Turn error path handles the failure without inventing response
evidence. If Trace preflight fails after a valid request is bound, the adapter writes
and flushes a fixed content-free JSON-RPC error before failing the Turn; that backend
is reusable only after the error write succeeds. A denial-response or fallback-error
write/flush failure marks the adapter unavailable and restart-required instead of
returning a potentially blocked protocol channel to the next Turn. Missing/invalid
request IDs and malformed params receive a fixed `id:null`, `-32602` error and also
discard the backend; safely bound thread/Turn/Item/time mismatches echo only the
validated request ID and may reuse the channel after the error flush succeeds.

`response_state=decline-flushed` proves only that Aegisy wrote and flushed the local
JSON response into the Codex child-process stdin. It does not prove that a Provider
received, processed, or acted on the response. Every denial is attributed to
`runtime-policy`, fixes `user_decision_observed=false`,
`approval_authority_observed=false`, and `execution_authority=false`, and carries
Runtime-observed metadata-only evidence. Denials share delivery ordering with Tool
and Usage observations and must precede Error and Terminal evidence. Store terminal
admission, direct Trace read, projection replay, and startup quarantine independently
recompute the exact Runtime/adapter/version/Provider-thread/policy binding, reject
duplicate request identities and identity/authority/redaction drift, and fail closed
on invalid denial ordering.

Provider `declined` remains only a terminal Tool observation. An observed
`approvalPolicy=never` remains only Runtime policy evidence. Neither is a
`RuntimeDenial`, and none of these three facts is a genuine user `Approval`. The
current read-only adapter still has no genuine-user Approval producer; any future
producer must use separate Approval-authority evidence and cannot be inferred from
Runtime policy, Runtime denial, or Provider Tool state.

Every producer admission serializes the complete outer
`turn.trace.recorded/0.1` envelope and enforces the exact 72 KiB durable limit. It
reserves the worst legal terminal for every open Tool, the worst failed Error and
completed/failed/interrupted Terminal, and one emergency Started while admission
remains open. At exhaustion the emergency Started is still emitted and retained, its
terminal is denied before Item/Blob persistence, and Runtime durably fails the Turn
with Started + Error + failed Terminal. A Provider-completed Turn with a missing Tool
terminal follows the same failed compensation path. Store independently rechecks the
72 KiB limit on admission, direct read, projection replay, and restart; oversized
records fail closed even if producer logic regresses.

Trace records persist only metadata and identities: no prompt, Provider request or
response body, path, command, output, diff, credential, raw request ID, or raw Item ID
enters the Trace. Trace records are not Timeline Items and have no AAP/Qt read,
audit/export, or retention surface. Clients must not infer task success, automatic
compaction, trace visibility, or trace export from these fields.

The Qt Timeline treats this metadata as untrusted protocol input. It requires
the exact usage schema, all four metric classes, consistent authority/value
types, and a threshold that explicitly denies automatic authority. Only fixed
localized labels and non-negative numeric values are rendered; malformed or
unknown versions become a fixed unknown state without exposing raw provider
text.

## Capability Degradation

`runtime/degradations` is a versioned, content-free explanation for features that
are unavailable, metadata-only, or blocked. The current Codex adapter reports
read-only Agent mutation, metadata-only provider thread items, blocked provider
delete/compact, runtime-only desktop gaps, and the release gate for
background/multi-agent autonomy. Version `0.2` binds the complete declaration to
the pinned Codex `0.144.5` schema through a fixed capability-matrix identity and
exact request/notification/item counts. The
autonomy entries use `availability: not-advertised`, `stable_enabled: false`,
`override_available: false`, and bounded `missing_gates`; there is no hidden
experimental switch. A client must fail closed when the schema is unknown or the
request fails, and it must not start a new Turn until the complete snapshot is
validated.

```jsonl
{"jsonrpc":"2.0","id":"6","method":"runtime/degradations","params":{}}
{"jsonrpc":"2.0","id":"6","result":{"schema_version":"runtime-degradations/0.2","backend":{"kind":"codex","adapter":"codex-app-server","version":"codex-cli 0.144.5","status":"ready"},"capability_matrix":{"schema_version":"codex-capability-matrix/0.1","identity":"codex-capability-matrix:sha256:473ddd66cd30b903778c248f28aa55d3cfb2ff37123c4831a23a263703362d04","adapter":"codex-app-server","codex_version":"codex-cli 0.144.5","vendor_schema_version":"v2","vendor_schema_sha256":"e66ff6063c146734a92c9a018e43efefb079278ee597782f30674edcccedbdb2","client_request_count":87,"server_notification_count":68,"thread_item_count":18,"complete":true},"complete":true,"degradations":[{"feature":"agent-mutation","state":"disabled","reason":"Aegisy Codex sessions use read-only sandbox and never approve writes or mutating commands","scope":"runtime","authority_granted":false},{"feature":"provider-thread-item-content","state":"metadata-only","reason":"provider thread list/read omit raw rollout items until stable AAP item mappings exist","scope":"provider","authority_granted":false},{"feature":"provider-thread-delete","state":"blocked","reason":"requires scoped user review, recovery, retention, and compensation","scope":"provider","authority_granted":false},{"feature":"provider-thread-compact","state":"blocked","reason":"requires a durable checkpoint, preservation review, and failure recovery","scope":"provider","authority_granted":false},{"feature":"turn.steer.same-turn","state":"runtime-only","reason":"Codex runtime supports same-turn steering but the desktop surface is not complete","scope":"provider","authority_granted":false,"runtime_supported":true,"desktop_surface_available":false},{"feature":"session.provider.lifecycle.list-read","state":"runtime-only","reason":"Codex runtime supports provider list/read metadata but the desktop surface is not complete","scope":"provider","authority_granted":false,"runtime_supported":true,"desktop_surface_available":false},{"feature":"background-jobs","state":"disabled","reason":"durable scheduling, recovery, budgets, notifications, and release evidence are incomplete","scope":"runtime","authority_granted":false,"availability":"not-advertised","stable_enabled":false,"override_available":false,"missing_gates":["21.2","21.6","21.8","21.9","20.9"]},{"feature":"multi-agent","state":"disabled","reason":"child contracts, isolated worktrees, approvals, budgets, recovery, and review are incomplete","scope":"runtime","authority_granted":false,"availability":"not-advertised","stable_enabled":false,"override_available":false,"missing_gates":["18.3","21.3","21.4","21.5","21.6","21.10"]},{"feature":"unattended-writes","state":"disabled","reason":"Agent mutation remains read-only until permission, sandbox, approval, checkpoint, and recovery gates complete","scope":"runtime","authority_granted":false,"availability":"not-advertised","stable_enabled":false,"override_available":false,"missing_gates":["15.3","16.7","18.3","18.4","18.5","18.6"]}]}}
```

No degradation response grants write, command, Hook, network, deletion, or
compaction authority. The Qt host displays the state and does not expose an
action that would simulate a blocked provider operation.

Provider list pagination treats a cursor as an opaque Provider token. A cursor
that is at most 4 KiB and contains neither controls nor credential-shaped content
is returned and sent back byte-for-byte. An invalid request or response cursor
fails the operation; it is never truncated, redacted, or replaced with a token
that could change Provider pagination semantics.

## Model Catalog Boundary

The negotiated `model.catalog.read-only` capability exposes
`model/catalog`. The current response is an offline runtime-binding projection,
not the signed Aegisy cloud catalog. It may identify the active provider/model,
but availability, limits, capabilities, role suitability, entitlement, and
policy fields remain explicitly unknown until an authenticated catalog is
validated. Unknown booleans are serialized as `null`; clients must disable a
dependent feature rather than treating `null` as support. The response contains
no credentials, refresh token, prompt, or provider response content. The legacy
`runtime_compatibility` summary remains for additive compatibility. New clients
consume `runtime_compatibility_matrix`, whose entries identify the adapter
family, adapter and protocol IDs, exact evaluated versions, field authority,
evidence version, and structured known degradations.

```jsonl
{"jsonrpc":"2.0","id":"7","method":"model/catalog","params":{}}
{"jsonrpc":"2.0","id":"7","result":{"schema_version":"model-catalog/0.1","catalog_version":"offline-runtime-binding","state":"offline","source":"runtime-binding","signature_validated":false,"refresh_supported":false,"models":[{"model_id":"local:deterministic-echo","provider":"local","availability":"unknown","entitlement":"unknown","lifecycle":"unknown","limits":{"context_tokens":null,"output_tokens":null,"authority":"unknown"},"capabilities":{"tool_calls":null,"image_input":null,"authority":"unknown"},"runtime_compatibility":{"adapter":"preview","adapter_version":"0.1.0","state":"metadata-only","known_degradations":["catalog-not-authenticated","model-capabilities-unknown"]},"runtime_compatibility_matrix":[{"schema_version":"model-runtime-compatibility/0.1","adapter_family":"native","adapter":"preview","protocol":"aap-native","exact_versions":["0.1.0"],"state":"unknown","authority":"unknown","evidence_version":null,"known_degradations":[{"code":"catalog-not-authenticated","severity":"warning","affected_features":["model-selection"],"summary":"runtime compatibility is not authenticated"}]}]}],"contains_credentials":false}}
```

This boundary is read-only and does not select a model, issue a token, refresh
the cache, or emit a model-change event. Catalog signing, authenticated refresh,
durable cache states, capability matching, profiles, and model switching remain
OpenSpec tasks `9.1` through `10.12`.

The additive read-only method `model/catalog-refresh-status` reports the state of
the host-owned catalog refresh contract. The current response is deliberately
`unconfigured` because no production endpoint or trust anchor is embedded in the
repository:

```jsonl
{"jsonrpc":"2.0","id":"8","method":"model/catalog-refresh-status","params":{}}
{"jsonrpc":"2.0","id":"8","result":{"schema_version":"model-catalog-refresh-status/0.1","state":"unconfigured","endpoint_configured":false,"trust_anchor_configured":false,"authenticated_transport_required":true,"conditional_requests_supported":true,"last_attempt_at_ms":null,"last_http_status":null,"last_outcome":null,"last_error_code":"production-catalog-endpoint-and-trust-anchor-unavailable","retryable":false,"etag_present":false,"last_modified_present":false,"validator_identity":null,"response_body_retained":false,"credentials_included":false,"cache_install_authority":false,"selection_allowed":false}}
```

The transport contract accepts only a Qt-owned authenticated observation of a
bounded JSON response: a 200 response must carry a signed bundle and a
conditional validator, while a 304 response must be backed by a prior ETag or
Last-Modified validator. Redirects, non-identity content encodings, oversized or
malformed bodies, and content-free authentication/rate-limit/server failures are
rejected. The status method never refreshes or installs the cache and never
grants model, token, routing, or Turn authority.

The additive `model.capability-check.read-only` capability exposes
`model/capability-check` for preflight only. Its requirements identify Chat or
Work mode, attachments, tools, reasoning, a context-token floor, the expected
runtime and exact runtime version, and an optional zero-data-retention policy.
Work mode implicitly requires tool calls. A requested Runtime without a version
remains `unknown`; a version outside an authoritative `exact_versions` set is
blocked with `runtime-version-not-verified`. Only an exact match may proceed to
the Compatible, Degraded, or Incompatible state. The result is `compatible`,
`blocked`, or `unknown` with per-capability authority and mismatch codes.
Selection is allowed only when the catalog is fresh, signature-validated, and
every required value is known;
offline metadata remains non-selectable, and a present value whose authority is
still `unknown` or `estimated` remains an unknown check.

```jsonl
{"jsonrpc":"2.0","id":"8","method":"model/capability-check","params":{"model_id":"local:deterministic-echo","requirements":{"mode":"work","attachments":["image"],"context_tokens":1024,"runtime":"preview","runtime_version":"0.1.0"}}}
{"jsonrpc":"2.0","id":"8","result":{"schema_version":"model-capability-check/0.1","model_id":"local:deterministic-echo","catalog_state":"offline","decision":"unknown","selection_allowed":false,"checks":[{"capability":"runtime","required":{"adapter":"preview","version":"0.1.0"},"observed":{"schema_version":"model-runtime-compatibility/0.1","adapter_family":"native","adapter":"preview","protocol":"aap-native","exact_versions":["0.1.0"],"state":"unknown","authority":"unknown"},"authority":"unknown","result":"unknown"}],"mismatches":[]}}
```

The internal `model-profile/0.1` contract is metadata-only and has no writable
AAP method. It validates global/project scope and explicit role bindings for
Agent, plan, apply, review, utility, embedding, and rerank. The single-model
default binds only Agent; an unconfigured role is disabled rather than silently
using the default model. The existing private snapshot remains Runtime read
authority and supplies the negotiated read-only `model/profile/list` and
`model/profile/read` projections.

SQLite schema v14 separately persists a bounded event-backed `model_profiles`
projection with revision CAS, monotonic generation/sequence, idempotent retries,
atomic projection/event writes, globally unique active profile IDs, and startup
chain/cursor ownership verification. Every row and lifecycle event fixes model
selection, routing, token issuance, and Turn start
authority to false. Runtime/AAP/Qt do not consume or mutate this SQLite projection,
and it does not replace the snapshot. The derived `model-profile-stream-*`
namespace is reserved for these internal lifecycle streams; ordinary Session create,
portable import, and projection rebuild reject it before writing. Profiles remain unavailable to the Qt picker
until the signed catalog, profile switching, token/routing, immutable change-event,
and cross-platform gates are complete.

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
`permission_granted:false` and `execution_available:false`. Their reusable semantic
validator rejects out-of-contract limits, impossible reservation/source accounting,
inconsistent remaining values, forged classifications, and authority flags. Provider usage and a
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
Workbench schema v12 retains the v11 exact canonical request/state JSON, hashes, schedule
metadata, generation, cancellation, attempts, and recovery ordering. Creation and
generation-CAS updates commit with typed `background-job.*` session events; identical
retries are idempotent, stale writers fail, event failure rolls back the projection,
and startup performs a bounded integrity scan before the store becomes writable.
Active jobs protect session deletion/retention. The v11 foundation additionally persists one
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

Internal `background-job-notification-intent/0.1` defines content-free evidence for
future completion, failure, approval-needed, and budget-exhausted notifications. An
intent binds the exact job request/state identities, job generation/status, project,
session, root, terminal/result/approval evidence, and optional validated
`child-runtime-budget/0.1` snapshot identity plus exhausted dimensions. Budget
snapshots are checked for accounting, remaining-value, classification, provenance,
scope, and authority invariants; generation zero is not rejected merely for being
the initial generation. The deduplication identity deliberately excludes notification
creation time, while the full intent identity includes it.

The record has no title, body, prompt, result body, or platform payload. It fixes
`content_included:false`, `delivery_available:false`, `delivery_attempted:false`,
and `platform_delivery_authority:false`; changing any delivery flag or bound identity
invalidates the record. Workbench schema v12 persists one canonical record per stable
deduplication identity in `background_notification_outbox` and appends the matching
`background-job.notification-recorded/0.1` session event in the same transaction.
The projection binds the exact lifecycle event sequence, request/state/generation,
canonical JSON/hash, creation/record times, and fixed `recorded`/zero-attempt/false-
authority delivery state. Identical retries remain idempotent after the job advances;
conflicting evidence, stale first writes, event failure, projection/event mismatch, or
missing lifecycle evidence fail closed. Startup validates at most 10,000 records, and
session purge removes the job, outbox record, and event atomically. An internal
session-scoped cursor inspection is read-only and ordered by record time descending;
capability `background-notification.outbox.read-only` advertises it only with a healthy
durable store. AAP method `session/background-notifications` accepts one session ID,
an optional structured cursor, and a 1-100 limit. The response binds the session even
when empty and keeps content, delivery mutation, and platform authority false. It is
allowed while a session is archived, pending deletion, reconciliation-blocked, or in
session-level quarantine because it cannot mutate or deliver. Whole-store recovery
does not advertise or serve it because the outbox is not verified there.

```jsonl
{"jsonrpc":"2.0","id":"notification-1","method":"session/background-notifications","params":{"session_id":"session-1","limit":100}}
{"jsonrpc":"2.0","id":"notification-1","result":{"schema_version":"background-notification-page/0.1","session_id":"session-1","notifications":[],"next_cursor":null,"content_included":false,"delivery_mutation_available":false,"platform_delivery_authority":false}}
```

Qt exposes this metadata-only page from the Session context menu, validates the page
schema/session/cursor/kind/state/sequence plus every false-authority flag, and rejects
content-bearing or mismatched records before rendering. No scheduler producer,
delivery retry, operating-system permission request, or macOS/Windows notification
call consumes the outbox yet.

Background recovery inspection is a separate read-only surface. Capability
`background-job.recovery.inspect` is advertised only with writable durable storage;
`session/background-recovery` accepts one session ID, an optional `{job_id,
entry_identity}` cursor, and a 1-100 limit. Runtime rebuilds the bounded internal
`background-job-scheduler/0.2` snapshot without acquiring leases, observing caller
PIDs, dispatching work, or changing job state. The response
`background-recovery-page/0.1` contains only metadata for each job's recovery action,
status, cancellation, schedule, lease/process ownership, blockers, and an optional
matching `background-recovery-review/0.1` journal summary. It fixes
`dispatch_available`, `automatic_retry`, `automatic_approval`,
`automatic_takeover`, and `mutation_authority` to false. Cursor anchors bind the current
entry identity; forged or changed entries fail closed. Qt renders the page from the
Session context menu with an empty state and keyset `加载更多` control; no recovery,
retry, approval, takeover, or delivery action is exposed.

```jsonl
{"jsonrpc":"2.0","id":"recovery-1","method":"session/background-recovery","params":{"session_id":"session-1","limit":100}}
{"jsonrpc":"2.0","id":"recovery-1","result":{"schema_version":"background-recovery-page/0.1","session_id":"session-1","entries":[],"next_cursor":null,"content_included":false,"dispatch_available":false,"automatic_retry":false,"automatic_approval":false,"automatic_takeover":false,"mutation_authority":false}}
```

## Session Workspace Binding And Search

Capabilities `session.workspace-binding.read-only` and `session.search.branch`
negotiate the additive `session-workspace-binding/0.1` projection and exact branch
filter. New Work Sessions capture their registered primary root and read-only Git
state before provider startup. The response includes no repository path and grants
no permission or dedicated-worktree authority.

```jsonl
{"jsonrpc":"2.0","id":"workspace-1","method":"session/search","params":{"project_id":"project-1","branch":"main","limit":100}}
{"jsonrpc":"2.0","id":"workspace-1","result":{"schema_version":"session-search/0.2","sessions":[{"session_id":"session-1","project_id":"project-1","mode":"work","workspace":{"schema_version":"session-workspace-binding/0.1","session_id":"session-1","project_id":"project-1","root_id":"root-1","workspace_kind":"project-root","git_state":"worktree","branch":"main","branch_redacted":false,"head_oid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","detached":false,"unborn":false,"dedicated_worktree":false,"captured_at_ms":1,"raw_paths_included":false,"permission_granted":false},"matched_fields":["branch"]}],"durable":true,"next_cursor":null,"truncated":false,"unavailable_filters":[]}}
```

Branch matching uses the exact SHA-256 identity stored with the Session. A
secret-shaped branch label is never persisted or returned as display text. Resume
re-observes root, branch, HEAD, and worktree identity; drift returns `-32146` and
requires explicit fork/rebind rather than silently changing the Session context.
Schema-v12 Work Sessions are not backfilled from current Git state.

## Replay And Reconnect

The durable session stream is ordered by a session-local sequence. After a
transport loss, the host reads the newest page and then requests older pages with
the strict opaque cursor returned by the runtime. A cursor is exclusive, so a
newer live item cannot duplicate an already rendered item.

```jsonl
{"jsonrpc":"2.0","id":"7","method":"session/read","params":{"session_id":"session-1","limit":100}}
{"jsonrpc":"2.0","id":"7","result":{"session":{"id":"session-1","mode":"chat"},"items":[],"history_page":{"limit":100,"first_sequence":null,"last_sequence":null,"latest_sequence":0,"has_older":false,"older_cursor":null},"runtime":{"adapter":"preview","version":"0.1.0","permission_profile":"read-only","replayed":true},"workspace":null,"environment":{"replayed":true,"available":false}}}
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
- `backend.status: ready`, capability availability, and degradation metadata do
  not grant Agent write, command, approval, network, or background authority.
- Current stdio is not authenticated, encrypted, or peer-verified. Do not present
  it as the authenticated production IPC planned under tasks `4.2` through `4.4`.
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
