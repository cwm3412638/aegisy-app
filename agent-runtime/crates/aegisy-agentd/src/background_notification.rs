//! Content-free notification intents for durable background work.
//!
//! These records define deduplication and evidence binding only. They do not
//! render user-visible text or grant permission to call a platform notification API.

use crate::background_job::{BackgroundJobRequest, BackgroundJobState, BackgroundJobStatus};
use crate::child_budget::{
    BudgetDimension, BudgetState, ChildBudgetSnapshot, SCHEMA_VERSION as BUDGET_SCHEMA_VERSION,
};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const SCHEMA_VERSION: &str = "background-job-notification-intent/0.1";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundNotificationError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundNotificationError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundNotificationKind {
    Completed,
    Failed,
    ApprovalNeeded,
    BudgetExhausted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundNotificationUrgency {
    Normal,
    High,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundNotificationIntent {
    pub schema_version: String,
    pub kind: BackgroundNotificationKind,
    pub urgency: BackgroundNotificationUrgency,
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub job_generation: u64,
    pub job_status: BackgroundJobStatus,
    pub evidence_identity: String,
    pub budget_snapshot_identity: Option<String>,
    pub exhausted_dimensions: Vec<BudgetDimension>,
    pub created_at_ms: u64,
    pub deduplication_identity: String,
    pub content_included: bool,
    pub delivery_available: bool,
    pub delivery_attempted: bool,
    pub platform_delivery_authority: bool,
    pub intent_identity: String,
}

#[derive(Serialize)]
struct DeduplicationBinding<'a> {
    schema_version: &'a str,
    kind: BackgroundNotificationKind,
    job_id: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    evidence_identity: &'a str,
    budget_snapshot_identity: Option<&'a str>,
    exhausted_dimensions: &'a [BudgetDimension],
}

#[derive(Serialize)]
struct IntentIdentityBinding<'a> {
    schema_version: &'a str,
    kind: BackgroundNotificationKind,
    urgency: BackgroundNotificationUrgency,
    job_id: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    root_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    job_status: BackgroundJobStatus,
    evidence_identity: &'a str,
    budget_snapshot_identity: Option<&'a str>,
    exhausted_dimensions: &'a [BudgetDimension],
    created_at_ms: u64,
    deduplication_identity: &'a str,
    content_included: bool,
    delivery_available: bool,
    delivery_attempted: bool,
    platform_delivery_authority: bool,
}

impl BackgroundNotificationIntent {
    pub fn from_job_state(
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        created_at_ms: u64,
    ) -> Result<Self, BackgroundNotificationError> {
        validate_job(request, state, created_at_ms)?;
        let kind = match state.status {
            BackgroundJobStatus::Completed => BackgroundNotificationKind::Completed,
            BackgroundJobStatus::Failed => BackgroundNotificationKind::Failed,
            BackgroundJobStatus::WaitingApproval => BackgroundNotificationKind::ApprovalNeeded,
            _ => {
                return Err(error(
                    "background-notification-state-unsupported",
                    "background job state does not produce a notification intent",
                ));
            }
        };
        let evidence_identity = match kind {
            BackgroundNotificationKind::Completed => state.result_reference.clone(),
            BackgroundNotificationKind::Failed => state
                .attempts
                .last()
                .and_then(|attempt| attempt.terminal_evidence_identity.clone()),
            BackgroundNotificationKind::ApprovalNeeded => state.approval_identity.clone(),
            BackgroundNotificationKind::BudgetExhausted => None,
        }
        .ok_or_else(|| {
            error(
                "background-notification-evidence-missing",
                "background notification evidence is missing",
            )
        })?;
        build_intent(
            request,
            state,
            kind,
            evidence_identity,
            None,
            Vec::new(),
            created_at_ms,
        )
    }

    pub fn from_budget_exhaustion(
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        budget: &ChildBudgetSnapshot,
        created_at_ms: u64,
    ) -> Result<Self, BackgroundNotificationError> {
        validate_job(request, state, created_at_ms)?;
        if budget.validate().is_err()
            || budget.schema_version != BUDGET_SCHEMA_VERSION
            || request.child_task_identity.as_deref() != Some(budget.task_identity.as_str())
            || budget.state != BudgetState::Exhausted
            || budget.exhausted_dimensions.is_empty()
            || budget.exhausted_dimensions.len() > 7
            || budget.permission_granted
            || budget.execution_available
        {
            return Err(error(
                "background-notification-budget-invalid",
                "background budget exhaustion evidence is invalid",
            ));
        }
        let dimensions = budget
            .exhausted_dimensions
            .iter()
            .copied()
            .collect::<BTreeSet<_>>();
        if dimensions.len() != budget.exhausted_dimensions.len() {
            return Err(error(
                "background-notification-budget-invalid",
                "background budget exhaustion dimensions are invalid",
            ));
        }
        let budget_bytes = to_vec(budget).map_err(|_| {
            error(
                "background-notification-budget-serialize-failed",
                "background budget evidence could not be serialized",
            )
        })?;
        let budget_identity = format!(
            "background-job-budget-snapshot:sha256:{:x}",
            Sha256::digest(budget_bytes)
        );
        build_intent(
            request,
            state,
            BackgroundNotificationKind::BudgetExhausted,
            budget_identity.clone(),
            Some(budget_identity),
            budget.exhausted_dimensions.clone(),
            created_at_ms.max(budget.observed_at_ms),
        )
    }

    pub fn validate(&self) -> Result<(), BackgroundNotificationError> {
        let unique_dimensions = self
            .exhausted_dimensions
            .iter()
            .copied()
            .collect::<BTreeSet<_>>();
        if self.schema_version != SCHEMA_VERSION
            || [
                &self.job_id,
                &self.session_id,
                &self.project_id,
                &self.root_id,
            ]
            .into_iter()
            .any(|value| !valid_identifier(value))
            || !valid_identity(&self.request_identity, "background-job:sha256:")
            || !valid_identity(&self.state_identity, "background-job-state:sha256:")
            || !valid_evidence_identity(&self.evidence_identity)
            || self
                .budget_snapshot_identity
                .as_deref()
                .is_some_and(|value| {
                    !valid_identity(value, "background-job-budget-snapshot:sha256:")
                })
            || self.exhausted_dimensions.len() > 7
            || unique_dimensions.len() != self.exhausted_dimensions.len()
            || self.created_at_ms == 0
            || !valid_identity(
                &self.deduplication_identity,
                "background-job-notification-dedup:sha256:",
            )
            || self.content_included
            || self.delivery_available
            || self.delivery_attempted
            || self.platform_delivery_authority
        {
            return Err(error(
                "background-notification-intent-invalid",
                "background notification intent invariant is invalid",
            ));
        }
        let kind_valid = match self.kind {
            BackgroundNotificationKind::Completed => {
                self.job_status == BackgroundJobStatus::Completed
                    && self.urgency == BackgroundNotificationUrgency::Normal
                    && self.budget_snapshot_identity.is_none()
                    && self.exhausted_dimensions.is_empty()
            }
            BackgroundNotificationKind::Failed => {
                self.job_status == BackgroundJobStatus::Failed
                    && self.urgency == BackgroundNotificationUrgency::High
                    && self.budget_snapshot_identity.is_none()
                    && self.exhausted_dimensions.is_empty()
            }
            BackgroundNotificationKind::ApprovalNeeded => {
                self.job_status == BackgroundJobStatus::WaitingApproval
                    && self.urgency == BackgroundNotificationUrgency::High
                    && self.budget_snapshot_identity.is_none()
                    && self.exhausted_dimensions.is_empty()
            }
            BackgroundNotificationKind::BudgetExhausted => {
                self.urgency == BackgroundNotificationUrgency::High
                    && self.budget_snapshot_identity.as_deref()
                        == Some(self.evidence_identity.as_str())
                    && !self.exhausted_dimensions.is_empty()
            }
        };
        if !kind_valid {
            return Err(error(
                "background-notification-kind-invalid",
                "background notification kind does not match its evidence",
            ));
        }
        if self.deduplication_identity != deduplication_identity(self)?
            || self.intent_identity != intent_identity(self)?
        {
            return Err(error(
                "background-notification-identity-invalid",
                "background notification identity is invalid",
            ));
        }
        Ok(())
    }
}

fn validate_job(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
    created_at_ms: u64,
) -> Result<(), BackgroundNotificationError> {
    request.validate().map_err(|_| {
        error(
            "background-notification-request-invalid",
            "background notification request is invalid",
        )
    })?;
    state.validate(request).map_err(|_| {
        error(
            "background-notification-state-invalid",
            "background notification state is invalid",
        )
    })?;
    if created_at_ms < state.updated_at_ms {
        return Err(error(
            "background-notification-time-invalid",
            "background notification time precedes its evidence",
        ));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn build_intent(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
    kind: BackgroundNotificationKind,
    evidence_identity: String,
    budget_snapshot_identity: Option<String>,
    exhausted_dimensions: Vec<BudgetDimension>,
    created_at_ms: u64,
) -> Result<BackgroundNotificationIntent, BackgroundNotificationError> {
    let request_identity = request.identity().map_err(|_| {
        error(
            "background-notification-request-invalid",
            "background notification request identity is invalid",
        )
    })?;
    let state_identity = state.identity(request).map_err(|_| {
        error(
            "background-notification-state-invalid",
            "background notification state identity is invalid",
        )
    })?;
    let urgency = if kind == BackgroundNotificationKind::Completed {
        BackgroundNotificationUrgency::Normal
    } else {
        BackgroundNotificationUrgency::High
    };
    let mut intent = BackgroundNotificationIntent {
        schema_version: SCHEMA_VERSION.into(),
        kind,
        urgency,
        job_id: request.job_id.clone(),
        session_id: request.session_id.clone(),
        project_id: request.project_id.clone(),
        root_id: request.root_id.clone(),
        request_identity,
        state_identity,
        job_generation: state.generation,
        job_status: state.status,
        evidence_identity,
        budget_snapshot_identity,
        exhausted_dimensions,
        created_at_ms,
        deduplication_identity: String::new(),
        content_included: false,
        delivery_available: false,
        delivery_attempted: false,
        platform_delivery_authority: false,
        intent_identity: String::new(),
    };
    intent.deduplication_identity = deduplication_identity(&intent)?;
    intent.intent_identity = intent_identity(&intent)?;
    intent.validate()?;
    Ok(intent)
}

fn deduplication_identity(
    intent: &BackgroundNotificationIntent,
) -> Result<String, BackgroundNotificationError> {
    let bytes = to_vec(&DeduplicationBinding {
        schema_version: &intent.schema_version,
        kind: intent.kind,
        job_id: &intent.job_id,
        session_id: &intent.session_id,
        project_id: &intent.project_id,
        request_identity: &intent.request_identity,
        state_identity: &intent.state_identity,
        job_generation: intent.job_generation,
        evidence_identity: &intent.evidence_identity,
        budget_snapshot_identity: intent.budget_snapshot_identity.as_deref(),
        exhausted_dimensions: &intent.exhausted_dimensions,
    })
    .map_err(|_| {
        error(
            "background-notification-dedup-failed",
            "background notification deduplication identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-notification-dedup:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn intent_identity(
    intent: &BackgroundNotificationIntent,
) -> Result<String, BackgroundNotificationError> {
    let bytes = to_vec(&IntentIdentityBinding {
        schema_version: &intent.schema_version,
        kind: intent.kind,
        urgency: intent.urgency,
        job_id: &intent.job_id,
        session_id: &intent.session_id,
        project_id: &intent.project_id,
        root_id: &intent.root_id,
        request_identity: &intent.request_identity,
        state_identity: &intent.state_identity,
        job_generation: intent.job_generation,
        job_status: intent.job_status,
        evidence_identity: &intent.evidence_identity,
        budget_snapshot_identity: intent.budget_snapshot_identity.as_deref(),
        exhausted_dimensions: &intent.exhausted_dimensions,
        created_at_ms: intent.created_at_ms,
        deduplication_identity: &intent.deduplication_identity,
        content_included: intent.content_included,
        delivery_available: intent.delivery_available,
        delivery_attempted: intent.delivery_attempted,
        platform_delivery_authority: intent.platform_delivery_authority,
    })
    .map_err(|_| {
        error(
            "background-notification-identity-failed",
            "background notification intent identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-notification-intent:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':'))
}

fn valid_evidence_identity(value: &str) -> bool {
    value.split_once(":sha256:").is_some_and(|(prefix, hex)| {
        !prefix.is_empty()
            && prefix
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte == b'-')
            && hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn valid_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn error(code: &'static str, message: &'static str) -> BackgroundNotificationError {
    BackgroundNotificationError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::background_job::{
        JobRetryPolicy, JobSchedule, JobScheduleKind, REQUEST_SCHEMA_VERSION,
    };
    use crate::child_budget::{ChildBudgetLedger, ChildBudgetRemaining, UsageSourceCounts};
    use crate::child_task::{
        ChildTaskBudget, ChildTaskRequest, ExpectedResultShape, PermissionRequest, WorkspaceScope,
        HANDOFF_SCHEMA_VERSION,
    };

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn child_request(task_id: &str) -> ChildTaskRequest {
        ChildTaskRequest {
            schema_version: crate::child_task::SCHEMA_VERSION.into(),
            task_id: task_id.into(),
            parent_session_id: "session-1".into(),
            parent_turn_id: "turn-1".into(),
            goal: "Return bounded notification evidence".into(),
            context: Vec::new(),
            workspace: WorkspaceScope {
                project_id: "project-1".into(),
                root_id: "root-1".into(),
                isolation: "read_only".into(),
                base_revision: "revision-1".into(),
            },
            tools: vec!["workspace-read".into()],
            model_profile: "agent-default".into(),
            permissions: PermissionRequest {
                profile: "read-only".into(),
                network: "none".into(),
                write: false,
            },
            budget: ChildTaskBudget {
                max_tokens: 1_000,
                max_cost_micros: 2_000,
                max_wall_time_ms: 100,
                max_turns: 3,
                max_tool_calls: 4,
                max_concurrency: 2,
            },
            expected_result: ExpectedResultShape {
                schema_version: HANDOFF_SCHEMA_VERSION.into(),
                max_summary_bytes: 8 * 1024,
                required_sections: vec!["summary".into()],
            },
        }
    }

    fn job_request(child: &ChildTaskRequest) -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: REQUEST_SCHEMA_VERSION.into(),
            job_id: "job-1".into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'a'),
            idempotency_identity: identity("idempotency:sha256:", 'b'),
            child_task_identity: Some(child.identity().unwrap()),
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 100,
                safe_retry_boundary_identity: Some(identity("retry-boundary:sha256:", 'c')),
            },
            created_at_ms: 1_000,
        }
    }

    fn running_state(request: &BackgroundJobRequest) -> BackgroundJobState {
        let mut state = BackgroundJobState::new(request, 1_000).unwrap();
        state.start(request, 1_100).unwrap();
        state
    }

    fn generation_zero_wall_time_exhaustion(child: &ChildTaskRequest) -> ChildBudgetSnapshot {
        let ledger = ChildBudgetLedger::new(child, None, 80, 1_000).unwrap();
        let mut snapshot = ledger.snapshot();
        snapshot.observed_at_ms = 1_200;
        snapshot.wall_time_ms = 200;
        snapshot.remaining = ChildBudgetRemaining {
            tokens: 1_000,
            cost_micros: 2_000,
            wall_time_ms: 0,
            turns: 3,
            tool_calls: 4,
            concurrency: 2,
            network_requests: 0,
        };
        snapshot.state = BudgetState::Exhausted;
        snapshot.warning_dimensions = Vec::new();
        snapshot.saturated_dimensions = Vec::new();
        snapshot.exhausted_dimensions = vec![BudgetDimension::WallTimeMs];
        snapshot.token_usage_sources = UsageSourceCounts::default();
        snapshot.cost_usage_sources = UsageSourceCounts::default();
        assert_eq!(snapshot.generation, 0);
        snapshot.validate().unwrap();
        snapshot
    }

    fn assert_no_delivery_authority(intent: &BackgroundNotificationIntent) {
        assert!(!intent.content_included);
        assert!(!intent.delivery_available);
        assert!(!intent.delivery_attempted);
        assert!(!intent.platform_delivery_authority);
        let value = serde_json::to_value(intent).unwrap();
        assert!(value.get("title").is_none());
        assert!(value.get("body").is_none());
        assert!(value.get("content").is_none());
    }

    #[test]
    fn completed_intent_is_content_free_and_deduplicates_one_state() {
        let child = child_request("child-completed");
        let request = job_request(&child);
        let mut state = running_state(&request);
        let result = identity("artifact:sha256:", 'd');
        state
            .complete(
                &request,
                &result,
                &identity("job-evidence:sha256:", 'e'),
                1_200,
            )
            .unwrap();

        let first = BackgroundNotificationIntent::from_job_state(&request, &state, 1_300).unwrap();
        let later = BackgroundNotificationIntent::from_job_state(&request, &state, 1_400).unwrap();
        assert_eq!(first.kind, BackgroundNotificationKind::Completed);
        assert_eq!(first.urgency, BackgroundNotificationUrgency::Normal);
        assert_eq!(first.evidence_identity, result);
        assert_eq!(first.deduplication_identity, later.deduplication_identity);
        assert_ne!(first.intent_identity, later.intent_identity);
        assert_no_delivery_authority(&first);
        first.validate().unwrap();
    }

    #[test]
    fn failed_intent_binds_terminal_attempt_evidence() {
        let child = child_request("child-failed");
        let request = job_request(&child);
        let mut state = running_state(&request);
        let evidence = identity("job-evidence:sha256:", 'f');
        state.fail(&request, &evidence, false, 1_200).unwrap();

        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_300).unwrap();
        assert_eq!(intent.kind, BackgroundNotificationKind::Failed);
        assert_eq!(intent.urgency, BackgroundNotificationUrgency::High);
        assert_eq!(intent.evidence_identity, evidence);
        assert_no_delivery_authority(&intent);
        intent.validate().unwrap();
    }

    #[test]
    fn approval_needed_intent_binds_exact_approval() {
        let child = child_request("child-approval");
        let request = job_request(&child);
        let mut state = running_state(&request);
        let approval = identity("approval:sha256:", 'a');
        state.wait_for_approval(&request, &approval, 1_200).unwrap();

        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_300).unwrap();
        assert_eq!(intent.kind, BackgroundNotificationKind::ApprovalNeeded);
        assert_eq!(intent.evidence_identity, approval);
        assert_no_delivery_authority(&intent);
        intent.validate().unwrap();
    }

    #[test]
    fn budget_exhaustion_accepts_valid_generation_zero_snapshot() {
        let child = child_request("child-budget");
        let request = job_request(&child);
        let state = running_state(&request);
        let budget = generation_zero_wall_time_exhaustion(&child);

        let intent =
            BackgroundNotificationIntent::from_budget_exhaustion(&request, &state, &budget, 1_150)
                .unwrap();
        assert_eq!(intent.kind, BackgroundNotificationKind::BudgetExhausted);
        assert_eq!(intent.created_at_ms, budget.observed_at_ms);
        assert_eq!(
            intent.exhausted_dimensions,
            vec![BudgetDimension::WallTimeMs]
        );
        assert_eq!(
            intent.budget_snapshot_identity.as_deref(),
            Some(intent.evidence_identity.as_str())
        );
        assert_no_delivery_authority(&intent);
        intent.validate().unwrap();
    }

    #[test]
    fn unsupported_state_and_identity_drift_fail_closed() {
        let child = child_request("child-invalid-state");
        let request = job_request(&child);
        let queued = BackgroundJobState::new(&request, 1_000).unwrap();
        assert_eq!(
            BackgroundNotificationIntent::from_job_state(&request, &queued, 1_100)
                .unwrap_err()
                .code,
            "background-notification-state-unsupported"
        );

        let mut completed = running_state(&request);
        completed
            .complete(
                &request,
                &identity("artifact:sha256:", 'b'),
                &identity("job-evidence:sha256:", 'c'),
                1_200,
            )
            .unwrap();
        let intent =
            BackgroundNotificationIntent::from_job_state(&request, &completed, 1_300).unwrap();
        let mut drifted = intent.clone();
        drifted.project_id = "project-2".into();
        assert_eq!(
            drifted.validate().unwrap_err().code,
            "background-notification-identity-invalid"
        );
        let mut authority = intent;
        authority.platform_delivery_authority = true;
        assert_eq!(
            authority.validate().unwrap_err().code,
            "background-notification-intent-invalid"
        );
    }

    #[test]
    fn forged_budget_accounting_and_task_binding_are_rejected() {
        let child = child_request("child-budget-invalid");
        let request = job_request(&child);
        let state = running_state(&request);
        let budget = generation_zero_wall_time_exhaustion(&child);

        let mut forged = budget.clone();
        forged.remaining.wall_time_ms = 1;
        assert_eq!(
            BackgroundNotificationIntent::from_budget_exhaustion(&request, &state, &forged, 1_300,)
                .unwrap_err()
                .code,
            "background-notification-budget-invalid"
        );

        let other_child = child_request("child-budget-other");
        let other_budget = generation_zero_wall_time_exhaustion(&other_child);
        assert_eq!(
            BackgroundNotificationIntent::from_budget_exhaustion(
                &request,
                &state,
                &other_budget,
                1_300,
            )
            .unwrap_err()
            .code,
            "background-notification-budget-invalid"
        );
    }
}
