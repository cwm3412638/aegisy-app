# Timeline UI Design

## Overview

The Agent timeline is the central conversation surface in the Agent Workbench. It displays structured plan, command, patch, approval, question, error, and usage items in a virtualized, scrollable view. The timeline stays visible to the left of the work canvas on wide windows and becomes a drawer/tab on narrow windows. The composer and pending approval are never clipped.

## Timeline Item Types and Rendering

### Item Type Catalog

- **user**: User messages with text content
- **assistant**: Agent responses with text/reasoning content
- **plan**: Structured plan with stable step status and links to child sessions or evidence
- **reasoning**: Reasoning summaries (may be provider-encrypted)
- **command**: Command execution with actions, cwd, environment identity, risk classification, output deltas, duration, and result
- **file-change**: Workspace edit proposals with diffs, additions/deletions, sensitive path warnings, and content-reference paging
- **approval**: Approval requests showing command/diff/scope/risk/reason and exact available decision scopes
- **question**: Structured user input requests with ordered question/option IDs, kinds, required flags
- **error**: Runtime errors classified by protocol/provider/adapter/transport/timeout/sandbox/policy/tool/storage/workspace/git/budget
- **usage**: Token usage with context/cost/reasoning metrics and authority flags
- **artifact**: Generated artifacts, previews, screenshots, reports

### Rendering Rules

- Items use deterministic delta accumulation: `started` → `delta*` → `completed`
- Terminal states are `completed`, `failed`, or `interrupted`
- Each item has a contiguous positive sequence number per session
- Items bind to a Turn via correlation ID
- Unknown item types advance the cursor without changing product state and are retained as bounded content-free diagnostics
- Malformed items, duplicate IDs, and kind/role drift are inert
- Items are validated against both core and transport schemas

### Item Data Bounds

- Item `data` is limited to 16 levels and 4,096 aggregate values
- Optional Item data uses recursively bounded canonical JSON subset
- Only exact mathematical integers in the signed JSON-safe range
- Unicode-scalar strings and keys required
- Secret-shaped content and credential-like field names are rejected before storage

## Composer UI and Input Handling

### Composer Components

- **Input field**: Multi-line text input with bounded size (64 KiB per turn)
- **Context queue**: Inspectable queue showing:
  - Origin (file tree, editor selection, diagnostic, search result, terminal excerpt, Git diff)
  - Byte size
  - Inclusion checkbox
  - Truncation state
  - Removal control
- **Execution context strip**: Shows Chat/Work mode, project, workspace root, Runtime readiness/recovery state, provider/model, permission profile, Session-bound Git branch, selected-context count
- **Send/Stop control**: Stable-width control that changes to Stop/Stopping during execution

### Input Validation

- Turn input limited to 16 items, 16 KiB per rendered item, 64 KiB total
- Files are authoritatively reread by sidecar
- Inline excerpts remain explicitly untrusted data
- Sidecar revalidates project scope, sensitive paths, symlinks, Git ignore policy
- Secret-shaped content is rejected

### Turn Submission

- Turn start uses idempotency keys for durable mutation acknowledgement
- Equivalent retries return the same operation identity and Turn binding
- Conflicting fingerprint fails without dispatch
- Turn start fails with `-32152` unless Session owns a current subscription attempt
- Send is disabled until subscription is Active

### Turn Controls

- **Submit**: Start new turn with idempotency
- **Cancel**: Out-of-band cancellation reachable under request flooding
- **Steer**: Same-turn steering with eight-entry queue, 64 KiB input bound
- **Retry**: Retry failed turn
- **Edit-and-retry**: Edit input and retry
- **Fork-from-turn**: Create child session from specific turn boundary

## Streaming Updates and Pagination

### Live Event Streaming

- Events use contiguous positive safe-integer sequence per Session
- Content-hashed immutable Event ID
- Runtime observation time clamped non-decreasing per Session
- Correlation ID equals bound Turn ID
- Item updates carry contiguous per-Item revision
- Explicit `snapshot-replacement` content semantics

### Subscription State Machine

- Negotiated `timeline.subscription.fixed-watermark` capability
- States: `sync-required` → `snapshot-required` → `active`
- Subscribe selects initial state with fixed watermark
- Sync validates bounded contiguous page chain from cursor through watermark
- Snapshot validates bounded fixed-header page chain and ordered complete Items
- Activation drains buffered events as subscription-bound notifications
- Subscription ID never reused within connection generation

### Recovery and Reconnect

- Snapshot plus replay from sequence number for crash recovery
- Fixed-watermark replay with durable Journal/checkpoint/floor
- Retention gap returns `timeline-retention-gap/0.1` on `-32148`
- Snapshot available only when connection negotiated `timeline.snapshot.current`
- Complete snapshot bounded to 10,000 Items and 64 MiB
- Pages limited to 200 Items and 4 MiB frame
- Qt stages recovery privately, publishes only after exact activation response
- Old-generation and pre-activation traffic is inert

### Pagination

- Backward pagination for older history with strict session-sequence cursors
- First read returns newest bounded page (1-200 items)
- Explicit load-more control prepends older items while retaining scroll anchor
- Stale cursors restart from new snapshot
- Invalid cursor rejection with explicit error

### Virtualization

- Timeline must be virtualized for long sessions (10,000+ items)
- Bounded rendering window with virtual scrolling
- Deterministic oldest eviction for in-memory caches
- Large output uses head/tail/artifact capture without unbounded memory

## Approval UI and Interaction Patterns

### Approval Display

- **Command approvals**: Show command, cwd, environment identity, risk level (low/medium/high), reason, resulting diff
- **File-change approvals**: Show diffs, additions/deletions, sensitive path warnings, blocking warnings, aggregate totals
- **Permission approvals**: Show filesystem roots, denied paths, network policy, command policy, MCP/extension access
- **Risk classification**: Conservative classification for destructive Git/filesystem, network, privilege, dependency, project-execution patterns

### Decision Scopes

- `deny`: Reject once
- `allow once`: Allow for this single request
- `allow for turn`: Allow for current turn
- `allow for session`: Allow for current session
- Durable rules: Narrowly scoped, editable, hash command/extension definition

### Approval Workflow

- Approvals are never clipped (always visible)
- Inline approvals in timeline at point of request
- Medium and high-risk operations require explicit UI confirmation even with auto-approval enabled
- High-risk operations cannot be approved with blanket session rule in initial releases
- Approval responses use idempotency keys (future work)
- Durable acknowledgement for approval responses (future work)

### Read-Only Mode

- Current implementation is read-only: all file-change proposals are declined
- Proposals are persisted as immutable records before decline
- Qt restores durable Proposals into Changes with foreground auto-open
- Background Proposals add unread marker without stealing focus
- `View changes` action shows read-only diff preview
- No approval/apply controls in current read-only mode

## Implementation Phases

### Phase 1: Basic Timeline Rendering (Partial - In Progress)

- [x] Core timeline item types defined in schema
- [x] Event sequence and correlation established
- [x] Usage item projection and rendering
- [ ] Complete virtualized timeline for all item types
- [ ] Deterministic delta accumulation for all types
- [ ] Terminal-state rendering for all types

### Phase 2: Composer and Context (Partial - In Progress)

- [x] Basic composer input field
- [x] Context queue with origin tracking
- [x] Execution context strip with mode/project/runtime/model/branch
- [x] Send/Stop control with state management
- [ ] Complete context attachment from all surfaces
- [ ] File/image/diagnostic/terminal/Git attachment preview
- [ ] Provenance, size, inclusion, removal controls

### Phase 3: Streaming and Subscription (Complete)

- [x] Live event streaming with sequence numbers
- [x] Subscription state machine with sync/snapshot/activate
- [x] Fixed-watermark replay and recovery
- [x] Reconnect with bounded barrier
- [x] Heartbeat and out-of-band control
- [x] Durable Turn-start acknowledgement

### Phase 4: Pagination and History (Complete)

- [x] Backward pagination with cursors
- [x] Load-more control with scroll anchor
- [x] Session replay from durable storage
- [x] Snapshot materialization with identity validation
- [x] Retention gap handling

### Phase 5: Approval UI (Partial - Future)

- [x] Read-only file-change proposal display
- [x] Changes view with diff preview
- [ ] Interactive approval requests
- [ ] Decision scope selection
- [ ] Risk classification display
- [ ] Durable approval acknowledgement

### Phase 6: Advanced Features (Future)

- [ ] Live plan view with step status
- [ ] Structured user questions with option selection
- [ ] Chat-to-Work conversion
- [ ] Fork-from-turn with portable context
- [ ] Timeline stress tests for long sessions
- [ ] Concurrent events handling
- [ ] Large output references
- [ ] Unknown item type handling

## Technical Constraints

### Performance Budgets

- Timeline must handle 10,000+ items without degradation
- Virtualization required for sessions exceeding 1,000 items
- Render updates must not block input or scrolling
- Memory bounded per session (limits vary by item type)

### Security Boundaries

- Treat all timeline content as untrusted data
- Redact secrets before storage and display
- Never expose raw protocol JSON in timeline (diagnostics only)
- Validate all item data against schema before rendering
- Reject credential-like field names and secret values

### Cross-Platform Requirements

- Timeline rendering must work in Qt WebEngine (Monaco/xterm.js)
- Fallback to native Qt rendering if WebEngine unavailable
- Support macOS Retina and Windows 125%/150% scaling
- Chinese IME support required
- Accessibility compliance required

### Error Handling

- Malformed items are inert (don't crash timeline)
- Unknown item types advance cursor without state change
- Schema validation failures enter read-only recovery
- Drift/tampering freezes affected Session
- Late/stale responses are inert

## Open Questions

1. Should timeline virtualization use a fixed window size or dynamic based on item complexity?
2. How should timeline handle rapid concurrent updates (>100 items/second)?
3. Should approval UI support batch approval for multiple similar requests?
4. How should timeline indicate when items are truncated due to size limits?
5. Should timeline support search/filter within a session?
6. How should timeline handle session fork lineage visualization?
7. Should timeline support export of selected items or ranges?
