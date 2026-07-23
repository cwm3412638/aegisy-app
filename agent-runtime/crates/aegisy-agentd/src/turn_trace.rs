//! Content-free, source-qualified metadata for one local Agent turn.
//!
//! This is an internal observability contract.  It records identities,
//! classifications, timing, counts, and hashes only; it does not carry a
//! prompt, file body, path, command line, terminal output, diff, or secret.
//! A trace is evidence about a turn, not permission to run one.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

use crate::usage_authority::UsageAuthorityReport;

pub const LEGACY_SCHEMA_VERSION: &str = "turn-trace/0.1";
pub const V0_2_SCHEMA_VERSION: &str = "turn-trace/0.2";
pub const V0_3_SCHEMA_VERSION: &str = "turn-trace/0.3";
pub const V0_4_SCHEMA_VERSION: &str = "turn-trace/0.4";
pub const SCHEMA_VERSION: &str = "turn-trace/0.5";
pub(crate) const MAX_DURABLE_EVENT_BYTES: usize = 72 * 1024;
pub(crate) const TURN_TRACE_RECORDED_SCHEMA_VERSION: &str = "turn.trace.recorded/0.1";

const MAX_ID_BYTES: usize = 128;
const MAX_LABEL_BYTES: usize = 96;
const MAX_EVENTS: usize = 512;
const MAX_SOURCE_BYTES: u64 = 16 * 1024 * 1024;
const MAX_CONTEXT_ITEMS: u32 = 16_384;
const MAX_CHANGED_FILES: u32 = 100_000;
const MAX_TEST_CASES: u32 = 100_000;
const MAX_DOMAIN_OBSERVATIONS: u32 = 100_000;
const MAX_TOOL_ACTIONS: usize = 128;
const MAX_TOOL_DURATION_MS: u64 = i64::MAX as u64;
const MAX_RAW_PROVIDER_THREAD_ID_BYTES: usize = 4 * 1024;

pub(crate) fn durable_record_payload(
    trace: &TurnTrace,
    trace_identity: &str,
    state: &str,
    recorded_at_ms: u64,
) -> serde_json::Value {
    serde_json::json!({
        "schema_version": TURN_TRACE_RECORDED_SCHEMA_VERSION,
        "trace": trace,
        "trace_identity": trace_identity,
        "state": state,
        "recorded_at_ms": recorded_at_ms,
        "content_included": false,
        "execution_authority": false
    })
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TurnTraceError {
    pub code: &'static str,
    pub message: &'static str,
}

impl TurnTraceError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

fn error(code: &'static str, message: &'static str) -> TurnTraceError {
    TurnTraceError::new(code, message)
}

/// The authority level of an observation.  Unknown values deliberately carry
/// no value or identity so they cannot be mistaken for measured data.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum AuthorityLabel {
    Observed,
    CatalogDerived,
    Estimated,
    Stale,
    Unknown,
}

/// A bounded producer class.  This is intentionally an enum rather than a
/// caller-provided URL, path, process name, or provider endpoint.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum EvidenceSource {
    Runtime,
    ModelCatalog,
    Provider,
    ContextBuilder,
    ToolRuntime,
    ApprovalAuthority,
    UsageProvider,
    Workspace,
    Git,
    TestRunner,
    Aap,
    LocalRuntime,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EvidenceRef {
    pub authority: AuthorityLabel,
    pub source: EvidenceSource,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub observed_at_ms: Option<u64>,
}

impl EvidenceRef {
    pub fn validate(&self) -> Result<(), TurnTraceError> {
        match self.authority {
            AuthorityLabel::Unknown => {
                if self.identity.is_some() || self.observed_at_ms.is_some() {
                    return Err(error(
                        "turn-trace-unknown-evidence-has-value",
                        "unknown evidence cannot carry an identity or timestamp",
                    ));
                }
            }
            AuthorityLabel::Observed
            | AuthorityLabel::CatalogDerived
            | AuthorityLabel::Estimated
            | AuthorityLabel::Stale => {
                let identity = self.identity.as_deref().ok_or_else(|| {
                    error(
                        "turn-trace-evidence-identity-missing",
                        "qualified evidence requires an identity",
                    )
                })?;
                validate_identity(identity, "evidence identity")?;
                if self.observed_at_ms.is_none() {
                    return Err(error(
                        "turn-trace-evidence-time-missing",
                        "qualified evidence requires an observation time",
                    ));
                }
                if self.authority == AuthorityLabel::CatalogDerived
                    && self.source != EvidenceSource::ModelCatalog
                {
                    return Err(error(
                        "turn-trace-catalog-source-invalid",
                        "catalog-derived evidence must come from the model catalog",
                    ));
                }
            }
        }
        Ok(())
    }
}

/// Counts and hashes describing redaction, with no raw content field.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RedactionSummary {
    pub content_included: bool,
    pub raw_bytes: u64,
    pub retained_bytes: u64,
    pub redacted_fields: u32,
    pub omitted_fields: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub content_identity: Option<String>,
}

impl RedactionSummary {
    pub fn metadata_only() -> Self {
        Self {
            content_included: false,
            raw_bytes: 0,
            retained_bytes: 0,
            redacted_fields: 0,
            omitted_fields: 0,
            content_identity: None,
        }
    }

    fn validate(&self) -> Result<(), TurnTraceError> {
        if self.content_included {
            return Err(error(
                "turn-trace-content-included",
                "local turn traces cannot include content",
            ));
        }
        if self.retained_bytes > self.raw_bytes || self.raw_bytes > MAX_SOURCE_BYTES {
            return Err(error(
                "turn-trace-redaction-bounds",
                "redaction byte counts exceed the trace bounds",
            ));
        }
        if let Some(identity) = &self.content_identity {
            validate_hash_identity(identity, "redacted content identity")?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TraceBinding {
    pub session_id: String,
    pub turn_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub project_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub environment_identity: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum SessionMode {
    Chat,
    Work,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum TurnKind {
    Conversation,
    ReadOnlyInspection,
    Mutation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum TurnAccess {
    NonMutating,
    ReadOnly,
    Mutation,
}

impl TraceBinding {
    fn validate(&self) -> Result<(), TurnTraceError> {
        validate_identity(&self.session_id, "session identity")?;
        validate_identity(&self.turn_id, "turn identity")?;
        if let Some(value) = &self.project_id {
            validate_identity(value, "project identity")?;
        }
        if let Some(value) = &self.environment_identity {
            validate_environment_identity(value)?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum RuntimeState {
    Starting,
    Ready,
    Degraded,
    Unavailable,
    Restarting,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ModelRole {
    Agent,
    Plan,
    Apply,
    Review,
    Utility,
    Embedding,
    Rerank,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ModelReason {
    Profile,
    Retry,
    Fallback,
    UserSelection,
    CapabilityMatch,
    Continuation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ToolState {
    Requested,
    Started,
    Completed,
    Failed,
    Declined,
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ToolProviderStatus {
    InProgress,
    Completed,
    Failed,
    Declined,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ToolSource {
    Agent,
    UserShell,
    UnifiedExecStartup,
    UnifiedExecInteraction,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
#[serde(deny_unknown_fields)]
pub enum ToolTimelineBinding {
    NotPersisted,
    Persisted {
        item_identity: String,
        payload_identity: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ApprovalDecision {
    Requested,
    Allowed,
    Denied,
    Expired,
    NotRequired,
}

/// The only approval-policy value currently asserted by Aegisy's read-only
/// Codex adapter. This observation describes Runtime configuration; it is not
/// an Approval request or decision.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum RuntimeApprovalPolicy {
    Never,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ApprovalPolicyReviewer {
    User,
    AutoReview,
    GuardianSubagent,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ApprovalPolicySandbox {
    ReadOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ApprovalPolicyPermissionProfile {
    ReadOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ApprovalDecisionAttribution {
    NoUserDecision,
}

impl RuntimeApprovalPolicy {
    fn as_str(self) -> &'static str {
        match self {
            Self::Never => "never",
        }
    }
}

impl ApprovalPolicyReviewer {
    fn as_str(self) -> &'static str {
        match self {
            Self::User => "user",
            Self::AutoReview => "auto-review",
            Self::GuardianSubagent => "guardian-subagent",
        }
    }
}

impl ApprovalPolicySandbox {
    fn as_str(self) -> &'static str {
        match self {
            Self::ReadOnly => "read-only",
        }
    }
}

impl ApprovalPolicyPermissionProfile {
    fn as_str(self) -> &'static str {
        match self {
            Self::ReadOnly => "read-only",
        }
    }
}

pub fn configured_runtime_approval_policy_identity(
    policy: RuntimeApprovalPolicy,
    sandbox: ApprovalPolicySandbox,
    permission_profile: ApprovalPolicyPermissionProfile,
) -> String {
    framed_hash_identity(
        b"aegisy-runtime-approval-policy-configured/0.1\0",
        &[
            policy.as_str(),
            sandbox.as_str(),
            permission_profile.as_str(),
        ],
    )
}

/// Converts a backend thread ID into a content-free Trace identity. The raw
/// value is never returned or retained by this contract.
pub fn provider_thread_identity(raw_backend_thread_id: &str) -> Result<String, TurnTraceError> {
    if raw_backend_thread_id.is_empty()
        || raw_backend_thread_id.len() > MAX_RAW_PROVIDER_THREAD_ID_BYTES
    {
        return Err(error(
            "turn-trace-provider-thread-identity-invalid",
            "Provider thread identity input is empty or exceeds its bound",
        ));
    }
    Ok(framed_hash_identity(
        b"aegisy-provider-thread/0.1\0",
        &[raw_backend_thread_id],
    ))
}

pub fn effective_runtime_approval_policy_identity(
    provider_thread_identity: &str,
    policy: RuntimeApprovalPolicy,
    reviewer: ApprovalPolicyReviewer,
    sandbox: ApprovalPolicySandbox,
    permission_profile: ApprovalPolicyPermissionProfile,
) -> String {
    framed_hash_identity(
        b"aegisy-runtime-approval-policy-effective/0.1\0",
        &[
            provider_thread_identity,
            policy.as_str(),
            reviewer.as_str(),
            sandbox.as_str(),
            permission_profile.as_str(),
        ],
    )
}

pub fn runtime_approval_policy_authority_identity(
    binding: &TraceBinding,
    runtime_identity: &str,
    adapter_identity: &str,
    runtime_version: &str,
    provider_thread_identity: &str,
    configured_policy_identity: &str,
    effective_policy_identity: &str,
) -> String {
    let project_presence = if binding.project_id.is_some() {
        "project-present"
    } else {
        "project-absent"
    };
    let environment_presence = if binding.environment_identity.is_some() {
        "environment-present"
    } else {
        "environment-absent"
    };
    framed_hash_identity(
        b"aegisy-runtime-approval-policy-authority/0.1\0",
        &[
            &binding.session_id,
            project_presence,
            binding.project_id.as_deref().unwrap_or(""),
            environment_presence,
            binding.environment_identity.as_deref().unwrap_or(""),
            runtime_identity,
            adapter_identity,
            runtime_version,
            provider_thread_identity,
            configured_policy_identity,
            effective_policy_identity,
        ],
    )
}

fn framed_hash_identity(domain: &[u8], fields: &[&str]) -> String {
    let mut digest = Sha256::new();
    digest.update(domain);
    for field in fields {
        digest.update((field.len() as u64).to_be_bytes());
        digest.update(field.as_bytes());
    }
    format!("sha256:{:x}", digest.finalize())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageMetric {
    Token,
    Context,
    Cost,
    Reasoning,
    Latency,
}

/// Codex reports this snapshot for the provider thread, not for one Aegisy
/// turn attempt. The closed enum prevents a future producer from silently
/// narrowing the accounting scope.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageReportScope {
    ProviderThread,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageAccounting {
    AbsoluteSnapshot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageAttribution {
    Unavailable,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UsageValue {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub input_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cached_input_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub total_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context_window_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reasoning_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cost_micros: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latency_ms: Option<u64>,
}

impl UsageValue {
    fn validate(&self, metric: UsageMetric) -> Result<(), TurnTraceError> {
        let has_token = self.input_tokens.is_some()
            || self.output_tokens.is_some()
            || self.cached_input_tokens.is_some()
            || self.total_tokens.is_some();
        let has_context = self.context_tokens.is_some() || self.context_window_tokens.is_some();
        let has_reasoning = self.reasoning_tokens.is_some();
        let has_cost = self.cost_micros.is_some();
        let has_latency = self.latency_ms.is_some();
        let has_metric = match metric {
            UsageMetric::Token => has_token,
            UsageMetric::Context => has_context,
            UsageMetric::Cost => has_cost,
            UsageMetric::Reasoning => has_reasoning,
            UsageMetric::Latency => has_latency,
        };
        if !has_metric {
            return Err(error(
                "turn-trace-usage-value-missing",
                "usage event does not contain a value for its metric",
            ));
        }
        if let (Some(input), Some(output), Some(total)) =
            (self.input_tokens, self.output_tokens, self.total_tokens)
        {
            if input.checked_add(output) != Some(total) {
                return Err(error(
                    "turn-trace-token-total-invalid",
                    "token total does not equal input plus output",
                ));
            }
        }
        if let (Some(used), Some(window)) = (self.context_tokens, self.context_window_tokens) {
            if used > window {
                return Err(error(
                    "turn-trace-context-window-invalid",
                    "context usage exceeds its window",
                ));
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ChangeState {
    Proposed,
    Applied,
    Reverted,
    Blocked,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum TestOutcome {
    Passed,
    Failed,
    Skipped,
    Blocked,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ErrorClass {
    Runtime,
    Protocol,
    Provider,
    Transport,
    Timeout,
    Sandbox,
    Policy,
    Tool,
    Storage,
    Workspace,
    Git,
    Budget,
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum TerminalState {
    /// The provider/runtime turn lifecycle ended normally. This does not by
    /// itself claim task success, workspace mutation, or passed verification.
    Completed,
    Failed,
    Cancelled,
    Interrupted,
}

/// The terminal state of one completion domain. Applicability and observation
/// are explicit alternatives so absence is never interpreted as proof.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "kebab-case")]
#[serde(deny_unknown_fields)]
pub enum CompletionDomain {
    NotApplicable {
        evidence: EvidenceRef,
    },
    Applicable {
        evidence: EvidenceRef,
    },
    Unknown {
        evidence: EvidenceRef,
    },
    Observed {
        identity: String,
        observation_count: u32,
        evidence: EvidenceRef,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CompletionDomainKind {
    WorkspaceChange,
    GitChange,
    Verification,
}

impl CompletionDomain {
    fn validate(&self, kind: CompletionDomainKind) -> Result<(), TurnTraceError> {
        let allowed_sources: &[EvidenceSource] = match kind {
            CompletionDomainKind::WorkspaceChange => &[
                EvidenceSource::Workspace,
                EvidenceSource::Runtime,
                EvidenceSource::LocalRuntime,
                EvidenceSource::Aap,
            ],
            CompletionDomainKind::GitChange => &[
                EvidenceSource::Git,
                EvidenceSource::Runtime,
                EvidenceSource::LocalRuntime,
                EvidenceSource::Aap,
            ],
            CompletionDomainKind::Verification => &[
                EvidenceSource::TestRunner,
                EvidenceSource::ToolRuntime,
                EvidenceSource::Runtime,
                EvidenceSource::LocalRuntime,
                EvidenceSource::Aap,
            ],
        };
        let evidence = match self {
            Self::NotApplicable { evidence } | Self::Applicable { evidence } => {
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-domain-applicability-not-observed",
                        "applicability decisions require observed evidence",
                    ));
                }
                evidence
            }
            Self::Unknown { evidence } => {
                if evidence.authority != AuthorityLabel::Unknown {
                    return Err(error(
                        "turn-trace-domain-unknown-authority-invalid",
                        "unknown completion domains require unknown evidence",
                    ));
                }
                evidence
            }
            Self::Observed {
                identity,
                observation_count,
                evidence,
            } => {
                validate_hash_identity(identity, "completion domain identity")?;
                if *observation_count == 0 || *observation_count > MAX_DOMAIN_OBSERVATIONS {
                    return Err(error(
                        "turn-trace-domain-observation-count-invalid",
                        "observed completion domains require a bounded nonzero count",
                    ));
                }
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-domain-observation-authority-invalid",
                        "observed completion domains require observed evidence",
                    ));
                }
                evidence
            }
        };
        validate_source(evidence, allowed_sources)?;
        evidence.validate()
    }

    fn is_not_applicable(&self) -> bool {
        matches!(self, Self::NotApplicable { .. })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CompletionEvidence {
    /// Binds terminal domain classifications to the immutable start intent.
    pub intent_identity: String,
    pub workspace_change: CompletionDomain,
    pub git_change: CompletionDomain,
    pub verification: CompletionDomain,
}

impl CompletionEvidence {
    fn validate(&self) -> Result<(), TurnTraceError> {
        validate_hash_identity(&self.intent_identity, "completion intent identity")?;
        self.workspace_change
            .validate(CompletionDomainKind::WorkspaceChange)?;
        self.git_change.validate(CompletionDomainKind::GitChange)?;
        self.verification
            .validate(CompletionDomainKind::Verification)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TerminalEvidence {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub workspace_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub git_state_identity: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub verification_identity: Option<String>,
    pub observed_verification_count: u32,
    pub evidence: EvidenceRef,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub completion: Option<CompletionEvidence>,
}

impl TerminalEvidence {
    fn validate(&self, schema_version: &str, state: TerminalState) -> Result<(), TurnTraceError> {
        if let Some(value) = &self.workspace_identity {
            validate_hash_identity(value, "workspace evidence identity")?;
        }
        if let Some(value) = &self.git_state_identity {
            validate_hash_identity(value, "Git evidence identity")?;
        }
        if let Some(value) = &self.verification_identity {
            validate_hash_identity(value, "verification evidence identity")?;
        }
        self.evidence.validate()?;
        match schema_version {
            LEGACY_SCHEMA_VERSION => {
                if self.completion.is_some() {
                    return Err(error(
                        "turn-trace-v0-1-completion-domain-invalid",
                        "turn-trace/0.1 cannot contain completion domains",
                    ));
                }
                if state == TerminalState::Completed
                    && (self.workspace_identity.is_none()
                        || self.git_state_identity.is_none()
                        || self.verification_identity.is_none()
                        || self.observed_verification_count == 0)
                {
                    return Err(error(
                        "turn-trace-completion-evidence-missing",
                        "successful legacy completion requires workspace, Git, and verification evidence",
                    ));
                }
            }
            V0_2_SCHEMA_VERSION | V0_3_SCHEMA_VERSION | V0_4_SCHEMA_VERSION | SCHEMA_VERSION => {
                if self.workspace_identity.is_some()
                    || self.git_state_identity.is_some()
                    || self.verification_identity.is_some()
                    || self.observed_verification_count != 0
                {
                    return Err(error(
                        "turn-trace-v0-2-legacy-completion-invalid",
                        "current turn traces cannot contain legacy completion evidence",
                    ));
                }
                match (state, &self.completion) {
                    (TerminalState::Completed, Some(completion)) => completion.validate()?,
                    (TerminalState::Completed, None) => {
                        return Err(error(
                            "turn-trace-completion-domains-missing",
                            "completed current turn traces require explicit completion domains",
                        ));
                    }
                    (_, None) => {}
                    (_, Some(_)) => {
                        return Err(error(
                            "turn-trace-noncompleted-domains-invalid",
                            "failed, cancelled, or interrupted turns cannot claim completion domains",
                        ));
                    }
                }
            }
            _ => {
                return Err(error(
                    "turn-trace-schema-invalid",
                    "unsupported turn trace schema version",
                ));
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
#[serde(deny_unknown_fields)]
pub enum TracePayload {
    Intent {
        session_mode: SessionMode,
        turn_kind: TurnKind,
        access: TurnAccess,
        intent_identity: String,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Runtime {
        runtime_identity: String,
        adapter_identity: String,
        version: String,
        state: RuntimeState,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    RuntimeApprovalPolicy {
        runtime_identity: String,
        adapter_identity: String,
        runtime_version: String,
        provider_thread_identity: String,
        policy: RuntimeApprovalPolicy,
        reviewer: ApprovalPolicyReviewer,
        sandbox: ApprovalPolicySandbox,
        permission_profile: ApprovalPolicyPermissionProfile,
        configured_policy_identity: String,
        effective_policy_identity: String,
        policy_authority_identity: String,
        decision_attribution: ApprovalDecisionAttribution,
        user_decision_observed: bool,
        execution_authority: bool,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Model {
        role: ModelRole,
        model_identity: String,
        provider_identity: String,
        reason: ModelReason,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        reason_identity: Option<String>,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Context {
        manifest_identity: String,
        item_count: u32,
        included_items: u32,
        excluded_items: u32,
        bytes: u64,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Tool {
        tool_identity: String,
        action_identity: String,
        state: ToolState,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        provider_status: Option<ToolProviderStatus>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        source: Option<ToolSource>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        input_identity: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        output_identity: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        item_binding: Option<ToolTimelineBinding>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        duration_ms: Option<u64>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        exit_code: Option<i64>,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Approval {
        approval_identity: String,
        requirement_identity: String,
        authority_identity: String,
        decision: ApprovalDecision,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Usage {
        usage_identity: String,
        attempt_identity: String,
        metric: UsageMetric,
        value: UsageValue,
        retry: bool,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    UsageReport {
        report_identity: String,
        persisted_item_id: String,
        scope: UsageReportScope,
        accounting: UsageAccounting,
        attempt_attribution: UsageAttribution,
        retry_attribution: UsageAttribution,
        report: UsageAuthorityReport,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Change {
        change_identity: String,
        state: ChangeState,
        changed_file_count: u32,
        changed_bytes: u64,
        workspace_identity: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        git_state_identity: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        patch_identity: Option<String>,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Test {
        test_identity: String,
        runner_identity: String,
        outcome: TestOutcome,
        case_count: u32,
        command_identity: String,
        verification_identity: String,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Error {
        error_identity: String,
        stable_class: ErrorClass,
        source_class: String,
        retryable: bool,
        evidence: EvidenceRef,
        redaction: RedactionSummary,
    },
    Terminal {
        state: TerminalState,
        evidence: TerminalEvidence,
        redaction: RedactionSummary,
    },
}

impl TracePayload {
    fn validate(&self, schema_version: &str) -> Result<(), TurnTraceError> {
        match self {
            Self::Intent {
                session_mode,
                turn_kind,
                access,
                intent_identity,
                evidence,
                redaction,
            } => {
                if !matches!(
                    schema_version,
                    V0_2_SCHEMA_VERSION
                        | V0_3_SCHEMA_VERSION
                        | V0_4_SCHEMA_VERSION
                        | SCHEMA_VERSION
                ) {
                    return Err(error(
                        "turn-trace-v0-1-intent-invalid",
                        "turn-trace/0.1 cannot contain an intent event",
                    ));
                }
                validate_intent(*session_mode, *turn_kind, *access)?;
                validate_hash_identity(intent_identity, "turn intent identity")?;
                validate_source(evidence, &[EvidenceSource::Runtime])?;
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-intent-authority-invalid",
                        "turn intent requires Runtime-observed evidence",
                    ));
                }
                validate_common(evidence, redaction)?;
                if *redaction != RedactionSummary::metadata_only() {
                    return Err(error(
                        "turn-trace-intent-redaction-invalid",
                        "turn intent must use an empty metadata-only redaction summary",
                    ));
                }
            }
            Self::Runtime {
                runtime_identity,
                adapter_identity,
                version,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(runtime_identity, "runtime identity")?;
                validate_identity(adapter_identity, "adapter identity")?;
                validate_label(version, "runtime version")?;
                validate_source(
                    evidence,
                    &[EvidenceSource::Runtime, EvidenceSource::LocalRuntime],
                )?;
                validate_common(evidence, redaction)?;
            }
            Self::RuntimeApprovalPolicy {
                runtime_identity,
                adapter_identity,
                runtime_version,
                provider_thread_identity,
                policy,
                reviewer,
                sandbox,
                permission_profile,
                configured_policy_identity,
                effective_policy_identity,
                policy_authority_identity,
                decision_attribution,
                user_decision_observed,
                execution_authority,
                evidence,
                redaction,
            } => {
                if schema_version != SCHEMA_VERSION {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-version-invalid",
                        "Runtime approval-policy observations require turn-trace/0.5",
                    ));
                }
                validate_identity(runtime_identity, "approval-policy runtime identity")?;
                validate_identity(adapter_identity, "approval-policy adapter identity")?;
                validate_label(runtime_version, "approval-policy runtime version")?;
                validate_hash_identity(
                    provider_thread_identity,
                    "approval-policy Provider thread identity",
                )?;
                validate_hash_identity(
                    configured_policy_identity,
                    "configured approval-policy identity",
                )?;
                validate_hash_identity(
                    effective_policy_identity,
                    "effective approval-policy identity",
                )?;
                validate_hash_identity(
                    policy_authority_identity,
                    "Runtime approval-policy authority identity",
                )?;
                let expected_configured = configured_runtime_approval_policy_identity(
                    *policy,
                    *sandbox,
                    *permission_profile,
                );
                if *configured_policy_identity != expected_configured {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-configured-identity-mismatch",
                        "configured approval-policy identity does not bind the closed policy",
                    ));
                }
                let expected_effective = effective_runtime_approval_policy_identity(
                    provider_thread_identity,
                    *policy,
                    *reviewer,
                    *sandbox,
                    *permission_profile,
                );
                if *effective_policy_identity != expected_effective {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-effective-identity-mismatch",
                        "effective approval-policy identity does not bind the observed response",
                    ));
                }
                if *decision_attribution != ApprovalDecisionAttribution::NoUserDecision {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-decision-attribution-invalid",
                        "Runtime approval-policy observations cannot carry user-decision attribution",
                    ));
                }
                if *user_decision_observed {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-user-decision-invalid",
                        "Runtime approval-policy observations cannot claim a user decision",
                    ));
                }
                if *execution_authority {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-execution-authority-invalid",
                        "Runtime approval-policy observations cannot grant execution authority",
                    ));
                }
                validate_source(evidence, &[EvidenceSource::Runtime])?;
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-authority-invalid",
                        "Runtime approval-policy observations require Runtime-observed evidence",
                    ));
                }
                if evidence.identity.as_deref() != Some(policy_authority_identity.as_str()) {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-evidence-mismatch",
                        "Runtime approval-policy evidence must bind the policy authority identity",
                    ));
                }
                validate_common(evidence, redaction)?;
                if *redaction != RedactionSummary::metadata_only() {
                    return Err(error(
                        "turn-trace-runtime-approval-policy-redaction-invalid",
                        "Runtime approval-policy observations must be content-free metadata",
                    ));
                }
            }
            Self::Model {
                model_identity,
                provider_identity,
                reason_identity,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(model_identity, "model identity")?;
                validate_identity(provider_identity, "provider identity")?;
                if let Some(value) = reason_identity {
                    validate_identity(value, "model reason identity")?;
                }
                validate_source(
                    evidence,
                    &[
                        EvidenceSource::ModelCatalog,
                        EvidenceSource::Provider,
                        EvidenceSource::Runtime,
                    ],
                )?;
                validate_common(evidence, redaction)?;
            }
            Self::Context {
                manifest_identity,
                item_count,
                included_items,
                excluded_items,
                bytes,
                evidence,
                redaction,
            } => {
                validate_hash_identity(manifest_identity, "context manifest identity")?;
                if *item_count > MAX_CONTEXT_ITEMS
                    || included_items.saturating_add(*excluded_items) > *item_count
                    || *bytes > MAX_SOURCE_BYTES
                {
                    return Err(error(
                        "turn-trace-context-bounds",
                        "context counts or bytes exceed trace bounds",
                    ));
                }
                validate_source(evidence, &[EvidenceSource::ContextBuilder])?;
                validate_common(evidence, redaction)?;
            }
            Self::Tool {
                tool_identity,
                action_identity,
                state,
                provider_status,
                source,
                input_identity,
                output_identity,
                item_binding,
                duration_ms,
                exit_code,
                evidence,
                redaction,
            } => {
                if !matches!(schema_version, V0_4_SCHEMA_VERSION | SCHEMA_VERSION) {
                    if provider_status.is_some()
                        || source.is_some()
                        || item_binding.is_some()
                        || duration_ms.is_some()
                        || exit_code.is_some()
                        || *state == ToolState::Declined
                    {
                        return Err(error(
                            "turn-trace-tool-version-invalid",
                            "turn-trace/0.4 Tool lifecycle metadata cannot appear in an older trace",
                        ));
                    }
                    validate_identity(tool_identity, "tool identity")?;
                    validate_identity(action_identity, "tool action identity")?;
                    validate_optional_identity(input_identity, "tool input identity")?;
                    validate_optional_identity(output_identity, "tool output identity")?;
                    validate_source(evidence, &[EvidenceSource::ToolRuntime])?;
                    validate_common(evidence, redaction)?;
                    return Ok(());
                }

                validate_hash_identity(tool_identity, "tool identity")?;
                validate_hash_identity(action_identity, "tool action identity")?;
                let input_identity = input_identity.as_deref().ok_or_else(|| {
                    error(
                        "turn-trace-tool-input-identity-missing",
                        "turn-trace/0.4 Tool events require an input identity",
                    )
                })?;
                validate_hash_identity(input_identity, "tool input identity")?;
                validate_source(evidence, &[EvidenceSource::ToolRuntime])?;
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-tool-authority-invalid",
                        "turn-trace/0.4 Tool lifecycle evidence must be provider-observed",
                    ));
                }
                validate_common(evidence, redaction)?;
                if *redaction != RedactionSummary::metadata_only() {
                    return Err(error(
                        "turn-trace-tool-redaction-invalid",
                        "turn-trace/0.4 Tool lifecycle events must be metadata-only",
                    ));
                }

                let provider_status = provider_status.ok_or_else(|| {
                    error(
                        "turn-trace-tool-provider-status-missing",
                        "turn-trace/0.4 Tool events require a provider status",
                    )
                })?;
                source.ok_or_else(|| {
                    error(
                        "turn-trace-tool-source-missing",
                        "turn-trace/0.4 Tool events require a provider source",
                    )
                })?;
                let item_binding = item_binding.as_ref().ok_or_else(|| {
                    error(
                        "turn-trace-tool-item-binding-missing",
                        "turn-trace/0.4 Tool events require an explicit Timeline binding state",
                    )
                })?;
                if duration_ms.is_some_and(|value| value > MAX_TOOL_DURATION_MS) {
                    return Err(error(
                        "turn-trace-tool-duration-invalid",
                        "Tool duration exceeds the provider int64 bound",
                    ));
                }
                if exit_code.is_some_and(|value| i32::try_from(value).is_err()) {
                    return Err(error(
                        "turn-trace-tool-exit-code-invalid",
                        "Tool exit code exceeds the provider int32 bound",
                    ));
                }

                match state {
                    ToolState::Started => {
                        if provider_status != ToolProviderStatus::InProgress
                            || output_identity.is_some()
                            || !matches!(item_binding, ToolTimelineBinding::NotPersisted)
                            || duration_ms.is_some()
                            || exit_code.is_some()
                        {
                            return Err(error(
                                "turn-trace-tool-started-invalid",
                                "a started Tool must be in progress and not yet persisted or terminal",
                            ));
                        }
                    }
                    ToolState::Completed | ToolState::Failed | ToolState::Declined => {
                        let expected_status = match state {
                            ToolState::Completed => ToolProviderStatus::Completed,
                            ToolState::Failed => ToolProviderStatus::Failed,
                            ToolState::Declined => ToolProviderStatus::Declined,
                            _ => unreachable!("matched terminal Tool state"),
                        };
                        if provider_status != expected_status {
                            return Err(error(
                                "turn-trace-tool-terminal-status-mismatch",
                                "Tool terminal state does not match the provider status",
                            ));
                        }
                        match state {
                            ToolState::Completed if exit_code.is_some_and(|code| code != 0) => {
                                return Err(error(
                                    "turn-trace-tool-exit-code-status-mismatch",
                                    "a completed Tool cannot carry a nonzero exit code",
                                ));
                            }
                            ToolState::Failed if *exit_code == Some(0) => {
                                return Err(error(
                                    "turn-trace-tool-exit-code-status-mismatch",
                                    "a failed Tool cannot carry a successful exit code",
                                ));
                            }
                            ToolState::Declined if duration_ms.is_some() || exit_code.is_some() => {
                                return Err(error(
                                    "turn-trace-tool-declined-execution-invalid",
                                    "a declined Tool cannot claim execution duration or exit status",
                                ));
                            }
                            ToolState::Completed | ToolState::Failed | ToolState::Declined => {}
                            _ => unreachable!("matched terminal Tool state"),
                        }
                        let output_identity = output_identity.as_deref().ok_or_else(|| {
                            error(
                                "turn-trace-tool-output-identity-missing",
                                "terminal Tool events require an output identity",
                            )
                        })?;
                        validate_hash_identity(output_identity, "tool output identity")?;
                        let ToolTimelineBinding::Persisted {
                            item_identity,
                            payload_identity,
                        } = item_binding
                        else {
                            return Err(error(
                                "turn-trace-tool-terminal-item-missing",
                                "terminal Tool events require an exact persisted Timeline item binding",
                            ));
                        };
                        validate_hash_identity(
                            item_identity,
                            "persisted Tool Timeline item identity",
                        )?;
                        validate_hash_identity(
                            payload_identity,
                            "persisted Tool Timeline payload identity",
                        )?;
                    }
                    ToolState::Requested | ToolState::Cancelled => {
                        return Err(error(
                            "turn-trace-tool-state-invalid",
                            "turn-trace/0.4 Codex Tool lifecycle does not infer requested or cancelled states",
                        ));
                    }
                }
            }
            Self::Approval {
                approval_identity,
                requirement_identity,
                authority_identity,
                evidence,
                redaction,
                ..
            } => {
                if schema_version == SCHEMA_VERSION {
                    return Err(error(
                        "turn-trace-v0-5-approval-producer-unavailable",
                        "turn-trace/0.5 cannot record Approval without the durable approval authority producer",
                    ));
                }
                validate_identity(approval_identity, "approval identity")?;
                validate_identity(requirement_identity, "approval requirement identity")?;
                validate_identity(authority_identity, "approval authority identity")?;
                validate_source(evidence, &[EvidenceSource::ApprovalAuthority])?;
                validate_common(evidence, redaction)?;
            }
            Self::Usage {
                usage_identity,
                attempt_identity,
                metric,
                value,
                evidence,
                redaction,
                ..
            } => {
                if matches!(
                    schema_version,
                    V0_3_SCHEMA_VERSION | V0_4_SCHEMA_VERSION | SCHEMA_VERSION
                ) {
                    return Err(error(
                        "turn-trace-v0-3-attempt-usage-invalid",
                        "current usage-report traces cannot contain per-attempt usage without authoritative attempt attribution",
                    ));
                }
                validate_identity(usage_identity, "usage identity")?;
                validate_identity(attempt_identity, "attempt identity")?;
                value.validate(*metric)?;
                validate_source(
                    evidence,
                    &[
                        EvidenceSource::UsageProvider,
                        EvidenceSource::Provider,
                        EvidenceSource::Runtime,
                    ],
                )?;
                validate_common(evidence, redaction)?;
            }
            Self::UsageReport {
                report_identity,
                persisted_item_id,
                scope: UsageReportScope::ProviderThread,
                accounting: UsageAccounting::AbsoluteSnapshot,
                attempt_attribution: UsageAttribution::Unavailable,
                retry_attribution: UsageAttribution::Unavailable,
                report,
                evidence,
                redaction,
            } => {
                if !matches!(
                    schema_version,
                    V0_3_SCHEMA_VERSION | V0_4_SCHEMA_VERSION | SCHEMA_VERSION
                ) {
                    return Err(error(
                        "turn-trace-usage-report-version-invalid",
                        "provider thread usage reports require turn-trace/0.3 or newer",
                    ));
                }
                validate_usage_report_identity(report_identity)?;
                validate_identity(persisted_item_id, "persisted usage Timeline item identity")?;
                report.validate().map_err(|_| {
                    error(
                        "turn-trace-usage-report-invalid",
                        "embedded usage authority report is invalid",
                    )
                })?;
                let expected_identity = report.metadata_identity().map_err(|_| {
                    error(
                        "turn-trace-usage-report-invalid",
                        "embedded usage authority report is invalid",
                    )
                })?;
                if *report_identity != expected_identity {
                    return Err(error(
                        "turn-trace-usage-report-identity-mismatch",
                        "usage report identity does not bind the embedded report",
                    ));
                }
                validate_source(
                    evidence,
                    &[EvidenceSource::Provider, EvidenceSource::UsageProvider],
                )?;
                if evidence.authority != AuthorityLabel::Observed {
                    return Err(error(
                        "turn-trace-usage-report-authority-invalid",
                        "provider thread usage requires observed evidence",
                    ));
                }
                if evidence.identity.as_deref() != Some(report_identity.as_str()) {
                    return Err(error(
                        "turn-trace-usage-report-evidence-mismatch",
                        "usage evidence identity does not bind the embedded report",
                    ));
                }
                validate_common(evidence, redaction)?;
                if *redaction != RedactionSummary::metadata_only() {
                    return Err(error(
                        "turn-trace-usage-report-redaction-invalid",
                        "provider thread usage must use an empty metadata-only redaction summary",
                    ));
                }
            }
            Self::Change {
                change_identity,
                workspace_identity,
                git_state_identity,
                patch_identity,
                changed_file_count,
                changed_bytes,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(change_identity, "change identity")?;
                validate_hash_identity(workspace_identity, "change workspace identity")?;
                validate_optional_hash_identity(git_state_identity, "change Git identity")?;
                validate_optional_hash_identity(patch_identity, "patch identity")?;
                if *changed_file_count > MAX_CHANGED_FILES || *changed_bytes > MAX_SOURCE_BYTES {
                    return Err(error(
                        "turn-trace-change-bounds",
                        "change counts or bytes exceed trace bounds",
                    ));
                }
                validate_source(evidence, &[EvidenceSource::Workspace, EvidenceSource::Git])?;
                validate_common(evidence, redaction)?;
            }
            Self::Test {
                test_identity,
                runner_identity,
                case_count,
                command_identity,
                verification_identity,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(test_identity, "test identity")?;
                validate_identity(runner_identity, "test runner identity")?;
                validate_identity(command_identity, "test command identity")?;
                validate_hash_identity(verification_identity, "test verification identity")?;
                if *case_count > MAX_TEST_CASES {
                    return Err(error(
                        "turn-trace-test-bounds",
                        "test case count exceeds trace bounds",
                    ));
                }
                validate_source(evidence, &[EvidenceSource::TestRunner])?;
                validate_common(evidence, redaction)?;
            }
            Self::Error {
                error_identity,
                source_class,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(error_identity, "error identity")?;
                validate_label(source_class, "error source class")?;
                validate_source(
                    evidence,
                    &[
                        EvidenceSource::Aap,
                        EvidenceSource::Runtime,
                        EvidenceSource::Provider,
                        EvidenceSource::ToolRuntime,
                    ],
                )?;
                validate_common(evidence, redaction)?;
            }
            Self::Terminal {
                evidence,
                redaction,
                state,
            } => {
                evidence.validate(schema_version, *state)?;
                redaction.validate()?;
            }
        }
        Ok(())
    }

    fn is_terminal(&self) -> bool {
        matches!(self, Self::Terminal { .. })
    }
}

fn validate_common(
    evidence: &EvidenceRef,
    redaction: &RedactionSummary,
) -> Result<(), TurnTraceError> {
    evidence.validate()?;
    redaction.validate()
}

fn validate_intent(
    session_mode: SessionMode,
    turn_kind: TurnKind,
    access: TurnAccess,
) -> Result<(), TurnTraceError> {
    if !matches!(
        (session_mode, turn_kind, access),
        (
            SessionMode::Chat,
            TurnKind::Conversation,
            TurnAccess::NonMutating
        ) | (
            SessionMode::Work,
            TurnKind::ReadOnlyInspection,
            TurnAccess::ReadOnly
        ) | (SessionMode::Work, TurnKind::Mutation, TurnAccess::Mutation)
    ) {
        return Err(error(
            "turn-trace-intent-combination-invalid",
            "turn mode, kind, and access do not form a supported intent",
        ));
    }
    Ok(())
}

fn validate_completed_intent(
    intent: (SessionMode, TurnKind, TurnAccess),
    completion: &CompletionEvidence,
    applied_workspace_identities: &[&str],
) -> Result<(), TurnTraceError> {
    match intent {
        (SessionMode::Chat, TurnKind::Conversation, TurnAccess::NonMutating) => {
            if !completion.workspace_change.is_not_applicable()
                || !completion.git_change.is_not_applicable()
                || !completion.verification.is_not_applicable()
            {
                return Err(error(
                    "turn-trace-chat-completion-domain-invalid",
                    "Chat conversation completion domains must be not applicable",
                ));
            }
        }
        (SessionMode::Work, TurnKind::ReadOnlyInspection, TurnAccess::ReadOnly) => {
            if !completion.workspace_change.is_not_applicable()
                || !completion.git_change.is_not_applicable()
            {
                return Err(error(
                    "turn-trace-read-only-completion-domain-invalid",
                    "read-only Work completion cannot claim workspace or Git changes",
                ));
            }
        }
        (SessionMode::Work, TurnKind::Mutation, TurnAccess::Mutation) => {
            let CompletionDomain::Observed { identity, .. } = &completion.workspace_change else {
                return Err(error(
                    "turn-trace-mutation-workspace-observation-missing",
                    "completed mutation requires an observed workspace change",
                ));
            };
            if !applied_workspace_identities
                .iter()
                .any(|workspace_identity| *workspace_identity == identity)
            {
                return Err(error(
                    "turn-trace-mutation-change-evidence-missing",
                    "completed mutation workspace evidence must match an applied change",
                ));
            }
        }
        _ => {
            return Err(error(
                "turn-trace-intent-combination-invalid",
                "turn mode, kind, and access do not form a supported intent",
            ));
        }
    }
    Ok(())
}

fn validate_source(
    evidence: &EvidenceRef,
    allowed: &[EvidenceSource],
) -> Result<(), TurnTraceError> {
    if !allowed.contains(&evidence.source) {
        return Err(error(
            "turn-trace-source-mismatch",
            "event evidence source does not match its metadata kind",
        ));
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TraceEvent {
    pub event_id: String,
    pub sequence: u32,
    pub at_ms: u64,
    pub payload: TracePayload,
}

impl TraceEvent {
    fn validate(&self, schema_version: &str) -> Result<(), TurnTraceError> {
        validate_identity(&self.event_id, "event identity")?;
        if self.sequence == 0 {
            return Err(error(
                "turn-trace-sequence-invalid",
                "event sequence must start at one",
            ));
        }
        self.payload.validate(schema_version)?;
        if let TracePayload::UsageReport {
            report, evidence, ..
        } = &self.payload
        {
            if self.at_ms != report.as_of_ms || evidence.observed_at_ms != Some(report.as_of_ms) {
                return Err(error(
                    "turn-trace-usage-report-time-mismatch",
                    "usage event, report, and observed evidence times must match exactly",
                ));
            }
        }
        if matches!(schema_version, V0_4_SCHEMA_VERSION | SCHEMA_VERSION) {
            if let TracePayload::Tool { evidence, .. } = &self.payload {
                if evidence.observed_at_ms != Some(self.at_ms) {
                    return Err(error(
                        "turn-trace-tool-time-mismatch",
                        "Tool event and provider-observed evidence times must match exactly",
                    ));
                }
            }
        }
        if let TracePayload::RuntimeApprovalPolicy { evidence, .. } = &self.payload {
            if evidence.observed_at_ms != Some(self.at_ms) {
                return Err(error(
                    "turn-trace-runtime-approval-policy-time-mismatch",
                    "Runtime approval-policy event and evidence times must match exactly",
                ));
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TurnTrace {
    pub schema_version: String,
    pub binding: TraceBinding,
    pub events: Vec<TraceEvent>,
}

#[derive(Debug)]
struct ToolLifecycleObservation {
    tool_identity: String,
    input_identity: String,
    source: ToolSource,
    started_at_ms: u64,
    terminal_state: Option<ToolState>,
}

impl TurnTrace {
    pub fn new(binding: TraceBinding) -> Result<Self, TurnTraceError> {
        binding.validate()?;
        Ok(Self {
            schema_version: SCHEMA_VERSION.to_owned(),
            binding,
            events: Vec::new(),
        })
    }

    pub fn new_v0_2(binding: TraceBinding) -> Result<Self, TurnTraceError> {
        binding.validate()?;
        Ok(Self {
            schema_version: V0_2_SCHEMA_VERSION.to_owned(),
            binding,
            events: Vec::new(),
        })
    }

    pub fn new_v0_3(binding: TraceBinding) -> Result<Self, TurnTraceError> {
        binding.validate()?;
        Ok(Self {
            schema_version: V0_3_SCHEMA_VERSION.to_owned(),
            binding,
            events: Vec::new(),
        })
    }

    pub fn new_v0_4(binding: TraceBinding) -> Result<Self, TurnTraceError> {
        binding.validate()?;
        Ok(Self {
            schema_version: V0_4_SCHEMA_VERSION.to_owned(),
            binding,
            events: Vec::new(),
        })
    }

    pub fn new_legacy(binding: TraceBinding) -> Result<Self, TurnTraceError> {
        binding.validate()?;
        Ok(Self {
            schema_version: LEGACY_SCHEMA_VERSION.to_owned(),
            binding,
            events: Vec::new(),
        })
    }

    /// Append an event with an explicit deterministic identity.  Event IDs are
    /// not generated from content or from a process-global counter.
    pub fn append(
        &mut self,
        event_id: String,
        at_ms: u64,
        payload: TracePayload,
    ) -> Result<u32, TurnTraceError> {
        if self.events.len() >= MAX_EVENTS {
            return Err(error(
                "turn-trace-event-limit",
                "turn trace event limit exceeded",
            ));
        }
        if self.events.iter().any(|event| event.payload.is_terminal()) {
            return Err(error(
                "turn-trace-terminal-not-last",
                "no event may be appended after a terminal event",
            ));
        }
        let sequence = self.events.len() as u32 + 1;
        if let Some(previous) = self.events.last() {
            if at_ms < previous.at_ms {
                return Err(error(
                    "turn-trace-time-order-invalid",
                    "event timestamps must be non-decreasing",
                ));
            }
        }
        if self.events.iter().any(|event| event.event_id == event_id) {
            return Err(error(
                "turn-trace-event-duplicate",
                "event identity is already present",
            ));
        }
        let event = TraceEvent {
            event_id,
            sequence,
            at_ms,
            payload,
        };
        event.validate(&self.schema_version)?;
        self.events.push(event);
        Ok(sequence)
    }

    pub fn validate_open(&self) -> Result<(), TurnTraceError> {
        self.validate(false)
    }

    pub fn validate_complete(&self) -> Result<(), TurnTraceError> {
        self.validate(true)
    }

    pub fn validate(&self, require_terminal: bool) -> Result<(), TurnTraceError> {
        if !matches!(
            self.schema_version.as_str(),
            LEGACY_SCHEMA_VERSION
                | V0_2_SCHEMA_VERSION
                | V0_3_SCHEMA_VERSION
                | V0_4_SCHEMA_VERSION
                | SCHEMA_VERSION
        ) {
            return Err(error(
                "turn-trace-schema-invalid",
                "unsupported turn trace schema version",
            ));
        }
        self.binding.validate()?;
        if self.events.len() > MAX_EVENTS {
            return Err(error(
                "turn-trace-event-limit",
                "turn trace event limit exceeded",
            ));
        }
        let mut identities = BTreeSet::new();
        let mut usage_identities = BTreeSet::new();
        let mut terminal_count = 0usize;
        let mut intent_count = 0usize;
        let mut usage_report_count = 0usize;
        let mut runtime_approval_policy_count = 0usize;
        let mut error_seen = false;
        let mut intent = None;
        let mut runtime_binding = None;
        let mut runtime_approval_policy_window_closed = false;
        let mut terminal_state = None;
        let mut tool_lifecycles = BTreeMap::<String, ToolLifecycleObservation>::new();
        let mut applied_workspace_identities = Vec::new();
        let mut previous_time = None;
        for (index, event) in self.events.iter().enumerate() {
            event.validate(&self.schema_version)?;
            let expected_sequence = index as u32 + 1;
            if event.sequence != expected_sequence {
                return Err(error(
                    "turn-trace-sequence-gap",
                    "event sequence must be contiguous",
                ));
            }
            if !identities.insert(&event.event_id) {
                return Err(error(
                    "turn-trace-event-duplicate",
                    "event identities must be unique",
                ));
            }
            if previous_time.is_some_and(|value| event.at_ms < value) {
                return Err(error(
                    "turn-trace-time-order-invalid",
                    "event timestamps must be non-decreasing",
                ));
            }
            previous_time = Some(event.at_ms);
            if event.payload.is_terminal() {
                terminal_count += 1;
                if index + 1 != self.events.len() || terminal_count > 1 {
                    return Err(error(
                        "turn-trace-terminal-invariant",
                        "exactly one terminal event must be last",
                    ));
                }
                if let TracePayload::Terminal { state, .. } = &event.payload {
                    terminal_state = Some(*state);
                }
            }
            match &event.payload {
                TracePayload::Intent {
                    session_mode,
                    turn_kind,
                    access,
                    intent_identity,
                    ..
                } => {
                    intent_count += 1;
                    intent = Some((*session_mode, *turn_kind, *access, intent_identity.as_str()));
                }
                TracePayload::Runtime {
                    runtime_identity,
                    adapter_identity,
                    version,
                    ..
                } => {
                    if runtime_binding.is_some() {
                        return Err(error(
                            "turn-trace-runtime-duplicate",
                            "a turn trace cannot contain more than one Runtime observation",
                        ));
                    }
                    runtime_binding = Some((
                        runtime_identity.as_str(),
                        adapter_identity.as_str(),
                        version.as_str(),
                    ));
                }
                TracePayload::RuntimeApprovalPolicy {
                    runtime_identity,
                    adapter_identity,
                    runtime_version,
                    provider_thread_identity,
                    configured_policy_identity,
                    effective_policy_identity,
                    policy_authority_identity,
                    ..
                } => {
                    runtime_approval_policy_count += 1;
                    if runtime_approval_policy_count > 1 {
                        return Err(error(
                            "turn-trace-runtime-approval-policy-duplicate",
                            "a turn trace can contain at most one Runtime approval-policy observation",
                        ));
                    }
                    if runtime_approval_policy_window_closed {
                        return Err(error(
                            "turn-trace-runtime-approval-policy-order-invalid",
                            "Runtime approval-policy evidence must precede turn work and terminal evidence",
                        ));
                    }
                    let Some((observed_runtime, observed_adapter, observed_version)) =
                        runtime_binding
                    else {
                        return Err(error(
                            "turn-trace-runtime-approval-policy-runtime-missing",
                            "Runtime approval-policy evidence requires an earlier Runtime observation",
                        ));
                    };
                    if observed_runtime != runtime_identity
                        || observed_adapter != adapter_identity
                        || observed_version != runtime_version
                    {
                        return Err(error(
                            "turn-trace-runtime-approval-policy-runtime-mismatch",
                            "Runtime approval-policy evidence must bind the observed Runtime and adapter",
                        ));
                    }
                    let expected_authority = runtime_approval_policy_authority_identity(
                        &self.binding,
                        runtime_identity,
                        adapter_identity,
                        runtime_version,
                        provider_thread_identity,
                        configured_policy_identity,
                        effective_policy_identity,
                    );
                    if *policy_authority_identity != expected_authority {
                        return Err(error(
                            "turn-trace-runtime-approval-policy-authority-identity-mismatch",
                            "approval-policy authority identity does not bind the Trace and Runtime evidence",
                        ));
                    }
                }
                TracePayload::Change {
                    state: ChangeState::Applied,
                    workspace_identity,
                    ..
                } => applied_workspace_identities.push(workspace_identity.as_str()),
                TracePayload::Tool {
                    tool_identity,
                    action_identity,
                    state,
                    source,
                    input_identity,
                    duration_ms,
                    ..
                } if matches!(
                    self.schema_version.as_str(),
                    V0_4_SCHEMA_VERSION | SCHEMA_VERSION
                ) =>
                {
                    if error_seen {
                        return Err(error(
                            "turn-trace-tool-order-invalid",
                            "Tool lifecycle evidence must precede Error and Terminal events",
                        ));
                    }
                    let source = source.expect("validated turn-trace/0.4 Tool source");
                    let input_identity = input_identity
                        .as_ref()
                        .expect("validated turn-trace/0.4 Tool input identity");
                    match state {
                        ToolState::Started => {
                            if let Some(previous) = tool_lifecycles.get(action_identity) {
                                let code = if previous.terminal_state.is_some() {
                                    "turn-trace-tool-started-after-terminal"
                                } else {
                                    "turn-trace-tool-started-duplicate"
                                };
                                return Err(error(
                                    code,
                                    "a Tool action cannot have more than one started observation",
                                ));
                            }
                            if tool_lifecycles.len() >= MAX_TOOL_ACTIONS {
                                return Err(error(
                                    "turn-trace-tool-action-limit",
                                    "turn trace Tool action limit exceeded",
                                ));
                            }
                            tool_lifecycles.insert(
                                action_identity.clone(),
                                ToolLifecycleObservation {
                                    tool_identity: tool_identity.clone(),
                                    input_identity: input_identity.clone(),
                                    source,
                                    started_at_ms: event.at_ms,
                                    terminal_state: None,
                                },
                            );
                        }
                        ToolState::Completed | ToolState::Failed | ToolState::Declined => {
                            let lifecycle = tool_lifecycles.get_mut(action_identity).ok_or_else(|| {
                                error(
                                    "turn-trace-tool-terminal-without-start",
                                    "a Tool terminal observation requires an earlier started observation",
                                )
                            })?;
                            if lifecycle.terminal_state.is_some() {
                                return Err(error(
                                    "turn-trace-tool-terminal-duplicate",
                                    "a Tool action cannot have more than one terminal observation",
                                ));
                            }
                            if lifecycle.tool_identity != *tool_identity
                                || lifecycle.input_identity != *input_identity
                                || lifecycle.source != source
                            {
                                return Err(error(
                                    "turn-trace-tool-lifecycle-binding-mismatch",
                                    "Tool started and terminal observations do not bind the same action metadata",
                                ));
                            }
                            if duration_ms.is_some_and(|duration| {
                                duration > event.at_ms.saturating_sub(lifecycle.started_at_ms)
                            }) {
                                return Err(error(
                                    "turn-trace-tool-duration-interval-invalid",
                                    "Tool duration exceeds its observed lifecycle interval",
                                ));
                            }
                            lifecycle.terminal_state = Some(*state);
                        }
                        ToolState::Requested | ToolState::Cancelled => {
                            unreachable!("validated turn-trace/0.4 Tool state")
                        }
                    }
                }
                TracePayload::Error { .. } => error_seen = true,
                TracePayload::UsageReport { .. } => {
                    usage_report_count += 1;
                    if usage_report_count > 1 {
                        return Err(error(
                            "turn-trace-usage-report-duplicate",
                            "a turn trace can contain at most one provider thread usage snapshot",
                        ));
                    }
                    if error_seen {
                        return Err(error(
                            "turn-trace-usage-report-order-invalid",
                            "provider thread usage must precede error and terminal events",
                        ));
                    }
                }
                _ => {}
            }
            if !matches!(
                &event.payload,
                TracePayload::Intent { .. }
                    | TracePayload::Runtime { .. }
                    | TracePayload::RuntimeApprovalPolicy { .. }
            ) {
                runtime_approval_policy_window_closed = true;
            }
            if let TracePayload::Usage { usage_identity, .. } = &event.payload {
                if !usage_identities.insert(usage_identity) {
                    return Err(error(
                        "turn-trace-usage-duplicate",
                        "usage identity would double count an attempt",
                    ));
                }
            }
        }
        if self.schema_version == SCHEMA_VERSION && runtime_approval_policy_count != 1 {
            return Err(error(
                "turn-trace-runtime-approval-policy-count-invalid",
                "turn-trace/0.5 requires exactly one Runtime approval-policy observation",
            ));
        }
        match self.schema_version.as_str() {
            LEGACY_SCHEMA_VERSION if intent_count != 0 => {
                return Err(error(
                    "turn-trace-v0-1-intent-invalid",
                    "turn-trace/0.1 cannot contain an intent event",
                ));
            }
            V0_2_SCHEMA_VERSION | V0_3_SCHEMA_VERSION | V0_4_SCHEMA_VERSION | SCHEMA_VERSION
                if intent_count != 1 =>
            {
                return Err(error(
                    "turn-trace-intent-count-invalid",
                    "current turn traces require exactly one intent event",
                ));
            }
            _ => {}
        }
        if matches!(
            self.schema_version.as_str(),
            V0_2_SCHEMA_VERSION | V0_3_SCHEMA_VERSION | V0_4_SCHEMA_VERSION | SCHEMA_VERSION
        ) {
            let intent = intent.expect("validated current turn trace intent count");
            if intent.0 == SessionMode::Work && self.binding.project_id.is_none() {
                return Err(error(
                    "turn-trace-work-project-missing",
                    "Work turn intent requires a project binding",
                ));
            }
            if !applied_workspace_identities.is_empty()
                && (intent.0, intent.1, intent.2)
                    != (SessionMode::Work, TurnKind::Mutation, TurnAccess::Mutation)
            {
                return Err(error(
                    "turn-trace-applied-change-intent-invalid",
                    "an applied change requires an explicit Work mutation intent",
                ));
            }
            if let Some(TracePayload::Terminal {
                state: TerminalState::Completed,
                evidence:
                    TerminalEvidence {
                        completion: Some(completion),
                        ..
                    },
                ..
            }) = self.events.last().map(|event| &event.payload)
            {
                if completion.intent_identity != intent.3 {
                    return Err(error(
                        "turn-trace-completion-intent-mismatch",
                        "terminal completion does not bind the turn-start intent identity",
                    ));
                }
                validate_completed_intent(
                    (intent.0, intent.1, intent.2),
                    completion,
                    &applied_workspace_identities,
                )?;
            }
        }
        if matches!(
            self.schema_version.as_str(),
            V0_4_SCHEMA_VERSION | SCHEMA_VERSION
        ) && tool_lifecycles
            .values()
            .any(|lifecycle| lifecycle.terminal_state.is_none())
            && !matches!(
                terminal_state,
                Some(TerminalState::Failed | TerminalState::Interrupted) | None
            )
        {
            return Err(error(
                "turn-trace-tool-incomplete-on-terminal",
                "only failed or interrupted Turns can retain an unterminated Tool action",
            ));
        }
        if require_terminal && terminal_count != 1 {
            return Err(error(
                "turn-trace-terminal-missing",
                "a complete turn trace requires one terminal event",
            ));
        }
        Ok(())
    }

    /// Return a deterministic identity over the validated metadata envelope.
    /// This is a trace identity, not a digest of prompt or repository content.
    pub fn metadata_identity(&self) -> Result<String, TurnTraceError> {
        self.validate_open()?;
        let bytes = serde_json::to_vec(self).map_err(|_| {
            error(
                "turn-trace-serialization-failed",
                "validated trace could not be serialized",
            )
        })?;
        let digest = Sha256::digest(bytes);
        Ok(format!("turn-trace:sha256:{digest:x}"))
    }
}

fn validate_optional_identity(
    value: &Option<String>,
    field: &'static str,
) -> Result<(), TurnTraceError> {
    if let Some(value) = value {
        validate_identity(value, field)?;
    }
    Ok(())
}

fn validate_optional_hash_identity(
    value: &Option<String>,
    field: &'static str,
) -> Result<(), TurnTraceError> {
    if let Some(value) = value {
        validate_hash_identity(value, field)?;
    }
    Ok(())
}

fn validate_hash_identity(value: &str, field: &'static str) -> Result<(), TurnTraceError> {
    let digest = ["sha256:", "turn-trace:sha256:"]
        .iter()
        .find_map(|prefix| value.strip_prefix(prefix));
    if !digest.is_some_and(|digest| {
        digest.len() == 64
            && digest
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    }) {
        return Err(error("turn-trace-hash-identity-invalid", field));
    }
    validate_identity(value, field)
}

fn validate_usage_report_identity(value: &str) -> Result<(), TurnTraceError> {
    let digest = value.strip_prefix("usage-authority:sha256:");
    if !digest.is_some_and(|digest| {
        digest.len() == 64
            && digest
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    }) {
        return Err(error(
            "turn-trace-usage-report-identity-invalid",
            "usage report identity is not a canonical SHA-256 identity",
        ));
    }
    validate_identity(value, "usage report identity")
}

fn validate_environment_identity(value: &str) -> Result<(), TurnTraceError> {
    let digest = value.strip_prefix("environment:sha256:");
    if !digest.is_some_and(|digest| {
        digest.len() == 64
            && digest
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    }) {
        return Err(error(
            "turn-trace-environment-identity-invalid",
            "environment identity is not a canonical SHA-256 identity",
        ));
    }
    validate_identity(value, "environment identity")
}

fn validate_identity(value: &str, _field: &'static str) -> Result<(), TurnTraceError> {
    if value.is_empty() || value.len() > MAX_ID_BYTES || !value.is_ascii() {
        return Err(error(
            "turn-trace-identity-invalid",
            "identity is empty, non-ASCII, or exceeds its bound",
        ));
    }
    if value.contains('/')
        || value.contains('\\')
        || value.contains("..")
        || value
            .chars()
            .any(|character| character.is_ascii_whitespace())
        || value.chars().any(|character| character.is_ascii_control())
    {
        return Err(error(
            "turn-trace-identity-invalid",
            "identity contains path, whitespace, or control content",
        ));
    }
    if value
        .chars()
        .any(|character| !(character.is_ascii_alphanumeric() || ".:_@+-".contains(character)))
    {
        return Err(error(
            "turn-trace-identity-invalid",
            "identity contains an unsupported character",
        ));
    }
    let lower = value.to_ascii_lowercase();
    let secret_markers = [
        "sk-",
        "ghp_",
        "github_pat_",
        "jwt:",
        "bearer:",
        "api_key:",
        "apikey:",
        "password:",
        "secret:",
        "authorization:",
        "private-key:",
        "-----begin",
    ];
    if secret_markers.iter().any(|marker| lower.contains(marker)) {
        return Err(error(
            "turn-trace-secret-shaped",
            "identity resembles a credential or secret",
        ));
    }
    Ok(())
}

fn validate_label(value: &str, _field: &'static str) -> Result<(), TurnTraceError> {
    if value.is_empty() || value.len() > MAX_LABEL_BYTES || !value.is_ascii() {
        return Err(error(
            "turn-trace-label-invalid",
            "label is empty, non-ASCII, or exceeds its bound",
        ));
    }
    if value.chars().any(|character| {
        character.is_ascii_whitespace()
            || character.is_ascii_control()
            || !(character.is_ascii_alphanumeric() || ".:_-".contains(character))
    }) {
        return Err(error(
            "turn-trace-label-invalid",
            "label contains unsupported content",
        ));
    }
    let lower = value.to_ascii_lowercase();
    if [
        "sk-",
        "ghp_",
        "github_pat_",
        "jwt:",
        "bearer:",
        "api_key:",
        "apikey:",
        "password:",
        "secret:",
        "authorization:",
        "private-key:",
        "-----begin",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
    {
        return Err(error(
            "turn-trace-secret-shaped",
            "label resembles a credential or secret",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn hash_identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn binding() -> TraceBinding {
        TraceBinding {
            session_id: "session-1".into(),
            turn_id: "turn-1".into(),
            project_id: Some("project-1".into()),
            environment_identity: Some(hash_identity("environment:sha256:", 'a')),
        }
    }

    fn evidence(source: EvidenceSource) -> EvidenceRef {
        EvidenceRef {
            authority: AuthorityLabel::Observed,
            source,
            identity: Some(hash_identity("sha256:", 'b')),
            observed_at_ms: Some(10),
        }
    }

    fn unknown_evidence(source: EvidenceSource) -> EvidenceRef {
        EvidenceRef {
            authority: AuthorityLabel::Unknown,
            source,
            identity: None,
            observed_at_ms: None,
        }
    }

    fn redaction() -> RedactionSummary {
        RedactionSummary::metadata_only()
    }

    fn usage_report(at_ms: u64) -> UsageAuthorityReport {
        crate::usage_authority::from_provider_token_usage(
            &json!({
                "total": {
                    "input_tokens": 80,
                    "cached_input_tokens": 20,
                    "output_tokens": 20,
                    "reasoning_output_tokens": 5,
                    "total_tokens": 100
                },
                "last": {
                    "input_tokens": 12,
                    "cached_input_tokens": 3,
                    "output_tokens": 4,
                    "reasoning_output_tokens": 1,
                    "total_tokens": 16
                },
                "model_context_window": 128000
            }),
            at_ms,
        )
        .unwrap()
    }

    fn usage_report_payload(at_ms: u64, persisted_item_id: &str) -> TracePayload {
        let report = usage_report(at_ms);
        let report_identity = report.metadata_identity().unwrap();
        TracePayload::UsageReport {
            report_identity: report_identity.clone(),
            persisted_item_id: persisted_item_id.into(),
            scope: UsageReportScope::ProviderThread,
            accounting: UsageAccounting::AbsoluteSnapshot,
            attempt_attribution: UsageAttribution::Unavailable,
            retry_attribution: UsageAttribution::Unavailable,
            report,
            evidence: EvidenceRef {
                authority: AuthorityLabel::Observed,
                source: EvidenceSource::UsageProvider,
                identity: Some(report_identity),
                observed_at_ms: Some(at_ms),
            },
            redaction: redaction(),
        }
    }

    fn intent_payload(
        session_mode: SessionMode,
        turn_kind: TurnKind,
        access: TurnAccess,
    ) -> TracePayload {
        TracePayload::Intent {
            session_mode,
            turn_kind,
            access,
            intent_identity: hash_identity("sha256:", '1'),
            evidence: evidence(EvidenceSource::Runtime),
            redaction: redaction(),
        }
    }

    fn not_applicable(source: EvidenceSource, byte: char) -> CompletionDomain {
        let mut evidence = evidence(source);
        evidence.identity = Some(hash_identity("sha256:", byte));
        CompletionDomain::NotApplicable { evidence }
    }

    fn applicable(source: EvidenceSource, byte: char) -> CompletionDomain {
        let mut evidence = evidence(source);
        evidence.identity = Some(hash_identity("sha256:", byte));
        CompletionDomain::Applicable { evidence }
    }

    fn observed(source: EvidenceSource, byte: char, count: u32) -> CompletionDomain {
        CompletionDomain::Observed {
            identity: hash_identity("sha256:", byte),
            observation_count: count,
            evidence: evidence(source),
        }
    }

    fn chat_completion() -> CompletionEvidence {
        CompletionEvidence {
            intent_identity: hash_identity("sha256:", '1'),
            workspace_change: not_applicable(EvidenceSource::Runtime, '2'),
            git_change: not_applicable(EvidenceSource::Runtime, '3'),
            verification: not_applicable(EvidenceSource::Runtime, '4'),
        }
    }

    fn read_only_completion(verification: CompletionDomain) -> CompletionEvidence {
        CompletionEvidence {
            intent_identity: hash_identity("sha256:", '1'),
            workspace_change: not_applicable(EvidenceSource::Runtime, '2'),
            git_change: not_applicable(EvidenceSource::Runtime, '3'),
            verification,
        }
    }

    fn completed_terminal(completion: CompletionEvidence) -> TracePayload {
        TracePayload::Terminal {
            state: TerminalState::Completed,
            evidence: TerminalEvidence {
                workspace_identity: None,
                git_state_identity: None,
                verification_identity: None,
                observed_verification_count: 0,
                evidence: evidence(EvidenceSource::Runtime),
                completion: Some(completion),
            },
            redaction: redaction(),
        }
    }

    fn legacy_completed_terminal() -> TracePayload {
        TracePayload::Terminal {
            state: TerminalState::Completed,
            evidence: TerminalEvidence {
                workspace_identity: Some(hash_identity("sha256:", 'c')),
                git_state_identity: Some(hash_identity("sha256:", 'd')),
                verification_identity: Some(hash_identity("sha256:", 'e')),
                observed_verification_count: 1,
                evidence: evidence(EvidenceSource::TestRunner),
                completion: None,
            },
            redaction: redaction(),
        }
    }

    fn runtime_payload() -> TracePayload {
        TracePayload::Runtime {
            runtime_identity: "runtime-1".into(),
            adapter_identity: "codex-app-server".into(),
            version: "0.144.5".into(),
            state: RuntimeState::Ready,
            evidence: evidence(EvidenceSource::Runtime),
            redaction: redaction(),
        }
    }

    fn runtime_approval_policy_payload(binding: &TraceBinding, at_ms: u64) -> TracePayload {
        let provider_thread_identity = provider_thread_identity("provider-thread-1").unwrap();
        let policy = RuntimeApprovalPolicy::Never;
        let reviewer = ApprovalPolicyReviewer::User;
        let sandbox = ApprovalPolicySandbox::ReadOnly;
        let permission_profile = ApprovalPolicyPermissionProfile::ReadOnly;
        let configured_policy_identity =
            configured_runtime_approval_policy_identity(policy, sandbox, permission_profile);
        let effective_policy_identity = effective_runtime_approval_policy_identity(
            &provider_thread_identity,
            policy,
            reviewer,
            sandbox,
            permission_profile,
        );
        let policy_authority_identity = runtime_approval_policy_authority_identity(
            binding,
            "runtime-1",
            "codex-app-server",
            "0.144.5",
            &provider_thread_identity,
            &configured_policy_identity,
            &effective_policy_identity,
        );
        TracePayload::RuntimeApprovalPolicy {
            runtime_identity: "runtime-1".into(),
            adapter_identity: "codex-app-server".into(),
            runtime_version: "0.144.5".into(),
            provider_thread_identity,
            policy,
            reviewer,
            sandbox,
            permission_profile,
            configured_policy_identity,
            effective_policy_identity,
            policy_authority_identity: policy_authority_identity.clone(),
            decision_attribution: ApprovalDecisionAttribution::NoUserDecision,
            user_decision_observed: false,
            execution_authority: false,
            evidence: EvidenceRef {
                authority: AuthorityLabel::Observed,
                source: EvidenceSource::Runtime,
                identity: Some(policy_authority_identity),
                observed_at_ms: Some(at_ms),
            },
            redaction: RedactionSummary::metadata_only(),
        }
    }

    fn tool_evidence(at_ms: u64, byte: char) -> EvidenceRef {
        EvidenceRef {
            authority: AuthorityLabel::Observed,
            source: EvidenceSource::ToolRuntime,
            identity: Some(hash_identity("sha256:", byte)),
            observed_at_ms: Some(at_ms),
        }
    }

    fn started_tool(at_ms: u64) -> TracePayload {
        TracePayload::Tool {
            tool_identity: hash_identity("sha256:", '5'),
            action_identity: hash_identity("sha256:", '6'),
            state: ToolState::Started,
            provider_status: Some(ToolProviderStatus::InProgress),
            source: Some(ToolSource::Agent),
            input_identity: Some(hash_identity("sha256:", '7')),
            output_identity: None,
            item_binding: Some(ToolTimelineBinding::NotPersisted),
            duration_ms: None,
            exit_code: None,
            evidence: tool_evidence(at_ms, '8'),
            redaction: redaction(),
        }
    }

    fn terminal_tool(at_ms: u64, state: ToolState) -> TracePayload {
        let provider_status = match state {
            ToolState::Completed => ToolProviderStatus::Completed,
            ToolState::Failed => ToolProviderStatus::Failed,
            ToolState::Declined => ToolProviderStatus::Declined,
            _ => panic!("terminal Tool helper requires a terminal state"),
        };
        TracePayload::Tool {
            tool_identity: hash_identity("sha256:", '5'),
            action_identity: hash_identity("sha256:", '6'),
            state,
            provider_status: Some(provider_status),
            source: Some(ToolSource::Agent),
            input_identity: Some(hash_identity("sha256:", '7')),
            output_identity: Some(hash_identity("sha256:", '9')),
            item_binding: Some(ToolTimelineBinding::Persisted {
                item_identity: hash_identity("sha256:", 'a'),
                payload_identity: hash_identity("sha256:", 'a'),
            }),
            duration_ms: (state != ToolState::Declined).then_some(1),
            exit_code: match state {
                ToolState::Completed => Some(0),
                ToolState::Failed => Some(1),
                ToolState::Declined => None,
                _ => unreachable!("matched terminal Tool state"),
            },
            evidence: tool_evidence(at_ms, 'c'),
            redaction: redaction(),
        }
    }

    fn indexed_hash_identity(namespace: u64, index: usize) -> String {
        format!("sha256:{:064x}", namespace + index as u64)
    }

    fn indexed_tool(index: usize, at_ms: u64, state: ToolState) -> TracePayload {
        let terminal = match state {
            ToolState::Started => None,
            ToolState::Completed => Some(ToolProviderStatus::Completed),
            ToolState::Failed => Some(ToolProviderStatus::Failed),
            ToolState::Declined => Some(ToolProviderStatus::Declined),
            ToolState::Requested | ToolState::Cancelled => {
                panic!("indexed Tool helper requires a supported provider state")
            }
        };
        TracePayload::Tool {
            tool_identity: indexed_hash_identity(1_000, index),
            action_identity: indexed_hash_identity(2_000, index),
            state,
            provider_status: terminal.or(Some(ToolProviderStatus::InProgress)),
            source: Some(ToolSource::Agent),
            input_identity: Some(indexed_hash_identity(3_000, index)),
            output_identity: terminal.map(|_| indexed_hash_identity(4_000, index)),
            item_binding: Some(if terminal.is_some() {
                ToolTimelineBinding::Persisted {
                    item_identity: indexed_hash_identity(5_000, index),
                    payload_identity: indexed_hash_identity(5_000, index),
                }
            } else {
                ToolTimelineBinding::NotPersisted
            }),
            duration_ms: terminal
                .filter(|status| *status != ToolProviderStatus::Declined)
                .map(|_| 1),
            exit_code: terminal.and_then(|status| match status {
                ToolProviderStatus::Completed => Some(0),
                ToolProviderStatus::Failed => Some(1),
                ToolProviderStatus::Declined => None,
                ToolProviderStatus::InProgress => unreachable!("matched terminal Tool status"),
            }),
            evidence: EvidenceRef {
                authority: AuthorityLabel::Observed,
                source: EvidenceSource::ToolRuntime,
                identity: Some(indexed_hash_identity(6_000, index)),
                observed_at_ms: Some(at_ms),
            },
            redaction: redaction(),
        }
    }

    fn noncompleted_terminal(state: TerminalState) -> TracePayload {
        TracePayload::Terminal {
            state,
            evidence: TerminalEvidence {
                workspace_identity: None,
                git_state_identity: None,
                verification_identity: None,
                observed_verification_count: 0,
                evidence: evidence(EvidenceSource::Runtime),
                completion: None,
            },
            redaction: redaction(),
        }
    }

    fn append_read_only_intent(trace: &mut TurnTrace) {
        trace
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
    }

    #[test]
    fn v0_5_runtime_approval_policy_is_content_free_and_not_a_user_decision() {
        let binding = binding();
        let mut trace = TurnTrace::new(binding.clone()).unwrap();
        append_read_only_intent(&mut trace);
        trace
            .append("runtime-1".into(), 10, runtime_payload())
            .unwrap();
        trace
            .append(
                "runtime-approval-policy-1".into(),
                10,
                runtime_approval_policy_payload(&binding, 10),
            )
            .unwrap();
        trace
            .append(
                "terminal-1".into(),
                11,
                noncompleted_terminal(TerminalState::Interrupted),
            )
            .unwrap();
        trace.validate_complete().unwrap();

        let value = serde_json::to_value(&trace.events[2].payload).unwrap();
        assert_eq!(trace.schema_version, "turn-trace/0.5");
        assert_eq!(value["kind"], "runtime-approval-policy");
        assert_eq!(value["policy"], "never");
        assert_eq!(value["reviewer"], "user");
        assert_eq!(value["sandbox"], "read-only");
        assert_eq!(value["permission_profile"], "read-only");
        assert_eq!(value["decision_attribution"], "no-user-decision");
        assert_eq!(value["user_decision_observed"], false);
        assert_eq!(value["execution_authority"], false);
        assert_eq!(
            value["evidence"]["identity"],
            value["policy_authority_identity"]
        );
        let serialized = serde_json::to_string(&trace).unwrap();
        for forbidden in [
            "provider-thread-1",
            "approval request",
            "/Users/alice/project",
            "sk-secret-value",
        ] {
            assert!(!serialized.contains(forbidden));
        }
    }

    #[test]
    fn v0_5_rejects_approval_until_the_durable_authority_producer_exists() {
        let binding = binding();
        let mut trace = TurnTrace::new(binding.clone()).unwrap();
        append_read_only_intent(&mut trace);
        trace
            .append("runtime-1".into(), 10, runtime_payload())
            .unwrap();
        trace
            .append(
                "runtime-approval-policy-1".into(),
                10,
                runtime_approval_policy_payload(&binding, 10),
            )
            .unwrap();

        let error = trace
            .append(
                "approval-1".into(),
                10,
                TracePayload::Approval {
                    approval_identity: "approval-1".into(),
                    requirement_identity: "requirement-1".into(),
                    authority_identity: "authority-1".into(),
                    decision: ApprovalDecision::Allowed,
                    evidence: EvidenceRef {
                        authority: AuthorityLabel::Unknown,
                        source: EvidenceSource::ApprovalAuthority,
                        identity: None,
                        observed_at_ms: None,
                    },
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap_err();
        assert_eq!(error.code, "turn-trace-v0-5-approval-producer-unavailable");
        assert!(trace
            .events
            .iter()
            .all(|event| !matches!(event.payload, TracePayload::Approval { .. })));
    }

    #[test]
    fn v0_5_runtime_approval_policy_rejects_semantic_tampering_and_wrong_order() {
        let binding = binding();
        let valid_payload = runtime_approval_policy_payload(&binding, 10);

        assert_eq!(
            TurnTrace::new(binding.clone())
                .unwrap()
                .validate_open()
                .unwrap_err()
                .code,
            "turn-trace-runtime-approval-policy-count-invalid"
        );

        for (expected, mutate) in [
            (
                "turn-trace-runtime-approval-policy-user-decision-invalid",
                0_u8,
            ),
            (
                "turn-trace-runtime-approval-policy-execution-authority-invalid",
                1,
            ),
            (
                "turn-trace-runtime-approval-policy-configured-identity-mismatch",
                2,
            ),
            (
                "turn-trace-runtime-approval-policy-effective-identity-mismatch",
                3,
            ),
            ("turn-trace-runtime-approval-policy-evidence-mismatch", 4),
        ] {
            let mut payload = valid_payload.clone();
            let TracePayload::RuntimeApprovalPolicy {
                user_decision_observed,
                execution_authority,
                configured_policy_identity,
                effective_policy_identity,
                evidence,
                ..
            } = &mut payload
            else {
                unreachable!("helper returns Runtime approval-policy metadata")
            };
            match mutate {
                0 => *user_decision_observed = true,
                1 => *execution_authority = true,
                2 => *configured_policy_identity = hash_identity("sha256:", 'a'),
                3 => *effective_policy_identity = hash_identity("sha256:", 'b'),
                4 => evidence.identity = Some(hash_identity("sha256:", 'c')),
                _ => unreachable!(),
            }
            let mut trace = TurnTrace::new(binding.clone()).unwrap();
            assert_eq!(
                trace
                    .append("runtime-approval-policy-1".into(), 10, payload)
                    .unwrap_err()
                    .code,
                expected
            );
        }

        let mut before_runtime = TurnTrace::new(binding.clone()).unwrap();
        before_runtime
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        before_runtime
            .append(
                "runtime-approval-policy-1".into(),
                10,
                valid_payload.clone(),
            )
            .unwrap();
        assert_eq!(
            before_runtime.validate_open().unwrap_err().code,
            "turn-trace-runtime-approval-policy-runtime-missing"
        );

        let mut too_late = TurnTrace::new(binding.clone()).unwrap();
        append_read_only_intent(&mut too_late);
        too_late
            .append("runtime-1".into(), 10, runtime_payload())
            .unwrap();
        too_late
            .append(
                "context-1".into(),
                10,
                TracePayload::Context {
                    manifest_identity: hash_identity("sha256:", 'd'),
                    item_count: 0,
                    included_items: 0,
                    excluded_items: 0,
                    bytes: 0,
                    evidence: evidence(EvidenceSource::ContextBuilder),
                    redaction: redaction(),
                },
            )
            .unwrap();
        too_late
            .append("runtime-approval-policy-1".into(), 10, valid_payload)
            .unwrap();
        assert_eq!(
            too_late.validate_open().unwrap_err().code,
            "turn-trace-runtime-approval-policy-order-invalid"
        );

        let mut bound = TurnTrace::new(binding.clone()).unwrap();
        append_read_only_intent(&mut bound);
        bound
            .append("runtime-1".into(), 10, runtime_payload())
            .unwrap();
        bound
            .append(
                "runtime-approval-policy-1".into(),
                10,
                runtime_approval_policy_payload(&binding, 10),
            )
            .unwrap();
        for mutate in 0..3 {
            let mut drift = bound.clone();
            match mutate {
                0 => drift.binding.session_id = "other-session".into(),
                1 => drift.binding.project_id = None,
                2 => {
                    drift.binding.environment_identity =
                        Some(hash_identity("environment:sha256:", '9'))
                }
                _ => unreachable!(),
            }
            assert_eq!(
                drift.validate_open().unwrap_err().code,
                "turn-trace-runtime-approval-policy-authority-identity-mismatch"
            );
        }

        let mut forged_authority = bound.clone();
        if let TracePayload::RuntimeApprovalPolicy {
            policy_authority_identity,
            evidence,
            ..
        } = &mut forged_authority.events[2].payload
        {
            *policy_authority_identity = hash_identity("sha256:", 'f');
            evidence.identity = Some(policy_authority_identity.clone());
        }
        assert_eq!(
            forged_authority.validate_open().unwrap_err().code,
            "turn-trace-runtime-approval-policy-authority-identity-mismatch"
        );

        let mut duplicate = bound.clone();
        duplicate
            .append(
                "runtime-approval-policy-2".into(),
                10,
                runtime_approval_policy_payload(&binding, 10),
            )
            .unwrap();
        assert_eq!(
            duplicate.validate_open().unwrap_err().code,
            "turn-trace-runtime-approval-policy-duplicate"
        );

        let mut wrong_time = TurnTrace::new(binding.clone()).unwrap();
        assert_eq!(
            wrong_time
                .append(
                    "runtime-approval-policy-1".into(),
                    11,
                    runtime_approval_policy_payload(&binding, 10),
                )
                .unwrap_err()
                .code,
            "turn-trace-runtime-approval-policy-time-mismatch"
        );

        let mut nonmetadata = runtime_approval_policy_payload(&binding, 10);
        if let TracePayload::RuntimeApprovalPolicy { redaction, .. } = &mut nonmetadata {
            redaction.raw_bytes = 1;
            redaction.omitted_fields = 1;
        }
        let mut redaction_trace = TurnTrace::new(binding.clone()).unwrap();
        assert_eq!(
            redaction_trace
                .append("runtime-approval-policy-1".into(), 10, nonmetadata)
                .unwrap_err()
                .code,
            "turn-trace-runtime-approval-policy-redaction-invalid"
        );

        let mut v0_4 = TurnTrace::new_v0_4(binding).unwrap();
        let old_version_payload = runtime_approval_policy_payload(&v0_4.binding, 10);
        assert_eq!(
            v0_4.append("runtime-approval-policy-1".into(), 10, old_version_payload,)
                .unwrap_err()
                .code,
            "turn-trace-runtime-approval-policy-version-invalid"
        );
    }

    #[test]
    fn v0_4_tool_lifecycle_is_content_free_and_binds_the_persisted_item() {
        let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
        append_read_only_intent(&mut trace);
        trace
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        trace
            .append(
                "tool-completed-1".into(),
                12,
                terminal_tool(12, ToolState::Completed),
            )
            .unwrap();
        trace
            .append(
                "terminal-1".into(),
                13,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        trace.validate_complete().unwrap();

        let value = serde_json::to_value(&trace).unwrap();
        assert_eq!(value["schema_version"], "turn-trace/0.4");
        assert_eq!(
            value["events"][1]["payload"]["provider_status"],
            "in-progress"
        );
        assert_eq!(value["events"][1]["payload"]["source"], "agent");
        assert_eq!(
            value["events"][1]["payload"]["item_binding"]["kind"],
            "not-persisted"
        );
        assert_eq!(
            value["events"][2]["payload"]["item_binding"]["item_identity"],
            hash_identity("sha256:", 'a')
        );
        assert_eq!(
            value["events"][2]["payload"]["item_binding"]["kind"],
            "persisted"
        );
        let serialized = serde_json::to_string(&trace).unwrap();
        for forbidden in [
            "git status --short",
            "/Users/alice/project",
            "M src/main.rs",
            "sk-secret-value",
        ] {
            assert!(!serialized.contains(forbidden));
        }
    }

    #[test]
    fn v0_4_tool_terminal_states_map_exactly_to_provider_status() {
        for (state, status) in [
            (ToolState::Completed, ToolProviderStatus::Completed),
            (ToolState::Failed, ToolProviderStatus::Failed),
            (ToolState::Declined, ToolProviderStatus::Declined),
        ] {
            let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
            append_read_only_intent(&mut trace);
            trace
                .append("tool-started-1".into(), 11, started_tool(11))
                .unwrap();
            let terminal = terminal_tool(12, state);
            trace
                .append("tool-terminal-1".into(), 12, terminal)
                .unwrap();
            trace.validate_open().unwrap();

            let mut mismatch = terminal_tool(12, state);
            if let TracePayload::Tool {
                provider_status, ..
            } = &mut mismatch
            {
                *provider_status = Some(match status {
                    ToolProviderStatus::Completed => ToolProviderStatus::Failed,
                    _ => ToolProviderStatus::Completed,
                });
            }
            assert_eq!(
                TurnTrace::new(binding())
                    .unwrap()
                    .append("tool-terminal-1".into(), 12, mismatch)
                    .unwrap_err()
                    .code,
                "turn-trace-tool-terminal-status-mismatch"
            );
        }
    }

    #[test]
    fn v0_4_tool_terminal_execution_semantics_fail_closed() {
        for (state, duration, exit_code, expected_code) in [
            (
                ToolState::Completed,
                Some(2),
                Some(1),
                "turn-trace-tool-exit-code-status-mismatch",
            ),
            (
                ToolState::Failed,
                Some(2),
                Some(0),
                "turn-trace-tool-exit-code-status-mismatch",
            ),
            (
                ToolState::Declined,
                Some(1),
                None,
                "turn-trace-tool-declined-execution-invalid",
            ),
            (
                ToolState::Declined,
                None,
                Some(1),
                "turn-trace-tool-declined-execution-invalid",
            ),
        ] {
            let mut trace = TurnTrace::new(binding()).unwrap();
            append_read_only_intent(&mut trace);
            trace
                .append("tool-started-1".into(), 10, started_tool(10))
                .unwrap();
            let mut terminal = terminal_tool(12, state);
            if let TracePayload::Tool {
                duration_ms,
                exit_code: actual_exit_code,
                ..
            } = &mut terminal
            {
                *duration_ms = duration;
                *actual_exit_code = exit_code;
            }
            assert_eq!(
                trace
                    .append("tool-terminal-1".into(), 12, terminal)
                    .unwrap_err()
                    .code,
                expected_code
            );
        }

        let mut trace = TurnTrace::new(binding()).unwrap();
        append_read_only_intent(&mut trace);
        trace
            .append("tool-started-1".into(), 10, started_tool(10))
            .unwrap();
        let mut terminal = terminal_tool(12, ToolState::Completed);
        if let TracePayload::Tool { duration_ms, .. } = &mut terminal {
            *duration_ms = Some(3);
        }
        trace
            .append("tool-terminal-1".into(), 12, terminal)
            .unwrap();
        assert_eq!(
            trace.validate_open().unwrap_err().code,
            "turn-trace-tool-duration-interval-invalid"
        );
    }

    #[test]
    fn v0_4_tool_action_limit_accepts_128_complete_actions_and_rejects_129() {
        let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
        append_read_only_intent(&mut trace);
        for index in 0..MAX_TOOL_ACTIONS {
            let started_at_ms = 11 + (index as u64 * 2);
            trace
                .append(
                    format!("tool-started-{index}"),
                    started_at_ms,
                    indexed_tool(index, started_at_ms, ToolState::Started),
                )
                .unwrap();
            trace
                .append(
                    format!("tool-completed-{index}"),
                    started_at_ms + 1,
                    indexed_tool(index, started_at_ms + 1, ToolState::Completed),
                )
                .unwrap();
        }
        trace.validate_open().unwrap();

        let mut overflow = trace.clone();
        let overflow_at_ms = 11 + (MAX_TOOL_ACTIONS as u64 * 2);
        overflow
            .append(
                "tool-started-overflow".into(),
                overflow_at_ms,
                indexed_tool(MAX_TOOL_ACTIONS, overflow_at_ms, ToolState::Started),
            )
            .unwrap();
        assert_eq!(
            overflow.validate_open().unwrap_err().code,
            "turn-trace-tool-action-limit"
        );

        trace
            .append(
                "terminal-1".into(),
                overflow_at_ms,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        trace.validate_complete().unwrap();
    }

    #[test]
    fn v0_4_tool_state_machine_rejects_invalid_transitions_and_binding_drift() {
        let mut terminal_without_start = TurnTrace::new(binding()).unwrap();
        append_read_only_intent(&mut terminal_without_start);
        terminal_without_start
            .append(
                "tool-terminal-1".into(),
                12,
                terminal_tool(12, ToolState::Failed),
            )
            .unwrap();
        assert_eq!(
            terminal_without_start.validate_open().unwrap_err().code,
            "turn-trace-tool-terminal-without-start"
        );

        let mut duplicate_start = TurnTrace::new(binding()).unwrap();
        append_read_only_intent(&mut duplicate_start);
        duplicate_start
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        duplicate_start
            .append("tool-started-2".into(), 12, started_tool(12))
            .unwrap();
        assert_eq!(
            duplicate_start.validate_open().unwrap_err().code,
            "turn-trace-tool-started-duplicate"
        );

        let mut duplicate_terminal = TurnTrace::new(binding()).unwrap();
        append_read_only_intent(&mut duplicate_terminal);
        duplicate_terminal
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        duplicate_terminal
            .append(
                "tool-terminal-1".into(),
                12,
                terminal_tool(12, ToolState::Failed),
            )
            .unwrap();
        duplicate_terminal
            .append(
                "tool-terminal-2".into(),
                13,
                terminal_tool(13, ToolState::Failed),
            )
            .unwrap();
        assert_eq!(
            duplicate_terminal.validate_open().unwrap_err().code,
            "turn-trace-tool-terminal-duplicate"
        );

        let mut started_after_terminal = duplicate_terminal.clone();
        started_after_terminal.events.pop();
        started_after_terminal
            .append("tool-started-2".into(), 13, started_tool(13))
            .unwrap();
        assert_eq!(
            started_after_terminal.validate_open().unwrap_err().code,
            "turn-trace-tool-started-after-terminal"
        );

        let mut drift = TurnTrace::new(binding()).unwrap();
        append_read_only_intent(&mut drift);
        drift
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        let mut terminal = terminal_tool(12, ToolState::Completed);
        if let TracePayload::Tool { input_identity, .. } = &mut terminal {
            *input_identity = Some(hash_identity("sha256:", 'd'));
        }
        drift
            .append("tool-terminal-1".into(), 12, terminal)
            .unwrap();
        assert_eq!(
            drift.validate_open().unwrap_err().code,
            "turn-trace-tool-lifecycle-binding-mismatch"
        );
    }

    #[test]
    fn v0_4_completed_turn_requires_tool_terminal_but_failure_may_retain_started() {
        let mut completed = TurnTrace::new_v0_4(binding()).unwrap();
        append_read_only_intent(&mut completed);
        completed
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        completed
            .append(
                "terminal-1".into(),
                12,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        assert_eq!(
            completed.validate_complete().unwrap_err().code,
            "turn-trace-tool-incomplete-on-terminal"
        );

        for terminal_state in [TerminalState::Failed, TerminalState::Interrupted] {
            let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
            append_read_only_intent(&mut trace);
            trace
                .append("tool-started-1".into(), 11, started_tool(11))
                .unwrap();
            trace
                .append(
                    "terminal-1".into(),
                    12,
                    noncompleted_terminal(terminal_state),
                )
                .unwrap();
            trace.validate_complete().unwrap();
        }

        let mut cancelled = TurnTrace::new_v0_4(binding()).unwrap();
        append_read_only_intent(&mut cancelled);
        cancelled
            .append("tool-started-1".into(), 11, started_tool(11))
            .unwrap();
        cancelled
            .append(
                "terminal-1".into(),
                12,
                noncompleted_terminal(TerminalState::Cancelled),
            )
            .unwrap();
        assert_eq!(
            cancelled.validate_complete().unwrap_err().code,
            "turn-trace-tool-incomplete-on-terminal"
        );
    }

    #[test]
    fn v0_4_tool_requires_exact_time_observed_authority_and_metadata_only_redaction() {
        let mut wrong_time = started_tool(11);
        if let TracePayload::Tool { evidence, .. } = &mut wrong_time {
            evidence.observed_at_ms = Some(10);
        }
        assert_eq!(
            TurnTrace::new(binding())
                .unwrap()
                .append("tool-started-1".into(), 11, wrong_time)
                .unwrap_err()
                .code,
            "turn-trace-tool-time-mismatch"
        );

        let mut estimated = started_tool(11);
        if let TracePayload::Tool { evidence, .. } = &mut estimated {
            evidence.authority = AuthorityLabel::Estimated;
        }
        assert_eq!(
            TurnTrace::new(binding())
                .unwrap()
                .append("tool-started-1".into(), 11, estimated)
                .unwrap_err()
                .code,
            "turn-trace-tool-authority-invalid"
        );

        let mut nonmetadata = terminal_tool(12, ToolState::Completed);
        if let TracePayload::Tool { redaction, .. } = &mut nonmetadata {
            redaction.raw_bytes = 1;
        }
        assert_eq!(
            TurnTrace::new(binding())
                .unwrap()
                .append("tool-terminal-1".into(), 12, nonmetadata)
                .unwrap_err()
                .code,
            "turn-trace-tool-redaction-invalid"
        );

        let mut not_persisted = terminal_tool(12, ToolState::Completed);
        if let TracePayload::Tool { item_binding, .. } = &mut not_persisted {
            *item_binding = Some(ToolTimelineBinding::NotPersisted);
        }
        assert_eq!(
            TurnTrace::new(binding())
                .unwrap()
                .append("tool-terminal-1".into(), 12, not_persisted)
                .unwrap_err()
                .code,
            "turn-trace-tool-terminal-item-missing"
        );
    }

    #[test]
    fn older_tool_contracts_keep_their_shape_and_reject_v0_4_fields() {
        let old_tool = || TracePayload::Tool {
            tool_identity: "tool-1".into(),
            action_identity: "action-1".into(),
            state: ToolState::Completed,
            provider_status: None,
            source: None,
            input_identity: Some("input-1".into()),
            output_identity: Some("output-1".into()),
            item_binding: None,
            duration_ms: None,
            exit_code: None,
            evidence: evidence(EvidenceSource::ToolRuntime),
            redaction: redaction(),
        };
        let mut v0_3 = TurnTrace::new_v0_3(binding()).unwrap();
        append_read_only_intent(&mut v0_3);
        v0_3.append("tool-1".into(), 10, old_tool()).unwrap();
        v0_3.validate_open().unwrap();
        let serialized = serde_json::to_value(&v0_3.events[1].payload).unwrap();
        for absent in [
            "provider_status",
            "source",
            "item_binding",
            "duration_ms",
            "exit_code",
        ] {
            assert!(serialized.get(absent).is_none());
        }

        let mut cross_version = old_tool();
        if let TracePayload::Tool {
            provider_status, ..
        } = &mut cross_version
        {
            *provider_status = Some(ToolProviderStatus::Completed);
        }
        assert_eq!(
            v0_3.append("tool-2".into(), 11, cross_version)
                .unwrap_err()
                .code,
            "turn-trace-tool-version-invalid"
        );

        let mut declined = old_tool();
        if let TracePayload::Tool { state, .. } = &mut declined {
            *state = ToolState::Declined;
        }
        assert_eq!(
            v0_3.append("tool-3".into(), 11, declined).unwrap_err().code,
            "turn-trace-tool-version-invalid"
        );
    }

    #[test]
    fn valid_trace_covers_source_qualified_metadata_and_terminal_evidence() {
        let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
        trace
            .append(
                "event-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        trace
            .append("event-2".into(), 10, runtime_payload())
            .unwrap();
        trace
            .append(
                "event-3".into(),
                10,
                TracePayload::Test {
                    test_identity: "test-1".into(),
                    runner_identity: "runner-1".into(),
                    outcome: TestOutcome::Passed,
                    case_count: 2,
                    command_identity: hash_identity("sha256:", 'f'),
                    verification_identity: hash_identity("sha256:", 'e'),
                    evidence: evidence(EvidenceSource::TestRunner),
                    redaction: redaction(),
                },
            )
            .unwrap();
        trace
            .append(
                "event-4".into(),
                11,
                completed_terminal(read_only_completion(observed(
                    EvidenceSource::TestRunner,
                    'e',
                    1,
                ))),
            )
            .unwrap();
        trace.validate_complete().unwrap();
        assert!(trace
            .metadata_identity()
            .unwrap()
            .starts_with("turn-trace:sha256:"));
    }

    #[test]
    fn sequence_time_and_terminal_order_are_invariants() {
        let mut trace = TurnTrace::new(binding()).unwrap();
        trace
            .append(
                "event-1".into(),
                20,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        trace
            .append("event-2".into(), 20, runtime_payload())
            .unwrap();
        assert_eq!(
            trace
                .append("event-3".into(), 19, runtime_payload())
                .unwrap_err()
                .code,
            "turn-trace-time-order-invalid"
        );
        trace
            .append(
                "event-3".into(),
                20,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        assert_eq!(
            trace
                .append("event-4".into(), 21, runtime_payload())
                .unwrap_err()
                .code,
            "turn-trace-terminal-not-last"
        );

        let mut forged = trace.clone();
        forged.events[2].sequence = 4;
        assert_eq!(
            forged.validate_complete().unwrap_err().code,
            "turn-trace-sequence-gap"
        );
    }

    #[test]
    fn source_and_authority_mismatches_fail_closed() {
        let mut trace = TurnTrace::new_legacy(binding()).unwrap();
        let mut payload = runtime_payload();
        if let TracePayload::Runtime { evidence, .. } = &mut payload {
            evidence.source = EvidenceSource::Provider;
        }
        assert_eq!(
            trace.append("event-1".into(), 1, payload).unwrap_err().code,
            "turn-trace-source-mismatch"
        );

        let unknown = EvidenceRef {
            authority: AuthorityLabel::Unknown,
            source: EvidenceSource::Runtime,
            identity: Some(hash_identity("sha256:", '9')),
            observed_at_ms: None,
        };
        assert_eq!(
            unknown.validate().unwrap_err().code,
            "turn-trace-unknown-evidence-has-value"
        );
        let catalog = EvidenceRef {
            authority: AuthorityLabel::CatalogDerived,
            source: EvidenceSource::Provider,
            identity: Some("model-catalog:sha256:catalog-1".into()),
            observed_at_ms: Some(1),
        };
        assert_eq!(
            catalog.validate().unwrap_err().code,
            "turn-trace-catalog-source-invalid"
        );
    }

    #[test]
    fn sensitive_content_and_raw_payloads_are_rejected() {
        assert_eq!(
            validate_identity("sk-secret-value", "id").unwrap_err().code,
            "turn-trace-secret-shaped"
        );
        assert_eq!(
            validate_identity("/Users/alice/project", "id")
                .unwrap_err()
                .code,
            "turn-trace-identity-invalid"
        );
        for path in ["C:/Users/alice/project", "foo/bar", r"foo\bar"] {
            assert_eq!(
                validate_identity(path, "id").unwrap_err().code,
                "turn-trace-identity-invalid"
            );
        }
        for label in ["/Users/alice/project", "C:/Users/alice", "foo/bar"] {
            assert_eq!(
                validate_label(label, "label").unwrap_err().code,
                "turn-trace-label-invalid"
            );
        }
        for forged_hash in [
            "sha256:C:/Users/alice",
            "sha256:relative/path",
            "sha256:abc",
        ] {
            assert_eq!(
                validate_hash_identity(forged_hash, "hash")
                    .unwrap_err()
                    .code,
                "turn-trace-hash-identity-invalid"
            );
        }
        validate_environment_identity(&hash_identity("environment:sha256:", '7')).unwrap();
        assert_eq!(
            validate_label("api_key:credential", "label")
                .unwrap_err()
                .code,
            "turn-trace-secret-shaped"
        );
        let mut summary = redaction();
        summary.content_included = true;
        assert_eq!(
            summary.validate().unwrap_err().code,
            "turn-trace-content-included"
        );
        let serialized = serde_json::to_string(&runtime_payload()).unwrap();
        assert!(!serialized.contains("prompt"));
        assert!(!serialized.contains("authorization"));
        assert!(!serialized.contains("secret"));
    }

    #[test]
    fn legacy_completion_requires_authoritative_workspace_git_and_tests() {
        let mut trace = TurnTrace::new_legacy(binding()).unwrap();
        let payload = TracePayload::Terminal {
            state: TerminalState::Completed,
            evidence: TerminalEvidence {
                workspace_identity: Some(hash_identity("sha256:", 'c')),
                git_state_identity: None,
                verification_identity: Some(hash_identity("sha256:", 'e')),
                observed_verification_count: 1,
                evidence: evidence(EvidenceSource::Workspace),
                completion: None,
            },
            redaction: redaction(),
        };
        assert_eq!(
            trace.append("event-1".into(), 1, payload).unwrap_err().code,
            "turn-trace-completion-evidence-missing"
        );
    }

    #[test]
    fn usage_identity_prevents_double_counting_retries() {
        let mut trace = TurnTrace::new_legacy(binding()).unwrap();
        let usage = || TracePayload::Usage {
            usage_identity: "usage-1".into(),
            attempt_identity: "attempt-1".into(),
            metric: UsageMetric::Token,
            value: UsageValue {
                input_tokens: Some(1),
                output_tokens: Some(2),
                cached_input_tokens: None,
                total_tokens: Some(3),
                context_tokens: None,
                context_window_tokens: None,
                reasoning_tokens: None,
                cost_micros: None,
                latency_ms: None,
            },
            retry: false,
            evidence: evidence(EvidenceSource::UsageProvider),
            redaction: redaction(),
        };
        trace.append("event-1".into(), 1, usage()).unwrap();
        trace.append("event-2".into(), 2, usage()).unwrap();
        assert_eq!(
            trace.validate_open().unwrap_err().code,
            "turn-trace-usage-duplicate"
        );
    }

    #[test]
    fn v0_3_usage_report_binds_schema_source_time_item_and_report() {
        let mut trace = TurnTrace::new_v0_3(binding()).unwrap();
        trace
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        trace
            .append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap();
        trace.validate_open().unwrap();

        let value = serde_json::to_value(&trace.events[1].payload).unwrap();
        assert_eq!(value["kind"], "usage-report");
        assert_eq!(value["persisted_item_id"], "item-usage-1");
        assert_eq!(value["scope"], "provider-thread");
        assert_eq!(value["accounting"], "absolute-snapshot");
        assert_eq!(value["attempt_attribution"], "unavailable");
        assert_eq!(value["retry_attribution"], "unavailable");
        assert!(value.get("attempt_identity").is_none());
        assert!(value.get("retry").is_none());

        let mut wrong_source = trace.clone();
        if let TracePayload::UsageReport { evidence, .. } = &mut wrong_source.events[1].payload {
            evidence.source = EvidenceSource::Runtime;
        }
        assert_eq!(
            wrong_source.validate_open().unwrap_err().code,
            "turn-trace-source-mismatch"
        );

        let mut wrong_time = trace.clone();
        wrong_time.events[1].at_ms = 21;
        assert_eq!(
            wrong_time.validate_open().unwrap_err().code,
            "turn-trace-usage-report-time-mismatch"
        );

        let mut wrong_evidence_time = trace.clone();
        if let TracePayload::UsageReport { evidence, .. } =
            &mut wrong_evidence_time.events[1].payload
        {
            evidence.observed_at_ms = Some(19);
        }
        assert_eq!(
            wrong_evidence_time.validate_open().unwrap_err().code,
            "turn-trace-usage-report-time-mismatch"
        );

        let mut wrong_item = trace.clone();
        if let TracePayload::UsageReport {
            persisted_item_id, ..
        } = &mut wrong_item.events[1].payload
        {
            *persisted_item_id = "timeline/item".into();
        }
        assert_eq!(
            wrong_item.validate_open().unwrap_err().code,
            "turn-trace-identity-invalid"
        );

        let mut wrong_evidence_identity = trace.clone();
        if let TracePayload::UsageReport { evidence, .. } =
            &mut wrong_evidence_identity.events[1].payload
        {
            evidence.identity = Some(hash_identity("usage-authority:sha256:", '9'));
        }
        assert_eq!(
            wrong_evidence_identity.validate_open().unwrap_err().code,
            "turn-trace-usage-report-evidence-mismatch"
        );
    }

    #[test]
    fn v0_3_usage_report_rejects_semantic_tampering_and_nonmetadata_redaction() {
        let mut trace = TurnTrace::new_v0_3(binding()).unwrap();
        trace
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        trace
            .append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap();

        let mut tampered = trace.clone();
        if let TracePayload::UsageReport { report, .. } = &mut tampered.events[1].payload {
            report.as_of_ms = 21;
        }
        assert_eq!(
            tampered.validate_open().unwrap_err().code,
            "turn-trace-usage-report-identity-mismatch"
        );

        let mut nonmetadata = trace;
        if let TracePayload::UsageReport { redaction, .. } = &mut nonmetadata.events[1].payload {
            redaction.raw_bytes = 1;
        }
        assert_eq!(
            nonmetadata.validate_open().unwrap_err().code,
            "turn-trace-usage-report-redaction-invalid"
        );
    }

    #[test]
    fn v0_3_allows_at_most_one_usage_report_before_error_and_terminal() {
        let mut duplicate = TurnTrace::new_v0_3(binding()).unwrap();
        duplicate
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        duplicate
            .append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap();
        duplicate
            .append(
                "usage-report-2".into(),
                21,
                usage_report_payload(21, "item-usage-2"),
            )
            .unwrap();
        assert_eq!(
            duplicate.validate_open().unwrap_err().code,
            "turn-trace-usage-report-duplicate"
        );

        let mut after_error = TurnTrace::new_v0_3(binding()).unwrap();
        after_error
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        after_error
            .append(
                "error-1".into(),
                19,
                TracePayload::Error {
                    error_identity: "error-1".into(),
                    stable_class: ErrorClass::Provider,
                    source_class: "provider".into(),
                    retryable: true,
                    evidence: evidence(EvidenceSource::Provider),
                    redaction: redaction(),
                },
            )
            .unwrap();
        after_error
            .append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap();
        assert_eq!(
            after_error.validate_open().unwrap_err().code,
            "turn-trace-usage-report-order-invalid"
        );

        let mut valid = TurnTrace::new_v0_3(binding()).unwrap();
        valid
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        valid
            .append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap();
        valid
            .append(
                "terminal-1".into(),
                21,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        valid.validate_complete().unwrap();
    }

    #[test]
    fn usage_report_is_v0_3_or_newer_and_future_versions_fail_closed() {
        let mut legacy = TurnTrace::new_legacy(binding()).unwrap();
        assert_eq!(
            legacy
                .append(
                    "usage-report-1".into(),
                    20,
                    usage_report_payload(20, "item-usage-1"),
                )
                .unwrap_err()
                .code,
            "turn-trace-usage-report-version-invalid"
        );

        let mut v0_2 = TurnTrace::new_v0_2(binding()).unwrap();
        assert_eq!(
            v0_2.append(
                "usage-report-1".into(),
                20,
                usage_report_payload(20, "item-usage-1"),
            )
            .unwrap_err()
            .code,
            "turn-trace-usage-report-version-invalid"
        );

        let mut v0_3 = TurnTrace::new_v0_3(binding()).unwrap();
        v0_3.append(
            "usage-report-1".into(),
            20,
            usage_report_payload(20, "item-usage-1"),
        )
        .unwrap();

        let mut v0_4 = TurnTrace::new_v0_4(binding()).unwrap();
        v0_4.append(
            "usage-report-1".into(),
            20,
            usage_report_payload(20, "item-usage-1"),
        )
        .unwrap();

        let mut v0_5 = TurnTrace::new(binding()).unwrap();
        v0_5.append(
            "usage-report-1".into(),
            20,
            usage_report_payload(20, "item-usage-1"),
        )
        .unwrap();

        let mut future = TurnTrace::new(binding()).unwrap();
        future.schema_version = "turn-trace/0.6".into();
        assert_eq!(
            future.validate_open().unwrap_err().code,
            "turn-trace-schema-invalid"
        );
    }

    #[test]
    fn deserialization_rechecks_schema_and_content_flag() {
        let mut value = serde_json::to_value(TurnTrace::new(binding()).unwrap()).unwrap();
        value["schema_version"] = json!("turn-trace/9.9");
        let trace: TurnTrace = serde_json::from_value(value).unwrap();
        assert_eq!(
            trace.validate_open().unwrap_err().code,
            "turn-trace-schema-invalid"
        );
    }

    #[test]
    fn version_contracts_reject_cross_version_fields_and_require_one_v0_2_intent() {
        let mut missing_intent = TurnTrace::new_v0_4(binding()).unwrap();
        missing_intent
            .append(
                "terminal-1".into(),
                10,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        assert_eq!(
            missing_intent.validate_complete().unwrap_err().code,
            "turn-trace-intent-count-invalid"
        );

        let mut duplicate_intent = TurnTrace::new_v0_4(binding()).unwrap();
        for (event_id, byte) in [("intent-1", '1'), ("intent-2", '2')] {
            let mut payload = intent_payload(
                SessionMode::Work,
                TurnKind::ReadOnlyInspection,
                TurnAccess::ReadOnly,
            );
            if let TracePayload::Intent {
                intent_identity, ..
            } = &mut payload
            {
                *intent_identity = hash_identity("sha256:", byte);
            }
            duplicate_intent
                .append(event_id.into(), 10, payload)
                .unwrap();
        }
        assert_eq!(
            duplicate_intent.validate_open().unwrap_err().code,
            "turn-trace-intent-count-invalid"
        );

        let mut legacy = TurnTrace::new_legacy(binding()).unwrap();
        assert_eq!(
            legacy
                .append(
                    "intent-1".into(),
                    10,
                    intent_payload(
                        SessionMode::Work,
                        TurnKind::ReadOnlyInspection,
                        TurnAccess::ReadOnly,
                    ),
                )
                .unwrap_err()
                .code,
            "turn-trace-v0-1-intent-invalid"
        );

        let mut v0_2_legacy_fields = TurnTrace::new_v0_2(binding()).unwrap();
        assert_eq!(
            v0_2_legacy_fields
                .append("terminal-1".into(), 10, legacy_completed_terminal())
                .unwrap_err()
                .code,
            "turn-trace-v0-2-legacy-completion-invalid"
        );

        let mut v0_1_completion = TurnTrace::new_legacy(binding()).unwrap();
        assert_eq!(
            v0_1_completion
                .append(
                    "terminal-1".into(),
                    10,
                    completed_terminal(chat_completion()),
                )
                .unwrap_err()
                .code,
            "turn-trace-v0-1-completion-domain-invalid"
        );
    }

    #[test]
    fn chat_and_read_only_completed_turns_use_explicit_nonmutation_domains() {
        let mut chat_binding = binding();
        chat_binding.project_id = None;
        let mut chat = TurnTrace::new_v0_4(chat_binding).unwrap();
        chat.append(
            "intent-1".into(),
            10,
            intent_payload(
                SessionMode::Chat,
                TurnKind::Conversation,
                TurnAccess::NonMutating,
            ),
        )
        .unwrap();
        chat.append(
            "terminal-1".into(),
            11,
            completed_terminal(chat_completion()),
        )
        .unwrap();
        chat.validate_complete().unwrap();

        let mut read_only = TurnTrace::new_v0_4(binding()).unwrap();
        read_only
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        read_only
            .append(
                "terminal-1".into(),
                11,
                completed_terminal(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            )
            .unwrap();
        // Provider completion is a lifecycle fact. Unknown verification does
        // not become a fabricated success claim.
        read_only.validate_complete().unwrap();

        let mut invalid_chat = chat.clone();
        if let TracePayload::Terminal { evidence, .. } =
            &mut invalid_chat.events.last_mut().unwrap().payload
        {
            evidence.completion.as_mut().unwrap().verification = CompletionDomain::Unknown {
                evidence: unknown_evidence(EvidenceSource::Runtime),
            };
        }
        assert_eq!(
            invalid_chat.validate_complete().unwrap_err().code,
            "turn-trace-chat-completion-domain-invalid"
        );

        let mut mismatched_intent = read_only;
        if let TracePayload::Terminal { evidence, .. } =
            &mut mismatched_intent.events.last_mut().unwrap().payload
        {
            evidence.completion.as_mut().unwrap().intent_identity = hash_identity("sha256:", '9');
        }
        assert_eq!(
            mismatched_intent.validate_complete().unwrap_err().code,
            "turn-trace-completion-intent-mismatch"
        );
    }

    #[test]
    fn completed_mutation_requires_observed_workspace_matching_an_applied_change() {
        let workspace_identity = hash_identity("sha256:", 'c');
        let mut mutation = TurnTrace::new_v0_4(binding()).unwrap();
        mutation
            .append(
                "intent-1".into(),
                10,
                intent_payload(SessionMode::Work, TurnKind::Mutation, TurnAccess::Mutation),
            )
            .unwrap();
        mutation
            .append(
                "change-1".into(),
                11,
                TracePayload::Change {
                    change_identity: "change-1".into(),
                    state: ChangeState::Applied,
                    changed_file_count: 1,
                    changed_bytes: 12,
                    workspace_identity: workspace_identity.clone(),
                    git_state_identity: None,
                    patch_identity: None,
                    evidence: evidence(EvidenceSource::Workspace),
                    redaction: redaction(),
                },
            )
            .unwrap();
        mutation
            .append(
                "terminal-1".into(),
                12,
                completed_terminal(CompletionEvidence {
                    intent_identity: hash_identity("sha256:", '1'),
                    workspace_change: observed(EvidenceSource::Workspace, 'c', 1),
                    git_change: applicable(EvidenceSource::Runtime, 'd'),
                    verification: CompletionDomain::Unknown {
                        evidence: unknown_evidence(EvidenceSource::Runtime),
                    },
                }),
            )
            .unwrap();
        mutation.validate_complete().unwrap();

        let mut no_match = mutation.clone();
        if let TracePayload::Terminal { evidence, .. } =
            &mut no_match.events.last_mut().unwrap().payload
        {
            evidence.completion.as_mut().unwrap().workspace_change =
                observed(EvidenceSource::Workspace, '9', 1);
        }
        assert_eq!(
            no_match.validate_complete().unwrap_err().code,
            "turn-trace-mutation-change-evidence-missing"
        );

        let mut not_observed = mutation.clone();
        if let TracePayload::Terminal { evidence, .. } =
            &mut not_observed.events.last_mut().unwrap().payload
        {
            evidence.completion.as_mut().unwrap().workspace_change =
                applicable(EvidenceSource::Runtime, '8');
        }
        assert_eq!(
            not_observed.validate_complete().unwrap_err().code,
            "turn-trace-mutation-workspace-observation-missing"
        );

        let mut read_only_change = mutation;
        if let TracePayload::Intent {
            session_mode,
            turn_kind,
            access,
            ..
        } = &mut read_only_change.events[0].payload
        {
            *session_mode = SessionMode::Work;
            *turn_kind = TurnKind::ReadOnlyInspection;
            *access = TurnAccess::ReadOnly;
        }
        assert_eq!(
            read_only_change.validate_complete().unwrap_err().code,
            "turn-trace-applied-change-intent-invalid"
        );
    }

    #[test]
    fn completion_domain_states_enforce_authority_identity_count_and_source() {
        let mut applicable_unknown = applicable(EvidenceSource::Runtime, '1');
        if let CompletionDomain::Applicable { evidence } = &mut applicable_unknown {
            *evidence = unknown_evidence(EvidenceSource::Runtime);
        }
        assert_eq!(
            applicable_unknown
                .validate(CompletionDomainKind::Verification)
                .unwrap_err()
                .code,
            "turn-trace-domain-applicability-not-observed"
        );

        let unknown_observed = CompletionDomain::Unknown {
            evidence: evidence(EvidenceSource::Runtime),
        };
        assert_eq!(
            unknown_observed
                .validate(CompletionDomainKind::Verification)
                .unwrap_err()
                .code,
            "turn-trace-domain-unknown-authority-invalid"
        );

        let zero = observed(EvidenceSource::Workspace, '2', 0);
        assert_eq!(
            zero.validate(CompletionDomainKind::WorkspaceChange)
                .unwrap_err()
                .code,
            "turn-trace-domain-observation-count-invalid"
        );

        let wrong_source = not_applicable(EvidenceSource::Provider, '3');
        assert_eq!(
            wrong_source
                .validate(CompletionDomainKind::GitChange)
                .unwrap_err()
                .code,
            "turn-trace-source-mismatch"
        );
    }

    #[test]
    fn noncompleted_v0_2_turns_cannot_claim_completion_domains() {
        let mut trace = TurnTrace::new_v0_4(binding()).unwrap();
        trace
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        let terminal = TracePayload::Terminal {
            state: TerminalState::Interrupted,
            evidence: TerminalEvidence {
                workspace_identity: None,
                git_state_identity: None,
                verification_identity: None,
                observed_verification_count: 0,
                evidence: evidence(EvidenceSource::Provider),
                completion: Some(read_only_completion(CompletionDomain::Unknown {
                    evidence: unknown_evidence(EvidenceSource::Runtime),
                })),
            },
            redaction: redaction(),
        };
        assert_eq!(
            trace
                .append("terminal-1".into(), 11, terminal)
                .unwrap_err()
                .code,
            "turn-trace-noncompleted-domains-invalid"
        );

        let mut valid = TurnTrace::new_v0_4(binding()).unwrap();
        valid
            .append(
                "intent-1".into(),
                10,
                intent_payload(
                    SessionMode::Work,
                    TurnKind::ReadOnlyInspection,
                    TurnAccess::ReadOnly,
                ),
            )
            .unwrap();
        valid
            .append(
                "terminal-1".into(),
                11,
                TracePayload::Terminal {
                    state: TerminalState::Interrupted,
                    evidence: TerminalEvidence {
                        workspace_identity: None,
                        git_state_identity: None,
                        verification_identity: None,
                        observed_verification_count: 0,
                        evidence: evidence(EvidenceSource::Provider),
                        completion: None,
                    },
                    redaction: redaction(),
                },
            )
            .unwrap();
        valid.validate_complete().unwrap();
    }

    #[test]
    fn versioned_serialization_preserves_legacy_v0_2_and_v0_3_golden_shapes() {
        let mut legacy = TurnTrace::new_legacy(binding()).unwrap();
        legacy
            .append("terminal-1".into(), 10, legacy_completed_terminal())
            .unwrap();
        legacy.validate_complete().unwrap();
        assert_eq!(
            legacy.metadata_identity().unwrap(),
            "turn-trace:sha256:987ac38d2f5f7e9cadd5fc63fe114497889baa8830c76364977d96b6d101f785"
        );
        let legacy_value = serde_json::to_value(&legacy).unwrap();
        assert_eq!(legacy_value["schema_version"], LEGACY_SCHEMA_VERSION);
        assert!(legacy_value["events"][0]["payload"]["evidence"]
            .get("completion")
            .is_none());
        assert_eq!(
            legacy_value["events"][0]["payload"]["evidence"]["observed_verification_count"],
            1
        );
        let legacy_round_trip: TurnTrace = serde_json::from_value(legacy_value).unwrap();
        assert_eq!(legacy_round_trip, legacy);

        let mut v0_2 = TurnTrace::new_v0_2(binding()).unwrap();
        v0_2.append(
            "intent-1".into(),
            10,
            intent_payload(
                SessionMode::Work,
                TurnKind::ReadOnlyInspection,
                TurnAccess::ReadOnly,
            ),
        )
        .unwrap();
        v0_2.append(
            "terminal-1".into(),
            11,
            completed_terminal(read_only_completion(CompletionDomain::Unknown {
                evidence: unknown_evidence(EvidenceSource::Runtime),
            })),
        )
        .unwrap();
        v0_2.validate_complete().unwrap();
        assert_eq!(
            v0_2.metadata_identity().unwrap(),
            "turn-trace:sha256:a9f787c2e3e40c6adf4ee8bf15a2bb46c9865a4291ff4b45ac4ea3f884b9aaa7"
        );
        let value = serde_json::to_value(&v0_2).unwrap();
        let v0_2_golden = json!({
            "schema_version": "turn-trace/0.2",
            "binding": {
                "session_id": "session-1",
                "turn_id": "turn-1",
                "project_id": "project-1",
                "environment_identity": hash_identity("environment:sha256:", 'a')
            },
            "events": [
                {
                    "event_id": "intent-1",
                    "sequence": 1,
                    "at_ms": 10,
                    "payload": {
                        "kind": "intent",
                        "session_mode": "work",
                        "turn_kind": "read-only-inspection",
                        "access": "read-only",
                        "intent_identity": hash_identity("sha256:", '1'),
                        "evidence": {
                            "authority": "observed",
                            "source": "runtime",
                            "identity": hash_identity("sha256:", 'b'),
                            "observed_at_ms": 10
                        },
                        "redaction": {
                            "content_included": false,
                            "raw_bytes": 0,
                            "retained_bytes": 0,
                            "redacted_fields": 0,
                            "omitted_fields": 0
                        }
                    }
                },
                {
                    "event_id": "terminal-1",
                    "sequence": 2,
                    "at_ms": 11,
                    "payload": {
                        "kind": "terminal",
                        "state": "completed",
                        "evidence": {
                            "observed_verification_count": 0,
                            "evidence": {
                                "authority": "observed",
                                "source": "runtime",
                                "identity": hash_identity("sha256:", 'b'),
                                "observed_at_ms": 10
                            },
                            "completion": {
                                "intent_identity": hash_identity("sha256:", '1'),
                                "workspace_change": {
                                    "state": "not-applicable",
                                    "evidence": {
                                        "authority": "observed",
                                        "source": "runtime",
                                        "identity": hash_identity("sha256:", '2'),
                                        "observed_at_ms": 10
                                    }
                                },
                                "git_change": {
                                    "state": "not-applicable",
                                    "evidence": {
                                        "authority": "observed",
                                        "source": "runtime",
                                        "identity": hash_identity("sha256:", '3'),
                                        "observed_at_ms": 10
                                    }
                                },
                                "verification": {
                                    "state": "unknown",
                                    "evidence": {
                                        "authority": "unknown",
                                        "source": "runtime"
                                    }
                                }
                            }
                        },
                        "redaction": {
                            "content_included": false,
                            "raw_bytes": 0,
                            "retained_bytes": 0,
                            "redacted_fields": 0,
                            "omitted_fields": 0
                        }
                    }
                }
            ]
        });
        assert_eq!(value, v0_2_golden);
        let round_trip: TurnTrace = serde_json::from_value(value).unwrap();
        round_trip.validate_complete().unwrap();
        assert_eq!(round_trip, v0_2);

        let mut v0_3 = TurnTrace::new_v0_3(binding()).unwrap();
        v0_3.append(
            "intent-1".into(),
            10,
            intent_payload(
                SessionMode::Work,
                TurnKind::ReadOnlyInspection,
                TurnAccess::ReadOnly,
            ),
        )
        .unwrap();
        v0_3.append(
            "usage-report-1".into(),
            20,
            usage_report_payload(20, "item-usage-1"),
        )
        .unwrap();
        v0_3.append(
            "terminal-1".into(),
            21,
            completed_terminal(read_only_completion(CompletionDomain::Unknown {
                evidence: unknown_evidence(EvidenceSource::Runtime),
            })),
        )
        .unwrap();
        v0_3.validate_complete().unwrap();
        let report = usage_report(20);
        let report_identity = report.metadata_identity().unwrap();
        assert_eq!(
            report_identity,
            "usage-authority:sha256:d6830e09f43717129286d1d84741811b352f30ba9bec54bef25c7f5e4918b39d"
        );
        assert_eq!(
            v0_3.metadata_identity().unwrap(),
            "turn-trace:sha256:60b3d5716ad0d7866f55746ab3fbc92eb79845db84c7102817cb18defeb5ec19"
        );

        let mut v0_3_golden = v0_2_golden;
        v0_3_golden["schema_version"] = json!("turn-trace/0.3");
        let events = v0_3_golden["events"].as_array_mut().unwrap();
        let mut terminal = events.pop().unwrap();
        terminal["sequence"] = json!(3);
        terminal["at_ms"] = json!(21);
        events.push(json!({
            "event_id": "usage-report-1",
            "sequence": 2,
            "at_ms": 20,
            "payload": {
                "kind": "usage-report",
                "report_identity": report_identity,
                "persisted_item_id": "item-usage-1",
                "scope": "provider-thread",
                "accounting": "absolute-snapshot",
                "attempt_attribution": "unavailable",
                "retry_attribution": "unavailable",
                "report": serde_json::to_value(report).unwrap(),
                "evidence": {
                    "authority": "observed",
                    "source": "usage-provider",
                    "identity": report_identity,
                    "observed_at_ms": 20
                },
                "redaction": {
                    "content_included": false,
                    "raw_bytes": 0,
                    "retained_bytes": 0,
                    "redacted_fields": 0,
                    "omitted_fields": 0
                }
            }
        }));
        events.push(terminal);
        let v0_3_value = serde_json::to_value(&v0_3).unwrap();
        assert_eq!(v0_3_value, v0_3_golden);
        let v0_3_round_trip: TurnTrace = serde_json::from_value(v0_3_value).unwrap();
        v0_3_round_trip.validate_complete().unwrap();
        assert_eq!(v0_3_round_trip, v0_3);
    }
}
