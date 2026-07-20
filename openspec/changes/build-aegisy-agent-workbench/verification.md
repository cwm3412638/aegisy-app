# Verification Plan

## OpenSpec

- Run `openspec validate build-aegisy-agent-workbench --strict`.
- Confirm all ten capabilities listed in `proposal.md` have one delta spec.
- Confirm every requirement has at least one four-hash Scenario and normative
  SHALL/MUST language.
- Confirm `tasks.md` is fully machine-trackable through numbered unchecked boxes.

## Architecture Evidence Required Before Implementation

- UI spike report comparing Qt WebEngine and standalone Tauri fallback.
- AAP generated-schema round-trip across Rust, TypeScript, and C++.
- Authenticated IPC hostile-client test report.
- Codex App Server and ACP adapter fixture report.
- macOS and Windows sandbox feasibility report.
- Model catalog/schema and scoped-token backend review.
- Third-party license, attribution, and redistribution review.

## Milestone Evidence

Each milestone in `roadmap.md` must attach:

- Automated build/test/security/evaluation output.
- Signed package smoke-test matrix for claimed platforms.
- Failure-injection and recovery results.
- Performance against predeclared budgets.
- Data/privacy review for new persisted or transmitted fields.
- Screenshots or recordings for responsive, IME, accessibility, approval, error,
  and recovery states.
- Known limitations, rollback path, and feature-flag state.

## Current Implementation Verification

Implementation is in progress and does not claim that the full workbench, model
catalog, sandbox, or autonomous feature set is complete.

Autonomy release-gate evidence:

- OpenSpec `21.1` is complete. `runtime/degradations` reports bounded,
  content-free disabled gates for background jobs, multi-agent execution, and
  unattended writes. Each gate is explicitly `not-advertised`, has
  `stable_enabled=false` and `override_available=false`, and names the missing
  prerequisite tasks. The initialize response advertises no autonomy capability,
  and protocol coverage rejects a hidden background dispatch method. No
  scheduling, child execution, Agent write, or unattended mode is enabled by
  this gate.

Structured-plan foundation evidence:

- An internal `structured-plan/0.1` contract validates bounded step IDs,
  statuses, owners, dependency references/cycles, content-free SHA-256
  evidence, and required completion evidence. Base/evidence revision changes
  mark affected steps stale without silently changing their status. Four unit
  fixtures cover valid graphs, cycle/unknown dependency rejection, revision
  drift, and secret/non-hash rejection. AAP/UI persistence, plan questions,
  durable job state, and execution remain unavailable under task `21.2`.

Child-task contract evidence:

- An internal `child-task/0.1` request contract binds parent session/turn,
  bounded goal and context identities, project/root isolation, tools, model and
  permission policy, six independent budgets, and a bounded
  `child-handoff/0.1` result shape. Validation grants no permissions or
  execution authority. Four unit fixtures cover identity, unsafe scope,
  duplicate/budget bounds, and secret/result rejection; lineage, approvals,
  scheduling, worktree allocation, and executor integration remain open under
  task `21.3`.

Child-task lifecycle evidence:

- An internal `child-task-state/0.1` state machine validates parent/child
  identity, generation-bound transactional status transitions, cancellation
  request/rejection/acknowledgement, completion races, generation exhaustion,
  and content-free bounded handoff references/counts. Six unit fixtures cover
  binding, invalid transitions without partial state, idempotent and rejected
  cancellation, cancel-vs-complete races, exhaustion, and handoff rejection.
  Durable child-session lineage, navigation, AAP status/cancel methods, and
  parent review are still unavailable under task `21.4`.

Child worktree admission evidence:

- Internal `child-worktree-admission/0.1` validates the exact child-task and
  runnable lifecycle identities against the existing live Git worktree owner,
  base revision, registration, lock, path, health, and clean admission state.
  Five real-Git fixtures prove shared read-only behavior, healthy write-capable
  admission, mandatory dedicated isolation, cross-child reuse denial, dirty
  denial, and cancellation denial. The returned runtime proof is
  non-serializable, while its content-free receipt explicitly grants neither
  permission nor execution. Durable scheduling, project-root registry binding,
  per-tool revalidation, production permission/approval/sandbox intersection,
  executor integration, AAP/Qt controls, and Windows evidence remain open under
  task `21.5`.

Child runtime budget evidence:

- Internal `child-runtime-budget/0.1` reserves non-zero token/cost ceilings before
  model admission and transactionally tracks wall time, turns, tools, active
  concurrency, and policy-bound network requests. Authoritative and estimated
  usage retain provenance; unknown token or cost usage consumes the full
  reservation rather than becoming zero. Content-free snapshots return limits,
  used/reserved/remaining values and warning/saturated/exhausted dimensions but
  no operation IDs, content, permission, or execution authority. Six unit fixtures
  cover warnings, conservative unknown settlement, overcommit with no partial
  state, concurrency saturation/recovery, independent tool/network limits,
  network-policy mismatch, wall exhaustion, clock regression, and generation
  exhaustion. Provider usage, Runtime monotonic-clock integration, durable
  scheduler/session events, cancellation/refund rules, executor admission, AAP/Qt
  events, and endurance/cross-platform evidence remain open under task `21.6`.

Unified execution pipeline evidence:

- Internal `unified-execution-plan/0.1` emits the same ordered 14 stages for
  interactive, child, and background envelopes. Requirements are derived from
  mode and mutation policy rather than caller-selected omissions: child work
  requires budget/release and write isolation/approval when mutating; background
  additionally requires a durable job, notification, and unattended approval.
  Invalid child/job/unattended identity relabelling fails. The existing Codex
  read-only interactive `turn/start` invokes the planner immediately before real
  adapter dispatch, and all eleven stdio adapter tests pass through that path.
  Six unit fixtures prove exact cross-mode stage order, current interactive
  readiness, child/background missing-gate projection, binding rejection, and
  common reconciliation failure. Plans expose no permission or execution ticket.
  Typed proof composition, child/background dispatch, generic executor ownership,
  durable job/session events and recovery, budget/provider settlement, AAP/Qt mode
  status, and cross-platform evidence remain open under task `21.7`.

Background job lifecycle evidence:

- Internal `background-job-request/0.1` and `background-job-state/0.1` validate a
  content-free, persistence-ready job record with project/session/root, unified
  execution plan, optional child, idempotency, manual/one-shot schedule, bounded
  attempts/backoff, and optional safe retry boundary. Transactional transitions
  separate pause/cancel requests from acknowledgement, preserve waiting approval,
  bind every terminal attempt to evidence, and allow completion to win a cancel
  race. Restart marks active work interrupted rather than successful; only a
  pre-bound safe boundary can make it retry-eligible, and decisions never auto
  retry or approve. Schedule/retry backoff and cancellation cannot be bypassed by
  pause/resume/start. Six fixtures cover completion, pause/approval, cancellation
  race, idempotent bounded retry, queued/running restart, invalid schedule, and
  generation fail-closed behavior. Scheduler/process ownership, authoritative
  approval, notification, AAP/Qt control, and endurance/cross-platform evidence
  remain open under task `21.8`.
- Workbench schema v10 adds a bounded `background_jobs` projection whose canonical
  request/state JSON, SHA-256, request/state identity, generation, status,
  cancellation, attempt count, schedule, and timestamps are revalidated on every
  read and during a 10,000-row startup scan. Create and generation-CAS update commit
  with typed `background-job.*` events in one database transaction; identical
  retries are idempotent, stale state fails, and an injected event failure leaves no
  projection row. Active records block deletion/retention and terminal records are
  purged with their session. Four store fixtures cover reopen/event replay, rollback,
  tamper-triggered read-only startup recovery, deletion protection, and WAL-consistent
  v9-to-v10 backup migration. This is durable metadata only: no scheduler, process
  observation, automatic recovery, AAP/Qt control, notification, or execution exists.

Current editor evidence:

- The macOS Qt build and all 16 desktop tests pass.
- The Monaco render test opens two real workspace files, binds them to separate
  editor groups, switches native save targeting through group focus, verifies an
  atomic save cannot modify the other group file, and restores both groups in a
  new workbench instance.
- The native Qt render test verifies that the single-editor safety fallback keeps
  split controls disabled when WebEngine/Monaco is unavailable.
- Workspace search tests cover filename/text matching, Git and sensitive-path
  exclusion, page append, cancellation, stale cursors, result-to-editor navigation,
  and UI stale indicators after a user save.
- Repository indexing tests cover official Tree-sitter symbol extraction for Rust,
  Python, JavaScript, TypeScript/TSX, and C/C++; dependency edges; unchanged-file
  reuse; changed/deleted-file refresh; sensitive, symlink, and Git-ignore exclusion;
  symbol ranges/provenance; and token-budget truncation with focus-file ranking.
- The native workbench test opens a real Rust fixture, lazily builds its index,
  renders the symbol and dependency views, and verifies a non-empty repository map.
- Language-server tests cover bounded `Content-Length` framing, Unicode file URI
  mapping, workspace/symlink denial, server discovery, restricted rust-analyzer
  configuration, lifecycle stop, and real clangd definition and diagnostic flows.
  The AAP protocol and native workbench tests exercise an installed clangd with
  UTF-16 cursor mapping, unsaved document diagnostics, result navigation, and
  explicit unavailable behavior when clangd is absent.
- Observed-diagnostic tests cover SHA-256 identities and raw references, bounded
  artifact truncation, source snapshot replacement, fresh-to-stale invalidation,
  AAP raw retrieval, and the Qt provenance/raw/stale workflow. Raw artifacts contain
  only normalized diagnostics that already passed workspace path filtering.
- Turn-context tests cover authoritative file rereads, changed-revision stale
  metadata, ignored/outside-path denial, item/UTF-8/total byte limits, explicit
  truncation markers, structured AAP transport, and the Qt add/include/remove/send
  workflow across file, selection, search, diagnostic, terminal, and Git sources.
- Workspace/index edge tests cover Unicode paths, case sensitivity, symlinks,
  rename/delete/watch behavior, external edits and stale saves, 5,000-file and
  20,000-symbol limits, project-scoped cancellation, late-result suppression, and
  preservation of the previous complete index snapshot.
- macOS PTY tests cover executable shell discovery, scrubbed environment, canonical
  project-root revalidation, Unicode byte I/O, resize, nonzero exit status, failed
  startup, 1 MiB tail capture, foreground process-group interrupt, close, Work-mode
  binding, cross-session denial, and absence of an Agent terminal-open method.
- Named terminal lifecycle tests cover one foreground terminal per Work session,
  unique Unicode-aware background names, list/attach with absolute output offsets,
  cross-session denial, invalid-restart preservation, successful restart generation,
  stop-to-exit polling, exited-terminal removal, and runtime cleanup. Legacy
  read/close methods remain protocol aliases; no Agent terminal input method exists.
- The native Workbench and local xterm.js 6.0.0/FitAddon 0.11.0 render test run a
  real macOS PTY end to end, verify fitted nonblank rendering, streamed UTF-8 bytes,
  native clipboard copy/paste, exit observation, cleanup, CSP/local-only loading,
  and native-output fallback. A WebEngine renderer restart reloads the trusted page
  and reattaches to the runtime-owned bounded output tail by generation and offset.
- Codex command-translation fixtures are generated against the installed App Server
  0.144.5 schema and cover command started/outputDelta/completed ordering, stable
  item identity, parsed actions, cwd/status/process/source fields, duration, exit
  result, Unicode-safe 256 KiB output retention, 32 KiB timeline display, and
  conservative risk classification. The Qt renderer treats command/model output as
  selectable plain text. The Codex parent process now uses the shared allowlisted,
  secret-scrubbed environment builder and exposes its value-free hash; individual
  child-process environment identity remains vendor-unreported, so task 14.5 is not
  complete and no new Agent execution permission is exposed.
- Task `7.10` has deterministic redacted fixtures for initialization, session/provider
  lifecycle, partial streaming, usage/plan/diff metadata, approval denial, cancellation,
  reconnect, compaction degradation, provider failure, unloaded provider state, and
  failed compensation. Protocol tests parse every JSONL message, reject credential-
  shaped content, and assert stable error codes and recovery transitions. Real stdio
  tests cover the corresponding adapter paths. The Qt render fixture injects opaque
  provider failures for restart, archive, unarchive, and fork, then proves only the
  operation, stable code, and recovery guidance reach labels/tooltips. This fixture
  milestone does not expose provider delete/compact or weaken the read-only adapter.
- Large command-output tests cover a Unicode-safe 64 KiB head/192 KiB tail,
  1 MiB artifact head/tail, exact omission metadata, 100,000 deltas, and
  content-addressed session isolation/eviction. Output is redacted before capture;
  assignment, authorization, known token, JWT, split-delta, unterminated, and long
  secret cases are absent from the inline timeline, artifact, and AAP response.
  Codex stdout uses a 4 MiB bounded frame reader and fixed 16-message synchronous
  queue; adversarial tests prove exact-limit framing, oversized-frame draining, and
  producer backpressure/unblocking. The Qt render test verifies the session-bound
  full-output action; its dialog is read-only plain text and artifacts are never
  attached to model context implicitly. Task 14.6 is complete.
- Cancellation tests pin Codex App Server `0.144.5` `turn/interrupt` fields, keep
  request/acknowledgement/terminal states distinct, bind requests to exact active
  session/turn identity, and cover duplicate, stale, and completed races. A real
  sidecar stdio test holds a fake Codex turn open, saturates the 32-entry normal
  request queue, observes stable overload errors, then proves out-of-band cancel,
  vendor acknowledgement, and `turn.interrupted`. The Qt render test verifies
  stable Stop/Stopping/Send states. A second macOS process fixture uses the same
  blocked-turn and saturated-queue condition to prove out-of-band user-terminal
  stop, exact terminal ownership, PTY exit, and foreground-child cleanup through
  the shared terminal registry. Task 14.7 remains incomplete because native Agent
  command/daemon process producers do not exist to supply their process-tree
  cancellation evidence and Windows Job Object execution still needs a runner.
- Command-diagnostic fixtures cover rustc/cargo, Clang/GCC, MSVC/TypeScript,
  Ruff/mypy/Pyright, ESLint, pytest, Rust panic, ANSI, Unicode-safe bounds, 200-item
  truncation, and unknown-command rejection. Production ingestion revalidates cwd
  paths through project/sensitive/Git-ignore/symlink/text/revision policy and
  sanitizes command metadata before events. A real sidecar/fake-Codex stdio flow
  proves command delta ingestion, `diagnostics.observed`, forced small-output
  artifact authority, project-scoped normalized raw retrieval, and Qt provenance/
  navigation rendering. Task 14.8 is complete; live Windows compiler/process
  coverage remains in 14.9.
- macOS PTY process fixtures preserve raw ANSI and Unicode bytes, cover interactive
  resize/nonzero exit and failed workspace startup, interrupt a real foreground
  command, stop a named terminal while model dispatch is blocked, and exercise the
  fixed HUP/TERM/SIGKILL escalation against a process group that ignores graceful
  signals. The forced path is time-bounded and leaves no live foreground group.
  Task 14.9 remains incomplete pending equivalent clean-Windows ConPTY/Job Object
  execution and future gated native Agent command/daemon producers.
- Workspace-edit schema tests cover versioned create/update/delete/rename JSON,
  canonical root identity, lowercase SHA-256 base/content descriptors, content-
  reference binding, normalized UTF-8 relative paths, operation/path bounds,
  duplicate target denial, round-trip validation, forged root identity, path
  escape, ambiguous separators, and reference tampering. Task 15.1 defines only
  the internal contract; no AAP apply method or Agent write capability exists.
- Workspace-edit preview tests reread raw base bytes, verify SHA-256 identity, and
  cover create/update/delete/rename unified diffs, aggregate/per-file additions and
  deletions, Unicode content, inline/source truncation, content-addressed paging,
  session/project/root denial, store bounds, missing/mismatched/oversized content,
  and sensitive/Git-ignore/stale/symlink/existing-target warnings. Sensitive base
  contents are absent from preview artifacts. A real Qt/sidecar render flow pages a
  diff larger than 64 KiB, renders the Changes table and sensitive-path warning,
  and proves disk contents remain unchanged. The protocol fixture also proves the
  unimplemented apply method returns method-not-found. Task 15.2 is complete while
  public/Agent mutation remains gated behind later permission/approval milestones.
- Workspace-edit apply tests cover same-directory staged create/update content,
  immediate optimistic base/target rechecks, no-clobber hard-link backups, all four
  operation kinds, reverse rollback after partial and complete multi-file commit,
  exact final SHA-256 values, sensitive/Git-ignore/symlink/existing-target denial,
  and cleanup of hidden transaction files. A stage-time external change is rejected
  before commit. A post-commit external rewrite is preserved instead of overwritten,
  returns authoritative path state, reports rollback incomplete, and retains the
  original backup as a named recovery artifact. Task 15.3 is complete as an internal
  library transaction only; AAP apply remains absent. Deterministic failure injection
  is covered by task 15.9, while real Windows filesystem execution remains external
  evidence.
- Workspace-edit overlap tests bind proposal and post-apply restore baselines to the
  validated edit/project/root identity and exact absent/SHA-256 path states. They
  cover all create/update/delete/rename source and target roles, disk create/delete/
  content changes, declared unsaved editor paths, sensitive/Git-ignore/symlink
  denial, forged apply/root/final-hash data, 512 KiB file bounds, and the 8 MiB
  aggregate hash-read budget. Reports contain hashes and stable conflict/resolution
  codes rather than source content. Task 15.4 is complete as a reusable internal
  read-only detector; checkpoint capture/restore and UI mutation remain later tasks.
- Git-checkpoint fixtures initialize a real dirty repository and prove that an
  isolated `refs/aegisy/checkpoints/*` plumbing commit retains exact touched-file
  preimages plus a bounded manifest while leaving the worktree and semantic index
  tree unchanged. The manifest separates staged, unstaged, untracked, declared
  unsaved, redacted-sensitive, `agent-only`, and `agent-on-user-base` state; a
  post-apply bind verifies ref/commit/tree/manifest/blob OIDs, root/gitdir, HEAD,
  branch, index tree, operation roles, and final hashes. Negative fixtures cover
  ref overwrite, tampering, index drift, stale bases, ignored/symlink/pending paths,
  unsafe refs, parent repositories, and external metadata roots. Git commands use a
  project-external absolute executable, cleared minimal environment, disabled hooks
  and prompts, and bounded output. Task 15.5 is internal only; no AAP mutation exists.
- Non-Git checkpoint fixtures persist exact touched-file preimages into a disjoint
  project-external SHA-256 blob/manifest store, reopen it as a new store instance,
  verify duplicate-blob coalescing, and bind all create/update/delete/rename roles
  to a real committed apply result. Descriptor, manifest, pointer, blob, root, store,
  mode, byte-count, and final-hash bindings are checked before use. Negative tests
  cover sensitive pending-path redaction, stale/ignored/pending/symlink paths, Git
  worktree refusal, overlapping storage, manifest/blob/pointer tampering, oversized
  preimages, and conservative object/byte admission limits. Every result carries the
  explicit weaker guarantee and four concrete limitations. Task 15.6 adds no AAP
  checkpoint/apply/restore method.
- Restore fixtures construct plans from both Git and non-Git verified preimages,
  group rename source/target under one operation selection, restore one operation
  before the remaining full set, recognize already-restored/no-op paths, and prove
  an unrelated later user file remains byte-identical. Conflict execution requires
  the exact reviewed current hash; missing confirmation, stale review, duplicate
  selection, ignored/policy-unavailable state, and pending editor state fail before
  mutation. Unix coverage additionally proves that full restore recreates a deleted
  executable with its executable class intact. Task 15.7 is complete as an internal
  sidecar surface; no restore AAP or Qt action exists.
- Workspace-edit format fixtures require `workspace-edit/0.2` descriptors and bind
  exact content bytes to UTF-8/BOM, none/LF/CRLF, and preserve/regular/executable
  intent. Legacy/missing descriptors, mismatched bytes, mixed or lone-CR endings,
  binary/NUL content, oversized proposals, and binary/mixed/oversized bases fail
  before mutation. Final and parent symlinks are denied without touching their
  targets. Unix fixtures prove BOM+CRLF byte preservation, executable create and
  promotion, executable preservation through update/rename, explicit demotion to
  regular, checkpoint mode-tamper rejection, and executable full restore. Windows
  explicitly rejects executable intent because this layer has no durable POSIX mode
  representation; a real Windows SDK/runner remains required and is not claimed by
  this macOS evidence. Task 15.8 adds no AAP/Qt mutation surface.
- Transaction failure fixtures inject partial stage writes, `StorageFull`, and
  permission denial at deterministic boundaries. They prove incomplete stages are
  removed before visible mutation and a post-commit permission failure restores
  exact bytes and permissions. A child test process exits immediately after an
  update commit without unwinding; its parent verifies the exact installed target,
  retained exact preimage backup, and stale-base rejection on retry. Existing
  fixtures cover stale-after-stage and rollback-incomplete after an external
  rewrite. Task 15.9 does not claim automatic crash-journal replay or substitute
  for real disk-quota, ACL, forced-termination, and Windows runner tests.
- Git-status fixtures parse bounded porcelain-v2 NUL records and branch headers for
  ordinary, rename/copy, unmerged, untracked, initial, branch, detached, upstream,
  and ahead/behind state. Real repositories prove canonical discovery from a nested
  project directory, staged/unstaged/untracked/rename classifications, detached
  HEAD, non-repository behavior, and a conflicting merge with `MERGE_HEAD`. The AAP
  contract reports `git-status/0.2` while retaining Qt's `path/status` fields. Git
  execution uses a project-external absolute binary, cleared minimal environment,
  fixed locale, disabled prompts/optional locks/fsmonitor/config/hooks, 2 MiB output,
  and 5,000 entries. Task 16.1 is read-only and adds no Git mutation method.
- Git-query fixtures exercise `git-query/0.1` overview, paged log, root-commit
  parsing, commit detail, and worktree/staged/commit diffs in a real repository
  containing a nested opened project. Full-OID and path traversal rejection, 2 MiB
  command bounds, project-prefix `:(top)` pathspecs, sensitive file and sensitive
  rename exclusion, sibling-path exclusion, and credential-bearing remote URL
  authority redaction are asserted. AAP coverage proves project binding and keeps
  stage/branch/create-commit/push methods absent. Qt render coverage requires the
  summary, four-column history, three-scope selector, icon refresh, read-only diff,
  and real diff context action. Task 16.2 adds no Git mutation surface.
- Internal branch-transaction fixtures bind create/switch/rename plans to canonical
  repository root, exact HEAD/current branch and target/source OID, clean porcelain
  state, bounded validated names, protected patterns, and linked-worktree occupancy.
  Execution fully replans before mutation and verifies final branch/HEAD. Real Git
  tests cover dirty/protected/stale/worktree denial plus injected rollback for all
  three operations. Task 16.3 remains unchecked and unreachable from AAP/Qt until
  permission and approval gates can authorize user-visible Git mutation.
- Internal dedicated-worktree fixtures bind plans and descriptors to canonical
  repository/common-dir and external storage roots, exact base HEAD/branch, unique
  path/branch, and session/child owner. Real Git tests cover create plus lock,
  health refresh, clean terminal-child cleanup eligibility, dirty/stale/overlap/
  reused/symlink/invalid-name denial, owner and branch tampering, missing/prunable
  state, pending editor/retention gates, and complete rollback after injected
  post-add and post-lock failures. Task 16.4 remains unchecked because no durable
  scheduler association or permission/approval-backed AAP/Qt create, remove, or
  integrate action exists.
- Internal staging fixtures start from a real workspace edit, Git checkpoint,
  apply result, and bound application rather than fabricated Agent ownership.
  Content-free plans bind the checkpoint object graph, HEAD/branch, current index
  tree/file identity, exact Agent results, path/mode state, and stable hunk IDs.
  Real Git tests prove selected Agent hunks merge into the existing index while a
  distant user staged hunk remains staged, a user unstaged hunk remains excluded,
  an unselected Agent hunk remains unstaged, and the worktree remains byte-identical.
  Additional fixtures cover create/delete/rename, mode-only selection, overlapping
  hunk refusal, stale index, pending editor, competing lock, capture-time/later
  staged delete protection, exact post-install rollback, and preservation of an
  external index rewrite with rollback-incomplete evidence. Task 16.5 remains
  unchecked and unreachable from AAP/Qt pending permission, approval, durable
  events, hunk review, and unstage product controls. Windows cross-check still
  stops in existing Tree-sitter C before this module because Windows SDK headers
  are unavailable on the macOS host.
- Internal commit fixtures continue the real edit/checkpoint/apply/stage authority
  chain. A three-tree preview uses the pre-Agent index as merge base, HEAD as ours,
  and post-Agent index as theirs; the resulting exact patch/commit contains only
  the Agent delta while a same-file user staged hunk remains in the real index and
  a user unstaged hunk remains only in the worktree. Raw commit-object verification
  proves exact tree/parent/message/author/committer/timezone metadata and the failing
  executable hook marker proves disabled-hook plumbing does not execute hooks.
- Additional commit fixtures cover message-source and identity policy, detected
  hooks/custom hooksPath without path disclosure, Run/sign blockers, custom merge
  driver refusal without marker execution, invalid control characters/email, stale
  index receipt, compare-and-swap ref update, exact post-update rollback, and
  preservation of an external ref rewrite with rollback-incomplete evidence. The
  final internal result is `git.commit.completed`, but task 16.6 remains unchecked
  until permission/approval-backed AAP/Qt review, durable event storage, sandboxed
  hook output artifacts, secure signing, and Windows execution evidence exist.
- Workflow-state fixtures cover execution-complete `git-workflow-plan/0.2`
  requests for stash, three merge modes, rebase, single-parent cherry-pick, and
  exact-generation abort/continue. Plans bind roots, HEAD/branch/index, target OIDs,
  stash ref, predicted behavior, message source, identity/timezone, hook/signing
  policy, pending editor state, and value-free hazards. Tests reject ambiguous or
  missing merge metadata, impossible fast-forward, already-contained targets,
  merge-commit cherry-pick without mainline, invalid message/identity, custom
  drivers, protected rebase, stale plan/OID/generation, and unsafe dirty state.
- The private project-external `git-workflow-record/0.2` store survives reopen,
  detects payload/hash/path/symlink/semantic tampering, and serializes transitions
  through an OS advisory lock plus generation/record CAS. Its durable attempt journal
  records prepared/dispatching/observed/recovered phases without source content.
  Real conflicts retain exact stage mode/OID evidence while sensitive names are
  count-only; foreign/unknown operations are inspect-only.
- Authorization hashes both the exact record and freshly regenerated action plan.
  Separate short-lived allow-once permission and approval decisions are required
  for every medium/high action, including a newly classified high-risk abort, and
  replay/forged/stale scopes fail. The internal executor then proves real stash,
  fast-forward and merge-commit, rebase, cherry-pick, conflict continue, and abort.
  Hooks are bypassed only under explicit Disabled policy; a failing hook marker is
  not executed. Restart fixtures recover live conflicts, exact completed fast-
  forwards, and unknown dispatches conservatively. Task 16.7 remains unchecked:
  production authority/issuer, AAP/Qt flow, complete session projection, sandboxed
  hooks, signing, conflict UI, and Windows runtime evidence are absent. A partial
  `WorkbenchStore` now provides a private project-external SQLite WAL schema with
  full synchronous writes, application/schema integrity checks, durable decision
  rows, atomic decision consumption plus `approval.consumed`, and typed event
  replay with payload hashes and sequence-gap detection. Six focused store tests
  cover reopen/application identity, atomic rollback, replay/integrity, and direct
  authority rechecks. It remains internal and
  does not claim the production authority, migrations/backup/compaction, or UI
  workflow.
- The internal `permission-profile/0.1` boundary defines Chat, Read Only,
  Workspace Write, Developer, and Full Access profiles plus managed-policy
  intersection. Effective checks cover canonical writable/read-only roots,
  sensitive and denied paths, symlink components, shell wrappers and executable
  allowlists, network host patterns, extension IDs, browser hosts, and background
  execution. A private Git decision issuer accepts permission decisions only from
  a matching profile authority and explicit approvals only with a distinct,
  bounded user-gesture ID; read-only profiles and managed denials fail before any
  SQLite row is written. This remains an internal policy/issuer foundation and is
  not connected to AAP, Qt, native execution, or a production user-gesture bridge.
- The SQLite store now carries schema version 10 project/session/Blob/retention/job metadata and
  turn/item projections:
  canonical project roots and access, Chat/Work session mode, project binding,
  environment identity, `new`/`resume`/`fork` lineage, active/archived/failed/
  interrupted status, and archive/unarchive transitions. Turns bind to an active
  session, support bounded idempotency keys and input hashes, and transition from
  started/running to terminal states. Items use a session-local monotonic sequence,
  bounded redacted JSON payloads, content hashes, and turn binding. Replay detects
  sequence gaps and payload tampering; terminal turns reject late items. Work sessions cannot be
  created without a project; lineage parents must match project and mode; invalid
  rows are rejected before insertion. Reopen tests verify metadata durability and
  transactional v1→v10 through v9→v10 migrations. The v3 path rebuilds the event table
  with nullable project binding while preserving every existing event field, hash,
  and sequence; a new Chat session then proves a typed event can carry no project.
  The complete
  extensions/model-profile/checkpoint and complete scheduler/job recovery schema remains unchecked. Runtime
  integration now persists project/session metadata, Preview turns, and completed
  Codex timeline items under the Qt host's platform application-data Workbench root;
  `AEGISY_WORKBENCH_DATA_ROOT` remains an explicit developer/test override;
  `session/read` replays the bounded durable item projection after sidecar restart
  and labels the runtime as a read-only durable replay. `project.created`, session
  create/title/status, Turn create/terminal, and sanitized Item append events commit
  in the same transaction as their projection. An injected event-insert trigger proves
  a title mutation and event sequence allocation both roll back. Fresh sessions register
  event-source version 1; v3-migrated sessions have no marker and remain explicitly
  legacy/non-rebuildable. Richer runtime/environment reconstruction and future
  mutation event coverage remain unchecked.
- Every supported v1/v2/v3/v4/v5 source now receives a WAL-consistent SQLite Online
  Backup before migration. The standalone DELETE-journal backup and bounded JSON
  manifest bind source/target schema, application ID, exact bytes, SHA-256,
  creation time, and integrity state under a private no-clobber directory. Admission
  enforces a 1 GiB file limit, 16 retained evidence sets, a 256 MiB free-space
  reserve, and bounded inventory/manifest reads. Valid backups, unmanifested files,
  invalid manifests, interrupted temporary files, and tampered evidence are all
  handled conservatively; uncertain evidence is reported and never deleted.
- Migration fixtures prove v1/v2/v3/v4/v5 state preservation, full required-schema
  validation before `user_version` commit, schema-collision rollback with the
  original v4 state and backup intact, safe re-entry after an uncommitted migration,
  exact preservation of random corrupt database bytes, low-space rejection, backup
  tamper detection without deletion, and refusal to downgrade a newer schema.
  Failure starts a content-free read-only Runtime before Codex launch. Initialize
  advertises only four recovery/read-only capabilities, ordinary AAP methods return
  `-32120`, and status/export contain no database content, project paths, credentials,
  raw SQLite errors, or stored display data. Tasks `5.6` and `5.7` are complete.
- The schema-v6 store retains the schema-v5 durable Blob design and keeps project-external SHA-256 objects in a
  private, sharded, no-clobber filesystem and keeps exact bytes, media/kind,
  content reference, session/project/source owner, hashed bounded metadata,
  access/verification time, state, and retention deadline in SQLite. Command
  artifacts commit in the same `IMMEDIATE` transaction as their Item projection
  and typed event; injected event failure rolls back the database rows and removes
  the newly committed object. Changes preview content/diffs persist as an atomic
  batch and both command output and patch paging survive sidecar restart with the
  original session/project/edit scope. Media fixtures cover patch, binary image,
  diagnostic JSON, workspace-edit, generic, and empty artifacts.
- Blob admission enforces 16 MiB/object, 8,192 objects, 512 MiB total, and a
  256 MiB free-space reserve where capacity is available. Every read verifies
  file type, exact size, and SHA-256. Release retains at least a 24-hour undo
  window; GC refuses active, not-yet-expired, missing, corrupt, unknown, or
  unregistered objects. Fixtures cover deduplication, cross-session denial,
  reopen, metadata-secret rejection, low-space rejection, size/hash corruption,
  missing files, rollback cleanup, active-reference retention, grace-window GC,
  unknown orphan preservation, and v4→v6 migration through the Blob schema step. Task `5.3` is complete.
- The session-projection consistency and rebuild layer is internal and bounded. It checks
  project and lineage binding, turn hash descriptors, item sequence and same-session
  turn ownership, item payload integrity, event sequence cursor state, and event
  payload integrity without returning content or paths. A focused fixture first
  proves a healthy projection, then injects a cross-mode lineage parent, deleted item,
  cross-session turn binding, tampered item/event payloads, and a stale event cursor;
  every failure becomes a stable issue code and requires either a proven automatic
  projection rebuild or read-only recovery. Initial durable `session/read` enforces
  this report, while older-page requests retain page-local hash/sequence validation.
  Typed metadata/Turn/Item events replay into a separate candidate and compare exactly
  with the current projection. Repair requires a complete registered source and
  `event-projection-mismatch`, revalidates a
  SHA-256 identity of the whole event stream under an `IMMEDIATE` write lock, atomically
  replaces session/turn/item rows, and appends content-free rebuild audit evidence.
  Fixtures repair metadata, lineage, cross-session Turn binding, and missing Item rows;
  a post-review event append rejects a stale candidate. Deleting a registered source is
  detected rather than mistaken for legacy. Session verification now also checks
  durable reference ownership, projection owners, metadata integrity, and referenced
  file size/hash. A separate bounded content-free scan reports missing/corrupt objects,
  dangling references, malformed entries, and unregistered files while preserving
  every uncertain orphan. Startup performs a union scan of session rows, registered
  sources, and event-only streams before Codex launch. It caps identities at 10,000
  and each Turn/Item/event/Blob-reference class at 100,000, repairs complete sources,
  reconstructs simultaneously missing parent/child session rows in dependency order,
  and escalates scan/query/limit failures into whole-store read-only recovery.
  Pre-commit validation repeats project, lineage, Blob owner/content, source count,
  and complete stream-hash authority.
- Uncertain sources enter per-session quarantine. Store methods reject metadata,
  Turn, Item, Blob/Artifact, Git authorization/event, and GC mutations before their
  transactions; Runtime blocks session-bound AAP writes while retaining cancellation
  and terminal cleanup. `runtime/projection-recovery/status`,
  `session/recovery/status`, and `session/list.recovery_required` expose content-free
  state. Qt renders distinct whole-store, automatic-rebuild, aggregate quarantine,
  and current-session banners, disables unsafe composer/session/terminal/workspace
  controls, and keeps healthy sessions usable. Render tests exercise each state.
- Fixtures repair title/Item drift on first replay, rebuild missing parent/child rows
  at startup, preserve tampered event authority without an audit rewrite, block
  quarantined Store and AAP mutation plus Blob GC, and enter content-free whole-store
  recovery when startup limits are exceeded. Project creation and additional roots
  now append one bound project stream; startup scans projects before sessions and
  reconstructs an entirely missing two-root project with independent read/write
  scopes before its Work session is verified. Stored absolute canonical root text
  remains replayable while a directory is offline, whereas tampered project authority
  quarantines the project and every bound session without rewriting evidence.
- Project and session rebuild transactions have both deterministic storage-failure
  and real process-exit evidence. SQLite insert-abort triggers fire after destructive
  projection clears and prove the complete previous project/root or session/Turn/Item
  projection remains visible with no rebuild audit. Separate child test processes exit
  directly from the actual `IMMEDIATE` transaction after the clears; a raw reopen sees
  the prior complete projection, and normal startup then rebuilds it from the unchanged
  event stream. Task `5.4` is complete. Migration diagnostic export remains implemented
  under `5.6`.
- Database maintenance and low-space fixtures verify the shared write boundary rather
  than one mutation type. All ordinary `IMMEDIATE` transactions first require the
  256 MiB reserve plus 8 MiB transaction headroom and remain below a 1 GiB combined
  database/WAL ceiling; rejection leaves session, source-registration, and event rows
  absent. A verified artifact remains readable under low space without an access-time
  write, while release and other mutations fail content-free. A competing writer holds
  `BEGIN IMMEDIATE` and proves the configured two-second busy wait returns below five
  seconds without a partial row.
- Configuration fixtures assert WAL, `synchronous=FULL`, 1,000-page auto-checkpoint,
  16 MiB journal retention, and database page ceiling. Explicit maintenance requires
  `reserve + headroom + 2*database + WAL`, rejects one byte below that boundary, then
  truncates WAL, runs `VACUUM`, clears the freelist, and passes `quick_check`. Projection
  recovery blocks maintenance. Together with the WAL-consistent pre-migration backup
  and recovery fixtures under `5.6`/`5.7`, this completes task `5.5`.
- The read-only `session/list` AAP method now returns bounded project/mode/archive
  filtered metadata from either the in-memory Runtime or the durable SQLite store.
  Qt consumes the result in the left session rail, prioritizes current-project Work
  sessions, and invokes `session/read` when a row is selected; a restart fixture and
  the two Workbench render targets verify durable timeline replay. `session/read`
  now returns the newest 100 items by default, accepts 1-200, exposes item sequence
  metadata, and pages backward through strict `before:<sequence>` cursors. Protocol
  fixtures verify exact non-overlapping newest/middle/oldest pages and reject malformed,
  non-canonical, out-of-range cursors and invalid limits. The SQLite restart fixture
  verifies the same boundary from durable storage. Qt exposes a hidden-until-needed
  load-more control, prepends older items, and restores the prior scroll anchor.
  Task `6.8` is complete; project relink and full event-projection rebuild remain
  separately unchecked under `6.4` and `5.4`.
- AAP `session/search` now performs a bounded SQLite-side Session query by project,
  exact model/runtime/status, title, or a combined title/approved-transcript query.
  Transcript matching is restricted to `message` Items with `user`/`assistant` roles
  and visible `text`/`content`/`output`/`diff` fields; diagnostic and command payloads
  do not match, and the Runtime does not hydrate every transcript. Results carry
  runtime binding metadata, matched-field evidence, purged-session exclusion, a
  strict `after:<updated-at>:<session-id>` cursor, and a 100-result cap. Additive
  SQLite schema v9 migration creates and verifies indexes for status/order, model/runtime
  binding, and transcript ownership through the existing WAL-consistent backup gate.
  The Qt left rail debounces title/transcript search, scopes Work to the current
  project, renders explicit empty results, and restores the recent list when cleared.
  Store, protocol, and render fixtures cover approved-field isolation, model/runtime
  filtering, cursor canonicalization, UI matching, empty state, and clear-to-restore.
  Task `6.6` remains unchecked because branch/worktree identity is not yet persisted,
  branch requests are explicitly unavailable, and complete indexed-text scale and
  cross-platform evidence remain open.
- AAP `operation/reconcile` now validates content-free event/process/workspace/Git
  evidence through `operation-reconciliation/0.1` and appends a metadata-only
  `operation.reconciled/0.1` event to the session stream when durable storage is
  configured. Identical evidence is idempotent; the latest result per operation is
  validated from the event hash and survives Runtime restart. Unknown, running, or
  blocked results reject later session-bound mutations with `-32132`, while a newer
  authoritative terminal review clears the gate. Capability negotiation advertises
  `operation.reconciliation`; store and protocol fixtures cover persistence,
  restart blocking, idempotency, and unblocking. Task `6.9` remains unchecked because
  the current method does not perform authoritative host probes automatically and
  exposes no recovery or recovery-action controls; Qt review is limited to an
  explicit, identity-checked turn-only probe/reconcile flow.
- Capability `operation.reconciliation.probe` now exposes read-only
  `operation/probe`. It resolves only a registered root through a Work session,
  hashes bounded visible workspace metadata, reads the existing structured Git
  status query, and observes runtime-owned turn/terminal state. Probe responses
  contain state labels and snapshot hashes but no content, arbitrary paths, or
  caller-selected PIDs. Event state may be caller-supplied or derived from the
  bounded durable turn/Git mappings described below; the probe does not persist,
  approve, mutate, or recover an operation. Task `6.9` remains unchecked until
  complete startup discovery, authoritative event sourcing, Qt review, and
  recovery actions are integrated.
- Durable Runtime startup now scans and validates the latest
  `operation.reconciled/0.1` event per session/operation pair into a bounded
  cache; the SQLite event stream remains authoritative for request-time gating.
  A malformed, missing, or over-limit scan cannot be interpreted as a safe
  operation result. This is partial startup discovery and does not complete task
  `6.9` without automatic operation source registration, Qt review, or recovery.
- When `operation/probe` omits `event`, durable Runtime derives the latest
  validated registered `turn.created`, `turn.completed`, `turn.failed`, or
  `turn.interrupted/cancelled` state and the existing `git.workflow.*` lifecycle
  state from the session event stream; prepared/dispatching/in-progress Git
  events are only running evidence, completed/failed/aborted are terminal
  evidence, and conflicted/recovered remain unknown. Explicit event values
  remain caller-labelled. Fixtures prove durable Turn and Git lifecycle mapping,
  while malformed Git payloads remain rejected without inferring success. Other
  operation event families remain unknown until their authoritative sources are
  registered.
- Capability `operation.reconciliation.status` exposes a read-only
  `operation/status` snapshot while a session is blocked. It returns only the
  bounded review summary and `recovery_action_available:false`; it cannot clear
  the gate or invoke recovery. Qt renders the same summary and, for a blocked
  turn, can explicitly collect `operation/probe` evidence and record a validated
  `operation/reconcile`; malformed, stale, or unsupported operation responses
  remain fail-closed. Render coverage verifies the blocked, review, and cleared
  transitions.
- The manual session-compaction checkpoint AAP methods are now reachable from the
  Qt session context menu. Create collects bounded user-authored summary fields
  and optional preservation instructions; read accepts an explicit checkpoint ID;
  revise creates a new immutable checkpoint from an exact source Review ID while
  preserving the old object and recording a validated `supersedes` descriptor.
  Both results and revision results render in a plain-text read-only dialog and require
  `activation_available:false`, `provider_compact_invoked:false`, matching session
  identity, and a non-empty review ID before display. Rust protocol/store fixtures
  cover conflicting IDs, stale source identity, idempotent revision, restart replay,
  and tampered lineage; the Qt render fixture covers client request serialization and
  revision signal dispatch. No activation, provider compact, startup compensation, or
  model-generated summary producer is exposed;
  task `6.10` remains unchecked.
- The partial `context-manifest/0.1` foundation is emitted with `turn/start` when
  structured context is present. It records only bounded source/kind/priority/
  trust/hash/token/freshness/inclusion metadata, labels stale file revisions,
  exposes `turn.context.manifest`, and never includes attachment text. Rust unit
  and protocol fixtures verify deterministic metadata, conservative token sizing,
  truncation, and absence of a `content` field; task `17.1` remains unchecked until
  instruction, budget, inspector, tokenizer, and provider-scale consumers exist.
- The partial instruction-discovery/0.1 foundation exposes read-only
  workspace/instructions. It binds a registered project root, applies the
  deterministic weakest-first precedence managed > user > nested (closer depth
  wins) > project, and returns only bounded provenance, precedence, hash,
  revision, token, freshness, inclusion, truncation, and rejection metadata unless
  the caller explicitly requests bounded content. Managed/user sources are
  restricted to path-only environment configuration; project sources use the
  existing sensitive, symlink, built-in-ignore, and Git-ignore policy. Content is
  always marked untrusted-data and cannot authorize permissions, commands,
  Hooks, or network. Unit and protocol fixtures cover nested target selection,
  deterministic ordering, case collisions, secret/control/symlink denial,
  stale-read checks, and size/count bounds. Work-mode turn/start now appends up
  to eight valid discovered instructions after explicit user context, through
  the same bounded context preparation and manifest; secondary-root attachments
  do not select a primary-root instruction chain. Task 17.2 remains unchecked
  until durable managed/user configuration, complete exclusion/budget reporting,
  policy/trust review, context inspection, and cross-platform evidence are
  complete.
- The partial context-budget/0.1 allocator is emitted with each prepared
  turn context. It deterministically scores instruction precedence and pinned
  context, allocates within a 64 KiB total/16 KiB per-item hard bound without
  reordering rendered context, and returns requested/allocated bytes, class,
  priority score, inclusion, and content-free reason metadata. Unit fixtures
  cover priority ordering, excluded entries, and hard bounds; the protocol
  fixture verifies the budget schema. Task 17.4 remains unchecked until all
  context classes, tokenizer/provider window authority, and scale evidence use
  this allocator.
- AAP turn/context/inspect is a read-only preflight that reuses the exact
  instruction discovery and budget preparation path for the bound session.
  It returns context-inspector/0.1, manifest/budget metadata, and explicit
  content_included:false, model_started:false, and persisted:false flags;
  protocol coverage proves instruction/source bodies never enter the response.
  Qt exposes a preflight action for an existing session and renders a read-only
  source/type/trust/size/status/reason table; the Workbench render fixture
  exercises the control and metadata-only result. Unchecked client context is
  represented as an explicit exclusion marker. Task 17.6 remains unchecked until complete context classes,
  redaction/exclusion explanations, provider/tokenizer authority, and
  cross-platform evidence are complete.
- Session metadata management now has bounded `session/title`, `session/archive`,
  and `session/unarchive` AAP operations. Store timestamp guards prevent stale
  projection rewrites; Runtime checks reject archival during an active turn or
  running/stopping terminal, and archived sessions reject new turns, terminals,
  and workspace-edit previews. The Qt session context menu exercises rename,
  archive visibility, and restore in the Workbench render fixture. Resume, fork,
  and Codex thread lifecycle mapping remain unchecked; immutable create, title,
  archive, unarchive, and deletion lifecycle events now pass store replay/rebuild fixtures.
- Schema v6 adds project/session retention overrides and two-phase deletion state.
  `session-only` and descendant-lineage previews bind the complete bounded member,
  status/update, Turn, Item, event, and Blob-reference impact to a SHA-256 plan;
  only the first 200 affected sessions are displayed, while the hash covers up to
  10,000 members. Scheduling repeats the plan under an `IMMEDIATE` write lock,
  requires a 24-hour-to-30-day undo window, and freezes all selected sessions.
  Runtime checks every selected descendant for active Turns and running/stopping
  terminals; Store policy sweeps additionally protect durable live Turns, issued
  approvals, recovery quarantine, and the host-provided live-session set.
- A pending deletion remains readable but rejects session metadata, Turn, terminal
  input/start, edit-preview, Blob, approval, and authorization writes. Undo deletes
  the pending plan only after immutable audit append succeeds. Due purge atomically
  removes session content and authority rows, releases Blob references, extends
  physical retention by at least another 24 hours, and installs a minimal archived
  tombstone so existing child lineage foreign keys remain valid. Normal list/read
  paths and startup recovery hide/exclude purged tombstones, while host maintenance
  evicts resident timeline, command-artifact, edit-preview, and terminal caches before
  they can bypass durable deletion state.
- AAP now exposes `session/delete/preview`, `session/delete/schedule`,
  `session/deletion/status`, `session/delete/undo`, retention policy read/set/remove,
  and host-triggered retention/GC maintenance. Qt provides delete-scope choice,
  exact impact review, destructive confirmation, a seven-day undo period, pending
  row/banner state, disabled mutation controls, Undo, and project/session policy
  dialogs. Six focused Store fixtures, one Runtime deletion/maintenance/restart
  fixture, one external AAP contract test, and the Workbench render flow cover stale
  plans, rollback, live-work blocking, pending read/freeze, undo, purge, delayed Blob
  GC, policy precedence/removal, tombstone restart, and Qt state. Task `5.8` is complete.
- Portable session fixtures verify export category/warning previews, stale-preview
  rejection, a second secret scan, registered project-root replacement, and removal
  of provider response IDs, encrypted/hidden reasoning, cache/continuation handles,
  environment identity, and local artifact/content references. Version, content-hash,
  sequence, duplicate Item ID, and reintroduced opaque-field tampering fail with
  stable errors before import writes. The package contract and its 4 MiB/2,000 Item
  limits are documented in `docs/PORTABLE-SESSION-FORMAT.md`.
- Import fixtures prove Work requires an active target project, previews source
  Session/Item collisions, enforces `reject` inside the SQLite write transaction,
  and makes `copy` allocate a new Session plus deterministic remapped Item IDs. A
  readable same-store source with matching mode/project creates fork lineage; source
  history remains unchanged. Injecting failure on `session.imported` proves Session,
  Item, event, and projection-source rows all roll back. Candidate rebuild, normal
  consistency verification, raw reopen, and restart replay accept the validated audit
  event without changing projected Session/Turn/Item state.
- External AAP coverage verifies `session.portable.export/import` capability
  negotiation, export preview/hash commit, import preview, reject/copy behavior,
  tampered-package error mapping, immediate history reads, and restart consistency.
  Qt adds a visible import command and per-session export command, complete content
  and collision review, 4 MiB file admission, `QSaveFile` atomic output, target-project
  binding, and imported-session selection. Both Workbench render targets pass with
  isolated temporary data roots. Task `5.9` is complete; portable history explicitly
  omits provider continuation and does not complete model/runtime switching tasks.
- Task `5.10` adds one persistence-secret invariant shared by SQLite projection/event
  writes, Item replay, event replay, projection-source replay, canonical roots,
  display/title text, identity/identifier fields, and durable Blob metadata. Nested
  credential-like fields and recognized API-key/JWT/authorization values are rejected
  before event sequence allocation; user/model Item text is redacted first, then the
  sanitized bytes and hash are used identically for `items.payload_json` and
  `item.appended` events. A direct event-gate failure after a transient projection
  update rolls the transaction back with no sequence, event, or projection side effect.
  Project/session display rejection, nested JWT redaction, Blob metadata rejection,
  and zero-row assertions are covered by Store fixtures. A legacy source-less session
  export proves historical title re-redaction, while an injected hash-consistent secret
  event is refused during replay and quarantines its session on reopen.
- The Qt host now sanitizes the process environment before starting `aegisy-agentd`.
  A dedicated CTest fixture proves API keys, Aegisy login/refresh tokens, cloud
  credentials, and authenticated proxies are absent while the Workbench data root and
  ordinary model settings remain. The sidecar client has no secure-storage/token API
  and therefore cannot pass raw desktop credentials through AAP parameters.
- The complete verification run for this batch passes 232 Rust unit tests, 22 AAP
  protocol tests, 3 stdio/Codex contract tests, 16 desktop CTest tests, strict Clippy,
  `cargo fmt --check`, `git diff --check`, and `openspec validate --strict`. Task `5.10`
  is complete; provider-specific secret registration and clean Windows package runtime
  evidence remain later security/release work.
- Task `6.1` now derives project root identity from filesystem metadata rather than
  canonical path text alone. On Unix it binds device/inode; the Windows implementation
  binds volume serial/file ID, and unsupported platforms use a deterministic path
  fallback. `project/open` returns the original project ID for a moved root with
  `availability=moved`, candidate root, and `relink_required=true`; it returns the
  original project as `unavailable` for an exact missing saved root. No new project row
  or trusted root binding is created in either case. Legacy path-hash identities are
  migrated atomically through a typed project event, and project replay applies the
  migration. A runtime fixture renames a real directory, reopens both moved and missing
  paths, checks the stable ID/candidate metadata, and confirms only one durable project
  remains. Explicit relink mutation and navigation UI remain task `6.4`.
- User-save/watch regression coverage configures a live directory watch before an
  atomic user save and proves the next poll is empty, while the existing external
  write fixture still produces a stale-revision conflict. The full Qt render flow
  passes repeatedly without self-save changes being mislabeled as external edits.
- The Windows ConPTY module and its test targets pass isolated
  `x86_64-pc-windows-msvc` check and Clippy from macOS. The Windows packaging
  workflow now runs the full Rust tests and Clippy before packaging and requires
  `aegisy-agentd.exe` in the distribution. Task 14.2 remains incomplete until that
  runner executes its ConPTY/Job Object tests or equivalent clean-VM evidence exists.
- Session-environment tests cover allowlisted inheritance, canonical PATH filtering,
  project-directory removal, case-insensitive credential masking, dangerous loader
  variable rejection, value/count limits, deterministic session identity, explicit
  terminal derivation, and value-free AAP metadata.
- Two hundred and ninety-eight Rust sidecar unit tests, fifty AAP protocol tests,
  eleven sidecar stdio/Codex contract tests, and sixteen desktop CTest tests pass;
  Clippy passes with warnings denied.

Known limitations:

- Split groups are a Monaco capability; the emergency Qt text fallback remains a
  single editor.
- Command-output artifacts remain runtime-memory-only until the durable event/blob
  store is implemented. Redaction is deliberately conservative and heuristic;
  future secret-provider registration remains part of the security-governance
  milestone. One Codex JSON message is rejected above 4 MiB rather than streamed
  through a second framing protocol.
- Search and index cancellation immediately supersede the client result and ignore
  late responses. The current synchronous sidecar may finish the already bounded
  active search page or index request before it reads the queued cancellation.
- Repository indexing is currently file-level incremental and bounded to the
  workspace scan limits, 8 MiB of newly parsed text, 20,000 symbols, and 10,000
  dependency edges. Dependency targets are syntax-derived import/include labels,
  not language-server-resolved definitions. Semantic definition/reference support
  remains a separate language-server path rather than part of the syntax index.
- Repository-map token usage is conservatively estimated as four Unicode
  characters per token and is not yet connected to the turn-wide context allocator
  planned in 17.4.
- Language servers are discovered from fixed command names or explicit path-only
  environment overrides and are not bundled by this milestone. This macOS host
  has end-to-end evidence for clangd; rust-analyzer, Pyright, and TypeScript
  Language Server still require installed-server and clean Windows validation.
- LSP requests are synchronously bounded to 4 MiB frames, 512 KiB documents, 500
  results, and fixed startup/request/diagnostic timeouts. Server-initiated
  `workspace/applyEdit` is denied, process credentials are scrubbed, and relative
  or project-root `PATH` executables are rejected, but the
  external language-server binary is not yet OS-sandboxed. Rust analysis therefore
  runs in restricted standalone mode and intentionally lacks full Cargo workspace,
  dependency, proc-macro, build-script, and compiler-check precision.
- Observed diagnostics and their normalized raw artifacts are intentionally
  in-memory only. Process restart durability remains a later event-storage
  milestone; build/test/lint diagnostic parsing remains task 14.8. The raw artifact
  is the filtered Aegisy authority payload, not the original unfiltered LSP wire
  message.
- Terminal excerpts now come from the real runtime-owned PTY/xterm surface. Git diff
  context and Git commit/diff pins come from the filtered read-only Git query rather
  than fabricated text. macOS PTY and named lifecycle are runtime-verified, while
  Windows ConPTY is implemented but awaits Windows execution.
- The internal `pinned-context/0.1` contract validates content-free descriptors for
  every 17.3 source kind. Unit fixtures cover kind support, project/duplicate
  binding, deterministic identity, unsafe absolute/parent/network references,
  secret-shaped metadata, item size, and aggregate size bounds. The runtime now
  opens `pinned-context-store/0.1` beside the Workbench store and exposes
  metadata-only AAP list/save/remove methods when it is healthy. A successful
  mutation appends a separate content-free `project.pinned-context-updated/0.1` event
  after object publication; project projection replay validates its set/object
  identities, bounds, and `content_bodies_persisted:false` invariant. Protocol
  coverage verifies project/root/session scope, restart recovery, event replay,
  idempotent persistence, compare-and-swap stale-write rejection, and that
  responses contain no body. Cross-resource event/object/Blob atomicity remains
  incomplete. Standard `*:sha256:` Blob references are checked through a
  read-only metadata query for active project/session ownership and exact
  hash/byte identity; no Blob body is read and no access timestamp is updated.
  Capability `turn.context.pinned-selected` now binds an explicit selected file/
  selection-pin ID list to the exact current set identity for both
  `turn/context/inspect` and `turn/start`. The shared resolver rechecks
  project/session/root scope, rereads the file under workspace policy, compares
  raw-byte SHA-256 and revision, marks drift stale, and for selections extracts
  only bounded metadata line/column ranges while keeping inspection metadata-only.
  Session-owned artifact pins additionally resolve only validated command-output
  text references with matching UTF-8, byte count, and SHA-256; the protocol
  fixture verifies durable Blob reload after Runtime restart and turn assembly.
  Project/root-bound diagnostic pins resolve only normalized
  `diagnostic-raw:sha256:` content from the authoritative in-memory DiagnosticStore
  after media-type, reference, SHA-256, and byte-count validation. Clearing that
  source, as occurs across Runtime restart or eviction, makes inspection fail closed;
  the persisted descriptor is never treated as a body.
  Session-owned terminal pins use a strict
  `terminal-excerpt:<terminal>:<generation>:<start>:<end>` reference. Assembly
  re-reads the Runtime-owned retained PTY range, applies the same ANSI/OSC and
  control normalization, and validates session, generation, offsets, SHA-256, and
  byte count. Runtime/terminal restart, removal, generation change, and capture
  eviction therefore fail closed instead of using the descriptor as content.
  Primary-root Git commit/diff pins use strict full-OID or fixed-scope references,
  re-run the filtered read-only query, and compare the bounded content hash plus the
  complete normalized source hash/byte count/truncation state. Worktree/staged drift
  fails stale, while commit-backed context remains valid only while its exact object
  is available. Duplicate/missing IDs, stale set identity, cross-session selection,
  and invalid image/child-handoff identities fail explicitly. Qt now
  loads project pins and covers authoritative file and editor-selection pin creation,
  CAS persistence, range metadata, per-turn inclusion, bounded order changes, and
  unpin in the Workbench render fixture. The Structure diagnostics surface also
  exposes explicit diagnostic pin/unpin: Qt rereads `workspace/diagnostics/raw`,
  validates project/root, media type, reference, SHA-256, and UTF-8 byte count, and
  submits only a metadata descriptor through CAS. Qt terminal context separately
  exposes transient selected text and persistent `固定最近输出`; the latter reads a
  bounded `terminal-excerpt/0.1` authority, validates it, and saves only the
  descriptor. The Git surface exposes explicit `固定差异` and `固定提交`; a real Git
  render fixture validates both authority reads, CAS persistence, labels, and unpin.
  Real Runtime and render fixtures cover assembly, metadata-only inspection,
  pin/unpin, mutable Git drift, and failure after source removal. The focused
  sidecar invalidation fixture persists `fresh` to `stale` transitions for
  root-scoped file/selection/diagnostic descriptors after workspace changes and
  terminal-excerpt descriptors after terminal lifecycle changes, then reopens the
  Runtime and verifies every stale marker remains durable. The Qt render flow
  consumes the sidecar-advanced identity and proves a stale terminal/selection pin
  can still be explicitly unpinned without weakening stale-CAS rejection. Remaining image/
  child-task production/lineage handoff, cross-resource atomicity, and complete cross-platform
  pin evidence remain incomplete; task 17.3 remains unchecked. The child-handoff
  fixture proves a parent-session-scoped text Artifact Blob is revalidated by
  owner, source-session, media type, UTF-8, hash, and byte count before assembly;
  inspection remains metadata-only; invalid source identity remains a bounded error.
  Qt workspace-watch and user-save callbacks mark loaded
  matching file, selection, and diagnostic pins stale locally; terminal
  restart/removal marks matching terminal-excerpt pins stale. None of these local
  indicators rewrite durable metadata; the sidecar reread remains authoritative.
  The command-output Artifact dialog now receives an additive originating
  `session_id` from `artifact/read-command-output` and exposes an explicit
  `固定完整输出` control only for the active project-bound Work session after
  client-side UTF-8/reference/SHA-256 validation. The control assembles a
  session-owned metadata-only `artifact` descriptor (`metadata.item_id`, priority
  700, retained UTF-8 byte count) and submits it through the existing CAS path;
  it remains disabled for cross-session, recovery, deletion, reconciliation-blocked,
  busy, invalid-identity, or unsupported-media responses. Render coverage proves
  current-session enabled and cross-session disabled states. A real command
  Artifact click-to-persist fixture is still pending because the render runtime
  has no command producer; durable Artifact Blob reload and turn assembly remain
  covered by the protocol fixture.
- `pinned-context-store/0.1` persists metadata-only sets using private immutable
  content-addressed objects and atomic project-pointer replacement. Store fixtures
  cover reopen/update, idempotency, absence of a content body field, retained old
  objects, object/pointer tampering, update refusal on damaged current authority,
  and symlinked-layout denial. Cross-resource event/object/Blob atomicity, Blob
  release/lifecycle binding, Windows execution, remaining image/child-handoff
  assembly, complete Git lifecycle, and their Qt
  surfaces remain open.
- Windows packaging, TLS runtime, scaling, IME, and accessibility evidence remain
  required before a Windows release claim.
