# Aegisy Coding Terminology

**Status**: Draft for Product Review  
**Created**: 2026-07-31  
**Purpose**: Define canonical terminology for Aegisy Coding product

## Core Concepts

### Project

A **Project** is a bounded workspace with one or more filesystem roots that
contain the user's code, configuration, and assets.

**Properties:**
- Identified by canonical filesystem root path(s)
- Persists across application restarts
- Can have multiple roots (e.g., monorepo with separate services)
- Contains Git repository metadata (when present)
- Scopes all Work sessions and file operations

**Usage**: "Open a project", "Switch projects", "Project settings"

**Technical**: Internally identified by filesystem device/inode on Unix,
volume serial/file ID on Windows, with path fallback for unsupported filesystems.

### Session

A **Session** is a durable conversation thread between the user and the Agent,
containing turns, timeline events, and execution state.

**Properties:**
- Exists in either Chat or Work mode
- Work sessions are bound to exactly one project
- Survives runtime crashes and application restarts
- Contains complete history of turns and timeline events
- Can be archived, forked, or deleted

**Types:**
- **Chat Session**: Non-mutating exploration and planning
- **Work Session**: Project-bound execution with mutation capabilities

**Usage**: "Start a new session", "Resume session", "Fork session at this point"

**Technical**: Identified by UUID, stored in SQLite with event stream and
projections, bound to provider thread when using external adapters.

### Turn

A **Turn** is one complete request-response cycle between the user and the Agent
within a session.

**Properties:**
- Begins with user input (prompt, approval, steering)
- Contains Agent reasoning, tool use, and responses
- Ends with completion, error, or cancellation
- Atomic unit for checkpointing and replay
- Generates timeline items as it progresses

**Lifecycle States:**
- `started`: Turn has begun processing
- `in_progress`: Agent is actively working
- `completed`: Turn finished successfully
- `failed`: Turn encountered an error
- `cancelled`: User cancelled the turn

**Usage**: "Cancel this turn", "Retry from this turn", "Turn took 45 seconds"

**Technical**: Durably recorded with idempotency keys, mutation acknowledgements,
and Timeline anchors for crash recovery.

### Timeline

The **Timeline** is the ordered sequence of events and items within a session,
representing everything that happened during the conversation.

**Properties:**
- Append-only event stream
- Contains messages, tool uses, approvals, errors, usage
- Supports pagination and replay
- Survives crashes and reconnects
- Basis for session export and fork

**Item Types:**
- `message`: User or assistant text
- `tool`: Command execution, file change, search
- `approval`: User decision on proposed action
- `error`: Failure or warning
- `usage`: Token and cost information
- `plan`: Agent's proposed approach
- `diff`: Unified diff for file changes

**Usage**: "Scroll through timeline", "Timeline shows 47 items", "Export timeline"

**Technical**: Stored as event stream with projections, bounded by sequence
numbers, supports snapshot and subscription for live updates.

### Task

A **Task** is a discrete unit of work that the Agent is performing or has
completed, often corresponding to a tool use or multi-step operation.

**Properties:**
- Represents one logical operation (e.g., "Run tests", "Fix bug in auth.ts")
- Can have subtasks or dependencies
- Tracks progress and completion state
- May span multiple turns
- Visible in timeline and task list

**States:**
- `pending`: Not yet started
- `in_progress`: Currently executing
- `completed`: Successfully finished
- `failed`: Encountered an error
- `cancelled`: User or system cancelled

**Usage**: "View active tasks", "Task completed successfully", "Cancel running task"

**Technical**: Represented as timeline items with structured metadata, may
correspond to background jobs in the runtime.

### Runtime

The **Runtime** is the execution environment that manages Agent operations,
workspace access, and provider communication.

**Components:**
- **Sidecar**: Rust process (`aegisy-agentd`) that owns execution
- **Adapter**: Provider-specific integration (e.g., Codex App Server)
- **Store**: Durable SQLite database for sessions and events
- **Transport**: IPC channel between UI and sidecar (stdio, socket, named pipe)

**Properties:**
- Isolated from UI process for security
- Versioned protocol (AAP) for communication
- Supervises terminals, LSP servers, and child processes
- Enforces permission boundaries
- Handles crash recovery and reconnection

**Usage**: "Runtime is healthy", "Restart runtime", "Runtime version 0.1.0"

**Technical**: Communicates via Aegisy Agent Protocol (AAP) over authenticated
IPC, maintains session state, and coordinates with provider adapters.

### Workspace

The **Workspace** represents the current state of files, Git repository, and
filesystem within a project.

**Properties:**
- Scoped to project root(s)
- Tracks file changes and Git state
- Enforces canonical paths and symlink policy
- Provides structured edit operations
- Validates changes before application

**Operations:**
- `read`: Get file contents
- `write`: Save file changes
- `list`: Enumerate files and directories
- `watch`: Monitor for external changes
- `metadata`: Get file info without reading content

**Usage**: "Workspace has unsaved changes", "Workspace root", "Workspace files"

**Technical**: Enforced by Runtime with canonical path validation, ignore rules,
sensitive path denial, and atomic write operations.

## Supporting Concepts

### Approval

An **Approval** is a user decision about whether to allow a proposed Agent action.

**Decision Types:**
- `deny`: Reject the action
- `allow_once`: Permit this single instance
- `allow_for_turn`: Permit for current turn
- `allow_for_session`: Permit for this session
- `durable_rule`: Create reusable permission rule

**Context Shown:**
- Command or operation details
- Working directory and environment
- Affected files or network hosts
- Risk level assessment
- Reason for the request

**Usage**: "Approve file changes", "Deny command execution", "Create approval rule"

### Checkpoint

A **Checkpoint** is a saved snapshot of session state that can be used for
compaction or recovery.

**Properties:**
- Contains summary of decisions and changes
- Preserves context for continuation
- Enables session compaction
- Supports fork-from-checkpoint
- Immutable once created

**Contents:**
- Unresolved tasks
- Changed files
- Commands executed
- Test results
- Next actions

**Usage**: "Create checkpoint", "Resume from checkpoint", "Checkpoint at turn 42"

### Extension

An **Extension** is a user-installed or project-provided capability that extends
Agent behavior.

**Types:**
- **Instruction**: Project guidance (e.g., `AGENTS.md`)
- **Skill**: Reusable instruction package
- **Hook**: Lifecycle automation
- **MCP Server**: External tools/resources
- **Plugin**: Installable bundle
- **Runtime Adapter**: Signed Aegisy integration

**Properties:**
- Versioned and content-hashed
- Requires trust and permission grants
- Can be enabled per-project or globally
- Invalidates approvals when changed

**Usage**: "Install extension", "Enable skill", "Configure MCP server"

### Profile

A **Profile** defines configuration for model selection, provider routing, or
permission boundaries.

**Types:**
- **Model Profile**: Which model to use (e.g., "Claude Opus 5")
- **Connection Profile**: Provider credentials and routing
- **Permission Profile**: What operations are allowed

**Usage**: "Select model profile", "Switch to Codex profile", "Developer permissions"

## Terminology Guidelines

### Preferred Terms

Use these terms consistently in UI, documentation, and code:

- **Session** (not "thread", "conversation", "chat")
- **Turn** (not "request", "query", "message pair")
- **Timeline** (not "history", "transcript", "log")
- **Project** (not "workspace", "repository", "folder")
- **Runtime** (not "backend", "server", "engine")
- **Approval** (not "permission", "confirmation", "authorization")

### Avoid These Terms

- **Thread**: Ambiguous with OS threads and provider-specific terminology
- **Conversation**: Too informal, doesn't convey durability
- **Workspace**: Reserved for filesystem scope, not project
- **Backend**: Too generic, doesn't convey local execution
- **Agent**: Use sparingly, prefer specific concepts (Runtime, Session, Turn)

### Context-Specific Usage

Some terms have different meanings in different contexts:

- **Workspace**: Filesystem scope within a project (not the project itself)
- **Profile**: Can be model, connection, or permission (always qualify)
- **Task**: User-facing work unit (not OS task or background job)
- **Runtime**: The execution environment (not "runtime" as in "at runtime")

## Product Name

**Current Status**: To be determined

**Candidates Under Consideration:**
- Aegisy Coding
- Aegisy Workbench
- Aegisy Agent
- Aegisy Code
- [Other names under product review]

**Requirements:**
- Distinct from "Aegisy" (the platform/account service)
- Distinct from third-party products (Codex, Claude Code, etc.)
- Conveys coding/development focus
- Works in UI, marketing, and technical contexts
- Available as domain/trademark

**Decision Owner**: Product team  
**Target Date**: Before public beta

## Revision History

- 2026-07-31: Initial draft created

## Approval

This terminology requires approval from:

- [ ] Product Owner - terms align with product vision
- [ ] UX Lead - terms are clear and consistent
- [ ] Engineering Lead - terms map to technical concepts
- [ ] Documentation Lead - terms work across all content
