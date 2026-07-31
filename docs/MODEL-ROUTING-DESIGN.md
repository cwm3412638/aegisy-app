# Model Routing Design

## Overview

This document defines the model profile structure, provider routing, mid-turn switching constraints, capability negotiation, and Aegisy cloud service integration for the Agent Workbench. The design ensures that model selection is capability-aware, provider-agnostic at the protocol level, and integrated with Aegisy's account services for availability, entitlement, and billing.

## Model Profile Structure

### Profile Definition

A model profile is a project-scoped configuration that selects different models for specific roles:

- **Agent**: Primary model for interactive work and tool execution
- **Plan**: Model for generating structured plans and task decomposition
- **Apply**: Fast model for applying edits and patches
- **Review**: Model for code review and quality checks
- **Fast**: Model for quick responses and simple queries
- **Embedding**: Model for semantic search and context retrieval
- **Rerank**: Model for result reranking and relevance scoring

### Profile Storage

Model profiles are stored in SQLite schema with:
- Profile ID and name
- Project binding (optional, null for user default)
- Per-role model selections (Aegisy model IDs)
- Creation and update timestamps
- Active/archived state

### Default Profile

The default profile uses a single model for all roles to minimize cost and latency. Multi-model pipelines are opt-in and expose expected extra cost and latency in the UI.

## Provider Routing

### Supported Providers

The system routes to multiple provider families through adapters:

1. **Codex App Server** (0.144.5): Anthropic Messages protocol, Claude models
2. **Claude Direct**: Anthropic Messages API
3. **Gemini**: Google Gemini API
4. **OpenCode**: OpenAI-compatible protocol

### Routing Architecture

```
┌─────────────────┐
│  Model Catalog  │ (Aegisy cloud service)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Model Selection │ (Qt UI + AAP)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Runtime Adapter │ (aegisy-agentd)
└────────┬────────┘
         │
    ┌────┴────┬────────┬─────────┐
    ▼         ▼        ▼         ▼
┌───────┐ ┌──────┐ ┌──────┐ ┌─────────┐
│ Codex │ │Claude│ │Gemini│ │OpenCode │
└───────┘ └──────┘ └──────┘ └─────────┘
```

### Adapter Selection

The runtime selects an adapter based on:
- Model's declared wire protocol (from catalog)
- Session's current runtime binding
- Adapter availability and health
- Required capabilities for the current mode

## Mid-Turn Switching Constraints

### Switching Levels

Three levels of model switching are defined:

#### 1. Compatible Switch
- Same runtime adapter and portable protocol state
- Applied at turn boundary only
- Records a `modelChanged` timeline item
- No session fork required
- Example: Claude Opus → Claude Sonnet within Codex adapter

#### 2. Portable Fork
- Runtime or protocol changes required
- Creates a new child session with portable context package:
  - User/agent messages
  - Approved summaries
  - Current plan and open tasks
  - Selected files and repository state
  - Tool results needed for continuity
- Old session remains intact and linked
- UI may present seamless lineage but diagnostics show the fork
- Example: Codex adapter → native Gemini adapter

#### 3. Blocked Switch
- Required capability unavailable in target model
- UI explains exact missing capabilities
- Offers compatible alternative models
- No automatic fallback or silent degradation
- Example: Switching to a model without tool call support during active tool use

### Non-Portable State

The following state is never replayed across providers:
- Provider-encrypted reasoning
- Opaque response IDs
- Cache handles and continuation tokens
- Hidden thinking or internal reasoning traces
- Provider-specific metadata

### Turn Boundary Enforcement

Model changes are only applied:
- After current turn completes, fails, or is interrupted
- Never during an active upstream response
- Never during a pending tool call
- Never during an approval workflow

## Capability Negotiation

### Model Catalog Schema

The Aegisy cloud service exposes a versioned model catalog with entries containing:

#### Identity and Availability
- Stable Aegisy model ID
- Provider family (codex, claude, gemini, opencode)
- Upstream model ID
- Availability status (available, deprecated, preview)
- Entitlement requirements
- Region restrictions
- Deprecation state and sunset date
- Model aliases

#### Protocol Capabilities
- Supported wire protocols (responses, chat-completions, anthropic-messages, gemini, aegisy-native)
- Context window size (input tokens)
- Output token limits
- Tokenizer authority (for usage reporting)
- Auto-compaction guidance
- Reasoning controls and ranges
- Prompt caching support
- Structured output support
- Tool call support
- Parallel tool execution
- Image/video/audio input modalities
- Realtime support

#### Role Suitability
Evaluated roles backed by Aegisy evaluations:
- `agent`: Full agent capabilities with tools
- `plan`: Planning and task decomposition
- `apply`: Fast edit application
- `review`: Code review and quality checks
- `fast`: Quick responses
- `embedding`: Semantic embeddings
- `rerank`: Result reranking

#### Cost and Compliance
- Cost/usage display metadata
- Zero-data-retention flags
- Routing constraints (when contractually known)
- Enterprise compliance markers

#### Runtime Compatibility
- Compatible runtime adapter versions
- Known feature degradations
- Required AAP capabilities

### Catalog Caching

The desktop application:
- Caches a signed catalog with expiry timestamp
- Validates signature before use
- Marks catalog as stale when expired
- Continues with stale catalog during temporary outage
- Cannot invent updated limits, prices, or capabilities
- Refreshes on startup and periodically

### Capability Matching

A model is selectable only when:
- Required capabilities match the active mode (Chat vs Work)
- Runtime adapter supports the model's protocol
- User has required entitlements
- Model is not deprecated or sunset
- Session's required features are available

Unknown capabilities disable the dependent feature rather than simulating success.

### Negotiation Flow

```
1. User selects model or profile
2. Qt validates against cached catalog
3. Qt checks entitlements and availability
4. Qt matches capabilities to session requirements
5. If compatible: apply at turn boundary
6. If portable: offer fork with context package
7. If blocked: show missing capabilities and alternatives
```

## Aegisy Cloud Service Integration

### Authenticated Endpoints

The Aegisy cloud service provides:

#### Model Catalog
- `GET /api/v1/models/catalog`
- Returns signed, versioned catalog
- Includes expiry timestamp
- Filtered by user entitlements

#### Runtime Compatibility
- `GET /api/v1/runtime/compatibility`
- Returns adapter version matrix
- Indicates which runtime versions work with which models
- Controls which combinations can ship together

#### Agent Tokens
- `POST /api/v1/agent/token`
- Issues short-lived, audience-restricted tokens
- Scoped to specific model IDs
- Includes project/session correlation
- Does not grant account administration
- Expires quickly (minutes, not hours)

#### Usage Reporting
- `POST /api/v1/usage/report`
- Accepts token usage and cost estimates
- Correlates with project/session
- Does not include prompt or file content
- Aggregated for billing

#### Remote Job Control (Future)
- Optional remote job submission
- Requires separate security gate
- Not available in initial milestones

### Offline Operation

The desktop can operate during temporary account-service outage:
- Uses cached catalog (marked as stale)
- Uses existing credentials
- Cannot refresh entitlements
- Cannot get new agent tokens
- Cannot verify new model availability
- Queues usage reports for later submission

### Error Handling

Provider errors retain upstream classification:
- HTTP status codes preserved
- Rate limit information passed through
- Quota exhaustion reported clearly
- Authentication failures distinguished from authorization
- Network errors separated from provider errors

The AAP error model carries:
- Error class (provider, transport, timeout, etc.)
- Retryability flag
- Bounded error message
- No raw credentials or tokens

## Security and Privacy

### Credential Handling

- Long-lived Aegisy credentials remain in OS secure storage
- Database contains credential IDs only, never raw API keys
- Agent tokens are short-lived and audience-restricted
- Tokens are scoped to model IDs and session correlation
- Provider API keys never enter the Qt UI process
- All provider calls go through the sidecar

### Data Retention

- Model catalog is signed and versioned
- Usage reports are content-free (token counts only)
- No prompt content sent to Aegisy telemetry by default
- Provider routing constraints honored when contractually known
- Zero-data-retention models marked in catalog

### Audit Trail

Local audit events record:
- Model selection changes
- Profile updates
- Capability negotiation results
- Provider routing decisions
- Token issuance and expiry
- Usage report submission
- Content-minimal: no prompts, no file content

## Implementation Status

### Completed
- Model catalog schema definition
- Profile storage schema (v20)
- Codex adapter pinning (0.144.5)
- Runtime adapter selection logic
- Turn boundary enforcement
- Capability negotiation framework

### In Progress
- Model catalog endpoint implementation
- Agent token issuance
- Usage reporting integration
- Multi-provider adapter support

### Pending
- Gemini adapter
- OpenCode adapter
- Native Aegisy adapter
- Portable fork context packaging
- Cross-provider capability matrix
- Complete offline operation testing

## References

- Design document: `openspec/changes/build-aegisy-agent-workbench/design.md` (Decision 6)
- Task list: `openspec/changes/build-aegisy-agent-workbench/tasks.md` (Section 10)
- AAP Protocol Guide: `docs/AAP-PROTOCOL-GUIDE.md`
- Model catalog ADR: `docs/adr/0004-model-catalog-trust-and-authority.md`
