## Why

Aegisy's website already owns account access, model availability, API keys, and
service policy. The desktop application is most valuable when it turns that web
state into a working local environment without requiring users to understand each
tool's config format, plugin layout, localization mechanism, or Skills directory.

The previous plan made a complete multi-runtime coding workbench the primary
product. That created a much larger IDE, sandbox, Git, protocol, and provider
program than the near-term product needs. The active direction is now an Aegisy
website companion: one-click configuration, local environment repair, desktop
enhancements, plugins, Chinese UX, Skills, MCP, backup, diagnostics, and updates.

Codex remains the only near-term integrated programming runtime. Claude and Gemini
remain valid local configuration targets, but their Agent/workbench adapters are
deferred. Existing Workbench foundations are retained as reusable code and safety
evidence; they are not the default roadmap or a release dependency unless they
directly support the companion workflow or the bounded Codex surface.

## What Changes

- Make the configuration center the primary desktop experience for applying Aegisy
  website profiles to supported local tools with preview, backup, verification, and
  repair.
- Treat Claude Code, Gemini CLI, OpenCode, and Codex CLI as configuration targets;
  this does not imply that Aegisy implements each tool as an embedded coding Agent.
- Consolidate Codex plugins, desktop enhancements, Chinese-language support,
  custom Skills, MCP configuration, diagnostics, and updates into an inspectable
  extension workflow.
- Keep local gateway routing, secure storage, profile switching, configuration
  backup/restore, and system doctor behavior as first-class companion features.
- Keep a bounded, optional `Codex 编程` surface using the existing pinned Codex
  adapter and current read-only safety boundary until write/approval gates are
  deliberately resumed.
- Defer ACP, Claude Agent, Gemini Agent, native multi-provider Agent routing,
  background agents, and full IDE/Git mutation milestones. Deferred work must stay
  unavailable rather than appearing as partially working UI.
- Rebaseline packaging and release evidence around the companion workflow first;
  advanced Workbench gates no longer block a companion release unless their code is
  shipped and reachable in that release channel.

## Capabilities

### New Capability

- `aegisy-companion-control-center`: authenticated website-to-local configuration,
  environment detection and repair, extension/Skills management, localization,
  backup, diagnostics, and a clearly bounded Codex-only programming entry.

### Retained Capabilities

- Existing profile, gateway, updater, secure-storage, desktop-download,
  enhancement, plugin, Skills, MCP, and diagnostics capabilities remain active.
- Existing AAP, Store, terminal, editor, Git-read, and Codex adapter work remains
  checked in and testable. Only the Codex subset explicitly exposed by the active
  companion scope is a near-term product surface.

### Deferred Capabilities

- ACP and non-Codex Agent adapters.
- Claude/Gemini embedded programming runtimes.
- Full multi-provider Agent routing and portable cross-runtime sessions.
- Agent-authored file/Git mutation, unrestricted command execution, background
  agents, and full IDE replacement work.

## Impact

- The main Qt application remains the product host and configuration center.
- The website/API contract becomes the source for account and distributable
  configuration metadata; secrets remain in secure storage and never enter logs.
- The existing Rust sidecar and Workbench code are preserved, but near-term changes
  must justify themselves against the companion or Codex-only scope.
- macOS and Windows release validation prioritizes login, one-click configuration,
  config rollback, plugin/Skills/localization workflows, gateway, updater, and
  Codex launch/recovery.
- This reset removes planned scope; it does not grant new mutation, Approval,
  command, filesystem, Git, remote, or background authority.
