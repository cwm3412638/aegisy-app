# ADR: Embedded WebEngine vs Standalone Tauri Workbench

**Status**: PROVISIONAL  
**Date**: 2026-08-01  
**Owner**: Architecture Team  
**Consulted**: Product, Engineering, Security

## Context

Milestone 0 UI Technology Spike (Tasks 2.1-2.7) evaluated Qt WebEngine for rendering Monaco editor and xterm.js terminal in the Aegisy Agent Workbench. This ADR decides between:

1. **Embedded WebEngine**: Qt WebEngineView in the same process
2. **Standalone Tauri**: Separate Tauri-based workbench with protocol communication

## Decision

**PROVISIONAL: Proceed with Embedded Qt WebEngine**

## Rationale

### Technical Evidence (Tasks 2.1-2.7)

✓ **Integration Success**:
- Monaco editor: Full feature parity (edit, diff, themes, IME)
- xterm.js: PTY, resize, Unicode, links, virtualization all working
- QWebChannel: Bidirectional communication proven
- Security: Network isolation, isolated profiles functional

✓ **Performance**:
- Startup: <1.2s cold, <0.8s warm (debug)
- Memory: ~185MB idle, ~210MB active (under budget)
- Throughput: 1000 lines <100ms
- Large files: 10K lines smooth

✓ **Platform Support**:
- macOS: All features verified (Tasks 2.4, 2.5)
- Windows: Pending verification (Task 2.6)
- Qt WebEngine available on both platforms

### Advantages of Embedded WebEngine

1. **Single Process**: Simpler architecture, no IPC overhead
2. **Proven Integration**: QWebChannel works, file operations seamless
3. **Qt Ecosystem**: Consistent with existing Qt codebase
4. **Security**: Process-level isolation, no network exposure
5. **Performance**: Direct memory access, no serialization overhead

### Disadvantages of Embedded WebEngine

1. **Bundle Size**: Qt WebEngine adds ~100MB to installer
2. **Update Coupling**: UI updates require full app update
3. **Chromium Version**: Tied to Qt's Chromium version
4. **Memory**: Higher baseline than native UI

### Tauri Alternative (Not Chosen)

**Advantages**:
- Smaller bundle (system WebView)
- Independent UI updates
- Modern web stack

**Disadvantages**:
- Protocol complexity (IPC between processes)
- Security boundary management
- Additional maintenance burden
- Unproven integration with Codex/ACP

## Consequences

### Immediate Actions

1. Continue with Qt WebEngine for Milestone 0
2. Complete Windows testing (Task 2.6)
3. Implement production workbench with WebEngine
4. Monitor bundle size and memory usage

### Future Considerations

1. **Bundle Size**: If >200MB becomes blocker, revisit Tauri
2. **Update Frequency**: If UI updates needed weekly, revisit separation
3. **Platform Support**: If Linux required, evaluate alternatives

### Risks

1. **Bundle Size**: Mitigated by compression, acceptable for desktop app
2. **Chromium Updates**: Mitigated by Qt's regular updates
3. **Memory Usage**: Within budget, acceptable for workbench app

## Validation Criteria

This decision remains PROVISIONAL until:

- [ ] Task 2.6: Windows testing complete
- [ ] Task 22.4: Signed Release build measured
- [ ] Bundle size <200MB compressed
- [ ] Memory usage <500MB active
- [ ] Startup <3s cold, <1s warm

## Related Documents

- `experiments/webengine/TASK-2.4-RESULTS.md` - Monaco integration
- `experiments/webengine/TASK-2.5-RESULTS.md` - xterm.js integration
- `experiments/webengine/TASK-2.7-RESULTS.md` - Performance measurements
- `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md` - Performance budgets
- `docs/adr/001-embedded-webengine.md` - Original ADR

## Revision History

- 2026-08-01: Initial PROVISIONAL decision based on macOS testing
