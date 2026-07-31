# Aegisy Coding Workbench - Quick Reference

**Last Updated**: 2026-07-31

## Essential Commands

### Build
```bash
# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Using helper
./scripts/dev-helper.sh build
./scripts/dev-helper.sh build-debug
```

### Test
```bash
# All tests
cd build && ctest --output-on-failure

# Specific categories
./scripts/run-tests.sh update    # Update system tests
./scripts/run-tests.sh aap       # AAP protocol tests
./scripts/run-tests.sh runtime   # Runtime tests
./scripts/run-tests.sh quick     # Quick smoke tests
```

### OpenSpec
```bash
# Check progress
./scripts/openspec-status.sh

# View tasks
cat openspec/changes/build-aegisy-agent-workbench/tasks.md

# View status
cat docs/WORKBENCH-STATUS.md
```

## Key Concepts

### Chat vs Work Modes
- **Chat**: Non-mutating, safe exploration
- **Work**: Project-bound, can modify files (with approval)

### Core Entities
- **Project**: Workspace with filesystem roots
- **Session**: Durable conversation thread
- **Turn**: One request-response cycle
- **Timeline**: Ordered sequence of events
- **Runtime**: Execution environment (aegisy-agentd)

## Architecture

### Components
```
┌─────────────────┐
│   Qt UI (C++)   │  Main window, Monaco, terminal UI
└────────┬────────┘
         │ AAP (JSON-RPC)
┌────────▼────────┐
│ aegisy-agentd   │  Rust sidecar, execution isolation
│    (Rust)       │
└────────┬────────┘
         │
┌────────▼────────┐
│ Codex Adapter   │  Provider integration
└─────────────────┘
```

### Key Protocols
- **AAP**: Aegisy Agent Protocol (JSON-RPC over stdio/socket/pipe)
- **LSP**: Language Server Protocol (for editor intelligence)
- **MCP**: Model Context Protocol (for extensions)

## File Locations

### Configuration
- macOS: `~/Library/Application Support/Aegisy/`
- Windows: `%APPDATA%\Aegisy\`
- Linux: `~/.config/Aegisy/`

### Workbench Data
- macOS: `~/Library/Application Support/Aegisy/workbench/`
- Windows: `%APPDATA%\Aegisy\workbench\`
- Contains: SQLite database, session data, artifacts

### Logs
- Runtime log: `workbench/aegisy-runtime.log`
- Application log: Platform-specific

## Important Files

### Documentation
- `README.md` - Project overview
- `CONTRIBUTING.md` - Contribution guide
- `docs/WORKBENCH-STATUS.md` - Development status
- `docs/CHAT-WORK-BEHAVIORAL-CONTRACT.md` - Mode definitions
- `docs/AEGISY-CODING-TERMINOLOGY.md` - Terminology
- `PROJECT-MEMORY.md` - Project memory and context

### OpenSpec
- `openspec/changes/build-aegisy-agent-workbench/`
  - `proposal.md` - Initial proposal
  - `design.md` - Detailed design
  - `tasks.md` - Task tracking
  - `verification.md` - Verification criteria

### Code
- `src/` - C++ source files
- `include/` - C++ headers
- `tests/` - Test files
- `agent-runtime/` - Rust sidecar
- `cmake/` - CMake modules

## Common Tasks

### Adding a New Feature
1. Check OpenSpec tasks for related work
2. Create feature branch: `git checkout -b feature/name`
3. Implement with tests
4. Update documentation
5. Create pull request

### Fixing a Bug
1. Create fix branch: `git checkout -b fix/name`
2. Add regression test
3. Fix the bug
4. Verify test passes
5. Create pull request

### Updating Documentation
1. Edit relevant `.md` files
2. Update `CHANGELOG.md` if user-facing
3. Update OpenSpec `tasks.md` if completing tasks
4. Commit with `docs:` prefix

## Testing Patterns

### Unit Test
```cpp
TEST(ComponentTest, SpecificBehavior) {
    // Arrange
    Component component;
    
    // Act
    auto result = component.doSomething();
    
    // Assert
    EXPECT_EQ(result, expected);
}
```

### Protocol Test
```cpp
TEST(AapProtocolTest, HandshakeSequence) {
    // Test AAP initialize/initialized handshake
    // Verify request/response format
    // Check capability negotiation
}
```

## Debugging

### Enable Verbose Logging
```bash
# Set environment variable
export AEGISY_LOG_LEVEL=debug

# Run application
./build/Aegisy
```

### Attach Debugger
```bash
# GDB (Linux/macOS)
gdb ./build/Aegisy

# LLDB (macOS)
lldb ./build/Aegisy

# Visual Studio (Windows)
# Open solution and press F5
```

### Check Runtime Status
```bash
# View runtime log
tail -f ~/Library/Application\ Support/Aegisy/workbench/aegisy-runtime.log

# Check process
ps aux | grep aegisy-agentd
```

## Security

### Credential Storage
- macOS: Keychain
- Windows: DPAPI
- Linux: Secret Service

### Never Commit
- API keys
- Passwords
- Certificates
- Private keys
- `.env` files with secrets

## Performance

### Build Performance
```bash
# Use Ninja (faster than Make)
cmake -B build -G Ninja

# Use ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache

# Parallel builds
cmake --build build --parallel $(nproc)
```

### Test Performance
```bash
# Run tests in parallel
cd build && ctest --parallel $(nproc)

# Run only quick tests
./scripts/run-tests.sh quick
```

## Troubleshooting

### Build Fails
- Check CMake version: `cmake --version`
- Check Qt installation: `qmake --version`
- Clean build: `rm -rf build && cmake -B build`

### Tests Fail
- Check test output: `ctest --output-on-failure`
- Run specific test: `ctest -R TestName -V`
- Check for stale processes: `ps aux | grep aegisy`

### Runtime Issues
- Check runtime log
- Verify sidecar version matches
- Check AAP protocol version
- Verify project root permissions

## Resources

### Documentation
- Design docs: `docs/*.md`
- ADRs: `docs/adr/*.md`
- OpenSpec: `openspec/changes/*/`

### External
- Qt Documentation: https://doc.qt.io/
- Rust Book: https://doc.rust-lang.org/book/
- CMake Tutorial: https://cmake.org/cmake/help/latest/guide/tutorial/

## Getting Help

- Check `CONTRIBUTING.md` for guidelines
- Review `docs/WORKBENCH-STATUS.md` for current status
- Search existing GitHub issues
- Create new issue with details

---

**Tip**: Bookmark this file for quick reference during development!
