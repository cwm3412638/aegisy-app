# Aegisy Project Memory

Last updated: 2026-07-31

## Mandatory First Step

Read this file completely before performing any repository operation. Update it
whenever architecture, release requirements, security boundaries, incident
conclusions, or major implementation status changes. Never store secrets here.

## Product Goal

Aegisy is evolving from a multi-provider desktop configuration and gateway
client into Aegisy Coding: a native cross-platform coding-agent workbench with
Chat and Work modes, project/session management, an Agent timeline, Monaco,
terminal, Git, approvals, checkpoints, model routing, and Aegisy-hosted model
capabilities.

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
- Provider activation: Claude, Codex, Gemini, and OpenCode have independent
  profiles and activation state.
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
  transport instead of being dropped. The default stdio transport is local but not
  authenticated, encrypted, or peer-verified. The optional macOS Unix socket from
  `4.2` is owner-only and peer-verified but deliberately still unauthenticated;
  Windows named-pipe ACL/peer-validation implementation is now present and wired to
  a dedicated Windows initialization E2E, but task `4.3` remains open until its
  complete negative matrix executes on a clean Windows runner. One-time channel
  authentication remains under OpenSpec `4.4`.
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
  and the Qt/xterm.js flow are runtime-verified; Windows compiles in isolation and
  awaits Windows runner execution before its milestone is complete.
- Initial Agent adapter: installed Codex CLI launched as `codex app-server
  --stdio`, translated into stable AAP sessions, turns, and timeline items. The
  adapter requires pinned `codex-cli 0.144.5` and rejects other versions before
  launch; its generated v2 schema is checked in under `agent-runtime/aap-schema`.
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
  permission, approval, or execution authority. Approval, file-write, Git, and
  background-job producers remain absent.
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

- OpenSpec task baseline: 57 of 235 checkbox tasks are complete and 178 remain
  unchecked. Partial foundations are intentionally not counted until their AAP/Qt,
  persistence, security, and cross-platform evidence gates are complete.
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
  `windows_packaging_policy` CTests pass after the correction. This is not real
  Windows named-pipe signal-order evidence, and the corrected Windows-only source has
  not compiled or executed on Windows. A workflow result from `0f36ae1` cannot close
  the task; rerun the corrected commit. Do not check `4.3` until those negative paths
  and the complete dedicated E2E pass on a clean Windows runner.
  The Rust admission boundary now exposes a private injectable process-identity
  verifier for deterministic tests. Production still queries the real Windows
  process handle and creation time, while the focused negative fixture proves that
  an otherwise live client with the same PID but a different creation time is
  rejected before `VerifiedNamedPipe` construction and before Runtime/Store/Codex
  creation. This is implementation evidence only; the complete remote, Qt
  wrong-server, endpoint/callback, and PID-identity fixture matrix still requires
  clean Windows execution.
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
- `WorkbenchStore` schema version 20 now persists canonical projects and roots plus
  Chat/Work sessions with project binding, environment identity, new/resume/fork
  lineage, and active/archived/failed/interrupted status. Work sessions require a
  project, lineage parents must match project and mode, archive/unarchive is
  timestamp-guarded, and reopen plus v1-to-v2 migration fixtures pass. Turns now
  have bounded idempotency/input hashes and terminal states; items have session
  sequences, turn binding, bounded redacted JSON payloads, and content hashes with
  tamper/gap replay checks. Transactional v1-through-v14 source migrations into v15
  pass; the v3
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
  The normal WAL-consistent migration backup covers v12-to-v13, v13-to-v14, and
  v14-to-v15 plus the populated v15-to-v16 migration. Schema v19 adds the durable
  read-only Workspace Edit Proposal graph, and schema v20 adds the metadata-only
  `mutation_acknowledgements` ledger with a v19-to-v20 migration/backup. Final
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
  v1/v2/v3/v4/v5/v6/v7/v8/v9/v10/v11/v12-to-v13
  migration first uses SQLite Online Backup to capture a WAL-consistent logical
  snapshot, then normalizes it to a standalone `journal_mode=DELETE` database.
- The private `migration-backups-v1` directory retains at most 16 bounded evidence
  sets. One backup is capped at 1 GiB and admission preserves the shared 256 MiB
  free-space reserve. Backup and manifest publication use create-new staging, file
  sync, no-clobber hard links, and directory sync where supported.
- A manifest binds source/target schema version, application ID, exact byte count,
  SHA-256, timestamp, and integrity state. Inventory and manifest reads are bounded.
  Partial files, invalid/unmanifested backups, tampered evidence, and unknown entries
  are preserved and reported; recovery never deletes uncertain evidence.
- Every migration validates all required v12 tables and indexes inside its transaction before
  advancing `user_version` and committing. A newer schema is never downgraded.
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
  deletion, and newer-schema recovery. This is a migration/startup safety boundary,
  not the automatic session-projection repair or Qt recovery UI required by `5.4`.

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
  without Windows SDK headers. The `windows-2022` packaging workflow now runs the
  full Rust test/Clippy suite; task `14.2` stays incomplete until that runner or a
  clean Windows VM supplies real ConPTY and process-tree evidence.
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
  AAP, Qt, Codex, or genuine user decision. Runtime denial, Provider `declined`, and
  `approvalPolicy=never` remain distinct and must never be projected as Approval.

- OpenSpec `3.6` remains unchecked, but the internal
  `file-write-acknowledgement/0.1` contract now covers a future metadata-only
  file-write producer. Operation identity binds Session/project/root,
  idempotency key, request fingerprint, and Workspace Edit identity. Accepted,
  committed, failed, and reconciliation-required revisions are contiguous and
  monotonic; terminal observations require an opaque hash; uncertain state
  cannot be resolved by the producer; and mutation/execution authority are
  fixed false. It is not connected to AAP, Store, Qt, filesystem writes,
  approvals, Git, or jobs. Five focused tests and the `aegisy-agentd` library
  target (763 passed, one ignored live fixture) plus strict Clippy and format
  checks pass.
- OpenSpec `22.5` has a partial Qt foundation. The local
  `aegisy-artifact-manifest/0.1` verifier checks a present sidecar/adapter
  manifest before launch, including fixed identities, versions, bounded sizes,
  canonical in-tree files, symlink/path escape, unknown-field rejection, and
  streaming SHA-256. A failure suppresses launch and automatic reconnect;
  developer builds may omit the manifest. `artifact_manifest_verification` and
  `agent_runtime_environment` pass. Formal manifest generation, Rust adapter
  verification, updater compatibility, signed packaging, and Windows evidence
  remain open.
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
  pinned Codex adapter executable; Rust adapter verification, updater
  compatibility binding, signed release integration, and Windows evidence remain
  required. See `docs/AEGISY-ARTIFACT-MANIFEST-PACKAGING.md`.

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
  misleading Qt 5 fallback diagnostic. The workflow must install
  `qtdeclarative`, `qtpositioning`, `qtwebchannel`, `qtwebengine`, and
  `qtwebsockets`. Both Windows validation and installer construction set the
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
  false. The module has three focused tests and is not connected to Git, Store,
  AAP, Qt, or an approval issuer.
- `mutation_reservation.rs` defines internal
  `mutation-reservation-draft/0.1` as the reviewed bridge from existing approval,
  file-write, Git-mutation, and background-job request metadata toward a future
  ledger. It binds source and scope identities, normalizes fingerprints, and
  classifies exact replay, same Session/kind/idempotency conflict, and unrelated
  requests. Every record fixes schema-v20/Turn-anchor compatibility, persistence,
  dispatch, mutation, Approval, and execution authority to false. Three focused
  tests cover all four source contracts, retry classification, strict round-trip,
  drift, secret shapes, and forged authority.
- Do not insert these drafts into the current schema-v20 table: its database
  constraint admits only `mutation_kind = 'turn-start'` and its transitions require
  Turn Timeline anchors. Approval/file/Git/job persistence needs a reviewed schema
  migration, durable source-record binding, per-kind anchors, atomic Store
  transactions, startup reconciliation, AAP methods, and Qt consumption before any
  producer or dispatch path can be enabled.
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
- All four modules are deliberately not connected to the schema-v20 ledger,
  AAP, Qt, Workbench Store, Codex server requests, secure storage, Git execution,
  or genuine user Approval. Do not advertise these foundations as usable
  mutation, structured-input, credential refresh, or elicitation functionality.

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

## Next Product Priorities

1. Finish OpenSpec `3.10` by obtaining a successful Windows validation run from the
   new clean `windows-验证-源码` checkout. The workflow now runs the complete
   generator and desktop gate there; the Qt consumer migration, generated dispatch,
   exact pending correlation, and safe projection are implemented without changing
   stable `0.1` wire behavior. Do not claim Windows evidence until that run succeeds.
2. Continue OpenSpec `3.5` by obtaining complete Windows reconnect/runtime evidence,
   then add reviewed acknowledgement producers for approval/file/Git/job mutations.
   Durable Turn-start acknowledgement, fixed-watermark replay, structured
   retention-gap snapshot recovery, out-of-band heartbeat, bounded reconnect, and
   live subscribe/sync-or-snapshot/activate are implemented. Keep automatic pruning
   disabled until the remaining mutation-producer and cross-platform gates are verified.
3. Finish OpenSpec `22.5` by generating the trusted manifest in signed packaging,
   binding Rust adapter launch and updater compatibility to the same artifact
   set, then validate it on a clean Windows runner.
4. Validate the hardened TLS installer on a clean Windows x64 VM.
5. Reproduce and correlate any remaining streaming disconnect with redacted logs.
6. Continue consolidating widget-local QSS and replace remaining Qt stock icons;
   the Codex health/restart toolbar state is now covered by the render suite.
   Keep the context-inspection and model-state fixtures in the complete desktop
   render gate as local and CI resource usage changes.
7. Run the Windows packaging workflow or a clean Windows VM to validate ConPTY,
   Unicode, resize, Ctrl+C, exit status, and Job Object process-tree cleanup, then
   close `14.2` without exposing Agent execution permissions.
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
