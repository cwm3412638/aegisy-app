## ADDED Requirements

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
- **WHEN** the current Qt host connects to its child sidecar over stdio
- **THEN** both peers SHALL report `transport: stdio`, `local: true`, and `authenticated`, `encrypted`, and `peer_verified` as false and SHALL NOT treat that channel as the authenticated socket or named-pipe target

#### Scenario: AAP frame reaches the transport limit
- **WHEN** either peer would send or receives a newline-delimited JSON frame larger than the exact AAP 0.1 `max_frame_bytes` value of 4 MiB
- **THEN** it SHALL refuse the write or drain and reject the input with bounded content-free behavior, SHALL NOT allocate an unbounded frame, and SHALL preserve framing for a later valid message when the transport remains usable

#### Scenario: Negotiated connection is lost
- **WHEN** stdio disconnects, the runtime exits, initialization fails, or either peer rejects malformed protocol input
- **THEN** the client SHALL clear ready state, capabilities, and negotiated limits, SHALL fail pending requests, and SHALL require a complete new two-stage handshake before sending more business requests

#### Scenario: Read-only backend is ready
- **WHEN** a backend reports ready and negotiates `permission.read-only`
- **THEN** that state SHALL NOT grant Agent file writes, commands, approvals, network access, background work, or any other mutation authority, while explicit user editor saves and user terminals remain separately scoped operations

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

The future current-Session snapshot capability SHALL be exactly
`timeline.snapshot.current` and SHALL expose only `timeline/snapshot`. Runtime SHALL
advertise it only when the durable floor-visible-state projection, retained Public
Timeline tail, and snapshot materialization path are healthy. Until that complete
path exists, the capability and method SHALL remain unavailable and a retention-gap
response SHALL continue to report `snapshot_available:false`.

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
through the ordinary event validator. A malformed, missing, expired, over-bound, or
identity-drifted page SHALL leave the previous projection visible and the Session
frozen; it SHALL NOT partially append snapshot Items or affect another Session.

The fixed-watermark slice does not satisfy the complete reconnect requirement by
itself. The schema v16 floor/checkpoint is internal retention and startup authority,
not a current-Session snapshot or public recovery response. Automatic production
pruning SHALL remain unreachable until snapshot replacement and structured
retention-gap recovery exist. Live subscription, heartbeat, reconnect orchestration,
and explicit acknowledgement SHALL also remain required before this requirement is
considered complete.

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

The schema v16 retention foundation and implemented structured gap response do not
yet satisfy snapshot recovery. Until the versioned floor-visible-state projection,
snapshot method, complete identities, paging, and Qt replacement land, Runtime fails
closed without returning a partial tail or fabricating a snapshot, and the affected
Session remains frozen.

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

### Requirement: Mutating protocol requests are idempotent
The protocol SHALL require or accept client-generated idempotency keys for turn
starts, approval responses, file writes, Git mutations, and job submission.

#### Scenario: Client retries after timeout
- **WHEN** the runtime receives the same key and equivalent request again
- **THEN** it SHALL return the original operation identity or result without applying the mutation twice

#### Scenario: Key is reused with different content
- **WHEN** an existing idempotency key accompanies a non-equivalent request
- **THEN** the runtime SHALL reject it as a conflict and SHALL NOT apply either a second or merged mutation

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

### Requirement: Runtime adapters are compatibility tested
Each shipped adapter SHALL declare supported runtime versions, feature mappings,
known degradations, and fixture coverage.

#### Scenario: Installed agent version is unsupported
- **WHEN** discovery finds a runtime outside the adapter's supported range
- **THEN** Work mode SHALL not start through that adapter and SHALL offer a compatible managed runtime or documented upgrade path

#### Scenario: Vendor adds an unknown event
- **WHEN** an adapter receives an event not represented in its pinned schema
- **THEN** it SHALL retain a redacted diagnostic record, avoid inventing a mapping, and fail only the dependent feature when safe
