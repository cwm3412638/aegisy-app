# Idempotency Design for Aegisy Agent Workbench

## Overview

This document defines the idempotency requirements and implementation strategy for mutation operations in the Aegisy Agent Workbench. Task 3.6 requires idempotency semantics for turns, approvals, file writes, Git mutations, and job submission to ensure reliable operation across network failures, process restarts, and retry scenarios.

## Core Requirements

### Idempotency Guarantees

1. **Exact Retry**: Identical requests with the same idempotency key return the original operation result without re-execution
2. **Conflict Detection**: Same idempotency key with different request parameters fails explicitly
3. **Durable Reservation**: Operations are reserved before dispatch to prevent duplicate execution
4. **Atomic State Transitions**: State changes use revision-based CAS to prevent concurrent modification
5. **Reconciliation**: Uncertain states (e.g., after crash) require explicit reconciliation before retry

### Mutation Lifecycle States

All mutation operations follow a common state machine:

- **Requested/Accepted**: Operation reserved, not yet dispatched
- **Committed**: Operation completed successfully with evidence
- **Failed**: Operation failed with evidence
- **ReconciliationRequired**: Uncertain state requiring external reconciliation

Terminal states (Committed, Failed) cannot be advanced by the producer. ReconciliationRequired is producer-sticky and requires external resolution.

## Current Implementation Status

### 1. Turn Start (Complete)

**Schema**: `mutation-acknowledgement/0.1` (schema v20)  
**Status**: ✅ Fully implemented and integrated

**Implementation**:
- Durable SQLite ledger with session-scoped operations
- Idempotency key: session + turn + request fingerprint
- Revision-based CAS for state transitions
- Timeline anchor binding (accepted + terminal anchors)
- Startup verification and reconciliation
- AAP capability: `session.mutation-acknowledgements`
- Qt consumption with exact anchor validation

**Key Files**:
- Schema: `agent-runtime/crates/aegisy-agentd/src/workbench_store.rs` (v20 migration)
- Runtime: Turn dispatch and acknowledgement logic
- Qt: Consumption and validation

**Gaps**: None - this is the reference implementation

### 2. Approval Acknowledgement (Partial)

**Schema**: `approval-acknowledgement/0.1`  
**Status**: ⚠️ Metadata-only contract, not integrated

**Current Implementation** (`approval_ack.rs`):
- Operation identity: SHA-256(session, turn, idempotency_key, fingerprint, scope, scope_identity, requirement_identity)
- Scope types: CommandExecution, FileChange, Permissions
- States: Requested → Resolved/Failed/ReconciliationRequired
- Resolutions: Denied, Expired, NotRequired (no "allowed" - cannot grant approval)
- Validation: Rejects secret-shaped IDs, JWT patterns, credential markers
- Authority fields: Fixed false (no mutation/approval/execution/user-decision)

**What Exists**:
- Complete identity derivation and validation
- Lifecycle state machine with revision tracking
- Retry classification (exact replay vs conflict)
- Serde with unknown field rejection
- Comprehensive test coverage (11 tests)

**What's Missing**:
1. Schema v21+ migration to add `mutation_kind IN ('turn-start', 'approval')`
2. Store integration: reservation, binding, consumption
3. AAP routes: `approval/request`, `approval/acknowledge`, `approval/status`
4. Qt UI: approval prompt, user decision capture
5. Codex integration: server request correlation
6. Authority issuer: actual user decision → approval grant
7. Timeline event: `approval.requested`, `approval.resolved`

**Implementation Roadmap**:
1. Extend schema v20 table to support multiple mutation kinds
2. Add Store methods: `reserve_approval`, `bind_approval_outcome`, `consume_approval`
3. Implement AAP handlers with capability `session.approval-acknowledgements`
4. Build Qt approval dialog with scope-specific prompts
5. Connect to Codex server request lifecycle
6. Add approval authority issuer (separate from acknowledgement)

### 3. File Write Acknowledgement (Partial)

**Schema**: `file-write-acknowledgement/0.1`  
**Status**: ⚠️ Metadata-only contract, not integrated

**Current Implementation** (`file_write_ack.rs`):
- Operation identity: SHA-256(session, project, root, idempotency_key, fingerprint, edit_identity)
- Edit identity: `workspace-edit:sha256:...` (immutable change set)
- States: Accepted → Committed/Failed/ReconciliationRequired
- Bounds: 1-256 changed files, 1-4MiB total bytes
- Authority fields: Fixed false (no mutation/execution)

**What Exists**:
- Complete identity derivation and validation
- Lifecycle state machine with revision tracking
- Edit identity binding (immutable workspace change set)
- Retry classification
- Comprehensive test coverage (4 tests)

**What's Missing**:
1. Schema migration to support `mutation_kind = 'file-write'`
2. Store integration: reservation before filesystem writes
3. AAP routes: `workspace/write/reserve`, `workspace/write/commit`
4. Filesystem execution: atomic write with observation evidence
5. Approval integration: file-change approval → write authority
6. Observation identity: hash of actual filesystem state post-write
7. Timeline event: `workspace.write-accepted`, `workspace.write-committed`
8. Reconciliation: compare requested edit vs actual filesystem state

**Implementation Roadmap**:
1. Extend mutation ledger for file-write kind
2. Add Store methods: `reserve_file_write`, `commit_file_write`, `reconcile_file_write`
3. Implement AAP handlers with capability `workspace.write-acknowledgements`
4. Modify workspace write path: reserve → approve → execute → commit
5. Add filesystem observation: hash changed files, compare to edit identity
6. Build reconciliation: detect partial writes, missing files, hash mismatches
7. Connect to approval system for file-change scope

### 4. Git Mutation Acknowledgement (Partial)

**Schema**: `git-mutation-acknowledgement/0.1`  
**Status**: ⚠️ Metadata-only contract, not integrated

**Current Implementation** (`git_mutation_ack.rs`):
- Operation identity: SHA-256(session, project, root, kind, idempotency_key, fingerprint, plan_identity)
- Kinds: BranchCreate, BranchSwitch, BranchRename, Stage, Commit, WorktreeCreate, WorktreeRemove
- Plan identity: `git-plan:sha256:...` (immutable Git operation plan)
- States: Accepted → Committed/Failed/ReconciliationRequired
- Authority fields: Fixed false (no mutation/approval/execution)

**What Exists**:
- Complete identity derivation and validation
- Lifecycle state machine with revision tracking
- Plan identity binding (immutable Git operation)
- Retry disposition: Replay/Conflict/Unrelated
- Comprehensive test coverage (3 tests)

**What's Missing**:
1. Schema migration to support `mutation_kind = 'git-mutation'`
2. Store integration: reservation before Git execution
3. AAP routes: `git/mutate/reserve`, `git/mutate/commit`
4. Git execution: atomic operation with observation evidence
5. Approval integration: Git approval → mutation authority
6. Observation identity: hash of Git state post-mutation (refs, HEAD, worktree)
7. Timeline event: `git.mutation-accepted`, `git.mutation-committed`
8. Reconciliation: compare planned Git state vs actual repository state

**Implementation Roadmap**:
1. Extend mutation ledger for git-mutation kind with subkind column
2. Add Store methods: `reserve_git_mutation`, `commit_git_mutation`, `reconcile_git_mutation`
3. Implement AAP handlers with capability `git.mutation-acknowledgements`
4. Modify Git workflow: plan → reserve → approve → execute → commit
5. Add Git observation: capture refs, HEAD, worktree state, compare to plan
6. Build reconciliation: detect partial operations, ref mismatches, worktree drift
7. Connect to approval system for Git-specific scopes

### 5. Job Submission (Partial)

**Schema**: `background-job-request/0.1` (via `mutation-reservation-draft/0.1`)  
**Status**: ⚠️ Draft reservation only, not integrated

**Current Implementation** (`mutation_reservation.rs`):
- Operation identity: `background-job:sha256:...` (same as fingerprint)
- Execution plan identity: `unified-execution-plan:sha256:...`
- Idempotency identity: `idempotency:sha256:...`
- Schedule: Manual, Scheduled, Recurring
- Retry policy: max attempts, backoff, safe retry boundary

**What Exists**:
- Background job request contract (`background_job.rs`)
- Mutation reservation draft conversion
- Retry disposition classification
- Job identity derivation

**What's Missing**:
1. Schema migration to support `mutation_kind = 'job-submission'`
2. Store integration: reservation before job dispatch
3. AAP routes: `job/submit/reserve`, `job/submit/commit`
4. Job scheduler: dispatch with idempotency
5. Job execution: atomic state transitions with evidence
6. Observation identity: hash of job execution result
7. Timeline event: `job.submitted`, `job.completed`
8. Reconciliation: detect orphaned jobs, duplicate dispatch

**Implementation Roadmap**:
1. Extend mutation ledger for job-submission kind
2. Add Store methods: `reserve_job_submission`, `commit_job_submission`, `reconcile_job_submission`
3. Implement AAP handlers with capability `job.submission-acknowledgements`
4. Modify job dispatch: reserve → dispatch → observe → commit
5. Add job observation: capture execution result, compare to plan
6. Build reconciliation: detect running jobs, failed dispatch, orphaned state
7. Connect to background job scheduler

## Shared Foundation

### Mutation Reservation Draft (`mutation_reservation.rs`)

**Purpose**: Normalize future ledger key and fingerprint shapes across all mutation kinds

**Status**: ✅ Complete metadata-only foundation

**Features**:
- Unified reservation identity: SHA-256(kind, schema, subkind, operation, binding, session, project, root, turn, idempotency, fingerprint)
- Retry disposition: Replay (exact match), Conflict (same key, different params), Unrelated (different key)
- Explicit incompatibility flags: `v20_store_compatible: false`, `turn_timeline_anchor_compatible: false`
- Authority denial: All authority fields fixed false
- Conversion from existing contracts: approval, file-write, git-mutation, job-submission

**Key Insight**: This prevents accidental insertion into the current `mutation_kind = 'turn-start'` table by explicitly declaring incompatibility.

### Common Patterns

All mutation acknowledgements share:

1. **Identity Derivation**: SHA-256 hash of immutable request parameters
2. **Fingerprint Normalization**: Lowercase SHA-256 without prefix
3. **Revision-Based CAS**: Contiguous revisions prevent concurrent modification
4. **Observation Evidence**: Terminal states require opaque hash of actual outcome
5. **Secret Rejection**: Identifiers reject JWT patterns, API keys, credential markers
6. **Authority Denial**: Acknowledgement ≠ authorization (separate concern)
7. **Unknown Field Rejection**: Strict schema prevents accidental persistence

## Schema Evolution Strategy

### Current State (v20)

```sql
CREATE TABLE mutation_acknowledgements (
    session_id TEXT NOT NULL,
    mutation_kind TEXT NOT NULL CHECK(mutation_kind = 'turn-start'),
    idempotency_key TEXT NOT NULL,
    request_fingerprint TEXT NOT NULL,
    operation_identity TEXT NOT NULL,
    revision INTEGER NOT NULL,
    state TEXT NOT NULL,
    accepted_anchor TEXT,
    terminal_anchor TEXT,
    observed_at_ms INTEGER NOT NULL,
    PRIMARY KEY (session_id, mutation_kind, idempotency_key)
);
```

### Proposed v21+ Migration

```sql
-- Relax mutation_kind constraint
ALTER TABLE mutation_acknowledgements 
    DROP CONSTRAINT mutation_kind_check;

ALTER TABLE mutation_acknowledgements 
    ADD CONSTRAINT mutation_kind_check 
    CHECK(mutation_kind IN ('turn-start', 'approval', 'file-write', 'git-mutation', 'job-submission'));

-- Add subkind for Git and approval variants
ALTER TABLE mutation_acknowledgements 
    ADD COLUMN mutation_subkind TEXT;

-- Add per-kind anchor columns (nullable, kind-specific)
ALTER TABLE mutation_acknowledgements 
    ADD COLUMN project_id TEXT,
    ADD COLUMN root_id TEXT,
    ADD COLUMN turn_id TEXT,
    ADD COLUMN scope_identity TEXT,
    ADD COLUMN requirement_identity TEXT,
    ADD COLUMN edit_identity TEXT,
    ADD COLUMN plan_identity TEXT,
    ADD COLUMN execution_plan_identity TEXT;

-- Add observation identity (terminal evidence)
ALTER TABLE mutation_acknowledgements 
    ADD COLUMN observation_identity TEXT;

-- Indexes for per-kind queries
CREATE INDEX idx_mutation_ack_project ON mutation_acknowledgements(project_id, mutation_kind);
CREATE INDEX idx_mutation_ack_turn ON mutation_acknowledgements(turn_id, mutation_kind);
```

**Migration Strategy**:
1. Create v21 schema with relaxed constraints
2. Migrate existing `turn-start` rows (no data change)
3. Add new columns with NULL defaults
4. Validate existing rows still pass constraints
5. Enable new mutation kinds incrementally

## AAP Integration

### Capability Negotiation

```json
{
  "session.mutation-acknowledgements": true,
  "session.approval-acknowledgements": false,
  "workspace.write-acknowledgements": false,
  "git.mutation-acknowledgements": false,
  "job.submission-acknowledgements": false
}
```

### Request/Response Patterns

**Reserve**:
```json
{
  "method": "approval/reserve",
  "params": {
    "session_id": "session-1",
    "turn_id": "turn-1",
    "idempotency_key": "approval-retry-1",
    "request_fingerprint": "request:sha256:...",
    "scope": "file-change",
    "requirement": { /* approval details */ }
  }
}
```

**Response**:
```json
{
  "result": {
    "operation_identity": "approval-operation:sha256:...",
    "revision": 1,
    "state": "requested",
    "disposition": "reserved" | "replay" | "conflict"
  }
}
```

**Commit**:
```json
{
  "method": "approval/commit",
  "params": {
    "operation_identity": "approval-operation:sha256:...",
    "revision": 2,
    "resolution": "denied" | "expired" | "not-required",
    "observation_identity": "approval-observation:sha256:..."
  }
}
```

## Qt Integration

### Consumption Flow

1. **Reserve**: Qt calls AAP reserve before showing approval prompt
2. **User Decision**: Qt captures user input (approve/deny/cancel)
3. **Commit**: Qt calls AAP commit with decision and observation
4. **Validation**: Qt validates response matches exact operation identity and revision
5. **Timeline Update**: Qt updates UI with committed state

### Reconnect Handling

- Pending operations survive process restart
- Qt re-queries operation status on reconnect
- Exact revision and anchor validation prevents stale state
- ReconciliationRequired state triggers explicit user review

## Reconciliation Strategy

### Triggers

- Process crash during mutation
- Network failure after dispatch
- Filesystem/Git state drift
- Startup with uncertain operations

### Reconciliation Flow

1. **Detect**: Startup scans for non-terminal operations
2. **Probe**: Read actual system state (filesystem, Git, process)
3. **Compare**: Match actual state against operation plan
4. **Classify**: Committed (success), Failed (error), Uncertain (unknown)
5. **Record**: Update operation with reconciliation evidence
6. **Gate**: Block new operations until reconciliation complete

### Per-Kind Reconciliation

**File Write**:
- Hash changed files, compare to edit identity
- Detect partial writes, missing files, permission errors
- Classify: Committed (all files match), Failed (none match), Uncertain (partial)

**Git Mutation**:
- Read refs, HEAD, worktree state
- Compare to plan identity
- Detect partial operations, ref mismatches, worktree drift
- Classify: Committed (plan matches), Failed (no change), Uncertain (partial)

**Job Submission**:
- Query job scheduler for job state
- Check process table for running jobs
- Detect orphaned jobs, duplicate dispatch
- Classify: Committed (job running), Failed (not found), Uncertain (ambiguous)

## Implementation Phases

### Phase 1: Schema Migration (v21)
- Extend mutation_acknowledgements table
- Add per-kind columns
- Migrate existing turn-start rows
- Validate schema constraints

### Phase 2: Approval Integration
- Implement Store methods
- Add AAP routes
- Build Qt approval dialog
- Connect to Codex server requests
- Add Timeline events

### Phase 3: File Write Integration
- Implement Store methods
- Add AAP routes
- Modify workspace write path
- Add filesystem observation
- Build reconciliation logic

### Phase 4: Git Mutation Integration
- Implement Store methods
- Add AAP routes
- Modify Git workflow
- Add Git observation
- Build reconciliation logic

### Phase 5: Job Submission Integration
- Implement Store methods
- Add AAP routes
- Modify job dispatch
- Add job observation
- Build reconciliation logic

### Phase 6: Cross-Platform Validation
- Windows reconnect evidence
- macOS/Windows reconciliation tests
- Stress testing: crashes, network failures
- Performance validation: latency, throughput

## Security Considerations

### Authority Separation

- **Acknowledgement ≠ Authorization**: Recording an operation does not grant permission
- **Approval Authority**: Separate issuer validates user decision and grants approval
- **Execution Authority**: Separate gate validates approval and grants execution
- **Mutation Authority**: Separate gate validates execution and performs mutation

### Secret Protection

- All identifiers reject JWT patterns, API keys, credential markers
- Request fingerprints are content-hashed (no raw secrets)
- Observation identities are opaque hashes (no sensitive data)
- Unknown fields rejected to prevent accidental persistence

### Audit Trail

- Every state transition recorded with revision and timestamp
- Observation evidence binds terminal states
- Reconciliation events preserve uncertainty
- Timeline events provide user-visible audit

## Testing Strategy

### Unit Tests (Existing)

- Identity derivation and validation
- Lifecycle state machine
- Retry classification
- Secret rejection
- Authority denial

### Integration Tests (Required)

- Store reservation and consumption
- AAP request/response cycles
- Qt UI flows
- Codex integration
- Timeline event emission

### Reconciliation Tests (Required)

- Process crash during mutation
- Filesystem/Git state drift
- Partial operations
- Startup recovery
- Cross-platform behavior

### Stress Tests (Required)

- Concurrent operations
- Network failures
- Process restarts
- Large-scale retries
- Performance under load

## Open Questions

1. **Approval Scope Granularity**: Should file-change approval be per-file or per-edit?
2. **Git Observation Depth**: Should observation include full worktree hash or just refs?
3. **Job Retry Semantics**: Should failed jobs auto-retry or require explicit re-submission?
4. **Reconciliation UI**: Should uncertain operations block all work or just affected sessions?
5. **Performance Impact**: What is the latency overhead of durable reservation?

## References

- Task 3.6: `openspec/changes/build-aegisy-agent-workbench/tasks.md`
- Turn-start implementation: `agent-runtime/crates/aegisy-agentd/src/workbench_store.rs` (v20)
- Approval contract: `agent-runtime/crates/aegisy-agentd/src/approval_ack.rs`
- File write contract: `agent-runtime/crates/aegisy-agentd/src/file_write_ack.rs`
- Git mutation contract: `agent-runtime/crates/aegisy-agentd/src/git_mutation_ack.rs`
- Reservation draft: `agent-runtime/crates/aegisy-agentd/src/mutation_reservation.rs`
