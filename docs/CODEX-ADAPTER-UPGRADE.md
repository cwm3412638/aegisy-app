# Codex Adapter Upgrade and Rollback

This document is the release procedure for the installed Codex CLI/App Server
used by `aegisy-agentd`. It is intentionally conservative: an adapter version
is not accepted because it starts successfully. It must preserve the pinned
schema contract, the AAP projection, security defaults, and the failure
recovery behavior.

## Compatibility Matrix

| Component | Supported value | Policy |
| --- | --- | --- |
| Codex CLI/App Server | `0.144.5` | The only version accepted by the adapter today |
| Generated vendor schema | `v2` from `0.144.5` | Checked in under `agent-runtime/aap-schema/codex-app-server-0.144.5/` |
| Aegisy Agent Protocol | `0.1` | Stable host/sidecar handshake namespace |
| Permission profile | `read-only` | The adapter cannot approve writes or mutating commands |
| Startup supervision | 15-second initialize deadline, 3 attempts | Only transient transport/EOF/read/write/timeout failures retry |
| Recovery action | AAP `runtime/restart` | Only exited/unavailable adapters; active turns and user terminals block it |

An installed version outside this matrix is rejected before the app-server
process is launched. Do not loosen the version check to make an upgrade appear
compatible. Add a new matrix row only after the candidate evidence below passes.

## Upgrade Procedure

1. Create a release branch and record the current pinned version, schema hash,
   sidecar commit, adapter binary hash, and the last green macOS and Windows
   runner builds. Keep the current release artifacts available as the rollback
   slot.
2. Install the candidate Codex CLI in an isolated test environment. Record its
   exact version and SHA-256. Generate the vendor schema from that exact binary
   and review the diff against the checked-in schema; never hand-edit vendor
   field names from memory.
3. Update the adapter pin and schema directory together. Add or update redacted
   fixtures for initialize, health, degradation, partial output, approval and
   denial, cancellation, reconnect, compaction, provider failure, and shutdown.
   Scan every fixture and generated artifact for credential-shaped values,
   prompts, paths that should be private, and provider-opaque rollout content.
4. Run the local release gates:

   ```sh
   cargo fmt --all --manifest-path agent-runtime/Cargo.toml -- --check
   cargo test --workspace --manifest-path agent-runtime/Cargo.toml -- --test-threads=1
   cargo clippy --workspace --all-targets \
     --manifest-path agent-runtime/Cargo.toml -- -D warnings
   openspec validate build-aegisy-agent-workbench --strict
   ctest --test-dir build --output-on-failure
   ```

5. Run the pinned-binary contract suite on macOS and Windows. It must cover
   version rejection, initialize timeout/retry bounds, stderr summary
   redaction, partial stream, approval denial, cancellation, reconnect and
   `runtime/restart`, compaction/provider failure mapping, shutdown cleanup,
   and the read-only sandbox/environment contract.
6. Compare the AAP output, not just process exit status. Stable AAP fields must
   remain compatible; new vendor fields belong behind explicit adapter mapping
   and degradation metadata. Unknown or missing capabilities must remain
   unavailable rather than being simulated by Qt.
7. Package a canary build with the candidate sidecar, adapter, schema, notices,
   and hashes. Exercise startup, a Chat session, a Work session, session replay,
   renderer restart, terminal cleanup, and offline/unavailable recovery before
   promotion. Record the redacted diagnostics and artifact hashes as release
   evidence.

## Emergency Pin

The emergency pin is the last matrix row whose contract suite and signed release
artifacts are known good. For the current product that is Codex `0.144.5`.

- Stop promotion of the candidate and disable the candidate adapter asset in the
  updater manifest. Do not bypass the adapter's exact-version rejection.
- Restore the known-good sidecar, Codex adapter asset, generated schema bundle,
  and matching hashes as one release set. Keep session data untouched.
- If only the installed Codex binary changed, the runtime must report
  `unavailable` and `restart_required`; do not silently use a different version.
- Record the incident, candidate hash, known-good hash, and the failing fixture
  or runner evidence without storing prompts, credentials, or raw stderr.

## Rollback

Rollback is an artifact operation, not a database migration. Stop the sidecar,
restore the previous signed sidecar/adapter/schema set, verify hashes before
launch, and start it again. Existing durable sessions remain readable. A session
whose provider binding is incompatible with the restored adapter must fail
explicitly and offer a portable fork; it must not silently reinterpret provider
state.

After rollback, run the same health, session replay, turn cancellation, terminal
cleanup, and redaction smoke checks used for promotion. If the outcome is
ambiguous, leave the runtime unavailable/read-only and preserve the evidence for
manual recovery rather than retrying a mutating operation.

## Evidence Record

Every upgrade or rollback record contains only bounded metadata:

- source and target Codex versions, sidecar commit, schema and binary SHA-256;
- platform, runner image, test commands, and pass/fail results;
- compatibility/degradation changes and the selected release slot;
- redacted request ID or support correlation ID when an incident is involved.

Never include API keys, login tokens, authorization headers, prompts, source
content, full paths, raw provider items, or raw stderr in this record.
