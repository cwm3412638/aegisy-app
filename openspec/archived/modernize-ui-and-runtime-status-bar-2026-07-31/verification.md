# Verification

## Automated

- macOS Qt 6 build completed from both the normal and a clean build directory.
- All 11 CTest targets passed, including runtime state, status-bar rendering,
  four-tool activation, gateway security, and existing regression suites.
- Gateway fixtures cover OpenAI/Codex SSE, Anthropic streaming usage, Gemini
  JSON usage metadata, reasoning metadata, SSE tail frames, and content/secret
  non-disclosure.
- Status-bar rendering tests cover live, direct/unmonitored, missing-usage,
  warning, critical, large-balance, and click-to-restore states.
- `openspec validate modernize-ui-and-runtime-status-bar` passes.

## Visual

- The macOS main window was inspected from a clean application bundle. The
  graphite shell, scrollable grouped navigation, compact profile rows, overflow
  menus, activity panel, and account controls render without overlap.
- The 660 x 62 logical-pixel status bar was rendered and inspected with a long
  model identifier, 84K/272K context usage, high reasoning depth, and a large
  account balance.

## Platform Follow-Up

- The implementation uses Qt 5.14+/Qt 6 APIs with explicit Qt 5/6 mouse-event
  handling and no new macOS-only runtime dependency. The existing Windows
  packaging workflow remains the release verification path.
- Before publishing the next Windows installer, run `windows-package.yml` on a
  versioned release commit and smoke-test minimize, tray restore, drag, display
  hot-plug position recovery, and 125%/150% display scaling on Windows 11.
- Before publishing the next macOS package, repeat tray/status-bar checks on a
  second display and verify the signed/notarized bundle rather than the debug
  bundle used here.
