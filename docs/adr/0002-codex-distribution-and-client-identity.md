# ADR 0002: Codex Distribution and Client Identity

- Status: Proposed
- Accountable owner: Runtime Integrations
- Consulted owners: Legal, Release Engineering, Enterprise Security
- Due gates: OpenSpec `1.5` and `7.1`; must close before Codex is bundled by `22.4`

## Context

The current adapter requires an externally installed `codex-cli 0.144.5`, verifies
that exact version, and launches `codex app-server --stdio`. Developer installation
is not redistribution evidence. Enterprise deployments may also impose client
identity, policy, authentication, telemetry, or installation-ownership conditions.

## Proposed Decision

Keep Codex externally installed and user/administrator owned for internal and preview
channels. Do not copy a Codex executable into an Aegisy package, updater slot, or
artifact manifest until legal and release owners approve exact terms and provenance.
Continue to pin the adapter contract and fail closed on version drift.

## Required Decision Inputs

- Exact package/license source, redistribution terms, NOTICE obligations, and update
  ownership for every platform artifact.
- Whether enterprise identity or managed-policy headers/configuration are required,
  who may issue them, and which values are credentials.
- A signed compatibility matrix, rollback artifact, generated-schema diff, and real
  macOS/Windows contract fixture for the candidate version.
- A package manifest binding Aegisy host, sidecar, adapter bytes, schemas, and updater
  compatibility without exposing tokens or machine paths.

## Prohibited Shortcuts

An executable found on a developer PATH cannot become package evidence. A version
range cannot replace exact compatibility testing. Credential-bearing enterprise
identity and managed-policy attributes cannot be invented or placed in process
arguments, ordinary logs, QSettings, or AAP. This does not prohibit AAP's existing
bounded, non-secret client product/version identity used for compatibility.
If bundling is rejected, Aegisy must keep explicit installation/version guidance and
must not claim offline availability of Codex.
