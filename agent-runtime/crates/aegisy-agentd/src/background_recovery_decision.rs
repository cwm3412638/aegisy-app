//! Content-free recovery decisions derived from verified scheduler snapshots.
//!
//! A decision records what the recovery inspector concluded for one durable job.
//! It never changes job state and grants no dispatch, retry, approval, takeover,
//! or other mutation authority.

use crate::background_job::{BackgroundJobStatus, JobCancellationState};
use crate::background_scheduler::{
    recovery_entry_identity, BackgroundSchedulerSnapshot, SchedulerLeaseState,
    SchedulerProcessOwnershipState, SchedulerRecoveryAction,
};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const SCHEMA_VERSION: &str = "background-job-recovery-decision/0.1";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundRecoveryDecisionError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundRecoveryDecisionError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundRecoveryDisposition {
    AwaitSchedule,
    AdmissionReviewRequired,
    KeepPaused,
    KeepWaitingApproval,
    RetryReviewRequired,
    MonitorOwnedProcess,
    ManualReconciliationRequired,
    TerminalReviewRequired,
}

impl From<SchedulerRecoveryAction> for BackgroundRecoveryDisposition {
    fn from(value: SchedulerRecoveryAction) -> Self {
        match value {
            SchedulerRecoveryAction::AwaitSchedule => Self::AwaitSchedule,
            SchedulerRecoveryAction::AdmissionReview => Self::AdmissionReviewRequired,
            SchedulerRecoveryAction::KeepPaused => Self::KeepPaused,
            SchedulerRecoveryAction::KeepWaitingApproval => Self::KeepWaitingApproval,
            SchedulerRecoveryAction::RetryReviewRequired => Self::RetryReviewRequired,
            SchedulerRecoveryAction::MonitorOwnedProcess => Self::MonitorOwnedProcess,
            SchedulerRecoveryAction::ManualReconciliation => Self::ManualReconciliationRequired,
            SchedulerRecoveryAction::TerminalReview => Self::TerminalReviewRequired,
        }
    }
}

impl BackgroundRecoveryDisposition {
    fn required_blocker(self) -> Option<&'static str> {
        match self {
            Self::AwaitSchedule => Some("schedule-not-due"),
            Self::AdmissionReviewRequired => Some("admission-review-required"),
            Self::KeepPaused => Some("job-paused"),
            Self::KeepWaitingApproval => Some("approval-required"),
            Self::RetryReviewRequired => Some("retry-review-required"),
            Self::MonitorOwnedProcess => Some("owned-process-running"),
            Self::ManualReconciliationRequired => None,
            Self::TerminalReviewRequired => Some("terminal-review-only"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundRecoveryDecision {
    pub schema_version: String,
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub job_generation: u64,
    pub job_status: BackgroundJobStatus,
    pub cancellation: JobCancellationState,
    pub next_eligible_at_ms: Option<u64>,
    pub scheduler_owner_identity: String,
    pub scheduler_generation: u64,
    pub scheduler_snapshot_identity: String,
    pub scheduler_entry_identity: String,
    pub disposition: BackgroundRecoveryDisposition,
    pub lease_state: SchedulerLeaseState,
    pub lease_identity: Option<String>,
    pub lease_generation: Option<u64>,
    pub process_ownership_state: SchedulerProcessOwnershipState,
    pub process_observation_identity: Option<String>,
    pub blockers: Vec<String>,
    pub blocker_count: u16,
    pub blockers_identity: String,
    pub approval_required: bool,
    pub cancellation_acknowledgement_required: bool,
    pub observed_at_ms: u64,
    pub recorded_at_ms: u64,
    pub automatic_retry: bool,
    pub automatic_approval: bool,
    pub automatic_takeover: bool,
    pub dispatch_authority: bool,
    pub mutation_authority: bool,
    pub decision_identity: String,
}

#[derive(Serialize)]
struct DecisionIdentityBinding<'a> {
    schema_version: &'a str,
    job_id: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    root_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    job_status: BackgroundJobStatus,
    cancellation: JobCancellationState,
    next_eligible_at_ms: Option<u64>,
    scheduler_owner_identity: &'a str,
    scheduler_generation: u64,
    scheduler_snapshot_identity: &'a str,
    scheduler_entry_identity: &'a str,
    disposition: BackgroundRecoveryDisposition,
    lease_state: SchedulerLeaseState,
    lease_identity: Option<&'a str>,
    lease_generation: Option<u64>,
    process_ownership_state: SchedulerProcessOwnershipState,
    process_observation_identity: Option<&'a str>,
    blockers: &'a [String],
    blocker_count: u16,
    blockers_identity: &'a str,
    approval_required: bool,
    cancellation_acknowledgement_required: bool,
    observed_at_ms: u64,
    recorded_at_ms: u64,
    automatic_retry: bool,
    automatic_approval: bool,
    automatic_takeover: bool,
    dispatch_authority: bool,
    mutation_authority: bool,
}

impl BackgroundRecoveryDecision {
    pub fn from_snapshot(
        snapshot: &BackgroundSchedulerSnapshot,
        job_id: &str,
        recorded_at_ms: u64,
    ) -> Result<Self, BackgroundRecoveryDecisionError> {
        snapshot.validate().map_err(|_| {
            error(
                "background-recovery-snapshot-invalid",
                "background recovery scheduler snapshot is invalid",
            )
        })?;
        let entry = snapshot
            .entries
            .iter()
            .find(|entry| entry.job_id == job_id)
            .ok_or_else(|| {
                error(
                    "background-recovery-job-missing",
                    "background recovery job is missing from the scheduler snapshot",
                )
            })?;
        if recorded_at_ms < snapshot.observed_at_ms {
            return Err(error(
                "background-recovery-time-invalid",
                "background recovery decision time precedes its observation",
            ));
        }
        let blocker_count = u16::try_from(entry.blockers.len()).map_err(|_| {
            error(
                "background-recovery-blocker-limit",
                "background recovery blocker count exceeds its bound",
            )
        })?;
        let mut decision = Self {
            schema_version: SCHEMA_VERSION.into(),
            job_id: entry.job_id.clone(),
            session_id: entry.session_id.clone(),
            project_id: entry.project_id.clone(),
            root_id: entry.root_id.clone(),
            request_identity: entry.request_identity.clone(),
            state_identity: entry.state_identity.clone(),
            job_generation: entry.job_generation,
            job_status: entry.status,
            cancellation: entry.cancellation,
            next_eligible_at_ms: entry.next_eligible_at_ms,
            scheduler_owner_identity: snapshot.owner_identity.clone(),
            scheduler_generation: snapshot.generation,
            scheduler_snapshot_identity: snapshot.snapshot_identity.clone(),
            scheduler_entry_identity: recovery_entry_identity(entry).map_err(|_| {
                error(
                    "background-recovery-entry-invalid",
                    "background recovery scheduler entry is invalid",
                )
            })?,
            disposition: entry.action.into(),
            lease_state: entry.lease_state,
            lease_identity: entry.lease_identity.clone(),
            lease_generation: entry.lease_generation,
            process_ownership_state: entry.process_ownership_state,
            process_observation_identity: entry
                .process_observation
                .as_ref()
                .map(|observation| observation.observation_identity.clone()),
            blockers: entry.blockers.clone(),
            blocker_count,
            blockers_identity: blockers_identity(&entry.blockers)?,
            approval_required: entry.approval_required,
            cancellation_acknowledgement_required: entry.cancellation_acknowledgement_required,
            observed_at_ms: snapshot.observed_at_ms,
            recorded_at_ms,
            automatic_retry: false,
            automatic_approval: false,
            automatic_takeover: false,
            dispatch_authority: false,
            mutation_authority: false,
            decision_identity: String::new(),
        };
        decision.decision_identity = decision_identity(&decision)?;
        decision.validate()?;
        Ok(decision)
    }

    pub fn validate(&self) -> Result<(), BackgroundRecoveryDecisionError> {
        let unique_blockers = self.blockers.iter().collect::<BTreeSet<_>>();
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
            || !valid_identity(&self.scheduler_owner_identity, "scheduler-owner:sha256:")
            || self.scheduler_generation == 0
            || !valid_identity(
                &self.scheduler_snapshot_identity,
                "background-job-scheduler-snapshot:sha256:",
            )
            || !valid_identity(
                &self.scheduler_entry_identity,
                "background-job-scheduler-entry:sha256:",
            )
            || self.lease_identity.is_some() != self.lease_generation.is_some()
            || self.lease_identity.as_deref().is_some_and(|value| {
                !valid_identity(value, "background-job-scheduler-lease:sha256:")
            })
            || self.lease_generation == Some(0)
            || self
                .process_observation_identity
                .as_deref()
                .is_some_and(|value| {
                    !valid_identity(value, "background-job-process-observation:sha256:")
                })
            || self.blocker_count < 2
            || self.blocker_count > 16
            || usize::from(self.blocker_count) != self.blockers.len()
            || unique_blockers.len() != self.blockers.len()
            || self
                .blockers
                .iter()
                .any(|blocker| !valid_identifier(blocker))
            || !self
                .blockers
                .iter()
                .any(|blocker| blocker == "dispatch-release-gate-disabled")
            || !valid_identity(
                &self.blockers_identity,
                "background-job-recovery-blockers:sha256:",
            )
            || self.blockers_identity != blockers_identity(&self.blockers)?
            || self.observed_at_ms == 0
            || self.recorded_at_ms < self.observed_at_ms
            || self.approval_required != (self.job_status == BackgroundJobStatus::WaitingApproval)
            || self.cancellation_acknowledgement_required
                != (self.cancellation == JobCancellationState::Requested)
            || self.automatic_retry
            || self.automatic_approval
            || self.automatic_takeover
            || self.dispatch_authority
            || self.mutation_authority
        {
            return Err(error(
                "background-recovery-decision-invalid",
                "background recovery decision invariant is invalid",
            ));
        }
        let disposition_valid = match self.disposition {
            BackgroundRecoveryDisposition::AwaitSchedule => {
                self.job_status == BackgroundJobStatus::Queued
                    && self.cancellation != JobCancellationState::Requested
                    && self
                        .next_eligible_at_ms
                        .is_some_and(|eligible| eligible > self.observed_at_ms)
            }
            BackgroundRecoveryDisposition::AdmissionReviewRequired => {
                self.job_status == BackgroundJobStatus::Queued
                    && self.cancellation != JobCancellationState::Requested
                    && self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::KeepPaused => {
                self.job_status == BackgroundJobStatus::Paused
                    && self.cancellation != JobCancellationState::Requested
                    && self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::KeepWaitingApproval => {
                self.job_status == BackgroundJobStatus::WaitingApproval
                    && self.cancellation != JobCancellationState::Requested
                    && self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::RetryReviewRequired => {
                matches!(
                    self.job_status,
                    BackgroundJobStatus::Failed | BackgroundJobStatus::Interrupted
                ) && self.cancellation != JobCancellationState::Requested
                    && self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::MonitorOwnedProcess => {
                matches!(
                    self.job_status,
                    BackgroundJobStatus::Running
                        | BackgroundJobStatus::PauseRequested
                        | BackgroundJobStatus::Cancelling
                ) && self.lease_state == SchedulerLeaseState::Current
                    && self.process_ownership_state == SchedulerProcessOwnershipState::Current
                    && self.process_observation_identity.is_some()
                    && self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::ManualReconciliationRequired => {
                self.next_eligible_at_ms.is_none()
            }
            BackgroundRecoveryDisposition::TerminalReviewRequired => {
                self.job_status.is_terminal()
                    && self.cancellation != JobCancellationState::Requested
                    && self.next_eligible_at_ms.is_none()
            }
        };
        if !disposition_valid
            || self
                .disposition
                .required_blocker()
                .is_some_and(|required| !self.blockers.iter().any(|value| value == required))
        {
            return Err(error(
                "background-recovery-disposition-invalid",
                "background recovery disposition does not match its job evidence",
            ));
        }
        if self.lease_state == SchedulerLeaseState::Missing
            && (self.lease_identity.is_some() || self.lease_generation.is_some())
        {
            return Err(error(
                "background-recovery-lease-invalid",
                "missing background recovery lease cannot carry lease evidence",
            ));
        }
        if self.lease_state != SchedulerLeaseState::Missing
            && (self.lease_identity.is_none() || self.lease_generation.is_none())
        {
            return Err(error(
                "background-recovery-lease-invalid",
                "background recovery lease state is missing lease evidence",
            ));
        }
        if self.decision_identity != decision_identity(self)? {
            return Err(error(
                "background-recovery-identity-invalid",
                "background recovery decision identity is invalid",
            ));
        }
        Ok(())
    }

    pub fn validate_against_snapshot(
        &self,
        snapshot: &BackgroundSchedulerSnapshot,
    ) -> Result<(), BackgroundRecoveryDecisionError> {
        self.validate()?;
        let expected = Self::from_snapshot(snapshot, &self.job_id, self.recorded_at_ms)?;
        if expected != *self {
            return Err(error(
                "background-recovery-snapshot-stale",
                "background recovery decision does not match its scheduler snapshot",
            ));
        }
        Ok(())
    }
}

fn blockers_identity(blockers: &[String]) -> Result<String, BackgroundRecoveryDecisionError> {
    let bytes = to_vec(blockers).map_err(|_| {
        error(
            "background-recovery-blockers-invalid",
            "background recovery blockers could not be serialized",
        )
    })?;
    Ok(format!(
        "background-job-recovery-blockers:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn decision_identity(
    decision: &BackgroundRecoveryDecision,
) -> Result<String, BackgroundRecoveryDecisionError> {
    let bytes = to_vec(&DecisionIdentityBinding {
        schema_version: &decision.schema_version,
        job_id: &decision.job_id,
        session_id: &decision.session_id,
        project_id: &decision.project_id,
        root_id: &decision.root_id,
        request_identity: &decision.request_identity,
        state_identity: &decision.state_identity,
        job_generation: decision.job_generation,
        job_status: decision.job_status,
        cancellation: decision.cancellation,
        next_eligible_at_ms: decision.next_eligible_at_ms,
        scheduler_owner_identity: &decision.scheduler_owner_identity,
        scheduler_generation: decision.scheduler_generation,
        scheduler_snapshot_identity: &decision.scheduler_snapshot_identity,
        scheduler_entry_identity: &decision.scheduler_entry_identity,
        disposition: decision.disposition,
        lease_state: decision.lease_state,
        lease_identity: decision.lease_identity.as_deref(),
        lease_generation: decision.lease_generation,
        process_ownership_state: decision.process_ownership_state,
        process_observation_identity: decision.process_observation_identity.as_deref(),
        blockers: &decision.blockers,
        blocker_count: decision.blocker_count,
        blockers_identity: &decision.blockers_identity,
        approval_required: decision.approval_required,
        cancellation_acknowledgement_required: decision.cancellation_acknowledgement_required,
        observed_at_ms: decision.observed_at_ms,
        recorded_at_ms: decision.recorded_at_ms,
        automatic_retry: decision.automatic_retry,
        automatic_approval: decision.automatic_approval,
        automatic_takeover: decision.automatic_takeover,
        dispatch_authority: decision.dispatch_authority,
        mutation_authority: decision.mutation_authority,
    })
    .map_err(|_| {
        error(
            "background-recovery-identity-failed",
            "background recovery decision identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-recovery-decision:sha256:{:x}",
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

fn valid_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn error(code: &'static str, message: &'static str) -> BackgroundRecoveryDecisionError {
    BackgroundRecoveryDecisionError::new(code, message)
}
