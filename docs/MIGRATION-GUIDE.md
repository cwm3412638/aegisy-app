# Aegisy Coding Workbench Migration Guide

Welcome to Aegisy Coding Workbench! This guide will help you smoothly transition from legacy Aegisy Desktop Client to the new integrated coding environment.

## What's Changing

### From Desktop Client to Coding Workbench

**Legacy Aegisy Desktop Client** focused on managing API keys and CLI tool configurations. **Aegisy Coding Workbench** expands this foundation into a complete AI-powered development environment.

#### Key Changes

| Area | Legacy Client | Coding Workbench |
|------|---------------|------------------|
| **Primary Focus** | API key & CLI configuration management | Full-featured AI coding environment with integrated agent runtime |
| **UI Architecture** | Qt desktop application | Qt desktop + embedded web workbench |
| **AI Interaction** | External CLI tools (Claude Code, Codex, Gemini) | Native integrated agent with Monaco editor, terminal, Git |
| **Project Management** | Directory selection for CLI launch | Persistent project workspaces with session history |
| **Data Storage** | QSettings + system keychain | SQLite workbench store + secure credential vault |
| **Chat Interface** | Standalone AI chat dialog | Integrated Chat/Work modes with structured context |

### What Stays the Same

- API key management and Aegisy account integration
- System credential storage (DPAPI/Keychain/Secret Service)
- Profile-based configuration for external CLI tools
- Local gateway mode for request monitoring
- Desktop enhancement features (plugins, model lists, Claude localization)
- Skills management and execution
- Encrypted profile import/export

## Data Migration

### Automatic Migration

When you first launch Aegisy Coding Workbench, the application will automatically:

1. **Preserve existing profiles**: All Claude Code, Codex CLI, Gemini CLI, and OpenCode configurations remain intact
2. **Migrate credentials**: API keys stored in system keychain are automatically accessible
3. **Retain settings**: QSettings data (active profiles, preferences) carries over
4. **Keep backups**: All configuration backups in `~/.aegisy/backups/` are preserved
5. **Maintain chat history**: AI conversation history from the legacy client is retained

### Manual Data Review

After migration, verify these items:

#### Profile Configurations
```bash
# Check that your profiles are present
# macOS/Linux: ~/.config/Aegisy/AegisyClient.conf
# Windows: Registry HKEY_CURRENT_USER\Software\Aegisy\AegisyClient
```

#### CLI Tool Configurations
- Claude Code: `~/.claude/settings.json`
- Codex CLI: `~/.codex/auth.json`, `~/.codex/config.toml`
- Gemini CLI: `~/.gemini/.env`
- OpenCode: `~/.config/opencode/config.json`

#### Backup Archives
```bash
# Verify backup directory exists
ls ~/.aegisy/backups/
```

### What Gets Migrated

✅ **Automatically Migrated**
- API keys and authentication tokens
- Profile configurations (name, tool, model selections)
- Active profile selections per tool
- Chat conversation history
- Skills installations and configurations
- MCP server configurations
- Local gateway settings
- Desktop enhancement preferences

❌ **Not Migrated** (Workbench-specific)
- Project workspaces (new feature)
- Agent session history (new feature)
- Monaco editor state (new feature)
- Terminal session history (new feature)
- Git integration state (new feature)

## Feature Mapping

### Legacy Client → Coding Workbench

| Legacy Feature | Workbench Equivalent | Notes |
|----------------|---------------------|-------|
| Profile Management | Profile Management | Same interface, same functionality |
| API Keys | API Keys | Unchanged |
| AI Chat | Chat Mode | Enhanced with project context |
| Skills | Skills + Agent Runtime | Skills now integrate with agent execution |
| System Doctor | System Doctor | Expanded with workbench health checks |
| Desktop Enhancement | Desktop Enhancement | Unchanged |
| Local Gateway | Local Gateway | Unchanged |
| Usage Center | Usage Center | Unchanged |
| Launch CLI | Launch CLI + Work Mode | Can launch external CLI or use integrated agent |
| MCP Configuration | MCP Configuration | Unchanged |

### New Features in Workbench

**Work Mode**: Native AI coding agent with:
- Monaco code editor with syntax highlighting
- Integrated terminal
- Read-only Git status viewer
- Structured context engine
- Background job execution with recovery

**Project Management**:
- Persistent project workspaces
- Session history and recovery
- Multi-file editing support
- Context-aware code assistance

**Advanced Agent Runtime**:
- Durable job scheduling with lease management
- Recovery snapshots and decision logging
- Background notification intents
- Process observation and lifecycle management

## Breaking Changes

### 1. Application Binary Name

**Before**: `AegisyClient` / `AegisyClient.exe`  
**After**: `AegisyClient` / `AegisyClient.exe` (unchanged)

No breaking change - the binary name remains the same.

### 2. Configuration File Locations

All configuration locations remain unchanged. The workbench adds new data stores but does not modify existing paths.

### 3. API Compatibility

The workbench maintains full backward compatibility with:
- Aegisy API endpoints
- CLI tool configuration formats
- Encrypted profile archive format
- System credential storage APIs

### 4. Workbench-Specific Limitations

**Current Restrictions** (as documented in README):
- Agent runtime is **read-only** for Codex integration
- Background jobs do not auto-dispatch or auto-approve
- No automatic process takeover or unattended writes
- Recovery logs are evidence-only, do not mutate job state
- Notification intents are metadata-only, no platform delivery
- No scheduler auto-production of background work

These are safety constraints, not bugs. The workbench provides infrastructure for future capabilities while maintaining strict safety boundaries.

## Migration Steps

### Step 1: Backup Current Configuration

Before upgrading, create a safety backup:

```bash
# Export all profiles using legacy client
# Open Aegisy Client → Migration → Export Profiles
# Save to a secure location with a strong password
```

### Step 2: Install Aegisy Coding Workbench

**macOS**:
```bash
# Download from https://aegisy.cc/desktop/macos/
# Or use Homebrew (if available)
brew install --cask aegisy-workbench
```

**Windows**:
```powershell
# Download installer from https://aegisy.cc/desktop/windows/
# Run AegisyWorkbenchSetup.exe
# Follow installation wizard
```

**Linux**:
```bash
# Download AppImage or .deb from https://aegisy.cc/desktop/linux/
# Make executable and run
chmod +x AegisyWorkbench.AppImage
./AegisyWorkbench.AppImage
```

### Step 3: First Launch

1. Launch Aegisy Coding Workbench
2. Log in with your Aegisy account credentials
3. The application will automatically detect and migrate existing data
4. Review the migration summary dialog

### Step 4: Verify Migration

Check that your data migrated correctly:

1. **Profiles**: Open Profile Management, verify all profiles are present
2. **API Keys**: Check API Keys page, confirm keys are accessible
3. **Active Configurations**: Verify active profile per tool (Claude/Codex/Gemini/OpenCode)
4. **Chat History**: Open AI Chat, check conversation history
5. **Skills**: Open Skills page, verify installed skills

### Step 5: Test Functionality

1. **Activate a profile**: Select a profile and click "Activate"
2. **Test connection**: Click "Test" to verify API connectivity
3. **Launch CLI** (optional): Click "Launch" to test external CLI integration
4. **Try Work Mode** (new): Click "Work" sidebar to explore the integrated agent

### Step 6: Explore New Features

- Create your first project workspace in Work Mode
- Try the integrated Monaco editor
- Explore the terminal integration
- Review background job notifications (metadata-only)

## Rollback Procedures

If you need to return to the legacy client:

### Option 1: Keep Both Installed

The legacy client and workbench can coexist. They share the same configuration data, so you can run either application.

**To use legacy client**:
1. Launch the legacy `AegisyClient` binary
2. All profiles and settings remain accessible
3. No data loss occurs

### Option 2: Uninstall Workbench

**macOS**:
```bash
# If installed via Homebrew
brew uninstall --cask aegisy-workbench

# If installed manually
rm -rf /Applications/AegisyWorkbench.app
```

**Windows**:
```powershell
# Use Windows Settings → Apps → Uninstall
# Or run uninstaller from installation directory
```

**Linux**:
```bash
# Remove AppImage
rm AegisyWorkbench.AppImage

# Or uninstall package
sudo apt remove aegisy-workbench  # Debian/Ubuntu
```

### Option 3: Restore from Backup

If you need to restore configuration:

1. Open legacy Aegisy Client
2. Go to Migration → Import Profiles
3. Select your backup `.aegisy` file
4. Enter the password you used during export
5. Confirm import

**Note**: Backup restoration overwrites current profiles. Create a new backup before restoring if you want to preserve any changes made in the workbench.

## Frequently Asked Questions

### General Questions

**Q: Do I need to uninstall the legacy client?**  
A: No. Both applications can coexist and share the same configuration data. You can use whichever interface you prefer.

**Q: Will my API keys be exposed during migration?**  
A: No. API keys remain in system secure storage (DPAPI/Keychain/Secret Service) and are never written to plain text files or logs.

**Q: Can I go back to the legacy client after trying the workbench?**  
A: Yes. Simply launch the legacy client binary. All your profiles and settings remain accessible.

**Q: Will the workbench replace my CLI tools?**  
A: No. The workbench provides an integrated agent as an alternative, but you can still use external CLI tools (Claude Code, Codex, Gemini, OpenCode) exactly as before.

### Profile & Configuration

**Q: What happens to my active profile selections?**  
A: They are preserved. Each tool (Claude/Codex/Gemini/OpenCode) retains its active profile selection.

**Q: Are my configuration backups still valid?**  
A: Yes. All backups in `~/.aegisy/backups/` remain valid and can be restored using either the legacy client or workbench.

**Q: Can I export profiles from the workbench and import them in the legacy client?**  
A: Yes. The encrypted profile archive format (`.aegisy`) is identical in both applications.

**Q: Will activating a profile in the workbench affect external CLI tools?**  
A: Yes. Profile activation writes to the same configuration files (`~/.claude/settings.json`, `~/.codex/auth.json`, etc.), so changes apply to both the workbench and external CLI tools.

### Workbench Features

**Q: Is the integrated agent as capable as Claude Code or Codex CLI?**  
A: The workbench agent is currently in preview with read-only Codex integration. It provides Monaco editor, terminal, and Git viewing, but does not yet support full autonomous code modification. External CLI tools remain the recommended option for production coding workflows.

**Q: What are "background notification intents"?**  
A: These are metadata-only records of background job state changes (completion, failure, approval needed, budget exhausted). They are stored in the workbench database but not delivered as system notifications. This is infrastructure for future notification features.

**Q: Can the workbench agent automatically fix bugs or write code?**  
A: Not yet. The current agent runtime is read-only and does not perform unattended writes. All modifications require explicit user approval. This is a safety constraint, not a limitation.

**Q: Why can't I see background job progress in real-time?**  
A: The current implementation provides metadata-only inspection through session menus. Real-time progress tracking, automatic retry, and notification delivery are planned for future releases.

### Data & Privacy

**Q: Where is workbench data stored?**  
A: 
- **Profiles & settings**: Same as legacy client (`QSettings` + system keychain)
- **Project workspaces**: `~/.aegisy/workbench/projects/`
- **Session history**: `~/.aegisy/workbench/sessions/`
- **SQLite database**: `~/.aegisy/workbench/store.db`

**Q: Is my code stored on Aegisy servers?**  
A: No. All project files, editor state, and session history are stored locally on your machine. Only API requests (prompts and completions) are sent to Aegisy servers, consistent with how external CLI tools operate.

**Q: Can I delete workbench data without affecting my profiles?**  
A: Yes. You can safely delete `~/.aegisy/workbench/` to remove all workbench-specific data (projects, sessions, agent history) without affecting your profiles, API keys, or CLI tool configurations.

### Troubleshooting

**Q: The workbench won't launch after migration. What should I do?**  
A: 
1. Check system requirements (Qt 6, OpenSSL 3.0+)
2. Review logs in `~/.aegisy/logs/`
3. Try launching the legacy client to verify configuration integrity
4. If needed, restore from backup and retry

**Q: My profiles are missing after migration. How do I recover them?**  
A:
1. Check if the legacy client can see the profiles
2. If yes, the workbench should also see them (shared data source)
3. If no, restore from your backup `.aegisy` file
4. Contact support if backups are unavailable

**Q: Profile activation fails in the workbench but works in the legacy client.**  
A: This indicates a workbench-specific issue. Please:
1. Check the activation log in the workbench UI
2. Verify file permissions on CLI configuration files
3. Try "Repair" instead of "Activate"
4. Report the issue with log details

**Q: Can I use the local gateway with the workbench agent?**  
A: Yes. The local gateway works with both external CLI tools and the integrated workbench agent. Enable it in Local Gateway settings.

### Updates & Support

**Q: How do I update the workbench?**  
A: 
- **macOS**: Sparkle auto-update (check Updates menu)
- **Windows**: WinSparkle auto-update (check Updates menu)
- **Linux**: Manual update (download new version)

**Q: Will updates affect my profiles or data?**  
A: No. Updates preserve all configuration data, profiles, API keys, and workbench projects.

**Q: Where can I get help?**  
A: 
- Documentation: `USER-GUIDE.md`, `README.md`
- Website: https://aegisy.cc
- Support: Contact through your Aegisy account dashboard

**Q: How do I report bugs or request features?**  
A: Use the feedback mechanism in the workbench Help menu, or contact Aegisy support through the website.

---

## Need Help?

If you encounter issues during migration:

1. **Check logs**: `~/.aegisy/logs/` (macOS/Linux) or `%APPDATA%\Aegisy\logs\` (Windows)
2. **Review documentation**: `USER-GUIDE.md` for detailed usage instructions
3. **Restore from backup**: Use Migration → Import Profiles if needed
4. **Contact support**: Visit https://aegisy.cc for assistance

**Welcome to Aegisy Coding Workbench!** We're excited to have you explore the new integrated development environment while maintaining full access to your existing workflows.
