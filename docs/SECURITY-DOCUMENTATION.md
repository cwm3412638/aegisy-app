# Aegisy Security Documentation

This document explains Aegisy's security model, trust boundaries, content handling,
secret protection, sandbox limits, network access, extensions, and remote/background risks.

## Security Philosophy

Aegisy is designed with **defense in depth**:
1. **Least Privilege**: Start with minimal permissions
2. **Explicit Approval**: User confirms each sensitive action
3. **Fail Closed**: Unknown or ambiguous states deny access
4. **Transparency**: Clear visibility into what the agent can do
5. **Isolation**: Separate untrusted content from trusted operations

## Trust Model

### Trust Boundaries

Aegisy maintains strict trust boundaries:

**Trusted**:
- Your local filesystem (within project roots)
- Your Git repositories
- Your terminal sessions
- Your explicit approvals
- Aegisy application code

**Untrusted**:
- Agent responses (treated as suggestions, not commands)
- Model outputs (validated before execution)
- External network content
- Extension code (sandboxed)
- Project instructions (reviewed before use)

### Project Trust Review

When opening a project for the first time, Aegisy shows:
- All project roots and their access levels
- Detected Git repositories
- Instruction files (`.claude/` directory)
- Executable hooks
- Permission implications

**Trusting a project does NOT grant automatic execution**. It only allows Aegisy to:
- Read files within project roots
- Detect project structure
- Discover instructions

All modifications still require explicit approval.

### Trust Acknowledgment

After reviewing a project, you can acknowledge trust. This:
- Records that you've reviewed the project
- Does NOT grant write, command, hook, or network authority
- Must be renewed if project structure changes
- Is invalidated if instructions or hooks change

## Untrusted Content Handling

### Agent Responses

All agent responses are treated as **untrusted suggestions**:
- File changes are previewed before applying
- Commands are shown before execution
- Git operations require approval
- Network requests need confirmation

### Content Validation

Before executing any agent suggestion, Aegisy:
1. Validates syntax and structure
2. Checks against permission profile
3. Scans for sensitive patterns
4. Previews full impact
5. Requires explicit approval

### Redaction

Aegisy automatically redacts sensitive content from:
- Timeline displays
- Session exports
- Diagnostic reports
- Error messages
- Logs

Redacted categories:
- API keys and tokens
- Passwords and credentials
- Private keys
- Authentication headers
- Session cookies
- Database connection strings

## Secret Protection

### Secret Detection

Aegisy scans for secrets in:
- Agent responses
- File content
- Command output
- Terminal sessions
- Git commits
- Diagnostic data

### Secret Patterns

Detected secret types:
- API keys (AWS, GitHub, OpenAI, etc.)
- OAuth tokens
- JWT tokens
- Private keys (SSH, PGP, TLS)
- Database credentials
- Cloud service credentials
- Authentication cookies

### Secret Handling

When secrets are detected:
- **Display**: Replaced with `[REDACTED]` markers
- **Storage**: Never persisted in session history
- **Export**: Excluded from diagnostic bundles
- **Logs**: Stripped from all logs
- **Network**: Never sent to external services

### False Positives

If legitimate content is incorrectly flagged:
- Review the pattern that triggered detection
- Content remains protected (fail-safe)
- Report false positives for pattern refinement
- Use environment variables for actual secrets

### Best Practices

1. **Never paste secrets** into chat or code
2. **Use environment variables** for credentials
3. **Use secret management tools** (1Password, etc.)
4. **Rotate secrets** if accidentally exposed
5. **Review exports** before sharing

## Sandbox Limits

### Filesystem Sandbox

Aegisy can only access:
- Files within registered project roots
- Temporary files it creates
- Its own data directory

Aegisy **cannot** access:
- Files outside project roots
- System directories
- Other users' files
- Network filesystems (without explicit permission)
- Symbolic links outside project roots

### Process Sandbox

Terminal processes run with:
- Limited environment variables
- No access to Aegisy credentials
- Cleared execution environment
- Bounded resource limits
- Process group isolation

### Network Sandbox

By default, Aegisy:
- Does NOT make network requests
- Does NOT send code to external services
- Does NOT upload files

Network access requires:
- Explicit permission profile (Developer or Full Access)
- Per-request approval
- Clear indication of destination
- User confirmation

### Sandbox Limitations

**What the sandbox does NOT prevent**:
- Commands that modify files within project roots
- Git operations within the repository
- Network requests if permission is granted
- Resource consumption (CPU, memory, disk)
- Malicious code execution if approved

**User responsibility**:
- Review all commands before approval
- Understand what code does
- Don't approve suspicious operations
- Use appropriate permission profiles

## Network Access

### Network Permissions

Network access is controlled by permission profile:

**Read Only / Workspace Write**: No network access

**Developer**: Limited network access
- Language server downloads
- Package manager operations (with approval)
- Git remote operations (with approval)

**Full Access**: Broader network access
- API calls (with approval)
- Web requests (with approval)
- Custom network operations (with approval)

### Network Approval

Before any network request, Aegisy shows:
- Destination URL or hostname
- Request method (GET, POST, etc.)
- Purpose of the request
- Data being sent (if any)

You must explicitly approve each request.

### Network Indicators

When network access is active:
- Visual indicator in UI
- Request logged in timeline
- Destination shown in context
- Approval recorded

### Network Risks

**Risks of network access**:
- Data exfiltration
- Credential theft
- Malware download
- Unauthorized API calls
- Privacy violations

**Mitigation**:
- Start with no network access
- Grant only when necessary
- Review each request carefully
- Use network monitoring tools
- Revoke access when done

## Extensions

### Extension Types

**Skills**: Reusable workflows
- Defined in `.claude/skills/`
- Can include instructions and examples
- Cannot execute code directly
- Require approval for actions

**Hooks**: Event-triggered actions
- Defined in `.claude/hooks/`
- Run on specific events (pre-commit, etc.)
- Require explicit enablement
- Sandboxed execution

**MCP (Model Context Protocol)**: External tools
- Third-party integrations
- Run in separate processes
- Limited to defined capabilities
- Require user installation

**Plugins**: Custom functionality
- Extend Aegisy capabilities
- Require code review
- Sandboxed execution
- Explicit permission grants

### Extension Security

**Before enabling an extension**:
1. Review what it does
2. Check required permissions
3. Verify source/author
4. Understand data access
5. Read user reviews (if available)

**Extension permissions**:
- Filesystem access (which directories)
- Network access (which hosts)
- Command execution (which commands)
- Git operations (which operations)
- Secret access (never granted)

### Extension Isolation

Extensions run with:
- Limited filesystem access
- No direct secret access
- Bounded resource limits
- Separate process space
- Monitored execution

### Extension Risks

**Potential risks**:
- Malicious code execution
- Data exfiltration
- Credential theft
- Resource abuse
- Privacy violations

**Mitigation**:
- Only use trusted extensions
- Review extension code
- Grant minimal permissions
- Monitor extension activity
- Disable unused extensions

## Remote and Background Risks

### Remote Execution

**Current Status**: Remote execution is **not implemented** and is blocked by design.

**If implemented in the future**, remote execution would:
- Run agent operations on remote servers
- Require explicit opt-in
- Use encrypted communication
- Never send secrets
- Provide clear indicators

**Risks of remote execution**:
- Code sent to external servers
- Potential data exposure
- Network dependency
- Privacy concerns
- Compliance issues

### Background Jobs

**Background jobs** run without active user supervision:
- Long-running tests
- Build processes
- Batch operations
- Scheduled tasks

**Risks**:
- Unmonitored resource usage
- Unexpected side effects
- Difficult to cancel
- May outlive session

**Mitigation**:
- Require explicit approval
- Show clear indicators
- Provide stop controls
- Log all actions
- Limit resource usage

### Autonomous Operation

Aegisy does **not** support fully autonomous operation:
- Every action requires approval
- No automatic code execution
- No unattended operations
- User always in control

## Permission Profiles

### Read Only

**Grants**:
- View files
- Read Git status
- Search code
- View diagnostics

**Denies**:
- File modifications
- Command execution
- Git commits
- Network access
- Extension execution

**Use When**: Exploring unfamiliar code

### Workspace Write

**Grants**:
- All Read Only permissions
- Create/modify/delete files (with approval)
- Preview Git changes

**Denies**:
- Command execution
- Git commits
- Network access
- Extension execution

**Use When**: Making file-only changes

### Developer

**Grants**:
- All Workspace Write permissions
- Run commands (with approval)
- Make Git commits (with approval)
- Use language servers
- Run tests

**Denies**:
- Background processes
- Network access (except Git/LSP)
- Hook execution

**Use When**: Active development

### Full Access

**Grants**:
- All Developer permissions
- Background processes (with approval)
- Network access (with approval)
- Hook execution (with approval)
- Extension execution (with approval)

**Denies**:
- Nothing (all capabilities available with approval)

**Use When**: Trusted projects requiring full capabilities

**Warning**: Only use Full Access for projects you fully trust.

## Incident Response

### If You Suspect a Security Issue

1. **Stop immediately**: Cancel any active operations
2. **Revoke permissions**: Switch to Read Only mode
3. **Review timeline**: Check what actions were taken
4. **Inspect changes**: Review all file modifications
5. **Check Git history**: Look for unexpected commits
6. **Rotate secrets**: If any secrets may have been exposed
7. **Report**: Contact Aegisy security team

### Reporting Security Vulnerabilities

**Do NOT** report security vulnerabilities in public issues.

**Instead**:
- Email: security@aegisy.example.com
- Include: Detailed description, steps to reproduce
- Provide: Aegisy version, OS, relevant logs (redacted)
- Wait: For response before public disclosure

### Security Updates

- Subscribe to security announcements
- Enable automatic updates (recommended)
- Review release notes for security fixes
- Test updates in non-production first

## Privacy

### Data Collection

Aegisy collects:
- **Locally**: Session history, file changes, settings
- **Never**: Secrets, credentials, private keys
- **Optional**: Diagnostic data (with explicit consent)

### Data Storage

- **Local**: All data stored on your machine
- **Encrypted**: Sensitive data encrypted at rest
- **Isolated**: Per-project data separation
- **Deletable**: You can delete all data anytime

### Data Sharing

Aegisy does **not**:
- Send code to external servers (except model API)
- Share data with third parties
- Upload files automatically
- Track user behavior

### Model API

When using the agent:
- Your prompts are sent to the model provider
- Model provider's privacy policy applies
- Aegisy redacts secrets before sending
- You control what context is included

## Compliance

### Data Residency

- All data stored locally by default
- No automatic cloud sync
- User controls data location
- Portable session export available

### Audit Trail

Aegisy maintains audit logs for:
- Permission changes
- Approvals granted
- File modifications
- Command executions
- Git operations
- Network requests

Logs are:
- Stored locally
- Redacted for secrets
- Exportable for review
- Deletable by user

### Regulatory Compliance

Aegisy is designed to support:
- GDPR (data portability, deletion)
- SOC 2 (access controls, audit logs)
- HIPAA (local storage, encryption)
- ISO 27001 (security controls)

**Note**: Compliance responsibility is shared between Aegisy and the user.

## Best Practices Summary

1. **Start with minimal permissions** (Read Only)
2. **Review all approvals carefully**
3. **Never paste secrets** into chat
4. **Use environment variables** for credentials
5. **Review project trust** before opening
6. **Disable unused extensions**
7. **Monitor network indicators**
8. **Review timeline regularly**
9. **Export and backup** important sessions
10. **Report security issues** responsibly

## Additional Resources

- User Guide: `docs/END-USER-CONCEPTS-GUIDE.md`
- Troubleshooting: `docs/AEGISY-TROUBLESHOOTING-RUNBOOK.md`
- Privacy Policy: `docs/AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md`
- Extension Guide: `docs/EXTENSION-AUTHOR-GUIDE.md` (planned)
