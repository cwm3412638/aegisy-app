# ACP Runtime Adapter Support

This document defines the supported Agent Communication Protocol (ACP) agents, their
versions, known feature gaps, installation requirements, and troubleshooting guidance.

## Overview

The ACP Runtime Adapter enables Aegisy to work with third-party agent implementations
that conform to the Agent Communication Protocol specification. Unlike the built-in
Codex adapter, ACP agents are installed and managed by the user.

## Supported ACP Agents

### Status: Not Yet Implemented

The ACP adapter is planned but not yet implemented. This document serves as the
specification for future implementation.

### Planned Support

| Agent | Minimum Version | Status | Notes |
|-------|----------------|--------|-------|
| Kimi CLI | TBD | Planned | Reference implementation |
| Generic ACP | 1.0+ | Planned | Any spec-compliant agent |

## Feature Matrix

### Core Features

| Feature | Required | Fallback Behavior |
|---------|----------|-------------------|
| Session create/load | ✓ | N/A - blocking |
| Prompt/response | ✓ | N/A - blocking |
| Cancellation | ✓ | N/A - blocking |
| Authentication | ✓ | N/A - blocking |

### Optional Features

| Feature | Fallback Behavior |
|---------|-------------------|
| Structured patches | Show raw diff text |
| Git operations | Disable Git context |
| Child sessions | Disable session forking |
| Background terminals | User terminals only |
| File references | Inline content only |
| Plan steps | Show as single message |

## Installation Requirements

### User Responsibility

ACP agents are **not bundled** with Aegisy. Users must:

1. Install the ACP agent binary independently
2. Ensure the agent is in PATH or configure explicit path
3. Manage agent updates and compatibility
4. Handle agent-specific authentication

### Aegisy Responsibilities

Aegisy will:

1. Detect installed ACP agents
2. Validate protocol version compatibility
3. Report missing features gracefully
4. Provide clear error messages for setup issues

## Configuration

### Agent Discovery

```json
{
  "acp": {
    "agents": [
      {
        "name": "kimi",
        "executable": "kimi",
        "protocol_version": "1.0"
      }
    ]
  }
}
```

### Authentication

ACP agents handle their own authentication. Aegisy does not:
- Store ACP agent credentials
- Manage ACP agent login flows
- Proxy ACP agent authentication

Users must authenticate with their ACP agent before use.

## Known Limitations

### Current Limitations (Pre-Implementation)

- No ACP agents are currently supported
- Protocol version not yet pinned
- Capability negotiation not implemented
- Fallback UI not implemented

### Design Limitations

1. **Installation Ownership**: Users must install and update ACP agents
2. **Authentication**: Users must handle agent-specific auth flows
3. **Feature Parity**: ACP agents may lack features available in Codex
4. **Performance**: External process communication may be slower than built-in

## Troubleshooting

### Agent Not Found

**Symptom**: "ACP agent not found" error

**Solutions**:
1. Verify agent is installed: `which <agent-name>`
2. Add agent to PATH
3. Configure explicit path in settings
4. Check agent executable permissions

### Protocol Version Mismatch

**Symptom**: "Incompatible ACP protocol version" error

**Solutions**:
1. Check agent version: `<agent-name> --version`
2. Update agent to supported version
3. Check Aegisy release notes for version requirements

### Authentication Required

**Symptom**: "Authentication required" error

**Solutions**:
1. Run agent login: `<agent-name> login`
2. Verify credentials are valid
3. Check agent-specific auth documentation

### Missing Features

**Symptom**: Features unavailable or degraded

**Expected Behavior**:
- Aegisy will show clear indicators for missing features
- Fallback behavior will be used where possible
- Some workflows may be unavailable

**Solutions**:
1. Check agent's supported features
2. Consider using Codex adapter for full feature set
3. Report feature gaps to agent maintainer

## Migration from Codex

### Feature Comparison

| Feature | Codex | ACP (Planned) |
|---------|-------|---------------|
| Installation | Bundled | User-managed |
| Authentication | Integrated | Agent-specific |
| Structured patches | ✓ | Agent-dependent |
| Git operations | ✓ | Agent-dependent |
| Child sessions | ✓ | Agent-dependent |
| Background terminals | ✓ | Agent-dependent |
| Performance | Optimized | Variable |

### When to Use ACP

Use ACP agents when:
- You need a specific agent's capabilities
- You're developing an ACP-compatible agent
- You want to use multiple agent backends

Use Codex when:
- You want the best-integrated experience
- You need all features working reliably
- You prefer bundled, tested components

## Development and Testing

### Contract Tests

ACP adapter contract tests will verify:
- Protocol version negotiation
- Session lifecycle (create/load/prompt/cancel)
- Authentication flows
- Feature capability reporting
- Error handling and recovery
- Multi-session support

### Test Agents

- **Kimi CLI**: Primary reference implementation
- **Generic Mock**: Minimal spec-compliant agent for testing

## Support and Feedback

### Reporting Issues

When reporting ACP adapter issues, include:
- Agent name and version
- ACP protocol version
- Aegisy version
- Error messages and logs
- Steps to reproduce

### Feature Requests

Feature requests should specify:
- Which ACP agent(s) support the feature
- Expected behavior
- Fallback behavior if unavailable
- Use case and priority

## Future Enhancements

### Planned

- [ ] Protocol version 1.0 support
- [ ] Kimi CLI integration
- [ ] Generic ACP agent support
- [ ] Capability-based UI adaptation
- [ ] Multi-agent switching

### Under Consideration

- Agent marketplace/discovery
- Automatic agent updates
- Agent performance profiling
- Custom agent configuration UI

## References

- Agent Communication Protocol Specification: [TBD]
- Kimi CLI Documentation: [TBD]
- Aegisy ACP Integration Guide: [TBD]
