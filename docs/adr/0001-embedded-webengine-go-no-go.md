# ADR 0001: Embedded Qt WebEngine Go/No-Go

- Status: Provisional
- Accountable owner: Desktop Platform
- Consulted owners: Accessibility, Release Engineering, Product Security
- Due gate: OpenSpec `2.8`; stable evidence under `2.6`, `2.7`, `22.7`, and `22.8`

## Context

Aegisy already embeds trusted local Monaco and xterm.js content in Qt WebEngine,
with a native Qt fallback. The original question is whether this architecture meets
signed installer size, startup, memory, Chinese IME, accessibility, crash recovery,
and update-delta budgets on both macOS and Windows. Local render tests prove behavior
on the current macOS development host, not signed-package fitness.

## Provisional Decision

Continue Qt Widgets plus embedded Qt WebEngine as the product architecture while the
Workbench is internal/preview. Keep Monaco and xterm.js local, CSP restricted,
network blocked, and external navigation blocked. Keep the native Qt editor/terminal
fallback as a recovery surface.

This is not the final `2.8` go decision. A standalone Tauri workbench remains the
recorded fallback if signed-package measurements fail a non-waivable budget or if
accessibility/IME defects cannot be corrected without weakening isolation.

## Required Evidence

- Signed macOS and Windows installer growth and updater-delta measurements against
  `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md`.
- Twenty-run cold/warm startup, idle/active memory, editor latency, terminal
  throughput, and renderer recovery measurements on both reference classes.
- Chinese IME, 125%/150% Windows scaling, keyboard/focus, VoiceOver/Narrator, and
  high-contrast review.
- Nonblank Monaco/xterm rendering and deterministic native fallback after renderer
  termination.
- Proof that packaged WebEngine resources stay local and do not weaken the current
  request interceptor, CSP, storage, plugin, or navigation restrictions.

## Consequences

No release note may call the spike complete based only on a screenshot or a developer
build. Failing a correctness, isolation, IME, or accessibility gate is a no-go even
when performance is within budget. Replacing WebEngine requires its own migration ADR
and cannot bypass AAP, local-content, or secure-storage boundaries.
