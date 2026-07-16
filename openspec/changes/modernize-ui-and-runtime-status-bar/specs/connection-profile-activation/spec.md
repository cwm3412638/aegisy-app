## ADDED Requirements

### Requirement: Bulk activation selects profiles
Bulk activation SHALL configure each tool from the credential and model stored
by the exact profile selected by the user.

#### Scenario: User selects multiple profiles
- **WHEN** the user applies bulk activation for any subset of Claude, Codex, Gemini, and OpenCode profiles
- **THEN** each successful tool configuration SHALL match its selected profile and that exact profile SHALL become active

#### Scenario: Configuration write fails
- **WHEN** a selected tool configuration cannot be written
- **THEN** its previous active profile and disk configuration SHALL remain authoritative and the failure SHALL be reported

### Requirement: Four-tool parity
Tool status and activation workflows SHALL include Claude, Codex, Gemini, and
OpenCode wherever the operation applies to all supported CLI tools.

#### Scenario: OpenCode profile exists
- **WHEN** an OpenCode profile is configured
- **THEN** bulk activation, version status, health inspection, tray switching, and runtime status SHALL recognize OpenCode

### Requirement: Codex third-party capability compatibility
Codex profile activation SHALL generate a custom Aegisy provider configuration that
does not require OpenAI account authentication, supplies the Aegisy capability header,
and explicitly enables live web search.

#### Scenario: User activates a Codex profile
- **WHEN** a direct or local-gateway Codex profile is activated
- **THEN** the active provider SHALL use `requires_openai_auth = false`, bearer authentication, `x-openai-actor-authorization = aegisy`, and `web_search = live`

#### Scenario: User activates GPT-5.6 Sol
- **WHEN** the selected model is `gpt-5.6-sol` or its `gpt-5.6` alias
- **THEN** Codex SHALL use a 372000-token client context threshold and high reasoning effort
