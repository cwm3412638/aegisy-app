## ADDED Requirements

### Requirement: Projects represent trusted workspace roots
The system SHALL model each project as one or more explicit workspace roots with
stable identity, trust state, repository metadata, instructions, and policy.

#### Scenario: User opens a folder for the first time
- **WHEN** the user selects an unregistered folder
- **THEN** the system SHALL display the resolved path, detected repositories, instruction sources, and trust consequences before enabling executable project content

#### Scenario: Project contains additional roots
- **WHEN** the user adds another folder to a project
- **THEN** the system SHALL record its read/write scope independently and SHALL NOT grant the new root permissions inherited from the original root without confirmation

#### Scenario: Project path moves
- **WHEN** a saved project path no longer exists
- **THEN** the project SHALL remain in history as unavailable and SHALL offer relinking without silently binding to a different path

### Requirement: Projects and sessions are easy to find
The workbench SHALL support pinned projects and searchable session history scoped
to the current project or all projects.

#### Scenario: User pins a project
- **WHEN** the user pins an existing project
- **THEN** it SHALL appear in persistent navigation with its availability and active-session state

#### Scenario: User searches sessions
- **WHEN** the user searches by title, project, branch, model, runtime, status, or text indexed for local search
- **THEN** matching sessions SHALL be returned without loading every full transcript into memory

#### Scenario: Session is currently active
- **WHEN** a session has an in-progress turn, pending approval, or background child
- **THEN** navigation SHALL show that live state and SHALL NOT present it as an idle completed conversation

### Requirement: Session lifecycle is durable
Users SHALL be able to create, name, resume, fork, archive, unarchive, export, and
delete sessions with explicit lineage and state.

#### Scenario: User resumes a stored session
- **WHEN** the selected runtime and workspace can be restored
- **THEN** the system SHALL reconstruct its latest stable state, replay missing events, and append new turns to the same session

#### Scenario: User forks a session
- **WHEN** the user forks at a completed turn or supported item boundary
- **THEN** a new session SHALL copy portable history through that boundary and SHALL retain a link to the source session

#### Scenario: User deletes a session with descendants
- **WHEN** the user requests deletion of a parent session
- **THEN** the system SHALL show affected child sessions and stored artifacts and SHALL require an explicit deletion scope

#### Scenario: User exports a session
- **WHEN** the user requests an export
- **THEN** the system SHALL preview included metadata and content, warn about code or secrets, and produce a documented portable format

### Requirement: Session runtime state is isolated
Each Work session SHALL own its model profile, runtime, permission decisions,
terminals, extensions, added roots, worktree, child tasks, and context state.

#### Scenario: Two sessions run concurrently
- **WHEN** one session changes model, enables an extension, or grants a session-scoped approval
- **THEN** the other session's effective state SHALL remain unchanged

#### Scenario: Session is resumed after policy changes
- **WHEN** project or organization policy became stricter while a session was closed
- **THEN** restored approvals and tools SHALL be intersected with current policy and any revoked access SHALL be reported

### Requirement: Session context can be compacted without deleting history
The system SHALL preserve local event history separately from the compacted
context sent to a model.

#### Scenario: Automatic compaction threshold is reached
- **WHEN** authoritative or conservative context accounting reaches the configured threshold
- **THEN** the system SHALL create a reviewable checkpoint summary containing decisions, unresolved tasks, changed files, commands, tests, failures, and next actions

#### Scenario: User starts manual compaction
- **WHEN** the user supplies optional preservation instructions
- **THEN** those instructions SHALL be stored with the compaction event and the resulting summary SHALL be inspectable before future turns rely on it

#### Scenario: Compaction fails
- **WHEN** the selected model or runtime cannot produce a valid checkpoint summary
- **THEN** the original context and event history SHALL remain authoritative and the session SHALL offer model change, new fork, or manual cleanup

### Requirement: Sessions recover from interruption
The system SHALL distinguish completed, interrupted, failed, waiting, and unknown
turn states after process or application failure.

#### Scenario: UI reconnects to a running runtime
- **WHEN** the client reconnects with the last received event sequence
- **THEN** the runtime SHALL replay later events exactly once in display order

#### Scenario: Runtime died during a mutation
- **WHEN** no terminal event exists for an in-progress command or file change
- **THEN** the recovered session SHALL mark the operation unknown, refresh filesystem and Git state, and require reconciliation before continuing writes

#### Scenario: Event store is inconsistent
- **WHEN** checksums or sequence continuity fail validation
- **THEN** the workbench SHALL enter read-only recovery for the affected session and offer diagnostic export without deleting data
