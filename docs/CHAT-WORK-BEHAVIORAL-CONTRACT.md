# Chat versus Work Behavioral Contract

**Status**: Draft for Product and Security Review  
**Created**: 2026-07-31  
**Owners**: Product, Security, Engineering

## Purpose

This document defines the behavioral contract and mutation guarantees that
distinguish Chat mode from Work mode in Aegisy Coding. The contract establishes
clear boundaries for what operations are permitted in each mode, ensuring users
understand the safety and capability differences.

## Core Principles

1. **Chat is non-mutating by default** - protects against accidental changes
2. **Work requires explicit project binding** - ensures intentional workspace access
3. **Mode transitions are explicit** - users consciously choose to enable mutations
4. **Permissions are mode-scoped** - different authority levels per mode

## Chat Mode

### Behavioral Contract

Chat mode is designed for safe exploration, planning, and discussion without
modifying the user's workspace or system state.

**Permitted Operations:**
- Read explicitly shared context (files, URLs, documentation)
- Search and analyze code structure
- Answer questions about code, APIs, and best practices
- Generate code examples and suggestions
- Propose tasks and implementation plans
- Create session history and conversation state

**Prohibited Operations:**
- Write or modify workspace files
- Execute shell commands
- Modify Git state (commit, branch, merge, rebase)
- Install dependencies or modify project configuration
- Access or modify system files outside shared context
- Create or modify background tasks

### Mutation Guarantees

**Guarantee 1: Filesystem Immutability**
- Chat mode SHALL NOT write, delete, or modify any files in the user's workspace
- Chat mode SHALL NOT create new files or directories
- File reads require explicit user sharing or approval

**Guarantee 2: Process Isolation**
- Chat mode SHALL NOT spawn shell processes
- Chat mode SHALL NOT execute commands in terminals
- Chat mode SHALL NOT modify environment variables

**Guarantee 3: Git Immutability**
- Chat mode SHALL NOT modify Git repository state
- Chat mode MAY read Git status and history (read-only)
- Chat mode SHALL NOT create commits, branches, or tags

**Guarantee 4: Network Restrictions**
- Chat mode SHALL NOT make arbitrary network requests
- Chat mode MAY access explicitly approved URLs for context
- Chat mode SHALL NOT install packages or dependencies

### Conversion to Work

Users can convert a Chat session to Work mode through:
1. Explicit "Convert to Work" action in the UI
2. Binding the session to a specific project root
3. Accepting the Work mode permission profile

Conversion is one-way and requires user confirmation.

## Work Mode

### Behavioral Contract

Work mode enables full Agent capabilities within a bounded project context,
with explicit permission profiles and approval workflows.

**Permitted Operations:**
- All Chat mode read operations
- Write and modify files within the project root
- Execute approved shell commands
- Modify Git state (with approval)
- Install dependencies (with approval)
- Create and manage terminals
- Apply structured workspace edits
- Execute background tasks (when enabled)

**Bounded Operations:**
- Filesystem access limited to canonical project roots
- Commands execute in project context with scrubbed environment
- Network access requires host and purpose approval
- Git operations require explicit approval for destructive actions
- Background execution requires separate permission grant

### Mutation Guarantees

**Guarantee 1: Bounded Filesystem Access**
- Work mode SHALL only access files within registered project roots
- Work mode SHALL NOT access parent directories or symlink escapes
- Work mode SHALL deny sensitive paths (credentials, SSH keys, browser profiles)
- Work mode SHALL validate canonical paths before all operations

**Guarantee 2: Approval-Gated Mutations**
- Destructive operations require explicit user approval
- Approvals are scoped (once, turn, session, or durable rule)
- Users see command, cwd, environment, paths, and risk level before approval
- Changed tool definitions invalidate prior approval hashes

**Guarantee 3: Git Safety**
- Git operations preserve user's working state
- Destructive Git operations (force push, rebase, reset --hard) require approval
- Git state is validated before and after operations
- Failed operations preserve original state

**Guarantee 4: Process Supervision**
- Shell processes are runtime-owned and supervised
- Processes execute with scrubbed minimal environment
- Process tree termination on session close or runtime shutdown
- Terminal output is bounded and captured

**Guarantee 5: Durable State**
- All mutations are recorded in durable event stream
- Failed operations are distinguishable from successful ones
- Interrupted operations can be reconciled on restart
- Session state survives runtime crashes

### Permission Profiles

Work mode uses one of these permission profiles:

1. **Read Only**: Browse project, no mutations
2. **Workspace Write**: File edits only, no commands or Git
3. **Developer**: File edits + approved commands + Git operations
4. **Full Access**: All operations with approval workflow

Each profile declares:
- Filesystem roots and denied paths
- Network policy (hosts, purposes)
- Command policy (allowed/denied patterns)
- MCP/extension access
- Background execution permission

## Mode Comparison Matrix

| Capability | Chat | Work (Read Only) | Work (Workspace Write) | Work (Developer) | Work (Full Access) |
|------------|------|------------------|------------------------|------------------|--------------------|
| Read files | Shared only | Project root | Project root | Project root | Project root |
| Write files | ❌ | ❌ | ✅ (approved) | ✅ (approved) | ✅ (approved) |
| Execute commands | ❌ | ❌ | ❌ | ✅ (approved) | ✅ (approved) |
| Git operations | ❌ | ❌ | ❌ | ✅ (approved) | ✅ (approved) |
| Terminal access | ❌ | ❌ | ❌ | ✅ | ✅ |
| Network requests | Limited | Limited | Limited | ✅ (approved) | ✅ (approved) |
| Background tasks | ❌ | ❌ | ❌ | ❌ | ✅ (approved) |
| MCP/Extensions | ❌ | ❌ | Limited | ✅ (approved) | ✅ (approved) |

## Security Boundaries

Both modes enforce these security boundaries:

1. **Untrusted Input**: Source files, tool output, websites, MCP responses treated as data
2. **Credential Protection**: Deny access to credential stores, SSH keys, cloud credentials
3. **Secret Redaction**: Known secrets redacted from UI, logs, diagnostics
4. **Network Safety**: Host and purpose approval, no silent credential forwarding
5. **Signed Updates**: Verify sidecar/runtime adapter hash
6. **Audit Trail**: Content-minimal events (actor, operation, scope, decision, result)

## Implementation Requirements

### Runtime Enforcement

- Mode state is session-scoped and durable
- Permission checks occur at the Runtime boundary before execution
- Failed permission checks return stable error codes
- Mode cannot be changed while operations are in progress

### UI Requirements

- Current mode is always visible in the UI
- Mode-specific capabilities are clearly indicated
- Conversion from Chat to Work requires explicit confirmation
- Approval dialogs show complete operation context

### Testing Requirements

- Automated tests verify mode boundaries
- Permission denial tests for each prohibited operation
- Mode transition tests verify state preservation
- Cross-mode operation tests verify isolation

## Open Questions

1. **Q**: Should Chat mode allow read-only terminal viewing?
   **A**: Deferred - terminals are Work-only in Milestone 0

2. **Q**: Can Work sessions be downgraded to Read Only?
   **A**: Yes, but requires explicit user action and blocks pending mutations

3. **Q**: How are shared files in Chat mode specified?
   **A**: Through explicit file picker or drag-drop, not automatic workspace scanning

4. **Q**: Can Chat mode access MCP servers?
   **A**: Only MCP servers explicitly marked as Chat-safe with read-only capabilities

## Approval Process

This contract requires approval from:

- [ ] Product Owner - behavioral contract aligns with product vision
- [ ] Security Owner - mutation guarantees meet security requirements
- [ ] Engineering Lead - implementation is feasible and testable
- [ ] UX Lead - mode distinction is clear to users

## Revision History

- 2026-07-31: Initial draft created
