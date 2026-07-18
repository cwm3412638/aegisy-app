## ADDED Requirements

### Requirement: Runtime connections negotiate versions and capabilities
Every AAP connection SHALL complete an initialization handshake before project,
session, turn, tool, or storage methods are accepted.

#### Scenario: Compatible client initializes
- **WHEN** the client sends supported protocol version, identity, platform, and capabilities
- **THEN** the runtime SHALL return negotiated stable and experimental capabilities, limits, runtime identity, and transport security state

#### Scenario: Protocol versions are incompatible
- **WHEN** no supported protocol range overlaps
- **THEN** the connection SHALL fail before loading a session and SHALL identify the required client or runtime upgrade

#### Scenario: Optional capability is missing
- **WHEN** an adapter cannot provide a feature such as steering, structured patches, Skills, or child sessions
- **THEN** the runtime SHALL report it unavailable and the UI SHALL disable the dependent action without simulating success

### Requirement: Work is represented as Project, Session, Turn, and Item events
The protocol SHALL expose typed lifecycle objects and immutable event identity for
all user and Agent activity.

#### Scenario: User starts a turn
- **WHEN** a valid turn-start request is accepted
- **THEN** the runtime SHALL return a turn identity, emit a started event, stream ordered item events, and emit exactly one completed, interrupted, or failed terminal event

#### Scenario: Item streams incremental state
- **WHEN** a message, plan, reasoning summary, command output, patch, or tool result arrives incrementally
- **THEN** every delta SHALL reference the same session, turn, and item identities and SHALL be reconstructable in sequence order

#### Scenario: Turn fails after partial output
- **WHEN** a runtime, provider, sandbox, tool, or transport error terminates the turn
- **THEN** the terminal event SHALL retain partial items and a structured error class without marking incomplete mutations successful

### Requirement: Runtime events support reconnect and replay
The runtime SHALL assign a monotonic session sequence and persist replayable
events before acknowledging terminal mutations to the client.

#### Scenario: Client reconnects after losing transport
- **WHEN** the client supplies its last acknowledged sequence
- **THEN** the runtime SHALL replay later persisted events and then continue live streaming without duplicate effects

#### Scenario: Requested replay point is no longer retained
- **WHEN** the event sequence predates retained replay data
- **THEN** the runtime SHALL return a current session snapshot plus the first available sequence and SHALL identify any non-replayable diagnostic gap

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
