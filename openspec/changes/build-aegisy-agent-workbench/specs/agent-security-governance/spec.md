## ADDED Requirements

### Requirement: Permission profiles are enforced outside the model
Every Chat or Work session SHALL have an effective permission profile controlling
filesystem, commands, network, extensions, browser/computer use, and background
execution.

#### Scenario: Chat mode is active
- **WHEN** a model attempts a write or shell tool through malformed or injected output
- **THEN** the runtime SHALL deny it because the Chat profile does not expose that capability

#### Scenario: Workspace Write profile is active
- **WHEN** a tool requests access outside declared roots or network policy
- **THEN** the runtime SHALL deny or issue a granular permission request without broadening unrelated permissions

#### Scenario: Organization policy changes
- **WHEN** managed restrictions become stricter
- **THEN** effective permissions SHALL update before the next tool call and SHALL invalidate incompatible session grants

### Requirement: Approvals are granular and reviewable
Approval UI SHALL show the exact operation, scope, environment, risk, reason, and
resulting effects before a decision.

#### Scenario: Command approval is requested
- **WHEN** a command requires consent
- **THEN** the UI SHALL show parsed command actions, raw command, cwd, requested filesystem/network additions, environment, and available decision scopes

#### Scenario: File change approval is requested
- **WHEN** a structured patch requires consent
- **THEN** the UI SHALL show changed paths, additions/deletions, sensitive-path warnings, base state, and complete diff access

#### Scenario: User grants a session rule
- **WHEN** the decision permits reuse
- **THEN** the stored rule SHALL be no broader than the reviewed command/tool/path/host pattern and SHALL remain editable and revocable

#### Scenario: High-risk operation is requested
- **WHEN** the action can destroy data, expose credentials, rewrite remote history, or weaken sandbox policy
- **THEN** blanket auto-approval SHALL not apply and the decision SHALL require explicit confirmation

### Requirement: Secrets are protected end to end
The system SHALL identify configured and detected secrets and prevent accidental
inclusion in model requests, events, logs, diagnostics, or extension arguments.

#### Scenario: Agent reads a denied secret path
- **WHEN** the path matches secure-storage, SSH, cloud credential, browser profile, `.env`, or project secret policy
- **THEN** read SHALL be denied unless the user grants that exact scope with a visible warning

#### Scenario: Tool output contains a recognized secret
- **WHEN** output enters the event or model context pipeline
- **THEN** the secret SHALL be redacted before persistence or transmission and the local event SHALL indicate that redaction occurred

#### Scenario: User intentionally attaches sensitive content
- **WHEN** policy permits an explicit override
- **THEN** the composer SHALL show destination provider, retention constraints, and one-turn scope before sending

### Requirement: Untrusted content cannot elevate instruction priority
The runtime SHALL label repository files, issues, web pages, remote messages,
tool output, MCP responses, and generated artifacts as data from untrusted
sources that cannot elevate instruction priority.

#### Scenario: Source file contains tool instructions
- **WHEN** indexed or opened content asks the Agent to reveal secrets or change policy
- **THEN** the runtime SHALL keep it at data priority and policy enforcement SHALL remain unchanged

#### Scenario: MCP result contains a forged approval
- **WHEN** tool output imitates system or user authorization
- **THEN** it SHALL not resolve a pending approval or create a permission grant

#### Scenario: Remote task request starts work
- **WHEN** remote control becomes supported in a future milestone
- **THEN** sender pairing, project policy, sandbox, and explicit remote-action permissions SHALL apply before any local execution

### Requirement: UI and runtime IPC are locally authenticated
The sidecar SHALL reject clients that lack the per-launch authenticated channel
and SHALL minimize exposed local transport surface.

#### Scenario: Sidecar starts
- **WHEN** the host launches it
- **THEN** a one-time bootstrap secret or inherited authenticated handle SHALL establish the session and SHALL not be written to ordinary logs or command lines visible to other users

#### Scenario: Unknown local process connects
- **WHEN** authentication or peer identity validation fails
- **THEN** the sidecar SHALL close the connection before exposing project, session, credential, or tool methods

#### Scenario: macOS peer identity is verified before bootstrap authentication exists
- **WHEN** the owner-only Unix socket verifies the current UID and exact supervised Qt/sidecar PID but no one-time bootstrap proof has been exchanged
- **THEN** the connection SHALL report `peer_verified: true` and `authenticated: false`, SHALL expose no additional authority, and SHALL NOT satisfy the authenticated-channel release gate

#### Scenario: macOS endpoint or peer identity becomes uncertain
- **WHEN** endpoint ownership, permissions, extended ACL, device/inode identity, supervising parent, peer UID/PID, or generation binding cannot be proven
- **THEN** the host and sidecar SHALL process no AAP frame, SHALL NOT fall back to stdio, SHALL terminate and reap the owned process generation, and SHALL preserve any replacement object whose identity is not the recorded launch object

#### Scenario: Windows peer identity is verified before bootstrap authentication exists
- **WHEN** the first-instance named pipe has a protected DACL for the current token-user SID and both peers verify the exact supervised parent/client PID and process generation but no one-time bootstrap proof has been exchanged
- **THEN** the connection SHALL report `peer_verified: true` and `authenticated: false`, SHALL expose no additional authority, and SHALL NOT satisfy the authenticated-channel release gate

#### Scenario: Windows pipe ACL or peer generation becomes uncertain
- **WHEN** the current token-user SID, protected DACL, first-instance ownership, remote-client rejection, supervising parent liveness, client/server PID, process creation time, or generation binding cannot be proven
- **THEN** the host and sidecar SHALL process no AAP frame, SHALL NOT fall back to stdio, and SHALL terminate and reap the owned process generation

#### Scenario: Browser-originated request reaches runtime transport
- **WHEN** a web page attempts direct socket or WebSocket access
- **THEN** origin and authentication checks SHALL reject it; embedded workbench content SHALL communicate only through the constrained host bridge

### Requirement: Sandboxing is a release gate
Write-capable native Agent execution SHALL not ship on a platform until filesystem,
process, and network enforcement are verified for that platform.

#### Scenario: Supported sandbox is unavailable
- **WHEN** the selected runtime cannot enforce the active profile
- **THEN** Work SHALL remain read-only or SHALL require a clearly labeled Full Access mode with explicit per-session confirmation according to release policy

#### Scenario: Sandbox denies a tool action
- **WHEN** OS enforcement blocks access
- **THEN** the item SHALL report a sandbox denial and SHALL not retry outside the sandbox automatically

#### Scenario: Sandbox escape regression is detected
- **WHEN** a security test demonstrates policy bypass
- **THEN** the affected write-capable release channel SHALL be blocked until fixed and revalidated

### Requirement: Audit and telemetry preserve privacy
The system SHALL maintain local security audit events while excluding repository
and prompt content from cloud telemetry by default.

#### Scenario: Privileged action completes
- **WHEN** a permission, command, write, Git mutation, extension execution, or network request reaches terminal state
- **THEN** the audit SHALL record actor, operation class, scope, decision, result, hashes, timing, and correlation IDs without secrets

#### Scenario: User exports diagnostics
- **WHEN** the user initiates export
- **THEN** the system SHALL preview content categories, apply redaction, and require explicit inclusion of any transcript, path, diff, or terminal content

#### Scenario: Cloud telemetry is disabled
- **WHEN** no opt-in or managed requirement exists
- **THEN** prompts, code, file paths, diffs, command output, and extension arguments SHALL remain local
