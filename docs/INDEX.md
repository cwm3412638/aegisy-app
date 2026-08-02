# Documentation Index

**Last Updated**: 2026-08-01  
**Total Documents**: 116 markdown files  
**Project Status**: 101/239 tasks (42%)

## Quick Start

- [README](../README.md) - Project overview
- [QUICKSTART](../QUICKSTART.md) - 5-minute setup guide
- [ARCHITECTURE](../ARCHITECTURE.md) - System design
- [CONTRIBUTING](../CONTRIBUTING.md) - Contribution guide
- [TESTING](../TESTING.md) - Testing guide

## User Documentation

- [First-Run Onboarding](FIRST-RUN-ONBOARDING.md) - New user guide
- [End-User Concepts](END-USER-CONCEPTS-GUIDE.md) - Core concepts
- [Model Switching Guide](MODEL-SWITCHING-GUIDE.md) - Model management
- [Security Documentation](SECURITY-DOCUMENTATION.md) - Security model

## Developer Documentation

### Getting Started
- [Quick Start](../QUICKSTART.md) - Setup in 5 minutes
- [Architecture](../ARCHITECTURE.md) - System design
- [Contributing](../CONTRIBUTING.md) - Development workflow
- [Testing Guide](../TESTING.md) - Writing and running tests

### API & Protocol
- [AAP Protocol Guide](AAP-PROTOCOL-GUIDE.md) - Complete protocol spec
- [AAP API Reference](AAP-API-REFERENCE.md) - Quick API lookup
- [AAP Adapter Guide](AAP-ADAPTER-CONTRIBUTOR-GUIDE.md) - Adapter development

### Extension Development
- [Extension Author Guide](EXTENSION-AUTHOR-GUIDE.md) - Skills, Hooks, MCP, Plugins

### Architecture & Design
- [Architecture](../ARCHITECTURE.md) - Multi-process design, ADRs
- [Chat/Work Contract](CHAT-WORK-BEHAVIORAL-CONTRACT.md) - Mode definitions
- [Coding Terminology](AEGISY-CODING-TERMINOLOGY.md) - Canonical terms
- [ADR Index](adr/README.md) - Architecture decisions

## Operational Documentation

### Troubleshooting & Support
- [Troubleshooting Runbook](Aegisy-TROUBLESHOOTING-RUNBOOK.md) - Problem solving
- [Support Training](AEGISY-SUPPORT-AND-RELEASE-RECOVERY-TRAINING.md) - Support guide

### Security & Privacy
- [Security Documentation](SECURITY-DOCUMENTATION.md) - Security model
- [Privacy & Diagnostics](AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md) - Data handling
- [Security Audit Checklist](SECURITY-AUDIT-CHECKLIST.md) - Security review
- [Legal Review Checklist](LEGAL-REVIEW-CHECKLIST.md) - Legal compliance

### Platform & Performance
- [Platform Matrix](AEGISY-SUPPORTED-PLATFORM-MATRIX.md) - Platform support
- [Performance Budgets](AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md) - Performance targets
- [Feature Channel Policy](AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md) - Feature flags

### Release & Packaging
- [Third-Party Inventory](THIRD-PARTY-COMPONENT-INVENTORY.md) - Dependencies
- [Artifact Packaging](AEGISY-ARTIFACT-MANIFEST-PACKAGING.md) - Packaging design

## Technical Reference

### Workbench Components
- [Workbench Visible State](AEGISY-WORKBENCH-VISIBLE-STATE-MATRIX.md) - UI states
- [Agent Workbench](AGENT-WORKBENCH.md) - Workbench overview

### Adapters & Integration
- [ACP Adapter Design](ACP-ADAPTER-DESIGN.md) - ACP integration
- [ACP Adapter Support](ACP-ADAPTER-SUPPORT.md) - ACP support

### Background & Advanced
- [Background Execution](BACKGROUND-EXECUTION-DESIGN.md) - Background jobs

## Progress & Reports

- [Progress Reports](progress/) - Session summaries (30+ reports)
- [OpenSpec Tasks](../openspec/changes/build-aegisy-agent-workbench/tasks.md) - Task tracking
- [OpenSpec Design](../openspec/changes/build-aegisy-agent-workbench/design.md) - Detailed design

## Developer Tools

### Scripts (in `scripts/`)
- `project-status.sh` - Comprehensive project status
- `openspec-status.sh` - OpenSpec progress
- `openspec-find.sh` - Find and filter tasks
- `run-tests.sh` - Test execution
- `check-deps.sh` - Dependency verification
- `dev-helper.sh` - Development commands
- `watch-status.sh` - Real-time monitoring
- `commit-stats.sh` - Repository analytics
- `clean.sh` - Clean build artifacts

### Usage Examples
```bash
# Project status
./scripts/project-status.sh

# Find incomplete tasks
./scripts/openspec-find.sh --incomplete

# Run tests
./scripts/run-tests.sh

# Check dependencies
./scripts/check-deps.sh
```

## Project Status

**Overall Progress**: 101/239 tasks (42%)

**Completed Sections (100%)**:
- Event Store, Database, and Recovery
- Workbench Host and Navigation
- Agent Timeline and Composer
- Files, Editor, Search, and Diagnostics
- Structured Edits, Diffs, and Checkpoints

**High Completion (67-89%)**:
- UI Technology Spike (89%)
- AAP Protocol Foundation (75%)
- Terminal and Process Execution (67%)

**Documentation (70%)**:
- Complete user, developer, and operational guides

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for:
- Development setup
- Workflow guidelines
- Code style standards
- Testing requirements
- Documentation guidelines

---

**For the latest status**: Run `./scripts/project-status.sh`
