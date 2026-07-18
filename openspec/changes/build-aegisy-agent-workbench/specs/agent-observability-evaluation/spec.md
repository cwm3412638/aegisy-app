## ADDED Requirements

### Requirement: Every turn has a structured local trace
The system SHALL record runtime/model selection, context manifest, event timing,
tools, approvals, usage, changed paths, commands/tests, errors, retries, and final
state with correlation IDs.

#### Scenario: Turn completes successfully
- **WHEN** a terminal event is persisted
- **THEN** the trace SHALL identify the evidence supporting completion, including authoritative file/Git state and observed verification commands

#### Scenario: Turn fails across adapter boundary
- **WHEN** the vendor runtime, provider, transport, sandbox, tool, or AAP adapter fails
- **THEN** the trace SHALL preserve both the stable AAP error class and redacted underlying source classification

#### Scenario: Trace contains large or sensitive content
- **WHEN** terminal output, diff, prompt, or file content is not required as metadata
- **THEN** the trace SHALL store a bounded hash/reference or redacted summary according to local retention policy

### Requirement: Usage and cost reporting names its source
Token, cache, reasoning, context, latency, and cost data SHALL be labeled observed,
catalog-derived, locally estimated, stale, or unknown.

#### Scenario: Provider reports usage
- **WHEN** exact response usage is available
- **THEN** the trace and status surfaces SHALL display it as observed for that request and accumulate it without double counting retries

#### Scenario: Cost metadata is stale
- **WHEN** the catalog price or routing charge cannot be verified for the event time
- **THEN** the system SHALL avoid an authoritative cost total and SHALL identify the missing source

#### Scenario: Retry uses another model
- **WHEN** routing changes effective model
- **THEN** usage SHALL be attributed to each actual attempt and the reroute SHALL be visible

### Requirement: Diagnostic export is previewable and redacted
Users SHALL be able to export protocol, runtime, crash, environment, and feature
state without automatically exporting repository or conversation content.

#### Scenario: User creates default diagnostic bundle
- **WHEN** export starts with default privacy settings
- **THEN** it SHALL include versions, capabilities, states, error classes, redacted events, hashes, and timing while excluding prompts, code, paths, diffs, terminal content, and secrets

#### Scenario: Support requires content
- **WHEN** the user opts into a content category
- **THEN** the preview SHALL enumerate affected sessions/items and SHALL allow removal before the bundle is written

### Requirement: Runtime adapters have replay fixtures
Every adapter SHALL include redacted deterministic fixtures for initialization,
session lifecycle, streaming items, approvals, cancellation, errors, and reconnect.

#### Scenario: Supported runtime is upgraded
- **WHEN** its schema or observed event stream changes
- **THEN** compatibility fixtures SHALL be regenerated/reviewed and stable AAP mappings SHALL pass before release

#### Scenario: Event ordering varies legally
- **WHEN** the vendor protocol permits concurrent item events
- **THEN** fixture and property tests SHALL verify valid ordering without assuming one captured timing sequence

### Requirement: Workspace safety has adversarial tests
Release validation SHALL include path escape, symlink, stale patch, encoding,
large output, command parsing, secret leakage, injected instructions, and sandbox
bypass cases.

#### Scenario: New file or tool implementation is added
- **WHEN** it expands filesystem, process, network, extension, or model access
- **THEN** its threat cases and policy tests SHALL be added before write-capable release

#### Scenario: Security test fails
- **WHEN** a protected resource is accessed or an approval is bypassed
- **THEN** the affected release gate SHALL fail regardless of ordinary functional test success

### Requirement: Agent quality is evaluated on repository tasks
The project SHALL maintain a representative corpus measuring task completion,
regressions, tests, user correction, approval burden, latency, and cost across
supported models and platforms.

#### Scenario: Model recommendation changes
- **WHEN** Aegisy marks a model suitable for Agent, plan, apply, or review roles
- **THEN** the recommendation SHALL reference current evaluation results and known runtime/tool limitations

#### Scenario: Faster model lowers reliability
- **WHEN** latency improves but completion, patch validity, or regression metrics degrade below policy
- **THEN** it SHALL not become the default for that role solely because it is faster or cheaper

#### Scenario: Evaluation repository contains secrets or licenses
- **WHEN** fixtures are added
- **THEN** provenance, redistribution rights, secret scanning, and deterministic reset SHALL pass before inclusion

### Requirement: Release gates cover cross-platform product behavior
Every milestone SHALL define required build, test, sandbox, migration, recovery,
accessibility, IME, high-DPI, performance, packaging, and update evidence.

#### Scenario: macOS validation passes but Windows is unverified
- **WHEN** the milestone claims Windows support
- **THEN** promotion SHALL wait for its Windows sandbox, terminal, filesystem, Git, signing, installer, update, and scaling matrix

#### Scenario: Database migration is introduced
- **WHEN** persisted schema changes
- **THEN** upgrade, rollback/read-only recovery, backup, and corrupted-input tests SHALL pass from every supported prior schema

#### Scenario: Workbench performance regresses
- **WHEN** startup, memory, terminal throughput, editor latency, timeline rendering, or index CPU exceeds its milestone budget
- **THEN** the release SHALL block or explicitly reduce the enabled scope before promotion
