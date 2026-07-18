## ADDED Requirements

### Requirement: Complex work uses a visible structured plan
The runtime SHALL represent plans as ordered steps with stable IDs, status,
explanation, owner, dependencies, and relevant session or child task.

#### Scenario: Agent creates or revises a plan
- **WHEN** plan state changes
- **THEN** the UI SHALL show pending, in-progress, completed, blocked, and cancelled steps without deriving status from prose

#### Scenario: Plan requires user choice
- **WHEN** an architectural or destructive decision materially changes implementation
- **THEN** the plan SHALL pause with a structured question before dependent write steps begin

#### Scenario: Plan is stale after repository change
- **WHEN** relevant base revision, files, or requirements change
- **THEN** affected steps SHALL be marked for revalidation rather than remaining silently complete

### Requirement: Child tasks have explicit contracts and lineage
A parent Agent SHALL delegate through a structured child-task request containing
goal, context, workspace, tools, model profile, permissions, budget, and expected
result shape.

#### Scenario: Parent starts a child task
- **WHEN** delegation is accepted by policy
- **THEN** the system SHALL create a child session linked to the parent turn and SHALL display its goal, owner, state, and resource limits

#### Scenario: Child requests broader access
- **WHEN** work exceeds delegated permissions or workspace
- **THEN** the request SHALL return through the parent/user approval path and SHALL not inherit broader parent access automatically

#### Scenario: Child completes
- **WHEN** terminal state is reached
- **THEN** it SHALL return a bounded structured handoff containing result summary, evidence, artifacts, changed paths/branch, tests, risks, and unresolved items

### Requirement: Concurrent writes are isolated
Write-capable child and background tasks SHALL use separate worktrees or another
future workspace mechanism providing equivalent isolation and provenance.

#### Scenario: Two children modify related files
- **WHEN** both tasks execute concurrently
- **THEN** their filesystem and Git writes SHALL not be visible to each other unless the parent explicitly shares a reviewed artifact or commit

#### Scenario: Isolation cannot be created
- **WHEN** repository, disk, or policy prevents a dedicated workspace
- **THEN** the task SHALL remain queued/read-only or fail before write tools become available

### Requirement: Resource budgets are enforced by the runtime
Every child and background task SHALL have token, cost, wall-clock, turn, tool,
concurrency, and optional network budgets independent of prompt compliance.

#### Scenario: Task approaches a budget
- **WHEN** a warning threshold is reached
- **THEN** the UI and parent SHALL receive a structured budget event with used and remaining amounts

#### Scenario: Hard budget is exhausted
- **WHEN** continuing would exceed the configured limit
- **THEN** new model/tool work SHALL stop and the task SHALL return a budget-exhausted state with partial results

#### Scenario: Provider usage is unknown
- **WHEN** authoritative cost or token data is unavailable
- **THEN** hard monetary automation SHALL use conservative configured limits or remain disabled rather than treating unknown as zero

### Requirement: Background jobs have durable lifecycle controls
Background work SHALL be queued, started, paused when supported, cancelled,
retried, inspected, and recovered through the same session event model as
interactive work.

#### Scenario: Application UI closes
- **WHEN** background service mode is enabled and a job may continue
- **THEN** job state SHALL remain durable and the user SHALL receive platform-appropriate completion, failure, or approval-needed notification

#### Scenario: Runtime restarts during a job
- **WHEN** a safe retry boundary exists
- **THEN** the scheduler SHALL resume or retry idempotently from that boundary; otherwise it SHALL mark the job interrupted for manual reconciliation

#### Scenario: Job waits for approval
- **WHEN** no authorized user is present
- **THEN** it SHALL pause without broadening permissions or timing out into automatic approval

### Requirement: Child results integrate through review
Parent sessions SHALL NOT silently merge child changes into their active branch or
context.

#### Scenario: Child proposes code changes
- **WHEN** the parent receives its handoff
- **THEN** the UI SHALL provide branch/diff review, test evidence, conflicts, and explicit integrate, request changes, keep, or discard actions

#### Scenario: Parent accepts a read-only analysis
- **WHEN** its summary is added to parent context
- **THEN** the context manifest SHALL retain child identity, source revision, artifact links, and truncation state

### Requirement: Autonomy is feature-gated by release evidence
Multi-agent, scheduling, and unattended write modes SHALL remain disabled until
their security, recovery, evaluation, and support gates are satisfied.

#### Scenario: Build lacks a required gate
- **WHEN** a user or extension requests an unavailable autonomy mode
- **THEN** the system SHALL refuse it with the missing gate rather than exposing an experimental hidden switch in production

#### Scenario: Regression crosses a release threshold
- **WHEN** evaluation, security, crash, cost, or rollback metrics fall below policy
- **THEN** the affected autonomy feature SHALL be disabled for promotion until remediation and revalidation
