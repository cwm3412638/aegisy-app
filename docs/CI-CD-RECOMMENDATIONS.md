# CI/CD Configuration Recommendations

**Status**: Recommendations for Future Implementation  
**Created**: 2026-07-31

## Overview

This document provides recommendations for setting up continuous integration and
deployment pipelines for Aegisy Coding Workbench.

## Current State

- Manual builds on developer machines
- Manual testing via CTest
- Manual packaging for macOS and Windows
- Git-based version control with GitHub

## Recommended CI/CD Pipeline

### Phase 1: Basic CI (Immediate Priority)

#### Pull Request Checks
```yaml
# .github/workflows/pr-checks.yml
- Build verification (macOS, Windows)
- Run all CTest suites
- Rust workspace tests and Clippy
- OpenSpec validation
- Git diff checks (no trailing whitespace, LF line endings)
```

#### Automated Checks
- Code compiles on both platforms
- All tests pass
- No Clippy warnings in Rust code
- OpenSpec tasks.md is valid
- No merge conflicts

### Phase 2: Extended CI (Short Term)

#### Additional Checks
```yaml
- Code formatting verification (C++, Rust)
- Documentation generation
- Test coverage reporting
- Static analysis (cppcheck, clang-tidy)
- Dependency vulnerability scanning (cargo-deny)
```

#### Platform Matrix
- macOS: arm64, x64 (if supported)
- Windows: x64
- Qt versions: 5.15, 6.x
- Build types: Debug, Release

### Phase 3: CD Pipeline (Medium Term)

#### Automated Packaging
```yaml
# .github/workflows/release.yml
- Build signed packages on version tags
- Generate release notes from CHANGELOG.md
- Upload to GitHub Releases
- Update Sparkle/WinSparkle appcast feeds
- Verify installer signatures
```

#### Release Process
1. Tag version (e.g., v1.2.0)
2. CI builds and tests
3. Generate signed installers
4. Create GitHub release
5. Update appcast feeds
6. Notify users via update mechanism

### Phase 4: Advanced CD (Long Term)

#### Staged Rollout
- Internal builds (every commit to main)
- Preview builds (weekly)
- Beta builds (monthly)
- Stable releases (as needed)

#### Monitoring
- Crash reporting integration
- Usage telemetry (privacy-preserving)
- Update success rates
- Performance metrics

## Recommended Tools

### CI Platform
**GitHub Actions** (Recommended)
- Native GitHub integration
- Good macOS and Windows support
- Free for public repositories
- Mature ecosystem

Alternatives:
- CircleCI (good macOS support)
- Azure Pipelines (good Windows support)

### Build Tools
- **CMake**: Already in use
- **Ninja**: Faster builds than Make
- **ccache**: Build caching for C++
- **sccache**: Build caching for Rust

### Testing Tools
- **CTest**: Already in use
- **Valgrind**: Memory leak detection (Linux/macOS)
- **Dr. Memory**: Memory leak detection (Windows)
- **ASAN/UBSAN**: Sanitizers for C++

### Code Quality
- **clang-format**: C++ formatting
- **rustfmt**: Rust formatting (already configured)
- **Clippy**: Rust linting (already in use)
- **cppcheck**: C++ static analysis
- **clang-tidy**: C++ linting

### Packaging
- **Sparkle**: macOS updates (already in use)
- **WinSparkle**: Windows updates (already in use)
- **create-dmg**: macOS DMG creation
- **Inno Setup**: Windows installer (already in use)

## Security Considerations

### Code Signing
- Store signing certificates in GitHub Secrets
- Use separate certificates for different channels (internal/beta/stable)
- Implement certificate rotation policy
- Verify signatures in CI before upload

### Secrets Management
- Never commit secrets to repository
- Use GitHub Secrets for CI/CD secrets
- Rotate secrets regularly
- Audit secret access

### Supply Chain Security
- Pin all dependencies (Cargo.lock, package-lock.json)
- Verify dependency checksums
- Run cargo-deny in CI
- Monitor security advisories

## Implementation Roadmap

### Week 1-2: Basic CI
- [ ] Create .github/workflows/pr-checks.yml
- [ ] Configure macOS and Windows runners
- [ ] Set up CTest execution
- [ ] Add Rust workspace tests
- [ ] Enable OpenSpec validation

### Week 3-4: Extended CI
- [ ] Add code formatting checks
- [ ] Configure static analysis
- [ ] Set up test coverage reporting
- [ ] Add dependency scanning

### Month 2: CD Pipeline
- [ ] Create release workflow
- [ ] Configure code signing
- [ ] Set up appcast feed generation
- [ ] Test end-to-end release process

### Month 3+: Advanced Features
- [ ] Implement staged rollout
- [ ] Add crash reporting
- [ ] Set up performance monitoring
- [ ] Create internal/preview/beta channels

## Example Workflows

### Pull Request Check
```yaml
name: PR Checks

on:
  pull_request:
    branches: [ main ]

jobs:
  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v3
      - name: Install Qt
        run: brew install qt
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --parallel
      - name: Test
        run: cd build && ctest --output-on-failure

  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Install Qt
        run: choco install qt6
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --parallel
      - name: Test
        run: cd build && ctest --output-on-failure

  rust-checks:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Rust Tests
        run: cd agent-runtime && cargo test
      - name: Clippy
        run: cd agent-runtime && cargo clippy -- -D warnings
      - name: Format Check
        run: cd agent-runtime && cargo fmt --check
```

### Release Workflow
```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build and Package
        run: ./package-macos.sh
      - name: Sign
        env:
          SIGNING_CERT: ${{ secrets.MACOS_SIGNING_CERT }}
        run: codesign --sign "$SIGNING_CERT" dist/*.dmg
      - name: Upload
        uses: actions/upload-artifact@v3
        with:
          name: macos-installer
          path: dist/*.dmg

  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build and Package
        run: .\package-windows.bat
      - name: Sign
        env:
          SIGNING_CERT: ${{ secrets.WINDOWS_SIGNING_CERT }}
        run: signtool sign /f "$env:SIGNING_CERT" dist\*.exe
      - name: Upload
        uses: actions/upload-artifact@v3
        with:
          name: windows-installer
          path: dist\*.exe

  create-release:
    needs: [build-macos, build-windows]
    runs-on: ubuntu-latest
    steps:
      - name: Download Artifacts
        uses: actions/download-artifact@v3
      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: |
            macos-installer/*
            windows-installer/*
          generate_release_notes: true
```

## Monitoring and Alerts

### Build Health
- Monitor build success rate
- Track build duration trends
- Alert on consecutive failures
- Report flaky tests

### Release Health
- Monitor update success rate
- Track crash rates by version
- Alert on critical issues
- Monitor rollback requests

## Cost Considerations

### GitHub Actions
- Free for public repositories
- 2,000 minutes/month for private repos (free tier)
- macOS runners: 10x multiplier
- Windows runners: 2x multiplier

### Estimated Usage
- PR checks: ~30 minutes per PR (macOS + Windows)
- Release builds: ~60 minutes per release
- Monthly cost (private repo): ~$50-100 for active development

## References

- GitHub Actions Documentation: https://docs.github.com/actions
- CMake CI Best Practices: https://cmake.org/cmake/help/latest/guide/tutorial/
- Qt CI Examples: https://doc.qt.io/qt-6/ci.html
- Rust CI Guide: https://doc.rust-lang.org/cargo/guide/continuous-integration.html

## Approval

This document requires approval from:

- [ ] Engineering Lead - CI/CD architecture
- [ ] DevOps Lead - Infrastructure and costs
- [ ] Security Lead - Security practices
- [ ] Release Manager - Release process

---

**Next Steps**: Create basic PR checks workflow as proof of concept
