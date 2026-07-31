# Observability Design

## Overview

This document defines the observability, diagnostics, and evaluation architecture for the Aegisy Agent Workbench. The design prioritizes privacy-preserving local telemetry, structured diagnostics, and comprehensive evaluation frameworks while maintaining strict boundaries around sensitive content.

## 1. Telemetry and Metrics Collection

### 1.1 Structured Turn Traces

Each turn maintains a local structured trace (`turn-trace/0.6`) containing:

- **Runtime/Model Selection**: Adapter version, provider, model ID, capability flags
- **Context Manifest**: Content-addressed hashes and byte sizes (no actual content)
- **Event Timing**: Request start, provider response times, tool execution durations, completion time
- **Tool Lifecycle**: Command start/completion with sanitized metadata, exit codes, output identities
- **Approval Lifecycle**: Policy observations (never/always), denial events (no user decisions stored)
- **Token Usage**: Provider-reported observed metrics, catalog-derived estimates, cache hits, reasoning tokens
- **Cost Estimates**: Source-qualified (observed/estimated/unknown), catalog version binding
- **Changed Paths**: Content-addressed file identities (no diffs or source code)
- **Commands/Tests**: Sanitized command structure, action types, exit status (no arguments or output)
- **Errors**: Classified by domain (protocol/provider/adapter/transport/timeout/sandbox/policy/tool/storage/workspace/git/budget)
- **Final State**: Completed/failed/interrupted with completion domains (workspace-change/git-change/verification as not-applicable/applicable/unknown/observed)

**Privacy Boundaries:**
- Prompt and file content excluded by default
- Credentials, API keys, environment variables redacted before storage
- Provider response IDs, cache handles, hidden reasoning excluded
- Absolute paths replaced with content-addressed identities
- Command arguments and output stored as artifacts with separate opt-in

### 1.2 Usage Authority and Attribution

Usage metrics carry explicit source labels:

- **Observed**: Provider-reported authoritative values
- **Catalog-Derived**: Estimated from model catalog + context size
- **Estimated**: Fallback when provider data unavailable
- **Unknown**: No reliable source available
- **Stale**: Catalog version expired

Per-turn usage includes:
- Input/output/cache/reasoning token counts
- Context window utilization
- Cost with currency and catalog version
- Timestamp and observation source

Future: Per-attempt and retry attribution for background jobs.

### 1.3 Approval and Policy Observations

Turn traces record content-free policy state:
- Configured approval policy (never/always)
- Effective permission profile (Chat/Read Only/Workspace Write/Developer/Full Access)
- Sandbox state (read-only/write-capable)
- Runtime denial events with request kind only (no prompts, paths, commands)

**Authority Boundaries:**
- No user approval decisions stored (only denials)
- No execution results or mutation evidence
- Policy observations are metadata-only

### 1.4 Local Audit Events

Content-minimal audit log for privileged operations:
- Actor (user/system/runtime)
- Operation (project open, session start, file write, git commit, extension enable)
- Scope (project ID, session ID, file hash)
- Decision (allowed/denied/deferred)
- Result (success/failure/interrupted)
- Content hashes (no actual content)
- Timing and correlation IDs

Audit events use append-only storage with checksums and are rebuildable from event streams.

## 2. Diagnostic Tools and Debugging Support

### 2.1 Diagnostic Bundle Export

User-initiated, previewable, redacted diagnostic packages:

**Bundle Structure:**
- Envelope: `aegisy-diagnostic-bundle/0.1` with version, timestamp, correlation ID
- Categories: Runtime state, protocol events, session metadata, error logs, system info
- Redaction report: Count of secrets removed, paths sanitized, content excluded
- Deterministic archive: Reproducible byte-for-byte for same input

**Category Opt-In:**
- Default: Metadata-only (versions, timings, error classes, counts)
- Explicit opt-in per category: Session titles, sanitized protocol events, error messages
- Never included: Prompts, code, diffs, terminal output, credentials, provider response bodies

**Export Flow:**
1. Preview: Show categories, redaction counts, estimated size
2. User selection: Choose categories to include
3. Redaction: Multi-pass secret scanning, path normalization
4. Archive: Deterministic zip with manifest
5. Correlation ID: Generated for support tracking

### 2.2 Runtime Health and Degradation

`runtime/health` exposes:
- Process state (running/exited/unavailable)
- PID and uptime
- Adapter version and compatibility
- Transport state (connected/degraded/disconnected)
- Store health (healthy/read-only/recovery)

`runtime/degradations` reports:
- Missing capabilities with reason codes
- Feature gates (background-jobs/multi-agent/unattended-writes) with blocking task IDs
- Availability state (advertised/not-advertised/disabled)
- Override availability (for emergency disable)

### 2.3 Session and Store Recovery

**Read-Only Recovery Mode:**
- Triggered by: Migration failure, schema corruption, integrity check failure
- Capabilities: Status export, session read, portable export only
- Blocked: New sessions, turns, writes, imports, adapter launch
- UI: Clear recovery banner, export controls, no mutation actions

**Reconciliation for Interrupted Operations:**
- Evidence collection: Event state, process state, workspace state, Git state
- Decision: Unknown/running/completed/failed/blocked
- Blocker list: Missing evidence, changed workspace, Git conflicts
- User review: Explicit confirmation before recovery actions
- Durable recording: Content-free reconciliation events

### 2.4 Protocol Event Inspection

**Diagnostics View:**
- Timeline: Structured plan, tool events, approvals, errors
- Raw protocol: Available in diagnostics (not default view)
- Event sequence: Monotonic sequence numbers, correlation IDs
- Replay: From sequence number for reconnect testing

**Redaction:**
- Secrets removed before display
- Paths shown as relative or content-addressed
- Provider IDs and cache handles excluded
- User can copy sanitized events for bug reports

## 3. Evaluation Frameworks and Quality Metrics

### 3.1 Deterministic Adapter Replay

**Replay Harness:**
- Input: Recorded protocol fixtures (no secrets)
- Adapter: Codex/ACP/Native under test
- Verification: Event ordering, state transitions, error handling
- Property tests: Idempotency, cancellation, reconnect, overload

**Fixture Management:**
- Recorded from real sessions (redacted)
- Version-tagged (adapter version, protocol version)
- Canonical byte identity (SHA-256)
- Regression detection (fixture identity changes)

### 3.2 Repository Task Corpus

**Corpus Structure:**
- Task: Description, language, expected changes, test commands
- Provenance: Source repository, commit, license
- Reset script: Restore to initial state
- Expected tests: Commands that should pass after completion
- Secret/license scanning: Verify no leakage

**Metrics per Task:**
- Completion: Task fully completed (yes/no/partial)
- Regression: Previously passing tests now fail
- Test pass rate: Percentage of expected tests passing
- User correction rate: Edits needed after agent completion
- Approval burden: Number of approval prompts
- Latency: Time to completion
- Token usage: Input/output/cache/reasoning tokens
- Cost: Estimated cost with source

**Corpus Maintenance:**
- Regular updates for new languages/frameworks
- Difficulty levels (simple/medium/complex)
- Multi-turn scenarios
- Cross-file refactoring tasks

### 3.3 Model Role Recommendations

**Evaluation Pipeline:**
- Run corpus against model in each role (agent/plan/apply/review/fast)
- Measure: Completion rate, quality, speed, cost
- Compare: Against baseline and other models
- Version: Tie recommendations to evaluation version

**Recommendation Output:**
- Suitable roles per model
- Known limitations (context, tools, languages)
- Cost/quality trade-offs
- Confidence level (high/medium/low)

**Update Cadence:**
- Re-evaluate on model catalog updates
- Re-evaluate on corpus additions
- Mark stale recommendations (>90 days)

### 3.4 Adversarial Security Suites

**Prompt Injection Tests:**
- Untrusted sources: Files, issues, tool output, websites, MCP responses
- Attack vectors: Instruction override, secret exfiltration, unauthorized writes
- Expected behavior: Treat as data, not instructions; deny/redact secrets

**Secret Exfiltration Tests:**
- Target secrets: API keys, SSH keys, .env files, cloud credentials
- Attack methods: Read attempts, network exfiltration, log injection
- Expected behavior: Deny reads, redact from logs, block unauthorized network

**Sandbox Escape Tests:**
- Path traversal, symlink attacks, permission escalation
- Expected behavior: Deny outside workspace, reject symlinks, enforce policy

### 3.5 Endurance and Reliability Suites

**Crash Recovery:**
- Renderer crash: Workbench restarts, sessions preserved
- Sidecar crash: Reconnect, replay from sequence, resume work
- Database corruption: Read-only recovery, export available

**Reconnect Scenarios:**
- Network interruption, process restart, transport failure
- Expected: Snapshot/replay, no data loss, clear degradation state

**Migration Testing:**
- From every supported schema version
- Interrupted migrations (rollback, safe retry)
- Backup verification, schema validation

**Offline/Low-Resource:**
- Offline operation with cached catalog
- Low disk space (admission gates, GC)
- Memory pressure (bounded queues, backpressure)

**Runtime Upgrade:**
- Adapter version mismatch (reject, rollback)
- Protocol version negotiation
- Compatibility matrix enforcement

## 4. Privacy-Preserving Logging

### 4.1 Redaction Pipeline

**Multi-Pass Scanning:**
1. **Known Patterns**: API keys, JWTs, SSH keys, AWS credentials, tokens
2. **Field Names**: Credential-like field names (password, secret, token, key)
3. **Absolute Paths**: Replace with content-addressed identities or relative paths
4. **Environment Variables**: Remove credential-bearing vars before sidecar launch
5. **Provider Metadata**: Response IDs, cache handles, hidden reasoning

**Redaction Markers:**
- Visible indication that redaction occurred
- Count of redactions per category
- No original content recoverable

### 4.2 Content Exclusion

**Never Stored:**
- Raw API keys, long-lived JWTs, secure storage values
- Provider response IDs, cache continuation handles
- Encrypted reasoning, hidden thinking
- User's absolute filesystem paths (use content-addressed IDs)
- Environment variables from host

**Stored with Opt-In:**
- Prompts (default: excluded)
- Code diffs (default: excluded)
- Terminal output (default: excluded)
- File contents (default: excluded)

### 4.3 Local-First Telemetry

**Default Behavior:**
- All telemetry stored locally only
- No automatic cloud upload
- User controls retention (archive/delete policies)

**Opt-In Cloud Telemetry:**
- Aggregated reliability metrics only
- No prompt/code/path/diff/terminal content
- No extension arguments or output
- Explicit user consent required
- Clear data categories in consent UI

### 4.4 Diagnostic Export Privacy

**Preview Before Export:**
- Show categories to be included
- Display redaction counts
- Estimate bundle size
- Allow category removal

**Redaction Report:**
- Secrets removed: Count by pattern type
- Paths sanitized: Count of absolute paths replaced
- Content excluded: Categories not included
- Correlation ID: For support tracking (no PII)

## 5. Implementation Phases

### Phase 1: Foundation (Current)
**Status: Partially Complete**

- [x] Structured turn traces (`turn-trace/0.6`)
- [x] Usage authority labels (observed/estimated/unknown)
- [x] Error classification by domain
- [x] Content-addressed artifact storage
- [x] Secret redaction pipeline
- [x] Append-only event persistence
- [x] Read-only recovery mode
- [ ] Complete audit event logging
- [ ] Diagnostic bundle export (in progress)

### Phase 2: Diagnostics and Recovery
**Status: In Progress**

- [x] Runtime health reporting
- [x] Degradation and capability reporting
- [x] Session reconciliation (partial)
- [ ] Complete reconciliation for all operation types
- [ ] Diagnostic bundle with category opt-in
- [ ] Support correlation ID generation
- [ ] Recovery action execution

### Phase 3: Evaluation Infrastructure
**Status: Planned**

- [x] Deterministic adapter replay (fixtures exist)
- [ ] Repository task corpus
- [ ] Automated corpus execution
- [ ] Model role evaluation pipeline
- [ ] Adversarial security suites
- [ ] Endurance and reliability suites

### Phase 4: Quality Metrics and Recommendations
**Status: Planned**

- [ ] Completion/regression/test-pass metrics
- [ ] User correction rate tracking
- [ ] Approval burden analysis
- [ ] Latency/token/cost tracking by model/runtime
- [ ] Model role recommendations
- [ ] Evaluation version tracking
- [ ] Stale recommendation detection

### Phase 5: Release Gates and Dashboards
**Status: Planned**

- [ ] Release gate dashboard
- [ ] Required evidence tracking
- [ ] Threshold enforcement (block promotion)
- [ ] Cross-platform evidence matrix
- [ ] Known limitations documentation
- [ ] Support playbooks

## 6. Architecture Decisions

### 6.1 Local-First by Default

**Rationale:** User trust requires that sensitive data (code, prompts, diffs) never leaves the local machine without explicit consent.

**Implementation:**
- All traces stored in local SQLite
- Cloud telemetry opt-in only
- Aggregated metrics only (no content)
- Clear data category labels

### 6.2 Content-Addressed Identities

**Rationale:** Enable correlation and deduplication without storing actual content.

**Implementation:**
- SHA-256 hashes for files, artifacts, commands
- Domain-separated hash contexts
- Deterministic identity computation
- No path or content in identities

### 6.3 Explicit Source Labels

**Rationale:** Distinguish authoritative data from estimates to prevent false precision.

**Implementation:**
- Usage: observed/catalog-derived/estimated/unknown/stale
- Errors: classified by domain with retryability
- Costs: source-qualified with catalog version
- Metrics: never fabricated or inferred

### 6.4 Fail-Closed Validation

**Rationale:** Unknown or malformed telemetry should not corrupt state or grant authority.

**Implementation:**
- Schema validation before storage
- Unknown fields rejected (not ignored)
- Malformed traces quarantine session
- No authority granted by telemetry

### 6.5 Rebuildable Projections

**Rationale:** Corruption recovery requires authoritative event streams.

**Implementation:**
- Append-only event journal
- Projections rebuildable from events
- Checksums and sequence validation
- Read-only recovery on failure

## 7. Security and Privacy Guarantees

### 7.1 No Credentials in Telemetry

- API keys, JWTs, tokens redacted before storage
- Credential IDs stored (not values)
- Secure storage references only
- Multi-pass secret scanning

### 7.2 No Execution Authority from Telemetry

- Traces are observations only
- No approval decisions stored
- No mutation authority granted
- Policy observations are metadata-only

### 7.3 User Control Over Data

- Retention policies (archive/delete)
- Export with preview and redaction
- Local-only by default
- Opt-in for cloud telemetry

### 7.4 Support Correlation Without PII

- Correlation IDs for support tracking
- No usernames, emails, paths in IDs
- Deterministic bundle generation
- Redaction report included

## 8. Open Questions and Future Work

### 8.1 Automatic Compaction Telemetry

Should compaction checkpoints include telemetry summaries (token usage, cost, errors) for the compacted range?

**Trade-offs:**
- Pro: Historical metrics preserved after event pruning
- Con: Increases checkpoint size
- Decision: Defer until compaction is production-ready

### 8.2 Cross-Session Correlation

How to correlate related sessions (forks, child tasks) without exposing lineage in telemetry?

**Options:**
- Content-addressed lineage IDs
- Explicit correlation tokens
- No cross-session correlation in telemetry

### 8.3 Real-Time Metrics Dashboard

Should the UI expose real-time metrics (token rate, cost accumulation, approval rate)?

**Trade-offs:**
- Pro: User awareness of resource usage
- Con: Complexity, potential for false precision
- Decision: Start with post-turn summaries only

### 8.4 Evaluation Corpus Licensing

What licenses are acceptable for repository task corpus?

**Requirements:**
- Permissive licenses (MIT, Apache-2.0, BSD)
- No GPL/AGPL (copyleft concerns)
- Clear attribution in corpus metadata
- Regular license scanning

### 8.5 Model Performance Benchmarking

Should Aegisy publish public model performance benchmarks?

**Considerations:**
- Transparency vs. vendor relations
- Evaluation version staleness
- Corpus representativeness
- Legal review required

## 9. Success Criteria

### 9.1 Privacy

- [ ] Zero credentials in telemetry (verified by scanning)
- [ ] Zero automatic cloud uploads (verified by network monitoring)
- [ ] User preview before every export (UI test)
- [ ] Redaction report in every bundle (schema validation)

### 9.2 Diagnostics

- [ ] Read-only recovery from any migration failure (tested)
- [ ] Diagnostic bundle generation <5 seconds (performance test)
- [ ] Support can diagnose 80% of issues from bundle (support metrics)
- [ ] Reconciliation for all operation types (coverage test)

### 9.3 Evaluation

- [ ] Corpus covers 10+ languages (inventory)
- [ ] Deterministic replay for all adapters (CI gate)
- [ ] Adversarial suite blocks 100% of known attacks (security test)
- [ ] Endurance suite passes 1000 iterations (reliability test)

### 9.4 Quality Metrics

- [ ] Completion rate tracked per model/task (metrics pipeline)
- [ ] Regression detection within 1 hour (CI gate)
- [ ] Model recommendations updated monthly (automation)
- [ ] Release gate dashboard blocks bad builds (CI integration)

## 10. References

- `design.md` Section 15: Observability, evaluation, and privacy
- `tasks.md` Section 20: Observability, Diagnostics, and Evaluation
- `AAP-PROTOCOL-GUIDE.md`: Protocol event structure
- `AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md`: Export categories and redaction
- `AEGISY-TROUBLESHOOTING-RUNBOOK.md`: Recovery procedures
