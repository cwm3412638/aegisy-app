# Aegisy Privacy And Diagnostic Export 0.1

Status: internal product and support contract. This document describes the
privacy boundary and the preview-first diagnostic export that must be reviewed
before a public support workflow is enabled. It does not grant telemetry,
provider, filesystem, terminal, approval, or Agent execution authority.

## Principles

- A diagnostic export is user initiated, local-first, and previewable. Aegisy
  never uploads a bundle automatically.
- The default bundle is metadata-only. Prompts, conversation text, source code,
  diffs, paths, terminal output, file contents, extension arguments, and
  credentials are excluded unless the user explicitly opts into a named
  category.
- Redaction runs before counting, hashing, previewing, serializing, or writing
  an export. A failed redaction or uncertain classification fails closed; it is
  never silently replaced with an apparently safe value.
- An export is a copy. It is not a database backup, a provider continuation,
  a checkpoint, an approval record, a Git operation, or a recovery mutation.
- Local history and user repositories remain authoritative. A server feature
  disable or a failed export must not delete local history or block its local
  export.

## Data Categories

The preview presents the exact category names below. A category is either
`included`, `excluded`, `removed-by-user`, or `unavailable`; no generic
"diagnostics" bucket may hide its contents.

### Local categories

| Category ID | Default | Includes | Never includes |
| --- | --- | --- | --- |
| `local.runtime-metadata` | included | Aegisy, sidecar, adapter, schema, and bundled asset versions; artifact hashes; platform/architecture; process generation; negotiated capability names; read-only readiness/recovery state; bounded stable error classes and exit codes | credentials, environment values, prompts, provider bodies, raw stderr, PIDs, raw paths, user identifiers |
| `local.timeline-metadata` | included | Session/Turn/Item counts, sequence and Event-ID anchors, states, timestamps, correlation IDs, bounded event-kind labels, retention floor/head, snapshot/reconnect state | Item payloads, transcript text, code, diffs, terminal text, images, artifact bytes |
| `local.security-state` | included | redacted permission-policy state, sandbox/network posture, capability gates, approval-policy observations, mutation/apply flags, quarantine and recovery classes | secrets, user Approval decisions that are not actually recorded, authority inferred from Runtime denial or `approvalPolicy=never` |
| `local.gateway-metadata` | included | redacted request ID, route/tool class, upstream status, content/transfer encoding, byte count, duration, normal-end/abort/client-close class, bounded retry count | authorization headers, API keys, request/response bodies, prompts, provider content, raw URLs/query values |
| `local.project-metadata` | excluded | only when opted in: opaque project/session IDs, project mode, Git branch state, bounded file-count and change-count summaries | absolute paths, repository names, OIDs, code, patch hunks, commit messages, remotes |
| `local.context-metadata` | excluded | only when opted in: selected context kind/count, byte-size and conservative token estimates, source freshness and exclusion reasons | selected file/selection/image/diagnostic/terminal/Git/artifact bodies, prompt text, hidden reasoning |
| `local.transcript` | excluded | only when opted in per Session/Item: redacted conversation or model event summaries | unreviewed prompt, hidden reasoning, provider response IDs, continuation tokens, credentials |
| `local.code-and-diff` | excluded | only when opted in per Session/Item: redacted code or diff content | unredacted source, full paths, generated secrets, private keys, ignored files |
| `local.terminal` | excluded | only when opted in per terminal/generation/range: redacted bounded excerpt | commands with secrets, environment values, raw output beyond the selected range, PIDs |
| `local.diagnostic-content` | excluded | only when opted in per diagnostic: normalized diagnostic metadata and an explicitly selected redacted raw reference | raw diagnostic bodies, unfiltered paths, source snippets, compiler environment |
| `local.artifact-content` | excluded | only when opted in per artifact: selected redacted text artifact bytes | credential-shaped blobs, binary archives, database/WAL files, hidden provider state |
| `local.credentials` | never | none | secure-storage values, API keys, access/refresh tokens, cookies, passwords, private keys, credential-bearing environment variables |

The default bundle may report counts for excluded categories, but counts are
bounded and do not disclose content. A zero count means "not observed", not
"proven absent".

### Cloud categories

The desktop sidecar and diagnostic exporter do not send any category to the
cloud automatically. If a separately authenticated Aegisy service or provider
receives data while a user is using a model, that service's own policy and the
selected provider route apply; diagnostic export does not expand that route.

When a future, separately reviewed cloud support flow is offered, its preview
must use these exact categories and opt-in states:

| Category ID | Default | Allowed contents |
| --- | --- | --- |
| `cloud.account-operations` | excluded | opaque account/tenant support correlation ID and service incident class; no email, token, billing value, or raw user identifier |
| `cloud.service-health` | excluded | aggregate endpoint availability, protocol error class, region/SDK version, and bounded timing/count metrics; no session or repository identity |
| `cloud.model-operations` | excluded | aggregate model/adapter family and outcome class, with model identity only when already public in the active binding; no prompt, completion, tool argument, code, or provider body |
| `cloud.support-bundle` | excluded | a user-selected subset of the redacted local categories after a second preview; no category may be silently added |
| `cloud.content` | never by diagnostic export | none; transcript, code, diffs, paths, terminal output, and artifact bytes require a distinct reviewed product flow and explicit per-item consent |

Cloud telemetry is disabled when no user opt-in or managed policy exists.
Prompts, code, file paths, diffs, command output, extension arguments, and
provider bodies remain local under the current contract. A zero-data-retention
or provider-routing promise is displayed only when its authoritative source is
verified; unknown policy is shown as `unknown`, never as compliant.

## Redaction Contract

The redactor is streaming, bounded, and applied recursively to every string,
key, header, URL, and structured field before any export operation. It must
recognize at least:

- `Authorization` and `Proxy-Authorization` values, Bearer/basic/API-key
  assignments, common provider-token prefixes, JWT-shaped values, cookies, and
  signed query parameters;
- passwords, client secrets, webhook/signing keys, OAuth codes, private-key
  PEM blocks, certificate-key pairs, connection strings, and credential-shaped
  environment values;
- prompts, model responses, command arguments, terminal lines, source snippets,
  diff hunks, absolute paths, repository remotes, and user identifiers when a
  selected opt-in category makes them eligible for export.

Redacted values use a stable type sentinel such as `[REDACTED:api-key]`; the
original value is never retained for a later "unredact" operation. Paths use
`[REDACTED:path]` unless the user explicitly selects a root-relative path
category. User/account identifiers use a per-bundle salted opaque identity and
are not reversible. Hashes are reported only when the category contract says
they are useful for correlation; a hash is not proof that content is safe to
share.

The redaction report contains only category, detector class, and bounded count
fields. It never includes the matched value, a surrounding excerpt, or a
length that could reveal a secret. A detector false positive can be removed by
the user only by removing the entire affected item/category, not by disabling
the detector.

## Bundle Preview And Export

The exporter uses the versioned envelope `aegisy-diagnostic-bundle/0.1` and
writes a deterministic UTF-8 manifest plus bounded JSONL records. An
implementation may place these files in a ZIP container, but the manifest and
record bytes must remain deterministic for the same fixed input, category set,
redaction policy, and export nonce.

The preview is a separate operation from writing the bundle and reports:

1. bundle schema/version, export nonce, support correlation ID, and redaction
   policy version;
2. every category's state (`included`, `excluded`, `removed-by-user`, or
   `unavailable`), estimated bytes, bounded record count, and risk label;
3. affected Session/Turn/Item or terminal identities for every opt-in content
   category, without displaying bodies before the user selects that category;
4. excluded fields and reasons, detector counts, hash/reference counts,
   retention warnings, and whether a cloud upload is disabled;
5. the exact final category set and a preview hash used to prevent a stale
   preview from being exported.

The user may remove any Session, Item, terminal range, diagnostic, artifact, or
category from the preview. Export repeats all source reads, policy checks,
redaction, bounds, and the preview hash inside one read-only operation. A stale
preview, source loss, changed hash, redaction uncertainty, or size limit fails
without writing a partial bundle. The destination is chosen by the user; a
temporary file is atomically renamed only after final hash verification, and a
failed attempt removes the temporary file.

The bundle contains no database, SQLite WAL, secure-storage file, credential
store, provider continuation, raw protocol frame, or automatic upload token.
The support correlation ID is generated locally and is safe to quote in a
ticket only together with the user's chosen bundle. It does not grant access
to a Session or repository.

## Retention And User Controls

- Local Workbench history remains under the platform Aegisy data root. Session,
  Timeline, Proposal, Blob, terminal, and diagnostic retention are independent
  controls; deleting a diagnostic bundle does not delete its source history.
- The default diagnostic bundle is ephemeral until the user chooses a
  destination. Aegisy deletes failed temporary files and does not retain a
  second server copy. User-chosen files follow the destination's normal OS
  retention and backup policy.
- Users can preview/export local history, remove selected categories/items,
  delete local diagnostic files, and disable any future cloud support upload.
  A server feature disable cannot prevent local history export.
- Timeline pruning, Session purge, Blob release, and portable-session export
  follow their own reviewed retention contracts. A diagnostic exporter must
  never bypass a retention floor, resurrect a purged item, or use a stale
  disconnected cache as authority.
- Managed policy may require a minimum metadata category for incident support,
  but it must be visible in the preview. Managed policy cannot enable
  credentials, raw provider bodies, hidden reasoning, or content export by
  default.

## Current Security Boundary

The current Aegisy Coding adapter and Agent Runtime are read-only:

```text
file_mutation_authority = false
approval_recorded       = false
apply_available         = false
```

Runtime denial, `Provider declined`, and `approvalPolicy=never` are distinct
observations and must not be presented as user approval. Diagnostic preview and
export add no file-write, command, Git mutation, provider delete/compact,
checkpoint/apply, rollback, background-job, or multi-agent authority. A user
editor save remains separately bounded to the canonical opened project root;
export itself never performs that save.

## Support Review Checklist

Before a support owner accepts a bundle, verify:

- the manifest category set and preview hash match the file;
- no `local.credentials` or `cloud.content` record exists;
- redaction counts are present without matched values;
- all opted-in content names the exact Session/Item/terminal/artifact scope;
- the bundle contains no absolute path, raw header, token, prompt, code, diff,
  terminal output, provider body, or hidden reasoning outside the reviewed
  opt-in scope;
- the current read-only authority flags remain false; and
- the user knows the support correlation ID is not an access grant.

Any uncertainty blocks support upload and requires a new preview after removing
the affected category. This document must be updated together with the
redaction policy, exporter schema, or retention contract; prose alone cannot
claim that an unimplemented AAP/Qt export surface exists.
