## Context

Aegisy is currently a C++17 Qt Widgets application for Windows and macOS. The
main process owns login, secure token storage, account/model APIs, connection
profiles, a local gateway process, Skills, MCP configuration, desktop-client
integration, updates, and runtime status. The existing AI chat is a dialog that
calls model APIs but is not an execution Agent: it does not own a workspace,
terminal, structured file changes, Git state, approvals, durable execution
events, or crash-resumable task lifecycle.

The requested product is closer to a compact coding environment than another
settings page. It must support a persistent Agent surface on the left, Chat and
Work modes, projects, sessions, files, terminal, Git, model selection, Skills,
plugins, MCP, and eventually isolated subagents and background work. Building
all of that inside `MainWindow` would create a security and maintainability
failure. The UI, Agent execution, provider routing, and Aegisy cloud control
plane need explicit boundaries before feature implementation begins.

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

- Provide an Aegisy-owned coding experience with fixed Agent interaction beside
  the active workspace and a clear Chat/Work contract.
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
bootstrap-authentication guarantees. During the current task `3.3` milestone,
Qt launches the sidecar as a child and communicates over newline-delimited stdio.
That transport truthfully negotiates `local: true` with `authenticated`,
`encrypted`, and `peer_verified` all `false`. Stdio also remains appropriate for
controlled fixtures and runtime-adapter child processes; it is not evidence that
the authenticated production IPC target is complete.

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

Define AAP as versioned JSON-RPC 2.0 with generated TypeScript, C++, and Rust
types. It borrows proven primitives but remains Aegisy-owned:

- `Project`: trusted folder set, repository metadata, instructions, and policy.
- `Session`: durable conversation/execution lineage bound to a project.
- `Turn`: one user request and its terminal state.
- `Item`: typed user, agent, reasoning-summary, plan, command, file-change,
  approval, question, tool, diagnostic, usage, or artifact event.
- `Runtime`: one Agent backend instance and its negotiated capabilities.
- `Workspace`: local, sandbox, SSH, or future Aegisy-hosted execution target.

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
  compacted prefix plus the retained Journal tail. This checkpoint is internal
  recovery authority, not the user-visible current-Session snapshot required for
  retention-gap recovery; AAP must not expose it as Item content or claim that it
  completes reconnect.
- Bounded outbound queues, overload errors, cancellation, heartbeat, and
  backpressure.
- Idempotency keys for turn start, approval responses, writes, and background job
  submission.
- Server-initiated requests for approval, structured user input, credential
  refresh, and extension elicitation.
- Stable core namespace plus experimental namespaced methods. Unknown events are
  preserved for diagnostics and ignored safely by older clients.
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
Automatic production pruning remains disabled until a versioned current-Session
snapshot, structured retention-gap response, and client recovery flow exist.
The structured gap response is `timeline-retention-gap/0.1` on JSON-RPC `-32148`.
It publishes only requested anchors, the validated floor/head, and fixed recovery
metadata. It requires `timeline.snapshot.current` through `timeline/snapshot`, marks
that route unavailable until the snapshot stage lands, declares retained event
history incomplete, and forbids replay directly from the floor. Retained anchors
with substituted Event IDs remain drift failures. The v16 Sequencer checkpoint is
never serialized into this response.

The public current-Session snapshot requires a separate durable visible-state
projection rather than `session/read`: projected Items omit live `started`/`delta`
updates, do not bind one Public Timeline head, and the v16 checkpoint intentionally
omits Item content. The next schema stage therefore adds one durable floor-visible-
state snapshot at the exact retention-floor anchor. Advancing retention must commit
that sanitized visible state, the content-free Sequencer checkpoint, floor movement,
and exact prefix deletion in one SQLite transaction. Runtime materializes a public
snapshot by replaying the retained tail strictly after that floor through one fixed
Public Timeline head; events appended after that head do not enter the snapshot.

The future stable capability is exactly `timeline.snapshot.current` and its only
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
stable Schema, identities, and cross-page fixture now implement this wire contract,
but the Runtime method remains unadvertised until Store materialization and Qt atomic
replacement land. Automatic pruning remains disabled, and OpenSpec task `3.5` stays
unchecked.

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
  startup, and memory budgets on both supported platforms?
- Which Codex App Server versions and integration terms can Aegisy redistribute or
  require, and which enterprise client identity requirements apply?
- Which ACP extensions are necessary for diffs, Git graph, model capability,
  child sessions, and background jobs, and should any be proposed upstream?
- What exact Aegisy model-catalog schema and signing mechanism can the website
  deliver, and which values are authoritative versus measured recommendations?
- Should local Ollama/LM Studio models be routed through Aegisy policy or offered
  as an explicitly unmanaged offline provider profile?
- Which native Windows sandbox combination provides acceptable filesystem,
  process, and network isolation without requiring administrator privileges?
- What data retention defaults apply to terminal output, diffs, images, and local
  session history, and which enterprise policy controls are required?
- Should the first editor milestone include LSP, or is Monaco syntax plus build
  diagnostics sufficient until the structured patch workflow stabilizes?
- What user-visible name will distinguish the product (`Aegisy Workbench`,
  `Aegisy Studio`, or another name) without implying compatibility guarantees not
  yet delivered?
