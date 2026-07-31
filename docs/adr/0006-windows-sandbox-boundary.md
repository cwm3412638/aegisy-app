# ADR 0006: Windows Native Sandbox Selection

- Status: Deferred
- Accountable owner: Product Security
- Consulted owners: Windows Runtime, Release Engineering, Enterprise Security
- Due gate: OpenSpec `18.10`; release block `18.12`

## Context

The `aegisy-agentd` sidecar executes model-proposed commands, filesystem operations,
Git, MCP processes, and provider adapters as the logged-in user. Without additional
isolation, a compromised Agent or malicious tool could access credentials, SSH keys,
browser profiles, cloud tokens, and arbitrary filesystem paths. The security model
requires per-session sandbox enforcement that restricts filesystem roots, denies
secret paths, controls network access, and limits process capabilities.

Codex App Server provides a mature sandbox through its adapter, but the native Aegisy
runtime must deliver equivalent Windows enforcement before it can replace the adapter
for workspace writes. macOS sandbox implementation is owned separately; this ADR
addresses only Windows isolation.

## Decision Options

### Option 1: Windows AppContainer

AppContainer provides strong process isolation with filesystem, registry, network, and
capability restrictions. It is the recommended Windows sandbox for untrusted code.

Advantages:
- Strong isolation boundary enforced by the kernel
- Granular filesystem and network access control
- No administrator privileges required
- Supported on Windows 10+ and Windows Server 2016+

Disadvantages:
- Complex ACL and capability configuration
- May conflict with existing user filesystem permissions
- Requires careful handling of inherited handles and named objects
- Limited compatibility with some developer tools and Git operations

### Option 2: Job Objects with restricted tokens

Job Objects combined with restricted security tokens provide process containment and
resource limits without full AppContainer isolation.

Advantages:
- Simpler configuration than AppContainer
- Better compatibility with developer tools
- No administrator privileges required
- Supports process tree termination and resource accounting

Disadvantages:
- Weaker isolation than AppContainer
- Filesystem restrictions require additional enforcement
- Network isolation requires separate firewall rules or proxy
- Does not prevent all privilege escalation paths

### Option 3: Codex sandbox through adapter

Continue using Codex App Server's existing sandbox implementation through the runtime
adapter without implementing native Windows isolation.

Advantages:
- Already implemented and tested
- No additional Windows-specific development
- Proven compatibility with coding workflows

Disadvantages:
- Blocks native Aegisy runtime from owning workspace writes
- Maintains dependency on Codex distribution and updates
- Cannot customize isolation policy for Aegisy-specific requirements
- Does not satisfy the native runtime security milestone

### Option 4: Custom user-mode enforcement

Implement filesystem, network, and process restrictions through user-mode monitoring
and interception without kernel-enforced boundaries.

Advantages:
- Maximum flexibility for policy customization
- Easier debugging and policy iteration
- No Windows version dependencies

Disadvantages:
- Not a security boundary; can be bypassed by malicious code
- Requires continuous monitoring overhead
- Does not protect against compromised Agent or tool
- Fails the security model requirement for enforced isolation

## Required Evidence

Before selecting a final option, the following evidence must be gathered:

- Compatibility testing with Git, Node.js, Python, build tools, and common developer
  workflows under each sandbox option on Windows 10, Windows 11, and Windows Server.
- Filesystem access patterns for typical Agent sessions: which paths must be readable,
  writable, or denied, and how inherited permissions interact with sandbox ACLs.
- Network policy requirements: localhost access for MCP servers, outbound HTTPS for
  package managers, DNS resolution, and proxy/VPN compatibility.
- Performance impact measurements: process creation, filesystem operations, and
  terminal throughput overhead for each option.
- Privilege requirements: whether any configuration requires administrator rights or
  can be applied by a standard user at runtime.
- Recovery and diagnostics: how sandbox violations are reported, logged, and surfaced
  to users without exposing sensitive paths or credentials.

## Consequences

No native Aegisy runtime workspace write operations are authorized on Windows until
this decision is accepted and the selected sandbox passes OpenSpec `18.10` gates.
Until then, Windows users requiring workspace writes must use the Codex adapter, which
applies its own sandbox policy.

The selected sandbox must enforce the permission profiles defined in the security
architecture: filesystem roots, denied paths, network policy, and command restrictions.
User-mode monitoring alone does not satisfy the security boundary requirement.

A provisional decision may be recorded after initial compatibility testing, but the
status remains Deferred until complete evidence is gathered and the release block
`18.12` is cleared.
