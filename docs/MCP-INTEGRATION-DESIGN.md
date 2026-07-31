# MCP Integration Design

## Overview

This document defines the Model Context Protocol (MCP) server integration architecture for Aegisy Agent Workbench. MCP enables external tools and resources to extend Agent capabilities through a standardized protocol. This design establishes security boundaries, trust models, and implementation phases for Skills, Plugins, MCP servers, Hooks, and Instructions.

## Extension Concepts

Aegisy uses distinct extension concepts with unified management:

### Instruction
Project or user guidance files (e.g., `AGENTS.md`, `.cursorrules`) that provide context to the Agent without executable code.

### Skill
Reusable instruction package with optional file references and scripts. Skills are invoked by name and may include parameterized templates.

### Hook
Lifecycle policy or automation that executes at specific points: prompt preparation, tool invocation, edit application, or turn completion. Hooks can modify, approve, deny, or observe operations.

### MCP Server
External process providing tools, resources, or prompts over the Model Context Protocol (stdio or HTTP transport). MCP servers run as supervised child processes with declared permissions.

### Plugin
Installable bundle containing Skills, Hooks, MCP server configurations, assets, commands, and metadata. Plugins are versioned, signed, and subject to trust review.

### Runtime Adapter
Privileged signed integration maintained by Aegisy (e.g., Codex App Server adapter, ACP adapter). Not user-installable.

## MCP Server Architecture

### Process Model

MCP servers run as supervised child processes of `aegisy-agentd`:

- **Stdio transport**: Server communicates via stdin/stdout with bounded frame sizes
- **HTTP transport**: Server runs on localhost with ephemeral port and connection token
- **Lifecycle**: Launch on first use, idle timeout, explicit shutdown, crash recovery
- **Isolation**: Each server runs in its own process with declared filesystem/network permissions
- **Supervision**: Health checks, stderr capture, restart policy, crash-loop protection

### Protocol Integration

MCP protocol features map to Aegisy capabilities:

- **Tools**: Exposed as Agent-callable functions with parameter schemas and approval requirements
- **Resources**: URI-addressable content (files, API responses) with MIME types and size limits
- **Prompts**: Reusable prompt templates with parameter substitution
- **Progress**: Streaming progress notifications for long-running operations
- **Sampling**: LLM completion requests from MCP servers (requires explicit approval)

### Configuration Schema

MCP server configuration includes:

```json
{
  "id": "unique-server-id",
  "name": "Display Name",
  "version": "1.0.0",
  "command": ["node", "server.js"],
  "args": ["--option", "value"],
  "env": {"KEY": "value"},
  "transport": "stdio",
  "permissions": {
    "filesystem": {
      "read": ["/allowed/path"],
      "write": ["/allowed/path"]
    },
    "network": {
      "allowed_hosts": ["api.example.com"]
    }
  },
  "trust": {
    "source": "marketplace|user|project",
    "signature": "base64-signature",
    "content_hash": "sha256-hash",
    "verified": true
  },
  "scopes": ["user", "project:project-id"],
  "enabled": true
}
```

## Permission and Trust Model

### Trust Levels

1. **Verified Marketplace**: Signed by Aegisy, reviewed for security, automatic updates
2. **Signed Third-Party**: Valid signature from known publisher, user review required
3. **User-Installed**: No signature, explicit trust decision, hash-pinned
4. **Project-Provided**: Discovered in project, requires first-open trust review

### Permission Profiles

MCP servers declare required permissions:

- **Filesystem**: Read/write access to specific paths (workspace-relative or absolute)
- **Network**: Allowed destination hosts and ports
- **Environment**: Access to environment variables (excluding credentials)
- **Execution**: Ability to spawn child processes
- **Credentials**: Access to credential store (requires explicit approval per use)

### Approval Flow

1. **Installation**: User reviews permissions, source, and trust level
2. **First Use**: Confirmation dialog shows server identity and requested permissions
3. **Runtime**: Tool calls may require per-invocation approval based on risk level
4. **Revocation**: Trust revoked if content hash changes or signature becomes invalid

### Security Boundaries

- MCP servers cannot access Aegisy credentials without explicit per-use approval
- Filesystem access is bounded by declared paths and workspace roots
- Network access is limited to allowed hosts (no credential forwarding)
- Stderr output is captured, redacted, and bounded (no raw logs in timeline)
- Tool results are treated as untrusted data, never as instructions
- Sampling requests require explicit user approval and model budget

## Skills and Plugins

### Skill Structure

Skills are instruction packages with:

- **Metadata**: ID, name, version, description, author
- **Content**: Markdown instruction text with parameter placeholders
- **References**: Optional file paths to include as context
- **Scripts**: Optional executable scripts (require trust review)
- **Parameters**: Typed parameters with validation rules

### Plugin Manifest

Plugins bundle multiple extensions:

```json
{
  "id": "plugin-id",
  "name": "Plugin Name",
  "version": "1.0.0",
  "author": "Author Name",
  "description": "Plugin description",
  "components": {
    "skills": ["skill-1.md", "skill-2.md"],
    "hooks": ["pre-edit.js", "post-command.js"],
    "mcp_servers": ["server-config.json"],
    "assets": ["icon.png", "templates/"]
  },
  "permissions": {
    "filesystem": {"read": ["${workspace}"]},
    "network": {"allowed_hosts": []}
  },
  "signature": "base64-signature",
  "content_hash": "sha256-hash"
}
```

### Hook Execution Model

Hooks execute at defined lifecycle points:

- **Pre-prompt**: Modify or augment prompt before sending to model
- **Pre-tool**: Approve, deny, or modify tool invocations
- **Pre-edit**: Review and modify proposed file changes
- **Post-command**: Process command output, extract diagnostics
- **Post-turn**: Summarize turn results, update context

Hook constraints:

- **Timeout**: 5 seconds default, 30 seconds maximum
- **Output**: 64 KiB maximum
- **Failure policy**: `allow`, `deny`, or `warn`
- **Execution**: Sandboxed with declared permissions
- **Attribution**: Hook identity recorded in timeline events

## Scope and Precedence

Extensions can be scoped to different levels:

1. **Managed**: Aegisy-provided, always enabled, highest precedence
2. **User**: User-installed, applies to all projects
3. **Project**: Project-specific, applies to all sessions in project
4. **Session**: Session-specific, applies to current session only
5. **Child**: Inherited from parent session with restrictions

Precedence rules:

- More specific scopes override broader scopes
- Explicit disable overrides inherited enable
- Managed extensions cannot be disabled
- Effective state is computed and explained in UI

## Implementation Phases

### Phase 1: Foundation (Milestone 3)
- Extension registry schema and storage
- Trust review UI for project-provided instructions
- Skill discovery and invocation
- MCP server configuration parsing
- Permission profile validation

### Phase 2: MCP Integration (Milestone 4)
- MCP stdio transport and process supervision
- Tool discovery and invocation with approval
- Resource access with size limits
- Progress notifications
- Stderr capture and redaction

### Phase 3: Plugins and Hooks (Milestone 5)
- Plugin manifest validation and installation
- Hook discovery and execution
- Scope precedence and effective state
- Trust revocation on content change
- Extension marketplace integration

### Phase 4: Advanced Features (Milestone 6)
- MCP HTTP transport
- MCP sampling with approval
- Plugin atomic upgrade and rollback
- Extension health monitoring
- Supply-chain audit and signature verification

## Security Validation

### Required Tests

1. **Malicious Extension**: Attempts to access credentials, exfiltrate secrets
2. **Excessive Output**: Generates unbounded output to exhaust resources
3. **Timeout**: Hangs indefinitely, tests timeout enforcement
4. **Crash**: Crashes during execution, tests recovery
5. **Dependency Failure**: Missing dependencies, tests error handling
6. **Manifest Spoofing**: Invalid signatures, hash mismatches

### Audit Requirements

- All extension executions logged with identity and permissions
- Content hashes verified before execution
- Signatures validated against trusted keys
- Permission changes require re-approval
- Revoked extensions cannot execute

## Migration from Existing Aegisy

Existing Aegisy Skills and MCP configurations are imported with:

1. **Preview**: Show current configuration and proposed migration
2. **Backup**: Create reversible backup of existing configuration
3. **Migration**: Convert to new schema with trust review
4. **Validation**: Verify migrated configuration works correctly
5. **Rollback**: Ability to restore previous configuration

## Open Questions

1. Should MCP servers share a common sandbox or run with individual permissions?
2. What is the maximum number of concurrent MCP servers per session?
3. Should Hook execution be synchronous or asynchronous with bounded wait?
4. How should conflicting Hooks at the same lifecycle point be ordered?
5. Should Skills support nested invocation or only flat composition?

## References

- [Extension Model (design.md §13)](../openspec/changes/build-aegisy-agent-workbench/design.md#13-extension-model)
- [Task 19: Skills, Plugins, MCP, Hooks, and Instructions](../openspec/changes/build-aegisy-agent-workbench/tasks.md#19-skills-plugins-mcp-hooks-and-instructions)
- [Model Context Protocol Specification](https://spec.modelcontextprotocol.io/)
- [Security and Approval Architecture (design.md §12)](../openspec/changes/build-aegisy-agent-workbench/design.md#12-security-and-approval-architecture)
