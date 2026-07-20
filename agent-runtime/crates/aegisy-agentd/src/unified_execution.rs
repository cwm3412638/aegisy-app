//! One execution-pipeline plan shared by interactive, child, and background modes.
//!
//! The plan is a fail-closed invariant and does not grant permission or dispatch
//! authority. The current Codex read-only interactive path validates this plan;
//! child and background execution remain unadvertised and unavailable.

use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "unified-execution-plan/0.1";
const MAX_IDENTIFIER_BYTES: usize = 128;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnifiedExecutionError {
    pub code: &'static str,
    pub message: &'static str,
}

impl UnifiedExecutionError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionMode {
    Interactive,
    Child,
    Background,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionMutation {
    ReadOnly,
    WorkspaceWrite,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionStage {
    Identity,
    Reconciliation,
    Permission,
    Approval,
    Workspace,
    Budget,
    Sandbox,
    Recovery,
    DurableJob,
    Notification,
    Release,
    Dispatch,
    Observation,
    Handoff,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExecutionStageState {
    Ready,
    Missing,
    NotRequired,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionStagePlan {
    pub stage: ExecutionStage,
    pub state: ExecutionStageState,
    pub requirement: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionEnvelope {
    pub schema_version: String,
    pub execution_identity: String,
    pub mode: ExecutionMode,
    pub mutation: ExecutionMutation,
    pub session_id: String,
    pub project_id: Option<String>,
    pub child_task_identity: Option<String>,
    pub job_identity: Option<String>,
    pub unattended: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExecutionGateEvidence {
    pub identity_bound: bool,
    pub reconciliation_clear: bool,
    pub permission_ready: bool,
    pub approval_ready: bool,
    pub workspace_ready: bool,
    pub budget_ready: bool,
    pub sandbox_ready: bool,
    pub recovery_ready: bool,
    pub durable_job_ready: bool,
    pub notification_ready: bool,
    pub release_ready: bool,
}

impl ExecutionGateEvidence {
    fn current_interactive_read_only() -> Self {
        Self {
            identity_bound: true,
            reconciliation_clear: true,
            permission_ready: true,
            approval_ready: false,
            workspace_ready: true,
            budget_ready: false,
            sandbox_ready: true,
            recovery_ready: true,
            durable_job_ready: false,
            notification_ready: false,
            release_ready: false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UnifiedExecutionPlan {
    pub schema_version: String,
    pub execution_identity: String,
    pub mode: ExecutionMode,
    pub mutation: ExecutionMutation,
    pub stages: Vec<ExecutionStagePlan>,
    pub missing_requirements: Vec<String>,
    pub admission_ready: bool,
    pub permission_granted: bool,
    pub execution_available: bool,
    pub plan_identity: String,
}

#[derive(Serialize)]
struct PlanBinding<'a> {
    schema_version: &'a str,
    execution_identity: &'a str,
    mode: ExecutionMode,
    mutation: ExecutionMutation,
    stages: &'a [ExecutionStagePlan],
    missing_requirements: &'a [String],
    admission_ready: bool,
    permission_granted: bool,
    execution_available: bool,
}

pub fn plan_current_interactive_read_only(
    execution_identity: &str,
    session_id: &str,
    project_id: Option<&str>,
) -> Result<UnifiedExecutionPlan, UnifiedExecutionError> {
    plan_execution(
        &ExecutionEnvelope {
            schema_version: SCHEMA_VERSION.into(),
            execution_identity: execution_identity.into(),
            mode: ExecutionMode::Interactive,
            mutation: ExecutionMutation::ReadOnly,
            session_id: session_id.into(),
            project_id: project_id.map(str::to_owned),
            child_task_identity: None,
            job_identity: None,
            unattended: false,
        },
        ExecutionGateEvidence::current_interactive_read_only(),
    )
}

pub fn plan_execution(
    envelope: &ExecutionEnvelope,
    evidence: ExecutionGateEvidence,
) -> Result<UnifiedExecutionPlan, UnifiedExecutionError> {
    validate_envelope(envelope)?;
    let requirements = requirements(envelope);
    let mut stages = Vec::with_capacity(14);
    let mut missing_requirements = Vec::new();

    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Identity,
        "identity-bound",
        true,
        evidence.identity_bound,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Reconciliation,
        "reconciliation-clear",
        true,
        evidence.reconciliation_clear,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Permission,
        "permission-policy-ready",
        true,
        evidence.permission_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Approval,
        "approval-ready",
        requirements.approval,
        evidence.approval_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Workspace,
        requirements.workspace_requirement,
        true,
        evidence.workspace_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Budget,
        "runtime-budget-ready",
        requirements.budget,
        evidence.budget_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Sandbox,
        "sandbox-ready",
        true,
        evidence.sandbox_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Recovery,
        "recovery-clear",
        true,
        evidence.recovery_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::DurableJob,
        "durable-job-ready",
        requirements.durable_job,
        evidence.durable_job_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Notification,
        "notification-ready",
        requirements.notification,
        evidence.notification_ready,
    );
    push_gate(
        &mut stages,
        &mut missing_requirements,
        ExecutionStage::Release,
        requirements.release_requirement,
        requirements.release,
        evidence.release_ready,
    );

    let admission_ready = missing_requirements.is_empty();
    stages.push(ExecutionStagePlan {
        stage: ExecutionStage::Dispatch,
        state: if admission_ready {
            ExecutionStageState::Ready
        } else {
            ExecutionStageState::Missing
        },
        requirement: "single-dispatch-pipeline".into(),
    });
    stages.push(ExecutionStagePlan {
        stage: ExecutionStage::Observation,
        state: if admission_ready {
            ExecutionStageState::Ready
        } else {
            ExecutionStageState::Missing
        },
        requirement: "common-observation-pipeline".into(),
    });
    stages.push(ExecutionStagePlan {
        stage: ExecutionStage::Handoff,
        state: if admission_ready {
            ExecutionStageState::Ready
        } else {
            ExecutionStageState::Missing
        },
        requirement: requirements.handoff_requirement.into(),
    });

    let binding = PlanBinding {
        schema_version: SCHEMA_VERSION,
        execution_identity: &envelope.execution_identity,
        mode: envelope.mode,
        mutation: envelope.mutation,
        stages: &stages,
        missing_requirements: &missing_requirements,
        admission_ready,
        permission_granted: false,
        execution_available: false,
    };
    let bytes = to_vec(&binding).map_err(|_| {
        UnifiedExecutionError::new(
            "unified-execution-plan-serialize-failed",
            "unified execution plan could not be serialized",
        )
    })?;
    Ok(UnifiedExecutionPlan {
        schema_version: SCHEMA_VERSION.into(),
        execution_identity: envelope.execution_identity.clone(),
        mode: envelope.mode,
        mutation: envelope.mutation,
        stages,
        missing_requirements,
        admission_ready,
        permission_granted: false,
        execution_available: false,
        plan_identity: format!("unified-execution-plan:sha256:{:x}", Sha256::digest(bytes)),
    })
}

struct ModeRequirements {
    approval: bool,
    budget: bool,
    durable_job: bool,
    notification: bool,
    release: bool,
    workspace_requirement: &'static str,
    release_requirement: &'static str,
    handoff_requirement: &'static str,
}

fn requirements(envelope: &ExecutionEnvelope) -> ModeRequirements {
    let autonomous = matches!(
        envelope.mode,
        ExecutionMode::Child | ExecutionMode::Background
    );
    ModeRequirements {
        approval: envelope.mutation == ExecutionMutation::WorkspaceWrite || envelope.unattended,
        budget: autonomous,
        durable_job: envelope.mode == ExecutionMode::Background,
        notification: envelope.mode == ExecutionMode::Background,
        release: autonomous,
        workspace_requirement: if envelope.mutation == ExecutionMutation::WorkspaceWrite {
            "dedicated-workspace-ready"
        } else {
            "read-only-workspace-ready"
        },
        release_requirement: match envelope.mode {
            ExecutionMode::Interactive => "interactive-release-gate",
            ExecutionMode::Child => "multi-agent-release-gate",
            ExecutionMode::Background => "background-job-release-gate",
        },
        handoff_requirement: match envelope.mode {
            ExecutionMode::Interactive => "interactive-turn-terminal-event",
            ExecutionMode::Child => "bounded-child-handoff",
            ExecutionMode::Background => "durable-job-terminal-event",
        },
    }
}

fn push_gate(
    stages: &mut Vec<ExecutionStagePlan>,
    missing: &mut Vec<String>,
    stage: ExecutionStage,
    requirement: &str,
    required: bool,
    ready: bool,
) {
    let state = if !required {
        ExecutionStageState::NotRequired
    } else if ready {
        ExecutionStageState::Ready
    } else {
        missing.push(requirement.into());
        ExecutionStageState::Missing
    };
    stages.push(ExecutionStagePlan {
        stage,
        state,
        requirement: requirement.into(),
    });
}

fn validate_envelope(envelope: &ExecutionEnvelope) -> Result<(), UnifiedExecutionError> {
    if envelope.schema_version != SCHEMA_VERSION
        || !valid_sha_identity(&envelope.execution_identity, "execution:sha256:")
    {
        return Err(UnifiedExecutionError::new(
            "unified-execution-envelope-invalid",
            "unified execution envelope is invalid",
        ));
    }
    validate_identifier(&envelope.session_id)?;
    if let Some(project_id) = envelope.project_id.as_deref() {
        validate_identifier(project_id)?;
    }
    match envelope.mode {
        ExecutionMode::Interactive => {
            if envelope.child_task_identity.is_some()
                || envelope.job_identity.is_some()
                || envelope.unattended
            {
                return Err(mode_error());
            }
        }
        ExecutionMode::Child => {
            if envelope.project_id.is_none()
                || envelope.job_identity.is_some()
                || envelope.unattended
                || envelope
                    .child_task_identity
                    .as_deref()
                    .is_none_or(|value| !valid_sha_identity(value, "child-task:sha256:"))
            {
                return Err(mode_error());
            }
        }
        ExecutionMode::Background => {
            if envelope.project_id.is_none()
                || !envelope.unattended
                || envelope
                    .job_identity
                    .as_deref()
                    .is_none_or(|value| !valid_sha_identity(value, "background-job:sha256:"))
                || envelope
                    .child_task_identity
                    .as_deref()
                    .is_some_and(|value| !valid_sha_identity(value, "child-task:sha256:"))
            {
                return Err(mode_error());
            }
        }
    }
    Ok(())
}

fn validate_identifier(value: &str) -> Result<(), UnifiedExecutionError> {
    if value.is_empty()
        || value.len() > MAX_IDENTIFIER_BYTES
        || value
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(UnifiedExecutionError::new(
            "unified-execution-identifier-invalid",
            "unified execution identifier is invalid",
        ));
    }
    Ok(())
}

fn valid_sha_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn mode_error() -> UnifiedExecutionError {
    UnifiedExecutionError::new(
        "unified-execution-mode-binding-invalid",
        "execution mode bindings are invalid",
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn envelope(mode: ExecutionMode, mutation: ExecutionMutation) -> ExecutionEnvelope {
        ExecutionEnvelope {
            schema_version: SCHEMA_VERSION.into(),
            execution_identity: identity("execution:sha256:", 'a'),
            mode,
            mutation,
            session_id: "session-1".into(),
            project_id: Some("project-1".into()),
            child_task_identity: (mode == ExecutionMode::Child)
                .then(|| identity("child-task:sha256:", 'b')),
            job_identity: (mode == ExecutionMode::Background)
                .then(|| identity("background-job:sha256:", 'c')),
            unattended: mode == ExecutionMode::Background,
        }
    }

    fn all_ready() -> ExecutionGateEvidence {
        ExecutionGateEvidence {
            identity_bound: true,
            reconciliation_clear: true,
            permission_ready: true,
            approval_ready: true,
            workspace_ready: true,
            budget_ready: true,
            sandbox_ready: true,
            recovery_ready: true,
            durable_job_ready: true,
            notification_ready: true,
            release_ready: true,
        }
    }

    #[test]
    fn every_mode_uses_the_exact_same_ordered_pipeline() {
        let plans = [
            envelope(ExecutionMode::Interactive, ExecutionMutation::ReadOnly),
            envelope(ExecutionMode::Child, ExecutionMutation::ReadOnly),
            envelope(ExecutionMode::Background, ExecutionMutation::ReadOnly),
        ]
        .map(|envelope| plan_execution(&envelope, all_ready()).unwrap());
        let expected = plans[0]
            .stages
            .iter()
            .map(|stage| stage.stage)
            .collect::<Vec<_>>();
        assert_eq!(expected.len(), 14);
        for plan in plans {
            assert_eq!(
                plan.stages
                    .iter()
                    .map(|stage| stage.stage)
                    .collect::<Vec<_>>(),
                expected
            );
            assert!(plan.admission_ready);
            assert!(!plan.permission_granted);
            assert!(!plan.execution_available);
        }
    }

    #[test]
    fn current_interactive_read_only_path_is_ready_without_autonomy_gates() {
        let plan = plan_current_interactive_read_only(
            &identity("execution:sha256:", 'd'),
            "session-1",
            None,
        )
        .unwrap();
        assert!(plan.admission_ready);
        for stage in [
            ExecutionStage::Approval,
            ExecutionStage::Budget,
            ExecutionStage::DurableJob,
            ExecutionStage::Notification,
            ExecutionStage::Release,
        ] {
            assert_eq!(
                plan.stages
                    .iter()
                    .find(|item| item.stage == stage)
                    .unwrap()
                    .state,
                ExecutionStageState::NotRequired
            );
        }
        assert!(plan
            .plan_identity
            .starts_with("unified-execution-plan:sha256:"));
    }

    #[test]
    fn child_write_requires_approval_workspace_budget_and_release() {
        let envelope = envelope(ExecutionMode::Child, ExecutionMutation::WorkspaceWrite);
        let mut evidence = all_ready();
        evidence.approval_ready = false;
        evidence.workspace_ready = false;
        evidence.budget_ready = false;
        evidence.release_ready = false;
        let plan = plan_execution(&envelope, evidence).unwrap();
        assert!(!plan.admission_ready);
        assert_eq!(
            plan.missing_requirements,
            vec![
                "approval-ready",
                "dedicated-workspace-ready",
                "runtime-budget-ready",
                "multi-agent-release-gate"
            ]
        );
        assert_eq!(
            plan.stages
                .iter()
                .find(|stage| stage.stage == ExecutionStage::Dispatch)
                .unwrap()
                .state,
            ExecutionStageState::Missing
        );
    }

    #[test]
    fn background_requires_durable_job_notification_and_release() {
        let envelope = envelope(ExecutionMode::Background, ExecutionMutation::ReadOnly);
        let mut evidence = all_ready();
        evidence.durable_job_ready = false;
        evidence.notification_ready = false;
        evidence.release_ready = false;
        let plan = plan_execution(&envelope, evidence).unwrap();
        assert!(!plan.admission_ready);
        assert!(plan
            .missing_requirements
            .contains(&"durable-job-ready".into()));
        assert!(plan
            .missing_requirements
            .contains(&"notification-ready".into()));
        assert!(plan
            .missing_requirements
            .contains(&"background-job-release-gate".into()));
    }

    #[test]
    fn mode_bindings_cannot_be_relabelled_to_skip_requirements() {
        let mut child = envelope(ExecutionMode::Child, ExecutionMutation::ReadOnly);
        child.child_task_identity = None;
        assert_eq!(
            plan_execution(&child, all_ready()).unwrap_err().code,
            "unified-execution-mode-binding-invalid"
        );

        let mut interactive = envelope(ExecutionMode::Interactive, ExecutionMutation::ReadOnly);
        interactive.job_identity = Some(identity("background-job:sha256:", 'e'));
        assert_eq!(
            plan_execution(&interactive, all_ready()).unwrap_err().code,
            "unified-execution-mode-binding-invalid"
        );

        let mut background = envelope(ExecutionMode::Background, ExecutionMutation::ReadOnly);
        background.unattended = false;
        assert_eq!(
            plan_execution(&background, all_ready()).unwrap_err().code,
            "unified-execution-mode-binding-invalid"
        );
    }

    #[test]
    fn missing_common_gate_blocks_every_mode_without_granting_authority() {
        for mode in [
            ExecutionMode::Interactive,
            ExecutionMode::Child,
            ExecutionMode::Background,
        ] {
            let mut evidence = all_ready();
            evidence.reconciliation_clear = false;
            let plan =
                plan_execution(&envelope(mode, ExecutionMutation::ReadOnly), evidence).unwrap();
            assert!(!plan.admission_ready);
            assert!(plan
                .missing_requirements
                .contains(&"reconciliation-clear".into()));
            assert!(!plan.permission_granted);
            assert!(!plan.execution_available);
        }
    }
}
