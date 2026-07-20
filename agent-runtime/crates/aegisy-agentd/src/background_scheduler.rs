//! Read-only scheduler ownership and recovery planning for durable background jobs.
//!
//! This module owns a bounded inspection snapshot only. It does not lease a process,
//! mutate a job, approve, retry, notify, or dispatch work. Active jobs without an
//! authoritative process observation always require manual reconciliation.

use crate::background_job::{BackgroundJobStatus, JobCancellationState, JobRecoveryDisposition};
use crate::background_process_observation::{
    process_observation_required, BackgroundProcessObservation, BackgroundProcessObservationState,
    BackgroundProcessRegistry, VerifiedBackgroundProcessObservation,
};
use crate::background_scheduler_lease::{
    BackgroundSchedulerLeaseStatus, SCHEMA_VERSION as LEASE_SCHEMA_VERSION,
};
use crate::workbench_store::{StoredBackgroundJob, StoredBackgroundSchedulerLease, WorkbenchStore};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const SCHEMA_VERSION: &str = "background-job-scheduler/0.2";
const MAX_RECOVERY_JOBS: usize = 1_000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundSchedulerError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundSchedulerError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SchedulerRecoveryAction {
    AwaitSchedule,
    AdmissionReview,
    KeepPaused,
    KeepWaitingApproval,
    RetryReviewRequired,
    MonitorOwnedProcess,
    ManualReconciliation,
    TerminalReview,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SchedulerLeaseState {
    Missing,
    Current,
    Expired,
    Released,
    StateStale,
    OwnerMismatch,
}

impl SchedulerLeaseState {
    fn blocker_code(self) -> &'static str {
        match self {
            Self::Missing => "scheduler-lease-missing",
            Self::Current => "scheduler-lease-current",
            Self::Expired => "scheduler-lease-expired",
            Self::Released => "scheduler-lease-released",
            Self::StateStale => "scheduler-lease-state-stale",
            Self::OwnerMismatch => "scheduler-lease-owner-mismatch",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SchedulerProcessOwnershipState {
    NotRequired,
    MissingLease,
    MissingRegistration,
    ObservationUnavailable,
    ObservedNotRunning,
    Mismatched,
    Current,
}

impl SchedulerProcessOwnershipState {
    fn blocker_code(self) -> &'static str {
        match self {
            Self::NotRequired => "process-ownership-not-required",
            Self::MissingLease => "process-ownership-lease-missing",
            Self::MissingRegistration => "process-ownership-registration-missing",
            Self::ObservationUnavailable => "process-ownership-observation-unavailable",
            Self::ObservedNotRunning => "process-ownership-observed-not-running",
            Self::Mismatched => "process-ownership-mismatched",
            Self::Current => "process-ownership-current",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SchedulerRecoveryEntry {
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub status: BackgroundJobStatus,
    pub cancellation: JobCancellationState,
    pub job_generation: u64,
    pub action: SchedulerRecoveryAction,
    pub next_eligible_at_ms: Option<u64>,
    pub lease_required: bool,
    pub lease_state: SchedulerLeaseState,
    pub lease_schema_version: Option<String>,
    pub lease_identity: Option<String>,
    pub lease_generation: Option<u64>,
    pub lease_expires_at_ms: Option<u64>,
    pub process_ownership_state: SchedulerProcessOwnershipState,
    pub process_observation_required: bool,
    pub process_observation: Option<BackgroundProcessObservation>,
    pub approval_required: bool,
    pub cancellation_acknowledgement_required: bool,
    pub automatic_retry: bool,
    pub automatic_approval: bool,
    pub automatic_takeover: bool,
    pub dispatch_available: bool,
    pub blockers: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundSchedulerSnapshot {
    pub schema_version: String,
    pub owner_identity: String,
    pub generation: u64,
    pub observed_at_ms: u64,
    pub job_count: usize,
    pub process_observation_available: bool,
    pub durable_lease_available: bool,
    pub notification_available: bool,
    pub dispatch_available: bool,
    pub entries: Vec<SchedulerRecoveryEntry>,
    pub snapshot_identity: String,
}

#[derive(Debug)]
pub struct BackgroundJobScheduler {
    owner_identity: String,
    generation: u64,
    observed_at_ms: u64,
    process_observation_available: bool,
    durable_lease_available: bool,
    entries: BTreeMap<String, SchedulerRecoveryEntry>,
}

#[derive(Serialize)]
struct SnapshotBinding<'a> {
    schema_version: &'a str,
    owner_identity: &'a str,
    generation: u64,
    observed_at_ms: u64,
    process_observation_available: bool,
    durable_lease_available: bool,
    notification_available: bool,
    dispatch_available: bool,
    entries: &'a [SchedulerRecoveryEntry],
}

impl BackgroundJobScheduler {
    pub fn load(
        store: &WorkbenchStore,
        owner_identity: impl Into<String>,
        now_ms: u64,
        limit: usize,
    ) -> Result<Self, BackgroundSchedulerError> {
        let mut scheduler = Self::new(owner_identity)?;
        scheduler.refresh(store, now_ms, limit)?;
        Ok(scheduler)
    }

    pub fn load_with_process_registry(
        store: &WorkbenchStore,
        process_registry: &mut BackgroundProcessRegistry,
        owner_identity: impl Into<String>,
        now_ms: u64,
        limit: usize,
    ) -> Result<Self, BackgroundSchedulerError> {
        let mut scheduler = Self::new(owner_identity)?;
        scheduler.refresh_with_process_registry(store, process_registry, now_ms, limit)?;
        Ok(scheduler)
    }

    fn new(owner_identity: impl Into<String>) -> Result<Self, BackgroundSchedulerError> {
        let owner_identity = owner_identity.into();
        validate_owner_identity(&owner_identity)?;
        Ok(Self {
            owner_identity,
            generation: 0,
            observed_at_ms: 0,
            process_observation_available: false,
            durable_lease_available: true,
            entries: BTreeMap::new(),
        })
    }

    pub fn refresh(
        &mut self,
        store: &WorkbenchStore,
        now_ms: u64,
        limit: usize,
    ) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
        self.refresh_internal(store, None, now_ms, limit)
    }

    pub fn refresh_with_process_registry(
        &mut self,
        store: &WorkbenchStore,
        process_registry: &mut BackgroundProcessRegistry,
        now_ms: u64,
        limit: usize,
    ) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
        self.refresh_internal(store, Some(process_registry), now_ms, limit)
    }

    fn refresh_internal(
        &mut self,
        store: &WorkbenchStore,
        mut process_registry: Option<&mut BackgroundProcessRegistry>,
        now_ms: u64,
        limit: usize,
    ) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
        if now_ms == 0 || now_ms < self.observed_at_ms {
            return Err(error(
                "background-scheduler-time-invalid",
                "scheduler observation time is invalid",
            ));
        }
        if limit == 0 || limit > MAX_RECOVERY_JOBS {
            return Err(error(
                "background-scheduler-limit-invalid",
                "scheduler recovery limit is invalid",
            ));
        }
        let records = store
            .load_background_jobs_for_recovery(limit)
            .map_err(|_| {
                error(
                    "background-scheduler-store-unavailable",
                    "scheduler could not load verified durable jobs",
                )
            })?;
        let next_generation = self.generation.checked_add(1).ok_or_else(|| {
            error(
                "background-scheduler-generation-exhausted",
                "scheduler generation is exhausted",
            )
        })?;
        let process_observation_available = process_registry.is_some();
        let mut next_entries = BTreeMap::new();
        for record in records {
            let lease = store
                .load_background_scheduler_lease(&record.request.job_id)
                .map_err(|_| {
                    error(
                        "background-scheduler-lease-store-unavailable",
                        "scheduler could not load verified durable lease evidence",
                    )
                })?;
            let observation = if process_observation_required(record.state.status) {
                process_registry
                    .as_deref_mut()
                    .map(|registry| {
                        registry.observe_for_scheduler(
                            &record.request,
                            &record.state,
                            &self.owner_identity,
                            now_ms,
                        )
                    })
                    .transpose()
                    .map_err(|_| {
                        error(
                            "background-scheduler-process-observation-invalid",
                            "scheduler could not verify runtime-owned process evidence",
                        )
                    })?
            } else {
                None
            };
            let entry = recovery_entry(
                &record,
                lease.as_ref(),
                &self.owner_identity,
                now_ms,
                observation.as_ref(),
            )?;
            if next_entries.insert(entry.job_id.clone(), entry).is_some() {
                return Err(error(
                    "background-scheduler-duplicate-job",
                    "scheduler recovery input contains a duplicate job",
                ));
            }
        }
        let snapshot = snapshot(
            &self.owner_identity,
            next_generation,
            now_ms,
            process_observation_available,
            self.durable_lease_available,
            next_entries.values().cloned().collect(),
        )?;
        self.generation = next_generation;
        self.observed_at_ms = now_ms;
        self.process_observation_available = process_observation_available;
        self.entries = next_entries;
        Ok(snapshot)
    }

    pub fn snapshot(&self) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
        snapshot(
            &self.owner_identity,
            self.generation,
            self.observed_at_ms,
            self.process_observation_available,
            self.durable_lease_available,
            self.entries.values().cloned().collect(),
        )
    }
}

fn recovery_entry(
    record: &StoredBackgroundJob,
    stored_lease: Option<&StoredBackgroundSchedulerLease>,
    scheduler_owner_identity: &str,
    now_ms: u64,
    verified_observation: Option<&VerifiedBackgroundProcessObservation>,
) -> Result<SchedulerRecoveryEntry, BackgroundSchedulerError> {
    record.request.validate().map_err(|_| {
        error(
            "background-scheduler-request-invalid",
            "scheduler received an invalid durable request",
        )
    })?;
    record.state.validate(&record.request).map_err(|_| {
        error(
            "background-scheduler-state-invalid",
            "scheduler received an invalid durable state",
        )
    })?;
    let request_identity = record.request.identity().map_err(|_| {
        error(
            "background-scheduler-request-invalid",
            "scheduler could not bind the durable request",
        )
    })?;
    let state_identity = record.state.identity(&record.request).map_err(|_| {
        error(
            "background-scheduler-state-invalid",
            "scheduler could not bind the durable state",
        )
    })?;
    let decision = record.state.recovery_decision(&record.request, now_ms);
    let mut action = match decision.disposition {
        JobRecoveryDisposition::RunWhenDue if record.state.next_eligible_at_ms > now_ms => {
            SchedulerRecoveryAction::AwaitSchedule
        }
        JobRecoveryDisposition::RunWhenDue => SchedulerRecoveryAction::AdmissionReview,
        JobRecoveryDisposition::KeepPaused => SchedulerRecoveryAction::KeepPaused,
        JobRecoveryDisposition::KeepWaitingApproval => SchedulerRecoveryAction::KeepWaitingApproval,
        JobRecoveryDisposition::RetryEligible => SchedulerRecoveryAction::RetryReviewRequired,
        JobRecoveryDisposition::ManualReconciliation => {
            SchedulerRecoveryAction::ManualReconciliation
        }
        JobRecoveryDisposition::Terminal => SchedulerRecoveryAction::TerminalReview,
    };
    let lease_required = action != SchedulerRecoveryAction::TerminalReview;
    let lease_state = classify_lease(
        record,
        stored_lease,
        scheduler_owner_identity,
        now_ms,
        &request_identity,
        &state_identity,
    )?;
    let requires_process_observation = process_observation_required(record.state.status);
    let process_observation = verified_observation.map(|verified| verified.observation().clone());
    if let Some(observation) = &process_observation {
        observation.validate().map_err(|_| {
            error(
                "background-scheduler-process-observation-invalid",
                "scheduler process observation is invalid",
            )
        })?;
        if observation.job_id != record.request.job_id
            || observation.session_id != record.request.session_id
            || observation.project_id != record.request.project_id
            || observation.root_id != record.request.root_id
            || observation.request_identity != request_identity
            || observation.state_identity != state_identity
            || observation.job_generation != record.state.generation
        {
            return Err(error(
                "background-scheduler-process-binding-invalid",
                "scheduler process observation does not bind the durable job",
            ));
        }
    }
    let process_ownership_state = classify_process_ownership(
        requires_process_observation,
        lease_state,
        stored_lease,
        process_observation.as_ref(),
    );
    if requires_process_observation {
        action = if lease_state == SchedulerLeaseState::Current
            && process_ownership_state == SchedulerProcessOwnershipState::Current
        {
            SchedulerRecoveryAction::MonitorOwnedProcess
        } else {
            SchedulerRecoveryAction::ManualReconciliation
        };
    }
    let approval_required = record.state.status == BackgroundJobStatus::WaitingApproval;
    let cancellation_acknowledgement_required =
        record.state.cancellation == JobCancellationState::Requested;
    let mut blockers = vec!["dispatch-release-gate-disabled".into()];
    match action {
        SchedulerRecoveryAction::AwaitSchedule => blockers.push("schedule-not-due".into()),
        SchedulerRecoveryAction::AdmissionReview => {
            blockers.push("admission-review-required".into())
        }
        SchedulerRecoveryAction::KeepPaused => blockers.push("job-paused".into()),
        SchedulerRecoveryAction::KeepWaitingApproval => blockers.push("approval-required".into()),
        SchedulerRecoveryAction::RetryReviewRequired => {
            blockers.push("retry-review-required".into())
        }
        SchedulerRecoveryAction::MonitorOwnedProcess => {
            blockers.push("owned-process-running".into())
        }
        SchedulerRecoveryAction::ManualReconciliation => blockers.push(
            if requires_process_observation {
                process_observation
                    .as_ref()
                    .map(|observation| observation.state.blocker_code())
                    .unwrap_or("process-observation-required")
            } else if cancellation_acknowledgement_required {
                "cancellation-acknowledgement-required"
            } else {
                "manual-review-required"
            }
            .into(),
        ),
        SchedulerRecoveryAction::TerminalReview => blockers.push("terminal-review-only".into()),
    }
    if cancellation_acknowledgement_required
        && !blockers
            .iter()
            .any(|blocker| blocker == "cancellation-acknowledgement-required")
    {
        blockers.push("cancellation-acknowledgement-required".into());
    }
    if record.state.status == BackgroundJobStatus::PauseRequested {
        blockers.push("pause-acknowledgement-required".into());
    }
    if lease_required && lease_state != SchedulerLeaseState::Current {
        blockers.push(lease_state.blocker_code().into());
    }
    if requires_process_observation
        && process_ownership_state != SchedulerProcessOwnershipState::Current
    {
        blockers.push(process_ownership_state.blocker_code().into());
    }
    if !lease_required
        && stored_lease
            .is_some_and(|stored| stored.lease.status == BackgroundSchedulerLeaseStatus::Active)
    {
        blockers.push("terminal-lease-release-required".into());
    }
    Ok(SchedulerRecoveryEntry {
        job_id: record.request.job_id.clone(),
        session_id: record.request.session_id.clone(),
        project_id: record.request.project_id.clone(),
        root_id: record.request.root_id.clone(),
        request_identity,
        state_identity,
        status: record.state.status,
        cancellation: record.state.cancellation,
        job_generation: record.state.generation,
        action,
        next_eligible_at_ms: (action == SchedulerRecoveryAction::AwaitSchedule)
            .then_some(record.state.next_eligible_at_ms),
        lease_required,
        lease_state,
        lease_schema_version: stored_lease.map(|stored| stored.lease.schema_version.clone()),
        lease_identity: stored_lease.map(|stored| stored.lease.lease_identity.clone()),
        lease_generation: stored_lease.map(|stored| stored.lease.lease_generation),
        lease_expires_at_ms: stored_lease.map(|stored| stored.lease.expires_at_ms),
        process_ownership_state,
        process_observation_required: requires_process_observation,
        process_observation,
        approval_required,
        cancellation_acknowledgement_required,
        automatic_retry: false,
        automatic_approval: false,
        automatic_takeover: false,
        dispatch_available: false,
        blockers,
    })
}

fn classify_lease(
    record: &StoredBackgroundJob,
    stored_lease: Option<&StoredBackgroundSchedulerLease>,
    scheduler_owner_identity: &str,
    now_ms: u64,
    request_identity: &str,
    state_identity: &str,
) -> Result<SchedulerLeaseState, BackgroundSchedulerError> {
    let Some(stored) = stored_lease else {
        return Ok(SchedulerLeaseState::Missing);
    };
    if stored.schema_version != "stored-background-scheduler-lease/0.1"
        || stored.lease.schema_version != LEASE_SCHEMA_VERSION
        || stored.lease.validate(&record.request).is_err()
    {
        return Err(error(
            "background-scheduler-lease-invalid",
            "scheduler received invalid durable lease evidence",
        ));
    }
    let lease = &stored.lease;
    Ok(match lease.status {
        BackgroundSchedulerLeaseStatus::Released => SchedulerLeaseState::Released,
        BackgroundSchedulerLeaseStatus::Expired => SchedulerLeaseState::Expired,
        BackgroundSchedulerLeaseStatus::Active if now_ms >= lease.expires_at_ms => {
            SchedulerLeaseState::Expired
        }
        BackgroundSchedulerLeaseStatus::Active
            if lease.owner_identity != scheduler_owner_identity =>
        {
            SchedulerLeaseState::OwnerMismatch
        }
        BackgroundSchedulerLeaseStatus::Active
            if lease.job_id != record.request.job_id
                || lease.session_id != record.request.session_id
                || lease.project_id != record.request.project_id
                || lease.root_id != record.request.root_id
                || lease.request_identity != request_identity
                || lease.state_identity != state_identity
                || lease.job_generation != record.state.generation =>
        {
            SchedulerLeaseState::StateStale
        }
        BackgroundSchedulerLeaseStatus::Active => SchedulerLeaseState::Current,
    })
}

fn classify_process_ownership(
    required: bool,
    lease_state: SchedulerLeaseState,
    stored_lease: Option<&StoredBackgroundSchedulerLease>,
    observation: Option<&BackgroundProcessObservation>,
) -> SchedulerProcessOwnershipState {
    if !required {
        return SchedulerProcessOwnershipState::NotRequired;
    }
    if lease_state == SchedulerLeaseState::Missing {
        return SchedulerProcessOwnershipState::MissingLease;
    }
    if lease_state != SchedulerLeaseState::Current {
        return SchedulerProcessOwnershipState::Mismatched;
    }
    let Some(lease) = stored_lease.map(|stored| &stored.lease) else {
        return SchedulerProcessOwnershipState::MissingLease;
    };
    let (Some(leased_registration_identity), Some(leased_process_identity)) = (
        lease.process_registration_identity.as_deref(),
        lease.process_identity.as_deref(),
    ) else {
        return SchedulerProcessOwnershipState::MissingRegistration;
    };
    let Some(observation) = observation else {
        return SchedulerProcessOwnershipState::ObservationUnavailable;
    };
    match observation.state {
        BackgroundProcessObservationState::OwnedRunning
            if observation.process_registration_identity.as_deref()
                == Some(leased_registration_identity)
                && observation.process_identity.as_deref() == Some(leased_process_identity) =>
        {
            SchedulerProcessOwnershipState::Current
        }
        BackgroundProcessObservationState::OwnedExited
            if observation.process_registration_identity.as_deref()
                == Some(leased_registration_identity)
                && observation.process_identity.as_deref() == Some(leased_process_identity) =>
        {
            SchedulerProcessOwnershipState::ObservedNotRunning
        }
        BackgroundProcessObservationState::Absent
        | BackgroundProcessObservationState::Inaccessible
        | BackgroundProcessObservationState::Unknown => {
            SchedulerProcessOwnershipState::ObservationUnavailable
        }
        _ => SchedulerProcessOwnershipState::Mismatched,
    }
}

fn snapshot(
    owner_identity: &str,
    generation: u64,
    observed_at_ms: u64,
    process_observation_available: bool,
    durable_lease_available: bool,
    entries: Vec<SchedulerRecoveryEntry>,
) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
    validate_owner_identity(owner_identity)?;
    if generation == 0
        || observed_at_ms == 0
        || !durable_lease_available
        || entries.len() > MAX_RECOVERY_JOBS
    {
        return Err(error(
            "background-scheduler-snapshot-invalid",
            "scheduler snapshot invariant is invalid",
        ));
    }
    if entries.iter().any(|entry| {
        entry.dispatch_available
            || entry.automatic_retry
            || entry.automatic_approval
            || entry.automatic_takeover
            || entry.blockers.is_empty()
    }) {
        return Err(error(
            "background-scheduler-authority-invalid",
            "scheduler snapshot cannot grant execution authority",
        ));
    }
    if entries.iter().any(|entry| {
        let observation = entry.process_observation.as_ref();
        observation.is_some_and(|value| {
            !process_observation_available
                || value.owner_identity != owner_identity
                || value.observed_at_ms != observed_at_ms
                || value.validate().is_err()
        }) || (process_observation_available
            && entry.process_observation_required
            && observation.is_none())
            || (!entry.process_observation_required && observation.is_some())
            || (entry.action == SchedulerRecoveryAction::MonitorOwnedProcess
                && (observation.is_none_or(|value| {
                    value.state != BackgroundProcessObservationState::OwnedRunning
                }) || entry.lease_state != SchedulerLeaseState::Current
                    || entry.process_ownership_state != SchedulerProcessOwnershipState::Current))
    }) {
        return Err(error(
            "background-scheduler-process-snapshot-invalid",
            "scheduler snapshot process evidence is invalid",
        ));
    }
    if entries.iter().any(|entry| {
        let has_lease = entry.lease_schema_version.is_some()
            && entry.lease_identity.is_some()
            && entry.lease_generation.is_some()
            && entry.lease_expires_at_ms.is_some();
        (entry.lease_state == SchedulerLeaseState::Missing && has_lease)
            || (entry.lease_state != SchedulerLeaseState::Missing && !has_lease)
            || entry
                .lease_schema_version
                .as_deref()
                .is_some_and(|value| value != LEASE_SCHEMA_VERSION)
            || (entry.lease_required
                && entry.lease_state != SchedulerLeaseState::Current
                && !entry
                    .blockers
                    .iter()
                    .any(|blocker| blocker == entry.lease_state.blocker_code()))
    }) {
        return Err(error(
            "background-scheduler-lease-snapshot-invalid",
            "scheduler snapshot durable lease evidence is invalid",
        ));
    }
    let binding = SnapshotBinding {
        schema_version: SCHEMA_VERSION,
        owner_identity,
        generation,
        observed_at_ms,
        process_observation_available,
        durable_lease_available,
        notification_available: false,
        dispatch_available: false,
        entries: &entries,
    };
    let bytes = to_vec(&binding).map_err(|_| {
        error(
            "background-scheduler-snapshot-serialize-failed",
            "scheduler snapshot could not be serialized",
        )
    })?;
    Ok(BackgroundSchedulerSnapshot {
        schema_version: SCHEMA_VERSION.into(),
        owner_identity: owner_identity.into(),
        generation,
        observed_at_ms,
        job_count: entries.len(),
        process_observation_available,
        durable_lease_available,
        notification_available: false,
        dispatch_available: false,
        entries,
        snapshot_identity: format!(
            "background-job-scheduler-snapshot:sha256:{:x}",
            Sha256::digest(bytes)
        ),
    })
}

fn validate_owner_identity(value: &str) -> Result<(), BackgroundSchedulerError> {
    if value
        .strip_prefix("scheduler-owner:sha256:")
        .is_none_or(|hex| {
            hex.len() != 64
                || hex
                    .bytes()
                    .any(|byte| !byte.is_ascii_hexdigit() || byte.is_ascii_uppercase())
        })
    {
        return Err(error(
            "background-scheduler-owner-invalid",
            "scheduler owner identity is invalid",
        ));
    }
    Ok(())
}

fn error(code: &'static str, message: &'static str) -> BackgroundSchedulerError {
    BackgroundSchedulerError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::background_job::{
        BackgroundJobRequest, BackgroundJobState, JobRetryPolicy, JobSchedule, JobScheduleKind,
        REQUEST_SCHEMA_VERSION,
    };
    use crate::background_process_observation::{
        BackgroundProcessObservationState, BackgroundProcessRegistry,
    };
    use crate::background_scheduler_lease::{
        BackgroundSchedulerLease, BackgroundSchedulerLeaseRebind,
    };
    use crate::workbench_store::{
        StoredProjectCreate, StoredSessionCreate, StoredSessionLineage, StoredSessionMode,
    };
    use std::fs;
    use std::path::PathBuf;
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    use std::process::{Command, Stdio};
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct Fixture {
        parent: PathBuf,
        data_root: PathBuf,
        store: WorkbenchStore,
    }

    impl Fixture {
        fn new(label: &str) -> Self {
            let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-background-scheduler-{label}-{}-{sequence}",
                std::process::id()
            ));
            let data_root = parent.join("data");
            let project_root = parent.join("project");
            fs::create_dir_all(&data_root).unwrap();
            fs::create_dir(&project_root).unwrap();
            let data_root = data_root.canonicalize().unwrap();
            let project_root = project_root.canonicalize().unwrap();
            let mut store = WorkbenchStore::open(&data_root).unwrap();
            store
                .create_project(StoredProjectCreate {
                    project_id: "scheduler-project".into(),
                    root_id: "root-1".into(),
                    canonical_root: project_root.to_string_lossy().into_owned(),
                    root_identity: identity("root:sha256:", 'a'),
                    display_name: "Scheduler project".into(),
                    root_access: "write".into(),
                    created_at_ms: 100,
                })
                .unwrap();
            store
                .create_session(StoredSessionCreate {
                    session_id: "scheduler-session".into(),
                    project_id: Some("scheduler-project".into()),
                    mode: StoredSessionMode::Work,
                    title: "Scheduler session".into(),
                    parent_session_id: None,
                    lineage_kind: StoredSessionLineage::New,
                    environment_identity: Some(identity("environment:sha256:", 'b')),
                    created_at_ms: 110,
                })
                .unwrap();
            Self {
                parent,
                data_root,
                store,
            }
        }

        fn persist(&mut self, job_id: &str, scheduled_for_ms: Option<u64>) -> BackgroundJobState {
            let request = job_request(job_id, scheduled_for_ms, true);
            let state = BackgroundJobState::new(&request, 120).unwrap();
            self.store.create_background_job(&request, &state).unwrap();
            state
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.parent);
        }
    }

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn owner() -> String {
        identity("scheduler-owner:sha256:", 'f')
    }

    fn job_request(
        job_id: &str,
        scheduled_for_ms: Option<u64>,
        safe_retry: bool,
    ) -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: REQUEST_SCHEMA_VERSION.into(),
            job_id: job_id.into(),
            session_id: "scheduler-session".into(),
            project_id: "scheduler-project".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'c'),
            idempotency_identity: identity(
                "idempotency:sha256:",
                job_id.bytes().next().map(char::from).unwrap_or('d'),
            ),
            child_task_identity: None,
            schedule: JobSchedule {
                kind: if scheduled_for_ms.is_some() {
                    JobScheduleKind::At
                } else {
                    JobScheduleKind::Manual
                },
                scheduled_for_ms,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 100,
                safe_retry_boundary_identity: safe_retry
                    .then(|| identity("retry-boundary:sha256:", 'e')),
            },
            created_at_ms: 120,
        }
    }

    #[test]
    fn queued_jobs_are_owned_for_review_without_dispatch_authority() {
        let mut fixture = Fixture::new("queued");
        fixture.persist("alpha", Some(500));
        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 200, 10).unwrap();
        let snapshot = scheduler.snapshot().unwrap();
        assert_eq!(snapshot.job_count, 1);
        assert!(!snapshot.dispatch_available);
        assert!(snapshot.durable_lease_available);
        assert!(!snapshot.process_observation_available);
        assert!(!snapshot.notification_available);
        assert_eq!(
            snapshot.entries[0].action,
            SchedulerRecoveryAction::AwaitSchedule
        );
        assert_eq!(snapshot.entries[0].next_eligible_at_ms, Some(500));
        assert!(!snapshot.entries[0].automatic_retry);
        assert!(!snapshot.entries[0].automatic_approval);
        assert!(!snapshot.entries[0].automatic_takeover);
        assert_eq!(
            snapshot.entries[0].lease_state,
            SchedulerLeaseState::Missing
        );
        assert!(snapshot.entries[0]
            .blockers
            .contains(&"scheduler-lease-missing".into()));
    }

    #[test]
    fn active_and_cancel_pending_jobs_require_manual_reconciliation() {
        let mut fixture = Fixture::new("active");
        let request = job_request("bravo", None, true);
        let mut state = BackgroundJobState::new(&request, 120).unwrap();
        fixture
            .store
            .create_background_job(&request, &state)
            .unwrap();
        let queued = state.clone();
        state.start(&request, 200).unwrap();
        fixture
            .store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 300, 10).unwrap();
        let entry = &scheduler.snapshot().unwrap().entries[0];
        assert_eq!(entry.action, SchedulerRecoveryAction::ManualReconciliation);
        assert!(entry.process_observation_required);
        assert!(!entry.dispatch_available);

        let running = state.clone();
        state.request_cancel(&request, 310).unwrap();
        fixture
            .store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let queued_cancel_request = job_request("delta", None, true);
        let mut queued_cancel = BackgroundJobState::new(&queued_cancel_request, 120).unwrap();
        fixture
            .store
            .create_background_job(&queued_cancel_request, &queued_cancel)
            .unwrap();
        let queued_before_cancel = queued_cancel.clone();
        queued_cancel
            .request_cancel(&queued_cancel_request, 315)
            .unwrap();
        fixture
            .store
            .update_background_job_state(
                &queued_cancel_request,
                &queued_before_cancel,
                &queued_cancel,
            )
            .unwrap();
        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 320, 10).unwrap();
        let snapshot = scheduler.snapshot().unwrap();
        let active_cancel = snapshot
            .entries
            .iter()
            .find(|entry| entry.job_id == "bravo")
            .unwrap();
        assert_eq!(
            active_cancel.action,
            SchedulerRecoveryAction::ManualReconciliation
        );
        assert!(active_cancel.process_observation_required);
        assert!(active_cancel.cancellation_acknowledgement_required);
        let queued_cancel = snapshot
            .entries
            .iter()
            .find(|entry| entry.job_id == "delta")
            .unwrap();
        assert_eq!(
            queued_cancel.action,
            SchedulerRecoveryAction::ManualReconciliation
        );
        assert!(!queued_cancel.process_observation_required);
        assert!(queued_cancel.cancellation_acknowledgement_required);
        assert!(queued_cancel
            .blockers
            .contains(&"cancellation-acknowledgement-required".into()));
    }

    #[test]
    fn approval_wait_and_retry_eligibility_never_become_automatic_actions() {
        let mut fixture = Fixture::new("approval-retry");
        let request = job_request("charlie", None, true);
        let mut state = BackgroundJobState::new(&request, 120).unwrap();
        fixture
            .store
            .create_background_job(&request, &state)
            .unwrap();
        let queued = state.clone();
        state.start(&request, 200).unwrap();
        let running = state.clone();
        fixture
            .store
            .update_background_job_state(&request, &queued, &running)
            .unwrap();
        state
            .wait_for_approval(&request, &identity("approval:sha256:", 'a'), 210)
            .unwrap();
        let waiting = state.clone();
        fixture
            .store
            .update_background_job_state(&request, &running, &waiting)
            .unwrap();
        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 220, 10).unwrap();
        let entry = &scheduler.snapshot().unwrap().entries[0];
        assert_eq!(entry.action, SchedulerRecoveryAction::KeepWaitingApproval);
        assert!(entry.approval_required);
        assert!(!entry.automatic_approval);

        state
            .fail(&request, &identity("job-evidence:sha256:", 'b'), true, 230)
            .unwrap();
        fixture
            .store
            .update_background_job_state(&request, &waiting, &state)
            .unwrap();
        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 240, 10).unwrap();
        let entry = &scheduler.snapshot().unwrap().entries[0];
        assert_eq!(entry.action, SchedulerRecoveryAction::RetryReviewRequired);
        assert!(!entry.automatic_retry);
        assert!(!entry.dispatch_available);
    }

    #[test]
    fn available_registry_reports_absence_without_inferring_process_outcome() {
        let mut fixture = Fixture::new("process-absent");
        let request = job_request("foxtrot", None, true);
        let mut state = BackgroundJobState::new(&request, 120).unwrap();
        fixture
            .store
            .create_background_job(&request, &state)
            .unwrap();
        let queued = state.clone();
        state.start(&request, 200).unwrap();
        fixture
            .store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let mut registry = BackgroundProcessRegistry::new(owner()).unwrap();
        let scheduler = BackgroundJobScheduler::load_with_process_registry(
            &fixture.store,
            &mut registry,
            owner(),
            220,
            10,
        )
        .unwrap();
        let snapshot = scheduler.snapshot().unwrap();
        let entry = &snapshot.entries[0];
        assert!(snapshot.process_observation_available);
        assert_eq!(entry.action, SchedulerRecoveryAction::ManualReconciliation);
        assert_eq!(entry.lease_state, SchedulerLeaseState::Missing);
        assert_eq!(
            entry.process_ownership_state,
            SchedulerProcessOwnershipState::MissingLease
        );
        assert_eq!(
            entry.process_observation.as_ref().unwrap().state,
            BackgroundProcessObservationState::Absent
        );
        assert!(entry.blockers.contains(&"owned-process-absent".into()));
        assert!(!entry.dispatch_available);
    }

    #[test]
    fn expired_and_state_stale_leases_never_enable_automatic_takeover() {
        let mut fixture = Fixture::new("lease-recovery");
        let expired_request = job_request("cafe", None, true);
        let expired_state = BackgroundJobState::new(&expired_request, 120).unwrap();
        fixture
            .store
            .create_background_job(&expired_request, &expired_state)
            .unwrap();
        let expired_lease = crate::background_scheduler_lease::BackgroundSchedulerLease::acquire(
            &expired_request,
            &expired_state,
            owner(),
            130,
            1_000,
        )
        .unwrap();
        fixture
            .store
            .create_background_scheduler_lease(&expired_request, &expired_state, &expired_lease)
            .unwrap();

        let stale_request = job_request("dead", None, true);
        let mut stale_state = BackgroundJobState::new(&stale_request, 120).unwrap();
        fixture
            .store
            .create_background_job(&stale_request, &stale_state)
            .unwrap();
        let stale_lease = crate::background_scheduler_lease::BackgroundSchedulerLease::acquire(
            &stale_request,
            &stale_state,
            owner(),
            130,
            2_000,
        )
        .unwrap();
        fixture
            .store
            .create_background_scheduler_lease(&stale_request, &stale_state, &stale_lease)
            .unwrap();
        let queued = stale_state.clone();
        stale_state.start(&stale_request, 200).unwrap();
        fixture
            .store
            .update_background_job_state(&stale_request, &queued, &stale_state)
            .unwrap();

        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 1_200, 10).unwrap();
        let snapshot = scheduler.snapshot().unwrap();
        let expired = snapshot
            .entries
            .iter()
            .find(|entry| entry.job_id == "cafe")
            .unwrap();
        assert_eq!(expired.lease_state, SchedulerLeaseState::Expired);
        assert!(expired.blockers.contains(&"scheduler-lease-expired".into()));
        assert!(!expired.automatic_takeover);

        let stale = snapshot
            .entries
            .iter()
            .find(|entry| entry.job_id == "dead")
            .unwrap();
        assert_eq!(stale.lease_state, SchedulerLeaseState::StateStale);
        assert_eq!(stale.action, SchedulerRecoveryAction::ManualReconciliation);
        assert!(stale
            .blockers
            .contains(&"scheduler-lease-state-stale".into()));
        assert!(!stale.automatic_takeover);
        assert!(!stale.dispatch_available);
    }

    #[test]
    fn terminal_job_with_active_lease_remains_visible_until_explicit_release() {
        let mut fixture = Fixture::new("terminal-lease");
        let request = job_request("face", None, true);
        let mut state = BackgroundJobState::new(&request, 120).unwrap();
        fixture
            .store
            .create_background_job(&request, &state)
            .unwrap();
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner(), 130, 2_000).unwrap();
        fixture
            .store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();

        let queued_state = state.clone();
        state.start(&request, 200).unwrap();
        fixture
            .store
            .update_background_job_state(&request, &queued_state, &state)
            .unwrap();
        let queued_lease = lease.clone();
        lease
            .rebind_job_state(
                &request,
                BackgroundSchedulerLeaseRebind {
                    previous: &queued_state,
                    next: &state,
                    owner_identity: &owner(),
                    process_registration: None,
                    now_ms: 210,
                    ttl_ms: 2_000,
                },
            )
            .unwrap();
        fixture
            .store
            .update_background_scheduler_lease(&request, &state, &queued_lease, &lease)
            .unwrap();

        let running_state = state.clone();
        state
            .complete(
                &request,
                &identity("artifact:sha256:", 'a'),
                &identity("job-evidence:sha256:", 'b'),
                300,
            )
            .unwrap();
        fixture
            .store
            .update_background_job_state(&request, &running_state, &state)
            .unwrap();
        let running_lease = lease.clone();
        lease
            .rebind_job_state(
                &request,
                BackgroundSchedulerLeaseRebind {
                    previous: &running_state,
                    next: &state,
                    owner_identity: &owner(),
                    process_registration: None,
                    now_ms: 310,
                    ttl_ms: 2_000,
                },
            )
            .unwrap();
        fixture
            .store
            .update_background_scheduler_lease(&request, &state, &running_lease, &lease)
            .unwrap();

        let scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 320, 10).unwrap();
        let entry = &scheduler.snapshot().unwrap().entries[0];
        assert_eq!(entry.action, SchedulerRecoveryAction::TerminalReview);
        assert_eq!(entry.lease_state, SchedulerLeaseState::Current);
        assert!(!entry.lease_required);
        assert!(entry
            .blockers
            .contains(&"terminal-lease-release-required".into()));
        assert!(!entry.automatic_takeover);
        assert!(!entry.dispatch_available);
    }

    #[cfg(any(target_os = "macos", target_os = "windows"))]
    #[test]
    fn runtime_owned_running_process_is_monitored_without_dispatch_authority() {
        let mut fixture = Fixture::new("process-running");
        let request = job_request("beacon", None, true);
        let mut state = BackgroundJobState::new(&request, 120).unwrap();
        fixture
            .store
            .create_background_job(&request, &state)
            .unwrap();
        let queued = state.clone();
        state.start(&request, 200).unwrap();
        fixture
            .store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();

        let mut command = blocking_command();
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let input = child.stdin.take().unwrap();
        let mut registry = BackgroundProcessRegistry::new(owner()).unwrap();
        let registration = registry
            .register_spawned_child(&request, &state, child, 200)
            .unwrap();
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner(), 205, 2_000).unwrap();
        fixture
            .store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();
        let unbound_lease = lease.clone();
        lease
            .bind_process(&request, &state, &owner(), &registration, 210)
            .unwrap();
        fixture
            .store
            .update_background_scheduler_lease(&request, &state, &unbound_lease, &lease)
            .unwrap();
        let scheduler = BackgroundJobScheduler::load_with_process_registry(
            &fixture.store,
            &mut registry,
            owner(),
            220,
            10,
        )
        .unwrap();
        let entry = &scheduler.snapshot().unwrap().entries[0];
        assert_eq!(entry.action, SchedulerRecoveryAction::MonitorOwnedProcess);
        assert_eq!(entry.lease_state, SchedulerLeaseState::Current);
        assert_eq!(
            entry.process_ownership_state,
            SchedulerProcessOwnershipState::Current
        );
        let observation = entry.process_observation.as_ref().unwrap();
        assert_eq!(
            observation.state,
            BackgroundProcessObservationState::OwnedRunning
        );
        assert!(!observation.completion_inferred);
        assert!(!observation.dispatch_authority);
        assert!(!entry.dispatch_available);
        let stored_lease = fixture
            .store
            .load_background_scheduler_lease(&request.job_id)
            .unwrap()
            .unwrap();
        let mut mismatched_registration = observation.clone();
        mismatched_registration.process_registration_identity =
            Some(identity("background-job-process-registration:sha256:", 'f'));
        assert_eq!(
            classify_process_ownership(
                true,
                SchedulerLeaseState::Current,
                Some(&stored_lease),
                Some(&mismatched_registration),
            ),
            SchedulerProcessOwnershipState::Mismatched
        );
        drop(input);
    }

    #[test]
    fn refresh_is_transactional_and_rejects_invalid_owner_time_and_limit() {
        let mut fixture = Fixture::new("transactional");
        fixture.persist("delta", None);
        assert_eq!(
            BackgroundJobScheduler::load(&fixture.store, "invalid", 200, 10)
                .unwrap_err()
                .code,
            "background-scheduler-owner-invalid"
        );
        let mut scheduler = BackgroundJobScheduler::load(&fixture.store, owner(), 200, 10).unwrap();
        let before = scheduler.snapshot().unwrap();
        assert_eq!(
            scheduler.refresh(&fixture.store, 199, 10).unwrap_err().code,
            "background-scheduler-time-invalid"
        );
        assert_eq!(
            scheduler.refresh(&fixture.store, 201, 0).unwrap_err().code,
            "background-scheduler-limit-invalid"
        );
        assert_eq!(scheduler.snapshot().unwrap(), before);
        fixture.persist("echo", None);
        assert_eq!(
            scheduler.refresh(&fixture.store, 201, 1).unwrap_err().code,
            "background-scheduler-store-unavailable"
        );
        assert_eq!(scheduler.snapshot().unwrap(), before);
        assert!(fixture.data_root.exists());
    }

    #[cfg(target_os = "macos")]
    fn blocking_command() -> Command {
        let mut command = Command::new("/bin/sh");
        command.args(["-c", "read line"]);
        command
    }

    #[cfg(target_os = "windows")]
    fn blocking_command() -> Command {
        let mut command = Command::new("cmd.exe");
        command.args(["/D", "/Q", "/C", "more > NUL"]);
        command
    }
}
