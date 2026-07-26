# Aegisy Support And Release Recovery Training 0.1

Status: internal training package for support engineers and release owners.
This is the material and sign-off checklist for recovery handling; it is not a
claim that a person, platform, installer, or Windows runner has passed a live
exercise.

## Learning Outcomes

After the session, a trainee must be able to:

- preserve a user's repository, Workbench Store, Timeline, and Session history
  while collecting bounded evidence;
- distinguish `completed`, `failed`, `interrupted`, `reconciliation-required`,
  `unknown`, and `read-only recovery` without inventing a successful outcome;
- use the troubleshooting runbook, privacy export preview, and portable-session
  format for their separate purposes;
- stop retries when provider or mutation ownership is ambiguous;
- refuse TLS verification bypass, permission bypass, manual database edits, and
  any request to expose secrets or raw provider content; and
- state exactly which macOS evidence exists and which Windows evidence still
  requires a clean signed runner or VM.

## Non-Negotiable Red Lines

These rules are tested in every exercise and override ordinary support speed:

1. Never delete, reset, rename, or "repair in place" a user's repository,
   project root, Workbench data root, SQLite database, WAL, migration backup,
   recovery marker, or Session history.
2. Never report success because a process stayed alive, a request returned 2xx,
   a UI spinner stopped, or a provider connection closed. Require an
   authoritative terminal event or a verified recovery result.
3. Never disable certificate validation, hostname checks, Schannel/OpenSSL trust,
   sandbox rules, path checks, permission gates, or read-only capability checks
   to make a test pass.
4. Never ask for or record API keys, access/refresh tokens, authorization
   headers, cookies, passwords, private keys, prompts, source code, diffs,
   terminal output, provider bodies, full paths, PIDs, or raw stderr.
5. Never retry an ambiguous turn, file change, command, Git operation, or
   provider action until ownership and idempotency are re-established.
6. A Runtime denial, `Provider declined`, and `approvalPolicy=never` are not
   user Approval. The current Agent/Codex boundary remains read-only.

## Roles And Evidence

### Support engineer

The support engineer owns first response, user communication, bounded evidence,
and safe escalation. They do not alter the repository or Store. They may ask the
user to choose a destination for a local diagnostic bundle after the privacy
preview, but must not request a raw database or an unredacted transcript.

### Release owner

The release owner owns artifact identity, signing, platform matrix, rollback,
and promotion decisions. They verify the exact sidecar/adapter/schema/package
hash set and maintain the clean-runner evidence record. A macOS pass cannot close
a Windows installer, TLS, ConPTY, Git, scaling, or update gate.

### Incident commander

The incident commander stops repeated retries, records the recovery state and
correlation ID, assigns an owner, and decides whether the affected Session or
release must remain read-only. They do not grant themselves a permission or
approval authority through a support action.

## First-Response Drill

Use this sequence for every report, regardless of the visible error:

1. Record App/sidecar/adapter versions, artifact hashes, platform/architecture,
   runner or VM image, UTC time, stable error class/code, generation, and the
   smallest reproducing test. Redact identity and paths.
2. Ask the user to stop repeated retries. Keep the application and affected
   project/session available; do not delete or move the repository or data root.
3. Use the read-only health/recovery surface. Preserve whether confirmed
   Timeline state remained visible and whether the Session is frozen or
   `read-only recovery`.
4. Start a diagnostic export preview. Select only metadata by default; remove
   any category or Session the user does not want to share. Export locally only
   after the preview hash and redaction report are accepted.
5. Escalate the bundle's support correlation ID, stable classes, and exit codes.
   Do not attach raw logs, database files, screenshots containing secrets, or
   unredacted terminal/transcript content.
6. Select exactly one recovery path below. If authority is unknown, stop and
   preserve evidence rather than guessing.

## Recovery Paths

### A. Sidecar startup, handshake, heartbeat, or reconnect

**Symptoms:** runtime unavailable, version/capability mismatch, initialize
timeout, heartbeat Unknown, or reconnect that cannot activate a subscription.

**Actions:**

- Verify the signed artifact manifest, pinned `codex-cli 0.144.5` adapter, and
  bounded `runtime/health`/`runtime/degradations` state. Do not launch a copied
  binary or use a stale capability as authority.
- Confirm the exact `initialize`/`initialized` state and current process
  generation. Late responses from an older generation are inert.
- A heartbeat Unknown state blocks ordinary business work but keeps confirmed
  Timeline and stop/shutdown controls available. Do not turn Unknown into
  success or silently create a new Session.
- Restart only an exited/unavailable adapter when no model Turn or user
  terminal is active. If subscription ownership is ambiguous, replace the
  connection generation through the bounded reconnect barrier.

**Pass evidence:** the new generation negotiates the expected capability,
replays or snapshots one fixed watermark, activates its Session subscription,
and preserves confirmed history. A process that merely starts is not a pass.

### B. Workbench Store migration, corruption, or reconciliation

**Symptoms:** Store unavailable, schema migration failure, projection mismatch,
retention gap, or a `reconciliation-required` mutation acknowledgement.

**Actions:**

- Stop the app before making a filesystem copy of the data root. Preserve
  SQLite, WAL, migration backup, and recovery markers; never hand-edit rows.
- Use read-only Store status, Timeline snapshot, and mutation-acknowledgement
  list/read paths. A retention gap requires the negotiated snapshot; replay from
  the retained floor is forbidden.
- Treat an accepted Turn-start without a bound Turn as
  `reconciliation-required`; it cannot be redispatched automatically. Require
  exact Session/Turn/sequence/Event-ID anchors before consuming evidence.
- If semantic hashes, sequence continuity, or ownership fail, quarantine the
  affected Session/Store and escalate the preserved evidence. Do not recompute
  hashes around altered content.

**Pass evidence:** a reviewed migration/recovery produces a validated fixed-head
snapshot or an explicit read-only/reconciliation state, with no deleted local
history and no fabricated terminal outcome.

### C. Session Timeline, subscription, or retention recovery

**Symptoms:** missing live events, sequence gap, stale reconnect response,
retention floor error, or a Session frozen during recovery.

**Actions:**

- Bind every request to the exact Session, connection generation, subscription
  attempt, request ID, floor/watermark, and Event-ID. Cross-Session or old
  generation traffic is inert.
- Let Runtime choose fixed-watermark sync versus snapshot. Qt must validate all
  pages privately and replace only the affected Session atomically.
- Keep queued input and the last confirmed projection visible on failure. Do not
  infer a missing event, completed Turn, or successful provider action.
- If the reference is older than the retention floor, explain that history is
  incomplete and use the snapshot path; never claim replay from the floor.

**Pass evidence:** the exact Session reaches Active with a generation-bound
subscription, or remains visibly frozen/read-only with a diagnostic export path.

### D. Windows `TLS initialization failed`

**Symptoms:** installed Windows app cannot initialize secure transport.

**Actions:**

- Confirm the signed installer and bundled Qt/OpenSSL/Schannel assets match the
  release manifest. Never copy DLLs from a developer machine.
- Run the clean Windows x64 VM probe against the production HTTPS endpoint and
  record only certificate-chain result, protocol, hostname/trust-store class,
  architecture, and stable error code. Check clock, supported OS, enterprise
  proxy/inspection, root policy, and architecture.
- Never disable hostname or certificate verification and never fall back to
  plaintext. Do not close the incident from a macOS build or a Windows compile.

**Pass evidence:** the signed clean-VM installed-app probe completes a verified
TLS handshake plus upgrade/rollback checks. Until then, the release gate is
open and the issue remains a Windows external evidence item.

### E. Streaming disconnect or response decode error

**Symptoms:** `stream disconnected before completion`, transport/network error,
or `error decoding response body`.

**Actions:**

- Preserve only the redacted request ID, route/tool class, status, content and
  transfer encoding, bytes, duration, termination class, generation, and retry
  count. Never capture the body or authorization header.
- Correlate local gateway framing/backpressure with Runtime's Started + Error +
  failed Terminal and Turn Trace. A truncated body is not a successful response.
- Reproduce once through the local gateway and once against the same endpoint
  under a deterministic fixture when allowed. Do not change compression/TLS or
  retry policy before identifying the failing layer.
- Retry only a classified transient failure with proven idempotency. A provider
  may have accepted a turn even when the client saw a disconnect.

**Pass evidence:** the failure is classified and durably represented, or a
verified terminal event exists. No support note claims success from a closed
socket alone.

### F. Renderer or WebEngine recovery

**Symptoms:** blank Monaco/xterm pane, stale pane, clipped controls, or a
renderer restart that loses visible state.

**Actions:**

- Confirm the bundled local assets, remote-navigation block, WebChannel request
  ID, and current renderer generation. Do not load a remote debugging page.
- A restart may discard incomplete private staging but must preserve the last
  confirmed Session projection, queued input, and recovery intent. Re-read and
  revalidate the affected Session before enabling Send.
- Mark terminal output unverified if generation/terminal identity cannot be
  checked; never infer process exit.

**Pass evidence:** confirmed history remains unchanged, stale generation traffic
is inert, and the affected surface is either revalidated or clearly unavailable.

### G. Git, terminal, or sandbox report

**Actions:**

- Use read-only, project-scoped Git status/log/diff and exact branch/index
  binding. Redact remotes, paths, commit messages, and patch bodies from
  support evidence. Push, commit, reset, checkout, and conflict actions remain
  unavailable until their permission/approval/checkpoint gates ship.
- Bind terminal output to Session/terminal/generation/range. Reconnect cannot
  prove output, retain it as unverified and do not infer exit status.
- Treat sandbox denial, Runtime denial, Provider `declined`, and genuine user
  Approval as distinct. Do not weaken sensitive-path, symlink, executable,
  network, or shell-wrapper checks to make a fixture pass.

**Pass evidence:** the read-only state is authoritative and unchanged, or the
  action is blocked with a clear policy reason. No repository mutation is used
  as a diagnostic step.

## Relationship Between The Three User Artifacts

| Artifact | Purpose | What it may contain | What it must not be used for |
| --- | --- | --- | --- |
| `docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md` | first-response symptom checks and safe recovery | bounded operational classes, commands, release gates | collecting raw logs or authorizing a mutation |
| `docs/AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md` | category consent, redaction, preview, and local-first diagnostic bundle | exact metadata categories plus explicitly opted-in redacted content | automatic upload, database backup, or a substitute for a recovery decision |
| `docs/PORTABLE-SESSION-FORMAT.md` | explicit redacted Session history export/import | bounded Session Items after a separate content preview | provider continuation, checkpoint restore, credentials, or diagnostic evidence |

Use the runbook to choose a path, the privacy document to decide what may leave
the machine, and the portable format only when the user explicitly wants to
move redacted history. A diagnostic bundle cannot be imported as Session
history, and a portable Session package cannot prove Runtime health or TLS.

## Release Owner Gate Review

Before a release or support incident is closed, the owner records:

- artifact manifest/hash set, signing channel, platform/architecture, runner or
  VM image, exact command names, and exit codes;
- migration/recovery, reconnect, TLS, streaming, renderer, Git, and sandbox
  evidence relevant to the claim;
- whether confirmed Timeline/Store state survived and which recovery method was
  used; and
- any open Windows evidence item and its owner.

The record excludes prompts, code, diffs, terminal output, credentials, headers,
provider bodies, full paths, user identifiers, PIDs, and raw stderr. A macOS
pass is never substituted for a Windows installer/TLS/ConPTY/Git/scaling/update
pass. A release owner must block promotion when evidence is missing, ambiguous,
or only inferred from process liveness.

## Training Exercises And Sign-Off

Each trainee completes the following with a disposable fixture and records only
bounded pass/fail evidence:

1. Sidecar heartbeat timeout followed by a generation-bound reconnect.
2. Store retention gap followed by a validated snapshot without deleting the
   fixture Session or repository.
3. Accepted Turn-start with uncertain dispatch, showing
   `reconciliation-required` and no redispatch.
4. Streaming decode failure with redacted gateway diagnostics and a failed
   terminal, without retrying an ambiguous operation.
5. Windows TLS probe review in a clean-VM evidence template, explicitly marked
   unverified when no Windows runner is available.
6. Renderer restart and read-only Git/sandbox review that preserves confirmed
   state and rejects mutation.
7. Diagnostic export preview and portable-session preview, removing content
   categories before any local file is written.

The sign-off records exercise IDs, fixture/build hashes, platform, date,
trainer, and pass/fail outcome. It contains no user data. A failed exercise
requires retraining; it never authorizes a live customer repair.

## Escalation And Closure

Escalate when ownership, integrity, redaction, or platform evidence is unknown.
The incident remains open until a reviewed recovery result or an explicit
read-only limitation is recorded. Closure must state what was preserved, what
was not verified, and which user controls remain available. "No further error
observed" is not equivalent to a verified success.
