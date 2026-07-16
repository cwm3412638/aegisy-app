## Context

Aegisy is a Qt Widgets application supporting Windows and macOS. The current
main window owns navigation, profile activation, account data, tray behavior,
and a circular balance overlay. Runtime observations are fragmented: chat owns
usage counters, profiles own configured models, the gateway logs only request
identity and latency, and account balance is stored in MainWindow.

External CLI telemetry is observable only when traffic passes through the
loopback gateway. Direct-mode processes cannot be inspected reliably without
platform-specific process injection, which is outside the product's security
model.

## Goals / Non-Goals

**Goals:**

- Create one truthful in-memory runtime status snapshot for all status surfaces.
- Show real gateway/chat usage when observed and explicitly mark fallback data.
- Replace the balance orb with a compact draggable cross-platform status bar.
- Modernize the main application shell without changing credential security or
  configuration file contracts.
- Correct bulk activation and OpenCode parity before surfacing status broadly.
- Keep the UI responsive while runtime and plugin tasks execute.

**Non-Goals:**

- Tokenizing arbitrary external prompts locally and presenting that estimate as
  authoritative usage.
- Persisting prompt, completion, tool argument, or file-content telemetry.
- Native Windows acrylic or macOS vibrancy in the first implementation.
- Rewriting the Qt Widgets application in QML or a web framework.

## Decisions

### Shared runtime status store

Add a QObject-owned `RuntimeStatusStore` and immutable `RuntimeStatusSnapshot`.
Sources update explicit fields with provenance (`configured`, `chat`, or
`gateway`). The store emits a single `statusChanged` signal. MainWindow,
RuntimeStatusBar, and future pages consume the same snapshot.

This is preferred over letting the status bar inspect MainWindow because it
keeps telemetry testable and prevents another UI component from becoming an
application controller.

### Gateway metadata extraction

The loopback gateway will emit a request-started event after reading the request
JSON and a request-finished event after the upstream stream ends. It will parse
only model, reasoning metadata, and provider usage fields. Stream payloads are
forwarded immediately and discarded after metadata extraction.

SSE parsing uses a bounded line buffer; non-SSE JSON capture is bounded. Missing
or unparseable usage stays unknown. Direct-mode CLI status falls back to the
configured profile and is labeled unmonitored.

### Context limits

The context limit uses model API metadata when available. The Codex configured
limit is valid fallback data because Aegisy writes and validates that exact
value in `config.toml`. Other tools show an unknown limit unless a provider or
model response reports one.

### Compact background status bar

Replace `BalanceOrb` with a frameless Qt Tool window using a fixed logical
layout painted by Qt. It remains draggable, restores the main window on click,
supports a context menu, persists and clamps its position to available screens,
and is shown when the main window is minimized or hidden to the tray.

The first version uses an opaque graphite surface with semantic teal, amber,
and red accents. This is more reliable across Windows/macOS than platform blur
APIs and preserves contrast under high-DPI scaling.

### UI modernization

Retain Qt Widgets but stop forcing Fusion globally. Centralize semantic color,
spacing, and control styles in AppTheme. Reduce the global top bar to account
status and move feature commands into grouped sidebar navigation. Replace
platform-dependent standard icons incrementally with a consistent resource icon
set. Profile rows expose primary actions directly and secondary actions through
one menu.

### Profile activation consistency

Bulk switching selects existing profiles, not raw keys detached from profiles.
All four supported tools participate. Configuration writes and active-index
changes occur only for the exact selected profile credential. Failed writes do
not mark a profile active.

## Risks / Trade-offs

- [Provider SSE formats vary] -> Keep protocol parsers isolated, bounded, and
  covered by fixture tests; unknown fields do not fail proxying.
- [Gateway monitoring is optional] -> Label direct-mode data as configured or
  unmonitored instead of implying live usage.
- [Frameless windows differ by platform] -> Use Qt Tool flags, logical pixels,
  screen-bound position restoration, and macOS/Windows manual verification.
- [Large UI refactor can regress workflows] -> Preserve existing slots and
  dialogs initially, modernize the shell and profile surface first, then migrate
  pages incrementally.

## Migration Plan

1. Add specs/tests and correct activation behavior.
2. Introduce RuntimeStatusStore without changing the visible orb.
3. Add gateway metadata events and connect chat/profile/balance sources.
4. Replace BalanceOrb with RuntimeStatusBar while retaining saved position
   compatibility.
5. Apply the new shell/theme and verify all existing commands remain reachable.
6. Roll back by restoring the old overlay wiring; no persistent data migration
   is required.

## Open Questions

- Whether balance should be visible by default on shared screens. A future
  privacy preference can hide it without changing the runtime data contract.
- Provider model metadata is not guaranteed to include context limits. Unknown
  remains a supported, explicit state.
