# Release Checklist

## 1. Pre-Release

### 1.1 Version Bump
- [ ] Update version in `CMakeLists.txt` (line 2: `project(AegisyClient VERSION x.y.z)`)
- [ ] Verify version consistency:
  ```bash
  grep -n "VERSION" CMakeLists.txt | head -1
  ```

### 1.2 Changelog
- [ ] Create release notes in `release/notes/<version>.md`
- [ ] Update `CHANGELOG.md` with new version section
- [ ] Include breaking changes, new features, bug fixes, and known issues

### 1.3 Testing
- [ ] Run full test suite:
  ```bash
  ./scripts/run-tests.sh
  ```
- [ ] Verify Rust dependencies:
  ```bash
  ./scripts/verify-rust-dependencies.sh
  ```
- [ ] Check OpenSpec status:
  ```bash
  ./scripts/openspec-status.sh
  ```
- [ ] Run CMake tests:
  ```bash
  cd build-test && ctest --output-on-failure
  ```

## 2. Build

### 2.1 Clean Environment
- [ ] Remove old build artifacts:
  ```bash
  rm -rf build build-test build-preview dist
  ```

### 2.2 macOS Build
- [ ] Set signing identity (production only):
  ```bash
  export AEGISY_CODESIGN_IDENTITY="Developer ID Application: <name>"
  export AEGISY_NOTARY_PROFILE="<keychain-profile>"
  ```
- [ ] Set Sparkle key:
  ```bash
  export AEGISY_SPARKLE_PRIVATE_KEY="<ed25519-private-key>"
  ```
- [ ] Run packaging:
  ```bash
  ./package-macos.sh
  ```
- [ ] Verify output:
  ```bash
  ls -lh dist/AegisyClient-*.dmg
  ls -lh dist/updates/macos/AegisyClient-*.zip
  ```

### 2.3 Windows Build
- [ ] Set environment variables:
  ```cmd
  set AEGISY_WINDOWS_CERT_SHA1=<cert-thumbprint>
  set AEGISY_SPARKLE_PRIVATE_KEY_FILE=%USERPROFILE%\.aegisy\sparkle-private-key
  set OPENSSL_ROOT_DIR=C:\OpenSSL
  ```
- [ ] Run packaging:
  ```cmd
  package-windows.bat
  ```
- [ ] Verify output:
  ```cmd
  dir dist\AegisyClientSetup-*.exe
  dir dist\updates\windows\AegisyClientSetup-*.exe
  ```

## 3. Testing

### 3.1 Smoke Tests
- [ ] macOS: Mount DMG and launch app
  ```bash
  hdiutil attach dist/AegisyClient-*.dmg
  open /Volumes/Aegisy\ Client/AegisyClient.app
  ```
- [ ] Windows: Run installer and launch
- [ ] Verify app launches without crashes
- [ ] Check version in About dialog

### 3.2 Regression Testing
- [ ] Login with valid credentials
- [ ] Create and activate profile
- [ ] Test API key management
- [ ] Verify local gateway functionality
- [ ] Test chat dialog with streaming
- [ ] Verify image generation
- [ ] Test skill installation
- [ ] Check usage statistics display

### 3.3 Update Mechanism
- [ ] Verify appcast.xml generation:
  ```bash
  xmllint --noout dist/updates/macos/appcast.xml
  xmllint --noout dist/updates/windows/appcast.xml
  ```
- [ ] Validate Sparkle signatures:
  ```bash
  # macOS
  build/_deps/sparkle-*/bin/sign_update --verify \
    dist/updates/macos/AegisyClient-*.zip "<signature>"
  
  # Windows
  build\_deps\WinSparkle-*/bin\winsparkle-tool.exe verify \
    dist\updates\windows\AegisyClientSetup-*.exe
  ```

### 3.4 Platform-Specific Verification
- [ ] macOS: Verify code signature:
  ```bash
  codesign --verify --deep --strict --verbose=2 \
    /Volumes/Aegisy\ Client/AegisyClient.app
  spctl --assess --verbose=4 --type execute \
    /Volumes/Aegisy\ Client/AegisyClient.app
  ```
- [ ] macOS: Check notarization (if applicable):
  ```bash
  xcrun stapler validate /Volumes/Aegisy\ Client/AegisyClient.app
  ```
- [ ] Windows: Verify Authenticode signature:
  ```cmd
  signtool verify /pa /v dist\AegisyClientSetup-*.exe
  ```

## 4. Packaging

### 4.1 Generate Checksums
- [ ] macOS:
  ```bash
  shasum -a 256 dist/AegisyClient-*.dmg > dist/checksums-macos.txt
  shasum -a 256 dist/updates/macos/AegisyClient-*.zip >> dist/checksums-macos.txt
  ```
- [ ] Windows:
  ```cmd
  certutil -hashfile dist\AegisyClientSetup-*.exe SHA256 > dist\checksums-windows.txt
  ```

### 4.2 Verify Signatures
- [ ] Confirm Sparkle Ed25519 signatures in appcast.xml
- [ ] Verify code signing certificates are valid
- [ ] Check signature timestamps

### 4.3 Archive Build Artifacts
- [ ] Create release archive:
  ```bash
  VERSION=$(grep "project(AegisyClient VERSION" CMakeLists.txt | \
    sed -E 's/.*VERSION ([0-9.]+).*/\1/')
  tar -czf "aegisy-release-${VERSION}.tar.gz" \
    dist/*.dmg dist/*.exe dist/checksums-*.txt \
    dist/updates/*/appcast.xml
  ```

## 5. Distribution

### 5.1 Upload Artifacts
- [ ] Upload to CDN/hosting:
  ```bash
  # Example for macOS
  rsync -avz dist/updates/macos/ \
    user@server:/var/www/aegisy.cc/desktop/macos/
  
  # Example for Windows
  rsync -avz dist/updates/windows/ \
    user@server:/var/www/aegisy.cc/desktop/windows/
  ```
- [ ] Verify URLs are accessible:
  ```bash
  curl -I https://aegisy.cc/desktop/macos/appcast.xml
  curl -I https://aegisy.cc/desktop/windows/appcast.xml
  ```

### 5.2 Update Appcast
- [ ] Deploy appcast.xml to update server
- [ ] Verify appcast URL matches `AEGISY_UPDATE_BASE_URL`
- [ ] Test update check from previous version

### 5.3 Release Notes
- [ ] Publish release notes at configured URL
- [ ] Ensure Markdown formatting is correct
- [ ] Include upgrade instructions if needed

### 5.4 Git Tagging
- [ ] Create and push release tag:
  ```bash
  VERSION=$(grep "project(AegisyClient VERSION" CMakeLists.txt | \
    sed -E 's/.*VERSION ([0-9.]+).*/\1/')
  git tag -a "v${VERSION}" -m "Release v${VERSION}"
  git push origin "v${VERSION}"
  ```

## 6. Post-Release

### 6.1 Monitoring
- [ ] Monitor update server logs for download activity
- [ ] Check error reporting systems for crash reports
- [ ] Monitor user feedback channels
- [ ] Track update adoption rate

### 6.2 Hotfix Readiness
- [ ] Ensure build environment remains available
- [ ] Keep signing certificates accessible
- [ ] Document any build-specific configurations
- [ ] Maintain rollback artifacts

### 6.3 Documentation
- [ ] Update user guide if needed
- [ ] Update API documentation
- [ ] Notify support team of changes
- [ ] Update FAQ with known issues

## 7. Rollback Procedures

### 7.1 Immediate Rollback
If critical issues are discovered:
- [ ] Revert appcast.xml to previous version:
  ```bash
  # Restore from backup
  cp dist/updates/macos/appcast.xml.backup \
    dist/updates/macos/appcast.xml
  rsync -avz dist/updates/macos/appcast.xml \
    user@server:/var/www/aegisy.cc/desktop/macos/
  ```
- [ ] Verify old version is served to update checks
- [ ] Communicate rollback to users

### 7.2 Partial Rollback
For platform-specific issues:
- [ ] Edit appcast.xml to remove problematic version entry
- [ ] Keep working platform's update available
- [ ] Add release notes explaining the situation

### 7.3 Hotfix Release
For urgent fixes:
- [ ] Create hotfix branch from release tag:
  ```bash
  git checkout -b hotfix/v<version>-patch v<version>
  ```
- [ ] Apply minimal fix
- [ ] Bump patch version
- [ ] Follow abbreviated release process (skip full regression)
- [ ] Deploy hotfix update immediately

### 7.4 Communication
- [ ] Notify users via in-app message if possible
- [ ] Post announcement on website/social media
- [ ] Update release notes with rollback information
- [ ] Document incident for post-mortem

## Sign-Off

- [ ] Release Manager: _________________ Date: _______
- [ ] QA Lead: _________________ Date: _______
- [ ] Engineering Lead: _________________ Date: _______
