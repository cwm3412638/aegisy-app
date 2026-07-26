# Aegisy Coding Troubleshooting Runbook

Status: internal operational guide
Scope: desktop host, `aegisy-agentd`, Codex adapter, Workbench Store, gateway,
terminal, Git, sandbox, and local renderer.

This runbook is for collecting bounded evidence and choosing a safe recovery
path. It is not permission to disable validation, bypass TLS, expose secrets, or
retry an ambiguous mutation. Never put API keys, login tokens, authorization
headers, source content, full user paths, provider bodies, or raw stderr in a
ticket or `PROJECT-MEMORY.md`.

## First Response

1. Record platform, App version, sidecar version, adapter pin, build/installer
   hash, UTC time, and the visible error class. Redact user identity and paths.
2. Preserve the affected project/session. Do not delete the Workbench data root,
   SQLite database, WAL, or recovery files before an export/backup is made.
3. Stop repeated retries when ownership is ambiguous, a Session is frozen, or a
   mutation may have reached a provider. Use read-only health/recovery methods.
4. Run the smallest focused check, then the complete local gates. Attach only
   exit codes, bounded counts, stable error codes, and content-free diagnostics.

## Sidecar Startup Or Handshake

**Symptoms:** `runtime unavailable`, initialize timeout, protocol mismatch,
`required capability was not negotiated`, or the Agent workbench remains in
`确认能力未知`.

**Checks:**

- Confirm the bundled `aegisy-agentd` exists, is executable, and has the expected
  SHA-256. Do not launch a sidecar copied from another build directory.
- Check `runtime/health` and `runtime/degradations`; these expose state, version,
  restart recommendation, and bounded stderr counters, never raw stderr.
- Verify the Qt host sent the exact `initialized` notification after a valid
  `initialize` response. A missing capability is a contract result, not a UI
  rendering problem.
- On macOS run `ctest --test-dir build -R 'agent_runtime_protocol|agent_runtime_environment'`.
  On Windows use the packaged runner command and preserve the signed artifact.

**Recovery:** restart only an exited/unavailable adapter with no active Turn or
user terminal. A heartbeat Unknown state blocks new work but keeps Stop and
shutdown controls available. Never replace an unavailable capability with a
cached success.

## Workbench Database And Recovery

**Symptoms:** Store unavailable, read-only recovery, `session/read` failure,
`reconciliation-required`, schema migration failure, or a Session frozen after
reconnect.

- Preserve the data root and make a filesystem copy while the application is
  stopped. Include SQLite, WAL, migration backup, and bounded recovery marker.
- Use the read-only recovery/status, Timeline snapshot, and mutation-acknowledge
  list methods. A retention gap requires the negotiated snapshot method; replay
  from the floor is forbidden.
- An accepted durable Turn-start operation without a bound Turn is converted to
  `reconciliation-required` at startup and must not redispatch automatically.
  Confirmed accepted/terminal anchors are consumed only after exact Session,
  sequence, and Event-ID validation.
- Do not manually edit SQLite rows. Semantic tampering quarantines the affected
  Session or Store and must be repaired from an authoritative export or reviewed
  migration, never by recomputing hashes around altered content.

**Checks:**

```sh
jq empty agent-runtime/aap-schema/stable/v0.1/aap.schema.json
openspec validate build-aegisy-agent-workbench --strict
cargo test --workspace --manifest-path agent-runtime/Cargo.toml
```

## Codex Adapter Mismatch Or Crash Loop

**Symptoms:** adapter unavailable, exact-version rejection, repeated startup
attempts, provider thread unavailable, or `runtime/restart` blocked.

- The supported pin is `codex-cli 0.144.5`; an unpinned version is rejected before
  launch. Confirm the installed binary version without recording its path or
  output body.
- `runtime/health` reports only bounded stderr bytes/lines/redaction counts and a
  last class. A version mismatch or protocol rejection is not transient and must
  not be retried three times.
- Follow `docs/CODEX-ADAPTER-UPGRADE.md` for candidate schema generation,
  fixture review, signed artifact retention, and rollback. A rollback restores
  sidecar, adapter, schema, and hashes as one set; it does not rewrite history.

## Streaming Disconnect And Response Decode Errors

**Symptoms:**
`stream disconnected before completion: Transport error: network error: error
decoding response body` or a provider stream ends before a terminal event.

1. Preserve the content-free upstream class (`transport`, `provider`, `timeout`,
   or `adapter`) and timestamp. Do not log the response body or authorization
   header.
2. Compare gateway request/response byte counts, HTTP status, content encoding,
   and reconnect generation. Confirm that the local proxy preserves streaming
   bytes and backpressure and does not parse an incomplete chunk as JSON.
3. Check whether Runtime persisted Started + Error + failed Terminal and whether
   the Turn Trace is present after restart. A missing terminal event is an
   uncertain operation, not a successful retry signal.
4. Retry only when the adapter marks the failure transient and no mutation or
   provider action could have been accepted. Otherwise freeze/reconcile first.

The known network-error message is not proof of a TLS problem. Correlate it with
redacted gateway diagnostics, response framing, proxy encoding policy, provider
status, and the exact adapter version. Reproduce against a deterministic fixture
before changing retry or decode behavior.

## Windows TLS Initialization Failed

**Symptoms:** the installed Windows app reports `TLS initialization failed`.

- Verify the installer was built by the hardened packaging workflow and that the
  bundled Qt runtime, OpenSSL/Schannel selection, and signed assets match the
  release manifest. Do not copy DLLs from a developer machine.
- Run the Windows clean-VM TLS probe against the production HTTPS endpoint and
  record only certificate-chain result, protocol, hostname, trust-store class,
  and stable error code. A process that stays alive is not a successful TLS
  handshake.
- Check system clock, supported Windows version, enterprise proxy/inspection,
  root certificate policy, and architecture. Keep endpoint and certificate
  subjects out of ordinary logs when they identify internal infrastructure.

The macOS build cannot close this incident. The release gate remains open until
the clean Windows x64 installer and installed-app probe pass, including upgrade
and rollback. Never disable certificate verification or fall back to plaintext.

## Terminal, Git, And Sandbox

- **Terminal:** verify Session/terminal/generation bindings, PTY/ConPTY state,
  resize and exit status. If output cannot be reverified after reconnect, retain
  the prior output and mark it unverified; do not infer that the process exited.
- **Git:** use read-only project-scoped overview/log/diff queries. Branch, index,
  worktree, push, commit, and conflict actions remain unavailable without the
  reviewed permission/approval/checkpoint gates. Redact remotes and paths.
- **Sandbox:** a denied command, Runtime denial, Provider `declined` Tool, and
  genuine user Approval are distinct. Do not turn a policy explanation into a
  grant, and do not weaken sensitive-path, symlink, executable, network, or
  shell-wrapper checks to make a fixture pass.

## Renderer And UI

**Symptoms:** blank Monaco/xterm surface, stale pane, clipped Send/Stop, or a
renderer restart that loses confirmed history.

- Confirm the bundle is local/trusted, external navigation remains blocked, and
  the WebChannel request ID matches the current renderer generation.
- A renderer restart may discard incomplete private staging but must preserve the
  last confirmed Session projection, queued prompt, and recovery intent.
- Run the focused targets and then the complete desktop suite:

```sh
cmake --build build -j4
ctest --test-dir build -R 'agent_workbench_render|agent_runtime_environment|monaco_editor_render' --output-on-failure
ctest --test-dir build --output-on-failure
```

## Escalation Record

Include only:

- App/sidecar/adapter versions and artifact hashes;
- platform, architecture, runner/VM image, and command exit codes;
- stable error classes/codes, bounded health counters, generation, and recovery
  state;
- whether confirmed Timeline/Store state was preserved and which recovery method
  was used;
- the smallest reproducing fixture or test name.

Exclude prompts, source code, diffs, terminal output, credentials, headers,
provider bodies, raw paths, user identifiers, PIDs, and full stderr. A release
owner must explicitly approve any new diagnostic field before it enters AAP,
SQLite, logs, or this runbook.
