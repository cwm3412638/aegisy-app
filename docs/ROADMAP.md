# Aegisy Coding Workbench - Roadmap

**Last Updated**: 2026-07-31  
**Status**: Living Document

## Vision

Transform Aegisy from a configuration client into a comprehensive coding agent
workbench that empowers developers with AI-assisted development while maintaining
security, transparency, and user control.

## Milestones

### ✅ Milestone -1: Foundation (Completed)
**Status**: Complete  
**Completion**: 2026-Q2

- Desktop client with profile management
- API key integration for Claude Code, Codex, Gemini, OpenCode
- Local gateway for provider routing
- Skills and MCP support
- Update mechanism (Sparkle/WinSparkle)
- Secure credential storage

### 🟡 Milestone 0: Architecture & Spike (In Progress)
**Target**: 2026-Q3  
**Progress**: 24% (57/235 tasks)

**Goals**:
- Complete architecture decisions (ADRs)
- UI technology spike (Qt WebEngine vs alternatives)
- AAP protocol foundation
- Runtime sidecar basics
- Event store and database
- Read-only Agent capabilities

**Key Deliverables**:
- [x] ADRs for all architecture questions
- [x] Chat/Work behavioral contract
- [x] Terminology definitions
- [ ] Qt WebEngine spike results
- [ ] Complete AAP 0.1 implementation
- [ ] Authenticated IPC (macOS socket, Windows pipe)
- [ ] SQLite event store with migrations
- [ ] Read-only Codex adapter

**Blockers**:
- Windows validation for several components
- UI technology go/no-go decision
- Legal review completion

### 🔵 Milestone 1: Read-Only Agent (Planned)
**Target**: 2026-Q4  
**Progress**: 0%

**Goals**:
- Complete workbench UI
- Read-only Agent operations
- Project and session management
- Timeline rendering
- Basic approval UI (deny only)

**Key Deliverables**:
- [ ] Workbench navigation and layout
- [ ] Project/session UI
- [ ] Agent timeline with streaming
- [ ] Monaco editor integration
- [ ] Terminal integration
- [ ] Read-only file browsing
- [ ] Read-only Git status
- [ ] Approval UI (deny path only)
- [ ] macOS and Windows packages

**Success Criteria**:
- Users can chat with Agent about their code
- Agent can read files and provide suggestions
- No mutations without explicit approval
- Stable on macOS and Windows

### 🔵 Milestone 2: Workspace Mutations (Planned)
**Target**: 2027-Q1  
**Progress**: 0%

**Goals**:
- Enable file write operations
- Git operations with approval
- Command execution with sandbox
- Permission profiles
- Audit trail

**Key Deliverables**:
- [ ] File write approvals
- [ ] Structured workspace edits
- [ ] Git operation approvals
- [ ] Command execution with sandbox
- [ ] Permission profiles (Chat, Read Only, Workspace Write, Developer, Full Access)
- [ ] Audit trail for all mutations
- [ ] Rollback capabilities
- [ ] Security review completion

**Success Criteria**:
- Agent can modify files with approval
- Git operations work safely
- Commands execute in sandbox
- All mutations are auditable
- Users feel in control

### 🔵 Milestone 3: Background & Multi-Agent (Planned)
**Target**: 2027-Q2  
**Progress**: 0%

**Goals**:
- Background job execution
- Multi-agent coordination
- Parallel work streams
- Quality gates

**Key Deliverables**:
- [ ] Background job scheduler
- [ ] Multi-agent orchestration
- [ ] Parallel session support
- [ ] Quality gates and safety checks
- [ ] Comprehensive testing
- [ ] Performance optimization
- [ ] Production readiness review

**Success Criteria**:
- Background jobs execute reliably
- Multiple agents can work in parallel
- Quality gates prevent issues
- Performance meets budgets
- Ready for production use

### 🔵 Milestone 4: Cloud Integration (Planned)
**Target**: 2027-Q3  
**Progress**: 0%

**Goals**:
- Aegisy cloud service integration
- Model catalog and routing
- Usage tracking and billing
- Account management

**Key Deliverables**:
- [ ] Cloud API integration
- [ ] Model catalog with capabilities
- [ ] Usage tracking and reporting
- [ ] Billing integration
- [ ] Account management UI
- [ ] Subscription features

**Success Criteria**:
- Seamless cloud integration
- Accurate usage tracking
- Transparent billing
- Easy account management

### 🔵 Milestone 5: Extensions & Ecosystem (Planned)
**Target**: 2027-Q4  
**Progress**: 0%

**Goals**:
- MCP server integration
- Skills and plugins
- Extension marketplace
- Community contributions

**Key Deliverables**:
- [ ] MCP server support
- [ ] Skills marketplace
- [ ] Plugin system
- [ ] Extension API
- [ ] Community guidelines
- [ ] Extension review process

**Success Criteria**:
- Rich extension ecosystem
- Easy to create extensions
- Safe and secure extensions
- Active community

## Feature Roadmap

### Core Features

#### Chat Mode
- [x] Basic chat interface (legacy)
- [ ] Workbench chat mode
- [ ] Context sharing
- [ ] Session history
- [ ] Export conversations

#### Work Mode
- [ ] Project binding
- [ ] File operations
- [ ] Git integration
- [ ] Terminal access
- [ ] Approval workflow

#### Editor
- [x] Monaco integration (complete)
- [ ] Language intelligence (LSP)
- [ ] Diff view
- [ ] Multi-file editing
- [ ] Search and replace

#### Terminal
- [x] macOS PTY (complete)
- [ ] Windows ConPTY (validation pending)
- [ ] Multiple terminals
- [ ] Terminal history
- [ ] Command palette

#### Git
- [x] Read-only status (complete)
- [ ] Commit operations
- [ ] Branch management
- [ ] Merge and rebase
- [ ] Worktree support

### Advanced Features

#### Context Engine
- [ ] Repository maps
- [ ] Code indexing
- [ ] Relevance ranking
- [ ] Token budget management
- [ ] Session compaction

#### Observability
- [ ] Telemetry collection
- [ ] Crash reporting
- [ ] Performance metrics
- [ ] Usage analytics
- [ ] Diagnostic tools

#### Security
- [ ] Permission profiles
- [ ] Sandbox enforcement
- [ ] Secret management
- [ ] Audit logging
- [ ] Security reviews

## Platform Support

### macOS
- [x] arm64 (complete)
- [ ] x64 (compilation only)
- [ ] Signed packages
- [ ] Notarization
- [ ] App Store distribution

### Windows
- [x] x64 (compilation)
- [ ] Runtime validation
- [ ] Signed packages
- [ ] Microsoft Store distribution
- [ ] ARM64 (future)

### Linux
- [ ] Debian/Ubuntu packages
- [ ] RPM packages
- [ ] AppImage
- [ ] Flatpak
- [ ] Snap

## Technology Evolution

### Current Stack
- C++17 + Qt Widgets
- Rust sidecar (aegisy-agentd)
- SQLite for persistence
- JSON-RPC (AAP protocol)
- Codex App Server adapter

### Future Considerations
- Qt 6 migration (in progress)
- WebEngine vs Tauri decision
- Additional runtime adapters
- Cloud-native features
- Mobile companion app

## Success Metrics

### Adoption
- Active users
- Session count
- Project count
- Retention rate

### Quality
- Crash rate
- Bug reports
- User satisfaction
- Performance metrics

### Ecosystem
- Extension count
- Community contributions
- Third-party integrations
- Documentation quality

## Risks & Mitigation

### Technical Risks
- **WebEngine performance**: Spike and benchmark
- **Windows compatibility**: Dedicated validation
- **Protocol stability**: Versioning and compatibility tests
- **Security vulnerabilities**: Regular audits

### Product Risks
- **User adoption**: Beta program and feedback
- **Feature complexity**: Phased rollout
- **Competition**: Focus on unique value
- **Support burden**: Documentation and automation

### Business Risks
- **Development cost**: Prioritize MVP features
- **Time to market**: Parallel development
- **Legal compliance**: Early legal review
- **Sustainability**: Cloud service integration

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for how to contribute to this roadmap.

## Updates

This roadmap is reviewed and updated quarterly. Major changes require stakeholder
approval.

---

**Next Review**: 2026-10-31  
**Owner**: Product Team
