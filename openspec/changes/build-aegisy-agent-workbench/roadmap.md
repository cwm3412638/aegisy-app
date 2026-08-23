# Product Roadmap

## Active Roadmap Reset (2026-08-23)

The active product is an Aegisy website companion, not a replacement IDE. Delivery
is ordered around the shortest path from an Aegisy account to a verified local
environment:

```text
C0 Website/account contract and secure local projection
  |
  v
C1 One-click CLI/profile configuration, verification, repair, and rollback
  |
  +--------------------+
  v                    v
C2 Plugins/Skills/MCP  C3 Desktop enhancement and Chinese UX
  |                    |
  +----------+---------+
             v
C4 Bounded Codex programming surface
             |
             v
C5 Companion-focused signed macOS/Windows release
```

The companion release gates are login, authenticated configuration metadata,
per-tool config isolation, backup/restore, diagnostics, extension provenance,
localization rollback, gateway/update safety, and Codex launch/recovery. Claude,
Gemini, OpenCode, and other tools may be configured without becoming embedded
Agent runtimes. Their existing configuration compatibility may be maintained where
needed by the one-click flow, but new Claude/Gemini programming work is outside the
active roadmap.

ACP, Claude/Gemini Agent adapters, full multi-provider Agent routing, Agent-authored
mutation, background agents, and full IDE replacement are deferred. The completed
Workbench foundations remain in the repository and retain their safety tests, but
they do not drive near-term staffing or block a companion release unless shipped as
reachable code.

## Deferred Workbench Roadmap Reference

## Planning Assumptions

This is not a normal feature addition. It combines a rich desktop workbench,
cross-platform process/sandbox runtime, Agent protocol, provider integration,
durable data, Git safety, extension supply chain, and Aegisy backend work.

The first estimates must be recalibrated after Milestone 0. A reasonable delivery
team is:

- 1 product/design owner with strong developer-tool experience.
- 2 desktop/workbench engineers across Qt, TypeScript, Monaco, and xterm.js.
- 2 runtime engineers across Rust, IPC, PTY, filesystem, Git, sandbox, and Agent
  protocol adapters.
- 1 backend engineer for model catalog, scoped tokens, routing metadata, usage,
  and release controls.
- Shared QA automation, security, release engineering, and legal review.

The complete program is roughly 80-130 engineer-weeks before operational support
and ongoing model/runtime compatibility. A focused team of 4-6 engineers should
expect approximately 9-15 months to reach a defensible stable local product; a
single engineer should expect 18-30 months and should narrow the first stable
scope. These are planning ranges, not commitments.

## Dependency Order

```text
M0 Feasibility and architecture gates
  |
  v
M1 Read-only shell + projects + sessions + protocol/storage
  |
  v
M2 Single-agent local Work: terminal + patches + approvals + recovery
  |
  +-------------------+
  v                   v
M3 Git/worktrees      M4 Aegisy model catalog/routing
  |                   |
  +---------+---------+
            v
M5 Extensions + security hardening + evaluations
            |
            v
M6 Child agents and background jobs
            |
            v
M7 Stable productization and default landing migration
```

No milestone may claim completion from UI screenshots alone. Its exit evidence
must include protocol, persistence, security, failure, cross-platform, and signed
package verification appropriate to its scope.

## Milestone 0: Feasibility and Architecture Gates

**Purpose:** Prove the architecture before committing the existing client to a
large dependency or runtime strategy.

**Indicative effort:** 6-10 engineer-weeks across desktop, runtime, security, and
release work.

**Scope:**

- Embedded Qt WebEngine versus standalone Tauri workbench spike.
- Monaco, xterm.js, Chinese IME, accessibility, high-DPI, signed packaging, and
  updater measurements.
- AAP schema and generated Rust/TypeScript/C++ fixture.
- Authenticated local IPC and supervised sidecar proof.
- Codex App Server event/approval/diff adapter proof.
- Kimi CLI or another ACP agent multi-session proof.
- macOS and Windows sandbox feasibility report.
- Model catalog and scoped token API draft.
- Licensing and redistribution review.

**Exit gates:**

- One UI technology passes explicit size, memory, latency, IME, accessibility,
  packaging, and recovery budgets.
- A real Agent can start a session, stream an answer/plan/command/diff, request an
  approval, cancel, and reconnect through AAP without the UI consuming vendor
  protocol events.
- A sidecar rejects unauthenticated local clients.
- Storage and schema approaches survive an injected crash and restart fixture.
- Security and release owners approve a viable sandbox path for both platforms.
- Product owner approves Chat/Work contract and first stable scope.

**Stop condition:** If no UI option meets packaging/IME/accessibility budgets or
no Windows sandbox path is viable, narrow the product to a protocol-connected
standalone preview before writing the main implementation.

## Milestone 1: Read-Only Agent Workbench Preview

**Purpose:** Ship a safe internal product surface that proves projects, sessions,
events, context, model selection, and recovery without repository mutation.

**Indicative effort:** 10-14 engineer-weeks.

**Scope:**

- Product rail, Chat/Work navigation, project open/trust, pinned projects, and
  session list/search/resume/fork/archive.
- Typed Agent timeline, composer context strip, attachments, cancellation, errors,
  token/context status, and renderer recovery.
- Sidecar, AAP, SQLite/event journal, replay, migration, and diagnostics.
- Read-only file tree/editor/search, repository map, context inspector, and Git
  status/log/diff.
- Codex adapter and one ACP adapter in read-only permission profile.
- Aegisy model catalog preview with capability gating and no silent fallback.

**Exit gates:**

- Application restart, renderer crash, sidecar crash, network loss, and adapter
  restart preserve truthful session state.
- Two concurrent sessions do not leak model, extension, approval, or context state.
- Read-only policy resists shell/file-write attempts through model output, repo
  injection, and malformed adapter events.
- Large repository search, timeline virtualization, and editor open meet budgets.
- Existing Aegisy management, login, gateway, logout, and updater remain usable.

## Milestone 2: Local Single-Agent Work MVP

**Purpose:** Deliver the first genuinely useful coding Agent with tight user
control. This is the earliest milestone that may be described as vibe coding.

**Indicative effort:** 14-20 engineer-weeks.

**Scope:**

- macOS PTY and Windows ConPTY, foreground/background commands, structured output,
  stop/restart, and observed diagnostics.
- Structured patches, version hashes, diff preview, stale edit detection, atomic
  apply, checkpoint, selective restore, and non-Git fallback.
- Inline command/file/network/permission approvals and policy profiles.
- Initial sandbox enforcement through the selected runtime adapter.
- Context budget, instructions, pinned context, repository map, and compaction.
- End-to-end one-session workflow: explore, plan, approve, edit, run tests, inspect
  diff, cancel/retry, and recover after restart.

**Exit gates:**

- No fixture can write outside roots, follow denied symlinks, read configured
  secrets, bypass approval, or retry outside sandbox.
- Partial write, disk-full, stale patch, process crash, and rollback tests preserve
  authoritative file state.
- Agent claims are not rendered as verified tests without observed command evidence.
- Signed macOS and Windows preview packages pass terminal, sandbox, IME, scale,
  update, and rollback smoke tests.
- Repository task corpus meets minimum patch validity, test pass, correction, and
  approval-burden thresholds selected before evaluation.

## Milestone 3: Git and Isolated Workflows

**Purpose:** Make branches, commits, worktrees, and recovery first-class rather
than relying on free-form shell commands.

**Indicative effort:** 8-12 engineer-weeks.

**Scope:**

- Structured Git state, branch lifecycle, staging, commit, stash, conflict, merge,
  rebase, cherry-pick, push preview, and risk policy.
- Separate user dirty state from Agent changes.
- Worktree creation and cleanup associated with sessions.
- Git-aware checkpoints and reviewed result integration.

**Exit gates:**

- High-risk Git actions cannot inherit blanket auto-approval.
- Shell-based Git commands receive equivalent risk classification.
- User dirty state survives task start, checkpoint, rollback, branch switch,
  conflict, crash, and child integration fixtures.
- Real repository matrix covers linked worktrees, submodules, LFS, hooks, detached
  HEAD, conflicts, and remote failure.

## Milestone 4: Aegisy Multi-Provider Model Control Plane

**Purpose:** Turn Aegisy's model access into a differentiated, honest capability
layer rather than a dropdown of provider names.

**Indicative effort:** 10-16 engineer-weeks across client, runtime, backend, and
evaluation.

**Scope:**

- Signed model catalog, source-qualified limits/capabilities/roles, cache expiry,
  compatibility, and deprecation.
- Project model profiles and optional Agent/plan/apply/review/embedding roles.
- Short-lived scoped Agent tokens and usage correlation.
- Compatible next-turn switching and portable cross-provider session forks.
- Transparent routing, reroute, fallback, zero-data-retention policy, and errors.
- Provider/runtime contract fixtures for Responses, Chat Completions, Anthropic,
  and Gemini routes exposed by Aegisy.

**Exit gates:**

- Unknown capabilities disable dependent features; no model-name heuristic can
  enable write tools, modalities, or context limits.
- Cross-provider switching never transfers opaque reasoning or cache state.
- Usage and cost distinguish observed, estimated, stale, and unknown data.
- Role recommendations are tied to a reproducible evaluation version.
- Catalog or routing outage produces a safe stale/offline state and never an
  unreviewed model substitution.

## Milestone 5: Extension Platform and Security Hardening

**Purpose:** Unify existing Aegisy Skills/MCP features with hooks and plugins
under an executable supply-chain and permission model.

**Indicative effort:** 10-14 engineer-weeks.

**Scope:**

- Unified extension registry, scope precedence, hash trust, signed marketplace,
  install/update/rollback, and managed policy.
- Skills, nested project instructions, MCP lifecycle/OAuth/elicitation, hooks,
  plugins, and runtime adapter distinction.
- Secret redaction, untrusted-data provenance, audit, diagnostic export, and
  security dashboard.
- Expanded adapter replay, migration, adversarial, and repository evaluation.

**Exit gates:**

- Executable content cannot run before trust and loses trust when content or
  permission requests change.
- Extensions cannot leak across projects/sessions/children or exceed their scope.
- Malicious extension fixtures cannot obtain protected secrets, forge approval,
  disable managed hooks, or block the event loop with output/timeouts.
- Existing Skills/MCP migration is previewable, reversible, and does not delete
  source configuration.

## Milestone 6: Child Agents and Background Jobs

**Purpose:** Add controlled concurrency and unattended work only on top of a
reliable single-Agent foundation.

**Indicative effort:** 12-20 engineer-weeks.

**Scope:**

- Structured plans, child-task contracts, parent/child lineage, worktree isolation,
  scoped extensions, model profiles, and bounded handoffs.
- Runtime-enforced token/cost/time/turn/tool/concurrency/network budgets.
- Unified interactive/child/background executor and durable job queue.
- Pause/cancel/retry/recover, approval-needed notification, and reviewed branch
  integration.

**Exit gates:**

- Concurrent write tasks cannot share mutable checkout state.
- Child permissions and extensions are explicit subsets and cannot grow silently.
- Budget exhaustion stops work without unknown spend being treated as zero.
- Crash, orphan process, cost runaway, cross-session leakage, stale target, and
  integration conflict suites pass.
- A separate product/security approval enables each stable autonomy mode.

## Milestone 7: Stable Productization

**Purpose:** Make the workbench the coherent default Aegisy product without
removing recovery or legacy management paths prematurely.

**Indicative effort:** 8-12 engineer-weeks plus release observation.

**Scope:**

- Onboarding, sample/first project, empty/offline/error/recovery states, docs,
  support tools, and privacy controls.
- Signed packaging, sidecar/runtime compatibility, update/rollback, emergency
  disable, and operational dashboards.
- Account/model/Skills/MCP/settings consolidation and deliberate legacy workflow
  migration.
- Accessibility, localization, keyboard, high-DPI, performance, retention, and
  enterprise policy completion.

**Exit gates:**

- Stable usage shows acceptable task success, crash-free sessions, correction,
  approval burden, support load, cost clarity, and retention behavior.
- Signed Windows/macOS update and rollback preserve projects, sessions, existing
  profile configuration, and secure credentials.
- Workbench startup failure cannot prevent login or legacy recovery.
- Product, security, privacy, support, and release owners approve default landing
  migration. Removing legacy behavior requires a later OpenSpec.

## Suggested First Build Slice

The first implementation change after this proposal should be limited to
Milestone 0 plus the following vertical proof:

1. Open one local folder as a read-only project.
2. Start one Codex-backed session through AAP.
3. Stream user/agent/plan/command-proposal items into the fixed Agent surface.
4. Deny the command under Read Only and show the structured approval request.
5. Persist events, restart UI and sidecar, replay the same session, and verify no
   duplicate turn or mutation.
6. Switch to one ACP-backed runtime and show unsupported capabilities explicitly.

This slice validates the hardest boundaries without committing to the editor,
Git mutation, multi-provider switching, or background autonomy all at once.
