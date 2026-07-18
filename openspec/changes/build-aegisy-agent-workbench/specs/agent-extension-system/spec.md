## ADDED Requirements

### Requirement: Extension types have explicit contracts
The system SHALL distinguish project instructions, Skills, hooks, MCP servers,
plugins, and privileged runtime adapters.

#### Scenario: Extension is discovered
- **WHEN** an extension is found in a user, project, managed, marketplace, or runtime source
- **THEN** the UI SHALL show its type, identifier, version, source, scope, trust state, requested capabilities, and effective enablement

#### Scenario: Bundle contains multiple components
- **WHEN** a plugin includes Skills, hooks, MCP configuration, commands, or assets
- **THEN** the installation preview SHALL list every component and its permissions before activation

#### Scenario: Component type is unknown
- **WHEN** a manifest declares an unsupported executable component
- **THEN** installation SHALL fail closed while preserving inspectable metadata

### Requirement: Extension origin and content are trusted deliberately
Executable or model-visible extension content SHALL have an origin, content hash,
and trust decision before use.

#### Scenario: Project hook is first discovered
- **WHEN** a project declares an executable hook not covered by managed policy
- **THEN** it SHALL remain disabled until the user reviews origin, command, events, scope, and hash

#### Scenario: Trusted extension changes
- **WHEN** its content hash, signature, executable command, or requested permissions change
- **THEN** prior trust and durable approvals SHALL become stale and execution SHALL pause for review

#### Scenario: Managed extension is enforced
- **WHEN** organization policy mandates or blocks an extension
- **THEN** the UI SHALL display the managed state and SHALL not allow a user override outside policy

### Requirement: Extension enablement is scoped
Extensions SHALL be enabled independently at global, project, session, and child
task scope with an explicit precedence model.

#### Scenario: Skill is enabled for one project
- **WHEN** another project is opened
- **THEN** the Skill SHALL not become active there unless global or managed policy also enables it

#### Scenario: Child task receives a limited extension set
- **WHEN** a parent delegates work
- **THEN** the child SHALL receive only the declared subset and SHALL not inherit unrelated parent MCP servers, hooks, or permissions

#### Scenario: Scope rules conflict
- **WHEN** a lower scope enables a component denied by a higher-priority policy
- **THEN** the component SHALL remain disabled and the effective-policy explanation SHALL identify the blocking source

### Requirement: Skills and project instructions are inspectable context
The runtime SHALL load instruction sources with deterministic precedence,
provenance, and context-size accounting.

#### Scenario: Nested project instructions apply
- **WHEN** work targets a file under a directory with closer scoped instructions
- **THEN** the context manifest SHALL include the applicable instruction chain and its precedence

#### Scenario: Skill is invoked
- **WHEN** the user or Agent selects a Skill
- **THEN** the turn SHALL record Skill identity, version/hash, source path, included references, and any script/tool permissions

#### Scenario: Instruction conflicts with security policy
- **WHEN** model-visible guidance requests behavior forbidden by runtime policy
- **THEN** runtime policy SHALL win and the denial SHALL be visible without rewriting the instruction as trusted policy

### Requirement: MCP server lifecycle and tools are observable
The system SHALL manage MCP startup, authentication, tools, resources, prompts,
elicitation, progress, errors, and shutdown per effective scope.

#### Scenario: MCP server starts for a session
- **WHEN** it becomes enabled and trusted
- **THEN** the runtime SHALL expose starting, ready, failed, authentication-required, and stopped states with bounded logs

#### Scenario: MCP tool requests approval
- **WHEN** policy requires confirmation
- **THEN** the approval SHALL show server identity, tool, arguments with secret redaction, requested permissions, and persistence options

#### Scenario: MCP server becomes unavailable
- **WHEN** a turn depends on a failed server
- **THEN** the item SHALL fail with server-specific remediation and SHALL not be reported as a model failure or successful tool result

### Requirement: Hooks execute through a policy engine
Hooks SHALL declare lifecycle event, matcher, command or handler, timeout, working
scope, permissions, failure behavior, and trust state.

#### Scenario: Pre-tool hook denies an action
- **WHEN** a trusted hook returns a deny result within its contract
- **THEN** the target tool SHALL not execute and the timeline SHALL attribute the denial to that hook

#### Scenario: Hook times out or crashes
- **WHEN** its declared timeout is exceeded
- **THEN** the configured fail-open or fail-closed policy SHALL apply, with fail-closed required for managed security hooks

#### Scenario: Hook emits unbounded output
- **WHEN** output exceeds limits
- **THEN** it SHALL be truncated or stored as an artifact without blocking the Agent event loop

### Requirement: Extension installation and update are reversible
Installing, upgrading, disabling, or removing an extension SHALL preserve enough
metadata and backup state to recover from failure.

#### Scenario: Upgrade validation fails
- **WHEN** signature, manifest, compatibility, dependency, or health checks fail
- **THEN** the active previous version SHALL remain unchanged and the candidate SHALL not execute

#### Scenario: User removes a plugin
- **WHEN** sessions still reference its Skills or events
- **THEN** stored history SHALL retain immutable identity metadata while executable content is disabled or removed according to retention policy
