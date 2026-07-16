## 1. Profile activation correctness

- [x] 1.1 Replace raw-key bulk switching with exact profile selection for all four tools
- [x] 1.2 Include OpenCode in version and health workflows
- [x] 1.3 Add automated coverage for bulk activation planning and four-tool parity
- [x] 1.4 Generate and validate Codex third-party capability compatibility fields

## 2. Runtime telemetry

- [x] 2.1 Add RuntimeStatusSnapshot and RuntimeStatusStore with provenance-aware updates
- [x] 2.2 Extend the loopback gateway with bounded provider usage and reasoning metadata parsing
- [x] 2.3 Connect profile, gateway, chat, model-limit, and balance updates to the shared store
- [x] 2.4 Add gateway and runtime-store tests that verify real, unknown, and privacy-preserving states

## 3. Background status bar

- [x] 3.1 Replace BalanceOrb with the compact draggable RuntimeStatusBar
- [x] 3.2 Preserve restore, tray, quit, position recovery, and warning/critical context states
- [x] 3.3 Verify status-bar rendering and interactions on macOS and build compatibility for Windows

## 4. Modern desktop UI

- [x] 4.1 Introduce semantic theme tokens and stop forcing the Fusion style
- [x] 4.2 Simplify the top bar and group feature navigation in a modern sidebar
- [x] 4.3 Convert profile cards to compact rows with primary actions and an overflow menu
- [x] 4.4 Ensure the main shell remains reachable at minimum size and high-DPI logical scaling

## 5. Responsive background work

- [x] 5.1 Move PPT runtime installation and generation off the UI thread with progress state
- [x] 5.2 Move Codex plugin discovery and installation off the UI thread or expose cancellable asynchronous APIs

## 6. Verification

- [x] 6.1 Build the application and run the full CTest suite
- [x] 6.2 Validate the OpenSpec change and inspect the macOS UI and status bar visually
- [x] 6.3 Document remaining Windows/macOS manual verification requirements
