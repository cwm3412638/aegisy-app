# ADR 0007: Local Content Retention

- Status: Provisional
- Accountable owner: Data Governance
- Consulted owners: Product Security, Product, Support, Enterprise Administration
- Due gates: An accepted dedicated enterprise-retention OpenSpec before beta
  mutation; OpenSpec `5.8` and publication under `22.9` remain prerequisites

## Context

Aegisy stores different content classes with different recovery value and privacy
risk. Current foundations include durable Session history, bounded runtime terminal
capture, command artifacts, Workspace Edit Proposals, images, retention policies,
two-phase deletion, and content-addressed Blob GC. Enterprise policy and final stable
defaults are not yet complete.

## Provisional Defaults

- Session history: local until explicit user/policy deletion; archived is retained.
- Session deletion: two phase with a reviewed scope and a default seven-day undo UI;
  the protocol permits 24 hours through 30 days. Purge retains released Blob content
  for at least another 24 hours before conservative GC.
- User terminal output: runtime memory only, newest 1 MiB per terminal; it is not
  durable Session history unless the user explicitly creates a bounded excerpt pin.
- Command artifacts: durable only when required by the command/artifact contract,
  scoped to the Session, with the current 30-day retention metadata.
- Diffs and Workspace Edit Proposals: durable with their owning Session because they
  are review/recovery evidence; deletion follows Session policy.
- Imported images and pinned content: durable only after explicit user import/pin,
  scope bound, and released when the last durable reference is removed; physical GC
  remains conservative and honors the undo floor.
- Diagnostic raw data: in-memory and bounded unless it references an existing
  Session command artifact; no raw provider/LSP wire bodies are persisted.

## Enterprise Controls Required Before Stable

Per-project/session retention, maximum local age/bytes, export/delete restrictions,
content-class inclusion, legal hold, diagnostic opt-in, and cloud-upload prohibition
must have explicit precedence and audit semantics. A policy can shorten or prevent
new retention only through reviewed recovery behavior; it cannot silently delete
uncertain, corrupt, active, or referenced content.

The dedicated follow-up OpenSpec must add machine-trackable implementation and
verification tasks for every control above. Completing only `5.8` or documenting
release criteria under `22.9` cannot close this ADR.

## Consequences

No automatic cloud upload is authorized. Prompts, source, diffs, paths, terminal
output, images, provider bodies, or hidden reasoning remain excluded from default
diagnostic export. Stable release documentation must distinguish logical deletion,
undo retention, physical GC, backup evidence, and externally exported files.
