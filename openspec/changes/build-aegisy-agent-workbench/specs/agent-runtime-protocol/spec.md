## ADDED Requirements

### Requirement: AAP wire schemas are package-versioned by namespace
The repository SHALL maintain AAP wire schemas in one dedicated package whose
package version is independent from AAP wire versions and provider-adapter
versions. Stable and experimental contracts SHALL have separate package-local
registries with bounded relative paths, explicit compatibility and wire
availability, and canonical Schema identities.

#### Scenario: Stable wire schema is registered
- **WHEN** a stable AAP wire version is added or validated
- **THEN** its namespace registry SHALL bind a unique protocol version, matching version directory, ordinary package-local Schema file, and exact `$id`, and the Schema SHALL compile without referencing the experimental namespace

#### Scenario: Experimental namespace has no enabled contract
- **WHEN** the package is validated while no experimental AAP contract is approved
- **THEN** the experimental registry SHALL remain explicitly empty and wire-unavailable and SHALL NOT advertise, negotiate, or authorize any experimental method or capability

#### Scenario: Experimental contract is promoted
- **WHEN** an experimental contract is proposed for stable use
- **THEN** promotion SHALL require a reviewed new stable wire version with compatibility, authority, recovery, fixture, generated-type, Runtime, and Qt gates rather than reinterpreting an experimental URI as stable

### Requirement: Core domains use strict reusable definitions
The stable AAP package SHALL define Project, Session, Turn, Item, Runtime,
Workspace, Approval acknowledgement, Error, Usage, Artifact-read, and Capability
set contracts as independently compilable reusable definitions. The package
SHALL NOT represent them as one aggregate wire message or grant behavior that no
method currently exposes. Direct wire shapes, method-specific projections, and
typed domain objects SHALL be documented and tested against their real producers.

#### Scenario: Every core definition validates independently
- **WHEN** the stable core component and fixture catalog are validated
- **THEN** each named `$defs` entry SHALL compile and accept its bounded positive fixture while rejecting unknown fields, cross-domain state values, unsafe integers, and authority-bearing substitutions

#### Scenario: Runtime output is checked against core definitions
- **WHEN** the Preview/Store Runtime serves project open/root/list, Chat and Work session start/list/search/read, turn start, and Timeline Item output
- **THEN** the exact direct value or documented field projection SHALL validate against the corresponding core definition, and every Timeline Item SHALL also validate against transport `$defs/timelineItem`

#### Scenario: Chat and Work bindings cannot drift
- **WHEN** Session values or durable projections are validated
- **THEN** Chat SHALL have a null Project and Workspace binding, Work SHALL bind a Project and project-root Workspace, and lineage ownership SHALL remain subject to typed cross-field validation

#### Scenario: Turn lifecycle is distinct from start acknowledgement
- **WHEN** a Turn domain state or `turn/start.result.turn` value is validated
- **THEN** lifecycle SHALL use only `running|completed|failed|interrupted`, the request acknowledgement SHALL use only `started|terminal`, and Session/correlation identities SHALL remain in their owning envelopes rather than being fabricated on Turn

#### Scenario: Workspace Git state fails closed
- **WHEN** a project-root Workspace is encoded
- **THEN** it SHALL use only `unavailable|not-repository|repository-only|worktree`, SHALL expose a lowercase 40- or 64-character HEAD only where valid, SHALL reject contradictory branch/detached/unborn state, and SHALL keep dedicated-worktree, raw-path, and permission authority false

#### Scenario: Approval and Provider Error cannot fabricate authority or content
- **WHEN** an Approval acknowledgement or Provider Error is validated
- **THEN** `allowed`, user-decision, mutation, execution, credential, response-body, message, and unknown-field substitutions SHALL be rejected while the current content-free metadata-only denial/error projection remains valid

#### Scenario: Usage and Capability boundaries remain typed
- **WHEN** Usage evidence or negotiated Capability sets are validated
- **THEN** values and times SHALL stay within JSON-safe integers, metric/value/evidence and reasoning relationships SHALL fail closed on drift, stable capabilities SHALL be unique bounded names, and the AAP `0.1` experimental set SHALL remain exactly empty

#### Scenario: Schema character limits do not replace typed byte limits
- **WHEN** a string, recursive Item value, Usage report, or Workspace projection passes JSON Schema
- **THEN** Rust and Qt SHALL still enforce normative UTF-8 byte, aggregate-tree, identity, arithmetic, and cross-field limits before accepting or emitting it

### Requirement: Runtime connections negotiate versions and capabilities
Every AAP connection SHALL complete an initialization handshake before project,
session, turn, workspace, terminal, tool, runtime-control, or storage methods are
accepted.

#### Scenario: Compatible client initializes
- **WHEN** the client sends a valid numeric minimum, maximum, and preferred protocol range together with bounded identity, platform, stable and experimental capabilities, limits, and transport security state
- **THEN** the runtime SHALL return the selected overlapping version, runtime identity and platform, backend state, supported stable and experimental capability intersection, effective limits, truthful transport security state, and `upgrade_direction: none`

#### Scenario: Older client must upgrade
- **WHEN** the client maximum protocol version is below the runtime minimum
- **THEN** initialization SHALL fail before loading project or session state with bounded `initialize-error/0.1` data and `upgrade_direction: client`

#### Scenario: Older runtime must upgrade
- **WHEN** the client minimum protocol version is above the runtime maximum
- **THEN** initialization SHALL fail before loading project or session state with bounded `initialize-error/0.1` data and `upgrade_direction: runtime`

#### Scenario: Client acknowledges initialization
- **WHEN** the runtime has returned a valid initialize result but has not consumed an exact `initialized` notification with no ID and empty object params
- **THEN** the connection SHALL remain not ready and every business request SHALL fail without loading or mutating project or session state

#### Scenario: Capability sets are negotiated
- **WHEN** the client declares a non-empty stable set, an empty AAP 0.1 experimental set, and capabilities unknown to the runtime
- **THEN** the runtime SHALL return only the supported intersection, omit unknown capabilities, require the applicable backend marker and read-only permission boundary, and SHALL NOT infer availability from a missing declaration

#### Scenario: Optional capability is missing
- **WHEN** an adapter cannot provide a feature such as steering, structured patches, Skills, or child sessions
- **THEN** the runtime SHALL report it unavailable, the UI SHALL disable the dependent action, and the runtime SHALL independently reject the method without simulating success

#### Scenario: Out-of-band method was not negotiated
- **WHEN** cancellation, steering, or terminal stop is received before readiness or without its required negotiated capability
- **THEN** the runtime SHALL reject it through the same readiness and per-method capability boundary instead of bypassing negotiation

#### Scenario: Stdio security is reported truthfully
- **WHEN** the current Qt host connects to its child sidecar over stdio and has completed the one-time bootstrap authentication prelude for that process generation
- **THEN** both peers SHALL report `transport: stdio`, `local: true`, `authenticated: true`, and `encrypted` and `peer_verified` as false, SHALL reject any mismatched declaration, and a sidecar started without a bootstrap token SHALL report `authenticated: false`

#### Scenario: Verified macOS Unix socket security is reported truthfully
- **WHEN** the Qt host explicitly selects the macOS Unix-domain-socket transport, both peers have verified the current UID and exact supervised parent/child PID, and the one-time bootstrap authentication prelude has completed for that process generation before any AAP frame is processed
- **THEN** both peers SHALL report `transport: unix-domain-socket`, `local: true`, `peer_verified: true`, `authenticated: true`, and `encrypted` as false, SHALL reject any mismatched declaration, and SHALL NOT treat peer verification alone as the one-time bootstrap authentication

#### Scenario: Unix socket bytes arrive outside the verified process generation
- **WHEN** socket bytes arrive before peer verification, after disconnect, or from a process generation other than the currently supervised generation
- **THEN** the Qt host SHALL NOT decode or dispatch them as AAP, SHALL clear the peer proof on failure, and SHALL require a new verified connection and complete two-stage handshake

#### Scenario: Verified Windows named-pipe security is reported truthfully
- **WHEN** the Qt host explicitly selects the Windows named-pipe transport, the sidecar creates a first-instance protected current-token-user pipe, both peers verify the exact supervised process generation, and the one-time bootstrap authentication prelude has completed for that process generation before any AAP frame is processed
- **THEN** both peers SHALL report `transport: windows-named-pipe`, `local: true`, `peer_verified: true`, `authenticated: true`, and `encrypted` as false, SHALL reject any mismatched declaration, and SHALL NOT treat peer verification alone as the one-time bootstrap authentication

#### Scenario: Named-pipe bytes arrive outside the verified process generation
- **WHEN** pipe bytes arrive before client/server PID and generation verification, after disconnect, or from a process generation other than the currently supervised generation
- **THEN** the Qt host SHALL NOT decode or dispatch them as AAP, SHALL clear the peer proof on failure, and SHALL require a new first-instance pipe, verified connection, and complete two-stage handshake

#### Scenario: AAP frame reaches the transport limit
- **WHEN** either peer would send or receives a newline-delimited JSON frame larger than the exact AAP 0.1 `max_frame_bytes` value of 4 MiB
- **THEN** it SHALL refuse the write or drain and reject the input with bounded content-free behavior, SHALL NOT allocate an unbounded frame, and SHALL preserve framing for a later valid message when the transport remains usable

#### Scenario: Negotiated connection is lost
- **WHEN** stdio or the verified local socket disconnects, the runtime exits, initialization fails, or either peer rejects malformed protocol input
- **THEN** the client SHALL clear ready state, capabilities, and negotiated limits, SHALL fail pending requests, and SHALL require a complete new two-stage handshake before sending more business requests

#### Scenario: Read-only backend is ready
- **WHEN** a backend reports ready and negotiates `permission.read-only`
- **THEN** that state SHALL NOT grant Agent file writes, commands, approvals, network access, background work, or any other mutation authority, while explicit user editor saves and user terminals remain separately scoped operations

### Requirement: Host and sidecar complete one-time bootstrap authentication
The Qt host SHALL generate a fresh 256-bit cryptographically random bootstrap
token per supervised sidecar process generation, pass it only through the
sanitized launch environment (never through process arguments, ordinary logs,
or AAP frames), and a token-configured sidecar SHALL require the exact one-time
`aegisy-bootstrap-auth/0.1` prelude as the first transport line before any AAP
frame is processed. The prelude is a transport-layer line, not an AAP message;
it carries the token and nothing else.

#### Scenario: Token reaches the sidecar only through the environment
- **WHEN** the Qt host launches a sidecar process generation
- **THEN** it SHALL strip any inherited bootstrap token from the sanitized environment, insert a freshly generated token, and the sidecar SHALL read and immediately remove that variable before spawning any further child process so Codex, terminal, or Git subprocesses never inherit it

#### Scenario: Exact prelude authenticates the connection
- **WHEN** a token-configured sidecar receives the exact `aegisy-bootstrap-auth/0.1` prelude with the matching token as the first transport line
- **THEN** it SHALL consume the token exactly once, erase it from memory, and report `authenticated: true` in the initialize result while the Qt host SHALL declare and require the same fact

#### Scenario: Missing, malformed, mismatched, or replayed prelude fails closed
- **WHEN** the first transport line is absent, malformed, carries a wrong or replayed token, or exceeds the bounded prelude limit
- **THEN** the sidecar SHALL respond with the fixed content-free bootstrap-authentication error and close the connection without constructing runtime, store, or adapter state from client input

#### Scenario: Sidecar without a token keeps legacy unauthenticated mode
- **WHEN** a sidecar starts without the bootstrap token environment variable
- **THEN** it SHALL accept AAP frames without a prelude and report `authenticated: false` so existing fixtures and direct developer launches remain unchanged

#### Scenario: Token never persists or leaks
- **WHEN** the bootstrap token is generated, transmitted, verified, or discarded
- **THEN** neither peer SHALL write it to logs, diagnostics, persistence, crash reports, or process arguments, both peers SHALL erase their in-memory copies after the prelude exchange, and a repeated prelude line after authentication SHALL be handled as ordinary invalid input rather than an authentication path

### Requirement: Work is represented as Project, Session, Turn, and Item events
The protocol SHALL expose typed lifecycle objects and immutable event identity for
all user and Agent activity.

Every live event SHALL use the strict `timeline-event/0.1` envelope. Its positive
JSON-safe Session sequence SHALL be contiguous within the current Runtime stream;
its Runtime-observed Unix-millisecond timestamp SHALL be non-decreasing within that
Session; its correlation ID SHALL equal the bound Turn ID; and its content-hashed
Event ID SHALL bind the immutable envelope fields other than the Event ID itself.
An Item-bearing event SHALL also carry a positive contiguous revision and declare
`snapshot-replacement` content semantics. Runtime restart persistence, subscription,
snapshot, replay, and sequence-gap recovery remain the separate requirement below.
Optional Item data SHALL be a recursively bounded canonical JSON tree with at most
16 levels and 4,096 values, at most 128 properties per object and 4,096 entries per
array, 1-128 byte ASCII graphical property names, and only exact mathematical
integers in the signed JSON-safe range.

The Event identity material SHALL be one compact UTF-8 JSON object without
`event_id`, ordered as `schema_version`, `sequence`, `timestamp_ms`,
`correlation_id`, `session_id`, `turn_id`, `turn_state`, `event`, `item`, and
`item_update`. Item fields SHALL be ordered `id`, `kind`, `role`, `state`,
`content`, then optional `data`; Item-update fields SHALL be ordered `revision`,
then `content_mode`. Nullable Item and Item-update fields SHALL remain literal
`null`; absent optional Item data SHALL remain omitted. Item-data object keys SHALL
sort by ascending UTF-8 bytes at every level, arrays SHALL retain input order,
strings SHALL use compact JSON escaping without Unicode normalization, and invalid
Unicode scalar sequences SHALL be rejected. Every accepted mathematical integer
SHALL serialize in shortest base-10 integer form without an exponent, decimal
point, leading plus, or redundant leading zero, and every representation of zero
SHALL serialize as `0`.

Let `material` be those exact bytes. Event SHA-256 input SHALL be the ASCII domain
`aegisy-timeline-event/0.1` followed by one NUL byte, the material byte length as
one unsigned eight-byte big-endian integer, and `material`. `event_id` SHALL be
`event:sha256:` plus the 64 lowercase hexadecimal digest characters. The transport
wrapper and Event ID itself SHALL NOT enter the digest. Schema validates recursive
types and per-container limits; typed Rust and Qt validation SHALL additionally
enforce the 16-level and 4,096-value aggregate bounds. This boundary SHALL make the
Event identity independently reproducible without numeric precision, key-order, or
nullable-field drift.

#### Scenario: User starts a turn
- **WHEN** a valid turn-start request is accepted
- **THEN** the runtime SHALL return a turn identity, emit a started event, stream ordered item events, and emit exactly one completed, interrupted, or failed terminal event

#### Scenario: Events from two sessions interleave
- **WHEN** live events for multiple Sessions are emitted by the same Runtime
- **THEN** each Session SHALL retain its own contiguous sequence, non-decreasing Runtime observation time, exact Turn correlation, and immutable unique Event IDs without one Session consuming another Session's cursor

#### Scenario: Item streams incremental state
- **WHEN** a message, plan, reasoning summary, command output, patch, or tool result arrives incrementally
- **THEN** the Item SHALL emit `started`, zero or more `delta` revisions, and one `completed` revision with unchanged Session, Turn, Item, kind, and role identity, and each update SHALL contain the complete replacement snapshot needed to reconstruct state without append heuristics

#### Scenario: Atomic item completes without streaming
- **WHEN** a user message or other atomic Item has no incremental phase
- **THEN** it MAY emit one `completed` revision at revision one and SHALL NOT fabricate a started or delta event

#### Scenario: Provider emits an invalid item order
- **WHEN** an adapter observes a delta before start, a duplicate start or completion, an identity drift, an update after completion, or a Turn terminal while an Item is open
- **THEN** the Runtime SHALL reject that provider sequence, retain already accepted partial Items, and close the Turn with one structured `failed` terminal event rather than forwarding the invalid event or terminating the Runtime

#### Scenario: Turn fails after partial output
- **WHEN** a runtime, provider, sandbox, tool, or transport error terminates the turn
- **THEN** the terminal event SHALL retain partial items and a structured error class without marking incomplete mutations successful

#### Scenario: Persistence fails while a turn terminates
- **WHEN** terminal persistence cannot be confirmed
- **THEN** the live Runtime SHALL use the single `failed` terminal state with bounded structured storage metadata and SHALL NOT invent `turn.persistence-failed` or any fourth terminal state

#### Scenario: Runtime clock moves backwards
- **WHEN** the wall clock is lower than the last emitted timestamp for a Session
- **THEN** the next event SHALL retain the previous timestamp so observable Runtime time remains non-decreasing without rewriting provider-observed times stored separately in Item metadata

#### Scenario: Older client receives an unknown event
- **WHEN** an event name is unknown but the envelope is valid, bound to the active Turn, running, and contains no Item update
- **THEN** the client SHALL advance the Session sequence and timestamp cursor, SHALL NOT project product state, and SHALL retain only a bounded content-free diagnostic; malformed or Item-bearing unknown events SHALL be rejected atomically

#### Scenario: Event exceeds the JSON safe integer boundary
- **WHEN** a sequence, timestamp, or Item revision is zero or exceeds `9007199254740991`
- **THEN** the event SHALL be rejected before projection and SHALL NOT advance any Runtime or client cursor

#### Scenario: Item data cannot be canonically represented by every peer
- **WHEN** Item data contains a non-integral number, a mathematical integer outside `[-9007199254740991, 9007199254740991]`, invalid Unicode, or exceeds the recursive structure limits
- **THEN** the event SHALL be rejected before sequence allocation and SHALL NOT enter a live or replay Timeline

### Requirement: Runtime events support reconnect and replay
The runtime SHALL assign a monotonic session sequence and persist replayable
events before acknowledging terminal mutations to the client.

The public replay sequence SHALL be supplied by a dedicated AAP Event Journal.
Internal Workbench projection events include metadata operations outside the public
Timeline and SHALL NOT be exposed directly or used as an accidental replay cursor.

Workbench schema v16 SHALL durably bind each Session cursor to a retention floor
containing the exact pruned-through sequence, Event ID, and Runtime timestamp, and
SHALL maintain exactly one `public-timeline-checkpoint/0.1` projection for that
Session. A nonzero floor SHALL bind a canonical
`event-sequencer-checkpoint/0.1` value containing the Session anchor and only the
Turn/Item lifecycle state required to validate later events. The checkpoint SHALL
NOT contain Item content or Item data. Its identity SHALL be
`event-sequencer-checkpoint:sha256:` followed by exactly 64 lowercase hexadecimal
digits over domain-separated canonical material. One checkpoint SHALL be limited
to 16 MiB, 100,000 Turns, and 100,000 Items.

Advancing the retention floor SHALL validate the existing checkpoint and retained
prefix through the same Sequencer rules used for live events, then replace the
checkpoint, advance the cursor floor, and delete the exact Journal prefix in one
SQLite transaction. Any validation, checkpoint write, floor write, or deletion
failure SHALL preserve the previous checkpoint, floor, cursor head, and Journal.
Startup SHALL validate the checkpoint identity, anchor, lifecycle bounds, cursor,
and contiguous retained tail before restoring the Sequencer and continuing at the
existing head sequence. A schema v15-to-v16 migration SHALL initialize a zero floor
and empty checkpoint projection without fabricating a nonzero checkpoint or
rewriting existing Journal events. Session purge SHALL reset Journal, checkpoint,
and floor in the same deletion transaction. Public Timeline foreign keys SHALL
restrict deletion of a Session projection rather than cascading authority loss.
Pre-recovery validation MAY replay an otherwise complete Journal whose rebuildable
Session projection is temporarily absent, but strict Session ownership SHALL pass
after projection recovery and before the Store becomes writable.

AAP fixed-watermark catch-up SHALL use `timeline/sync`. Its request SHALL contain
exactly a bounded `session_id`, an `after` anchor, a nullable `watermark`, and a
`limit` from 1 through 200. Every anchor SHALL contain exactly `sequence` and
`event_id`. Sequence zero SHALL pair only with a null Event ID; every positive
JSON-safe sequence SHALL pair with the exact lowercase `event:sha256:` identity of
that event. A first request with a null watermark SHALL atomically select the current
Session Journal head. Every response SHALL use `timeline-sync-page/0.1`, repeat the
same Session and after anchor, return the selected fixed watermark, include only
contiguous same-Session events after the anchor and no later than the watermark, and
remain below the AAP frame limit as well as the requested count limit.

An incomplete page SHALL contain at least one event and SHALL return its final exact
sequence/Event-ID pair as `next_after` with `complete:false`. A complete page SHALL
reach the fixed watermark and SHALL return `next_after:null` with `complete:true`.
Every continuation SHALL repeat the same watermark so newly appended live events do
not move the target. The client SHALL validate the complete page, including envelope
identity, Session binding, continuity, after/watermark agreement, final anchor, and
request limit, before advancing its cursor or applying any event.

Runtime SHALL prepare an event against a private Session lifecycle candidate before
serialization or persistence. Preparation SHALL NOT advance the live sequence,
timestamp, Turn, or Item state. Runtime SHALL append the exact serialized envelope to
the public Journal before committing that candidate to the live sequencer; abandoning
or failing a prepared event SHALL leave the live lifecycle state unchanged.

When a public event represents a durable Turn or Item projection, the affected
projection rows, internal projection event, durable Blob reference when present, and
public Journal append SHALL commit in one SQLite transaction. A public Item SHALL
exactly equal the sanitized durable Item projection and SHALL use the same Session,
Turn, kind, role, state, content, non-null data, and projection timestamp. Any
identity mismatch or Journal failure SHALL roll back the complete projection write.

On a live sequence gap, the client SHALL freeze only the affected Session at its last
confirmed sequence/Event-ID anchor, queue later live events within fixed count and
byte bounds, and stage every validated sync page in a private candidate. An
incomplete page SHALL NOT change visible Timeline state. Only a complete page at the
unchanged watermark SHALL atomically publish the staged candidate, after which the
client SHALL drain queued live events through the ordinary validator. A malformed
page, anchor drift, request failure, missing capability, or queue/batch overflow
SHALL preserve the confirmed projection and leave that Session frozen.

Negotiated `timeline.subscription.fixed-watermark` SHALL expose
`timeline/subscribe`, `timeline/subscription-sync`,
`timeline/subscription-snapshot`, and `timeline/subscription-activate`, plus the
subscription-bound live-event and terminal-failure notifications. Runtime SHALL
advertise the capability only when the durable writable Timeline Store is healthy.
Subscribe SHALL return exactly one of `sync-required` with a non-null fixed
watermark or `snapshot-required` with a null watermark; it SHALL NOT return active
inline.

Runtime SHALL bind every recovery page request to one connection-generation-owned
attempt, forbid subscription-ID reuse for the lifetime of that connection, permit at
most one attempt per Session, and capture the durable floor, head, and head timestamp
used by the attempt. Retained Sync/Snapshot recovery units and events after that fixed
head SHALL share one 10,000-unit/64 MiB aggregate connection bound; an event at or
before the fixed head SHALL NOT be republished. Completion, activation, accepted
failure, retirement, and disconnect SHALL release exact accounting once. The first
post-watermark event timestamp, and every later timestamp, SHALL be no earlier than
the durable fixed-head timestamp.

A syntactically valid activation SHALL NOT be treated as recovery evidence. Typed
activation SHALL consume both a private non-serializable structural proof derived
from the complete contiguous Sync page chain or complete fixed-header Snapshot page
chain and the matching private connection-owned registry token. The activation
source, Session, connection generation, subscription, cursor, watermark, and optional
Snapshot identity SHALL exactly equal that proof and token. Runtime SHALL emit the
exact active response before draining buffered events in sequence as
`timeline/subscription-event` notifications. Subscribe, Sync, Snapshot, activate,
and live failures SHALL be terminal and cleanup-required. Request-stage failures
SHALL carry a domain-separated identity of the complete exact request, including
Snapshot identity, continuation cursor, and page limit; live failure SHALL bind the
exact active cursor. Every accepted failure SHALL retire only its bound attempt, and
disconnect SHALL retire the complete registry and buffered state. A Sync, Snapshot,
or Activate request bound to another Session or connection generation SHALL be
rejected without consuming the true owner's recovery proof/token or retiring that
owner's attempt.

After subscription negotiation, Runtime SHALL NOT fall back to an unbound bare
`event` notification. A live event received before the exact activation response,
from another generation, from another subscription, or for another Session SHALL be
inert and SHALL NOT change the confirmed client projection. An ordinary successful
`session/read` SHALL NOT replace an already active subscription; an actual sequence
gap on that active subscription SHALL still freeze the Session and begin recovery.
`turn/start` SHALL fail with `-32152` before mutation unless its Session owns a
current connection-bound subscription attempt. Events produced while that attempt is
recovering SHALL remain bounded and buffered; the Qt product SHALL still wait for
Active before enabling Send. A retryable typed failure SHALL preserve the confirmed
projection and queued prompt, allocate a fresh subscription ID, and retry no more
than three times using bounded 0/250/1000 ms delays.

#### Scenario: Activation has only well-formed but unverified recovery metadata
- **WHEN** activation carries a valid-looking cursor, watermark, or Snapshot identity without the complete matching recovery proof
- **THEN** activation SHALL be rejected and the subscription SHALL NOT become active

#### Scenario: Subscription failure changes stage or binding
- **WHEN** a terminal failure changes its stage, connection generation, Session, subscription, cursor, or fixed watermark
- **THEN** it SHALL NOT retire or advance the matching attempt and the malformed traffic SHALL fail closed

#### Scenario: Heartbeat expires with ambiguous subscription ownership
- **WHEN** the heartbeat deadline expires while subscribe, subscription-sync, subscription-snapshot, or activate remains pending
- **THEN** the client SHALL retire the pending request, abandon the current connection, and start one bounded fresh process generation instead of retrying on the ownership-ambiguous connection

#### Scenario: Client cannot verify subscription state locally
- **WHEN** a subscribe, subscription-sync, subscription-snapshot, or activate result is invalid, an Active wrapper/cursor drifts, continuation is unsafe, or Runtime reports `session-attempt-exists` or `subscription-id-reused`
- **THEN** the client SHALL freeze confirmed state, preserve queued input, abandon the current connection, and start one bounded fresh Runtime generation without completing reconnect or retrying that ownership conflict on the same connection

#### Scenario: Active Session is read normally
- **WHEN** `session/read` succeeds for a Session whose subscription is already active
- **THEN** the client SHALL preserve the active generation, subscription identity, cursor, and live state without starting another subscription attempt

#### Scenario: Turn has no subscription ownership
- **WHEN** a negotiated client starts a Turn for a Session with no current subscription attempt
- **THEN** Runtime SHALL return `-32152` before creating the Turn or advancing the durable Timeline

#### Scenario: Retryable subscription stage fails
- **WHEN** a correctly bound subscription failure is retryable and the connection remains healthy
- **THEN** Qt SHALL preserve the confirmed projection and queued prompt and MAY retry only with a fresh subscription ID under the bounded three-attempt schedule

When the requested `after` anchor is strictly older than the validated durable
retention floor, Runtime SHALL return JSON-RPC error `-32148` with exact
`timeline-retention-gap/0.1` data. The data SHALL bind the Session, requested after
and optional watermark, retained floor, current head, required snapshot capability
and method, snapshot availability, incomplete event-history state, and the explicit
prohibition on replaying from the floor. It SHALL contain no Item content, checkpoint
content, checkpoint identity, path, provider data, or credential. A retained
sequence with a substituted Event ID SHALL remain anchor or watermark drift and
SHALL NOT be reported as a retention gap.

Until `timeline.snapshot.current` is negotiated and the complete current-Session
snapshot is atomically installed, the affected Session SHALL remain frozen. The
client SHALL NOT discard its confirmed projection, clear bounded queued live events,
or resume at the floor merely because the gap response was structurally valid.
Other Sessions SHALL continue independently.

The current-Session snapshot capability SHALL be exactly
`timeline.snapshot.current` and SHALL expose only `timeline/snapshot`. Runtime SHALL
advertise it only when the durable floor-visible-state projection, retained Public
Timeline tail, and snapshot materialization path are healthy. A retention-gap
response SHALL set `snapshot_available` to true exactly when the current connection
negotiated that capability, and to false otherwise. A client that receives false
SHALL keep the Session frozen and SHALL NOT call the snapshot method.

The `timeline/snapshot` request SHALL contain exactly `session_id`, nullable
`snapshot_identity`, nullable `watermark`, nullable `after`, and `limit`. `limit`
SHALL be 1 through 200. The first request SHALL set the three nullable fields to
null. Runtime SHALL atomically capture the current Public Timeline head as one fixed
sequence/Event-ID watermark and SHALL materialize the Session's visible state only
through that watermark. Every continuation SHALL repeat the exact returned
`snapshot_identity` and watermark and SHALL use only the exact Runtime-issued
`after` cursor. A continuation mismatch, expired materialization, or unavailable
fixed head SHALL fail without silently selecting a new snapshot.

Every response SHALL use `timeline-session-snapshot-page/0.1` and SHALL contain
exactly `schema_version`, `session_id`, `snapshot_identity`, `floor`, `watermark`,
`active_turn`, `total_items`, `total_canonical_bytes`, `after`, `items`,
`next_after`, `complete`, and `page_identity`. `floor` and `watermark` SHALL be exact
sequence/Event-ID anchors. `active_turn` SHALL be null or an exact closed object
containing exactly `turn_id`, matching `correlation_id`, `state:"running"`,
`started_event`, `latest_event`, and ordered unique `open_item_ids`. Both Event
anchors SHALL come from the validated Public Timeline, and the open Item list SHALL
exactly match the current visible `started`/`delta` Items for that Turn. Runtime SHALL
NOT claim an active Turn from provider/process state outside the validated Public
Timeline.

Each ordered snapshot Item SHALL contain exactly `ordinal`, `item_identity`,
`turn_id`, `correlation_id`, `turn_state`, `first_event`, `latest_event`, `item`, and
`item_update`. Ordinals SHALL be contiguous positive JSON-safe integers starting at
one. The two Event anchors SHALL bind the Item's first visible appearance and latest
accepted revision at the fixed watermark. `item` SHALL be the complete current
sanitized visible Item, including a current `started` or `delta` state when the fixed
head falls inside a running Turn. `item_update` SHALL contain the exact current
positive revision and `snapshot-replacement` content mode. The Turn/correlation
binding, anchors, kind, role, Item ID, and revision SHALL NOT drift between the floor
state, retained-tail reduction, pages, or later live validation.

An Item identity SHALL have the form
`timeline-session-snapshot-item:sha256:<64-lowercase-hex>` and SHALL hash
domain-separated canonical JSON containing the schema, Session, ordinal,
Turn/correlation binding, Turn state, first/latest Event anchors, complete Item, and
item update, excluding only the identity field itself. The complete snapshot identity SHALL have the form
`timeline-session-snapshot:sha256:<64-lowercase-hex>` and SHALL hash domain-separated
canonical JSON containing the schema, Session, exact floor, fixed watermark,
nullable active Turn, total Item count, total canonical Item bytes, and the complete
ordered Item-identity list. The page identity SHALL have the form
`timeline-session-snapshot-page:sha256:<64-lowercase-hex>` and SHALL bind that
snapshot identity, exact request cursor, ordered Item identities returned by the
page, next cursor, and completion state. No identity SHALL include transport request
IDs or mutable provider/process metadata.

The nullable item cursor SHALL otherwise contain exactly the final returned
`ordinal`, `item_id`, and `item_identity`. An incomplete page SHALL contain at least
one Item, return that exact final cursor as `next_after`, and set `complete:false`.
A complete page SHALL return `next_after:null` and set `complete:true`. Every page
SHALL remain below the 4 MiB AAP frame limit and MAY stop before the requested Item
count to preserve that bound. One complete materialization SHALL contain at most
10,000 Items and at most 64 MiB of canonical Item material. Runtime SHALL reject an
over-bound Session before returning a usable first page and SHALL NOT truncate,
prune, or label a partial set complete.

The durable source for that materialization SHALL be a sanitized visible-state
snapshot at the exact retention-floor anchor plus the contiguous retained Public
Timeline tail strictly after the floor and no later than the fixed watermark.
`session/read` SHALL NOT substitute for this source. Advancing the retention floor
SHALL validate and commit the replacement visible-state floor snapshot, matching
content-free Sequencer checkpoint, exact floor, and exact prefix deletion in one
SQLite transaction. Any visible-state, checkpoint, floor, or deletion failure SHALL
preserve the complete previous floor authority and Journal. Both the durable floor
state and materialized current snapshot SHALL obey the 10,000-Item/64-MiB bounds;
an over-bound state SHALL make pruning and snapshot recovery unavailable rather than
discarding visible history.

Qt SHALL preserve the affected Session's last confirmed projection while it stages
every snapshot page privately. It SHALL verify the repeated Session, floor,
watermark, active Turn, totals, snapshot identity, page identity, contiguous
ordinals, every Item identity, cursor chain, and complete ordered identity before
changing visible state. Only the final complete page SHALL atomically replace that
one Session's Timeline, active-Turn/Item lifecycle state, and confirmed cursor at the
fixed watermark. Qt SHALL then drain only queued live events after that watermark
through the ordinary event validator and SHALL discard delayed notifications at or
below the watermark. Snapshot state carries no timestamp baseline; the first valid
post-watermark live event SHALL establish the new client timestamp baseline while
Runtime continues to guarantee non-decreasing event timestamps. A malformed,
missing, expired, over-bound, or
identity-drifted page SHALL leave the previous projection visible and the Session
frozen; it SHALL NOT partially append snapshot Items or affect another Session.
Transport loss SHALL discard private staging and release its bounded accounting while
preserving the confirmed projection and snapshot-recovery intent. After a fresh
handshake negotiates the capability again, Qt SHALL restart from a null first-page
request rather than continuing an abandoned cursor.

The fixed-watermark snapshot slice does not satisfy the complete reconnect
requirement by itself. The schema v16 floor/checkpoint remains internal retention and
startup authority; schema v17 separately persists the public floor-visible state.
Automatic production pruning SHALL remain unreachable until a later reviewed stage
explicitly enables it. The later negotiated live-subscription and reconnect scenarios
satisfy those portions; durable Turn-start acknowledgement is implemented separately,
while complete Windows recovery evidence SHALL remain required before this requirement
is considered complete.

#### Scenario: Prepared event persistence fails
- **WHEN** Runtime prepares and serializes an event but its Journal or combined projection transaction fails
- **THEN** Runtime SHALL emit no event, SHALL advance no Session or Item lifecycle cursor, and SHALL leave no partial durable projection or Journal row

#### Scenario: Preview lifecycle commits atomically
- **WHEN** Runtime completes a synthetic Preview Turn whose lifecycle contains Turn start, user Item completion, Agent Item start/delta/completion, and Turn completion
- **THEN** Runtime SHALL commit the Turn projection, both sanitized Item projections, all internal projection events, terminal state, and all six Public Journal events in one transaction before advancing the live Sequencer or in-memory Timeline

#### Scenario: Failed-turn compensation commits atomically
- **WHEN** adapter, transport, protocol, or persistence failure requires a durable failed Turn after the Turn has started
- **THEN** Runtime SHALL commit the exact Error Item, Turn Trace, failed terminal projection, internal trace and terminal events, and public failed terminal event in one transaction before notification, and SHALL reject a later attempt to repair an existing Trace with a new public terminal

#### Scenario: Sanitization changes a durable Item
- **WHEN** durable Item admission redacts or otherwise sanitizes the requested payload
- **THEN** the public event and replay Journal SHALL contain exactly that sanitized persisted Item, and a raw or independently reconstructed Item SHALL be rejected before transaction commit

#### Scenario: Client stages multiple recovery pages
- **WHEN** a gapped Session receives one or more valid incomplete pages followed by a valid complete page at the fixed watermark
- **THEN** no incomplete page SHALL become visible, the complete staged projection SHALL become visible once, and queued live events SHALL then be validated and applied in sequence

#### Scenario: One Session recovery fails
- **WHEN** a sync page, anchor, queue, or request fails for one Session while another Session receives valid contiguous live events
- **THEN** only the affected Session SHALL remain frozen and the independent Session SHALL continue advancing

#### Scenario: Client starts fixed-watermark catch-up
- **WHEN** the client requests `timeline/sync` with `{sequence: 0, event_id: null}` and a null watermark
- **THEN** the runtime SHALL return the current Journal head as one fixed sequence/Event-ID watermark and SHALL NOT include an event beyond that anchor

#### Scenario: Catch-up spans multiple pages while live events append
- **WHEN** a page is incomplete and newer events append after its fixed watermark
- **THEN** the client SHALL request the returned `next_after` with the unchanged watermark, and the runtime SHALL continue only toward that original watermark without duplicates or a moving target

#### Scenario: Empty Journal is already caught up
- **WHEN** the Session Journal head is sequence zero
- **THEN** the runtime SHALL return an empty complete page whose after and watermark are both `{sequence: 0, event_id: null}` and whose `next_after` is null

#### Scenario: Replay anchor or page identity is forged
- **WHEN** zero is paired with an Event ID, a positive sequence has a null or malformed Event ID, equal sequences carry different IDs, a page changes Session/after/watermark, events gap or cross Session, or `next_after` does not identify the final event
- **THEN** the peer SHALL reject the complete request or page before cursor advancement or Timeline projection

#### Scenario: Requested replay point is no longer retained
- **WHEN** a valid `timeline/sync` request names an after anchor strictly before the durable retention floor
- **THEN** Runtime SHALL return `-32148` with exact content-free floor/head and snapshot-recovery metadata, and the client SHALL keep only that Session frozen without replaying from the floor

#### Scenario: Snapshot-capable client reaches a retention gap
- **WHEN** the current connection negotiated `timeline.snapshot.current` and a valid sync request is strictly before the retained floor
- **THEN** Runtime SHALL report `snapshot_available:true` and Qt SHALL begin a null first-page snapshot while keeping its confirmed projection visible

#### Scenario: Older client reaches a retention gap
- **WHEN** the current connection did not negotiate `timeline.snapshot.current` and a valid sync request is strictly before the retained floor
- **THEN** Runtime SHALL report `snapshot_available:false` and the client SHALL keep the Session frozen without calling `timeline/snapshot`

#### Scenario: Retained replay identity is forged
- **WHEN** an after or fixed-watermark sequence is still retained but its Event ID is substituted
- **THEN** Runtime SHALL return the ordinary anchor/watermark drift failure without `timeline-retention-gap/0.1` data

#### Scenario: Internal retention advances atomically
- **WHEN** maintenance checkpoints a validated Session prefix and prunes through its exact sequence/Event-ID/timestamp anchor
- **THEN** the checkpoint projection, cursor floor, and Journal deletion SHALL commit together, and a failure at any step SHALL retain the complete previous state

#### Scenario: Runtime restarts after an internal prefix prune
- **WHEN** a Session has a validated nonzero checkpoint and a contiguous retained Journal tail
- **THEN** startup SHALL restore Turn/Item lifecycle state from the content-free checkpoint, replay the retained tail, and allocate the next event after the durable head without reusing a sequence

#### Scenario: Existing v15 Journal migrates to schema v16
- **WHEN** migration opens a populated schema v15 Public Journal
- **THEN** it SHALL preserve every existing event and head anchor, initialize the retention floor at zero, and SHALL NOT fabricate a nonzero lifecycle checkpoint

#### Scenario: Session with retained history is purged
- **WHEN** Session deletion commits after its Public Journal has a nonzero floor
- **THEN** the Journal rows, checkpoint projection, and floor SHALL reset or delete atomically so no purged conversation content remains replay-readable

#### Scenario: Rebuildable Session projection is temporarily missing
- **WHEN** a complete Public Timeline checkpoint and retained tail exist but the rebuildable Session projection row is absent
- **THEN** foreign-key behavior SHALL NOT delete the Timeline authority, startup SHALL validate and replay it without exposing public sync, Session recovery SHALL rebuild the owner, and final strict ownership validation SHALL pass before writes are enabled

#### Scenario: Client reconnects after losing transport
- **WHEN** the client supplies its last acknowledged sequence
- **THEN** the runtime SHALL replay later persisted events and then continue live streaming without duplicate effects

#### Scenario: Requested replay point is no longer retained
- **WHEN** the event sequence predates retained replay data
- **THEN** the runtime SHALL return structured `-32148`; only a separately negotiated identity-complete `timeline/snapshot` result MAY let the client atomically replace that Session at a fixed watermark before continuing live delivery

The schema-v17 floor-visible projection, fixed-head Runtime paging, Qt atomic
replacement, negotiated fixed-watermark subscription, and schema-v20 Turn-start
acknowledgement form the current recovery chain. Complete Windows reconnect/runtime
evidence remains required to finish this requirement.

#### Scenario: Snapshot pages stay fixed while a Turn continues
- **WHEN** Runtime captures a snapshot watermark during a running Turn and later Item deltas or a terminal event append while Qt pages
- **THEN** every page SHALL retain the original watermark and active running-Turn state, the later events SHALL remain outside the snapshot, and Qt SHALL validate them only after atomic replacement

#### Scenario: Snapshot materializes floor state plus retained tail
- **WHEN** the durable floor snapshot contains visible Items and retained events update or complete them before the selected watermark
- **THEN** Runtime SHALL reduce the exact contiguous tail over the floor state and SHALL identify the resulting complete ordered Item state without exposing the internal Sequencer checkpoint

#### Scenario: Snapshot identity or cursor is substituted
- **WHEN** a continuation changes the Session, floor, watermark, snapshot identity, active Turn, totals, Item/page identity, ordinal, or cursor chain
- **THEN** Runtime or Qt SHALL reject the snapshot before visible replacement and SHALL keep only the affected Session frozen

#### Scenario: Snapshot exceeds its complete-state bound
- **WHEN** materialization would contain more than 10,000 Items or more than 64 MiB of canonical Item material
- **THEN** Runtime SHALL fail before returning a usable first page, SHALL NOT truncate or prune the state, and Qt SHALL preserve its prior confirmed projection

#### Scenario: Final snapshot page replaces one Session atomically
- **WHEN** Qt validates all pages, exact totals, ordered Item identities, active running Turn, and the complete snapshot identity at the fixed watermark
- **THEN** Qt SHALL replace only that Session's visible Timeline and lifecycle cursor once, then SHALL drain queued post-watermark events through normal validation

#### Scenario: Transport disconnects during snapshot paging
- **WHEN** transport is lost after one or more valid incomplete snapshot pages
- **THEN** Qt SHALL discard private staging, release its pending bounds, preserve the prior confirmed UI and recovery-required state, and restart from a null first page only after a fresh handshake negotiates the capability

### Requirement: Mutating protocol requests are idempotent
The protocol SHALL require or accept client-generated idempotency keys for turn
starts, approval responses, file writes, Git mutations, and job submission. The
currently advertised mutation producer is limited to `turn/start`; approval, file,
Git, and job producers remain unavailable until they have the same durable ledger and
authority review.

For `turn/start`, Runtime SHALL reserve a schema-v20 durable operation before
dispatch. The operation identity SHALL be derived from Session, mutation kind,
idempotency key, and request fingerprint, and SHALL not include process generation.
The response SHALL expose the request fingerprint, connection acknowledgement, and
metadata-only durable operation. The operation SHALL contain no prompt, context,
provider body, result content, permission, approval, or execution authority.

The implemented schema-v22 non-Turn source-record slice SHALL remain crate-internal Store
behavior. Its accepted source SHALL be exactly one complete typed
`approval-acknowledgement/0.1`, `file-write-acknowledgement/0.1`,
`git-mutation-acknowledgement/0.1`, or `background-job-request/0.1` value. The Store
SHALL derive the existing lossy reservation draft from that source and SHALL NOT
reconstruct a source from a draft or accept independently supplied source and draft
values. This slice SHALL introduce no AAP capability or method.

The implemented schema-v23 terminal-outcome slice SHALL extend only that internal
Store graph. `mutation-reservation-record/0.3` SHALL permit a validated `present`
reservation to move from `reserved` revision 1 to `terminal` revision 2 and SHALL
bind exactly one immutable `mutation-reservation-outcome-record/0.1` plus one
metadata-only internal `mutation.reservation-outcome-recorded` Session event. The
outcome contract SHALL remain limited to the exact terminal approval, file-write,
Git-mutation, or background-job value validated against the complete source. This
slice SHALL introduce no AAP capability, method, producer, consumer, or authority.

#### Scenario: Client retries after timeout
- **WHEN** the runtime receives the same key and equivalent request again
- **THEN** it SHALL return the original operation identity and Turn identity without dispatching a second Turn

#### Scenario: Reservation precedes dispatch
- **WHEN** a valid `turn/start` request is admitted
- **THEN** Runtime SHALL commit the accepted durable operation before dispatch, and a reservation or dispatch failure SHALL leave no fabricated accepted Turn

#### Scenario: Key is reused with different content
- **WHEN** an existing idempotency key accompanies a non-equivalent request
- **THEN** the runtime SHALL reject it as a conflict and SHALL NOT apply either a second or merged mutation

#### Scenario: Typed non-Turn source derives its reservation
- **WHEN** the internal Store admits one of the four supported complete typed sources
- **THEN** it SHALL validate and persist the exact canonical source, SHALL derive the reservation draft itself, and SHALL mark the new reservation provenance exactly `present`

#### Scenario: Non-Turn source graph commits atomically
- **WHEN** a new valid Session/kind/idempotency tuple is admitted
- **THEN** the source record, derived reservation, and metadata-only internal Session event `mutation.reservation-source-recorded` SHALL commit in one `IMMEDIATE` transaction, and the event SHALL NOT enter the Public Timeline Journal or advance its sequence

#### Scenario: Exact non-Turn source is retried
- **WHEN** the same complete canonical source is retried for an existing tuple, including while low-space admission blocks new writes or a competing writer owns the SQLite write lock
- **THEN** the Store SHALL return the original source/reservation/event graph with zero writes and SHALL NOT advance the internal Session sequence

#### Scenario: Source drift is hidden by the lossy draft
- **WHEN** the same tuple is retried with any complete-source drift even though both sources derive the same reservation draft
- **THEN** the Store SHALL reject the retry as an idempotency conflict and SHALL NOT replace, merge, or append any graph member

#### Scenario: Non-Turn terminal outcome commits atomically
- **WHEN** a valid `present` reservation at `reserved` revision 1 records its exact terminal outcome with expected revision 1
- **THEN** the internal outcome event, immutable outcome row, reservation state/revision compare-and-swap, complete terminal-graph validation, and final commit SHALL share one `IMMEDIATE` transaction

#### Scenario: Exact non-Turn outcome is retried
- **WHEN** the same terminal outcome is retried for the same Session and reservation, including after sampled low-space admission would block a new write
- **THEN** the Store SHALL validate and return the original source/reservation/outcome/event graph with zero writes, SHALL retain its original observed/recorded times regardless of the retry attempt's `recorded_at_ms`, and SHALL NOT advance the internal Session sequence

#### Scenario: Peer commits while outcome admission is pending
- **WHEN** a deferred retry observation finds no outcome and a competing writer commits before the caller acquires the `IMMEDIATE` write lock
- **THEN** the caller SHALL reclassify the graph under that lock, SHALL return the peer graph for the exact same outcome, and SHALL report a stable conflict for a different outcome without appending a third lifecycle event

#### Scenario: Terminal outcome admission scope changes
- **WHEN** Session ownership or archive state, pending deletion, project/root/Turn scope, source/kind binding, expected revision, or recording time is no longer valid under the outcome write lock
- **THEN** the Store SHALL reject the outcome without changing the reservation, outcome table, lifecycle history, or internal Session sequence

#### Scenario: Reconciliation or legacy reservation receives an outcome
- **WHEN** an outcome targets a `reconciliation-required` reservation or a reservation with provenance `legacy-unavailable`
- **THEN** the Store SHALL reject it and SHALL NOT infer a source, terminal result, caller authority, or recovery decision

#### Scenario: Non-Turn graph persistence fails
- **WHEN** source or outcome insertion, reservation insertion or revision CAS, event append, sequence update, graph validation, or final commit fails
- **THEN** the transaction SHALL leave no partial source, reservation, outcome, or event row and SHALL leave the Session sequence unchanged

#### Scenario: Startup finds a present reserved non-Turn graph
- **WHEN** startup has first validated the complete source/reservation/event graph and finds a `present` reservation in `reserved` revision 1
- **THEN** it SHALL atomically move the reservation to `reconciliation-required` revision 2 and append exactly one metadata-only internal `mutation.reservation-reconciliation-required` Session event before refusing a retry

#### Scenario: Reconciled non-Turn graph restarts again
- **WHEN** a second or later startup finds the already validated `present` reservation at `reconciliation-required` revision 2 with its exact reconciliation event
- **THEN** startup SHALL perform zero graph writes and SHALL NOT advance the Session sequence

#### Scenario: Startup scan races a reservation writer
- **WHEN** startup scans an empty or non-empty set of open non-Turn reservations while another connection attempts to reserve a new source graph
- **THEN** candidate capture, graph validation, reconciliation, and the final no-open-row assertion SHALL share one `IMMEDIATE` transaction, so the writer SHALL linearize wholly before or after that reconciliation boundary

#### Scenario: Non-Turn lifecycle events are reordered or extended
- **WHEN** a `present` reserved graph has history other than exactly `[source]`, a `present` terminal graph has history other than exactly `[source, outcome]`, a `present` reconciliation-required graph has history other than exactly `[source, reconciliation]`, or a `legacy-unavailable` graph has history other than exactly `[]`, including any unknown same-operation event
- **THEN** startup and read validation SHALL reject the graph and SHALL NOT make the Store writable

#### Scenario: Schema-v22 graph migrates to v23
- **WHEN** migration encounters the exact validated v22 schema and a complete valid source/reservation/event graph
- **THEN** it SHALL publish the reviewed migration backup, copy the source and reservation with record schema `0.3`, create the v23 outcome table/index/Trigger, fabricate no outcome row or outcome event, validate the complete v23 schema and graph, and only then commit `user_version = 23`

#### Scenario: Schema-v22 graph is invalid before v23 migration
- **WHEN** the v22 schema identity, row bound, canonical source/reservation bytes, redundant binding, authority field, lifecycle history, event namespace, or semantic graph is invalid
- **THEN** migration SHALL roll back without retaining v23 objects, changing `user_version`, or replacing the migration source

#### Scenario: Schema-v21 reservation migrates to v23
- **WHEN** migration encounters a valid v21 reservation that never stored its complete source or an internal source event
- **THEN** it SHALL preserve the reservation at record schema `0.3` as provenance `legacy-unavailable` and `reconciliation-required` revision 2 with exactly empty lifecycle history `[]`, without fabricating a source record, source-recorded event, outcome row/event, or reconciliation event

#### Scenario: Schema-v21 reservation data is invalid or over its migration bound
- **WHEN** the v21 `mutation_reservation_records` table contains more than 10,000 rows or any row fails canonical draft, redundant binding, scope, lifecycle, time, or hash validation
- **THEN** migration SHALL fail before copying a row, SHALL preserve `user_version = 21` and the original rows, and SHALL retain no v23 reservation, source, or outcome objects

#### Scenario: Pre-v22 history occupies the reservation namespace
- **WHEN** any supported pre-v22 schema contains a reserved mutation-reservation event kind, operation-ID prefix, or Event-ID prefix, or has a Trigger attached to `events` or `session_sequences`
- **THEN** migration SHALL roll back without reinterpreting that history, executing the Trigger, advancing `user_version`, or retaining v23 reservation objects

#### Scenario: Schema version changes before the migration lock
- **WHEN** another connection advances the database to v23 after the caller's initial version observation but before its `IMMEDIATE` migration transaction
- **THEN** the caller SHALL recheck `user_version` under the transaction, retry against the completed v23 schema, and SHALL NOT apply the stale migration branch

#### Scenario: Schema version drifts to another legacy version before the migration lock
- **WHEN** the caller observes one pre-v23 version but the locked database reports a different pre-v23 version
- **THEN** migration SHALL fail with a stable schema-version-change diagnostic before backup, schema copy, or `user_version` advancement

#### Scenario: Migration source application identity changes before the lock
- **WHEN** the locked pre-v23 database has an `application_id` other than zero or the Aegisy Workbench application ID
- **THEN** migration SHALL fail with a stable source-identity diagnostic before backup or schema mutation

#### Scenario: Current schema opens while another writer holds the database
- **WHEN** the initial observation is already schema v23 while another connection owns a write transaction
- **THEN** open SHALL perform only the database file-identity check for migration purposes and SHALL NOT attempt to acquire a migration write lock or publish a migration backup

#### Scenario: Database path is replaced during migration backup
- **WHEN** the canonical database path changes after the migration and backup connections were opened, including after the backup source snapshot was validated
- **THEN** the backup SHALL read only the pre-opened source snapshot, migration SHALL detect the file-identity drift before becoming writable, and the schema transaction SHALL NOT commit

#### Scenario: Present provenance graph is missing or tampered
- **WHEN** startup or read validation finds missing, duplicate, orphaned, hash-drifted, anchor-drifted, authority-drifted, or semantically altered source, outcome, or event data for a `present` reservation
- **THEN** it SHALL treat the graph as corruption and SHALL NOT infer `legacy-unavailable`, perform reconciliation updates, or make the Store writable

#### Scenario: Shared event-write path has an unexpected Trigger
- **WHEN** the v23 schema inventory finds any Trigger attached to `events` or `session_sequences`, or any unrecognized table, index, Trigger, or auto-index attached to the reservation source/outcome graph
- **THEN** Store startup or migration SHALL fail closed before reconciliation or a source graph write can execute that object

#### Scenario: Session purge owns terminal outcome dependencies
- **WHEN** an owning Session is purged after its reviewed deletion boundary
- **THEN** the Store SHALL remove outcome rows before source and reservation rows and their internal events in the same deletion transaction, without leaving an orphaned foreign-key or lifecycle anchor

#### Scenario: Non-Turn source records confer no authority
- **WHEN** a v23 source, reservation, outcome, or internal event is created, replayed, migrated, reconciled, read, or purged
- **THEN** permission, mutation, approval, execution, and dispatch authority SHALL remain false, and no production producer, genuine user Approval, consume/external-caller-CAS route, filesystem write, Git mutation, or background submission SHALL be inferred

#### Scenario: Accepted and terminal Timeline evidence bind atomically
- **WHEN** the reserved Turn-start produces its `turn.started` or terminal Timeline event
- **THEN** Runtime SHALL bind the exact Session, Turn, sequence, Event ID, timestamp, and operation revision in the same SQLite transaction as the corresponding projection and Journal row using compare-and-swap

#### Scenario: Startup finds an uncertain accepted operation
- **WHEN** Store startup finds an accepted Turn-start whose dispatch outcome is not durably proven
- **THEN** it SHALL advance that operation to `reconciliation-required`, SHALL expose it for Session-scoped recovery, and SHALL refuse redispatch

#### Scenario: Client lists pending durable operations
- **WHEN** a client calls `session/mutation-acknowledgements`
- **THEN** Runtime SHALL return only unconsumed operations for that Session in bounded operation-identity order with a validated continuation cursor; another Session's rows SHALL be rejected

#### Scenario: Client consumes confirmed evidence
- **WHEN** Qt has validated the exact bound Timeline anchor and calls `mutation/acknowledgement/consume`
- **THEN** Runtime SHALL require Session ownership, operation identity, target phase, expected revision, and exact sequence/Event-ID anchor; accepted evidence SHALL be consumed before terminal evidence and a repeated equivalent consume SHALL be idempotent

#### Scenario: Evidence cannot be confirmed
- **WHEN** an anchor drifts, a row is malformed or tampered, Store is unavailable/read-only, or reconciliation is required
- **THEN** consumption SHALL fail closed, the affected Session SHALL freeze for reconciliation, and the client SHALL not infer mutation success

### Requirement: Turns can be cancelled and conditionally steered
The protocol SHALL support cancellation and SHALL advertise whether the active
turn accepts additional user steering.

#### Scenario: User cancels an active turn
- **WHEN** the client sends cancellation for the active turn
- **THEN** the runtime SHALL stop new model and tool work, terminate or detach commands according to policy, resolve pending approvals, and emit an interrupted terminal event

#### Scenario: User steers a supported turn
- **WHEN** the active adapter advertises steering and the turn kind permits it
- **THEN** the input SHALL be recorded as a typed event and applied to that turn without creating an untracked parallel turn

#### Scenario: Turn cannot be steered
- **WHEN** a review, compaction, approval, or adapter limitation makes steering invalid
- **THEN** the request SHALL be rejected with a specific reason and SHALL NOT be silently queued as a new turn

### Requirement: Runtime overload and transport failure are bounded
The runtime SHALL use bounded queues, content limits, heartbeat, and structured
retry guidance.

#### Scenario: Ingress queue is saturated
- **WHEN** the runtime cannot accept more requests safely
- **THEN** it SHALL return a retryable overload error with backoff guidance instead of consuming unbounded memory

#### Scenario: Item payload exceeds inline limit
- **WHEN** command output, diff, image, or artifact exceeds the negotiated inline size
- **THEN** the event SHALL contain a bounded preview and authenticated content reference with size and hash

#### Scenario: Runtime heartbeat expires
- **WHEN** the client misses the configured heartbeat window
- **THEN** it SHALL mark connection state unknown, stop sending new mutations, and attempt bounded reconnection before offering runtime restart

#### Scenario: Heartbeat remains reachable during a long request
- **WHEN** a Turn occupies the ordinary Runtime dispatcher or its bounded request queue is saturated
- **THEN** an explicitly negotiated nonce-bound heartbeat SHALL still receive one complete response through the independent control path without reading Store or Provider state

#### Scenario: Heartbeat expiry does not fabricate terminal state
- **WHEN** the heartbeat deadline expires while the sidecar process and initialized control connection still exist
- **THEN** the client SHALL preserve confirmed Timeline and active-Turn state, reject new ordinary business requests, keep out-of-band Stop and cleanup controls available, and SHALL NOT claim that the Turn or terminal process ended

### Requirement: Runtime adapters are compatibility tested
Each shipped adapter SHALL declare supported runtime versions, feature mappings,
known degradations, and fixture coverage.

#### Scenario: Installed agent version is unsupported
- **WHEN** discovery finds a runtime outside the adapter's supported range
- **THEN** Work mode SHALL not start through that adapter and SHALL offer a compatible managed runtime or documented upgrade path

#### Scenario: Vendor adds an unknown event
- **WHEN** an adapter receives an event not represented in its pinned schema
- **THEN** it SHALL retain a redacted diagnostic record, avoid inventing a mapping, and fail only the dependent feature when safe
