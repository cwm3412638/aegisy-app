# Aegisy Agent Workbench - Architecture

## Overview

Aegisy Agent Workbench is an AI-powered development environment built on a secure, multi-process architecture. This document describes the system design, component interactions, data flow, and key architectural decisions.

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Desktop Host (Qt)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │   Workbench  │  │    Legacy    │  │   Settings   │     │
│  │      UI      │  │  Chat Client │  │   & Prefs    │     │
│  └──────┬───────┘  └──────────────┘  └──────────────┘     │
│         │                                                    │
│         │ AAP/IPC                                           │
│         ▼                                                    │
│  ┌──────────────────────────────────────────────────────┐  │
│  │         Agent Runtime Client (Qt)                    │  │
│  │  - Request/Response handling                         │  │
│  │  - Event subscription                                │  │
│  │  - Connection management                             │  │
│  └──────────────┬───────────────────────────────────────┘  │
└─────────────────┼───────────────────────────────────────────┘
                  │
                  │ Unix Socket (macOS) / Named Pipe (Windows)
                  │ Authenticated, Encrypted
                  ▼
┌─────────────────────────────────────────────────────────────┐
│              Agent Runtime Sidecar (Rust)                   │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  AAP Server (Aegisy Agent Protocol)                  │  │
│  │  - Request routing                                   │  │
│  │  - Event streaming                                   │  │
│  │  - Capability negotiation                            │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────┴───────────────────────────────────────┐  │
│  │  Core Services                                       │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │  │
│  │  │  Project   │  │  Session   │  │  Timeline  │    │  │
│  │  │  Manager   │  │  Manager   │  │   Store    │    │  │
│  │  └────────────┘  └────────────┘  └────────────┘    │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │  │
│  │  │ Workspace  │  │  Terminal  │  │    Git     │    │  │
│  │  │  Manager   │  │  Manager   │  │  Manager   │    │  │
│  │  └────────────┘  └────────────┘  └────────────┘    │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────┴───────────────────────────────────────┐  │
│  │  Runtime Adapters                                    │  │
│  │  ┌────────────────┐  ┌────────────────┐            │  │
│  │  │  Codex Adapter │  │   ACP Adapter  │            │  │
│  │  │  (App Server)  │  │   (Claude)     │            │  │
│  │  └────────────────┘  └────────────────┘            │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────┴───────────────────────────────────────┐  │
│  │  Storage Layer                                       │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │  │
│  │  │  SQLite    │  │   Cache    │  │   Secrets  │    │  │
│  │  │  (Events)  │  │   Store    │  │   Store    │    │  │
│  │  └────────────┘  └────────────┘  └────────────┘    │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                  │
                  │ stdio / Process Management
                  ▼
┌─────────────────────────────────────────────────────────────┐
│              External Runtimes                              │
│  ┌────────────────┐  ┌────────────────┐                    │
│  │ Codex App      │  │  ACP Agents    │                    │
│  │ Server         │  │  (Kimi, etc)   │                    │
│  └────────────────┘  └────────────────┘                    │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. Desktop Host (Qt/C++)

The desktop host provides the user interface and manages the application lifecycle.

**Responsibilities:**
- Render UI (Workbench, Legacy Chat, Settings)
- Handle user input and interactions
- Manage application windows and menus
- Supervise sidecar process lifecycle
- Maintain UI state and preferences

**Key Classes:**
- `AgentWorkbenchWidget`: Main workbench UI
- `AgentRuntimeClient`: AAP client implementation
- `FeatureFlags`: Feature flag management
- `ProfileManager`: User profile management

**Technologies:**
- Qt 6.5+ (Widgets, WebEngine, Network)
- C++17
- CMake build system

### 2. Agent Runtime Sidecar (Rust)

The sidecar is a separate process that handles all business logic, storage, and external integrations.

**Responsibilities:**
- Implement AAP server
- Manage projects and sessions
- Store events and state (SQLite)
- Coordinate with external runtimes
- Enforce security policies
- Handle filesystem operations
- Manage terminals and processes
- Coordinate Git operations

**Key Modules:**
- `aap_server`: AAP protocol implementation
- `project_manager`: Project lifecycle
- `session_manager`: Session lifecycle
- `timeline_store`: Event storage
- `workspace_manager`: File operations
- `terminal_manager`: PTY/ConPTY management
- `git_manager`: Git operations
- `codex_adapter`: Codex App Server integration
- `acp_adapter`: ACP agent integration

**Technologies:**
- Rust 2021 edition
- Tokio async runtime
- SQLite (rusqlite)
- portable-pty (terminal)
- git2 (libgit2 bindings)

### 3. Runtime Adapters

Adapters translate between AAP and external runtime protocols.

#### Codex Adapter
- Manages Codex App Server process
- Translates AAP ↔ Codex protocol
- Handles file operations, Git, terminal
- Supports command execution and diagnostics

#### ACP Adapter
- Manages ACP agent processes (Kimi, etc)
- Translates AAP ↔ ACP protocol
- Provides broader model support
- Different capability set than Codex

### 4. Storage Layer

#### Event Store (SQLite)
- Stores timeline events
- Supports snapshot and replay
- Handles compaction and retention
- Provides indexed queries

**Schema:**
- Projects: Project metadata and roots
- Sessions: Session lifecycle and bindings
- Timeline: Ordered event sequence
- Items: Turn items (messages, commands, edits)
- Proposals: Workspace edit proposals
- Checkpoints: Git-aware snapshots

#### Cache Store
- Model catalog cache
- Model profiles
- Runtime state
- Separate from main event store

#### Secret Store
- Encrypted credential storage
- Platform keychain integration
- Never logged or transmitted

## Data Flow

### Turn Execution Flow

```
1. User Input (Qt)
   │
   ▼
2. AgentRuntimeClient.startTurn()
   │
   ▼
3. AAP Request: turn/start
   │ (Unix Socket / Named Pipe)
   ▼
4. Sidecar: AAP Server
   │
   ▼
5. Session Manager
   │ - Validate session
   │ - Check permissions
   │ - Prepare context
   ▼
6. Runtime Adapter (Codex/ACP)
   │ - Translate to runtime protocol
   │ - Send to external runtime
   ▼
7. External Runtime (Codex/ACP Agent)
   │ - Process request
   │ - Generate response
   │ - Stream events
   ▼
8. Runtime Adapter
   │ - Translate events to AAP
   │ - Stream to sidecar
   ▼
9. Timeline Store
   │ - Persist events
   │ - Update state
   ▼
10. AAP Events: timeline/event
    │ (Unix Socket / Named Pipe)
    ▼
11. AgentRuntimeClient
    │ - Receive events
    │ - Update UI state
    ▼
12. Workbench UI
    │ - Render timeline
    │ - Show responses
    │ - Display proposals
```

### Workspace Edit Flow

```
1. Agent proposes edit
   │
   ▼
2. Workspace Manager
   │ - Validate edit
   │ - Check permissions
   │ - Generate preview
   ▼
3. Preview Store
   │ - Compute diffs
   │ - Check sensitive paths
   │ - Store preview
   ▼
4. AAP Event: workspace-edit-proposal
   │
   ▼
5. Qt UI
   │ - Show proposal dialog
   │ - Display diffs
   │ - Request user approval
   ▼
6. User Decision
   │
   ▼
7. AAP Request: workspace/edit/apply (if approved)
   │
   ▼
8. Workspace Manager
   │ - Create checkpoint
   │ - Apply changes atomically
   │ - Verify final hashes
   │ - Handle rollback if needed
   ▼
9. Timeline Event: workspace-edit-applied
   │
   ▼
10. Qt UI
    │ - Update file tree
    │ - Refresh editor
    │ - Show confirmation
```

## Communication Protocols

### AAP (Aegisy Agent Protocol)

JSON-RPC 2.0 based protocol for host ↔ sidecar communication.

**Transport:**
- macOS: Unix domain socket (owner-only permissions)
- Windows: Named pipe (current-user ACL)

**Security:**
- Peer validation (PID, UID)
- One-time bootstrap authentication
- Bounded message sizes
- Heartbeat and reconnection

**Message Types:**
- Requests: Client → Server (with response)
- Notifications: Server → Client (no response)
- Events: Streaming updates

**Key Methods:**
- `runtime/initialize`: Handshake and capability negotiation
- `project/open`: Open project
- `session/create`: Create session
- `turn/start`: Start model turn
- `turn/cancel`: Cancel active turn
- `workspace/edit/preview`: Preview changes
- `workspace/edit/apply`: Apply changes
- `terminal/open`: Open terminal
- `git/status`: Get Git status

### Codex Protocol

Codex App Server uses JSON-RPC over stdio.

**Key Methods:**
- `initialize`: Initialize app server
- `thread/start`: Start conversation thread
- `thread/sendMessage`: Send message
- `thread/interrupt`: Cancel turn
- Notifications: `commandExecution`, `fileChange`, etc.

### ACP (Anthropic Claude Protocol)

ACP agents use a different protocol structure.

**Key Methods:**
- Session management
- Message exchange
- Tool invocation
- Event streaming

## Security Architecture

### Process Isolation

- **Desktop Host**: User-facing, minimal privileges
- **Sidecar**: Business logic, controlled privileges
- **External Runtimes**: Sandboxed, monitored

### Permission Model

**Permission Profiles:**
1. **Read Only**: Browse and search only
2. **Workspace Write**: File modifications
3. **Developer**: Terminal and Git access
4. **Full Access**: Network and system operations

**Enforcement:**
- Sidecar validates all operations
- Permissions checked before execution
- Approval required for mutations
- Audit trail for all actions

### Secret Protection

- Secrets never logged
- Automatic detection and masking
- Platform keychain integration
- Encrypted at rest

### Sandbox Boundaries

- Filesystem: Limited to project roots
- Process: Controlled spawning
- Network: Restricted by permission
- Git: Repository-scoped operations

## Data Storage

### Event Store Schema

**Projects Table:**
```sql
CREATE TABLE projects (
    id TEXT PRIMARY KEY,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    name TEXT NOT NULL,
    roots JSON NOT NULL,
    trust_state JSON NOT NULL
);
```

**Sessions Table:**
```sql
CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    mode TEXT NOT NULL,  -- 'chat' or 'work'
    runtime_binding JSON,
    model_binding JSON,
    permission_profile TEXT,
    state TEXT NOT NULL,  -- 'active', 'archived', etc.
    FOREIGN KEY (project_id) REFERENCES projects(id)
);
```

**Timeline Table:**
```sql
CREATE TABLE timeline (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    event_id TEXT NOT NULL UNIQUE,
    timestamp_ms INTEGER NOT NULL,
    event_type TEXT NOT NULL,
    event_data JSON NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);
```

**Items Table:**
```sql
CREATE TABLE items (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    turn_id TEXT,
    item_type TEXT NOT NULL,  -- 'message', 'command', 'edit', etc.
    item_role TEXT,  -- 'user', 'assistant', 'system'
    item_state TEXT NOT NULL,  -- 'pending', 'complete', 'failed'
    item_data JSON NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);
```

### Retention and Compaction

- **Retention Policies**: Per-project and per-session
- **Compaction**: Summarize old events
- **Checkpoints**: Preserve important states
- **Pruning**: Remove old data per policy

## Key Architectural Decisions

### ADR-001: Multi-Process Architecture

**Decision**: Separate desktop host (Qt) and business logic (Rust sidecar)

**Rationale:**
- Security: Isolate UI from sensitive operations
- Reliability: Sidecar crash doesn't kill UI
- Performance: Async Rust for I/O-heavy operations
- Maintainability: Clear separation of concerns

**Trade-offs:**
- Complexity: IPC overhead and coordination
- Latency: Cross-process communication
- Debugging: Multi-process debugging required

### ADR-002: Event Sourcing for Timeline

**Decision**: Store timeline as ordered events, not mutable state

**Rationale:**
- Auditability: Complete history of all actions
- Replay: Reconstruct state from events
- Debugging: Understand what happened
- Compaction: Summarize old events

**Trade-offs:**
- Storage: More data than state snapshots
- Complexity: Event replay logic
- Performance: Query performance considerations

### ADR-003: Approval-Based Mutations

**Decision**: Require explicit approval for workspace changes

**Rationale:**
- Safety: User controls all mutations
- Trust: Build confidence gradually
- Transparency: User sees all changes
- Reversibility: Checkpoints enable undo

**Trade-offs:**
- Friction: More user interaction required
- Speed: Slower than automatic application
- UX: Approval dialogs can interrupt flow

### ADR-004: Git-Aware Checkpoints

**Decision**: Create Git commits for checkpoints, not custom snapshots

**Rationale:**
- Familiarity: Users understand Git
- Tooling: Existing Git tools work
- Integration: Works with existing workflows
- Portability: Standard Git format

**Trade-offs:**
- Complexity: Git plumbing is complex
- Limitations: Requires Git repository
- Performance: Git operations can be slow

### ADR-005: Runtime Adapters

**Decision**: Support multiple runtimes (Codex, ACP) via adapters

**Rationale:**
- Flexibility: Support different AI providers
- Evolution: Add new runtimes without core changes
- Compatibility: Different models have different capabilities
- Migration: Users can switch runtimes

**Trade-offs:**
- Complexity: Adapter layer adds indirection
- Compatibility: Feature parity challenges
- Testing: Must test all adapters

## Performance Considerations

### Startup Performance

**Targets (Milestone 0):**
- Cold start: < 3 seconds
- Warm start: < 1 second
- Sidecar handshake: < 500ms

**Optimizations:**
- Lazy loading of UI components
- Parallel sidecar startup
- Cached capability negotiation
- Deferred project loading

### Runtime Performance

**Targets:**
- Editor latency: < 50ms
- Terminal echo: < 16ms
- File tree refresh: < 200ms
- Git status: < 500ms

**Optimizations:**
- Incremental file tree updates
- Debounced Git status checks
- Cached directory listings
- Async I/O throughout

### Memory Usage

**Targets:**
- Idle: < 200 MB
- Active: < 500 MB
- Peak: < 1 GB

**Optimizations:**
- Bounded event buffers
- Streaming large content
- Eviction policies for caches
- Explicit resource cleanup

## Testing Strategy

### Unit Tests

- Rust: `cargo test`
- Qt: QTest framework
- Coverage target: > 80%

### Integration Tests

- AAP protocol tests
- Runtime adapter tests
- Storage layer tests
- Git operation tests

### End-to-End Tests

- UI automation (Qt Test)
- Real sidecar + runtime
- Actual file operations
- Complete workflows

### Security Tests

- Permission enforcement
- Secret detection
- Sandbox escape attempts
- Malicious input handling

## Deployment

### Packaging

**macOS:**
- `.app` bundle
- Code signed
- Notarized
- DMG installer

**Windows:**
- Inno Setup installer
- Code signed
- MSI option
- Portable option

**Linux:**
- AppImage
- Debian package
- RPM package
- Flatpak

### Updates

- Sparkle (macOS)
- WinSparkle (Windows)
- Delta updates
- Rollback support

### Telemetry

- Opt-in only
- Privacy-preserving
- Crash reports
- Performance metrics

## Future Architecture

### Planned Enhancements

1. **Remote Execution**: Cloud-based agent execution
2. **Multi-Agent**: Parallel agent workflows
3. **Plugin System**: Native and script plugins
4. **Language Servers**: Enhanced code intelligence
5. **Collaborative**: Multi-user sessions

### Scalability

- Support for large repositories (> 100k files)
- Long-running sessions (> 1000 turns)
- Multiple concurrent projects
- Background job execution

## References

- [AAP Protocol Guide](AAP-PROTOCOL-GUIDE.md)
- [AAP Adapter Contributor Guide](AAP-ADAPTER-CONTRIBUTOR-GUIDE.md)
- [Security Documentation](SECURITY-DOCUMENTATION.md)
- [Contributing Guide](../CONTRIBUTING.md)
- [Architecture Decision Records](adr/README.md)
