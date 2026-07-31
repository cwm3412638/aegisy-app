# Packaging and Release Design

## Overview

This document defines the packaging, update, migration, and release requirements for Aegisy Agent Workbench on macOS and Windows. It covers artifact verification, delta updates, legacy-to-workbench migration, signing/notarization, and quality gates.

## Packaging Requirements

### macOS Bundle Structure

```
Aegisy.app/
├── Contents/
│   ├── MacOS/
│   │   ├── Aegisy                    # Qt application
│   │   └── aegisy-agentd             # Rust sidecar
│   ├── Resources/
│   │   ├── workbench/                # Web bundle (Monaco, xterm.js)
│   │   ├── codex-cli                 # Pinned Codex App Server 0.144.5
│   │   ├── schemas/                  # AAP schemas
│   │   ├── artifact-manifest.json    # Verified artifact manifest
│   │   └── licenses/                 # NOTICE, third-party licenses
│   └── Info.plist
```

**Requirements:**
- Signed with Developer ID Application certificate
- Notarized through Apple notary service
- Hardened runtime enabled
- Entitlements: camera (future), microphone (future), network client
- Universal binary (arm64 + x86_64) or separate architecture-specific builds
- Minimum target: macOS 11.0 (Big Sur)

### Windows Bundle Structure

```
Aegisy/
├── Aegisy.exe                        # Qt application
├── aegisy-agentd.exe                 # Rust sidecar
├── workbench/                        # Web bundle
├── codex-cli.exe                     # Pinned Codex App Server 0.144.5
├── schemas/                          # AAP schemas
├── artifact-manifest.json            # Verified artifact manifest
├── licenses/                         # NOTICE, third-party licenses
└── [Qt/OpenSSL DLLs]
```

**Requirements:**
- Signed with Authenticode certificate
- Application manifest with requestedExecutionLevel=asInvoker, longPathAware=true
- Minimum target: Windows 10 1809 (build 17763)
- Architecture: x64 (ARM64 future consideration)
- Installer: Inno Setup with MinVersion=10.0.17763

### Artifact Manifest

`aegisy-artifact-manifest/0.1` validates sidecar and adapter binaries before launch:

- **Manifest location:** Adjacent to application binary
- **Contents:** Runtime identity, adapter identity, relative paths, SHA-256 hashes, version metadata
- **Verification:** Qt validates manifest before launching `aegisy-agentd`; Runtime validates before selecting Codex
- **Failure modes:** Missing manifest (developer builds only), hash mismatch, path tampering, extra hard links → fail closed
- **Path policy:** Portable relative paths, canonical in-tree files, no symlinks/reparse points

**Generation:** `cmake/generate_artifact_manifest.cmake` produces deterministic manifest from assembled bundle.

## Update Mechanism

### Signed Update Architecture

Updates use a cryptographic chain of trust:

1. **Trust Anchor:** Compile-time `aegisy-update-signing-trust-anchor/0.1` with embedded Ed25519 root public key
2. **Key Ring:** `aegisy-update-signing-key-ring/0.1` signed by root or prior ring
   - Sequential rotation with monotonic generation
   - Retains prior keys for verification
   - Cannot widen validity windows or reverse revocation
3. **Artifact Set:** `aegisy-update-artifact-set/0.2` signed by active key
   - Binds receipt/candidate signer, signing/expiry times
   - Source and target application size/SHA-256
   - Complete manifest of included files
   - Requires `InstalledArtifactSetAuthority` for evaluation

**Key Rotation:**
- Advances exactly one generation
- Preserves prior Key IDs and public keys
- Requires monotonic Ring signing time
- First-time bootstrap requires signer active at verification time
- Idempotent replay allowed for exact accepted envelopes

### Delta Updates

**Sparkle (macOS):**
- Binary delta patches using bsdiff
- Reduces download size for incremental updates
- Resume support for interrupted downloads
- Signature verification before application

**WinSparkle (Windows):**
- Full installer downloads (delta support limited)
- Pre-download veto mechanism (implementation gap)
- Signature verification before installation

**Compatibility Checks:**
- Sidecar version compatibility matrix
- Adapter version compatibility (Codex 0.144.5 pinned)
- Database schema migration support (v1-v20)
- Rollback to last compatible version on failure

### Update Progress Tracking

`aegisy-update-progress-record/0.1` provides local continuity:
- Current state: checking, downloading, ready, installing, failed
- Download progress, installed version, candidate version
- Rollback metadata
- **Authority:** Fixed false for download/install (framework-controlled)

**Current Gaps:**
- No authenticated Ring fetch
- No persistent anti-rollback high-water mark
- Sparkle resume/delta integration incomplete
- WinSparkle pre-download veto gap

## Migration from Legacy to Workbench

### Data Isolation

**Separate Data Roots:**
- **Legacy:** QSettings profile state, existing chat history, connection profiles
- **Workbench:** Versioned data root with SQLite database, event journal, blob store, checkpoints

**Workbench Data Root Structure:**
```
~/Library/Application Support/Aegisy/workbench/  (macOS)
%APPDATA%/Aegisy/workbench/                      (Windows)
├── aegisy.db                    # SQLite database (WAL mode)
├── aegisy.db-wal
├── aegisy.db-shm
├── blobs/                       # Content-addressed artifacts
├── checkpoints/                 # Git and non-Git checkpoints
├── model-catalog-cache/         # Signed catalog cache
├── model-catalog-trust/         # Key ring trust store
├── pinned-context/              # Pinned context objects
└── backups/                     # Pre-migration backups
```

### Migration Strategy

**Phase 1: Feature-Flagged Coexistence**
- Workbench behind disabled feature flag initially
- Legacy main window, chat, profiles, gateway unchanged
- No automatic data migration

**Phase 2: Opt-In Import (Task 22.2)**
- Import active model selection → default model profile
- Import Skills configuration → unified extension registry
- Import MCP configuration → MCP server registry
- Import project history (if applicable)
- **Non-destructive:** Source configuration preserved, reversible with backup

**Phase 3: Gradual Promotion**
- Internal → Preview → Beta → Stable channels
- Workbench becomes default only after parity/reliability gates pass
- One-click return to legacy management pages during transition

### Failure Isolation (Task 22.3)

**Critical Requirement:** Workbench startup or migration failure cannot block:
- Login and authentication
- Legacy connection management
- Gateway process
- Update checks and installation
- Logout

**Implementation:**
- Separate initialization paths
- Workbench errors logged but non-fatal to application launch
- Read-only recovery mode for database corruption
- Diagnostic export available even in degraded state

## Signing and Notarization

### macOS Code Signing

**Certificate:** Developer ID Application
**Signing Command:**
```bash
codesign --sign "Developer ID Application: [Team]" \
         --options runtime \
         --entitlements Aegisy.entitlements \
         --timestamp \
         --deep \
         Aegisy.app
```

**Entitlements:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "...">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>  <!-- For Qt WebEngine -->
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>  <!-- For Qt WebEngine -->
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>  <!-- For Qt plugins -->
    <key>com.apple.security.network.client</key>
    <true/>
</dict>
</plist>
```

**Notarization:**
```bash
# Submit for notarization
xcrun notarytool submit Aegisy.dmg \
    --apple-id [email] \
    --team-id [team] \
    --password [app-specific-password] \
    --wait

# Staple ticket to DMG
xcrun stapler staple Aegisy.dmg
```

**Verification:**
```bash
codesign --verify --deep --strict --verbose=2 Aegisy.app
spctl --assess --verbose=4 --type execute Aegisy.app
xcrun stapler validate Aegisy.dmg
```

### Windows Code Signing

**Certificate:** Authenticode (EV or OV)
**Signing Command:**
```powershell
signtool sign /f certificate.pfx /p password /tr http://timestamp.digicert.com /td sha256 /fd sha256 Aegisy.exe
signtool sign /f certificate.pfx /p password /tr http://timestamp.digicert.com /td sha256 /fd sha256 aegisy-agentd.exe
signtool sign /f certificate.pfx /p password /tr http://timestamp.digicert.com /td sha256 /fd sha256 AegisySetup.exe
```

**Verification:**
```powershell
signtool verify /pa /v Aegisy.exe
```

**SmartScreen Reputation:**
- EV certificates bypass SmartScreen initially
- OV certificates require reputation building
- Consistent signing identity across releases
- Sufficient download volume for reputation

## Release Gates and Quality Requirements

### Milestone 0 Performance Budgets (Task 1.6)

**Installer Size:**
- Compressed: ≤ 150 MB (macOS), ≤ 200 MB (Windows)
- Installed: ≤ 400 MB (macOS), ≤ 500 MB (Windows)

**Startup Performance:**
- Cold start (legacy): ≤ 3s to main window
- Cold start (workbench): ≤ 5s to workbench ready
- Warm start: ≤ 2s

**Memory:**
- Idle (legacy): ≤ 200 MB
- Idle (workbench): ≤ 400 MB
- Active (workbench + session): ≤ 800 MB

**Editor Performance:**
- File open: ≤ 500ms (10K lines)
- Input latency: ≤ 50ms (p95)
- Save: ≤ 200ms

**Terminal:**
- PTY echo: ≤ 100ms (p95)
- Throughput: ≥ 10 MB/s

**Indexing:**
- Initial: ≤ 30s (5K files)
- Incremental: ≤ 5s (100 changed files)

**Crash Recovery:**
- Renderer crash: ≤ 2s to fallback
- Sidecar crash: ≤ 5s to reconnect
- Full app crash: ≤ 10s to restore session

### Platform Support Matrix (Task 1.7)

**macOS:**
- Supported: macOS 11.0+ (Big Sur and later)
- Architectures: arm64 (Apple Silicon), x86_64 (Intel)
- Filesystems: APFS (required), HFS+ (unsupported)
- Display: 1x, 2x (Retina)
- Evidence required: Signed package on clean arm64 and x86_64 machines

**Windows:**
- Supported: Windows 10 1809+ (build 17763)
- Architectures: x64 (ARM64 future)
- Filesystems: NTFS (required)
- Display: 100%, 125%, 150%, 200% scaling
- Long paths: Enabled via manifest
- Evidence required: Signed package on clean Windows 10/11 x64 machines

**Unsupported:**
- Network filesystems (SMB, NFS)
- Cloud storage (Dropbox, OneDrive, iCloud Drive)
- Removable media
- WSL filesystems (from Windows host)

### Security and Sandbox Gates (Task 18.12)

**Requirement:** Write-capable release blocked until platform sandbox passes.

**macOS Sandbox:**
- Codex App Server sandbox profiles (initial)
- Native Aegisy sandbox (future)
- Verified through adversarial test suite

**Windows Sandbox:**
- Restricted execution without admin rights
- Process, filesystem, network enforcement
- Job Objects for process tree termination
- Verified through adversarial test suite

**Adversarial Tests:**
- Path escape attempts
- Symlink race conditions
- Command wrapping
- Credential leak via redirects
- Forged approval messages
- Prompt injection
- Sandbox bypass attempts

### Feature Channel Policy (Task 1.8)

**Channels:**
- **Internal:** Aegisy employees only, all features enabled
- **Preview:** Early adopters, experimental features opt-in
- **Beta:** Public beta, stable features only
- **Stable:** General availability, proven features

**Remote/Background Gates:**
- Remote execution: Fixed unavailable pending separate OpenSpec
- Background jobs: Gated by tasks 21.1-21.12
- Unattended writes: Requires permission/sandbox/recovery gates

**Emergency Disable (Task 22.6):**
- Server-controlled feature disable via signed policy
- Does not expose session content
- Preserves local history export
- Authenticated HTTPS endpoint
- Ed25519 signature verification
- Monotonic sequence with anti-rollback

### Database and Recovery Gates

**Schema Migrations (Task 5.6):**
- Transactional v1→v20 migrations
- WAL-consistent pre-upgrade backup
- Read-only recovery on failure
- Never auto-delete history
- Tested from every supported schema version

**Corruption Recovery:**
- Rebuildable projections from event journal
- Consistency verifier for sequence/checksums
- Bounded scan for orphaned blobs
- Read-only mode for uncertain state
- Diagnostic export without content

**Retention and Cleanup (Task 5.8):**
- 24-hour to 30-day undo window
- Integrity-checked garbage collection
- Lineage-preserving tombstones
- No automatic deletion of uncertain orphans

### Cross-Platform Evidence Requirements

**macOS Evidence (Task 22.7):**
- Signed/notarized package installation
- Update and rollback
- Multi-display behavior
- Chinese IME input
- VoiceOver accessibility
- Sandbox enforcement

**Windows Evidence (Task 22.8):**
- Signed installer on clean machine
- Install/upgrade/rollback
- 125%/150% display scaling
- ConPTY terminal functionality
- Git operations
- Sandbox enforcement
- Antivirus compatibility
- Long path support

### Release Criteria (Task 22.9)

**Internal Release:**
- All unit and integration tests pass
- Performance budgets met
- No known critical bugs
- Internal dogfooding for 1 week

**Preview Release:**
- Milestone 0 spike complete
- Read-only Chat mode functional
- Session persistence working
- Known limitations documented

**Beta Release:**
- Single-agent Work mode complete
- File operations functional
- Terminal and Git read-only
- Security gates passed
- Migration from legacy tested

**Stable Release:**
- All stable features complete
- Cross-platform evidence collected
- Support playbooks ready
- Parity with legacy features
- Reliability metrics met (crash rate < 0.1%)
- Adoption threshold met (internal usage)

## Packaging Workflow

### Build Process

1. **Compile native code:**
   - Qt application (Release configuration)
   - Rust sidecar (`cargo build --release`)
   - Pinned Codex adapter (verified binary)

2. **Bundle web assets:**
   - Monaco editor (0.55.1)
   - xterm.js (6.0.0) + fit addon (0.11.0)
   - Webpack/esbuild production build
   - Content Security Policy enforcement

3. **Generate artifact manifest:**
   - Run `cmake/generate_artifact_manifest.cmake`
   - Verify deterministic output
   - Include in bundle

4. **Assemble licenses:**
   - NOTICE file with all third-party components
   - Individual license files
   - Verify against third-party inventory (Task 1.4)

5. **Sign binaries:**
   - macOS: codesign with hardened runtime
   - Windows: signtool with Authenticode

6. **Create installer:**
   - macOS: DMG with background image, symlink to /Applications
   - Windows: Inno Setup with custom pages

7. **Notarize (macOS):**
   - Submit to Apple notary service
   - Wait for approval
   - Staple ticket to DMG

8. **Sign installer:**
   - macOS: codesign DMG
   - Windows: signtool setup executable

9. **Verify signatures:**
   - Run verification commands
   - Test installation on clean VM

### Continuous Integration

**GitHub Actions Workflows:**

**macOS Build:**
```yaml
- Checkout with Unicode path validation
- Install Qt, Rust toolchain
- Build Release configuration
- Run CTest suite (unfiltered)
- Generate artifact manifest
- Sign binaries (if release branch)
- Create DMG
- Notarize (if release branch)
- Upload artifacts
```

**Windows Build:**
```yaml
- Checkout to Unicode path (windows-验证-源码)
- Reject ASCII path or dirty Git state
- Install Qt, Rust toolchain, MSVC
- Build Release configuration
- Run CTest suite (unfiltered, including Unicode E2E)
- Generate artifact manifest
- Sign binaries (if release branch)
- Create installer
- Upload artifacts
```

**Validation:**
- Repository policy test enforces complete trigger set
- Requires unfiltered CTest command
- Absolute Unicode installer artifact path
- LF-pinned workflow files

## Known Gaps and Future Work

### Current Implementation Gaps

1. **Artifact Manifest (Task 22.5):**
   - Developer builds may omit manifest
   - Need non-downgradable require-manifest identity
   - Verify-path-to-spawn replacement window
   - Updater compatibility binding incomplete

2. **Update Mechanism:**
   - No authenticated Key Ring fetch
   - No persistent anti-rollback high-water mark
   - Sparkle resume/delta integration incomplete
   - WinSparkle pre-download veto gap

3. **Emergency Disable (Task 22.6):**
   - Production endpoint/signing publisher missing
   - First install with no cached policy uses current behavior
   - QSettings high-water marker not OS-secure
   - Healthy-Store diagnostic bundle exporter absent

4. **Windows Evidence:**
   - Clean Windows runner execution incomplete
   - ConPTY terminal evidence pending
   - Long path support not verified
   - Sandbox enforcement not tested

### Future Enhancements

1. **Differential Updates:**
   - Improve delta compression
   - Resume support for large downloads
   - Bandwidth-aware scheduling

2. **Rollback Improvements:**
   - Automatic rollback on startup failure
   - Multiple rollback slots
   - User-initiated rollback from UI

3. **Migration Tools:**
   - Bulk project import
   - Legacy chat history conversion
   - Configuration migration wizard

4. **Monitoring:**
   - Update success/failure telemetry
   - Crash reporting integration
   - Performance regression detection

## References

- Design document: `openspec/changes/build-aegisy-agent-workbench/design.md`
- Tasks: `openspec/changes/build-aegisy-agent-workbench/tasks.md` (Section 22)
- Third-party inventory: `docs/THIRD-PARTY-COMPONENT-INVENTORY.md`
- Platform matrix: `docs/AEGISY-SUPPORTED-PLATFORM-MATRIX.md`
- Performance budgets: `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md`
- Feature channel policy: `docs/AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md`
- Codex adapter upgrade: `docs/CODEX-ADAPTER-UPGRADE.md`
- Troubleshooting: `docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md`
