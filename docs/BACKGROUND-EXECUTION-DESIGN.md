# Background Execution Design

## Overview

This document defines requirements, patterns, safety gates, and implementation phases for background job execution and multi-agent coordination in Aegisy Agent Workbench. Background and multi-agent features remain disabled until prerequisite quality, security, and recovery gates pass.

## Requirements

### Functional Requirements

**Background Jobs**
- Durable job queue with schedule metadata, pause/resume, cancellation, and idempotent retry
- Manual and one-shot scheduling with bounded attempts and backoff
- Approval wait states that block execution until explicit user decision
- Recovery from crash, restart, and interrupted operations
- Platform notifications for completion, failure, approval-needed, and budget exhaustion

**Multi-Agent Coordination**
- Parent/child session lineage with structured task delegation
- Dedicated worktree isolation for concurrent write-capable children
- Bounded result handoff between parent and child sessions
- Child task cancellation and status visibility
- Reviewed integration of child work into parent branch

**Unified Execution**
- Single execution pipeline for interactive, child, and background work
- Consistent permission, approval, workspace, budget, and sandbox enforcement
- Mode-specific lifecycle gates without duplicating Agent logic

### Non-Functional Requirements

**Safety**
- No unattended writes until permission, sandbox, and recovery gates pass
- Explicit approval for medium and high-risk operations
- Budget enforcement: token, cost, time, turn, tool-call, concurrency, network
- Orphan process prevention and cleanup
- Cross-session isolation preventing shared worktree conflicts

**Observability**
- Structured traces for runtime, model, context, tool, approval, usage, change, test, error
- Content-free audit events for privileged operations
- Recovery decision journal with idempotent replay
- Lease ownership and process observation without exposing PIDs or paths

**Durability**
- WAL-mode SQLite with synchronous=FULL and bounded busy handling
- Transactional state updates with generation compare-and-swap
- Versioned migration with pre-upgrade backup
- Read-only recovery mode on corruption or schema mismatch

## Multi-Agent Coordination Patterns

### Parent-Child Task Delegation

**Task Contract**
```
child-task/0.1:
  parent: { session_id, turn_id }
  goal: bounded UTF-8 description
  context: content-addressed identities
  isolation:
    project_root: canonical path
    mode: read_only | dedicated_worktree
  tools: allowed tool set
  model_profile: role bindings
  permissions: { filesystem, command, network, extensions }
  budgets:
    token: { soft, hard }
    cost: { soft, hard }
    time_seconds: max
    turns: max
    tool_calls: max
    concurrency: max
  handoff: expected result shape
```

**Lineage and Status**
- Parent/child session identity with generation tracking
- Status: queued, running, waiting-approval, completed, failed, cancelled
- Cancellation request/acknowledgement with completion race handling
- Bounded result handoff with content-addressed references
- Navigation from parent to child and child to parent

**Worktree Isolation**
- Every concurrent write-capable child receives dedicated Git worktree
- Shared read-only children may use snapshot
- Admission gate validates: healthy worktree, clean state, unique branch, exact base revision
- Cleanup requires: terminal child, clean worktree, saved editor state, explicit integration/discard

### Budget Enforcement

**Runtime Ledger**
```
child-runtime-budget/0.1:
  limits: { token, cost, time, turns, tool_calls, concurrency, network }
  used: authoritative settled values
  reserved: active uncommitted allocations
  remaining: computed available budget
  sources: usage observation counts
  dimensions: { warning, saturated, exhausted }
```

**Enforcement Rules**
- Reserve token/cost before model admission
- Settle authoritative usage; charge unknown as full reservation
- Enforce wall-time, turn, tool-call, active-concurrency limits transactionally
- Network requests bound by permission policy
- Overcommit rollback on budget exhaustion
- Concurrency recovery on child completion

### Handoff and Integration

**Child Handoff**
```
child-handoff/0.1:
  parent_session: session_id
  child_session: session_id
  result: { summary, artifacts, diffs }
  evidence: content-addressed references
  metadata: { completion_time, final_status }
```

**Integration Review**
- Diff preview showing child changes
- Test results and diagnostic evidence
- Conflict detection with current parent branch
- Explicit keep/discard/selective-merge decision
- Cleanup of child worktree after integration

## Safety Gates and Quality Requirements

### Release Gates (Task 21.1)

Background and multi-agent features remain `not-advertised` with `stable_enabled=false` until:

**Permission and Sandbox** (Tasks 18.1-18.12)
- Permission profiles defined and enforced
- Filesystem root, denied path, symlink, command, network policy
- Granular approval rules for once, turn, session, durable scopes
- Risk classification for commands, files, Git, network, MCP
- Secure path defaults for credentials, SSH, cloud, browser, .env
- Secret detection and redaction at all boundaries
- Untrusted-data provenance preventing authority escalation
- Platform-specific sandbox enforcement (macOS/Windows)
- Adversarial tests for path escape, symlink race, command wrapping, redirect leak, forged approval, prompt injection, sandbox bypass

**Recovery and Durability** (Tasks 5.1-5.10, 6.9)
- Durable event persistence with monotonic sequence
- Content-addressed storage with checksums and retention
- Rebuildable projections and consistency verification
- WAL configuration, backup, low-disk behavior
- Versioned migrations with pre-upgrade backup and read-only recovery
- Interrupted operation reconciliation against filesystem, process, Git, event state
- Credentials never enter database or event payloads

**Git and Worktree** (Tasks 16.3-16.4)
- Dedicated worktree creation, locking, session association, health, cleanup
- Branch create/switch/rename with dirty-state preflight
- Protected-branch policy enforcement
- Worktree admission gate preventing shared/missing/reused/dirty/conflicted isolation

**Context and Compaction** (Tasks 17.1-17.10)
- Context manifest with source, priority, trust, hash, token size, freshness
- Instruction discovery and precedence
- Pinned context for files, selections, images, diagnostics, terminal, Git, artifacts
- Token-budget allocation across layers
- Model-specific tokenizer adapters
- Context inspector showing inclusion/exclusion reasons

**Observability** (Tasks 20.1-20.11)
- Structured turn traces with runtime/model/context/tool/approval/usage/change/test/error
- Observed, catalog-derived, estimated, stale, unknown labels
- Privacy-preserving audit events
- Diagnostic bundle with redaction report
- Deterministic adapter replay harness
- Repository task corpus with provenance and reset scripts
- Completion, regression, test pass, correction, approval burden, latency, token, cost metrics
- Crash, reconnect, migration, offline, low-disk, runtime-upgrade, rollback endurance

### Quality Thresholds

**Before Background Jobs**
- Single-agent interactive work reliable on macOS and Windows
- Crash recovery proven with deterministic injection tests
- Sandbox enforcement verified with adversarial suites
- Budget enforcement prevents cost runaway
- Notification delivery tested on target platforms
- Orphan process cleanup verified

**Before Multi-Agent**
- Background jobs stable and observable
- Dedicated worktree isolation proven
- Parent/child lineage navigation functional
- Budget enforcement prevents cross-session leakage
- Handoff and integration workflow complete
- Conflict detection and resolution tested

**Before Unattended Writes**
- Multi-agent coordination stable
- Permission and approval intersection complete
- Recovery from all failure modes proven
- Audit trail complete and tamper-evident
- Emergency disable mechanism functional
- Separate OpenSpec and release gate approved (Task 21.12)

## Implementation Phases

### Phase 1: Unified Execution Foundation (Tasks 21.2-21.3, 21.7)

**Structured Plans**
- Step IDs, dependencies, owners, evidence, stale-state revalidation
- Content-free SHA-256 evidence binding
- Completion evidence validation
- Dependency cycle detection

**Child Task Contract**
- Goal, context, worktree, tools, model profile, permissions, budgets, handoff
- Content-addressed context identities
- Validation without authority grant

**Unified Execution Plan**
- 14-stage order: identity, reconciliation, permission, approval, workspace, budget, sandbox, recovery, durable job, notification, release, dispatch, observation, handoff
- Mode-specific gate derivation
- Interactive/child/background envelope consistency

### Phase 2: Parent-Child Coordination (Tasks 21.4-21.6)

**Lineage and State**
- Parent/child session identity and generation
- Status tracking: queued, running, waiting-approval, terminal
- Cancellation request/acknowledgement
- Completion race handling
- Bounded handoff references

**Worktree Admission**
- Dedicated worktree validation
- Health check: missing, symlinked, prunable, unlocked, dirty, conflict, operation state
- Exact parent/child owner binding
- Delegated base revision

**Budget Enforcement**
- Token/cost reservation before model admission
- Authoritative/estimated usage settlement
- Wall-time, turn, tool-call, concurrency, network limits
- Warning/saturated/exhausted dimensions
- Overcommit rollback and concurrency recovery

### Phase 3: Background Job Scheduling (Tasks 21.8-21.9)

**Job Queue**
- Durable job request and state persistence
- Manual/one-shot schedule with bounded attempts/backoff
- Pause request/acknowledgement
- Approval wait states
- Cancellation request/acknowledgement and completion races
- Terminal result/evidence
- Exact retry eligibility
- Restart recovery: interrupted for active, preserved for queued/paused/waiting

**Scheduler Lease**
- Lease acquisition, renewal, state rebind, verified process bind, release, expiry
- Generation compare-and-swap
- Typed lease events with rollback on failure
- Startup revalidation of bounded recovery set
- Active leases protect terminal-session deletion
- Stale leases expire without adopting newer state

**Process Observation**
- Runtime-owned child handle binding
- Exact owner/job/request/state/generation/attempt/time
- Owned-running, owned-exited, absent, inaccessible, mismatched, unknown
- Process exit does not imply job completion
- Failed refresh retains prior snapshot

**Recovery Decision**
- Semantically revalidated snapshot entry
- Exact job/lease/process/blocker/timing evidence
- Idempotent recovery-reviewed session event
- Startup-validated 10,000-event journal
- Event failure rolls back sequence allocation
- Hash-consistent semantic tampering fails startup
- Decisions never mutate state; fix dispatch, mutation, automatic retry/approval, takeover authority to false

**Notification Intent**
- Content-free completed, failed, approval-needed, budget-exhausted evidence
- Stable deduplication identity across creation times
- Bounded notification outbox with recorded state
- Identical retries idempotent after job transitions
- Session-scoped pagination read-only
- Terminal session purge removes jobs/outbox/events together

### Phase 4: Integration and Cleanup (Task 21.10)

**Child Integration Review**
- Diff preview of child changes
- Test results and diagnostic evidence
- Conflict detection with parent branch
- Explicit keep/discard/selective-merge decision
- Worktree cleanup after integration

**Cleanup Eligibility**
- Child terminal
- Worktree healthy and clean
- Editor state saved
- Integration or discard explicitly selected

### Phase 5: Adversarial Testing and Gates (Task 21.11)

**Adversarial Suites**
- Cost runaway: budget exhaustion, overcommit, unknown usage
- Cross-session leakage: shared worktree, budget, context
- Orphan process: crash, kill, restart, cleanup
- Injected task: forged parent, budget, approval, handoff
- Crash recovery: interrupted job, stale lease, process mismatch

**Release Evidence**
- macOS and Windows execution
- Notification delivery and permission
- Sandbox enforcement under load
- Budget enforcement under adversarial usage
- Recovery from all failure modes

## What Must Be Proven

### Before Enabling Background Jobs

1. **Durability**: Job state survives crash, restart, process kill
2. **Isolation**: Jobs cannot access other sessions' data or worktrees
3. **Budget**: Token, cost, time, turn, tool, concurrency limits enforced
4. **Cancellation**: User can cancel running job; cleanup is complete
5. **Notification**: Platform notifications delivered on completion, failure, approval-needed, budget exhaustion
6. **Recovery**: Interrupted jobs reconcile against filesystem, process, Git, event state
7. **Audit**: All privileged operations logged with content-free evidence
8. **Sandbox**: Jobs run with restricted permissions; cannot escape
9. **Orphans**: No orphan processes after crash or kill
10. **Adversarial**: Cost runaway, cross-session leakage, injected task suites pass

### Before Enabling Multi-Agent

1. **Lineage**: Parent/child navigation functional; status visible
2. **Worktree**: Dedicated worktree created, locked, cleaned up
3. **Handoff**: Child results transferred to parent with content-addressed references
4. **Integration**: Diff preview, conflict detection, explicit merge decision
5. **Budget**: Parent budget allocated to children; enforced independently
6. **Cancellation**: Parent can cancel child; child cleanup is complete
7. **Isolation**: Children cannot access parent or sibling data
8. **Concurrent**: Multiple children run concurrently without conflict
9. **Recovery**: Interrupted children reconcile; parent resumes
10. **Adversarial**: Shared worktree, budget leakage, orphan process suites pass

### Before Enabling Unattended Writes

1. **Approval**: All writes require explicit user approval or durable rule
2. **Risk**: High-risk operations (force push, reset, clean, destructive checkout, history rewrite, branch/worktree deletion) require explicit confirmation
3. **Audit**: All writes logged with before/after state, approval decision, evidence
4. **Recovery**: Failed writes roll back completely; partial state reconciled
5. **Sandbox**: Writes restricted to allowed roots; sensitive paths denied
6. **Git**: Git operations respect protected branches, dirty state, conflicts
7. **Filesystem**: File operations respect symlink, sensitive, Git-ignore policy
8. **Secrets**: No secrets written to files, logs, events, or exposed to model
9. **Emergency**: Server-controlled disable prevents writes without exposing session content
10. **OpenSpec**: Separate OpenSpec and release gate approved (Task 21.12)

## Security Boundaries

**Untrusted Data Sources**
- Source files, issue text, tool output, websites, MCP responses, remote messages
- Never treated as higher-priority instructions
- Cannot grant permissions, execute commands, enable hooks, authorize network

**Credential Protection**
- Deny reads of credential stores, SSH keys, browser profiles, cloud credentials, .env files, configured secret globs
- Redact known secrets from UI events, logs, diagnostics, model requests
- Preserve local indication that redaction occurred
- Credentials never enter database or event payloads

**Network Policy**
- Allow network by host and purpose
- Prevent silent credential forwarding across redirects or to tool-provided URLs
- Budget enforcement for network requests

**Process Isolation**
- MCP servers and plugins run with declared permissions
- Origin/version trust required
- Sandbox enforcement per platform
- No orphan processes after crash or kill

**Audit Trail**
- Content-minimal: actor, operation, scope, decision, result, hashes, timing, correlation IDs
- No prompt, code, path, diff, terminal, extension argument content
- Tamper-evident with hash-chained events

## Observability

**Turn Traces**
```
turn-trace/0.6:
  intent: chat_conversation | work_read_only | work_mutation
  runtime: { adapter, version, provider, model }
  context: manifest with source, priority, trust, hash, token size, freshness
  tools: started/terminal observations with identity, status, duration, exit
  approvals: policy observation (read-only) or denial (no user decision yet)
  usage: absolute snapshot with observed/catalog-derived/estimated/unknown labels
  changes: applied workspace edits with checkpoint, final hashes (future)
  tests: observed results (future)
  errors: classified with kind, class, retryable, http_status
  completion: workspace_change, git_change, verification domains
```

**Recovery Decisions**
```
background-job-recovery-decision/0.1:
  job: { id, generation, status }
  lease: { owner, state, process_observation }
  blockers: content-free list
  timing: { created, last_health, decision_time }
  decision: idempotent recovery-reviewed event
  authority: dispatch=false, mutation=false, automatic_retry=false, takeover=false
```

**Notification Intents**
```
background-job-notification-intent/0.1:
  kind: completed | failed | approval_needed | budget_exhausted
  job: { id, generation, status }
  evidence: terminal/result/approval/budget
  deduplication_identity: stable across creation times
  intent_identity: full identity including creation time
  authority: content_included=false, delivery_available=false, platform_delivery=false
```

## Migration and Rollback

**Feature Flags**
- Background jobs: `not-advertised` until gates pass
- Multi-agent: `not-advertised` until gates pass
- Unattended writes: `not-advertised` until separate OpenSpec approved
- Emergency disable: server-controlled, preserves local read/cleanup/recovery

**Data Migration**
- Background job schema v11: jobs, leases, events
- Notification outbox schema v12: intents, deduplication
- WAL-consistent backup before migration
- Read-only recovery on migration failure
- No automatic downgrade

**Rollback Strategy**
- Feature flags hide background/multi-agent without deleting data
- Sidecar/runtime updates versioned; can roll back to last compatible pair
- Database migrations retain backup and forward-compatible event journal
- Failed migration starts workbench read-only; offers diagnostic export
- Emergency disable preserves only reviewed local read/cleanup/recovery paths

## Open Questions

1. **Notification Delivery**: Which macOS/Windows notification APIs? Permission model? Retry policy?
2. **Scheduler Lease Renewal**: Interval? Timeout? Backoff on contention?
3. **Budget Refund**: Partial refund on cancellation? Rollback on failure?
4. **Child Concurrency**: Global limit? Per-project limit? Per-user limit?
5. **Handoff Size**: Maximum result size? Compression? Chunking?
6. **Integration Conflicts**: Automatic resolution? Manual review? Abort?
7. **Audit Retention**: How long? Compaction policy? Export format?
8. **Emergency Disable**: Scope (all writes, background only, multi-agent only)? Duration? Re-enable policy?
9. **Remote Messaging**: Separate OpenSpec required (Task 21.12). Pairing? Authentication? Authorization?
10. **Unattended Writes**: Approval rules? Risk thresholds? Audit requirements?

## References

- Design Section 14: Background and multi-agent execution (lines 828-843)
- Task 21: Background Jobs and Multi-Agent Milestone (lines 1104-1126)
- Task 18: Permission, Sandbox, and Secret Enforcement (lines 931-946)
- Task 5: Event Store, Database, and Recovery (lines 338-373)
- Task 16: Git and Worktree Workflows (lines 823-866)
- Task 17: Context Engine and Compaction (lines 868-929)
- Task 20: Observability, Diagnostics, and Evaluation (lines 961-1102)
