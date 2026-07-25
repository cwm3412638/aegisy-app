## ADDED Requirements

### Requirement: Workspace files are browsable and editable
The workbench SHALL provide a searchable file tree and production-grade editor
for files inside explicitly configured workspace roots.

#### Scenario: User opens a project
- **WHEN** indexing is incomplete
- **THEN** the file tree and direct file opening SHALL remain usable while index progress and ignored paths are visible

#### Scenario: File lies outside granted roots
- **WHEN** the user or Agent attempts to open a path outside current read scope
- **THEN** the operation SHALL be denied or SHALL trigger a scoped permission request before content is read

#### Scenario: File is binary or too large
- **WHEN** the selected file exceeds editor policy or is not safely decodable as text
- **THEN** the workbench SHALL provide metadata and supported preview/download actions without injecting raw content into the editor or model context

### Requirement: Repository search is context-aware and bounded
The system SHALL support filename, text, symbol, definition, reference, and
diagnostic search as available, with explicit indexing provenance.

#### Scenario: Agent searches the repository
- **WHEN** a search tool returns matches
- **THEN** results SHALL include normalized path, range, match type, index freshness, and truncation state

#### Scenario: Search result set is large
- **WHEN** matches exceed negotiated count or content limits
- **THEN** results SHALL be ranked and paginated, and the model SHALL receive a bounded subset plus an explicit truncation indicator

### Requirement: File changes use structured patches and version checks
Agent-proposed edits SHALL be represented as structured path changes with base
content hashes before they are applied.

#### Scenario: Base file is unchanged
- **WHEN** the user approves a valid patch whose base hashes match disk
- **THEN** the runtime SHALL apply it atomically, emit final changed hashes, and refresh the aggregated turn diff

#### Scenario: User changed a file after patch creation
- **WHEN** a patch base hash no longer matches
- **THEN** the runtime SHALL stop application, show the conflicting current state, and require regeneration, explicit rebase, or user resolution

#### Scenario: Patch targets a symlink or escapes the root
- **WHEN** normalization resolves a write outside granted roots or through a denied symlink
- **THEN** the runtime SHALL reject the patch before any file is modified

#### Scenario: Read-only provider proposes a file change
- **WHEN** a bound provider file-change item requests approval while Agent mutation remains read-only
- **THEN** the runtime SHALL validate and persist one immutable, session/turn/project/root-bound Proposal and its referenced preview artifacts before sending the fixed policy denial, SHALL record no user approval or apply authority, and SHALL leave every workspace path unchanged

#### Scenario: Proposal persistence fails before policy denial
- **WHEN** Proposal normalization, artifact storage, binding validation, or the atomic Store transaction fails
- **THEN** the runtime SHALL NOT acknowledge or deny the provider request as durably handled, SHALL fail the Turn closed, and SHALL leave no partial Proposal, artifact reference, event, or workspace mutation

#### Scenario: Caller fails after Proposal commit
- **WHEN** the Proposal, its Blob references, and metadata-only Session event have committed but the caller subsequently fails
- **THEN** the runtime SHALL preserve the committed Proposal graph and SHALL NOT compensate away its Blob references

#### Scenario: One Proposal graph is corrupt
- **WHEN** startup or read validation detects a corrupt Proposal row or referenced Blob
- **THEN** the runtime SHALL quarantine the owning Session without making unrelated healthy Sessions unreadable

#### Scenario: Multi-file patch partially fails
- **WHEN** atomic application cannot complete for every file
- **THEN** the runtime SHALL restore the pre-application checkpoint or report each authoritative file state without presenting the patch as complete

### Requirement: Terminals use runtime-owned PTY sessions
The sidecar SHALL own terminal processes and expose interactive bytes plus
structured lifecycle metadata to the client.

#### Scenario: User opens a terminal
- **WHEN** a terminal is created for a Work session
- **THEN** it SHALL inherit the selected workspace, approved environment, session correlation ID, and declared shell profile without exposing secrets in protocol metadata

#### Scenario: Agent starts a long-running command
- **WHEN** a process remains active after the initiating tool item
- **THEN** it SHALL become a named background terminal with running status, bounded output tail, input policy, and stop control

#### Scenario: UI disconnects
- **WHEN** the desktop renderer restarts while a permitted process remains active
- **THEN** the runtime SHALL preserve or terminate it according to session policy and SHALL report its authoritative state after reconnect

### Requirement: Diagnostics are observed, not invented
The workbench SHALL distinguish diagnostics emitted by language servers,
compilers, linters, and tests from Agent explanations.

#### Scenario: Build produces diagnostics
- **WHEN** a supported parser recognizes file, range, severity, and message
- **THEN** the diagnostic SHALL link to the file and source command while preserving raw output access

#### Scenario: Agent claims a test passed without an observed run
- **WHEN** no successful command event supports the claim
- **THEN** the UI SHALL NOT display a verified-pass indicator

#### Scenario: File changes invalidate diagnostics
- **WHEN** affected file hashes change
- **THEN** prior diagnostics SHALL be marked stale until their source reruns or recomputes them

### Requirement: Workspace mutations have recoverable checkpoints
The runtime SHALL capture sufficient pre-mutation state to inspect and recover
Agent changes without discarding unrelated user work.

#### Scenario: Work turn begins in a dirty repository
- **WHEN** pre-existing changes are present
- **THEN** the checkpoint SHALL record them separately and SHALL NOT include them in an Agent rollback or commit without explicit user selection

#### Scenario: User restores an Agent checkpoint
- **WHEN** later user edits overlap files changed by the Agent
- **THEN** the system SHALL show the overlap and require a selective restore or explicit destructive confirmation

#### Scenario: Project has no Git repository
- **WHEN** a write-capable turn starts outside Git
- **THEN** the runtime SHALL offer a content-addressed file checkpoint or project initialization and SHALL state the weaker recovery guarantees

### Requirement: Large outputs and artifacts are managed explicitly
Tool output and generated artifacts SHALL be bounded, stored by reference, and
included in model context only by deliberate policy.

#### Scenario: Command emits excessive output
- **WHEN** output exceeds the active capture budget
- **THEN** the runtime SHALL keep a bounded head/tail or artifact, report omitted byte count, and avoid blocking the process on an unbounded UI queue

#### Scenario: Agent generates a preview or report
- **WHEN** an artifact is stored
- **THEN** the timeline SHALL show type, path or content identity, size, source turn, retention state, and an explicit context inclusion control
