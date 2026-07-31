# Permission System Design

## Overview

The Aegisy Agent Workbench permission system enforces security boundaries between user intent, Agent execution, and system resources. It provides granular control over filesystem access, command execution, network operations, and extension capabilities through permission profiles, approval workflows, and runtime enforcement.

## Permission Profiles

Five permission profiles define the security posture for Agent sessions:

### Chat
- **Filesystem**: No write access; read access limited to explicitly shared context
- **Commands**: None
- **Network**: None (except model API calls)
- **Extensions**: Read-only Skills and instructions only
- **Background**: Not allowed
- **Use case**: Safe exploration and planning without system modification

### Read Only
- **Filesystem**: Read access to project roots; no writes
- **Commands**: None
- **Network**: None (except model API calls)
- **Extensions**: Read-only Skills, instructions, and MCP resources
- **Background**: Not allowed
- **Use case**: Code analysis, search, and inspection without mutation risk

### Workspace Write
- **Filesystem**: Read/write within project roots; denied paths enforced
- **Commands**: Allowlisted safe commands (e.g., `git status`, `npm test`)
- **Network**: Allowlisted hosts for package registries
- **Extensions**: Skills, MCP tools with declared permissions
- **Background**: Not allowed
- **Use case**: Standard coding tasks with bounded mutation scope

### Developer
- **Filesystem**: Read/write within project roots; denied paths enforced
- **Commands**: Broad command access with approval; shell wrappers denied
- **Network**: Broad network access with approval
- **Extensions**: Skills, MCP, plugins with approval
- **Background**: Allowed with approval
- **Use case**: Full-featured development with approval gates

### Full Access
- **Filesystem**: Read/write within project roots; denied paths enforced
- **Commands**: Unrestricted commands with approval
- **Network**: Unrestricted network with approval
- **Extensions**: All extensions with approval
- **Background**: Allowed with approval
- **Use case**: Advanced workflows requiring maximum flexibility
- **Note**: Still subject to managed policy, sensitive path denial, and symlink restrictions

## Approval Workflow

### Decision Types

Approval requests present context and require one of five decisions:

1. **Deny**: Reject the operation; Agent receives failure
2. **Allow Once**: Approve this exact operation for this request only
3. **Allow for Turn**: Approve similar operations until the current turn completes
4. **Allow for Session**: Approve similar operations for the session lifetime
5. **Durable Rule**: Create a hash-bound rule that persists across sessions

### Approval Context

Each approval request includes:
- **Operation type**: Command, file write, network request, Git mutation, extension call
- **Risk level**: Low, medium, high (see Risk Classification)
- **Command/path/host**: Exact operation details
- **Working directory**: Execution context
- **Environment**: Relevant variables (credentials redacted)
- **Reason**: Agent's stated purpose
- **Resulting diff**: Preview of changes when available

### Durable Rules

Durable approval rules:
- Are scoped to specific operations (command pattern, file glob, host, extension ID)
- Include a hash of the command/extension definition
- Become untrusted when the hashed content changes
- Are editable and revocable through the UI
- Never apply to high-risk operations in initial releases

### Risk Classification

**Low Risk:**
- Read-only filesystem operations within project roots
- Creating/switching branches in clean repositories
- Staging Agent-owned changes when policy allows
- Allowlisted safe commands (status, test, build)

**Medium Risk:**
- Writing files within project roots
- Git commit, stash, merge, rebase, cherry-pick, push
- Network requests to allowlisted hosts
- MCP tool calls with bounded scope

**High Risk:**
- Git force push, reset, clean, branch deletion, history rewriting
- Discarding user changes
- Commands with shell wrappers or redirection
- Network requests to arbitrary hosts
- Filesystem operations outside project roots
- Extension installation or execution

## Security Boundaries

### Untrusted Data Sources

The following are treated as untrusted and never grant authority:
- Source code files
- Issue and pull request text
- Tool output and command results
- Website content
- MCP server responses
- Remote messages
- Repository instructions (until explicitly trusted)

### Sensitive Path Denial

The following paths are denied by default unless explicitly granted:
- Credential stores (Keychain, Windows Credential Manager)
- SSH keys (`~/.ssh/`)
- Browser profiles and cookies
- Cloud provider credentials (`~/.aws/`, `~/.config/gcloud/`)
- `.env` files and configured secret globs
- System directories outside project roots

### Secret Handling

Secrets are detected and redacted at multiple boundaries:
- **Tool output**: Before storage and display
- **Event persistence**: Before database writes
- **Logs and diagnostics**: Before file writes
- **UI rendering**: Before display to user
- **Model requests**: Before sending to provider

Redaction preserves a local indication that redaction occurred without exposing the secret value.

### Network Policy

Network operations are controlled by:
- **Host allowlists**: Per-profile permitted hosts
- **Purpose declaration**: Package registry, API, webhook
- **Credential isolation**: No silent forwarding across redirects
- **Tool-provided URL denial**: Agent cannot specify arbitrary URLs without approval

### Extension Security

Extensions (Skills, MCP servers, plugins) are subject to:
- **Declared permissions**: Filesystem, network, command scope
- **Origin and version trust**: Signature verification when available
- **Content hashing**: Executable hooks require hash-bound approval
- **Scope isolation**: Project, session, or child-task boundaries
- **Trust revocation**: Changed code returns to untrusted state

## Sandbox Implementation

### Requirements

The sandbox enforces permission profiles at the OS level:
- **Filesystem**: Restrict access to declared roots; deny sensitive paths
- **Process**: Limit command execution to allowlisted binaries
- **Network**: Enforce host allowlists and block unauthorized connections
- **IPC**: Prevent unauthorized inter-process communication

### Adapter Integration

Initial releases use Codex App Server's sandbox through the adapter:
- Codex sandbox profiles map to Aegisy permission profiles
- Adapter translates Aegisy policy to Codex sandbox configuration
- Sandbox violations are reported as structured errors

### Native Runtime

The native Aegisy runtime must provide equivalent enforcement:
- **macOS**: Sandbox profiles using App Sandbox or `sandbox-exec`
- **Windows**: Restricted tokens, job objects, and AppContainer isolation
- **Requirement**: No administrator privileges required
- **Gate**: Native runtime cannot replace adapter until sandbox parity is verified

## Audit and Enforcement

### Audit Events

Content-minimal audit events record:
- **Actor**: User, Agent, extension
- **Operation**: Command, file write, network request, Git mutation
- **Scope**: Project, session, paths, hosts
- **Decision**: Approved, denied, rule applied
- **Result**: Success, failure, error
- **Hashes**: Content hashes for verification
- **Timing**: Start, duration, completion
- **Correlation IDs**: Session, turn, item

Audit events exclude:
- File content and diffs
- Command output
- Secrets and credentials
- Repository paths (except relative to project root)

### Enforcement Mechanisms

**Runtime enforcement:**
- Permission checks before tool execution
- Sandbox violations terminate operations
- Policy changes invalidate cached approvals
- Managed policy intersection narrows profiles

**UI enforcement:**
- Approval dialogs block execution
- High-risk operations require explicit confirmation
- Durable rules are reviewable and editable
- Emergency disable revokes all permissions

**Storage enforcement:**
- Credentials never enter database or event payloads
- Secrets are redacted before persistence
- Audit events are append-only
- Tampering triggers read-only recovery

### Managed Policy

Managed policy (enterprise or organizational) can:
- Narrow permission profiles (cannot expand)
- Add denied paths and hosts
- Disable background execution
- Require approval for specific operations
- Set retention and audit requirements

Managed policy intersection is fail-closed: if policy cannot be retrieved or validated, the most restrictive profile applies.

## Implementation Status

Per task 18 in `tasks.md`:

- **18.1**: Permission profile schemas and managed-policy intersection (partial internal foundation)
- **18.2**: Policy engine for filesystem, command, network, extension, browser, background (partial internal foundation)
- **18.3**: Granular approval rules (not started)
- **18.4**: Risk classification (not started)
- **18.5**: Secure path defaults (not started)
- **18.6**: Secret detection and redaction (not started)
- **18.7**: Untrusted-data provenance tests (not started)
- **18.8**: Codex sandbox integration (not started)
- **18.9**: Native macOS sandbox (not started)
- **18.10**: Native Windows sandbox (not started)
- **18.11**: Adversarial security tests (not started)
- **18.12**: Platform release gates (not started)

## References

- Design document section 12: Security and approval architecture
- Task 18: Permission, Sandbox, and Secret Enforcement
- `permission-profile/0.1`: Internal permission profile contract
- AAP Protocol Guide: Security boundaries and approval lifecycle
