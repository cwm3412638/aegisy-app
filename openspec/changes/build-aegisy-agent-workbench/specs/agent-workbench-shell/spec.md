## ADDED Requirements

### Requirement: Chat and Work are distinct modes
The workbench SHALL expose `Chat` and `Work` as explicit top-level modes with
different mutation guarantees.

#### Scenario: User starts a Chat conversation
- **WHEN** the user creates a conversation in Chat mode
- **THEN** the session SHALL deny workspace writes and shell execution by default and SHALL label the conversation as non-mutating

#### Scenario: User converts Chat into Work
- **WHEN** the user chooses to continue a Chat conversation as repository work
- **THEN** the system SHALL create or fork a Work session bound to a selected project, runtime, model profile, and permission profile without changing the original Chat history

#### Scenario: User starts Work without a project
- **WHEN** the user selects Work while no project root is active
- **THEN** the system SHALL require folder selection or an existing project before enabling write-capable tools

### Requirement: Agent interaction remains adjacent to the workspace
On a wide supported viewport, the workbench SHALL keep the Agent timeline and
composer visible to the left of the active work canvas.

#### Scenario: Workbench opens on a wide window
- **WHEN** the available workbench width meets the defined wide-layout breakpoint
- **THEN** the product rail, Agent surface, and work canvas SHALL be simultaneously usable without overlapping text, approvals, or editor controls

#### Scenario: Workbench narrows below the breakpoint
- **WHEN** the window becomes too narrow for three usable panes
- **THEN** the system SHALL collapse secondary panes into explicit drawers or tabs while keeping the active composer and pending approval reachable

#### Scenario: User resizes a pane
- **WHEN** the user changes the Agent or work-canvas split
- **THEN** the system SHALL enforce minimum usable widths and SHALL persist the accepted layout per device

### Requirement: Structured Agent timeline
The Agent surface SHALL render plans, messages, reasoning summaries, commands,
file changes, approvals, questions, diagnostics, errors, usage, and artifacts as
typed timeline items.

#### Scenario: Runtime streams a command
- **WHEN** a command item starts, produces output, and completes
- **THEN** the same timeline item SHALL update incrementally and SHALL display command, working directory, status, duration, and bounded output

#### Scenario: Runtime sends an unknown item type
- **WHEN** the client receives a valid protocol item type it does not understand
- **THEN** the client SHALL preserve it for diagnostics, render a safe unsupported-item placeholder, and SHALL NOT infer a completed or successful state

#### Scenario: Turn contains many events
- **WHEN** a session contains more events than the visible timeline can render economically
- **THEN** the timeline SHALL virtualize off-screen items without losing focus, scroll position, or live-event ordering

### Requirement: Composer exposes execution context
The command composer SHALL show the active project, mode, workspace, runtime,
model profile, permission profile, Git branch, and context status before a turn
is submitted.

#### Scenario: User submits a Work request
- **WHEN** the user sends a Work request
- **THEN** the turn SHALL capture the visible execution context as immutable turn-start metadata

#### Scenario: Required execution context is unavailable
- **WHEN** the runtime, model, project, or permission profile is unavailable or incompatible
- **THEN** submission SHALL be blocked with the exact missing or incompatible state and a direct remediation action

#### Scenario: User attaches files or images
- **WHEN** the user adds supported context through the composer
- **THEN** the composer SHALL list each attachment with origin, size, inclusion state, and removal control before submission

### Requirement: Workbench views and commands are keyboard accessible
The workbench SHALL make primary navigation, session selection, editor/work
views, composer, approvals, and cancellation reachable by keyboard and
assistive technology.

#### Scenario: Keyboard user changes mode
- **WHEN** focus is in the workbench and the user invokes the documented mode command
- **THEN** Chat or Work SHALL become active without moving focus to an unrelated control

#### Scenario: Approval requests attention
- **WHEN** an approval request arrives while focus is elsewhere
- **THEN** the client SHALL announce the request, expose its risk and action text, and provide keyboard actions without obscuring the active content

#### Scenario: Display scale or text size increases
- **WHEN** the application runs at supported Windows scaling or macOS accessibility text settings
- **THEN** controls SHALL retain readable labels, stable hit targets, and non-overlapping state indicators

### Requirement: Workbench layout restores safely
The workbench SHALL persist project-local view state and device-local pane state
without persisting transient secrets or invalid screen coordinates.

#### Scenario: Application restarts normally
- **WHEN** the user reopens the application
- **THEN** the last project, selected session, safe view tabs, and pane arrangement SHALL restore without automatically resuming an interrupted write operation

#### Scenario: Saved layout is invalid
- **WHEN** a monitor is removed or persisted dimensions violate current minimums
- **THEN** the system SHALL clamp the layout to the available screen and provide a reset-layout command
