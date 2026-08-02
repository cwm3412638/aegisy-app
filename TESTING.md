# Testing Guide

## Quick Start

```bash
# Build with tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j 4

# Run all tests
ctest --test-dir build --output-on-failure

# Run specific test
ctest --test-dir build -R test_name -V
```

## Test Structure

```
tests/
├── *_test.cpp           # C++ unit tests
├── agent_workbench_render_test.cpp  # UI tests
└── agent_runtime_*_test.cpp         # Runtime tests
```

## Test Categories

### Unit Tests
- Feature flags
- Profile management
- API client
- Skill manager

### Integration Tests
- AAP protocol
- Runtime environment
- Socket/pipe transport

### UI Tests
- Workbench rendering
- Monaco editor
- Accessibility
- Security

### Rust Tests
```bash
cd agent-runtime
cargo test
cargo test -- --nocapture  # with output
```

## Writing Tests

### C++ Test Template
```cpp
#include <QtTest/QtTest>

class MyTest : public QObject {
    Q_OBJECT
private slots:
    void testFeature() {
        QVERIFY(true);
        QCOMPARE(1 + 1, 2);
    }
};

QTEST_MAIN(MyTest)
#include "my_test.moc"
```

### Rust Test Template
```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_feature() {
        assert_eq!(1 + 1, 2);
    }
}
```

## Running Tests

### All Tests
```bash
ctest --test-dir build
```

### Specific Test
```bash
ctest --test-dir build -R feature_flags
```

### Verbose Output
```bash
ctest --test-dir build -V
```

### Failed Tests Only
```bash
ctest --test-dir build --rerun-failed
```

## Test Coverage

### Current Coverage
- Unit tests: 29 files
- Integration tests: Comprehensive
- UI tests: Render, accessibility, security
- Rust tests: Protocol, storage, adapters

### Coverage Goals
- Unit tests: >80%
- Integration tests: All critical paths
- UI tests: All user workflows
- Cross-platform: macOS ✓, Windows ⏳

## Platform-Specific Testing

### macOS
```bash
./build.sh
ctest --test-dir build
```

### Windows
```bat
build.bat
ctest --test-dir build -C Release
```

### Linux
```bash
./build.sh
ctest --test-dir build
```

## Continuous Integration

Tests run automatically on:
- Pull requests
- Main branch commits
- Release tags

## Debugging Tests

### GDB/LLDB
```bash
lldb build/tests/test_name
```

### Valgrind
```bash
valgrind --leak-check=full build/tests/test_name
```

### Qt Test Options
```bash
# Run with debugging
QT_LOGGING_RULES="*.debug=true" ctest --test-dir build -R test_name -V
```

## Best Practices

1. **Test one thing** - Each test should verify one behavior
2. **Use descriptive names** - `testFeatureBehavior()` not `test1()`
3. **Clean up** - Reset state after each test
4. **Mock external dependencies** - Don't rely on network/filesystem
5. **Test edge cases** - Empty input, null, overflow, etc.
6. **Fast tests** - Keep tests under 1 second each

## Common Issues

**Tests fail on CI but pass locally:**
- Check platform differences
- Verify dependencies
- Check timing assumptions

**Flaky tests:**
- Add proper synchronization
- Avoid sleep() calls
- Use Qt test utilities

**Memory leaks:**
- Use smart pointers
- Check object ownership
- Run with valgrind

## Resources

- [Qt Test Documentation](https://doc.qt.io/qt-6/qtest-overview.html)
- [Rust Testing](https://doc.rust-lang.org/book/ch11-00-testing.html)
- [CMake CTest](https://cmake.org/cmake/help/latest/manual/ctest.1.html)
