//! Durable-job lifecycle contract for future background execution.
//!
//! The contract is content-free and persistence-ready, but this module does not
//! enqueue, persist, schedule, notify, approve, or execute a job. Restart recovery
//! never infers success and retry eligibility requires a pre-bound safe boundary.

use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};

pub const REQUEST_SCHEMA_VERSION: &str = "background-job-request/0.1";
pub const STATE_SCHEMA_VERSION: &str = "background-job-state/0.1";
const MAX_ATTEMPTS: u16 = 16;
const MAX_BACKOFF_MS: u64 = 24 * 60 * 60 * 1_000;
const MAX_IDENTIFIER_BYTES: usize = 128;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundJobError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundJobError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobScheduleKind {
    Manual,
    At,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobSchedule {
    pub kind: JobScheduleKind,
    pub scheduled_for_ms: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobRetryPolicy {
    pub max_attempts: u16,
    pub backoff_ms: u64,
    pub safe_retry_boundary_identity: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundJobRequest {
    pub schema_version: String,
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub execution_plan_identity: String,
    pub idempotency_identity: String,
    pub child_task_identity: Option<String>,
    pub schedule: JobSchedule,
    pub retry: JobRetryPolicy,
    pub created_at_ms: u64,
}

impl BackgroundJobRequest {
    pub fn validate(&self) -> Result<(), BackgroundJobError> {
        if self.schema_version != REQUEST_SCHEMA_VERSION {
            return Err(error(
                "background-job-request-schema-invalid",
                "background job request schema is unsupported",
            ));
        }
        for value in [
            &self.job_id,
            &self.session_id,
            &self.project_id,
            &self.root_id,
        ] {
            validate_identifier(value)?;
        }
        if !valid_sha_identity(
            &self.execution_plan_identity,
            "unified-execution-plan:sha256:",
        ) || !valid_sha_identity(&self.idempotency_identity, "idempotency:sha256:")
            || self
                .child_task_identity
                .as_deref()
                .is_some_and(|value| !valid_sha_identity(value, "child-task:sha256:"))
        {
            return Err(error(
                "background-job-request-identity-invalid",
                "background job request contains an invalid identity",
            ));
        }
        if self.created_at_ms == 0 {
            return Err(error(
                "background-job-request-time-invalid",
                "background job creation time is invalid",
            ));
        }
        match self.schedule.kind {
            JobScheduleKind::Manual if self.schedule.scheduled_for_ms.is_some() => {
                return Err(error(
                    "background-job-schedule-invalid",
                    "manual job cannot carry a scheduled time",
                ));
            }
            JobScheduleKind::At
                if self
                    .schedule
                    .scheduled_for_ms
                    .is_none_or(|value| value < self.created_at_ms) =>
            {
                return Err(error(
                    "background-job-schedule-invalid",
                    "scheduled job time is missing or before creation",
                ));
            }
            _ => {}
        }
        if self.retry.max_attempts == 0
            || self.retry.max_attempts > MAX_ATTEMPTS
            || self.retry.backoff_ms > MAX_BACKOFF_MS
            || self
                .retry
                .safe_retry_boundary_identity
                .as_deref()
                .is_some_and(|value| !valid_sha_identity(value, "retry-boundary:sha256:"))
        {
            return Err(error(
                "background-job-retry-policy-invalid",
                "background job retry policy is invalid",
            ));
        }
        Ok(())
    }

    pub fn identity(&self) -> Result<String, BackgroundJobError> {
        self.validate()?;
        let bytes = to_vec(self).map_err(|_| {
            error(
                "background-job-request-serialize-failed",
                "background job request could not be serialized",
            )
        })?;
        Ok(format!("background-job:sha256:{:x}", Sha256::digest(bytes)))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundJobStatus {
    Queued,
    Running,
    PauseRequested,
    Paused,
    WaitingApproval,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Interrupted,
}

impl BackgroundJobStatus {
    fn terminal(self) -> bool {
        matches!(
            self,
            Self::Completed | Self::Failed | Self::Cancelled | Self::Interrupted
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobCancellationState {
    NotRequested,
    Requested,
    Acknowledged,
    Failed,
    SupersededByCompletion,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobAttemptStatus {
    Running,
    Completed,
    Failed,
    Cancelled,
    Interrupted,
}

impl JobAttemptStatus {
    fn terminal(self) -> bool {
        self != Self::Running
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobAttempt {
    pub number: u16,
    pub execution_plan_identity: String,
    pub idempotency_identity: String,
    pub status: JobAttemptStatus,
    pub started_at_ms: u64,
    pub ended_at_ms: Option<u64>,
    pub terminal_evidence_identity: Option<String>,
    pub retryable: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobRecoveryDisposition {
    RunWhenDue,
    KeepPaused,
    KeepWaitingApproval,
    RetryEligible,
    ManualReconciliation,
    Terminal,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct JobRecoveryDecision {
    pub status: BackgroundJobStatus,
    pub disposition: JobRecoveryDisposition,
    pub next_eligible_at_ms: Option<u64>,
    pub automatic_approval: bool,
    pub automatic_retry: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundJobState {
    pub schema_version: String,
    pub request_identity: String,
    pub status: BackgroundJobStatus,
    pub cancellation: JobCancellationState,
    pub generation: u64,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
    pub next_eligible_at_ms: u64,
    pub approval_identity: Option<String>,
    pub result_reference: Option<String>,
    pub attempts: Vec<JobAttempt>,
}

impl BackgroundJobState {
    pub fn new(request: &BackgroundJobRequest, now_ms: u64) -> Result<Self, BackgroundJobError> {
        request.validate()?;
        if now_ms < request.created_at_ms {
            return Err(error(
                "background-job-state-time-invalid",
                "background job state time is before request creation",
            ));
        }
        let next_eligible_at_ms = request
            .schedule
            .scheduled_for_ms
            .unwrap_or(now_ms)
            .max(now_ms);
        let state = Self {
            schema_version: STATE_SCHEMA_VERSION.into(),
            request_identity: request.identity()?,
            status: BackgroundJobStatus::Queued,
            cancellation: JobCancellationState::NotRequested,
            generation: 0,
            created_at_ms: now_ms,
            updated_at_ms: now_ms,
            next_eligible_at_ms,
            approval_identity: None,
            result_reference: None,
            attempts: Vec::new(),
        };
        state.validate(request)?;
        Ok(state)
    }

    pub fn validate(&self, request: &BackgroundJobRequest) -> Result<(), BackgroundJobError> {
        request.validate()?;
        if self.schema_version != STATE_SCHEMA_VERSION
            || self.request_identity != request.identity()?
            || self.created_at_ms < request.created_at_ms
            || self.updated_at_ms < self.created_at_ms
            || self.next_eligible_at_ms < self.created_at_ms
            || self.attempts.len() > usize::from(request.retry.max_attempts)
        {
            return Err(error(
                "background-job-state-invalid",
                "background job state invariant is invalid",
            ));
        }
        for (index, attempt) in self.attempts.iter().enumerate() {
            validate_attempt(attempt, request, index)?;
        }
        let running_attempts = self
            .attempts
            .iter()
            .filter(|attempt| !attempt.status.terminal())
            .count();
        if running_attempts > 1
            || (running_attempts == 1
                && !matches!(
                    self.status,
                    BackgroundJobStatus::Running
                        | BackgroundJobStatus::PauseRequested
                        | BackgroundJobStatus::WaitingApproval
                        | BackgroundJobStatus::Cancelling
                ))
            || (running_attempts == 0
                && matches!(
                    self.status,
                    BackgroundJobStatus::Running
                        | BackgroundJobStatus::PauseRequested
                        | BackgroundJobStatus::WaitingApproval
                        | BackgroundJobStatus::Cancelling
                ))
        {
            return Err(error(
                "background-job-attempt-state-invalid",
                "background job active attempt does not match lifecycle state",
            ));
        }
        if self.status == BackgroundJobStatus::Completed && self.result_reference.is_none() {
            return Err(error(
                "background-job-result-missing",
                "completed background job requires a bounded result reference",
            ));
        }
        if self.status != BackgroundJobStatus::Completed && self.result_reference.is_some() {
            return Err(error(
                "background-job-result-state-invalid",
                "background job result is only valid after completion",
            ));
        }
        let terminal_attempt_matches = match self.status {
            BackgroundJobStatus::Queued | BackgroundJobStatus::Paused
                if !self.attempts.is_empty() =>
            {
                self.attempts.last().is_some_and(|attempt| {
                    attempt.retryable
                        && matches!(
                            attempt.status,
                            JobAttemptStatus::Failed | JobAttemptStatus::Interrupted
                        )
                })
            }
            BackgroundJobStatus::Completed => self
                .attempts
                .last()
                .is_some_and(|attempt| attempt.status == JobAttemptStatus::Completed),
            BackgroundJobStatus::Failed => self
                .attempts
                .last()
                .is_some_and(|attempt| attempt.status == JobAttemptStatus::Failed),
            BackgroundJobStatus::Interrupted => self
                .attempts
                .last()
                .is_some_and(|attempt| attempt.status == JobAttemptStatus::Interrupted),
            BackgroundJobStatus::Cancelled if !self.attempts.is_empty() => self
                .attempts
                .last()
                .is_some_and(|attempt| attempt.status.terminal()),
            _ => true,
        };
        if !terminal_attempt_matches {
            return Err(error(
                "background-job-terminal-attempt-mismatch",
                "background job terminal state does not match its final attempt",
            ));
        }
        if (self.status == BackgroundJobStatus::WaitingApproval) != self.approval_identity.is_some()
        {
            return Err(error(
                "background-job-approval-state-invalid",
                "background job approval identity does not match lifecycle state",
            ));
        }
        if self
            .result_reference
            .as_deref()
            .is_some_and(|value| !valid_result_reference(value))
            || self
                .approval_identity
                .as_deref()
                .is_some_and(|value| !valid_sha_identity(value, "approval:sha256:"))
        {
            return Err(error(
                "background-job-reference-invalid",
                "background job content reference is invalid",
            ));
        }
        validate_cancellation(self.status, self.cancellation)?;
        Ok(())
    }

    pub fn identity(&self, request: &BackgroundJobRequest) -> Result<String, BackgroundJobError> {
        self.validate(request)?;
        let bytes = to_vec(self).map_err(|_| {
            error(
                "background-job-state-serialize-failed",
                "background job state could not be serialized",
            )
        })?;
        Ok(format!(
            "background-job-state:sha256:{:x}",
            Sha256::digest(bytes)
        ))
    }

    pub fn start(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        self.validate(request)?;
        if self.status != BackgroundJobStatus::Queued
            || self.cancellation != JobCancellationState::NotRequested
            || now_ms < self.next_eligible_at_ms
        {
            return Err(error(
                "background-job-start-not-eligible",
                "background job is not queued and eligible",
            ));
        }
        if self.attempts.len() >= usize::from(request.retry.max_attempts) {
            return Err(error(
                "background-job-attempt-limit",
                "background job attempt limit is exhausted",
            ));
        }
        let number = u16::try_from(self.attempts.len() + 1).map_err(|_| {
            error(
                "background-job-attempt-limit",
                "background job attempt count is invalid",
            )
        })?;
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::Running;
            state.cancellation = JobCancellationState::NotRequested;
            state.approval_identity = None;
            state.attempts.push(JobAttempt {
                number,
                execution_plan_identity: request.execution_plan_identity.clone(),
                idempotency_identity: request.idempotency_identity.clone(),
                status: JobAttemptStatus::Running,
                started_at_ms: now_ms,
                ended_at_ms: None,
                terminal_evidence_identity: None,
                retryable: false,
            });
            Ok(())
        })
    }

    pub fn request_pause(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        self.validate(request)?;
        match self.status {
            BackgroundJobStatus::Queued => self.apply_update(request, now_ms, |state| {
                state.status = BackgroundJobStatus::Paused;
                Ok(())
            }),
            BackgroundJobStatus::Running
                if request.retry.safe_retry_boundary_identity.is_some() =>
            {
                self.apply_update(request, now_ms, |state| {
                    state.status = BackgroundJobStatus::PauseRequested;
                    Ok(())
                })
            }
            BackgroundJobStatus::Running => Err(error(
                "background-job-pause-unsupported",
                "running background job has no safe pause boundary",
            )),
            BackgroundJobStatus::PauseRequested | BackgroundJobStatus::Paused => Ok(()),
            _ => Err(error(
                "background-job-pause-invalid",
                "background job cannot be paused from its current state",
            )),
        }
    }

    pub fn acknowledge_pause(
        &mut self,
        request: &BackgroundJobRequest,
        terminal_evidence_identity: &str,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.status != BackgroundJobStatus::PauseRequested
            || !valid_sha_identity(terminal_evidence_identity, "job-evidence:sha256:")
        {
            return Err(error(
                "background-job-pause-ack-invalid",
                "background job pause acknowledgement is invalid",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            finish_active_attempt(
                state,
                JobAttemptStatus::Interrupted,
                terminal_evidence_identity,
                now_ms,
                request.retry.safe_retry_boundary_identity.is_some(),
            )?;
            state.status = BackgroundJobStatus::Paused;
            Ok(())
        })
    }

    pub fn resume(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.status != BackgroundJobStatus::Paused
            || self.cancellation != JobCancellationState::NotRequested
        {
            return Err(error(
                "background-job-resume-invalid",
                "only a paused background job can resume to the queue",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::Queued;
            state.next_eligible_at_ms = state
                .next_eligible_at_ms
                .max(request.schedule.scheduled_for_ms.unwrap_or(now_ms))
                .max(now_ms);
            Ok(())
        })
    }

    pub fn wait_for_approval(
        &mut self,
        request: &BackgroundJobRequest,
        approval_identity: &str,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.status != BackgroundJobStatus::Running
            || !valid_sha_identity(approval_identity, "approval:sha256:")
        {
            return Err(error(
                "background-job-approval-wait-invalid",
                "background job approval wait is invalid",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::WaitingApproval;
            state.approval_identity = Some(approval_identity.into());
            Ok(())
        })
    }

    pub fn resume_after_approval(
        &mut self,
        request: &BackgroundJobRequest,
        approval_identity: &str,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.status != BackgroundJobStatus::WaitingApproval
            || self.approval_identity.as_deref() != Some(approval_identity)
        {
            return Err(error(
                "background-job-approval-mismatch",
                "background job approval identity does not match",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::Running;
            state.approval_identity = None;
            Ok(())
        })
    }

    pub fn request_cancel(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<bool, BackgroundJobError> {
        self.validate(request)?;
        if self.status.terminal() {
            return Err(error(
                "background-job-cancel-terminal",
                "terminal background job cannot be cancelled",
            ));
        }
        if self.cancellation == JobCancellationState::Requested {
            return Ok(false);
        }
        self.apply_update(request, now_ms, |state| {
            state.cancellation = JobCancellationState::Requested;
            if matches!(state.status, BackgroundJobStatus::Running) {
                state.status = BackgroundJobStatus::Cancelling;
            }
            Ok(())
        })?;
        Ok(true)
    }

    pub fn acknowledge_cancel(
        &mut self,
        request: &BackgroundJobRequest,
        terminal_evidence_identity: Option<&str>,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.cancellation != JobCancellationState::Requested
            || terminal_evidence_identity
                .is_some_and(|value| !valid_sha_identity(value, "job-evidence:sha256:"))
        {
            return Err(error(
                "background-job-cancel-ack-invalid",
                "background job cancellation acknowledgement is invalid",
            ));
        }
        let has_active = self.active_attempt().is_some();
        if has_active && terminal_evidence_identity.is_none() {
            return Err(error(
                "background-job-terminal-evidence-missing",
                "active background job cancellation requires terminal evidence",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            if let Some(evidence) = terminal_evidence_identity {
                finish_active_attempt(state, JobAttemptStatus::Cancelled, evidence, now_ms, false)?;
            }
            state.status = BackgroundJobStatus::Cancelled;
            state.cancellation = JobCancellationState::Acknowledged;
            state.approval_identity = None;
            Ok(())
        })
    }

    pub fn reject_cancel(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if self.cancellation != JobCancellationState::Requested
            || self.status != BackgroundJobStatus::Cancelling
        {
            return Err(error(
                "background-job-cancel-reject-invalid",
                "background job cancellation rejection is invalid",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::Running;
            state.cancellation = JobCancellationState::Failed;
            Ok(())
        })
    }

    pub fn complete(
        &mut self,
        request: &BackgroundJobRequest,
        result_reference: &str,
        terminal_evidence_identity: &str,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if !matches!(
            self.status,
            BackgroundJobStatus::Running | BackgroundJobStatus::Cancelling
        ) || !valid_result_reference(result_reference)
            || !valid_sha_identity(terminal_evidence_identity, "job-evidence:sha256:")
        {
            return Err(error(
                "background-job-completion-invalid",
                "background job completion evidence is invalid",
            ));
        }
        self.apply_update(request, now_ms, |state| {
            finish_active_attempt(
                state,
                JobAttemptStatus::Completed,
                terminal_evidence_identity,
                now_ms,
                false,
            )?;
            state.status = BackgroundJobStatus::Completed;
            state.result_reference = Some(result_reference.into());
            if state.cancellation == JobCancellationState::Requested {
                state.cancellation = JobCancellationState::SupersededByCompletion;
            }
            Ok(())
        })
    }

    pub fn fail(
        &mut self,
        request: &BackgroundJobRequest,
        terminal_evidence_identity: &str,
        retryable: bool,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        if !matches!(
            self.status,
            BackgroundJobStatus::Running
                | BackgroundJobStatus::WaitingApproval
                | BackgroundJobStatus::Cancelling
        ) || !valid_sha_identity(terminal_evidence_identity, "job-evidence:sha256:")
        {
            return Err(error(
                "background-job-failure-invalid",
                "background job failure evidence is invalid",
            ));
        }
        let retryable = retryable && request.retry.safe_retry_boundary_identity.is_some();
        self.apply_update(request, now_ms, |state| {
            finish_active_attempt(
                state,
                JobAttemptStatus::Failed,
                terminal_evidence_identity,
                now_ms,
                retryable,
            )?;
            state.status = BackgroundJobStatus::Failed;
            state.approval_identity = None;
            if state.cancellation == JobCancellationState::Requested {
                state.cancellation = JobCancellationState::Failed;
            }
            Ok(())
        })
    }

    pub fn retry(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> Result<(), BackgroundJobError> {
        self.validate(request)?;
        let retryable = self.attempts.last().is_some_and(|attempt| {
            attempt.retryable
                && attempt.status.terminal()
                && attempt.idempotency_identity == request.idempotency_identity
        });
        if !matches!(
            self.status,
            BackgroundJobStatus::Failed | BackgroundJobStatus::Interrupted
        ) || !retryable
            || self.attempts.len() >= usize::from(request.retry.max_attempts)
        {
            return Err(error(
                "background-job-retry-not-eligible",
                "background job is not eligible for an idempotent retry",
            ));
        }
        let next = now_ms
            .checked_add(request.retry.backoff_ms)
            .ok_or_else(|| {
                error(
                    "background-job-time-overflow",
                    "background job retry time overflowed",
                )
            })?;
        self.apply_update(request, now_ms, |state| {
            state.status = BackgroundJobStatus::Queued;
            state.cancellation = JobCancellationState::NotRequested;
            state.next_eligible_at_ms = next;
            Ok(())
        })
    }

    pub fn recover_after_restart(
        &mut self,
        request: &BackgroundJobRequest,
        observation_identity: &str,
        now_ms: u64,
    ) -> Result<JobRecoveryDecision, BackgroundJobError> {
        self.validate(request)?;
        if !valid_sha_identity(observation_identity, "job-recovery:sha256:") {
            return Err(error(
                "background-job-recovery-evidence-invalid",
                "background job recovery evidence is invalid",
            ));
        }
        if matches!(
            self.status,
            BackgroundJobStatus::Running
                | BackgroundJobStatus::PauseRequested
                | BackgroundJobStatus::Cancelling
        ) {
            let retryable = request.retry.safe_retry_boundary_identity.is_some();
            self.apply_update(request, now_ms, |state| {
                finish_active_attempt(
                    state,
                    JobAttemptStatus::Interrupted,
                    observation_identity,
                    now_ms,
                    retryable,
                )?;
                state.status = BackgroundJobStatus::Interrupted;
                state.approval_identity = None;
                if state.cancellation == JobCancellationState::Requested {
                    state.cancellation = JobCancellationState::Failed;
                }
                Ok(())
            })?;
        }
        Ok(self.recovery_decision(request, now_ms))
    }

    pub fn recovery_decision(
        &self,
        request: &BackgroundJobRequest,
        now_ms: u64,
    ) -> JobRecoveryDecision {
        let disposition = if self.cancellation == JobCancellationState::Requested {
            JobRecoveryDisposition::ManualReconciliation
        } else {
            match self.status {
                BackgroundJobStatus::Queued => JobRecoveryDisposition::RunWhenDue,
                BackgroundJobStatus::Paused => JobRecoveryDisposition::KeepPaused,
                BackgroundJobStatus::WaitingApproval => JobRecoveryDisposition::KeepWaitingApproval,
                BackgroundJobStatus::Failed | BackgroundJobStatus::Interrupted
                    if self.attempts.last().is_some_and(|attempt| {
                        attempt.retryable
                            && self.attempts.len() < usize::from(request.retry.max_attempts)
                    }) =>
                {
                    JobRecoveryDisposition::RetryEligible
                }
                status if status.terminal() => JobRecoveryDisposition::Terminal,
                _ => JobRecoveryDisposition::ManualReconciliation,
            }
        };
        JobRecoveryDecision {
            status: self.status,
            disposition,
            next_eligible_at_ms: matches!(disposition, JobRecoveryDisposition::RunWhenDue)
                .then_some(self.next_eligible_at_ms.max(now_ms)),
            automatic_approval: false,
            automatic_retry: false,
        }
    }

    fn active_attempt(&self) -> Option<&JobAttempt> {
        self.attempts
            .last()
            .filter(|attempt| !attempt.status.terminal())
    }

    fn apply_update(
        &mut self,
        request: &BackgroundJobRequest,
        now_ms: u64,
        update: impl FnOnce(&mut Self) -> Result<(), BackgroundJobError>,
    ) -> Result<(), BackgroundJobError> {
        self.validate(request)?;
        if now_ms < self.updated_at_ms {
            return Err(error(
                "background-job-clock-regressed",
                "background job clock moved backwards",
            ));
        }
        let next_generation = self.generation.checked_add(1).ok_or_else(|| {
            error(
                "background-job-generation-exhausted",
                "background job generation is exhausted",
            )
        })?;
        let previous = self.clone();
        if let Err(cause) = update(self) {
            *self = previous;
            return Err(cause);
        }
        self.generation = next_generation;
        self.updated_at_ms = now_ms;
        if let Err(cause) = self.validate(request) {
            *self = previous;
            return Err(cause);
        }
        Ok(())
    }
}

fn validate_attempt(
    attempt: &JobAttempt,
    request: &BackgroundJobRequest,
    index: usize,
) -> Result<(), BackgroundJobError> {
    if usize::from(attempt.number) != index + 1
        || attempt.execution_plan_identity != request.execution_plan_identity
        || attempt.idempotency_identity != request.idempotency_identity
        || attempt.started_at_ms < request.created_at_ms
        || (attempt.status.terminal()
            && (attempt
                .ended_at_ms
                .is_none_or(|ended| ended < attempt.started_at_ms)
                || attempt
                    .terminal_evidence_identity
                    .as_deref()
                    .is_none_or(|value| {
                        !valid_sha_identity(value, "job-evidence:sha256:")
                            && !valid_sha_identity(value, "job-recovery:sha256:")
                    })))
        || (!attempt.status.terminal()
            && (attempt.ended_at_ms.is_some()
                || attempt.terminal_evidence_identity.is_some()
                || attempt.retryable))
        || (attempt.retryable
            && !matches!(
                attempt.status,
                JobAttemptStatus::Failed | JobAttemptStatus::Interrupted
            ))
    {
        return Err(error(
            "background-job-attempt-invalid",
            "background job attempt invariant is invalid",
        ));
    }
    Ok(())
}

fn validate_cancellation(
    status: BackgroundJobStatus,
    cancellation: JobCancellationState,
) -> Result<(), BackgroundJobError> {
    let valid = match cancellation {
        JobCancellationState::NotRequested | JobCancellationState::Failed => {
            status != BackgroundJobStatus::Cancelled
        }
        JobCancellationState::Requested => matches!(
            status,
            BackgroundJobStatus::Queued
                | BackgroundJobStatus::Paused
                | BackgroundJobStatus::WaitingApproval
                | BackgroundJobStatus::PauseRequested
                | BackgroundJobStatus::Cancelling
        ),
        JobCancellationState::Acknowledged => status == BackgroundJobStatus::Cancelled,
        JobCancellationState::SupersededByCompletion => status == BackgroundJobStatus::Completed,
    };
    if valid {
        Ok(())
    } else {
        Err(error(
            "background-job-cancellation-state-invalid",
            "background job cancellation state is invalid",
        ))
    }
}

fn finish_active_attempt(
    state: &mut BackgroundJobState,
    status: JobAttemptStatus,
    terminal_evidence_identity: &str,
    now_ms: u64,
    retryable: bool,
) -> Result<(), BackgroundJobError> {
    let attempt = state.attempts.last_mut().ok_or_else(|| {
        error(
            "background-job-active-attempt-missing",
            "background job active attempt is missing",
        )
    })?;
    if attempt.status != JobAttemptStatus::Running {
        return Err(error(
            "background-job-active-attempt-invalid",
            "background job attempt is already terminal",
        ));
    }
    attempt.status = status;
    attempt.ended_at_ms = Some(now_ms);
    attempt.terminal_evidence_identity = Some(terminal_evidence_identity.into());
    attempt.retryable = retryable;
    Ok(())
}

fn validate_identifier(value: &str) -> Result<(), BackgroundJobError> {
    if value.is_empty()
        || value.len() > MAX_IDENTIFIER_BYTES
        || value
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(error(
            "background-job-identifier-invalid",
            "background job identifier is invalid",
        ));
    }
    Ok(())
}

fn valid_result_reference(value: &str) -> bool {
    valid_sha_identity(value, "artifact:sha256:")
        || valid_sha_identity(value, "child-handoff:sha256:")
}

fn valid_sha_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn error(code: &'static str, message: &'static str) -> BackgroundJobError {
    BackgroundJobError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn job_request(safe_retry: bool) -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: REQUEST_SCHEMA_VERSION.into(),
            job_id: "job-1".into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'a'),
            idempotency_identity: identity("idempotency:sha256:", 'b'),
            child_task_identity: Some(identity("child-task:sha256:", 'c')),
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 100,
                safe_retry_boundary_identity: safe_retry
                    .then(|| identity("retry-boundary:sha256:", 'd')),
            },
            created_at_ms: 1_000,
        }
    }

    #[test]
    fn queued_job_runs_and_completes_with_bounded_result() {
        let request = job_request(true);
        let mut state = BackgroundJobState::new(&request, 1_000).unwrap();
        state.start(&request, 1_100).unwrap();
        state
            .complete(
                &request,
                &identity("artifact:sha256:", 'e'),
                &identity("job-evidence:sha256:", 'f'),
                1_200,
            )
            .unwrap();
        assert_eq!(state.status, BackgroundJobStatus::Completed);
        assert_eq!(state.attempts[0].status, JobAttemptStatus::Completed);
        assert!(state.identity(&request).is_ok());
    }

    #[test]
    fn pause_and_approval_wait_never_auto_resume_or_approve() {
        let request = job_request(true);
        let mut queued = BackgroundJobState::new(&request, 1_000).unwrap();
        queued.request_pause(&request, 1_050).unwrap();
        assert_eq!(queued.status, BackgroundJobStatus::Paused);
        queued.resume(&request, 1_100).unwrap();
        queued.start(&request, 1_200).unwrap();
        let approval = identity("approval:sha256:", 'a');
        queued
            .wait_for_approval(&request, &approval, 1_300)
            .unwrap();
        let decision = queued.recovery_decision(&request, 9_000);
        assert_eq!(
            decision.disposition,
            JobRecoveryDisposition::KeepWaitingApproval
        );
        assert!(!decision.automatic_approval);
        assert!(!decision.automatic_retry);
        assert_eq!(
            queued
                .resume_after_approval(&request, &identity("approval:sha256:", 'b'), 9_100)
                .unwrap_err()
                .code,
            "background-job-approval-mismatch"
        );
        queued
            .resume_after_approval(&request, &approval, 9_100)
            .unwrap();
    }

    #[test]
    fn cancellation_is_requested_acknowledged_and_completion_can_win_race() {
        let request = job_request(true);
        let mut state = BackgroundJobState::new(&request, 1_000).unwrap();
        assert!(state.request_cancel(&request, 1_010).unwrap());
        assert!(!state.request_cancel(&request, 1_020).unwrap());
        state.acknowledge_cancel(&request, None, 1_030).unwrap();
        assert_eq!(state.status, BackgroundJobStatus::Cancelled);

        let mut race = BackgroundJobState::new(&request, 1_000).unwrap();
        race.start(&request, 1_100).unwrap();
        race.request_cancel(&request, 1_200).unwrap();
        race.complete(
            &request,
            &identity("child-handoff:sha256:", 'e'),
            &identity("job-evidence:sha256:", 'f'),
            1_300,
        )
        .unwrap();
        assert_eq!(race.status, BackgroundJobStatus::Completed);
        assert_eq!(
            race.cancellation,
            JobCancellationState::SupersededByCompletion
        );
    }

    #[test]
    fn retry_requires_safe_boundary_same_idempotency_and_attempt_capacity() {
        let request = job_request(true);
        let mut state = BackgroundJobState::new(&request, 1_000).unwrap();
        state.start(&request, 1_100).unwrap();
        state
            .fail(
                &request,
                &identity("job-evidence:sha256:", 'e'),
                true,
                1_200,
            )
            .unwrap();
        state.retry(&request, 1_300).unwrap();
        assert_eq!(state.next_eligible_at_ms, 1_400);
        state.request_pause(&request, 1_320).unwrap();
        state.resume(&request, 1_330).unwrap();
        assert_eq!(state.next_eligible_at_ms, 1_400);
        assert_eq!(
            state.start(&request, 1_399).unwrap_err().code,
            "background-job-start-not-eligible"
        );
        state.start(&request, 1_400).unwrap();
        state
            .fail(
                &request,
                &identity("job-evidence:sha256:", 'f'),
                true,
                1_500,
            )
            .unwrap();
        assert_eq!(
            state.retry(&request, 1_600).unwrap_err().code,
            "background-job-retry-not-eligible"
        );

        let unsafe_request = job_request(false);
        let mut unsafe_state = BackgroundJobState::new(&unsafe_request, 1_000).unwrap();
        unsafe_state.start(&unsafe_request, 1_100).unwrap();
        assert_eq!(
            unsafe_state
                .request_pause(&unsafe_request, 1_150)
                .unwrap_err()
                .code,
            "background-job-pause-unsupported"
        );
        unsafe_state
            .fail(
                &unsafe_request,
                &identity("job-evidence:sha256:", 'e'),
                true,
                1_200,
            )
            .unwrap();
        assert_eq!(
            unsafe_state.retry(&unsafe_request, 1_300).unwrap_err().code,
            "background-job-retry-not-eligible"
        );
    }

    #[test]
    fn restart_interrupts_running_job_and_preserves_queued_or_waiting_state() {
        let request = job_request(true);
        let mut running = BackgroundJobState::new(&request, 1_000).unwrap();
        running.start(&request, 1_100).unwrap();
        let decision = running
            .recover_after_restart(&request, &identity("job-recovery:sha256:", 'e'), 1_200)
            .unwrap();
        assert_eq!(running.status, BackgroundJobStatus::Interrupted);
        assert_eq!(decision.disposition, JobRecoveryDisposition::RetryEligible);
        assert!(!decision.automatic_retry);

        let mut queued = BackgroundJobState::new(&request, 1_000).unwrap();
        let queued_decision = queued
            .recover_after_restart(&request, &identity("job-recovery:sha256:", 'f'), 1_200)
            .unwrap();
        assert_eq!(queued.status, BackgroundJobStatus::Queued);
        assert_eq!(
            queued_decision.disposition,
            JobRecoveryDisposition::RunWhenDue
        );

        let mut cancelling = BackgroundJobState::new(&request, 1_000).unwrap();
        cancelling.request_cancel(&request, 1_100).unwrap();
        let cancelling_decision = cancelling
            .recover_after_restart(&request, &identity("job-recovery:sha256:", 'a'), 1_200)
            .unwrap();
        assert_eq!(
            cancelling_decision.disposition,
            JobRecoveryDisposition::ManualReconciliation
        );
        assert!(!cancelling_decision.automatic_retry);
    }

    #[test]
    fn invalid_updates_and_generation_exhaustion_leave_state_unchanged() {
        let mut request = job_request(true);
        request.schedule = JobSchedule {
            kind: JobScheduleKind::At,
            scheduled_for_ms: Some(999),
        };
        assert_eq!(
            request.validate().unwrap_err().code,
            "background-job-schedule-invalid"
        );

        let request = job_request(true);
        let mut state = BackgroundJobState::new(&request, 1_000).unwrap();
        state.request_pause(&request, 1_010).unwrap();
        state.request_cancel(&request, 1_020).unwrap();
        assert_eq!(
            state.resume(&request, 1_030).unwrap_err().code,
            "background-job-resume-invalid"
        );
        state.acknowledge_cancel(&request, None, 1_040).unwrap();

        let mut scheduled = job_request(true);
        scheduled.schedule = JobSchedule {
            kind: JobScheduleKind::At,
            scheduled_for_ms: Some(1_100),
        };
        let scheduled_state = BackgroundJobState::new(&scheduled, 1_200).unwrap();
        assert_eq!(scheduled_state.next_eligible_at_ms, 1_200);

        let mut state = BackgroundJobState::new(&request, 1_000).unwrap();
        state.generation = u64::MAX;
        let before = state.clone();
        assert_eq!(
            state.request_pause(&request, 1_100).unwrap_err().code,
            "background-job-generation-exhausted"
        );
        assert_eq!(state, before);
    }
}
