# Aegisy Extension Author Guide

## Overview

Aegisy Agent Workbench supports four extension types:

1. **Skills**: Packaged workflows and commands
2. **Hooks**: Lifecycle event handlers
3. **MCP (Model Context Protocol)**: External context providers
4. **Plugins**: Native code extensions

This guide covers authoring, testing, signing, and distributing extensions safely and compatibly.

## Extension Types

### Skills

Skills are packaged workflows that extend Aegisy's capabilities through structured commands.

**Structure:**
```
.aegisy/skills/
├── my-skill/
│   ├── skill.yaml          # Manifest
│   ├── README.md           # Documentation
│   ├── main.sh             # Entry point
│   └── tests/              # Test suite
```

**skill.yaml:**
```yaml
schema: skill/0.1
name: my-skill
version: 1.0.0
description: Brief description of what this skill does
author: Your Name <email@example.com>
license: MIT

# Permissions required
permissions:
  - workspace.read
  - workspace.write
  - terminal.execute

# Entry point
entry: main.sh

# Arguments
arguments:
  - name: target
    type: string
    required: true
    description: Target file or directory
  - name: mode
    type: enum
    values: [fast, thorough]
    default: fast
    description: Execution mode

# Compatibility
requires:
  aegisy: ">=0.1.0"
  runtime: codex

# Security
sandbox: true
network: false
```

**main.sh:**
```bash
#!/bin/bash
set -euo pipefail

# Skill receives arguments as environment variables
TARGET="${SKILL_ARG_TARGET}"
MODE="${SKILL_ARG_MODE:-fast}"

# Skill context available
PROJECT_ROOT="${AEGISY_PROJECT_ROOT}"
SESSION_ID="${AEGISY_SESSION_ID}"

# Output structured results
echo "::skill-output::Processing ${TARGET} in ${MODE} mode"

# Return structured data
cat <<EOF
{
  "status": "success",
  "files_processed": 42,
  "duration_ms": 1234
}
EOF
```

**Best Practices:**
- Keep skills focused on one task
- Validate all inputs
- Use structured output for machine-readable results
- Provide clear error messages
- Document all arguments and behavior
- Include comprehensive tests

### Hooks

Hooks respond to lifecycle events in Aegisy.

**Supported Hooks:**
- `pre-session-start`: Before session creation
- `post-session-start`: After session creation
- `pre-turn-start`: Before model turn
- `post-turn-complete`: After turn completion
- `pre-workspace-edit`: Before applying edits
- `post-workspace-edit`: After applying edits
- `pre-git-operation`: Before Git operations
- `post-git-operation`: After Git operations

**Structure:**
```
.aegisy/hooks/
├── pre-turn-start
├── post-turn-complete
└── pre-workspace-edit
```

**Example Hook (pre-workspace-edit):**
```bash
#!/bin/bash
# Runs before workspace edits are applied

# Hook receives event data via stdin
EVENT=$(cat)

# Parse event
FILES=$(echo "$EVENT" | jq -r '.files[]')
SESSION=$(echo "$EVENT" | jq -r '.session_id')

# Perform validation
for file in $FILES; do
  if [[ "$file" == *"production"* ]]; then
    echo "::hook-error::Cannot edit production files" >&2
    exit 1
  fi
done

# Allow edit to proceed
exit 0
```

**Hook Contract:**
- Exit 0: Allow operation to proceed
- Exit 1: Block operation with error
- stdout: Informational messages
- stderr: Error messages
- stdin: JSON event data

**Security:**
- Hooks run with project permissions
- Cannot escalate privileges
- Sandboxed by default
- Network access requires explicit permission

### MCP (Model Context Protocol)

MCP servers provide external context to the AI model.

**MCP Server Structure:**
```
my-mcp-server/
├── package.json
├── src/
│   └── index.ts
└── README.md
```

**package.json:**
```json
{
  "name": "@myorg/aegisy-mcp-server",
  "version": "1.0.0",
  "description": "MCP server for X integration",
  "main": "dist/index.js",
  "aegisy": {
    "type": "mcp-server",
    "protocol": "stdio",
    "permissions": ["network.https"],
    "compatibility": ">=0.1.0"
  }
}
```

**index.ts:**
```typescript
import { MCPServer, Tool, Resource } from '@aegisy/mcp-sdk';

const server = new MCPServer({
  name: 'my-mcp-server',
  version: '1.0.0'
});

// Register tools
server.registerTool({
  name: 'search_docs',
  description: 'Search documentation',
  parameters: {
    query: { type: 'string', required: true }
  },
  handler: async (params) => {
    const results = await searchDocs(params.query);
    return { results };
  }
});

// Register resources
server.registerResource({
  uri: 'docs://latest',
  name: 'Latest Documentation',
  mimeType: 'text/markdown',
  handler: async () => {
    return await fetchLatestDocs();
  }
});

server.start();
```

**MCP Best Practices:**
- Implement proper error handling
- Respect rate limits
- Cache responses when appropriate
- Validate all inputs
- Document all tools and resources
- Provide clear descriptions for AI consumption

### Plugins

Native plugins extend Aegisy with compiled code.

**Plugin Structure:**
```
my-plugin/
├── plugin.yaml
├── src/
│   ├── plugin.cpp
│   └── plugin.h
├── CMakeLists.txt
└── README.md
```

**plugin.yaml:**
```yaml
schema: plugin/0.1
name: my-plugin
version: 1.0.0
description: Native plugin for X
author: Your Name <email@example.com>
license: Apache-2.0

# Binary compatibility
abi_version: 1
qt_version: "6.5"
platforms:
  - macos-arm64
  - macos-x64
  - windows-x64

# Permissions
permissions:
  - filesystem.read
  - process.spawn

# Entry point
entry: libmyplugin.so

# Dependencies
dependencies:
  - qt6-core
  - qt6-gui
```

**plugin.cpp:**
```cpp
#include <aegisy/plugin.h>

class MyPlugin : public AegisyPlugin {
public:
    QString name() const override { return "my-plugin"; }
    QString version() const override { return "1.0.0"; }
    
    bool initialize(AegisyContext* context) override {
        // Plugin initialization
        context->registerCommand("my-command", this, &MyPlugin::handleCommand);
        return true;
    }
    
    void handleCommand(const QJsonObject& args, AegisyCallback* callback) {
        // Command implementation
        QJsonObject result;
        result["status"] = "success";
        callback->resolve(result);
    }
};

AEGISY_EXPORT_PLUGIN(MyPlugin)
```

**Plugin Best Practices:**
- Follow Qt coding conventions
- Handle all errors gracefully
- Never block the main thread
- Use async APIs for I/O
- Respect memory limits
- Provide comprehensive tests
- Document all APIs

## Permissions

Extensions must declare required permissions.

**Permission Categories:**

**Filesystem:**
- `filesystem.read`: Read project files
- `filesystem.write`: Write project files
- `filesystem.delete`: Delete files
- `filesystem.watch`: Watch for changes

**Process:**
- `process.spawn`: Spawn child processes
- `process.signal`: Send signals to processes
- `process.env`: Access environment variables

**Network:**
- `network.http`: HTTP requests
- `network.https`: HTTPS requests
- `network.websocket`: WebSocket connections

**Git:**
- `git.read`: Read Git state
- `git.write`: Modify Git repository
- `git.remote`: Access remote repositories

**Terminal:**
- `terminal.read`: Read terminal output
- `terminal.write`: Write to terminal
- `terminal.execute`: Execute commands

**Model:**
- `model.context`: Add model context
- `model.prompt`: Modify prompts
- `model.response`: Access responses

**Permission Declaration:**
```yaml
permissions:
  - filesystem.read
  - filesystem.write
  - terminal.execute
  - network.https

# Optional: Explain why each permission is needed
permission_rationale:
  filesystem.write: "Needed to generate output files"
  terminal.execute: "Runs build commands"
  network.https: "Fetches latest templates"
```

**Permission Best Practices:**
- Request minimum necessary permissions
- Explain why each permission is needed
- Never request permissions "just in case"
- Fail gracefully if permission denied
- Document security implications

## Testing

### Skill Testing

**Test Structure:**
```
tests/
├── test_basic.sh
├── test_edge_cases.sh
└── fixtures/
    ├── input.txt
    └── expected_output.txt
```

**test_basic.sh:**
```bash
#!/bin/bash
set -euo pipefail

# Setup
export SKILL_ARG_TARGET="fixtures/input.txt"
export SKILL_ARG_MODE="fast"
export AEGISY_PROJECT_ROOT="$(pwd)"
export AEGISY_SESSION_ID="test-session"

# Run skill
OUTPUT=$(bash main.sh)

# Verify output
if echo "$OUTPUT" | grep -q "success"; then
  echo "✓ Basic test passed"
  exit 0
else
  echo "✗ Basic test failed"
  exit 1
fi
```

### Hook Testing

**Test Hook:**
```bash
#!/bin/bash
# Test pre-workspace-edit hook

# Create test event
EVENT='{
  "session_id": "test-123",
  "files": ["src/main.py", "tests/test_main.py"]
}'

# Run hook
echo "$EVENT" | .aegisy/hooks/pre-workspace-edit

# Check exit code
if [ $? -eq 0 ]; then
  echo "✓ Hook test passed"
else
  echo "✗ Hook test failed"
  exit 1
fi
```

### MCP Testing

**test/server.test.ts:**
```typescript
import { MCPClient } from '@aegisy/mcp-test-utils';
import { describe, it, expect } from 'vitest';

describe('MCP Server', () => {
  it('should handle search_docs tool', async () => {
    const client = new MCPClient('./dist/index.js');
    await client.start();
    
    const result = await client.callTool('search_docs', {
      query: 'authentication'
    });
    
    expect(result.results).toBeDefined();
    expect(result.results.length).toBeGreaterThan(0);
    
    await client.stop();
  });
});
```

### Plugin Testing

**tests/test_plugin.cpp:**
```cpp
#include <QtTest/QtTest>
#include "plugin.h"

class PluginTest : public QObject {
    Q_OBJECT
private slots:
    void testInitialization() {
        MyPlugin plugin;
        AegisyContext context;
        QVERIFY(plugin.initialize(&context));
    }
    
    void testCommand() {
        MyPlugin plugin;
        QJsonObject args;
        args["input"] = "test";
        
        TestCallback callback;
        plugin.handleCommand(args, &callback);
        
        QVERIFY(callback.resolved);
        QCOMPARE(callback.result["status"].toString(), "success");
    }
};

QTEST_MAIN(PluginTest)
#include "test_plugin.moc"
```

## Signing and Distribution

### Code Signing

**Why Sign:**
- Verifies extension authenticity
- Prevents tampering
- Enables trust chain
- Required for distribution

**Signing Process:**

1. **Generate Key Pair:**
```bash
aegisy-sign keygen \
  --name "Your Name" \
  --email "email@example.com" \
  --output ~/.aegisy/signing-key.pem
```

2. **Sign Extension:**
```bash
aegisy-sign sign \
  --key ~/.aegisy/signing-key.pem \
  --extension my-skill/ \
  --output my-skill-1.0.0.aex
```

3. **Verify Signature:**
```bash
aegisy-sign verify my-skill-1.0.0.aex
```

**Signature Format:**
```
my-skill-1.0.0.aex
├── manifest.json       # Extension metadata
├── signature.sig       # Digital signature
└── content/            # Extension files
```

### Distribution

**Publishing to Aegisy Extension Registry:**

1. **Create Account:**
```bash
aegisy-publish login
```

2. **Validate Extension:**
```bash
aegisy-publish validate my-skill-1.0.0.aex
```

3. **Publish:**
```bash
aegisy-publish upload my-skill-1.0.0.aex \
  --category development \
  --tags "testing,automation"
```

**Private Distribution:**

```bash
# Host on your server
https://extensions.example.com/my-skill-1.0.0.aex

# Users install via URL
aegisy extension install https://extensions.example.com/my-skill-1.0.0.aex
```

**GitHub Releases:**
```yaml
# .github/workflows/release.yml
name: Release Extension
on:
  push:
    tags: ['v*']
jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Sign Extension
        run: aegisy-sign sign --key ${{ secrets.SIGNING_KEY }} --extension .
      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: my-skill-*.aex
```

## Compatibility

### Version Compatibility

**Semantic Versioning:**
- MAJOR: Breaking changes
- MINOR: New features, backward compatible
- PATCH: Bug fixes

**Declare Compatibility:**
```yaml
requires:
  aegisy: ">=0.1.0 <2.0.0"
  runtime: 
    codex: ">=0.144.0"
    acp: ">=1.0.0"
  qt: ">=6.5.0"
```

### Runtime Compatibility

**Codex Runtime:**
- Uses Codex App Server protocol
- Supports file operations, Git, terminal
- Limited to Codex-compatible models

**ACP Runtime:**
- Uses Anthropic Claude Protocol
- Broader model support
- Different capability set

**Check Runtime:**
```bash
if [ "$AEGISY_RUNTIME" = "codex" ]; then
  # Codex-specific logic
elif [ "$AEGISY_RUNTIME" = "acp" ]; then
  # ACP-specific logic
fi
```

### Platform Compatibility

**Cross-Platform Extensions:**
```yaml
platforms:
  - macos-arm64
  - macos-x64
  - windows-x64
  - linux-x64

# Platform-specific entry points
entry:
  macos: main.sh
  windows: main.ps1
  linux: main.sh
```

**Platform Detection:**
```bash
case "$AEGISY_PLATFORM" in
  macos*)
    # macOS-specific
    ;;
  windows*)
    # Windows-specific
    ;;
  linux*)
    # Linux-specific
    ;;
esac
```

## Security Guidelines

### Input Validation

**Always Validate:**
```bash
# Bad: No validation
rm -rf "$USER_INPUT"

# Good: Validate and sanitize
if [[ "$USER_INPUT" =~ ^[a-zA-Z0-9_-]+$ ]]; then
  rm -rf "${PROJECT_ROOT}/${USER_INPUT}"
else
  echo "Invalid input" >&2
  exit 1
fi
```

### Secret Handling

**Never:**
- Log secrets
- Store secrets in extension code
- Pass secrets via command line
- Include secrets in error messages

**Do:**
```bash
# Use Aegisy secret management
SECRET=$(aegisy secret get API_KEY)

# Mask in logs
echo "Using API key: ${SECRET:0:4}****"

# Clean up
unset SECRET
```

### Sandboxing

**Respect Sandbox:**
```yaml
sandbox: true  # Enable sandbox
network: false # Disable network
```

**Sandbox Limits:**
- Cannot access files outside project
- Cannot spawn arbitrary processes
- Cannot access network (unless permitted)
- Cannot escalate privileges

### Audit Trail

**Log Actions:**
```bash
aegisy-log info "Processing file: $FILE"
aegisy-log warn "Large file detected: $SIZE bytes"
aegisy-log error "Failed to process: $ERROR"
```

## Best Practices Summary

### Development
1. Start with minimal permissions
2. Validate all inputs
3. Handle errors gracefully
4. Provide clear documentation
5. Write comprehensive tests
6. Follow platform conventions

### Security
1. Never trust user input
2. Sanitize all data
3. Use Aegisy APIs for secrets
4. Respect sandbox boundaries
5. Log security-relevant actions
6. Keep dependencies updated

### Distribution
1. Sign all releases
2. Use semantic versioning
3. Maintain changelog
4. Provide migration guides
5. Support multiple versions
6. Respond to security issues promptly

### Compatibility
1. Declare all requirements
2. Test on all platforms
3. Handle missing features gracefully
4. Provide fallbacks
5. Document breaking changes
6. Maintain backward compatibility

## Resources

- **API Reference**: https://docs.aegisy.dev/api
- **SDK Documentation**: https://docs.aegisy.dev/sdk
- **Extension Registry**: https://extensions.aegisy.dev
- **Example Extensions**: https://github.com/aegisy/examples
- **Community Forum**: https://community.aegisy.dev
- **Security Policy**: https://aegisy.dev/security

## Support

- **Issues**: https://github.com/aegisy/aegisy/issues
- **Discussions**: https://github.com/aegisy/aegisy/discussions
- **Email**: extensions@aegisy.dev
- **Discord**: https://discord.gg/aegisy
