## Why

The desktop client has grown beyond connection management, but its fixed-size,
dialog-heavy interface and platform-dependent standard icons make the product
feel dated and reduce usability on Windows scaling and macOS appearance modes.
The existing balance orb also exposes only account balance even though users
need trustworthy visibility into the model and runtime context they are using.

## What Changes

- Introduce a modern, responsive application shell with a restrained technical
  visual language, semantic design tokens, consistent icons, and platform-aware
  light/dark behavior for Windows and macOS.
- Simplify the main connection view so global actions, navigation, profile
  status, and activity have clear hierarchy and remain usable at smaller window
  sizes and high-DPI scaling.
- Replace the circular balance orb with a draggable compact runtime status bar
  showing the latest observed tool/model, reasoning setting, context usage
  against its reported limit, gateway state, and account balance.
- Add a shared runtime status store fed by active profiles, in-app chat usage,
  gateway request/response metadata, model capability metadata, and account
  balance updates.
- Extend the local gateway to extract usage and reasoning metadata from streamed
  OpenAI/Codex, Anthropic, and Gemini responses without persisting prompts,
  completions, tool arguments, or file content.
- Display unavailable telemetry as unknown or unmonitored; never synthesize
  values that appear authoritative.
- Make profile activation and bulk switching consistent for Claude, Codex,
  Gemini, and OpenCode, and prevent the active profile from disagreeing with the
  credential written to disk.
- Move long-running runtime and plugin operations off the UI thread with
  cancellable progress state.

## Capabilities

### New Capabilities

- `modern-desktop-ui`: Responsive cross-platform application shell, navigation,
  visual tokens, iconography, and accessible Windows/macOS behavior.
- `runtime-status-monitoring`: Truthful runtime telemetry aggregation and the
  compact background status bar.
- `connection-profile-activation`: Transactional, tool-complete profile and bulk
  activation behavior with accurate active-state reporting.

### Modified Capabilities

None. The repository did not contain existing OpenSpec capability specs.

## Impact

- Affects the main window, balance orb, chat usage flow, profile activation,
  local gateway protocol, gateway manager, theme system, and related tests.
- Adds an in-process runtime status model and new gateway metadata events.
- Keeps credentials in existing secure storage and retains loopback-only gateway
  access. Runtime telemetry remains in memory unless a future spec explicitly
  introduces persistence.
- Requires validation on macOS light/dark modes and Windows 100%, 125%, and 150%
  display scaling.
