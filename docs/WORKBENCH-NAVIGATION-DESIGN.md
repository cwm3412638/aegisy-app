# Workbench Navigation Design

## Overview

This document defines the main window layout, navigation structure, and responsive behavior for the Aegisy Agent Workbench. The design implements the product shell specified in `design.md` section 1, supporting Chat and Work modes with persistent project/session navigation.

## 1. Main Window Layout

### Three-Pane Wide Layout

On wide windows (≥1280px), the workbench uses a three-pane horizontal layout:

```
+------------------+----------------------+--------------------------------+
| Product Rail     | Agent Surface        | Work Canvas                    |
| (fixed 64px)     | (min 320px)          | (flexible, min 480px)          |
+------------------+----------------------+--------------------------------+
```

**Product Rail** (left, 64px fixed width):
- Chat/Work mode toggle
- New task action
- Projects list
- Extensions destination
- Settings destination
- Vertical icon-based navigation

**Agent Surface** (center-left, 320-640px default, resizable):
- Project + session selector
- Conversation timeline (virtualized)
- Plan + tool events
- Approvals + composer
- Always visible on wide layouts

**Work Canvas** (right, flexible):
- Editor / diff / preview (tabbed)
- File tree / terminal / Git (tabbed)
- Diagnostics / task artifacts (tabbed)
- Context-dependent content

### Narrow Drawer Layout

On narrow windows (<1280px), the three panes become mutually exclusive drawers/tabs:

- Product rail collapses to hamburger menu or bottom tab bar
- Agent surface and work canvas occupy full width
- User switches between Agent timeline and work canvas views
- Composer and pending approvals remain accessible (never clipped)
- Active approval overlays work canvas when necessary

### Minimum Window Sizes

- Minimum width: 800px
- Minimum height: 600px
- Below minimum: display "resize window" message
- Composer minimum height: 120px (never clipped)
- Approval controls: always visible when pending

## 2. Chat/Work Mode Switching

### Mode Definitions

**Chat Mode**:
- Non-mutating by default
- Can search explicitly shared context
- Can propose tasks
- Cannot run workspace write or shell tools
- Displays across all projects (global scope)
- Permission profile: Read Only

**Work Mode**:
- Binds one session to a project root
- Enables workspace write and shell tools (with approval)
- Runtime, permission profile, model profile bound
- Change set tracked
- Agent timeline stays visible beside work canvas
- Permission profile: Workspace Write or higher

### Switching Behavior

**Chat → Work Conversion**:
1. User selects "Convert to Work" action
2. Project selection dialog appears
3. User selects target project root
4. New Work session created with Chat history
5. Runtime bindings established
6. Workspace tools become available (with approval)
7. Mode indicator updates

**Work → Chat**:
- Not supported (Work sessions remain Work)
- User can start new Chat session instead
- Work session remains in project history

**Mode Indicator**:
- Prominent badge in Agent surface header
- Color-coded: Chat (blue), Work (green)
- Shows current permission profile
- Click to view mode details (read-only)

## 3. Project and Session Navigation

### Project Rail (Left Side of Agent Surface)

**Project List**:
- Pinned projects (top, user-ordered)
- Recent projects (by last activity)
- Unavailable projects (grayed, with relink action)
- Each entry shows:
  - Project name (derived from root directory)
  - Active session count badge
  - Running turn indicator (animated)
  - Approval needed indicator (alert badge)
  - Failed/interrupted state (error badge)

**Project Actions**:
- Pin/unpin (context menu)
- Open project (selects most recent session or creates new)
- Relink unavailable project
- Project settings (retention, profiles)

### Session Navigation (Within Agent Surface)

**Session List** (below project selector):
- Current project's sessions (Work mode)
- All sessions (Chat mode)
- Sorted by: pinned, then recent activity
- Each entry shows:
  - Session title (auto-generated or user-edited)
  - Timestamp (last activity)
  - Runtime state badge (running/stopped/failed)
  - Model indicator
  - Turn count

**Session Actions**:
- Resume session (loads timeline)
- Fork session (at current or selected turn)
- Rename session
- Archive session
- Delete session (with confirmation)

**Session Search**:
- Debounced search input above session list
- Searches: title, transcript content (approved fields only)
- Filters: project, branch, model, runtime, status
- Results paginated (100 per page)
- Empty state with search tips

### Navigation State Persistence

**Per-Device Storage**:
- Last selected project
- Last selected session per project
- Pinned projects list
- Pane sizes and visibility
- Scroll positions (best-effort)

**Not Persisted**:
- Temporary UI state (hover, focus)
- Uncommitted composer input (cleared on close)
- Transient error messages

## 4. Responsive Layout for Different Window Sizes

### Breakpoints

- **Wide** (≥1280px): Three-pane layout, all visible
- **Medium** (1024-1279px): Agent surface narrower (320px min), work canvas compressed
- **Narrow** (<1024px): Drawer/tab mode, one pane at a time

### Wide Layout (≥1280px)

**Default Pane Sizes**:
- Product rail: 64px (fixed)
- Agent surface: 400px (resizable 320-640px)
- Work canvas: remaining width (min 480px)

**Resize Behavior**:
- Drag divider between Agent surface and work canvas
- Agent surface: 320px min, 640px max
- Work canvas: 480px min, no max
- Resize handle visible on hover
- Double-click divider to reset to default

**Pane Visibility**:
- All panes always visible
- Work canvas tabs switch content
- Agent surface always shows timeline + composer

### Medium Layout (1024-1279px)

**Adjusted Sizes**:
- Product rail: 64px (fixed)
- Agent surface: 320px (fixed at minimum)
- Work canvas: remaining width (may be <480px)

**Behavior**:
- No resize handle (fixed at minimums)
- Work canvas may show horizontal scroll for wide content
- Consider collapsing file tree to icons-only

### Narrow Layout (<1024px)

**Drawer/Tab Mode**:
- Product rail: collapsed to hamburger or bottom tabs
- Agent surface OR work canvas visible (not both)
- Toggle button to switch between views
- Composer remains accessible in Agent view
- Approval overlays work canvas when needed

**View Switching**:
- "Timeline" tab: shows Agent surface full-width
- "Files" tab: shows file tree + editor
- "Terminal" tab: shows terminal full-width
- "Git" tab: shows Git workspace
- Tab bar at top or bottom
- Active tab highlighted

**Composer in Narrow Mode**:
- Fixed at bottom of Agent surface view
- Minimum 120px height
- Expands on focus (up to 50% of viewport)
- Send/Stop button always visible

**Approval in Narrow Mode**:
- Modal overlay on any view
- Full-width approval card
- Approve/Deny buttons prominent
- Cannot be dismissed without decision
- Blocks other interactions

## 5. Implementation Phases

### Phase 1: Foundation (Task 11.1-11.2)

**Deliverables**:
- Add Agent Workbench destination behind feature flag
- Trusted local bundle loading in QWebEngineView
- Content security policy (no external navigation)
- Renderer crash recovery page
- Basic HTML/CSS shell (no functionality)

**Acceptance**:
- Workbench destination appears in disabled state
- Bundle loads without network requests
- CSP blocks external resources
- Crash page displays on renderer failure
- Legacy startup behavior unchanged

### Phase 2: Product Rail and Basic Navigation (Task 11.3)

**Deliverables**:
- Product rail with Chat/Work toggle
- New task action (creates Chat or Work session)
- Projects destination (placeholder)
- Sessions destination (placeholder)
- Extensions destination (placeholder)
- Settings destination (placeholder)
- Navigation between destinations

**Acceptance**:
- Rail icons visible and clickable
- Chat/Work toggle changes mode indicator
- New task creates appropriate session type
- Destinations switch content area
- No functional project/session lists yet

### Phase 3: Layout and Responsiveness (Task 11.4-11.5)

**Deliverables**:
- Three-pane wide layout implementation
- Narrow drawer/tab layout implementation
- Breakpoint detection and switching
- Pane resize with drag handles
- Pane hide/show controls
- Layout persistence (device-local)

**Acceptance**:
- Wide layout shows all three panes
- Narrow layout shows drawer/tab mode
- Resize works within constraints
- Layout persists across sessions
- Composer never clipped
- Approval always accessible

### Phase 4: Navigation Integration (Task 11.6-11.7)

**Deliverables**:
- Native menu integration
- Keyboard shortcuts (Cmd/Ctrl+K palette)
- Project/session live-state badges
- Running turn indicator (animated)
- Approval needed badge
- Failed/interrupted state badge
- Background work indicator

**Acceptance**:
- Menu commands trigger workbench actions
- Keyboard shortcuts work consistently
- Badges update in real-time
- Accessibility labels present
- Screen reader announces state changes

### Phase 5: Theming and Accessibility (Task 11.8-11.9)

**Deliverables**:
- Theme support (light/dark/system)
- System font integration
- High-DPI rendering (1x, 2x, 125%, 150%, 200%)
- Reduced motion support
- High contrast mode
- Screen reader compatibility
- Responsive screenshots (macOS/Windows)
- Accessibility audit results

**Acceptance**:
- Theme switches without restart
- Fonts match system preferences
- Sharp rendering at all DPI scales
- Animations respect reduced motion
- High contrast meets WCAG AA
- Screen reader navigates all controls
- Screenshots document all breakpoints
- Accessibility tests pass

## Design Rationale

### Why Three Panes on Wide Layouts

- Agent timeline provides persistent context during work
- Reduces mode switching and cognitive load
- Matches user mental model (conversation + workspace)
- Proven pattern in Claude Code and similar tools

### Why Drawer Mode on Narrow Layouts

- Prevents cramped, unusable panes
- Maintains readable text and clickable targets
- Composer and approvals remain accessible
- Mobile-first responsive pattern

### Why Fixed Product Rail Width

- Icon-based navigation doesn't need flexibility
- Predictable layout reduces visual noise
- Maximizes space for content panes
- Consistent with desktop application patterns

### Why Minimum Window Sizes

- Below 800x600, layout becomes unusable
- Prevents support burden from extreme sizes
- Matches minimum for Monaco editor usability
- Aligns with Qt minimum window policies

## Open Questions

1. Should narrow mode use top tabs or bottom tabs for view switching?
   - **Recommendation**: Top tabs (closer to content, familiar pattern)

2. Should project rail support drag-and-drop reordering of pinned projects?
   - **Recommendation**: Yes, in Phase 4 (low complexity, high value)

3. Should layout persistence be per-project or global?
   - **Recommendation**: Global (simpler, matches user expectation)

4. Should composer expand automatically when user starts typing?
   - **Recommendation**: Yes, up to 50% viewport (better UX)

5. Should approval overlay dim the background in narrow mode?
   - **Recommendation**: Yes, with 50% opacity (focuses attention)

## References

- `openspec/changes/build-aegisy-agent-workbench/design.md` section 1
- `openspec/changes/build-aegisy-agent-workbench/tasks.md` section 11
- `docs/CHAT-WORK-BEHAVIORAL-CONTRACT.md` (mode definitions)
- `docs/AEGISY-CODING-TERMINOLOGY.md` (terminology)
