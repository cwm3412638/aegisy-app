## ADDED Requirements

### Requirement: Aegisy is a website companion first
The desktop application SHALL use authenticated Aegisy website state to help users
configure and maintain supported local tools. The default navigation SHALL lead to
configuration and environment workflows rather than requiring an Agent project or
repository.

#### Scenario: User opens the application
- **WHEN** an authenticated user reaches the main window
- **THEN** the first page SHALL present local configuration/environment status and direct actions for configuration, desktop enhancement, extensions, diagnostics, and updates

#### Scenario: Website state is unavailable
- **WHEN** current website configuration metadata cannot be authenticated or loaded
- **THEN** the desktop SHALL preserve the last valid local state, expose a bounded offline/error status, and SHALL NOT fabricate a new profile or overwrite local tool configuration

#### Scenario: Website metadata is cached for offline status
- **WHEN** an exact account-bound website response passes origin, authentication, bounds, type, and projection validation
- **THEN** the desktop MAY cache only credential-free non-authorizing metadata under that account identity and SHALL NOT expose the cache to another account

#### Scenario: Website response fails its trust boundary
- **WHEN** the account is unverified, auth epoch changes, origin or final URL differs, a redirect occurs, Content-Type or body bounds fail, pagination is incomplete, or projected metadata is malformed
- **THEN** the desktop SHALL publish no configuration candidate or raw Key signal from that response and SHALL preserve the prior local/cache/tool state

#### Scenario: User selects a projected Key in the connection wizard
- **WHEN** an active tool-compatible candidate has an exact account/Key-bound SecureStorage handle and the user explicitly tests, queries models, or saves the Profile
- **THEN** the wizard SHALL resolve only that exact handle, SHALL keep credential plaintext out of widget item data and QSettings, and SHALL persist only hashed website source identities with the Profile

#### Scenario: Credential handle is cross-bound
- **WHEN** a handle is replayed with another account identity, Key identity, projection entry, or malformed binding
- **THEN** resolution SHALL fail without returning a credential, creating/updating a Profile, changing active state, or writing tool configuration

#### Scenario: Connection wizard queries models for a website candidate
- **WHEN** the user selects a current active candidate and requests its real model list
- **THEN** the request SHALL bind a unique request ID, auth epoch, account, source projection, Key, credential handle, tool platform, and exact origin/URL, and the result SHALL contain only bounded model IDs with no provider body or selection authority

#### Scenario: Model response arrives after selection changes
- **WHEN** the account, Key, handle, source projection, tool platform, base origin, auth epoch, or pending request changes before a model response completes
- **THEN** the response SHALL be stale or inert and SHALL NOT change the current model selector, Profile, active state, or tool configuration

#### Scenario: User browses account models outside the connection wizard
- **WHEN** the user opens the model browser and selects a website candidate
- **THEN** the browser SHALL expose only sanitized active candidate metadata, SHALL accept no raw or manually pasted Key, and SHALL use the same request-specific model projection without subscribing to global Key/model signals

#### Scenario: User starts a website-backed Chat or Chat Skill request
- **WHEN** the user selects a current active website candidate and starts Chat, image generation, or presentation planning
- **THEN** the UI SHALL store only account/Key/projection/handle/platform metadata, ApiClient SHALL revalidate that exact binding and resolve the credential once from SecureStorage, and persisted Chat history SHALL contain only the hashed Key identity and bounded safe display name

#### Scenario: Chat credential binding changes during a request
- **WHEN** the account, auth epoch, origin, source projection, Key, handle, or platform changes before a Chat or Chat Skill request completes
- **THEN** the request SHALL be retired or its late result SHALL be inert and SHALL NOT append content, save a generated artifact, or replace the current model selection

#### Scenario: User generates an image from the standalone image tool
- **WHEN** the user selects an active `gpt-image` candidate and starts generation
- **THEN** the image tool SHALL store only sanitized group/display and account/Key/projection/handle/platform metadata, SHALL call the companion image broker with a unique request ID, and SHALL accept only the exactly correlated result

### Requirement: One-click configuration is previewed and recoverable
Applying a website-backed profile SHALL affect only the selected local tool. The
desktop SHALL show the target and config files, create a recoverable backup, write
through the existing tool-specific configuration implementation, and verify the
result before reporting success.

#### Scenario: User applies a profile
- **WHEN** the user confirms a valid profile for one supported tool
- **THEN** only that tool's reviewed config targets SHALL change and the active profile SHALL advance only after configuration verification succeeds

#### Scenario: Configuration fails
- **WHEN** installation, backup, write, or verification fails
- **THEN** Aegisy SHALL report the bounded failure, retain or restore the prior active configuration, and SHALL NOT mark the new profile active

#### Scenario: Provider is only a configuration target
- **WHEN** the user configures Claude Code, Gemini CLI, or OpenCode
- **THEN** Aegisy MAY write and verify that tool's supported connection configuration but SHALL NOT advertise an embedded Agent runtime for it

### Requirement: Extensions and Chinese UX are first-class companion tools
The desktop SHALL provide inspectable entry points for Codex plugins, custom Skills,
MCP configuration, desktop enhancements, and Chinese-language support. Installation
or modification SHALL show origin/target, preserve recoverability, and fail closed
when compatibility or trust cannot be established.

#### Scenario: User manages a custom Skill
- **WHEN** the user imports, enables, disables, updates, or removes a Skill
- **THEN** Aegisy SHALL operate on the bounded Skills root, validate the package and manifest, and reflect the resulting state without granting script execution authority

#### Scenario: User applies a desktop enhancement or localization
- **WHEN** the user selects a supported enhancement or Chinese-language package
- **THEN** Aegisy SHALL verify the supported application/version and preserve a rollback path before changing local application resources

#### Scenario: Extension compatibility is unknown
- **WHEN** a plugin, Skill, MCP entry, or localization package has unknown compatibility or provenance
- **THEN** Aegisy SHALL show it as unavailable or requiring explicit review and SHALL NOT silently install or execute it

### Requirement: Integrated programming is Codex-only in the active scope
The optional integrated programming surface SHALL use only the pinned Codex adapter.
Claude, Gemini, ACP, and other Agent adapters SHALL remain deferred and unavailable.

#### Scenario: User opens integrated programming
- **WHEN** the user selects `Codex 编程`
- **THEN** the desktop SHALL start or connect only to the reviewed Codex runtime and SHALL present an unavailable/recovery state if that runtime is missing or incompatible

#### Scenario: Non-Codex Agent support is requested
- **WHEN** a UI, config value, adapter event, or capability attempts to select Claude, Gemini, ACP, or another embedded Agent runtime
- **THEN** the active product SHALL reject the selection without fallback, dispatch, or simulated compatibility

### Requirement: Deferred Workbench authority remains unavailable
The product reset SHALL NOT convert retained Workbench implementation into release
authority. Agent-authored writes, command execution, Git mutation, approvals,
background work, and remote control remain governed by their existing incomplete
gates.

#### Scenario: Retained code exists outside active scope
- **WHEN** a retained Workbench component is compiled or packaged
- **THEN** its unavailable capability state SHALL remain truthful and it SHALL NOT become reachable solely because the companion application ships
