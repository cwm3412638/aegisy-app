# Aegisy Cloud Contracts Design

## Overview

This document defines the Aegisy cloud service APIs and contracts that support the Agent Workbench. The cloud services provide model catalog distribution, capability discovery, account management, authentication, and usage tracking while maintaining strict separation from local execution and user data.

## Design Principles

- **Local-first execution**: Cloud services provide metadata and routing; actual Agent execution remains local
- **Explicit authority**: Every catalog field declares whether it is upstream-authoritative, Aegisy-configured, evaluation-derived, estimated, or unknown
- **Fail-safe degradation**: Desktop operates with cached catalog during temporary outages
- **No repository content**: Cloud telemetry receives only aggregated reliability metrics, never code or file content
- **Audience-restricted tokens**: Agent tokens are short-lived, scoped to specific models/sessions, and cannot administer accounts

## 1. Model Catalog Service

### 1.1 Catalog Schema

The versioned `model-catalog/0.1` schema provides:

**Core Identity**
- Stable Aegisy model ID (canonical identifier)
- Provider family (Anthropic, OpenAI, Google, etc.)
- Upstream model ID (provider's native identifier)
- Model aliases (alternative names)

**Availability and Lifecycle**
- Availability state (available, preview, deprecated, unavailable)
- Entitlement requirements (free tier, paid, enterprise)
- Region restrictions
- Deprecation timeline and replacement recommendations
- Lifecycle state (active, sunset, removed)

**Protocol and Capabilities**
- Supported wire protocols (Responses, Chat Completions, Anthropic Messages, Gemini, Aegisy-native)
- Context window limits (input tokens, output tokens)
- Tokenizer authority (upstream-measured vs estimated)
- Auto-compaction guidance thresholds
- Reasoning controls and ranges
- Prompt caching support
- Structured output capabilities
- Tool call support (single, parallel)
- Multimodal input (image, video, audio)
- Realtime/streaming support

**Role Suitability**
- Evaluated roles: `agent`, `plan`, `apply`, `review`, `fast`, `embedding`, `rerank`
- Evaluation version and sample size
- Known limitations per role
- Performance characteristics (latency, throughput)

**Cost and Usage**
- Display pricing metadata (per-token costs)
- Zero-data-retention indicators
- Routing constraints (regional, compliance)
- Usage tracking requirements

**Runtime Compatibility**
- Compatible runtime adapter families (Codex, ACP, native)
- Exact evaluated adapter versions
- Known degradations (warnings vs blocking)
- Feature availability matrix

### 1.2 Field Authority

Every catalog field explicitly declares its authority source:

- **Upstream-authoritative**: Provider-declared values (context limits, protocols)
- **Aegisy-configured**: Service-level settings (availability, entitlement, routing)
- **Evaluation-derived**: Measured through Aegisy testing (role suitability, compatibility)
- **Estimated**: Calculated approximations (token counts for non-reporting providers)
- **Unknown**: Explicitly unavailable information

Unknown and estimated values are non-authoritative and cannot be used for hard limits or billing.

### 1.3 Catalog Distribution

**Signing and Verification**

The catalog uses Ed25519 signatures with key rotation:

```
model-catalog-signature/0.1:
  - Canonical catalog payload (deterministic JSON serialization)
  - Signature timestamp
  - Signing key ID
  - Ed25519 signature bytes

model-catalog-key-ring/0.1:
  - Positive monotonic generation number
  - Content identity (SHA-256)
  - Ordered public keys with:
    - Key ID
    - Ed25519 public key
    - Validity window (not-before, not-after)
    - Revocation state
    - Replacement lineage
```

**Key Rotation Rules**
- Generation must advance exactly by one
- Prior keys and validity windows are preserved
- Old validity cannot be widened
- Revocation is permanent
- Identical generation is idempotent
- Rollback is rejected

**Root Trust Anchor**

Generation 1 requires external Ed25519 root anchor verification. Subsequent generations must be signed by a current, active, unrevoked key from the previous ring. The trust store:
- Persists root anchor and current ring
- Creates empty first-open marker (prevents re-bootstrap)
- Validates monotonic generation progression
- Detects tampering and rollback attempts

**Cache Management**

```
model-catalog-cache/0.1:
  - Signed catalog content identity
  - Positive monotonic sequence number
  - Receipt timestamp
  - Issue timestamp (from signature)
  - Expiry timestamp
  - TTL and stale window durations
  - Signature validation state
```

Cache states:
- **Fresh**: Within TTL, signature valid, can be used for selection
- **Stale**: Past TTL but within stale window, visible but marked
- **Expired**: Past stale window, hidden from selection
- **Invalid**: Failed signature or schema validation
- **Offline**: No cache available

The desktop can operate with stale cache during temporary outages but cannot invent updated limits, prices, or capabilities.

### 1.4 Catalog Endpoint

**HTTP Contract**

```
GET /v1/model-catalog
Authorization: Bearer <aegisy-token>
Accept-Encoding: identity
If-None-Match: "<etag>"
If-Modified-Since: <timestamp>

Response 200 OK:
Content-Type: application/json
ETag: "<etag>"
Last-Modified: <timestamp>
Body: <signed-catalog-envelope>

Response 304 Not Modified:
(no body, cache remains valid)
```

**Error Classification**

The refresh contract classifies failures:
- Authentication failures (401, 403)
- Redirect responses (3xx)
- Rate limiting (429)
- Server errors (5xx)
- Encoding violations (non-identity)
- Size limit exceeded
- Invalid body structure

Failures preserve existing cache and enter explicit degraded state.

### 1.5 Admin Validation

Catalog admin service validates:
- No duplicate aliases
- Aliases differ from model ID
- No duplicate role entries
- Supported authority keys only
- Positive token limits
- Valid protocol combinations
- No secret-shaped metadata
- Price policy compliance
- Capability consistency

## 2. Authentication and Authorization

### 2.1 Account Authentication

Desktop uses existing Aegisy account authentication:
- Login flow with secure token storage
- Long-lived refresh tokens (OS secure storage)
- Short-lived access tokens (memory only)
- Token refresh before expiry

Account tokens grant:
- Catalog access
- Profile management
- Usage query
- Agent token issuance

Account tokens do NOT grant:
- Direct model API access
- Repository content upload
- Execution control

### 2.2 Agent Tokens

Short-lived, audience-restricted tokens for model API calls:

```
agent-token/0.1:
  - Audience: specific provider/model
  - Scope: model IDs, optional session correlation
  - Expiry: minutes to hours (not days)
  - Capabilities: model API only
  - Restrictions: no account admin, no catalog write
```

**Issuance Flow**
1. Desktop requests agent token for specific model
2. Cloud validates account entitlement
3. Cloud issues scoped token with expiry
4. Desktop passes token to local gateway
5. Gateway uses token for provider API calls
6. Token expires, desktop requests refresh

**Token Refresh**
- Proactive refresh before expiry
- Graceful degradation on refresh failure
- No silent capability escalation
- Correlation preserved across refresh

### 2.3 Permission Boundaries

Cloud services cannot:
- Execute commands on user's machine
- Read repository contents
- Access filesystem
- Control local runtime
- Approve operations
- Modify user data

Local runtime cannot:
- Issue account tokens
- Modify catalog
- Access other users' data
- Bypass entitlement checks

## 3. Usage Tracking and Billing

### 3.1 Usage Correlation

Track usage with separation of concerns:

```
usage-event/0.1:
  - Account ID
  - Session correlation ID (opaque)
  - Model ID
  - Timestamp
  - Token counts:
    - Input tokens (prompt)
    - Output tokens (completion)
    - Cache read tokens
    - Cache write tokens
    - Reasoning tokens (if separate)
  - Request classification:
    - Initial request
    - Retry (same request)
    - Reroute (fallback model)
    - Child task
  - Cost attribution:
    - Parent session (for child tasks)
    - Retry deduplication
  - Provider response metadata:
    - HTTP status
    - Error classification
    - Latency
```

**Privacy Guarantees**
- No prompt content
- No completion content
- No file paths
- No repository names
- Opaque session IDs (cannot reverse to content)
- Aggregated metrics only

### 3.2 Cost Display

Desktop shows:
- Per-request token counts and estimated cost
- Session cumulative usage
- Project usage over time period
- Model comparison (cost per task type)
- Cache hit rates and savings

Cost metadata includes:
- Display currency
- Per-token rates (input, output, cache)
- Reasoning token rates (if applicable)
- Minimum charges
- Rate effective date
- Estimated vs actual (when provider reports)

### 3.3 Billing Integration

Cloud service:
- Aggregates usage events
- Applies pricing rules
- Generates invoices
- Handles payment processing
- Provides usage export

Desktop:
- Displays current billing period usage
- Shows cost breakdown by model/project
- Alerts on budget thresholds
- Links to cloud billing portal

## 4. Provider Error Handling

### 4.1 Error Classification

```
provider-error/0.1:
  - Kind: authentication, rate_limit, invalid_request, server_error, timeout, connection
  - Class: client_error, server_error, network_error
  - HTTP status (if applicable)
  - Retryable: boolean (conservative)
  - Provider-specific code (sanitized)
```

**Error Mapping**

Preserve upstream classification through gateway and AAP:
- HTTP status codes
- Provider error types (Anthropic, OpenAI, etc.)
- Rate limit metadata (retry-after)
- Quota exhaustion indicators
- Regional availability issues

**Content Filtering**
- Remove provider response bodies
- Remove credentials from errors
- Remove dynamic rollout text
- Preserve structured error codes
- Add bounded `x-aegisy-error-*` headers

### 4.2 Retry Orchestration

Gateway handles:
- Transient network failures (retry)
- Rate limits (backoff with retry-after)
- Server errors (limited retry)
- Authentication refresh
- Fallback routing (if configured)

Gateway does NOT:
- Retry client errors (4xx except 429)
- Fabricate success responses
- Hide persistent failures
- Exceed budget limits during retries

## 5. Implementation Phases

### Phase 1: Foundation (Current)
- [x] Internal catalog schema validation
- [x] Field authority labels
- [x] Signature verification contracts
- [x] Key ring rotation logic
- [x] Cache state machine
- [x] Trust store persistence
- [ ] HTTP refresh client
- [ ] Desktop cache integration
- [ ] Qt catalog UI

### Phase 2: Cloud Service
- [ ] Catalog signing service
- [ ] Key publication endpoint
- [ ] Authenticated catalog endpoint
- [ ] Conditional request support (ETag, Last-Modified)
- [ ] Admin validation service
- [ ] Catalog versioning and rollback protection

### Phase 3: Authentication
- [ ] Agent token issuance endpoint
- [ ] Token refresh flow
- [ ] Audience/scope validation
- [ ] Token revocation
- [ ] Desktop token management

### Phase 4: Usage and Billing
- [ ] Usage event ingestion
- [ ] Correlation and deduplication
- [ ] Cost calculation engine
- [ ] Billing integration
- [ ] Usage export API
- [ ] Desktop usage display

### Phase 5: Provider Integration
- [ ] Error classification mapping
- [ ] Retry orchestration
- [ ] Fallback routing
- [ ] Provider health monitoring
- [ ] Regional routing

### Phase 6: Advanced Features
- [ ] Role-based model recommendations
- [ ] Evaluation result publication
- [ ] Runtime compatibility matrix
- [ ] Degradation warnings
- [ ] Multi-model profile optimization

## 6. Security Considerations

### 6.1 Threat Model

**Threats Mitigated**
- Catalog tampering (signature verification)
- Key compromise (rotation, revocation)
- Rollback attacks (monotonic generation)
- Token theft (short-lived, scoped)
- Unauthorized model access (entitlement checks)
- Usage fraud (correlation, deduplication)

**Threats Accepted**
- Local machine compromise (desktop is trusted)
- Account credential theft (standard auth practices)
- Network eavesdropping (TLS required)

### 6.2 Data Privacy

**Never Sent to Cloud**
- Repository contents
- File paths
- Command output
- Terminal history
- Diff contents
- User messages (except to model APIs)
- Model responses (except from model APIs)

**Sent to Cloud**
- Account credentials
- Model selection
- Token counts
- Session correlation IDs (opaque)
- Error classifications
- Timing metadata

### 6.3 Compliance

Cloud service supports:
- Zero-data-retention routing (when contractually available)
- Regional data residency
- Usage export for audit
- Token revocation
- Account deletion

## 7. Operational Considerations

### 7.1 Availability

**Service SLAs**
- Catalog: 99.9% availability, stale cache fallback
- Token issuance: 99.9% availability, blocks new work
- Usage tracking: best-effort, queued locally

**Degraded Operation**
- Stale catalog: visible but marked, selection allowed
- Expired catalog: no new model selection, existing sessions continue
- Token issuance failure: blocks new turns, preserves existing
- Usage tracking failure: local queue, retry with backoff

### 7.2 Monitoring

Cloud service monitors:
- Catalog request rate and latency
- Token issuance rate and failures
- Usage event ingestion lag
- Provider error rates by model
- Cache hit rates
- Key rotation events

Desktop monitors:
- Catalog freshness
- Token refresh success rate
- Usage event queue depth
- Provider error rates
- Retry counts

### 7.3 Incident Response

**Catalog Compromise**
- Revoke signing key
- Issue new key ring (generation + 1)
- Desktop validates new ring against previous
- Automatic rollout to all clients

**Token Compromise**
- Revoke specific token (short-lived limits blast radius)
- Force refresh for affected sessions
- Audit usage for anomalies

**Provider Outage**
- Update catalog availability
- Desktop shows degraded state
- Fallback routing (if configured)
- User notification with alternatives

## 8. Testing Strategy

### 8.1 Contract Tests

- Catalog schema validation (positive and negative)
- Signature verification (valid, invalid, expired)
- Key rotation (valid progression, rollback rejection)
- Cache state transitions (fresh → stale → expired)
- HTTP refresh (200, 304, errors)
- Token issuance (valid, unauthorized, expired)

### 8.2 Integration Tests

- End-to-end catalog refresh
- Token lifecycle (issue, use, refresh, expire)
- Usage event flow (generate, send, verify)
- Error propagation (provider → gateway → AAP → UI)
- Degraded operation (stale cache, offline)

### 8.3 Security Tests

- Signature tampering detection
- Rollback attack prevention
- Token scope enforcement
- Entitlement bypass attempts
- Usage fraud detection

## 9. Future Considerations

### 9.1 Remote Execution

If remote execution is added (not in initial scope):
- Separate remote execution tokens
- Workspace isolation
- Result streaming
- Cost attribution
- Security boundaries

### 9.2 Collaborative Features

If collaboration is added:
- Shared session access
- Usage attribution per user
- Permission delegation
- Audit logging

### 9.3 Enterprise Features

- Custom model catalog
- Private model hosting
- Usage quotas and budgets
- Organizational policies
- SSO integration
- Audit export

## 10. References

- `openspec/changes/build-aegisy-agent-workbench/design.md` - Overall architecture
- `openspec/changes/build-aegisy-agent-workbench/tasks.md` - Section 9 implementation tasks
- `docs/AAP-PROTOCOL-GUIDE.md` - Agent protocol specification
- `docs/AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md` - Feature flags and channels
