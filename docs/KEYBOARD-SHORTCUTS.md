# Keyboard Shortcuts Reference

## Global Shortcuts

### Navigation
- `Alt+1` — Switch to Launch Pad page
- `Alt+2` — Switch to Desktop Clients page
- `Alt+3` — Switch to Connection Configuration page
- `Alt+4` — Switch to Local Gateway page
- `Alt+5` — Switch to System & Extensions page
- `Alt+6` — Switch to Agent Workbench page

## Editor Shortcuts (Monaco)

### File Operations
- `Ctrl+S` (Windows/Linux) / `Cmd+S` (macOS) — Save current file

### Search & Replace
- `Ctrl+F` (Windows/Linux) / `Cmd+F` (macOS) — Find in file
- `Ctrl+H` (Windows/Linux) / `Cmd+H` (macOS) — Replace in file

### Code Navigation
- `F12` — Go to definition
- `Shift+F12` — Find all references
- `Ctrl+Shift+M` (Windows/Linux) / `Cmd+Shift+M` (macOS) — Show diagnostics panel

### Standard Monaco Keybindings
Monaco editor includes standard VS Code keybindings:
- `Ctrl+/` (Windows/Linux) / `Cmd+/` (macOS) — Toggle line comment
- `Ctrl+D` (Windows/Linux) / `Cmd+D` (macOS) — Add selection to next find match
- `Ctrl+Shift+K` (Windows/Linux) / `Cmd+Shift+K` (macOS) — Delete line
- `Alt+Up/Down` — Move line up/down
- `Shift+Alt+Up/Down` — Copy line up/down
- `Ctrl+]` (Windows/Linux) / `Cmd+]` (macOS) — Indent line
- `Ctrl+[` (Windows/Linux) / `Cmd+[` (macOS) — Outdent line

## Terminal Shortcuts

### Clipboard Operations
- `Ctrl+C` (Windows/Linux) / `Cmd+C` (macOS) — Copy selection (when text is selected)
- `Ctrl+V` (Windows/Linux) / `Cmd+V` (macOS) — Paste from clipboard

Note: Copy/paste operations are handled through the bridge interface and respect platform conventions.

## Composer Shortcuts

### Message Submission
- `Ctrl+Enter` — Submit prompt/message

## Platform-Specific Differences

### macOS
- Uses `Cmd` (⌘) as primary modifier key
- Terminal copy/paste uses `Cmd+C` / `Cmd+V`
- Editor shortcuts use `Cmd` instead of `Ctrl`

### Windows/Linux
- Uses `Ctrl` as primary modifier key
- Terminal copy/paste uses `Ctrl+C` / `Ctrl+V`
- Editor shortcuts use `Ctrl`

### Modifier Key Detection
The application automatically detects the platform:
```javascript
const modifier = navigator.platform.toLowerCase().includes("mac")
    ? event.metaKey
    : event.ctrlKey;
```

## Customization Notes

### Editor Keybindings
Monaco editor keybindings are defined in `/workbench-web/src/editor.js`. Custom shortcuts are registered using:
```javascript
editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, callback);
```

### Application Shortcuts
Qt-based application shortcuts are defined in C++ source files:
- Main window navigation: `src/main_window.cpp` (lines 1734-1738)
- Workbench shortcuts: `src/agent_workbench_widget.cpp` (lines 4957-6252)

### Adding Custom Shortcuts
To add custom shortcuts:

1. **Editor shortcuts** — Modify `workbench-web/src/editor.js`:
```javascript
editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyX, () => {
    // Your custom action
});
```

2. **Application shortcuts** — Add to relevant C++ widget:
```cpp
auto *shortcut = new QShortcut(QKeySequence("Ctrl+X"), this);
connect(shortcut, &QShortcut::activated, this, &Widget::customAction);
```

## Accessibility

All keyboard shortcuts support:
- Full keyboard-only navigation
- Screen reader compatibility
- High contrast mode
- Platform-specific accessibility features (VoiceOver on macOS, Narrator on Windows)

## Known Limitations

1. Terminal shortcuts are limited to copy/paste operations
2. Interactive terminal features (Ctrl+C for interrupt) are handled by the terminal emulator
3. Some Monaco shortcuts may conflict with browser shortcuts in web contexts
4. Custom shortcuts must avoid conflicts with system-level shortcuts

## References

- Monaco Editor API: https://microsoft.github.io/monaco-editor/api/
- Qt Keyboard Shortcuts: https://doc.qt.io/qt-5/qkeysequence.html
- xterm.js Documentation: https://xtermjs.org/
