## Context

Aegisy is currently a C++17 Qt Widgets application for Windows and macOS. The
main process owns login, secure token storage, account/model APIs, connection
profiles, a local gateway process, Skills, MCP configuration, desktop-client
integration, updates, and runtime status. The existing AI chat is a dialog that
calls model APIs but is not an execution Agent: it does not own a workspace,
terminal, structured file changes, Git state, approvals, durable execution
events, or crash-resumable task lifecycle.

The active product decision changed on 2026-08-23. Aegisy is first an Aegisy
website companion that turns account/configuration state into a verified local
environment. The default shell therefore prioritizes one-click connection
configuration, repair and rollback, gateway status, desktop enhancements, Chinese
UX, plugins, custom Skills, MCP, diagnostics, and updates.

The existing coding-environment design remains a retained architecture reference,
not the near-term product definition. Only the pinned Codex adapter is in active
integrated-programming scope. ACP and Claude/Gemini Agent adapters, full provider
routing, Agent-authored mutation, background agents, and IDE replacement work are
deferred. Existing safety boundaries stay enforced while that code remains checked
in; deferred capability must be reported unavailable rather than simulated.

The first companion data boundary is a non-authorizing website observation, not an
automatic configuration command. The host verifies the account before requesting
Keys, binds every request to an auth epoch and exact reviewed HTTPS origin, and
projects the response into bounded metadata without raw account/Key identifiers or
credential values. Offline cache entries are keyed by a domain-separated account
identity and may support status display only. They cannot create/activate a Profile,
select a model, write tool configuration, or prove the credential remains available
in SecureStorage. Those authorities require the later credential-broker and
one-click apply transactions.

Validated credentials cross a second, narrower boundary before the connection
wizard can use them. `CompanionCredentialBroker` stores each value only at an exact
account/Key-derived SecureStorage slot and adds an opaque handle to the metadata
projection. The wizard stores no Key in widget item data and resolves a handle only
for an explicit test, model query, or final Profile save. Profile schema 7 records
only the hashed website account, Key, and projection identities. ModelsDialog uses
the same sanitized candidate and correlated model path. API-Key management, Chat,
image, and usage consumers still use the legacy raw inventory and must
move behind equivalent broker APIs before the website projection is complete.

ConnectWizard model discovery is separately correlated. A unique request binds the
current auth epoch, account or local Profile identity, exact Key/credential handle,
source projection identity, tool platform, and reviewed origin/URL. Completion
rechecks the live projection entry before accepting a strict
`aegisy-companion-model-projection/0.1` containing only bounded model IDs and fixed
false selection authority. Key, account, origin, projection, or tool changes retire
the pending UI state; a late global or request-specific response cannot replace the
new selection.

The design is informed by the source research in `research.md`. Codex App Server
demonstrates a mature rich-client protocol; Kimi CLI demonstrates UI/runtime and
provider separation plus ACP; Claude Code demonstrates disciplined workflows;
OpenClaw demonstrates a durable control plane and background sessions; Cline,
Aider, OpenHands, Continue, Roo Code, OpenCode, and Goose demonstrate checkpoints,
repository maps, isolated runtimes, role-specific models, and orchestration.

Stakeholders include individual developers, Aegisy account/model-service owners,
desktop and runtime engineers, security/release owners, and future plugin or MCP
authors. The first supported execution targets remain local macOS and Windows.

## Goals / Non-Goals

**Goals:**

- Make website-backed local setup, configuration verification, repair, rollback,
  extensions, localization, and diagnostics the primary product workflow.
- Keep Claude Code, Gemini CLI, OpenCode, and Codex CLI configuration support
  separate from embedded Agent-runtime support.
- Retain one bounded Codex programming destination without expanding authority.

- Provide an Aegisy-owned coding experience with fixed Agent interaction beside
  the active workspace and a clear Chat/Work contract when the optional Codex
  destination is used.
- Make projects, sessions, runtime events, files, diffs, terminals, approvals,
  Git, model state, and extensions durable and inspectable.
- Reach useful production capability quickly by adapting proven Agent runtimes,
  while keeping the UI and persisted product data independent of any vendor.
- Use Aegisy's model catalog and account services as the authority for provider
  availability, model capability, routing, usage, and billing.
- Keep file and command execution outside the Qt UI process behind a local,
  versioned protocol, and require authenticated IPC before the production
  transport milestone is complete.
- Preserve user control through sandbox profiles, granular approvals, visible
  plans and diffs, cancellation, checkpoints, and crash recovery.
- Establish quality gates that prevent background and multi-agent autonomy from
  shipping before local single-agent work is reliable.

**Non-Goals:**

- Implement Claude, Gemini, ACP, or another non-Codex integrated Agent runtime in
  the active companion milestones.
- Make the full Workbench roadmap a release prerequisite for the companion tool.

- Reimplement a complete IDE, language server ecosystem, terminal emulator, Git
  client, and autonomous Agent in the first release.
- Clone the Codex desktop UI, Claude Code prompts, OpenClaw identity, or any third
  party branding or private implementation.
- Promise lossless mid-turn model switching between incompatible provider APIs.
- Send repository contents, shell history, diffs, or secrets to Aegisy telemetry
  by default.
- Expose background tasks to remote messaging channels in the first milestones.
- Replace Aegisy's existing connection management before the workbench reaches
  parity for account, model, Skills, MCP, update, and recovery workflows.
- Ship unrestricted host execution as the default merely because the user is the
  local machine owner.

## Decisions

### 1. Product shell and Chat/Work interaction model

The main application will gain an `Agent Workbench` destination. Its logical
layout is:

```text
+------------------+----------------------+--------------------------------+
| Product rail     | Agent surface        | Work canvas                    |
|                  |                      |                                |
| Chat / Work      | project + sessions   | editor / diff / preview        |
| New task         | conversation timeline|                                |
| Projects         | plan + tool events   | file tree / terminal / Git      |
| Extensions       | approvals + composer | diagnostics / task artifacts    |
+------------------+----------------------+--------------------------------+
```

- `Chat` is non-mutating by default. It can search explicitly shared context and
  can propose a task, but it cannot run workspace write or shell tools until the
  user converts the conversation into Work.
- `Work` binds one session to a project root, runtime, permission profile, model
  profile, and change set. The Agent timeline stays visible to the left of the
  work canvas on wide windows.
- On narrow windows, product rail, Agent surface, and work canvas become mutually
  exclusive drawers/tabs; the composer and pending approval are never clipped.
- Project/session navigation is persistent, while terminal, file tree, editor,
  diff, Git, preview, and diagnostics are view tabs, not separate modal dialogs.
- The Agent surface displays structured plan, command, patch, approval, question,
  error, and usage items. Raw protocol JSON is available only in diagnostics.

Alternative considered: add Work controls to the existing chat dialog. Rejected
because the dialog lifecycle, layout, and direct API ownership cannot support a
durable project execution model.

### 2. Hybrid UI instead of a hand-built Qt IDE or immediate full rewrite

Keep the signed Qt application as the host for login, secure storage, tray,
updates, desktop integration, native menus, and current management pages. Add a
web workbench bundle inside `QWebEngineView` for Monaco editor, xterm.js terminal,
virtualized timelines, diff rendering, and flexible split panes. Communicate with
the Qt host through a narrow `QWebChannel` bridge and with the Agent runtime
through the host's protocol client. The current child-process stdio transport is
explicitly unauthenticated; tasks `4.2` through `4.4` own the authenticated
production transport.

Before committing to this dependency, Milestone 0 must measure bundle size,
startup memory, Chinese IME, accessibility, drag/drop, clipboard, Retina and
Windows scaling, terminal throughput, code signing, and updater impact.

Alternatives considered:

- Pure Qt Widgets with `QPlainTextEdit`: too weak for a production editor,
  terminal, diff, and large virtualized timeline.
- QScintilla plus a native terminal library: possible, but creates separate
  cross-platform integration and accessibility work for every workbench view.
- Immediate Electron/Tauri rewrite: gives a strong web UI but needlessly risks
  current account, gateway, packaging, updater, and native integration behavior.

If the WebEngine spike fails its gates, the fallback is a separately versioned
Tauri workbench process opened from Aegisy and connected to the same local Agent
protocol. The protocol and storage decisions therefore cannot depend on Qt.

### 3. Separate `aegisy-agentd` runtime sidecar

Create a Rust sidecar process named `aegisy-agentd`. The production transport
target has the Qt host start and monitor it, pass a one-time bootstrap secret
through an inherited handle, and connect over a user-local transport:

- Unix domain socket on macOS.
- Named pipe on Windows.

Tasks `4.2` through `4.4` own those socket/pipe, peer-validation, ACL, and
bootstrap-authentication guarantees. Task `4.2` adds a production-shaped macOS
Unix-domain-socket path behind an explicit Qt transport mode. The sidecar creates
a fresh per-launch endpoint directory with mode `0700`, binds an `agent.sock`
socket with mode `0600`, and records the directory and socket device/inode/owner
identities before accepting one peer. Parent and endpoint directories are opened
through owner-only, no-follow descriptors; extended ACLs, symlinks, path escapes,
pre-existing objects, and identity drift fail closed. Both peers use macOS `getpeereid`; the
sidecar also requires the peer PID to be its Qt parent, while Qt requires the
server PID to be the exact supervised `QProcess`. Peer verification completes
before Runtime/Store/Codex construction or any AAP frame processing, and failure
does not fall back to stdio. The accept loop is bounded, rechecks that the
supervising parent is unchanged before and after peer verification, and restores
the accepted stream to blocking mode before handing it to the existing bounded AAP
reader. After the first verified accept, the listener path is unlinked.

Qt binds the peer proof to one process generation; socket ingress, writes, and the
initialize request require the proof. Endpoint-invalid, peer-mismatch, and
unexpected-disconnect paths share a generation-owned terminate/kill/reap path and
preserve the first specific failure. Rust and Qt cleanup use device/inode/owner
identity and random quarantine. Replacement objects are preserved, while Qt may
finish removing a matching sidecar quarantine if termination interrupted cleanup;
unknown or mismatched entries are never deleted.

Qt also binds each connection attempt to a fresh `QLocalSocket` and monotonically
increasing attempt epoch. Every socket signal captures a guarded pointer, process
generation, and epoch and validates that triple before reading, changing peer proof,
scheduling reconnect, terminating a process, or sending initialize. Peer proof is
valid only for the exact generation and epoch. Retiring the current socket clears
that proof, disconnects its callbacks, aborts it, and defers deletion; already queued
or subsequently delivered callbacks from an older socket or epoch remain inert.

The verified socket truthfully negotiates `transport: unix-domain-socket`,
`local: true`, `peer_verified: true`, and `authenticated`/`encrypted` as false.
Same-UID and exact-PID verification narrows the local peer but is not the one-time
bootstrap authentication owned by `4.4`; it supplies no replay resistance and
does not complete the authenticated production IPC target. Until that bootstrap
is implemented, Qt keeps stdio as its default mode. Stdio continues to negotiate
`local: true` with `authenticated`, `encrypted`, and `peer_verified` all false and
remains appropriate for controlled fixtures and runtime-adapter child processes.

The sidecar owns Agent threads, terminals, filesystem operations, Git, sandbox
policy, MCP processes, provider/runtime adapters, event persistence, context
assembly, and background execution. The UI process never executes model-proposed
commands and never sends long-lived Aegisy credentials to web content.

The sidecar runs as the logged-in user but applies a stricter per-session sandbox.
It is a child of the desktop application in initial milestones, with an idle
shutdown policy. A supervised background service is deferred until background
jobs are production-ready.

Alternative considered: build the runtime in C++ inside the main process.
Rejected because a UI crash would terminate every Agent and a compromised web
surface would share an address space with credentials and command execution.

### 4. Aegisy Agent Protocol (AAP)

Define AAP as versioned JSON-RPC 2.0 with an Aegisy-owned stable Schema package.
The `core.schema.json` component is a reusable definition library, not a message
that aggregates every domain. Method-specific wire results select exact
definitions or documented projections:

- thin `Project` and `Session` values remain separate from `projectRoot`,
  `projectNavigationEntry`, and durable `sessionProjection` records;
- core `Turn` lifecycle uses `running|completed|failed|interrupted`, while the
  `turn/start` request acknowledgement independently uses `started|terminal`;
- core `Item` stays behaviorally aligned with transport `timelineItem`, while a
  replayed Session history Item adds its sequence and optional Turn binding;
- Runtime uses distinct live, search, and durable-replay projections instead of
  one synthetic shape;
- Workspace represents only the current project-root binding and its four Git
  states. Dedicated worktrees, raw paths, and permission authority remain false;
- Approval is the existing non-authorizing acknowledgement. Artifact and
  Capability cover the current command-output read and negotiated capability
  set rather than generic or fabricated wire objects.

JSON Schema bounds Unicode characters. Rust and Qt typed validators continue to
enforce UTF-8 byte lengths, aggregate tree limits, cross-field identities,
timestamps, Usage arithmetic, and Workspace Git invariants.

The partial `3.10` core generation slice treats `core.schema.json` as the sole
input for checked-in Rust, TypeScript, and Qt/C++ core domain types and strict
definition-level validators. The generator audits the complete Schema AST and
fails closed on unknown or unsupported keywords instead of silently weakening a
rule. It also rejects normalized generated-name collisions and property-bearing
open objects that its strict DTOs cannot represent. All three validators
additionally enforce JSON-safe integers, Unicode-scalar strings and keys, and the
Item `data` boundary of 16 levels and 4,096 aggregate values. One reviewed map
binds the complete positive fixture catalog to its definition and canonical byte
identity; a separate shared 43-case raw-JSON corpus binds exact accept/reject
decisions. Each runtime parses each case independently, so malformed wire Unicode
cannot be normalized by a different language before validation.
The CMake gate compares independent three-language identities, requires Node and
Cargo when tests are enabled, consumes Unicode fixture paths through native
argument handling, and combines with repository LF normalization and a packaged
`aegisy-aap` check.

The partial Transport generation slice independently treats stable
`aap.schema.json` as the request/response/notification source. Stable `0.1`
already declares generic `result` and `error.data` as true Schemas and
`jsonRpcError.code` as an unbounded mathematical integer. The generator and
validators therefore do not narrow these values to IEEE-754 or signed 64-bit
integers. Node, Rust, and Qt/C++ share the parser profile
`exact-json-number-schema-bounded-integer-unicode-scalar-no-duplicate-keys/0.1`:
raw frames are limited to 4 MiB, 128 nesting levels, and 65,536 nodes; invalid
UTF-8, a leading BOM, duplicate decoded keys, and unpaired surrogates fail before
Schema validation; arbitrary-precision number lexemes survive parsing. Canonical
Transport JSON sorts object keys by UTF-8 bytes and emits normalized coefficient
plus optional decimal exponent without passing through a floating-point value.

A reviewed method registry binds all stable root dispatch conditions, typed
request/success/error definitions, and the two generic unknown-method fallbacks.
The generated Rust/TypeScript/Qt APIs validate all 101 definitions and root
messages. The standalone C++ runtime implements the required Draft 2020-12 subset,
including reference siblings, composition, exact `oneOf`, negation, and conditional
schemas. One CMake gate compares the materializer, independent Node oracle,
generated TypeScript, Rust, and Qt/C++ fixture/corpus identities; C++ targets compile
with warnings denied. Generated Rust items use the same repository-owned
`#[rustfmt::skip]` stability convention as core generation, so generator bytes do
not depend on the host rustfmt layout version.

TypeScript arbitrary-precision Transport numbers are parser-created opaque values.
Their public declaration exposes read-only lexical, canonical, and mathematical-
integer fields plus a non-constructible brand, while the runtime uses a private
membership registry rather than a copyable property or exported symbol. A plain
lookalike or copied object therefore remains an ordinary JSON object or fails
canonicalization; it cannot silently become a number. The Schema npm package
publishes only the C++ Runtime header and implementation required by generated C++.
An exact `npm pack --dry-run --json` inventory is part of both generation freshness
and the aggregate CTest, not an informal release observation.

The generated Rust Transport dispatcher is driven by the same reviewed registry.
Generation binds each success wrapper to its result definition, derives six
method-qualified typed-error entries, and proves that every root `allOf` condition
is restored by method or typed-error dispatch before compiling a generic envelope
validator. A pending response context is indivisible: exact request ID, exact method,
and any typed-error request identity travel together. Wrong or null IDs are unmatched;
the four subscription methods also require their static stage and exact request
identity. Known methods and known typed discriminators fail closed rather than
falling through, while unknown values retain forward-compatible generic handling.
Parse kind and byte offset survive dispatch, known-wrapper failure remains distinct
from generic-envelope failure, and unavailable generated validators are a local
implementation fault rather than a peer JSON-RPC error.

The generated Qt/C++ surface provides the same parsed-message dispatch boundary.
Its raw helpers perform one lossless parse and delegate to overloads that accept the
already parsed `TransportMessage`. When a known typed error has a wrong or null ID,
the dispatcher validates a private clone with a legal placeholder ID so the typed
payload remains fail-closed without changing correlation semantics: a valid payload
is still unmatched and cannot retire pending state, while a malformed payload is an
invalid known message. The production build owns generated C++ and its Schema runtime
through one warnings-denied `AegisyAapTransport` library shared by the application
and tests.

This remains a partial generation architecture, not completion of `3.10`.
The production Rust stdio consumer now parses each accepted frame exactly once with
the generated lossless parser, validates the generic envelope before queue admission,
and carries the decoded message plus generated method classification into ordinary
and out-of-band dispatch. Known definition validation occurs only after the existing
handshake/capability or queue-overload precedence point, so malformed repeated
`initialize`, wrong-kind `initialized`, and saturated generic-valid requests keep
their stable errors. A generic or per-definition `ValidatorUnavailable` failure is a
local implementation fault: no peer response or request-ID claim occurs, no Runtime
or Store effect is allowed, and a mutex-backed fault gate linearizes the failure
against active and queued dispatch.

The production Qt wire consumer now uses the generated lossless parser,
method/typed-error metadata, and indivisible pending response context before
QJsonObject projection; pre-validating and then reparsing the same bytes through
`QJsonDocument` is not equivalent. Clean Windows Unicode-checkout execution
evidence remains required before `3.10` can be checked. A future AAP `0.2` may deliberately narrow
generic integers and remap current `-321xx` application errors outside JSON-RPC's
reserved range, but that would be a reviewed versioned contract change rather than
a silent `0.1` generator behavior.

Protocol properties:

- Two-stage `initialize`/`initialized` handshake with numeric client protocol
  ranges, bounded client/runtime identity, platform, limits, and truthful
  transport-security state. Business methods remain unavailable until the
  runtime consumes the exact empty `initialized` notification.
- Separate stable and experimental capability declarations. The runtime returns
  only the supported intersection, and both the host and runtime enforce the
  resulting per-method gates. Unknown or absent capabilities never simulate
  success or confer authority.
- An exact 4 MiB bidirectional frame ceiling for AAP `0.1`, with bounded
  draining/rejection of oversized input. Negotiated inline payload sizes,
  chunking, and content references remain owned by task `3.8`.
- Disconnect, runtime exit, or protocol rejection clears readiness, capability,
  and limit state; reconnect always starts a new handshake.
- `timeline-event/0.1` uses a contiguous positive safe-integer sequence per
  Session within the current Runtime live stream, a content-hashed immutable
  Event ID, the Runtime observation time clamped non-decreasing per Session, and
  a correlation ID equal to the bound Turn ID. Item updates carry a contiguous
  per-Item revision and explicitly use `snapshot-replacement` content semantics.
  Optional Item data uses a recursively bounded canonical JSON subset and only
  exact mathematical integers in the signed JSON-safe range. Event identity hashes
  a fixed-order compact UTF-8 envelope without `event_id`: Item data keys sort by
  UTF-8 bytes, arrays retain order, nullable fields remain present, integers use
  shortest base-10 form, and Item/Item-update fields have fixed order. SHA-256
  consumes the NUL-terminated `aegisy-timeline-event/0.1` domain, an eight-byte
  big-endian material length, and the material bytes. This lets Rust and Qt
  independently reproduce the same immutable Event identity without numeric
  precision or key-order drift.
  The only Turn terminal states are `completed`, `failed`, and `interrupted`.
- A single Runtime sequencer validates Turn and Item state before allocating a
  public sequence. Preview, Codex, and future adapters cannot bypass
  `started -> delta* -> completed`, emit after a terminal Turn, invent a fourth
  persistence terminal, or change Session/Turn/Item/kind/role identity midstream.
  Structurally valid unknown itemless running events advance the client cursor
  without changing product state; their names are retained only as bounded
  content-free diagnostics.
- Snapshot plus replay from sequence number for reconnect and crash recovery.
  This is a separate `3.5` public Event Journal contract: the public sequence is
  durable and independent of the internal Workbench projection-event sequence.
  Schema v16 adds a per-Session durable retention floor and one validated
  checkpoint projection so startup can restore Sequencer lifecycle state from a
  compacted prefix plus the retained Journal tail. Schema v17 separately stores the
  sanitized visible Item and active-Turn state at that exact floor. Runtime reduces
  the retained tail through one fixed head, and Qt stages every page privately before
  atomically replacing one Session. The v16 checkpoint remains internal lifecycle
  authority and is never exposed as Item content. This retention-gap path now feeds
  the negotiated subscription state machine. Durable Turn-start acknowledgement is
  implemented on top of this Journal, while complete cross-platform reconnect
  evidence remains a separate release gate.
- Bounded outbound queues, overload errors, cancellation, heartbeat, and
  backpressure. The first heartbeat slice negotiates
  `runtime.heartbeat.out-of-band` and uses the existing independent stdio control
  reader, so a long Turn or saturated 32-request ordinary queue cannot delay the
  response. Every five seconds Qt sends one exact, nonce-bound
  `runtime-heartbeat-request/0.1`; a 15-second expiry marks only connection liveness
  Unknown. It fails ordinary pending work and blocks new business operations without
  killing the sidecar, clearing confirmed Timeline state, or inventing a Turn
  terminal. Cancellation, steering, terminal stop, shutdown, and heartbeat remain
  available through the initialized control connection. The heartbeat carries no
  timestamp, PID, Store/Provider state, permission, or execution authority and never
  touches SQLite. A late or prior-process-generation response is inert. Bounded
  process reconnect and live subscription are implemented below. Durable Turn-start
  acknowledgement is implemented as a separate Store-backed slice; complete Windows
  reconnect/runtime evidence remains a later `3.5` gate.

The bounded reconnect barrier is deliberately request-scoped. A reconnect attempt
records a distinct barrier request ID for each `session/read`, terminal
`terminal/list`/`terminal/attach`, latest-Proposal read, and degradation snapshot.
Every success, malformed response, and explicit failure retires that ID. Session
read failure freezes only its Session and never claims recovery; terminal recovery
requires exact Session/terminal/generation bindings and retains old output as
unverified when proof is unavailable; Proposal drift, empty, or invalid responses
cannot replace a previously validated cache. The degradation response must match
the exact request ID created for that handshake, so a late response from an older
connection is inert. The live subscription state machine composes with this bounded
barrier; it does not provide acknowledgement for approval, file, Git, or background
job mutations.

The independent Runtime control reader also has a handshake ordering barrier. Before
the exact `initialized` notification is consumed, heartbeat, cancellation,
steering, and terminal-stop messages are queued in arrival order. Once the main
dispatcher consumes `initialized`, it re-runs those queued messages through the
ordinary out-of-band router. This prevents the control reader from bypassing
negotiation while preserving control reachability under a blocked ordinary queue.

Every reconnect control token is generation-bound: initialize IDs are retired on
success and rejection, heartbeat deadlines require the exact request plus heartbeat
and process generations, and stability timers require the process generation that
completed recovery. Stop, negotiation loss, or suppression clears those tokens so
late callbacks cannot change readiness or recovery state.

Timeline subscription is now a negotiated live state machine. Stable Schema, Rust,
and Qt register `timeline/subscribe`, `timeline/subscription-sync`,
`timeline/subscription-snapshot`, and `timeline/subscription-activate` behind
`timeline.subscription.fixed-watermark`. Subscribe selects only `sync-required`
with a non-null fixed watermark or `snapshot-required` with a null watermark; it
cannot become active inline. Runtime creates one connection-generation registry,
never reuses a subscription ID in that generation, permits one attempt per Session,
and captures one durable floor/head/timestamp before returning the route. Every
recovery page is validated against the exact attempt. Retained Sync/Snapshot recovery
units and events after the fixed head share one connection-wide 10,000-unit/64 MiB
budget; events at or before the fixed head are already represented by recovery and
are not republished.

A private non-serializable recovery proof remains the typed structural bridge to
activation. The Sync proof validates a bounded contiguous page chain from the
subscribed cursor through the exact fixed watermark. The Snapshot proof validates a
bounded fixed-header page chain, ordered complete Items, and the domain-separated
snapshot identity. Runtime additionally consumes the private connection-owned
registry token, changes the attempt to active, returns the exact active result, and
then drains the buffered events as subscription-bound notifications. Terminal
failure correlation fixes the exact stage and request identity, or the exact active
cursor for a live failure; every accepted failure retires its attempt. Completion,
activation, accepted failure, retirement, and disconnect release exact connection
accounting once. A cross-Session or cross-generation Sync, Snapshot, or Activate
request is rejected without retiring the true owner's attempt. Disconnect retires
the complete registry, contexts, Session bindings, retained units, events, and bytes.
Qt stages recovery privately, publishes only after the exact activation response,
keeps old-generation and pre-activation traffic inert, and preserves an active
attempt across an ordinary `session/read`; a genuine active sequence gap still
starts recovery.

Because negotiated Runtime suppresses unbound bare events, an event-producing Turn
must not be admitted without connection ownership. `turn/start` returns `-32152`
unless its Session owns a current attempt; events created while that attempt is still
recovering remain in the bounded Runtime buffer, while the Qt product keeps Send
disabled until Active. A typed retryable failure preserves the confirmed projection
and queued prompt, allocates a fresh subscription ID, and retries at most three times
with 0/250/1000 ms delays. Exhaustion remains frozen instead of guessing recovery.

If heartbeat expires while one of the four subscription requests is pending, the
client cannot know whether Runtime consumed it. AAP intentionally has no unsubscribe
shortcut, so Qt seals and terminates that connection, retires every pending request,
and starts one bounded fresh process generation through the normal reconnect barrier.
Qt uses the same generation replacement when a subscribe/Sync/Snapshot/activate
result cannot be validated locally, an Active wrapper or cursor drifts, continuation
is unsafe, or Runtime reports `session-attempt-exists` or
`subscription-id-reused`. It preserves confirmed state and queued input, does not
falsely complete reconnect, and never retries those ownership conflicts on the same
connection. Old-generation traffic remains inert.
Heartbeat Unknown without such a pending request keeps the existing same-process
probe behavior. OpenSpec `3.5` remains unchecked for complete Windows
reconnect/runtime evidence, and automatic Timeline pruning remains disabled.

- Idempotency keys for turn start, approval responses, writes, and background job
  submission.

Turn-start is the first durable mutation slice. AAP capability
`session.mutation-acknowledgements` exposes the session-scoped list method
`session/mutation-acknowledgements` and the exact-anchor consume method
`mutation/acknowledgement/consume`. Runtime reserves a schema-v20
`mutation_acknowledgements` row before dispatch, keyed by Session, `turn-start`,
idempotency key, and a SHA-256 request fingerprint. An equivalent retry returns the
same operation identity and Turn binding; a conflicting fingerprint fails without
dispatch. Accepted and terminal Timeline anchors are bound atomically with the
corresponding projection and Journal transaction using revision compare-and-swap.
Startup converts accepted rows whose dispatch outcome is uncertain to
`reconciliation-required`; those rows cannot be redispatched. Qt lists only
unconsumed rows, validates Session/Turn/sequence/Event-ID anchors, and consumes
accepted evidence before terminal evidence. Anchor drift, unavailable storage,
malformed data, tampering, or read-only recovery freezes the affected Session and
never fabricates success. The ledger stores metadata and Timeline identities only;
it grants no mutation, approval, or execution authority.

Workbench database schema v21 is the historical reservation-only baseline. It added
the separate internal `mutation_reservation_records` wrapper for approval,
editor/file-write, Git, and background-job request metadata without changing the
schema-v20 Turn ledger or AAP wire contract. It persisted the validated lossy
`mutation-reservation-draft/0.1`, redundant scope/key/fingerprint columns, and the
draft SHA-256. Exact reserved retries bypassed write admission, scope was rechecked
inside an `IMMEDIATE` transaction, startup advanced unresolved rows once, migration
and canonical `sqlite_master` inventory checks failed closed on collisions, and owning
Session purge removed the rows. The inner draft and wrapper remained metadata-only and
non-authorizing.

The implemented schema-v22 source-record slice closes the v21 provenance gap. Its exact
typed source union is limited to `approval-acknowledgement/0.1`,
`file-write-acknowledgement/0.1`, `git-mutation-acknowledgement/0.1`, and
`background-job-request/0.1`. The Store accepts one validated typed source and derives
the existing draft itself. It must never reconstruct a source from that lossy draft,
and no API may accept an independently supplied source/draft pair. Each new v22 row
uses provenance exactly `present`; the complete canonical source bytes, source hash,
derived draft, redundant scope bindings, and reservation identity form one immutable
graph.

For a new idempotency tuple, the source record, reservation, and one metadata-only
internal Session event `mutation.reservation-source-recorded` commit in the same
`IMMEDIATE` transaction. The event binds the exact source and reservation graph but
contains no request body, result, path, command, provider content, credential, or
authority. It is an internal Store event only: it is not a Public Timeline Journal
event, does not consume or advance a public Timeline sequence, and is not exposed as
an AAP Item. Failure while writing the source, reservation, event, Session sequence,
or final commit leaves no partial row and no sequence advancement.

An exact complete-source retry returns the original source/reservation/event graph
with zero writes and no Session-sequence advancement, including when low-space
admission blocks new writes or another SQLite writer holds the lock. Reusing the same
Session/kind/idempotency tuple with any complete-source drift is a conflict even when
both sources derive byte-identical drafts. Draft equality is therefore insufficient
for replay authority. Admission still rechecks Session ownership, deletion state,
Session status, project/root/Turn scope, bounds, secret rejection, and the tuple under
the write transaction. Root removal and Session purge cover both provenance classes
without leaving an orphaned source, reservation, or event anchor.

Startup captures the open-candidate scan, validates each complete
source/reservation/event graph, performs every transition, and proves that no
`present`/`reserved`/revision-1 row remains inside one `IMMEDIATE` transaction. This
linearizes both an initially empty scan and a competing non-empty scan with every
reservation writer. For a `present` reservation left `reserved` at revision 1, the
first restart atomically changes it to `reconciliation-required` revision 2 and
appends exactly one internal
`mutation.reservation-reconciliation-required` Session event. A second restart is
stable: it appends no event and advances no Session sequence. Missing, duplicated,
or tampered source/event/hash/anchor data for a `present` row is corruption and enters
read-only recovery; immutable provenance validation prevents such damage from being
reclassified as legacy. For provenance `present`, the only accepted event histories
are exactly `[source]` for reserved and `[source, reconciliation]` for reconciled;
reversed, duplicated, or unknown same-operation events fail closed. A migrated
`legacy-unavailable` reconciliation-required row instead requires exactly empty
history `[]`.

The v21-to-v22 migration preserves every v21 reservation as provenance exactly
`legacy-unavailable` and state `reconciliation-required` revision 2. It fabricates no
source record, no historical source-recorded event, and no reconciliation event.
Legacy absence is valid only because the migration durably records that provenance;
new rows can never use it. Before copy, the migration proves the exact canonical v21
schema, enforces the 10,000-row bound, and semantically validates every canonical
draft, redundant binding, scope, lifecycle, time, and hash. Invalid v21 reservation
rows are
never copied. Every pre-v22 migration also rejects the reserved lifecycle event-kind,
operation-ID prefix, and Event-ID prefixes, so legacy history cannot be reinterpreted
as v22 provenance. Migration collision checks and the canonical `sqlite_master`
inventory extend to every v22 reservation table, index, auto-index, and attached
object. They reject any Trigger attached to the shared `events` or
`session_sequences` write path before the Store becomes writable or reconciliation
runs.

The pre-v22 version observation is not trusted across lock acquisition. The migration
rechecks `user_version` after obtaining its `IMMEDIATE` transaction; if another
connection has already committed v22, the caller retries against the current schema,
while any other version drift fails closed. It also rechecks `application_id` under
that lock. A separately pre-opened read-only connection holds one `DEFERRED` snapshot
across source version/application-ID validation and SQLite Online Backup because a
connection that owns the write transaction cannot itself be an Online Backup source.
The database file identity is captured around initial open and rechecked before and
after backup, before commit, and after commit; path replacement fails closed. A
database already at v22 performs no migration write-lock attempt. The lifecycle lookup uses the covering
index `events(session_id, operation_id, sequence, event_kind)`. A regression fixture
requires SQLite `EXPLAIN QUERY PLAN`, without `ANALYZE`, to select that index so a
schema change cannot silently turn bounded graph validation into an unindexed event
scan.

At the historical v22 boundary, this slice added no production approval/file/Git/job
producer, AAP capability or method, Qt surface or recovery flow, Public Timeline
event, outcome/result anchor, consume or caller-CAS route, dispatch, filesystem
write, Git mutation, background submission, or genuine user Approval. Every
permission, mutation, approval, execution, and dispatch authority remained fixed
false.

Schema v23 adds the crate-internal terminal-outcome Store boundary without widening
that authority. `mutation-reservation-record/0.3` allows only a `present`,
`reserved`, revision-1 graph to reach `terminal` revision 2. The outcome must be the
exact source-bound terminal union already validated by
`mutation_reservation_outcome.rs`; its canonical JSON, SHA-256, byte count,
domain-separated identity, source identity, kind, schema/state, observed time, and
recorded time are redundantly persisted in immutable
`mutation-reservation-outcome-record/0.1` state. The internal
`mutation.reservation-outcome-recorded` event binds the same metadata and keeps all
dispatch, mutation, approval, and execution authority fields false.

Outcome recording uses two replay boundaries. A `DEFERRED` snapshot first permits an
exact existing outcome to return without write admission. After admission sampling,
an `IMMEDIATE` transaction reclassifies the graph so a peer commit cannot race the
decision: an exact peer outcome returns the peer graph, while drift returns a stable
conflict. For a new outcome, the internal event append, immutable outcome insert,
reservation `reserved` revision-1 to `terminal` revision-2 compare-and-swap, complete
graph validation, and final commit are one rollback boundary. Failure at any stage
leaves the prior source/reservation graph and internal Session sequence unchanged.

Both replay and new-write classification validate the exact Session owner, source
and kind, Session archive and pending deletion state, and project/root/Turn scope
under the relevant lock. Exact replay validates the persisted outcome's original
observed and recorded times and intentionally ignores the retry attempt's later
`recorded_at_ms`; it returns the immutable original graph without rewriting time. A
new outcome additionally validates expected revision 1 and the caller's observed/
recorded-time ordering under the write lock. The only valid histories are
`present/reserved` `[source]`, `present/terminal`
`[source, outcome]`, `present/reconciliation-required`
`[source, reconciliation]`, and migrated `legacy-unavailable` `[]`. Startup rejects
missing, duplicated, reordered, orphaned, hash-drifted, authority-drifted, or unknown
outcome state into read-only recovery rather than repairing it.

The v22-to-v23 migration first verifies the exact v22 schema and every bounded source
graph, uses the existing pre-opened snapshot and file-identity migration backup
boundary, rebuilds the reservation table at record schema `0.3`, and copies source
rows without fabricating an outcome. It validates the exact v23 inventory and graph
before the version transition commits. Canonical inventory and reserved event,
operation-ID, and Event-ID namespaces include the outcome table/index/Trigger and
event kind. Session purge removes outcome dependencies before source/reservation
rows in the same transaction.

Schema v24 adds a separate crate-internal evidence-consumption ledger without
changing the reservation lifecycle or AAP. A strict
`mutation-reservation-consumption-receipt/0.1` contains no source/outcome body. It
binds only the reservation/session/kind, exact source or resolution evidence
identity, exact internal event sequence/ID/timestamp, core reservation revision,
independent consumption revision, previous source receipt identity, consumption
time, and four fixed-false authority fields. The durable row retains the immutable
source receipt and permits exactly one guarded transition from `c1` to `c2`; valid
histories are `c0 []`, `c1 [source]`, `c2 [source, terminal]`, and
`c2 [source, reconciliation-required]`. Resolution consumption before source and
legacy-unavailable consumption both fail closed.

Core reservation and consumption revisions are deliberately orthogonal. Source
evidence may be consumed against core `r1` or `r2`; terminal and reconciliation
evidence require core `r2` plus consumption `c1`. Startup can therefore reconcile a
reserved graph from `r1` to `r2` while preserving a pre-existing `c0` or `c1`
ledger. For an `r2` source receipt, both admission and persisted validation require
the consumption time to be at or after the reservation's `updated_at_ms`; the
final `r2` graph may also retain a source receipt created against `r1`, but that
receipt's consumption time must be at or before the `r2` transition time. Outcome
admission rejects a future-dated `r1` receipt before changing the core graph, and the
whole-Store verifier rebuilds source and resolution identities so either direction of
hash-consistent multi-field time forgery enters read-only recovery.

Startup reconciliation treats an existing `c1` source receipt as a durable time
floor. When moving a core reservation from `r1` to reconciliation-required `r2`,
the transition time is the maximum of the current observation, reservation time,
and immutable source consumption time. A wall-clock rollback therefore cannot turn
a valid `r1/c1` history into an invalid late-`r1` receipt during the same startup
transaction.

Consumption uses the same two-boundary replay model as outcome recording. A
`DEFERRED` snapshot returns an exact receipt before write admission; after admission
sampling, an `IMMEDIATE` transaction rechecks owner, archive/pending-deletion,
project/root/Turn scope, core revision, consumption revision, evidence identity,
event anchor, and time. An exact peer commit returns the peer's immutable first
receipt; drift reports a stable conflict. Source insert or resolution CAS, complete
graph validation, and final commit share one rollback boundary. No consumption
operation appends an internal or Public Timeline event or advances either sequence.

Schema-v24 startup scans at most the bounded reservation limit of consumption rows
and semantically validates each complete graph. Schema identity, receipt, evidence,
anchor, owner/kind, phase, time, and authority drift fail into whole-Store read-only
recovery. Session purge deletes consumption before outcome, source, and reservation
rows. The v23-to-v24 migration first validates the exact v23 schema/graph, publishes
the existing WAL-consistent backup, creates an empty ledger plus index/Triggers, and
then validates v24; it preserves all prior reservation/source/outcome/event bytes
and fabricates no consumption evidence.

The ledger has no independent high-water or parent consumed marker. Complete
deletion of an otherwise valid consumption row is therefore indistinguishable from
a legitimate `c0` history. v24 detects partial, malformed, orphaned, and cross-bound
evidence but is not anti-deletion or at-most-once authority; a later reviewed durable
anchor is required before production recovery may make that claim.

This remains an internal persistence foundation. There is still no production
approval/file/Git/job producer, AAP capability or method, Qt recovery flow, Public
Timeline event, external caller consume/CAS route, reconciliation resolution,
dispatch, filesystem write, Git mutation, background submission, or genuine user
Approval. The API remains crate-internal, every authority remains false, and
Agent/Codex remains read-only until the later reviewed producer and recovery
boundaries are complete.

- Server-initiated requests for approval, structured user input, credential
  refresh, and extension elicitation.
- Stable core namespace plus a separate experimental namespace. The checked-in
  Schema package owns explicit namespace registries; stable versions are
  additive-only and cannot reference experimental schemas. The experimental
  registry is currently empty and wire-unavailable, so its presence does not
  advertise a method or capability. Unknown events are preserved for diagnostics
  and ignored safely by older clients.
- Content size limits and references for large command output, diffs, images,
  artifacts, and files rather than unbounded JSON messages.

AAP adapters translate Codex App Server and ACP events into stable AAP items.
The UI never consumes vendor events directly.

### 5. Runtime adapter sequence

Implement adapters in this order:

1. `CodexAppServerAdapter`: launches a pinned compatible Codex App Server,
   configures an Aegisy custom provider, maps thread/turn/item events, plans,
   diffs, approvals, terminal events, token usage, Skills, MCP, hooks, and errors.
2. `AcpAgentAdapter`: supports ACP-compatible agents such as Kimi CLI and other
   installed coding agents; unsupported features are reported through negotiated
   capability flags rather than simulated.
3. `AegisyNativeAdapter`: incrementally owns provider calls, context, tools, and
   orchestration after the protocol, security, and evaluation corpus stabilize.

Adapter processes are pinned and health-checked. Protocol fixtures are recorded
without secrets and replayed in CI. A compatibility matrix controls which runtime
versions can ship with each Aegisy release.

Forking Codex as the entire product was rejected because upstream protocol churn,
provider assumptions, release cadence, and merge burden would control Aegisy's
UI and roadmap. Process-level adaptation uses the mature engine while preserving
replacement boundaries.

### 6. Aegisy model catalog and capability negotiation

The website/API will expose a versioned model catalog. Each model entry includes:

- Stable Aegisy model ID, provider family, upstream model ID, availability,
  entitlement, region, deprecation state, and aliases.
- Supported wire protocols: Responses, Chat Completions, Anthropic Messages,
  Gemini, or Aegisy-native Agent streaming.
- Context and output limits, tokenizer/usage authority, auto-compaction guidance,
  reasoning controls and ranges, prompt caching, structured output, tool calls,
  parallel tools, image/video/audio input, and realtime support.
- Suitability roles such as `agent`, `plan`, `apply`, `review`, `fast`,
  `embedding`, and `rerank`, backed by Aegisy evaluations rather than marketing.
- Cost/usage display metadata and zero-data-retention or routing constraints when
  contractually known.
- Runtime compatibility and known feature degradations.

The desktop caches a signed catalog with expiry and an explicit stale state. A
model is selectable only when its required capabilities match the active mode
and runtime adapter. Unknown capability disables the dependent feature.

Project model profiles may select different models for Agent work, planning,
fast edit/apply, review, session naming, embeddings, and reranking. The default
profile uses the fewest roles necessary; multi-model pipelines expose expected
extra cost and latency.

### 7. Model switching and portable context

Model changes apply at a turn boundary, never midway through an upstream response
or tool call. Three switching levels are defined:

1. **Compatible switch**: same runtime and portable protocol state. The next turn
   selects the new model and records a `modelChanged` item.
2. **Portable fork**: runtime or protocol changes. A new child session receives a
   portable context package: user/agent messages, approved summaries, plan, open
   tasks, selected files, repository state, and tool results needed for continuity.
3. **Blocked switch**: required tool, modality, context, or policy is unavailable.
   The UI explains the exact missing capabilities and offers compatible models.

Provider-encrypted reasoning, opaque response IDs, cache handles, and hidden
thinking are never replayed across providers. The old session remains intact and
linked. The UI may present a seamless lineage, but diagnostics must show the fork.

### 8. Durable session and event storage

Use SQLite in WAL mode for metadata, indexes, lineage, projects, sessions, turns,
items, approvals, model selections, extension state, Git checkpoints, and jobs.
Store the canonical append-only event payloads in chunked JSONL or a content-
addressed blob store with hashes referenced by SQLite. Large terminal output,
patches, images, and artifacts are compressed and bounded.

Session-derived views are rebuildable from events. Schema migrations are
transactional, versioned, backed up before upgrade, and tested from every
supported previous version. A failed migration starts the workbench read-only
and offers diagnostic export; it never deletes history automatically.

Public Timeline retention uses the schema v16 cursor floor plus exactly one
`public-timeline-checkpoint/0.1` projection per Session. Its canonical inner
`event-sequencer-checkpoint/0.1` value binds the Session and exact sequence,
Event-ID, and timestamp anchor, plus only the Turn/Item lifecycle state needed to
validate later events. It excludes Item content and data, is capped at 16 MiB,
100,000 Turns, and 100,000 Items, and has a domain-separated
`event-sequencer-checkpoint:sha256:` identity. Checkpoint replacement, floor
advancement, and prefix deletion are one SQLite transaction. Startup validates
the checkpoint and retained contiguous tail before restoring the Sequencer;
tampering, anchor drift, gaps, or bounds failures fail closed. The v15-to-v16
migration initializes an empty floor/checkpoint without rewriting existing public
events, and Session purge resets Journal, checkpoint, and floor atomically.
Public Timeline foreign keys restrict Session deletion instead of cascading, so a
missing rebuildable Session projection cannot silently erase replay authority.
Startup may replay a complete cursor/checkpoint/tail while that projection is
temporarily missing, but it enforces exact Session ownership immediately after
projection recovery and before the Store becomes writable.
Schema v19 adds one durable Change reference for each newly recorded Codex
file-change Proposal without making the prunable Journal row permanent. The
producer prepares a metadata-only completed `file-change` Item and public
`item.completed` envelope, then one SQLite transaction commits the Item and its
internal `item.appended` event, the unchanged internal
`workspace-edit.proposal-recorded` event, Proposal and artifact/Blob-reference
rows, the public envelope, and an immutable
`workspace-edit-proposal-reference/0.1` binding. Only after commit may Runtime
advance its in-memory Sequencer and notify Qt. The binding retains the exact public
envelope bytes/hash and sequence plus the Item identity, sequence, and payload
hash. Its Item foreign key is deferred so projection recovery can delete and
reinsert Items in the same transaction; it deliberately has no foreign key to
`public_timeline_events`, because a reviewed checkpoint/prune may remove that
envelope while the durable Item and Proposal reference remain valid. Reads require
the exact retained envelope when present, or prove that its sequence is at or below
the validated retention floor after pruning.
Automatic production pruning remains disabled until a later reviewed stage enables
it, even though the versioned current-Session snapshot, structured retention-gap
response, and client recovery flow now exist.
The structured gap response is `timeline-retention-gap/0.1` on JSON-RPC `-32148`.
It publishes only requested anchors, the validated floor/head, and fixed recovery
metadata. It requires `timeline.snapshot.current` through `timeline/snapshot`, marks
that route available only when the current connection negotiated the capability,
declares retained event history incomplete, and forbids replay directly from the
floor. Retained anchors
with substituted Event IDs remain drift failures. The v16 Sequencer checkpoint is
never serialized into this response.

The public current-Session snapshot uses a separate durable visible-state
projection rather than `session/read`: projected Items omit live `started`/`delta`
updates, do not bind one Public Timeline head, and the v16 checkpoint intentionally
omits Item content. Schema v17 adds one durable floor-visible-state snapshot at the
exact retention-floor anchor. Advancing retention commits
that sanitized visible state, the content-free Sequencer checkpoint, floor movement,
and exact prefix deletion in one SQLite transaction. Runtime materializes a public
snapshot by replaying the retained tail strictly after that floor through one fixed
Public Timeline head; events appended after that head do not enter the snapshot.

The stable capability is exactly `timeline.snapshot.current` and its only
method is `timeline/snapshot`. The first request supplies a Session, null snapshot
identity, null watermark, null item cursor, and a 1-200 Item limit. Runtime captures
the current Journal head once and returns `timeline-session-snapshot-page/0.1`.
Continuations repeat the exact snapshot identity and watermark plus the Runtime-
issued last-Item cursor; neither the head nor the complete snapshot may move while
paging. Each page stays below the 4 MiB AAP frame limit and may stop before the count
limit. The complete materialized snapshot is limited to 10,000 Items and 64 MiB of
canonical Item material; exceeding either bound fails closed without pruning or
partial client replacement.

Every snapshot Item has a contiguous positive snapshot ordinal and an identity over
its Session, ordinal, Turn/correlation binding, Turn state, first/latest Public Event
anchors, complete sanitized Item, and current `item_update` revision/content mode.
The complete snapshot identity is
domain-separated over the schema, Session, floor anchor, fixed watermark, nullable
active running Turn, total Item count/bytes, and the ordered complete Item-identity
list. A page identity additionally binds the snapshot identity, request cursor,
returned ordered identities, next cursor, and completion state. A snapshot taken
during a running Turn includes that exact active Turn plus every current
`started`/`delta` Item revision. The active Turn binds its exact started/latest Public
Event anchors and ordered open Item IDs, so live events strictly after the watermark
can continue through normal validation; it never invents provider execution state.

Qt keeps the affected Session frozen and its previous confirmed projection visible,
queues bounded later live events, and validates every snapshot page into private
staging. It verifies the fixed floor/watermark, snapshot/page/Item identities,
contiguous ordinals, active-Turn consistency, totals, and final cursor before making
anything visible. Only a complete identity-valid snapshot atomically replaces that
one Session's Timeline and lifecycle cursor; Qt then drains queued events after the
watermark through the ordinary event validator. Failure preserves the previous
projection and frozen state while unrelated Sessions continue. The strict Rust types,
stable Schema, schema-v17 Store materialization, Runtime fixed-head pages, and Qt
cross-page replacement implement this contract end to end. The gap response reports
availability per connection negotiation. Qt discards delayed notifications at or
below the watermark and validates only later events; because a snapshot has no
timestamp, the first valid post-watermark event establishes the new client baseline.
A disconnect drops incomplete page staging but preserves confirmed UI, bounded queued
live events, their accounting, and recovery intent so capture restarts after a fresh
handshake. Automatic pruning remains disabled. The later subscription/reconnect
design below supersedes those historical gaps; OpenSpec task `3.5` stays unchecked
until complete Windows recovery evidence lands.

Credentials remain in OS secure storage. The database contains credential IDs or
short-lived token references, never API keys or long-lived Aegisy JWT values.
Each project can choose retention limits and delete/export its sessions.

### 9. Context engine and repository intelligence

Build context in explicit layers with provenance and token budgets:

1. System/runtime instructions and security policy.
2. Project instructions (`AGENTS.md`, supported vendor files, Aegisy rules).
3. Active goal, plan, task state, approvals, and compacted session memory.
4. User-pinned files, selections, images, diagnostics, and terminal excerpts.
5. Symbol-aware repository map and ranked search results.
6. Recent conversation and required tool results.

Use tree-sitter initially for symbols and dependencies, with language-server
integration added for definition, reference, diagnostic, and rename precision.
Repository indexing runs incrementally, honors ignore and secret rules, and never
blocks opening a project. Every context item exposes origin, size, last update,
and whether it will be sent to the selected model.

Compaction creates a checkpoint containing decisions, unresolved work, changed
files, commands/tests, failures, and next steps. Users can inspect and edit a
manual compaction instruction. Raw history remains locally available even when
it is no longer model context.

### 10. Workspace tools, editor, terminal, and patches

- Monaco provides editing, diff, search presentation, and language-client UI.
- xterm.js displays PTY sessions owned by the sidecar. The sidecar emits structured
  process lifecycle and bounded output events in addition to terminal bytes.
- File operations use normalized workspace-relative paths, symlink checks,
  optimistic version hashes, atomic writes, and structured `WorkspaceEdit` data.
- Agent patches are previewed as unified and side-by-side diffs. User edits made
  after proposal trigger a rebase/re-read requirement; stale patches are not
  applied silently.
- A read-only provider file-change request becomes an immutable local Proposal
  before the adapter sends its fixed policy denial. The Proposal binds the exact
  Session, durable Turn, provider thread/item, project/root/filesystem identity,
  normalized operations, overlap baseline, preview summary, and content-addressed
  artifacts. For new schema-v19 writes, the Proposal, artifact references, completed
  metadata-only `file-change` Item, both internal events, public `item.completed`
  envelope, and immutable Proposal/Timeline binding commit atomically; all mutation,
  user-approval, and apply authority fields remain false. A caller fault after commit
  cannot compensate away committed rows or Blob references, while Proposal, binding,
  Item, public-envelope, or Blob corruption quarantines only its owning Session.
  Current `workspace-edit-proposal/0.2` records bind complete aggregate and
  ordered per-file summaries, with semantic statistics rechecked from untruncated
  persisted diff Blobs. Exact `0.1` canonical bytes and identities remain readable
  through an explicitly incomplete public projection. A v18-to-v19 migration marks
  every existing Proposal as legitimately reference-less and fabricates no Item or
  public history; every Proposal created by the v19 Runtime requires exactly one
  reference. Negotiated
  `workspace.edit.proposal.read-only` plus `permission.read-only` exposes
  Session-scoped latest/exact Proposal reads and 64 KiB Proposal-owned artifact
  pages with typed domain-separated identities; every public authority field remains
  false. Qt negotiates the capability and restores strictly validated Session-bound
  Proposals into Changes after initialization, reconnect, and Work Session recovery.
  Foreground Proposals auto-open Changes, background Proposals add an unread marker
  without stealing focus, and disconnected cached records remain unverified until a
  fresh latest read confirms them. Artifact pages are bound to the frozen Session,
  Proposal, file/reference, offset, accumulated bytes, and generation; Qt verifies
  Base64, chunk and complete hashes, the fixed page identity, byte boundaries, and
  complete UTF-8 decoding before treating a diff as complete. Provider `completed`
  is treated as a potential unauthorized write until the later
  approval/checkpoint/apply pipeline exists.
- The public `file-change` Item contains only the fixed text
  `File changes proposed (read-only)` and a closed
  `workspace-edit-proposal-reference/0.1` value. The value binds Session, Turn,
  Proposal, project, root, edit, preview identity, aggregate counts/applicability,
  the Item identity through its domain-separated Reference ID, and three false
  authority fields; it contains no path, diff, source body, raw Provider identity,
  approval, or apply token. Qt validates that exact identity and binding before
  rendering `View changes`, then performs the existing Session-scoped exact Proposal
  read and rechecks the returned Proposal summary. A stale, forged, cross-bound, or
  failed exact read changes neither the latest-Proposal cache nor the visible Changes
  view. This is a durable review link, not an Approval or mutation capability.
- A file watcher invalidates context and UI state. Agent and user changes carry
  origin metadata.
- Diagnostics come from build/test output first, then language servers. The UI
  distinguishes observed diagnostics from model claims.
- Long-running commands become named background terminals with stop/restart and
  output-tail controls.
- Generated artifacts, previews, screenshots, and reports belong to the turn and
  can be opened without injecting all content into model context.

### 11. Git and worktree safety model

Use Git porcelain v2 and structured plumbing commands through the sidecar; avoid
parsing localized human output. Read-only status, log, branches, and diff are
available without approval. Mutations are classified:

- Low risk: create/switch a new branch in a clean repository, stage selected
  Agent-owned changes when policy allows.
- Medium risk: commit, stash, merge, rebase, cherry-pick, push.
- High risk: force push, reset, clean, deleting branches/worktrees, discarding
  user changes, or rewriting shared history.

Medium and high-risk operations require explicit UI confirmation even when model
tool auto-approval is enabled. High-risk operations cannot be approved with a
blanket session rule in initial releases.

Each background or concurrent coding task receives a dedicated Git worktree and
branch. The runtime records starting HEAD, dirty state, generated checkpoint,
changed paths, tests, and final commit candidates. User pre-existing changes are
never mixed into an automatic checkpoint or commit without explicit selection.

### 12. Security and approval architecture

Define permission profiles: `Chat`, `Read Only`, `Workspace Write`, `Developer`,
and `Full Access`. Each declares filesystem roots, denied paths, network policy,
command policy, MCP/extension access, browser/computer-use access, and whether
background execution is allowed.

Approval decisions are `deny`, `allow once`, `allow for turn`, `allow for
session`, or a narrowly scoped durable rule. The UI shows command, cwd,
environment, requested paths/hosts, risk level, reason, and resulting diff.
Durable rules are editable and hash the relevant command/extension definition so
changed code returns to untrusted state.

Security boundaries include:

- Treat source files, issue text, tool output, websites, MCP responses, and remote
  messages as untrusted data, never higher-priority instructions.
- Deny reads of credential stores, SSH keys, browser profiles, cloud credentials,
  `.env` files, and configured secret globs unless specifically granted.
- Redact known secrets from UI events, logs, diagnostics, and model requests while
  preserving a local indication that redaction occurred.
- Allow network by host and purpose; prevent silent credential forwarding across
  redirects or to tool-provided URLs.
- Run MCP servers and plugins with declared permissions and origin/version trust.
- Require signed update manifests and verify the sidecar/runtime adapter hash.
- Update-signing continuity is reconstructed rather than deserialized. The local
  continuity cache persists only exact signed Ring envelope bytes and bounded
  integrity metadata. On every open it replays generations `1..N` from the
  embedded Root with the current verification time. Only strict current-time
  replay may reconstruct a valid Ring `Authority`; a historically complete chain is
  `CachedButNotAuthoritative` only when its strict current-time failure is the
  explicitly replayable activity error (`bootstrap-root-invalid`,
  `signer-inactive`, or `no-current-active-usage`) and carries no valid
  `Authority`; revoked, malformed, structurally invalid, and signature-invalid
  chains remain `Invalid`. The cache grants no update, network, download, install, rollback,
  resume, execution, anti-rollback, anti-deletion, trusted-time, or
  expired-signer-recovery authority. Complete local deletion is observed as
  `Empty` only when the cache directory itself is absent; retained partial evidence
  is `Invalid`. Detecting deletion or a consistent whole-state rollback requires an
  independent authenticated high-water/checkpoint.
- Keep audit events content-minimal: actor, operation, scope, decision, result,
  hashes, timing, and correlation IDs.

Sandbox implementation is a milestone gate. Codex's supported sandbox may be
used through its adapter initially. The native runtime must provide equivalent
macOS and Windows enforcement before it replaces the adapter for writes.

### 13. Extension model

Use distinct concepts with one management surface:

- `Instruction`: project or user guidance such as `AGENTS.md`.
- `Skill`: reusable instruction package with optional references and scripts.
- `Hook`: lifecycle policy or automation around prompts, tools, edits, and turns.
- `MCP Server`: external tools/resources over MCP.
- `Plugin`: installable bundle containing Skills, hooks, MCP configuration,
  assets, commands, and metadata.
- `Runtime Adapter`: privileged signed integration maintained by Aegisy.

Every extension record includes ID, version, source, content hash, signature when
available, trust state, capabilities, requested permissions, update channel,
enabled scopes, and last execution. Project-provided executable hooks do not run
until trusted. Extension changes invalidate prior approval hashes.

### 14. Background and multi-agent execution

The single execution pipeline handles interactive turns, child tasks, and future
scheduled jobs. Execution mode changes policy and lifecycle, not implementation.

- A parent Agent creates a structured child task with goal, allowed workspace,
  model profile, extension set, budget, completion contract, and handoff format.
- Each write-capable child uses an isolated worktree. Read-only explorer tasks may
  share a snapshot.
- Parent/child session lineage and status are visible. Child results are summaries
  plus artifacts/diffs, not unbounded transcript injection.
- Concurrency, token, wall-clock, tool-call, and cost budgets are enforced by the
  runtime, not only by prompts.
- The user can pause, cancel, reprioritize, or inspect any child.
- Integration into the parent branch is always a separate reviewable action.

Background/scheduled work remains disabled until crash recovery, sandboxing,
budget enforcement, notification, and evaluation gates pass.

### 15. Observability, evaluation, and privacy

Maintain a structured local trace for every turn: runtime/model selection,
context manifest hashes and sizes, event timing, tool/approval lifecycle, token
usage, cost estimate/source, changed paths, commands/tests, errors, retries, and
final state. Prompt and file content are excluded from telemetry by default.

Diagnostic export is user-initiated, previewable, redacted, and can include
protocol events without repository content. Cloud telemetry is opt-in and uses
aggregated reliability metrics.

Release evaluation includes:

- Deterministic protocol/replay fixtures for each adapter.
- Patch application, stale-file, symlink, encoding, and large-output tests.
- Prompt-injection and secret-exfiltration adversarial suites.
- Cross-provider tool and context capability tests.
- Repository task corpus measuring completion, regressions, test pass rate,
  user correction rate, approval burden, latency, and cost.
- Crash/restart, database migration, runtime upgrade, and offline recovery tests.
- Windows/macOS sandbox, packaging, IME, accessibility, and high-DPI matrices.

### 16. Aegisy cloud contracts

Add authenticated endpoints for model catalog, runtime compatibility, short-lived
Agent tokens, usage, and optional remote job control. Agent tokens are audience-
restricted, expire quickly, and can be scoped to model IDs and project/session
correlation without granting account administration.

The desktop can operate against cached catalog and existing credentials during a
temporary account-service outage, but cannot invent updated limits, prices, or
capabilities. Provider errors retain upstream status/classification through the
gateway and AAP error model.

## Risks / Trade-offs

- [Scope expands into a full IDE] -> Ship milestone contracts independently and
  reuse Monaco, xterm.js, language servers, Git, Codex App Server, and ACP instead
  of recreating mature components.
- [Qt WebEngine increases installer size and memory] -> Gate it through a measured
  spike and retain a separate Tauri surface as a protocol-compatible fallback.
- [Codex or ACP protocols change] -> Pin versions, generate schemas, translate
  through adapters, retain replay fixtures, and support controlled upgrades.
- [Provider semantics differ] -> Use declared capabilities and portable session
  forks; never replay opaque reasoning state across providers.
- [Aegisy model metadata becomes stale] -> Sign, version, expire, and visibly mark
  cached catalogs; disable capabilities that cannot be verified.
- [Agent execution leaks secrets or obeys injected instructions] -> Enforce
  runtime policy outside prompts, label untrusted sources, redact secrets, and
  continuously run adversarial security tests.
- [User work is overwritten] -> Hash file versions, preview patches, isolate
  worktrees, preserve dirty state, and require approval for destructive Git.
- [Database or event journal corrupts] -> Append-only events, checksums, WAL,
  backups before migration, rebuildable projections, and read-only recovery.
- [Multi-agent work creates unreviewable changes and high cost] -> Defer release,
  isolate tasks, enforce budgets, limit concurrency, and require integration review.
- [Licensing or branding contamination] -> Keep a third-party inventory, legal
  review, NOTICE files, original Aegisy UX and protocol, and no Claude core reuse.
- [Background service becomes an attack surface] -> Keep the initial runtime a
  child process, authenticate IPC, bind locally, minimize privileges, and delay
  remote control until pairing and policy are complete.
- [One model is used for every role despite poor fit] -> Support evaluated role
  profiles but default to a simple one-model setup until added roles prove value.

## Migration Plan

1. Complete Milestone 0 spikes: AAP schema, Codex App Server adapter, ACP adapter,
   Qt WebEngine workbench, sidecar IPC/auth, sandbox feasibility, and packaging.
2. Introduce the workbench behind a disabled feature flag. Existing main window,
   chat dialog, profiles, and gateway remain unchanged.
3. Ship read-only Chat plus project/session browsing and event persistence to an
   internal channel. No workspace write tools are enabled.
4. Add single-agent Work mode with file reads, terminal, structured patches,
   approvals, diff, checkpoints, and Git status on test repositories.
5. Enable workspace writes for opt-in beta users after security and recovery
   gates pass. Preserve a one-click return to legacy Aegisy management pages.
6. Add Aegisy model capability catalog and model profiles; migrate existing active
   model selection into a default profile without deleting old configuration.
7. Add Skills/MCP/plugins through the unified trust surface, importing existing
   Aegisy configuration with a preview and reversible backup.
8. Add worktrees, commits, and role-specific models after Git and context evals.
9. Add child agents and background jobs only after their dedicated release gates.
10. Promote the workbench to the default landing surface only after usage,
    reliability, support, and parity criteria are met.

Rollback strategy:

- Feature flags can hide the workbench while leaving its database untouched.
- Sidecar/runtime updates are versioned and can roll back to the last compatible
  pair; database migrations retain a backup and forward-compatible event journal.
- Existing profile, gateway, secure-storage, and updater contracts remain
  authoritative until an explicit later OpenSpec deprecates them.
- A failed workbench migration or startup cannot prevent login or legacy
  connection management.

## Open Questions

- Does the Qt WebEngine spike meet signed installer size, IME, accessibility,
  startup, and memory budgets on both supported platforms? See
  [ADR 0001](../../../docs/adr/0001-embedded-webengine-go-no-go.md).
- Which Codex App Server versions and integration terms can Aegisy redistribute or
  require, and which enterprise client identity requirements apply? See
  [ADR 0002](../../../docs/adr/0002-codex-distribution-and-client-identity.md).
- Which ACP extensions are necessary for diffs, Git graph, model capability,
  child sessions, and background jobs, and should any be proposed upstream? See
  [ADR 0003](../../../docs/adr/0003-acp-extension-policy.md).
- What exact Aegisy model-catalog schema and signing mechanism can the website
  deliver, and which values are authoritative versus measured recommendations? See
  [ADR 0004](../../../docs/adr/0004-model-catalog-trust-and-authority.md).
- Should local Ollama/LM Studio models be routed through Aegisy policy or offered
  as an explicitly unmanaged offline provider profile? See
  [ADR 0005](../../../docs/adr/0005-local-model-provider-policy.md).
- Which native Windows sandbox combination provides acceptable filesystem,
  process, and network isolation without requiring administrator privileges? See
  [ADR 0006](../../../docs/adr/0006-windows-native-sandbox.md).
- What data retention defaults apply to terminal output, diffs, images, and local
  session history, and which enterprise policy controls are required? See
  [ADR 0007](../../../docs/adr/0007-local-content-retention.md).
- Should the first editor milestone include LSP, or is Monaco syntax plus build
  diagnostics sufficient until the structured patch workflow stabilizes? See
  [ADR 0008](../../../docs/adr/0008-editor-language-intelligence.md).
- What user-visible name will distinguish the product (`Aegisy Workbench`,
  `Aegisy Studio`, or another name) without implying compatibility guarantees not
  yet delivered? See [ADR 0009](../../../docs/adr/0009-public-product-name.md).
