## ADDED Requirements

### Requirement: Aegisy publishes an authoritative model catalog
The client SHALL use a versioned Aegisy catalog for model identity, provider,
availability, entitlement, protocol, limits, capabilities, roles, and lifecycle.

#### Scenario: Catalog refresh succeeds
- **WHEN** the authenticated catalog response passes schema and signature validation
- **THEN** it SHALL replace the active cache atomically and record version, issue time, expiry, and source

#### Scenario: Catalog is stale while offline
- **WHEN** refresh fails and a previously valid cache exists
- **THEN** the client SHALL mark it stale, preserve only compatible known behavior, and SHALL not invent new prices, limits, capabilities, or availability

#### Scenario: Model is deprecated
- **WHEN** a catalog entry declares replacement or retirement
- **THEN** existing sessions SHALL remain readable, new selection SHALL follow policy, and migration guidance SHALL identify any capability differences

### Requirement: Model selection is capability-gated
The system SHALL compare mode and runtime requirements with declared model
capabilities before allowing selection.

#### Scenario: Work requires tool calls
- **WHEN** a selected model lacks verified tool-call support for the active runtime protocol
- **THEN** Work submission SHALL be blocked or downgraded to Chat with a visible explanation

#### Scenario: User attaches an image
- **WHEN** the active model does not support the image input path
- **THEN** the composer SHALL identify the incompatibility and offer removal or a compatible model rather than silently dropping the image

#### Scenario: Capability is unknown
- **WHEN** the catalog omits a capability required by a feature
- **THEN** the dependent feature SHALL remain disabled until explicit metadata or evaluation establishes support

### Requirement: Model profiles assign models by role
Users SHALL be able to define a project or global model profile for Agent, plan,
apply/edit, review, fast utility, embeddings, and reranking roles.

#### Scenario: Simple profile uses one model
- **WHEN** the user selects a model valid for every enabled role
- **THEN** the system SHALL avoid unnecessary additional model calls and SHALL show that one-model profile clearly

#### Scenario: Role-specific profile is selected
- **WHEN** plan, edit, review, embedding, or naming uses a different model
- **THEN** every call SHALL record its role, model, reason, usage, and cost source in the turn trace

#### Scenario: Role model becomes unavailable
- **WHEN** an optional role model cannot be used
- **THEN** the system SHALL follow an explicit profile fallback or pause; it SHALL not silently substitute an unevaluated model

### Requirement: Model changes preserve truthful session lineage
Model switching SHALL occur only at a turn boundary and SHALL use compatible
continuation, portable fork, or explicit refusal.

#### Scenario: Compatible model is selected for next turn
- **WHEN** runtime and context state are portable without dropping required items
- **THEN** the next turn SHALL use the new model and the session SHALL record old model, new model, user action, and effective capability changes

#### Scenario: Provider protocol changes
- **WHEN** continuing would require replaying incompatible or provider-opaque state
- **THEN** the system SHALL create a linked child session with a reviewable portable context package and SHALL preserve the source session unchanged

#### Scenario: Required capability would be lost
- **WHEN** the requested model cannot support active tools, attachments, context size, or policy
- **THEN** switching SHALL be blocked with the exact capability mismatch and compatible alternatives

### Requirement: Provider-opaque reasoning is not transferred
The runtime SHALL NOT copy encrypted reasoning, hidden thinking, cache handles,
provider response IDs, or other opaque continuation state across providers.

#### Scenario: Session forks across provider families
- **WHEN** portable context is assembled
- **THEN** only user-visible messages, approved summaries, plan/task state, selected context, and necessary tool results SHALL transfer

#### Scenario: Opaque state is required for continuation
- **WHEN** the destination cannot continue without provider-specific state
- **THEN** the system SHALL explain that limitation and require a summarized fork or cancellation of the switch

### Requirement: Context and usage status are authoritative or qualified
The workbench SHALL display context limits, token usage, cost, and reasoning state
with source and uncertainty.

#### Scenario: Provider reports exact usage
- **WHEN** the response supplies authoritative input, output, cache, and reasoning usage
- **THEN** the session SHALL record and display it as observed provider usage

#### Scenario: Only local estimation is available
- **WHEN** no authoritative usage is returned
- **THEN** any estimate SHALL be labeled estimated with tokenizer/source and SHALL not be used as exact billing data

#### Scenario: Context limit is unknown
- **WHEN** neither catalog nor provider supplies a verified limit
- **THEN** the UI SHALL display unknown and use a conservative policy rather than a fabricated percentage

### Requirement: Routing and fallback are visible
Aegisy routing SHALL select provider instances or fallback models only according
to declared policy, and every effective change SHALL be observable.

#### Scenario: Provider instance fails over without model change
- **WHEN** routing moves to an equivalent provider instance
- **THEN** the trace SHALL record retry class and routing event without exposing secret infrastructure details

#### Scenario: Fallback changes model identity
- **WHEN** policy permits substitution after failure
- **THEN** the runtime SHALL emit a model-rerouted event with source model, effective model, reason, and capability differences

#### Scenario: Zero-data-retention constraint is active
- **WHEN** no eligible route satisfies the configured data policy
- **THEN** the request SHALL fail rather than using a route with weaker retention guarantees

### Requirement: Agent credentials are scoped and short-lived
The runtime SHALL receive only credentials required for the active session and
model route.

#### Scenario: Work session starts
- **WHEN** the sidecar needs Aegisy model access
- **THEN** the host SHALL request an audience, model, session, and expiry-scoped token and SHALL not pass the long-lived desktop login token to extensions or model tools

#### Scenario: Token expires during a turn
- **WHEN** safe refresh is supported
- **THEN** the sidecar SHALL request refresh through authenticated host IPC and SHALL redact both old and new credentials from events and logs
