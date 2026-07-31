# ADR 008: Local Model Support

- Status: Deferred
- Accountable owner: Product
- Consulted owners: Model Runtime, Product Security, Privacy, Support
- Due gate: Cloud model maturity evidence; dedicated local-provider OpenSpec before implementation

## Context

Users have requested support for local and offline models (Ollama, LM Studio, and other OpenAI-compatible endpoints) to enable:

- Privacy-sensitive work without cloud transmission
- Offline development environments
- Cost reduction for high-volume usage
- Custom or fine-tuned model deployment
- Regulatory compliance requirements

However, local models introduce challenges around capability verification, security boundaries, quality assurance, and support burden that cloud-managed models do not present.

## Decision Options

### Option 1: No Local Support

Maintain cloud-only model routing through Aegisy's signed catalog. All models are verified, capability-declared, and entitlement-controlled.

**Advantages:**
- Consistent quality and capability guarantees
- Simplified support and troubleshooting
- No unverified execution paths
- Clear billing and usage attribution

**Disadvantages:**
- Cannot serve offline or air-gapped environments
- Privacy-sensitive users blocked
- No cost optimization for high-volume users
- Competitive disadvantage vs. local-first tools

### Option 2: Ollama Integration

Direct integration with Ollama's API and model management.

**Advantages:**
- Popular local runtime with active community
- Model download and version management included
- Consistent API surface

**Disadvantages:**
- Tight coupling to one vendor
- Ollama-specific capability detection
- Limited to Ollama's supported models
- Additional dependency to maintain

### Option 3: Generic OpenAI-Compatible Endpoint

Support any localhost endpoint implementing OpenAI's Chat Completions API.

**Advantages:**
- Works with Ollama, LM Studio, vLLM, LocalAI, and others
- No vendor lock-in
- User controls model selection and hosting

**Disadvantages:**
- Cannot verify model identity or capabilities
- Inconsistent tool-calling support
- No standardized context limits or tokenization
- Quality varies dramatically by endpoint

### Option 4: Custom Adapter per Runtime

Dedicated adapters for Ollama, LM Studio, and other major local runtimes.

**Advantages:**
- Runtime-specific capability negotiation
- Better error handling and diagnostics
- Can leverage runtime-specific features

**Disadvantages:**
- High maintenance burden
- Adapter proliferation
- Still cannot verify model quality
- Delayed support for new runtimes

## Performance and Capability Considerations

Local models present significant capability gaps compared to cloud models:

- **Context limits:** Most local models support 4K-32K tokens vs. 200K+ for cloud models
- **Tool calling:** Inconsistent or absent structured tool support
- **Reasoning quality:** Local models often fail multi-step planning and code generation
- **Streaming:** Variable cancellation and backpressure support
- **Multimodal:** Limited image understanding, no video/audio
- **Speed:** Inference latency depends on user hardware

These limitations mean local models cannot reliably execute the Agent workbench's core workflows (structured patches, Git operations, multi-file edits, background tasks) without significant degradation.

## Support Considerations

Supporting local models creates substantial support burden:

- Users blame Aegisy for local model quality issues
- Debugging requires reproducing user's exact model/hardware/runtime combination
- No telemetry or error attribution for local execution
- Cannot distinguish Aegisy bugs from model capability gaps
- Documentation must cover every supported runtime's installation and configuration

## Current Decision: Deferred

Local model support is **deferred** until cloud model integration reaches production maturity and the following evidence is available:

### Required Evidence Before Implementation

1. **Cloud model baseline:** Aegisy Agent workbench demonstrates reliable multi-turn coding workflows with cloud models in production use
2. **Capability framework:** AAP capability negotiation can gracefully degrade features when local models lack required capabilities
3. **Quality gates:** Automated evaluation suite can measure local model suitability for specific workflows
4. **Security architecture:** Localhost-only binding, DNS rebinding defense, credential isolation, and request bounds are implemented and tested
5. **Support plan:** Clear documentation distinguishing cloud vs. local model capabilities, troubleshooting guides, and support escalation criteria
6. **User research:** Evidence that local model demand justifies the maintenance and support cost

### Implementation Constraints When Approved

If local support is approved after evidence gates pass:

- Local profiles are **opt-in** and **explicitly unmanaged**
- UI clearly indicates unverified/unsupported status
- Initial support is **Chat/read-only only**
- Work mode tools, structured patches, Git operations, background jobs, and model switching remain disabled until each capability is proven for the specific local runtime
- No automatic fallback from cloud to local models
- Local endpoints cannot receive Aegisy credentials
- Excluded from Aegisy entitlement, billing, and usage claims
- Localhost/Unix-socket binding only; remote LAN endpoints require separate security decision

## Consequences

### Immediate Consequences (Deferred Status)

- Aegisy focuses engineering effort on cloud model reliability and core Agent workflows
- Users requiring local models must use alternative tools or wait for future support
- Competitive positioning emphasizes quality and reliability over feature breadth
- Support burden remains manageable during initial product maturity phase

### Future Consequences (If Approved)

- Local model users accept degraded capabilities and unsupported status
- Clear capability boundaries prevent false expectations
- Security architecture prevents local endpoints from becoming attack vectors
- Support can distinguish Aegisy issues from local model limitations
- Gradual capability expansion as local models improve

## Related Decisions

- [ADR 0004](0004-model-catalog-trust-and-authority.md): Model catalog schema and authority
- [ADR 0005](0005-local-model-provider-policy.md): Detailed local provider policy (superseded by this ADR's broader scope)
