# ACP Runtime Adapter Design

## Overview

The ACP Runtime Adapter enables Aegisy Agent Workbench to integrate with ACP-compatible agents such as Kimi CLI and other installed coding agents. This adapter translates between the Aegisy Agent Protocol (AAP) and the Agent Communication Protocol (ACP), providing protocol translation, lifecycle management, and capability negotiation while maintaining security boundaries.

## Architecture

### Adapter Position

```text
Qt Workbench <-> AAP <-> aegisy-agentd <-> ACP Adapter <-> ACP Agent Process
```

The ACP adapter is one of three planned runtime adapters:

1. **CodexAppServerAdapter**: Codex App Server integration (primary)
2. **AcpAgentAdapter**: ACP-compatible agents (this document)
3. **AegisyNativeAdapter**: Native Aegisy orchestration (future)

### Adapter Responsibilities

- Launch and supervise ACP-compatible agent processes
- Translate AAP requests/responses to/from ACP protocol
- Map AAP session/turn/item lifecycle to ACP equivalents
- Negotiate and enforce capability boundaries
- Handle process health, restart, and graceful shutdown
- Provide structured error classification and recovery

## Protocol Translation

### Session Lifecycle Mapping

| AAP Concept | ACP Equivalent | Notes |
|-------------|----------------|-------|
| `session/start` | ACP session initialization | Map project root, mode, model profile |
| `session/resume` | ACP session restoration | Requires portable context if thread state unavailable |
| `session/fork` | ACP fork or new session | Portable context package with approved history |
| `session/archive` | ACP archive (if supported) | Graceful degradation if unsupported |
| `session/delete` | ACP cleanup | Local AAP state remains authoritative |

### Turn Lifecycle Mapping

| AAP State | ACP Equivalent | Translation |
|-----------|----------------|-------------|
| `turn/start` | ACP message submission | Include context, tools, model selection |
| `running` | ACP processing | Monitor for progress events |
| `completed` | ACP success | Extract final items, usage, artifacts |
| `failed` | ACP error | Map to AAP `runtime-error/0.1` classes |
| `interrupted` | ACP cancellation | Explicit cancel or timeout |

### Item Type Mapping

| AAP Item | ACP Equivalent | Handling |
|----------|----------------|----------|
| `message` (user) | ACP user message | Direct translation |
| `message` (assistant) | ACP assistant response | Extract text content |
| `reasoning-summary` | ACP thinking/reasoning | If exposed by ACP agent |
| `command` | ACP tool execution | Map to AAP command lifecycle |
| `file-change` | ACP file operations | Convert to AAP Proposal format |
| `tool` | ACP tool call | Generic tool mapping |
| `artifact` | ACP output artifacts | Content-addressed storage |

### Unsupported Features

When ACP agents lack AAP-required features, the adapter:

- Reports missing capabilities through negotiated capability flags
- Returns structured errors rather than simulating success
- Documents degraded functionality in session metadata
- Never fabricates protocol responses

## Lifecycle Management

### Process Supervision

```rust
struct AcpAdapterProcess {
    child: Child,
    pid: u32,
    version: String,
    transport: AcpTransport,
    health_state: HealthState,
    last_heartbeat: Instant,
}
```

**Launch Sequence:**

1. Validate ACP agent binary path and version
2. Spawn process with controlled environment
3. Establish stdio or socket transport
4. Perform ACP handshake and capability negotiation
5. Report readiness to AAP runtime
6. Begin health monitoring

**Health Monitoring:**

- Periodic heartbeat checks (configurable interval)
- Stderr monitoring for fatal errors
- Process exit detection
- Bounded restart attempts with backoff
- Crash-loop protection (max 3 attempts)

**Shutdown Sequence:**

1. Send ACP graceful shutdown request
2. Wait for acknowledgment (timeout: 5 seconds)
3. Send SIGTERM if unresponsive
4. Send SIGKILL after additional timeout (3 seconds)
5. Reap process and clean up resources
6. Report final state to AAP runtime

### Restart Policy

| Failure Type | Restart | Reason |
|--------------|---------|--------|
| Transport error | Yes | Transient network/IPC issue |
| Protocol timeout | Yes | May recover on retry |
| Version mismatch | No | Incompatible binary |
| Repeated crashes | No | After 3 attempts with backoff |
| Explicit shutdown | No | User or system initiated |

## Error Handling

### Error Classification

Map ACP errors to AAP `runtime-error/0.1` classes:

| ACP Error | AAP Class | Retryable |
|-----------|-----------|-----------|
| Connection failure | `transport` | Yes |
| Timeout | `timeout` | Yes |
| Protocol violation | `protocol` | No |
| Agent internal error | `adapter` | Depends |
| Model/provider error | `provider` | Depends |
| Resource exhaustion | `budget` | No |
| File operation failure | `workspace` | No |
| Git operation failure | `git` | No |

### Error Recovery

**Transient Failures:**
- Automatic retry with exponential backoff
- Preserve turn state and user input
- Report retry attempts to user

**Permanent Failures:**
- Mark turn as `failed` with structured error
- Preserve session state
- Offer recovery options (restart adapter, fork session)

**Partial Failures:**
- Complete successful operations
- Report partial results with error context
- Allow user to continue or retry

## Security Boundaries

### Permission Enforcement

The ACP adapter enforces AAP security policies:

- **Filesystem Access**: Validate all paths against project roots and denied globs
- **Command Execution**: Require approval per session permission profile
- **Network Access**: Enforce host allowlists and purpose restrictions
- **Secret Protection**: Redact credentials from ACP requests and responses

### Trust Model

```text
User Trust -> AAP Runtime -> ACP Adapter -> ACP Agent Binary
```

**Trust Requirements:**

1. ACP agent binary must be explicitly trusted by user
2. Binary hash verified against known-good values
3. Version compatibility checked before launch
4. Capabilities negotiated and enforced at runtime
5. No automatic privilege escalation

### Isolation

- ACP agent runs as user process (no elevated privileges)
- Separate process boundary from Qt UI and AAP runtime
- Controlled environment variables (no credential leakage)
- Bounded resource usage (memory, CPU, file handles)

## Capability Negotiation

### Negotiation Flow

1. **AAP Handshake**: Qt ↔ aegisy-agentd establishes AAP capabilities
2. **ACP Discovery**: Adapter queries ACP agent for supported features
3. **Intersection**: Adapter computes available capability set
4. **Advertisement**: Adapter reports final capabilities to AAP runtime
5. **Enforcement**: Both AAP and adapter enforce negotiated boundaries

### Capability Categories

**Core Capabilities:**
- `acp.agent.basic` - Basic message exchange
- `acp.agent.tools` - Tool/command execution
- `acp.agent.files` - File read/write operations
- `acp.agent.git` - Git operations

**Optional Capabilities:**
- `acp.agent.reasoning` - Exposed thinking/reasoning
- `acp.agent.artifacts` - Structured output artifacts
- `acp.agent.streaming` - Incremental response streaming
- `acp.agent.context` - Advanced context management

**Degraded Modes:**

When ACP agent lacks features:
- Report capability as unavailable
- Disable dependent AAP features
- Document limitations in session metadata
- Provide clear user feedback

## Implementation Phases

### Phase 1: Foundation (Deferred)

**Status**: Not yet implemented. Codex App Server adapter takes priority.

**Scope:**
- Basic ACP process launch and supervision
- Stdio transport with bounded frame handling
- Simple message exchange (user → agent → response)
- Health monitoring and graceful shutdown
- Version validation and compatibility checks

**Deliverables:**
- `AcpAdapterProcess` struct and lifecycle
- Basic protocol translation for messages
- Health state reporting to AAP
- Integration tests with mock ACP agent

### Phase 2: Protocol Translation (Deferred)

**Scope:**
- Complete AAP ↔ ACP item type mapping
- Turn lifecycle state machine
- Command/tool execution translation
- File change proposal conversion
- Error classification and recovery

**Deliverables:**
- Full item type translators
- Structured error mapping
- Retry and backoff logic
- Protocol compliance tests

### Phase 3: Advanced Features (Deferred)

**Scope:**
- Capability negotiation implementation
- Session fork and portable context
- Streaming response handling
- Artifact and content-addressed storage
- Git operation translation

**Deliverables:**
- Capability discovery and enforcement
- Portable session context serialization
- Incremental update handling
- Integration with AAP content references

### Phase 4: Production Hardening (Deferred)

**Scope:**
- Crash recovery and restart policies
- Resource limit enforcement
- Security boundary validation
- Performance optimization
- Cross-platform compatibility (macOS, Windows)

**Deliverables:**
- Bounded restart with crash-loop protection
- Memory and CPU monitoring
- Path validation and secret redaction
- Benchmark suite
- Windows named-pipe transport

## Current Status

**Implementation Status**: Deferred

The ACP Runtime Adapter design is complete but implementation is deferred. The Codex App Server adapter (task 7) takes priority as the primary runtime integration. ACP adapter implementation will begin after:

1. Codex adapter reaches production stability
2. AAP protocol stabilizes through real-world usage
3. ACP-compatible agents demonstrate sufficient maturity
4. User demand justifies the engineering investment

**Design Stability**: This document defines the architecture and contracts. Implementation details may evolve based on:
- Lessons learned from Codex adapter
- ACP protocol evolution
- AAP capability additions
- Security and performance requirements

## References

- [Aegisy Agent Protocol Guide](AAP-PROTOCOL-GUIDE.md)
- [Agent Workbench Design](../openspec/changes/build-aegisy-agent-workbench/design.md)
- [Runtime Error Classification](../agent-runtime/aap-schema/stable/v0.1/core.schema.json)
- [Codex Adapter Implementation](../agent-runtime/aegisy-agentd/src/codex_adapter.rs)
