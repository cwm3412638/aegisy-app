## Why

Aegisy already manages model access, credentials, CLI profiles, local gateway
routing, desktop clients, skills, and MCP configuration, but users still leave
the application to perform serious repository work in a separate coding agent.
The opportunity is to turn those disconnected capabilities into an Aegisy-owned
coding workspace where chat, code execution, files, Git, sessions, models, and
extensions share one trustworthy runtime and one account experience.

## What Changes

- Add an Agent Workbench as a first-class Aegisy workspace with persistent left
  navigation and two top-level modes: `Chat` for conversational assistance and
  `Work` for repository-scoped agent execution.
- Add project and session management for opening folders, pinning projects,
  naming/searching/resuming/forking sessions, and restoring the previous layout
  and runtime state after restart.
- Add a versioned, event-streaming Aegisy Agent Protocol between the desktop UI
  and replaceable agent runtimes. Initial adapters may use Codex App Server and
  ACP-compatible agents while preserving a path to an Aegisy-native runtime.
- Add an integrated coding surface with workspace tree, editor, search, diff,
  terminal, diagnostics, task plan, approvals, and live tool execution output.
- Add Git-aware workflows for status, diff, branch creation/switching, commits,
  worktrees, checkpoints, rollback, and protected destructive operations.
- Add an Aegisy model capability registry and provider routing layer that can
  select models from Aegisy's catalog, negotiate tool/reasoning/context/media
  capabilities, and handle deliberate model changes without corrupting session
  context or provider-specific reasoning state.
- Unify Skills, plugins, MCP servers, hooks, project instructions, and future
  Agent-to-Agent integrations under an inspectable extension and permission
  model.
- Add sandbox profiles, granular approvals, secret protection, untrusted-input
  handling, audit events, and explicit boundaries for local, remote, and
  background execution.
- Add background tasks and isolated subagents only after the single-agent local
  workflow is reliable, with each concurrent coding task isolated by worktree or
  equivalent workspace boundary.
- Deliver the product incrementally through gated milestones rather than a
  single rewrite of the current Qt application.

## Capabilities

### New Capabilities

- `agent-workbench-shell`: Persistent Chat/Work navigation, responsive pane
  layout, command composer, status surfaces, and keyboard-accessible workspace
  interactions.
- `project-session-management`: Folder/project lifecycle, pinned projects,
  durable session history, resume/fork/archive/search, compaction, and recovery.
- `agent-runtime-protocol`: Versioned thread/turn/item protocol, streaming
  events, runtime adapters, cancellation, steering, approvals, and crash
  reconnection.
- `workspace-tools`: File tree, editor, repository search, terminal, diagnostics,
  patches, diffs, checkpoints, and tool-result presentation.
- `git-worktree-operations`: Repository status, branches, commits, worktrees,
  conflict handling, rollback, and protected Git mutations.
- `model-provider-routing`: Aegisy model catalog, capability negotiation,
  provider adapters, context accounting, model switching, routing, and fallback.
- `agent-extension-system`: Skills, plugins, MCP, hooks, project instructions,
  discovery, installation, enablement, trust, and scoped configuration.
- `agent-security-governance`: Sandbox and permission profiles, approvals,
  secret redaction, prompt-injection boundaries, auditability, and data privacy.
- `background-multi-agent-execution`: Plans, subagent delegation, isolated
  concurrent work, background jobs, scheduling, and result integration.
- `agent-observability-evaluation`: Structured traces, token/cost/latency status,
  diagnostics, reliability metrics, replay fixtures, and release quality gates.

### Modified Capabilities

None. The Agent Workbench is introduced as a new product surface and does not
change the requirements of an archived capability spec in this repository.

## Impact

- Affects the main application shell, navigation, account/model state, secure
  storage, Skills and MCP management, local gateway, update packaging, and
  Windows/macOS release verification.
- Introduces a separate agent runtime process and a local authenticated IPC
  boundary so model execution, terminals, file operations, and sandbox policy do
  not live inside the Qt UI process.
- Requires an embedded production-grade editor and terminal strategy rather than
  extending the current profile-management widgets into a hand-built IDE.
- Requires durable project/session storage, schema migration, crash recovery,
  protocol compatibility testing, and privacy-preserving diagnostic export.
- Requires Aegisy website/API additions for a capability-rich model catalog and
  consistent Responses, Anthropic, and Gemini-compatible routing contracts.
- Adds substantial security responsibility because the product can execute code,
  modify repositories, access networks, and coordinate background agents.
- Will be implemented as independently releasable milestones with explicit entry
  and exit criteria; the existing connection-management workflows remain usable
  throughout the transition.
