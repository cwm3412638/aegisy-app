# Troubleshooting Guide

This guide covers common issues, diagnostic procedures, and recovery steps for Aegisy Desktop Client.

## Quick Diagnostics

**System Doctor**: Run the built-in system check from the main menu to verify Node.js, npm, Git, CLI tools, and configuration health.

**Log Locations**:
- macOS: `~/Library/Application Support/Aegisy/`
- Windows: `%APPDATA%/Aegisy/`
- Workbench data: `workbench/aegisy.db`, `workbench/aegisy.db-wal`
- Qt debug output: stderr (run from terminal to capture)

## 1. Build Issues

### CMake Configuration Failures

**Qt not found**:
```bash
# macOS with Homebrew
brew install qt@6
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/qt@6"

# Verify Qt installation
qmake --version
```

**OpenSSL missing**:
```bash
# macOS
brew install openssl
export OPENSSL_ROOT_DIR="/opt/homebrew/opt/openssl"

# Windows: Install from https://slproweb.com/products/Win32OpenSSL.html
```

**C++17 compiler required**:
- macOS: Install Xcode Command Line Tools: `xcode-select --install`
- Windows: Install Visual Studio 2019+ with C++ workload
- Linux: `sudo apt install build-essential cmake`

### Qt Version Conflicts

**Qt 5 vs Qt 6**:
- Developer builds default to Qt 5 fallback
- Release builds require Qt 6 with WebEngine: `cmake -DAEGISY_REQUIRE_QT6=ON`
- Check detected version: `grep "Qt6_FOUND\|Qt5" build/CMakeCache.txt`

**Missing WebEngine**:
```bash
# macOS
brew install qt-webengine

# Ubuntu/Debian
sudo apt install qt6-webengine-dev libqt6webenginewidgets6
```

### Dependency Issues

**Node.js required for gateway**:
```bash
node --version  # Requires 16+
npm --version
```

**Linux Secret Service**:
```bash
# Ubuntu/Debian
sudo apt install libsecret-1-dev libsecret-tools

# Verify
secret-tool --version
```

### Build Commands

```bash
# Clean build
rm -rf build
mkdir build && cd build
cmake ..
cmake --build . -j4

# Run tests
ctest --output-on-failure

# Specific test
ctest -R agent_runtime_protocol --verbose
```

## 2. Runtime Crashes

### Segmentation Faults

**Qt plugin loading**:
```bash
# Check Qt plugins
export QT_DEBUG_PLUGINS=1
./build/Aegisy

# Verify platform plugin
ls /opt/homebrew/opt/qt@6/plugins/platforms/
```

**Database corruption** (see section 5)

**Null pointer in API client**:
- Check network connectivity before API calls
- Verify `ApiClient` initialization in `main_window.cpp`
- Review `qWarning()` output for failed requests

### Application Won't Start

**macOS Gatekeeper**:
```bash
# Remove quarantine attribute
xattr -d com.apple.quarantine /Applications/Aegisy.app

# Check code signature
codesign -vvv --deep --strict /Applications/Aegisy.app
```

**Windows SmartScreen**:
- Right-click → Properties → Unblock
- Verify Authenticode signature: `signtool verify /pa Aegisy.exe`

**Missing libraries**:
```bash
# macOS: Check dynamic libraries
otool -L build/Aegisy.app/Contents/MacOS/Aegisy

# Linux
ldd build/Aegisy | grep "not found"
```

### Assertion Failures

**Check debug output**:
```bash
# Run from terminal to capture Qt warnings
./Aegisy 2>&1 | tee aegisy-debug.log
```

**Common assertions**:
- `Q_ASSERT` failures indicate logic errors; report with stack trace
- Database schema mismatches: delete `aegisy.db` to reset (loses data)
- Invalid JSON responses: check API endpoint health

## 3. Connection Issues

### AAP (Agent Protocol) Failures

**Symptoms**: "runtime unavailable", initialize timeout, capability negotiation failed

**Diagnostics**:
```bash
# Verify sidecar binary exists
ls -l agent-runtime/target/release/aegisy-agentd  # or debug/

# Check sidecar can start
./agent-runtime/target/release/aegisy-agentd --version

# Run protocol tests
cd agent-runtime
cargo test --workspace
```

**Handshake issues**:
- Verify `initialize` → `initialized` sequence in debug output
- Check protocol version compatibility (AAP 0.1)
- Confirm capability negotiation: `permission.read-only`, `runtime.health`
- Frame size limit: 4 MiB maximum

**Recovery**:
- Restart application (sidecar auto-restarts)
- Rebuild sidecar: `cd agent-runtime && cargo build --release`
- Check `runtime/health` and `runtime/degradations` endpoints

### Sidecar Startup Failures

**Binary not found**:
- Verify artifact manifest: `artifact-manifest.json` adjacent to binary
- Check SHA-256 hashes match
- Ensure no symlinks or path tampering

**Permission denied**:
```bash
# macOS/Linux
chmod +x agent-runtime/target/release/aegisy-agentd

# Windows: Run as administrator if needed
```

**Port conflicts** (Unix domain socket mode):
- Socket path: `~/.aegisy/runtime-socket/`
- Cleanup stale sockets: `rm -rf ~/.aegisy/runtime-socket/`

### API Connection Failures

**Login failed**:
- Verify network connectivity: `ping api.aegisy.com`
- Check TLS: `curl -v https://api.aegisy.com/health`
- Review credentials in secure storage
- Clear stored credentials: Settings → Account → Logout

**TLS initialization failed (Windows)**:
- Verify system clock is correct
- Check Windows version: Requires Windows 10 1809+
- Ensure OpenSSL/Schannel DLLs present
- Disable enterprise proxy/inspection temporarily
- Verify root certificates: `certutil -store Root`

**Gateway connection issues**:
- Check Node.js installed: `node --version`
- Verify gateway process: `ps aux | grep gateway`
- Check port availability: `lsof -i :3000` (macOS/Linux)
- Review gateway logs in application data directory

### Provider/Model Unavailable

**API key invalid**:
- Test key in API Keys dialog
- Check key permissions and quotas
- Verify model availability for key's group

**Model catalog stale**:
- Refresh model list in Models dialog
- Check catalog cache: `workbench/model-catalog-cache/`
- Verify catalog signing key ring

## 4. Performance Problems

### Slow Startup

**Database size**:
```bash
# Check database size
ls -lh ~/Library/Application\ Support/Aegisy/workbench/aegisy.db

# Vacuum database
sqlite3 aegisy.db "VACUUM;"
```

**Large chat history**:
- Archive old conversations
- Clear chat history: Settings → Data Management
- Limit retained messages per session

**Network delays**:
- Disable gateway mode if not needed
- Check DNS resolution: `nslookup api.aegisy.com`
- Use local model catalog cache

### High Memory Usage

**WebEngine memory**:
- Qt WebEngine (Monaco/xterm) uses 100-300 MB baseline
- Each chat session adds 50-100 MB
- Close unused chat windows
- Restart application periodically

**Database WAL growth**:
```bash
# Check WAL size
ls -lh aegisy.db-wal

# Checkpoint WAL
sqlite3 aegisy.db "PRAGMA wal_checkpoint(TRUNCATE);"
```

**Memory leaks**:
- Monitor with Activity Monitor (macOS) or Task Manager (Windows)
- Report sustained growth over 1 GB with reproduction steps

### CPU Spikes

**Streaming responses**:
- Normal during active AI responses
- Check provider latency in usage dialog

**Background processes**:
```bash
# Check for runaway processes
ps aux | grep aegisy
ps aux | grep node  # Gateway

# Kill stuck gateway
pkill -f "node.*gateway"
```

**Qt event loop**:
- Disable animations if sluggish: Settings → Appearance
- Check for infinite signal/slot loops in debug output

## 5. Database Corruption

### Symptoms

- "Store unavailable"
- "session/read failure"
- "reconciliation-required"
- Schema migration failure
- Application crashes on startup

### Recovery Steps

**1. Stop application completely**

**2. Backup data root**:
```bash
# macOS
cp -r ~/Library/Application\ Support/Aegisy/workbench ~/Desktop/aegisy-backup

# Windows
xcopy "%APPDATA%\Aegisy\workbench" "%USERPROFILE%\Desktop\aegisy-backup" /E /I
```

**3. Check database integrity**:
```bash
cd ~/Library/Application\ Support/Aegisy/workbench
sqlite3 aegisy.db "PRAGMA integrity_check;"
```

**4. Repair options**:

**Option A: WAL recovery**:
```bash
sqlite3 aegisy.db "PRAGMA wal_checkpoint(RESTART);"
```

**Option B: Export and reimport**:
```bash
sqlite3 aegisy.db .dump > backup.sql
mv aegisy.db aegisy.db.corrupt
sqlite3 aegisy.db < backup.sql
```

**Option C: Reset database** (loses all data):
```bash
rm aegisy.db aegisy.db-wal aegisy.db-shm
# Restart application to recreate
```

### Prevention

- Enable automatic backups: Settings → Data Management
- Regular exports: File → Export Diagnostic Bundle
- Don't force-quit during database writes
- Ensure sufficient disk space (1 GB minimum)

## 6. Update Failures

### Update Download Failed

**Network issues**:
- Check internet connectivity
- Disable VPN temporarily
- Retry download from Help → Check for Updates

**Signature verification failed**:
- Corrupted download; retry
- Check system clock is correct
- Verify update server: `curl -I https://updates.aegisy.com`

### Update Installation Failed

**Insufficient permissions**:
- macOS: Move app to /Applications before updating
- Windows: Run installer as administrator

**Disk space**:
- Requires 2x application size free space
- Clean up: `~/Library/Caches/Aegisy/` (macOS)

**Rollback**:
```bash
# macOS: Sparkle keeps previous version
ls ~/Library/Caches/Aegisy/

# Windows: Reinstall previous version from downloads
```

### Manual Installation

**macOS**:
1. Download DMG from https://aegisy.com/download
2. Verify SHA-256: `shasum -a 256 Aegisy.dmg`
3. Mount and drag to Applications
4. Remove quarantine: `xattr -cr /Applications/Aegisy.app`

**Windows**:
1. Download installer EXE
2. Verify signature: Right-click → Properties → Digital Signatures
3. Run installer
4. Restart application

### Update Stuck

**Clear update cache**:
```bash
# macOS
rm -rf ~/Library/Caches/Aegisy/Updates/

# Windows
del /s /q "%LOCALAPPDATA%\Aegisy\Updates"
```

**Disable auto-update temporarily**:
- Settings → Updates → Check manually only
- Restart application
- Re-enable after successful manual update

## 7. Platform-Specific Issues

### macOS

**Keychain access denied**:
```bash
# Reset keychain permissions
security delete-generic-password -s "com.aegisy.client"
# Restart app and re-login
```

**Notarization issues**:
```bash
# Check notarization
spctl -a -vv /Applications/Aegisy.app

# Expected: "source=Notarized Developer ID"
```

**Terminal integration**:
- iTerm2: Preferences → Profiles → Advanced → Semantic History
- Terminal.app: Automatically detected
- Check terminal selection: Settings → Terminal

**Sparkle update issues**:
- Check: `~/Library/Caches/Aegisy/`
- Clear: `defaults delete com.aegisy.client SULastCheckTime`

### Windows

**DPAPI encryption failed**:
- Indicates Windows credential store issue
- Workaround: Use profile export/import with password
- Check: `certutil -user -store My`

**WebView2 missing**:
- Download: https://go.microsoft.com/fwlink/?linkid=2124701
- Or install via winget: `winget install Microsoft.EdgeWebView2Runtime`

**Terminal integration**:
- Windows Terminal: Automatically detected
- PowerShell: Check execution policy: `Get-ExecutionPolicy`
- CMD: Fallback if others unavailable

**Installer issues**:
- Requires Windows 10 1809+ (build 17763)
- Check version: `winver`
- Run installer as administrator
- Disable antivirus temporarily

**Path length limits**:
- Enable long paths: `New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force`
- Or use shorter installation path

### Linux

**Secret Service unavailable**:
```bash
# Check D-Bus
systemctl --user status dbus

# Install secret service
sudo apt install gnome-keyring  # GNOME
sudo apt install kwalletmanager  # KDE

# Test
secret-tool store --label='test' test test
secret-tool lookup test test
```

**Qt platform plugin**:
```bash
export QT_QPA_PLATFORM=xcb  # or wayland
export QT_DEBUG_PLUGINS=1
./Aegisy
```

**Missing dependencies**:
```bash
# Ubuntu/Debian
sudo apt install libxcb-xinerama0 libxcb-cursor0

# Fedora
sudo dnf install xcb-util-cursor
```

## 8. Common Error Messages

### "Runtime unavailable"
- Sidecar failed to start or crashed
- Check sidecar binary exists and is executable
- Review section 3: AAP Failures
- Check artifact manifest integrity

### "TLS initialization failed"
- Windows: System clock, root certificates, OpenSSL DLLs
- macOS: Keychain access, certificate trust
- Network: Proxy, firewall, enterprise inspection
- See section 3: API Connection Failures

### "Database locked"
- Another instance running: check Task Manager/Activity Monitor
- Stale lock: delete `aegisy.db-shm`
- WAL checkpoint: `PRAGMA wal_checkpoint(RESTART);`

### "Session reconciliation required"
- Unconfirmed operation after crash
- Review session state in Timeline
- Safe to continue or create new session
- See section 5: Database Corruption

### "Capability not negotiated"
- AAP handshake incomplete
- Required capability missing from sidecar
- Check protocol version compatibility
- Restart application

### "Model unavailable"
- API key lacks access to model
- Model deprecated or removed
- Refresh model catalog
- Select different model

### "Quota exceeded"
- Account balance depleted
- Check usage in Usage dialog
- Top up balance or wait for reset
- Review API key quotas

### "Gateway connection failed"
- Node.js not installed
- Port conflict (default 3000)
- Gateway process crashed
- Disable gateway mode or restart

### "Configuration file corrupted"
- Invalid JSON/TOML in CLI config
- Backup exists: `~/.codex/config.toml.backup`
- Restore from backup or reset profile
- See section 5: Recovery Steps

### "Signature verification failed"
- Update artifact tampered or corrupted
- Retry download
- Check system clock
- Manual installation required

## Diagnostic Commands

### System Information
```bash
# Application version
./Aegisy --version

# Qt version
qmake --version

# Sidecar version
./aegisy-agentd --version

# System info
uname -a  # macOS/Linux
systeminfo  # Windows
```

### Database Inspection
```bash
cd ~/Library/Application\ Support/Aegisy/workbench

# Schema version
sqlite3 aegisy.db "PRAGMA user_version;"

# Table list
sqlite3 aegisy.db ".tables"

# Database size
sqlite3 aegisy.db "SELECT page_count * page_size as size FROM pragma_page_count(), pragma_page_size();"

# Integrity check
sqlite3 aegisy.db "PRAGMA integrity_check;"
```

### Network Diagnostics
```bash
# API connectivity
curl -v https://api.aegisy.com/health

# DNS resolution
nslookup api.aegisy.com

# TLS handshake
openssl s_client -connect api.aegisy.com:443 -servername api.aegisy.com

# Gateway status
curl http://localhost:3000/health
```

### Process Inspection
```bash
# Running processes
ps aux | grep -i aegisy

# Open files (macOS/Linux)
lsof -c Aegisy

# Network connections
lsof -i -P | grep Aegisy  # macOS/Linux
netstat -ano | findstr Aegisy  # Windows
```

## Getting Help

### Before Reporting

1. Check this troubleshooting guide
2. Run System Doctor from main menu
3. Review recent changes (updates, config edits)
4. Collect diagnostic information (see below)
5. Search existing issues

### Diagnostic Bundle

Export from: Help → Export Diagnostic Bundle

**Includes** (privacy-preserving):
- Application and sidecar versions
- Platform and system info
- Error classes and counts
- Configuration health status
- Database schema version
- Capability negotiation results

**Excludes**:
- API keys and credentials
- Chat history and prompts
- File contents and diffs
- Terminal output
- User paths and identifiers

### Report Template

```
**Environment**:
- OS: macOS 14.2 / Windows 11 / Ubuntu 22.04
- Application version: 2.5.2
- Sidecar version: 0.1.0
- Qt version: 6.5.3

**Issue**:
Clear description of the problem

**Steps to reproduce**:
1. Step one
2. Step two
3. Observed behavior

**Expected behavior**:
What should happen

**Logs/Errors**:
Relevant error messages (redact sensitive info)

**Diagnostic bundle**:
Attached: aegisy-diagnostic-2024-07-31.zip
```

### Support Channels

- GitHub Issues: https://github.com/aegisy/aegisy-app/issues
- Documentation: https://docs.aegisy.com
- Community: https://community.aegisy.com

### Emergency Recovery

**Complete reset** (loses all data):
```bash
# Backup first!
# macOS
rm -rf ~/Library/Application\ Support/Aegisy/
rm -rf ~/Library/Caches/Aegisy/

# Windows
rmdir /s "%APPDATA%\Aegisy"
rmdir /s "%LOCALAPPDATA%\Aegisy"

# Restart application for clean state
```
