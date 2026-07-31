# ADR 0007: Local Content Retention

- Status: Provisional
- Accountable owner: Data Governance
- Consulted owners: Product Security, Product, Support, Enterprise Administration
- Due gates: Dedicated enterprise-retention OpenSpec before beta mutation; `5.8` and `22.9` remain prerequisites

## Context

Aegisy Agent Workbench stores sessions, events, artifacts, and backups locally. Different content classes have different recovery value and privacy risk. The system must balance user expectations for crash recovery and history against storage costs, privacy concerns, and enterprise compliance requirements.

Current foundations include durable Session history, bounded runtime terminal capture, command artifacts, Workspace Edit Proposals, images, and content-addressed Blob storage with garbage collection. Enterprise policy and stable defaults require explicit decisions.

## Decision Options

### Option 1: Retain Forever
Store all session data, events, artifacts, and backups indefinitely until explicit user deletion.

**Advantages:**
- Complete history for debugging and recovery
- No data loss from automatic cleanup
- Simple mental model for users

**Disadvantages:**
- Unbounded storage growth
- Privacy risk from accumulated sensitive data
- Compliance challenges for enterprise deployments
- Performance degradation with large databases

### Option 2: Time-Based Expiry
Automatically delete content after fixed retention periods (e.g., 30/90/365 days).

**Advantages:**
- Predictable storage bounds
- Automatic compliance with data retention policies
- Reduced privacy exposure over time

**Disadvantages:**
- Risk of deleting valuable history
- Arbitrary time limits may not match user needs
- Complexity in handling active vs. archived sessions

### Option 3: Size-Based Limits
Enforce storage quotas and delete oldest content when limits are reached.

**Advantages:**
- Guaranteed storage ceiling
- Adapts to actual usage patterns
- Protects against runaway growth

**Disadvantages:**
- Unpredictable retention duration
- May delete recent valuable data during heavy usage
- Difficult to explain to users

### Option 4: User-Controlled with Safe Defaults (Selected)
Provide explicit user/project controls with conservative defaults and two-phase deletion.

**Advantages:**
- User agency over their data
- Flexible for different use cases
- Enterprise policy can override defaults
- Two-phase deletion provides undo safety

**Disadvantages:**
- Requires user understanding and action
- More complex implementation
- Risk of users ignoring storage growth

## Provisional Defaults

**Session history:** Retained locally until explicit user or policy deletion. Archived sessions remain durable.

**Session deletion:** Two-phase with reviewed scope and default seven-day undo window. Protocol permits 24 hours through 30 days. Purge retains released Blob content for at least 24 hours before conservative garbage collection.

**Terminal output:** Runtime memory only, newest 1 MiB per terminal. Not durable Session history unless user explicitly creates bounded excerpt pin.

**Command artifacts:** Durable only when required by command/artifact contract, scoped to Session, with 30-day retention metadata.

**Diffs and Workspace Edit Proposals:** Durable with owning Session as review/recovery evidence. Deletion follows Session policy. Schema v19 adds immutable Proposal/Timeline bindings that survive Journal pruning.

**Images and pinned content:** Durable only after explicit user import/pin, scope-bound, released when last durable reference removed. Physical GC honors undo floor.

**Diagnostic data:** In-memory and bounded unless referencing existing Session command artifact. No raw provider/LSP wire bodies persisted.

**Timeline events:** Schema v16 uses cursor floor plus one checkpoint projection per Session. Checkpoint replacement, floor advancement, and prefix deletion are atomic. Automatic pruning disabled until reviewed stage enables it.

## Privacy and Storage Considerations

**Privacy:**
- No automatic cloud upload authorized
- Prompts, source, diffs, paths, terminal output, images, provider bodies, and hidden reasoning excluded from default diagnostic export
- Credentials stored in OS secure storage, not database
- Database contains credential IDs or short-lived token references only
- Redaction of known secrets from UI events, logs, and diagnostics

**Storage:**
- SQLite in WAL mode for metadata and indexes
- Content-addressed Blob store for large artifacts
- Compression for terminal output, patches, images
- Bounded inline payload sizes (4 MiB frame ceiling for AAP 0.1)
- Conservative garbage collection respects undo floor and active references

## Enterprise Controls Required Before Stable

Per-project/session retention, maximum local age/bytes, export/delete restrictions, content-class inclusion, legal hold, diagnostic opt-in, and cloud-upload prohibition must have explicit precedence and audit semantics.

A policy can shorten or prevent new retention only through reviewed recovery behavior. It cannot silently delete uncertain, corrupt, active, or referenced content.

The dedicated follow-up OpenSpec must add machine-trackable implementation and verification tasks for every control. Completing only `5.8` or documenting release criteria under `22.9` cannot close this ADR.

## Consequences

**Accepted:**
- User control over session lifecycle with safe defaults
- Two-phase deletion with undo window
- Content-addressed storage with conservative GC
- No automatic cloud upload
- Privacy-first diagnostic export

**Deferred:**
- Automatic time-based or size-based cleanup
- Enterprise policy precedence and audit
- Legal hold mechanisms
- Cloud backup integration

**Documentation Requirements:**
Stable release documentation must distinguish logical deletion, undo retention, physical GC, backup evidence, and externally exported files.

**Implementation Gates:**
- Schema migrations must be transactional, versioned, backed up before upgrade
- Failed migration starts workbench read-only with diagnostic export
- Automatic Timeline pruning remains disabled until reviewed
- Enterprise retention OpenSpec required before beta mutation
