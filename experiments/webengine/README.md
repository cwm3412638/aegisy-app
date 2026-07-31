# Qt WebEngine Experiments

Isolated experiments for Milestone 0 UI Technology Spike (OpenSpec Section 2).

## Experiments

### Task 2.1: Basic WebEngine Integration ✓
**Target**: `aegisy_webengine_experiment`

Minimal QWebEngineView application to verify Qt WebEngine linking works.

### Task 2.2: Secure Local Bundle ✓
**Target**: `aegisy_webengine_secure_bundle`

Features:
- Local HTML bundle loaded via setHtml()
- Network navigation blocked via acceptNavigationRequest()
- Isolated QWebEngineProfile
- No cache, no persistent cookies
- LocalContentCanAccessRemoteUrls disabled
- Security test with blocked external link

### Task 2.3: QWebChannel Bridge ✓
**Target**: `aegisy_webengine_webchannel_bridge`

Features:
- Typed request IDs
- 1MB size limit enforcement
- Request cancellation support
- Origin check placeholders

### Task 2.4: Monaco Editor Integration ✓
**Target**: `aegisy_webengine_monaco_editor`

Features:
- Open/edit/save functionality
- Large file handling (10K lines)
- Diff view support
- Theme switching (dark/light)
- Font fallback with Chinese support
- IME testing (中文输入)

See `TASK-2.4-RESULTS.md` for detailed test results.

## Build Instructions

```bash
cmake -B build -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
cmake --build build --target aegisy_webengine_monaco_editor
./build/aegisy_webengine_monaco_editor
```

## Next Tasks
- Task 2.5: xterm.js integration
- Task 2.6: Windows testing
- Task 2.7: Performance measurements
- Task 2.8: Go/no-go ADR
