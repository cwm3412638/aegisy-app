# Context Engine Design

## Overview

The Context Engine manages context gathering, relevance ranking, session compaction, and token budget allocation for the Aegisy Agent Workbench. It provides explicit layered context with provenance tracking, supports repository intelligence through tree-sitter and language servers, and enables session compaction for long-running work.

## Context Gathering and Relevance Ranking

### Context Layers

Context is built in six explicit layers with deterministic priority:

1. **System/Runtime Instructions** - Security policy, runtime constraints, capability boundaries
2. **Project Instructions** - `AGENTS.md`, supported vendor files, managed/user/nested instruction discovery
3. **Active Session State** - Current goal, plan, task state, approvals, compacted memory
4. **User-Pinned Context** - Files, selections, images, diagnostics, terminal excerpts, Git commits/diffs, artifacts
5. **Repository Intelligence** - Symbol-aware repository map, ranked search results, dependency graph
6. **Recent History** - Conversation turns, tool results, command output

### Instruction Discovery

Instruction files are discovered with deterministic precedence:

- **Managed** - `AEGISY_MANAGED_INSTRUCTIONS_DIR` (highest precedence)
- **User** - `AEGISY_USER_INSTRUCTIONS_DIR`
- **Nested** - Project-root-to-target ancestor chain (closer depth wins)
- **Project** - Project root (lowest precedence)

Discovery enforces:
- Symlink component rejection
- Sensitive/Git-ignored path filtering
- Case-collision detection
- UTF-8 validation, control character rejection
- Secret-shaped content denial
- Per-file and aggregate size limits

### Pinned Context

User-pinned context supports multiple source kinds with metadata-only persistence:

**Supported Kinds:**
- `file` - Complete file content with root-relative path
- `selection` - Bounded line/column range from file
- `image` - PNG/JPEG/WebP under 8 MiB, 8192px edge, 40M pixel limits
- `diagnostic` - Observed diagnostic with source command/server provenance
- `terminal-excerpt` - Bounded PTY tail excerpt (16 KiB max)
- `git-commit` - Filtered commit detail with exact OID
- `git-diff` - Worktree, staged, or commit diff with exact OID
- `artifact` - Command output artifact reference
- `child-handoff` - Parent-to-child session handoff data

**Persistence:**
- Content-addressed immutable objects under Workbench data root
- Atomically replaced project pointers with compare-and-swap
- SHA-256 content identity for all descriptors
- Metadata-only storage (no bodies in descriptors)
- Bounded: 1,024 projects, 4,096 objects, 1 MiB/set, 256 MiB total

**Freshness Tracking:**
- Workspace watch/save events mark file/selection/diagnostic pins stale
- Terminal restart/removal marks terminal-excerpt pins stale
- Git object availability checked for commit/diff pins
- Stale descriptors preserved with explicit labels

### Repository Intelligence

**Tree-sitter Symbol Extraction:**
- Supported: Rust, Python, JavaScript, TypeScript/TSX, C/C++
- Official grammar tag queries for symbol provenance and ranges
- Syntax-tree import/include queries for dependency edges
- Bounded file-level incremental cache
- Reparses only changed supported files
- Never stores source content

**Repository Map:**
- Token-budgeted: 256-8192 token range
- Symbol navigation and dependency inspection
- Stale state indicators
- Lazy Structure view

**Language Server Integration:**
- Supported: rust-analyzer, Pyright, TypeScript Language Server, clangd
- Lazy start, supervised lifecycle, graceful timeout
- Bounded LSP stdio framing
- Definition, reference, diagnostic results
- UTF-16 position translation
- Explicit provenance and stale revision checks
- Git/sensitive/symlink filtering
- Server-initiated edits denied
- Restricted configurations (no build scripts, proc macros, checks, dependency fetching)

### Search and Ranking

**Filename Search:**
- Bounded 50-result UI pages
- Deterministic snapshot cursors
- Stale cursor restart from new snapshot
- Workspace sensitivity, symlink, ignore policy
- Hard entry/file/byte/match limits

**Text Search:**
- Same pagination and policy as filename search
- Stale-index indicators after watched/user changes
- Cancellation support with last-complete-snapshot preservation

## Session Compaction and Checkpoint Creation

### Compaction Checkpoints

**Schema:** `session-compaction/0.1`

Validated bounded summaries for:
- Decisions made during session
- Unresolved tasks and next actions
- Changed files with paths and summaries
- Commands executed with results
- Tests run with outcomes
- Failures encountered with context
- Next steps and recommendations

**Validation:**
- Secret-shaped/control-character content rejection
- Item and byte limits enforcement
- Content-hashed review IDs
- Activation requires exact sequence and source-context identity

**Storage:** `session-compaction-checkpoint-store/0.1`

- Content-addressed objects under Workbench data root
- Hashed session/checkpoint pointers
- No-clobber publication
- Bounded: counts and bytes per checkpoint
- Private Unix permissions
- Restart revalidation
- Idempotent identical writes
- Tamper preservation

**Events:**
- `session.compaction-checkpointed/0.1` - Metadata-only, idempotent
- Revision events carry validated `supersedes` descriptor
- Original history remains authoritative
- No summary/instruction content in events

### Compaction Triggers

**Automatic Thresholds:** `context-threshold/0.1`

Evaluator distinguishes:
- Authoritative observations (provider-reported)
- Conservative estimates (fallback tokenizer)
- Stale observations (outdated usage data)
- Unknown observations (missing data)

Decisions:
- `no-action` - Within soft limits with hysteresis
- `preview-required` - Approaching soft limit
- `hard-limit-exceeded` - Must compact before next turn

**Manual Compaction:**
- Explicit user-initiated checkpoint creation
- Review dialog with editable preservation instructions
- Edit-to-new-review flow
- Activation requires reviewed checkpoint

### Compaction Workflow

1. **Create** - Derive sequence/context identity from complete verified event stream
2. **Review** - User inspects/edits summaries and preservation instructions
3. **Revise** (optional) - Create new checkpoint from source with changes
4. **Activate** - Apply checkpoint to session context (not yet implemented)
5. **Preserve** - Original history remains locally available

**Constraints:**
- Blocks active turns during creation
- Detects conflicting checkpoint reuse
- Requires validated object and matching event after restart
- Filesystem object and SQLite event not one transaction
- Startup compensation for incomplete publications

## Repository Maps and Code Indexing

### Incremental Indexing

**Index Lifecycle:**
- Runs incrementally on file changes
- Honors ignore and secret rules
- Never blocks project opening
- Cancellation preserves last complete snapshot
- Synchronous stdio runtime may finish bounded request before processing queued cancel

**Index Content:**
- Symbol definitions with provenance
- Import/include dependency edges
- File-level granularity
- No source content storage
- Bounded: 5,000 files, 20,000 symbols hard limits

**Index Invalidation:**
- Watched file changes trigger reparse
- User saves trigger reparse
- Deleted files removed from index
- Ignored files removed from index
- Stale state exposed in Structure view

### Symbol Navigation

**Structure View:**
- Symbol list with provenance
- Dependency inspection
- Stale state indicators
- Definition/reference navigation
- Diagnostic integration

**Language Server Queries:**
- Definition lookup with UTF-16 position
- Reference finding with filtering
- Diagnostic observation with source tracking
- File hash binding for freshness
- Unavailable-server states

### Dependency Graph

**Extraction:**
- Tree-sitter syntax-tree queries
- Import/include statement parsing
- Bounded edge storage
- No transitive closure computation

**Usage:**
- Repository map token budget allocation
- Related file discovery
- Context relevance ranking

## Token Budget Management

### Budget Allocation

**Schema:** `context-budget/0.1`

Deterministic allocation order:
1. Instruction precedence ranks (managed > user > nested > project)
2. Pinned context priority (user-assigned)
3. Task state and recent turns
4. Tool results
5. Search results
6. Repository map

**Bounds:**
- 64 KiB total hard limit
- 16 KiB per-item hard limit
- Content-free allocation metadata
- Explicit exclusion reasons in manifest

### Tokenizer Adapters

**Schema:** `tokenizer/0.1`

**Fallback:** `unknown-utf8-four-byte`
- Marked `conservative-unknown`
- `exact: false`
- `provider_window_authoritative: false`
- Conservative 4-byte-per-token estimate

**Provider-Specific:** (not yet implemented)
- Model-specific tokenizer authority
- Exact token counts
- Provider context window enforcement

### Context Manifest

**Schema:** `context-manifest/0.1`

Per-entry metadata:
- Source kind and origin
- Pinned priority
- Trust level (untrusted-data for external sources)
- SHA-256 content identity
- Conservative token estimate
- Freshness state
- Inclusion reason
- Included/excluded state
- Truncation indicators

**Manifest Properties:**
- Never contains attachment text
- Stale revisions explicitly labelled
- Bounded items with explicit reasons
- Fail-closed truncation handling

### Context Inspector

**AAP Method:** `turn/context/inspect`

Read-only preflight:
- Reuses exact Work-session instruction discovery
- Stale validation
- Context-budget preparation
- No model start or content persistence
- Returns manifest/budget provenance
- `content_included: false` (bodies never returned)

**Qt UI:**
- Search-icon preflight action
- Read-only metadata table
- Trust, size, freshness, inclusion/exclusion display
- Unchecked client context as explicit exclusion marker

## Implementation Phases

### Phase 1: Foundation (Partial - In Progress)

**Completed:**
- Context manifest schema with metadata-only entries
- Instruction discovery with precedence rules
- Pinned context store with content-addressed objects
- Tree-sitter symbol extraction for 5 languages
- Language server integration for 4 servers
- Repository map with token budgets
- Search with pagination and cancellation
- Compaction checkpoint schema and storage
- Context threshold evaluator
- Budget allocation framework
- Tokenizer fallback contract

**Remaining:**
- Atomic/recoverable boundary across object/pointer and event/Blob transactions
- Complete artifact/image/diagnostic/terminal/Git cross-platform evidence
- Child handoff lifecycle integration
- Qt lifecycle/order/inclusion recovery

### Phase 2: Compaction Activation (Not Started)

**Requirements:**
- Preservation instructions editable before activation
- Model-generated summaries
- Automatic threshold authority
- Provider compact integration
- Failure compensation and recovery
- Permission/provider gates

### Phase 3: Advanced Context (Not Started)

**Requirements:**
- Provider-specific tokenizer adapters
- Authoritative context window enforcement
- Task state and recent turn producers
- Tool result context integration
- Search result ranking
- Complete context inspector with all classes

### Phase 4: Quality and Scale (Partial)

**Completed:**
- Large monorepo ignored tree handling
- Irrelevant repository-map resistance
- Stale file reread validation
- Nested target-scoped instruction inheritance
- Intentional exclusion vs budget truncation distinction

**Remaining:**
- Provider switching evidence
- Cross-platform scale evidence
- Large session compaction tests
- Multi-model context portability

### Phase 5: Production Hardening (Not Started)

**Requirements:**
- Host-managed policy roots
- Durable configuration
- Complete instruction exclusion reporting
- Trust/approval intersection
- Clean Windows/runtime evidence
- Complete cross-platform validation

## Security and Privacy

### Untrusted Data Handling

All external sources marked `untrusted-data`:
- Project instructions (cannot grant permissions, execute commands, enable hooks, authorize network)
- Repository content
- Search results
- Tool output
- Terminal excerpts
- Diagnostic observations

### Secret Protection

**Detection:**
- Secret-shaped content in instruction files
- Credential-like field names
- Recognized secret values
- API keys, tokens, passwords in context

**Redaction:**
- Before event sequence allocation
- Before projection write
- In item payloads before storage
- In diagnostic artifacts
- In terminal excerpts
- In command output

**Exclusion:**
- Sensitive paths from repository map
- Git-ignored files from search
- Credential stores from file operations
- Environment variables from context

### Provenance Tracking

Every context item includes:
- Source kind and origin
- Trust level
- Last update timestamp
- Content hash
- Whether included in model request
- Exclusion reason if not included

## Error Handling and Recovery

### Stale Context

**Detection:**
- File content hash mismatch
- Workspace watch/save events
- Terminal restart/removal
- Git object unavailability
- Diagnostic source loss

**Handling:**
- Explicit stale labels in UI
- Authoritative reread before turn start
- Fail-closed on validation failure
- Preserved descriptors with stale metadata

### Missing Context

**Scenarios:**
- Runtime restart clears in-memory stores
- Diagnostic source eviction
- Terminal buffer eviction
- Git object deletion
- Artifact reference loss

**Recovery:**
- Fail-closed rather than inferring content
- Explicit unavailable states
- Preserved metadata for inspection
- No automatic content regeneration

### Compaction Failures

**Checkpoint Creation:**
- Active turn blocks creation
- Conflicting checkpoint reuse detected
- Validation failure preserves original context

**Activation Failures:**
- Model-change recovery option
- Portable-fork recovery option
- Manual-cleanup recovery option
- Original context preserved

### Budget Overflow

**Handling:**
- Hard 64 KiB total limit enforced
- Hard 16 KiB per-item limit enforced
- Explicit truncation metadata
- Content-free exclusion reasons
- Fail-closed on limit violation

## Future Considerations

### Multi-Agent Context

- Parent/child session handoff data
- Child task result summaries
- Artifact-based result transfer
- Lineage-aware context inheritance

### Cross-Provider Portability

- Opaque response ID stripping
- Encrypted reasoning exclusion
- Cache handle removal
- Hidden thinking exclusion
- Portable context packages

### Advanced Ranking

- Relevance scoring algorithms
- Usage pattern learning
- Temporal decay functions
- Cross-file relationship weights

### Streaming Context

- Incremental context updates
- Partial result streaming
- Progressive refinement
- Real-time freshness updates
