# Agent Workbench Research

Research date: 2026-07-16. This document records product and architecture
patterns, not permission to copy branding, private behavior, prompts, or assets.

## Research Questions

1. Which interaction model makes an agent feel like a coding workspace rather
   than a chat dialog attached to a settings application?
2. Which runtime boundaries allow Aegisy to use existing agents early without
   giving up control of the product or locking sessions to one vendor?
3. Which context, Git, approval, and recovery capabilities are table stakes for
   trustworthy repository work?
4. How should Aegisy expose many model vendors without pretending their APIs,
   reasoning state, tools, and context limits are interchangeable?
5. Which autonomous-agent patterns are valuable, and which are too dangerous to
   adopt before sandboxing and evaluation are mature?

## Source Comparison

| System | Useful strengths | Adopt for Aegisy | Do not copy directly |
| --- | --- | --- | --- |
| OpenAI Codex | Rich clients are driven through a bidirectional App Server protocol. The public protocol models work as Thread, Turn, and Item; streams plans, diffs, commands, reasoning summaries, token usage, approvals, cancellation, steering, compaction, thread forks, filesystem events, Skills, hooks, plugins, and MCP state. | Use an event-streaming thread model and build the UI against an Aegisy-owned protocol. Implement a Codex App Server adapter for the first production Agent runtime. | Do not make the UI depend on one installed Codex version or expose experimental protocol fields without an adapter and compatibility tests. Do not assume every provider supports Responses-specific state. |
| Kimi CLI | Separates CLI/UI, Wire events, the Soul loop, context, toolset, approvals, provider abstraction, OS abstraction, and ACP. Persists session state and subagents, supports checkpoints and compaction, and implements OpenAI Responses, Anthropic, Gemini, and Kimi provider types. | Use a UI/runtime separation, per-session approval state, capability-based providers, ACP adapter, context checkpoints, and an OS abstraction that can later target SSH or sandboxes. | Do not make provider configuration a loose collection of model-name heuristics. Do not allow restored session approvals to silently exceed current workspace policy. |
| Claude Code | Strong human workflow discipline through exploration, clarifying questions, architecture alternatives, implementation approval, specialized agents, quality review, hooks, commands, Skills, and plugins. | Make Plan and Work explicit product modes. Provide reusable workflow templates and specialized explorer/architect/reviewer roles with visible handoffs and user gates. | The core Claude Code implementation is not open source; its repository license reserves rights. Only public behavior, documentation, examples, and separately licensed interfaces can be referenced. Do not copy private prompts or implementation. |
| OpenClaw | Local-first gateway, durable workspace, multi-channel session routing, background automation, Skills, tools, companion apps, nodes, and isolated agents. It treats the gateway as a control plane and the app as one optional client. | Reuse the control-plane idea for long-running jobs, device/remote runtimes, and one inspectable session model across interactive and background work. Require per-agent workspace isolation. | Its main local session can run host tools with broad access. A coding product must not inherit that default. Remote messages, repository content, tool output, and web pages must all be treated as untrusted input. |
| Cline | Plan/Act split, inline approvals, live terminal output, browser-assisted testing, diagnostics, edit diffs, task checkpoints, restore, model/provider switching, MCP, and project rules. | Treat reversible checkpoints, visible diffs, diagnostics, and approval ergonomics as MVP quality, not advanced features. Keep mode and model state explicit. | Do not use hidden Git commits as the only checkpoint representation, and do not let provider switching reset mode or task state. |
| Aider | Tight Git integration, automatic safety commits, `/diff` and `/undo`, architect/editor model roles, and a compact tree-sitter repository map ranked to a token budget. | Build a symbol-aware repository map and context budgeter. Separate reasoning/architecture and deterministic edit application when a model benefits from that split. | Do not auto-commit user work without an explicit project policy. Avoid adding the entire repository to every prompt; large context can reduce quality and increase cost. |
| OpenHands | Immutable typed events form an append-only execution log, while workspaces isolate execution and the same client can target local or remote agent servers. | Use append-only runtime events plus derived views, and make local/remote execution a workspace implementation detail behind the same protocol. | Do not start with a remote-control-plane dependency. Local repository trust, latency, and offline failure modes must be solved first. |
| Continue | Assigns models to roles such as chat, edit, apply, autocomplete, embeddings, and reranking instead of using one global model for every job. | Extend Aegisy's model catalog with role suitability and capability metadata. Allow a project profile to select different models for interactive agent, fast apply, naming, embeddings, and review. | Do not infer role suitability from vendor or price alone. Aegisy needs evaluated recommendations and user-visible overrides. |
| Roo Code | Specialized modes and Boomerang/Orchestrator tasks decompose work into focused child tasks with separate context. | Add explicit parent/child task lineage, bounded instructions, result summaries, budgets, and isolated worktrees after single-agent reliability. | Do not let child tasks share one mutable checkout or unconstrained extension set. Orchestration without isolation makes review and rollback unreliable. |
| OpenCode / Goose | Broad provider support, client/server or desktop/CLI separation, recipes, ACP, MCP, headless work, and provider routing options. Goose's public design discussion also highlights why one shared Agent across sessions causes state leakage and concurrency problems. | Keep one Agent instance and extension/approval state per session. Route chat, background jobs, recipes, and subagents through one execution pipeline with different policies. | Do not build separate execution paths for chat, scheduled jobs, and subagents. Behavior drift becomes difficult to secure and test. |
| ACP | A standard JSON-RPC protocol between code editors and coding agents, supporting local and remote clients. | Implement ACP as an adapter boundary and contribute compatible extensions where possible. Keep Aegisy-specific operations namespaced and optional. | ACP is not a complete product data model for Aegisy's account, model catalog, Git graph, diagnostics, or background orchestration. Do not force every feature into the standard. |

## Sources

- [OpenAI Codex repository](https://github.com/openai/codex)
- [Codex App Server protocol](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md)
- [Codex MCP/runtime protocol overview](https://github.com/openai/codex/blob/main/codex-rs/docs/codex_mcp_interface.md)
- [Kimi CLI architecture guidance](https://github.com/MoonshotAI/kimi-cli/blob/main/AGENTS.md)
- [Kimi session and context model](https://github.com/MoonshotAI/kimi-cli/blob/main/docs/zh/guides/sessions.md)
- [Kimi provider and capability model](https://github.com/MoonshotAI/kimi-cli/blob/main/docs/zh/configuration/providers.md)
- [Kimi ACP integration](https://github.com/MoonshotAI/kimi-cli/blob/main/docs/zh/reference/kimi-acp.md)
- [Claude Code public repository](https://github.com/anthropics/claude-code)
- [Claude feature-development workflow](https://github.com/anthropics/claude-code/tree/main/plugins/feature-dev)
- [OpenClaw repository and security defaults](https://github.com/openclaw/openclaw)
- [OpenClaw architecture](https://docs.openclaw.ai/concepts/architecture)
- [Cline repository](https://github.com/cline/cline)
- [Aider repository map](https://aider.chat/docs/repomap.html)
- [Aider Git integration](https://aider.chat/docs/git.html)
- [OpenHands event architecture](https://docs.openhands.dev/sdk/arch/events)
- [OpenHands agent-server overview](https://docs.openhands.dev/sdk/guides/agent-server/overview)
- [Continue model roles](https://docs.continue.dev/customize/model-roles)
- [Roo Code Boomerang tasks](https://roocodeinc.github.io/Roo-Code/features/boomerang-tasks/)
- [Agent Client Protocol overview](https://agentclientprotocol.com/protocol/v1/overview)
- [OpenCode providers](https://opencode.ai/docs/providers)
- [Goose custom distribution interfaces](https://github.com/aaif-goose/goose/blob/main/CUSTOM_DISTROS.md)

## Product Principles Derived From Research

1. **Protocol before presentation.** Every visible state must come from a typed
   runtime event or durable project/session record; the UI must not scrape terminal
   text to guess what the Agent is doing.
2. **One session, one isolated Agent state.** Model, tools, approvals, terminals,
   worktree, context, and child tasks belong to a session and cannot leak into
   another session.
3. **Conversation and work are different promises.** Chat can reason and explain
   without workspace mutation. Work can use tools, but only under a visible
   workspace, permission profile, and reviewable change set.
4. **Reversibility is a core feature.** Every Agent mutation must be attributable,
   reviewable, and recoverable through structured patches, checkpoints, or Git.
5. **Capabilities beat model-name rules.** The model catalog must describe
   protocols, context limits, tool behavior, reasoning, modalities, and role
   suitability. Unknown capability remains unknown and disables unsafe features.
6. **Context is a budgeted product subsystem.** Pinned files, repository map,
   search results, instructions, tool output, and summaries need provenance,
   token budgets, and user inspection.
7. **Extensions are executable supply chain.** Skills, hooks, MCP servers, and
   plugins require origin, version, trust state, permission scope, and revocation.
8. **Autonomy follows reliability.** Background and multi-agent execution are not
   enabled by default until single-agent edits, approvals, rollback, crash recovery,
   and evaluations meet defined release gates.
9. **Local first, remote compatible.** The first trusted runtime is local. Protocol
   and workspace abstractions may later support SSH, containers, or Aegisy cloud
   without changing the UI session model.
10. **Aegisy remains the model control plane.** Provider credentials, entitlement,
    catalog metadata, routing policy, usage, and billing remain Aegisy services;
    the Agent runtime receives only short-lived scoped credentials.

## Licensing and Product Identity

- Codex and Kimi CLI publish Apache-2.0 source; OpenClaw publishes MIT source.
  Reuse still requires attribution, NOTICE handling, dependency review, and a
  deliberate choice between linking, process integration, or original code.
- Claude Code's repository states that use is subject to Anthropic commercial
  terms and does not publish the core implementation under an open-source license.
  Aegisy can learn from documented workflows and integrate supported interfaces,
  but cannot copy its implementation or private prompts.
- Aegisy SHALL use its own names, icons, interaction copy, protocol types, and
  visual system. The screenshots define workflow intent, not assets to reproduce.
