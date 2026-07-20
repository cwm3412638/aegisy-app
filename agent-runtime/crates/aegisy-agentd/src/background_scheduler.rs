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
use crate::workbench_store::{StoredBackgroundJob, WorkbenchStore};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const SCHEMA_VERSION: &str = "background-job-scheduler/0.1";
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
    pub process_observation_required: bool,
    pub process_observation: Option<BackgroundProcessObservation>,
    pub approval_required: bool,
    pub cancellation_acknowledgement_required: bool,
    pub automatic_retry: bool,
    pub automatic_approval: bool,
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
    entries: BTreeMap<String, SchedulerRecoveryEntry>,
}

#[derive(Serialize)]
struct SnapshotBinding<'a> {
    schema_version: &'a str,
    owner_identity: &'a str,
    generation: u64,
    observed_at_ms: u64,
    process_observation_available: bool,
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
            let entry = recovery_entry(&record, now_ms, observation.as_ref())?;
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
            self.entries.values().cloned().collect(),
        )
    }
}

fn recovery_entry(
    record: &StoredBackgroundJob,
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
    if requires_process_observation {
        action = match process_observation.as_ref().map(|value| value.state) {
            Some(BackgroundProcessObservationState::OwnedRunning) => {
                SchedulerRecoveryAction::MonitorOwnedProcess
            }
            _ => SchedulerRecoveryAction::ManualReconciliation,
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
        process_observation_required: requires_process_observation,
        process_observation,
        approval_required,
        cancellation_acknowledgement_required,
        automatic_retry: false,
        automatic_approval: false,
        dispatch_available: false,
        blockers,
    })
}

fn snapshot(
    owner_identity: &str,
    generation: u64,
    observed_at_ms: u64,
    process_observation_available: bool,
    entries: Vec<SchedulerRecoveryEntry>,
) -> Result<BackgroundSchedulerSnapshot, BackgroundSchedulerError> {
    validate_owner_identity(owner_identity)?;
    if generation == 0 || observed_at_ms == 0 || entries.len() > MAX_RECOVERY_JOBS {
        return Err(error(
            "background-scheduler-snapshot-invalid",
            "scheduler snapshot invariant is invalid",
        ));
    }
    if entries.iter().any(|entry| {
        entry.dispatch_available
            || entry.automatic_retry
            || entry.automatic_approval
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
                && observation.is_none_or(|value| {
                    value.state != BackgroundProcessObservationState::OwnedRunning
                }))
            || (observation.is_some_and(|value| {
                value.state == BackgroundProcessObservationState::OwnedRunning
            }) && entry.action != SchedulerRecoveryAction::MonitorOwnedProcess)
    }) {
        return Err(error(
            "background-scheduler-process-snapshot-invalid",
            "scheduler snapshot process evidence is invalid",
        ));
    }
    let binding = SnapshotBinding {
        schema_version: SCHEMA_VERSION,
        owner_identity,
        generation,
        observed_at_ms,
        process_observation_available,
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
        assert!(!snapshot.process_observation_available);
        assert!(!snapshot.notification_available);
        assert_eq!(
            snapshot.entries[0].action,
            SchedulerRecoveryAction::AwaitSchedule
        );
        assert_eq!(snapshot.entries[0].next_eligible_at_ms, Some(500));
        assert!(!snapshot.entries[0].automatic_retry);
        assert!(!snapshot.entries[0].automatic_approval);
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
        assert_eq!(
            entry.process_observation.as_ref().unwrap().state,
            BackgroundProcessObservationState::Absent
        );
        assert!(entry.blockers.contains(&"owned-process-absent".into()));
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
        registry
            .register_spawned_child(&request, &state, child, 200)
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
        let observation = entry.process_observation.as_ref().unwrap();
        assert_eq!(
            observation.state,
            BackgroundProcessObservationState::OwnedRunning
        );
        assert!(!observation.completion_inferred);
        assert!(!observation.dispatch_authority);
        assert!(!entry.dispatch_available);
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
