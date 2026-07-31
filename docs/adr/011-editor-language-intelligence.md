# ADR 011: Editor Language Intelligence Architecture

- Status: Accepted
- Accountable owner: Editor Platform
- Consulted owners: Runtime Integrations, Product Security, Performance
- Recheck gate: Supported-language scope changes

## Context

The Agent Workbench editor requires code completion, diagnostics, go-to-definition, and other language intelligence features. Users expect IDE-like capabilities when editing code, but language servers can execute arbitrary code through project configuration, build scripts, and plugins. The editor must provide useful intelligence without creating execution authority outside the permission model.

Four architectural approaches were considered:

1. **Full LSP integration**: Direct Monaco-to-LSP connection with unrestricted capabilities
2. **Monaco built-in only**: Syntax highlighting and basic editing without external servers
3. **Tree-sitter only**: Static analysis for symbols without language server processes
4. **Hybrid approach**: Sidecar-owned LSP with bounded communication and restricted capabilities

## Decision Options

### Option 1: Full LSP Integration

Connect Monaco's language client directly to language server processes with full protocol support.

**Advantages:**
- Complete IDE feature parity
- Minimal custom implementation
- Leverages existing LSP ecosystem

**Disadvantages:**
- Language servers can execute arbitrary code through workspace configuration
- Project build scripts become hidden execution authority
- No isolation from Qt process or web content
- Difficult to enforce permission boundaries
- Security review required for every supported language server

### Option 2: Monaco Built-in Only

Use only Monaco's built-in syntax highlighting and basic editing features.

**Advantages:**
- No external process execution
- Minimal security surface
- Simple implementation

**Disadvantages:**
- Poor developer experience
- No semantic understanding
- No cross-file navigation
- Diagnostics limited to observed build output
- Not competitive with existing coding tools

### Option 3: Tree-sitter Only

Use tree-sitter for static syntax analysis and symbol extraction without language servers.

**Advantages:**
- No process execution
- Fast, incremental parsing
- Sufficient for repository maps and basic navigation

**Disadvantages:**
- No semantic analysis or type information
- Limited cross-file understanding
- No diagnostics beyond syntax errors
- Requires grammar maintenance for each language
- Cannot leverage existing LSP ecosystem

### Option 4: Sidecar-Owned LSP with Bounded Communication

Run language servers as child processes of the sidecar with restricted protocol support and bounded stdio framing.

**Advantages:**
- Isolated from Qt UI process and web content
- Sidecar enforces permission boundaries
- Can leverage LSP ecosystem with restrictions
- Bounded message sizes prevent resource exhaustion
- Read-only operations available without approval
- Graceful degradation when servers unavailable

**Disadvantages:**
- Custom protocol translation layer required
- Some LSP features permanently restricted
- Per-language security review still needed
- Additional implementation complexity

## Performance Considerations

- **Startup latency**: Language servers add 100-500ms per language on project open
- **Memory overhead**: Each server process consumes 50-200MB
- **Message throughput**: Bounded framing limits messages to 4MB, preventing large responses
- **Incremental updates**: Tree-sitter provides fast symbol updates between LSP requests

## Capability Considerations

- **Semantic features**: Available through LSP (completion, hover, definition, references)
- **Syntax features**: Always available through Monaco and tree-sitter
- **Diagnostics**: Observed build output is authoritative; LSP diagnostics are supplemental
- **Refactoring**: `workspace/applyEdit` permanently denied; refactors become proposals
- **Configuration**: Project-specific LSP configuration disabled in initial release

## Maintenance Considerations

- **Language server versions**: Fixed or policy-discovered paths, not auto-downloaded
- **Protocol evolution**: AAP translation layer isolates Qt from LSP version changes
- **Security updates**: Each language server requires independent review and fixtures
- **Cross-platform**: Server discovery and execution must work on macOS and Windows

## Decision

Use **Option 4: Sidecar-Owned LSP with Bounded Communication**.

The sidecar (`aegisy-agentd`) owns all language server processes and enforces these boundaries:

1. **Process isolation**: Language servers run as child processes of the sidecar, never the Qt UI
2. **Bounded stdio framing**: All LSP messages limited to 4MB; oversized messages rejected
3. **Protocol translation**: Sidecar translates LSP to AAP; Qt never receives native LSP messages
4. **Read-only operations**: Definition, reference, hover, completion available without approval
5. **Write operations denied**: `workspace/applyEdit` always returns error; no hidden mutations
6. **Restricted discovery**: Fixed-name or path-policy-discovered servers only; no auto-download
7. **Bounded resources**: Environment, roots, paths, frames, results, and timeouts all limited
8. **Unsafe features disabled**: Project configuration, build scripts, dependency fetching, proc macros, and arbitrary code execution remain off unless future sandbox decisions explicitly enable them

Monaco provides the baseline: syntax highlighting, editing, and save operations work without language servers. LSP is an optional enhancement that renders explicit unavailable state on failure.

## Implementation Status

- Sidecar LSP process management: Implemented
- Bounded stdio frame reader: Implemented
- AAP translation layer: Implemented for core methods (initialize, definition, references, hover, completion, diagnostics)
- Monaco language client integration: Implemented
- Security review: Completed for rust-analyzer, typescript-language-server
- Cross-platform evidence: macOS complete, Windows in progress

## Consequences

### Positive

- Language intelligence available without compromising security model
- Graceful degradation when servers unavailable or restricted
- Clear separation between syntax (always available) and semantic (LSP-dependent) features
- Sidecar isolation prevents UI compromise from affecting language server execution
- Bounded framing prevents resource exhaustion attacks
- AAP translation provides stable interface despite LSP protocol evolution

### Negative

- Some LSP features permanently unavailable (workspace edits, configuration, build integration)
- Each new language requires security review and fixtures
- Additional complexity in sidecar implementation
- Cannot claim full IDE semantic precision for restricted modes
- Users may expect unrestricted LSP behavior from other tools

### Neutral

- Build diagnostics remain authoritative; LSP diagnostics are supplemental
- Missing or failed servers render unavailable state; Monaco continues working
- Adding new languages requires installed-version fixtures and cross-platform evidence
- Trusted project configuration remains future work behind explicit sandbox decisions
