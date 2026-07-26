# Aegisy Workbench Visible-State Matrix

Status: pre-release review inventory, 2026-07-26

This matrix is the maintained audit surface for OpenSpec `23.10`. It covers only
states that currently exist in the Qt Workbench. An `automated` verdict applies
only to the transitions named in that row's Evidence cell; other listed variants
remain inventory until they receive an explicit fixture. It does not imply Windows
evidence, accessibility review, localization review, or stable-release approval.

Run the current automated evidence with:

```bash
cmake --build build --target AegisyAgentWorkbenchRenderTest -j4
ctest --test-dir build --output-on-failure -R '^agent_workbench_render$'
```

The durable protocol and Store behavior behind recovery states remains covered by
the Rust and protocol suites listed in `PROJECT-MEMORY.md`; this document does not
promote a visual assertion into protocol or persistence evidence.

## State Coverage

| Class | Current visible variants | Stable Qt locator | Evidence | Current verdict |
| --- | --- | --- | --- | --- |
| Empty | New Chat timeline; no editor tabs; no context; no terminal; no structured changes; hidden healthy recovery banners | `agentEmptyTimeline`, `agentEditorTabs`, `agentContextPanel`, `agentTerminalStatus`, `agentWorkspaceEditSummary`, `agentRecoveryBanner` | Render test asserts text, visibility, empty counts, disabled actions, and read-only previews | Automated on the current macOS offscreen renderer |
| Loading | Runtime connecting; capability negotiation pending; workspace search/index pending; history page pending; terminal attach pending; Timeline sync/snapshot/subscription pending | `agentRuntimeStatus`, `agentRuntimeCapabilityStatus`, `agentWorkspaceSearchStatus`, `agentSessionHistoryMoreButton`, `agentTerminalStatus`, `agentSendButton` | Render test asserts capability `能力检查中`, disabled new-Turn paths, and Timeline recovery gates. Search/index progress, history loading, and terminal attach pending still need explicit visible-state assertions | Partially automated; slow-device timing, the named pending variants, and Windows rendering remain open |
| Offline | Runtime transport disconnected; heartbeat liveness Unknown; unavailable project root; unavailable language server | `agentRuntimeStatus`, `agentFailureNotice`, `agentProjectList`, `agentLanguageStatus` | A synthetic Workbench signal fixture asserts `运行时离线`, failure notice, disabled Send, and visible status restoration. Separate Runtime/Qt reconnect fixtures in `PROJECT-MEMORY.md` cover heartbeat Unknown, retained Stop, generation replacement, and recovery barriers; the synthetic fixture is not transport evidence | Partially automated; production provider/catalog offline and Windows installer offline behavior remain open |
| Permission | Default Agent read-only; missing Runtime binding; whole-store read-only recovery; unavailable mutation capability; user editor save remains separately scoped | `agentExecutionContextStrip`, `agentRuntimeCapabilityStatus`, `agentSendButton`, `agentRecoveryBanner`, `agentEditorSaveButton` | Render test asserts `权限 只读`, unknown binding stays a read-only gate, missing capabilities disable Send, recovery disables unsafe controls, and editor save only enables for a validated user-editable buffer | Automated for current read-only product boundary; genuine approval/profile picker and write-capable Agent states do not exist and are not reviewed |
| Conflict | External file changed before user save; operation reconciliation required; Timeline identity/gap drift; Workspace Edit Proposal drift/race | `agentFileStatus`, `agentEditorPath`, `agentOperationStatusBanner`, `agentWorkspaceEditSummary`, `agentSendButton` | Real file fixture proves conflict text, disabled save, and unchanged external bytes. Render fixtures also prove operation pause/review and Proposal/Timeline drift fails closed | Automated on macOS; Windows filesystem sharing/case/ACL conflict evidence and future Git conflict UI remain open |
| Failure | Structured Runtime error classes; provider lifecycle failure with hidden details; degradation failure; Timeline recovery failure; renderer fallback; background inspection failure | `agentFailureNotice`, `agentRuntimeCapabilityStatus`, `agentFileStatus`, `agentTerminalStatus`, `agentSendButton` | Render test asserts bounded error class/retry guidance, provider-detail redaction, fail-closed capability state, and stable failure notices. Actual Monaco/xterm renderer crash-to-fallback transitions are not exercised by this fixture | Partially automated; renderer fallback, every dialog, and real process/platform failure remain open |
| Interrupted | Running Turn; cancellation acknowledged but not terminal; authoritative `turn.interrupted`; completion winning cancellation race | `agentSendButton`, `agentStatusNotice`, `timelineBubble` | Render test asserts `停止` -> `正在停止` -> `发送`, keeps cancellation acknowledgement non-terminal, and requires visible `任务已停止。` on interruption | Automated for the current model Turn; background job and native Agent command interruption controls do not exist |
| Recovery | Whole-store diagnostic-only mode; isolated Session quarantine; successful projection rebuild; operation review; Timeline sync/snapshot/subscription; Runtime reconnect; editor/terminal renderer fallback | `agentRecoveryBanner`, `agentOperationStatusBanner`, `agentOperationStatusReviewButton`, `agentRuntimeStatus`, `agentSendButton` | Render test asserts unsafe controls disabled, healthy Sessions remain distinct, operation review cannot fabricate recovery, and Timeline projections publish only after complete validation. Synthetic status restoration checks only the Workbench projection; real reconnect barriers are separate protocol/Qt fixtures. Renderer crash-to-fallback remains unasserted here | Partially automated; renderer fallback, complete Windows reconnect/runtime, and signed-package recovery evidence remain open |

## Release Review Gaps

OpenSpec `23.10` must remain unchecked. The current executable evidence does not yet
prove all visible states required for a stable cross-platform release:

1. Capture and review Windows screenshots/recordings for the eight classes at
   supported display scales, including IME, keyboard focus, high contrast, and text
   expansion.
2. Add accessibility assertions for names, roles, focus order, non-color status
   cues, and screen-reader announcements on banners, notices, dialogs, and progress.
3. Inventory every modal and secondary surface. Background notification/recovery,
   retention, import/export, compaction, context inspection, image preview, artifact,
   and project-root dialogs currently have selective fixtures rather than a complete
   eight-class review.
4. Add real clean-machine evidence for sidecar absence/version mismatch, signed
   package recovery, Windows TLS failure guidance, ConPTY interruption, renderer
   restart, and filesystem conflict behavior.
5. Revisit this matrix when genuine approval, Agent mutation, Git conflict actions,
   background jobs, model selection, authenticated catalog refresh, or extension
   execution becomes visible. States for those absent capabilities must not be
   represented as already verified.

## Review Rule

A state is release-reviewed only when its trigger is reproducible, the visible text
and controls are stable, unsafe actions are asserted unavailable, recovery is proven
from an authoritative result, and both macOS and Windows evidence exists where the
state is platform-sensitive. A screenshot alone does not satisfy protocol,
persistence, permission, or recovery correctness.
