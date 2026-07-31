# Aegisy Coding Workbench - Development Status

**Last Updated**: 2026-07-31  
**OpenSpec Progress**: 57/235 tasks completed (24.3%)

## Overview

Aegisy Coding Workbench is evolving from a desktop configuration client into a
native cross-platform coding agent workbench with Chat and Work modes, project/
session management, Monaco editor, terminal, Git integration, and Agent capabilities.

## Current Status by Section

| Section | Progress | Status |
|---------|----------|--------|
| 1. Product Baseline and Decision Gates | 5/8 (62%) | 🟡 In Progress |
| 2. Milestone 0 UI Technology Spike | 0/9 (0%) | ⚪ Not Started |
| 3. Aegisy Agent Protocol Foundation | 7/12 (58%) | 🟡 In Progress |
| 4. Runtime Sidecar and Authenticated IPC | 2/10 (20%) | 🟡 In Progress |
| 5. Event Store, Database, and Recovery | 8/10 (80%) | 🟢 Nearly Complete |
| 6. Project and Session Management | 2/10 (20%) | 🟡 In Progress |
| 7. Codex App Server Runtime Adapter | 2/12 (17%) | 🟡 In Progress |
| 8. ACP Runtime Adapter | 0/8 (0%) | ⚪ Deferred |
| 9. Aegisy Model Catalog and Cloud Contracts | 0/10 (0%) | ⚪ Not Started |
| 10. Model Profiles, Routing, and Switching | 0/12 (0%) | ⚪ Not Started |
| 11. Workbench Host and Navigation | 0/9 (0%) | ⚪ Not Started |
| 12. Agent Timeline and Composer | 0/10 (0%) | ⚪ Not Started |
| 13. Files, Editor, Search, and Diagnostics | 10/10 (100%) | ✅ Complete |
| 14. Terminal and Process Execution | 5/9 (56%) | 🟡 In Progress |
| 15. Structured Edits, Diffs, and Checkpoints | 9/9 (100%) | ✅ Complete |
| 16. Git and Worktree Workflows | 2/12 (17%) | 🟡 In Progress |
| 17. Context Engine and Compaction | 0/10 (0%) | ⚪ Not Started |
| 18. Permission, Sandbox, and Secret Enforcement | 0/12 (0%) | ⚪ Not Started |
| 19. Skills, Plugins, MCP, Hooks, and Instructions | 0/10 (0%) | ⚪ Not Started |
| 20. Observability, Diagnostics, and Evaluation | 0/11 (0%) | ⚪ Not Started |
| 21. Background Jobs and Multi-Agent Milestone | 1/12 (8%) | 🟡 In Progress |
| 22. Migration, Packaging, and Release | 0/10 (0%) | ⚪ Not Started |

## Completed Sections

### Section 13: Files, Editor, Search, and Diagnostics (100%)
- ✅ Monaco editor integration with WebEngine/WebChannel
- ✅ File tree, search, and navigation
- ✅ Diagnostics and error display
- ✅ Diff view and preview capabilities
- ✅ Tested fallback to QPlainTextEdit

### Section 15: Structured Edits, Diffs, and Checkpoints (100%)
- ✅ Workspace edit proposals with validation
- ✅ Diff generation and preview
- ✅ Checkpoint creation and review
- ✅ Durable storage with SQLite
- ✅ Restart recovery

## In Progress Sections

### Section 1: Product Baseline (62%)
- ✅ Architecture Decision Records (ADRs) created
- ✅ Third-party component inventory
- ✅ Performance budgets defined
- ✅ Platform matrix documented
- ✅ Feature channel policy established
- 🟡 Chat/Work behavioral contract (draft, awaiting approval)
- 🟡 Terminology definition (draft, awaiting approval)
- 🟡 Legal review (checklist created, awaiting approval)

### Section 3: AAP Foundation (58%)
- ✅ Schema package with stable/experimental namespaces
- ✅ Core domain schemas (Project, Session, Turn, Timeline, etc.)
- ✅ Initialize/initialized handshake
- ✅ Event sequence and correlation
- ✅ Error classification
- ✅ Schema compatibility tests
- ✅ Protocol guide documentation
- 🟡 Snapshot/replay/subscription (partial implementation)
- 🟡 Idempotency semantics (partial foundation)
- 🟡 Type generation (Rust/TypeScript/C++ partial, awaiting Windows validation)

### Section 5: Event Store (80%)
- ✅ SQLite schema with projects, sessions, turns, items
- ✅ Append-only event persistence
- ✅ Content-addressed storage for artifacts
- ✅ Rebuildable session projections
- ✅ WAL configuration and compaction
- ✅ Versioned migrations with backup
- ✅ Retention and garbage collection
- ✅ Portable session export/import
- ✅ Credential redaction enforcement
- 🟡 Complete Windows evidence pending

## Key Capabilities

### Implemented
- **Chat Mode**: Non-mutating exploration and planning
- **Work Mode**: Project-bound execution (read-only Agent currently)
- **Project Management**: Filesystem-backed identity, multi-root support
- **Session Management**: Durable conversations with event streams
- **Monaco Editor**: Trusted local bundle with WebEngine isolation
- **Terminal**: macOS PTY and Windows ConPTY (Windows pending validation)
- **LSP Integration**: Sidecar-owned language servers with bounded stdio
- **Git Integration**: Read-only status and history
- **Workspace Edits**: Structured proposals with validation
- **Checkpoints**: Session compaction with review workflow
- **Updates**: Sparkle (macOS) and WinSparkle (Windows)

### In Development
- **Agent Mutations**: File writes, Git operations, command execution
- **Approvals**: User decision workflow for proposed actions
- **Background Jobs**: Durable scheduling and execution
- **Multi-Agent**: Parallel agent coordination
- **Model Routing**: Provider selection and switching
- **MCP Integration**: External tool and resource servers
- **Cloud Integration**: Aegisy model catalog and account services

### Deferred
- **ACP Adapter**: Awaiting protocol maturity and security review
- **Remote Execution**: Separate security gate required
- **Local Models**: Pending cloud model maturity

## Security Boundaries

### Current Enforcement
- ✅ Chat mode is non-mutating by default
- ✅ Work mode requires explicit project binding
- ✅ Sidecar process isolation (Rust `aegisy-agentd`)
- ✅ Authenticated IPC (macOS Unix socket, Windows named pipe in progress)
- ✅ Canonical path validation and symlink denial
- ✅ Sensitive path blocking (credentials, SSH keys, etc.)
- ✅ Secret redaction in events and logs
- ✅ Credential storage in platform secure storage
- ✅ Read-only Agent operations (writes require approval)

### Planned Enforcement
- 🟡 Permission profiles (Chat, Read Only, Workspace Write, Developer, Full Access)
- 🟡 Approval workflow for destructive operations
- 🟡 Sandbox enforcement (macOS/Windows native or Codex adapter)
- 🟡 Network policy and host approval
- 🟡 Extension trust and validation
- 🟡 Audit trail for all mutations

## Platform Support

### macOS
- ✅ arm64 builds tested and working
- ✅ Unix socket transport with peer verification
- ✅ PTY terminal implementation
- ✅ Sparkle updates
- ✅ Keychain credential storage
- 🟡 Intel builds (compilation only, no runtime evidence)
- 🟡 Signed/notarized packages pending

### Windows
- ✅ x64 compilation successful
- ✅ Named pipe transport implemented
- ✅ ConPTY terminal implementation
- ✅ WinSparkle updates
- ✅ DPAPI credential storage
- 🟡 Complete runtime validation pending
- 🟡 Signed packages pending

## Documentation

### Completed
- ✅ Chat/Work Behavioral Contract
- ✅ Aegisy Coding Terminology
- ✅ Legal Review Checklist
- ✅ Third-Party Component Inventory
- ✅ Performance Budgets
- ✅ Platform Matrix
- ✅ Feature Channel Policy
- ✅ AAP Protocol Guide
- ✅ ADRs for architecture decisions

### In Progress (21 agents working)
- 🟡 Permission System Design
- 🟡 Idempotency Design
- 🟡 Background Execution Design
- 🟡 Model Routing Design
- 🟡 MCP Integration Design
- 🟡 Git Workflow Design
- 🟡 Context Engine Design
- 🟡 Packaging and Release Design
- 🟡 Observability Design
- 🟡 Cloud Contracts Design
- 🟡 Timeline UI Design
- 🟡 Workbench Navigation Design
- 🟡 Editor and Files Design
- 🟡 ACP Adapter Design
- 🟡 Additional ADRs (Windows sandbox, content retention, local models, model catalog trust)

## Next Milestones

### Milestone 0: UI Technology Spike
- Isolated Qt WebEngine build experiment
- Local workbench bundle rendering
- QWebChannel bridge with security
- Monaco and xterm.js integration
- Performance measurements
- Go/no-go decision for embedded WebEngine

### Milestone 1: Read-Only Agent
- Complete AAP protocol implementation
- Codex adapter with read-only operations
- Project and session UI
- Timeline rendering
- Basic approval UI (deny only)
- macOS and Windows validation

### Milestone 2: Workspace Mutations
- File write approvals and execution
- Git operation approvals
- Command execution with sandbox
- Permission profiles
- Audit trail
- Security review

### Milestone 3: Background and Multi-Agent
- Background job scheduling
- Multi-agent coordination
- Quality gates and safety checks
- Comprehensive testing
- Production readiness review

## Development Commands

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Test
cd build && ctest --output-on-failure

# Check OpenSpec progress
./scripts/openspec-status.sh

# Run application
./build/Aegisy
```

## Contributing

See `openspec/changes/build-aegisy-agent-workbench/` for detailed specifications,
design documents, and task tracking.

## License

See `docs/THIRD-PARTY-COMPONENT-INVENTORY.md` for component licenses and
`docs/LEGAL-REVIEW-CHECKLIST.md` for compliance requirements.
