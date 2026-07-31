# ADR 010: ACP Extension Support

- Status: Deferred
- Accountable owner: Protocol Working Group
- Consulted owners: Runtime Integrations, Product Security, Desktop Platform
- Due gates: ACP maturity assessment at `8.1`; security review before any implementation

## Context

Anthropic Computer Protocol (ACP) is an emerging protocol for computer use and tool extensions that enables AI agents to interact with development environments through standardized interfaces. The Aegisy Agent Workbench design includes an `AcpAgentAdapter` as the second runtime adapter (after Codex App Server), intended to support ACP-compatible agents such as Kimi CLI and other installed coding agents.

ACP provides a protocol layer for:
- Computer use capabilities (terminal, filesystem, editor operations)
- Tool extension mechanisms
- Agent-to-environment communication
- Structured event streams for agent actions

The design document states that "unsupported features are reported through negotiated capability flags rather than simulated" and that "ACP adapters translate Codex App Server and ACP events into stable AAP items." The UI never consumes vendor events directly; all external protocols are translated through the Aegisy Agent Protocol (AAP).

## Decision Options

### Option 1: Full ACP Support
Implement comprehensive ACP adapter with all protocol features, including extensions.

**Pros:**
- Maximum compatibility with ACP-based agents
- Enables rich third-party agent ecosystem
- Leverages emerging industry standard

**Cons:**
- ACP protocol maturity and stability unknown
- Security model may not align with Aegisy's permission architecture
- Maintenance burden for protocol changes
- Risk of vendor-specific assumptions becoming de facto requirements

### Option 2: Limited ACP Subset
Support only core ACP methods; defer or reject extensions.

**Pros:**
- Controlled security surface
- Predictable maintenance scope
- Clear capability boundaries

**Cons:**
- May limit agent functionality
- Requires ongoing evaluation of which features to support
- Potential user confusion about partial support

### Option 3: Defer to Codex App Server
Prioritize Codex adapter; treat ACP as future work.

**Pros:**
- Focus on proven, mature runtime
- Reduces initial implementation complexity
- Allows ACP ecosystem to stabilize

**Cons:**
- Delays multi-runtime strategy
- May miss early adoption opportunity
- Limits user choice of agent runtimes

### Option 4: No ACP Support
Exclude ACP entirely; focus on Codex and native Aegisy runtime.

**Pros:**
- Simplest architecture
- No external protocol dependencies
- Full control over capabilities

**Cons:**
- Eliminates third-party agent compatibility
- Contradicts design document's adapter sequence
- Reduces ecosystem participation

## Security Considerations

ACP support introduces several security concerns:

1. **Untrusted Protocol Events**: ACP events from third-party agents must be treated as untrusted data, never as higher-priority instructions.

2. **Permission Boundary Enforcement**: ACP agents may request operations that violate Aegisy's permission profiles (Chat, Read Only, Workspace Write, Developer, Full Access). The adapter must enforce Aegisy policy, not delegate trust to the agent.

3. **Credential Isolation**: ACP agents must not receive long-lived Aegisy credentials or access to secure storage. Short-lived, scoped tokens are required.

4. **Extension Validation**: ACP extensions require the same validation as Aegisy extensions: namespacing, versioning, capability negotiation, redaction review, and fixture coverage.

5. **Sandbox Compatibility**: ACP agents must operate within Aegisy's sandbox model. If ACP assumes unrestricted host access, the adapter must mediate or reject operations.

## Capability Considerations

1. **Feature Parity**: ACP may not support Aegisy-specific concepts like durable sessions, event journals, Git worktrees, or background jobs. Missing features must be represented through capability flags, not simulated.

2. **Protocol Translation**: All ACP events must translate cleanly to AAP items. Lossy or ambiguous translations create UX and debugging problems.

3. **Versioning**: ACP protocol versions must be pinned and tested. Automatic protocol upgrades risk breaking changes.

4. **Degradation Transparency**: When ACP capabilities are unavailable, the UI must clearly indicate limitations rather than failing silently.

## Maintenance Considerations

1. **Protocol Stability**: ACP maturity is unknown. Frequent breaking changes would create ongoing maintenance burden.

2. **Fixture Coverage**: Each supported ACP version requires deterministic replay fixtures for testing.

3. **Compatibility Matrix**: ACP adapter versions must be tracked against Aegisy releases, similar to Codex App Server.

4. **Upstream Dependency**: Aegisy would depend on external protocol governance and evolution.

## Decision

**Defer ACP extension support** pending protocol maturity assessment and security review.

Rationale:
- ACP protocol stability and governance model are not yet established
- Security implications of third-party agent integration require thorough review
- Codex App Server adapter provides immediate value with known characteristics
- Aegisy native adapter will eventually provide full control over capabilities
- Deferring allows ACP ecosystem to mature and demonstrate stability

## Evidence Required Before Implementation

Before ACP support can be reconsidered, the following evidence is required:

1. **Protocol Maturity**:
   - Stable ACP specification with versioning guarantees
   - Multiple independent implementations demonstrating interoperability
   - Documented security model and permission architecture

2. **Security Review**:
   - Threat model for third-party agent integration
   - Permission boundary enforcement design
   - Credential isolation mechanism
   - Extension validation framework

3. **Technical Feasibility**:
   - Proof-of-concept ACP-to-AAP translation for core operations
   - Capability negotiation design
   - Fixture and replay test strategy
   - Compatibility matrix approach

4. **Product Validation**:
   - User demand for specific ACP-compatible agents
   - Clear use cases not served by Codex or native adapters
   - Cost-benefit analysis of maintenance burden

## Consequences

### Immediate Consequences

- ACP adapter implementation is blocked until due gates are met
- Design document's adapter sequence (Codex → ACP → Native) is revised to (Codex → Native, with ACP reconsidered later)
- Third-party ACP agents cannot be used with Aegisy Agent Workbench
- Development resources focus on Codex adapter and native runtime

### Long-term Consequences

- If ACP becomes an industry standard, Aegisy may need to implement support to remain competitive
- Deferring allows learning from other implementations' security and compatibility challenges
- Native Aegisy runtime may incorporate lessons from ACP without protocol dependency
- Future ACP support would require full security review and cannot bypass established gates

### Compatibility Impact

- No backward compatibility concerns (feature not yet implemented)
- Future ACP support must follow ADR 0003 extension policy
- Any Aegisy-specific ACP extensions require upstream proposal consideration

## Related Decisions

- ADR 0003: ACP Extension Policy (establishes guardrails for any future ACP extensions)
- ADR 0002: Codex Distribution and Client Identity (prioritized adapter)
- Design document section 5: Runtime adapter sequence

## Review Schedule

- Recheck ACP maturity at OpenSpec `8.1`
- Security review required before any implementation work begins
- Each proposed ACP extension requires separate review under `8.4` per ADR 0003
