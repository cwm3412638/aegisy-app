# Aegisy Portable Session Format 0.1

Status: preview, versioned, forward-incompatible by default.

The portable session package is a UTF-8 JSON object intended for explicit user
export and import. It carries a redacted history snapshot, not a database backup,
credential bundle, provider continuation, checkpoint, or artifact archive.

## Envelope

```json
{
  "schema_version": "aegisy-portable-session/0.1",
  "exported_at_ms": 0,
  "content": {
    "schema_version": "aegisy-portable-session-content/0.1",
    "source_session_id": "session-id",
    "source_had_project": true,
    "mode": "work",
    "title": "Session title",
    "source_created_at_ms": 0,
    "source_updated_at_ms": 0,
    "items": []
  },
  "content_hash": {
    "sha256": "lowercase-hex-sha256",
    "bytes": 0
  },
  "redacted_value_count": 0,
  "excluded_field_count": 0
}
```

`content_hash` is SHA-256 over the compact UTF-8 JSON serialization of `content`
produced by `serde_json`. Importers must validate it before inspecting or storing
items. The outer export timestamp and redaction counters are descriptive metadata;
the content hash does not bind them.

One package is limited to 4 MiB and 2,000 items. Item `source_sequence` starts at
1 and is contiguous. `source_item_id` values are unique inside the package.

## Item

Each item contains:

- `source_sequence`: original session-local sequence.
- `source_item_id`: source identity used only for collision reporting.
- `item_kind`, `role`, and `state`: bounded AAP timeline labels.
- `payload`: bounded redacted JSON.
- `source_created_at_ms`: source observation time.

Imported Item IDs are always newly derived from the package hash, destination
Session ID, source sequence, and source Item ID. Source IDs are never installed as
destination authority.

## Exclusions And Redaction

Export re-runs the streaming secret redactor over every string, even though normal
persistence already sanitizes Item payloads. Recognized assignments, authorization
values, common API-token prefixes, and JWT shapes are replaced before serialization.

The portable payload excludes credential fields and local/provider-opaque state,
including authorization values, access or refresh tokens, passwords, private keys,
cookies, encrypted or hidden reasoning, provider response IDs, cache handles,
continuation tokens, environment identity, artifact objects, and local content or
artifact references. Durable Blob bytes are never embedded.

The export preview reports content categories and warns when the package contains
conversation text, command output, code or diffs, or path metadata. Export requires
an explicit user confirmation after that preview.

## Import And Collisions

Import validates the envelope version, content version, content hash, byte/item
limits, sequence continuity, unique source Item IDs, payload bounds, and a second
redaction/exclusion pass. Unsupported or altered packages are rejected before an
SQLite write transaction starts.

The AAP import methods require one collision strategy:

- `reject`: block when the source Session ID or any source Item ID already exists.
- `copy`: create a new Session and remap every Item ID. When the source Session is
  readable in the same store and the destination project/mode matches, the new
  Session records fork lineage; otherwise it records new lineage.

Work packages require an explicit active target project. Project absolute roots and
source environment identity are not carried. The destination creates a fresh local
environment identity.

Session, Item, projection-source, and audit-event writes commit in one SQLite
`IMMEDIATE` transaction. A failed import leaves no partial destination state.

## AAP Methods

- `session/export/preview { session_id }`
- `session/export { session_id, package_hash }`
- `session/import/preview { package, target_project_id?, collision_strategy }`
- `session/import { package, target_project_id?, collision_strategy }`

Export repeats package construction and rejects a stale preview hash. Import repeats
all validation and project checks inside the operation. Imported portable history is
immediately readable, but provider response IDs and continuation state are absent by
design; continuing with another provider/runtime requires a later explicit runtime
fork/context operation.
