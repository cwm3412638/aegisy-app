#!/bin/bash
# Generate documentation index
# Usage: ./scripts/generate-doc-index.sh

set -euo pipefail

OUTPUT="docs/INDEX.md"

cat > "$OUTPUT" << 'EOF'
# Documentation Index

**Last Updated**: $(date +%Y-%m-%d)

## Quick Links

- [Quick Reference](QUICK-REFERENCE.md) - Fast access to common commands
- [Contributing Guide](../CONTRIBUTING.md) - How to contribute
- [Roadmap](ROADMAP.md) - Product roadmap and milestones

## Getting Started

- [README](../README.md) - Project overview
- [Workbench Status](WORKBENCH-STATUS.md) - Current development status
- [Quick Reference](QUICK-REFERENCE.md) - Essential commands and concepts

## Development

### Setup & Tools
- [Contributing Guide](../CONTRIBUTING.md) - Development workflow
- [Quick Reference](QUICK-REFERENCE.md) - Commands and shortcuts
- [CI/CD Recommendations](CI-CD-RECOMMENDATIONS.md) - Automation setup
- [Dependency Checker](../scripts/check-deps.sh) - Verify dependencies

### Architecture & Design
- [Architecture Decision Records](adr/README.md) - ADR index
- [Chat/Work Behavioral Contract](CHAT-WORK-BEHAVIORAL-CONTRACT.md) - Mode definitions
- [Aegisy Coding Terminology](AEGISY-CODING-TERMINOLOGY.md) - Canonical terms
- Design documents (in progress via agents)

### Testing & Quality
- [Test Coverage Analysis](TEST-COVERAGE-ANALYSIS.md) - Coverage report
- [Code Review Checklist](CODE-REVIEW-CHECKLIST.md) - Review guidelines
- [Test Runner](../scripts/run-tests.sh) - Run tests by category

### Security
- [Security Audit Checklist](SECURITY-AUDIT-CHECKLIST.md) - Security review
- [Legal Review Checklist](LEGAL-REVIEW-CHECKLIST.md) - Legal compliance

## Operations

### Release & Deployment
- [Release Checklist](RELEASE-CHECKLIST.md) - Release process
- [Roadmap](ROADMAP.md) - Product milestones
- [Packaging Design](PACKAGING-RELEASE-DESIGN.md) - (in progress)

### Troubleshooting & Support
- [Troubleshooting Guide](TROUBLESHOOTING.md) - Problem solving
- [Migration Guide](MIGRATION-GUIDE.md) - (in progress)

## Reference

### Technical Documentation
- [Workbench Status](WORKBENCH-STATUS.md) - Detailed status
- [Third-Party Components](THIRD-PARTY-COMPONENT-INVENTORY.md) - Dependencies
- [Performance Budgets](AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md) - Performance targets
- [Platform Matrix](AEGISY-SUPPORTED-PLATFORM-MATRIX.md) - Platform support

### Progress & Reports
- [Progress Reports](progress/) - Session reports
- [OpenSpec Status](../scripts/openspec-status.sh) - Task progress

## Scripts

Located in `scripts/`:
- `openspec-status.sh` - OpenSpec progress
- `dev-helper.sh` - Development commands
- `run-tests.sh` - Test execution
- `watch-status.sh` - Real-time monitoring
- `commit-stats.sh` - Repository analytics
- `check-deps.sh` - Dependency verification
- `clean.sh` - Clean build artifacts

## OpenSpec

- [Build Aegisy Agent Workbench](../openspec/changes/build-aegisy-agent-workbench/) - Main OpenSpec change
  - [Tasks](../openspec/changes/build-aegisy-agent-workbench/tasks.md) - Task tracking
  - [Design](../openspec/changes/build-aegisy-agent-workbench/design.md) - Detailed design
  - [Verification](../openspec/changes/build-aegisy-agent-workbench/verification.md) - Verification criteria

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for:
- Development setup
- Workflow guidelines
- Code style standards
- Testing requirements
- Documentation guidelines

---

**Note**: Documents marked "(in progress)" are being created by background agents.
EOF

echo "Documentation index generated at $OUTPUT"
