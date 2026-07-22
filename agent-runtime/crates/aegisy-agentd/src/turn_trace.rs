//! Content-free, source-qualified metadata for one local Agent turn.
//!
//! This is an internal observability contract.  It records identities,
//! classifications, timing, counts, and hashes only; it does not carry a
//! prompt, file body, path, command line, terminal output, diff, or secret.
//! A trace is evidence about a turn, not permission to run one.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const LEGACY_SCHEMA_VERSION: &str = "turn-trace/0.1";
pub const SCHEMA_VERSION: &str = "turn-trace/0.2";

const MAX_ID_BYTES: usize = 128;
const MAX_LABEL_BYTES: usize = 96;
const MAX_EVENTS: usize = 512;
const MAX_SOURCE_BYTES: u64 = 16 * 1024 * 1024;
const MAX_CONTEXT_ITEMS: u32 = 16_384;
const MAX_CHANGED_FILES: u32 = 100_000;
const MAX_TEST_CASES: u32 = 100_000;
const MAX_DOMAIN_OBSERVATIONS: u32 = 100_000;

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
    Cancelled,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageMetric {
    Token,
    Context,
    Cost,
    Reasoning,
    Latency,
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
            SCHEMA_VERSION => {
                if self.workspace_identity.is_some()
                    || self.git_state_identity.is_some()
                    || self.verification_identity.is_some()
                    || self.observed_verification_count != 0
                {
                    return Err(error(
                        "turn-trace-v0-2-legacy-completion-invalid",
                        "turn-trace/0.2 cannot contain legacy completion evidence",
                    ));
                }
                match (state, &self.completion) {
                    (TerminalState::Completed, Some(completion)) => completion.validate()?,
                    (TerminalState::Completed, None) => {
                        return Err(error(
                            "turn-trace-completion-domains-missing",
                            "completed turn-trace/0.2 requires explicit completion domains",
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
        input_identity: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        output_identity: Option<String>,
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
                if schema_version != SCHEMA_VERSION {
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
                input_identity,
                output_identity,
                evidence,
                redaction,
                ..
            } => {
                validate_identity(tool_identity, "tool identity")?;
                validate_identity(action_identity, "tool action identity")?;
                validate_optional_identity(input_identity, "tool input identity")?;
                validate_optional_identity(output_identity, "tool output identity")?;
                validate_source(evidence, &[EvidenceSource::ToolRuntime])?;
                validate_common(evidence, redaction)?;
            }
            Self::Approval {
                approval_identity,
                requirement_identity,
                authority_identity,
                evidence,
                redaction,
                ..
            } => {
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
        self.payload.validate(schema_version)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TurnTrace {
    pub schema_version: String,
    pub binding: TraceBinding,
    pub events: Vec<TraceEvent>,
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
            LEGACY_SCHEMA_VERSION | SCHEMA_VERSION
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
        let mut intent = None;
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
                TracePayload::Change {
                    state: ChangeState::Applied,
                    workspace_identity,
                    ..
                } => applied_workspace_identities.push(workspace_identity.as_str()),
                _ => {}
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
        match self.schema_version.as_str() {
            LEGACY_SCHEMA_VERSION if intent_count != 0 => {
                return Err(error(
                    "turn-trace-v0-1-intent-invalid",
                    "turn-trace/0.1 cannot contain an intent event",
                ));
            }
            SCHEMA_VERSION if intent_count != 1 => {
                return Err(error(
                    "turn-trace-intent-count-invalid",
                    "turn-trace/0.2 requires exactly one intent event",
                ));
            }
            _ => {}
        }
        if self.schema_version == SCHEMA_VERSION {
            let intent = intent.expect("validated turn-trace/0.2 intent count");
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

    #[test]
    fn valid_trace_covers_source_qualified_metadata_and_terminal_evidence() {
        let mut trace = TurnTrace::new(binding()).unwrap();
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
        let mut missing_intent = TurnTrace::new(binding()).unwrap();
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

        let mut duplicate_intent = TurnTrace::new(binding()).unwrap();
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

        let mut v0_2_legacy_fields = TurnTrace::new(binding()).unwrap();
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
        let mut chat = TurnTrace::new(chat_binding).unwrap();
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

        let mut read_only = TurnTrace::new(binding()).unwrap();
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
        let mut mutation = TurnTrace::new(binding()).unwrap();
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
        let mut trace = TurnTrace::new(binding()).unwrap();
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

        let mut valid = TurnTrace::new(binding()).unwrap();
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
    fn versioned_serialization_preserves_legacy_and_v0_2_golden_shapes() {
        let mut legacy = TurnTrace::new_legacy(binding()).unwrap();
        legacy
            .append("terminal-1".into(), 10, legacy_completed_terminal())
            .unwrap();
        legacy.validate_complete().unwrap();
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

        let mut v0_2 = TurnTrace::new(binding()).unwrap();
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
        let value = serde_json::to_value(&v0_2).unwrap();
        assert_eq!(
            value,
            json!({
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
            })
        );
        let round_trip: TurnTrace = serde_json::from_value(value).unwrap();
        round_trip.validate_complete().unwrap();
        assert_eq!(round_trip, v0_2);
    }
}
