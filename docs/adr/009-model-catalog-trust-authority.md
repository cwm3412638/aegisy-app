# ADR 0009: Model Catalog Trust Authority

- Status: Accepted
- Accountable owner: Model Control Plane
- Consulted owners: Product Security, Runtime Integrations, Desktop Platform
- Due gates: OpenSpec `9.1` through `9.4`, before model selection and routing

## Context

The Agent Workbench requires an authoritative source for model capabilities, routing, trust, and compatibility. Models from different providers have varying capabilities (context limits, tool support, streaming protocols, reasoning controls, caching, structured output, modalities). The system must determine which models are available, what features they support, which runtime adapters can use them, and how to route requests correctly.

Four potential authorities exist:

1. **Aegisy cloud service**: centralized catalog with signed metadata
2. **Local configuration**: user-maintained model definitions
3. **Provider-reported**: capabilities discovered from provider APIs
4. **Hybrid**: combination of the above sources

The decision affects security, reliability, user experience, offline operation, and the ability to enforce entitlements and routing policies.

## Decision Options

### Option 1: Aegisy Cloud Service as Authority

The Aegisy cloud service publishes a signed, versioned model catalog containing:

- Stable Aegisy model IDs, provider families, upstream model IDs
- Availability, entitlement, region, deprecation state, aliases
- Supported wire protocols (Responses, Chat Completions, Anthropic Messages, Gemini, Aegisy-native)
- Context/output limits, tokenizer authority, reasoning controls, prompt caching
- Tool calls, parallel tools, structured output, modalities (image/video/audio)
- Suitability roles (`agent`, `plan`, `apply`, `review`, `fast`, `embedding`, `rerank`)
- Cost/usage metadata, zero-data-retention constraints
- Runtime compatibility and feature degradations

The desktop caches a signed catalog with expiry and explicit stale state. Models are selectable only when required capabilities match the active mode and runtime adapter.

**Advantages:**
- Single source of truth for capabilities and routing
- Aegisy can update capabilities without desktop releases
- Enforces entitlements and billing policies
- Supports evaluated suitability roles backed by Aegisy testing
- Prevents capability drift between provider changes
- Enables deprecation and migration paths

**Disadvantages:**
- Requires network connectivity for updates
- Aegisy must maintain catalog accuracy
- Cannot support arbitrary local models without catalog entries
- Dependency on cloud service availability

### Option 2: Local Configuration

Users maintain model definitions in local configuration files.

**Advantages:**
- Works completely offline
- Users can add arbitrary models
- No dependency on Aegisy services

**Disadvantages:**
- No capability validation or updates
- Cannot enforce entitlements or routing policies
- Users must manually track provider changes
- No evaluated suitability roles
- Security and billing enforcement impossible

### Option 3: Provider-Reported Capabilities

Query provider APIs for model capabilities at runtime.

**Advantages:**
- Always current with provider state
- No Aegisy catalog maintenance

**Disadvantages:**
- Providers may not expose all relevant metadata
- No standardized capability schema across providers
- Cannot enforce Aegisy entitlements or routing
- Latency on every model selection
- No evaluated suitability roles
- Providers may report inaccurate capabilities

### Option 4: Hybrid Approach

Combine Aegisy catalog with local overrides and provider discovery.

**Advantages:**
- Flexibility for local models
- Fallback when catalog unavailable

**Disadvantages:**
- Complex precedence rules
- Inconsistent capability guarantees
- Difficult to enforce policies
- Increased testing surface

## Security and Capability Considerations

- **Trust**: Aegisy catalog uses Ed25519 signatures with key-ring rotation (see ADR 0004)
- **Freshness**: Desktop enforces fresh/stale/expired/invalid/offline/empty states
- **Authority classes**: upstream-authoritative, Aegisy-configured, evaluation-derived, estimated, observed, stale, unknown
- **Capability enforcement**: Runtime adapters validate negotiated capabilities; unknown capabilities disable dependent features
- **Entitlement**: Catalog controls which models are available to which accounts
- **Routing**: Catalog specifies correct provider endpoints and protocols
- **Offline operation**: Cached catalog allows operation during temporary outages but cannot invent updated limits or capabilities

## Current Decision

**Aegisy cloud service is the authority for model capabilities, routing, and trust.**

The website/API exposes a versioned, signed model catalog. The desktop caches it with explicit freshness states. A model is selectable only when:

1. Catalog signature is valid and fresh
2. Model is available for the current account/entitlement
3. Required capabilities match the active mode and runtime adapter
4. Model is not deprecated or the deprecation allows continued use

Unknown capabilities disable dependent features rather than simulating support. Provider errors retain upstream classification through the gateway and AAP error model.

Local model support (Ollama, LM Studio) is addressed separately in ADR 0005 and requires explicit policy decisions about unmanaged providers.

## Evidence and Implementation Status

Current implementation (per design.md section 6):

- Catalog schema defined with capability fields
- Desktop caching with expiry and stale state
- Model selection gated on capability match
- Project model profiles support role-specific models (agent, plan, apply, review, fast, embedding, rerank)
- Default profile uses minimal roles

Required production evidence (per ADR 0004):

- Production trust root custody and rotation ceremony
- Authenticated conditional GET endpoint
- Admin validation for contradictions
- Fresh/stale/expired/offline/invalid desktop fixtures
- macOS/Windows restart and clock-regression evidence

## Consequences

**Positive:**
- Consistent capability guarantees across installations
- Aegisy can update capabilities without desktop releases
- Evaluated suitability roles improve model selection
- Entitlement and routing enforcement possible
- Deprecation and migration paths supported
- Security through signed catalog and key rotation

**Negative:**
- Network dependency for catalog updates (mitigated by caching)
- Aegisy must maintain catalog accuracy (operational burden)
- Local/arbitrary models require separate policy (see ADR 0005)
- Temporary outages limit new model selection (existing cached models continue working)

**Neutral:**
- Catalog becomes critical infrastructure requiring monitoring and SLAs
- Provider capability changes require Aegisy catalog updates
- Model profiles add complexity but improve role-specific selection
