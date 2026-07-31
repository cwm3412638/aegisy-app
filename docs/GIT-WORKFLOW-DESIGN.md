# Git Workflow Design

## Overview

This document defines Git operation requirements, safety guarantees, worktree isolation for parallel work, approval requirements for destructive operations, state validation and reconciliation, and implementation phases for Git integration in the Aegisy Agent Workbench.

## Git Operation Requirements

### Read-Only Operations

Available without approval:
- `git status` (porcelain v2)
- `git log` with structured output
- `git branch --list`
- `git diff` (working tree, staged, commits)
- `git show` for commit inspection
- Repository metadata queries

### Mutation Classification

**Low Risk** (may proceed with session-level approval):
- Create new branch in clean repository
- Switch to new branch
- Stage Agent-owned changes when policy allows

**Medium Risk** (requires explicit UI confirmation):
- `git commit`
- `git stash` / `git stash pop`
- `git merge`
- `git rebase`
- `git cherry-pick`
- `git push`

**High Risk** (requires per-operation confirmation, no blanket approval):
- `git push --force`
- `git reset --hard`
- `git clean`
- Delete branches or worktrees
- Discard user changes
- Rewrite shared history

### Command Execution

- Use Git porcelain v2 and structured plumbing commands
- Avoid parsing localized human output
- Execute through sidecar process, never in UI process
- Validate Git version ≥ 2.31.0 at runtime startup
- Bind operations to exact Session/project/root identity

## Worktree Isolation for Parallel Work

### Dedicated Worktree Model

Each background or concurrent coding task receives:
- Dedicated Git worktree
- Isolated branch
- Independent working directory
- Separate index state

Read-only explorer tasks may share a snapshot without worktree allocation.

### Worktree Lifecycle

**Creation**:
- Record starting HEAD
- Capture dirty state before worktree creation
- Bind worktree identity (device/inode or volume serial/file ID)
- Store worktree path and branch name
- Generate checkpoint of initial state

**Active State**:
- Track changed paths
- Record test execution results
- Maintain commit candidates
- Preserve user pre-existing changes separately from Agent changes

**Integration**:
- User pre-existing changes never mixed into automatic checkpoint
- Explicit selection required for user change inclusion
- Integration into parent branch is separate reviewable action
- Preview all changes before merge

**Cleanup**:
- Validate worktree state before removal
- Preserve work if uncertain
- Remove worktree directory
- Prune worktree registration

### Workspace Binding

Work Sessions bind to:
- Project root with filesystem identity
- Git availability state
- Branch hash and safe display name
- HEAD commit
- Worktree identity
- Explicit `dedicated_worktree` flag (initially false)

Schema v13 `session_workspace_bindings` projection stores:
- Primary root identity
- Git availability
- Branch hash (used for exact search)
- Safe branch display (sensitive shapes excluded)
- HEAD commit
- Worktree identity
- Dedicated worktree flag

## Approval Requirements

### Approval Architecture

Approval decisions:
- `deny` - operation blocked
- `allow once` - single operation
- `allow for turn` - current turn only
- `allow for session` - session duration
- Scoped durable rule - narrowly defined, editable

### Approval UI

Display for user review:
- Command and arguments
- Current working directory
- Environment variables
- Requested paths/hosts
- Risk level classification
- Reason for operation
- Resulting diff preview

### Durable Rules

- Editable through UI
- Hash relevant command/extension definition
- Changed code returns to untrusted state
- Cannot blanket-approve high-risk operations in initial releases

### Metadata-Only Contract

`approval-acknowledgement/0.1` (from task 3.6):
- Deterministic scope, requirement, operation identities
- Session/Turn/scope binding
- Exact request fingerprint
- Idempotency key
- Equivalent requests replay exactly
- Drift conflicts fail closed
- Contiguous revision transitions
- Terminal and reconciliation-required states producer-sticky
- Fixed-false authority fields: no user-decision, Approval, mutation, or execution authority
- Unknown fields, unsafe integers, credential/token shapes, `allowed`/`approved` values fail closed

Not connected to authority issuer, schema-v20 ledger, AAP, Qt, Codex, or user Approval producer (as of current implementation).

## State Validation and Reconciliation

### Git Mutation Acknowledgement

`git-mutation-acknowledgement/0.1` (from task 3.6):
- Session/project/root binding
- Mutation kind classification
- Idempotency key
- Request fingerprint
- Immutable Git-plan identity
- Operation identity derivation
- Equivalent retries replayable
- Same-key drift is conflict
- Unrelated keys distinct
- Accepted/Committed/Failed/ReconciliationRequired revisions contiguous and monotonic
- Uncertainty producer-sticky
- Terminal observations require opaque evidence
- Fixed-false mutation/approval/execution authority

Not connected to Git execution, Store, AAP, Qt, or approval issuer (as of current implementation).

### Operation Reconciliation

`operation-reconciliation/0.1` (from task 6.9):

**Evidence Inputs**:
- Content-free event state
- Process state
- Workspace state
- Git state

**Outputs**:
- State classification (unknown/running/completed/failed/interrupted)
- Decision
- Blocker list
- Observed domains
- Content-hashed review ID

**Constraints**:
- Missing terminal events remain `unknown`
- Live processes remain `running`
- Changed/unavailable workspace or Git state requires explicit review
- In-progress Git operations block subsequent writes
- Missing required evidence blocks writes
- Never infers mutation success
- Never probes host automatically
- Never executes recovery automatically
- No AAP/Qt mutation controls exposed

### Durable Integration

AAP `operation/reconcile`:
- Validates evidence contract
- Appends content-free `operation.reconciled/0.1` event
- Idempotent for identical evidence
- Reads latest per-operation result after restart
- Unknown/blocked results gate later mutations with error `-32132`
- Newer authoritative completed/failed/interrupted review removes gate

### Probe Integration

Capability `operation.reconciliation.probe` exposes `operation/probe`:
- Resolves registered Work-session root
- Hashes bounded visible workspace metadata
- Reads structured Git status
- Observes runtime-owned turn/terminal state only
- Returns state labels and snapshot hashes without content, arbitrary paths, or caller-selected PIDs
- Callers may provide event state
- Durable turn/Git mappings may supply event state when omitted
- Callers must submit result to `operation/reconcile`

### Startup Discovery

Durable Runtime preload:
- Scans latest validated reconciliation event per session/operation pair
- Bounded cache
- Event store remains authoritative
- Scan failures never imply safety

### Event Authority

When `operation/probe` omits `event`, durable Runtime derives:
- Latest validated `turn.*` state from session event stream
- Existing `git.workflow.*` lifecycle state
- Git prepared/dispatching/in-progress map to running
- Completed/failed/aborted map to terminal evidence
- Conflicted/recovered remain unknown
- Explicit caller event values labelled as caller-supplied
- Workspace-edit/terminal/background-job event families not inferred

### Recovery Status

Capability `operation.reconciliation.status` exposes `operation/status`:
- Current session gate
- Bounded review summary
- Explicit `recovery_action_available:false` while blocked

Qt provides:
- Explicit turn-only review path
- Collects `operation/probe` evidence
- Records `operation/reconcile`
- Identity/schema failures remain read-only
- Git/Workspace/Terminal review unavailable (as of current implementation)

## Implementation Phases

### Phase 1: Read-Only Git Integration (Completed)

- Git version validation (≥ 2.31.0)
- Structured status queries
- Log and branch listing
- Diff generation
- Repository metadata
- No mutations enabled

### Phase 2: Workspace Binding (Completed)

- Schema v13 `session_workspace_bindings`
- Project root identity binding
- Git availability detection
- Branch hash and display
- HEAD tracking
- Worktree identity (dedicated_worktree: false)
- Event-backed binding: `session.created` records workspace state
- Restart recovery: branch drift blocks resume

### Phase 3: Metadata-Only Contracts (In Progress)

- `approval-acknowledgement/0.1` contract defined
- `git-mutation-acknowledgement/0.1` contract defined
- `operation-reconciliation/0.1` contract defined
- Not connected to execution, Store, AAP, Qt, or authority issuers
- Focused tests cover contract boundaries

### Phase 4: Durable Reconciliation (Partial)

- `operation/reconcile` AAP method
- `operation/probe` AAP method
- `operation/status` AAP method
- Content-free `operation.reconciled/0.1` event
- Startup preload of reconciliation state
- Qt turn-only review path
- Workspace/terminal/background-job event sources pending
- Complete authoritative host probes pending
- Full cross-kind Qt review pending
- Recovery actions pending

### Phase 5: Low-Risk Mutations (Planned)

**Prerequisites**:
- Complete Phase 4 reconciliation
- Session-level approval flow
- Git plan validation
- Idempotency enforcement

**Operations**:
- Create branch in clean repository
- Switch to new branch
- Stage Agent-owned changes

**Requirements**:
- Bind to schema-v20 mutation acknowledgement ledger
- Atomic reservation before dispatch
- Revision CAS for state transitions
- Startup reconciliation for uncertain operations
- Qt consumption after exact anchor validation

### Phase 6: Medium-Risk Mutations (Planned)

**Prerequisites**:
- Phase 5 complete
- Explicit UI confirmation flow
- Diff preview integration
- Rollback capability

**Operations**:
- Commit with message
- Stash/pop
- Merge
- Rebase
- Cherry-pick
- Push

**Requirements**:
- Per-operation UI confirmation
- Cannot use blanket session approval
- Preview all changes
- Validate clean state where required
- Record operation in event stream

### Phase 7: Dedicated Worktrees (Planned)

**Prerequisites**:
- Phase 6 complete
- Background job infrastructure
- Concurrent session isolation tests
- Worktree cleanup validation

**Capabilities**:
- Create dedicated worktree per background task
- Isolate branch per worktree
- Track worktree lifecycle
- Preserve user changes separately
- Integration review workflow

**Requirements**:
- Update `dedicated_worktree` flag in workspace binding
- Store worktree path and identity
- Checkpoint initial state
- Track changed paths
- Validate state before cleanup
- Prevent mixing user and Agent changes

### Phase 8: High-Risk Mutations (Planned)

**Prerequisites**:
- Phase 7 complete
- Enhanced confirmation UI
- Backup/snapshot capability
- Comprehensive testing

**Operations**:
- Force push
- Reset --hard
- Clean
- Delete branches/worktrees
- Discard user changes
- Rewrite history

**Requirements**:
- Per-operation confirmation only
- No durable approval rules
- Enhanced risk warnings
- Backup before execution
- Validation of shared history impact
- Cannot proceed if uncertain

### Phase 9: Complete Reconciliation (Planned)

**Prerequisites**:
- All mutation phases complete
- Complete Windows evidence
- Cross-platform validation

**Capabilities**:
- Workspace/terminal/background-job event sources
- Complete authoritative host probes
- Durable operation discovery beyond bounded preload
- Full cross-kind Qt review
- Recovery actions for all operation types

**Requirements**:
- Complete task 6.9
- Cross-platform evidence
- All recovery paths validated
- User review for all uncertain states

## Security Boundaries

### Execution Isolation

- All Git operations execute in sidecar process
- Never execute in UI process
- Validate peer identity before Git operations
- Bind operations to authenticated session

### Path Validation

- Normalize workspace-relative paths
- Check symlinks
- Validate against project roots
- Reject path escapes
- Verify filesystem identity

### State Protection

- Hash file versions before operations
- Detect concurrent modifications
- Preserve dirty state
- Never silently overwrite user work
- Require explicit selection for user change inclusion

### Audit Trail

Content-minimal audit events:
- Actor (user/agent)
- Operation type
- Scope (paths, branch)
- Decision (approved/denied)
- Result (success/failure)
- Hashes (before/after)
- Timing
- Correlation IDs

No repository content in audit by default.

## Error Handling

### Classification

`runtime-error/0.1` Git class:
- Protocol errors
- Git command failures
- Workspace unavailable
- Permission denied
- Merge conflicts
- Detached HEAD
- Dirty working tree

### Recovery

- Preserve state on error
- Never fabricate success
- Explicit reconciliation required
- User review for uncertain states
- Rollback capability where possible

### User Communication

- Clear error messages
- Suggested remediation
- Link to reconciliation review
- Preserve context for retry

## Testing Requirements

### Unit Tests

- Git command construction
- Path normalization
- State validation
- Identity binding
- Approval logic

### Integration Tests

- Real Git operations
- Worktree lifecycle
- Concurrent modifications
- Restart recovery
- Branch drift detection

### Security Tests

- Path escape attempts
- Symlink attacks
- Concurrent user modifications
- Permission boundary violations
- Approval bypass attempts

### Cross-Platform Tests

- macOS and Windows
- Different Git versions
- Various repository states
- Filesystem identity handling
- Unicode paths

## Future Considerations

### Remote Operations

- Credential management
- SSH key handling
- HTTPS authentication
- Proxy support
- Network policy enforcement

### Advanced Workflows

- Submodules
- Git LFS
- Sparse checkout
- Partial clone
- Worktree prune strategies

### Performance

- Incremental status
- Cached repository state
- Parallel worktree operations
- Efficient diff generation
- Index optimization

## References

- Design document: `openspec/changes/build-aegisy-agent-workbench/design.md` section 11
- Tasks document: `openspec/changes/build-aegisy-agent-workbench/tasks.md` task 16
- Task 3.6: Idempotency semantics
- Task 6.9: Operation reconciliation
- Schema v13: Workspace bindings
- Schema v20: Mutation acknowledgements
