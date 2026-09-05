# Aegisy Project Memory

Last updated: 2026-09-05 CST

## Mandatory First Step

Read this file completely before performing any repository operation. Update it
whenever architecture, release requirements, security boundaries, incident
conclusions, or major implementation status changes. Never store secrets here.

## Product Goal

Aegisy is an Aegisy website companion for turning account and service state into
a working local AI-tool environment. The active product prioritizes one-click
connection configuration, local environment detection and repair, gateway/profile
management, backup/restore, desktop enhancements, Chinese UX, Codex plugins,
custom Skills, MCP, diagnostics, downloads, and updates.

The optional integrated programming surface is Codex-only in the active roadmap.
Claude Code, Gemini CLI, and OpenCode remain supported configuration targets, but
Aegisy does not currently implement or advertise Claude/Gemini/ACP Agent runtimes.
The existing Agent Workbench, AAP, Store, editor, terminal, and Git foundations are
retained as reusable code and safety evidence; non-Codex adapters, full IDE
replacement, Agent-authored mutation, and background/multi-agent execution are
deferred unless a later product decision explicitly resumes them.

Active implementation order is companion-first: trusted website projection,
one-click local configuration/repair/rollback, Codex plugins plus custom Skills/MCP,
recoverable Chinese/desktop enhancements, the already bounded optional Codex
programming surface, and then companion-focused release evidence. Claude/Gemini
configuration adapters receive only compatibility work required by the one-click
flow; their embedded programming runtimes are not active implementation targets.

The detailed proposal, design, roadmap, research, specifications, and task list
live under `openspec/changes/build-aegisy-agent-workbench/`.

## Current Architecture

- Desktop host: C++17 and Qt Widgets. macOS and Windows are first-class targets.
- Existing product services: login/account/API key management, provider profiles,
  local gateway, tool installation, updater, diagnostics, and secure storage.
- Credentials: profile metadata uses `QSettings`; credential values belong in
  platform secure storage through `SecureStorage`, never ordinary logs/config UI.
  The Qt host also removes credential-bearing environment variables before
  launching `aegisy-agentd`; the sidecar client exposes no secure-storage/token
  value API.
  Windows SecureStorage saves now require `QSettings::sync()` plus `NoError` before
  caching success. Removal on macOS, Windows, and Linux clears the in-memory cache
  only after the platform backend confirms deletion; Windows removal also requires
  `sync()/NoError`. This closes false local durability reporting for the future
  encrypted configuration-backup root key. `loadEncryptedFresh` bypasses the process
  cache and distinguishes Found, Missing, Unavailable, and Invalid across Windows
  DPAPI/QSettings, macOS Keychain, and Linux Secret Service. Compatibility reads
  cache only Found. Cache bootstrap must use this typed result so a locked or broken
  backend can never be interpreted as first install.
- Companion website observation: the Qt host verifies the website account before
  requesting API Keys and binds account/Key responses to the current auth epoch,
  exact request URL, reviewed `aegisy.cc` HTTPS origin, manual redirect policy,
  JSON Content-Type, pagination, and byte bounds. It derives account/Key identities
  with domain-separated SHA-256 and caches only
  `aegisy-companion-config-projection/0.1` metadata under the account identity.
  The projection contains no token, raw account/Key ID, credential value/fragment,
  user info, or inferred model list and fixes configuration authority/applied false.
  Invalid or stale responses publish no raw Key signal. A complete valid response is
  transactionally staged by `CompanionCredentialBroker` into exact account/Key-
  derived SecureStorage slots; only opaque handles enter the projection, and
  cross-account/cross-Key resolution fails. ConnectWizard consumes only those
  candidates and resolves a credential on explicit test/model/save actions, never in
  widget item data. Its model query uses a unique request and exact auth/account/
  current-projection/Key/handle/platform/origin binding. Both website candidates and
  existing local Profiles receive only validated
  `aegisy-companion-model-projection/0.1` model IDs; Key/tool/projection/auth changes
  retire or invalidate late responses, and the wizard no longer consumes the global
  model signal. ModelsDialog also uses only sanitized active candidates and the
  correlated model result; it has no manual Key input, Key fragment, raw-Key signal,
  or global model signal. API-Key management now consumes an online-only strict
  management projection with safe Key/group metadata and random action-scoped
  handles; the raw inventory is cleared without publication. Chat consumes only sanitized candidates
  and correlated model projections. Its chat, image-Skill, and presentation-Skill
  calls enter ApiClient through exact companion request bindings; ApiClient
  revalidates auth/account/current projection/Key/handle/platform/origin and resolves
  the credential once from SecureStorage. Chat history schema 2 stores only the
  hashed website-Key identity and a bounded safe display name; legacy raw `key_id`
  values are ignored and never rewritten.
  Any terminal current-generation website Key transport/trust/type/pagination/
  projection/broker failure now clears all live configuration, usage, management,
  group, and website-model state and retires pending/active companion operations
  before publishing failure. Old-generation failures are inert. Website model state
  excludes local Profile and management Key-test results.
  `CompanionConfigurationCache` now provides the canonical v0.2 metadata/model
  cache with HMAC-SHA256, one SecureStorage authority envelope,
  Prepared/Committed recovery, highest reserved revision, strict A/B slots, typed
  secure outcomes, exact namespace scan, clock/high-water and Fresh/Stale/Expired
  views. A strict production SecureStorage adapter and one serialized worker persist
  live website configuration and ordinary website model observations off the UI
  thread. MainWindow renders all nine cache states as read-only status and never
  restores live authority. Cached bytes strip handles/credentials/raw IDs and fix
  all operational authority false. A strict presentation adapter now converts only
  the current generation/account View into handle-free DTOs. ConnectWizard, Models,
  and Chat render Fresh data or Stale Key metadata through roles disjoint from live
  authority; cached rows cannot query, test, save, send, run Skills, select a model
  for execution, resolve credentials, or write tool configuration.
- Profile credential binding: profile schema 7 requires a strict local UUID and the
  exact derived `profile/<uuid>/api-key` SecureStorage reference. QSettings cannot
  redirect profile read/update/delete to another secure-storage namespace. Display
  hints are domain-separated SHA-256 fingerprints rather than credential tails.
  Website-created Profiles persist only hashed account/Key/projection source
  identities. ToolManager now routes all Claude, Codex, Gemini, and OpenCode config
  backups through `ConfigurationBackupStore`: a per-tool SecureStorage-rooted 256-bit
  key, path-free slot-based AES-256-GCM v2 manifest, strict bounds/authentication,
  stable double-read capture, actual-file readback, complete restore prevalidation,
  encrypted safety snapshot, resumable legacy-v1 migration, four-state inventory,
  and manifest-identity-bound verified removal. Invalid/unavailable backup state
  blocks CLI configuration before mutation. This removes new plaintext backup
  payloads and migrates exact legacy records when secure storage is available;
  unverified legacy evidence remains preserved and blocks use. Complete one-click
  preview/profile compensation and cross-platform evidence remain open.
- Provider activation: Claude, Codex, Gemini, and OpenCode have independent
  profiles and activation state as local configuration targets. Only Codex is an
  active integrated programming runtime.
- Local model gateway: a supervised Node.js HTTP proxy bound to
  `127.0.0.1:43112`. It routes each tool to its selected Aegisy profile and must
  preserve streaming response bytes and backpressure.
- Agent runtime: Rust sidecar `aegisy-agentd`, communicating with the Qt host over
  the Aegisy Agent Protocol (AAP) on stdio JSON-RPC.
- AAP `0.1` initialization is a strict two-stage contract. `initialize` carries
  structured client version range, bounded identity/platform, stable capabilities,
  an exactly empty experimental declaration, the exact 4 MiB frame limit, and
  truthful local stdio security facts; Runtime returns the deterministic stable
  capability intersection, bounded Runtime/backend identity, and selected range.
  Business methods remain unavailable until the exact object-params `initialized`
  notification is consumed and fail closed when their capability is absent. Ready
  and recovery require the corresponding backend marker plus
  `permission.read-only`. Protocol versions compare as numeric `u64` pairs, request
  and notification params are objects, and success response IDs are non-null bounded
  ASCII graphical strings. Both peers enforce the 4 MiB boundary before writing;
  physical oversized input is drained without body parsing or echo, oversized
  responses become same-ID `-32005`, and an oversized notification closes the
  transport instead of being dropped. The default stdio transport is local and
  unencrypted but is now authenticated: every supervised launch passes a fresh
  256-bit bootstrap token through the sanitized environment and the sidecar
  requires the exact one-time `aegisy-bootstrap-auth/0.1` prelude as the first
  transport line before any AAP frame. OpenSpec `4.4` is macOS-verified, and the
  Windows named-pipe/bootstrap component passed in predecessor run `31426799633`;
  the current complete clean Windows workflow remains pending. The optional macOS
  Unix socket from
  `4.2` is owner-only, peer-verified, and authenticated by the same prelude
  after verification; peer verification alone is never treated as
  authentication. A sidecar started directly without `AEGISY_BOOTSTRAP_TOKEN`
  keeps the legacy unauthenticated mode for fixtures and developer launches.
  Windows named-pipe ACL/peer-validation implementation is present, and its dedicated
  E2E plus negative matrix passed in predecessor run `31426799633`. Task `4.3`
  remains open until the current complete clean Windows workflow is green; the
  predecessor component result is not installer, package, signing, or release evidence.
  Negotiated `runtime.heartbeat.out-of-band` now proves only local Runtime connection
  liveness through the independent stdio control reader. Qt sends one nonce-bound
  heartbeat every five seconds with a 15-second deadline. Expiry enters a separate
  Unknown state: ordinary pending work fails and new business requests are blocked,
  while the process, initialized control connection, confirmed Timeline, active Turn,
  and cancellation/steering/terminal-stop/shutdown controls remain available. Late or
  prior-process-generation responses are inert. Heartbeat reads no Store or Provider
  state, carries no timestamp/PID/permission, and grants no execution authority.
- Durable Workbench data: the Qt host passes `AEGISY_WORKBENCH_DATA_ROOT` to the
  sidecar, defaulting to the platform `AppDataLocation/workbench` directory. An
  explicit environment value remains a developer/test override; a standalone
  sidecar without either value keeps its bounded in-memory fallback.
- User terminals: runtime-owned macOS PTY and Windows ConPTY implementations built
  on pinned `portable-pty` 0.9.0 and exposed as session/project-scoped AAP
  operations, with a shared foreground/named-background lifecycle registry. macOS
  and the Qt/xterm.js flow are runtime-verified. The Windows component passed in
  predecessor run `31426799633`, while current run `31572128979` failed the same
  post-interrupt test before later Windows gates. A deterministic fixture defect is
  repaired by tracking DSR cursor queries across the complete terminal session and
  replying once per newly observed query. Production ConPTY behavior is unchanged;
  a complete green current Windows workflow is still required before the milestone
  is complete.
- Initial Agent adapter: installed Codex CLI launched as `codex app-server
  --stdio`, translated into stable AAP sessions, turns, and timeline items. The
  adapter requires pinned `codex-cli 0.144.5` and rejects other versions before
  launch; its generated v2 schema is checked in under `agent-runtime/aap-schema`.
  When an adjacent `aegisy-artifact-manifest/0.1` is present, Runtime resolves
  Codex only from that manifest, ignores `AEGISY_CODEX_PATH`, binds the exact
  Runtime/adapter versions, paths, single-link file identities, and SHA-256 values,
  requires the exact `.exe` adapter image on Windows, and revalidates the fixed
  manifest identity before version probing and App Server spawn. A malformed or
  drifted present manifest never falls back to developer
  discovery. Manifest absence still permits developer resolution until signed
  packaging can require the manifest without breaking current developer builds.
  The pin and schema-driven lifecycle fixtures pass the full Rust workspace test
  and Clippy gates; cross-platform binary contract and upgrade/rollback evidence
  remain open under OpenSpec task `7.1`.
  AAP `runtime/health` reports the Codex child process state, PID, exit code, restart
  recommendation, and a content-free stderr summary (bytes, newline count, redacted
  line count, and stable last class) without exposing stderr text or environment
  values. Adapter startup now uses a 15-second initialize timeout, retries only
  transient EOF/transport/timeout failures, and caps startup attempts at three with
  a bounded backoff. AAP `runtime/restart` can replace an exited/unavailable
  adapter, preserves session bindings, and refuses to restart while a model turn or
  user terminal is active. Qt polls `runtime/health`, exposes exited/unavailable
  state, and provides a guarded `重启 Codex` action. Full crash-loop recovery UI and
  cross-platform recovery evidence remain under task `7.2`.
  AAP `runtime-degradations/0.2` binds its content-free feature states to the exact
  vendored Codex `0.144.5` v2 schema hash, complete `87/68/18`
  request/notification/item classification, and a deterministic matrix identity.
  It reports read-only Agent mutation, metadata-only provider items, blocked
  provider delete/compact, runtime-only desktop gaps, and the
  background/multi-agent/unattended-write release gates so clients do not simulate
  unavailable behavior. The autonomy entries are
  `availability=not-advertised`, `stable_enabled=false`, and
  `override_available=false`, with bounded missing-task IDs; no hidden stable
  or experimental switch can enable them.
  Provider thread list/read `0.2` return only bounded content-free metadata and
  domain-separated identities; names, previews, cwd/path, model-provider/source
  text, raw items, and content are omitted. Unknown Codex notifications are retained
  only as bounded method SHA-256/count diagnostics in `runtime/health` and never
  mapped into ordinary Timeline Items. User-input server requests receive a fixed
  unsupported error rather than fabricated empty answers.
  Qt requests the degradation snapshot after initialization and renders a compact
  capability status. Not-requested, pending, invalid, or failed snapshots disable
  all new-Turn paths, including queued Session creation, while an active Stop remains
  usable. Live and replay Timeline inputs share strict item/event validation;
  unknown or cross-bound events, malformed pages, duplicate identities, and
  kind/role drift are inert, and replay changes UI only after whole-page validation.
  No provider delete/compact action is exposed. Full capability negotiation and all
  runtime-only desktop surfaces remain open under task `7.9`.
  The internal `docs/AAP-PROTOCOL-GUIDE.md` records the stable AAP handshake,
  lifecycle/error/cancel/degradation/replay examples and the current read-only
  security boundary. It is intentionally an internal guide tied to checked-in
  fixtures, not a public compatibility promise. The checked-in
  `aap-schema/fixtures/codex-thread-lifecycle.jsonl` covers the
  current request sequence and `codex-turn-metadata.jsonl` covers usage/plan/diff
  notifications; both are tested for JSON validity and credential-shaped content
  absence. Partial-stream, approval/denial, reconnect, compaction, and provider-
  failure fixtures are present; final Qt lifecycle projection is complete under
  task `7.10`.
- Current Agent security boundary: Agent/Codex is read-only. User-initiated editor
  saves are separately allowed only inside the canonical opened project root.
- Codex file-change Proposal foundation: pinned `0.144.5` fileChange events now
  follow the strict volatile-patch/Started/approval/decline/resolved/declined
  lifecycle. `patchUpdated` is only volatile parsed-so-far preview and supplies no
  Proposal authority; canonical `item/started` may legitimately differ in path and
  diff shape, while `item/completed` must exactly repeat Started. Required lifecycle
  timestamps are monotonic, pending plus Started Items share a 256-Item limit, and
  retained file-change state is capped at 16 MiB per Turn. The Runtime compiles
  add/update/delete/pure-rename data into the shared `WorkspaceEdit` preview without
  writing the project. Workbench schema v19 commits one immutable
  `workspace-edit-proposal/0.2`, exact content/diff Blob references, one completed
  metadata-only `file-change` Item, its internal event, the unchanged Proposal event,
  the exact public `item.completed` envelope, and an immutable
  `workspace-edit-proposal-reference/0.1` binding in one SQLite transaction before
  the adapter may send its fixed read-only decline. Proposal admission binds Session,
  durable Turn, project/root/filesystem, read-only Runtime backend thread, provider item/time,
  normalized operations, overlap baseline, preview, and domain-separated identities;
  mutation, user approval, and apply authority are fixed false. Store failure sends
  no decline and fails the Turn closed; a post-commit caller fault never removes
  committed Blob references, and Proposal/Blob corruption quarantines only the owning
  Session. The durable reference binds the enclosing Item and exact public envelope,
  survives verified Session projection rebuild, and remains valid after reviewed
  Journal pruning only when its sequence is at or below the validated retention
  floor. An uncertain post-commit caller failure retries against the exact same
  envelope and cannot create a second graph. The v18-to-v19 migration preserves
  legacy Proposal bytes without fabricating Timeline history. New `0.2` records
  include aggregate and ordered per-file summaries whose
  complete diff statistics and text formats are rechecked from persisted Blobs;
  exact legacy `0.1` bytes and identities remain readable as explicitly incomplete.
  Negotiated `workspace.edit.proposal.read-only` plus `permission.read-only` exposes
  Session-scoped latest/exact Proposal reads and Proposal-owned 64 KiB artifact pages
  with typed domain-separated identities. Every public mutation, approval, and apply
  authority field is fixed false. Qt negotiates this capability and restores strictly
  validated durable Proposals into Changes across Work Session recovery and reconnect,
  with foreground auto-open, background unread state, revalidated disconnected caches,
  generation-bound artifact paging, and no Approval/Apply controls. Qt strictly
  validates the Timeline reference and exposes a read-only `View changes` action
  whose exact Proposal read cannot replace the latest cache on drift, race, or
  cross-binding failure. Genuine approval, checkpoint/apply, and rollback authority
  remain absent.
- Durable mutation acknowledgement: stable capability
  `session.mutation-acknowledgements` exposes Session-scoped list and exact-anchor
  consume routes. Schema v20 stores one metadata-only `turn-start` operation keyed by
  Session, idempotency key, and SHA-256 request fingerprint. Runtime reserves it
  before dispatch; equivalent retries return the original operation/Turn and
  conflicting fingerprints fail without dispatch. Accepted `turn.started` and
  terminal Timeline anchors bind atomically with projection/Journal transactions via
  revision CAS. Startup converts uncertain accepted rows to
  `reconciliation-required`, which cannot redispatch. Qt consumes only after exact
  Session/Turn/sequence/Event-ID validation, accepted before terminal; drift,
  tampering, unavailable/read-only recovery, and cross-Session access freeze the
  affected Session. The ledger contains no prompt, context, provider body, result,
  permission, approval, or execution authority. Workbench database schema v24 builds
  on the historical v22 complete-source graph for validated approval, file-write,
  Git-mutation, and job-submission metadata while leaving the schema-v20 Turn ledger
  and AAP contract unchanged. A new tuple still atomically commits the exact typed
  source, derived reservation, and internal source event. The crate-internal Store
  may now atomically bind one exact terminal outcome row and internal outcome event
  while compare-and-swapping a `present` reservation from `reserved` revision 1 to
  `terminal` revision 2. Exact source and outcome replay are zero-write; peer outcome
  drift conflicts after write-lock reclassification. Outcome admission and persisted
  graph validation enforce
  `reserved_at_ms <= observed_at_ms <= recorded_at_ms`; a pre-reservation observation
  is rejected with zero writes, and persisted drift enters whole-Store read-only
  recovery. Valid histories are reserved
  `[source]`, terminal `[source, outcome]`, reconciliation-required
  `[source, reconciliation]`, and migrated legacy `[]`. Archive, pending deletion,
  project/root/Turn scope, startup integrity, migration, and purge remain fail closed.
  Schema v24 adds a separate crate-internal, content-free reservation consumption
  ledger. Core reservation revisions `r1/r2` and consumption revisions `c0/c1/c2`
  are orthogonal. Valid consumption histories are exactly `[]`, `[source]`,
  `[source, terminal]`, or `[source, reconciliation-required]`; resolution cannot
  precede source. Exact retry returns the first immutable receipt/time before write
  admission, while an `IMMEDIATE` CAS rechecks peer commits, core/consumption
  revisions, evidence, Session/archive/deletion, and project/root/Turn scope.
  Startup preserves `c0/c1` while moving an open core graph to reconciliation `r2`;
  the transition time is clamped to any existing `c1` source consumption time so a
  wall-clock rollback cannot invalidate a legitimate receipt. Startup then performs
  a bounded semantic scan of every consumption row. Receipt, anchor,
  evidence, phase, time, or authority drift, including hash-consistent r2 time
  forgery, enters whole-Store read-only recovery. The v23-to-v24 migration publishes
  the exact v23 backup and creates an empty ledger without fabricating receipts or
  moving internal/Public sequences; purge deletes consumption dependencies first.
  Complete deletion of a consumption row remains indistinguishable from legitimate
  `c0`, so v24 is not anti-deletion or strict at-most-once authority.
  The graph grants no dispatch, mutation, Approval, or execution authority. No
  production producer, external caller consume/CAS route, AAP/Qt method, recovery
  consumer, or dispatch path uses it; Agent/Codex remains read-only.
- Workspace filesystem: the sidecar enforces canonical project roots, denies
  sensitive paths and symlinks, honors ignore rules, preserves UTF-8/BOM and
  LF/CRLF, uses revision checks, and performs atomic user saves.
- Editor: trusted local Monaco 0.55.1 inside Qt WebEngine/WebChannel, with a tested
  `QPlainTextEdit` fallback. The bundle blocks remote access and external
  navigation. Native tabs own corresponding Monaco models.
- Terminal UI: trusted local xterm.js 6.0.0 with FitAddon 0.11.0 inside the same
  remote-blocked Qt WebEngine/WebChannel boundary, with a tested native output
  fallback, native clipboard bridge, generation-aware restart, and bounded-tail
  reattachment after a renderer restart.
- Language intelligence: sidecar-owned LSP processes use bounded stdio framing and
  expose normalized definition, reference, diagnostic, status, start, and stop
  operations through AAP. The Qt UI never consumes native LSP messages directly.
- Updates: Sparkle on macOS and WinSparkle on Windows. Update feeds and installer
  signatures are platform-specific.

## Current Workbench Status

- Product direction reset (2026-08-23): section 0 of the linked OpenSpec is now the
  active delivery plan for the Aegisy companion control center. Existing Workbench
  sections 1-24 are retained as implementation history/long-horizon reference and
  are active only when they support the companion workflow or bounded Codex surface.
  Claude/Gemini/ACP Agent adapters, full multi-provider Agent routing, full IDE
  replacement, Agent-authored mutation, and background agents are deferred and
  unavailable. This is a scope reduction, not a rollback of completed safety work.
- OpenSpec now reports 78 of 247 checkbox rows complete and 169 unchecked. Task IDs
  `12.5`, `12.7`, and `12.8` are duplicated, so the unique-ID baseline is 78 of
  243 complete and 165 unchecked. A 2026-08-07 evidence audit corrected 24 checked
  rows covering 20 unique task IDs whose own notes or verification gates still say
  remaining, pending, deferred, or keep unchecked. This was a task-status correction,
  not a functional regression or code rollback; all partial implementation and
  verification evidence remains intact. Partial foundations are not release
  completion until their AAP/Qt, persistence, security, and cross-platform evidence
  gates pass.
- Intensive documentation session on 2026-07-31 produced 27 commits, 45+ documents,
  and ~8000 lines covering foundational contracts, architecture decisions, design
  documents, development tools, and comprehensive guides. Session utilized 25+
  parallel agents for maximum throughput.
- OpenSpec change `modernize-ui-and-runtime-status-bar` completed all 20 tasks
  and was archived on 2026-07-31.
- OpenSpec tasks `1.1`, `1.2`, and `1.5` have foundational documents created and
  await stakeholder approval.
- ADRs 010 and 011 completed: ACP extension support (deferred), editor language
  intelligence (sidecar-owned LSP, accepted).
- Development infrastructure enhanced with scripts for status tracking, testing,
  dependency checking, cleaning, and commit analysis.
- Comprehensive documentation added: workbench status, test coverage analysis,
  CI/CD recommendations, contributing guide, quick reference, release checklist,
  troubleshooting guide, security audit checklist, code review checklist, roadmap.
- 25 background agents still working on additional design documents and ADRs.
- OpenSpec task `4.1` is complete. `agent-runtime` is a locked two-crate Rust
  workspace with formatting, unit/integration tests, strict all-target Clippy, and a
  Thin-LTO/single-codegen-unit/stripped Release profile. Pinned `cargo-deny 0.19.9`
  plus `deny.toml` fail closed on missing/wrong tooling or advisory database failure,
  deny RustSec advisories, yanked/wildcard dependencies, unknown registries, and Git
  sources, and restrict licenses to the reviewed SPDX set present in the lockfile.
  Existing transitive duplicate versions remain visible warnings without skip
  entries. Repository Rust CI and Windows packaging CI run locked quality, Release,
  and audit gates. CMake now passes `--release` and bundles `target/release` for a
  Release desktop configuration instead of silently packaging the debug sidecar;
  Debug developer builds retain `target/debug`. An isolated macOS Release build
  produced a byte-identical arm64 sidecar in the app bundle. The final gate passes
  the dependency audit, Rust formatting, strict Clippy, locked Release build, 1050
  Rust tests with one explicitly ignored live Codex fixture, and all 23/23 serial
  Release CTests. Windows named pipe and one-time bootstrap authentication remain
  tasks `4.3` and `4.4`; the completed `4.2` socket is peer-verified but is not an
  authenticated channel.
- OpenSpec task `4.2` is complete. macOS now has an explicitly selected per-launch
  Unix-domain-socket transport; stdio remains the default. The Rust sidecar opens
  private parent/endpoint directories through no-follow descriptors, rejects
  extended ACLs, creates `0700`/`0600` objects, bounds accept time, revalidates its
  supervising parent around same-UID/exact-PID peer verification, and constructs no
  Runtime, Store, or Codex adapter before that boundary. Qt independently verifies
  the exact sidecar UID/PID and binds the proof to one process generation; socket
  ingress, writes, and initialize require that proof. Selected-socket failure never
  falls back to stdio. Security failures share a generation-owned terminate/kill/
  reap path and retain the first specific error. Rust and Qt cleanup are identity-
  bound and quarantine-based: recorded launch objects can be removed, including a
  matching sidecar quarantine interrupted by termination, while replacement or
  uncertain objects are preserved. The strict handshake union reports
  `transport: unix-domain-socket`, `local: true`, `peer_verified: true`, and keeps
  `authenticated`/`encrypted` false until `4.4`. Local evidence passes 11 focused
  Rust socket tests, all 24 handshake Schema tests, real Qt-to-Rust initialization
  and wrong-PID rejection, 20 repeated socket E2E runs, 1062 Rust tests with one
  explicit installed-Codex live fixture ignored, strict Clippy, locked Release
  build, and all 24/24 desktop CTests. This is macOS evidence only and grants no
  Agent/Codex mutation authority.
- OpenSpec task `4.3` has an implementation-present, evidence-pending slice. The
  Rust sidecar creates a first-instance Windows byte-mode named pipe with a protected
  `D:P(A;;GA;;;<TokenUser SID>)` DACL for the exact current token user,
  non-inheritable handle, remote-client rejection, full canonical UTF-16 name bound,
  and bounded nonblocking accept using the documented `PIPE_NOWAIT` result/error
  distinction. It retains a query/synchronization handle to the supervising parent,
  revalidates PID, creation time, and liveness around accept, and requires the client
  PID plus creation time to match before constructing Runtime/Store/Codex.
  Qt has an explicit `VerifiedWindowsNamedPipe` mode, passes only a randomized
  `aegisy-agent-*` name through the sanitized environment, connects through
  `QLocalSocket`, verifies `GetNamedPipeServerProcessId` against the exact supervised
  sidecar generation, and gates ingress, writes, and initialize on that proof.
  Every local-socket connection attempt owns a fresh `QLocalSocket` and a
  monotonically increasing attempt epoch. All ready-read, connected, error, and
  disconnected callbacks capture the socket, process generation, and epoch and are
  inert unless all three still identify the current attempt. The peer proof is bound
  to both generation and epoch; retiring a socket clears that exact proof before
  disconnect/abort/deferred deletion, so a delayed old callback cannot read bytes,
  clear a new proof, schedule reconnect, terminate the current sidecar, or send
  initialize.
  Selected-pipe failure does not fall back to stdio. Stable AAP now includes the
  generated `windows-named-pipe` transport union with `local=true`,
  `peer_verified=true`, and `authenticated=false`/`encrypted=false`; this is still
  peer verification, not the `4.4` bootstrap authentication boundary. The Windows
  validation workflow builds and runs the named-pipe E2E. That test source now
  covers exact security facts, protected current-token-user DACL inspection,
  stop/restart process generation and endpoint rotation, retired-endpoint isolation,
  supervising-parent exit, selected-pipe failure against a fake sidecar capable of a
  valid stdio handshake, malformed names, same-name collision, remote-form rejection
  after a matching protected current-user DACL control without the rejection flag
  proves host route availability, wrong-client-PID rejection, Qt wrong-server-PID
  rejection before initialize, no stdio fallback, and bounded cleanup.
  Validation runs in a read-only GitHub Actions job even when the release version is
  already published. Installer construction remains a separate main-only path, and
  only a minimal dependent publication job receives `contents: write`; a reused
  version is validation evidence only and never installer evidence. Release-version
  reuse checks the canonical installer locations, Windows appcasts, and matching
  tags. The Qt installer Action is pinned to a full reviewed commit. The obsolete
  global build-marker branch is no longer produced by the workflow.
  Local evidence passes Schema generation/freshness, 24 handshake tests, strict
  Clippy/formatting, the complete Rust workspace, the Release desktop build, all
  24/24 macOS desktop CTests, strict OpenSpec validation, and diff checks. A minimal
  extracted Windows API crate passes Windows-target check, test compilation, and
  strict Clippy, but the full cross-target build is blocked on this macOS host by
  native C dependencies. The platform-neutral Qt callback fixture now
  covers directly delivered old-generation and old-attempt signals, queued signals
  from a retired/deferred-deleted socket, peer-proof preservation, absence of
  termination/handshake state changes, and actual stale socket bytes remaining
  unread. The focused Runtime environment target passes ten consecutive repetitions;
  the complete desktop build and all 25/25 serial macOS CTests pass. After adding the
  Windows-only endpoint, remote-form, and wrong-server-PID assertions, an independent
  review of commit `0f36ae1` found four test-evidence defects: the fake sidecar path
  crossed the ANSI environment boundary in the required Unicode checkout; the
  missing-pipe case expected process reconnect exhaustion after six seconds even
  though production performs same-generation endpoint retries until its 60-second
  startup deadline; any remote-form error counted as rejection; and cancelled
  overlapped connect state could outlive its stack storage. The corrected test uses
  `SetEnvironmentVariableW` and exact Runtime-path assertions, shortens only the
  active test timer while requiring multiple endpoint attempts and zero process
  reconnects, requires `ERROR_ACCESS_DENIED` plus an independently observed
  `PIPE_REJECT_REMOTE_CLIENTS` flag, and drains exact cancellation with
  `CancelIoEx`/`GetOverlappedResult` before cleanup. The complete local desktop build
  and focused `agent_runtime_environment`, `agent_runtime_macos_socket`, and
  `windows_packaging_policy` CTests pass after the correction. The corrected
  named-pipe E2E later passed inside clean Unicode Windows run `31426799633`, while
  that run failed three separate Qt CTests. This is predecessor component evidence,
  not a complete green Windows workflow. Keep `4.3` unchecked until the current
  complete workflow passes.
  The Rust admission boundary now exposes a private injectable process-identity
  verifier for deterministic tests. Production still queries the real Windows
  process handle and creation time, while the focused negative fixture proves that
  an otherwise live client with the same PID but a different creation time is
  rejected before `VerifiedNamedPipe` construction and before Runtime/Store/Codex
  creation. The predecessor named-pipe E2E executed this and the broader negative
  matrix on Windows; the current complete workflow still requires a green rerun.
- OpenSpec task `3.1` is complete. `agent-runtime/aap-schema` is now an explicit
  private package with independent package/wire/provider versioning, one stable
  registry, and one experimental registry. Stable AAP `0.1` is additive-only and
  binds its version directory, ordinary package-local file, and canonical `$id`;
  the experimental registry is empty and wire-unavailable. The package gate rejects
  path escape/symlinks, duplicate identities, registry/Schema drift, invalid Schema,
  and stable references to experimental. This structure grants no experimental
  capability and does not complete three-language generation under `3.10`. The
  package gate passes 3/3, handshake Schema passes
  23/23, and the complete Rust workspace passes 977 tests with one explicitly
  ignored live Codex fixture plus strict Clippy and formatting. The complete
  desktop build and CTest suite pass 20/20, and strict OpenSpec validation passes.
- OpenSpec task `3.2` is complete. Stable AAP `0.1` additively registers
  `core.schema.json` while preserving the transport registry contract. The file
  is a reusable definition library rather than an aggregate wire message and
  covers thin Project/Session, durable/navigation projections, separate Turn
  lifecycle and start acknowledgement, transport-aligned Item/history Item,
  live/search/replay Runtime, project-root Workspace Git state, non-authorizing
  Approval acknowledgement, content-free Error, typed Usage, command-output
  Artifact read, and negotiated Capability sets. The fixture catalog validates
  each definition independently, and a black-box Store Runtime fixture checks real
  initialize/project/session/turn/Timeline output against the registered definitions.
  The package gate preserves every public core enum and security-relevant bound as
  an additive compatibility baseline and cross-checks shared Runtime/Backend and
  live/history Item boundaries against transport. Runtime now validates final
  Session titles before backend dispatch, preserves filesystem identity in memory-
  only and Store-backed project navigation/root projections, rejects same-path
  directory replacement, and rejects Usage values and evidence outside the JSON-
  safe integer range. Approval/user decision,
  Agent mutation/execution, generic Artifact, experimental, dedicated-worktree,
  remote, and Windows release authority remain absent. Core-domain generation is
  now a partial `3.10` slice; transport generation and complete consumer migration
  remain open. The final `3.2` local gate passes
  994 Rust tests with one explicitly ignored live Codex fixture, strict Clippy
  and formatting, the complete desktop build and 20/20 CTests, JSON parsing,
  strict OpenSpec validation, and `git diff --check`.
- OpenSpec task `3.10` now has partial core-domain and Transport generation slices.
  `core.schema.json` deterministically produces checked-in Rust, TypeScript, and
  Qt/C++ types plus strict definition validators. Generation rejects unknown or
  unsupported Schema keywords, dialects, local-reference combinations, fractional
  number shapes, unbounded integers, normalized generated-name collisions, and
  property-bearing open objects rather than silently widening or narrowing a type.
  Rust, TypeScript, and Qt/C++ independently enforce JSON-safe integers,
  Unicode-scalar strings and object keys, and the recursive ItemData root limit of
  depth 16 and 4,096 aggregate value nodes.
  A reviewed fixture map binds the complete positive catalog to the canonical
  identity
  `9801 94a27009b9c2439cef5a31f3078eacd3265df24a47c75bffb0c59d75f87d7f11`.
  A separate bounded 43-case positive/negative corpus includes exact depth and
  node-count boundaries plus lone-surrogate DTO strings, ItemData strings, and
  ItemData keys. Materialization preserves raw `value_json`, each runtime parses
  each case independently, and the common decision identity is
  `43 4d606e318e836e001f1cf9ec69d8b5ff558cc76158ed61c9862b92e4399b94ce`.
  CMake requires Node and Cargo for test builds, compares all three independent
  fixture/corpus runners, and uses native Qt arguments for Unicode paths. Generated
  Rust lives inside `aegisy-aap`; `cargo package -p aegisy-aap --allow-dirty`
  verifies the packaged crate, and `.gitattributes` fixes gate inputs/outputs to LF.
  Stable `aap.schema.json` now independently generates checked-in Rust, TypeScript,
  and Qt/C++ request/response/notification types plus raw definition/root validators.
  Stable `0.1` keeps generic `result` and `error.data` as true Schemas and
  `jsonRpcError.code` as an unbounded mathematical integer. The shared lossless
  parser profile therefore preserves arbitrary-precision number lexemes, accepts
  mathematical integer forms such as `1.0`/`1e0`, and rejects a leading BOM,
  invalid UTF-8, duplicate decoded keys, unpaired surrogates, frames above 4 MiB,
  depth above 128, or more than 65,536 JSON nodes. Canonical JSON sorts object keys
  by UTF-8 bytes and normalizes numbers without a floating-point round trip.
  A reviewed method registry binds every root dispatch and generic fallback. The
  101-definition fixture identity is
  `29903 d2961275431323f968bd18c4d8c2535cb8b05bda003ff0dea97f6e73be124757`;
  the 72-case parser/Schema decision identity is
  `72 f0ce6bdc14c815b2b80b273126da8b20a80ec47371d39128c7e2155246f60404`.
  Node oracle, generated TypeScript, Rust, and generated Qt/C++ reproduce the
  applicable identities. TypeScript arbitrary-precision numbers expose the declared
  read-only lexical/canonical/integer view only through parser-created, privately
  branded objects; structurally similar or property-copied objects cannot be
  canonicalized as Transport numbers. CMake compiles the standalone C++ Runtime and
  generated public-API runner with warnings denied and registers both in CTest. The
  private Schema package includes only the required
  `runtime/cpp/aap_transport_runtime.h` and `.cpp` production dependency pair
  alongside generated C++; test-only Runtime sources are not published. Both
  `generate:check` and the aggregate CTest invoke an exact 47-file
  `npm pack --dry-run --json` inventory gate, so generated C++ cannot be packed
  without its Runtime dependencies.
  The reviewed Transport registry has SHA-256
  `b90f2572b61f4e6c75548b5655cd2374b469231e282e5dd1a3e6f9f9da09953c`
  and generates 14 exact method entries plus six method-bound typed-error entries.
  Every request binds its request, success wrapper, and success-result definition;
  every root `allOf` condition must be classified as method or typed-error dispatch
  before the generic validator may remove it. Generator negative tests execute the
  semantic candidate validator before the reviewed-hash gate, so definition swaps,
  result drift, duplicate errors, optional or ambiguous discriminators, and
  unclassified root conditions cannot hide behind checksum rejection.
  Generated Rust dispatch parses each raw frame once with the lossless Transport
  parser and distinguishes parse errors with offsets, invalid generic envelopes,
  invalid known messages, and local validator unavailability. Known methods and
  known typed-error discriminators cannot fall back to generic handling; unknown
  methods and discriminators remain forward-compatible. Response dispatch consumes
  one indivisible pending context containing the exact request ID, method, and
  optional typed-error request identity. A wrong or null response ID is unmatched
  and cannot retire that context. Subscription typed errors additionally require
  the exact method-bound stage (`subscribe`, `sync`, `snapshot`, or `activate`) and
  request identity. `UnknownDefinition` and validator compilation failure become
  local `ValidatorUnavailable` and must never be reported to a peer as `-32600` or
  `-32602`.
  Generated Rust Transport validation now parses the embedded Schema once and
  caches the root plus each of the 101 definition validators independently with
  thread-safe `OnceLock` state. Concurrent first use of one definition compiles
  exactly once. Public non-exhaustive errors preserve parser classes and offsets
  separately from `UnknownDefinition`, `InvalidValue`, and local
  `ValidatorUnavailable` failures. Production consumers must treat
  `ValidatorUnavailable` as a local fail-closed implementation fault rather than
  reporting it to a peer as `-32600` or `-32602`.
  The production Rust stdio consumer now uses the generated lossless ingress path:
  one parse, generic-envelope validation before queue admission, generated method/
  kind classification, and generated known-definition validation at the existing
  Runtime/OOB policy boundary. Saturated generic-valid frames retain `-32004`
  precedence before known kind/parameter validation. Generic or per-definition
  validator unavailability emits no peer response, claims no request ID, performs no
  Runtime/Store side effect, and closes later queued dispatch through a linearized
  mutex-backed transport fault gate.
  The final Rust-ingress local gate passes generator negative/freshness tests, Rust
  formatting, `1018` workspace tests with one explicitly ignored live Codex fixture,
  strict Clippy, Cargo packaging, the complete desktop build and `23/23` CTests,
  strict OpenSpec validation, and `git diff --check`.
  Keep `3.10` unchecked: the production Qt consumer now uses the generated
  lossless ingress, parsed-message dispatch, indivisible pending context, and safe
  projection. The Windows validation job is configured to check out directly under
  `windows-验证-源码`, prove that the path contains non-ASCII characters and
  the Git checkout is clean, then run the complete Release build and unfiltered
  CTest suite from that directory. This workflow change has not yet completed on a
  clean Windows runner, so it is configuration evidence only. The
  `jsonschema` 0.48.5 Rust dependency also has an internal exponent-materialization
  limit at 1,000,000; this is an implementation limit, not permission to narrow
  stable `0.1`. A future reviewed AAP `0.2` may define JSON-safe generic integers
  and remap current `-321xx` application errors outside the JSON-RPC reserved range.
  These slices grant no capability, permission, Approval, mutation, execution,
  experimental, remote, or Windows release authority.
- OpenSpec task `3.3` is complete. The stable Schema, Rust Runtime/stdio daemon, Qt
  client, lifecycle fixtures, internal guide, design, and delta spec now share the
  structured two-stage AAP `0.1` handshake, numeric range negotiation, deterministic
  stable capability intersection, per-method capability enforcement, fixed 4 MiB
  bidirectional frame limit, strict envelope/ID/params rules, and fail-closed
  disconnect cleanup. This does not complete event replay, authenticated IPC, or
  Agent mutation authority; tasks `3.5`, `4.3`, and `4.4` remain open.
- OpenSpec task `3.4` is complete. Stable `timeline-event/0.1` envelopes now use
  Session-local contiguous positive sequences, non-decreasing positive millisecond
  timestamps, exact Turn correlation, explicit terminal states, and contiguous Item
  revisions. Rust and Qt independently reproduce the domain-separated SHA-256 Event
  ID from fixed-order canonical JSON; mathematical JSON integers normalize to the
  shortest decimal integer, while fractional or out-of-safe-range numbers fail
  closed. The Runtime owns Turn/Item lifecycle sequencing and closes stdio if a live
  terminal event cannot be sequenced after durable terminal persistence. Qt validates
  A/B/A interleaved Sessions independently, advances background Session state without
  visible-Session UI contamination, accepts generic 64-byte Item kinds, and enforces
  the exact 4 MiB frame boundary. Qt mirrors Runtime terminal semantics: structured
  failure retains accepted `started`/`delta` partial Items, while completed and
  interrupted terminals reject those open streams; repeatable `updated` snapshots may
  remain at terminal, and a revision-one atomic `truncated` marker is valid. Durable
  public journaling, fixed-watermark catch-up, structured retention-gap snapshot
  recovery, live subscription, and bounded reconnect are implemented under `3.5`;
  durable Turn-start acknowledgement is implemented; complete Windows
  reconnect/runtime evidence remains open. Agent/Codex remains read-only.
- OpenSpec task `3.5` now has an end-to-end fixed-watermark catch-up slice. Workbench
  schema v16 stores exact validated `timeline-event/0.1` envelopes behind one
  compare-and-swap cursor per Session, restores Sequencer state page by page, and
  serves exact sequence/Event-ID anchors through negotiated `timeline/sync` below the
  4 MiB frame limit. Normal durable Codex Turn start, sanitized Item, command
  Item/Blob, and completed/failed/interrupted terminal paths commit projection,
  internal event, and Public Journal in one SQLite transaction. Prepared Sequencer
  tickets reject stale baselines, and projection timestamps use the Sequencer's
  non-decreasing timestamp when the system wall clock moves backward. Qt freezes
  only the affected Session on a gap, validates all fixed-watermark pages privately,
  publishes the complete batch atomically, drains bounded queued live events, retries
  after disconnect, and syncs new/resumed/history Sessions before enabling a Turn.
  Aggregate pending state is capped at 10,000 events/64 MiB and tracking fails closed
  at 10,000 Sessions. Preview now stages its synthetic six-event lifecycle through a
  cloned Sequencer and commits the Turn, sanitized user/agent Items, internal events,
  terminal state, and all six Public Journal events in one SQLite transaction; only
  then do the live Sequencer and in-memory Items advance. Adapter, transport,
  protocol, and persistence compensation now commit the exact durable Error Item,
  Turn Trace, terminal state, internal trace/terminal events, and public terminal
  event in one transaction before notification. An existing Trace cannot receive a
  later Public Journal repair, and the Trace-only Store helper is compiled only for
  tests so production callers cannot bypass the atomic producer API. Schema v16
  adds a durable sequence/Event-ID/timestamp retention floor and one canonical,
  content-free Sequencer checkpoint per Session. Checkpoint replacement, floor
  movement, and exact prefix deletion commit in one transaction; startup restores
  from the checkpoint plus retained tail, v15 migration preserves every existing
  Journal event, and Session purge resets Journal/checkpoint/floor atomically.
  Public Timeline foreign keys restrict Session projection deletion rather than
  cascading authority loss. Pre-recovery replay may validate a complete cursor,
  checkpoint, and retained tail while its rebuildable Session projection is
  temporarily absent; strict ownership is enforced after recovery before the Store
  becomes writable.
  A genuine `timeline/sync` request whose after anchor is before the validated floor
  now returns JSON-RPC `-32148` with closed `timeline-retention-gap/0.1` data. It
  exposes only the requested anchors and durable floor/head, requires
  `timeline.snapshot.current` through `timeline/snapshot`, reports availability true
  only when the current connection negotiated that capability, declares event history
  incomplete, and forbids replay from the floor. The response contains no Item/
  checkpoint content or internal checkpoint identity. A retained after or fixed
  watermark with a substituted Event ID remains ordinary `-32147` drift.
  `session/read` cannot serve as the public recovery snapshot: it pages projected
  Items without a fixed Public Timeline head and omits live `started`/`delta` state,
  while the v16 Sequencer checkpoint intentionally omits Item content/data. Schema
  v17 supplies that separate floor-visible state in the checkpoint/floor/prune
  transaction; Runtime materializes a fixed-head page set and Qt replaces one Session
  only after complete private validation. Automatic pruning remains disabled.
  The out-of-band heartbeat slice adds the exact content-free
  `runtime-heartbeat-request/0.1` / `runtime-heartbeat/0.1` nonce round trip,
  independent queue-saturation reachability, Qt single-flight 5-second/15-second
  liveness gating, generation-bound late-response rejection, and the Windows
  packaging workflow's Qt Runtime environment-test gate. Live subscription and
  bounded reconnect orchestration are now implemented. Durable Turn-start
  acknowledgement is also implemented by the schema-v20 ledger slice below;
  complete Windows reconnect/runtime evidence remains open. Keep `3.5` unchecked.
- The Timeline snapshot recovery slice is implemented end to end. AAP types and the
  stable Schema define the
  exact null-only `timeline/snapshot` request, fixed floor/watermark, strict
  continuation cursor, complete Item `first_event`/`latest_event` anchors, active
  Turn `started_event`/`latest_event`/ordered `open_item_ids`, and domain-separated
  Item/full-snapshot/page identities. Rust enforces the 10,000-Item/64 MiB complete
  materialization bound, 200-Item page bound, and 4 MiB frame budget. Schema v17
  visible-state persistence runs behind the same
  checkpoint/floor/prune SQLite transaction. Each Session starts with a strict
  empty visible snapshot; pruning reconstructs the public projection from the
  prior durable visible floor and only replays events through the requested
  `through` anchor, while the lifecycle Sequencer checkpoint continues through
  the current head. Canonical snapshot JSON is bound to its raw-byte SHA-256,
  floor anchor, complete Item count, and domain-separated snapshot identity.
  Redundant header/active-Turn/Item rows carry byte/hash/anchor/revision checks
  and are validated on read and startup; CAS on the previous snapshot identity
  prevents stale replacement. Snapshot, checkpoint, floor movement, and exact
  prefix deletion remain one rollback boundary. Focused tests cover durable
  materialization, visible Item recovery, active/open state, tamper rejection,
  and Session ownership. Runtime's capability-gated handler materializes one fixed
  head from the durable visible floor plus retained
  tail, converts validated Items/active Turn metadata into the AAP snapshot
  contract, computes complete snapshot/Item/page identities, and enforces
  continuation cursor, watermark, page-size, and response validation. Qt validates
  fixed headers, totals, open Items, Turn states, cursors, and identities in private
  staging, then atomically replaces only the target Session, drops delayed events at
  or below the watermark, and validates later events normally. Snapshot carries no
  timestamp, so the first post-watermark event establishes Qt's new baseline. A
  disconnect releases incomplete page staging but preserves confirmed UI, bounded
  queued live events, their accounting, and recovery intent for a fresh negotiated
  first-page request. The capability is advertised only with a healthy Store and
  `snapshot_available` is per-connection negotiation.
- Model catalog foundation (2026-07-21): the sidecar now validates an internal
  `model-catalog/0.1` metadata contract and exposes the read-only AAP capability
  `model.catalog.read-only` with `model/catalog`. The current projection is
  explicitly `state=offline`, `signature_validated=false`, and
  `refresh_supported=false`; it may identify the active runtime-bound model but
  leaves availability, limits, capabilities, roles, entitlement, and policy
  unknown until an authenticated catalog is validated. Unknown booleans and
  limits serialize as `null`, credential-shaped metadata is rejected, and the
  response grants no model-selection, token, routing, or execution authority.
  A separate `model.catalog.cache.read-only` capability and
  `model/catalog-cache` method now expose the internal cache lifecycle. With no
  authenticated catalog installed, the result is explicitly
  `availability=empty` and `selection_allowed=false`; Qt consumes this state
  and shows the cache condition in the model-binding tooltip only.
  Qt requests this projection after initialization through
  `AgentRuntimeClient::modelCatalog` and emits `modelCatalogRead`; the existing
  model display remains a Session binding display, not a picker. OpenSpec `9.1`
  through `10.12` remain unchecked pending signed cloud refresh/cache, complete
  matcher/profile integration, durable profiles, and truthful switching events.
- Capability matcher foundation (2026-07-21): the sidecar now exposes the
  read-only `model.capability-check.read-only` method
  `model/capability-check`. It validates Chat/Work, attachments, tools,
  reasoning, context-token floors, exact Runtime identity/version,
  entitlement/availability, and zero-data-retention requirements. Work mode
  implicitly requires tool calls; results distinguish `compatible`, `blocked`, and `unknown`, and
  `selection_allowed` remains false unless the catalog is fresh, signed, and
  every required value has a verified authority; present values labelled
  `unknown` or `estimated` still fail closed. This is preflight only and does not
  select a model, issue credentials, start a turn, or emit a model-change event; keep
  OpenSpec `10.2` unchecked until authenticated catalog/profile integration and
  cross-platform UI evidence exist.
- Desktop capability preflight projection (2026-07-21): after a catalog-bound
  model is observed, Qt requests the read-only capability check using the active
  Chat/Work requirements and rebuilds it when the mode changes. It validates
  exact model identity, bounded check/mismatch arrays, and decision versus
  `selection_allowed` consistency; compatible, blocked, unknown, and malformed
  results are rendered as metadata-only status, with compatible explicitly
  labeled read-only. No picker, routing, credential, Turn, or execution authority
  is granted. Render coverage compiles but remains unexecuted on this host due
  to the known Qt startup exit-137 resource limit; OpenSpec `10.2` remains
  unchecked pending authenticated catalog and cross-platform evidence.
- Runtime compatibility metadata foundation (2026-07-21): additive
  `model-runtime-compatibility/0.1` entries preserve the legacy summary while
  representing Codex App Server, ACP, native, and unknown adapter families with
  canonical adapter/protocol IDs, bounded exact evaluated versions, compatibility
  state, field authority, evidence version, and structured warning/blocking
  degradations. Validation rejects duplicate or contradictory entries and
  secret-shaped metadata. Capability preflight keeps a Runtime request without a
  version `unknown`, blocks a version outside an authoritative set with stable code
  `runtime-version-not-verified`, blocks missing/incompatible verified adapters,
  and exposes warning degradations without treating them as blockers. The offline
  projection identifies the current adapter/version but retains Unknown authority
  and grants no model selection, routing, token, Turn, or execution authority. A
  signed production catalog, real ACP/native adapter fixtures, authenticated cloud
  publication, and macOS/Windows compatibility evidence remain absent; keep
  OpenSpec `9.5` unchecked.
- Catalog refresh transport foundation (2026-07-21): internal
  `model-catalog-refresh/0.1` now validates the Qt-owned authenticated
  transport observation without receiving a credential. Conditional
  ETag/Last-Modified values are bounded, header-injection safe, and hashed for
  content-free status. Requests require the fixed `aegisy-model-catalog` scope
  and `Accept-Encoding: identity`; responses accept only an authenticated JSON
  200 signed-bundle envelope or a validator-backed 304. Redirects, unsupported
  encodings, oversized/malformed bodies, and authentication/rate-limit/server
  failures map to stable content-free codes with retryability. A deterministic
  304 fixture and nine unit tests cover the contract. Read-only AAP
  `model/catalog-refresh-status` and Qt now expose an explicit `目录刷新未配置`
  state with `conditional_requests_supported=true` while endpoint and trust
  anchor configuration are absent. No HTTP request, token transfer, Trust Store
  installation, cache mutation, model selection, routing, token, or Turn
  authority was added; keep OpenSpec `9.4` and `10.1` unchecked.
- Desktop catalog state projection (2026-07-21): Qt now separately validates
  and displays `offline`, `invalid`, `fresh`, `stale`, `expired`, and `empty`
  catalog/cache states in the read-only model-binding tooltip. Malformed
  catalog/cache results, including a cache response claiming
  `selection_allowed=true`, become explicit invalid status and cannot leave a
  previous usable-looking state behind. Deterministic render coverage exercises
  the lifecycle and fail-closed cases, and the complete desktop CTest suite now
  executes successfully on this host. This remains metadata only: no picker,
  catalog refresh, cache install, routing, token, or Turn authority was added, and
  OpenSpec `10.1` remains unchecked.
- Model profile foundation (2026-07-21): internal `model-profile/0.1` now
  validates global/project scope, bounded role bindings for Agent/plan/apply/
  review/utility/embedding/rerank, secret-free source metadata, and a
  deterministic SHA-256 identity. `single_model` enables only the Agent role;
  unconfigured roles never fall back to the default model, and role-specific
  bindings must be explicit. Runtime now opens the private Profile Store when
  available and advertises `model.profile.read-only`; AAP
  `model/profile/list` and `model/profile/read` expose only validated profile
  metadata, generation, identity, and revision. Both responses explicitly fix
  `selection_allowed`, `routing_authority`, `token_issued`, and `turn_started`
  to `false`; missing profiles return a stable not-found error. This is still
  not connected to catalog selection, Qt picker controls, token issuance, or
  routing/turn authority. Keep OpenSpec `10.3` and `10.4` unchecked until
  authenticated catalog/profile integration, picker/switching UI, and
  cross-platform evidence exist.
- Model catalog policy hardening (2026-07-21): `model-catalog/0.1` now rejects
  fresh catalogs without `signature_validated`, invalid catalogs that claim a
  validated signature, duplicate aliases or roles, aliases equal to a model ID,
  unsupported `field_authority` keys, non-positive token limits, and
  secret-shaped catalog/source metadata. This strengthens partial foundations
  for OpenSpec `9.2`, `9.3`, and `9.10`; the separate signature foundation below
  now supplies local cryptographic verification, signed key rotation, and a
  persistent externally anchored Trust Store, but is not connected to a real
  production root, authenticated refresh/key publication, cache refresh, or an
  admin publication service, so those tasks remain unchecked.
- Model catalog signature foundation (2026-07-21): internal
  `model-catalog-signature/0.1` verifies a canonical schema/Key-ID/catalog payload
  using strict Ed25519 verification through pinned `ed25519-dalek` 2.1.1. A
  bounded `model-catalog-key-ring/0.1` binds positive generation, content
  identity, public-key validity, revocation, and replacement lineage. Rotation
  advances exactly one generation, retains prior Key IDs/public keys, cannot
  widen old validity or undo revocation, and rejects rollback, conflicting same-
  generation content, missing key history, or unknown lineage. Internal
  `model-catalog-key-ring-signature/0.1` binds the signer, signature time, exact
  ring, payload identity, and Ed25519 signature. A private
  `model-catalog-trust-store/0.1` accepts generation one only when it is signed by
  the exact externally supplied `model-catalog-trust-anchor/0.1` root and preserves
  that root; later generations must be signed by a known, currently active and
  unrevoked key from the previous ring before monotonic rotation validation. The
  Store persists an empty first-open marker plus the anchor/current ring in a
  private atomic snapshot, rejects wrong anchors, forged/unknown/inactive signers,
  rollback, tampering, or a deleted snapshot, and restores prior in-memory authority
  after disk commit failure. The cache Store `install_trusted` path sets
  `signature_validated` only after Trust Store verification; raw cache admission is
  module-private. This still does not establish production trust: no real Aegisy
  root is embedded or configured, Runtime/AAP/Qt do not open the Trust Store, and
  private-key signing, authenticated Key Ring/catalog download, conditional cloud
  refresh, key publication, Workbench SQLite events, and model selection remain
  absent. Keep OpenSpec `9.3`, `9.4`, and `10.1` unchecked.
- Model catalog cache foundation (2026-07-21): internal
  `model-catalog-cache/0.1` accepts only a clean catalog marked fresh and
  signature-validated, binds its SHA-256 identity to a positive monotonic
  sequence and receipt/issue/expiry times, bounds TTL and stale retention,
  treats exact replay as idempotent, and rejects older/conflicting generations,
  snapshot tampering, or clock regression. Reads derive explicit
  fresh/stale/expired views and expose no catalog after the stale window. The
  low-level cache contract assumes an already verified catalog, while public
  Store admission accepts only the root-anchored Trust Store verification path. It does
  not refresh the cache or grant selection authority; every view fixes
  `selection_allowed:false`. A private
  `model-catalog-cache-store/0.1` now persists the validated cache outside the
  project root through an atomically replaced bounded snapshot, private Unix
  permissions, restart identity validation, and snapshot tamper detection.
  Runtime opens this Store from the durable Workbench data root; standalone
  runtimes use a bounded in-memory fallback. The Store is not Workbench-SQLite
  event-backed and does not fetch cloud data, open the Trust Store, rotate keys,
  or grant selection authority. Runtime/AAP/Qt expose the empty cache view, while
  production-root configuration, authenticated refresh, and real fresh/stale/
  expired desktop transitions remain open. Keep OpenSpec `9.3` and `10.1`
  unchecked.
- Model profile store foundation (2026-07-21): internal
  `model-profile-store/0.1` persists one global and bounded project profiles in
  a private, atomically replaced snapshot outside project roots. It validates
  scope and secret-free profile metadata, rejects duplicate profile IDs,
  requires exact revision CAS for updates/removals, makes identical retries
  idempotent, and revalidates the snapshot identity after restart. This snapshot
  store is not itself Workbench-SQLite event-backed or writable through AAP/Qt.
  Its validated snapshot remains Runtime read authority and is consumed by the
  read-only AAP list/read projection described above, but remains disconnected
  from catalog capability checks and routing/token/turn authority; keep OpenSpec
  `10.3` and `10.4` unchecked.
- Model profile SQLite projection foundation (2026-07-23): Workbench schema v14
  adds one global and bounded historical project `model_profiles` projection.
  Save/update/remove requires revision CAS, monotonic generation and event sequence,
  idempotent retries, and commits the lifecycle event plus projection in one
  `IMMEDIATE` transaction. Startup revalidates canonical profile JSON/hash/identity,
  the complete bounded event chain and cursor, current-lifecycle creation time,
  project ownership, and orphan streams. Admission caps 256 active profiles, 1,025
  historical scopes, and 10,000 lifecycle events before projection or sequence
  mutation; active profile IDs remain globally unique across scopes. Every row and
  event fixes `selection_allowed`, `routing_authority`,
  `token_issued`, and `turn_started` to false. The dedicated model-profile streams
  are excluded from Session recovery and cannot create false Session quarantine.
  Ordinary Session creation, portable import, and projection rebuild reject the
  reserved `model-profile-stream-*` namespace before any database write, so a
  Session cannot commit a cursor that makes the next Store open fail ownership
  verification.
  This projection does not migrate/delete the existing snapshot and has no Runtime,
  AAP, Qt, model-selection, routing, token, or Turn integration. OpenSpec `5.1` and
  `5.2` remain unchecked because extensions, Git-checkpoint projections, complete
  scheduler/job recovery schema, and complete mutation-ack coverage remain absent.
  The complete stage passes 616 library tests with one ignored live fixture, 10
  context-threshold tests, 63 protocol tests, and 19 stdio tests, plus formatting,
  strict Clippy, strict OpenSpec validation, and `git diff --check`.
- OpenSpec task `21.1` is complete: background jobs, multi-agent execution, and
  unattended writes remain unadvertised and disabled until their prerequisite
  security, recovery, budget, evaluation, and release gates are recorded. The
  runtime exposes only a bounded degradation report and no dispatch capability;
  tasks `21.2+` remain future work.
- Task `21.2` now has a partial internal `structured-plan/0.1` contract. It
  validates bounded step IDs/statuses/owners, dependency references and cycles,
  content-free SHA-256 evidence, and completion evidence. Base or evidence
  revision drift marks affected steps `stale` without resetting their status.
  The contract is unit-tested but is not persisted, exposed through AAP/Qt, or
  connected to an executor; keep `21.2` unchecked.
- Task `21.3` now has a partial internal `child-task/0.1` contract. It binds a
  parent session/turn to bounded goal/context/workspace/tool/model/permission/
  budget/result metadata, validates isolation and content identities, and grants
  no authority. It is unit-tested but has no child-session lineage, approval,
  worktree, scheduler, or executor integration; keep `21.3` unchecked.
- Task `21.4` now has a partial internal `child-task-state/0.1` lifecycle
  contract. It tracks parent/child identity, generation-bound statuses,
  transactional state updates, cancellation request/rejection/acknowledgement,
  completion races, generation exhaustion, and bounded handoff references/counts.
  It is unit-tested but is not persisted, exposed through AAP/Qt, or connected to
  parent review; keep `21.4` unchecked.
- Task `21.5` now has a partial internal `child-worktree-admission/0.1` gate. It
  binds the exact child request and runnable lifecycle identity to the existing
  live dedicated-worktree descriptor/health check, exact parent/child owner and
  base revision. A write request fails before admission when isolation is shared,
  missing, reused by another child, dirty, conflicted, unhealthy, or cancelling.
  Its non-serializable workspace proof and content-free receipt explicitly grant
  no permission or execution authority. Scheduler persistence, project-root
  registry binding, per-tool revalidation, permission/approval/sandbox
  intersection, and executor integration remain absent; keep `21.5` unchecked.
- Task `21.6` now has a partial internal `child-runtime-budget/0.1` ledger. It
  reserves model token/cost capacity before admission; settles authoritative or
  estimated usage; charges unknown usage as the full reservation rather than
  zero; and enforces wall-time, turn, tool-call, concurrency, and policy-bound
  network-request limits with transactional updates. Content-free snapshots
  expose limits, used/reserved/remaining amounts, source counts, and warning,
  saturated, or exhausted dimensions while explicitly granting no permission or
  execution authority. Provider usage, a Runtime-owned monotonic clock, durable
  job/session events, scheduler/executor admission, cancellation/refund policy,
  AAP/Qt budget events, and cross-platform endurance evidence remain absent; keep
  `21.6` unchecked.
- Task `21.7` now has a partial internal `unified-execution-plan/0.1` pipeline
  invariant. Interactive, child, and background envelopes use the same ordered
  identity, reconciliation, permission, approval, workspace, budget, sandbox,
  recovery, durable-job, notification, release, dispatch, observation, and handoff
  stages; mode changes only which gates and terminal evidence are required. Mode
  binding prevents child/job/unattended relabelling. The current Codex read-only
  interactive `turn/start` now passes this plan immediately before adapter dispatch.
  Plans are content-free and explicitly grant no permission or execution authority;
  child/background dispatch remains absent and unadvertised. Typed proof composition,
  generic executor ownership, durable events/recovery, provider/tool budget
  settlement, AAP/Qt mode status, and cross-platform evidence remain incomplete;
  keep `21.7` unchecked.
- Task `21.8` now has partial internal `background-job-request/0.1` and
  `background-job-state/0.1` contracts. They bind project/session/root, unified
  plan, child, idempotency, one-shot schedule, bounded attempts/backoff, and an
  optional safe retry boundary. Transactional lifecycle updates cover queued,
  running, pause-requested/paused, waiting approval, cancelling, terminal states,
  bounded result references, and exact attempt evidence. Cancel request and
  acknowledgement remain distinct; completion can win the race. Restart never
  infers success: active work becomes interrupted, waiting approval stays waiting,
  and retry eligibility requires the pre-bound safe boundary and attempt capacity;
  no automatic retry or approval is emitted. Pause/resume cannot bypass schedule or
  retry backoff. Workbench schema v11 persists canonical request/state JSON,
  SHA-256 identities, generation, schedule/recovery metadata, cancellation, and
  attempts in a bounded `background_jobs` projection. Create and generation-CAS
  updates commit with typed `background-job.*` session events; identical retries
  are idempotent, stale writers fail, event failure rolls back the projection, and
  startup revalidates at most 10,000 records before the store becomes writable.
  Active jobs block session deletion/retention and terminal records purge with the
  session. Schema v11 also persists one optional
  `background-job-scheduler-lease/0.1` per job. Leases bind exact job/request/state
  generation, scheduler owner, bounded acquisition/renewal/expiry, optional verified
  process-registration/process identities, and generation-CAS updates. Acquire,
  renew, state-rebind, process-bind, release, and expire commit with typed
  `background-job.lease-*` events; event failure rolls back the projection, startup
  revalidates canonical JSON/hashes, and v10-to-v11 uses the normal WAL-consistent
  migration backup. Active leases protect deletion even after a job becomes terminal,
  and stale leases may only expire without adopting the newer job generation.
  Internal `background-job-scheduler/0.2` now loads a complete bounded
  recovery set into one owner-identity/generation-bound, content-hashed inspection
  snapshot. It classifies schedule wait, admission review, paused, approval wait,
  retry review, terminal review, and manual reconciliation while always reporting
  `dispatch_available:false`, `automatic_retry:false`, and
  `automatic_approval:false` and `automatic_takeover:false`. Lease recovery states
  distinguish missing, current, expired, released, stale-job, and owner mismatch.
  Process ownership separately distinguishes missing lease/registration,
  unavailable/non-running observation, mismatch, and exact current ownership. Only
  the conjunction of a current durable lease, matching process registration hash,
  and a live Runtime-owned handle becomes monitor-only; terminal jobs with an active
  lease remain visible until explicit release. Internal
  `background-job-process-observation/0.1` now accepts only a Runtime-owned
  `std::process::Child` handle, never a caller-selected PID. Its content-free
  evidence binds scheduler owner, project/session/root/job, exact request/state
  identities, job generation, attempt, opaque process identity, registration time,
  and observation time. It distinguishes owned-running, owned-exited, absent,
  inaccessible, mismatched, and unknown. Process exit or disappearance never implies
  job completion; every non-running or ambiguous result requires manual
  reconciliation, and an owned exit separately requires a terminal job event.
  The scheduler can consume the non-serializable verified observation and classify
  exact owned-running work as monitor-only while keeping dispatch disabled. Missing
  ownership remains manual, pending cancellation separately reports acknowledgement,
  and failed refresh retains the previous snapshot. Persisted process hashes are not
  a process handle: after Runtime restart the in-memory registry is absent and the job
  remains manual rather than being adopted. Automatic lease acquisition/renewal,
  authoritative approval, automatic recovery transitions, notification, AAP/Qt
  controls, and cross-platform endurance remain absent. Internal
  `background-job-recovery-decision/0.1` now converts one fully revalidated scheduler
  snapshot entry into a content-free decision bound to exact job status/cancellation,
  request/state generation, scheduler owner/generation/snapshot, lease, process
  observation, bounded blocker codes/hash, and timing identities. `WorkbenchStore`
  rechecks the current job and lease before and under its write transaction, then
  appends an idempotent
  `background-job.recovery-reviewed` session event. The bounded 10,000-event journal
  is semantically revalidated at startup; event failure rolls back sequence allocation
  and hash-consistent semantic tampering fails startup. Decisions fix retry, approval,
  takeover, dispatch, and mutation authority to false and never change job or lease
  state. AAP capability `background-job.recovery.inspect` and method
  `session/background-recovery` now expose a session-bound, metadata-only page built
  from a fresh owner-bound scheduler snapshot. Keyset anchors bind the current
  scheduler entry identity; the page may include only a matching recorded-decision
  summary. Qt exposes `后台恢复状态…` from the Session menu with strict page/entry/
  cursor/review validation, empty state, and keyset loading. The page fixes dispatch,
  automatic retry/approval/takeover, and mutation authority to false and never acquires
  leases, observes caller PIDs, changes job/lease/process state, or performs recovery.
  Missing storage, missing sessions, forged cursors, whole-store recovery, and invalid
  evidence fail closed. Automatic decision production/consumption, recovery mutation,
  notifications, and cross-platform endurance remain absent; keep `21.8` unchecked.
- Task `21.9` now has a partial internal
  `background-job-notification-intent/0.1` contract. Completed, failed,
  approval-needed, and budget-exhausted intents bind exact project/session/root,
  request/state identities, job generation/status, terminal/result/approval evidence,
  optional child-budget snapshot evidence, exhausted dimensions, creation time,
  stable deduplication identity, and full intent identity without title, body, prompt,
  result content, or platform payload. `ChildBudgetSnapshot::validate` rechecks
  accounting, remaining values, counters, usage provenance, classifications, scope,
  and false authority flags; generation zero is allowed when the complete snapshot is
  otherwise valid. Intents always set content inclusion, delivery availability,
  delivery attempt, and platform delivery authority to false. Workbench schema v12
  persists one canonical intent per deduplication identity in a bounded
  `background_notification_outbox` and appends the exact
  `background-job.notification-recorded/0.1` session event in the same transaction.
  Rows bind exact job/request/state/generation, lifecycle event sequence, canonical
  JSON/hash, creation/record times, and fixed `recorded`/zero-attempt/false-authority
  delivery state. Identical retries remain idempotent after later job transitions;
  stale first writes, conflicts, event failure, projection/event/lifecycle tampering,
  and more than 10,000 records fail closed. Internal session-scoped keyset inspection
  is read-only, v11-to-v12 uses the WAL-consistent backup, and terminal session purge
  removes jobs, outbox rows, and events together. Scheduler production, AAP/Qt
  inspection and settings, delivery transitions/retry/confirmation, platform APIs and
  permissions, localization/privacy review, and cross-platform evidence remain absent;
  keep `21.9` unchecked.
- The `21.9` read-only inspection layer is now reachable through the negotiated
  `background-notification.outbox.read-only` capability and AAP method
  `session/background-notifications`. It serves only a bounded, session-bound
  `background-notification-page/0.1` metadata page with strict keyset cursors; it is
  unavailable without writable durable storage or during whole-store recovery, while
  archived, pending-deletion, quarantined, and reconciliation-blocked sessions remain
  readable. Qt exposes a capability-gated Session-menu viewer with keyset loading,
  empty/error states, and strict validation of schema, session, kind, lifecycle
  identity, timestamps, delivery state, zero attempts, false authority, and forbidden
  content-field absence. No delivery action, platform permission, retry, or background
  producer exists. The latest Rust run passes 392 tests with one ignored fixture, 56
  protocol tests, 11 stdio/Codex tests, and strict Clippy; the complete Qt build and
  `agent_runtime_protocol` pass. The `agent_workbench_render` process was killed by
  host resource pressure at startup (0.56s) without assertion output, so it is not
  treated as a render assertion failure. Keep `21.9` unchecked.
- Task `6.10` now has an internal `session-compaction/0.1` contract foundation.
  Bounded summaries cover decisions, unresolved tasks, changed files, commands,
  tests, failures, and next actions; secret-shaped/control-character content is
  rejected, item and byte limits are enforced, review IDs are content-hashed, and
  activation requires an exact sequence plus source-context identity. Failed
  checkpoints preserve the original context and expose bounded model-change,
  portable-fork, and manual-cleanup recovery options. This does not count as task
  completion. An internal `session-compaction-checkpoint-store/0.1` now persists
  the exact validated review under the Workbench data root using bounded content-
  addressed objects, hashed pointers, no-clobber publication, private Unix
  permissions, and restart/tamper validation. `WorkbenchStore` can append an
  idempotent metadata-only `session.compaction-checkpointed/0.1` event and validates
  it during projection replay; the event never copies summary/instruction content.
  The filesystem object and SQLite event are not one cross-resource transaction.
  AAP capability `session.compaction.checkpoint-review` exposes manual create/read
  methods only when both durable stores are healthy. Create derives its sequence
  and context hash from the complete verified event stream, blocks active turns,
  and is content-idempotent; read requires a matching object and event after
  restart. Both responses state that activation and provider compact are
  unavailable. Qt now exposes session-menu actions to create a bounded manual
  review or read a persisted checkpoint in a plain-text, read-only dialog. A read
  review can now be edited into a new immutable checkpoint through
  `session/compaction/checkpoint/revise`: the source Review ID is revalidated, the
  old object is never overwritten, and a metadata-only `supersedes` descriptor is
  event-backed and replay-validated. Revision retries are idempotent and Qt keeps
  the same activation/provider-compact fail-closed state. Startup compensation,
  model-generated summaries, editable activation, and Codex
  `thread/compact/start` remain unavailable. Original event history remains
  authoritative and must never be discarded.
- Task `6.9` now has an internal `operation-reconciliation/0.1` contract
  foundation. It accepts content-free event, process, workspace, and Git evidence
  and emits a bounded state/decision, blockers, observed domains, and a
  content-hashed review ID. Missing terminal events remain `unknown`; live
  processes remain `running`; changed or unavailable workspace/Git state, Git
  operations in progress, and missing required evidence block subsequent writes.
  It never infers mutation success, probes the host, executes recovery, or exposes
  AAP/Qt mutation controls. Durable records, restart probes, user review, and
  recovery actions remain required before task completion.
- Task `6.9` now also has a durable event-backed reconciliation step. AAP
  `operation/reconcile` validates the content-free evidence contract and appends
  `operation.reconciled/0.1` to the session event stream. Identical evidence is
  idempotent; the latest per-operation result is hash-validated from the event and
  survives Runtime restart. Unknown, running, or blocked results gate subsequent
  session-bound mutations with stable error `-32132`; a newer authoritative
  completed/failed/interrupted review clears the gate. Capability
  `operation.reconciliation` is advertised and store/protocol fixtures cover
  persistence, restart blocking, idempotency, and unblocking. This remains partial:
  the method consumes caller-supplied evidence only and has no authoritative host
  probes, Qt review/recovery surface, or recovery action.
- Capability `operation.reconciliation.probe` now adds a separate read-only
  `operation/probe` method. It resolves only a registered project root through a
  Work session, hashes bounded visible workspace metadata, reads the structured
  Git status query, and observes runtime-owned turn or terminal state. The response
  contains state labels and snapshot hashes but no source content, arbitrary host
  paths, or caller-selected PIDs. Event state is caller-supplied except for the
  bounded durable turn/Git lifecycle mappings described below; the probe does not
  persist, approve, mutate, or recover an operation. Startup discovery, complete
  authoritative event sourcing, Qt review, and recovery actions remain open.
- Durable Runtime startup now preloads the latest validated reconciliation event
  for each session/operation pair into a bounded cache. The SQLite event stream
  remains authoritative for request-time gating, and a malformed, missing, or
  over-limit startup scan never implies that an operation is safe. This is only
  partial startup discovery; automatic operation source registration, Qt review,
  and recovery actions remain incomplete.
- If `operation/probe` omits `event`, durable Runtime derives the latest validated
  registered `turn.*` state and existing `git.workflow.*` lifecycle state from the
  session event stream. Git prepared/dispatching/in-progress evidence maps only to
  running; completed/failed/aborted maps to terminal evidence; conflicted/recovered
  remains unknown. Explicit event values remain caller-labelled; workspace-edit,
  terminal, and background-job event sources are not inferred. The probe still
  requires a later `operation/reconcile` review and never exposes prompt/content
  data.
- Capability `operation.reconciliation.status` exposes read-only
  `operation/status` for the current session gate. It returns a bounded review
  summary and `recovery_action_available:false` while blocked, never clears the
  gate, and never invokes recovery. Qt requests this status after session
  start/resume/fork/load/import, renders a content-free gate banner, and keeps
  the composer and Send action read-only while the result is unknown, malformed,
  unavailable, or blocked. A queued first turn is deferred until an explicit
  unblocked status arrives; a status error cannot be interpreted as success.
  A visible `重新检查` action may collect a bounded `operation/probe` result and
  submit `operation/reconcile` only for a blocked `turn`; the flow validates
  session/operation identities, never performs recovery, and leaves other
  operation kinds unavailable.
- Task `6.6` now has a bounded read-only Session search foundation. AAP
  `session/search` returns `session-search/0.2` and filters durable Session projections
  by project, exact branch, model, runtime, status, title, or a combined
  title/approved-transcript query. Transcript
  matching executes inside SQLite and is restricted to `message` Items with
  `user`/`assistant` roles and visible `text`, `content`, `output`, or `diff` fields;
  diagnostic, command, and other payloads are not searched, and Runtime does not
  hydrate every transcript. Results exclude purged sessions, include matched-field
  plus Runtime/Workspace binding metadata, cap one page at 100, and use a strict
  `after:<updated-at>:<session-id>` cursor. Additive indexes cover status/order,
  runtime/model binding, branch identity, and transcript ownership. Qt exposes a debounced left-rail
  search, scopes Work to the current project, searches all Chat sessions, and shows
  explicit empty results. Schema v13 now persists one event-backed
  `session_workspace_bindings` row for newly created/imported/forked Work Sessions.
  It binds the primary project root, Git availability, safe branch display plus exact
  SHA-256, HEAD, repository/worktree filesystem identities, capture time, and explicit
  `dedicated_worktree:false`/no-authority state without raw repository paths. Runtime
  resume re-observes the root/branch/HEAD/worktree and fails with `-32146` on drift
  instead of running against stale context. Keep `6.6` unchecked until complete
  indexed-text/scale evidence, branch-filter UI, model control-plane and dedicated-
  worktree integration, and final cross-platform behavior are complete.
- OpenSpec tasks `3.9`, `3.11`, `3.12`, `5.3` through `5.10`, `6.1`, `6.8`, `7.10`, `13.1` through `14.1`, tasks `14.3`, `14.4`, `14.6`,
  `14.8`, `15.1` through `15.9`, `16.1`, `16.2`, and `7.12` are complete. Task `14.2`
  awaits Windows runtime evidence.
- File tree, Git decorations, safe file opening, Monaco editing, dirty state,
  conflict rejection, atomic save, recent files, view restoration, and native
  Chat/Work shell are implemented.
- A successful user-origin atomic save advances the matching runtime watch snapshot
  after invalidating search/index/language/diagnostic state. The watcher therefore
  does not misclassify Aegisy's own save as an external editor conflict; a later
  independent disk change still produces the normal conflict state.
- Monaco supports two shared-model editor groups with explicit active-group save
  targeting and per-project restart restoration. The Qt emergency fallback remains
  a single editor and keeps the split control disabled.
- Workspace search supports filename/text/all modes, case sensitivity, 50-result
  UI pages, explicit cancellation, result navigation, snapshot-stale detection,
  and stale indicators after watched or user-saved changes.
- The lazy Structure workspace indexes Rust, Python, JavaScript, TypeScript/TSX,
  and C/C++ with official Tree-sitter grammar queries. It exposes symbol ranges,
  import/include dependency edges, changed-file reuse, deleted-file removal,
  exact editor navigation, and a token-budgeted repository map without persisting
  source content.
- Editor language commands now bridge installed rust-analyzer, Pyright, TypeScript
  Language Server, or clangd through AAP. Definition/reference/diagnostic results
  are root-scoped, provenance-labelled, bounded, stale-aware, and navigable; missing
  servers remain explicit unavailable states.
- Observed diagnostics now carry content-addressed identity, source kind and
  server-or-command provenance, analyzed-file hash, observation time, freshness,
  stale time, and a normalized raw-artifact reference. The Structure view exposes
  provenance, fresh/stale state, and raw authority without retaining source text.
- The composer now has an explicit context queue with inclusion and removal
  controls. File-tree, editor-selection, search-result, diagnostic, terminal-
  excerpt, and Git-diff actions produce structured `turn/start` context rather
  than silently concatenating UI text into a prompt.
- OpenSpec task `17.1` now has a partial `context-manifest/0.1` foundation. The
  `turn.context.manifest` capability adds content-free entries to `turn/start`
  responses with source/kind, pinned priority, untrusted-data trust, SHA-256
  content identity, conservative token estimates, freshness, inclusion reason,
  and included state. File attachments are hashed only after the existing root,
  ignore, symlink, and stale checks; truncation is explicit and the manifest never
  contains attachment text. Instruction discovery and a first deterministic
  context-budget foundation now share the turn preparation path, while tokenizer
  authority, context inspection, and provider-scale evidence remain open, so
  `17.1` is not complete.
- OpenSpec task `17.2` now has a partial `instruction-discovery/0.1`
  foundation. Read-only AAP `workspace/instructions` binds a registered project
  root and optional target path, and deterministically returns a weakest-first
  merge list with precedence `managed > user > nested (closer depth wins) >
  project`. Managed/user roots are accepted only through the path-only
  environment overrides `AEGISY_MANAGED_INSTRUCTIONS_DIR` and
  `AEGISY_USER_INSTRUCTIONS_DIR`; callers cannot provide arbitrary host paths.
  Entries carry logical scope/path, depth, precedence, `untrusted-data` trust,
  bounded bytes/token estimate, SHA-256, revision, freshness, inclusion, and
  rejection state. Explicit `include_content:true` returns bounded instruction
  text only as untrusted data; the response explicitly states that it cannot
  grant permissions, execute commands, enable Hooks, or authorize network.
  Symlink, sensitive/built-in/Git-ignored, case-collision, invalid UTF-8,
  control-character, secret-shaped, stale, file-count, directory, and byte-limit
  cases fail closed or are reported without the body. Unit and protocol fixtures
  cover precedence, nested targets, metadata-only output, rejection, and bounds.
  Work-mode `turn/start` now appends up to eight valid discovered instructions
  after explicit user context through the same bounded context preparation and
  manifest. Generated entries carry instruction precedence in priority and
  inclusion metadata, remain untrusted, and secondary-root attachments do not
  select the primary-root instruction chain. Durable managed/user configuration,
  complete rejection/budget reporting, trust/managed-policy intersection,
  context inspection, and cross-platform evidence remain open; keep `17.2`
  unchecked.
- OpenSpec task `17.3` now has an internal `pinned-context/0.1` data-contract
  foundation. It supports file, selection, image, diagnostic, terminal excerpt,
  Git commit/diff, artifact, and child-handoff descriptors without storing source
  bodies. Items bind project plus optional session/root, bounded source/label and
  non-network reference, lowercase SHA-256, byte count, revision, freshness,
  priority, and secret-free bounded metadata. Sets reject duplicate IDs, project
  mismatch, unsupported schemas/kinds, unsafe paths/references, secret-shaped
  metadata, and 128-item/16 MiB-item/64 MiB-total overflow, and expose a
  deterministic content identity. A project-external `pinned-context-store/0.1`
  now persists exact descriptor sets as private immutable content-addressed
  objects behind atomically replaced project pointers. Reopen revalidates object
  hash, schema, project/set identity, bounds, and the explicit
  `content_bodies_persisted:false` invariant; identical writes are idempotent,
  updates retain old objects, and missing/tampered current authority blocks pointer
  replacement without repair. The store is bounded to 1,024 projects, 4,096
  objects, 1 MiB/descriptor set, and 256 MiB total. Object publication and pointer
  replacement are not one transaction, so a failed pointer update may retain an
  unreferenced immutable object. Runtime now opens the store beside the
  Workbench store and, when healthy, advertises
  `workspace.pinned-context.store` and `workspace.pinned-context.manage`.
  AAP `workspace/pinned-context/list` reads a project set,
  `save` validates project/root/session bindings and supports optional
  `expected_set_identity` compare-and-swap protection, and `remove` performs an
  explicit unpin through the same path. Removing the final item persists an
  empty set identity. Protocol fixtures prove metadata-only persistence,
  restart recovery, project event replay, Blob scope/hash validation,
  idempotency, stale-write rejection, and no body fields. A successful save or
  remove appends `project.pinned-context-updated/0.1` after immutable
  object publication; the two resources are deliberately not one transaction,
  and event failure is reported as an incomplete save. Runtime compares the
  previous and next complete sets for every save/remove. When the final descriptor
  for a session-owned image disappears, its project event and durable Blob-reference
  release now commit in one SQLite transaction; shared duplicate image pins keep the
  reference active until the last one is removed, and release preserves at least the
  normal 24-hour undo window. Event/storage failure rolls back every release and
  conservatively leaves the Blob active. A later explicit import of the exact same
  content, scope, reference, and metadata reactivates the released reference in the
  write transaction; identity drift remains rejected. Updates that release Blobs carry
  a content-hashed, body-free release-batch identity and count, so repeated transitions
  to the same empty set do not reuse an unrelated earlier event. The earlier external
  pointer publication is still outside that SQLite transaction. A private
  `pinned-context-publication/0.1` journal now records the previous/next set and
  object identities before pointer replacement and is removed only after the SQLite
  event/release transaction succeeds. Runtime startup validates each journal against
  its pointer, immutable objects, and latest project event; it cleans an unchanged
  previous pointer, replays only an uncommitted forward event/release transaction,
  and cleans an already-committed forward event without duplicating it. Any malformed,
  tampered, or ambiguous state disables pinned-context capabilities while preserving
  immutable objects and Blob data. After successful compensation, startup runs the
  bounded `pinned-context-object-gc/0.1` sweep with a 24-hour orphan grace period.
  It protects every current pointer and pending journal object, rechecks object
  hash/schema/set integrity and file metadata before deletion, and reports while
  preserving unknown, corrupt, future-dated, recently-created, or changed entries.
  Capability
  `turn.context.pinned-selected` now lets `turn/context/inspect` and `turn/start`
  consume only explicit selected file/selection/image/diagnostic/terminal/Git/artifact pin IDs
  together with the exact current set identity. Selection is capped at 16 unique IDs; project/session/
  root bindings and file policy are rechecked. Both paths share authoritative
  file reread, raw-byte SHA-256/revision stale detection, bounded untrusted
  context, and metadata-only inspection. Selection pins carry bounded line/
  column metadata (1-based Unicode scalar columns) and are sliced only after source
  validation. Session-owned
  `artifact` pins may resolve validated command-output text references. Project/
  root-bound `diagnostic` pins may resolve only normalized `diagnostic-raw:sha256:`
  content from the authoritative in-memory DiagnosticStore after media-type,
  reference, SHA-256, and byte-count validation; Runtime restart or eviction removes
  that source and must fail closed. Session-owned `terminal_excerpt` pins resolve
  only `terminal-excerpt:<terminal>:<generation>:<start>:<end>` references against
  the Runtime-owned PTY capture. The reread requires the same session, terminal,
  generation, retained absolute range, normalized UTF-8 content, SHA-256, and byte
  count; Runtime/terminal restart, removal, generation change, or buffer eviction
  fails closed. Primary-root `git_commit` and `git_diff` pins use strict full-OID or
  fixed-scope references and re-run the filtered read-only Git query at assembly.
  They bind both the bounded 16 KiB content hash and the complete normalized source
  hash/byte count/truncation state. Worktree/staged drift fails stale; commit and
  commit-diff context remains valid only while the exact Git object is available.
  Child-handoff descriptors now have a read-only `child-handoff/0.1` assembly
  foundation: they must be parent-session bound, reference a session-owned text
  Artifact Blob with matching owner/handoff metadata, and name an existing
  same-project source session. Assembly rereads bounded UTF-8 content and checks
  hash/bytes; inspection remains metadata-only. The Rust unit and protocol
  fixtures cover valid authority, metadata-only inspection, and invalid identity
  rejection. Child-task production, parent/
  child lineage/handoff persistence, approvals, and multi-agent execution remain
  unavailable. Qt now loads
  project pins into the composer, creates or refreshes file descriptors from
  authoritative workspace reads with reconstructed raw UTF-8/BOM/newline bytes,
  saves through CAS, and supports explicit per-turn inclusion, deterministic
  order changes, and unpin. The editor context menu also creates a range-bound
  clean, conflict-free selection pin after the same authoritative reread;
  transient inline selections remain available for unsaved buffers. It passes selected IDs and set identity separately to
  inspect/start and never turns persisted pins into implicit model context.
  Qt workspace watcher and user-save events mark matching loaded file, selection,
  and diagnostic pins stale in the composer using their root-relative metadata path;
  terminal restart/removal marks matching terminal-excerpt pins stale by terminal
  identity. These local indicators never rewrite the durable descriptor or hash;
  sidecar reread/authority remains the final gate at inspect/start.
  `artifact/read-command-output-page` now binds the originating Session, wire Item,
  content reference, immutable Artifact metadata, and bounded UTF-8 page window.
  The Qt read-only Artifact dialog exposes an explicit `固定完整输出` action only
  after every page, the terminal byte count/SHA-256, and any canonical truncation
  marker boundary are validated for the active project-bound Work session; it saves
  a session-owned artifact descriptor through the existing pinned-context CAS with
  `metadata.item_id`, priority 700, and the retained content byte count. The action
  never sends the Artifact implicitly. Cross-
  session, recovery, deletion, reconciliation-blocked, busy-mutation, invalid
  hash, and unsupported-media states remain disabled/fail closed.
  The Structure diagnostics surface now exposes explicit `固定选中诊断`. Qt first
  reads `workspace/diagnostics/raw`, rechecks project/root, media type, reference,
  SHA-256, and UTF-8 byte count, and then persists only a project/root-bound
  metadata descriptor through the same CAS path. It never persists the raw body or
  includes the pin automatically; the Runtime remains the authority at inspect/start.
  The Terminal context menu now separates transient `添加选中内容` from persistent
  `固定最近输出`. The latter calls read-only `terminal/excerpt/read`, which captures
  at most 16 KiB of the current retained tail, strips ANSI/OSC through pinned
  `strip-ansi-escapes` 0.2.1, removes remaining non-text controls, and returns an
  absolute range/generation/hash. Qt validates that response and persists only a
  session/root-bound metadata descriptor through CAS. It does not claim that xterm's
  visual selection has raw PTY offsets.
  Capability `workspace.git-context.read-only` exposes
  `workspace/git/context/read` for filtered Git commit detail and worktree/staged/
  full-OID commit diffs. Qt exposes explicit `固定差异` and `固定提交`, validates the
  project/root/kind/scope/OID/media/hash/bytes/full-source/truncation identity, and
  persists only the descriptor through CAS. The real Git render fixture clicks both
  actions, verifies their persisted rows, and unpins them.
  Capabilities `workspace.image.import-user` and `workspace.image.preview` provide
  the explicit user image path. Import is bound to the current project Work
  session/root and rejects encoded input before Base64 decode. Pinned `image`
  0.25.10 independently decodes only PNG/JPEG/WebP under 8 MiB, 8192px-edge,
  40M-pixel, and 192 MiB decode-allocation limits before writing a session/project-
  owned `image:sha256:` durable Blob. Descriptors persist only reference, hash,
  bytes, media type, and dimensions. Read-only preview revalidates the authority
  and returns only a 320x180 PNG thumbnail. Inspect/start reread and decode again;
  inspection remains metadata-only and each selected image uses a conservative
  all-or-nothing 16 KiB/4096-token budget entry outside prompt text. The pinned
  Codex 0.144.5 adapter sends included images as `localImage` paths through verified
  private temporary hard links. Normal turn completion drops the links and Blob-
  store startup removes only safely named crash leftovers, preserving unknown
  entries. Qt exposes `固定图片`, client preflight, dimension/media/size display,
  explicit thumbnail preview, inclusion/order, and unpin. The final image unpin
  releases its active Blob reference atomically with the project event while retaining
  the bounded undo window; an earlier duplicate pin keeps the shared reference active.
  Transient inline `selection` context with client-provided content remains on
  the existing bounded inline path; only selection items without inline content
  are resolved from the authoritative file range.
  This foundation still has no atomic boundary across the external pin object/pointer
  and SQLite event/release transaction, or child-task handoff production. Runtime now
  propagates workspace watch/save changes, diagnostic re-observation, and terminal
  restart/removal into a durable `pinned-context-source-invalidation/0.1` metadata
  publication. The sidecar marks matching file/selection/diagnostic/terminal
  descriptors stale through the existing immutable object, pointer journal, and
  SQLite event path; a failure reports a bounded state and never substitutes for
  authoritative source reread. Qt advances its local CAS identity only when the
  schema, old/new hashes, and previous identity match; otherwise it reloads the
  complete durable set before another mutation. Complete image/Git lifecycle, cross-platform
  evidence, and child-task handoff production remain open, so keep `17.3` unchecked.
- OpenSpec task `17.4` now has a partial `context-budget/0.1` allocator.
  Prepared turn responses include a content-free plan for explicit context and
  auto-discovered instructions. Instruction precedence ranks and pinned priority
  determine deterministic allocation order without reordering rendered text. Existing
  task-state, recent-turn, tool-result, search/search-result, and repository-map item
  kinds now receive explicit class labels in the same allocator; no implicit context
  is added. A 64 KiB total and 16 KiB per-item hard bound report requested/allocated
  bytes, class, score, inclusion, and exclusion reason. Tokenizer and provider-window
  authority, complete source producers, scale, and cross-platform evidence remain
  open; keep `17.4` unchecked.
- OpenSpec task `17.5` now has a partial `tokenizer/0.1` fallback contract. The
  current estimate is explicitly identified as `unknown-utf8-four-byte` with
  `authority=conservative-unknown`, `exact=false`, and
  `provider_window_authoritative=false`. Each budget entry and aggregate plan
  reports overflow-safe conservative estimated token counts. No provider-specific
  tokenizer, model context window, or fit guarantee is claimed; keep `17.5`
  unchecked.
- OpenSpec task `17.6` now has a partial read-only
  `turn/context/inspect` preflight. It reuses the exact session-root,
  instruction discovery, stale, and `context-budget/0.1` preparation path
  without starting a model or persisting history. The
  `context-inspector/0.1` response returns only manifest/budget metadata and
  explicit `content_included:false`, `model_started:false`, and
  `persisted:false`; source/instruction bodies never cross the response. Qt
  now exposes a composer search-icon preflight action for an existing session
  and renders a read-only source/type/trust/size/status/reason table; unchecked
  client context is sent as an explicit exclusion marker. The Workbench render
  fixture exercises the control and metadata-only result.
  Complete context classes, explicit redaction/exclusion explanations,
  provider/tokenizer authority, and cross-platform evidence remain open; keep
  `17.6` unchecked.
- OpenSpec task `17.10` now has deterministic context-quality coverage for
  large monorepos, ignored dependency/build/cache trees, irrelevant repository
  maps, nested target-scoped instruction precedence, and stale file rereads.
  The tests prove intentional exclusions do not masquerade as budget
  truncation and that stale context uses the current file body rather than
  reusing an old snapshot. They remain unit-level evidence only; provider
  switching, model-specific tokenizers, and clean cross-platform scale runs
  are still absent, so `17.10` remains unchecked.
- OpenSpec task `17.7` now has an internal `context-threshold/0.1` evaluator.
  It validates fresh/stale/unknown observations, authoritative versus
  conservative evidence, soft and hard limits, checked overflow, and explicit
  hysteresis. Results are `no-action`, `preview-required`, or
  `hard-limit-exceeded`, and every result fixes
  `automatic_compaction_authority:false`. Codex usage Timeline items now
  evaluate provider-observed last-input/context-window values. Runtime session
  reconstruction, fork, and portable import now replay the complete hydrated
  usage history through the exact `usage-authority/0.1` and
  `context-threshold/0.1` contracts; malformed or missing threshold metadata
  fails closed to `preview-required`, while genuinely empty history starts at
  `no-action`. The latch is carried into the next turn and remains a review
  signal only; it cannot invoke checkpoint creation, provider compact, a model,
  or Qt authority. The additive `session-context-threshold/0.1` projection is
  now returned by Session start/resume/fork/read and portable import, and Qt
  validates it before rendering `阈值正常`, `阈值需预检`, or `阈值已达上限`.
  Missing, malformed, unknown, or `automatic_compaction_authority:true`
  summaries render `阈值未知`; no stale normal-looking state is retained.
  A cold durable `session/read` now reports `history_state=empty` only after a
  complete scan finds no usage Items; usage evidence is `replayed`, and scan
  failure, malformed evidence, or a bound hit remains `replayed` plus
  `preview-required`. Qt retains at most 128 Session threshold summaries and
  deterministically evicts inactive entries while protecting the current Chat,
  Work, history, and active-Turn Sessions. Rust protocol fixtures, a focused Qt
  cache run, and direct C++17 syntax checks cover these paths.
  Complete Qt runtime/render execution and cross-platform evidence remain open,
  so keep `17.7` unchecked.
- User-initiated macOS PTY execution, the named lifecycle, and the Qt/xterm.js
  terminal frontend are verified; Windows ConPTY is implemented but not yet
  runtime-verified. Read-only Codex command events now have a partial structured
  timeline mapping, and turn failure/cancellation Items now use bounded redacted
  `runtime-error/0.1` class/retryable metadata across protocol/provider/adapter/
  transport/timeout/sandbox/policy/tool/storage/workspace/Git/budget classes;
  schema-driven token usage, plan, and
  unified-diff notifications now map to bounded AAP timeline events. Native Agent command actions are not available. Agent
  writes/execution, structured patches,
  approvals, checkpoints, full Git
  workflows, complete durable session storage, and model control plane remain future
  gated work. Do not present the current preview as a complete coding Agent.
- Codex usage, plan, and unified-diff notifications now append unique AAP
  timeline Items when the Workbench data root is configured. Each metadata kind
  is capped at 32 updates per turn; one bounded truncation marker replaces later
  updates, and the item/event is committed through the existing Store append
  transaction. A stdio fixture proves usage/plan/diff Items survive sidecar
  restart and `session/read` replay. Standalone in-memory runtimes keep the same
  cap without durable writes. Qt projects `runtime-error/0.1` class and retryability
  without raw provider text. Task `7.10` is complete: deterministic fixtures cover
  cancellation, reconnect, compact-blocked degradation, provider-state-unavailable,
  lifecycle failure, and compensation failure, while the Qt render suite proves
  restart, archive, unarchive, and fork errors retain only operation/code/recovery
  guidance in labels and tooltips. Provider delete/compact product actions remain
  gated by `6.10`/`7.3`, and full turn semantics remain under `7.4`.
- Codex same-turn steering is now reachable through the out-of-band AAP control
  reader as `turn/steer`. It requires an exact active `session_id` and `turn_id`,
  caps each input at 64 KiB and pending requests at eight, and returns only a
  `steering-requested` queue acknowledgement. The adapter emits the pinned
  schema request with `expectedTurnId`; provider acceptance or rejection is
  reported separately as `turn.steering-acknowledged` or
  `turn.steering-failed`, while the steering message is a bounded user timeline
  Item. This remains read-only and does not grant Agent writes, commands, or
  approvals. Durable usage/plan/diff projection and complete steering/reconnect
  fixtures remain under task `7.4`/`7.10`.
- The adapter now classifies Codex output-channel EOF/read/write failures as
  retryable transport errors in `runtime-error/0.1`. A redacted
  `codex-recovery.jsonl` fixture and real stdio fixture cover partial agent output,
  transport failure, health exit, identity-preserving restart, recovered Turn
  completion, provider metadata read, and compaction request shape. Qt now renders
  content-free failure class/retry status for live turns and hides provider/Codex
  lifecycle and restart error text behind bounded operation/error-code/recovery
  status. Protocol and Qt projection coverage for this boundary is complete under
  task `7.10`.
- Provider failures now carry a content-free `provider-error/0.1` classification
  from Codex `codexErrorInfo` and HTTP status metadata through AAP
  `runtime-error/0.1`; response bodies, credentials, and dynamic provider text
  are excluded from Timeline items. The local gateway classifies 4xx/5xx,
  rate-limit, connection, and SSE-disconnect cases, strips spoofed diagnostic
  headers, and emits only bounded `x-aegisy-error-*` metadata. This is a
  provider-error contract foundation for task `9.9`, not a complete provider
  retry or routing policy.
- Internal `usage-authority/0.1` now validates source-qualified token, context,
  cost, and reasoning entries labelled observed, catalog-derived, estimated,
  stale, or unknown. Unknown values cannot carry numbers, stale values cannot
  remain authoritative, catalog-derived cost requires fresh catalog and
  derivation identities, and reports must contain exactly the four metrics.
  Codex `thread/tokenUsage/updated` Timeline items now retain raw bounded usage
  plus a validated report: provider token, context-window, and reasoning fields
  are observed, cost is unknown, and unreconciled totals are not rewritten. A
  deterministic metadata identity canonicalizes the complete validated report for
  `turn-trace/0.3` and `0.4`; the report remains a Provider-thread absolute snapshot and
  grants no Turn Attempt or Retry attribution.
  Qt validates the schema, complete metric set, authority flags/value kinds,
  and false automatic-compaction authority before rendering fixed labels and
  numeric values; malformed or unknown reports show a fixed unknown state and
  no provider text. This remains Codex-only and is not connected to catalog
  pricing, cross-provider correlation, billing, routing, or durable cross-turn
  threshold state; keep task `20.2` unchecked.
- OpenSpec task `20.1` now has an internal `turn-trace/0.6` contract with strict
  durable reads for `turn-trace/0.1` through `0.6`. Fixed hand-written legacy JSON
  and the version-specific behavior and identities of `0.1` through `0.5` remain
  unchanged; future `0.7+` versions and
  cross-version fields fail closed. The `0.2` Intent/completion-domain contract remains intact:
  one immutable Runtime-observed Intent identifies Chat conversation, Work read-only
  inspection, or future Work mutation, and completion never implies file changes,
  Git state, or passing tests.
  `0.3` adds at most one final `usage-report`. It binds the latest successfully
  validated and persisted Codex Provider-thread `usage-authority/0.1` snapshot,
  exact Timeline Item, deterministic report identity, observation time,
  `scope=provider-thread`, and `accounting=absolute-snapshot`; Attempt and Retry
  attribution remain explicitly unavailable. Runtime clone-preflights the candidate
  Trace, persists the ordinary Usage Item, and replaces the authoritative accumulator
  only after commit. Later valid notifications
  replace the retained absolute snapshot rather than being summed. Completed,
  failed, and interrupted traces retain the snapshot before Error/Terminal, while a
  Turn with no valid persisted authority report emits no fabricated Usage event.
  Store admission, direct read, and projection replay scan the complete bounded
  Session Item prefix through the target Turn's final Usage Item. They reconstruct
  one context-threshold latch from the single `NoAction` Session initial state,
  validate every prior Usage transition, verify Item hashes/bindings, rebuild
  authority from raw Provider values, and require the Trace to bind the final valid
  snapshot. The bound matches Runtime restoration's 100,000-all-Item fail-closed
  limit; later Turns cannot influence an older Trace. Genuinely malformed
  non-authoritative Provider metadata is ignored as a Trace candidate but advances
  the live, Store, and restart latch to conservative `PreviewRequired`; a later valid
  observation may clear it only through the normal hysteresis rule. Valid raw Usage
  with authority removed is rejected as a semantic downgrade. Direct reads recheck
  Session mode, project, and environment together, matching admission and replay.
  Initial-latch substitution, cross-Turn hysteresis downgrade, earlier-valid-snapshot
  substitution, raw/threshold tampering, binding substitution, and restart replay all
  fail closed or quarantine the Session.
  `0.4` adds content-free Codex command Tool lifecycle production. Started binds the
  closed Provider status/source, timestamp, action, and a stable identity over the
  fixed compile-time-known command/cwd/action projection while declaring
  `not-persisted`; unknown Provider keys and values do not enter this identity. JSON
  object keys are canonicalized and action-array order remains semantic. The adapter
  separately compares an opaque memory-only SHA-256 fingerprint of the complete
  untruncated Provider command/actions/cwd, including unknown fields. It is never
  serialized or persisted. Terminal observations are built from Store's exact
  would-be sanitized Item, preflighted on an accumulator clone, and become
  authoritative only after the command Item and any Artifact/Blob transaction commits.
  They bind a Session/Turn/Item domain-separated identity instead of the raw Provider
  Item ID, the complete sanitized payload SHA-256, output identity, duration, exit
  status, and terminal timestamp. Neither raw fingerprint, raw Item ID, command,
  cwd/path, action content, output, PID, Provider body, nor credential content enters
  the Trace.
  Producer admission serializes the complete outer durable envelope against the exact
  72 KiB limit, reserving every open Tool's worst terminal, worst legal failed/terminal
  metadata, and one emergency Started while admission is open. Capacity exhaustion
  retains and emits that Started, denies its terminal Item/Blob before persistence,
  and durably fails the Turn. Store independently rejects oversized Trace events on
  admission, direct read, projection replay, and restart. Fixtures also prove SQLite
  command Item failure and Provider completion with an unmatched Started retain
  Started + Error + failed Terminal with no terminal Tool, command Item, Blob
  reference, or object after restart. Provider `declined` is a Tool observation;
  it is not a Runtime denial or user Approval decision.
  `0.5` adds exactly one content-free Runtime approval-policy observation after
  Runtime and before Model/Context/Tool metadata. The pinned Codex producer binds
  the exact Runtime, `codex-app-server` adapter, Trace Runtime version `0.144.5`,
  durable adapter version `codex-cli 0.144.5`, fixed producing Runtime identity
  `aegisy-agentd:0.1.0`, domain-separated Provider-thread identity,
  configured/effective `approvalPolicy=never` identities, reviewer,
  read-only sandbox, and read-only permission profile. It always records
  `decision_attribution=no-user-decision`, `user_decision_observed=false`, and
  `execution_authority=false`. Store admission, direct read, projection replay, and
  startup quarantine independently rebind it to the durable Session Runtime row and
  reject missing or drifted adapter, version, permission, or Provider thread state.
  The fixed Runtime identity preserves existing `0.5` replay across a future binary
  version change; a new producer version must explicitly revise the Trace contract or
  add a reviewed compatibility entry instead of silently inheriting authority.
  `0.6` adds a distinct content-free Runtime-denial producer for active-Turn Codex
  command-execution, file-change, and permissions approval requests. Command/file
  requests receive `{"decision":"decline"}`; permissions receives
  `{"permissions":{},"scope":"turn"}`, an empty grant interpreted as denial rather
  than a literal decline response. The complete bounded request is hashed but never
  persisted; `request_kind` participates in the durable request and denial identities.
  Runtime preflights identity, duplicate/count/order, and exact durable budget into a
  non-serializable ticket, the adapter writes and flushes the response, and Runtime
  commits only after successful flush. A failed write or abandoned ticket produces no
  denial and follows the fail-closed Turn error path. A Trace preflight failure writes
  a fixed content-free error and reuses the backend only after that error flush
  succeeds. Any denial-response or fallback-error write failure marks Codex
  unavailable/restart-required instead of reusing a possibly blocked channel.
  Missing/invalid request IDs and malformed params receive fixed `id:null`, `-32602`
  errors and discard the backend; safely bound mismatches may reuse it only after the
  error flush. `decline-flushed` proves only a local child-stdin write/flush, never
  Provider receipt or action.
  Every denial has Runtime-observed metadata-only evidence and fixes user decision,
  approval authority, and execution authority to false. Store admission, direct read,
  projection replay, and startup quarantine revalidate the exact durable Runtime/
  adapter/version/Provider-thread/policy binding and denial identities. Invalid or
  mismatched active-Turn requests receive `-32602`, fail the Turn, and produce no
  denial or Approval. The checked-in generated Codex `0.144.5` schema lacks these
  three `ServerRequest` definitions; their current shape evidence is the same-version
  protocol source plus deterministic real-stdio fixtures, not generated-schema
  validation. The current read-only adapter still has no genuine-user Approval
  producer. Runtime denial, Provider `declined`, and `approvalPolicy=never` remain
  three distinct facts and none may be represented as user Approval. Both
  `turn-trace/0.5` and `0.6` reject every `Approval` payload until a durable authority
  producer and ledger binding exist.
  `WorkbenchStore::finish_turn_with_trace` still writes the unchanged outer
  `turn.trace.recorded/0.1` event immediately before the terminal event in one SQLite
  transaction. Turn Trace itself required no migration, backfill, or legacy event
  rewrite; schema v14 remains the model-profile projection introduction, schema v15
  introduced the separate Public Timeline Journal, and schema v16 adds its durable
  retention floor and validated Sequencer checkpoint.
  The Trace remains content-free: no prompt, provider body, path, command, diff,
  output, or credential content is recorded. Complete genuine-user
  Approval/Change/Test production, non-command Tool families,
  per-Attempt/Retry Usage authority, Timeline projection, AAP/Qt,
  audit/export, and retention remain absent; keep `20.1` and `20.2` unchecked.
- Codex startup supervision now has a bounded 15-second initialize deadline and
  at most three retries for transient output-channel, transport, write, read, or
  timeout failures. Version mismatch and protocol rejection are not retried; the
  final unavailable error is redacted and bounded. A crash fixture proves exactly
  three app-server attempts and an unavailable health state. A later child exit can
  be recovered through identity-preserving `runtime/restart`; active turns and user
  terminals block restart, and a stdio fixture verifies both recovery and the
  running-adapter guard. Qt `AgentRuntimeClient` now polls `runtime/health`,
  exposes content-free Codex exited/unavailable state in the workbench toolbar,
  and enables a visible `重启 Codex` action only when `restart_required` is true.
  Successful `runtime/restart` clears the action; failed attempts remain
  retryable. The control is unavailable during read-only store recovery and never
  changes the Agent read-only permission boundary. Full crash-loop recovery UI
  and cross-platform evidence remain under `7.2`.
- OpenSpec task `7.12` is complete. The versioned
  `docs/CODEX-ADAPTER-UPGRADE.md` runbook records the pinned compatibility
  matrix, candidate schema/fixture review, macOS/Windows contract gates,
  emergency pin, artifact rollback, incompatible provider-binding behavior, and
  content-free evidence requirements. It does not weaken exact-version rejection
  or the read-only permission boundary.
- Large command output now has bounded head/tail and content-addressed artifact
  retrieval, pre-capture secret redaction, fixed-frame Codex transport, and Qt
  full-output inspection. With durable storage configured, completed command
  artifacts commit atomically with their Item/event and remain session-readable
  after sidecar restart; without it, the existing bounded in-memory cache remains.
  Artifacts are never implicitly sent back to the model.
- Model-turn cancellation now has an exact-identity AAP request, Codex
  `turn/interrupt` mapping, distinct requested/acknowledged/interrupted states, an
  overload-safe stdin control path, and a stable Qt Stop state. User-terminal stop
  now uses the same overload-safe control reader and a shared authoritative terminal
  registry, so it remains reachable while a model turn blocks normal dispatch. Task
  `14.7` remains incomplete because native Agent foreground/daemon execution remains
  gated and Windows process-tree cancellation still lacks runner evidence.
- Completed Work commands now produce observed build/test/lint diagnostics for the
  initial Rust, C/C++, TypeScript, Python, ESLint, and pytest formats. Paths are
  revalidated through workspace policy, command identity is redacted, raw authority
  remains a session-scoped command artifact, and Qt exposes provenance/navigation.
- The first structured-edit milestone defines and validates create/update/delete/
  rename proposals with canonical roots and SHA-256 base/content identities. It is
  not connected to AAP execution or disk mutation; the runtime remains read-only.
- The Changes workspace now renders session-scoped structured-edit previews with
  authoritative base checks, aggregate/per-file diffs, `+/-` totals, blocking
  warnings, and paged content references. With durable storage configured, proposed
  content and complete diffs persist as one batch and remain session/project/edit-
  scoped and page-readable after restart. No AAP or UI apply operation exists.
- Codex fileChange now reaches the same preview compiler and persists an immutable
  Proposal before Runtime denial. A real stdio fixture proves the Proposal survives
  sidecar shutdown/reopen and that the proposed project path remains absent. Qt now
  reads and restores the latest strictly validated Session-bound Proposal into
  Changes, revalidates cached state after reconnect, auto-opens foreground Proposals,
  marks background Proposals unread without stealing focus, and keeps the surface
  explicitly read-only with no Approval or Apply action.
- A sidecar-library-only workspace-edit transaction now stages content in destination
  directories, performs optimistic base/target rechecks, uses no-clobber hard-link
  backups, rolls partial commits back in reverse order, preserves later external
  rewrites, reports recovery artifacts and authoritative states, and verifies final
  hashes. It is deliberately not dispatched through AAP or exposed in Qt.
- Internal proposal/restore overlap baselines bind touched source/target paths to
  expected absent or SHA-256 states. Detection includes saved disk changes and
  caller-declared unsaved editor paths, returns content-free conflict/resolution
  evidence, and is ready for later Git/non-Git checkpoint restore gates.
- Git repositories can now receive an internal pre-mutation checkpoint ref containing
  only touched preimages and a manifest that separates pre-existing dirty/pending
  user state from planned and verified Agent deltas. Capture and binding do not
  alter the user worktree/index and are not reachable through AAP.
- Non-Git projects can use an internal project-external SHA-256 blob/manifest store
  for touched preimages. It survives sidecar restart and verifies all content, while
  explicitly declaring weaker recovery than Git because it has no repository anchor
  and does not capture unrelated files or directory metadata.
- Git and non-Git checkpoints now feed one internal restore review/transaction layer.
  Bounded UTF-8 Agent changes support selective, full, conflict-confirmed, and
  idempotent restoration without including unrelated paths; Unix restore also
  recreates a deleted executable with its mode class. No restore AAP/UI action exists.
- Read-only Git status now uses porcelain v2 and reports repository root, HEAD/unborn,
  branch/detached, upstream, ahead/behind, staged/unstaged/untracked/conflict paths,
  rename origins, and operation-in-progress state. Qt's existing file decorations
  remain compatible; structured Git mutation is still unavailable.
- The Git workspace now shows repository/ref/worktree summary, commit history, and
  project-scoped worktree/staged/commit diffs. Only real selected diff text can enter
  structured turn context; stage, branch, commit, merge, and push remain unavailable.
- Task `16.3` has an internal branch plan/transaction foundation for create-and-switch,
  switch, and rename, with exact HEAD/OID, dirty, protected-branch, worktree-occupancy,
  post-state, and rollback evidence. It is not exposed through AAP or Qt and remains
  incomplete as a product workflow.
- Task `16.4` has an internal dedicated-worktree lifecycle foundation. Versioned plans,
  descriptors, health, and cleanup decisions bind an external storage path and exact
  base to session/child ownership; create/lock verification and rollback are covered.
  It is not persisted by a scheduler or exposed through AAP/Qt, so product completion
  and worktree deletion are not claimed.
- Task `16.5` has an internal selective staging foundation. It revalidates bound Git
  checkpoint applications, exposes content-free path/hunk plans, three-way merges
  selected Agent hunks into the existing index without staging user worktree-only
  content, and atomically installs or conservatively restores the index. It remains
  unreachable from AAP/Qt and is not a complete product workflow.
- Task `16.6` has an internal commit review/transaction foundation. It derives an
  Agent-only tree that excludes user index/worktree changes, binds exact patch,
  message provenance and identities, gates hooks/signing/custom merge drivers, and
  compare-and-swap updates the reviewed ref with verified event evidence. It remains
  unreachable from AAP/Qt and lacks durable events and authorized hook/signing paths.
- Task `16.7` now has an internal execution-complete plan, persistent operation/
  conflict journal, record+action-plan authorization scope, and real executor for
  stash, merge, rebase, single-parent cherry-pick, abort, and continue. Medium/high
  actions require distinct short-lived permission and approval decisions. A partial
  `WorkbenchStore` now provides private project-external SQLite WAL persistence with
  `synchronous=FULL`, foreign keys, application/schema integrity checks, durable
  allow-once decisions, atomic decision consumption, and typed content-hashed event
  replay with monotonic sequences. Direct authority calls repeat evidence binding,
  expiry, scope, and distinct-decision checks. This remains an internal fixture:
  production issuer/ledger integration, migrations/backup/compaction, typed session
  projection, AAP/Qt flow, reviewed conflict UI, sandboxed hooks/signing, and Windows
  execution proof are still absent.
- The internal `permission-profile/0.1` boundary now defines Chat, Read Only,
  Workspace Write, Developer, and Full Access profiles plus a managed-policy
  intersection. Effective checks cover canonical roots, sensitive/denied paths,
  symlink components, shell wrappers, executable and host allowlists, extension and
  browser scopes, and background execution. A private Git decision issuer requires
  a matching profile authority for permission decisions and a distinct bounded
  user-gesture ID for explicit approvals; it refuses read-only or managed-denied
  Git actions before SQLite mutation. This remains an internal foundation, not an
  AAP/Qt approval bridge or native execution grant.
- `WorkbenchStore` schema version 24 now verifies 35 required tables and persists
  canonical projects and roots plus
  Chat/Work sessions with project binding, environment identity, new/resume/fork
  lineage, and active/archived/failed/interrupted status. Work sessions require a
  project, lineage parents must match project and mode, archive/unarchive is
  timestamp-guarded, and reopen plus v1-to-v2 migration fixtures pass. Turns now
  have bounded idempotency/input hashes and terminal states; items have session
  sequences, turn binding, bounded redacted JSON payloads, and content hashes with
  tamper/gap replay checks. Every supported schema-v1-through-v23 source now migrates
  directly to v24; the v3
  path preserves existing events while allowing projectless Chat event streams, and
  v4 adds only the durable Blob schema. The v11 background-job and scheduler-lease
  projections plus the v12 notification outbox are
  internal, bounded, content-free canonical contract JSON, event-backed,
  generation-CAS protected, and startup-verified. The notification outbox is exposed
  only through the read-only session inspection capability described under `21.9`;
  no producer or delivery authority is exposed through AAP/Qt. Schema v13 adds the
  bounded `session_workspace_bindings` projection, branch index, semantic startup
  validation, and `session.workspace-bound/0.1` event. New Work Session creation,
  portable import, and fork commit this binding with their other Session projections;
  Work Session observation occurs before provider thread creation, and a later Store
  failure best-effort archives the newly created Codex thread. Event failure rolls
  back all local rows. Schema v14 adds the bounded authority-free model-profile
  projection and full-chain verification described above. Schema v15 adds the
  registered Public Timeline Journal/cursor and negotiated fixed-watermark catch-up
  described under task `3.5`. Schema v16 adds one validated content-free lifecycle
  checkpoint and retention floor per Session so a pruned prefix plus retained tail
  can restore the exact Sequencer state without reusing public sequences.
  The normal WAL-consistent migration backup covers every non-empty supported
  schema-v1-through-v23 source before its v24 transaction. Schema v19 adds the durable
  read-only Workspace Edit Proposal graph, schema v20 adds the metadata-only
  `mutation_acknowledgements` Turn ledger with a v19-to-v20 migration/backup, and
  schema v21 added the historical draft-only `mutation_reservation_records` wrapper,
  schema v22 adds complete typed source records, provenance, and internal lifecycle
  events with a v21-to-v22 migration/backup, and schema v23 adds immutable terminal
  outcome rows/events plus the reservation revision CAS with a v22-to-v23
  migration/backup. Schema v24 adds the content-free mutation-reservation
  consumption ledger, guarded `c1` to `c2` transition, bounded whole-Store semantic
  validation, and v23-to-v24 migration/backup without fabricating consumption.
  Final
  two-phase Session purge
  removes the binding in the same transaction as Turns, Items, and events. No
  repository absolute path, permission,
  dedicated-worktree claim, or mutation authority enters the binding. Extensions,
  Git checkpoint projections, complete scheduler/recovery state, and Runtime/AAP/Qt
  use of the model-profile projection are still future work. Runtime durable
  replay is now partially integrated: the Qt host's platform data root enables
  SQLite persistence for project/session creation, Preview turns, and completed
  Codex items/terminal states; `session/list` exposes bounded metadata filters and
  `session/read` can replay the stored projection after restart. Active projects are
  hydrated into the Runtime index on startup, reopening the same canonical root
  reuses its stored project identity, and Qt's left session rail can select and
  render the durable timeline. `session/title`, `session/archive`, and
  `session/unarchive` now update the durable/in-memory projection; archived sessions
  remain readable but reject turns, terminals, and edit previews, and an active turn
  or running terminal blocks archival. Qt exposes these actions from the session-row
  context menu. `session/read` now returns a bounded newest page and strict backward
  history cursor; Qt can explicitly prepend older pages while preserving the current
  reading position. Schema v7 adds a private `project_navigation` table for pinned/
  recent ordering. AAP `project/list` reports unavailable roots, relink requirements,
  session counts, and live-turn counts without loading transcripts; `project/navigation`
  persists pin/unpin changes with a typed event. Qt consumes this list for project
  selection, fixed/unavailable/live badges, and explicit relink confirmation. Complete
  approval/background active-state badges remain pending under task `6.4`. The runtime
  binding schema introduced in v8 persists adapter/version and opaque
  backend thread identity. `session/resume` reconstructs Preview sessions after
  restart and maps the pinned Codex 0.144.5 `thread/resume` contract when the exact
  binding is present; `session/fork` copies redacted history through the latest or a
  completed turn boundary and maps `thread/fork`. Missing/incompatible provider
  bindings fail explicitly and require a portable fork. Provider thread list/read now
  have bounded AAP metadata projections (`session/provider-list` and
  `session/provider-read`) with raw provider items omitted; provider delete/compact,
  complete lifecycle recovery, and full runtime reconstruction remain future work.
  Legacy Work Sessions migrated from schema v12 have no fabricated Workspace binding;
  direct resume fails with `-32146` and requires an explicit fork/rebind path. Fork can
  capture the currently reviewed project workspace while preserving portable history.
  Resume now updates the durable session environment identity and the
  `session.resumed` projection replay applies the same identity, so a resume cannot
  leave SQLite rows and the event-derived candidate with different environment state.
  A protocol isolation fixture now verifies that two Work sessions receive distinct
  environment identities while retaining read-only runtime profiles, and that
  structured context and in-memory history never cross the session boundary.
  A durable `session/resume` now hydrates the in-memory timeline from validated
  SQLite Items in 200-item pages, capped at 2,000 Items, before the resumed runtime
  becomes active; this is runtime state reconstruction only and never adds history
  implicitly to model context.
  The pinned Codex 0.144.5 schema also now has adapter-only, schema-driven mappings
  for `thread/list`, `thread/read`, `thread/unarchive`, `thread/delete`, and
  `thread/compact/start`, including thread-scoped metadata parsing. AAP archive and
  unarchive now update a loaded bound Codex thread, reject an unloaded provider
  continuation, and compensate provider state when local persistence fails. Provider
  list/read are now reachable only through those metadata projections; provider
  delete/compact remain unreachable from AAP until read scoping, user
  deletion/compaction review, recovery, and provider failure compensation are complete.
  Two-phase
  scoped deletion, retention policies, undo, purge, and Blob GC coordination are now
  implemented under task `5.8`.
- Portable session export/import is complete under task `5.9`. A documented 4 MiB,
  2,000-Item package previews content categories, rescans every string for secrets and
  registered project roots, excludes provider-opaque/environment/artifact state, and
  binds portable content to SHA-256. Import validates again, requires a target project
  for Work, gives the destination a fresh environment identity, and supports explicit
  transaction-revalidated `reject` or remapping `copy` collision behavior. Qt uses
  preview/confirmation and atomic `QSaveFile` output. Portable history does not carry
  provider continuation and must not be presented as a lossless runtime/model switch.
- Task `5.10` is complete. One persistence-secret invariant rejects recognized
  credential-like field names and secret values before SQLite projection/event
  side effects, including nested event JSON, identifiers, display/title text,
  canonical roots, identity fields, and durable Blob metadata. Item text is
  redacted before storage and replay rechecks that persisted bytes are already
  safe; event and projection-source replay refuse hash-consistent legacy secret
  payloads and quarantine the session on reopen. A source-less legacy export still
  re-redacts historical title text. Store fixtures cover nested JWT/API-key
  redaction, zero-row project/session/blob rejection, event-gate transaction
  rollback, and legacy replay quarantine. Qt sanitizes the sidecar process
  environment so login/refresh tokens, API keys, cloud credentials, JWTs, and
  authenticated proxies do not cross the host boundary; safe Workbench settings
  remain available. Credential IDs and ordinary model settings remain allowed.
- Task `6.1` is complete. New projects bind their primary root to a filesystem
  identity (Unix device/inode; Windows volume serial/file ID) rather than path text
  alone. `project/open` reuses the original project identity when a directory moves,
  returns explicit `moved`/`relink_required` metadata and candidate root, and never
  silently rewrites the trusted root. An exact missing saved path remains represented
  as `unavailable` with relink required instead of becoming a new project. Legacy
  path-hash identities migrate atomically through a typed project event, and replay
  applies that identity transition. Explicit `project/relink` now revalidates the
  reviewed root identity and persists `project.root-relinked/0.1`; the remaining
  navigation acceptance gates stay under task `6.4`. Clean Windows runner evidence is
  still required for the Windows metadata path.
- Task `6.2` now has a read-only first-open trust-review foundation. AAP exposes
  `project/trust-review`, and `project/open` returns a bounded
  `project-trust-review/0.1` snapshot containing the resolved primary root, access and
  symlink policy, repository metadata, instruction paths without instruction bodies,
  executable Hook paths with bounded size/hash metadata, scan truncation, and explicit
  policy impact. Qt renders the discovered paths and read-only/write/network/Hook
  consequences in the Agent timeline. `project/trust-acknowledge` re-scans the exact
  registered root, requires the reviewed root identity/hash, and appends a content-
  free `project.trust-acknowledged/0.1` event. Instruction and Hook content identities
  participate in the review hash without returning bodies; root or content changes
  invalidate the acknowledgement across restart. Duplicate acknowledgement is
  idempotent. Qt exposes explicit confirmation from the active project context menu
  and states that it grants no write, command, Hook, or network authority. Managed-
  policy intersection, the complete permission/approval bridge, and executable-
  content authorization remain incomplete; keep task `6.2` unchecked until those
  gates exist.
- Task `6.3` now has a scoped-root Runtime foundation. AAP exposes
  `project/root-list`, `project/root-add`, and `project/root-remove`; add validates
  read/write scope, canonicalizes existing directories, rejects a symlink final root,
  binds filesystem identity, and refuses duplicate canonical/identity roots. Remove is
  event-backed, cannot remove the primary `root-1` or the last root, and project
  projection replay plus restart fixtures preserve the remaining root set. Qt client
  and the project-list context menu expose a reviewed root dialog with explicit
  read/write selection, path/range/availability display, add confirmation, and
  non-destructive remove confirmation. Core `workspace/list`, `workspace/read`,
  `workspace/metadata`, `workspace/save-user-text`, `workspace/watch`, search,
  index/repository-map, LSP, observed diagnostics, and structured turn context now
  accept or carry an optional `root_id`; requests revalidate the registered root
  identity, and writes to read-only roots remain blocked. Persistent trust
  acknowledgement/invalidation, complete approval/authorization policy intersection,
  and full Agent authorization are still missing, so task `6.3` remains incomplete.
  Search cancellation is root-scoped for new clients with a legacy globally unique
  ID fallback; Qt drops late search/index/LSP/diagnostic responses from a different
  active root after a root switch.
- The project-external durable Blob store uses a private sharded SHA-256 layout and
  SQLite references with exact bytes, media/kind, content identity, session/project/
  source owner, hashed bounded metadata, access/verification time, active/released
  state, and retention deadline. Same-directory create-new staging, file sync,
  no-clobber hard links, directory sync, and read-time size/hash verification protect
  contents. Admission is capped at 16 MiB/object, 8,192 objects, 512 MiB total, and a
  256 MiB free-space reserve where capacity is available. Patch, image, diagnostic,
  workspace-edit, command-output, generic, and empty binary media share this path.
  Release always retains at least a 24-hour undo window; GC never deletes active,
  not-yet-expired, corrupt, missing, unknown, or unregistered objects. OpenSpec `5.3`
  is complete. Windows ACL/runtime behavior still requires its normal Windows runner;
  the macOS-to-MSVC check remains blocked before Rust code by missing Windows SDK C
  headers required by Tree-sitter.
- `WorkbenchStore` now has an internal bounded project/session-projection consistency and
  recovery layer.
  It checks project/lineage binding, turn hash descriptors, item sequence and turn
  ownership, item payload hashes, event sequence/cursor continuity, event payload
  hashes, Blob reference ownership/projection owner, reference metadata integrity,
  and referenced file size/hash without exposing stored content. A separate bounded,
  content-free scan reports missing/corrupt objects, dangling references, malformed
  entries, and unregistered files while preserving all uncertain orphans. The first
  durable `session/read` and bounded startup scan automatically repair a complete
  registered event source only when every issue is confined to rebuildable projection
  rows; otherwise unverified history is quarantined read-only. Fresh project/session,
  session title/status, Turn create/terminal, and sanitized Item append mutations now
  commit typed content-hashed events atomically with projections. Fresh sessions carry
  an event-source marker; migrated legacy sessions remain explicitly non-rebuildable.
  A complete fresh session stream can rebuild a separate candidate and, internally,
  atomically repair only session/turn/item projections after full-stream hash recheck
  under a write lock. Project creation and additional-root events likewise reconstruct
  exact independently scoped multi-root projections before dependent sessions. Repair
  appends immutable content-free audit evidence; tampered project authority quarantines
  every bound session. SQLite insert-abort and real child-process-exit fixtures prove
  destructive project and session rebuild rollback plus clean next-start recovery.
  OpenSpec `5.4` is complete. Content-free diagnostic export for store-open and
  migration failures is implemented under `5.6`.
- All ordinary `WorkbenchStore` `IMMEDIATE` writes now share one low-space and size
  admission boundary before lock acquisition. It preserves 256 MiB free space plus
  8 MiB transaction headroom and rejects a combined database-plus-WAL footprint above
  1 GiB with stable content-free codes and no projection/event side effects. Verified
  artifact content remains readable without an access-time metadata update during low
  space; state changes, releases, deletion, and new writes stay blocked.
- SQLite retains WAL with `synchronous=FULL`, a two-second busy timeout, 1,000-page
  automatic checkpoint, and 16 MiB journal retention limit. Explicit maintenance is
  unavailable during projection recovery and requires capacity for the shared reserve,
  headroom, twice the database, and current WAL. It truncates WAL, runs `VACUUM`, and
  requires a successful `quick_check` before reporting bounded byte/freelist evidence.
  OpenSpec `5.5` is complete; migration backup/recovery remains owned by `5.6`/`5.7`.

## Safety and Engineering Invariants

- Preserve dirty user worktree changes; never reset or revert unrelated changes.
- Agent/Codex remains read-only until permission, sandbox, approval, patch, and
  recovery milestones are implemented and tested.
- User editor saves require project-root authorization, optimistic revision
  checks, supported encoding/newline policy, and atomic replacement.
- Never expose desktop login tokens or long-lived provider credentials to Agent
  adapters, extensions, logs, command lines, or persisted event payloads.
- UI must consume AAP, not vendor-specific Codex or ACP events directly.
- A UI screenshot alone cannot complete an OpenSpec milestone; protocol,
  persistence, security, failure, and cross-platform evidence must match risk.
- Web workbench content must remain local, CSP-restricted, remote-network blocked,
  and external-navigation blocked. Keep the Qt editor as a failure fallback.

## Build and Verification Baseline

macOS development build:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@6
cmake --build build -j4
ctest --test-dir build --output-on-failure
$HOME/.cargo/bin/cargo test --workspace --manifest-path agent-runtime/Cargo.toml
$HOME/.cargo/bin/cargo clippy --workspace --all-targets \
  --manifest-path agent-runtime/Cargo.toml -- -D warnings
git diff --check
```

Historical AAP `3.3` verified baseline: the complete desktop run was 16 tests; that
Rust run had 8 AAP type tests, 630 passed sidecar unit tests plus
one explicitly ignored live Codex fixture, 6 daemon-main tests, 13 handshake Runtime
tests, 5 Draft 2020-12 handshake Schema tests, 63 Rust protocol tests, ten
context-threshold contract tests, 22 macOS sidecar stdio/Codex contract tests, and
Clippy with warnings denied. The focused Qt `agent_runtime_protocol`,
`agent_workbench_render`, and `agent_runtime_environment` tests pass. The latest unit
and protocol counts include the structured-plan dependency/evidence/stale contract,
the child-task scope/budget/handoff, lifecycle, dedicated-worktree admission,
runtime budget-ledger, unified-execution-plan, durable-job lifecycle contracts, and
schema-v11 job/lease persistence/CAS/migration/integrity fixtures, the schema-v12
notification outbox/event/paging/migration/purge fixtures, the owner-bound
read-only scheduler recovery snapshot, durable lease/process-identity classification,
the event-backed recovery-decision journal and semantic tamper checks, and Runtime-
owned process-observation contract, the four-kind content-free notification-intent
contract and semantic budget-snapshot validation, plus diagnostic, terminal, Git, and
child-handoff pinned-context authority, strict Git references, complete-source drift
detection, terminal normalization, image import/preview/assembly/release rollback,
source-loss fail-closed invariants, and schema-v13 Session Workspace binding,
branch-search, v12 migration, rollback, semantic-tamper, restart, and drift fixtures.

On 2026-07-20 the schema-v13 Session Workspace binding and `session-search/0.2`
stage passed 394 Rust unit tests with one ignored live fixture, 57 protocol tests,
11 stdio/Codex tests, strict Clippy, formatting, the complete desktop build, and
CTest `agent_runtime_protocol`. A real Git fixture proves atomic creation, restart
read/search/resume, exact branch filtering, and fail-closed resume after branch
drift; a store fixture proves event-failure rollback and startup rejection after
semantic hash tampering. The Qt render fixture now also requires `root-1` and the
bound branch, but this host killed the process at startup after 0.45s without any
assertion output. Tasks `6.6` and `12.6` remain unchecked for the control-plane,
turn-metadata, dedicated-worktree, scale, and cross-platform gates listed above.

On 2026-07-21 the model-catalog, capability-matcher, model-profile, and
catalog-policy foundation stages added the internal `model-catalog/0.1` and
`model-profile/0.1` validation contracts, the read-only AAP `model/catalog`,
`model/capability-check`, `model/catalog-cache`, `model/profile/list`, and
`model/profile/read`
projections, and the corresponding Qt request/signals. Qt validates the
metadata-only profile list and displays only its bounded count in the existing
model-binding tooltip; the model control remains non-selecting. The
policy-focused Rust verification passed 428 unit tests with one ignored live
fixture, 62
protocol tests, and 11 stdio/Codex tests; strict Clippy, the complete CMake
desktop build, and CTest `agent_runtime_protocol` passed. The projection is
explicitly offline and unsigned, leaves unverified capability/limit values
unknown, and grants no selection, token, routing, or execution authority.
`openspec validate build-aegisy-agent-workbench --strict` was attempted but the
host terminated the Node-backed CLI with exit 137 before producing validation
output; keep the prior successful validation as the last authoritative result
and rerun it when host memory is available. The focused
`agent_workbench_render` and `agent_runtime_environment` CTest processes were
also terminated by the host with exit 137 before assertions; the CMake build
itself completed successfully, so those render/runtime checks remain pending
on a host with sufficient memory.

On 2026-07-21 the signed Key Ring and root-anchored Trust Store stage passed 434
Rust unit tests with one ignored live fixture, 62 protocol tests, 11 stdio/Codex
tests, strict Clippy, formatting, the complete CMake desktop build, and CTest
`agent_runtime_protocol`. Focused fixtures prove an empty first-open marker,
root-signed bootstrap, previous-generation-signed rotation, restart catalog/cache
verification, wrong-anchor and forged/unknown/expired/revoked signer denial,
generation-gap denial, snapshot tamper/missing detection, and in-memory rollback
after disk commit failure. No production root, authenticated download, signing or
key-publication service, Runtime/AAP/Qt trust-store integration, model picker, token,
routing, or turn authority was added. OpenSpec `9.3`, `9.4`, and `10.1` remain
unchecked.

On 2026-07-21 the Runtime compatibility metadata stage passed 439 Rust unit tests
with one ignored live fixture, 62 protocol tests, 11 stdio/Codex tests, strict
Clippy, formatting, the complete CMake desktop build, and CTest
`agent_runtime_protocol`. Strict OpenSpec validation passed. Focused evidence covers
Codex/ACP/native family serialization, duplicate and contradictory metadata denial,
secret rejection, missing-version Unknown handling, exact-version matching, stable
version/adaptor blockers, warning degradation visibility, and the offline AAP
projection's false/absent selection, routing, token, Turn, and execution authority.
No signed production compatibility catalog, authenticated publication, real ACP or
native adapter contract fixture, or macOS/Windows compatibility matrix evidence was
added. OpenSpec `9.5` remains unchecked.

On 2026-07-21 the catalog refresh transport stage added nine unit tests and one
protocol test for the host-owned authenticated 200/304 contract, bounded
ETag/Last-Modified validators, identity content encoding, content-free failure
classification, and the read-only refresh status projection. The full Rust
workspace now passes 448 tests plus one ignored live fixture, 63 protocol tests,
and 11 stdio/Codex tests with strict Clippy. The incremental CMake build and
`agent_runtime_protocol` passed. A fresh CMake configure was killed by the host
while running Qt `uic -h`; 15 of the other 16 CTest processes were also killed at
startup by host resource pressure, while `agent_runtime_protocol` passed. The
Node-backed strict OpenSpec validator exited 137 before producing output. These
environment-limited checks are pending rerun on a host with sufficient memory;
the refresh contract remains partial and OpenSpec `9.4`/`10.1` stay unchecked.

On 2026-07-20 the schema-v12 durable notification outbox and its read-only AAP/Qt
inspection layer passed all Rust counts above, strict Clippy, formatting,
`git diff --check`, the complete desktop build, and CTest `agent_runtime_protocol`.
Focused evidence covers progressed-state idempotency, durable restart, stable
paging/cursor validation, event/projection rollback, semantic tampering, v11-to-v12
backup migration, session-purge cleanup, missing-storage degradation, and Qt metadata
validation paths. Task `21.9` remains unchecked because scheduler production,
platform delivery, permissions, and cross-platform evidence are absent. The
`agent_workbench_render` process was killed by the host at startup with no assertion
output; the earlier full CTest attempt could not
re-establish the 16-test desktop baseline because the remaining 15 test processes
were killed at startup by the host under memory pressure; they produced no test
assertion failure.
The Node-backed OpenSpec CLI was likewise killed with exit 137 before validation.
Treat the earlier 16-test run as the last complete desktop baseline and rerun CTest
and strict OpenSpec validation when host memory is available.

The process-observation fixtures use a real owned child on macOS and prove exact
generation rebinding, running/exited observation, no PID in serialized evidence, and
no inferred completion. The same implementation uses only cross-platform
`std::process::Child` ownership and includes a Windows `cmd.exe` fixture, but the
macOS-to-MSVC check is currently blocked in bundled SQLite C before this module by
missing Windows SDK headers. Do not claim Windows runtime evidence until the
`windows-2022` runner executes it.

## Session History Boundary

- AAP `session/read` returns the newest 100 projected items by default and accepts
  an explicit 1-200 item limit. Each replayed item carries its session-local sequence.
- Older pages use strict opaque-format `before:<sequence>` cursors generated only by
  the runtime. Malformed, non-canonical, zero, or out-of-range cursors fail instead
  of silently restarting or duplicating history.
- Page windows are stable when newer live items arrive because a cursor is an exclusive
  upper sequence bound. In-memory and SQLite restart replay use the same boundaries;
  durable pages still verify contiguous sequence and payload hashes before response.
- Qt shows `加载更早记录` only when the current page reports older history. It prepends
  the page in ascending order, deduplicates by item ID, and retains the reader's scroll
  anchor. This completes OpenSpec `6.8`, not project relink, event-projection rebuild,
  resume/fork, compaction, or complete runtime reconstruction. Scoped deletion is
  implemented separately under `5.8`.

## Session Search Boundary

- AAP `session/search` accepts bounded project, branch, model, runtime, status, title, and
  combined title/transcript filters. One page contains at most 100 Session rows and
  durable pages use the canonical exclusive cursor
  `after:<updated-at-ms>:<session-id>` ordered by update time descending and Session
  ID ascending. Malformed and non-canonical cursors fail instead of restarting.
- Capabilities `session.workspace-binding.read-only` and `session.search.branch`
  explicitly negotiate the additive Workspace projection and exact branch filter.
- Durable search runs in SQLite and returns only Session metadata, Runtime/Workspace
  binding metadata, and matched-field labels. It never hydrates all Session Items into the
  Runtime. Purged tombstones are excluded. Schema v9 creates and verifies additive
  indexes covering Session status/order, model/runtime binding, and transcript
  ownership through the existing WAL-consistent migration-backup gate.
- Transcript matching is restricted to `message` Items whose role is `user` or
  `assistant`, and only the visible top-level `text`, `content`, `output`, or `diff`
  field participates. Diagnostic, command, artifact, metadata, and arbitrary nested
  fields are not approved search sources.
- Qt debounces the left-rail query. Work mode binds search to the current project;
  Chat mode searches all projects. Empty results are explicit and clearing the query
  returns to the recent Session list.
- Branch search hashes the exact requested label and compares it with the immutable
  Session Workspace binding. Sensitive-shaped branch labels are represented only by
  their SHA-256 plus `branch_redacted:true` and are never returned as display text.
  A live Git overview may identify drift but cannot overwrite the Session binding.
  OpenSpec `6.6` remains incomplete until complete text-index scale, filter UI,
  model/dedicated-worktree control-plane integration, and final cross-platform
  behavior exist.

## Session Compaction Boundary

- The internal `session-compaction/0.1` library contract is limited to bounded,
  redaction-gated summary validation, content-hashed review identity, exact
  sequence/context activation checks, and content-free failure recovery options.
- A compaction review never replaces or deletes the original event history. The
  complete event stream remains the authoritative source for replay, recovery, and
  later durable checkpoint creation.
- `session-compaction-checkpoint-store/0.1` persists an exact validated review in
  the Workbench data root. Content-addressed objects and hashed session/checkpoint
  pointers are bounded, privately permissioned on Unix, published without clobber,
  revalidated after restart, and preserved unchanged when tampering is detected.
- `WorkbenchStore` can append an idempotent metadata-only
  `session.compaction-checkpointed/0.1` event and rejects malformed/tampered event
  metadata during projection replay. The external object and event are not one
  cross-resource transaction, and startup does not activate or recover compaction.
- AAP advertises `session.compaction.checkpoint-review` only when both durable
  stores opened successfully. Manual `session/compaction/checkpoint/create` derives
  its source identity from the complete verified session event stream, blocks
  active turns, persists before event registration, and treats an identical retry
  as the same review/event. `read` requires the exact validated object and matching
  event after restart. None of these methods activates the review or calls a provider.
- The Qt manual create/read review does not activate the review or call a provider;
  a read review can be edited only into a new immutable revision with an exact
  source Review ID, and the old checkpoint remains readable. Runtime and Store
  validate the source event and replay the `supersedes` lineage without copying
  summary/instruction content into SQLite. No model/provider summary producer
  consumes these reviews yet. Codex `thread/compact/start` remains unavailable until
  preservation instructions and summaries are editable and event-backed before
  activation, failure compensation/recovery is complete, and permission/provider
  gates pass.

## Operation Reconciliation Boundary

- The internal `operation-reconciliation/0.1` contract combines content-free event,
  process, workspace, and Git evidence into a bounded state, decision, blocker
  list, observed-domain list, and review ID. It does not read the host or execute
  a recovery action.
- No authoritative terminal event means `unknown`; a live process means `running`.
  Required evidence that is missing, changed, unavailable, or reports an in-progress
  Git operation blocks later writes. A completed/failed/interrupted event is only
  considered authoritative when no conflicting evidence remains.
- AAP `operation/reconcile` now persists a validated metadata-only
  `operation.reconciled/0.1` event in the bound session stream. Identical retries
  return the original event; a newer result creates a new review event. Startup and
  every session-bound request consult the latest event per operation, so unknown,
  running, or blocked results fail closed with `-32132` until an authoritative
  terminal review clears the gate. Event replay validates the evidence/result hash
  and identity without adding operation content to the projection. The reconcile
  method still does not perform probes automatically and there is no recovery
  action; Qt exposes the read-only review summary plus an explicit turn-only
  probe/reconcile review, while Git/Workspace/Terminal operation review remains
  unavailable.
  The read-only status query remains available while a session is pending
  deletion, and Qt refreshes it after deletion undo so a stale unknown state
  cannot keep a healthy session blocked.
- AAP `operation/probe` is the first host-observation boundary. It binds to a
  registered Work-session root, hashes bounded workspace metadata without reading
  source bodies, consumes the existing structured Git status query, and observes
  only runtime-owned turn/terminal state. It returns no paths/content/PIDs and
  leaves event state explicitly caller-supplied except for the bounded durable
  turn/Git mappings above. This is evidence collection only; it does not write
  events or execute recovery. Qt can explicitly submit a turn probe result through
  `operation/reconcile`, but the probe itself never mutates state.
- On durable startup, `WorkbenchStore::load_operation_reconciliations` scans the
  latest event for each session/operation pair, validates the content-hashed
  evidence and identity, and hydrates the Runtime cache with a 10,000-record
  bound. Per-request store checks remain authoritative when the cache is absent.
- `WorkbenchStore::latest_operation_event_state` maps only validated session
  `turn.created`, `turn.completed`, `turn.failed`, `turn.interrupted/cancelled`,
  and existing `git.workflow.*` lifecycle events to reconciliation evidence.
  Git conflicted/recovered and unknown operation event kinds return no authority
  rather than being treated as success; malformed Git lifecycle payloads fail
  closed even when their stored payload hash is internally consistent.
- `operation/status` is a content-free status projection for the latest blocked
  reconciliation in a session. It is intentionally available during the block
  and during pending deletion so Qt can explain the gate without adding a
  mutation or recovery path.

## Project And Session Projection Consistency And Rebuild Boundary

- Verification is internal and read-only. Startup caps project and session identities
  at 10,000 each, and caps project roots, Turns, Items, events, Blob references,
  database Blob objects, and disk entries at 100,000 per class. Limit exhaustion is
  an inconsistency, not permission to skip rows.
- Stable issue codes cover missing/mismatched lineage and project bindings, invalid
  turn hash descriptors, item gaps or cross-session turn references, item/event
  payload integrity, event gaps, event-cursor mismatch, Blob ownership/projection
  owners, reference metadata, file integrity, dangling references, and unregistered
  disk objects. Reports contain counts and codes only, never prompts, output, paths,
  payload JSON, hashes, or credential values.
- New session streams atomically append typed `session.created`, title/status,
  `turn.created`, terminal Turn, and sanitized `item.appended` events with their
  projection updates. `session_projection_sources` distinguishes registered v1 event
  sources from migrated legacy sessions; deleting an entire registered stream is an
  inconsistency, not a legacy fallback. Event payloads are limited to 72 KiB so the
  existing 64 KiB sanitized Item payload fits inside its typed envelope.
- Startup first scans project rows, root rows, and project event streams, then scans
  session rows, projection-source rows, and event-only streams before Codex starts.
  Scan/query/limit failure enters whole-store read-only recovery rather than starting
  Codex on unverified state.
- Complete sources repair safe project/root drift before repairing missing sessions in
  parent-first lineage order. Candidate validation rechecks project and parent bindings, every scoped Blob
  owner and content hash, source count, and the complete event-stream hash before the
  transaction. A first durable replay also repairs only a complete registered source whose
  issues are confined to rebuildable session/turn/item projection rows. It binds a
  SHA-256 identity over the complete event stream, rechecks it under an SQLite
  `IMMEDIATE` write lock, atomically replaces session/turn/item rows, and appends
  `session.projection-rebuilt` audit evidence. The AAP response reports
  `session-projection-read-recovery/0.1` status `projection-rebuilt`. Healthy reads
  report `not-needed`; older pages report `not-evaluated`.
- Event payload/sequence/cursor damage, Blob content or owner damage outside a missing
  projection owner, incomplete sources, legacy migrated sessions, and any unclassified
  inconsistency still return `-32115` without rewriting rows. Fixtures prove a restart
  repairs title plus missing Item projection rows, while tampered event authority
  produces no rebuild and no audit event.
- Uncertain sessions enter an in-memory quarantine recomputed on startup. Store methods
  reject session metadata, Turn, Item, Blob/Artifact, Git authorization/event, and
  destructive GC mutations before transaction start. Runtime blocks session-bound
  AAP writes with `-32115` but retains turn cancellation and terminal stop/remove for
  cleanup; healthy sessions remain usable.
- AAP `runtime/projection-recovery/status` returns bounded aggregate counts and
  `session/recovery/status` returns one content-free issue report. `session/list`
  labels `recovery_required`. Qt shows distinct whole-store, automatic-rebuild,
  aggregate-quarantine, and current-session recovery banners; it disables unsafe
  session, composer, terminal, editor, and workspace controls while leaving healthy
  sessions and cleanup operations available.
- `project.created` and `project.root-added` form a bound replayable project stream.
  Additional roots retain independent read/write scope; stored absolute canonical path
  text remains recoverable when a root is temporarily offline. A missing multi-root
  project rebuilds before its Work sessions, while incomplete/tampered project authority
  quarantines every bound session. Project and session rebuilds use `IMMEDIATE`
  transactions, recheck source cursor/count/hash, and append audit only on commit.
- SQLite insert-abort fixtures after destructive projection clears prove storage failure
  restores the complete previous project/root and session/Turn/Item rows. Separate test
  child processes exit from those actual transactions; raw reopen proves WAL rollback,
  no false audit, and successful automatic recovery on the next startup. OpenSpec `5.4`
  is complete. Blob scans still preserve and report uncertain orphans without deleting
  or repairing them.

## Migration Backup And Read-Only Recovery Boundary

- OpenSpec `5.6` and `5.7` are complete. Every supported
  schema-v1-through-v23 source-to-v24 migration first uses SQLite Online Backup to
  capture a WAL-consistent logical snapshot, then normalizes it to a standalone
  `journal_mode=DELETE` database. The migration connection acquires
  `BEGIN IMMEDIATE`, while a separately pre-opened read-only connection holds one
  `DEFERRED` snapshot across source-version/application-ID validation and backup.
  Database file identity is captured around initial open and rechecked before and
  after backup, before commit, and after commit; path replacement fails closed. A
  database already at v24 performs the identity check but never competes for a
  migration write lock.
- The private `migration-backups-v1` directory retains at most 16 bounded evidence
  sets. One backup is capped at 1 GiB and admission preserves the shared 256 MiB
  free-space reserve. Backup and manifest publication use create-new staging, file
  sync, no-clobber hard links, and directory sync where supported.
- A manifest binds source/target schema version, application ID, exact byte count,
  SHA-256, timestamp, and integrity state. Inventory and manifest reads are bounded.
  Partial files, invalid/unmanifested backups, tampered evidence, and unknown entries
  are preserved and reported; recovery never deletes uncertain evidence.
- Every migration validates the complete required v24 table/index/Trigger inventory
  inside its transaction before advancing `user_version` and committing. A newer
  schema is never downgraded.
  Stable content-free error codes distinguish backup, configuration, schema,
  integrity, migration, and newer-version failures.
- `WorkbenchStore::open_or_recover` returns either a writable store or a content-free
  read-only recovery diagnostic. Recovery inspection opens SQLite read-only and
  exposes schema/version state, boolean integrity/identity evidence, byte/count
  metadata, and stable issue codes only; it never includes database content,
  project paths, credentials, stored display data, or raw SQLite errors.
- Recovery Runtime startup does not launch Codex. Initialize advertises only
  `runtime.recovery.read-only`, `runtime.recovery.status`,
  `runtime.recovery.diagnostic-export`, and `permission.read-only`. AAP exposes only
  `runtime/recovery/status` and `runtime/recovery/export`; ordinary project, session,
  turn, workspace, terminal, artifact, and Git operations return `-32120`.
- Fixtures cover every supported source schema, migration schema collision with
  rollback, corrupt bytes preserved exactly, interrupted transaction re-entry,
  preserved partial evidence, low-space backup rejection, tampering without
  deletion, newer-schema recovery, locked version/application-ID drift, concurrent
  migration completion, migration-lock timeout/retry, current-v24 lock avoidance,
  and pre-backup/path-replacement identity. This is a migration/startup safety
  boundary, not the automatic session-projection repair or Qt recovery UI required
  by `5.4`.

## Database Write, WAL, And Maintenance Boundary

- Every ordinary metadata, projection, event, Blob-reference, approval, and Git
  authorization write uses the common database admission boundary. Migrations remain
  separate because they first require their versioned WAL-consistent backup.
- The admission reserve is 256 MiB plus 8 MiB fixed transaction headroom. The database
  page ceiling and combined database/WAL write ceiling are 1 GiB. Unknown platform
  capacity does not fabricate a value; SQLite errors remain authoritative.
- Low space is read-only for existing verified artifact bytes: their access timestamp
  may remain stale, but release, GC metadata, and all other mutations are denied before
  transaction start. A one-byte-below-boundary fixture proves no session, projection-
  source, or event row is created.
- WAL uses full synchronous durability, a two-second busy timeout, automatic checkpoint
  after 1,000 pages, and a 16 MiB retained journal limit. A real competing connection
  proves write contention returns within the bounded timeout without partial state.
- Explicit compaction is never automatic and never runs during project/session recovery.
  Admission requires `256 MiB + 8 MiB + 2*database + WAL`; maintenance truncates WAL,
  runs SQLite `VACUUM`, verifies `quick_check`, and returns counts/bytes only. It does
  not expose database content. Retention/deletion uses the separate boundary below.

## Retention, Deletion, And Content GC Boundary

- Schema v6 and later persist project/session retention policies and exact two-phase deletion
  plans. Policies can archive inactive active sessions and schedule deletion of archived
  sessions; a session policy overrides its project policy. Qt can read, set, or remove
  both project and session policies and the host triggers bounded maintenance on runtime
  connection.
- `session/delete/preview` accepts `session-only` or `lineage`. The plan hash binds the
  complete selected lineage, depth, status/update state, Turn/Item/event counts, and
  Blob-reference count/bytes for up to 10,000 members. Only the first 200 affected
  sessions are displayed; the reviewed hash still covers every selected member.
- `session/delete/schedule` repeats the exact plan before and under an `IMMEDIATE` write
  lock, requires the reviewed hash and a 24-hour-to-30-day undo window, then freezes every
  member without deleting content. Any selected descendant with an active Turn,
  running/stopping terminal, durable live Turn/approval, or recovery quarantine blocks
  manual or policy scheduling. Agent/Codex has no deletion producer.
- Pending sessions remain history/artifact-readable but reject metadata, Turn, terminal
  input/start, edit-preview, Blob, approval, authorization, and other writes.
  `session/delete/undo` appends durable audit evidence before removing the pending plan.
- After expiry, purge atomically removes Turns, Items, session events/cursors, projection
  sources, approvals, and authorization rows; it releases Blob references and extends
  physical content retention by at least another 24 hours. A minimal archived `Deleted
  session` tombstone preserves descendant foreign keys. `session-only` keeps children
  linked to that tombstone; `lineage` purges every selected descendant.
- Normal list/read and startup projection recovery exclude purged tombstones. Host
  maintenance also evicts resident session timelines, command artifacts, edit previews,
  and terminal records. Integrity-checked Blob GC remains conservative: active,
  not-yet-expired, corrupt, missing, unknown, unregistered, or recovery-uncertain content
  is preserved. OpenSpec `5.8` is complete.
- Qt exposes delete-range choice, descendant/Turn/Item/artifact impact review, destructive
  confirmation, a seven-day undo period, pending row/banner state, disabled mutation
  controls, context-menu Undo, and project/session retention dialogs.

## Portable Session Boundary

- AAP methods are `session/export/preview`, `session/export`,
  `session/import/preview`, and `session/import`; capabilities are
  `session.portable.export` and `session.portable.import`.
- The package versions are `aegisy-portable-session/0.1` and
  `aegisy-portable-session-content/0.1`. Compact UTF-8 JSON is capped at 4 MiB and
  2,000 contiguous uniquely identified Items; SHA-256 binds the complete content.
  The public contract is `docs/PORTABLE-SESSION-FORMAT.md`.
- Export requires a verified readable projection and a reviewed content hash. It
  rebuilds after preview so later Item/title changes fail stale rather than writing a
  mismatched package. Preview reports transcript, command-output, code/diff, and path
  categories plus redaction/exclusion counts and warnings.
- Every exported string is rescanned by the secret redactor and has every registered
  project absolute root replaced. Credentials, provider response IDs, hidden/encrypted
  reasoning, cache/continuation handles, environment identity, Blob bytes, and local
  artifact/content references are excluded. Import repeats schema, hash, sequence,
  duplicate-ID, payload, redaction, and size checks before transaction start.
- Work import requires an active target project and creates a fresh local environment
  identity. `reject` refuses source Session/Item collisions and rechecks them under the
  `IMMEDIATE` transaction; `copy` creates a new Session and remaps every Item ID. Same-
  store matching source/project/mode records fork lineage, otherwise lineage is new.
- Session, Item, projection-source, and `session.imported` audit writes commit together.
  The audit event is validated during projection replay but does not mutate the rebuilt
  Session/Turn/Item candidate. Failure leaves no partial destination rows.
- Qt limits import files before JSON parsing, previews target/content/collisions, writes
  exports through `QSaveFile`, and selects imported history. Provider continuation is
  intentionally absent; task `10.7` owns portable runtime/provider forking.

## Workspace Search Limits

- AAP methods: `workspace/search` and `workspace/search/cancel`.
- Search never leaves the authorized canonical project root and excludes sensitive,
  symlinked, built-in ignored, and Git-ignored paths.
- One request examines at most 20,000 directory entries, collects 5,000 files,
  reads 8 MiB of eligible UTF-8 text, retains 1,000 matches, and returns at most
  100 matches per protocol page; the Qt client requests 50.
- Search cursors bind to a deterministic file metadata snapshot. A cursor used
  after project changes returns `stale=true` and restarts from the new first page.
- Cancellation immediately stops the Qt workflow and invalidates later pages. The
  synchronous sidecar may finish the already bounded active page before processing
  the cancellation request; late results are ignored by request/search identity.

## Repository Index Limits

- AAP methods: `workspace/index` and `workspace/repository-map`.
- Supported first languages: Rust, Python, JavaScript, TypeScript/TSX, and C/C++.
  Versions are exactly pinned in the Cargo workspace and attribution is recorded in
  `agent-runtime/THIRD-PARTY-NOTICES.md`.
- Index visibility reuses the workspace sensitive-path, symlink, built-in ignore,
  Git ignore, file-size, entry, and file-count policy.
- A refresh reads at most 8 MiB of changed eligible UTF-8 files and retains at most
  20,000 symbols and 10,000 dependency edges. Unchanged files are reused by metadata
  revision; changed/new files are reparsed and deleted/ignored files are removed.
- Only paths, revisions, language, symbol ranges, and dependency labels are cached
  in memory. Source content and credentials are never persisted in the index.
- Repository maps accept 256-8192 tokens, use a conservative four-Unicode-character
  estimate, rank focused files first, and never include complete source contents.
- Dependency edges are syntax-derived imports/includes, not LSP-resolved semantic
  references. `workspace/index/cancel` is project-scoped and cancellation preserves
  the previous complete snapshot; the synchronous sidecar may finish an already
  running bounded index before processing its queued cancel request, while Qt
  immediately ignores that late response.
- Stress tests enforce the 5,000 candidate-file, 8 MiB parsed-byte, 20,000 symbol,
  and 10,000 dependency limits. Unicode paths, case-sensitive/folded search,
  rename/delete watch events, symlink denial, and external-edit conflicts are
  covered by unit, protocol, and render tests.

## Language Server Boundary

- AAP methods: `workspace/language-servers`, `workspace/language-server/start`,
  `workspace/language-server/stop`, `workspace/definition`,
  `workspace/references`, and `workspace/diagnostics`.
- Registered server families: rust-analyzer for Rust, Pyright for Python,
  TypeScript Language Server for JavaScript/TypeScript, and clangd for C/C++.
  Servers are discovered from fixed executable names or path-only `AEGISY_LSP_*`
  overrides; they are not downloaded or bundled automatically.
- LSP uses UTF-16 positions, at most 4 MiB per frame, 512 KiB per open document,
  500 returned locations/diagnostics, 2,000 characters per diagnostic message,
  and fixed startup/request/diagnostic timeouts.
- Returned paths are revalidated against the canonical project root, sensitive and
  built-in path policy, symlink denial, and Git ignore before entering AAP.
- Server environment inheritance is scrubbed to a minimal executable/system set;
  HOME and cache locations are isolated under the system temp directory. Incoming
  `PATH` entries must be absolute and outside the project root, server executables
  cannot resolve from the project, `workspace/applyEdit` requests are always denied,
  and no LSP source content or diagnostics are persisted by this milestone.
- clangd starts with background indexing and clang-tidy disabled. Rust analyzer
  runs in restricted standalone mode with project discovery, dependency fetching,
  sysroot discovery, build scripts, proc macros, cache priming, and check-on-save
  disabled because upstream documents that trusted project configuration can cause
  arbitrary code execution.
- Language-server binaries remain trusted external processes and are not yet under
  an OS sandbox. Only clangd is installed and end-to-end verified on this macOS
  machine; the other registered servers and clean Windows behavior still require
  installed-server validation. Do not claim full Cargo/project semantic precision
  while the restricted Rust safety mode is active.

## Observed Diagnostic Boundary

- AAP methods: `workspace/observed-diagnostics` and
  `workspace/diagnostics/raw`; `workspace/diagnostics` also returns the current
  observation and raw reference.
- Observations are stored only in sidecar memory. They contain normalized path and
  range, severity/code/message, source kind/identity, optional server or command,
  analyzed-file SHA-256 revision, observation/stale timestamps, freshness, and a
  content-addressed raw reference. Source content is never stored.
- A user save or watcher-reported path change marks matching observations stale.
  Recomputing the same source/server/file snapshot replaces its prior records.
- Each project retains at most 2,000 diagnostics, 128 artifacts, and 4 MiB of raw
  artifacts; one artifact is at most 256 KiB. Eviction or artifact reduction is
  explicitly reported as truncation.
- Raw artifacts contain only normalized diagnostics after root/sensitive/symlink/
  Git-ignore filtering. Language-server records are not original LSP wire payloads.
  Command records retain a reference to the authoritative session-scoped command
  artifact rather than copying unfiltered output into the project diagnostic API.
- Command parsing supports bounded rustc/cargo, Clang/GCC, MSVC/TypeScript,
  Ruff/mypy/Pyright, ESLint, pytest, and Rust-panic locations. Unknown commands or
  nonconforming lines are not promoted to diagnostics. Each command yields at most
  200 observations and a diagnostic-producing small output is forced into the same
  bounded command artifact store. Task `14.8` is complete; durable event-backed
  history and registered/provider-specific secret detectors remain later milestones.

## Turn Context Boundary

- AAP capability `turn.context.structured` adds typed context to `turn/start` for
  files, editor selections, diagnostics, search results, terminal excerpts, and
  Git diffs. The user-visible prompt remains separate from context evidence.
- Capability `turn.context.manifest` adds a `context-manifest/0.1` object to the
  `turn/start` response. Entries are bounded and content-free: source/kind,
  `pinned` priority, `untrusted-data` trust, a lowercase SHA-256 identity,
  conservative four-byte token estimate, freshness, inclusion reason, and
  included state. The hash is computed from the authoritative post-validation
  attachment; stale file revisions are labelled `stale`, and omitted/truncated
  context is never implied complete. The manifest is not persisted as source text
  and does not grant authority or activate compaction.
- The same prepared response carries `context-budget/0.1`. It allocates current
  explicit context and instruction items by deterministic priority score under
  the existing 64 KiB total/16 KiB per-item boundary, while preserving input
  order in rendered text. It labels existing task-state, recent-turn, tool-result,
  search, and repository-map classes when those items are explicitly supplied. The
  plan is metadata-only and does not imply tokenizer or provider-window authority.
- `turn/context/inspect` exposes that same plan and manifest before sending,
  with no model call, persistence, or source body in the response. The Qt Work
  composer exposes this as a read-only preflight dialog for an existing
  session; it displays only source/type/trust/size/status/reason metadata.
- One turn accepts at most 16 items. Rendered content is limited to 16 KiB per
  item and the complete context envelope to 64 KiB, including metadata and
  explicit truncation markers. Client-supplied inline content is also rejected
  above a 64 KiB aggregate input limit.
- The sidecar, not Qt, rereads file attachments and records the actual revision;
  a revision mismatch is marked stale. Every path-bearing item is revalidated
  against the bound project root, sensitive-path policy, symlink denial, and Git
  ignore before model context is assembled.
- Context evidence is labelled as untrusted data rather than instructions. The
  composer displays origin, size, inclusion, truncation, and removal before send.
- Work-mode turn/start discovers the primary-root instruction chain and appends
  at most eight valid instruction items after explicit user context, capped at
  32 KiB before the shared 16-item/64 KiB context boundary. Generated manifest
  entries preserve the instruction precedence rank and inclusion reason.
  Rejected or per-instruction budget-excluded files produce no-body exclusion
  entries when capacity permits; they never become authority or permission
  inputs. Secondary-root context paths do not select the primary-root chain.
- The terminal excerpt action now consumes real selected PTY/xterm output. The Git
  view still provides only the shared excerpt-action contract without fabricated
  data. Git diff context now comes from the project-scoped read-only task `16.2`
  query and the user's explicit selection. The internal `pinned-context/0.1`
  descriptor validates all planned source kinds and its project-external store
  durably preserves metadata-only sets. AAP list/save methods expose that store
  with project/root/session validation and compare-and-swap identity checks;
  standard `*:sha256:` Blob references receive a read-only SQLite metadata
  ownership/hash/byte check, and save records the separate metadata-only
  project event. `turn/context/inspect` and `turn/start` can consume only an
  explicit reviewed list of file/selection/image/diagnostic/terminal/Git/artifact pin IDs plus
  the exact current set identity; they never include every persisted pin automatically. Files are
  reread through the normal root/policy resolver and raw-byte hash or revision
  drift is marked stale before the same budget/manifest path is used; selection
  ranges are extracted only after that validation. Session-owned artifact pins
  resolve only validated command-output text references with matching UTF-8,
  byte count, and SHA-256. Diagnostic pins resolve only project/root-scoped
  in-memory normalized raw Artifacts with matching media type, reference, hash,
  and byte count; missing authority after Runtime restart fails closed. Terminal
  excerpt pins re-read the exact session/terminal/generation/absolute PTY range,
  apply the same ANSI/control normalization, and validate hash and byte count;
  missing or evicted authority fails closed. Inspection still returns metadata only.
  Git commit/diff pins re-run the filtered primary-root query and validate strict
  references, bounded plus complete source identity, bytes, and truncation; mutable
  diffs fail on drift and commit-backed context depends on the exact object remaining
  available. Session-owned image pins reread the durable Blob under exact project/
  session scope, revalidate PNG/JPEG/WebP hash, bytes, media, dimensions, and decode
  limits, and enter the manifest/budget without entering prompt text. Included images
  become temporary verified `localImage` paths only for the Codex turn. Child-task
  production, parent/child handoff persistence, cross-resource external-object/
  SQLite compensation, and orphan GC remain unavailable; the
  composer queue is otherwise transient. Qt render evidence covers file and
  editor-selection pin persistence, range labels, inclusion, order boundaries,
  and unpin. The diagnostic, terminal, and Git surfaces provide explicit authority-read,
  validate, CAS pin, include, and unpin behavior without persisting their bodies.
  Watch/save changes, diagnostic re-observation, and terminal lifecycle changes now
  project stale state both locally and through the sidecar's durable pin metadata;
  child-handoff production and complete cross-platform pin behavior remain open.

## Workspace Edit Boundary

- Task `15.1` defines a versioned internal `WorkspaceEdit` contract for create,
  update, delete, and rename operations. It is a data/validation boundary only;
  there is no AAP mutation method, content store, approval bypass, or apply path.
- Edits bind to a canonical project root and SHA-256 root identity. Every operation
  uses a bounded UTF-8 root-relative `/` path; absolute, parent, backslash-ambiguous,
  empty, duplicate, and rename-self paths are rejected before later preview/apply.
- Update/delete/rename require lowercase SHA-256 base descriptors. Create/update
  content is represented by a byte-counted hash and matching
  `workspace-edit-content:sha256:` reference rather than an unbounded inline body.
  Deserialization revalidates schema version, root identity, paths, hashes, and
  references so callers cannot bypass the constructor with forged JSON.
- Agent/Codex remains read-only. Preview and content paging do not authorize disk
  mutation, which remains gated by atomic apply, checkpoint, permission, approval,
  sandbox, and recovery work.
- Task `15.2` adds read-only AAP `workspace/edit/preview` and
  `workspace/edit/artifact/read`. Preview requires a project-bound Work session;
  session project, canonical root, and root identity are compared before edit
  deserialization or base reads. Artifacts are additionally scoped by session,
  project, edit ID, and SHA-256 reference.
- The sidecar rereads eligible base files and compares raw-byte SHA-256 while using
  normalized UTF-8 text for display. Sensitive, Git-ignored, stale, unavailable,
  existing-target, and symlink paths become blocking warnings; sensitive/ignored
  existing contents are never read into a preview.
- Proposed content is limited to 512 KiB per file and 4 MiB per edit. A file diff
  is limited to 512 KiB, aggregate diff to 2 MiB, inline file/aggregate text to
  32/64 KiB, and one page to 64 KiB. The in-memory store keeps at most 32 previews
  and 16 MiB, evicts oldest records, and clears at shutdown. Pinned `similar` 3.1.1
  provides timeout-aware unified text diffing under Apache-2.0.
- Qt exposes a Changes review table with aggregate/per-file `+/-` totals, warnings,
  plain-text diff, explicit continuation paging, and selected-diff context. There
  is no apply button or `workspace/edit/apply` method; public/Agent disk mutation
  remains gated by checkpoint, permission, approval, sandbox, and recovery work.
- Task `15.3` implements apply only as a sidecar library transaction. It accepts an
  already validated edit, referenced UTF-8 content, and the caller's authoritative
  Git-ignore set; it repeats canonical-root, sensitive/ignored/symlink, size, text,
  target-absence, and raw SHA-256 checks before mutation.
- Create/update bodies are written with `create_new` to hidden files in the target
  directory, flushed, and then linked into place without clobbering. Update/delete
  originals are hard-linked to unique hidden backups before their visible names are
  removed. Every destructive transition is journaled first; rollback runs in reverse
  and only removes a visible file whose hash still matches transaction-installed
  content. External rewrites are preserved and make rollback explicitly incomplete.
- Success verifies exact final hashes for create/update/rename, verifies delete
  absence, syncs affected directories on Unix, and reports cleanup warnings. Failure
  returns authoritative path states plus retained hidden recovery-artifact names.
  Unit tests cover stale-before/stale-after-stage, all operation kinds, partial/full
  rollback, conservative rollback failure, and policy denial. Task `15.9` adds
  deterministic partial-write, `StorageFull`, permission-loss, process-exit,
  stale-base, and rollback-incomplete fixtures. A post-commit child-process exit
  leaves the exact installed target and exact preimage backup; retry from the old
  base is stale. This does not claim automatic journal replay, and real disk quota,
  ACL, forced-termination, and Windows filesystem execution remain external tests.
- `workspace/edit/apply` is still method-not-found and Qt has no apply control. Do
  not connect the internal transaction to Codex/Agent until checkpoint, permission,
  approval, sandbox, and recovery gates are implemented and tested.
- Task `15.4` adds versioned internal `before-apply` and `before-restore` overlap
  baselines. Create/update/delete/rename source and target paths carry expected
  absent or exact raw-byte SHA-256 states; restore baselines additionally require a
  committed apply result matching edit/project/root identity and all final hashes.
- Overlap detection rereads only path metadata and bounded bytes, never returns file
  content, denies sensitive/Git-ignored/symlinked paths, and caps hashing at 512 KiB
  per file and 8 MiB total. It distinguishes created, deleted, content-changed,
  unavailable, policy-denied, and caller-declared `pending-user-edit` states. Disk
  changes without origin evidence are treated as overlaps but not falsely labelled
  as definitely user-authored.
- Before-apply conflicts require regeneration, explicit rebase, or user resolution.
  Before-restore conflicts require selective restore or explicit destructive
  confirmation. The detector is internal/read-only; checkpoint storage, restore,
  AAP/UI mutation, and durable origin history remain future gated work.
- Task `15.5` stores Git checkpoints as standalone commits under no-clobber
  `refs/aegisy/checkpoints/<id>` refs. The tree contains a bounded manifest and only
  touched-file preimage blobs; starting HEAD/branch/index tree, porcelain-v2 dirty
  state, pending editor paths, operation roles, before/planned-after hashes, mode,
  and `agent-only` versus `agent-on-user-base` ownership remain explicit.
- Capture supports at most 128 Aegisy checkpoint refs, 512 KiB per text preimage,
  8 MiB preimages total, 4 MiB manifest, and 5,000/2 MiB status evidence. Sensitive
  dirty paths are counted without retaining names. A pending editor path touched by
  the edit, stale/ignored/symlink path, unsafe ref, or drifted/tampered bind fails.
- Git plumbing uses a project-external absolute executable, cleared minimal
  environment, disabled hooks/fsmonitor/signing/prompts, and bounded output. Initial
  authorization requires repository root = project root and an in-root `.git`
  directory; parent repositories and worktrees with external gitdirs must wait for
  explicit metadata-root grants.
- Successful post-apply binding verifies checkpoint ref/commit/tree/manifest/blob
  OIDs, repository/gitdir, HEAD, branch, index tree, edit/root identity, and every
  final file hash. Checkpoint capture writes only Git objects/ref metadata and is
  internal; no AAP/UI apply, checkpoint, or restore method exists.
- Task `15.6` provides a persistent non-Git fallback under a trusted storage root
  disjoint from the project. Preimages and manifests are SHA-256-addressed; checkpoint
  IDs are no-clobber pointer files. Reopen/load verifies pointer, manifest, blob,
  byte-count, mode, edit/project/root/store identity, and post-apply final hashes.
- Every non-Git descriptor carries `weaker-non-git-content-addressed` and explicitly
  states that it has no HEAD/index anchor, does not capture unrelated files or
  directory metadata, and loses recovery if the external store is lost. Never render
  or describe it as equivalent to the Git checkpoint guarantee.
- The store denies Git worktrees, project-contained/containing storage, sensitive/
  ignored/symlink/stale/binary/oversized paths, and Agent paths with unsaved editor
  edits. Limits are 512 KiB/file, 8 MiB/capture, 4 MiB/manifest, 128 checkpoints,
  4,096 objects, and conservative admission below 256 MiB. Unix directories/files
  are restricted to 0700/0600 and content commits use no-clobber hard links plus
  directory sync. It remains internal with no AAP/UI mutation method.
- Task `15.7` is implemented internally. Git and non-Git verified preimages
  produce one content-free review; selections use operation indices so a rename's
  source and target stay grouped. Reviews expose desired/current hashes, conflict,
  pending-user, confirmability, and already-restored state without source content.
- Execution recomputes the complete review, requires exact current-state confirmation
  for destructive conflicts, refuses pending/policy-unavailable paths, compiles only
  selected targets into an inverse `WorkspaceEdit`, and reuses atomic apply. Tests
  cover selective-then-full restore, no-op restore, stale review, exact conflict
  confirmation, Git/non-Git plans, and unrelated-file preservation.
- Restore compilation carries checkpoint `100644`/`100755` intent into the inverse
  edit, so Unix full restore can recreate deleted executable files without losing
  their executable class. Restore remains internal; no AAP/UI checkpoint, apply, or
  restore action exists.
- Task `15.8` upgrades structured edits to `workspace-edit/0.2`. Create/update
  descriptors bind exact bytes to `utf-8` or `utf-8-bom`, `none`/`lf`/`crlf`, and
  `preserve`/`regular`/`executable` intent. Preview and apply share the same body
  validation and reject legacy/missing descriptors, binary/NUL/invalid UTF-8,
  mixed/lone-CR endings, descriptor mismatches, and content above bounded limits.
- Apply denies final and parent symlinks, preserves rename content/mode through a
  hard link, defaults creates to regular, preserves update permissions, and on Unix
  changes only the executable-bit class for explicit regular/executable intent.
  Final `100644`/`100755` modes are bound into Git/non-Git checkpoint application
  evidence and verified against tampering.
- Windows regular/preserve policy remains supported, but explicit executable edits,
  checkpoint capture, and restore are rejected because this internal layer has no
  durable POSIX executable-bit representation. Do not claim Windows executable-mode
  support without a deliberate Git metadata design and real Windows runner evidence.

## Git Read-Only Query Boundary

- AAP `workspace/git-status` is read-only and returns `git-status/0.2`. It reports
  Git availability, repository/worktree state, canonical repository root, HEAD OID
  or unborn state, branch or detached state, upstream, ahead/behind, detailed path
  entries, staged/unstaged/untracked/conflict sets, and operation-in-progress state.
- Status consumes `git status --porcelain=v2 --branch -z` plus structured
  `rev-parse`; it never parses localized human output. Ordinary, rename/copy,
  unmerged, untracked, and submodule records are bounded to 5,000 entries and 2 MiB.
- Known non-symlink Git-dir markers identify merge, rebase, apply-mailbox,
  cherry-pick, revert, bisect, or sequencer state. This state is derived from Git on
  every request rather than fabricated or separately persisted.
- Git and `check-ignore` use an absolute executable outside the project with a
  cleared minimal environment, fixed `C` locale, prompts/optional locks/fsmonitor/
  hooks/global/system config disabled, and bounded input/output. The current Qt
  file tree continues to consume compatible `path/status` decorations.
- Product Git construction now performs a bounded `git --version` preflight and
  fails closed with `-32041` below `2.31.0`, for malformed/oversized output, or for
  a failed check. `ignored_paths` intentionally uses a private compatibility
  runner without this product gate so an old Git cannot make ignored files appear
  in search or context; that exception grants no Git product capability.
- No structured branch, index, commit, worktree, merge, or remote mutation method
  exists yet. Do not infer task `16.3+` write capabilities from query visibility.
- AAP `workspace/git/overview`, `workspace/git/log`, `workspace/git/commit`, and
  `workspace/git/diff` return `git-query/0.1` structured data. Log pages accept
  1-100 entries and full-OID cursors; commit/diff revisions require complete
  lowercase OIDs, preventing option/ref ambiguity.
- Query limits are 2 MiB output, 2,000 changed paths, 256 branches, 512 tags, and
  128 remotes/worktrees. Nested-project diff and commit paths use top-anchored Git
  pathspecs but are reduced back to the authorized opened project.
- Sensitive and Git-ignored files, sensitive rename sources, and repository siblings
  are excluded before patch generation. Remote AAP data contains names and sanitized
  fetch/push authorities only; URL userinfo, path, query, fragment, and local paths
  are not returned.
- Qt exposes a quiet Git tool surface with summary, history, worktree/staged/commit
  scope selection, refresh, read-only patch, and explicit selected-diff context.
- Internal `git-branch-plan/0.1` plans bind canonical repository root, exact HEAD and
  branch, target/source OID, clean status, branch-name validation, protected patterns,
  and linked-worktree occupancy. Execution fully replans, verifies post-state, and
  has create/switch/rename rollback fixtures. Nested project roots are blocked.
- Branch mutation remains unreachable from AAP/Qt. Do not mark task `16.3` complete
  until permission/approval-backed product controls and their review flow exist.
- Internal `git-worktree-plan/0.1` planning requires the opened project to be the
  repository root and binds canonical repository/common-dir, exact base HEAD/branch,
  external non-overlapping storage, validated branch/worktree IDs, and session/child
  owner. Dirty/conflicted/truncated bases, operations in progress, symlink/reused
  targets, existing branches, and linked-worktree occupancy block creation.
- Internal creation fully replans, creates the exact-base branch/worktree, locks it,
  and verifies common-dir, branch, HEAD, clean state, lock, and non-prunable status.
  Descriptor health is rederived from Git/filesystem state; cleanup is only eligible
  for a terminal child, healthy clean worktree, saved editor state, and explicit
  integrated or discard-approved disposition. Injected post-add/post-lock failures
  remove the worktree and branch or report incomplete rollback.
- Dedicated worktree create/remove remains unreachable from AAP/Qt. Do not mark task
  `16.4` complete until durable child/session association, permission/approval, visible
  integration/discard review, and recovery exist. Arbitrary checkout filters and LFS
  behavior also remain part of later real-repository/policy validation.
- Internal `git-stage-plan/0.1` accepts only a reverified Git checkpoint application
  and binds ref/commit/tree/manifest/preimage objects, repository, HEAD/branch,
  edit/project/root, exact Agent results, current index tree, and stable index-file
  hash. Review data contains hashes, modes, ranges, counts, and hunk IDs, not content.
- Update staging three-way merges current index (ours), checkpoint preimage (base),
  and selected Agent hunks (theirs). Non-overlapping user staged changes survive;
  user worktree-only and unselected Agent changes stay out of the index. Overlaps,
  pending editor paths, stale state, conflicts/operations, unsafe policy paths, and
  deletes that would replace staged user content block before installation.
- Create/delete/rename use whole-path selection; rename transfers the current source
  index entry, preserving already staged content. Mode changes use a separate
  three-way decision. The standard `index.lock` is initialized from the exact index,
  permission-preserved, modified through the cleared Git runner, atomically installed,
  and tree-verified without changing worktree files. Rollback restores only if the
  installed index is still authoritative; a later external rewrite is retained and
  makes rollback explicitly incomplete.
- `workspace/git/stage` remains method-not-found and Qt has no stage/unstage control.
  Do not mark task `16.5` complete until permission/approval, durable staging events,
  and visible path/hunk review plus unstage recovery exist. Six macOS real-Git tests
  pass; Windows execution is not claimed, and cross-check is still blocked earlier by
  Tree-sitter C requiring Windows SDK headers unavailable on this host.
- Internal `git-commit-plan/0.1` binds the verified staging receipt, exact HEAD/
  branch/ref, staging-before/after trees, Agent-only commit tree, selected and
  excluded-user paths, exact bounded patch/hash/statistics, normalized message and
  user/Agent/template source, explicit author/committer identity/time/source, hook
  inventory/policy, signing policy, and blockers. Sensitive excluded path names are
  counted rather than exposed.
- The Agent-only tree uses the pre-Agent index as merge base, HEAD as ours, and the
  post-Agent index as theirs. Non-overlapping user staged and worktree-only content
  is excluded from the commit but retained in the real index/worktree. Conflicts,
  empty/unselected/sensitive results, stale receipts, custom merge attributes, and
  any configured external merge driver block before ref mutation or project code.
- Commit identities cross the cleared Git environment only through fixed explicit
  author/committer fields. Disabled hooks and unsigned commits are explicit reviewed
  policies; configured hooks/custom hooksPath are reported without custom paths and
  bypassed by plumbing. Requests to execute hooks or sign are blocked until sandbox,
  permission, approval, output artifact, and secure signer gates exist.
- Execution fully replans and compare-and-swap updates only the reviewed branch ref
  or detached HEAD. Index/worktree remain untouched; HEAD/ref/branch, parent/tree,
  raw commit metadata, and index tree are verified. `git.commit.completed` reports
  exact commit evidence. Rollback restores only if the ref is still transaction-owned;
  a later external rewrite is preserved and makes rollback incomplete.
- `workspace/git/commit/create` remains method-not-found and Qt has no commit action.
  Do not mark task `16.6` complete until permission/approval-backed review, durable
  event persistence, sandboxed hook failure/output handling, secure signing, and real
  Windows execution exist. Five real macOS Git fixtures pass.
- Internal `git-workflow-plan/0.2` plans stash capture, three merge modes, rebase,
  single-parent cherry-pick, and exact operation-ID/generation-bound abort or
  continue. It binds roots, HEAD/branch/index, full target OIDs, stash-ref baseline,
  predicted behavior, pending editor/redacted dirty state, message source, explicit
  identity/timezone, and hook/signing policy. Plans with execution ambiguity or
  irrelevant/missing merge metadata are blocked rather than interpreted later.
- Start preflight blocks dirty/conflicted/truncated state as appropriate, concurrent
  operations, pending editor content, protected-branch rebase, missing/stale targets,
  and configured custom merge/filter drivers that could execute repository code.
  Hooks and custom hooksPath presence are reported by name/boolean only. Every
  request requires permission and explicit approval; rebase and abort are high risk.
  Planning itself does not grant either authority.
- `git-workflow-record/0.2` persists reviewed metadata but no source/conflict content
  under a disjoint trusted external store. Directories/files are private on Unix; creation
  is no-clobber, updates are atomic, and schema, canonical root, semantic request/
  kind/target bindings, size/count limits, and SHA-256 envelope integrity are
  revalidated after reopen. A plan is fully regenerated before persistence so a
  changed HEAD, index, dirty state, or config invalidates the review.
- Reconciliation derives authority from live Git markers plus bounded unmerged index
  records. Matching operations become `in-progress` or `conflicted`; exact conflict
  stages retain mode/OID only, while sensitive conflict names/content are omitted and
  counted. Foreign and disappeared operations become inspect-only rather than being
  adopted. Allowed abort/continue actions are generation-bound to prevent stale UI
  recovery requests.
- `git-workflow-authorization-requirement/0.1` hashes the exact record and exact
  freshly generated action plan plus project, session, roots, action, operation kind,
  generation, observed Git state, and action-specific risk. Permission and explicit
  approval decisions each bind that hash, are `allow-once`, expire within five
  minutes, and must be atomically consumed by a
  trusted authority. Permission/approval IDs must be distinct; changing only the
  outer authorization ID cannot replay consumed decisions. The verified ticket is
  opaque and non-serializable and revalidates the current record before execution.
- The authorization module defines a trusted authority interface, not a permission
  engine. Production has no issuer or consumption ledger, so no caller can currently
  obtain execution authority. Never add a boolean approval shortcut or deserialize a
  verified ticket from UI/model input.
- The internal executor journals `prepared`, `dispatching`, and `observed/recovered`
  phases through advisory-lock and generation/record CAS before and after commands.
  It implements fixed noninteractive stash, merge, rebase, single-parent cherry-pick,
  abort, and continue with disabled hooks/unsigned commits. Postconditions verify
  ref/HEAD/index, merge metadata, cherry-pick provenance, rebase ancestry, and abort
  restoration. Restart recovery adopts matching markers or exact proven outcomes;
  anything ambiguous remains inspect-only.
- No stash/merge/rebase/cherry-pick/abort/continue mutation AAP or Qt action exists.
  Do not mark task `16.7` complete until the production permission/approval authority,
  complete durable atomic ticket consumption, typed session projection, sandboxed
  hook/signing policy, conflict/recovery UI, and real Windows evidence exist. The
  current SQLite store has six focused tests, while the broader 16.7 suite has
  thirteen state/store, four authorization, and six real execution/recovery fixtures
  passing on macOS.

## Terminal Boundary

- AAP methods: `terminal/open-user`, `terminal/list`, `terminal/attach`,
  `terminal/read`, `terminal/excerpt/read`, `terminal/input-user`, `terminal/resize`,
  `terminal/signal-user`, `terminal/stop-user`, `terminal/close-user`,
  `terminal/restart-user`, and `terminal/remove-user`.
- Creation and input are explicitly user-labelled operations and require a
  project-bound Work session. Chat sessions and other Work sessions cannot open,
  read, or control the terminal; no Agent terminal-open or command method exists.
- The sidecar revalidates the canonical project root immediately before spawn so
  the PTY library cannot silently fall back to HOME if a workspace disappears or
  changes. `$SHELL` must be an absolute executable; fallback order is `/bin/zsh`
  then `/bin/sh`.
- Shells start as clean interactive shells with user startup files disabled and
  consume the frozen environment belonging to their Work session. Provider/API
  credentials are not inherited.
- PTY bytes cross JSON-RPC as Base64. Each terminal retains at most the newest
  1 MiB with absolute output offsets and explicit omission metadata; one input is
  limited to 64 KiB and dimensions to 1-1000 rows/columns.
- Read-only `terminal/excerpt/read` snapshots at most 16 KiB from the retained tail
  and returns normalized UTF-8 text plus session/project/root, terminal generation,
  absolute start/end, SHA-256, and byte count. It strips terminal escape sequences
  with pinned `strip-ansi-escapes` 0.2.1 and removes remaining non-text controls.
  This endpoint is explicit user pin preparation, not durable terminal history and
  not an Agent read primitive; the corresponding descriptor never stores the body.
- Resize uses the native PTY and foreground signals target the current terminal
  process group. Close/runtime shutdown signal both foreground work and the shell
  session leader, retain exit code/signal for polling, and avoid unbounded waits.
  macOS user stop snapshots the active foreground/shell groups, allows fixed 150 ms
  HUP and TERM grace periods, then sends SIGKILL and uses the child handle fallback.
  Real fixtures cover ANSI/Unicode bytes and a foreground group that ignores both
  graceful signals; task `14.9` still awaits equivalent Windows runner evidence.
- Windows uses ConPTY, whose host channel is always UTF-8. Discovery prefers
  PowerShell 7, then Windows PowerShell, then absolute `ComSpec`/system cmd.exe;
  PowerShell profiles and cmd AutoRun are disabled, and cmd starts with code page
  65001 so its client encoding matches the UTF-8 ConPTY channel. Shells inside the
  project are rejected with case-insensitive canonical containment checks.
- Each Windows terminal owns a Job Object configured with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Assignment failure aborts startup; close or
  sidecar shutdown terminates the whole job. Ctrl+C is sent through ConPTY input,
  while terminate/hangup/kill use job termination. Windows 10 older than 1809 is
  reported as unsupported rather than crashing the sidecar.
- Windows code and tests pass isolated `x86_64-pc-windows-msvc` check and Clippy.
  A full macOS-to-MSVC workspace check cannot compile existing Tree-sitter C code
  without Windows SDK headers. The `windows-2022` packaging workflow runs the
  full Rust test/Clippy suite, but before 2026-08-04 that step silently
  continued after failed native commands, so its green status was not evidence
  of a clean Windows compile; the step now fails closed and the first real
  Windows compile is tracked below. Task `14.2` stays incomplete until that
  runner or a clean Windows VM supplies real ConPTY and process-tree evidence.
- Every Chat and Work session freezes a bounded environment at creation. Inheritance
  is allowlisted by platform; PATH keeps at most 128 existing absolute canonical
  directories, removes duplicates and project-contained targets, and adds known
  system fallbacks.
- Secret-like names are matched case-insensitively and removed, including token,
  secret, password, API key, credential, cookie, authenticated proxy, SSH agent,
  and cloud credential variables. AAP reports only a masked count, never names or
  values that could reveal configured providers or credentials.
- Tools derive an environment explicitly from the session snapshot. One derivation
  accepts at most 32 additions, 4 KiB per value, and 32 KiB total; it cannot override
  Aegisy session identity or add secret/dangerous loader and execution-control
  variables such as `LD_PRELOAD`, `DYLD_*`, `BASH_ENV`, `NODE_OPTIONS`, or askpass.
- Session and derived tool environments have SHA-256 identities. AAP exposes the
  identity, counts, PATH-entry count, and safe explicit variable names only. The
  runtime capability is `terminal.environment.session-scoped`.
- Lifecycle metadata is runtime-owned and includes foreground/background kind,
  bounded name, running/stopping/exited state, user-only input policy, generation,
  creation/update timestamps, process ID, and environment identity. Each Work
  session may have one running foreground terminal; background names are unique
  within the session. Restart preserves terminal ID/name and increments generation;
  running terminals must stop before user removal.
- Qt lists and attaches terminals by Work session, polls only the active terminal by
  absolute output offset, resets xterm when generation changes, and keeps inactive
  background output in the sidecar's bounded capture. Official local xterm.js and
  FitAddon provide ANSI rendering and resize; native Qt output remains the failure
  fallback. Copy/paste crosses a native clipboard bridge only after a user gesture,
  with paste bounded to the same 64 KiB input limit.
- Web terminal resources use CSP, local-only request interception, disabled remote
  navigation/storage/plugins, and no runtime network dependency. A crashed renderer
  retries the trusted local page twice and reattaches to the authoritative bounded
  tail. Desktop process/session durability remains the later event-store and session
  recovery milestones; runtime shutdown still terminates all owned process trees.

## Structured Command Boundary

- The installed Codex App Server `0.144.5` generated JSON schema is the current
  adapter contract evidence for `commandExecution` item fields and
  `item/commandExecution/outputDelta`; do not guess vendor field names from memory.
- The adapter maps started/delta/completed notifications into one AAP timeline item
  with a redacted command shape, bounded redacted vendor-parsed actions, cwd,
  vendor status/source/process
  ID, duration, exit code, and a 256 KiB UTF-8 tail. The visible timeline is further
  bounded to 32 KiB and forced to plain text so tool/model output cannot become Qt
  rich text.
- Conservative Aegisy risk classification recognizes destructive Git/filesystem,
  remote Git, network, privilege, metadata, dependency-script, and project-code
  execution patterns. Unknown or missing vendor actions are at least medium risk;
  current classification is advisory evidence, not an approval decision.
- Codex App Server now starts with `env_clear` from the shared allowlisted environment
  builder. API keys, authenticated proxies, cloud credentials, loader injection,
  askpass, and execution-control variables are scrubbed; AAP exposes only the
  parent-process environment hash and counts plus the hashed, versioned
  `codex-child-environment/0.1` launch contract. The contract binds allowlisted
  platform inheritance, credential/proxy redaction, loader injection denial, and
  execution-control denial. Codex does not report whether a specific child modified
  that parent snapshot, so the item explicitly records
  `vendor-command-item-does-not-report-child-environment` rather than claiming
  exact child-process proof.
- The real macOS stdio command fixture now asserts structured Item cwd presence,
  environment identity/binding, value-free metadata, conservative risk level,
  duration, exit code, and diagnostic artifact linkage. This strengthens task
  `14.5` evidence without claiming child-process mutation proof or native command
  producers.
- An explicitly ignored macOS live fixture has run against the installed pinned
  `codex-cli 0.144.5` selected through `AEGISY_CODEX_PATH`, proving app-server
  initialize and read-only `thread/start` while keeping the adapter permission
  profile read-only. It does not start a model turn or claim command-item or
  child-process observation; task `14.5` therefore remains incomplete.
- Codex remains configured with `sandbox=read-only` and `approvalPolicy=never`;
  approval requests are declined. No AAP/native Agent command-open or arbitrary
  execution method was added. User terminal input remains a separate explicit user
  operation.

## Command Output Boundary

- Streaming command output retains a Unicode-safe 64 KiB head and 192 KiB tail,
  with exact total, retained, and omitted byte counts. The Qt display remains
  separately limited to 32 KiB.
- Output exceeding the inline budget creates a content-addressed
  `command-output:sha256:` artifact with at most a 1 MiB head and 1 MiB tail plus
  an explicit omission marker. Small output stays inline unless it is the raw
  authority for accepted command diagnostics, in which case a session-scoped
  artifact is forced.
- The legacy `artifact/read-command-output` read remains session-scoped for
  compatibility. Production Qt uses the Session/Item/reference-bound
  `artifact/read-command-output-page` route. The live cache retains at most
  64 artifacts and 16 MiB per session and clears at runtime shutdown. With the Qt
  host's default platform data root (or an explicit override), a completed command
  artifact and its
  Item/event/reference commit atomically, carry 30-day retention metadata, and fall
  back to verified durable content after cache eviction or sidecar restart. Without
  durable storage, the bounded in-memory behavior remains unchanged.
- A streaming redactor runs before capture and recognizes secret assignments,
  authorization values, `sk-`, `ghp_`, `github_pat_`, and JWT token shapes across
  delta boundaries. Timeline/artifact metadata reports raw source byte counts and
  redaction counts; tests prove recognized values do not enter head, tail, artifact,
  timeline JSON, or AAP read responses.
- Codex stdout accepts at most 4 MiB per newline-delimited JSON message and crosses
  a synchronous queue of 16 messages. Queue saturation applies pipe backpressure;
  oversized frames are fully drained then fail with a content-free transport error.
  Codex stderr is drained with a fixed 8 KiB buffer and is not copied into logs.
- Qt retrieves a completed artifact through paged requests bound to the originating
  Session, wire Item, reference, and Runtime generation, renders accumulated UTF-8
  as read-only plain text, and never adds it to model context implicitly. The
  read-only dialog exposes an explicit `固定完整输出` action only after terminal
  length, SHA-256, and canonical truncation-marker validation. It persists only a
  metadata descriptor through the existing pinned-context CAS and remains disabled
  for cross-session or blocked/recovery states. Task `14.6` is complete; the remaining Artifact pin
  lifecycle and non-file pin classes remain under OpenSpec `17.3`.

## Cancellation Boundary

- AAP `turn/cancel` requires active `session_id` and `turn_id`. The first request
  returns `cancellation-requested`; repeats are idempotent, while stale, mismatched,
  or completed identities fail explicitly. Acceptance never claims the turn has
  already stopped.
- The installed Codex App Server `0.144.5` schema defines
  `turn/interrupt { threadId, turnId }`. Aegisy emits separate
  `turn.cancellation-acknowledged`, `turn.cancellation-failed`, and terminal
  `turn.interrupted` events. Completion can legitimately win a cancellation race.
- Sidecar stdin is read independently of synchronous Runtime work. Normal requests
  cross a fixed 32-entry queue and receive `-32004` on overload; turn cancellation
  uses a narrowly scoped control path so it remains reachable under saturation.
  Shared stdout writes whole JSON lines under a mutex.
- User `terminal/stop-user` uses the same narrowly scoped stdin control reader and
  an `Arc<Mutex<...>>` terminal state containing both the platform manager and its
  lifecycle records. Normal and out-of-band operations therefore serialize against
  one owner; the manager rechecks exact session/terminal ownership before signaling.
  A macOS process fixture proves stop remains responsive during a blocked turn and
  saturated queue and leaves no live foreground child.
- Qt changes the fixed-size Send action to Stop and then Stopping while a turn is
  active, and restores Send only after an authoritative completed, failed, or
  interrupted event.
- Native Agent foreground commands and daemon processes remain unavailable. Do not
  mark task `14.7` complete until those process classes have reachable cancellation,
  escalation, race, and orphan-cleanup evidence on macOS and Windows, and the user-
  terminal path has corresponding real Windows Job Object execution evidence.

## Release Incident: Windows `TLS initialization failed`

Status: the original user-machine root cause cannot be proven without the failed
installer/runtime DLL inventory, but the verified packaging flaw is closed in the
release script. Clean Windows x64 validation is still mandatory before publishing.

Original packaging behavior that caused the release gap:

- `package-windows.bat` links against the CMake-selected OpenSSL installation but
  later copies every DLL from independently supplied `OPENSSL_DIR`.
- The script only proves that the staged application remains alive for four
  seconds. It does not perform a real Qt TLS handshake or verify the loaded SSL
  backend and library versions.
- This allows a build/runtime OpenSSL mismatch or incomplete transitive DLL set to
  pass packaging and fail only on a user's machine.

Implemented release gate:

- `package-windows.bat` requires `OPENSSL_ROOT_DIR`, passes it to CMake, and only
  accepts `OPENSSL_DIR` inside the same root.
- `release/verify-windows-tls-runtime.ps1` rejects ambiguous, mixed, mismatched,
  non-x64, or wrong-hash SSL/crypto runtime DLLs and checks Qt Network imports.
- `AegisyTlsProbe` checks `QSslSocket::supportsSsl()`, reports build/runtime SSL
  versions and backend, and performs a real HTTPS request from the staged layout.
- Packaging stops before installer creation if the verifier or TLS probe fails.
- The staged package must include Qt WebEngine runtime files and Monaco workbench.
- Windows packaging now also requires and copies `aegisy-agentd.exe`; the
  `windows-2022` workflow runs Rust tests and strict Clippy before installer build.

Remaining release verification:

- Build and run the complete installer on a clean Windows x64 VM with no developer
  OpenSSL or Qt directories on `PATH`.
- Run the installed application and TLS probe against the production endpoint.
- If it fails, preserve the probe output and staged DLL hashes; never work around it
  by copying unrelated OpenSSL DLLs into the package.

Do not treat “the GUI process stayed alive” as proof that Windows TLS works.

## Incident: `stream disconnected before completion`

Observed message:

`stream disconnected before completion: Transport error: network error: error decoding response body`

Status: mitigation and redacted diagnostics are implemented; the exact upstream
failure still requires a production reproduction. The observed local configuration
used a direct Aegisy provider URL with the Responses API, so the Node gateway cannot
be assumed to be the failing layer for that occurrence.

Generated Codex provider configuration now includes:

- `request_max_retries = 4`
- `stream_max_retries = 5`
- `stream_idle_timeout_ms = 600000`
- `supports_websockets = false`
- `accept-encoding = identity` to avoid a broken compressed response body path

The local gateway records a redacted request ID, upstream status, content and
transfer encoding, bytes, duration, and whether termination was a normal end,
upstream abort/error, or client close. It strips hop-by-hop response headers,
preserves streaming backpressure, and must never log credentials or body content.

Likely layers to distinguish before changing retry behavior:

1. Upstream Aegisy/provider connection closed before the streaming body completed.
2. Local gateway forwarded a truncated or incorrectly encoded/compressed body.
3. Proxy/VPN/antivirus interrupted long-lived HTTP streaming.
4. Provider returned a non-streaming HTML/error/compressed response under a 2xx
   or otherwise incompatible response contract.
5. Codex App Server surfaced an underlying SDK transport/decompression failure.

Required diagnostics:

- Record redacted request ID, tool, route, upstream status, content type,
  content encoding, transfer encoding, bytes received, duration, upstream end vs
  abort/error event, and retry attempt.
- Never log authorization headers, API keys, prompts, or response content.
- Reproduce once through the gateway and once directly against the same endpoint.
- Do not automatically retry a mutating/tool turn until idempotency is proven.

If the error recurs, first determine whether the active Codex config points directly
to Aegisy or to `127.0.0.1:43112`, then correlate the timestamp/request ID with the
gateway or upstream logs. Retries reduce transient failures but do not make a
truncated or invalid response contract correct.

## Desktop Client Download Routing (2026-07-28)

- Claude Code maps to the Claude Desktop installer and Codex CLI maps to the
  ChatGPT Desktop installer. On supported macOS/Windows targets both mappings use
  `DesktopDownloader` and the authenticated
  `/api/v1/desktop-downloads/{product}/{platform}` Aegisy proxy; they do not open
  the public download page from a download action.
- One shared `proxiedProductForTool` policy owns the tool/product/platform decision
  for the dashboard, environment check, and desktop-download dialog. Public-page
  fallback remains only for tools or platforms without a supported installer, such
  as ChatGPT on Intel macOS. The proxy still rejects redirects, missing proxy
  attestation, HTML/JSON bodies, oversized payloads, and invalid package signatures.
- `desktop_download_proxy_contract` covers the Codex-to-ChatGPT proxy mapping,
  exact proxy path family, Claude mapping, unsupported tools/platforms, and installer
  format checks. Server-side proxy/CDN and signing evidence remains governed by
  `docs/DESKTOP-DOWNLOAD-PROXY.md` and clean platform release validation.

## UI Direction

The first shared visual-system pass is implemented, but the workbench remains an
engineering preview rather than the final product. The redesign uses an original
Aegisy design language rather than copying a third-party product:

- Quiet, work-focused coding-tool layout; dense, scannable, and predictable.
- Shared design tokens for spacing, typography, borders, focus, selection,
  semantic status, and light/dark themes.
- 8px maximum card/control radius unless a platform convention requires less.
- Consistent 8-12px control spacing and 12-20px page padding.
- Replace Qt stock icons with one coherent SVG icon set and accessible tooltips.
- Use layouts and responsive constraints; never hard-code screen coordinates.
- Style scrollbars, menus, inputs, tabs, splitters, tooltips, dialogs, and status
  surfaces globally instead of accumulating widget-local QSS fragments.
- Prefer Qt-Material/QDarkStyleSheet only as reference or optional dependency.
  Aegisy should own its token layer so Monaco, Qt, macOS, and Windows remain
  visually consistent and dependency licenses remain controllable.
- Avoid frameless-window, blur, heavy shadows, and decorative animation until
  accessibility, resizing, native window behavior, and performance are verified.

Implemented visual baseline:

- Fusion provides a predictable cross-platform control base beneath Aegisy QSS.
- Shared Qt palette and QSS use the `#165DFF` accent, neutral canvas/surfaces,
  consistent focus/selection states, 8px maximum radius, and platform-aware CJK
  font fallbacks.
- Monaco uses matching editor tokens so the native shell and web editor agree.
- Monaco loading overlays must define an explicit `[hidden] { display: none; }`
  rule; an author `display` rule can otherwise override WebEngine's default hidden
  behavior and cover a ready editor. Render tests verify both split regions.
- The read-only context preflight now displays the negotiated tokenizer authority
  and each budget entry's conservative estimated token count alongside byte size.
  It reads 64-bit JSON values without claiming provider-window precision; the
  inspection remains content-free and model-free.
- The Agent surface header now exposes a compact execution-context strip with
  Chat/Work mode, project, persisted workspace root, Runtime readiness/recovery
  state, provider/model, fixed read-only permission, Session-bound Git branch, and
  selected-context count. Qt keeps bounded Runtime and Workspace bindings per Session
  and consumes the bindings from
  start, resume, fork, read, and search responses. Switching Chat/Work or replaying
  a Session updates the model display from that Session rather than a global last
  value. Adapter/version/provider/model metadata is bounded and control-free, and
  only exact `read-only` permission is accepted; missing or malformed data becomes
  `Runtime 未绑定` plus an explicit unknown/read-only gate instead of stale authority.
  Workspace metadata must match `session-workspace-binding/0.1`, contain no raw paths,
  and grant no permission. The live read-only Git overview is comparison-only: a
  mismatch marks the persisted branch as drifted and never overwrites the Session
  binding.
  The render fixture locates the strip by a stable object name and executes the
  empty read-only state plus a real Git fixture branch, `root-1`, and active Runtime/
  Workspace binding as part of the passing desktop CTest suite.
  The model control is still a binding display, not the model-profile picker or
  switching control required by OpenSpec `10.3` through `10.6`.
- Primary workbench actions use a small vendored Lucide SVG set with accessible
  tooltips; its ISC/MIT license is stored under `assets/icons/lucide/LICENSE`.
- Combo-box arrows use the shared Lucide `chevron-down` SVG resource. Do not build
  CSS border triangles in QSS: Qt renders those browser-style borders as bars on
  some styles and display scales. Widget-local QSS must not hide the shared arrow.
- Do not layer Qt-Material, QDarkStyleSheet, or Fluent QSS over the current 400+
  local style rules. Continue consolidating local QSS into semantic components.
- The main three-pane Workbench splitter now persists only its bounded Qt layout
  state in device-local `QSettings` and restores the fixed rail/conversation/
  workspace defaults when the setting is absent, malformed, or over 4 KiB. No
  project path, Session content, permission, or Runtime authority is stored in
  this layout key. The toolbar exposes an icon-only reset action with an
  accessible tooltip that restores the defaults. Editor tabs and split state
  remain separately project-scoped; the render fixture verifies the reset path.

## Verification Snapshot (2026-07-26)

- The earlier contract-only Timeline subscription foundation passed the complete
  Rust workspace: 37 AAP type, 729 `aegisy-agentd` library tests with one ignored
  live fixture, 7 daemon-main, 10 context-threshold, 14 handshake Runtime, 20
  handshake Schema, 67 protocol, and 23 stdio/Codex tests (907 passed, zero failed,
  one ignored). Strict Clippy, formatting, JSON Schema parsing, strict OpenSpec
  validation, and `git diff --check` pass. The types require a private complete
  Sync/Snapshot structural recovery proof before activation and an exact complete
  request identity or active-state binding for terminal failures. The proof is not
  connection authority. The live state machine recorded below supersedes that stage
  and now also consumes the exact connection-owned registry token. The historical
  count is retained only as prior gate evidence.

- The out-of-band Runtime heartbeat stage passes 28 AAP tests, 729
  `aegisy-agentd` library tests with one ignored live fixture, 7 daemon-main, 10
  context-threshold, 14 handshake Runtime, 18 Schema, 67 protocol, and 23
  stdio/Codex tests: 895 passed, zero failed, and one ignored in the final complete
  workspace run. Strict workspace Clippy, Rust formatting, the complete desktop
  build, all 16 desktop CTests, strict OpenSpec validation, and `git diff --check`
  pass. The Runtime test proves heartbeat remains responsive during a blocked Turn
  and saturated ordinary queue; Qt proves healthy single-flight probes, Unknown
  without disconnect, ordinary-request failure, retained cancellation/terminal-stop
  controls, inert late responses, and fresh-handshake recovery. The Workbench render
  test preserves the active Turn and Stop action while showing Unknown. The Windows
  packaging workflow now builds and runs `agent_runtime_environment` before
  packaging. Initial parallel verification saw three non-reproducing stdio fixture
  timing failures (one missing temporary instance file, one early fixture disconnect,
  and one missing Session field); each focused rerun, the complete stdio/protocol
  reruns, and the final original full gates passed. Bounded reconnect, live
  subscription and bounded reconnect are implemented. Durable Turn-start
  acknowledgement is recorded in the current slice below; complete Windows
  reconnect/runtime evidence remains open and automatic pruning remains disabled.

- The durable Codex file-change Proposal Timeline reference and Qt Changes recovery
  stage passes 27 AAP tests, 728
  `aegisy-agentd` library tests with one ignored live fixture, 6 daemon-main, 10
  context-threshold, 13 handshake Runtime, 17 Schema, 67 protocol, and 23 stdio/Codex
  tests. Strict workspace Clippy, formatting, strict OpenSpec validation,
  `git diff --check`, and all 16 desktop CTests pass. Schema v19 migration/backup,
  immutable Proposal/artifact/event persistence, nested domain-separated identities,
  retry/conflict/rollback/tamper/restart behavior, Runtime/provider/root binding, and
  the real read-only decline lifecycle are covered. The stdio fixture verifies exact
  `codex-app-server` / `codex-cli 0.144.5`, provider/backend thread,
  `read-only` permission, Session/Turn/root/time bindings, all three authority flags
  false, restart read, and unchanged workspace. Public latest/exact Proposal AAP,
  Proposal-bound artifact paging, fixed page identity, `-32149`/`-32150`, semantic
  summary rechecks, and exact legacy `0.1` compatibility are covered. Qt verifies
  strict Proposal semantics, foreground/background behavior, reconnect cache
  invalidation, stale-response isolation, zero-byte and 64 KiB artifact pages, and
  UTF-8 crossing page boundaries while rejecting irreparable invalid UTF-8 page tails
  without exposing Approval or Apply. Windows Qt also rejects drive-prefixed Proposal
  paths to match the Runtime path boundary. Schema v19 additionally covers atomic
  Proposal/Blob/Item/internal-event/public-envelope/reference persistence, exact
  retry and post-commit idempotency, semantic tamper quarantine, v18 migration
  without fabricated history, retention-floor pruning, Session projection rebuild,
  final purge, the outer `session/read.turn_id` binding, and the real stdio decline
  lifecycle. Qt verifies
  the read-only `View changes` reference, stale-generation and response-race
  isolation, and forged/cross-bound binding rejection. The complete desktop suite
  passes all 16 CTests and strict OpenSpec validation passes on 2026-07-26. Genuine
  Approval/Apply/checkpoint authority remains open.

- The Timeline Snapshot recovery stage passes 27 AAP type tests, the full
  `aegisy-agentd --lib` suite (`690` passed and one ignored live fixture), 13
  handshake Runtime tests, 17 Schema tests, the focused Qt Snapshot render run,
  all 16 desktop CTests, strict workspace Clippy, formatting, strict OpenSpec
  validation, and `git diff --check`. Schema v17 materializes canonical visible
  Items and active/open Turn state at the requested floor, validates redundant rows
  and snapshot identity on restart/read, and atomically rolls back with checkpoint,
  floor movement, and prefix deletion. Runtime serves capability-gated fixed-head
  pages, and Qt validates them privately before atomically replacing one Session.
  `snapshot_available` is true only for a connection that negotiated
  `timeline.snapshot.current`. Qt additionally binds a `-32148` response to the
  exact pending sync Session and after/watermark request before signalling recovery;
  malformed or request-drifted gap data closes the protocol instead of starting a
  snapshot. Automatic pruning remains disabled.

- The Public Timeline retention/checkpoint slice advances OpenSpec `3.5` without
  completing it. Schema v16 Journal/cursor CAS, bounded replay restoration,
  negotiated AAP `timeline/sync`, exact sanitized normal Codex transactions, and Qt
  per-Session atomic gap catch-up now operate end to end. Failure coverage includes
  rollback, restart, forged anchors, semantic tampering, oversized pages/events,
  stale prepared tickets, wall-clock rollback, terminal Item boundary enforcement,
  aggregate pending limits, 10,001st-Session fail-closed behavior, initial/history
  sync, pending-deletion read-only sync, and real disconnect signal ordering. The
  complete Rust workspace passes 23 AAP type tests, 675 sidecar tests plus one
  ignored live fixture, 6 daemon-main, 10 threshold, 13 handshake Runtime, 16
  Schema, 66 protocol, and 23 stdio/Codex tests, with strict Clippy and formatting.
  The complete desktop build, all 16 CTests, strict OpenSpec validation, and
  `git diff --check` also pass. Preview's six-event transaction and adapter/
  persistence compensation now share the atomic producer boundary. Failure
  injection proves fourth-Journal-insert, internal Item event, terminal update, and
  compensation Journal failures roll back projection, Trace, internal events,
  cursor, and Journal together. Stdio failure fixtures prove the unique durable
  Error Item exactly matches the replayed failed terminal event. Existing Traces
  reject later Public Journal repair, and production builds expose no Trace-only
  terminal Store helper. The durable retention floor and content-free Sequencer
  checkpoint restore only internal lifecycle authority; schema v17 supplies the
  separate public visible-state snapshot. The strict content-free `-32148`
  retention-gap response and fixed-head snapshot recovery are implemented end to
  end. Automatic pruning remains disabled. Subscription and complete reconnect
  orchestration were absent at that historical stage and are superseded by the live
  subscription stage below; durable Turn-start acknowledgement is recorded in the
  current slice below, while Windows recovery evidence remains absent.

- The AAP initialization-negotiation stage completes OpenSpec `3.3`. Rust and Qt
  implement the exact two-stage `initialize`/`initialized` state machine, numeric
  version ranges and upgrade direction, bounded identities, deterministic stable
  capability intersection, per-method gates, strict JSON-RPC envelopes, fixed 4 MiB
  inbound/outbound frames, and disconnect cleanup. The daemon drains oversized
  physical input without parsing or echoing it, replaces oversized responses with a
  same-ID `-32005`, and closes on oversized notifications. The current stdio channel
  remains explicitly unauthenticated, unencrypted, and not peer-verified; Agent/Codex
  remains read-only. Verification passes 8 AAP type, 630 sidecar library (one live
  fixture ignored), 6 daemon-main, 10 threshold, 13 handshake Runtime, 5 Schema, 63
  protocol, and 22 stdio/Codex tests, strict Clippy, formatting, the focused three Qt
  tests, strict OpenSpec validation, and `git diff --check`.

- The ordered Timeline stage completes OpenSpec `3.4`. Rust, Schema, Qt, fixtures,
  the protocol guide, design, and delta spec share the strict
  `timeline-event/0.1` sequence, timestamp, correlation, Turn terminal, Item
  lifecycle, revision, canonical JSON, and Event ID contract. Qt accepts A/B/A
  Session interleaving without rendering background events into the visible
  Session, and Rust sequencing failure advances no cursor and closes transport when
  a valid terminal cannot be emitted. Qt retains partial streaming Items on structured
  failure, accepts terminal Turns with repeatable `updated` metadata snapshots and
  revision-one atomic `truncated` markers, but still rejects completed/interrupted
  terminals while a `started`/`delta` stream remains open. Mathematical integer forms and exact safe
  boundaries hash identically across Rust and Qt; fractional/out-of-range values,
  65-byte kinds, and frames above 4 MiB fail closed. Verification passes 17 AAP
  type tests, 644 sidecar library tests plus one ignored live fixture, 6 daemon-main,
  10 threshold, 13 handshake Runtime, 12 Schema, 63 protocol, 23 stdio/Codex, all
  16 desktop CTests, strict Clippy, formatting, strict OpenSpec validation, and
  `git diff --check`. The partial fixed-watermark replay/gap slice is now recorded
  above. Live subscription, bounded reconnect, and durable Turn-start acknowledgement
  are now recorded below; Windows evidence remains under `3.5`.

- The Codex degradation/provider hardening stage passes 630 `aegisy-agentd`
  library tests with one ignored live fixture, 63 protocol tests, and 21 stdio/Codex
  tests. Runtime event counters and Qt live cursors are Session-scoped, so a new
  Session starts at sequence one and returning to an earlier Session continues its
  own cursor; Chat/Work mode switching is blocked while the single active Turn is
  running. Duplicate, decreasing, and gapped events remain inert. Rust formatting,
  `git diff --check`, the focused Qt degradation/Timeline run, and the ordinary Qt
  render run pass. Fixed-watermark replay and per-Session gap catch-up are now
  partial `3.5` foundations. Live subscription and bounded reconnect are now
  implemented; durable Turn-start acknowledgement is now implemented, while Windows
  evidence, complete vendor capability negotiation, and remaining runtime-only
  desktop surfaces are still absent, so OpenSpec `3.5` and `7.9` remain unchecked.

- The current Rust workspace passes formatting, full workspace tests, and strict
  workspace Clippy with the exact current counts recorded in the
  `Live Timeline Subscription And Ownership Recovery` section below.
- The previously verified bundled application Node runtime passes both local gateway
  integration suites and JavaScript syntax checks. The current Homebrew OpenSpec CLI
  passes `openspec validate build-aegisy-agent-workbench --strict`; the earlier
  host exit-137 condition did not recur in this run.
- The context-quality, provider-error, usage-authority, context-threshold, and
  turn-trace changes are partial foundations. Their OpenSpec tasks remain
  unchecked until authoritative producers, complete AAP/Qt integration,
  provider/model wiring, and cross-platform release evidence are complete.
- The context-threshold restoration foundation now passes 493 `aegisy-agentd`
  library tests (one ignored live fixture). It restores only a strict,
  content-free review latch from complete persisted usage Items on resume,
  fork, import, and Runtime reconstruction. A complete cold read without usage
  is explicitly `empty`/`no-action`; malformed, incomplete, over-limit, or
  unreadable history is `replayed`/`preview-required` and never silently normal.
  No automatic compaction, provider request, checkpoint activation, or execution
  authority was added.
- The Qt usage-authority and context-threshold widget/render sources pass direct
  C++17 syntax compilation using the CMake compile database. The complete render
  target now passes MOC/RCC/compile/link and its focused threshold-cache mode
  exits successfully. The ordinary render run now also completes successfully on
  this host; cross-platform validation and automatic-compaction authority remain
  absent.
- The focused threshold verification passes 493 library tests, 63 AAP protocol
  tests, 11 stdio/Codex tests, strict workspace Clippy, Rust format checking,
  `git diff --check`, strict OpenSpec validation, two Qt C++17 syntax checks, and
  the focused Qt cache run. This evidence is still not sufficient to mark `17.7`
  complete without cross-platform validation and the remaining compaction authority.
- The durable Turn Trace slice now uses inner `turn-trace/0.6` with strict `0.1`
  through `0.6` version-specific replay compatibility, future `0.7+` rejection,
  and an unchanged outer `turn.trace.recorded/0.1` event.
  Producer/adapter/Store and stdio coverage preserves exact Intent/domain/terminal
  binding and adds the final valid Provider-thread Usage snapshot described above.
  Tests prove completed/failed/interrupted retention, no-Usage behavior, latest-only
  replacement, raw/report/threshold reconstruction, full Session binding,
  earlier-valid substitution rejection, semantic-tamper quarantine, fixed legacy
  identities, idempotent restart equality, and no fabricated Workspace/Git/Test,
  Attempt, or Retry evidence. Store and Runtime now share one exact cross-Turn
  threshold-history reconstruction: Session history starts at `NoAction`, scans all
  Items under the existing 100,000-Item uncertainty boundary, stops at the target
  Turn's final Usage, and moves genuinely malformed authority-less Provider metadata
  to conservative `PreviewRequired` both live and after restart. Tests reject a
  forged first-Session latch and a hash-consistent 80%-90% cross-Turn hysteresis
  downgrade at admission, direct read, replay, and restart. The direct Store path
  counts the complete Item prefix in SQL and validates matching Usage rows lazily,
  retaining only the threshold latch, current-Turn count, and final valid report;
  peak memory therefore does not grow with accumulated Usage payload bytes. Long
  Sessions still repeat this bounded historical scan at terminal admission, and
  projection replay currently rebuilds the same prefix per Trace. A verified-prefix
  cache and single-pass replay remain required before the large-Session performance
  gate. Codex command Tool lifecycle now binds Started plus completed/failed/declined
  status, source, time, closed-projection input identity, output identity, duration,
  exit status, and the exact sanitized persisted terminal Item payload. Full raw input
  drift is checked only through an opaque memory-owned fingerprint; no raw Item ID,
  command/path/output/PID enters the Trace. Exact 72 KiB producer reservation plus
  independent Store admission/read/replay/restart limits are covered. Budget
  exhaustion, SQLite insertion failure, and Provider completion after an unmatched
  Started all durably retain Started + Error + failed Terminal with no terminal Tool,
  command Item, Blob reference, or object. `0.5` and `0.6` record exactly one
  content-free `approvalPolicy=never` Runtime observation with no user decision or
  execution authority and validates its exact durable Adapter/Runtime/Provider-thread
  binding on admission, read, replay, and restart. `0.6` additionally records
  content-free Runtime denials for bound command, file-change, and permissions
  approval requests after the fixed response is successfully written and flushed.
  Their durable identities include request kind; prepare performs every fallible
  identity/order/budget check before the write, and commit occurs only after flush, so
  failed writes leave no denial. `decline-flushed` proves only the local child-pipe
  write, not Provider receipt or action. The Store revalidates these observations on
  admission, read, replay, and restart. No prompt/body/path/command/output/credential
  content is persisted. Runtime denial, Provider `declined`, and
  `approvalPolicy=never` remain distinct semantics, and genuine-user Approval still
  has no producer. The complete `0.6` Rust workspace passes 603 library tests (one
  ignored live fixture), 10 threshold tests, 63 protocol tests, and 19 stdio tests,
  plus formatting and strict Clippy. Strict OpenSpec validation and `git diff --check`
  pass.
  Complete genuine-user Approval/Change/Test production, non-command Tool families,
  per-Attempt/Retry Usage, and
  any AAP/Qt, audit/export, or retention surface remain absent, so `20.1` and `20.2`
  stay unchecked.

## Bounded Reconnect And OOB Update (2026-07-26)

- The bounded reconnect barrier is implemented at the Qt request boundary. Dedicated
  reconnect IDs for `session/read`, terminal `terminal/list`/`terminal/attach`, and
  latest Proposal revalidation are retired on success, malformed response, and
  explicit failure. A failed `session/read` freezes only the affected Session and
  never fabricates recovery; successful reads continue Timeline sync. Terminal
  responses are accepted only when Session, terminal, and generation match. If a
  reconnect cannot verify the new output, the prior output remains visible and is
  marked unverified rather than inferred exited. Proposal responses that are empty,
  drifted, invalid, or failed cannot replace a previously validated cache.
- `AgentRuntimeClient` emits a degradation-request-created signal, and Workbench
  accepts `runtime/degradations` only when the response ID exactly matches the
  request ID recorded for that handshake. Late responses from an older connection
  are inert. This is request correlation, not a capability or authority grant.
- Initialize success and failure paths retire their response IDs before clearing
  pending state, so a duplicate late handshake response cannot become a new
  protocol authority. Heartbeat deadlines bind the exact request ID to both the
  heartbeat generation and current process generation; reconnect stability timers
  bind the same process generation and are cleared on negotiation loss, stop, or
  suppression. Stale deadline/timer callbacks are inert.
- Runtime's independent OOB control reader now queues heartbeat, cancellation,
  steering, and terminal-stop messages received before `initialized`; after the
  main dispatcher consumes the exact handshake notification, it re-runs queued
  messages through the normal OOB router. This closes the handshake race without
  bypassing negotiation or losing controls while the ordinary queue is blocked.
- Verification for this slice: focused Qt render/environment tests 2/2, complete
  desktop CTest 16/16, Rust AAP 28, `aegisy-agentd` library 729 (one ignored live
  fixture), daemon-main 7, context-threshold 10, handshake Runtime 14, handshake
  Schema 18, protocol 67, and stdio/Codex 23 all passed. Strict Clippy, Rust
  formatting, `cmake --build build -j4`, and `git diff --check` passed. Strict
  OpenSpec validation passes after the documentation update.
- OpenSpec `3.5` remains intentionally unchecked. Live subscription and race-free
  subscribe/sync/activate orchestration are implemented by the stage below; durable
  Turn-start acknowledgement is implemented by the following ledger slice, while
  complete Windows reconnect/runtime evidence is still missing. Automatic Timeline
  pruning remains disabled. The detailed plan is in
  `openspec/changes/build-aegisy-agent-workbench/`.

## Durable Turn-start Mutation Acknowledgement (2026-07-26)

- Stable AAP Schema and Rust types register the metadata-only capability
  `session.mutation-acknowledgements`, list method
  `session/mutation-acknowledgements`, and exact-anchor consume method
  `mutation/acknowledgement/consume`. The durable operation identity is derived from
  Session, `turn-start`, idempotency key, and request fingerprint; process generation
  remains only in the transient request acknowledgement. The operation contains no
  prompt, context, provider body, result content, permission, approval, or execution
  authority.
- Workbench schema v20 adds `mutation_acknowledgements` with strict Session,
  operation, revision, Timeline anchor, consumption receipt, and state constraints.
  Reservation commits before Turn dispatch; an equivalent key/fingerprint retry
  returns the same operation/Turn, while a conflicting fingerprint fails without
  dispatch. Accepted `turn.started` and terminal Timeline anchors bind atomically
  with projection and Public Journal writes using revision CAS. Preview reservation,
  binding, rollback, and retry use the same atomic producer boundary.
- Terminal-anchor binding accepts the already validated terminal envelope, including
  the durable structured Error Item required by failed Turns; the ledger stores only
  the sequence/Event-ID anchor and never copies terminal content or authority.
- Store startup converts accepted rows whose dispatch result is uncertain to
  `reconciliation-required` and refuses redispatch. Session-scoped list pagination
  returns only unconsumed rows with a strict keyset cursor. Qt validates the exact
  Session, Turn, sequence, Event-ID, and revision anchor, consumes accepted evidence
  before terminal evidence, and freezes only the affected Session on drift,
  cross-Session access, malformed/tampered rows, unavailable/read-only Store, or
  reconciliation-required state. Session purge removes the ledger atomically; v19 to
  v20 migration preserves existing data without fabricating acknowledgements.
- Focused verification passes 5 durable acknowledgement Store tests and all 23
  stdio/Codex lifecycle fixtures, including provider failures, runtime denials, and
  terminal Error Items. The complete Rust workspace passes 44 AAP tests, 758
  `aegisy-agentd` library tests with one ignored live fixture, 7 daemon-main tests,
  10 context-threshold tests, 21 handshake Runtime tests, 23 handshake Schema tests,
  68 protocol tests, and 23 stdio tests. The complete desktop CTest suite passes
  16/16; strict Clippy, Rust formatting, Schema parsing, strict OpenSpec validation,
  and `git diff --check` also pass. Approval, file-write, Git, and background-job
  mutation producers still have no durable acknowledgement path. OpenSpec `3.5`
  and `3.6` remain unchecked; complete Windows reconnect/runtime evidence is still
  required. Agent/Codex remains read-only.

## AAP Contributor Documentation (2026-07-26)

- OpenSpec `23.6` is complete. `docs/AAP-ADAPTER-CONTRIBUTOR-GUIDE.md` now
  records the schema/repository map, stable versus experimental version policy,
  Rust Runtime/Qt/adapter ownership boundary, redacted deterministic fixture
  workflow, adapter upgrade/rollback rules, local verification commands, and
  macOS/Windows release evidence gates. It links the normative AAP wire guide and
  Codex upgrade procedure and does not claim unavailable Windows evidence.

## Third-Party Component Inventory (2026-07-26)

- OpenSpec `1.4` is complete. `docs/THIRD-PARTY-COMPONENT-INVENTORY.md`
  records repository pins, bundled/external/planned status, licenses, authorities,
  and release obligations for Codex/AAP/ACP, Monaco/xterm, Tree-sitter grammars,
  language servers, terminal/crypto/storage dependencies, the unselected sandbox
  boundary, updater frameworks, Lucide, Qt, and OpenSSL.
- This is engineering inventory, not legal or redistribution approval. OpenSpec
  `1.5` still owns legal review and `22.4` still requires a complete bundle-matched
  NOTICE/license set in signed macOS and Windows packages. A developer-installed
  binary cannot silently replace a repository pin or become package evidence.

## Milestone 0 Performance Budgets (2026-07-26)

- OpenSpec `1.6` is complete. `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md`
  predeclares clean macOS/Windows reference classes, signed Release measurement,
  20-run p95 reporting, standard repository/Timeline/editor/terminal fixtures, and
  absolute installer, startup, memory, editor, terminal, indexing, and crash-
  recovery budgets with non-waivable correctness gates.
- This does not prove the product meets those budgets. OpenSpec `2.7` remains open
  until signed packages are measured on both reference platforms; missing metrics,
  state loss/fabrication, orphan processes, weakened security, unsafe indexing, or
  legacy-client blocking are failures rather than acceptable performance tradeoffs.

## Troubleshooting Runbook (2026-07-26)

- OpenSpec `23.7` is complete. `docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md` gives
  bounded first-response and recovery procedures for sidecar/handshake, Store
  migration/reconciliation, Codex version/crash-loop, streaming decode errors,
  Windows `TLS initialization failed`, terminal/Git/sandbox, and renderer
  failures. It forbids secret/raw-provider logging and explicitly keeps clean
  Windows installer/TLS/ConPTY evidence outside macOS claims.

## Privacy And Diagnostic Export Documentation (2026-07-26)

- OpenSpec `23.8` is complete as a documentation slice. The internal
  `docs/AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md` contract enumerates exact
  `local.*` and `cloud.*` categories, default inclusion states, per-Session/
  Item/range content opt-in, streaming redaction order, preview hash and
  removal controls, deterministic `aegisy-diagnostic-bundle/0.1` packaging,
  retention, and support correlation IDs.
- The default bundle is metadata-only and local-first. Credentials, prompts,
  source/diff/path/terminal content, provider bodies, hidden reasoning, raw
  frames, SQLite/WAL files, and automatic cloud upload remain excluded. The
  document does not claim that an AAP/Qt exporter or cloud support upload exists;
  future implementation must re-read and re-redact sources and fail closed on
  stale preview, source loss, overflow, or uncertain classification.

## Support And Release Recovery Training (2026-07-26)

- OpenSpec `23.9` is complete as an operational documentation stage.
  `docs/AEGISY-SUPPORT-AND-RELEASE-RECOVERY-TRAINING.md` defines support,
  release-owner, and incident-commander responsibilities; repository/history
  preservation rules; bounded evidence; sidecar, Store, Timeline, TLS, streaming,
  renderer, Git, terminal, and sandbox recovery drills; and a data-free sign-off
  checklist. It links the runbook, privacy export contract, and portable Session
  format without treating any of them as mutation authority.
- Completion records the reviewed training package, not human attendance or clean
  Windows runner evidence. Those remain operational release gates and must not be
  inferred from macOS documentation or test results.

## Approval, File-Write, And Artifact Integrity Foundations (2026-07-26)

- OpenSpec `3.6` remains unchecked, but internal
  `approval-acknowledgement/0.1` now binds deterministic Session/Turn/scope,
  request requirement, fingerprint, idempotency, operation, observation, revision,
  and time metadata. Exact request/acknowledgement retries are idempotent; binding
  or same-revision drift conflicts; real transitions are contiguous; terminal and
  reconciliation-required states cannot be advanced by the producer. Only denied,
  expired, not-required, failed, or uncertain observations exist. User-decision,
  Approval, mutation, and execution authority remain fixed false, and secret-shaped
  or `allowed`/`approved` inputs fail closed. Eight focused tests and the complete
  773-test library run (one ignored live fixture), strict Clippy, and formatting pass.
- The Approval contract is not connected to an authority issuer, schema-v20 ledger,
  AAP, Qt, Codex, or genuine user decision. Its complete typed source and exact
  terminal outcome may enter only the internal schema-v23 non-authorizing graph,
  where the Store derives the lossy draft itself; no production Approval path calls
  it. Runtime denial, Provider `declined`, and
  `approvalPolicy=never` remain distinct and must never be projected as Approval.

- OpenSpec `3.6` remains unchecked, but the internal
  `file-write-acknowledgement/0.1` contract now covers a future metadata-only
  file-write producer. Operation identity binds Session/project/root,
  idempotency key, request fingerprint, and Workspace Edit identity. Accepted,
  committed, failed, and reconciliation-required revisions are contiguous and
  monotonic; terminal observations require an opaque hash; uncertain state
  cannot be resolved by the producer; and mutation/execution authority are
  fixed false. Its complete typed source and exact terminal outcome may enter only
  the internal schema-v23 non-authorizing graph, where the Store derives the draft;
  no production producer, AAP/Qt route, consume path, filesystem write, Approval,
  Git, or job path uses it. Five focused tests and the `aegisy-agentd` library
  target (763 passed, one ignored live fixture) plus strict Clippy and format
  checks pass.
- OpenSpec `22.5` has partial Qt, generator, and Rust Runtime foundations. The local
  `aegisy-artifact-manifest/0.1` verifier checks a present sidecar/adapter
  manifest before launch, including fixed identities, versions, bounded reads,
  portable relative paths, canonical in-tree ordinary files, link/reparse and
  extra-hard-link rejection, unknown-field rejection, and streaming SHA-256. A
  failure suppresses launch and automatic reconnect;
  developer builds may omit the manifest. `artifact_manifest_verification` and
  `agent_runtime_environment` pass. The production-path
  `artifact_manifest_runtime_startup` fixture copies the real Release
  `aegisy-agentd` and a fixed `codex-cli 0.144.5` test adapter into a Unicode
  temporary directory, invokes the production manifest generator, sets a bogus
  `AEGISY_CODEX_PATH`, and proves exact manifest-bound AAP initialize, initialized,
  shutdown, and clean process exit. Its initial handshake correctly failed with
  `-32006` until the fixture declared the required `runtime.codex-app-server`
  capability; the corrected local run passes. The CTest timeout is 120 seconds so
  Windows antivirus or cold startup can consume the bounded internal waits without
  turning infrastructure delay into a false product failure.
  Rust Runtime now parses the same exact
  contract before Codex resolution, rejects duplicate/unknown fields, non-portable
  paths, a Windows adapter path without an explicit `.exe`, every symlink/reparse-
  point path component, any extra hard-link alias, version/hash/path/file-identity
  drift, and a manifest identity change within one startup attempt. Artifact hashing
  uses a bounded 64 KiB heap buffer rather than a large main-thread stack frame. A
  present manifest owns adapter selection and cannot fall back to
  `AEGISY_CODEX_PATH`; Runtime revalidates immediately before both `--version` and
  `app-server --stdio`. Sixteen focused Rust tests and the complete Rust workspace
  pass with 1,078 tests and one explicitly ignored live Codex fixture, including
  the final 23/23 stdio/Codex target. Formatting and strict package Clippy pass
  locally. The complete desktop build plus focused Runtime
  environment, manifest verification/generation, and Windows packaging-policy
  CTests pass 5/5 on macOS, including the real daemon startup fixture.
  The complete serial desktop gate passes 26/26; strict OpenSpec validation and
  `git diff --check` also pass.
- A deterministic packaging foundation now exists at
  `cmake/generate_artifact_manifest.cmake`. It accepts only explicit regular
  files inside a caller-provided bundle root, requires a canonical in-root output
  parent, rejects symlink/output/path escape and artifact replacement, records
  relative paths, hashes final bytes with SHA-256, and emits stable
  `aegisy-artifact-manifest/0.1` JSON without timestamps, machine paths, network
  access, execution, or environment discovery. `artifact_manifest_generation`
  runs the generator twice, rejects output drift and paths outside the root, and
  proves that the production Qt verifier accepts the generated file. The focused
  manifest CTest run passes 2/2. macOS and Windows
  packaging do not invoke it yet because neither script currently bundles a
  pinned Codex adapter executable. Packaged builds also lack a non-downgradable
  require-manifest identity, and path verification followed by path-based process
  creation still has a replacement window that must be closed by a reviewed
  file-handle/platform-signature and install-permission boundary. Updater
  compatibility binding, signed release integration, and clean Windows execution
  remain required. The clean Windows Unicode workflow is configured to execute the
  new real-daemon fixture through its unfiltered CTest run, but that source wiring is
  not Windows execution evidence. The macOS-to-Windows Cargo check is still blocked
  before this Rust module by missing Windows SDK C headers in SQLite/Tree-sitter.
  Keep `22.5` unchecked. See `docs/AEGISY-ARTIFACT-MANIFEST-PACKAGING.md`.
- The platform-neutral update-signing foundation now defines a compile-time
  `aegisy-update-signing-trust-anchor/0.1` and bounded
  `aegisy-update-signing-key-ring/0.1`. Generation-one bootstrap must be signed by
  and contain the exact Root. Rotation advances exactly one generation, retains
  all prior Key IDs/public keys, cannot widen prior validity or usage or reverse
  revocation, and requires monotonic Ring signing time. Authority state binds each
  key's first admitted generation/time, so neither Ring nor Artifact Set signatures
  may predate key admission. Because `signed_at_ms` is signer-controlled metadata,
  first-time bootstrap and rotation require the signer to be active both at that
  declared time and at local verification time. An already accepted exact envelope
  remains idempotently replayable after signer expiry, but an offline client cannot
  first admit it after expiry without a future independent witness/checkpoint trust
  path. Explicit revocation remains effective. Generation one keeps its fixed
  authority identity; later authority identities additionally bind complete admission
  history, including when distinct histories converge on the same later Ring envelope.
- Production signed-update compatibility now requires
  `aegisy-update-artifact-set/0.2`. It losslessly parses at most 256 KiB, verifies
  an Ed25519 signature selected through the current Ring, and binds the release,
  channel, signer Key ID, signing/exclusive-expiry times, payload identity,
  `macos/arm64` or `windows/x86_64` target application version/size/SHA-256,
  full-installer URL/name/size/SHA-256/Sparkle signature, target artifact-manifest
  plus exact Runtime/adapter, and one to 64 strictly increasing complete compatible
  source artifact sets including application size/SHA-256.
  Canonical URL policy rejects userinfo, query, fragment, non-443 ports, encoded or
  ambiguous path segments, basename drift, Windows device names, and wrong package
  suffixes. The lossless parser rejects duplicate decoded keys, invalid UTF-8 or
  surrogates, unsafe numbers, excessive depth/nodes, and unknown fields. A valid
  result can set only `candidateCompatible`; download and install authority remain
  false. A current candidate requires its signing key to remain active at evaluation;
  a historical installed receipt requires validity at its signing time but remains
  subject to the latest Ring's revocation and validity cutoff. Production candidate
  evaluation no longer accepts a publicly constructible installed tuple or caller-
  selected verification paths. The only production factory,
  `verifyCurrentInstallationAuthority`, derives the fixed Windows application
  directory or macOS `Contents/MacOS/AegisyClient` layout from the current
  application path. It verifies exact-name adjacent signed receipt, Manifest, Runtime,
  and adapter bytes; rejects link/reparse/multiple-link metadata; and binds the signed
  artifact tuple, trust anchor, Ring generation/identity/authority, and receipt
  signer together with canonical directory identities plus canonical path,
  native file identity, size, and SHA-256 for the application-path target, receipt,
  Manifest, Runtime, and adapter. Every hashed file must preserve one identity across
  the pre-open path observation, actual opened read handle, and post-read path
  observation; Runtime and adapter must be distinct paths and native files. Candidate
  evaluation rederives and revalidates the entire graph. Exact-byte replacement of any
  bound file, content drift, expectation drift, or key drift invalidates cached
  authority. Qt canonical paths use `/` on every platform; a shared explicit
  Windows/Posix containment policy now prevents Windows verification from combining
  those paths with native `\\` separators. Windows comparison is case-insensitive,
  accepts drive and UNC roots, and requires a strict separator-delimited descendant,
  while Posix remains case-sensitive. Platform-neutral string fixtures cover equal,
  sibling-prefix, cross-drive, root, separator, case, and UNC boundaries, and the
  Windows packaging policy requires both verifiers to use the shared helper. This is
  source-level regression evidence, not clean Windows execution evidence. The macOS
  outer bundle basename is intentionally not an authority field.
  The raw-key, legacy `0.1`, scalar-tuple, and arbitrary-root entry points exist only
  under dedicated CTest compile definitions. A separate no-testing-macro Unicode
  child-process target copies the test image into the production-shaped layout and
  proves embedded Root bootstrap, generation-two rotation, historical installed
  receipt verification, rotated-key candidate evaluation, production `0.1`
  rejection, and fixed-false download/install authority. The evaluation identity
  binds candidate/signer, installed tuple and authority, trust-anchor/Ring authority,
  selected channel, caller-supplied release-sequence high-water value, and time.
- `aegisy-update-progress-record/0.1` is an internal single-writer continuity
  foundation. It binds release sequence, artifact-set identity, ordered update phase,
  revision, monotonic time, previous identity, and current identity. Bounded lossless
  parsing, exact uncertain retry, same-sequence conflict, phase/rollback checks,
  single-link files, private Unix permissions with owner-execute, group/other, and
  setuid/setgid/sticky mode rejection,
  atomic replacement, and post-commit reread are covered. Persisted and returned
  download/install/rollback authority is fixed false. This Store is not connected to
  either updater and is not an anti-
  deletion anchor: it depends on a trusted external identity/floor. `QLockFile`
  supplies the local single-writer gate and expected-record-identity comparison is
  performed under that lock. There is no secure-storage anchor or transaction across
  that anchor, the progress record, updater/framework state, and signed package identity;
  deleting both local and external evidence permits a fresh record. The local lock
  fixture passes and `windows_packaging_policy` keeps the focused CTest in the clean-
  runner graph, but no clean Windows or process-crash execution evidence exists.
- This evaluator is not integrated into either platform `UpdateManager` and performs
  no network/download/install operation. The hash and file identity are locally bound
  for the path currently named by `applicationFilePath()`, but that does not prove the
  image loaded when the process started. Artifact Set `0.2` binds the application
  size/SHA-256, but the verifier does not prove macOS code signing/notarization,
  Windows Authenticode, or outer-installer membership; it is therefore not current
  signed-package authority. Key IDs, validity/revocation, sequential rotation, and
  admission history are locally enforced. A local envelope continuity cache now
  retains exact signed Ring bytes and bounded integrity metadata, and reconstructs
  verification by replaying `1..N` from the embedded Root at current time; it is not
  an authenticated publication or secure high-water anchor and supplies no
  cross-restart anti-rollback authority. The release Root defaults empty and there
  is no authenticated Ring fetch. Read handles are
  identity-matched to their before/after path observations,
  but path-derived verification and later process/install actions still have cross-file
  TOCTOU windows and no retained-handle/platform-signature boundary. The progress Store
  does not close anti-deletion or
  cross-resource updater/secure-anchor recovery; its exact
  `{sequence, artifact-set identity, phase, revision}`
  record must be bound to a secure durable anchor and updater transaction before it
  can permit recovery. Sparkle 2.9.4 can veto
  a new candidate through a strongly retained synchronous delegate, but resume skips
  that callback; current generated deltas also lack a signed source-to-target binding.
  WinSparkle 0.9.3 exposes no candidate object or pre-download veto, so Windows needs
  an audited Aegisy-owned flow or reviewed maintained fork. Server-side feed filtering,
  custom fields, and post-download callbacks grant no local compatibility authority.
  The focused CTest passes the macOS/Windows positive cases, exact payload/identity,
  signature/tamper, installed tuple/channel/replay, source count/order, URL/path,
  BOM/UTF-8/surrogate/duplicate/depth/node/raw-size, integer/time, fixed-false
  authority, opaque authority derivation, exact-byte application/receipt/Manifest/
  Runtime/adapter identity replacement, content drift, Runtime/adapter duplicate-file
  rejection, and link/hard-link boundaries. The Key Ring matrix covers denial of
  first-time bootstrap/rotation after signer expiry, exact accepted-envelope replay,
  revocation, validity narrowing, signed-time rollback, admission backdating,
  converged-Ring admission-history separation, lineage, signature, and identity failures;
  `windows_packaging_policy` locks it into the release test graph. OpenSpec `22.5`
  remains unchecked pending persistent/authenticated Ring integration, real updater integration, nested-
  binary-sign-then-manifest-then-outer-seal packaging order, delta/resume handling,
  signed clean Windows/macOS evidence, and the earlier manifest/spawn gates. The
  authority regression matrix also covers an oversized public-key argument before
  string conversion, channel/platform expectation drift, a valid signed receipt
  replacement with a different release sequence, Manifest replacement after authority
  creation, and signed receipt Runtime/adapter version disagreement with the local
  Manifest. An independent read-only security review found no remaining P1/P2 defect
  in dual-time admission, exact-envelope replay, generation-two admission-history
  binding, or production API isolation. The final local gate passes the complete
  application build and all `29/29` serial desktop CTests in 121.72 seconds.
  `nm -gU -C` on both the application and no-testing-macro fixture exposes only the
  Authority-based Artifact Set `0.2` candidate/installation entry points and the
  Key Ring bootstrap/rotation entry points; no testing namespace, raw-key, or
  arbitrary-root helper is exported. Strict OpenSpec validation reports the change
  valid and `git diff --check` passes. After the Windows canonical-containment fix,
  the complete application build, the five focused Manifest/Artifact Set/packaging
  CTests, and all `29/29` serial desktop CTests pass; the serial run completes in
  114.43 seconds. Both production consumers contain no `QDir::separator()` call,
  strict OpenSpec validation again returns valid with exit code zero, and
  `git diff --check` passes. This is macOS source and runtime evidence only and
  grants no packaged-update or Windows authority.

## Live Timeline Subscription And Ownership Recovery (2026-07-26)

- Stable AAP Schema, Rust types, Runtime dispatch, and Qt now negotiate
  `timeline.subscription.fixed-watermark` and register `timeline/subscribe`,
  `timeline/subscription-sync`, `timeline/subscription-snapshot`, and
  `timeline/subscription-activate`, plus subscription-bound live-event and terminal-
  failure notifications. Runtime advertises the capability only with a healthy
  writable Workbench Store; recovery mode does not advertise it.
- Runtime owns one connection-generation registry. Subscription IDs are never reused
  in that generation, each Session has at most one attempt, and registration captures
  one durable floor/head/head timestamp. Every Sync/Snapshot page is bound to that
  attempt. Retained Sync/Snapshot recovery units and events after the fixed head share
  one connection-wide 10,000-unit/64 MiB aggregate bound; completion, activation,
  accepted failure, retirement, and disconnect release their exact ownership once. The first and later
  post-watermark timestamps cannot move before the durable fixed-head timestamp.
  Activation consumes both the complete structural recovery proof and its private
  registry token, returns the exact active result, and then drains buffered events in
  sequence. A cross-Session or cross-generation Sync/Snapshot/Activate request is
  rejected without retiring the real owner; every accepted bound failure retires only
  its bound attempt. Disconnect drops the complete registry, contexts, IDs, recovery
  material, events, and bytes.
- Negotiated Runtime suppresses unbound bare `event` notifications. `turn/start`
  therefore fails with `-32152` unless the Session owns a current subscription
  attempt, preventing a nominally successful request from silently losing Timeline
  output. Qt remains stricter for product UX and enables Send only after the exact
  activation response. It privately stages Sync/Snapshot state, preserves confirmed
  projection and queued input on failure, makes stale generation/request/Session and
  pre-activation traffic inert, and does not replace an Active attempt after ordinary
  `session/read`. A genuine Active sequence gap still starts recovery.
- Retryable typed failures use a fresh subscription ID with bounded Qt backoff of
  0/250/1000 ms and stop after three attempts. A heartbeat deadline while subscribe,
  subscription-sync, subscription-snapshot, or activate is pending creates ambiguous
  server ownership. Because AAP has no unsubscribe shortcut, Qt seals and terminates
  the old connection, retires all pending request IDs, and starts exactly one bounded
  fresh process generation through the existing reconnect barrier. With no pending
  subscription request, Heartbeat Unknown retains the same-process probe behavior.
  Any locally invalid subscribe/Sync/Snapshot/activate result, active wrapper/cursor
  drift, or unsafe continuation also freezes confirmed state, preserves queued input,
  and replaces the Runtime generation instead of completing a false reconnect.
  Occupied `session-attempt-exists` and reused `subscription-id-reused` identities
  likewise require a fresh generation; they are not retried on the connection whose
  ownership cannot be proven. Old-generation traffic remains inert.
- The final complete gates pass: 38 AAP type, 753 `aegisy-agentd` library tests
  with one ignored live fixture, 7 daemon-main, 10 context-threshold, 21 handshake/
  subscription Runtime, 22 handshake Schema, 67 protocol, and 23 stdio/Codex tests
  (941 passed, zero failed, one ignored). Strict workspace Clippy, Rust formatting,
  `cmake --build build -j4`, all 16 desktop CTests, JSON Schema parsing, strict
  OpenSpec validation, and `git diff --check` pass.
- OpenSpec `3.5` remains unchecked. Durable Turn-start acknowledgement is implemented
  by the ledger slice above; approval/file/Git/job producers and complete Windows
  reconnect/runtime evidence remain absent. Automatic Timeline pruning stays
  disabled, and Agent/Codex remains read-only.

## Emergency Workbench Disable Foundation (2026-07-26)

- After login, Qt requests authenticated HTTPS
  `GET /api/v1/client/workbench-policy` every 15 minutes. Redirects, non-HTTPS
  origins, unbounded authentication values, non-JSON responses, and bodies above
  16 KiB fail with fixed content-free codes. The accepted inner contract is the
  exact signed `aegisy-workbench-emergency-policy/0.1` field set: positive sequence,
  issue/expiry time, disable boolean, bounded lowercase reason code, and Ed25519
  signature under the pinned updater key. Session content, paths, prompts, provider
  bodies, credentials, and arbitrary message fields are rejected rather than cached.
- QSettings stores the exact signed envelope plus a sequence/policy-identity
  high-water marker. The marker advances before envelope replacement; missing,
  malformed, mismatched, expired, rolled-back, or conflicting cache state blocks new
  Workbench work. A valid same-identity replay repairs an interrupted envelope, and
  only a valid higher sequence changes policy state. Deleting both QSettings values
  is not prevented by OS-secure storage, so adversarial anti-deletion anchoring remains
  a release gap.
- A disable applies at the Qt request boundary immediately and performs a bounded
  Sidecar generation switch. Emergency startup opens only the local Preview/Store
  backend and never Codex; missing data-root or Store-open failure becomes an
  unavailable emergency backend rather than a Codex fallback. Rust filters advertised
  capabilities and centrally rejects new Session, Turn, workspace mutation, portable
  import, and Runtime restart methods with `-32153`. Reviewed read paths preserve
  local Session history, Timeline recovery, artifacts, workspace/Git inspection,
  and portable Session export where their capabilities exist.
  Login, account/profile management, the legacy gateway, updates, logout, and the
  existing Store-recovery diagnostic export remain outside the Workbench mutation gate.
- Entering emergency mode intentionally retires the normal Sidecar generation even
  when it owns in-memory work. The old process receives bounded graceful shutdown and
  is killed on timeout; no interrupted Turn, terminal, or pending request is inferred
  successful. This preserves Qt/Runtime double enforcement. Retaining same-process
  cleanup while atomically revoking every queued mutation requires a future reviewed
  out-of-band Runtime policy-transition contract and must not be approximated by a
  Qt-only gate.
- Emergency Store recovery retains the emergency flag and central `-32153` mutation
  gate, preserves the recovery handshake marker and diagnostic capabilities, and
  exposes only the existing recovery diagnostics. The request allowlists
  omit retention-policy reads because the current protocol has no separate read-only
  retention capability; advertising an unreachable read or the broader manage
  capability would both be misleading.
- Focused verification passes the signed-policy/cache test, ApiClient HTTPS/auth
  rejection test, dynamic `normal -> emergency -> normal` fake-Sidecar test with an
  immediate pre-reconnect Qt mutation denial, and three Rust emergency tests covering
  no-data-root startup, persisted history/portable export, and corrupt-Store recovery.
  The final combined gate passes 44 AAP tests, 774 Sidecar library tests with one ignored
  live fixture, 7 daemon tests, 10 threshold tests, 21 Runtime tests, 23 Schema tests,
  68 protocol tests, 23 stdio/Codex tests, strict Clippy and formatting, the complete
  desktop build and all 19 CTests, strict OpenSpec validation, and `git diff --check`.
- OpenSpec `22.6` remains unchecked. The production server route/signing publisher,
  signed-package end-to-end macOS/Windows evidence, OS-secure anti-deletion anchor,
  and a healthy-Store diagnostic bundle exporter do not exist. First install with no
  cached policy intentionally keeps current behavior until the production control
  plane is deployed; this absence must not be represented as a verified allow policy.
  Detailed requirements and evidence remain under
  `openspec/changes/build-aegisy-agent-workbench/`.

## Workbench Visible-State Audit Foundation (2026-07-26)

- OpenSpec `23.10` remains unchecked. The maintained
  `docs/AEGISY-WORKBENCH-VISIBLE-STATE-MATRIX.md` inventories the current empty,
  loading, offline, permission, conflict, failure, interrupted, and recovery
  surfaces with stable Qt locators and exact evidence boundaries.
- The focused render fixture now verifies the initial empty Timeline, fail-closed
  capability loading, read-only permission state, a real external-file conflict,
  stable status/failure notice locators with exact text and semantic severity,
  authoritative Turn interruption, and synthetic
  offline/reconnected Qt projections. Synthetic connection signals prove only UI
  projection, not transport or reconnect behavior; protocol fixtures remain the
  authority for those transitions.
- The `AegisyAgentWorkbenchRenderTest` build and `agent_workbench_render` CTest
  pass. Search/history/terminal pending variants, actual Monaco/xterm crash fallback,
  complete secondary-dialog coverage, accessibility/focus/screen-reader behavior,
  Chinese IME, high contrast, supported scaling, and clean Windows evidence remain
  release gaps.

## Product Baseline Decision Gates (2026-07-26)

- OpenSpec `1.3` is complete. `docs/adr/README.md` registers all nine current
  `design.md` Open Questions and links one owned ADR for WebEngine, Codex
  distribution/client identity, ACP extensions, model-catalog trust, local models,
  Windows sandbox, local-content retention, editor language intelligence, and public
  naming. Each ADR has one accountable owner, consulted owners, a closed status, and
  an enforceable repository or dedicated-follow-up OpenSpec gate. Proposed,
  Provisional, and Deferred decisions grant no missing product authority. Model
  catalog closure includes schema/authority/signing/refresh/token/cache/admin gates;
  local providers and enterprise retention each require a dedicated accepted OpenSpec
  before implementation or beta mutation.
- OpenSpec `1.7` is complete as a support-policy definition.
  `docs/AEGISY-SUPPORTED-PLATFORM-MATRIX.md` separates upstream dependency
  capability, repository target, executed evidence, and release support. The current
  local baseline is macOS 26.5.2 arm64, Qt 6.11.1, an arm64 artifact, and deployment
  target 26.0. Windows is an x64/MSVC 2022/Qt 6.8.3 target with Windows 10 1809 as
  the ConPTY technical floor; `installer.iss` now declares `MinVersion=10.0.17763`
  and the application manifest embeds `longPathAware=true`. Clean installer/TLS/
  ConPTY/Git/IME/scaling/accessibility/update and OS long-path evidence remains
  absent. macOS Intel, Windows ARM64, Linux,
  remote, network/cloud/removable filesystems, WSL/Git Bash/MSYS/Cygwin, and other
  unverified combinations are unsupported. The external Git contract has a
  `2.31.0` minimum enforced by `GitRunner::new` with the stable Git-unavailable
  result `-32041`; Windows clean-runner and signed-release evidence remains absent.
- OpenSpec `1.8` is complete as a feature/channel policy definition.
  `docs/AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md` treats internal/preview/beta/
  stable maturity, local versus future remote surface, and emergency revocation as
  independent axes. Effective authority is the fail-closed intersection of signed
  artifact, channel/platform, server/managed policy, opt-in, AAP/model/runtime,
  trust, permission, Approval, sandbox, Store/recovery/liveness, and emergency
  state. Remote remains unavailable without a separate OpenSpec. Emergency `0.1` is
  global and subtractive only; feature-scoped disable requires a new schema.
  Production registry/cohort delivery, policy publisher/endpoint, secure
  anti-deletion anchor, Runtime verification of signed policy identity, Qt/Rust
  allowlist parity automation, and signed macOS/Windows evidence remain under
  `22.6`, `22.9`, and `22.10` and are not implied by policy completion.

## Git Runtime Version Gate (2026-07-26)

- `GitRunner::new` now resolves an absolute Git executable outside the canonical
  project root and performs a bounded 256-byte `git --version` preflight with a
  five-second wait deadline before
  any product Git status, query, staging, branch, commit, worktree, or workflow
  operation. The parser accepts the documented plain Git, Apple Git, and Git for
  Windows forms and rejects malformed, failed, oversized, or below-`2.31.0`
  versions with the stable Git-unavailable code `-32041`; timeout and read-error
  paths kill and reap the child before returning.
- `.gitignore` evaluation deliberately uses a private compatibility constructor
  without the product version gate. This keeps ignored files excluded when a
  legacy Git is installed and never grants that Git product-operation authority.
  All other callers use the gated constructor; the focused `git_status::tests`
  suite passes 9/9, including real executable fixtures for below-minimum,
  non-zero exit, malformed, oversized, and timeout cases in addition to the
  minimum-version boundary and current-install preflight. Version-check failure,
  timeout, and bounded-read paths share one kill-and-wait cleanup helper so a
  failed preflight cannot leave an owned Git child behind.
- This is Runtime implementation evidence only. Windows clean-runner Git,
  signed-package, long-path, and full cross-platform release evidence remain
  open, and the OpenSpec baseline is 55/235 completed tasks.

## Windows Packaging Policy Foundation (2026-07-29)

- `installer.iss` now declares `MinVersion=10.0.17763`, matching the Windows 10
  1809 ConPTY technical floor. The CMake Windows resource script embeds a
  requested `asInvoker` execution level and the generated application manifest
  sets `longPathAware=true`.
- The cross-platform `windows_packaging_policy` CTest checks the installer,
  manifest template, resource script, complete Windows Qt module selection, and
  fail-closed explicit Qt 6 SDK discovery, and passes alongside the desktop build.
  This proves source policy wiring only; clean Windows OS long-path policy,
  installer/TLS behavior, signed package, upgrade, and runtime evidence remain
  open. Do not advertise Windows release support from this static gate.
- Windows validation run `#204` at commit `1121e52` passed the Rust Runtime gate
  but failed during Qt configure because the selected Qt 6.8.3 SDK lacked required
  add-on components. Quiet Qt 6 discovery hid that component failure and produced a
  misleading Qt 5 fallback diagnostic. The workflow must install the four real
  add-on modules `qtpositioning`, `qtwebchannel`, `qtwebengine`, and
  `qtwebsockets`; `qtdeclarative` is not a separate online-installer module
  for Qt 6.8.3 `win64_msvc2022_64` because it ships inside the base
  `qt.qt6.683.win64_msvc2022_64` package, and listing it makes `aqtinstall`
  fail with "packages ['qtdeclarative'] were not found". Windows validation
  run `30867435889` at commit `2256187` (2026-08-04) confirmed exactly that
  failure: the Rust gate passed and `Install Qt` stopped the job, so the
  modules list and the packaging-policy test now require the four add-ons and
  reject an explicit `qtdeclarative` entry. Both Windows validation and
  installer construction set the
  explicit `AEGISY_REQUIRE_QT6=ON` release gate and pass `Qt6_DIR`; CMake then
  requires Core, Widgets, Network, SQL, WebSockets, WebChannel, and WebEngineWidgets
  in one `find_package(Qt6 REQUIRED ...)` call with the original Qt diagnostic.
  Developer Qt 5/native-editor fallback remains available only when that release
  gate is off, including across repeated configure runs. The Windows job executes
  the policy test itself. Local policy/environment tests pass after this correction,
  but a fresh clean Windows runner is still required before the CI or release gate
  is considered verified.
- The validation checkout is now placed directly under the Unicode relative path
  `windows-验证-源码`; a pre-build PowerShell gate rejects an ASCII path or a
  dirty Git state. The validation job builds the complete Release target graph and
  runs the unfiltered CTest suite plus locked offline `aegisy-aap` package
  verification, so the generated AAP, Qt consumer, desktop, packaging-policy, and
  Windows named-pipe tests share the same Unicode checkout.
  Transport Runtime and Artifact Manifest test executables now consume
  `QCoreApplication::arguments()` rather than the local 8-bit encoding, and their
  local fixtures force Chinese schema/manifest paths.
  `tests/windows_packaging_policy_test.cmake` prevents narrowing this back to the
  earlier four-test subset: it requires the complete 18-entry trigger set, exactly
  one unfiltered CTest command, and the installer artifact path rooted at
  `${{ github.workspace }}/windows-验证-源码`. The test also mutates each of those
  three policies in memory and proves that a missing Runtime trigger, appended
  CTest `-R` filter, or relative artifact path is rejected. The workflow is fixed
  to LF in `.gitattributes`, while the validator additionally proves that a CRLF
  copy has identical policy results. The hardened script passes directly and
  through its focused CTest. The complete local desktop build
  and all `25/25` serial CTests from the preceding workflow slice pass, including
  the three focused Unicode-path gates. Locked offline Cargo package verification,
  workflow YAML parsing, strict OpenSpec validation, and `git diff --check` also
  pass. The new workflow has not yet run on Windows, so OpenSpec `3.10`, `4.3`,
  and all release/runtime claims remain open.

## Chat Streaming Boundary (2026-07-26)

- `ApiClient` now keeps one shared SSE parser for incremental reads and the
  `finished` callback. A final `data:` record without a trailing newline is
  consumed before completion, and an SSE response is successful only after a
  parsed `[DONE]` marker. Malformed or truncated event streams fail closed with
  the fixed `stream disconnected before completion` message rather than
  treating a partial answer as complete.
- Non-SSE JSON responses retain their complete body for the existing fallback
  decoder; the SSE parser is not allowed to consume or erase that body. This
  boundary is covered by the existing account/API client CTest and the desktop
  build. Provider/network behavior still requires redacted live reproduction
  when investigating a remaining transport error.

## Responsive Workbench Pane Navigation (2026-07-26)

- The Workbench now switches to a compact single-pane layout below 900px. A
  compact toolbar exposes Chat/Project/Workspace pane buttons and only the
  selected surface remains visible; returning above the threshold restores all
  three panes and the persisted splitter proportions.
- The top toolbar uses an ignored horizontal size policy so its desktop-only
  status/model controls cannot impose a wider minimum size than the compact
  breakpoint. In compact mode those non-essential controls are hidden while
  session/project content remains unchanged.
- Compact pane selection is UI-only and is not written to Workbench durable
  data or session content. The 900px threshold and pane visibility are covered
  by the focused render test; accessibility, narrow-platform screenshots, and
  cross-platform sizing evidence remain release gaps.

## Rust Production Transport Ingress (2026-07-28)

- Rust production stdio ingress now carries a generated `TransportMessage` from the
  single lossless parse through queue admission and ordinary/OOB dispatch. The
  generated generic envelope validator runs before queue classification; method kind
  and known definition validators run at the reviewed policy boundary so the stable
  handshake, capability, notification, request-ID, and overload error order does not
  change.
- Queue-full generic-valid frames keep `-32004` precedence even when a known method
  later has invalid params or the request/notification kind is wrong. Known malformed
  messages cannot fall back to generic handling once admitted for dispatch.
- Generic and per-definition validator compilation failures are deterministically
  injectable in tests. `ValidatorUnavailable` produces no peer error, claims no ID,
  changes no Runtime/Store state, and closes future queued dispatch. One mutex-backed
  `TransportFaultGate` removes the previous check-then-dispatch race.
- Verification passes all `1018` Rust workspace tests with one ignored live Codex
  fixture, strict workspace Clippy and formatting, generator negative/freshness and
  exact 47-file package inventory gates, `aegisy-aap` Cargo packaging, the desktop
  build and `23/23` CTests, strict OpenSpec validation, and `git diff --check`.
  The render fixture's synthetic recovery Session now pre-stages its exact sync
  request instead of racing a real sidecar response; production behavior is unchanged.
- Qt production ingress now retains one generated lossless `TransportMessage` from
  `processStdout()` through generated request/notification/response dispatch,
  indivisible pending correlation, and safe Qt projection. Outgoing JSON remains
  serialized through the existing bounded Qt writer. A clean Windows Unicode
  checkout and execution gate remain required, so OpenSpec `3.10` stays unchecked.

## Generated Qt/C++ Transport Dispatch Foundation (2026-07-28)

- Generated C++ now exposes parsed-message request/notification and response
  dispatch overloads. Raw convenience APIs perform one lossless parse and delegate
  to those overloads, allowing the production Qt consumer to retain one authoritative
  `TransportMessage` through envelope, method, typed-error, and pending correlation
  validation.
- Wrong or null response IDs never match or retire a pending request. A known typed
  error with an unmatched ID is validated against a private DOM clone whose ID is
  replaced only for Schema validation; a valid payload remains `Unmatched`, while a
  malformed known payload remains `InvalidKnownMessage` and cannot fall back to a
  generic error.
- CMake builds generated C++ plus the standalone Schema runtime exactly once in the
  warnings-denied `AegisyAapTransport` static library. The desktop application and
  relevant Qt tests link that production implementation instead of compiling private
  copies.
- Focused generated/runtime tests cover parsed dispatch, wrong/null IDs, known typed
  payload validation, arbitrary-precision generic values, metadata inventory, and
  local validator failure. The production `AgentRuntimeClient` consumes the same
  generated dispatch and uses one pending context map plus safe Qt projection;
  clean Windows Unicode-checkout evidence remains absent. Keep OpenSpec `3.10`
  unchecked.
- The final local gate passes generator freshness/negative/package-inventory checks,
  Rust formatting, all 59 `aegisy-aap` tests, package Clippy with warnings denied,
  Cargo packaging, the complete desktop build, all `23/23` CTests (including the
  complete Rust workspace test), strict OpenSpec validation, and `git diff --check`.

## Qt Canonical Error-Code Boundary (2026-07-28)

- `AgentRuntimeClient::requestFailedExact` is the lossless production failure
  signal. It carries the canonical JSON-RPC mathematical-integer lexeme as a
  `QString`, including values outside Qt `int`/JSON-safe numeric ranges. The
  Workbench consumes this signal for all request-failure state and notice paths.
- The legacy `requestFailed(..., int)` signal remains only as a compatibility
  signal and is emitted when the canonical code is exactly representable as an
  `int`. An out-of-range wire code never becomes `0`, `-1`, or another sentinel;
  it is not emitted through the legacy signal. Local validation, handshake,
  write, and pending-failure paths use the same exact-first reporting boundary.
- Focused environment coverage proves a huge code remains canonical and does
  not disconnect or trigger the compatibility signal. Workbench render coverage
  exercises canonical failure handling and redacted provider/restart notices.

## Metadata-Only Mutation And Server-Request Contracts (2026-07-28)

- OpenSpec `3.6` remains unchecked. `agent-runtime/crates/aegisy-agentd/src/git_mutation_ack.rs`
  now defines internal `git-mutation-acknowledgement/0.1` metadata semantics for
  future Git producers. Session/project/root, mutation kind, idempotency key,
  request fingerprint, and immutable plan identity derive a domain-separated
  operation identity. Exact retries are replayable, same-key drift is a conflict,
  and unrelated keys remain distinct. Accepted/Committed/Failed/
  ReconciliationRequired revisions are contiguous and time-monotonic; uncertain
  and terminal states cannot be advanced by the producer, terminal observations
  require opaque evidence, and mutation/approval/execution authority are fixed
  false. Its complete typed source and exact terminal outcome may enter the internal
  v23 Store graph, but it is not connected to production Git, AAP, Qt, or an
  approval issuer.
- `mutation_reservation.rs` defines the internal complete typed-source union and
  existing `mutation-reservation-draft/0.1` bridge for approval, file-write,
  Git-mutation, and background-job metadata. It preserves canonical complete-source
  bytes, hashes, and identities, derives the lossy draft internally, and classifies
  exact replay versus hidden source drift. The inner draft's self-reported
  `reservation_persisted` flag, schema-v20/Turn-anchor compatibility, dispatch,
  mutation, Approval, and execution authority remain fixed false. Seven contract
  tests cover all four sources, canonical round-trip and byte bounds, source-to-draft
  derivation, hidden drift, strict nested fields, secret shapes, and forged authority.
- Historical Workbench schema v22 persisted one complete typed source, its derived
  draft, and a metadata-only internal Session event as the separate non-Turn
  reservation graph rather than inserting it into the schema-v20 `turn-start`
  ledger. Redundant
  source/scope/key/fingerprint columns, canonical source/draft JSON and hashes,
  provenance, exact lifecycle, Session ownership, and required project/root or Turn
  scope are revalidated on read and startup. Exact complete-source retries are
  no-write even when new writes fail low-space admission or another connection owns
  the write lock. Hidden complete-source drift conflicts even when the lossy draft is
  unchanged. For an absent key, the `IMMEDIATE` transaction rechecks deletion,
  Session status, scope, and idempotency before insertion. Startup performs candidate
  capture, validation, reconciliation, and its final no-open-row assertion inside one
  `IMMEDIATE` transaction. Global rows and sources are capped at 10,000 and each JSON
  body at 16 KiB. The canonical inventory covered both reservation tables, all named
  and auto-indexes, and rejects Triggers on the shared `events` or
  `session_sequences` write paths.
- The historical v21-to-v22 migration first validates the exact v21 schema, every
  canonical row, scope, lifecycle, time, hash, and the 10,000-row bound. Valid rows become
  `legacy-unavailable` plus `reconciliation-required` revision 2 without fabricated
  sources or events. Every pre-v22 branch rejects reserved lifecycle event kinds,
  operation-ID and Event-ID namespaces and shared event-path Triggers. `user_version`
  is rechecked under the migration lock. The current v23 migration preserves these
  invariants and also accepts an already completed v23 migration under the lock.
  Focused coverage locks the lifecycle lookup to the covering
  `events(session_id, operation_id, sequence, event_kind)` index without `ANALYZE`.
- Schema v23 now adds the internal kind-specific terminal outcome row/event and
  reservation CAS described in the current terminal-outcome section. There is still
  no consume/external-caller-CAS route, production caller, AAP/Qt surface, recovery
  consumer, or dispatch authority. Production lifecycle and reconciliation policy
  must still be reviewed before a producer is connected.
- OpenSpec `3.7` remains unchecked. `credential_refresh.rs` defines an internal
  `credential-refresh-request/0.1` contract that carries only provider/profile
  and one-way secure-storage identities. It never carries credential values,
  tokens, network or refresh authority, or secure-storage API access. Four focused
  tests cover identity/retry, strict metadata round-trip, lifecycle/reconciliation,
  and secret/unknown/authority rejection.
- `extension_elicitation.rs` defines internal `extension-elicitation/0.1`
  metadata semantics. It binds Session/Turn/request/extension identities and
  idempotency but excludes prompts, forms, URLs, arguments, and answers. Requested,
  non-decision terminal, failed, and reconciliation-required states are strict;
  five focused tests cover exact replay, lifecycle, drift, secret, unknown-field,
  and fixed-false decision/permission/execution/mutation authority checks. It is
  not a server-request UI, approval path, AAP method, or extension execution grant.
- `structured_user_input.rs` defines internal metadata-only
  `structured-user-input/0.1`. It binds at most 16 ordered opaque questions, 16
  options per question, and 128 options total to exact Session/Turn/request and
  idempotency identities. Cancellation stores its bounded idempotency key and
  recomputes the operation-bound identity; cancellation/completion races,
  contiguous revisions, monotonic time, terminal and reconciliation behavior are
  strict. Prompt, label, option value, answer, and form content are absent, and
  decision/permission/execution/mutation authority are fixed false. Five focused
  tests cover bounds, drift, strict wire input, cancellation binding, races, and
  fail-closed authority/uncertainty behavior. It is not a usable question UI or
  answer channel.
- Schema v21 was the historical non-authorizing draft-only wrapper; schema v22 added
  the complete typed source, and schema v23 adds the internal terminal-outcome graph.
  No production approval/file/Git/job path calls either foundation. The server-request
  modules remain disconnected from AAP, Qt, Workbench Store, Codex server requests,
  secure storage, Git execution, and genuine user Approval. Do not advertise these
  foundations as usable mutation, structured-input, credential refresh, or
  elicitation functionality.

## Content Reference Foundation (2026-07-28)

- OpenSpec `3.8` remains unchecked. `agent-runtime/crates/aegisy-agentd/src/content_reference.rs`
  defines strict metadata-only `content-reference/0.1`, `content-preview/0.1`,
  `content-inline-limits/0.1`, `content-reference-cursor/0.1`, and
  `content-reference-page/0.1` contracts. References bind reviewed artifact/blob,
  command, diagnostic, image, and workspace-edit domains to lowercase SHA-256,
  bounded byte counts, and an allowlisted MIME type. Preview metadata binds the
  exact reference and records only bounded truncation, text line count, or image
  dimensions. Four focused tests cover identity/MIME/hash bounds, preview binding,
  negotiated limit intersection, cursor/page identities, unknown fields, and
  secret-shaped inline rejection.
- Inline budgets intersect by minimum and cannot widen either side. Byte-window
  pages carry exact continuation cursors until the terminal page; page identities
  bind the reference, window, limits, inline digest, and continuation. Binary and
  image pages remain reference-only, while text previews are bounded and redacted
  at the contract boundary.
- This foundation does not read/write Blob or filesystem content, add an AAP route,
  grant artifact authority, or replace existing Store transaction and artifact-page
  gates. Do not advertise `3.8` as complete until producers, persistence, generated
  types, and Qt/AAP evidence are connected.
- `artifact/read-command-output-page` is the first production connection to this
  contract. It reuses `artifact.command-output.bounded`; first and continuation
  requests require the exact Session/Item/reference. In-memory lookup is keyed by
  `(item_id, reference)` and durable lookup by Session/Item/reference, so identical
  output from multiple Items remains independently bound after Runtime restart. It returns
  `command-output-artifact-page/0.1` with the strict reference/preview plus one
  UTF-8-safe inline page. First-page limits intersect caller/local item and aggregate
  ceilings; the aggregate limit applies to all inline items in one response, the
  single page is capped by both limits, and continuations cannot renegotiate. Cursor
  binding includes Session, Item, and complete immutable Artifact metadata, and the
  response exposes bounded `created_at_ms` for independent client recomputation. Durable
  reference identity, owner/MIME/exact metadata/redaction/truncation/content integrity,
  and the unique omission marker at its canonical head/tail boundary are independently
  revalidated. The complete Artifact is hash/length/secret-scanned before slicing, so
  `[REDACTED]` placeholders may cross pages while unredacted secret shapes cannot evade
  detection across pages. Hash-consistent owner rebinding, cross-Session or cross-Item
  use, purged/missing/corrupt content, unknown params, limit drift, cursor substitution,
  and scalar-splitting pages fail closed. The legacy whole-artifact route remains for
  compatibility callers and uses a deterministic creation-time/Item-ID owner selection.
- The command page binding hashes the UTF-8 bytes
  `command-output-artifact-page-binding\0` followed by the exact
  `schema_version`, Session, reference, SHA-256, MIME, Item, creation/source/redaction,
  and truncation fields. Every field has an unsigned 64-bit big-endian length;
  strings use exact UTF-8, integers use eight unsigned big-endian bytes, and booleans
  use one `0`/`1` byte. This avoids JSON serializer escape drift for every legal
  ASCII-graphical Item ID. The fixed simple and quote/slash/backslash results are
  `content-reference-binding:sha256:d6a8d1c07d2e2a8d0817b50a191e02a0ac677eddacd17bceaeb6c5f67f24216f`
  and
  `content-reference-binding:sha256:a0b5b2ca29247112b19796e03fafed08972eef39e3f4cd8bb769a7906f8b78a3`.
  Generic limit/cursor/page identities use prefix plus NUL and unsigned 64-bit
  big-endian length framing. A missing binding contributes no component, preserving
  legacy `0.1`; fixed Rust vectors lock the unbound limit/cursor/page identities for
  Qt reproduction. Generic page deserialization still rejects secret-shaped inline
  text, while only the producer path that already scanned the full source may admit a
  page fragment splitting `[REDACTED]`. Source text identical to the generated omission
  marker is escaped with a byte-length-preserving substitution before the single
  canonical marker is inserted; the retained source marker bytes are intentionally
  changed and cannot be reconstructed from the Artifact.
  Durable Item IDs use the AAP 1-128-byte ASCII-graphical boundary. Existing
  alphanumeric/`-`/`_` IDs retain their legacy internal event operation ID; a legal
  Item ID outside that narrower internal alphabet receives a deterministic
  `item-<sha256>` operation ID while the exact public Item ID remains unchanged in
  correlation and payload fields. This avoids rewriting legacy events while keeping
  restart and portable-session paths compatible with the public Item contract.
- Focused command-artifact tests plus explicit redaction and owner-rebinding tests cover
  compatible legacy reads, empty and UTF-8 pages, terminal cursors, continuation
  renegotiation/tamper, Session and Item isolation, durable semantic tampering, and
  Store reload after Runtime restart. This grants no generic Blob/filesystem,
  mutation, Approval, or execution authority. OpenSpec `3.8` remains unchecked until
  the remaining content domains, generated types, and cross-platform evidence are
  complete.
- Qt now consumes `artifact/read-command-output-page` in production and independently
  reproduces the command binding plus preview, inline-limit, cursor, and page
  identities. It requires exact fields and JSON-safe integers, binds every response
  to the requested Session/wire Item/reference, rejects limit renegotiation and
  metadata/window drift, and closes the protocol on an invalid known response. At
  terminal assembly it also requires exactly one omission marker at the fixed
  UTF-8-safe head/tail boundary whenever truncation is declared. Fixed
  Rust/Qt vectors cover the simple and quote/slash/backslash Item bindings plus the
  legacy unbound limit/cursor/page identities. The compatible whole-artifact client
  method remains available but has no Workbench product caller.
- The Workbench command Artifact dialog is one generation-bound workflow. It carries
  the exact wire Item ID rather than the presentation key, accumulates raw UTF-8 bytes
  without inserting line breaks, freezes immutable metadata across continuation
  pages, and keeps Pin disabled until the terminal byte count and complete SHA-256
  match. Load More is available only for a validated partial page. Dialog close,
  Session/project/mode changes, Runtime loss or generation replacement, Timeline
  Snapshot replacement, emergency disable, and a replacement workflow invalidate all
  owned requests; unowned or late responses are inert. The preview remains read-only
  and explicit Pin never automatically sends content to the model.
- The final backend command-output paging gate passes `1050` Rust tests with one
  explicitly ignored live Codex fixture, strict workspace Clippy and formatting.
  Three earlier complete runs exposed only the large PTY correctness fixture's
  five-second exit wait under workspace-test contention; that fixture passed alone
  each time and now uses a test-only 20-second correctness deadline. Product terminal
  behavior and the separately declared performance budgets did not change. The final
  complete run passes after the length-framed binding and equal-length secret fixture.
  Strict OpenSpec and diff gates pass before the backend commit. A clean isolated
  desktop configure and complete build pass. The first aggregate CTest run passed
  21 targets but its timer incorrectly expired `agent_runtime_protocol` and
  `aap_generated_types`; exact isolated reruns passed in 67.32s and 8.55s, so every
  one of the 23 desktop targets has successful execution evidence. This is recorded
  as 21 aggregate plus two exact reruns, not as one uninterrupted aggregate success.
  The final Qt paging slice adds fixed identity and request-shape tests, malformed
  response protocol closure, a correlated valid-response signal, empty-page and
  canonical truncation-marker validation, and render coverage for legal
  punctuation Item IDs, a UTF-8 scalar split at 64 KiB, Partial/Load More, terminal
  Pin, cross-Session rejection, and invalidated late responses. The complete desktop
  build passes. The latest serial aggregate CTest run passes 22 targets while
  `agent_workbench_render` misses its language-server stop lifecycle state; its
  exact rerun passes in 7.74 seconds, so record 22 aggregate plus one exact rerun,
  not 23/23. The full Rust workspace test passes in 276.66 seconds; strict Clippy and
  formatting, generator freshness/negative and exact 47-file inventory,
  `aegisy-aap` packaging, strict OpenSpec, and `git diff --check` pass. This is
  macOS/local evidence only and cannot close remaining content domains, generated
  protocol types, clean Windows, or live-provider gates. Keep OpenSpec `3.8`
  unchecked.

## Bootstrap Authentication Foundation (2026-08-03)

- OpenSpec `4.4` is implemented and macOS-verified. The Windows named-pipe
  bootstrap subtest passed in predecessor run `31426799633`, but the task stays
  open until the current complete clean Windows workflow is green. The design keeps
  authentication outside AAP messages: a one-time
  `aegisy-bootstrap-auth/0.1` transport-line prelude precedes any AAP frame, so
  no schema field carries the secret.
- Token lifecycle: Qt generates 32 random bytes per process generation
  (`QRandomGenerator::system`), base64url-encodes them without padding (43
  chars), strips any inherited `AEGISY_BOOTSTRAP_TOKEN` during environment
  sanitization (its TOKEN segment is always classified sensitive), inserts the
  fresh token, writes the prelude as the first stdio line after start or the
  first socket line after peer verification, then erases its copy. The sidecar
  reads and immediately `remove_var`s the variable in `main` so Codex,
  terminal, and Git subprocesses never inherit it; the new
  `bootstrap_auth.rs` verifies the exact 93-byte prelude byte-for-byte with a
  constant-time comparison, consumes and zeroes the token, and
  `Runtime::mark_bootstrap_authenticated` flips the reported
  `transport_security.authenticated` fact. Missing, malformed, mismatched, or
  replayed preludes receive the fixed content-free `-32154` error and the
  connection closes before any Runtime/Store/adapter state exists. A malformed
  environment token aborts startup without an AAP response.
- The AAP 0.1 schema changed exactly one way: `authenticated` is now
  `type: boolean` for all three local transport-security definitions instead of
  `const: false`; regenerated Rust/C++/TypeScript bindings pass the inventory
  check. Honesty is preserved by the exact client-declared versus
  runtime-reported fact comparison on both peers, not by schema constants.
  A sidecar without the token keeps legacy `authenticated: false` behavior, so
  existing fixtures are unchanged.
- Evidence: new `bootstrap_auth_stdio.rs` E2E fixtures (exact accept, missing,
  wrong/replayed, malformed shapes, post-authentication replay as ordinary
  invalid input, malformed environment token, legacy mode), module unit tests,
  adapted handshake/schema suites, and the Qt environment/macOS-socket suites
  with fake runtimes that now require and match the environment token. Local
  gate: strict Clippy, fmt, Rust workspace tests, full CMake build, all CTest,
  and `openspec validate --strict` pass. The single
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status` PTY
  failure reproduces identically on the base commit in this environment and is
  unrelated to this change; investigate it separately before the next release
  gate.
- The token never enters process arguments, logs, diagnostics, persistence, or
  crash reports; neither peer derives Debug/logging coverage for it.

## Windows Compile Gate Repair (2026-08-04)

- Windows validation run `30867435889` at commit `2256187` failed in
  `Install Qt`: `aqtinstall` reported `packages ['qtdeclarative'] were not
  found`. Qt 6.8.3 `win64_msvc2022_64` ships `qtdeclarative` inside the base
  `qt.qt6.683.win64_msvc2022_64` package (verified against the official
  Updates.xml), so it must not appear in the module list. The workflow now
  installs only the four real add-on modules and the packaging-policy test
  rejects an explicit `qtdeclarative` entry.
- Run `30873098248` at commit `d5d53c0` then revealed two deeper issues.
  First, the `Verify Windows agent runtime` step used plain PowerShell
  semantics, so a failed `cargo test`/`clippy` did not stop the step and the
  final `cargo deny check` exit code made the job green. Every pre-fix
  "Windows Rust gate passed" claim was therefore not compile evidence; the
  step now sets `$ErrorActionPreference='Stop'` and
  `$PSNativeCommandUseErrorActionPreference=$true`.
  Second, the first honest Windows compile failed: the Windows-only
  `filesystem_root_identity` used the unstable `windows_by_handle`
  `MetadataExt::volume_serial_number`/`file_index` APIs (E0658/E0277), which
  no macOS build or the blocked macOS-to-MSVC check could catch. The Windows
  path now opens the directory with `CreateFileW` + `FILE_FLAG_BACKUP_SEMANTICS`
  and reads `BY_HANDLE_FILE_INFORMATION` through the already-pinned
  `windows-sys`, matching the stable pattern in `artifact_manifest.rs`.
  Twenty Windows-only warnings (unused Unix-stub parameters and cfg-dependent
  `mut` bindings) were fixed in the same pass so the strict
  `-D warnings` Clippy gate can run for real.
- OpenSpec `3.10`, `4.3`, `4.4`, `14.2`, and `14.9` remain unchecked until a
  clean `windows-2022` run with the fail-closed gate completes the full Rust
  suite, the complete Release build, and the unfiltered CTest suite from the
  Unicode checkout.

- Run `30882080221` at commit `bc15b06` produced the first honest Windows test
  report: the suite compiled and ran, with 30+ Windows-only failures grouped
  into seven root causes. (1) The ConPTY interactive fixture hung because
  `ClosePseudoConsole` (MasterPty drop) blocks while the reader thread is in a
  synchronous `ReadFile` on the output pipe; production teardown now duplicates
  the reader thread handle, calls `CancelSynchronousIo`, and joins with a
  bounded wait before the master is dropped. (2) `File::open(...).sync_all()`
  fails on Windows because `FlushFileBuffers` requires write access; the
  migration backup sync now opens read+write. (3) `Path::canonicalize` returns
  `\\?\` verbatim paths on Windows; a shared `plain_path` helper strips the
  prefix for Git arguments, file URIs, descriptors, and path comparisons, and
  root-revalidation guards compare plain forms. (4) In-memory Git workflow
  record fixtures used `/tmp/...` roots that fail `is_absolute` on Windows;
  fixtures now use platform-appropriate absolute roots. (5) Tests deleting
  temp directories while a Runtime still holds store files open fail with
  sharing violations; they now drop the Runtime first. (6) Extension-less Git
  hooks with a shebang are executable content under Git for Windows and are
  now reported by the trust review. (7) The managed-policy command assertion
  now uses a command that genuinely matches the `* test *` deny pattern on
  every platform instead of depending on the macOS `/var` tempdir symlink.
  Windows ConPTY interactive evidence, the full Rust gate, and the complete
  desktop CTest suite remain pending the next clean run.

- Follow-up Windows slice: the first plain-path pass broke form-sensitive
  comparisons, which now compare plain path forms on every platform; the LSP
  URI layer normalizes verbatim prefixes and drive-letter case. ConPTY
  teardown no longer cancels the reader before closing: pre-24H2
  `ClosePseudoConsole` waits for the console host, which requires the output
  pipe to keep draining, so teardown terminates the job, gives the reader a
  bounded end-of-file window, drops the master on a helper thread with a
  bounded wait (abandoning rather than hanging on a wedge), and only then
  cancels and joins the reader.

- Run `30902802409` at commit `e4cfdfe` passed the complete Windows Rust
  gate for the first time: `cargo fmt --check`, the full locked workspace
  test suite (833 library tests plus every integration target, including the
  ConPTY interactive fixtures, real Git transactions, clangd LSP, and the
  bootstrap-authentication E2E), strict all-target Clippy with `-D warnings`,
  the locked Release build, offline `aegisy-aap` packaging, and `cargo deny`.
  That run also showed that pre-fix "Windows Rust gate passed" claims are now
  superseded by genuine fail-closed evidence. The Qt gate then failed on MSVC
  C2026: the generated core/transport C++ embedded each schema as one raw
  string literal above the 16380-character MSVC limit. The generators now emit
  adjacent concatenated literals split at ASCII boundaries, regenerated
  outputs pass the freshness, 47-file package-inventory, TypeScript corpus,
  and strict OpenSpec gates, and the fixture identities are unchanged. The
  complete Unicode-checkout CTest evidence for `3.10`/`4.3`/`4.4` is pending
  the next run. GitHub Actions minutes for the private repo were exhausted
  during this work; the repository was made public to continue validation and
  can be made private again afterwards.

- The first complete Unicode-checkout CTest run surfaced eight Windows-only
  Qt failures. Fixed so far: the npm inventory gate must spawn `npm.cmd`
  through the shell on modern Node (EINVAL otherwise); the Qt6 release
  policy fixture needed a longer configure timeout; the MSVC generator
  always uses rc.exe for resources, which cannot read the Unicode checkout,
  so non-ASCII checkouts compile resources through a custom llvm-rc command
  (Visual Studio ignores CMAKE_RC_COMPILER); the WebEngine sandbox is
  disabled for render fixtures on headless runners; `SkillManager::setEnabled`
  no longer keeps the manifest read handle open across the QSaveFile
  replacement; npm residue detection accepts a wider timeout for cold
  runners. The remaining open family is fake-sidecar selection in the Qt
  environment and named-pipe E2E tests: on the runner the client resolves
  the real `build/Release/aegisy-agentd.exe` instead of the
  `AEGISY_AGENTD_PATH` fake, so the fake never sees requests and the real
  Codex-less sidecar drives the failures. The named-pipe E2E stage-1
  handshake never completes within the fixture window; endpoint probing
  proved the sidecar process runs but produces no connectable pipe.

- The fake-sidecar mystery root cause is the Windows environment block
  duality: `qputenv` converts QByteArray values through the system ANSI
  code page (corrupting non-ASCII paths like the Unicode checkout), while
  `SetEnvironmentVariableW` updates only the Win32 block and is invisible
  to CRT `getenv`. Tests must use `_wputenv_s`, which keeps both blocks in
  sync without conversion. With that fix the complete Qt runtime
  environment suite (fake runtimes, heartbeat, reconnect, typed-error
  routing) passes on Windows. The artifact-manifest and update-signing
  suites, packaging policy, and Qt6 release policy also pass on the clean
  Unicode runner. Named-pipe E2E stage one (initialize, exact transport
  security facts, DACL, remote-form rejection, wrong-PID) passes; the
  restart-generation stage still fails: generation two's sidecar binds its
  pipe but the first client connection is dropped and the single-instance
  pipe then stays busy (ERROR_SEM_TIMEOUT) while the client retries.
  WebEngine render fixtures still crash silently on the headless runner
  even with sandbox/GPU disabled; investigate Chromium startup next,
  including whether the Unicode install path breaks QtWebEngineProcess.

## Cross-Platform CI Gate Repair (2026-08-07)

- Windows validation run `31152998711` at product-equivalent commit `4a2ead5`
  completed the clean Unicode checkout, version gate, and complete fail-closed
  Windows Rust gate, then exhausted the validate job's exact 90-minute budget while
  `Verify Windows Qt agent runtime` was still active. The Qt step was cancelled,
  installer/package/upload steps were skipped, and no unreported CTest may be
  inferred as passing. The validation budget is now 150 minutes and the packaging
  policy fixture rejects a regression to 90 minutes. Tasks `3.10`, `4.3`, `4.4`,
  `14.2`, and `14.9` remain open.
- Focused Windows run `31154013436` at `54129b9` passed the real Release named-pipe
  E2E, proving initialization, bootstrap-authenticated security facts, restart
  generation, endpoint rotation, and its dedicated negative matrix on Windows.
  This closes the known restart-generation incident but the workflow explicitly is
  not the release validation matrix. Its Monaco probe previously printed but did
  not enforce the process exit code, so that green run is not Monaco evidence. The
  probe now fails on native-command or nonzero process exit, is guarded by a policy
  fixture, and the named-pipe test no longer performs diagnostic connections that
  could alter a failing single-instance endpoint. The policy fixture is bound to the
  current probe step name and rejects removal of either native-command failure
  propagation or the explicit GUI process exit-code check. The complete local
  desktop gate passes `32/32`, including the policy fixture and Monaco render test;
  this is local regression evidence, not a replacement for a clean Windows run.
- macOS run `31154005122` failed with CTest exit 8 because the workflow built only
  `AegisyClient` before running every registered test; independent test executables
  were absent. The workflow now builds the complete default target graph and runs
  one unfiltered `ctest --test-dir build --no-tests=error --output-on-failure`.
  A CRLF-safe policy fixture rejects target-only builds and filtered CTest. This is
  workflow repair only; the next macOS run must provide runtime evidence.

## Windows CTest Campaign: npm Bundle and ConPTY Root Causes (2026-08-07, continued)

- Integrated a parallel session's uncommitted ConPTY hardening (injectable shell
  discovery, shared PSReadLine DSR-answer helper, four new ConPTY tests) as
  `6d0b24d`, plus its honest ledger audit: 24 checked rows covering 20 unique
  task IDs moved back to unchecked; defensible baseline is 77/235 unique tasks.
  The fail-closed fmt gate immediately caught rustfmt drift in that work (fixed
  in `d513e88`/`f97753b`).
- New ConPTY root cause: `canonical_shell` returned `Path::canonicalize` output
  verbatim, so pwsh launched as `\\?\C:\Program Files\PowerShell\7\pwsh.exe`.
  PowerShell derives `$PSHOME` from the executable path and then cannot load
  built-in modules (`Cannot find the built-in module '\\?\c:\program files\...'`),
  which failed the new interrupt test on the runner. Shells now launch through
  the shared `plain_path` form (`3c53593`).
- Monaco render test (#30) root cause, found by probing with the exact CTest
  environment: the test exits 1 on its own `Web workbench host controls are
  missing` guard because `agentMonacoEditor`/`agentXtermTerminal` are never
  created — the local web bundle is silently absent on every Windows build.
  Underlying build bug: Visual Studio custom build steps place every
  `add_custom_command` COMMAND line into one generated `.bat`, and invoking the
  npm batch shim from a `.bat` transfers control away (no implicit `call`), so
  `npm run build` and the stamp `touch` never executed; MSBuild reported
  success with only an MSB8065 warning. npm now runs as
  `node <node-dir>/node_modules/npm/bin/npm-cli.js` on Windows (`d786faf`),
  verified by a focused probe run (`31168003158`: stamp, `index.html`,
  `terminal.html`, and the bundled `build/Release/workbench` copy all present).
  Consequence: every prior Windows installer silently shipped without the
  Monaco/xterm bundle.
- Render-test probes must redirect stdout/stderr to files via `Start-Process`;
  these are GUI-subsystem binaries whose console output never reaches CTest
  (zero-output exit 1 looked like a WebEngine crash but was the Qt guard).
- `agent_workbench_render` (#18) hangs under the offscreen runner (bounded
  120s probe had to be killed); root cause still open. The ConPTY interrupt
  test's exact-byte ANSI assertion also remains open — ConPTY re-renders VT
  output, and run `31168286915` (`72f5409`) will capture the real bytes.
- Full validation run `31168286915` was in flight when this session paused; it
  carries the ConPTY plain-path fix, the npm bundle fix, and the ANSI
  diagnostics. OpenSpec `3.10`/`4.3`/`4.4` (and partial `14.2`) still wait for
  a completely green `windows-package.yml` run.

## Linux Platform Stub And Windows ConPTY ANSI CI Repair (2026-08-10)

- Main commit `a61ce4f` exposed two independent platform-gate defects. Rust Quality
  run `31325268705` failed in the Ubuntu `Run locked unit tests` step before
  Clippy, Release, or dependency audit. The non-macOS/non-Windows terminal stub had
  drifted to a one-field `TerminalSnapshot`, while shared Runtime code consumed the
  same 21-field snapshot contract as macOS and Windows, producing nine Linux-only
  `E0609` field errors. The unsupported stub now carries the complete shared shape,
  and a valid `24x80` protocol request proves unsupported platforms still fail with
  `-32090`; no Linux PTY, terminal, Agent execution, or fallback authority was added.
- Windows validation run `31325268703` failed in `Verify Windows agent runtime`
  before Qt installation or installer construction. The ConPTY interrupt fixture
  required one exact byte sequence, but the runner legitimately returned
  `ESC[31m + marker + CR + ESC[m + LF`. A platform-neutral test helper now requires
  the marker to be immediately wrapped by a valid non-reset SGR and either
  `ESC[m` or `ESC[0m`, permits only the observed optional single CR before reset,
  and rejects distant ANSI, reset-only prefixes, missing resets, and repeated CR.
  This changes test interpretation only; terminal output and process behavior are
  unchanged.
- The same baseline's macOS run `31325268727` completed its build but failed the
  `Run Tests` step with CTest exit 8. Public annotations do not identify the failing
  test, and the Linux/Windows fixes are not claimed to resolve it. The new slice has
  no clean Ubuntu, Windows, or macOS CI result yet.
- Local evidence for the code slice passes Rust formatting, strict all-target
  Clippy, the complete Rust workspace (`1137` passed, one ignored), the complete
  desktop build, all `32/32` CTests, and `git diff --check`. Clean runner evidence
  remains required; keep OpenSpec `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, and `14.9`
  unchecked. Agent/Codex remains read-only.

## Cross-Platform CI Failure Follow-Up (2026-08-10)

- Ubuntu Rust Quality run `31328260123` failed the bounded Codex startup crash-loop
  fixture because a `BrokenPipe` from direct `serde_json::to_writer` child-stdin
  output was labelled as an encoding error. That classification bypassed the
  existing transient write retry and made one startup attempt instead of three.
  Codex requests now serialize to memory before one write/flush path, and a
  deterministic BrokenPipe test requires the transport classification plus exactly
  three bounded attempts. Version and protocol failures remain non-retryable.
- Windows validation run `31328260149` stopped during strict all-target Clippy on
  `large_enum_variant` for `WorkbenchStoreOpen`; no Qt, CTest, installer, or package
  result may be inferred. Only the temporary writable variant now boxes
  `WorkbenchStore`; the two Runtime construction paths immediately recover the owned
  Store, and read-only recovery behavior is unchanged.
- macOS run `31328260142` failed only `agent_runtime_environment`. Its
  `timeline-sync-disconnect` fake exited every automatic reconnect generation while
  the assertion counted Sync calls globally, so a legal generation-two request was
  mistaken for generation-one leakage. The fixture now disconnects generation one
  only and holds reconnect behind a signal-blocked test timer. It requires the
  exact pending Sync to fail once, directly verifies that request ID is removed and
  retired plus the replay capability is cleared, then explicitly starts a strictly
  newer generation and proves each generation owns exactly one expected Sync. The
  focused test and 20 consecutive repetitions pass; production reconnect behavior
  is unchanged.
- The complete local gate passes the locked Rust workspace (`1138` passed and one
  explicitly ignored installed-Codex live fixture), Rust formatting, strict
  all-target Clippy, locked Release workspace build, complete desktop build, all
  `32/32` unfiltered CTests, strict OpenSpec validation, and `git diff --check`.
  No clean Ubuntu, Windows, or macOS run contains these fixes yet. Keep OpenSpec
  `3.5`, `3.10`, `4.3`, `4.4`, `7.2`, `14.2`, and `14.9` unchecked, and keep
  Agent/Codex read-only.

## Clean-Runner Results And Linux Strict-Clippy Repair (2026-08-10)

- At commit `560cf14784e92c5fec44ec6aac812b615d18524b`, macOS run
  `31348302508` completed successfully. Ubuntu run `31348302487` passed formatting
  and locked tests before strict Clippy failed. Windows run `31348302510` passed
  the clean Unicode checkout and complete Rust gate, then failed the Qt Runtime
  build/test step. Public evidence does not identify the Windows compiler or CTest
  failure, so it grants no complete Windows desktop, named-pipe,
  bootstrap-authentication, ConPTY, installer, or package claim.
- Linux reproduction on `rust:1.97.1-bookworm` identified only compile-boundary
  lint regressions: two macOS/Windows-only test import groups lacked their platform
  guards, the terminal-only `ToolVariable::new` constructor remained visible in a
  Linux non-test library build, and Unix `statvfs` available-space arithmetic used
  a same-type cast. The imports and constructor now use their exact platform/test
  guards, and available space uses a saturating `u128` product with checked `u64`
  conversion. No protocol, persistence, permission, Approval, mutation, execution,
  dispatch, terminal, or Agent/Codex authority changed.
- Native Windows and Linux-container formatting/strict all-target Clippy pass;
  affected Linux module tests and both locked Release workspace builds pass. The
  broader local container and Windows suites retain environment-specific existing
  failures outside this source-only repair, while the preceding clean runners passed
  the corresponding Rust test steps. A new clean Ubuntu run and a diagnosed,
  successful Windows Qt run remain required. Keep OpenSpec `3.5`, `3.10`, `4.3`,
  `4.4`, `7.2`, `14.2`, and `14.9` unchecked, and keep Agent/Codex read-only.

## Update Signing Key Ring Local-Integrity Continuity Cache (2026-08-09)

- The committed `aegisy-update-signing-key-ring-continuity/0.1` layer persists
  only exact signed Ring envelope bytes and bounded local integrity metadata as
  immutable generation objects, a private bootstrap marker, and an atomically
  replaced head. It never persists or deserializes `Authority`, admission time,
  `accepted_at`, or verification tickets.
- Every load replays generations `1..N` from the embedded Root with the current
  `nowMs`. Strict current-time replay returns `Authoritative`; only a complete
  historical chain whose strict failure is `bootstrap-root-invalid`,
  `signer-inactive`, or `no-current-active-usage` returns
  `CachedButNotAuthoritative` and carries no valid Authority. Revoked,
  malformed, structurally invalid, and signature-invalid chains remain
  `Invalid`. An exact retry of
  the already accepted latest envelope is idempotent and no-write, including a
  request carrying either the current or immediately previous cache identity;
  stale CAS that would admit a different envelope or create a new generation,
  repairs, and appends from cached-only state fail closed.
- The bounded cache accepts at most 64 envelopes, 128 KiB per envelope, and 8 MiB
  per chain. Marker/head/object and prefix-head identities, exact envelope binding,
  unknown/partial deletion, private Unix permissions, link/file identity, local locking,
  expected-head CAS, stale evidence, and authority-field tamper cases are covered
  by `update_signing_key_ring_cache_integrity`, which is in the complete desktop
  test graph. Complete deletion means the cache directory itself is absent and is
  reported as `Empty`; a retained directory with missing marker/head/objects is
  partial deletion and is `Invalid`. The cache does not claim to detect a
  consistent deletion and rollback of all local evidence. Unix permission evidence
  is local/macOS evidence; Windows ACL enforcement is implemented but still awaits
  clean-runner evidence.
- This is a local-integrity continuity cache, not a Trust Store or release authority.
  It is not consumed by an updater and grants no update, network, download, install,
  rollback, resume, execution, anti-rollback, anti-deletion, trusted-time, or
  expired-signer-recovery authority. Authenticated Ring publication, secure
  high-water anchoring, updater transaction binding, and clean Windows/package
  evidence remain OpenSpec `22.5` gates. Keep `22.5` unchecked.

## Historical Non-Turn Complete-Source Store Foundation (2026-08-10)

- Workbench database schema v22 adds the strict
  `mutation_reservation_sources` table and upgrades
  `mutation_reservation_records` to provenance-aware `0.2`. It accepts exactly one
  complete `approval-acknowledgement/0.1`, `file-write-acknowledgement/0.1`,
  `git-mutation-acknowledgement/0.1`, or `background-job-request/0.1` source and
  derives the historical lossy draft internally. The source, draft, hashes,
  redundant scope/key/fingerprint bindings, lifecycle, and four fixed-false authority
  fields contain no prompt, command, path, provider body, result, credential,
  permission, or authority data.
- A new tuple commits provenance `present`, its immutable complete source, the
  derived reservation, and one metadata-only
  `mutation.reservation-source-recorded` internal Session event atomically. Exact
  complete-source retry returns the unchanged graph with zero writes and no Session
  sequence movement under ordinary, low-space, and writer-contention conditions.
  Any complete-source drift conflicts even when the derived drafts are byte-identical.
  Focused failure-injection fixtures cover every graph stage and leave no partial row
  or sequence movement.
- Startup validates source/reservation/event identity, canonical JSON, hashes,
  anchors, ownership, scope, lifecycle, time, limits, and exact event order. Candidate
  capture, validation, revision-2 reconciliation, the single
  `mutation.reservation-reconciliation-required` event, and the final no-open-row
  assertion share one `IMMEDIATE` transaction. A second startup performs zero writes.
  A `present` reserved graph has exactly `[source]`; a `present`
  reconciliation-required graph has exactly `[source, reconciliation]`. Missing,
  reversed, duplicated, orphaned, unknown, or tampered history enters whole-Store
  read-only recovery. A `legacy-unavailable` reconciliation-required row instead has
  exactly empty lifecycle history `[]`. No Public Timeline event or public sequence
  is created.
- The v21-to-v22 migration marks every validated v21 row `legacy-unavailable` and
  `reconciliation-required` revision 2 without fabricating source/event history. It
  validates the exact v21 schema, every row, and the 10,000-row bound before copy.
  All pre-v22 migration paths reject reserved event-kind, operation-ID, and Event-ID
  namespaces plus Triggers on `events` or `session_sequences`; transactional
  `user_version` and `application_id` rechecks prevent a stale or foreign migration
  branch. The migration lock and backup source use separate pre-opened connections:
  the latter holds one `DEFERRED` WAL snapshot across identity checks and Online
  Backup, while cross-platform file identity prevents path substitution. A
  current-v22 open skips the migration write lock. Canonical v22
  inventory covers both tables, indexes, auto-indexes, and attached objects. The
  covering `events(session_id, operation_id, sequence, event_kind)` index is selected
  by the exact lifecycle query without requiring `ANALYZE`.
- Focused evidence passes `7/7` complete-source contract tests and `39/39`
  `non_turn_mutation` Store tests. Coverage includes all four sources, hidden source
  drift, atomic initial graph and rollback, zero-write retry, startup race and
  reconciliation, provenance/event tampering, v21 semantic and over-limit rejection,
  v20/v12 namespace collisions, shared Trigger rejection, concurrent migration,
  bounded lock timeout/retry, locked version/application-ID drift, current-v22 lock
  avoidance, backup-source/path identity replacement, scope/root/deletion/purge
  behavior, row/byte bounds, and query-plan selection. The complete local gate passes
  Rust formatting, strict all-target Clippy, `1135` workspace tests with one explicit
  installed-Codex live fixture ignored, the desktop build, all `32/32` CTests, strict
  OpenSpec validation, and `git diff --check`. The path-replacement runtime fixtures
  executed on the macOS/Unix host; the Windows file-identity branch still awaits its
  normal native runner evidence.
- At this historical v22 boundary, the Store graph was not a kind-specific
  acknowledgement or permission boundary and had no durable outcome row/event. Its
  source, migration, reconciliation, and validation behavior remains the exact v23
  migration baseline.

## Non-Turn Terminal Outcome Store Foundation (2026-08-10)

- `mutation_reservation_outcome.rs` defines the crate-internal terminal result union
  for the four complete-source kinds. Approval, file-write, and Git
  outcomes must be terminal revision-two acknowledgements and match the exact source;
  a job outcome must be a terminal `background-job-state/0.1` that validates against
  the exact reserved job request. Non-terminal and reconciliation-required values are
  rejected. Approval outcomes remain non-granting, and every underlying authority
  field remains fixed false.
- The outcome preserves the underlying reviewed JSON without another wire envelope,
  enforces a 16 KiB canonical byte limit, and derives both exact SHA-256 and a
  domain-separated identity bound to the complete source identity and kind. Kind,
  idempotency, request, edit/plan, or job-request drift fails closed. Unknown fields,
  non-canonical JSON, and forged authority are rejected, including unknown fields in
  the older background-job state serde shape.
- Workbench schema v23 persists one immutable outcome row and metadata-only internal
  `mutation.reservation-outcome-recorded` event while compare-and-swapping the
  reservation from `reserved` revision 1 to `terminal` revision 2. Event append,
  outcome insert, CAS, complete graph validation, and commit share one `IMMEDIATE`
  transaction. Failure at any stage rolls back the graph and internal Session
  sequence.
- Exact outcome retry returns the original graph with zero writes, including after
  sampled low space; a later retry-attempt timestamp neither rejects nor rewrites the
  persisted original time. If a peer commits while admission is pending, the write-lock
  recheck returns an exact peer outcome or a stable drift conflict without appending
  another event. Owner, source/kind, revision, Session archive/pending deletion, and
  project/root/Turn scope are rechecked before mutation. Outcome time must satisfy
  `reserved_at_ms <= observed_at_ms <= recorded_at_ms`; admission rejects an
  observation before reservation without changing the graph, and direct/startup
  validation treats the same persisted drift as an integrity failure.
- Valid lifecycle histories are exactly reserved `[source]`, terminal
  `[source, outcome]`, reconciliation-required `[source, reconciliation]`, and
  migrated legacy `[]`. Startup treats missing, reordered, orphaned, tampered, or
  authority-bearing outcome state as whole-Store read-only recovery. The exact
  v22-to-v23 migration validates and backs up the v22 graph, copies it without
  fabricating outcomes, then validates the complete v23 inventory and graph. Session
  purge removes outcome dependencies in the same deletion transaction.
- The focused `non_turn_mutation` suite passes `50/50`; Rust formatting, strict
  workspace/all-target Clippy, and the locked Release workspace build pass in
  `rust:1.97.1-bookworm`. The broader container run passes `894/896`
  `aegisy-agentd` library tests and retains the same two base-commit Git transaction
  fixture failures: `previews_and_commits_only_agent_delta_while_preserving_user_index_and_worktree`
  and `injected_ref_failure_rolls_back_and_external_ref_rewrite_is_preserved`. With
  only those two exact fixtures skipped, the library suite passes `894/894`.
- This slice remains an internal persistence foundation. It exposes no production
  caller, AAP/Qt surface, external caller CAS or consumption, reconciliation
  resolution, dispatch, filesystem/Git/job mutation, or genuine user Approval. All
  four authority fields remain false. Keep OpenSpec `3.6`, `5.1`, and `5.2`
  unchecked and Agent/Codex read-only.

## Non-Turn Outcome Time-Ordering Integrity (2026-08-12)

- Outcome admission and persistent graph validation now close the exact time
  invariant `reserved_at_ms <= observed_at_ms <= recorded_at_ms`. A newly observed
  outcome before its reservation returns
  `mutation-reservation-outcome-time-invalid` before any event, row, CAS, or Session
  sequence mutation. A hash-consistent persisted graph with the same drift fails
  direct read and enters `ReadOnlyRecovery` on restart with
  `workbench-database-integrity-failed`.
- Two focused tests cover zero-write admission and restart recovery, increasing the
  `non_turn_mutation` suite from `48/48` to `50/50`. The complete
  `aegisy-agentd --lib` run passes `894/896`; only the two documented base Git
  transaction fixtures fail, and excluding those exact fixtures yields `894/894`.
  Rust formatting, CI-equivalent strict workspace/all-target Clippy, locked Release
  workspace build, strict OpenSpec validation, and `git diff --check` pass in the
  Linux Rust container.
- At base commit `73b841f2beaedcc7cd911ec7664c8027cb26548a`, macOS run
  `31451782643` succeeded. Ubuntu run `31451782669` failed its locked unit-test step,
  and Windows run `31451782650` still failed
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi` during
  the Rust workspace test. No later Windows/Qt/package stage ran. These CI failures
  remain separate follow-up work and provide no completion evidence for this Store
  repair.
- This remains a crate-internal persistence invariant. It adds no AAP/Qt route,
  producer, consume/external-caller-CAS path, recovery consumer, dispatch,
  filesystem/Git/job mutation, genuine user Approval, or authority. OpenSpec `3.6`
  remains unchecked and Agent/Codex remains read-only.

## Windows Qt CI Diagnostic Visibility Follow-Up (2026-08-10)

- At base commit `a5fabf7bfff276abb347e762b239cb67d240af56`, Ubuntu Rust
  Quality run `31353499041` and macOS run `31353498997` completed successfully.
  Windows run `31353498994` passed the clean Unicode checkout and complete Windows
  Rust gate, then failed the combined `Verify Windows Qt agent runtime` step after
  30 minutes. Installer and package steps were skipped.
- The public check annotation reports only exit code 1, so it does not prove whether
  CMake configuration, MSVC compilation/linking, or CTest failed. No Windows desktop,
  reconnect, named-pipe, bootstrap-authentication, ConPTY, installer, or package claim
  is inferred from this run.
- `windows-package.yml` now separates configure, build, and test into distinct gates.
  A failed build emits at most 20 recognized compiler/CMake summary lines, while a
  failed CTest emits at most 50 entries from `LastTestsFailed.log`; every annotation
  is capped at 2,000 characters. The exact configure/build/test commands and
  fail-closed exit behavior are unchanged. A new clean Windows run is required before
  diagnosing or closing the affected OpenSpec gates. Keep Agent/Codex read-only.
- Diagnostic Windows run `31357547379` at commit `1f12072e3844289655c52b777e7c201eda3913f7`
  passed the clean Unicode checkout, complete Rust gate, Qt/OpenSSL installation,
  CMake configuration, and MSVC Qt build. CTest then failed exactly
  `agent_runtime_protocol`, `agent_workbench_render`, and `monaco_editor_render`;
  installer/package gates were skipped. Public annotations still expose no assertion
  text, so no protocol or UI root cause is inferred. The failure path now reruns only
  the failed tests, redacts runner roots, and emits at most 50 bounded diagnostic
  lines as annotations. A new clean Windows run is required to identify and repair
  the earliest concrete failure. No Windows Runtime, reconnect, named-pipe,
  bootstrap-authentication, renderer, installer, or package completion is claimed.
- Follow-up Windows run `31362432359` at commit `f709449dc6bf9d579a38cae7c4d63efb8bb0257d`
  passed the Unicode checkout, complete Windows Rust gate, Qt/OpenSSL installation,
  CMake configuration, and MSVC build. The first CTest pass failed
  `tool_manager_runtime_registry`, `agent_workbench_render`,
  `windows_packaging_policy`, and `monaco_editor_render`; the failed-set rerun passed
  `tool_manager_runtime_registry`, making `agent_workbench_render` the earliest
  stable public failure. Bounded public annotations still expose no concrete
  assertion, and installer/package/upload were skipped. This run closes no Windows,
  renderer, named-pipe, bootstrap, ConPTY, installer, or release gate.
- At terminal-outcome commit `795f60dac59b792357820d347c3f9db27f164024`,
  Ubuntu Rust Quality run `31380280496` passed every fmt/test/Clippy/Release/audit
  gate. Windows run `31380280481` passed the Unicode checkout but failed the combined
  Rust step before Qt, exposing only exit code 1; it provides no reliable sub-gate or
  test identity. macOS run `31380280575` passed policy, setup, configure, and build,
  then failed unfiltered CTest with no public failed-test identity. These results do
  not contradict the local v23 focused/Linux evidence, but they do not prove Windows
  or macOS completion and must be diagnosed before closing any platform gate.
- Windows Rust validation now separates toolchain setup, formatting, workspace tests,
  strict Clippy, locked Release build, offline AAP packaging, and pinned dependency
  audit into independent fail-closed steps without changing their commands. Test
  failure publishes one 2,000-character maximum annotation containing only failed
  test identities, panic source locations, aggregate failure lines, or a fixed
  fallback after runner-root redaction; arbitrary stdout and assertion values are
  excluded. `windows_packaging_policy` requires the exact unfiltered Rust test, all
  seven step names, and the exact complete CTest plus bounded failed-set rerun pair,
  with CRLF and filtered/merged negative cases. Local policy, PowerShell-parser, YAML,
  and diff gates pass. A new clean runner is required; no Windows Rust, desktop,
  installer, package, or release completion is claimed.

## Windows Qt Renderer Software Path And Bounded Diagnostics (2026-08-10)

- At commit `b7b671aa6e533410a80986212020c90fb2ade230`, macOS run
  `31382263963` passed. Windows run `31382263998` passed the clean Unicode
  checkout, all seven separated Rust gates, Qt/OpenSSL installation, CMake
  configure, and MSVC build before CTest failed. Its failed-set rerun passed the
  initially failing ToolManager test, reproduced `agent_workbench_render`, and
  started `monaco_editor_render`, but the public ten-error annotation ceiling hid
  the exact failure diagnostics and root causes. No Windows desktop, named-pipe, bootstrap, ConPTY,
  installer, package, or release claim follows from that run.
- The Monaco CTest environment previously set only
  `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`. Because the fixture installs its full
  Windows flags only when that variable is empty, CMake unintentionally suppressed
  `--disable-gpu-compositing`, `--no-sandbox`, and diagnostic logging. Windows now
  owns the complete test-only software path in CMake: `QT_OPENGL=software`, preferred
  software RHI, sandbox disable, disabled GPU/compositing, and bounded WebEngine
  context logging. Non-Windows retains the previous `--disable-gpu` behavior.
- Both renderer fixtures prefix reviewed static failures with
  `AEGISY_TEST_FAILURE:`. The workflow selects only that marker, fixed GLES/context
  fatal classes, or a high-level CTest fallback; it combines all failed test names
  into one annotation and all allowlisted diagnostics into one second annotation.
  Each is capped at 2,000 characters; runner roots are regex-escaped and redacted
  case-insensitively in both slash forms, and arbitrary test output is never
  published as an annotation. `windows_packaging_policy` requires the marker,
  scoped fixed filters, first/last bounds, combined-message construction, original
  CTest exit propagation, root redaction, and the complete Windows software-rendering
  environment. Negative mutations cover prefix, aggregation, length, fallback,
  redaction, and exit-code drift.
- Local evidence passes YAML parsing, direct and registered Windows packaging
  policy checks, both focused renderer tests, their complete target builds, and all
  `32/32` unfiltered desktop CTests. The complete gate also executes the Rust
  workspace through `agent_runtime_protocol`.
- A fresh clean Windows run is still required. The software path can affect Monaco
  only; `agent_workbench_render` does not link WebEngine and must be diagnosed from
  its own new marker if it remains red. Keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`,
  `14.9`, installer, package, and release gates open, and keep Agent/Codex read-only.

## Windows Qt Renderer D3D11 Path And Fixed Diagnostic Channel (2026-08-11)

- The Windows Monaco test now uses real `windows` QPA and D3D11 RHI with a software-
  adapter preference. QuickWidgets is part of the activation, link, and release
  gates. These settings and assertions do not prove WARP or Chromium's internal
  rendering path; detailed evidence remains in the OpenSpec verification record.
- Windows validation run `31426799633` at predecessor commit
  `4d3e10cde48531d17646058814e32ab49b00d986` reached the Qt CTest gate and failed
  exactly `8:tool_manager_runtime_registry`, `18:agent_workbench_render`, and
  `33:monaco_editor_render`. Its only fixed diagnostics were the old generic
  `AWB_ASSERTION` and `MONACO_ASSERTION`. The complete Windows Rust gate and every
  other registered CTest passed, including Runtime environment,
  named-pipe/bootstrap, generated AAP, and ConPTY coverage. This is predecessor
  component evidence only; it does not prove the three failures, WARP/Chromium,
  installer, package, signing, or release.
- ToolManager and both GUI renderers now share 50 fixed failure codes with stage-
  specific Workbench and Monaco diagnostics. ToolManager uses isolated Unicode fake
  `node`/`npm` shims requiring five ordered arguments; Windows comparison is case-
  insensitive while POSIX comparison is exact. Nested scoped guards use `_wputenv_s`
  so Qt and the CRT observe and restore the same Unicode environment state.
- The Windows workflow discards the initial unfiltered CTest output while retaining
  its original exit code once. A failed-set rerun is classified as a stream: at most
  50 fixed markers are retained, or a 20-line queue of fixed high-level CTest state
  is used. The exact Qt 6.8.3 DXGI-factory, D3D11 device/context, and
  `ID3D11DeviceContext1` failure prefixes all map to the single fixed
  `QT_D3D11_INITIALIZATION` code; HRESULTs and other dynamic suffixes are discarded.
  Raw rerun output and arbitrary assertion, `qCritical()`, GLES suffix, path, or
  environment text cannot enter a public annotation. Rerun lines over 4,096
  characters are never classified. Failed names must match one exact bounded ASCII
  `index:test-name` grammar from `LastTestsFailed.log`, with at most 50 entries; the
  fallback retains only six fixed CTest categories and never `$line`. Failed names
  and allowlisted diagnostics remain two separately encoded, post-encoding-capped
  2,000-character annotations. The diagnostic annotation separately receives
  case-insensitive runner-root redaction.
- `windows_packaging_policy` now proves the enum, C++ mapping, and workflow marker
  sets are one-to-one from one canonical 50-code list; requires all
  ToolManager/Workbench/Monaco stages, one
  line-bound stage immediately before each owned region, exactly one scoped stage in
  each Workbench helper, and direct binding of all five D3D11 assertions. The Qt test
  step must be unique, retain SHA-256
  `ce0990fd4e6b0c8b1b22611b275ddd45ad41a573b8f75888f81e43a5714c7f8e`, and own
  the normalized CTest temporary-log path, both CTest log names, annotation titles,
  fixed-code prefix, and private Qt diagnostic state in the workflow. Output-command
  rejection and the exact three-`Write-Output` count are case-insensitive. The
  ToolManager test macro must occur exactly once in the exact
  `AegisyToolManagerRuntimeTest PRIVATE` compile-definition command. Negative
  mutations cover duplicate publishing steps, external raw/wildcard CTest logs,
  borrowed diagnostic state, one-sided marker changes, test-macro target/scope drift,
  lowercase/uppercase console bypasses, early/late/comment-spoofed stages, alternate
  scoped-stage construction, comment-spoofed D3D11 bindings, permissive npm shims,
  Win32-only environment writes, unbounded capture, raw publication, dynamic suffixes,
  aliases, renderer settings, offscreen/software backends, and GPU-disable flags.
- Local macOS evidence includes the affected target rebuild, direct policy, focused
  `6/6`, twenty consecutive ToolManager registry runs, and the final post-hardening
  serial unfiltered `34/34` CTest run in 186.95 seconds
  (`agent_runtime_protocol` in 134.17 seconds).
  The complete desktop build, Rust formatting, locked strict all-target Clippy,
  locked Release workspace build, registered Windows packaging policy, workflow YAML
  parsing, strict OpenSpec validation, and `git diff --check` also pass. No current
  commit with this slice has
  compiled or executed the current 50-code/stage-specific repair on Windows. The
  predecessor run above does not prove the failing stages or their repair. Keep
  `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, `14.9`, installer, package, signing, and
  release gates open. Agent/Codex remains read-only.

## Windows ConPTY Post-Interrupt Readiness Follow-Up (2026-08-11)

- At commit `683449bb5ee772990f4e1d20f70e636236d9c573`, macOS run
  `31449651947` passed its complete build and unfiltered CTest gate. Windows run
  `31449651952` passed the Unicode checkout, toolchain setup, and Rust formatting,
  then failed the Rust workspace test
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi` with
  `897` passed, one failed, and `122.58s` total test time. Clippy, Release, Qt,
  CTest, installer, package, upload, and publication were skipped.
- Public evidence contains no panic location or assertion detail. The target is
  wrapped by a 120-second stage timeout, so the `122.58s` duration strongly suggests
  a timeout but does not prove the exact blocked operation. `agent-runtime` was
  unchanged from predecessor commit `4d3e10c`, where the ConPTY component passed;
  this supports a nondeterministic fixture diagnosis without replacing a repaired
  clean-runner result.
- The fixture previously sent its ANSI/exit command after a fixed 250 ms delay from
  Ctrl+C. It now sends a split-literal shell-readiness command whose echoed input
  does not contain `AEGISY_INTERRUPT_COMPLETE`, waits for that exact marker from
  successful cmd/PowerShell execution, and only then sends the ANSI marker plus
  `exit 23`. Production ConPTY behavior and timeout policy are unchanged.
- Local `rust:1.97.1-bookworm` evidence passes formatting, strict workspace/all-target
  Clippy, the locked Release workspace build, and the focused ANSI helper tests. The
  first full workspace run retained the two documented base Git transaction fixture
  failures and one transient Git-version fixture failure; the transient fixture
  passed its exact rerun. With only the two documented Git transaction fixtures
  skipped, all remaining workspace targets pass. Windows-only execution remains
  pending.
- No OpenSpec task closes. Keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, `14.9`,
  `23.10`, installer, package, signing, and release gates open. Agent/Codex remains
  read-only.

## Windows ConPTY Session-Scoped DSR Tracker Follow-Up (2026-08-12)

- At base commit `406c04113e99f718bbb66925fff156f3a0094bbb`, Ubuntu run
  `31572128958` and macOS run `31572128947` succeeded. Windows run `31572128979`,
  job `94036188371`, failed
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi` with
  `899` passed, one failed, and `181.16s` total test time. Clippy, Release, offline
  AAP packaging/audit, Qt, CTest, installer, package, upload, and publication were
  skipped.
- Public logs contain no panic or assertion detail, so they do not prove one unique
  root cause. Source review did identify a deterministic defect in the Windows-only
  fixture: each output/exit wait reset its DSR state and could answer retained
  `ESC[6n` queries repeatedly, while multiple new queries in one snapshot received
  only one reply. This is a credible failure mechanism, not proof that no other
  Windows issue remains.
- The fixture now owns one absolute-offset cursor-query tracker for the complete
  terminal lifecycle. It scans only newly observed bytes, retains the final three
  bytes for split-query detection across snapshots and the 1 MiB rolling-capture
  boundary, counts every new query, and fails immediately on output gaps, backward
  ranges, byte/range mismatch, or DSR write failure. No reply is sent after terminal
  exit. Production ConPTY, timeouts, protocol, and permission behavior are unchanged.
- Local evidence passes the six platform-neutral terminal helper tests, Rust
  formatting, strict workspace/all-target Clippy, the complete locked Rust workspace
  (`1160` passed and one installed-Codex live fixture ignored), the complete desktop
  build, all `34/34` unfiltered CTests, strict OpenSpec validation, and
  `git diff --check`. macOS cannot compile or execute the Windows-only ConPTY module;
  a fresh clean Windows run remains required.
- No OpenSpec task closes. Keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, `14.9`,
  `23.10`, installer, package, signing, and release gates open. Agent/Codex remains
  read-only.

## Non-Turn Reservation Consumption Ledger (2026-08-13)

- Workbench schema v24 adds one crate-internal
  `mutation_reservation_consumptions` row per consumed non-Turn reservation. Strict
  `mutation-reservation-consumption-receipt/0.1` values carry only identities,
  existing internal event anchors, core/consumption revisions, ordering, and time.
  They contain no request/outcome body, success claim, permission, decision, or
  authority; dispatch, mutation, Approval, and execution fields are fixed false.
- Core and consumption revisions are independent. Source evidence can move `c0` to
  `c1` against core `r1` or `r2`; terminal or reconciliation evidence can move
  `c1` to `c2` only at core `r2` and must bind the exact source receipt. Startup may
  move an open core reservation from `r1` to reconciliation-required `r2` without
  changing `c0/c1`. Resolution-before-source and legacy-unavailable consumption are
  rejected. Startup now clamps that `r1 -> r2` transition time to any validated
  `c1` source receipt consumption time, in addition to the reservation and current
  observation time, so wall-clock rollback cannot invalidate a legitimate receipt
  and force whole-Store read-only recovery.
- Exact retry uses a read-only `DEFERRED` snapshot before write admission and returns
  the first receipt/time with zero writes. A new write uses one `IMMEDIATE`
  transaction to reclassify peer commits, recheck Session owner/archive/pending
  deletion and project/root/Turn scope, compare both revision domains, insert or
  advance the ledger, rebuild the complete receipt graph, and commit. Low-space,
  peer-exact, peer-core-drift, insert/update/validation, and final-commit boundaries
  are covered. A source-INSERT race under sampled low space returns the peer's first
  immutable receipt/time, changes only the one ledger row, and leaves both event
  sequences unchanged; evidence and core-revision drift remain conflicts.
  Consumption appends no event and moves neither internal nor Public Timeline
  sequence.
- Whole-Store startup now performs a bounded semantic scan of every consumption
  identity and rebuilds its source/resolution receipts. Schema, receipt, evidence,
  event-anchor, owner/kind, phase, time, and authority drift enter
  `ReadOnlyRecovery`. In a final core `r2` graph, a source receipt claiming `r2`
  cannot predate reservation `updated_at_ms`, while one claiming `r1` cannot postdate
  that transition. A future-dated `r1` receipt rolls back outcome admission. Separate
  regressions synchronously recompute both receipt identities and prove both a
  hash-consistent early `r2` forgery and a hash-consistent late `r1` forgery fail the
  direct whole-Store scan and restart into read-only recovery. Contract coverage also
  recomputes identities while checking terminal and reconciliation previous-receipt,
  event-sequence, event-time, and consumption-time ordering.
- The v23-to-v24 migration first validates the exact v23 schema and graph, publishes
  a WAL-consistent v23 backup, adds the empty ledger/index/Triggers, and validates
  v24. Reservation/source/outcome DTOs, internal events, and internal/Public sequence
  state remain unchanged; no historical receipt is fabricated. Session purge removes
  consumption before outcome/source/reservation dependencies.
- Because `c0 []` is a valid history and v24 has no independent high-water or parent
  consumed marker, complete deletion of a valid consumption row is indistinguishable
  from never having consumed it. The ledger detects retained-row corruption but is
  not anti-deletion or strict at-most-once authority.
- Focused evidence currently passes the 9 receipt-contract tests, 13 Store consumption
  tests, and complete 63-test non-Turn mutation suite in
  `rust:1.97.1-bookworm`. Full workspace gates and CI status belong in the OpenSpec
  verification log for the committed slice.
- OpenSpec `3.6`, `5.1`, and `5.2` remain unchecked. This is not a production
  approval/file/Git/job producer, external caller-CAS or AAP/Qt consume route,
  recovery UI, dispatch path, filesystem/Git/job mutation, genuine user Approval,
  or authority. Agent/Codex remains read-only.

## Product Direction Reset (2026-08-23)

- The product direction was explicitly reaffirmed on 2026-08-24: Aegisy is first a
  companion for the Aegisy website, focused on one-click configuration, compatible
  plugin enablement, Chinese/desktop enhancements, and custom Skills/MCP management.
  Codex is the only integrated programming runtime in phase one. Claude, Gemini, and
  OpenCode remain configuration targets only where the companion flow requires them;
  their embedded programming runtimes are not implementation targets.
- The main navigation now names the active workflow `配置中心`, `桌面增强`,
  `接入配置`, `本地网关`, `插件与 Skills`, and `Codex 编程`. The window/login
  identity is `Aegisy 网站配套助手`; the first page remains the local configuration
  and environment dashboard.
- The retained WebEngine preview is labelled `Aegisy Codex Programming` and no
  longer advertises a Claude model. The production Rust Runtime continues to compile
  only the Codex adapter; Claude/Gemini/ACP Agent modules are absent.
- `product_scope_policy` statically verifies the companion navigation, all four
  supported configuration targets, Codex-only integrated programming, deferred
  adapter absence, and the controlling OpenSpec proposal/spec. Focused companion
  tests cover tool runtime/config, desktop enhancement, Skills, and the product
  scope policy. This decision changes product scope and naming only; it grants no
  new local or Agent authority.
- The website projection foundation is account/origin/auth-epoch bound and
  cache-isolated. The SecureStorage-backed opaque broker and ConnectWizard path are
  implemented, while Profile schema 7 closes SecureStorage reference injection,
  credential-tail persistence, and unbound website source metadata. It remains
  partial: the revisioned local-HMAC cache is production-persisted, ordinary website
  model observations can enter it, and validated read-only DTOs now reach
  ConnectWizard, Models, and Chat without restoring any live authority. The cache is
  not a server signature and cannot detect consistent rollback/deletion of all local
  evidence; native Windows production evidence remains open. ToolManager encrypted
  backup integration is implemented separately below.
- The execution order is now explicit: finish the website/configuration trust base,
  then one-click apply/repair, extension/Skills/MCP management, and Chinese/desktop
  enhancements. The retained Codex destination stays bounded and optional; no
  Claude/Gemini embedded programming implementation is scheduled.

## Chat Companion Credential Migration (2026-08-23)

- `ChatDialog` no longer subscribes to `apiKeysReceived` or global
  `modelsReceived`, retains the raw website Key array, stores credential plaintext in
  combo item data, displays Key fragments, or calls the raw chat/image/presentation
  credential methods. It accepts only active candidates whose credential state is
  `available-in-secure-storage`, stores only account/Key/projection/opaque-handle/
  platform/display/group metadata, and consumes the exact request-correlated
  `aegisy-companion-model-projection/0.1` result.
- ApiClient companion wrappers for Chat, image Skill, and presentation Skill bind a
  unique request ID to the current auth generation, verified account, current source
  projection, website-Key identity, credential handle, platform, and exact reviewed
  website origin. They resolve the credential once through
  `CompanionCredentialBroker` inside ApiClient and then delegate to the existing
  provider transport. Origin, auth, or projection replacement retires active bound
  operations; presentation retries and late results recheck the binding and become
  inert after retirement.
- Chat history advances to schema 2 and persists only a valid
  `website-key:sha256:` identity plus a bounded safe display name. Legacy `key_id`
  content is not loaded or rewritten. Active Profile matching now uses persisted
  hashed website account/Key bindings and does not load the Profile credential.
- The complete desktop target builds, focused companion/API/Profile/Tool/product-
  scope tests pass `8/8`, strict OpenSpec validation passes, and `git diff --check`
  passes. OpenSpec `0.2` remains unchecked because API-Key management and revisioned
  authenticated model/cache state remain open. Agent/Codex stays read-only.

## Standalone Image Companion Migration (2026-08-23)

- `ImageGenerationDialog` now consumes only the validated companion configuration.
  It selects active SecureStorage-backed candidates by the bounded `gpt-image` group
  label and stores only the opaque handle plus account, hashed Key, source projection,
  platform, group, and safe display metadata in widget item data. It no longer
  subscribes to `apiKeysReceived`, retains the raw website Key inventory, displays
  Key fragments, or calls the raw image credential API.
- Each generation uses a unique `image-dialog-*` request ID and the existing
  ApiClient `generateCompanionImage` boundary. The dialog consumes only matching
  `companionImageGenerated`/`companionImageFailed` results; auth, origin, or source-
  projection replacement retires the ApiClient binding before the old result can
  replace the preview or be saved.
- The complete desktop target builds, the focused companion/API/Profile/Tool/product-
  scope set passes `8/8`, strict OpenSpec validation passes, and
  `git diff --check` passes.
  OpenSpec `0.2` stays unchecked because API-Key management and revisioned
  authenticated model/cache state remain open. Agent/Codex stays read-only.

## Companion Usage Projection Migration (2026-08-23)

- `aegisy-companion-usage-projection/0.1` binds one result to the hashed website
  account identity and exact current configuration projection. Each row contains only
  hashed Key identity, bounded display/group/state metadata, non-negative today/total
  cost and quota metrics. Raw website Key IDs, credential values, and configuration
  authority are fixed absent/false; exact fields, counts, identities, values, and the
  projection digest are independently validated.
- ApiClient derives a minimal in-memory usage source after a complete validated Key
  response: raw website ID, hashed Key identity, and bounded quota values only. The
  source is cleared on auth/origin changes and replaced with the source projection.
  `getCompanionApiKeyUsage` binds a unique request to auth generation, verified
  account, exact current projection/origin/URL, manual redirect, identity encoding,
  JSON Content-Type, and 1 MiB response bounds. It maps raw-ID response keys back to
  hashed identities internally, rejects unexpected IDs, and emits only the validated
  usage projection; retired or stale responses are inert.
- `UsageDialog` no longer subscribes to `apiKeysReceived`, calls the raw-ID usage
  method, or retains raw-ID-keyed statistics. It consumes companion configuration for
  initial safe rows and only an exact request/account/configuration-correlated usage
  projection for costs and quota display.
- The complete desktop target builds, the focused companion/API/Profile/Tool/product-
  scope set passes `9/9`, strict OpenSpec validation passes, and
  `git diff --check` passes.
  OpenSpec `0.2` stays unchecked because model results are not yet merged into a
  revisioned authenticated cache. Agent/Codex stays read-only.

## Companion API-Key Management Migration (2026-08-23)

- `aegisy-companion-key-management-projection/0.1` is an online-only, exact-field
  projection bound to the verified website account and current configuration SHA.
  It contains safe Key/group display metadata, state/quota/date values, and separate
  group/create/test/update/delete handles. All handles are 256-bit system-random,
  action-scoped, globally unique within the projection, expire after 15 minutes, and
  are rechecked against the exact management projection. Raw Key/group IDs,
  credentials, credential fragments, configuration authority, and mutation authority
  are fixed absent/false; the projection is never cached or persisted.
- ApiClient alone retains the live capability table mapping those handles to raw
  Key/group IDs and SecureStorage credential handles. Management reads and writes
  bind unique request ID, auth generation, account, configuration and management
  projections, origin/final URL, manual redirect, no-cache policy, identity encoding,
  JSON Content-Type, TLS, and 256 KiB response bounds. Raw Key path segments are
  percent-encoded. Server response bodies and create response data never cross the
  safe result signal. Any dispatched mutation consumes the management context and
  forces a complete refresh; stale responses are inert. Delete reports local
  SecureStorage cleanup separately instead of claiming rollback after remote success.
- `ApiKeysDialog` no longer receives raw Key/group inventories, stores plaintext or
  raw IDs, displays credential fragments, writes preferred raw IDs to QSettings,
  copies credentials, or uses uncorrelated global operation/error signals. New/edit,
  group change, enable/disable, test, and delete use the exact action handles. The Key
  column shows only a short prefix of the hashed website-Key identity. MainWindow's
  redundant raw signal/slot and all legacy public raw management/list/test signals
  and methods were removed.
- The complete desktop target builds and the focused companion/API/Profile/Tool/
  product-scope set passes `16/16`. A trusted-origin fake transport covers two-read
  handle rotation, stale-handle zero-network rejection, percent-encoded raw-ID path
  segments, positive update/create, strict response-code typing, create credential
  staging, one-mutation-at-a-time admission, and auth-change `outcome-unknown` with
  late completion inert. Contract, unverified-account preflight, redacted-inventory
  credential rebind, and static raw-boundary regressions also pass. The dedicated
  offscreen Qt dialog fixture covers PlainText/literal rendering, safe table roles,
  exact-owned invalid projection failure, wrong-correlation inertia, synchronous
  rejection, refresh clearing, and late pre-refresh model results. A target-private
  one-shot SecureStorage removal failure proves a successful remote delete reports
  local cleanup false while retaining a recoverable credential for later explicit
  cleanup. A real transport fixture now holds an exact Key-test `GET /v1/models`,
  commits a different authenticated website configuration and SecureStorage
  credential, requires one fixed retirement failure, proves the released old reply
  inert, rotates the management/test handle, and proves a new test uses the refreshed
  credential and model projection. ApiClient now retires pending models when a new
  configuration projection replaces the current one. Strict OpenSpec validation and
  `git diff --check` pass. OpenSpec `0.2` remains unchecked because authenticated
  revisioned cache/model integration remains open. Agent/Codex stays read-only.

## SecureStorage Persistence Truthfulness (2026-08-23)

- Windows `saveEncrypted` now performs `setValue`, `sync`, and `NoError` validation
  before caching or reporting success. Windows `remove` likewise requires a synced
  `NoError` result.
- macOS Keychain, Windows QSettings, and Linux Secret Service deletion now share the
  same ordering: backend confirmation first, then one memory-cache removal. Backend
  failure returns false while preserving the cached credential, so callers cannot
  misreport local cleanup as complete.
- Product scope policy locks the production source ordering without adding a runtime
  test hook. Application and focused targets build and the policy test passes.
- This is a prerequisite for a SecureStorage-rooted encrypted backup key. Current
  ToolManager v1 backups still copy credential-bearing CLI files in plaintext, so
  OpenSpec `0.2` and `0.3` remain unchecked. Agent/Codex remains read-only.

## Encrypted Configuration Backup Store Foundation (2026-08-23)

- `ConfigurationBackupStore` owns a strict canonical v2 encrypted record. Its exact
  manifest contains only backup/tool/time/count/cipher metadata, nonce, tag, and
  ciphertext; config paths, HOME paths, plaintext content, and key references are
  absent. AES-256-GCM associated data length-frames the tool, strict backup ID,
  canonical UTC time, and file count.
- The inner payload uses contiguous slots with existence, canonical Base64, byte
  count, SHA-256, and 4 MiB/file, 8 MiB aggregate, 16-file limits. The store returns
  no content until the complete manifest and every slot authenticate and validate.
  Creation uses atomic publication, private Unix permissions, bounded real-file
  reread, exact byte comparison, decryption, and payload revalidation.
- Exact legacy v1 ToolManager directories can migrate through an authenticated
  `manifest.v2.pending` record. Unknown entries and ambiguous/missing evidence fail
  closed; partial payload deletion and final-manifest/pending cleanup interruptions
  are resumable without trusting caller-selected paths.
- The key-provider interface is injectable. The warnings-denied dedicated fake
  provider fixture covers path/credential absence, round trip, wrong key/tool/ID,
  outer and authenticated tampering, bounds, normal/interrupted legacy migration,
  unknown evidence preservation, and backend failure with repeated green runs.
- No production key provider, ToolManager/MainWindow caller, prune orchestrator, or
  CLI restore mutation uses this module yet. Existing product backups therefore remain
  plaintext and OpenSpec `0.2`/`0.3` stay unchecked. Agent/Codex remains read-only.

## Configuration Backup Inventory And Verified Removal (2026-08-24)

- Store-level inventory now distinguishes Empty, Ready, Unavailable, and Invalid.
  It scans the complete root under the backup lock, accepts only the lock plus at
  most 64 strict backup directories, migrates exact legacy records, and bounded-reads,
  authenticates, and fully validates every v2 record before returning Ready.
- Safe inventory rows contain only backup ID, tool, canonical creation time, file
  count, and a domain-separated manifest identity. They are deterministically ordered
  by creation time descending and ID ascending; decrypted payload copies are cleansed.
  Unknown entries, symlinks, tamper, overflow, or migration ambiguity are Invalid and
  preserved, while lock/key/backend failures are Unavailable rather than Empty.
- Verified removal requires the exact inventory identity, rescans and authenticates
  before quarantine, then authenticates the moved evidence again. Identity replacement
  and tamper are retained; a failed removal restores the directory when possible or
  preserves encrypted manifest evidence without claiming deletion.
- The warnings-denied dedicated fixture passes repeated runs across all inventory
  states, deterministic ordering, lock/key failure, corrupt/unknown/symlink evidence,
  inventory migration, wrong/replaced identities, tamper preservation, and exact
  removal. At this foundation stage, production ToolManager, MainWindow,
  SecureStorage key provider, safety rollback, and prune integration were absent;
  the production integration section below supersedes that status. Agent/Codex
  remains read-only.

## ToolManager Encrypted Backup Integration (2026-08-24)

- Production ToolManager uses the encrypted store for every supported configuration
  target. Its strict per-tool SecureStorage key provider accepts only canonical
  32-byte Base64 keys, generates with `RAND_bytes` only from Empty/exact legacy
  creation paths, requires save plus exact readback, and cleans generated key material.
- Backup creation consumes/migrates the locked inventory first, blocks Invalid or
  Unavailable state, stable-reads each managed file twice under 4/8 MiB bounds,
  publishes and rereads/authenticates v2, and compares the complete snapshot. Direct
  and gateway configuration recapture current files after backup and perform no CLI
  writes on drift.
- Manual restore authenticates the complete target before creating and rereading an
  encrypted safety snapshot. Safety failure or current-file drift is zero-write.
  Apply prevalidates every target before mutation; failure uses the verified in-memory
  safety snapshot, reports restored versus current-state-uncertain truthfully, and
  cleanses plaintext buffers.
- Prune consumes only Ready inventory and exact manifest identities through verified
  removal. Cleanup failure becomes `lastWarning` after a successful main operation
  and preserves evidence. MainWindow renders Empty/Ready/Unavailable/Invalid,
  gates restore, includes OpenCode, refreshes configuration watchers, and reports
  prune warnings separately.
- The complete desktop build and focused `4/4` backup/ToolManager/product tests pass.
  Isolated fake-provider evidence covers encrypted Codex two-file round trip,
  safety-key and invalid-inventory zero-write behavior, and absence of all four
  supported tool credentials plus HOME from the complete backup tree.
- This does not claim forensic erasure of blocks previously occupied by legacy v1
  files or clean Windows runtime evidence. OpenSpec `0.2` remains open for the
  authenticated revisioned website configuration/model cache. `0.3` remains open for
  complete preview/confirmation, active-profile compensation, broader rollback
  injection, and signed cross-platform one-click evidence. Agent/Codex remains
  read-only.

## SecureStorage Typed Fresh Read (2026-08-24)

- `SecureStorageReadResult` now provides Found, Missing, Unavailable, and Invalid.
  `loadEncryptedFresh` validates the storage key, bypasses process cache, and performs
  a fresh platform read. Compatibility `loadEncrypted` remains cache-first and adds
  only Found values to the cache.
- Windows requires QSettings sync/status, distinguishes definite absence, validates
  canonical Base64, rejects oversized DPAPI input/output, distinguishes decrypt
  failure from valid empty plaintext, requires strict UTF-8 round trip, and cleanses
  plaintext bytes. macOS distinguishes Keychain item-not-found from backend/
  interaction failure and invalid success bytes. Linux treats only a normal empty
  exit-1 lookup as Missing and classifies executable/start/timeout/crash/stderr/
  backend failures as Unavailable.
- A dedicated no-credential-access policy target locks the platform classification,
  cache bypass, strict decode/decrypt order, Found-only compatibility caching, and
  absence of a test hook. Application and adjacent targets build; focused policy
  tests pass repeated runs.
- This prevents companion cache bootstrap from generating a new HMAC authority when
  the secure backend is merely unavailable or corrupt. It does not itself implement
  the Prepared/Committed cache transaction, provide server signatures, or detect a
  consistent rollback/deletion of every secure and QSettings record. Agent/Codex
  remains read-only and OpenSpec `0.2` stays unchecked.

## Live Companion Authority Retirement (2026-08-24)

- ApiClient centralizes terminal current-generation website configuration failure.
  It clears live configuration, usage sources, Key management/group capability state,
  and website-only model state; retires pending model/usage/Key requests and active
  Chat/image/presentation bindings; then emits exactly one fixed configuration
  failure. Stale request/auth generations remain inert before this transition.
- Website Key success responses now require strict numeric code, object data, array
  items, and a bounded integral total consistent with the accumulated Key count.
  Redirect/origin/URL/content-type/pagination/projection/broker failures use the same
  retirement path.
- Ordinary website model responses may enter an in-memory map bound to the current
  configuration SHA. Local Profile and management Key-test model responses never do;
  rotation and failure clear the map. It is not persistent cache or selection
  authority.
- ConnectWizard clears website state but preserves its explicit local Profile path and
  rechecks live selection at save. Models, Chat, Image, Usage, and API-Key management
  clear website candidates/pending state and disable query/send/Skills/generate/
  provider actions on failure. Action entry points repeat live candidate binding.
- The fake HTTPS fixture holds mutation, usage, model, Chat, image, and presentation
  requests, forces current-generation config failure, and proves complete retirement,
  zero later network dispatch, inert late replies, local-Profile model exclusion, and
  stale old-generation failure isolation. The focused companion/API/product set
  passes `9/9` and application targets build.
- This live-authority boundary remains mandatory when the offline cache arrives:
  cached Fresh/Stale/Expired data is display-only and can never restore these
  operation bindings. Agent/Codex remains read-only and OpenSpec `0.2` stays unchecked.

## Authenticated Revisioned Companion Cache Core (2026-08-24)

- `CompanionConfigurationCache` defines canonical payload/envelope/configuration and
  HMAC domains at v0.2. One SecureStorage authority envelope holds the canonical
  32-byte HMAC key, Committed anchor, optional Prepared reservation, and highest
  reserved revision; typed fresh read/write outcomes distinguish definite,
  unavailable, invalid, and outcome-unknown states.
- A caller-supplied absolute external lock path and 30-second PID/host stale recovery
  guard A/B QSettings slots. Lock-time sync plus complete account namespace scan
  rejects unknown/nested slots. Prepared binds the target's exact preimage or absence
  and candidate; exact preimage aborts, exact candidate finalizes, third state fails,
  and no unanchored slot can bootstrap authority. Revisions reserved by a durable
  Prepared state are never reused.
- Committed state requires exact account/slot/revision/payload/envelope/high-water,
  constant-time HMAC, and the authenticated predecessor after revision one. Slot,
  anchor, previous-record, same-revision, namespace, account, SHA, HMAC, and rollback
  drift fail closed.
- Configuration is Fresh for 24 hours, status-only Stale for seven more days, then
  Expired. Website model rows are exact account/config-observation/Key/platform bound,
  valid for at most six hours and never beyond config. New config observation clears
  models; exact replay is idempotent; local Profile models are rejected.
- Cached bytes/views contain no credential/raw ID/handle/provider body and every
  configuration/apply/model-selection authority flag is false. Secret-shaped model
  and display fields are rejected. Legacy v1 remains LegacyUnverified and is never
  re-signed; successful v2 cleanup failure is a warning without fallback.
- Fixed canonical/HMAC vectors and the warnings-denied fixture cover transaction,
  crash-recovery, rollback, clock, namespace, predecessor, cross-account, typed
  backend, model/config binding, legacy, secret, and authority-negative matrices.
  Cache plus config/model tests pass five repeated runs.
- Production now uses a strict
  `companion/configuration-cache-authority/v1/<64-lower-hex>` SecureStorage adapter,
  fresh typed reads, conservative unknown write outcomes, and a stable private
  AppDataLocation lock path. One worker thread owns QSettings, the adapter, and the
  cache so lock and secure-backend waits never run in MainWindow's UI thread.
- ApiClient emits one dedicated website-model observation only after an ordinary
  website model request passes the complete live binding. Management Key tests and
  local Profile model queries emit no cache observation. MainWindow commits live
  configuration immediately after rendering online success, treats persistence
  failure as independent degradation, removes production v1 save/load calls, and
  loads only the current verified account's v2 status after live failure.
- MainWindow distinguishes Fresh, Stale, Expired, Empty, LegacyUnverified,
  Unavailable, Invalid, OutcomeUnknown, and RecoveryRequired. None restores
  ApiClient configuration, credential handles, Profile save, Chat/Skills, provider
  requests, or model-selection authority.
- The worker now returns the complete evaluated View with its account and evaluation
  time. MainWindow accepts only the current generation and exact verified account,
  rebuilds it through `CompanionConfigurationCachePresentationAdapter`, clears it on
  token/account change, filters expired model rows, and monotonically downgrades
  Fresh to Stale/Expired before constructing a dialog.
- Presentation DTOs contain only the safe hashed account identity, cache
  state/provenance/revision/times/SHA metadata, safe Key display/group/platform/state,
  and model IDs plus observation times. They
  have no credential handle, authority, operation, raw ID, provider body, or config
  write fields. Forged authority, account/time/content drift, secret-shaped values,
  expired models, and invalid state/content matrices fail closed.
- The presentation adapter owns monotonic display aging. MainWindow ages immediately
  before construction, and each open dialog schedules the next model/configuration
  transition. A model deadline removes only model rows, the configuration Fresh
  deadline clears models and produces Key-only Stale, and the final Stale deadline
  removes every cached row. A live dialog only ages its fallback snapshot and is
  never replaced by the cache timer.
- ConnectWizard, Models, and Chat use cached item roles starting above
  `Qt::UserRole + 31`; the existing live roles remain unset. All three repeat cache
  rejection at their action entry points. Cached data cannot resolve a credential,
  query models, test a connection, save a Profile, send Chat, run image/PPT Skills,
  select a model for execution, or write tool configuration. Models retains safe
  cached search/filter/copy and exposes explicit read-only provenance; Stale has no
  models and Expired/error states have no rows. Local Profile behavior remains a
  separate ConnectWizard path.
- The complete default build passes after the final timer repair. At the immediately
  preceding full-suite checkpoint, unfiltered local CTest passed `48/48` in 1192.83s
  and the embedded complete Rust workspace passed in 1122.91s. After the timer-only
  repair, the current focused companion, backup, profile, ToolManager, API, dialog,
  extension, policy, and cache set passes `20/20` in 10.64s.
  Adapter/worker coverage rejects adjacent SecureStorage scopes, invalid authority
  bytes, relative/symlink lock roots, cross-account reads, and state collapsing.
- The local HMAC is not a server signature and cannot detect consistent rollback or
  deletion of all SecureStorage and QSettings evidence. OpenSpec `0.2` remains
  unchecked pending native Windows production evidence. Agent/Codex remains
  read-only.

## Companion Cache CI Follow-Up And ConPTY Fixture Repair (2026-08-24)

- Commit `96ff593` triggered macOS run `32662755791` and Windows run
  `32662755813`. Windows stopped in the Rust test step with exactly
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi` failing;
  the library result was `925 passed, 1 failed` in 155.64s. Qt, cache-dialog,
  installer, and publication steps did not run, so the run grants no native cache or
  package evidence. macOS built successfully but its unfiltered CTest returned exit
  8 without a public failed-test identity.
- The Windows failure is in a test-only ConPTY harness whose source had not changed
  since the prior DSR tracker repair. The fixture allowed three separate 60-second
  output waits and one 60-second exit wait behind a 120-second outer watchdog.
  More importantly, it wrote a completion command immediately after Ctrl+C without
  proving that the shell had returned to its input prompt; the control transition
  could discard that input. Its typed finish command also contained the complete ANSI
  marker, so ConPTY/PSReadLine input echo could satisfy the marker assertion, and the
  final ANSI bytes could race shell-exit observation.
- The repaired fixture installs a split-literal custom prompt, records the exact
  pre-interrupt output checkpoint, sends Ctrl+C, and requires a new prompt strictly
  after that checkpoint before sending more input. It emits the ANSI marker from
  split shell variables, verifies the real adjacent non-reset-SGR/marker/reset output
  while the shell is still running, then sends a separate `exit 23`. One 120-second
  absolute workflow deadline prevents per-phase timeout reset; the 150-second outer
  watchdog leaves cleanup margin. The full-lifecycle DSR tracker, reader-error,
  exit-code, Job Object empty, shutdown, and directory cleanup checks remain.
- Platform-neutral test support now validates checkpoint slicing, old-output
  exclusion, output-range drift, and that cmd/PowerShell prompt and ANSI input cannot
  echo either complete marker. The helper also proves an expired deadline wins over
  marker/exit success. DSR reply failure re-reads the authoritative snapshot so a
  shell exit between snapshot and reply is not a false failure; other reply failures
  remain fixed-code failures. The focused helper run passes `9/9`; Rust formatting
  and strict workspace/all-target Clippy pass. Production ConPTY code, user terminal
  permissions, Agent/Codex read-only policy, and all execution authority are
  unchanged.
- The new cache TTL fixture replaces its fragile 40/80/160ms schedule with separated
  1.5/3.5/5.5-second deadlines. The affected cache/product/macOS-policy set passes
  after the final repair; the current cache/product/macOS/Windows-policy set passes
  `4/4` in 7.06s, and the TTL dialog test passes three consecutive runs in
  5.85-5.86s. The macOS workflow now publishes only at most 50 bounded
  grammar-checked test names, capped after encoding at 2,000 characters, while
  preserving the original unfiltered CTest command and exit code; raw failed-test
  logs are never annotated.
- Windows Rust diagnostics now additionally admit only fixed
  `CONPTY_INTERRUPT_*` or `CONPTY_DSR_*` stage codes alongside the existing bounded
  test identities/source locations. PTY output, command text, paths, and dynamic
  suffixes remain excluded; the packaging policy requires the fixed-code filter.
- macOS-to-MSVC Cargo check again stops in native Tree-sitter/SQLite C compilation
  because this host lacks the Windows SDK `stdlib.h`/`stdio.h`; it does not reach or
  validate the Windows Rust module. A fresh native Windows run and a new macOS run
  are required before OpenSpec `0.2`, `14.2`, `14.9`, installer, or release evidence
  can advance.

## Post-Teardown ConPTY Job Verification (2026-08-24)

- Follow-up macOS run `32665422212` passed. Windows run `32665422225` failed only
  with fixed code `CONPTY_INTERRUPT_JOB_NOT_EMPTY`; prompt recovery, real ANSI
  output, reader validation, and exact `exit 23` had already succeeded.
- The fixture checked Job emptiness before `remove_user` performed the complete
  pseudoconsole teardown. The exited shell could therefore leave the console host in
  the Job until the master handle closed. This was a test-evidence ordering defect,
  not permission to remove the process-tree assertion or extend its timeout.
- A test-only duplicated Job handle now survives removal of the terminal registry
  entry. The fixture validates exit, duplicates the handle, calls the normal
  `remove_user` teardown, and then requires the Job to become empty within five
  seconds. Duplicate/teardown failures retain fixed `CONPTY_INTERRUPT_*` codes.
  Production ConPTY, user-terminal permissions, and Agent/Codex authority are
  unchanged.
- Rust formatting, the `9/9` platform-neutral terminal-support target, strict
  workspace/all-target Clippy, and both workflow policy fixtures pass locally.
  Native Windows execution remains required before OpenSpec `0.2`, `14.2`, `14.9`,
  installer, or release evidence can advance.

## Explicit ConPTY Job Teardown (2026-08-24)

- macOS run `32666817150` passed. Windows run `32666817138` failed only
  `CONPTY_INTERRUPT_JOB_NOT_EMPTY`, with `928` other library tests passing; no later
  Qt/cache/installer/package evidence exists from that run.
- The duplicated verifier handle meant the production-owned Job handle was not the
  last handle, so `KILL_ON_JOB_CLOSE` could not fire. Teardown had also skipped
  `TerminateJobObject` after observing the shell exit, leaving the console host or a
  descendant in the Job and making the new assertion deterministically fail.
- Production teardown now records any available shell exit code and always
  terminates the Job before the existing bounded ConPTY drain/master-close/reader
  cleanup. Cleanup no longer depends on last-handle close semantics. The duplicated
  post-remove Job-empty assertion and five-second bound remain unchanged.
- This changes process-tree cleanup only. User-terminal permissions and Agent/Codex
  read-only authority are unchanged. Rust formatting, `9/9` platform-neutral helpers,
  strict workspace/all-target Clippy, both workflow policies, strict OpenSpec, and
  diff checks pass locally. A fresh native Windows run remains required before
  OpenSpec `0.2`, `14.2`, `14.9`, installer, or release evidence advances.

## One-Click Activation Ordering (2026-08-24)

- Single-card activation and `全工具一键切换` now use one Profile-index queue.
  The bulk path no longer writes configuration or active state directly, and always
  presents one combined review covering every selected tool.
- Configuration preview lists exact managed files plus any required Node.js/npm and
  CLI install or repair. Missing/damaged CLI state is a warning and therefore cannot
  be hidden by the saved single-profile skip-confirm preference.
- Each queue entry is revalidated, the CLI is installed and independently verified
  first, ToolManager performs its existing encrypted backup/write/readback/rollback,
  and only then does ProfileManager commit that tool's active Profile. Install,
  post-install verification, running-tool repair, Profile drift, or configuration
  failure stops the remaining queue without committing the failing Profile.
- Product-scope policy locks the install -> configure -> active-state order, combined
  bulk preview, and absence of direct bulk writes. The application and policy targets
  build; focused backup, ToolManager gateway, and product-scope tests pass `3/3`.
- OpenSpec `0.3` remains unchecked. Editing an already-active Profile still needs an
  exact pre-edit credential/metadata compensation boundary, and gateway control
  messages still need acknowledgement plus in-memory Profile compensation. Broader
  failure injection and clean macOS/Windows evidence also remain required.
- Claude, Gemini, and OpenCode remain configuration-only targets. Codex remains the
  sole integrated programming runtime; Agent/Codex authority is unchanged.

## Active Profile Immutable Replacement (2026-08-24)

- Editing an active Profile now creates a separate candidate with a fresh
  UUID-derived SecureStorage slot. The original Profile and active index remain
  unchanged while the candidate passes preview, environment installation, encrypted
  backup, configuration write, and readback verification.
- Activation queue entries bind the Profile UUID, complete safe metadata, full
  domain-separated credential identity, and reviewed gateway mode. They resolve by
  UUID and recompute identity after asynchronous installation, so index movement or
  credential/model/source/type/name/gateway drift fails before configuration. Active
  edits cannot change tool type; cross-tool changes require a new Profile.
- Preview cancellation or activation failure removes only the inactive candidate.
  Success activates the candidate before removing the original. Profile removal now
  shifts `last_activated` together with every per-tool active index, so removing the
  earlier original preserves the replacement's effective index.
- The environment review is informational for installation/repair and no longer
  launches a second installer. The shared activation queue remains the only owner of
  Node/CLI installation during Profile activation.
- The application and focused targets build; Profile activation, encrypted backup,
  ToolManager gateway, and product-scope CTests pass `4/4`. Product policy locks the
  immutable-replacement path, discard/finalize order, and single installer owner.
- OpenSpec `0.3` remains unchecked. Active-index QSettings commit still lacks typed
  sync/readback/outcome handling, inactive-candidate cleanup is not crash-journaled,
  and gateway control messages still lack correlated acknowledgement plus exact
  in-memory/tool compensation. Agent/Codex authority is unchanged.

## Profile Commit And Tool Rollback Receipt (2026-08-24)

- New Profile publication now requires QSettings sync/status plus exact field
  readback and, when present, a fresh exact SecureStorage credential readback. A
  failed verification emits no Profile publication and returns no candidate index.
- Active selection now returns a result and emits its change only after QSettings
  sync/status plus independent exact readback of the per-tool active index and
  `last_activated`. Failure attempts to restore both prior values and never reports
  the candidate active.
- ToolManager successful direct/gateway file apply now returns the exact verified
  encrypted preimage backup ID. The activation workflow retains it until active
  Profile commit; a commit failure invokes authenticated `restoreBackup` before the
  candidate is discarded and reports restored versus state-unknown truthfully.
- The application builds and the focused Profile activation, encrypted backup,
  ToolManager gateway, and product-scope tests pass `4/4`. Product policy locks the
  checked active commit and backup compensation path.
- OpenSpec `0.3` remains unchecked. Profile removal and credential cleanup still need
  a typed verified outcome, replacement state needs a durable crash journal, and the
  Node gateway still lacks correlated prepare/commit/abort acknowledgement and exact
  in-memory compensation. Agent/Codex authority is unchanged.

## Gateway Profile Transaction And Compensation (2026-08-24)

- The Node gateway now accepts strict `aegisy-gateway-control/0.1` Profile mutation
  messages. Exact request/transaction/tool/operation and per-tool revision CAS bind
  prepare-configure, prepare-remove, commit, and abort. Prepare changes only a pending
  candidate; commit alone changes the routing Map. Results contain no credential,
  upstream, fragment, or credential hash and fix `credential_included:false`.
- GatewayManager binds every callback/result to the current QProcess pointer and
  generation, validates the exact result field set and pending identities, bounds
  stdout and wait time, kills the generation on write/timeout/exit/protocol
  uncertainty, ignores unmatched results, and publishes only the fixed
  `gateway-runtime-stderr` class for child stderr.
- Activation order is gateway prepare -> verified encrypted-backup/file apply ->
  gateway commit -> verified Profile active commit. File failure aborts the pending
  gateway candidate; gateway commit failure restores local files; Profile commit
  failure restores the still-active prior gateway Profile (or confirmed removal)
  before restoring the files. Startup no longer reports all Profiles restored unless
  every gateway acknowledgement and file apply succeeds.
- The application and policy targets build, JavaScript syntax and the real Node
  gateway security plus streaming/backpressure/disconnect integrations pass, and the
  focused companion/gateway set passes `6/6`. The streaming fixture now uses the
  strict control protocol and always cleans its child/server on failure.
- OpenSpec `0.3` remains unchecked. Profile removal/credential cleanup still needs a
  typed verified result, replacement state needs durable crash recovery, Qt needs a
  complete fake-process outcome-unknown matrix, and clean macOS/Windows one-click
  evidence remains required. Agent/Codex authority is unchanged.

## Typed Profile Removal And Credential Cleanup (2026-08-24)

- Profile removal returns Removed, RemovedCredentialCleanupPending, Unchanged, or
  OutcomeUnknown. It syncs and independently verifies count, all active indices,
  `last_activated`, and removed-UUID absence before publishing metadata removal.
- Credential cleanup is a separate fresh-read/remove/fresh-Missing proof. UUID-based
  retry can target the exact derived SecureStorage scope after metadata is gone.
  A deleted Profile with backend cleanup failure is therefore reported as cleanup
  pending rather than fully removed.
- MainWindow ordinary deletion and replacement discard/finalize clear transaction
  ownership only after `metadataRemoved()`. OutcomeUnknown retains the pending IDs;
  credential-cleanup pending emits a distinct warning.
- The application builds and the gateway stream/security, encrypted backup, Profile,
  ToolManager, and product-policy set passes `6/6`. Strict OpenSpec and diff checks
  pass.
- OpenSpec `0.3` remains unchecked. Non-active `updateProfile` still needs complete
  write verification/compensation, cross-resource activation needs a durable recovery
  journal and full Qt gateway race injection, and native macOS/Windows one-click
  evidence remains required. Agent/Codex authority is unchanged.

## Verified Non-Active Profile Update (2026-08-24)

- Non-active `updateProfile` fresh-reads the exact old credential state and snapshots
  every old QSettings field plus the prior tool active index before mutation.
- New credentials and every updated metadata/source field require sync and exact
  independent readback; credential removal requires backend deletion plus fresh
  Missing. Signals emit only after the complete new state is verified.
- Any failed proof restores all prior fields/index and the exact old credential or
  old Missing state. Verified compensation reports the old Profile restored; a
  compensation that cannot be proved reports outcome unknown. Active Profiles still
  use immutable replacement and never take this in-place path.
- SecureStorage-backed replacement/removal tests plus the gateway stream/security,
  encrypted backup, Profile, ToolManager, and product-policy set pass `6/6`; the
  application builds and strict OpenSpec/diff checks pass.
- OpenSpec `0.3` remains unchecked for a durable cross-resource activation recovery
  journal, the complete injected failure/crash/outcome-unknown matrix, and clean
  native macOS/Windows evidence. Agent/Codex authority is unchanged.

## Gateway Control Result Contract (2026-08-24)

- `GatewayControlContract` is the single pure validator for gateway control results.
  It enforces the exact ten-field schema, request/transaction/operation/tool binding,
  false credential inclusion, and a JSON-safe integral revision.
- Accepted results require the operation-specific success outcome and empty error;
  rejected results require `rejected` plus a bounded lowercase/digit/hyphen fixed
  code. Mixed success/error, dynamic errors, wrong outcomes, unknown fields, and
  binding/revision drift are Invalid and fail the owning gateway generation closed.
- Its dedicated matrix covers valid prepare/reject, four cross-bindings, unknown
  fields, credential flag, fractional revision, false success, wrong outcome, and
  dynamic error. Product policy requires the production consumer and CTest.
- The application builds and the gateway stream/security, encrypted backup, Profile,
  ToolManager, product-policy, and contract set passes `7/7`; strict OpenSpec and
  diff checks pass.
- OpenSpec `0.3` remains unchecked for process-level timeout/exit injection, the
  durable cross-resource activation journal, and clean native one-click evidence.
  Agent/Codex authority is unchanged.

## Prepared Tool Configuration Receipt (2026-08-24)

- ToolManager exposes ordered prepare/apply/rollback/finalize configuration methods;
  existing direct/gateway configure wrappers use the same implementation.
- Prepare authenticates the encrypted preimage and inventory manifest, rechecks
  source stability, and returns a secret-free tool/backup/manifest/source-files/
  candidate-files/mode receipt before any target write. Candidate generation uses
  the existing four configuration writers in an in-memory capture mode and cleanses
  their transient credential-bearing bytes. Apply reauthenticates the receipt,
  requires the current files to match, regenerates the candidate, writes and verifies
  only the reviewed target, and requires the final identity to equal the candidate.
- Rollback requires the exact backup/manifest/source/candidate graph and refuses
  third-state drift before restoring and recapturing the source. An already restored
  source is idempotent. Finalize alone prunes, keeping the receipt usable through
  caller commit/compensation.
- The ToolManager fixture proves tampered-receipt zero-write rejection, valid apply,
  final identity, exact gateway-preimage rollback, and finalize. The application
  builds and focused backup/ToolManager/product tests pass `3/3`.
- MainWindow does not yet persist the receipt before apply. Durable activation journal
  and crash-stage recovery plus clean native evidence remain OpenSpec `0.3` gates.
  Agent/Codex authority is unchanged.

## Activation Recovery Journal Contract (2026-08-24)

- Strict `aegisy-companion-activation-journal/0.2` contains only transaction and
  original/candidate Profile identities, candidate digest, tool/mode, exact
  ToolManager backup/manifest/source/candidate/applied receipt identities, monotonic
  stage, and a domain-separated record identity. No credential, model, upstream,
  provider body, path, or raw website ID is stored.
- Synchronized QSettings keeps exact record bytes plus identity and uses readback and
  expected-identity CAS. Empty requires both absent; partial deletion, malformed
  types/fields, tamper, identity drift, stale CAS, illegal transition, or receipt
  drift is Invalid.
- Gateway history is Prepared -> FilesApplied -> GatewayCommitted -> ProfileCommitted;
  direct history skips the gateway stage. The applied files identity is absent only
  while Prepared and all preimage fields remain immutable.
- The dedicated lifecycle/CAS/tamper/partial-deletion matrix and product policy pass
  `2/2`; the application builds and the pure journal target avoids ToolManager QObject
  linkage through a separate `configuration_apply_receipt.h` data contract.
- This section records the initial contract foundation. The runtime integration below
  supersedes its initial unconsumed status and records the remaining recovery gates.
  Agent/Codex authority is unchanged.

## Activation Journal Runtime Integration (2026-08-24)

- MainWindow starts/rehydrates the old gateway state before receipt preparation,
  persists Prepared before any target write, then advances only after verified file,
  exact gateway, and synced Profile commits. Every clear uses expected-identity CAS.
- Known failures require verified gateway abort/old-state restore, receipt rollback,
  and journal clear as applicable. Any uncertain apply, abort, gateway commit,
  Profile compensation, stage advance, or clear retains the journal, keeps candidates,
  and enters RecoveryRequired.
- Startup clears unapplied Prepared, rolls back exact direct FilesApplied, or finishes
  cleanup when the candidate is verifiably active. Gateway FilesApplied or
  GatewayCommitted with a non-active candidate remains RecoveryRequired because the
  prior Node process may have committed before stage persistence. RecoveryRequired
  gates single/bulk activation and gateway auto-rehydration writes.
- The ToolManager fixture simulates Prepared -> apply -> FilesApplied -> restart ->
  receipt rollback -> clear/finalize. The application and focused gateway/security/
  backup/Profile/contract/journal/product set passes `8/8`; strict OpenSpec and diff
  checks pass.
- Journal hashing is local integrity evidence, not authenticated or anti-deletion:
  consistent recomputation or deletion of both QSettings values is outside the
  boundary. Deterministic Qt timeout/exit/late-generation injection is now covered
  below. An explicit restart-safe recovery action for ambiguous gateway stages and
  clean native evidence remain OpenSpec `0.3` gates. Agent/Codex authority is
  unchanged.

## Gateway Process Fault-Injection Matrix (2026-08-24)

- A target-private `GatewayManager` process test relaunches its own executable as a
  controlled gateway child. It deterministically covers timeout after reading the
  complete request, exit before acknowledgement, malformed result, valid prepare/
  commit, and late prior-generation `ready`/fatal events. Test executable, local
  token, and shortened timeout injection compile only in that test target; the
  product continues to resolve Node, use the embedded script and SecureStorage token,
  and enforce the production five-second deadline.
- Gateway failure now removes the current process pointer, advances the generation,
  and clears every expected request/transaction/operation/tool binding before kill.
  The retired child is given a bounded one-second reap. Its queued stdout, ready,
  result, fatal, and finished callbacks therefore cannot revive running state, update
  revision, or complete a request for a later process generation.
- Product policy fixes the registered CTest, target-private macro count, and retire-
  before-kill/reap order. The application builds and the real gateway stream/security,
  ToolManager gateway, control contract, process matrix, activation journal, and
  product-policy set passes `7/7`. The complete desktop gate passes `58/58` in 213.14
  seconds; strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.3` remains unchecked. The activation journal is not authenticated or
  anti-deletion, a reviewed restart-safe action for ambiguous gateway recovery does
  not yet exist, and complete native one-click evidence remains open. Agent/Codex
  authority is unchanged.

## Predeclared Configuration Candidate Identity (2026-08-24)

- `prepareConfigurationApply` now consumes the transient credential/model only to
  run the existing Claude, Codex, Gemini, or OpenCode writer in an in-memory capture
  mode. It requires the exact managed path set, overlays captured bytes on the
  authenticated preimage slots, computes one complete candidate files identity, and
  cleanses all candidate content. Planning creates no directory or config file.
- The receipt carries source and candidate identities before MainWindow creates the
  durable journal. Apply reauthenticates the backup/source, regenerates the candidate,
  then requires the observed final disk snapshot to equal it. Candidate mismatch
  invokes the verified preimage rollback path and reports uncertainty truthfully.
- Activation journal record `0.2` binds the candidate identity. A process crash after
  disk apply but before FilesApplied publication can no longer make Prepared look
  unapplied: the persisted receipt can roll back only an exact candidate, accepts an
  already restored source idempotently, and rejects third-state drift.
- The real ToolManager fixture proves candidate planning is zero-write, applied equals
  candidate, and a simulated Prepared-stage restart restores the prior gateway files.
  The application builds and the backup/Profile/ToolManager/journal/process/product
  focused set passes `6/6`. After the complete default graph rebuild, the unfiltered
  desktop aggregate passes `57/58` in 262.64 seconds; only unrelated
  `monaco_editor_render` reports fixed code `MONACO_SPLIT_LIFECYCLE`, and its exact
  rerun passes `1/1` in 4.56 seconds. Record this as 57 aggregate plus one exact
  rerun, not one uninterrupted 58/58 run. Strict OpenSpec validation and
  `git diff --check` pass.
- The journal remains local QSettings plus a recomputable hash. SecureStorage-backed
  A/B authority/high-water publication, durable gateway/Profile commit-requested
  intent, multi-resource recovery observation, explicit recovery UI, and native
  one-click evidence remain OpenSpec `0.3` gates. Agent/Codex remains read-only.

## Current Native Companion CI Evidence (2026-08-24)

- Companion HEAD `484beb2` macOS run `32673524896` passed policy, configure, build,
  unfiltered CTest, and feature-flag gates.
- Windows run `32673524863` passed the clean Unicode checkout, complete locked Rust
  workspace including the repaired ConPTY/Job fixture, strict Clippy, Release build,
  offline AAP package, dependency audit, Qt/OpenSSL installation, and Qt configure.
  It then failed `Build Windows Qt agent runtime`.
- Windows CTest, companion cache/dialog, one-click runtime, installer, package
  verification/upload, and publication were skipped. Public job metadata does not
  identify the specific bounded MSVC compile/link failure, so no Qt root cause is
  inferred.
- This advances Windows Rust/ConPTY evidence only. OpenSpec `0.2` and `0.3` remain
  unchecked until a current-source Windows run completes Qt build and unfiltered
  CTest; installer/package/signing remain separate release gates.

## Read-Only Extension Registry Contract (2026-08-24)

- Strict `extension-registry/0.1` unifies metadata for `codex-plugin`, `skill`, and
  `mcp`: bounded IDs/names/versions, source kind and source/content SHA-256 identities,
  trust, compatibility/fixed reason, scope, allowlisted requested capabilities, and
  installed/effective/update/recovery observations.
- It accepts at most 512 deterministically sorted records and 1 MiB. Duplicate
  identities/capabilities, secret/control metadata, unknown capability/enum, invalid
  identity/reason/scope, inconsistent compatibility, and unverified/incompatible
  effective enablement fail closed.
- Registry and every record fix install, enable, update, remove, and execution
  authority false. The contract performs no scan, CLI call, MCP start, Skill
  execution, or mutation.
- The three-kind/authority/duplicate/secret/capability/forged-enable/limit matrix and
  product policy pass `2/2`; the application builds.
- The strict Codex/Skill/MCP source adapters and unified read-only Extension Center
  described below now supersede this initial open status. OpenSpec `0.4` remains
  unchecked pending compatibility/provenance verification and recoverable user
  mutation workflows. No non-Codex programming runtime or Agent/Codex authority was
  added.

## Strict MCP Inventory And Save Guard (2026-08-24)

- `McpConfigurationInventory` bounds source bytes and servers, distinguishes
  Empty/Ready/Invalid/Unavailable, and rejects symlink/non-file/oversize/malformed
  sources, unsafe IDs, invalid/mixed server shapes, remote plaintext HTTP,
  shell-shaped commands, and args/env count/type/control overflow.
- Valid stdio and HTTPS/loopback-HTTP servers become unverified/compatibility-unknown
  hashed MCP registry records. Command, args, URL, env names/values, and credentials
  do not enter the registry.
- McpConfigDialog disables all mutation on Invalid/Unavailable instead of degrading
  malformed JSON to empty. Save requires exact current source identity, preserves
  unrelated root fields, writes atomically, and requires strict exact readback.
  Invalid and externally drifted fixtures prove byte-identical zero-write behavior;
  valid save preserves unrelated data.
- The application builds and registry/inventory/dialog/product tests pass `4/4`;
  strict OpenSpec and diff checks pass.
- MCP save still needs encrypted backup, reviewed preview, rollback/recovery, and a
  future lossless representation for formatting/order preservation. Duplicate decoded
  keys now fail closed through the shared strict validator. No MCP server starts and
  no Agent/AAP or non-Codex programming authority is added. OpenSpec `0.4` remains
  unchecked.

## Unified Read-Only Extension Sources And Center (2026-08-24)

- Codex plugin capture resolves an absolute canonical executable, verifies its path,
  size, and mtime before and after `plugin list --available --json`, uses a whitelist
  environment, bounds stdout/stderr independently, enforces a fixed timeout, and
  publishes only fixed issue codes. The strict bytes parser rejects ambiguous JSON,
  duplicate decoded keys/IDs, unsafe metadata, state contradictions, unknown fields,
  BOM, and size/count overflow; plugin paths never enter the registry or UI.
- The Skills adapter scans only one bounded root with count/depth/file/aggregate byte
  limits. It rejects symlinks, special files, traversal, case-fold ambiguity, drift,
  malformed/ambiguous manifests, unknown or duplicate fields/permissions/IDs, and
  invalid UTF-8 required content. Scripts are hashed as data and never run. Manifest
  trust/enabled/builtin claims cannot create effective authority.
- MCP, Skills, and Codex records are combined only after each source validates; a
  missing or invalid source contributes one fixed issue code while other valid sources
  remain readable. The aggregate `extension-registry/0.1` is rebuilt before display.
  All records remain unverified, compatibility-unknown, effectively disabled, and
  without install/enable/update/remove/execution authority.
- MainWindow now exposes `扩展与系统` as the primary extension destination and loads
  the unified inventory off the UI thread. The Extension Center provides search and
  kind filters plus source/trust/compatibility metadata. Its roles contain only safe
  IDs, enums, and hashes; cells are explicitly non-editable/non-checkable and Close is
  the only command.
- The application builds, the registry/MCP/Codex/Skills/coordinator/UI/product
  focused set passes `8/8`, and the complete desktop gate passes `57/57` in 349.73
  seconds. Strict OpenSpec validation and `git diff --check` pass. OpenSpec `0.4`
  remains unchecked for verified compatibility and provenance plus encrypted,
  previewed, reversible import/enable/disable/update/remove/recovery workflows. No
  Agent/AAP authority or non-Codex programming runtime was added.

## Authenticated Anti-Deletion Activation Journal (2026-08-24)

- The activation journal now carries an authority envelope in platform secure storage
  (`companion/activation-journal-authority/v1`, schema
  `aegisy-companion-activation-journal-authority/0.1`) holding a 32-byte `RAND_bytes`
  HMAC key, a monotonic `highest_serial`, one committed `{record_mac, serial}` anchor,
  and at most one reserved target. Record bytes stay in `QSettings` under record schema
  `0.3`, authenticated by HMAC-SHA256 over a domain-separated MAC domain plus the
  length-prefixed record. Comparison is `CRYPTO_memcmp`; the key is `OPENSSL_cleanse`d
  on every exit path and never leaves the process.
- `QSettings` alone can no longer forge or erase a transaction. Record bytes without an
  authority, a committed anchor whose record was deleted, an unauthenticated record, a
  serial that disagrees with the anchor, and a leftover legacy `0.2` identity key each
  produce a distinct `Invalid` verdict instead of degrading to `Empty`. Substituting
  the MAC key in the envelope also fails authentication, so local recomputation cannot
  mint a record.
- Mutations run reserve -> write record -> commit. Reserving records the candidate MAC
  and a strictly larger serial before any `QSettings` write, so a crash leaves a state
  the next load resolves deterministically: record bytes must equal exactly the
  preimage or the reserved candidate, and any third state is `Invalid`
  (`activation-journal-reserved-third-state`). An abandoned reservation never reclaims
  its serial. Authority writes are always re-read fresh, separating definite failure
  from `OutcomeUnknown`, and the adapter maps a locked or unreadable backend to
  `Unavailable` rather than first install.
- `MainWindow` owns the SecureStorage adapter, requires an authenticated read-back
  after `create` before touching any target file, and treats `Unavailable`,
  `OutcomeUnknown`, and `RecoveryRequired` as fail-closed recovery states with distinct
  operator messages. A create that fails while the journal is verifiably still `Empty`
  remains a clean abort.
- The journal fixture injects a fake authority store with scripted write outcomes and
  unavailable/invalid reads, covering round trip, CAS conflict, serial monotonicity
  across clear, deletion, orphaned records, key substitution, locked backend,
  interrupted-commit recovery, third-state refusal, unknown outcomes, legacy remnants,
  and a detached store. The application builds and the complete desktop gate passes
  `58/58` in 576.74 seconds; strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.3` remains unchecked: A/B authority slot publication, an explicit
  reviewed restart-safe action for ambiguous gateway `FilesApplied`/`GatewayCommitted`,
  gateway/profile commit-requested intent, and clean native one-click evidence are
  still open. Agent/Codex authority is unchanged and remains read-only.

## Commit-Requested Activation Intent (2026-08-24)

- Record schema `0.3` now carries two intent stages, `gateway-commit-requested` and
  `profile-commit-requested`, so the stage graph is Prepared -> FilesApplied ->
  GatewayCommitRequested -> GatewayCommitted -> ProfileCommitRequested ->
  ProfileCommitted for gateway mode and Prepared -> FilesApplied ->
  ProfileCommitRequested -> ProfileCommitted for direct mode. `advance` rejects any
  skip, so a commit can never be issued before its intent is durably journaled, and
  both gateway stages require `gatewayMode`.
- Recovery no longer has to guess whether a commit was issued. `FilesApplied` proves no
  commit was ever sent, so both gateway and direct transactions roll back
  deterministically to the authenticated preimage. Direct `ProfileCommitRequested` with
  an inactive candidate is also deterministic because the QSettings active index is the
  authority. Only `GatewayCommitRequested` and `GatewayCommitted` without a verifiably
  active candidate remain `RecoveryRequired`, each with its own message, because the
  prior Node process may have committed.
- `ProfileCommitRequested` is treated as commit-reached during startup recovery: when
  the candidate is verifiably active with a matching activation identity, the terminal
  state is already achieved and only cleanup remains.
- A `setActiveIndex` failure now checks `isActive` before compensating. If the candidate
  is in fact active, the transaction enters `RecoveryRequired` instead of rolling back
  over an already effective state.
- The journal fixture pins the skip rejections and both intent transitions in gateway
  and direct order; the ToolManager fixture additionally reapplies the candidate,
  journals the profile commit intent, proves it survives a simulated restart, and rolls
  back to the exact gateway preimage. Product policy locks the intent-before-commit
  ordering, the recovery branches, and the active-candidate compensation guard. The
  application builds and the complete desktop gate passes `58/58` in 231.98 seconds;
  strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.3` remains unchecked: A/B authority slot publication, an explicit reviewed
  restart-safe recovery action for the two remaining ambiguous gateway stages, and clean
  native one-click evidence are still open. Agent/Codex authority is unchanged and
  remains read-only.

## Reviewed Activation Recovery Action (2026-08-24)

- `RecoveryRequired` used to be a dead end: it correctly refused to guess, but it left no
  operator path out, so a single interrupted gateway commit permanently disabled every
  configuration switch on that machine. The recovery entry point now exists as an explicit
  reviewed action on the gateway page, visible and enabled only while
  `m_activationRecoveryRequired` holds.
- The action does not infer the past. Because `assets/local_gateway.js` keeps profiles only
  in process memory, a restarted gateway cannot be asked what it committed, so nothing about
  the previous process is knowable. Instead the action re-establishes a verified present
  state: after explicit `QMessageBox::question` confirmation it abandons the candidate, rolls
  configuration files back to the authenticated preimage through the journaled receipt, and
  re-drives the local gateway to hold exactly the currently active profile (or to hold none
  when the active profile is the candidate being abandoned).
- Every step must be verified before the transaction is cleared. A failed file rollback, a
  gateway that will not start, an unconfirmed `configureProfile`/`removeProfile`, an unknown
  candidate cleanup result, or a journal that cannot be cleared each abort recovery with a
  reason and leave `m_activationRecoveryRequired` set, so a partial recovery can never be
  mistaken for a completed one. The flag clears only after the journal is verifiably clear.
- The action deliberately never calls `setActiveIndex`. Choosing an active profile would be
  inferring an outcome; recovery only aligns the machine to the active profile that QSettings
  already authoritatively records.
- The ToolManager fixture drives the genuinely ambiguous path end to end: prepare and apply a
  gateway candidate, journal `FilesApplied` then `GatewayCommitRequested`, reload through a
  fresh journal to prove the stage survived a restart, then roll back and assert the preimage
  token is restored while the candidate credential is absent, clear the transaction to
  `Empty`, and retire the candidate backup. Product policy pins the confirmation, the
  load-before-act order, both gateway alignment calls, the absence of `setActiveIndex`, the
  visibility binding, and the signal connection. The application builds and the complete
  desktop gate passes `58/58` in 196.53 seconds; strict OpenSpec validation and
  `git diff --check` pass.
- OpenSpec `0.3` remains unchecked: A/B authority slot publication and clean native
  one-click evidence are still open. Agent/Codex authority is unchanged and remains
  read-only.

## A/B Activation Authority Slot Publication (2026-08-24)

- The authority envelope had a single-copy failure mode that no amount of journal logic
  could recover from: the HMAC key exists nowhere else, so one torn secure-storage write
  could leave the only copy unparseable and every future record permanently
  unauthenticatable. A/B publication downgrades that unrecoverable loss into "the last
  publication did not take effect", which the reserve/commit state machine already
  resolves deterministically.
- `CompanionActivationAuthoritySlots` is a pure unit with no I/O. Each slot holds an
  `aegisy-companion-activation-journal-authority-slot/0.1` frame carrying a monotonic
  generation, the base64 payload, and a SHA-256 digest over a domain-separated
  `(generation, payload)` preimage, so a stale digest cannot be paired with a new payload
  and neither field can be substituted independently.
- Publication always targets the peer of the selected slot, so the selected generation
  stays intact through the write. A corrupt slot with a valid peer selects the peer and
  publishes over the corrupt one. Two corrupt slots, a lone corrupt slot with no peer,
  and same-generation slots with differing payloads each report a distinct `Invalid`
  code rather than degrading to `Missing` — the same anti-deletion property the record
  layer already enforces. Any slot backend reporting `Unavailable` blocks inference,
  because a newer generation might simply be unreadable.
- Migration from the previous single scope is non-destructive: the old payload is adopted
  as generation 1, the first dual-slot publication writes generation 2 into slot A, and
  only after that write is confirmed is the legacy scope removed. A legacy remnant can
  never override an already-published slot.
- Generation exhaustion is reported instead of wrapping. `secure_storage` bounds moved to
  32 KiB to accommodate the frame overhead.
- A new `companion_activation_authority_slots` CTest target covers framing round trip,
  four digest/field substitutions, clean install, single slot, newest-wins in both slot
  orders, torn publication recovery, both corrupt cases, generation conflict versus
  identical frames, all three unavailable positions, invalid backends, legacy adoption
  and legacy-ignored, and exhaustion. Product policy pins the select-then-frame-then-write
  order, the absence of the in-place single-scope write, both slot scopes, and the
  anti-degradation codes. The complete desktop gate passes `59/59` in 202.86 seconds;
  strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.3` remains unchecked: clean native macOS/Windows one-click evidence is the
  last open item. Agent/Codex authority is unchanged and remains read-only.

## Native macOS One-Click Evidence (2026-08-24)

- Clean-clone macOS evidence for `0.3` is now recorded. A fresh `--depth 1` clone of the
  repository, configured and built out of tree with exactly the commands
  `.github/workflows/macos-build.yml` runs (`cmake --build build -j4` with no `--target`,
  then `ctest --test-dir build --no-tests=error --output-on-failure`), produced a zero
  exit status for both steps, zero compiler errors and zero warnings across the complete
  target graph, and `59/59` CTest passes in 267.84 seconds. The resulting
  `AegisyClient.app` has an executable binary, `Info.plist`, the embedded
  `Sparkle.framework`, the bundled `workbench` resources, and the `aegisy-agentd` sidecar
  in `Contents/MacOS`.
- One environment-only deviation is worth knowing for future runs: configuring a clean
  clone requires network egress for the pinned Sparkle 2.9.4 archive, and that download
  failed from this sandbox. Seeding `build/_deps/Sparkle-2.9.4.tar.xz` from the existing
  cache works because `cmake/Sparkle.cmake` re-verifies the archive against the pinned
  `ce89daf9...` SHA-256 and deletes it on mismatch, so the seeded path is hash-equivalent
  to the download CI performs. This is a sandbox limitation, not a repository defect.
- Two things remain unverified and must not be claimed. The bundle is adhoc/linker-signed
  with `Sealed Resources=none`, so it is not a distributable notarized artifact — release
  signing lives in the packaging workflows, not in this local build. And the Windows
  one-click path cannot be exercised from macOS at all; `gh` is installed but
  unauthenticated in this session, so remote CI status for
  `.github/workflows/windows-package.yml` could not be queried either.
- `0.3` therefore stays unchecked on native evidence: macOS clean-clone build and test
  evidence is complete, Windows evidence is not. Agent/Codex authority is unchanged and
  remains read-only.

## Evidence-Based Extension Compatibility (2026-08-24)

- Every extension record previously carried a blanket `Unknown` compatibility written by
  its own source adapter. That was honest but carried no information: `Unknown` meant
  "nobody looked", so the registry's `Verified + Compatible` enablement gate could never
  be satisfied by any real record, and an extension asking for authority the host does
  not grant looked exactly like an extension nobody had evaluated.
- `ExtensionCompatibilityPolicy` now makes that decision in one place, from evidence the
  host can actually check. Source adapters report facts only and no longer assert their
  own compatibility; their placeholder reason changed from `*-compatibility-unverified`
  to `*-compatibility-unevaluated` to say what it actually means.
- Two evidence inputs exist. The granted capability set is a product decision, fixed at
  `filesystem-read`, `mcp-tools`, `network`, `skill-content` — deliberately excluding
  `process` and any write capability, because Agent/Codex stays read-only until the
  permission, approval, sandbox, and recovery gates land. The Codex host version comes
  only from the version `ToolManager` actually detected on this machine.
- The evaluation order matters more than the individual checks and is pinned by
  `product_scope_policy`: a requested capability outside the granted set is a definite
  `Incompatible`, and that verdict is reached *before* any unknown-evidence fallback. The
  inverse order would let an unreadable version or a missing host version silently
  downgrade a definite rejection into `Unknown`, which is the more permissive answer.
- Missing evidence never produces `Compatible`. A Codex plugin without a detected host
  version resolves to `Unknown` with `codex-plugin-host-version-unknown`, and this
  repository pins no minimum Codex version anywhere, so the policy does not invent a
  floor to compare against — an unverifiable claim would be worse than an honest
  `Unknown`. Corrupt host evidence gets its own
  `codex-plugin-host-version-unreadable` code so "we could not find it" stays
  distinguishable from "we found something we cannot read".
- Practical consequence today: local Skills requesting only read capabilities become
  `Compatible`, stdio MCP servers become a definite `Incompatible` because they request
  process execution the read-only host does not grant, and Codex plugins stay `Unknown`
  until Codex CLI version detection has run.
- Deciding compatibility is not granting authority. `apply()` writes only the
  compatibility state and reason; it never touches trust, `effectiveEnabled`,
  `updateAvailable`, or `recoveryAvailable`. Enablement still requires the registry's
  `Verified + Compatible` gate, and no record in the product sets `Verified`, so nothing
  became enableable as a result of this change. Every emitted reason is a fixed
  lowercase code that satisfies the registry's own validation, checked by the test.
- Verified with `extension_compatibility_policy` plus the coordinator, registry, source,
  and product-scope tests, then the full serial gate: `60/60` in 244.80 seconds.
- `0.4` stays unchecked. Import, enable/disable, update, removal, backup, and recovery
  workflows remain entirely open, and no mutation authority was added.

## Reviewed Extension Trust (2026-08-24)

- Compatibility was only half of the registry's `Verified + Compatible` enablement gate.
  Trust was the other half and had no decision path at all: every source hard-coded
  `Unverified`, so nothing could ever be enabled and any future enable workflow would
  have had to invent its own trust rule at the point of use.
- `ExtensionTrustPolicy` now makes that decision from review evidence only. Trust comes
  from one human review pinned to exact content — never from what an extension says about
  itself. A review pin binds kind, id, source identity, and content identity jointly,
  because what was reviewed is a specific piece of content, not a name.
- Every drift revokes trust. Content drift reports
  `extension-review-content-drift`, source drift reports
  `extension-review-source-drift`, and when both changed it reports content drift because
  the content already differs and the source distinction no longer adds anything.
- Two fail-closed rules matter more than the happy path. Duplicate or conflicting pins for
  the same `(kind, id)` resolve to `extension-review-conflict` rather than picking one:
  picking the matching pin would mean an attacker only has to *append* a pin to a review
  store to pass, never needing to alter the existing one. And a single malformed pin fails
  the whole evaluation rather than being skipped, so a corrupt entry cannot be used to
  hide alongside a valid one. An oversized store is rejected outright rather than
  truncated, for the same reason.
- The ordering is pinned by `product_scope_policy`: unverifiable records, oversized
  stores, and malformed pins are rejected before any matching happens, and conflict is
  decided before a match can be reported.
- Granting trust is not granting authority. `apply()` writes only the trust state and
  leaves compatibility, `effectiveEnabled`, `updateAvailable`, and `recoveryAvailable`
  untouched. A record that is both `Verified` and `Compatible` still is not enabled — that
  requires a separate action which does not exist yet.
- Nothing in the product supplies review pins. `MainWindow` passes none, and
  `product_scope_policy` asserts it stays that way until a real review workflow exists, so
  every record in the shipping Extension Center remains `Unverified` and unenableable.
  This slice built the decision, deliberately not the evidence source.
- Verified with `extension_trust_policy` plus coordinator round-trip assertions proving an
  exact pin verifies exactly one record, enables nothing, and loses trust on content
  drift. Related extension and product-scope tests pass; full serial gate `61/61`.
- `0.4` stays unchecked. Import, enable/disable, update, removal, backup, and recovery
  workflows remain open and no mutation authority was added.

## Authenticated Extension Review Ledger (2026-08-24)

- The trust decision existed but its evidence had nowhere to live. Review pins had to be
  handed in from memory, which means the only way to persist them would have been an
  ordinary config file — and a review store that anyone can edit is not evidence of review
  at all. Trust that can be granted by editing a file is worth nothing.
- `ExtensionReviewLedger` is the authenticated record layer for that evidence, following
  the same conventions as the companion activation journal: domain-separated HMAC-SHA256
  under a 32-byte key, 8-byte big-endian length-prefix framing so no field boundary can be
  shifted, `CRYPTO_memcmp` for comparison, `OPENSSL_cleanse` for key material, exact
  key-set JSON validation, and a monotonic generation.
- The MAC covers the generation, the pin count, and every field of every pin in order.
  That is the load-bearing choice: a MAC over individual pins would let an attacker append
  their own extension to a legitimately reviewed store, or delete a pin, or reorder them,
  without ever forging anything. Appending, removing, reordering, and swapping a single
  content identity all fail as `extension-review-ledger-mac-mismatch`.
- Anti-degradation is explicit. `Empty` is reachable only from genuinely empty input.
  A payload that exists but cannot be authenticated or parsed reports `Invalid` with a
  distinct code (`-oversized`, `-record-invalid`, `-pin-limit`, `-pin-invalid`,
  `-pin-duplicate`, `-mac-mismatch`), and an unusable key reports `Unavailable` with
  `-key-unavailable`, because "I cannot read this" is a different fact from "there is
  nothing here" and only the latter may look like a clean slate.
- An authenticated empty set is a valid state meaning "reviewed, and nothing is currently
  trusted". It is distinguishable from no ledger at all: it carries a generation and an
  identity digest, while an absent ledger carries neither.
- Duplicates are rejected at the record layer rather than deferred to the trust policy.
  The trust policy would catch them as a conflict, but a store that can never produce a
  usable answer should not be writable or readable as valid in the first place.
- Authentication happens after structural parsing but strictly before any pin is returned;
  `product_scope_policy` pins that ordering. An unauthenticated payload yields an empty
  pin list, so a failed ledger can never fall back to a usable set of reviews.
- The layer carries evidence and grants nothing. It has no persistence of its own, no
  reference to `effectiveEnabled`, and never names `ExtensionTrustState::Verified`. Nothing
  in the product loads a ledger yet — `product_scope_policy` asserts `MainWindow` does not
  — so every shipping record still remains `Unverified` and unenableable.
- Verified with `extension_review_ledger` covering the round trip, deterministic
  serialization, identity binding to generation and pin ordering, every tamper and
  malformed case above, serialization guards (generation bounds, short/absent key,
  malformed pin, duplicate pins, pin limit), and agreement with the trust policy in both
  directions. Full serial gate `62/62` in 203.43s.
- Two findings worth carrying forward. First, `agent_workbench_render`'s `AWB_EDITOR_LSP`
  stage is not robust under machine contention: it drives a real `clangd` behind 5-second
  waits and failed once when a parallel build of the same tree was running, then passed
  standalone and in the clean gate. Treat a lone `AWB_EDITOR_LSP` failure as a load signal
  before treating it as a regression, and do not run the gate concurrently with a build.
- Second, a genuine defect found while investigating that: in
  `AgentWorkbenchWidget::finishPinnedDiagnosticRaw` the pinned-diagnostic identity digest is
  built with a four-argument `arg()` followed by three single-argument calls, which consumes
  every placeholder before `severity` and emits `QString::arg: Argument missing`. Severity is
  silently dropped from the identity, so two diagnostics differing only in severity at the
  same location collapse to one pinned descriptor. Not fixed here; it is its own slice.
- `0.4` stays unchecked. A human review workflow that produces pins, a persistence adapter,
  and every import/enable/disable/update/remove/backup/recovery workflow remain open.

## Pinned Diagnostic Identity Completeness (2026-08-24)

- Fixed the `finishPinnedDiagnosticRaw` defect recorded above. The digest now uses seven
  placeholders and one seven-argument `arg()` call, so project, root, raw reference, path,
  line, column, and severity all reach the identity. Before this, two diagnostics at the same
  location differing only in severity produced the same `pin-diagnostic-<digest>` id, and the
  second silently replaced the first in the pinned context set.
- The broader lesson is the reason this slice exists. Qt reports format-string misuse only as
  a runtime warning on stderr, so a digest input can vanish without any test failing. That is
  a bad property for identity computation specifically: the failure mode is a silent identity
  collision, not a visible error.
- `agent_workbench_render` now installs a message handler that counts any `QString::arg`
  warning across the whole render pass and fails at the end if the count is not zero. This
  catches the class of defect, not just this line. Confirmed both directions: the guard fails
  with the old code restored and passes with the fix.
- Note for anyone adding assertions to that test: `windows_packaging_policy` pins the exact
  ordered `setFailureStage` sequence inside the render main, so adding a stage call is drift
  and fails packaging policy. Use the explicit `expect(condition, message, code)` overload for
  anything that is not genuinely a new stage.
- Verified by full serial gate `62/62`. No authority change; this is a correctness fix inside
  existing read-only context pinning.

## Split Persistence For Extension Review Evidence (2026-08-24)

- The authenticated ledger had no place to live. Persistence is split the same way the
  companion activation journal splits it: authority (the 32-byte HMAC key, the committed
  generation and identity) in platform secure storage via an injected
  `ExtensionReviewLedgerSecureStore`, and the bulkier payload bytes in `QSettings` under
  `extensions/review-ledger/record`. The key never enters ordinary settings, and the pins are
  never duplicated into the authority — asserted by test, not by convention.
- The two halves must corroborate each other, so deleting either one is a distinct failure,
  never a quiet return to "never reviewed": an orphaned payload is
  `extension-review-store-record-without-authority`, an orphaned authority is
  `extension-review-store-record-deleted`, and `Empty` is reachable only when both halves are
  genuinely absent. A corrupt payload keeps the ledger's own `extension-review-ledger-*` code
  so "tampered" stays distinguishable from "absent".
- Replay is the attack this layer had to close. An old payload was legitimately signed once, so
  restoring it after reviews are revoked would otherwise re-grant trust. The authority pins the
  committed generation and identity, and a payload that does not match both is
  `extension-review-store-record-superseded` rather than a usable set.
- Publication is three-phase (reserve → write payload → commit) so an interrupted write is
  adjudicated by what actually landed on disk instead of inferred. On the next `load()`, a
  reservation whose identity matches the on-disk payload is promoted; one that does not is
  rolled back; and if that resolution cannot itself be persisted the result is
  `OutcomeUnknown`, which yields no pins and blocks further commits until the backend recovers.
  Each of the three interruption points is driven directly by an injectable fake, including
  recovery being persisted rather than re-inferred on every read.
- Concurrent reviews are resolved by compare-and-set on the generation
  (`extension-review-store-generation-conflict`), not by last-writer-wins. Malformed or
  duplicate pins are rejected before anything is written, so a failed commit leaves no
  reservation residue.
- Still read-only and still unwired: `product_scope_policy` asserts the store grants no
  `effectiveEnabled`, decides no trust, and that `MainWindow` names neither
  `ExtensionReviewLedger` nor `ExtensionReviewLedgerStore`. What remains for this area is a
  `SecureStorage`-backed adapter implementing the interface and an actual human review workflow
  that produces pins; neither should be wired in until reviewed.
- Verified by full serial gate `63/63` in 596.74s, and by temporarily removing the
  `record-deleted` branch to confirm the anti-degradation guard fails rather than passing
  vacuously.

## Shared A/B Authority Publication And Review Authority Anchor (2026-08-24)

- The review ledger's authority faced exactly the hazard the companion journal already solved:
  its HMAC key exists nowhere else, so one torn secure-storage write would make every stored
  review permanently unauthenticatable. Duplicating the A/B slot recovery logic would have
  created two copies that can drift apart, so it was extracted into
  `AuthoritySlotPublication` and both subsystems now call it.
- Domain separation is a security property of the shared layer, not a formatting detail. Each
  caller supplies its own frame schema, digest domain, and error prefix, all of which enter the
  persisted bytes. A slot frame from one subsystem therefore fails to parse in the other, and
  relabelling its `schema_version` does not help because the digest domain also participates. An
  unconfigured domain is rejected outright rather than falling back to a default format.
- The extraction had to be byte-compatible or existing installs would stop reading their own
  authority. `CompanionActivationAuthoritySlots` is now a thin facade holding only its two
  domain constants, and `authority_slot_publication_test` recomputes the companion digest
  independently — domain string, 8-byte big-endian length prefixes, generation then payload —
  so drift in the implementation is caught instead of being mirrored. The companion's own slot
  and journal tests pass unchanged.
- `SecureStorageExtensionReviewLedgerAdapter` anchors review authority in platform secure
  storage under `extensions/review-ledger-authority/slot-{a,b}/v1`, reading through
  `loadEncryptedFresh` so a locked backend is never misread as "never reviewed". It has no
  legacy single-slot migration path — the subsystem is new, so adopting a stray envelope would
  only be an attack surface — and it moves bytes only: it never parses pins, decides trust, or
  grants enablement. A write it cannot confirm returns `OutcomeUnknown`, which the store already
  handles as an unresolved publication.
- Full serial gate `64/64` in 195.91s. Both new guards were confirmed to fail when domain
  separation is removed.
- Build-staleness note worth remembering: after restoring a file from a sabotage check, `make`
  compared same-second mtimes and skipped the rebuild, so a gate run reported a failure from the
  sabotaged binary. `touch` the restored file (or check `Building CXX` appears) before trusting
  a gate that follows a revert.

## Human Review Planning For Extension Trust (2026-08-24)

- `ExtensionReviewWorkflow` turns an approve/revoke action into the complete review set that
  should exist after the commit, rather than mutating storage in place. Every security property
  of approval and revocation is therefore decidable and testable without persisting anything,
  and the store keeps sole authority over how the set lands on disk.
- Approval binds to the exact content the human saw. The request carries the source and content
  identities rendered at review time, and a mismatch against the current record fails with
  `extension-review-content-drift` / `extension-review-source-drift` instead of re-targeting the
  decision onto whatever is on disk now. An absent, duplicated, uninstalled, or unverifiable
  record cannot be approved — approving something absent would be pre-authorizing content that
  has not appeared yet.
- The stored set is adjudicated before it is used as a base. An unusable ledger state
  (`Invalid`, `Unavailable`, `OutcomeUnknown`) is rejected because committing a partial set as a
  complete one would silently delete the reviews that failed to load. A malformed existing pin
  is rejected rather than carried along, since committing it would launder it into authenticated
  evidence, and an already-conflicting set is rejected because "which duplicate to drop" has no
  correct answer.
- Re-approval replaces the pin for the same `(kind, id)` instead of appending a second one,
  which would manufacture exactly the conflict state the trust policy must reject. Revocation
  matches on `(kind, id)` only, so a tampered or uninstalled extension can still be removed —
  otherwise a compromised entry would be stranded in the set permanently. A full set rejects new
  approvals with `extension-review-pin-limit` rather than evicting an existing review, and
  revocation still works on a full set so it can shrink.
- The layer grants no enablement: a completed review moves a record to `Verified` and nothing
  else, so the registry's double gate still requires an independent enable action. It is not
  wired into `MainWindow` yet, and `product_scope_policy_test` pins that absence.
- Full serial gate `65/65` in 198.18s. Guards confirmed by sabotage: removing the drift
  comparison, the replace-in-place branch, or the unusable-ledger rejection each fails the test.
- Gate hygiene: do not export `AEGISY_AGENT_BACKEND=preview` for the whole `ctest` run. CMake
  already sets it per-test, and leaking it into `cargo test` makes `agent_runtime_protocol`'s
  stdio fixtures run the preview backend instead of the Codex fixture they spawn, producing 22
  spurious failures. Run `ctest` with a clean environment.

## Spoof-Resistant Review Presentation (2026-08-24)

- A human review decision is only as trustworthy as what was rendered. Extension names,
  versions, scopes, and capability lists all come from untrusted disk sources, so
  `ExtensionReviewPresentation` decides whether a record can be displayed at all before anyone is
  asked to approve it. It renders only; it approves nothing, persists nothing, decides no trust,
  and grants no enablement.
- Unpresentable text is rejected, never cleaned or truncated. Truncating would let two different
  extensions render identically, and sanitizing would show a name that does not exist. Control,
  format, surrogate, private-use, and unassigned categories are refused, as are the bidirectional
  overrides and isolates, zero-width characters, line/paragraph separators, and BOM that are not
  in those categories but still make the screen disagree with the string. Leading or trailing
  whitespace is refused for the same reason.
- The prompt echoes the exact full identities it displayed, and the review workflow compares
  against them, so drift between rendering and approval is detected rather than silently
  re-targeted. The short fingerprint is display-only and keeps both ends of the digest — showing
  a prefix alone would let a constructed prefix collision look identical on screen.
- Risks are surfaced as explicit warnings in a fixed order rather than left to be inferred from
  layout: a name unrelated to its identifier (the masquerade case, since the name is the most
  prominent thing on screen), an unknown version, a capability outside the granted set, a write
  or execution capability against the read-only boundary, unresolved compatibility, and content
  that changed since the last review. Claiming a prior review without a usable previous digest is
  rejected rather than assumed unchanged.
- Full serial gate `66/66` in 266.79s. Guards confirmed by sabotage: dropping the invisible-
  character range, truncating instead of rejecting an over-long name, and echoing the fingerprint
  instead of the full identity each fail the test.
- The mtime staleness hazard recorded in the A/B slice recurred here and made one sabotage check
  falsely pass. When verifying a guard, confirm `Building CXX` actually appears for the file
  before trusting the result; `touch` alone is not enough if the timestamps land in the same
  second.

## Product Extension Review Workflow (2026-08-25)

- Extension Center now consumes the authenticated review ledger through the SecureStorage A/B
  authority adapter and QSettings payload. Invalid, Unavailable, and OutcomeUnknown remain
  explicit fail-closed states and disable review; they never become an empty review set.
- `ExtensionReviewController` owns the composition boundary. Each approve/revoke operation
  fresh-loads the ledger and fresh-scans Codex plugins, Skills, and MCP, runs the drift-aware
  review workflow, commits the complete pin set through generation CAS, and full-rescans with
  returned authenticated pins. The UI never sets trust directly.
- The PlainText review prompt contains exact full source/content identities, version, scope,
  requested capabilities, and fixed warnings. Its confirmation checkbox starts unchecked and OK
  disabled. It states that review evidence does not install, enable, update, remove, execute, or
  change tool configuration. Table roles retain only kind/id.
- Authenticated pins whose source is missing remain visible as safe stale rows and can be revoked
  by `(kind,id)`. Content drift after presentation fails before ledger creation or write. No-op
  revoke does not advance generation. Invalid authority blocks writes.
- MainWindow owns one tracked review worker, binds results to a monotonic generation, ignores
  stale callbacks, and waits for an active worker during destruction. QSettings and SecureStorage
  adapters are constructed inside the worker thread.
- Full serial gate `67/67` in 781.74s. Verified remains trust evidence only and
  `effectiveEnabled` stays false; Extension Center has no install/enable/update/remove/execute
  control.
- OpenSpec `0.4` remains unchecked for reviewed enable/disable, import, update, removal, encrypted
  backup, rollback, and recovery. Agent/Codex stays read-only and no extension execution authority
  was added.

## Reviewed Enablement Decision (2026-08-28)

- `ExtensionEnablementPolicy` is the independent enable action the registry's
  `Verified + Compatible` gate always required but nothing produced. Review answers "a human saw
  and accepted this content"; compatibility answers "the current host grant can hold it". Neither
  means the user asked for it to run, so enablement is its own decidable layer.
- A grant binds `(kind, id, sourceIdentity, contentIdentity)` jointly, exactly like a review pin.
  Replacing content therefore cannot inherit the previous content's enablement — the precise
  hazard this authority exists to prevent. Content drift is reported ahead of source drift.
- Fail-closed rules mirror the trust policy: conflicting or duplicate grants for one `(kind, id)`
  reject in either ordering, one malformed grant fails the whole evaluation rather than being
  skipped, an oversized store is rejected rather than truncated, and an unverifiable record is
  refused before matching.
- A valid grant bypasses nothing. Uninstalled, unreviewed, and unknown/incompatible records each
  refuse with a distinct code, while a missing grant still reports `extension-not-enabled` so
  diagnostics never describe a missing grant as a missing review. Write and execution capabilities
  stay outside the granted set, so extensions requesting them remain definitely `Incompatible`.
- `apply()` writes only `effectiveEnabled`; trust and compatibility are untouched, so the registry
  gate remains an independent second check. The test proves the registry rejects the same
  enablement once trust is revoked, and that re-evaluation withdraws it.
- No grant producer exists in the product path. `MainWindow`, the Extension Center dialog, and the
  inventory coordinator name no enablement policy and product scope pins that absence, so every
  shipping record stays unenabled.
- Full serial gate `68/68` in 201.13s. Guards confirmed by sabotage: removing the conflict rejection, the
  content-drift comparison, or the trust gate each fails the test.
- OpenSpec `0.4` stays unchecked: enablement has no persistence, no UI, and no grant producer, and
  import, update, removal, encrypted backup, rollback, and recovery remain open. Agent/Codex stays
  read-only and no extension execution authority was added.

## Shared Evidence Ledger And Enablement Grant Authority (2026-08-28)

- Enablement grants need the same authenticated home review pins have: an ordinary editable file
  would let anyone grant enablement by editing text. The grant record shape is identical to a
  review pin, so duplicating the ledger would have created two copies that drift — the hazard the
  A/B slot extraction already resolved.
- `ExtensionEvidenceLedger` now holds the codec; `ExtensionReviewLedger` and
  `ExtensionEnablementLedger` are thin facades holding only their own domain constants.
- Domain separation is the security property. Review evidence and enablement grants are two
  different authorizations: a shared format would let a review payload's bytes move into the grant
  position, turning "a human saw this" into "a human asked to run this". Schema, MAC domain, and
  identity domain all differ, so a payload from one fails to parse in the other and relabelling
  `schema` does not help because the MAC domain also participates. An unconfigured domain is
  rejected rather than falling back to a default format.
- The extraction had to be byte-compatible or installs would stop reading their own review
  evidence. `extension_review_ledger` recomputes the review MAC and identity independently from
  the domain string, 8-byte big-endian length prefixes, and generation-then-set ordering, so drift
  is caught instead of mirrored. Entry-level codes keep each caller's own noun (`pin` / `grant`),
  so no previously pinned diagnostic code changed.
- The ledger carries evidence only. A parsed grant still goes through
  `ExtensionEnablementPolicy`, which additionally requires reviewed, compatible, and installed.
- Guards confirmed by sabotage: collapsing the enablement domains onto the review domains makes a
  relabelled review payload parse as a grant set; changing the review MAC domain fails the
  byte-compatibility check.
- Full serial gate `69/69` in 192.94s.
- Nothing in the product path reads or writes grant payloads, so every shipping record stays
  unenabled. OpenSpec `0.4` stays unchecked: grant persistence, a grant-producing UI action,
  import, update, removal, encrypted backup, rollback, and recovery remain open.

## Shared Evidence Persistence And Enablement Grant Store (2026-08-28)

- Grant payloads need the same split-persistence home review pins have: authority (HMAC key plus the
  committed generation and identity) in platform secure storage, payload bytes in `QSettings`.
  Deleting either half is a distinct failure, never "never granted" — degrading to empty is the safe
  direction here (no grant means no enablement) but it would report a tampered record as a user who
  never asked to enable anything, hiding the tamper.
- `ExtensionEvidenceLedgerStore` now holds the three-phase publication (reserve → write payload →
  commit), the anti-degradation adjudication, and the generation compare-and-set.
  `ExtensionReviewLedgerStore` and `ExtensionEnablementLedgerStore` are facades holding only their
  own authority schema, `QSettings` record key, diagnostic prefix, and ledger domain, plus an
  adapter forwarding their injected secure store. An unconfigured domain is rejected in both `load()`
  and `replace()` rather than falling back to a default format.
- Persistence-level domain separation is the security property. A review authority envelope and
  payload moved wholesale into the enablement position is rejected as
  `extension-enablement-store-authority-invalid`, and the two payload keys differ so neither can
  overwrite the other. Without this, "a human saw this content" could be relocated into "a human
  asked to run this content".
- Every previously pinned diagnostic code survived the extraction: the entries noun is a domain field
  (`entriesCodeNoun`), so the review store still emits `extension-review-store-pins-invalid` while
  the grant store emits `extension-enablement-store-grants-invalid`.
- Replay is rejected by anchoring on the committed generation *and* identity: an old payload was
  legitimately signed once, so restoring it would otherwise revive revoked enablement.
- Guards confirmed by sabotage: sharing the review authority schema makes a review envelope adoptable
  as enablement authority; sharing the record key makes review evidence read as grants; disabling the
  superseded check revives a revoked grant set; blanking the orphan code degrades a half-deleted
  record to empty; both generation CAS sites must be intact for a stale write to be refused.
- Full serial gate `70/70` in 227.41s.
- Nothing in the product path constructs an enablement store, and `product_scope_policy` pins that
  absence for the main window, the extension center, and the review controller. Every shipping
  record stays unenabled. OpenSpec `0.4` stays unchecked: a grant-producing UI action, import,
  update, removal, encrypted backup, rollback, and recovery remain open.

## Enablement Grant Planning Without Persistence (2026-08-28)

- The grant store could hold grants but nothing could produce one. `ExtensionEnablementWorkflow`
  translates an explicit human enable/disable request into the complete grant set that should exist
  after the commit, plus the generation to compare-and-set on — mirroring
  `ExtensionReviewWorkflow`, so every security property is decidable and testable with no
  persistence in the layer at all.
- Planning requires reviewed *and* compatible *and* installed at plan time, not just at evaluation
  time. Evaluation independently requires all three, so a premature grant would never enable
  anything today — but it would sit in the ledger as authenticated authority and take effect the
  moment review appeared. That is pre-authorizing future content, so the plan refuses it outright.
- The plan is the complete set, never a delta. Planning against an unreadable ledger is refused
  (`extension-enablement-ledger-unusable`) because committing a partial set would silently revoke
  the grants that failed to load; that direction is safe but it reports a tamper as a user who chose
  to disable, and it commits under the wrong generation.
- Revocation is keyed on `(kind, id)` alone. Content drift, a vanished record, a revoked review, and
  an incompatible record must all still be revocable, or a tampered extension could never have its
  enablement authority withdrawn. It does not match across kinds.
- An invalid grant already in the ledger fails the whole plan rather than riding along: committing
  it would authenticate the whole set and launder that entry into valid authority.
- Re-granting is replace, not append, so the set never reaches the conflicting state the policy must
  reject; an identical re-grant reports `changed == false` so no generation is burned.
- Guards confirmed by sabotage (each with `Building CXX` observed): dropping the trust gate, the
  compatibility gate, either drift comparison, or the unusable-ledger check; planning a delta instead
  of the full set; skipping the existing-invalid-grant rejection; matching revocation across kinds;
  and appending instead of replacing.
- Full serial gate `71/71` in 195.19s.
- The workflow has no caller. `product_scope_policy` pins that the main window, the extension center,
  and the review controller neither plan nor persist grants, so every shipping record stays
  unenabled. OpenSpec `0.4` stays unchecked: the UI action that raises a request, import, update,
  removal, encrypted backup, rollback, and recovery remain open.

## Enablement Grant Control Without Opening Execution (2026-08-28)

- `ExtensionEnablementController` binds the grant workflow to the grant store: load, plan, commit
  through generation CAS, then re-read. Structurally identical to `ExtensionReviewController`.
- The authority boundary is the whole point of the slice. The controller does **not** feed grants into
  `ExtensionInventoryCoordinator`. The coordinator would write `record.effectiveEnabled`, which is the
  authority to actually run extension content — the gate that must stay shut until the permission,
  approval, sandbox, and recovery gates exist. Instead the enablement decision is returned as a
  positional projection (`decisions`, parallel to `inventory.records`) for display and diagnostics
  only, and is never written back. A committed grant therefore evaluates to "should be enabled" while
  the record itself stays unenabled, and the registry's `Verified + Compatible` gate is untouched.
- After a commit the controller re-reads rather than trusting the plan: only the bytes read back are
  the authority that actually took effect.
- An unreadable ledger yields no grants and blocks writes — showing "these extensions were never
  granted" when the current grants are unknown would misreport a tamper as a user choice.
- Test-fixture lesson: a Skills record requests `filesystem-read`, so with an empty
  `host.grantedCapabilities` the compatibility policy decides `Incompatible`
  (`extension-capability-not-granted`) and a grant request never reaches the gates under test. The
  fixture must set `ExtensionCompatibilityPolicy::defaultGrantedCapabilities()`.
- Guards confirmed by sabotage: applying the policy to the records opens effective enablement;
  skipping the unusable-ledger check permits a write against invalid authority; committing when
  unchanged burns a generation; reusing the pre-commit snapshot instead of re-reading; reporting a
  missing store as empty rather than unavailable. The controller's own discard-grants-when-not-Ready
  branch is defense in depth and cannot be observed through the real store, so the test pins the
  store-side contract (`load()` returns no entries with a non-Ready state) directly rather than
  leaving an unverifiable guard.
- Full serial gate `72/72` in 195.24s.
- No caller exists. `product_scope_policy` pins that the main window and extension center drive
  neither the controller nor the workflow, and that the coordinator accepts no grant input. OpenSpec
  `0.4` stays unchecked: the UI action, import, update, removal, encrypted backup, rollback, and
  recovery remain open.

## Shared Platform Secure Storage Authority Slot Adapter (2026-08-28)

- `SecureStorageAuthoritySlotAdapter` is the third and final extraction along the authority-slot
  precedent: A/B slot selection, fresh (cache-bypassing) reads, strict UTF-8 framing, bounded payloads
  and the write-outcome trichotomy now live once, parameterized by a caller-supplied
  `SecureStorageAuthoritySlotScopes`. The activation journal, extension review ledger and extension
  enablement ledger are facades holding only their own scopes and domain strings.
- The enablement ledger now has a real platform backend
  (`SecureStorageExtensionEnablementLedgerAdapter`) with scopes, frame schema and digest domain fully
  independent of the review ledger's. Domain separation is now a tested property at all three layers
  (codec, persistence, secure storage): a shared format or scope would let a review payload be
  relocated into the grant position, turning "a human saw this content" into "a human asked to run
  this content".
- An unconfigured or collapsed scope set is refused outright (`secure-authority-slot-scopes-unconfigured`)
  rather than falling back to a default location. A facade that forgets its scopes must fail, not
  silently share another subsystem's authority envelope; collapsing both slots onto one scope would
  degrade A/B publication back to the single copy whose torn write destroys the only HMAC key.
- **Extraction hazard found and fixed:** the first cut of the shared layer silently dropped the
  activation journal's post-commit legacy-source removal (`selection.legacyPending` →
  `SecureStorage::remove`). Byte-compatible framing is not sufficient evidence that an extraction
  preserved behavior; every branch of the original, including cleanup that runs only on a migration
  path, has to be accounted for. `product_scope_policy` now pins the cleanup in the shared layer.
- **Verification-method lesson (the important one).** The first version of the adapter test built its
  expected domains from literals declared inside the test file, then compared the shared layer's
  output against those same literals. It passed while never reading the adapters at all: swapping the
  enablement digest domain onto the review digest domain, and drifting the review frame schema to
  `/0.2`, both went undetected. A byte-compatibility test must observe the value the product actually
  uses. Each adapter now exposes `authoritySlotScopes()` and the test compares those real values
  against its own independently rewritten copies of the persisted strings. Both previously-silent
  sabotages now fail.
- The activation domain strings existed in two places after the extraction (the slots class and the
  adapter). Two copies of a persisted constant drift independently, so
  `CompanionActivationAuthoritySlots::domain()` is now the single source the adapter reads.
- Guards confirmed by sabotage: reusing the review slot scope for enablement; reusing the review
  digest domain; drifting the review frame schema; adopting the review slot as a legacy scope;
  dropping legacy cleanup; hardcoding a subsystem scope in the shared layer; accepting an unconfigured
  scope set; re-copying the activation domain constants instead of reading the single source.
- Full serial gate `73/73` in 249.64s.
- This layer only moves bytes: it decides no trust, grants no enablement, and executes nothing.
  `product_scope_policy` pins the absence of `effectiveEnabled` and `QProcess` in both the shared
  layer and the enablement facade. OpenSpec `0.4` stays unchecked: the UI action that raises a grant
  request, import, update, removal, encrypted backup, rollback, and recovery remain open, and
  Agent/Codex stays read-only.

## Spoof-Resistant Enablement Prompts (2026-08-28)

- A grant prompt asks a different question than a review prompt. Review asks "did a human see this
  content"; enablement asks "do you want this to run". The second is the stronger authorization, so
  `ExtensionEnablementPresentation` additionally requires all three gates — installed, reviewed,
  compatible — to be satisfied *before the question is asked*.
- **Why gate at presentation time.** A grant recorded against an ungated record enables nothing today,
  because evaluation independently requires all three. But it would sit in the ledger as authenticated
  authority and activate the moment the missing gate appeared. Offering the action at all is therefore
  pre-authorizing future content. Ungated records render as `Blocked` with an explicit reason and no
  enable action, rather than being hidden or silently accepted.
- Block reasons are ordered installed → reviewed → compatible and are individually distinguishable.
  Reporting a missing review as a compatibility problem would tell the user that a different machine
  could run content no human has ever looked at.
- Every prompt carries `GrantDoesNotExecuteYet`. A grant currently runs nothing because the permission,
  approval, sandbox, and recovery gates do not exist; without saying so the UI would imply the user
  just switched on execution.
- **Revocation is deliberately ungated.** Content drift, a withdrawn review, an uninstalled target, and
  a vanished source must all remain revocable, otherwise a tampered extension could never have its
  authority withdrawn. An unsafe display name falls back to the identifier rather than refusing, and a
  vanished target is rendered as `targetAbsent` so the user is not told they revoked something still
  listed on screen.
- `ExtensionDisplaySafety` extracts the display-safety rules the two prompts share (invisible and
  bidirectional character rejection, digest form, both-ends fingerprint, capability read-only
  classification, name/identifier agreement). Two copies would drift, and drift here means one prompt
  accepts a bidirectional override the other rejects — the same extension rendering differently in two
  places. `product_scope_policy` pins that neither facade re-implements any character-category, code
  point, or trimming check locally.
- Guards confirmed by sabotage: dropping the trust gate, dropping the compatibility gate, reporting a
  missing review as a compatibility problem, dropping the executes-nothing disclosure, and gating
  revocation on trust. Shared-layer drift was verified to break *both* prompts: removing the invisible
  character range, truncating over-long text, showing only a fingerprint prefix, and accepting
  surrounding whitespace each fail the review and enablement tests together.
- Pin-quality correction: the first version of the no-duplication pin searched for the literal
  `code >= 0x200b`, so a facade that re-implemented the check with different spelling passed. Broadened
  to reject any local category, code point, or trimming check.
- Full serial gate `74/74` in 204.80s.
- The prompt has no caller. `product_scope_policy` pins that neither the main window nor the extension
  center names `ExtensionEnablementPresentation`, so no grant action exists in the product path.
  OpenSpec `0.4` stays unchecked: wiring the action, import, update, removal, encrypted backup,
  rollback, and recovery remain open, and Agent/Codex stays read-only.

## Approval Is A Credential, Not A Boolean (2026-08-28)

- `ExtensionApprovalPolicy` is the approval gate. It answers a different question than presentation:
  presentation decides *may we ask*, approval decides *does this answer constitute authority*. A
  renderable prompt is not a valid approval, and a forged or stale approval is exactly the path from
  "some text in tool output" to "the user asked to run this content" — the spec's forged-approval
  scenario.
- Approval is therefore not a boolean but a credential that must align item-by-item with what was on
  screen: target (kind *and* id — the same id under a different kind is a different extension), both
  identities, the exact disclosed risk set, and whether high risk was explicitly confirmed. Any
  mismatch refuses. This is stricter than "approved once, valid thereafter", which would carry consent
  for one piece of content onto another piece of content or another set of risks.
- **The risk set is part of the credential.** Every warning the prompt disclosed must appear in the
  acknowledgement, and an acknowledgement carrying a warning the prompt did *not* disclose is also
  refused — it came from a different UI state. Acknowledging nothing is "I saw no risks" and fails.
- Approving against a `Blocked` prompt is refused even when otherwise perfectly aligned: presentation
  already withheld the action, so such an approval is either from a stale UI or forged. Approving
  against an `Unpresentable` prompt is refused because no human can have seen unrenderable content.
- High risk (ungranted capability, beyond-read-only capability, content drift since a prior grant,
  name/identifier mismatch) requires per-decision explicit confirmation and **never** produces a
  reusable rule. A remembered rule auto-approves next time, which is precisely what high risk must not
  get. Purely informational disclosures do not demand confirmation, or the checkbox degrades into one
  the user always clicks.
- **No approval scope is broader than exact content.** `RememberForThisContent` binds the content
  digest, so the rule cannot match after the content changes. A by-name or by-identifier rule would
  transfer consent for reviewed content onto content nobody has seen.
- Guards confirmed by sabotage: accepting a blocked prompt; ignoring content drift; tolerating an
  undisclosed risk; skipping high-risk confirmation; letting high risk produce a reusable rule; not
  binding the extension kind; and defaulting an unclassified risk category to needing no confirmation.
- Verification lesson (recurring): the fail-closed default for unclassified warning categories sits
  after an exhaustive `switch`, so no ordinary test observes it. Rather than accept the passing
  sabotage, the test calls `requiresExplicitConfirmation` with an out-of-range value, which reaches the
  default directly. An earlier attempt to pin it via an enum-count formula did not fail when a value was
  appended, so it was replaced. Unreachable-looking guards are testable if you can construct the input
  that reaches them.
- Full serial gate `75/75` in 222.08s.
- The gate has no caller. `product_scope_policy` pins that neither the main window nor the extension
  center names `ExtensionApprovalPolicy`, and that the policy holds no `QSettings`, `SecureStorage`,
  `Ledger`, `QProcess`, or `effectiveEnabled` authority. OpenSpec `0.4` stays unchecked: the sandbox and
  recovery gates, wiring the action, import, update, removal, encrypted backup, and rollback remain
  open, and Agent/Codex stays read-only.

## Approval Is Intent; The Sandbox Is Enforcement (2026-08-28)

- `ExecutionSandboxGate` answers a question none of the earlier layers can: even for content a human
  reviewed and validly approved, *will the operating system actually stop it* from leaving the workspace
  to read credentials, rewrite system paths, or reach arbitrary hosts. A fully aligned approval expresses
  that someone is willing to let content run; it is not evidence that a boundary exists. Treating intent
  as enforcement announces a fence where there is none.
- The gate is therefore a **release** gate, not a runtime switch: write-capable native execution does
  not ship on a platform until that platform's filesystem, process, and network enforcement are each
  verified. ADR 0006 has not selected a Windows combination and macOS enforcement is separately owned
  with no delivered evidence, so the gate concludes read-only on every platform. That is the only
  conclusion the current evidence supports, not a conservative default.
- **Enforcement evidence is a projection of reviewed delivery, never a runtime probe.** A runtime probe
  asks the sandboxed process "are you sandboxed?", which is precisely the question it has no standing to
  answer. `currentEvidence()` leaves all three dimensions `Unverified` and the release gate unsigned;
  `product_scope_policy` pins that no optimistic constant is ever assigned there.
- All three dimensions must hold independently. Blocking filesystem writes while leaving child processes
  free just makes the same escape take a different route. A partial verdict reports *every* missing
  dimension, because naming only the first implies that fixing it would suffice.
- `Unavailable` and `Unverified` are distinct diagnostics: "not verified yet" is outstanding work, while
  "cannot be enforced here" is the conclusion that this platform must never open write execution.
- **A demonstrated escape blocks the write channel before any other consideration**, rather than
  degrading into a discussion of which dimensions are missing, and it also makes labeled Full Access
  unofferable. A boundary known to be bypassable is worse than none, because it looks like a boundary.
- Verified enforcement still is not sufficient: the platform's write-capable release gate report must be
  signed. Successful compilation, a working installer, and a process that starts are not enforcement
  evidence.
- A sandbox denial never auto-retries outside the sandbox — that converts one successful enforcement
  into a deferred escape — and is never attributable to the model, which would send people to edit the
  prompt instead of the boundary.
- Unclassified enforcement states and unclassified authority levels both fail closed, verified by direct
  out-of-range calls rather than by an unreachable fallback (the same pin-quality rule as the approval
  slice).
- Guards confirmed by sabotage: ignoring an open escape regression; dropping the release-gate
  requirement; tolerating missing dimensions; claiming enforcement the product never delivered; treating
  an unclassified enforcement state as enforced; defaulting an unclassified authority to read-only; and
  allowing a denial to retry outside the sandbox.
- Full serial gate `76/76` in 255.73s.
- The gate has no caller. `product_scope_policy` pins that neither the main window nor the extension
  center names `ExecutionSandboxGate`, and that the gate holds no `QProcess`, `QSettings`,
  `SecureStorage`, or `effectiveEnabled` authority. OpenSpec `0.4` stays unchecked: the recovery gate,
  wiring the action, import, update, removal, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only.

## Recovery Withdraws Authority, Never Reconstructs It (2026-08-28)

- `ExtensionRecoveryGate` answers what every earlier layer assumed away: what to do when the grant
  ledger itself is untrustworthy. This is a gate, not a finishing touch, because an unreadable ledger
  with no recovery path is a dead end — one interrupted publish permanently stalls every enablement
  decision on that machine. Same shape as [[reviewed-activation-recovery]]: `RecoveryRequired` correctly
  refused to guess but left the operator no way out.
- **Recovery does not infer the past.** A self-contradictory ledger cannot be "repaired" into the grant
  set it probably held; that would be forging authority. The only honest reconstruction is the empty
  set, and the empty set is a withdrawal.
- **Recovery can only reduce authority, never add it.** Any recovery path capable of producing a
  non-empty grant set is a path that manufactures consent — more dangerous than the corruption it means
  to fix. Every input combination is asserted to yield an empty plan.
- `Empty`, `Invalid`, `Unavailable`, and `OutcomeUnknown` are four distinct conclusions that must not
  degrade into each other. Treating unreadable as "never granted" destroys grants nobody can see;
  treating corrupt as "never granted" states a tampering event as the user's own choice.
- Unreadable (`Unavailable`) means **do nothing**: clearing what cannot be read destroys invisible
  grants that cannot be shown to the operator for confirmation. Unknown outcome means **re-read**, never
  write on top of uncertainty, which could overwrite a publish that actually committed.
- A readable ledger must never be touched by recovery. Since recovery clears every grant, a recovery
  action that worked on healthy ledgers would be a back door that revokes everything without going
  through approval.
- The operator's confirmation is bound to the conclusion that was displayed. A confirmation for
  "evidence is corrupt" must not be replayed against "cannot read the store" — the first permits
  clearing and the second never does.
- Withdrawal still requires explicit confirmation (it is a real authority change even though the
  direction is subtractive) and still commits the generation the operator read, so a concurrent grant
  cannot be silently overwritten.
- The transaction is never cleared at plan time. Completion requires a fresh read that is genuinely
  `Empty` *and* carries no grants; residual grants, persisting corruption, an unreadable backend, and an
  unknown outcome all keep it open, so a partial recovery can never pass as a completed one.
- Verification lesson (a new form of the recurring one): the withdraw-only invariant was initially
  **unobservable** because the fixture only attached a grant to the `Ready` state, so copying
  `ledger.grants` into the plan for an `Invalid` ledger still produced an empty plan and the sabotage
  passed. The fixture now attaches a grant to every state except `Empty`, and the test asserts the input
  really carried one — an assertion that a value was *discarded* is vacuous unless the input provably
  held it.
- Guards confirmed by sabotage: reconstructing past grants; acting on a readable ledger; replaying a
  stale assessment; offering a destructive clear for an unreadable store; clearing without confirmation;
  a stale generation; clearing the transaction before verification; reporting a partial recovery as
  complete; accepting `Empty`-with-grants as complete; and treating an unclassified store state as
  authoritative.
- Full serial gate `77/77` in 223.22s.
- **All four gates named in the standing read-only constraint — permission, approval, sandbox, recovery —
  now exist and are tested.** None has a caller: `product_scope_policy` pins that neither the main window
  nor the extension center names `ExtensionEnablementPresentation`, `ExtensionApprovalPolicy`,
  `ExecutionSandboxGate`, or `ExtensionRecoveryGate`. OpenSpec `0.4` stays unchecked: wiring the grant
  action into Extension Center, import, update, removal, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only.

## The Conjunction Of Four Gates Is Itself A Gate (2026-08-30)

- Four correct gates do not make a correct decision. Presentation answers *may we ask*, approval answers
  *is this answer authority*, the sandbox answers *can the OS enforce a boundary*, recovery answers *is
  the ledger trustworthy* — and nothing made any layer guarantee all four were consulted together.
- **That is the actual danger: a caller that queries three of the four passes silently.** With the gates
  spread across four types, missing one produces no compile error and no diagnostic; it just lets a grant
  stand while one precondition was never checked. So the conjunction must itself be an implemented,
  tested object rather than a convention each caller is trusted to remember. `ExtensionAdmissionGate`
  takes all four conclusions as required parameters — no defaults, so "forgot to pass it" is not a
  reachable state.
- It **delegates rather than re-derives**. It calls `ExtensionRecoveryGate::authoritative` instead of
  re-listing the readable states, and `ExtensionApprovalPolicy::evaluate` instead of re-implementing
  alignment. Two independent copies of a rule drift; the test pins that admission and recovery agree
  about readability for every ledger state.
- Approval diagnostics pass through verbatim rather than collapsing into one generic admission failure,
  so the operator learns *which* requirement failed.
- **The required enforcement level is derived from what was disclosed to the person, not from a re-read
  record.** Reading `prompt.warnings` (specifically `CapabilityBeyondReadOnly`) means a record rewritten
  after rendering cannot lower its own enforcement requirement — pinned by a test that rewrites
  `capabilities` and asserts the requirement holds, and by a `product_scope_policy` pin forbidding
  `prompt.capabilities` here entirely.
- **The gate is directional.** Read-only content is still admitted under today's read-only sandbox
  verdict. Only content whose disclosure reaches beyond read-only requires an enforced sandbox, and then
  the enforcement must actually cover the level needed — an "enforced" verdict still capped at read-only
  cannot carry a write grant. Refusing read-only content because writes are unenforced would be using
  the gate backwards, and a sabotage pins that too.
- Admission never widens what approval granted: `ruleGranted` is copied through, so high-risk content
  still gets no reusable rule.
- Guards confirmed by sabotage: omitting each of the four gates individually; deriving the required
  authority from the rewritable capability list; widening the reusable rule; and blocking read-only
  content on unenforced writes.
- Full serial gate `78/78` in 516.71s.
- Still no caller. `product_scope_policy` pins that neither the main window nor the extension center names
  `ExtensionAdmissionGate`, and that it holds no `QProcess`, `QSettings`, `SecureStorage`,
  `effectiveEnabled`, or store-`replace` authority. Gates being complete is not the same as the product
  offering the action: OpenSpec `0.4` stays unchecked, wiring the grant action plus import, update,
  removal, encrypted backup, and rollback remain open, and Agent/Codex stays read-only.

## Update Is When Content-Bound Trust Is Most At Risk (2026-08-30)

- Update is the single most dangerous moment for content-bound trust. The active version has been
  reviewed by a human and may hold a grant; the candidate is **by definition different content**. If trust
  or a grant travels by id, name, or version number, then "update" becomes the channel that runs arbitrary
  new content under the previous version's authority — and preventing exactly that is the entire reason
  identities are content-bound.
- So `ExtensionUpdatePolicy`'s verdict has only one shape: a validated candidate is `StagedUnreviewed`.
  Passing validation means it may be staged, never that it may run. `inheritsTrust`, `inheritsGrant`, and
  `candidateExecutable` are all false on **every** return path, including rejections — set in the reject
  helper rather than only on success, so a new early return cannot silently omit them.
- `reviewTransfers` returns true only when kind, id, source identity, and content identity all match, and
  empty identities never count as evidence. That is byte-identical content, which is not an update at all;
  the function exists to make the negative case explicit and testable rather than implicit.
- Each validation (signature, manifest, compatibility, dependency, health) is independently load-bearing
  with its own diagnostic, and on failure the active version is unchanged and the candidate cannot
  execute — the spec's "upgrade validation fails" scenario.
- **Identical content is not an update.** Accepting it would advance state for a no-op and could be used
  to clear a drift diagnostic that has not actually been dealt with.
- **Removal is the inverse requirement.** Executable content is disabled or deleted, but immutable
  identity metadata is retained — dropping it would erase the history that this content was once
  authorized to run, and removal is precisely when a record matters most for later audit. The grant is
  always withdrawn, or content reappearing under the same id/kind would inherit it. A vanished target is
  still removable and still leaves an identifiable record.
- Version numbers only **disclose** a downgrade; they never carry authority, since a version string is
  attacker-controllable. A downgrade is permitted but surfaced, because it reintroduces content that was
  already fixed. An incomparable version claims nothing rather than reporting "not a downgrade" as fact.
- Guards confirmed by sabotage: review transferring by identifier; a staged candidate inheriting
  authority; a failed upgrade disturbing the active version; removal discarding identity history; removal
  keeping the grant; skipping the health check; accepting identical content as an update; and claiming an
  incomparable version is a downgrade.
- Full serial gate `79/79` in 195.27s.
- No caller. `product_scope_policy` pins that neither the main window nor the extension center names
  `ExtensionUpdatePolicy`, and that it holds no `QProcess`, `QSettings`, `SecureStorage`,
  `QNetworkAccessManager`, `QFile`, or `effectiveEnabled` authority. OpenSpec `0.4` stays unchecked:
  wiring the grant/update/removal actions, import, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only.

## Import Previews Every Component, Not The Bundle Title (2026-08-30)

- A single extension bundle can carry Skills, hooks, MCP server configuration, commands, and inert
  assets at once, so **"import this bundle" is never one decision**. If the preview shows only the
  bundle's own name, version, and source, then what a person approves is a title while what is
  actually admitted is every executable component standing behind that title, any one of which may
  request capabilities the title never suggested. `ExtensionImportPreviewBuilder::build` therefore
  emits one row per component and each row carries **that component's own** requested capabilities.
- A bundle-level capability rollup cannot substitute for per-component disclosure. Two components
  separately requesting "read files" and "connect to the network" produce a rollup that is
  indistinguishable from one component requesting both, and only the latter is the dangerous
  combination. For the same reason `beyondReadOnly` is marked on the component that asked, and the
  bundle-level `anyBeyondReadOnly` is derived from those marks rather than computed alongside them —
  a read-only component must never be coloured by its neighbour's write request.
- An unrecognized **executable** component type fails the import closed rather than being skipped.
  Skipping it would let the bundle's real behaviour exceed what the preview described, and the person
  decided from the preview. `executable()` classifies `Unsupported` — and any unclassified future
  enum value — as executable precisely so a new component type cannot default into the harmless
  asset path; only `Asset` is non-executable.
- Failing closed does not mean discarding the evidence. The unsupported component is still listed,
  still marked `unsupported`, and still carries its `declaredType` string and content fingerprint, so
  it remains possible to determine what the bundle wanted to do. A refusal that also hides the reason
  leaves no one able to audit it.
- An empty bundle is `Unpresentable`, not an approvable import with nothing in it: with no rows, a
  person cannot know what they approved. Duplicate component identifiers are likewise refused,
  because when one identifier appears twice there is no way to tell which capability disclosure is
  the effective one. All display text passes `ExtensionDisplaySafety`, both bundle and component
  identities must be well-formed content/source digests, and component and capability counts are
  bounded, so a hostile manifest cannot decide what appears on screen.
- `grantsInstallation` is false on every path, including the ready path. This layer does not unpack,
  write, install, enable, or execute anything; it turns an already-parsed manifest into a decidable
  preview conclusion and nothing else.
- `extension_import_preview` covers the multi-component ready preview and per-component disclosure,
  fail-closed on an unsupported component with metadata preserved, per-component versus bundle-level
  write marking, unpresentable manifests (bad identifier, bidirectional override in the name,
  truncated identity, empty bundle, spoofed and malformed component identifiers, duplicate component
  and duplicate capability, component limit), and the absence of installation authority. Guards were
  confirmed by sabotage: silently skipping an unsupported component, treating an unrecognized type as
  an asset, discarding `declaredType` when failing closed, rolling per-component write marks up to
  the bundle, tolerating duplicate component identifiers, tolerating a spoofed component name,
  offering an empty bundle as approvable, and granting installation from a preview.
- Full serial gate `80/80` in 200.17s. No caller: `product_scope_policy` pins that neither the main
  window nor the extension center names `ExtensionImportPreview`, that the preview holds no
  `QProcess`, `QSettings`, `SecureStorage`, `QFile`, `QDir`, or `effectiveEnabled` authority, and that
  it contains no `continue;` that could skip a component instead of disclosing it. OpenSpec `0.4`
  stays unchecked: wiring the grant/update/removal/import actions, encrypted backup, and rollback
  remain open, and Agent/Codex stays read-only.

## Enablement Answers Whether, Scope Answers Where (2026-08-30)

- An enablement grant answers "does the user want this content to run", but it says nothing about
  **where**. With no scope model every enable is a global enable: a Skill approved for one project
  stays active when another project is opened, and an extension an organization forbids runs anyway
  because some lower level switched it on. Neither is an authorization bug — both are the absence of a
  layer that separates *authorized* from *applicable*. `ExtensionScopePolicy` is that layer, and the
  registry's `Verified + Compatible` gate plus the grant ledger remain untouched by it.
- Precedence is one-directional and explicit: `Managed` (0) outranks `Global` (1), `Project` (2),
  `Session` (3), `ChildTask` (4), and any unclassified future level falls to 1000 so a newly added
  level cannot acquire the power to overrule organization policy by default.
- **Denial and enablement are not symmetric, and treating them as symmetric is a real bug** — one this
  slice's first implementation contained. A lower level's *denial* always takes effect, because
  narrowing privilege is always the safe direction; a lower level's *enablement* can never overturn a
  higher level's denial, which is what the word "policy" means. Making both directions
  "highest-priority-wins" lets a child task that explicitly declined an extension be forced to accept
  it from above, the opposite of least privilege. A denial is therefore attributed to the
  highest-precedence level that denied, because reporting the child task would suggest that removing
  the child declaration would let it through while a global denial is still standing.
- Managed mandatory conclusions are processed before the grant check, since "not overridable by the
  user" means user-level opinions do not participate — a mandated extension activates with no user
  grant at all. But policy can compel *permission*, not *review*: a mandated extension whose content
  is unverified or incompatible still refuses with `extension-scope-managed-ungated`, because the
  double gate is about whether anyone examined the content and whether the host can accommodate it,
  and no policy statement answers either question. `mandatory` is honoured only on `Managed` rules, so
  a lower-level rule setting the flag gains nothing.
- Scope narrows an existing grant and never manufactures one: without `grantEnabled` every user-level
  scope rule refuses with `extension-scope-grant-absent`. Rules bind kind, id, source identity, and
  content identity jointly, exactly as grants do, so replaced content does not inherit the previous
  content's scope enablement. A level whose position identity is empty does not apply rather than
  applying everywhere — a project rule is inert with no project open, or "enabled for this project"
  would silently mean "enabled everywhere". Contradictory rules at one level are `Undecidable`, since
  there is no way to tell which disclosure is effective, and the rule count is bounded at 4096.
- Child tasks receive only their declared subset: a parent's global enablement does not travel to a
  child that never declared the extension, refusing with `extension-scope-child-task-undeclared`.
- Every refusal names its blocking level, so a person can tell which source to address instead of
  toggling levels one at a time — and toggling levels in an authorization surface means repeatedly
  asking for approval.
- `extension_scope_policy` covers per-project activation and non-activation elsewhere, global
  enablement reaching a second project, child-task declared subsets and child-level refusal, the
  directional precedence in both directions including two simultaneous denials, strict precedence
  ordering and unclassified levels, managed block and managed mandate plus the ungated refusal and the
  spoofed-mandatory case, undecidable rule sets, content and source drift, inapplicable levels, and
  the absence of enablement authority. Guards were confirmed by sabotage: reversing the precedence
  sweep, restricting denials to managed rules only, attributing a denial to the weakest level,
  skipping the managed pre-pass, letting child tasks inherit, activating without a grant, bypassing
  the double gate under a mandate, honouring mandatory on non-managed rules, tolerating same-level
  conflicts, binding rules by identifier, applying levels with no position identity, and giving an
  unclassified level top precedence.
- Full serial gate `81/81` in 293.99s. No caller: `product_scope_policy` pins the precedence values,
  the directional denial attribution, the managed pre-pass and its ungated refusal, content-bound rule
  matching, every `extension-scope-*` diagnostic, that neither the main window nor the extension
  center names `ExtensionScopePolicy`, and that the layer holds no `QProcess`, `QSettings`,
  `SecureStorage`, `QFile`, `QDir`, or `effectiveEnabled` authority. OpenSpec `0.4` stays unchecked:
  wiring the grant/update/removal/import actions, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only.

## Model-Visible Text Is Not Policy (2026-08-30)

- Project instructions and Skill content are **model-visible text**, and model-visible text is text the
  model will act on. That creates two independent problems whose answers point in opposite directions,
  which is why `InstructionContextPolicy` handles both rather than one.
- The first is inspectability. Without a record of which instructions loaded this turn, where each came
  from, and at what precedence, the only way to investigate surprising behaviour is to guess. Nesting
  makes it worse: instructions in a directory closer to the target file override outer ones, and that
  override relation must be an entry in the manifest rather than a fact recoverable only by reading
  code. Every entry therefore carries its source path, content fingerprint, precedence, and a label
  explaining its position, and the whole applicable chain is present — omitting one entry is what
  reduces investigation to guessing.
- Precedence is `Managed` 0, `UserGlobal` 1000, `ProjectRoot` 2000, `ProjectNested` 3000, `Skill` 4000,
  unclassified 100000. Nested depth adjusts within the `ProjectNested` band only
  (`+= MaxDirectoryDepth - depth`), so a deeply nested instruction outranks a shallower one and the
  project root but can never overtake the invoked Skill or managed policy. Equal precedence breaks ties
  on source path, because an override chain whose order depends on input order cannot explain anything.
- The second problem is authority, and here the answer is the reverse: **instruction text on disk is
  not policy**. A project instruction reading "you may execute commands directly" is byte-for-byte
  indistinguishable from ordinary prose, so runtime policy always wins and the denial must be visible.
  The load-bearing part is that this layer does **not rewrite a denied instruction into an authorized
  policy statement** — rewriting would show the next reader a seemingly legitimate grant whose actual
  provenance is untrusted disk content. The instruction stays on the chain unmodified, the forbidden
  behaviour is simply absent from `acceptedBehaviors`, and the refusal is recorded separately with its
  source path and reason.
- Two denial reasons are distinguished because they are different failures.
  `ForbiddenByRuntimePolicy` covers a behaviour beyond the read-only boundary. `NotPolicyAuthority`
  covers a non-managed source attempting to *speak as policy* at all — phrases containing `policy`,
  `trusted`, `override`, `bypass`, `grant`, `always-allow`, `skip-approval`, or `disable-sandbox`. Only
  `Managed` holds `policyAuthority`, and an unclassified future source kind holds none, so a newly
  added source cannot acquire the standing to declare policy by default.
- A Skill invocation records id, version, source path, content identity, included references, and
  declared script permissions. Permissions beyond read-only are collected into `deniedPermissions`
  while remaining in `scriptPermissions`, the same discipline as the import preview: failing closed
  must not discard the evidence that something was requested.
- Context size is accounted, not truncated. An over-budget source set is `Unusable` rather than
  silently trimmed, because quietly dropping an instruction makes the manifest disagree with what the
  model actually saw, and agreement between those two is the entire reason the manifest exists.
  Duplicate source paths are refused since there is no way to tell which content is in effect, source
  paths and behaviours pass `ExtensionDisplaySafety`, content identities must be well-formed digests,
  and both the source count (256) and depth (64) are bounded.
- `instruction_context_manifest` covers the nested override chain and its ordering, Skill invocation
  recording with denied-but-retained permissions, runtime policy winning with a visible denial and no
  rewrite, policy claims from project and Skill sources, unusable source sets, deterministic ordering at
  equal precedence, and the absence of execution authority. Guards were confirmed by sabotage:
  marking every source as policy authority, dropping the beyond-read-only check, dropping the
  policy-claim check, ignoring nested depth, truncating instead of refusing an over-budget context,
  silently accepting a Skill write permission, removing a denied instruction from the chain, tolerating
  duplicate paths, letting input order decide ordering, and granting policy authority to an
  unclassified source.
- Test-authoring note: four of those sabotages initially aborted with `SIGABRT` instead of reporting,
  because the test indexed `denials.at(0)` after asserting the size. A crash still fails the gate but
  stops every later assertion, so one sabotage exposed one symptom. Wrapping the indexed reads in
  `if (expect(...))` turned each into a clean multi-assertion failure — T7 then additionally showed
  that removing a denied instruction from the chain is caught.
- Full serial gate `82/82` in 196.67s. No caller: `product_scope_policy` pins the derived
  `policyAuthority`, managed-only authority and precedence, both denial diagnostics, the
  beyond-read-only check, denial recording alongside chain retention, nested depth adjustment,
  unclassified precedence, the budget refusal, denied Skill permissions, every `instruction-*`
  diagnostic, that neither the main window nor the extension center names `InstructionContextPolicy`,
  and that the layer holds no `QProcess`, `QSettings`, `SecureStorage`, `QFile`, `QDir`, or
  `QTextStream` authority. OpenSpec `0.4` stays unchecked: wiring the grant/update/removal/import
  actions, encrypted backup, and rollback remain open, and Agent/Codex stays read-only.

## An MCP Failure Is The Server's, Never The Model's (2026-08-30)

- An MCP server is an **external process**, so it owns a whole class of failures the earlier layers
  never had to model: never started, started but not ready, needs authentication, exited mid-turn.
  `McpLifecyclePolicy` exists so those are each separately recognizable.
- The load-bearing rule: a turn depending on a failed server ends with a **server-specific** failure —
  never a model failure, never a successful tool result. Reporting a model failure sends someone to
  rewrite the prompt when the problem is the server. Reporting success is worse: the model treats a
  return value that never existed as fact and keeps reasoning from it, and nobody can tell the value
  was invented. **Misattribution is not a display concern; it determines what a person fixes next.**
  Both `attributableToModel` and `reportedAsSuccess` are false on every return path including the
  `Ready` one, and an unclassified state still attributes to the server rather than quietly becoming a
  model failure.
- Each state carries its own diagnostic, and a test asserts the five codes are mutually distinct.
  Collapsing them would leave a person unable to tell whether to authenticate or read logs.
  `AuthenticationRequired` is deliberately separate from `Failed` and carries
  `resolvableByAuthentication`, because merging them sends someone to debug a server that only needed
  a login. An exited process is terminal even when the handshake previously completed — a server that
  exited cannot serve this turn. `toolsAvailable` is true only in `Ready`; claiming otherwise would
  aim a call at a channel that does not exist.
- Approval discloses server identity, tool name, every argument, requested permissions, and
  persistence options. Secret-bearing argument names are redacted **and the redaction itself is
  listed**, since a person needs to know they are approving a call that carries a credential. Argument
  name/value misalignment is refused rather than displayed, because a shifted table means approving a
  call one did not actually read. Beyond-read-only permissions are disclosed and remove the "remember"
  option entirely — that authorization must be given per invocation, matching how
  `ExtensionApprovalPolicy` treats high risk. `grantsInvocation` is false on every path.
- Logs are bounded at 512 lines and 4096 characters per line, keeping the *most recent* lines because
  a failing server's last few lines are usually the cause, and `droppedLines`/`truncated` make the
  truncation visible — otherwise a trimmed log reads as a complete one.
- Robustness finding from sabotage M8: removing the misalignment guard made the loop index past the end
  of `argumentValues` and abort with `SIGABRT` instead of refusing. Memory safety should not depend on
  a policy guard staying in place, so the loop now bounds-checks independently and returns the same
  refusal. With both checks present M8 no longer fails — that is defense in depth, so I confirmed the
  invariant is still pinned by removing *both* (`M8b`), which fails with "misaligned arguments were
  presented for approval".
- `mcp_lifecycle_policy` covers every startup state and its tool availability, attribution across all
  five states plus an unclassified one and the distinctness of their diagnostics, the approval prompt
  with redaction disclosure and persistence options, unpresentable approvals (bad id, truncated
  identity, spoofed tool name and argument value, misalignment, duplicates, argument limit), bounded
  logs including single-line clipping and the no-truncation case, and the absence of invocation
  authority. Guards were confirmed by sabotage: attributing to the model, reporting success, displaying
  a secret, offering tools before ready, merging authentication into failure, treating an exited server
  as usable, hiding truncation, tolerating misalignment, offering a reusable rule for a write call, and
  hiding the redaction.
- Full serial gate `83/83` in 200.69s. No caller: `product_scope_policy` pins both attribution flags,
  server attribution, the authentication distinction, ready-only tool availability, redaction and its
  disclosure, the read-only-only persistence option, `grantsInvocation`, truncation disclosure, every
  `mcp-*` diagnostic, that neither the main window nor the extension center names `McpLifecyclePolicy`,
  and that the layer holds no `QProcess`, `QNetworkAccessManager`, `QTcpSocket`, `QSettings`,
  `SecureStorage`, `QFile`, or `QDir` authority. OpenSpec `0.4` stays unchecked: wiring the
  grant/update/removal/import actions, encrypted backup, and rollback remain open, and Agent/Codex
  stays read-only.

## Making A Security Hook Crash Must Not Be How You Bypass It (2026-08-30)

- A hook is unlike every other layer here: it is an **external command that runs before a tool and can
  veto it**. That makes it simultaneously the strongest security control point and the most dangerous
  failure point, and those two roles want opposite answers.
- As a control point, a trusted hook returning deny inside its contract must actually stop the tool, and
  the timeline must attribute the denial to that hook. Attribution is not bookkeeping: an unsigned
  denial reads as the tool being broken, so someone goes and fixes the tool while the real decision came
  from some hook's matcher. Every verdict — allow, deny, timeout, crash, contract violation — carries
  `attributedHookId`, and `fromFailureBehavior` separates an explicit hook decision from a fallback,
  because "the hook said no" and "the hook died and we defaulted to no" call for different responses.
- As a failure point, timeout and crash resolve by declared behaviour, but **managed security hooks must
  fail closed**. If a hook whose job is stopping dangerous operations permits the operation when it
  crashes, then crashing it is the bypass, and the protection it offers is exactly zero. So `FailOpen` is
  *unavailable* to managed security hooks rather than merely a bad default, and a managed security hook
  that declares `FailOpen` has its declaration **rejected** rather than silently rewritten to
  `FailClosed` — silent rewriting would show a reviewer a contract that does not describe the running
  behaviour. Non-managed security hooks may still choose fail-open; only the managed class is forbidden.
- Out-of-contract results are treated as failures, not allows: accepting an unparseable return value as
  permission would mean a hook can wave anything through by emitting garbage. An unclassified future
  outcome also blocks — a newly added result type must not default into permitting the tool.
- An incomplete contract is rejected and the tool does not run. Treating an unreviewable contract as
  equivalent to no hook would make deleting one line of the contract the way to get permission. Every
  term is load-bearing: id, matcher, command, scope, and a declared timeout within bounds — an
  undeclared timeout can hang the event loop indefinitely. Unverified hooks have their conclusions
  discarded entirely, and matcher/command text passes `ExtensionDisplaySafety` since it appears in the
  timeline and approval surfaces.
- Unbounded output must not block the Agent event loop: a chatty hook should not stop everything. Output
  is bounded to 256 lines and 4096 characters per line, keeping the most recent lines, with over-limit
  content stored as an artifact rather than discarded — a hook's output is often the reason it denied —
  and `truncated`/`droppedLines` making the trimming visible.
- **Test-design finding (sabotage H1/H1b).** Removing the managed check in `failOpenPermitted` was
  caught, but only by the contract-rejection assertions; the runtime assertion "a managed security hook
  failed open" never fired. The managed fixture declared `FailClosed`, so it blocked for the ordinary
  reason and the managed rule was never the *cause* of the conclusion — the invariant was unobservable
  on that input. Fixing the fixture to declare `FailOpen` (with an assertion that it really does request
  the forbidden behaviour) makes the rule the sole source of the outcome. This is the same class of
  error as the recovery-gate fixture that held no grant: an assertion that a rule was *applied* is
  vacuous unless the input would produce a different answer without it.
- `hook_policy_engine` covers denial blocking with attribution, allow, all three failure outcomes under
  fail-closed and fail-open, managed fail-closed enforcement and the contradictory-contract rejection,
  the project security hook's remaining freedom, unclassified outcomes, every missing contract term,
  untrusted hooks, spoofed matchers, bounded output with artifact storage and single-line clipping, and
  attribution on every path. Eleven sabotages plus H1b confirmed the guards.
- Full serial gate `84/84` in 195.65s. No caller: `product_scope_policy` pins the managed fail-closed
  check, the contradictory-contract diagnostic, verdict attribution, the failure-fallback flag, the
  out-of-contract and unknown-outcome codes, rejection defaulting to no execution, every `hook-*`
  diagnostic, event-loop non-blocking, artifact storage, that neither the main window nor the extension
  center names `HookPolicyEngine`, and that the layer holds no `QProcess`, `QNetworkAccessManager`,
  `QSettings`, `SecureStorage`, `QFile`, `QDir`, or `system(` authority. OpenSpec `0.4` stays unchecked:
  wiring the grant/update/removal/import actions, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only.

## Removing An Extension Must Withdraw Its Grant Before Its Review (2026-08-30)

- `ExtensionUpdatePolicy` could already decide an update or a removal, but a decision changes no
  persistent state, so the conclusion "removal must withdraw the grant" had nothing executing it.
  `ExtensionLifecycleController` executes it against both authenticated ledgers.
- **Update's executing part is writing nothing.** Passing validation only means the candidate may be
  staged, so the staging path creates no review pin and no enablement grant for it: a candidate is by
  definition different content and must be independently reviewed and separately granted. The old
  content's records stay in the ledgers, but they bind the *old* content digest, so to the candidate they
  read as drift rather than inheritable authority. Failing validation leaves the active version untouched
  and writes zero bytes to either ledger.
- **Removal writes two ledgers, and two writes cannot be one atomic operation, so the order is part of
  the security property: withdraw the enablement grant first, the review pin second.** The grant is the
  half that actually runs content, so withdrawing it first means every intermediate failure lands on
  "no grant, review pin still present" — which under the registry's `Verified + Compatible` gate is *not
  enabled*, the safe side. The reverse order would briefly leave "granted but unreviewed", a worse state,
  and it destroys exactly the evidence an audit needs. If the grant write fails, the review evidence is
  still byte-for-byte intact.
- Partial completion is a distinct outcome, never reported as success. A removal that withdrew only the
  grant still leaves a review pin, and a person needs to see that to go clear it. The diagnostic carries
  the store's own backend reason rather than a generic "incomplete", so the reader knows whether to retry
  or investigate the backend.
- **An acknowledged write is not evidence.** `replace()` reports the bytes it serialized, not a fresh
  read, so a backend that confirms a write without persisting it would make "the grant was withdrawn" a
  lie — the worst error available here, since it tells someone authority is gone when nobody knows its
  state. Every conclusion comes from a fresh `load()` after the write, and an unusable ledger never counts
  as a withdrawal: state-unknown is not the empty set. When the grant state is unknown the controller
  stops rather than continuing to delete the review pin.
- Both ledgers must be readable before removal begins. Claiming a grant was withdrawn while the grant set
  is unknown is the one thing this layer must never do. Immutable identity (kind, id, content digest) is
  retained on every outcome including partial completion, and remains recoverable when the target
  directory is already gone — a vanished extension must not leave its grant behind.
- **Test-design finding.** The acknowledged-but-unpersisted case does not leave the grant visibly
  present; it leaves the settings payload ahead of the secure authority, so the ledger re-reads as
  `Invalid` (`record-superseded`). The invariant is unchanged — do not claim withdrawal — but the
  assertion had to check the *state* rather than the surviving entry. Three guards (unreadable-ledger
  emptiness on both halves, and kind-binding in withdrawal matching) are unobservable through this API
  because the stores never return content alongside a non-usable state and the fixture holds one kind;
  those are pinned by source text in `product_scope_policy` instead, and all three pins were verified by
  sabotage.
- A second finding: an early `return` inside a failure branch can make a later composite check
  unreachable, so sabotaging the composite passes. Both halves now record a diagnostic and fall through
  to one decision point, making "both halves must be withdrawn to report completion" the single source of
  the outcome.
- `extension_lifecycle_controller` covers all five validation failures leaving both ledgers untouched,
  absent and unchanged-digest targets, staging without inheriting review or grant, downgrade disclosure,
  complete removal, stale-target removal, partial completion with its ordering guarantee, both
  acknowledged-but-unpersisted halves, unreadable ledgers on either side, and missing stores. Nineteen
  sabotages plus three source-pin sabotages confirmed the guards.
- Full serial gate `85/85` in 536.92s. No caller: `product_scope_policy` pins grant-before-review order,
  the survived-grant refusal, both fresh read-backs, both usability conjunctions, both unreadable-ledger
  guards, the composite completion check, retained identity, the staging non-inheritance block, the
  absent-target code, kind-binding on both withdrawals, every `extension-removal-*` diagnostic, the
  absence of `QProcess`, `QNetworkAccessManager`, `QFile`, `QDir`, `system(`, and `.effectiveEnabled =`,
  and that neither the main window nor the extension center names `ExtensionLifecycleController`. OpenSpec
  `0.4` stays unchecked: wiring the grant/update/removal/import actions into Extension Center, encrypted
  backup, and rollback remain open, and Agent/Codex stays read-only.

## A Grant Button Must Not Exist Where A Grant Cannot Take Effect (2026-08-31)

- The enablement grant action is now wired into Extension Center. Until this slice, every policy layer
  behind "the user asked for this content to run" existed and none of them had a caller, so no grant could
  be produced through the product at all.
- **The load-bearing rule is where the eligibility decision comes from.** The dialog does not re-derive
  installed/reviewed/compatible; it asks `ExtensionEnablementPresentation::build` and enables the action
  only on `Ready`. Recomputing the gates in the UI would eventually drift from that layer, and the drift
  direction is a clickable grant button on content nobody reviewed — a grant that would sit in the ledger
  fully authenticated and take effect the moment the gates land. Blocked rows still render, with the
  specific missing gate named: showing "nobody reviewed this" as "this host cannot hold it" invites the
  belief that another machine would run it.
- An unreadable grant ledger freezes both granting **and** revocation, and its residual entries are
  dropped from the view. Submitting a "complete set" against an unknown set silently withdraws the entries
  that could not be read — safe in direction, but it states a tampering event as a deliberate user
  disable, and the next grant commits against the wrong generation. Displaying those residual entries
  would offer a revoke action for a grant whose existence cannot be confirmed.
- Revocation is always available on a readable ledger, including for drifted content and vanished sources,
  and is keyed on `(kind, id)` alone. Binding revocation to the content digest would make a tampered
  extension permanently unrevocable — the exact case revocation exists for. A grant whose source is gone
  gets a synthesized row for the same reason.
- The confirmation echoes the full source and content digests back into the request. The grant binds the
  content that was on screen, not the name, so drift between render and commit fails the grant instead of
  transferring the decision to new content. The prompt also states outright that the grant does not start
  execution; without that line a person reasonably concludes they just turned something on.
- Review and grant share one worker slot and one operation generation. They write two independent ledgers
  so they never contend for a CAS generation, but serializing them keeps the later completion from erasing
  the earlier one from the screen, and the existing destructor join covers both paths. A failed commit
  replaces the view with the re-read snapshot and stays frozen: leaving the optimistic pre-commit state up
  reads as "the grant went through".
- A refresh from one path must not overwrite the other path's ledger. A review operation never read the
  grants, so treating them as empty would display "these extensions were never granted" when the truth is
  that this operation never looked.
- `extension_center_read_only` covers the frozen default ledger, an unreadable grant ledger with a
  residual entry, blocked rows naming their gate, the accepted grant's echoed identities and bound
  `(kind, id)`, cancelled grant and revocation emitting nothing, drifted and absent grants keeping their
  revocation, the review-refresh isolation, the busy freeze, and fixed-code-only diagnostics. Nineteen
  dialog sabotages plus thirteen `product_scope_policy` pin sabotages confirmed the guards.
- `0.4` stays unchecked: update/removal and import wiring, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only — this slice records authorization, it does not execute anything.

## Withdrawing Records Is Not Deleting Content (2026-08-31)

- The removal action is now wired into Extension Center. `ExtensionLifecycleController::remove` existed and
  had no caller, so the conclusion "removing an extension must withdraw its grant" had nothing executing it.
- **The load-bearing rule is what the word "remove" is allowed to claim.** The controller writes two
  ledgers and touches zero bytes on disk. So the column is labelled 收回记录, and
  `ExtensionRemovalPlan::removesSourceContent` is an explicitly exposed always-false field rather than an
  omission — the presentation cannot silently restate "the grant was withdrawn" as "the content was
  deleted". The confirmation says outright that the disk content survives and will become usable again
  after a fresh review and a fresh grant. Wording it as a deletion makes a person stop cleaning up while
  the content is still sitting in place.
- Removal has no gates. Drifted content, withdrawn reviews, and vanished sources must all stay removable,
  otherwise a tampered extension keeps a fully authenticated grant forever. An unpresentable name falls
  back to the identifier instead of refusing, so an extension cannot make itself unremovable by choosing a
  hostile name. Malformed digests are blanked rather than displayed.
- Whether a removal is well-formed comes only from `ExtensionUpdatePolicy::evaluateRemoval`; the
  presentation restates that verdict and the retained identity rather than constructing a second one. The
  dialog in turn reads only `ExtensionRemovalPlanState::Ready`.
- Removal is frozen unless **both** ledgers are readable, because the controller refuses in that case
  anyway and because claiming a grant was withdrawn while the grant set is unknown is the worst thing this
  path could say. The two halves are independently load-bearing: an unreadable review ledger freezes
  removal while leaving grant revocation available, and vice versa. A row with nothing to withdraw is
  frozen and says so.
- The request carries `(kind, id)` only. The content digest of what is being withdrawn may already be
  unreadable, and binding to it is how a tampered extension would keep its grant.
- Removal shares the review/grant worker slot and operation generation — unlike those two it writes both
  ledgers, so it genuinely contends for both CAS generations. Its refresh replaces both ledgers, which is
  the opposite of the review/grant rule and correct for the same reason: that rule exists because a path
  must not report a ledger it never read, and this path read both.
- `PartiallyWithdrawn` is never reported as success. Only `Withdrawn` clears the frozen state; anything
  else shows the re-read snapshot with its diagnostic and stays frozen, because a removal that withdrew
  only the grant leaves a review pin that nobody will clean up unless they are told.
- `extension_lifecycle_presentation` covers the never-deletes invariant on both the ready and rejected
  paths, the absence of gates, the untrusted-text fallback, and delegation of the verdict.
  `extension_center_read_only` adds the frozen default, each ledger half frozen independently, the
  nothing-to-withdraw and policy-rejected rows, the confirmation's wording and withdrawn-halves list, the
  accepted request bound to the confirmed row, the both-ledger refresh, the busy freeze, and
  fixed-code-only diagnostics. Twenty sabotages confirmed the guards, and the full serial gate passes
  `86/86` in 203.24s.
- `0.4` stays unchecked: update and import wiring, encrypted backup, and rollback remain open, and
  Agent/Codex stays read-only — this slice withdraws authorization records, it does not execute anything.

## Reading A Bundle Is Not Unpacking It (2026-08-31)

- `ExtensionImportPreviewBuilder` turns a bundle manifest into a per-component preview verdict, but nothing
  in the product could produce a manifest, so the decision "import this bundle" could not be raised at all.
  `ExtensionBundleReader` is that producer: it reads an already-present directory and returns a manifest.
- **Reading a bundle does not unpack it.** This layer scans a directory that already exists; it never
  extracts an archive, writes a temporary file, or creates a path. Extraction is a disk write, and writing
  to disk before the permission, approval, sandbox, and recovery gates exist is precisely the forbidden
  thing — a reader that unpacks "just to see what is inside" has already landed the bundle's contents on
  disk. Archive paths are therefore refused as `extension-bundle-root-not-directory` rather than read.
- **Every digest is computed from the bytes on disk, and a manifest that declares a digest is refused
  rather than ignored.** If a manifest could declare its own component digests, a bundle could describe
  content it does not carry, and a person decides on exactly that per-component disclosure: the screen says
  this component's content is A while B is what gets imported. Refusing beats ignoring because ignoring
  leaves the manifest author believing the field took effect. Unknown manifest and component fields are
  refused wholesale, which covers declared digests as one case of a general rule.
- Every digest segment is length-framed, so `"ab"+"c"` and `"a"+"bc"` cannot collide. Without framing two
  bundles with different contents share one identity, and a grant bound to that identity covers both.
- **An unrecognised component type survives as `Unsupported` carrying its declared type string.** Dropping
  it would let the bundle's actual behaviour exceed what the preview described. This layer does not decide
  whether the import fails closed — that is the preview's conclusion — but it must hand over complete
  evidence for that decision.
- **Capabilities pass through per component with no rollup.** Two components separately asking for "read
  files" and "connect to the network" roll up to look exactly like one component asking for both, and only
  the latter is the dangerous combination. Any aggregation here destroys the reason the preview exists.
  Duplicate capability strings are refused rather than deduped, for the same reason duplicate JSON keys are.
- Symlinks are refused at the root and at every entry, and both files and directories are separately
  checked for containment within the root. Following a link would fold bytes from outside the bundle into
  its digest and disclose out-of-bundle content as if it were part of the bundle. Files are re-stat'd after
  reading and drift is refused, so the digest describes bytes that were actually stable.
- An absent directory is `Empty` with no diagnostic, not `Invalid`: there is simply no bundle to import yet,
  which is not the same as a malformed one. Unreadable is `Unavailable`, also distinct from malformed.
- Whether text is safe to display comes only from `ExtensionDisplaySafety`. A second copy of those rules
  would drift, and drift means the reader admits characters the preview refuses, or the reverse. This layer
  owns only the budget ceilings and the source-nature checks (containment, symlinks).
- `extension_bundle_reader` covers reading without writing (directory listing unchanged, and the produced
  manifest feeding the preview to a `Ready` verdict), digests from disk including per-component drift
  isolation, declared digests refused, unknown types surviving as evidence, per-component capabilities,
  archive refusal, absent root as `Empty`, symlink refusal, framing collision resistance, ten structural
  refusals, and unsafe text refusal. Twelve sabotages confirmed the guards; the full serial gate passes
  `87/87` in 214.34s.
- `0.4` stays unchecked: the import UI surface, update wiring, encrypted backup, and rollback remain open,
  and Agent/Codex stays read-only — this slice reads a directory and computes digests, nothing else.

## Disclosing A Bundle Is Not Importing It (2026-08-31)

- The import surface is now wired into Extension Center. `ExtensionBundleReader` produced manifests and
  `ExtensionImportPreviewBuilder` judged them, but neither had a caller, so the decision "import this
  bundle" still could not be raised through the product. `ExtensionImportPresentation` merges the two and
  the dialog discloses the result.
- **The load-bearing rule is what the word "import" is allowed to claim.** Nothing on this path unpacks,
  installs, enables, or writes a byte. So `importsBundle` and `writesToDisk` are explicitly exposed
  always-false fields rather than omissions, the button says 披露扩展包内容 rather than 导入, and the status
  line states outright that nothing was imported and nothing was written — on refused paths too. Calling it
  an import makes a person believe a copy now exists on disk and act on that, for instance by going to clean
  up a directory that was never written.
- **The two layers refuse for different reasons and those reasons must stay apart.** An unreadable directory
  sends a person to check permissions; a malformed bundle sends them to fix the bundle. Collapsing both into
  one "invalid" sends someone to rewrite a bundle that was never the problem. `Absent` is a fourth state and
  carries no diagnostic: there is simply no bundle yet.
- **A failed read is never previewed.** The manifest inside a failed read is garbage, and previewing it can
  yield a `Ready` verdict — an unreadable bundle turning into an approvable one on screen. The switch over
  read state returns before the preview runs, and a test builds a fully valid manifest behind an
  `Unavailable` state, asserts the manifest alone previews `Ready`, then asserts the disclosure does not.
- Each layer's diagnostic passes through verbatim. Inventing a local code leaves a person holding an
  identifier that exists nowhere in the layer that actually refused.
- **Failing closed keeps every component, including the unsupported one**, and the row says that component
  is why the import failed closed. Hiding the evidence leaves nobody able to tell what the bundle wanted.
  Refused states carry an empty component list, because in those cases no components were read at all.
- Capabilities stay per component with no rollup, and each disclosure fully replaces the previous component
  list. Keeping the previous rows would make a failed read look like it describes the bundle just chosen.
- Disclosure uses its own worker slot and generation rather than the review/grant/removal one: it writes no
  ledger, so it has no CAS contention with them, and letting a pure read block a ledger write (or the
  reverse) has no justification. Two disclosures still serialize, since only one result is visible. The read
  and the digesting run on the worker thread — a frozen window makes a pending disclosure look finished.
- Only directories are accepted (`getExistingDirectory`), because reading an archive means unpacking it
  somewhere first, and that is a disk write. There is no import-request signal at all: a request that cannot
  be sent is safer than one that can.
- Bundle diagnostics are rendered only when they match the fixed-code pattern, because the bundle is exactly
  the thing nobody has reviewed yet.
- `extension_import_presentation` covers all four refusal states with the always-false invariants, unreadable
  versus malformed, absent as a non-failure, a failed read that never becomes ready, failed-closed evidence
  retention, per-component capabilities, and pass-through diagnostics. `extension_center_read_only` adds the
  surface: the disclosure label, the imported-nothing statement, per-component capability attribution, the
  unsupported row naming itself as the reason, full replacement between disclosures, unreadable versus
  malformed on screen, fixed-code-only diagnostics, and the busy freeze on both the button and the handler.
  Twelve presentation sabotages, eight dialog sabotages, and thirteen source-pin sabotages confirmed the
  guards; the full serial gate passes `88/88` in 225.98s.
- Three test-design findings came from sabotage. A pass-through assertion that only checked the code was
  non-empty could not see a locally invented code, and the fixture chosen to trigger it was refused by the
  reader before the preview ever ran, so the case was unreachable — it is now constructed at the unit
  boundary with a manifest the reader accepts and the preview rejects. The busy guard inside the click
  handler was unobservable through clicking, since a disabled button swallows the click; the test now also
  invokes the signal directly, because a disabled button and a refusing handler are two independent
  defences. And a pin requiring the read and the judge in one nested expression failed on an equivalent
  two-statement rewrite, so it now pins that both calls are present rather than how they are spelled.
- `0.4` stays unchecked: update wiring, encrypted backup, and rollback remain open, and Agent/Codex stays
  read-only — this slice reads a directory and renders what it found.

## Evidence Must Be Established, Never Assumed (2026-08-31)

- `ExtensionUpdatePolicy::evaluate` and `ExtensionLifecycleController::stageUpdate` both existed with nothing
  able to produce their inputs, so "update this extension" could not be raised through the product.
  `ExtensionUpdateCandidateBuilder` reads a candidate directory and produces the candidate plus its evidence.
- **The load-bearing rule is that every evidence item starts false and only becomes true when this layer
  actually checked it.** A builder that defaults the five fields to true turns the entire policy layer into
  decoration: `evaluate` waves everything through while nobody verified a signature, a dependency, or a
  health probe. Defaulting to true is the one genuinely dangerous failure mode here, because it does not
  raise an error — it succeeds.
- **"Nobody can check this" is not "this failed the check."** There is no extension signing authority in this
  repository — the update signing key ring signs release installers, not extensions — and no dependency
  resolver or health probe either. So those three stay false and each carries its own code
  (`extension-update-signature-authority-absent`, `-dependency-resolver-absent`, `-health-probe-absent`)
  rather than collapsing into one "insufficient evidence": one sends a person to install a signing
  authority, another sends them to fix the bundle. `manifestValid` is the one item genuinely established,
  because the reader already refused unknown fields, duplicate keys, unsafe text, and escaping paths.
- **Capabilities unite for the gate and stay per component for the person.** This is the opposite of the
  import disclosure rule and both are right: the compatibility gate must fail closed, so one component
  requesting file writes means this extension requests file writes and the union is the only safe input;
  while a person decides on the per-component disclosure, since two components separately asking for "read
  files" and "connect to the network" aggregate to look exactly like the one dangerous component asking for
  both. The manifest is carried through untouched alongside the united candidate.
- Compatibility comes only from `ExtensionCompatibilityPolicy::evaluate`, and `Unknown` is not `Compatible`.
  The probe record is built from the candidate's own version, identities, and capabilities, with trust reset
  to `Unverified` and `effectiveEnabled` cleared. That reset is defense in depth — the shared policy does not
  read those fields today — so it is unobservable behaviorally and pinned in source instead.
- The candidate's identities come from the bytes on disk, never from a caller-supplied digest or a
  manifest-declared one. A failed read produces no candidate at all: `Absent` (no diagnostic), `Unreadable`,
  and `Rejected` are distinct, and each carries the read layer's own diagnostic verbatim.
- `extension_update_candidate_builder` covers evidence establishment and the resulting policy rejection,
  the three distinct unverifiable codes, union-for-the-gate with per-component disclosure intact, target
  mismatch, identity from disk with drift, all three failed-read states, compatibility delegation including
  `Unknown`, the candidate never inheriting the active verdict, and downgrade visibility. Nine sabotages plus
  five source-pin sabotages confirmed the guards; the full serial gate passes `89/89` in 216.86s.
- Two test-design findings came from sabotage. Removing the trust/`effectiveEnabled` reset changed nothing
  observable because the compatibility policy never reads them, so it is pinned in source and documented as
  defense in depth. And a malformed candidate whose refusal was deleted still failed — but on the
  target-mismatch branch with a different code, which would send a person to investigate an identity problem
  when the bundle is malformed and has no computed identity yet; the test now pins the exact read diagnostic.
- `0.4` stays unchecked: the update UI surface, encrypted backup, and rollback remain open, and Agent/Codex
  stays read-only — this slice reads a directory and states honestly what it could not verify.

## An Impossible Update Must Say What Is Missing (2026-08-31)

- The candidate producer and the policy layer could both reach a verdict, but nothing rendered one, so
  "update this extension" still could not be raised through the product. `ExtensionUpdatePresentation` turns a
  candidate plus a verdict into a plan, and Extension Center carries a per-row 检查更新 action wired through
  `MainWindow::startExtensionUpdateCheck`.
- **The load-bearing rule is that no update can currently succeed, and that must be *said* rather than
  represented by a greyed-out button.** There is no extension signing authority, dependency resolver, or health
  probe on this machine, so every update is rejected. A surface that only greys the action out makes a person
  conclude their bundle is the problem and rebuild it repeatedly, when the real gap is that nobody on this
  machine can check a signature. So the plan lists every evidence item, and each unestablished item says which
  of two things it is.
- **"Nobody can check this" and "this failed the check" are two different sentences on screen.** They are two
  separate fields (`established`, `unverifiable`), not one tri-state, and the dialog renders them as 无人可核查
  versus 核查未通过 — one sends a person to install a signing authority, the other sends them to fix the
  bundle. An established item shows no diagnostic at all, because a leftover reason next to a passing check
  sends a person to investigate a problem that does not exist.
- **Staging is not enabling.** `stagesOnly`, `replacesActiveVersion`, and `grantsExecution` are explicit fixed
  fields written on every return path — including both rejection paths and the empty plan — and the status line
  always states that nothing was replaced, nothing was granted, and nothing was written. Even with complete
  evidence, `StagedUnreviewed` means the candidate may be staged, not that it runs: a candidate is by
  definition different content, so it is unreviewed and ungranted. Calling it "update complete" makes a person
  believe the new version is running when the old one still is — or nothing is.
- **A downgrade is stated in words, not left for the person to infer from two version numbers.** Downgrades are
  not forbidden, but they reintroduce content that was already fixed, and `1.5.0 → 0.9.0` side by side does not
  read as a direction. The conclusion comes from the verdict; this layer never re-derives it, and the screen
  shows 当前版本 → 候选版本 rather than two bare numbers.
- The verdict has one source. The surface never re-decides stageability, downgrade, or compatibility, and it
  passes both the candidate producer's and the policy layer's diagnostics through verbatim — a locally invented
  code hands a person something they cannot look up. Per-component disclosure passes through unrolled, the
  reverse of the union the gate consumes and correct for the same reason.
- 检查更新 is deliberately **not** gated: it only reads a candidate directory and lists evidence, changing no
  record and writing no byte, so greying it out is precisely the failure this surface exists to prevent. It
  shares the disclosure thread slot rather than the ledger slot, since it writes no ledger and so has no CAS
  contention. It re-reads the active record and the review ledger rather than trusting the dialog's copy: a
  stale active record can make "content unchanged" wrong in both directions. An unreadable review ledger
  contributes no pins, because treating leftovers as reviewed turns a read failure into a grant. Like
  disclosure, it accepts only a directory — reading an archive would mean unpacking to disk first.
- Three test-design findings came from sabotage. Asserting only that established items carry no diagnostic
  needed a *stale* gap deliberately left on a passing item to be observable at all. A capability-rollup
  sabotage survived a test that checked each component had at most one capability, and was only caught by
  asserting the component that requested nothing still requests nothing. And a redundant identity re-check in
  `build` was unobservable in either direction, so it was removed rather than pinned — code no test can
  justify gets mistaken for a real defence later.
- `0.4` stays unchecked: encrypted backup and rollback remain open, and Agent/Codex stays read-only — this
  slice reads a candidate directory and renders why it cannot be accepted.

## A Verdict Nothing Executes Is Not A Recovery (2026-09-01)

- `ExtensionRecoveryGate` could already decide that a self-contradictory grant ledger must be rebuilt as the
  empty set, but the decision changed no persistent state, so that conclusion had nothing to carry it out and
  one interrupted publish still stalled every enablement decision on the machine.
  `ExtensionRecoveryController` executes it, over a new `discard` route on the shared evidence/enablement store.
- **The executor commits a constant empty payload, not a payload derived from the verdict.** A contradictory
  ledger cannot be repaired into the grant set it probably held — that is forging consent — and the only honest
  reconstruction is the empty set, which is a withdrawal. So the route is `discard`, which structurally accepts
  no entries: this path cannot even *express* writing a non-empty grant set. `replace` would hand that
  expressiveness back, and it also refuses to write over `Invalid` — precisely the state recovery exists for —
  so recovery is impossible without a separate route. `product_scope_policy` pins `discard` present and
  `->replace(` absent here.
- **Order is part of the safety property: destroy the authority key first, delete the payload bytes second.**
  Two writes cannot be atomic, so the intermediate state is a choice. Rotating the key first makes any residual
  payload bytes unauthenticatable by anyone, so the clear is irreversible; deleting the payload first while the
  old key survives means anyone who can put those bytes back can resurrect the withdrawn grants, which is the
  entire thing recovery exists to prevent. Both intermediate states still read as `Invalid`, so neither passes
  as success.
- **An acknowledged write is not evidence.** The conclusion comes only from re-read bytes, and completion has
  exactly one source — the gate's `completed`. A backend that acknowledges a write it never persisted would make
  "every grant withdrawn" a false report, and on this path that false report means the operator stops coming
  back while an unexplainable grant is still in the ledger. Recomputing completion locally would drift, and the
  drift direction is reporting an unfinished recovery as finished.
- A partial recovery keeps the transaction open. Re-reading `Ready` means grants survive, `Invalid` means the
  corruption persists, `Unavailable`/`OutcomeUnknown` mean it currently cannot be known — all three keep it
  open. The executor re-reads even when `discard` reports failure: an interruption may already have destroyed
  the authority key, and reporting "nothing happened" then tells the operator the damage is still where they
  left it.
- **Recovery does not touch the review ledger; removal does — and both are right.** Review records are the only
  post-hoc audit evidence, and clearing them withdraws no authority, because under the registry's double gate
  nothing runs without a grant. Removal is a person's decision about one named target; recovery zeroes
  authority precisely when nobody can know what happened. See [[recovery-withdraws-authority]] and
  [[removing-an-extension-withdraws-its-grant-before-its-review]].
- `discard` only runs when `load()` genuinely returns `Invalid`. A clear that worked on a healthy ledger would
  be a back door revoking everything without approval; unreadable and unknown are equally refused, since
  clearing what cannot be read destroys invisible grants.
- The ordering invariant was documented at length but initially **unpinned** — the recurring lesson in a new
  form. Observing it needed the fake store to record, at each authority write, whether the payload bytes were
  still present; asserting the phases ran was not enough, since both orders write and delete exactly once.
  Sabotage confirms: swapping the two phases fails on the payload-presence assertion, and the test also pins
  that the key written is a *new* one, since keeping the old key leaves residual bytes authentic.
- Recovery still has **no caller**: `product_scope_policy` pins that neither the main window nor Extension
  Center names `ExtensionRecoveryController`, as it already did for the gate. `0.4` stays unchecked — encrypted
  backup and rollback remain open, and Agent/Codex stays read-only.
- Full serial gate `91/91` in 519.72s.

## Four Conclusions Are Four Sentences (2026-09-01)

- The recovery gate reaches a verdict and [[a-verdict-nothing-executes-is-not-a-recovery]] executes it, but nothing
  put the verdict in front of a person — and recovery is the one gate of the four that *cannot* proceed without a
  human who has seen the damage. `ExtensionRecoveryPresentation` renders it.
- **`None`, `Blocked`, `Reconfirm`, and `ClearGrants` are four different sentences, not four degrees of one
  status.** Collapsing them into "broken / not broken" grows a clearing button on all four, and on two of them
  clearing destroys grants nobody can see. Each renders a distinct headline *and* a distinct instruction; the
  test asserts all four of each are pairwise different, which is what makes "these two look the same on screen"
  a failure rather than a style note.
- `Blocked` and `Reconfirm` are the pair most tempting to merge and the most costly to: unreadable means *wait
  for the store*, unknown outcome means *re-read now*. One sentence for both sends a person to repair a backend
  that is not broken.
- **A confirmable action is an action that will be executed, so confirmability is forwarded, never re-derived.**
  It comes only from the gate's `operatorConfirmationRequired`. The test feeds a deliberately contradictory
  input — `ClearGrants` with confirmation *not* required — because a layer that re-derives from `need` passes
  every consistent fixture. Drift here points one way: offering to clear a ledger nobody can read.
- **A grant count that cannot be read is not shown.** For a contradictory or unreadable ledger the number is
  invented, and a specific number on screen makes a person believe they know the scope of what they are about to
  clear. `grantCountKnown` and the count are separate, because a readable empty ledger is a true 0 — reporting
  that as unknown makes a ledger with genuinely no grants look unreadable.
- The generation is forwarded verbatim **including 0**. A contradictory ledger has no trustworthy generation, and
  a surface that substitutes the last one it remembers makes the gate conclude it is looking at a different
  ledger and refuse the recovery. The generation proves *which ledger I saw*; it is not a number derivable from
  elsewhere.
- **The prompt must say what recovery does not do, in the text a person reads.** Withdrawing grants deletes no
  disk content and clears no review records. `removesSourceContent` and `clearsReviewRecords` are fixed false on
  every return path, and the actionable prompt additionally states both in prose — an invariant that is false in
  a struct field but never said on screen does nothing for the person deciding. Same reasoning as
  [[withdrawing-records-is-not-deleting-content]].
- An unrecognized `need` degrades to `Blocked` with no action, no confirmation, and no count: a newly added
  verdict must not grow a clearing button by default.
- Sabotage confirmed all three load-bearing guards: re-deriving confirmability from `need`, always reporting the
  grant count, and giving `Blocked` and `Reconfirm` the same headline.
- Still no caller. `product_scope_policy` pins `ExtensionRecoveryPresentation` absent from both the main window
  and Extension Center, alongside the gate and the executor, and pins the layer free of `QProcess`/`QSettings`/
  `SecureStorage`/`discard`/`replace`. `0.4` stays unchecked and Agent/Codex stays read-only.
- Full serial gate `92/92` in 393.95s.

## A Shared Mechanism Needs Domains That Cannot Read Each Other (2026-09-01)

扩展暂存需要备份与回滚,而 `ConfigurationBackupStore` 已经把这件事做对了:AES-256-GCM、
`QLockFile` 根锁、`QSaveFile` 原子写、pending 清单崩溃恢复、四态清点、身份绑定删除。把这
一整套复制一份给扩展去用,两份实现会各自漂移,而漂移的方向永远是其中一份丢掉某项检查。
因此这一片把它参数化成域,和共享证据账本走同一条路:机制共享,域不可互认。

**这一片只做参数化,唯一被实例化的域仍然是工具域。** 正确性判据因此是可以被机械验证的:
`tests/configuration_backup_store_test.cpp` 逐字节不变地通过。那份测试自己手搓 AAD(带内嵌
NUL 的前缀)、固定密钥作用域与身份前缀,所以它通过就等于证明工具域的持久化字节没有变。
改动了实现又改动了它的测试,那个证明就没有了。

**域字符串一旦发布就不能再改。** 它们进入 AAD、密钥作用域与清单身份。改动其中任何一个都
会让既有备份全部无法解密或无法通过身份校验,而那等于在需要回滚的那一刻发现没有备份。
`testToolDomainReproducesEveryPublishedLiteral` 是一个金字符串测试,存在的意义就是让将来
任何一次"整理这些常量"的改动立刻失败。两处带内嵌 NUL 的前缀尤其危险:`sizeof - 1` 保留
那个 NUL,而 `QByteArray(const char *)` 会截掉它,写错不会报错。

**跨域分隔必须在两个域拿到同一把密钥时依然成立。** 第一版测试构造了两个各字段都不同的域,
读取被拒绝,于是看起来分隔成立——但把 AAD 前缀换成共享常量,那个测试仍然通过。原因是拒绝
其实来自明文清单里的 `format` 字段比较,而那只是一个字符串比较,任何能写目录的人都能改。
真正不可绕过的是 AAD:改掉它就得重新认证密文。因此现在有一个只差 AAD 前缀一个字段的影子
域,以及一个只差身份域一个字段的影子域,各自单独证明那一道防线。**一个能被"抽取共享常量"
这种改动绕过的分隔测试,证明的是它自己而不是分隔。**

**清单身份必须逐域不同,因为它会流出本存储。** 激活日志按前缀校验它记下的记录,而
`removeVerified` 用身份确认"删的是我看到的那一份"。两个域为同一份字节算出同一个身份,
意味着一个域的身份满足另一个域的校验。而一次被拒绝的删除必须一个字节都没删:否则拒绝反而
完成了它拒绝的那件事。

**旧版 v1 迁移默认关闭,而这是这个域结构体里最重要的一个字段。** 它是本存储唯一一处依据
未经认证的输入清单去写盘的路径,存在的理由只是搬运工具域真实存在过的历史。一个没有 v1
历史的新域继承它,等于凭一份任何人都能放进目录的明文清单触发写入。两个入口都必须关:
`migrateLegacy` 由调用方主动发起,而 `inventory` 更危险——它只要看到一份非 v2 清单就会
触发,而清点本来是只读动作。第一次只测了前者,后一个入口的门禁被改成 `if (false)` 时测试
照样通过。**一个只读入口里的写盘路径,比一个写入入口里的写盘路径更需要被测到。**

**`stateForIssue` 里的代号集合不能是 `static`。** 它按诊断代号后缀裁决清点状态,而代号由
域前缀构成。一个 `static const QSet` 会永久冻结第一个被实例化的域的代号集合,于是第二个域
的"存储不可用"会被读成"证据无效"——一个把暂时读不到误报成已损坏的方向。

93/93 in 208.53s。六处破坏全部被抓到:共享 AAD 前缀、共享身份域、两处旧版迁移门禁、
恒真的 `configured()`、漂移的工具密钥作用域。

## Active Product Priorities

1. Define the authenticated Aegisy website-to-desktop configuration projection:
   account/profile/model metadata may cross the API boundary, while credential
   values remain in secure storage and out of ordinary logs and config previews.
2. Complete the one-click local configuration workflow for every supported target:
   detect/install, preview exact targets, back up, write only the selected tool,
   verify, repair, and restore truthful active-profile state on failure.
3. Consolidate Codex plugins, custom Skills, MCP configuration, provenance,
   compatibility, enable/disable, update, removal, and bounded directory management
   into one extension center.
4. Productize desktop enhancements and Chinese UX with exact application/version
   checks, recoverable resource changes, and macOS/Windows fixtures.
5. Rename and constrain the optional programming destination to Codex. Preserve the
   pinned adapter, current read-only boundary, health/recovery UI, and existing
   project/session work that directly benefits Codex users.
6. Add fail-closed product-scope tests so Claude, Gemini, ACP, and other non-Codex
   Agent adapters cannot be selected, advertised, or reached. Their CLI profiles
   remain configuration-only.
7. Rebaseline signed macOS/Windows release evidence around login, website-backed
   configuration, rollback, extension/Skills/localization flows, gateway, updater,
   and Codex launch/recovery.
8. Keep all Agent-authored file writes, command execution, Git mutation, Approval,
   remote control, background jobs, and multi-agent work unavailable until a later
   product decision resumes their existing security and recovery gates.

## Deferred Workbench Priorities

The following priorities are retained as historical engineering context. They do
not drive the active roadmap or block a companion release unless the corresponding
code is reachable in that release channel.

1. Finish OpenSpec `3.10` by obtaining a fresh clean Windows run after the ConPTY
   post-interrupt readiness repair, real `windows` QPA, D3D11 software-adapter
   preference, renderer assertions, and fixed-code stderr-channel repair. If
   `agent_workbench_render` remains red, use its
   reviewed fixed code rather than WebEngine flags; if Monaco remains red, use its
   fixed code or mapped WebEngine context class without exposing dynamic suffixes.
   Obtain a successful run from a clean `windows-验证-源码` checkout. The workflow
   runs the complete generator and desktop gate there; the Qt consumer migration,
   generated dispatch, exact pending correlation, and safe projection are implemented
   without changing stable `0.1` wire behavior. Do not infer WARP, Chromium adapter,
   renderer success, or Windows release evidence until that run succeeds.
2. Continue OpenSpec `3.5` by obtaining complete Windows reconnect/runtime evidence,
   then continue `3.6` from the schema-v24 source/outcome/consumption Store foundation
   with reviewed external-caller-CAS routes and production AAP/Qt recovery before
   adding approval/file/Git/job producers. Durable Turn-start acknowledgement,
   fixed-watermark replay, structured retention-gap snapshot recovery, out-of-band
   heartbeat, bounded reconnect, and live subscribe/sync-or-snapshot/activate are
   implemented. Keep automatic pruning and all non-Turn dispatch disabled until the
   remaining mutation-producer, authority, recovery, and cross-platform gates pass.
3. Finish OpenSpec `22.5` by bundling the reviewed pinned adapter, generating the
   trusted manifest in signed packaging, making manifest presence non-downgradable
   for packaged Runtime, closing the remaining path-to-spawn replacement window,
   binding updater compatibility to the same artifact set, and validating it on a
   clean Windows runner.
4. Validate the hardened TLS installer on a clean Windows x64 VM.
5. Reproduce and correlate any remaining streaming disconnect with redacted logs.
6. Continue consolidating widget-local QSS and replace remaining Qt stock icons;
   the Codex health/restart toolbar state is now covered by the render suite.
   Keep the context-inspection and model-state fixtures in the complete desktop
   render gate as local and CI resource usage changes.
7. Carry forward run `31426799633` only as predecessor ConPTY component evidence,
   then run the current complete Windows packaging workflow or a clean Windows VM to
   validate the current Unicode, resize, Ctrl+C, exit-status, and Job Object process-
   tree behavior before closing `14.2`, without exposing Agent execution permissions.
8. Finish `14.5` with a pinned live command fixture and child-process observation
   evidence, without exposing native Agent execution before sandbox,
   permission, approval, and recovery gates exist.
9. Finish `14.7` by adding gated foreground/daemon producers only after permission,
   sandbox, and approval controls, then prove their process-tree cancellation on
   macOS and Windows and validate user-terminal stop on a real Windows runner.
10. Continue task `16.7` with the production permission/approval authority and durable
   consumption ledger, typed session events, and reviewed Qt conflict/recovery flow.
   Keep the internal executor unreachable from AAP/Qt until those gates are complete;
   add sandboxed hook output and secure signing as separate reviewed policies.
11. Continue task `7.3` only after `6.10` supplies a durable compaction checkpoint
   and preservation review; provider delete/compact must remain unavailable until
   their scoped review, compensation, and recovery contracts are complete.
12. Continue task `6.2`/`6.3` by intersecting acknowledged project trust with managed
   permission policy and the production approval ledger. A trust acknowledgement must
   never become an implicit write, command, Hook, or network grant.
13. Continue `21.9` by adding reviewed platform permission/delivery settings only
   after scheduler and approval gates are complete. Continue `21.8` with durable
   decision production/consumption and reviewed recovery transitions only after the
   permission, sandbox, budget, and release gates pass. Keep automatic lease
   acquisition/renewal, process adoption, retry, approval, recovery mutation, and
   dispatch unavailable until those gates pass.
14. Continue `20.1` in dependency order with genuine-user Approval only after the
    production approval authority and consumption ledger exist, then add complete
    Change/Test production and reviewed AAP/Qt, audit/export, and retention surfaces.
    Do not record prompts/provider bodies or treat Runtime denial, Provider
    `declined`, or `approvalPolicy=never` as user approval.
15. Continue with the next unchecked database/event, durable project/session, typed
    timeline, permission/approval, structured patch/checkpoint, terminal, and Git
    milestones in dependency order.
16. Replace the offline model-catalog projection with an authenticated,
   signature-validated cache; connect durable global/project profiles to the
   catalog matcher; then expose a real picker and immutable model-change event
   only after token, routing, and cross-platform evidence gates pass. The
   current Qt profile count is metadata-only and is not a picker.
17. Before any public platform claim, deliberately select and pin the macOS Qt/
    deployment target, validate the now-implemented Windows installer
    `MinVersion=10.0.17763` and application `longPathAware` policy on a clean host,
    and execute the complete signed platform matrix. Runtime enforcement of the
    Git `2.31.0` floor is complete, but source policy does not replace clean
    Windows and signed-package evidence.
18. Implement the repository-owned feature registry and channel delivery only after
    its signed-artifact upper bound and fail-closed intersection are testable. Finish
    emergency production publication, secure high-water anchoring, Runtime binding
    to the exact signed policy identity, and Qt/Rust method-classification parity;
    keep remote fixed unavailable until its separate OpenSpec is accepted.
19. Carry forward the named-pipe/bootstrap E2E and negative-matrix pass from
    predecessor run `31426799633` only as component evidence, then execute the
    current complete clean Windows workflow before closing `4.3`/`4.4` or unblocking
    the `4.8` hostile-client matrix. The stdio/Unix-socket paths remain macOS-verified;
    no predecessor component result is installer, package, signing, or release proof.
    Separately investigate the environment-specific
    `platform_terminal_protocol_supports_interaction_resize_and_exit_status` PTY
    failure that reproduces on the base commit.

## Extension Staging Backup Domain (2026-09-02)

- `ConfigurationBackupStore::extensionStagingDomain()` now defines the separate persistence domain that
  future extension tree snapshots and rollback may use. Its AAD and identity hash domains, key scope,
  manifest/payload formats, subject grammar, backup-id grammar, diagnostics, and bounds are distinct from
  the four tool configuration targets; embedded-NUL separators remain byte-stable via `sizeof - 1`.
- Extension storage has no trusted v1 history, so the domain keeps legacy migration disabled. The dedicated
  domain test proves configuration, namespace separation, encrypted round-trip, and persisted-string
  separation while the existing tool backup byte-compatibility test remains unchanged.
- This is a persistence-domain contract only. No extension tree flattening, path manifest, installation,
  enablement, execution, UI caller, or recovery action is connected; OpenSpec `0.4` remains unchecked and
  Agent/Codex remains read-only.

## Shared Extension Tree Capture (2026-09-04)

- `SkillExtensionInventory` 与 `ExtensionBundleReader` 曾各自在匿名命名空间里携带同一套有界
  扩展树压平逻辑的私有副本：递归遍历、确定性排序（原始 UTF-8 文件名、目录先列后递归）、
  符号链接/特殊文件/控制字符名/冒号名/大小写折叠重名拒绝、规范化路径包含检查、读取后漂移
  复查、全部预算上限（4096 条目、16 层、单文件 2 MiB、总量 16 MiB、相对路径 4096 字节），
  以及长度分帧的 SHA-256 内容身份。逐行 diff 确认两份副本在树机制上语义完全等价，差异只有
  调用方域：诊断代码前缀（`skill-` 对 `extension-bundle-`）与身份域串
  （`aegisy-skill-extension-content/0.1\0` 对 `aegisy-extension-bundle-content/0.1\0`）。
  两份副本会各自漂移，而漂移意味着同一份内容在两条路径上算出两个身份，而那个身份正是授权
  绑定的对象——因此这套机制现在只有一份：`include/extension_tree_capture.h` +
  `src/extension_tree_capture.cpp` 的 `ExtensionTreeCapture`。
- 参数化沿用证据账本抽取的形态：调用方提供 `ExtensionTreeCaptureDomain`（身份域、身份前缀、
  诊断代码前缀），任何一项为空即整体拒绝（`extension-tree-capture-domain-unconfigured`），
  不存在缺省域；两个调用方的公开 API 与全部诊断串逐字节不变，共享预算对象仍由技能清单在
  整根目录的多棵子树间持有。清单解析、SKILL.md 校验、组件分类与路径前缀判定等非树机制的
  校验仍留在各调用方。
- 字节兼容性证据：`skill_extension_inventory`、`extension_bundle_reader` 等全部既有测试不改
  一字并通过；新测试 `extension_tree_capture` 按分帧约定独立重算（域串 + 8 字节大端长度前缀
  + 条目序）两个域对同一棵固定小树的期望身份并断言逐字节相等，另证两域身份互不相同、身份域
  进入摘要字节、排序确定性、全部预算上限、符号链接/特殊文件/控制字符名/包含外目录/不可读
  文件的逐域拒绝、未配置域拒绝。破坏检查：排序反向与长度前缀改小端各被抓到一次，还原后通过。
  运行时漂移（TOCTOU 复查）分支无法确定性触发，靠与原实现逐行同构与既有测试覆盖。
- `product_scope_policy` 的 pin 按证据账本抽取时的先例跟到共享层：分帧、包含检查、符号链接
  拒绝、域绑定与未配置域拒绝钉在 `extension_tree_capture.cpp`，调用方钉域常量与"摘要来自
  磁盘字节"的调用点，另钉共享层只读（无写盘 token）与 CTest 注册。
- 这是内部代码健康抽取：没有暂存域调用方，没有安装、启用、执行、UI 或恢复接线；
  OpenSpec `0.4` 仍未勾选，Agent/Codex 保持只读。
- 门禁：94/94 中 93 通过，唯一失败是 `agent_runtime_protocol` 里已知的环境相关 PTY 用例
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`，在基线提交上
  同样复现（见 Deferred Workbench Priorities 19），与本切片无关。串行门禁 181.42s。

## Extension Staging Snapshot Contract (2026-09-04)

- 暂存域（2026-09-02）定义了"快照存在哪里、怎么加密"，但没有定义"一棵树如何变成一份
  备份"：存储的载荷是按槽位编号且无路径的 `ConfigurationBackupFile{slot, existed,
  content}`，而恢复一棵树必须知道每个槽位对应哪条相对路径。`ExtensionStagingSnapshot`
  （`include/extension_staging_snapshot.h` + `src/extension_staging_snapshot.cpp`）
  补上这一层契约：槽 0 固定承载路径清单文档
  `aegisy-extension-staging-snapshot-manifest/0.1`（规范化 JSON，键序固定、无空白），
  槽 1..N 按清单中文件条目的顺序承载文件内容；目录只占清单条目、不占槽位。清单顶层
  恰好六个字段 `entries`/`file_count`/`format`/`identity`/`subject`/`version`；文件
  条目恰好五个字段 `byte_count`/`kind`/`path`/`sha256`/`slot`，目录条目恰好两个
  `kind`/`path`。`subject` 是注册表风格的 `kind:id`，在任何捕获结果被触碰之前就按
  暂存域的主体语法校验；`identity` 是构建侧用调用方捕获域算出的整树分帧摘要。
- 上限对账是这一层最核心的契约点。捕获层允许 4096 条目、单文件 2 MiB、总量 16 MiB；
  暂存域允许 256 槽、单槽 4 MiB、载荷 64 MiB、清单 32 MiB。树先过捕获层再进暂存域，
  因此更紧的那一侧必须在产出任何字节之前就以独立诊断拒绝，绝不截断：槽 0 已被清单
  占用，文件条目至多 255 个（`file-count-limit`），单文件超过 4 MiB 拒绝
  （`file-oversized`——捕获层的 2 MiB 更紧，这一支只能由手工构造的输入触发，构建层
  独立守住），清单文档的实际天花板是单槽上限与域清单上限中更紧的那个即 4 MiB 而不是
  32 MiB，因为槽 0 在存储层就是一份普通文件（`manifest-oversized`），序列化后的存储
  载荷按 base64 膨胀的保守上界估算，超过 32 MiB 即拒绝（`payload-oversized`）。测试
  里用 300 个真实文件证明 interplay：捕获层完整放行，构建层以 `file-count-limit`
  拒绝；255 个文件恰好填满 256 个槽的边界用例必须成功。
- 清单是严格的。验证器对读回（已解密）的快照逐层失败关闭，每一类问题有独立的
  `extension-staging-snapshot-*` 诊断：槽 0 缺失/未占用/计数与槽序不符
  （`slot-mismatch`）、NUL 字节（`manifest-encoding`）、JSON 解析失败或非法 UTF-8
  （`manifest-parse`）、非规范化字节（多余空白、键序、被解析层吞掉的重复键——
  `manifest-canonical`）、未知或缺失字段/错误类型/错误格式或版本（`manifest-shape`）、
  遍历形状路径（`..`、绝对路径、空段——`path-invalid`）、重复路径
  （`path-duplicate`）、字节数不符（`byte-count-mismatch`）、逐槽 SHA-256 不符
  （`content-digest-mismatch`）、主体三方（清单、期望、快照）任何一对不符
  （`subject-mismatch`）、用调用方捕获域重算的整树身份不符（`identity-mismatch`）。
  用错捕获域验证会以 `identity-mismatch` 失败关闭而不是退回默认域。空树与纯目录树
  是合法快照：清单条目为空、文件数为零，快照只剩槽 0，可完整穿过加密域往返。
- 破坏检查：临时禁用验证器里的逐槽摘要比较后，`extension_staging_snapshot` 在
  "清单摘要被改""单字节槽篡改""同长度槽内容互换"三处立刻失败，还原后全部通过。
- 这一层不写盘、不实例化存储：`build` 只产出内存快照，`verify` 只读内存快照。没有
  任何产品调用方——`tool_manager.cpp`、`mainwindow*`、扩展中心都不引用它，
  `product_scope_policy` 同时钉住机制文件（清单格式串、域对账调用点、身份绑定、全部
  诊断后缀、只读 token 缺席、CTest 注册）与产品接线的缺席。没有存储写入调用方、没有
  恢复流程、没有安装/启用/执行/UI/恢复接线；OpenSpec `0.4` 仍未勾选，Agent/Codex
  保持只读。
- 门禁：95 个注册测试中 94 通过，唯一失败是 `agent_runtime_protocol` 里已知的环境
  相关 PTY 用例 `platform_terminal_protocol_supports_interaction_resize_and_exit_status`，
  在基线提交上同样复现（见 Deferred Workbench Priorities 19），与本切片无关。
  `configuration_backup_store`、`configuration_backup_store_domain`、
  `extension_tree_capture`、`skill_extension_inventory`、`extension_bundle_reader`
  逐字未改并通过。串行门禁 168.94s。`git diff --check` 干净。

## Non-Codex Adapter Fail-Closed Pins (2026-09-04)

- 既有钉板只守住"模块声明缺席"（`mod claude_adapter;`/`mod gemini_adapter;`/
  `mod acp_adapter;` 三个 `requireAbsent`），挡不住在 `lib.rs` 内部直接长出一个新
  `Backend` 变体或新构造函数。本切片把失败关闭的证据链补全，全部改动仅限
  `tests/product_scope_policy_test.cpp`（新增四个源码加载、空检查扩展与一组 pin），
  不含任何生产代码变更，不新增任何适配器。
- 后端封闭性：`enum Backend` 六行整块按原文钉住（`Preview`、`Codex(CodexAdapter)`、
  `Recovery(WorkbenchRecoveryDiagnostic)`、`Unavailable(String)`）。该块短且结构稳定，
  全文本 pin 的脆弱性代价低于"逐名缺席"漏掉新变体名的风险——任何新增变体无论叫什么
  都必须先改这一段才能让测试通过。另钉变体构造形态 `Claude(`/`Gemini(`/`Acp(`、
  适配器类型名 `ClaudeAdapter`/`GeminiAdapter`/`AcpAdapter` 与模块标识
  `claude_adapter`/`gemini_adapter`/`acp_adapter` 在 lib.rs 全文件缺席，覆盖
  use/路径等绕过 `mod` 声明的等价引入。
- 构造入口：勘察发现 daemon 入口并非切片简报假设的单一 `with_codex()`，而是恰好四条
  `Runtime::with_*` 调用点，pin 按代码现状落笔：main.rs 计数钉为 4 并逐条钉名——
  `with_store`/`with_emergency_store`（只落到 Preview/Recovery 的存储恢复路径）与
  `with_codex()`/`with_codex_and_store()`（仅有的活适配器路径）；第五条路径或任何
  `with_claude`/`with_gemini`/`with_acp` 命名在 lib.rs 与 main.rs 双侧缺席。
- 界面广告缺席：`agent_workbench_window.cpp`/`agent_workbench_widget.cpp`/
  `agent_runtime_client.cpp` 三个编程界面源文件逐一钉 `Gemini`/`ACP`/`Claude Opus`
  及三个适配器标识缺席。三个文件当前出现次数为零，没有合法提及（如延期说明）需要
  豁免，因此按全缺席钉；原有 `workbenchWindow` 的 `"Claude Opus"` 单点 pin 保留不动。
- 配置目标边界不动：tool_manager.h 的四目标枚举 pin（ClaudeCode/CodexCli/GeminiCli/
  OpenCode）原样保留并通过——Claude/Gemini/OpenCode 仍是配置目标，不是运行时。
- 目录元数据惰性：钉的是结构性隔离而不是发明代码里不存在的文档断言（枚举上方没有
  元数据注释）。`RuntimeAdapterFamily` 在拥有后端的 lib.rs 中全文件缺席——目录族
  一旦出现在那里就意味着目录条目开始驱动运行方选择；model_catalog.rs 缺席
  `CodexAdapter`/`with_backend`/`Backend::` 任何后端构造符号；保留的
  `RuntimeAdapterFamily::Acp if self.protocol != "acp"` 匹配臂被钉为只产出校验错误
  的形状，不构成可达路径。
- 破坏检查：向 lib.rs 注入 `Gemini(`、向 agent_workbench_widget.cpp 注入 `Gemini`
  字样各一次，测试分别以 "a non-Codex backend reached the Runtime implementation"
  与 "programming surface advertises a deferred non-Codex runtime" 立即失败；
  `git checkout` 还原后通过。该测试运行时读源码，破坏检查无需重新链接。
- 门禁：聚焦 `product_scope_policy` 通过（0.57s）。95 个注册测试中 94 通过，唯一
  失败是 `agent_runtime_protocol` 里已知的环境相关 PTY 用例
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`，在基线
  提交上同样复现（见 Deferred Workbench Priorities 19），与本切片无关。受前台 300
  秒限制串行门禁分段运行：33-64 段 2.74s，65-95 段 49.21s，1-32 段由
  agent_runtime_protocol 主导（其两个 Rust 套件合计约 90s，唯一失败即上述 PTY
  用例）。`git diff --check` 干净。
- OpenSpec `0.7` 仍未勾选：源码树层面的不可选/不可宣传/不可达已经钉住，但仍缺已
  签名安装包层面的可达性证据（发布的二进制而非源码树）与 Windows/Linux 门禁运行。
  Agent/Codex 保持只读。

## Native Companion Gate Evidence (2026-09-05)

- Current HEAD `8d3f6db` has a successful macOS workflow run `33859625795`.
- Windows workflow run `33859625810` failed at `Lint Windows agent runtime`
  before Qt installation/configure/build, companion cache/dialog evidence, or
  packaging. Public check metadata exposes only exit code 1; no Clippy
  diagnostic is available without authenticated job logs, so no Windows Rust
  root cause or Qt result is inferred.
- OpenSpec task `0.2` remains unchecked pending a fresh Windows run with
  bounded lint diagnostics followed by successful Qt and companion evidence.
  Agent/Codex remains read-only.

## Bounded Windows Clippy Diagnostics (2026-09-05)

- The Windows lint step now runs `tests/windows_clippy_gate.mjs`, preserving locked
  workspace/all-target Clippy with warnings denied and the original process exit
  status. Cargo JSON compiler messages supply only diagnostic level/code and an
  allowlisted, Git-tracked repository-relative Rust location. Absolute, external,
  malformed, or unknown source paths receive a fixed location label. Source text,
  rendered diagnostics, suggestions, raw stdout, and stderr are never published.
- The reader drains physical lines above 1 MiB, retains at most 20 distinct
  diagnostics, caps source locations at 512 characters, and caps the complete
  encoded annotation body at 2,000 characters without splitting a diagnostic.
  Missing Cargo, failed source inventory, abnormal exit, and unclassified failure
  remain failures with fixed diagnostic codes. This changes CI observability only.
- Seven Node tests cover structured parsing, chunk/EOF/oversize handling, source
  and code rejection, full encoded output bounds, real-child 0/7/101 exit status,
  missing executable, and abnormal termination. The registered diagnostic,
  product-scope, Windows packaging, and macOS CI policy tests pass (4/4 across two
  focused runs); CMake configure, workflow YAML parsing, strict OpenSpec validation,
  and diff checks pass. The real wrapper also passes locked workspace/all-target
  Clippy with warnings denied on macOS (Cargo's directory was explicitly added to
  this shell's PATH). The policy rejects modified, duplicated, skipped, or
  success-forced lint steps.
- Native Windows execution is still required to identify the lint failure from
  run `33859625810` and obtain complete Qt/companion evidence. OpenSpec `0.2` stays
  unchecked. No Agent/Codex permission or execution authority changed.

## Windows AAP Integer Lint Repair (2026-09-05)

- Windows run `33938917676` at `3f89a95` passed the clean Unicode checkout,
  formatting, and locked workspace tests. The structured lint annotation then
  identified exactly two `clippy::map_or_identity` errors in the AAP JSON integer
  normalizer. Qt, companion, installer, and package stages did not run.
- Exponent and fraction splitting now use `Option::unwrap_or` instead of an
  identity `map_or` closure. Both existing fallback tuples and every canonical
  integer, safe-number, and Timeline identity rule are unchanged. No lint is
  suppressed and no gate is narrowed.
- The pre-repair local desktop baseline passes `96/96` CTests in 526.84s,
  including the previously environment-sensitive PTY protocol fixture. The repaired
  AAP crate passes `59/59` tests and the actual diagnostic wrapper passes locked
  workspace/all-target Clippy with warnings denied. Rust formatting, strict
  OpenSpec validation, and diff checks pass. macOS run `33938917634` at the parent
  commit also completed successfully. These are not repaired Windows evidence.
- A fresh native Windows run remains required before `0.2` can close. Agent/Codex
  remains read-only; this repair adds no permission or execution authority.

## Account Refresh Authority Retirement (2026-09-05)

- A current-generation account refresh failure previously emitted a configuration
  failure while retaining ApiClient's verified account, live projection, and bound
  operations. A held Key response could then republish live configuration. A new
  fake-transport regression reproduced this before the production fix.
- Account trust/transport/type/identity failures now invalidate pending Key inventory,
  clear ApiClient account verification, and use the complete existing live-authority
  retirement path. New Key requests require successful account verification again.
  MainWindow retains its separate previously verified account identity for the
  existing read-only cache fallback; neither cache bytes nor tool configuration
  change. Invalid account data is never published through `userInfoReceived`.
- Each logical account refresh has its own request generation in addition to the
  auth epoch. Older success/failure/404 responses are inert. The existing legacy
  endpoint fallback retains its original refresh generation, and terminal fallback
  failure retires authority rather than retrying recursively.
- OpenSpec `0.2` remains unchecked pending current native Windows evidence.
  Agent/Codex remains read-only.

## Cross-Platform Clippy Compatibility Repair (2026-09-05)

- Rust Quality run `33939858767` at `bcb64c5` reached locked Clippy and reported
  three identity `map_or` closures in `content_reference.rs` plus
  `chunks_exact_to_as_chunks` in `git_commit_transaction.rs`. The equivalent
  `unwrap_or` and `chunks` forms preserve defaults and the prior divisibility
  check while satisfying the current stable Clippy policy.
- Local strict workspace Clippy and formatting pass after the repair. No warning
  was suppressed and no source/test scope was narrowed. Native CI rerun remains
  the evidence required for Linux and Windows.

## Extension Staging Snapshot Tree Rebuild (2026-09-06)

- `ExtensionStagingSnapshot::verify` now optionally returns the reconstructed tree
  only after complete manifest, slot, digest, subject, and identity validation.
  Its output is cleared before every attempt and remains empty on failure.
- Focused `extension_staging_snapshot` coverage proves deterministic reconstruction,
  manifest order, and same-length slot tamper clearing. No storage writer,
  installation, enablement, execution, UI, or recovery caller was added;
  OpenSpec `0.4` remains unchecked and Agent/Codex remains read-only.

## Extension Staging Restore Plan (2026-09-05)

- `ExtensionStagingRestorePlanBuilder` 把一份**完全验证通过**的暂存快照转换成有界、
  不含内容之外的恢复计划：它只消费快照验证器重建出的树，经由注入的只读观察校验目标根，
  拒绝逃逸/符号链接/特殊文件、观察不可用、目标冲突与暂存域上限故障，自身不写入也不创建
  任何文件系统对象。计划将目录操作排在文件操作之前、保留来源槽位，并把目标/主体/树身份
  连同每一条操作绑定进分帧计划身份；逐字节一致的已有文件标记 `alreadyInPlace` 而非静默
  跳过；任何失败都使输出计划为空。计划是纯数据对象：先经
  `ExtensionStagingSnapshot::verify` 五参重载重建并验证树（失败时原样透传
  `extension-staging-snapshot-*` 诊断，篡改快照在计划开始前失败关闭），再经注入的
  只读 `ExtensionStagingRestoreObservation`（规范化根、逐路径节点类型、已有文件字节）
  观察目标现状；观察不可用一律以 `extension-staging-restore-destination-unavailable`
  拒绝，绝不盲计划。
- 拒绝类各自独立：`destination-invalid`（空、相对、非规范化、缺失或非目录的目标根）、
  `root-symlink`（目标根本身是符号链接）、`symlink-component`（任何计划路径组件上的
  符号链接——专为它补了不含目录条目的深路径夹具，使逐段祖先检查而非逐条目检查成为
  触发点；该守卫经破坏测试：注释掉后测试立即失败，已还原）、
  `path-escapes-destination`（对每条清单路径的包含性重查，纵深防御；经验证的快照无法
  携带逃逸路径，因此该代码只能由守卫本身与策略 pin 守住）、`destination-conflict`
  （目标现状与计划内容不符——恢复到脏树被拒绝，绝不静默覆盖）与 `bounds-exceeded`
  （文件数、单文件字节、聚合字节按暂存域上限重查；验证层不单独守聚合上限，一份手工
  构造的 17×4 MiB 快照通过验证后被计划层拒绝）。逐字节一致的已有文件保留在计划中并
  标记 `alreadyInPlace`：显式的无需写入语义，携带期望摘要，验证永不被静默跳过。
- 计划身份是 `aegisy-extension-staging-restore-plan/0.1` 域下的长度分帧 SHA-256，覆盖
  规范化目标根、主体、树身份与每一条有序操作（目录创建在前、文件写入在后，各保持清单
  顺序；文件条目含路径、字节数、期望 sha256、来源槽位与 already-in-place 标志）。同一
  快照对两个目标根、或同一目标根对两棵树，身份必然不同。
- 门禁证据：新增 `extension_staging_restore_plan`（6 个聚焦测试：经加密暂存域往返后对
  第二个真实临时目录计划、already-in-place、目标校验、冲突与符号链接、上限纵深防御、
  计划身份绑定与篡改传播）；`extension_staging_snapshot`、`extension_tree_capture`、
  `configuration_backup_store*`、`product_scope_policy`（新增 pin：先验证边界、诊断
  前缀与全部拒绝代码、包含性重查、身份分帧、写 token 缺席、无产品接线）均通过。注册
  总数 97；本机串行分段运行 96/97 通过（分段墙钟约 123s/6s/51s）。唯一失败是
  `agent_runtime_protocol` 的 Rust PTY 用例
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"），单独重跑三次一致失败；它与本切片无任何共享代码
  （agent-runtime Rust 进程，不链接任何被改动的 Qt/C++ 文件），判定为既有的环境性
  PTY 生命周期回归，需在独立切片中按名复查。
- 仍未接通：没有任何执行器、目标根选择者、存储/ToolManager/工作台/UI 调用方；OpenSpec
  `0.4` 保持未勾选；Agent/Codex 保持只读。

## Extension Staging Backup Capture (2026-09-05)

- `ExtensionStagingBackupCapture`（`include/extension_staging_backup_capture.h`、
  `src/extension_staging_backup_capture.cpp`）是暂存链的生产者：快照能构建验证、恢复能
  计划，但此前没有任何东西从一棵活着的扩展树产出备份。它写入的唯一目标是应用私有的
  加密暂存备份存储（与 ToolManager 工具配置备份同一类写入）；不修改扩展来源树，不
  安装、不启用、不执行任何东西。位置权威在调用方：来源根与备份根都由调用方给出，
  本组件从不发明位置。
- 种类映射是封闭且诚实的：只有 `skill:` 主体被捕获，且复用
  `SkillExtensionInventory::treeCaptureDomain()`——技能捕获域的字面量从此只有一个
  来源（原匿名命名空间副本提为公开静态访问器），同一棵树在技能清单与备份里算出同一
  个身份。`codex-plugin:` 与 `mcp:` 被各自独立的诊断拒绝
  （`extension-staging-capture-codex-plugin-without-tree-source` 与
  `extension-staging-capture-mcp-without-tree-source`）：Codex 插件经捕获的 CLI 输出
  进入、MCP 经设置 JSON 进入，两者都不是这一层可以假装去捕获的树。语法之外的种类以
  `kind-unmapped` 失败关闭，绝不落到默认域。
- 顺序是安全性质：主体在任何文件系统工作之前按暂存域语法校验（`subject-invalid`，
  测试用金丝雀路径证明畸形主体连路径都不碰），随后是种类映射、来源根纪律、有界捕获
  （漂移复查由捕获层持有）、快照构建，最后才是存储写入。来源根的符号链接在规范化
  之前由本层拒绝（`root-symlink`）——canonicalFilePath 会静默解析它，解析之后捕获层
  看到的就不再是调用方指给它的那个位置；其余包含性/符号链接/上限纪律由捕获层在扫描
  内部完成，其诊断（`skill-symlink-invalid`、`skill-file-oversized` 等）与快照层
  （`extension-staging-snapshot-*`）、存储层（`extension-staging-backup-*`，含密钥不
  可用的 `key-unavailable`）代号逐字透传，不另造本地代号。
- 失败原子性：捕获层与快照层不写盘；存储的 `create` 是备份目录级原子的（加锁、原子
  写、写后重读重解析复核、任何失败回收整个备份目录），因此任何阶段失败都不留半份
  状态——测试在全部拒绝路径之后清点证明备份根一份备份都没有。
- 再捕获语义：同一棵未变化的树允许再捕获（存储分配新的备份 id），但结果报告新树
  内容身份与该主体最近一次既有备份是否一致（NoPriorBackup/Matched/Mismatched/
  Unknown）。比对是对该主体的只读清点加上对最近备份的完整读回验证——身份只能由
  验证器重建的树重算，绝不自行解析清单。清点退化或最近备份验不过时是显式 Unknown
  加 `prior-identity-degraded`，绝不静默变成"没有既有备份"；清点只是建议性输入，
  存储清点坏了的时候恰恰最不该丢备份，因此降级不阻断写入。写入成功后再清点一次取回
  清单身份供审计绑定；这次清点同样可能退化，退化时身份留空并携带
  `manifest-identity-degraded`，备份本身完整在盘上且可按 id 直接读回验证（种入外域
  清单的降级测试同时覆盖这两个降级出口）。本组件不做保留期裁剪、清单管理与
  恢复执行。
- 门禁证据：新增 `extension_staging_backup_capture`（6 个聚焦测试：端到端契约链
  ——真实临时目录技能树经捕获、加密暂存域往返、快照验证、对空目标的恢复计划，并
  用测试侧独立域字面量交叉核对身份字节；主体语法先于文件系统工作；两类无树种类
  各自独立拒绝且与捕获失败不同名；未变化再捕获报告 Matched、变更报告 Mismatched、
  各自拿到新 id；清点退化双出口；捕获层失败透传且不留备份）。降级守卫经破坏测试：
  把退化分支改成静默 NoPriorBackup 后测试立即以 "a degraded inventory silently
  became 'no prior backup'" 失败，已还原。`product_scope_policy` 新增 pin：显式边界、
  诊断前缀与全部代号、主体先于文件系统的相对顺序（requireOrdered）、种类映射封闭
  （禁止第二份技能域常量）、先验证再比对、写/恢复/裁剪/执行 token 缺席、无产品接线、
  CTest 注册。本机串行全量门禁：注册总数 98，97/98 通过，墙钟 229.7s；唯一失败仍是
  `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续
  既有环境性 PTY 判定）。
- 仍未接通：没有 UI/ToolManager/工作台调用方，没有保留期裁剪，没有恢复执行；
  OpenSpec `0.4` 保持未勾选；Agent/Codex 保持只读。

## Extension Staging Backup Inventory (2026-09-05)

- `ExtensionStagingBackupInventory`（`include/extension_staging_backup_inventory.h`、
  `src/extension_staging_backup_inventory.cpp`）是暂存域备份的清点、验证删除与保留期
  规划层：域钉住了 maxBackups（32），但捕获只往里写、没有任何东西管理它，而且存储
  自己的清点在根里出现第 33 份备份时就把整个根判 Invalid——超限状态连看都看不到，
  更谈不上修复。本组件的唯一写入是存储的身份绑定验证删除（与 ToolManager 工具配置
  备份同一类写入）；不修改扩展来源树，不安装、不启用、不执行，无任何产品调用方。
- 清点不是证明，验证级别固定为**清单身份级**：有界读清单字节、完整结构校验（格式、
  版本、算法、主体、备份 id 与目录名逐字节绑定、规范化时间戳与 base64 字段）、身份
  从读到的字节重算；载荷的 GCM 认证刻意留给恢复路径，因此清点路径完全不碰密钥——
  密钥不可用时清点照常工作，而那正是最需要在场的时候。`ListedCorrupt` 是独立状态，
  损坏条目永远留在清单里、绝不静默丢弃（一份被藏起来的损坏备份恰恰是回滚能力悄悄
  消失的方式）；损坏条目的身份同样从字节重算供审计指认，声称主体仅以未认证方式
  提取用于把损坏条目归进主体视野。根形状违例与锁冲突传播为各自独立的 Invalid /
  Unavailable，绝不伪装成空清单。与存储清点有一处刻意的分歧：扫描上限是 maxBackups
  的 4 倍（128）而不是超限即 Invalid——超限正是保留期规划要修复的现实；同一理由下
  存储 `removeVerified` 的扫描上限也放宽到同一宽度（签名化 `scanRootShape` 的计数
  上限，`inventory` 仍按 maxBackups），因为删除是唯一必须在超限根上工作的操作；
  既有存储测试无一钉住该计数行为，全部不变通过。
- 验证删除只按精确备份 id：id 语法先于任何存储工作（`backup-id-invalid`），随后经
  一次全主体清点取得主体与清单身份再调存储。id 畸形、id 不存在（`backup-absent`）、
  条目损坏（`backup-corrupt`，验证路径无法认证损坏清单，拒绝并原地保留证据）、清点
  退化（原诊断逐字透传）、存储拒绝（代号逐字透传）是五种可区分的结果；删除只动
  指定 id 那一份，其他主体的备份绝不被触碰。
- 保留期规划是纯数据对象、规划期间存储零写入（测试用清单身份指纹前后比对证明）：
  keep 集是该主体最新的 maxBackups 份 ListedIntact 备份（新到旧，同刻按 id 升序）；
  该主体最近一份完整备份**无条件保留**并记入显式的 `newestVerifiedKept` 字段——
  保留它是单独报告的决策而非隐含的名单成员资格，若计算出的 keep 集竟不含它则以
  `plan-inconsistent` 失败关闭；在这一构造下 keep 集永不超限。超出 keep 集的完整
  备份逐条列入 prune（OverLimit），该主体每份损坏备份也逐条列入（Corrupt），均携带
  清单身份，没有静默丢弃；未超限则 prune 为空。`applyRetention` 只逐条组合验证删除
  并逐条报告：损坏候选被如实报告 CorruptRefused——它们的物理清除需要一个尚未存在
  的证据处理决策。
- 门禁证据：新增 `extension_staging_backup_inventory`（5 个聚焦测试：多主体清点与
  作用域、测试侧独立域字面量交叉核对清单身份字节；三类结构级损坏各自可见且归入
  主体视野、根形状违例 Invalid 与锁冲突 Unavailable 绝不成空清单；删除五结果可区分、
  存储代号透传、拒绝后证据原地保留、其他主体不动；超限规划 newest-first、最近完整
  备份显式保留、损坏逐条列入、未超限空计划、规划零写入指纹比对；apply 逐条组合且
  损坏条目如实拒绝）。损坏可见性守卫经破坏测试：让清点丢弃损坏条目后测试立即以
  "a corrupt backup was silently dropped from the listing" 等四处失败，已还原。
  `product_scope_policy` 新增 pin：显式边界、诊断前缀与全部 13 个代号、主体语法先于
  存储触碰（requireOrdered）、清点路径无写/无密钥/无删除 token（sourceRange 缺席
  pin）、损坏条目唯一过滤理由是作用域、扫描上限 4×maxBackups、删除只经
  `store.removeVerified`、保留上限取域定义而非本地副本、计划路径纯数据缺席 pin、
  apply 逐条组合、无产品接线、CTest 注册。本机串行分段门禁：注册总数 99，分段
  1–40 / 41–75 / 76–99 墙钟 164.95s / 13.98s / 51.45s，98/99 通过；唯一失败仍是
  `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续
  既有环境性 PTY 判定）。
- 仍未接通：没有自动裁剪触发器，没有 UI/ToolManager/工作台调用方，没有恢复执行，
  损坏备份的物理清除待证据处理决策；OpenSpec `0.4` 保持未勾选；Agent/Codex 保持
  只读。

## MCP Configuration Backup Capture (2026-09-05)

- `mcp:` 主体的诚实备份单元是整个设置文件的原始字节，而不是一棵树：该文件同时是
  来源身份的单位与变更的单位（McpConfigDialog 经 QSaveFile 整文档重写、ToolManager
  合并写 env 键都作用于整个文件），且被文件里所有 `mcp:` 主体共享——按单个服务器
  抽取字节会产出一份恢复时覆盖其他服务器配置的 dishonest 备份，因此备份是整个文件，
  恢复语义也是整文件。`ExtensionStagingBackupCapture` 现在把 `mcp:<id>` 主体在
  `scanDirectory` 之前分流：`sourceRoot` 此时是调用方给出的设置文件路径，以与
  `McpConfigurationInventory::inspectFile` 相同的纪律读出原文（符号链接在打开之前
  拒绝、有界读取、读完重新 stat 漂移复查——被哈希的字节必须就是被存下的字节），再
  合成一棵固定单条目树送入不变的 `ExtensionStagingSnapshot::build`。合成相对路径恒为
  字面量 `settings.json`，绝不从调用方的文件名推导，因此无论文件住在哪里清单形状都
  稳定。备份层刻意不解析 JSON：字节就是真相，一个 JSON 已损坏的设置文件恰恰是最需要
  先备份下来的状态，有效性判定属于清单与恢复路径。
- 上限对账：MCP 读取走清单的 1 MiB 上限，它比捕获层的单文件 2 MiB 与暂存域的单槽
  4 MiB 都紧，更紧的一侧在产出任何字节之前获胜——同一份字节在任何一层都不会因为
  上限差异而被另一层拒绝。新捕获域挂在
  `McpConfigurationInventory::backupCaptureDomain()`（模式与
  `SkillExtensionInventory::treeCaptureDomain()` 相同），身份域
  `aegisy-mcp-config-backup-content/0.1\0`、展示前缀 `mcp-backup-content:sha256:`、
  诊断前缀 `mcp-backup`（grep 确认与既有前缀无冲突）。这是一个**新**身份：它与清单
  来源身份 `aegisy-mcp-config-source/0.1\0` 摘要的是同一批原始字节，但输入帧不同
  （带存在性标记的字节流 vs 快照树形状），两者绝不应被声称相等，头注释与测试侧的
  独立域字面量交叉核对都钉住了这一点。
- 共享文件语义在结果上显式可见：结果新增 `coversSharedSettingsFile`（`skill:` 捕获
  恒为 false），调用方据此知道这份备份覆盖的是整个共享设置文件而不只是该主体的
  服务器条目。文件级畸形各自独立诊断、拒绝后不留备份也不触碰备份根：
  `extension-staging-capture-mcp-source-missing`（缺失是拒绝，绝不是空备份）、
  `-mcp-source-invalid`（目录冒充文件等）、`-mcp-source-oversized`（超 1 MiB）、
  `-mcp-source-unavailable`（打不开/读失败）、`-mcp-source-symlink`、
  `-mcp-source-drift`（读取期间被换掉）。混合主体根上存储的按主体清点会如实退化
  （它要求根里每份清单的主体都与所查主体逐字节相等），捕获把这一点显式报告为
  `manifest-identity-degraded` / `prior-identity-degraded` 而不是假装确定，且降级
  绝不阻断写入。`codex-plugin:` 的拒绝代号
  `extension-staging-capture-codex-plugin-without-tree-source` 原样不变，理由记录到
  更深一层：应用对 Codex 插件只有观察——只消费捕获到的 CLI 列表输出，`source.path`
  是从未被子进程之外打开的未验证元数据，也没有任何变更面——权威内根本没有可备份的
  字节。原 `mcp-without-tree-source` 代号随拒绝路径整体退役（mcp 不再是"无源种类"），
  未被复用为任何其他含义。
- 门禁证据：新增 `extension_staging_backup_capture_mcp`（4 个聚焦测试：整文件端到端
  契约链——真实临时设置文件（mcpServers + 无关键）→ 捕获 `mcp:my-server` → 加密
  往返 → 验证通过 → 清单恰好一个 `settings.json` 文件条目 → 备份字节与文件原文逐
  字节相等（含非 MCP 键）→ 对空目标恢复计划成立，外加测试侧独立域字面量 + 独立
  合成树的身份交叉核对与未变化再捕获 `Matched`；两个主体共享同一文件各自独立捕获、
  内容身份相同、按 id 读回各自验证通过、混合根清点退化被显式报告；六类文件级畸形
  各自独立代号且备份根从未被建立；codex-plugin 拒绝代号不变）。既有
  `extension_staging_backup_capture` 的种类拒绝测试收窄为仅 codex-plugin（mcp 拒绝
  已被本切片取代），其余测试全部不变。符号链接守卫经破坏测试：移除打开前的
  `isSymLink` 检查后测试立即以 "a symlinked settings file was not refused" 失败
  （读后漂移复查仍以 `mcp-source-drift` 兜住，双层防卫确认），已还原。
  `product_scope_policy` 新增 pin：MCP 备份域访问器复用（不得复制第二份域字节）、
  六个新代号、合成路径字面量、漂移复查在场、1 MiB 更紧上限、共享文件标志、两个
  MCP 身份域字面量并列钉住、无产品接线循环扩入 `mcp_config_dialog.cpp`、CTest
  注册。本机串行分段门禁：注册总数 100，分段 1–40 / 41–75 / 76–100 墙钟
  136.22s / 11.58s / 49.89s，99/100 通过；唯一失败仍是
  `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续
  既有环境性 PTY 判定）。
- 仍未接通：没有 UI/ToolManager/McpConfigDialog/工作台调用方，没有恢复执行，没有
  自动裁剪，混合主体根的清点退化是被如实报告而非被解决；OpenSpec `0.4` 保持未勾选；
  Agent/Codex 保持只读。

## Mixed-Subject Backup Inventory (2026-09-05)

- 缺陷：`ConfigurationBackupStore::inventory(subject)` 要求根里每一份清单的主体都与所查
  主体逐字节相等，于是一个不同主体的备份目录就把整个根判 Invalid——在现实的共享暂存
  根（多个 `skill:`/`mcp:` 主体共存）里，第二个主体的捕获因此如实报告
  `manifest-identity-degraded`/`prior-identity-degraded`，尽管它自己的备份完全完好，
  按主体的清点与保留期规划也看不见它们。本切片把按主体清点改成作用域语义而不削弱任何
  完整性检查：只改清点的 READ 语义，没有新增任何写/恢复/安装/启用/执行路径，没有
  UI/ToolManager 接线。
- 新语义的分界线是完整验证而不是猜测：目录的清单声称主体语法合法且与所查主体不同时，
  该条目先通过与作用域内条目完全相同的验证——目录形状检查在前，随后以它自己的声称主体
  做完整清单解析（结构、版本、算法、id 与目录名逐字节绑定、规范化字段，含用它自己的
  密钥做 GCM 认证，密钥以 allowCreate=false 取得）——全部通过者才是 foreign-intact，
  越出本次查询作用域被跳过且绝不退化结果；任何一步失败都按原诊断如实退化（
  `stateForIssue` 逐字透传），损坏证据绝不因为"可能属于别人"而被静默跳过。声称主体取
  不出来（字段缺失、非字符串、语法非法）的清单没有资格被当成别人的，落回作用域内路径
  按所查主体判定，与既有行为逐字一致；foreign 密钥不可得时无法分辨 intact 与 corrupt，
  如实 `Unavailable`（`key-unavailable`）而不是猜。全是 foreign-intact 的根对所查主体是
  Ready 加空清单（捕获层据此如实报告 NoPriorBackup），既不是 Empty 伪装也不是 Invalid。
- 上限决策是重新做的而不是继承的：扫描上限放宽到 maxBackups 的 4 倍（与 `removeVerified`
  及暂存清点层同宽）且数的是目录总数——分辨 foreign 与 corrupt 要求读到每一个目录，单次
  清点的总工作量仍由它守住，越过它说明根已经大到一次诚实清点读不完，判 Invalid 而不是
  截断出一份看似完整的清单；而"超限即 Invalid"的判定只数所查主体自己验证通过的份数
  （`entries.size() > maxBackups` → Invalid，代号同为 `inventory-invalid`），别人主体的
  完整备份不占额度，一个主体的超限状态不再漏进另一个主体的清点。
- 捕获与清点层的更新真相：共享根里第二个主体的捕获现在报告干净的 `NoPriorBackup` 与已知
  清单身份，既有主体在混合根里的再捕获报告干净的 `Matched`；`prior-identity-degraded` 与
  `manifest-identity-degraded` 代号保留，只为真正退化的存储（外域格式清单、篡改清单、锁
  冲突等）触发；`ExtensionStagingBackupInventory` 的清单身份级清点与 `ListedCorrupt` 可见
  性完全未动（它本就走自己的主体容忍扫描），头注释里与存储清点的刻意分歧改述为按作用域
  的语义。工具域调用方（ToolManager 按工具各一根）无行为变化——容忍是通用机制而不是依赖
  单主体根的现状，证据是冻结的字节兼容测试 `configuration_backup_store` 逐字未动并通过。
- 门禁证据：`configuration_backup_store_domain` 新增三个聚焦测试（混合主体根按主体清点
  只返回本主体已验证条目、纯 foreign 根 Ready 加空清单；篡改的 foreign 清单以
  `authentication-failed` 退化、无可归类主体的清单与 id 不符的清单以 `manifest-invalid`
  退化、foreign 密钥不可得如实 `Unavailable`——四者都不被静默跳过；上限交互：三方各 8 份
  各自 Ready、作用域内第 9 份使该主体 Invalid 而旁观主体不受影响、目录总数越过 4× 扫描
  上限则任何主体都 Invalid）。既有 `testInventoryRefusesAForeignManifestInsteadOfMigratingIt`
  等全部不变通过。跳过验证的破坏测试（foreign 条目不经验证直接 continue）立即以
  "a tampered foreign manifest was silently skipped" 等三处失败，已还原。
  `extension_staging_backup_capture_mcp` 的共享文件测试改钉新真相（第二主体干净
  NoPriorBackup + 已知清单身份、混合根再捕获干净 Matched），其余捕获测试（含外域格式清单
  的退化测试）不变通过；`extension_staging_backup_inventory` 五个测试全部不变通过。
  `product_scope_policy` 新增 pin：存储 inventory 区间内扫描上限 4×maxBackups、foreign
  跳过必须经声称主体提取与完整 `parseManifest` 的顺序钉（requireOrdered）、作用域内超限
  判定在场，其余 pin 全部保留。本机串行分段门禁：注册总数 100，分段 1–40 / 41–75 /
  76–100 墙钟 123.86s / 11.22s / 50.24s，99/100 通过；唯一失败仍是
  `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续既有
  环境性 PTY 判定）。未触碰 agent-runtime，未跑 Rust 门禁。`git diff --check` 干净。
- 仍未接通：没有 UI/ToolManager/McpConfigDialog/工作台调用方，没有恢复执行，没有自动
  裁剪，损坏备份的物理清除待证据处理决策；超过 4×maxBackups 个目录的共享根对任何主体都
  是 Invalid（有界性的代价，暂存清点层与保留期规划仍可在那里工作）；OpenSpec `0.4` 保持
  未勾选；Agent/Codex 保持只读。

## Staging Restore Presentation (2026-09-05)

- 恢复计划只有在"渲染出来的正是将会执行的"时才可复核，而此前没有任何东西把暂存恢复
  计划渲染到人面前：计划层产出的是纯数据，没有人可读的呈现面。本切片新增
  `ExtensionStagingRestorePresentation`（`include/` + `src/`
  `extension_staging_restore_presentation.*`），把一份**已成功构建**的计划渲染成复核
  提示。它是纯呈现层：不创建目录、不写任何字节、不接触存储、不判定信任、不授予权限、
  没有"确定"按钮的处理逻辑，当前也没有任何产品调用方。
- 提示绑定的内容：主体、备份 id 加创建时间（描述来自清点层，验证状态必须是
  `ListedIntact`，否则无法诚实说明正在恢复哪一份备份）、目标根（调用方给出的字符串
  必须与计划的规范化目标根逐字节相等，漂移即 `destination-mismatch`）、如实的操作统计
  （目录创建数、待写文件数、already-in-place 文件数、含 already-in-place 内容的总字节
  数），以及有界的逐条清单。所有路径与文本都过共享的 `ExtensionDisplaySafety`（不存在
  第二份安全规则，`product_scope_policy` 钉住任何本地字符类别/码位/修剪检查缺席）；
  计划身份以两端指纹展示，同时完整身份原样回显到 `echoedPlanIdentity` /
  `echoedTreeIdentity`（沿用启用呈现 `reviewed*` 的先例），渲染与批准之间的任何漂移
  因此可检测。树身份的展示前缀由主体种类决定（`skill:` → `extension-content:sha256:`，
  `mcp:` → `mcp-backup-content:sha256:`），两个身份域绝不互认；无法归类的主体种类整体
  拒绝。
- `mcp:` 主体的共享文件警告是**强制的**：整文件语义意味着恢复覆盖整个共享设置文件，
  包括其他服务器的配置，警告文案逐字说出这一点（`sharedFileOverwriteNote`）。没有这条
  警告的 mcp 恢复呈现是呈现失败而不是少了点缀——破坏测试确认：删掉警告追加后
  `extension_staging_restore_presentation` 立即以 "an mcp restore lost its mandatory
  shared settings file warning" 失败，已还原。
- 截断有界而绑定不受界：计划最多 255 条目录加文件操作，清单只列前
  `MaxListedEntries`（16）条，超出渲染显式的"…以及另外 N 条操作未列出"标记（标记本身
  是回显内容的一部分）；绑定声明 `identityBindingNote` 始终在场，说明指纹覆盖完整计划
  包括未列出的条目——截断的只是清单，身份回声绑定的仍是完整计划。
- 风险是有序的显式警告：目标无冲突但可证明非空（already-in-place 文件或不在目录创建集
  里的祖先目录证明目标已有内容）、already-in-place 文件在场（无需写入，但执行侧仍必须
  复核其摘要——显式语义而非跳过）、共享设置文件恢复、大型恢复（文件数 >32 或总字节
  >1 MiB）、陈旧备份（创建时间距今 >90 天，`now` 由调用方注入，呈现层不自带时钟），
  以及每一份非 Unpresentable 提示都携带的 `RestoreDoesNotExecuteYet` 披露加散文文案
  （`doesNotExecuteNote`）：当前不存在任何恢复执行路径，沉默会让人以为恢复已经能执行。
- 构建失败的计划绝不能被渲染成可批准的恢复：`buildRefusal` 把计划层的拒绝理由原样
  透传为独立的 `Refused` 状态——没有计划摘要、没有可批准标记（`approvable` 恒假），
  不执行披露依然在场。Unpresentable 的每种失败各有独立代号，诊断前缀
  `extension-restore-presentation-`：`descriptor-corrupt`（清点状态非 ListedIntact）、
  `descriptor-invalid`（字段缺失或备份 id 不可展示）、`now-invalid`、
  `descriptor-mismatch`（描述与计划主体不符）、`destination-mismatch`、
  `subject-invalid` / `subject-unsafe`、`destination-unsafe`、`tree-identity-invalid`、
  `plan-identity-invalid`（畸形身份无法与任何内容对齐）、`entry-path-unsafe`（含双向
  控制字符的路径，手工构造计划驱动——捕获层本就会拒绝它们）、`entry-inconsistent`
  （目录条目夹带文件字段等）、`entry-digest-invalid`（期望摘要形状非法）、
  `operations-unordered`（目录排在文件之后）、`refusal-invalid`（拒绝理由本身不可展示，
  整体拒绝而不是清洗）。绝不清洗、绝不猜测。
- 门禁证据：新增 `extension_staging_restore_presentation`（六个聚焦测试：完整链路——
  真实临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 读回并计划 → 呈现，渲染字段
  与计划逐字段一致（统计、字节、两端指纹各端、完整身份回声）；mcp 主体整文件警告在场
  且文案含"整个共享设置文件/其他服务器"、技能主体缺席且树身份前缀为
  `mcp-backup-content:sha256:`；already-in-place 计划警告与统计如实；20 条操作截断到
  16 条、标记含省略数、身份回声与绑定声明在场；七类不可展示输入各自独立代号；
  构建失败渲染为 Refused、无计划摘要、无可批准标记）。`product_scope_policy` 新增 pin：
  展示安全委派（无第二份安全规则）、mcp 强制警告的有序钉、完整身份回显、截断标记与
  绑定声明、不执行披露、诊断前缀与十五个代号、纯呈现无写盘/存储/计划构建/审批 token、
  无产品接线（含 `mcp_config_dialog.cpp`）、CTest 注册。本机串行分段门禁：注册总数
  101，分段 1–40 / 41–75 / 76–101 墙钟 120.92s / 8.54s / 50.65s，100/101 通过；唯一
  失败仍是 `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续既有
  环境性 PTY 判定）。`git diff --check` 干净。
- 仍未接通：没有对话框/UI 调用方，没有恢复专用的审批策略，没有恢复执行器；OpenSpec
  `0.4` 保持未勾选；Agent/Codex 保持只读。

## Staging Restore Approval (2026-09-05)

- 呈现层回答"能不能问"，此前没有任何东西回答"这个回答是否构成恢复授权"：一份可安全
  展示的恢复提示不等于一份有效批准，而伪造或过期的批准正是把工具输出里的一段文字变成
  "用户要求把这份备份写回目标"的路径。本切片新增 `ExtensionStagingRestoreApprovalPolicy`
  （`include/` + `src/` `extension_staging_restore_approval.*`），形状镜像
  `ExtensionApprovalPolicy`：批准不是布尔值，而是一份与渲染出的提示逐项对齐的凭据。
  它是纯策略层：不执行、不持久化、不写任何字节、没有 UI 接线，也不暴露任何执行钩子；
  批准产出的凭据对象绑定确切计划身份，是纯数据——今天没有任何东西消费它，批准的恢复
  绝不因此获得执行能力。
- 对齐维度：主体、备份 id、目标根、回显的计划身份**与**回显的树身份两者、确切的披露
  警告集合、高风险的逐次确认。两个身份都绑是必要的：计划身份绑定规范化目标根与全部
  操作（目录/文件、路径、字节数、期望摘要、来源槽位、already-in-place 标记），树身份
  绑定内容本身；只批准计划身份会让另一份内容套用同一份操作清单，只批准树身份会让同一
  份内容换一套未获批准的操作或另一个目标——缺一维都是漂移通道。警告集合是凭据的一部
  分：披露过的警告缺一不可（对应风险更少的界面），回传未披露的警告同样拒绝（来自另一
  个界面状态），重复回传与什么都不回传各自独立拒绝。Refused 或 Unpresentable 提示上的
  批准一律拒绝：前者没有可批准的标的物，后者没有人可能看过——走到审批的这类批准要么
  过期要么伪造。
- 高风险集合及其理由：`SharedSettingsFileRestore` 无条件高风险（整文件覆盖越过本主体
  自己的数据边界，覆盖的是包括其他服务器配置在内的整个共享设置文件）；
  `DestinationNotEmpty` 仅当提示渲染出的待写文件数大于零时高风险（向一棵已有内容的树
  真实写入是冲突邻接的：恢复结果是新旧内容的混合，正是恢复悄悄造成损害的边界），非空
  完全由 already-in-place 文件证明时不写入任何字节，是纯信息性。`AlreadyInPlaceFiles`、
  `LargeRestore`、`OldBackup`、`RestoreDoesNotExecuteYet` 不要求确认：不执行披露在每一份
  提示上都在场，要求确认等于让所有人每次点同一个复选框——人人都点的复选框会退化成
  摆设（已记录的先例）；大型与陈旧说的是规模与年龄，计划身份已把每一个字节绑死，它们
  不携带额外的授权维度。未归类的警告类别失败关闭为需要确认，沿用"界外值直接调用
  requiresExplicitConfirmation 触达 default 分支"的钉法。
- 验证状态作为必需输入：沿用准入门禁"必需参数、无默认值"先例，`evaluate` 把备份的
  清点验证状态列为自己的必需参数（不是从提示字段里捡，也没有默认值——"忘记传入"不是
  可达状态）。非 `ListedIntact` 一律 `backup-unverified`：从一份未通过清单身份级验证的
  备份恢复等于从未经认证的字节恢复，比不恢复更糟；等值比较同时让任何未归类的未来状态
  失败关闭。本层绝不从原始字节重新推导计划有效性——提示回显的身份是权威，但验证状态
  必须作为自己的参数进场，因为呈现层的 descriptor-corrupt 防线只有调用方真的走了呈现
  层才存在。
- 可记住范围的决策：不提供任何"记住"范围。恢复是罕见的一次性决定，而任何被记住的
  规则唯一的消费场景是自动放行未来的恢复提示；同一份备份对另一个目标根重新计划就是
  另一份计划（计划身份绑定目标根），必须重新批准。任何宽于确切计划身份的记住范围都会
  把对一份计划的同意转移到另一份计划上，而确切计划身份本身就是凭据绑定的内容——
  可记住的范围没有存在的余地。产品策略钉住头文件里不存在任何 Remember* 形状。
- 诊断前缀 `extension-restore-approval-`，十四个代号各自独立：declined、
  prompt-unpresentable、prompt-refused、backup-unverified、subject-mismatch、
  backup-mismatch、destination-mismatch、identity-invalid、plan-drift、tree-drift、
  warning-duplicate、warning-undisclosed、warning-unknown、confirmation-required。
  身份形状检查枚举两个已知树身份域（extension-content / mcp-backup-content）而不是
  复制"主体种类 → 身份域"的映射——主体与域的绑定由呈现层在渲染时强制，这里按字节
  相等对齐回显身份即可重新绑紧。
- 门禁证据：新增 `extension_staging_restore_approval`（八个聚焦测试：完整链路——真实
  临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 读回并计划 → 呈现 → 逐项对齐
  批准，签发绑定确切计划身份与树身份的凭据，拒绝不产生授权；Refused 与 Unpresentable
  提示上的批准各自独立代号；ListedCorrupt 与界外验证状态即使其余逐项对齐也拒绝；六个
  错位维度各自独立代号（主体/备份 id/目标根/计划漂移/树漂移/畸形身份）；警告纪律——
  缺披露、多回传、重复、空集合；高风险分类——mcp 整文件恢复未确认拒绝、确认后授权，
  冲突邻接（already-in-place 加待写文件）要求确认、纯 already-in-place 与陈旧备份不要求；
  同一份备份对第二个目标根重新计划产生不同计划身份、旧凭据以 plan-drift 拒绝、对齐新
  提示签发新凭据；界外警告值经 evaluate 触达失败关闭分支）。破坏测试：删掉高风险确认
  守卫后 mcp 未确认、冲突邻接、界外警告三处立即失败，已还原。`product_scope_policy`
  新增 pin：双身份对齐钉、验证状态必需参数钉、高风险分类的有序钉（含信息性四者
  return false 与 default 失败关闭）、无 Remember* 范围、展示安全委派（无第二份安全
  规则）、诊断前缀与十四个代号、纯策略无写盘/存储/计划构建/执行 token（含
  `ExtensionApprovalPolicy` 缺席——两个审批域互不借用）、无产品接线（含
  `mcp_config_dialog.cpp`）、CTest 注册。本机串行分段门禁：注册总数 102，分段
  1–40 / 41–75 / 76–102 墙钟 120.06s / 8.49s / 48.35s，101/102 通过；唯一失败仍是
  `agent_runtime_protocol` 的
  `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
  （"terminal did not exit"，已在 LastTest.log 按名确认，与本切片无共享代码，延续既有
  环境性 PTY 判定）。未触碰 agent-runtime，未跑 Rust 门禁。`git diff --check` 干净。
- 仍未接通：凭据没有任何消费方，没有恢复执行器，没有 UI/对话框接线；呈现与审批之间
  没有控制器把它们串起来；OpenSpec `0.4` 保持未勾选；Agent/Codex 保持只读。
