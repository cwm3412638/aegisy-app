//! Runtime-owned process observation for durable background-job recovery.
//!
//! Registration consumes an actual [`std::process::Child`] handle. There is no API
//! that accepts a caller-selected PID. Observations contain only bound identities,
//! lifecycle state, and timestamps; process disappearance never means job success.

use crate::background_job::{
    BackgroundJobRequest, BackgroundJobState, BackgroundJobStatus, JobAttemptStatus,
};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::process::{Child, ExitStatus};
use std::thread;
use std::time::Duration;

pub const SCHEMA_VERSION: &str = "background-job-process-observation/0.1";
const OWNER_IDENTITY_PREFIX: &str = "scheduler-owner:sha256:";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackgroundProcessObservationError {
    pub code: &'static str,
    pub message: &'static str,
}

impl BackgroundProcessObservationError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundProcessObservationState {
    OwnedRunning,
    OwnedExited,
    Absent,
    Inaccessible,
    Mismatched,
    Unknown,
}

impl BackgroundProcessObservationState {
    pub fn blocker_code(self) -> &'static str {
        match self {
            Self::OwnedRunning => "owned-process-running",
            Self::OwnedExited => "owned-process-exited-terminal-event-required",
            Self::Absent => "owned-process-absent",
            Self::Inaccessible => "owned-process-inaccessible",
            Self::Mismatched => "owned-process-binding-mismatched",
            Self::Unknown => "owned-process-state-unknown",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundProcessRegistration {
    pub schema_version: String,
    pub owner_identity: String,
    pub job_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub job_generation: u64,
    pub attempt_number: u16,
    pub process_identity: String,
    pub registered_at_ms: u64,
    pub registration_identity: String,
}

#[derive(Debug)]
pub struct VerifiedBackgroundProcessRegistration {
    registration: BackgroundProcessRegistration,
}

impl VerifiedBackgroundProcessRegistration {
    pub fn registration(&self) -> &BackgroundProcessRegistration {
        &self.registration
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackgroundProcessObservation {
    pub schema_version: String,
    pub owner_identity: String,
    pub job_id: String,
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub request_identity: String,
    pub state_identity: String,
    pub job_generation: u64,
    pub attempt_number: Option<u16>,
    pub process_registration_identity: Option<String>,
    pub process_identity: Option<String>,
    pub state: BackgroundProcessObservationState,
    pub registered_at_ms: Option<u64>,
    pub observed_at_ms: u64,
    pub process_exit_code: Option<i32>,
    pub manual_reconciliation_required: bool,
    pub terminal_job_event_required: bool,
    pub completion_inferred: bool,
    pub dispatch_authority: bool,
    pub observation_identity: String,
}

impl BackgroundProcessObservation {
    pub fn validate(&self) -> Result<(), BackgroundProcessObservationError> {
        if self.schema_version != SCHEMA_VERSION
            || !valid_owner_identity(&self.owner_identity)
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
            || self
                .process_registration_identity
                .as_deref()
                .is_some_and(|value| {
                    !valid_identity(value, "background-job-process-registration:sha256:")
                })
            || self
                .process_identity
                .as_deref()
                .is_some_and(|value| !valid_identity(value, "background-job-process:sha256:"))
            || self.observed_at_ms == 0
            || self
                .registered_at_ms
                .is_some_and(|registered| registered == 0 || registered > self.observed_at_ms)
            || self.completion_inferred
            || self.dispatch_authority
            || self.attempt_number == Some(0)
        {
            return Err(error(
                "background-process-observation-invalid",
                "background process observation invariant is invalid",
            ));
        }
        let process_registration_bound = self.process_registration_identity.is_some()
            && self.process_identity.is_some()
            && self.registered_at_ms.is_some();
        let process_bound = process_registration_bound && self.attempt_number.is_some();
        let expected_manual = self.state != BackgroundProcessObservationState::OwnedRunning;
        let expected_terminal = self.state == BackgroundProcessObservationState::OwnedExited;
        if self.manual_reconciliation_required != expected_manual
            || self.terminal_job_event_required != expected_terminal
            || self.process_identity.is_some() != self.registered_at_ms.is_some()
            || self.process_registration_identity.is_some() != self.process_identity.is_some()
            || (matches!(
                self.state,
                BackgroundProcessObservationState::OwnedRunning
                    | BackgroundProcessObservationState::OwnedExited
                    | BackgroundProcessObservationState::Inaccessible
            ) && !process_bound)
            || (self.state == BackgroundProcessObservationState::Absent
                && process_registration_bound)
            || (process_bound && self.job_generation == 0)
            || (self.state != BackgroundProcessObservationState::OwnedExited
                && self.process_exit_code.is_some())
        {
            return Err(error(
                "background-process-observation-state-invalid",
                "background process observation state binding is invalid",
            ));
        }
        let expected_identity = observation_identity(self)?;
        if self.observation_identity != expected_identity {
            return Err(error(
                "background-process-observation-identity-invalid",
                "background process observation identity is invalid",
            ));
        }
        Ok(())
    }
}

#[derive(Debug)]
pub struct VerifiedBackgroundProcessObservation {
    observation: BackgroundProcessObservation,
}

impl VerifiedBackgroundProcessObservation {
    pub fn observation(&self) -> &BackgroundProcessObservation {
        &self.observation
    }
}

#[derive(Debug)]
struct OwnedBackgroundProcess {
    child: Child,
    job_id: String,
    session_id: String,
    project_id: String,
    root_id: String,
    request_identity: String,
    state_identity: String,
    job_generation: u64,
    attempt_number: u16,
    process_identity: String,
    process_registration_identity: String,
    registered_at_ms: u64,
}

#[derive(Debug)]
pub struct BackgroundProcessRegistry {
    owner_identity: String,
    processes: BTreeMap<String, OwnedBackgroundProcess>,
}

impl BackgroundProcessRegistry {
    pub fn new(
        owner_identity: impl Into<String>,
    ) -> Result<Self, BackgroundProcessObservationError> {
        let owner_identity = owner_identity.into();
        if !valid_owner_identity(&owner_identity) {
            return Err(error(
                "background-process-owner-invalid",
                "background process owner identity is invalid",
            ));
        }
        Ok(Self {
            owner_identity,
            processes: BTreeMap::new(),
        })
    }

    pub fn owner_identity(&self) -> &str {
        &self.owner_identity
    }

    pub fn register_spawned_child(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        child: Child,
        registered_at_ms: u64,
    ) -> Result<VerifiedBackgroundProcessRegistration, BackgroundProcessObservationError> {
        let binding = process_binding(request, state)?;
        if !process_observation_required(state.status)
            || registered_at_ms == 0
            || registered_at_ms < state.updated_at_ms
        {
            return Err(error(
                "background-process-registration-state-invalid",
                "background process registration requires an active exact job state",
            ));
        }
        if self.processes.contains_key(&request.job_id) {
            return Err(error(
                "background-process-registration-conflict",
                "background job already has a runtime-owned process",
            ));
        }
        let process_identity = process_identity(
            &self.owner_identity,
            request,
            &binding,
            child.id(),
            registered_at_ms,
        )?;
        let registration = registration(
            &self.owner_identity,
            request,
            state,
            &binding,
            process_identity.clone(),
            registered_at_ms,
        )?;
        let owned = OwnedBackgroundProcess {
            child,
            job_id: request.job_id.clone(),
            session_id: request.session_id.clone(),
            project_id: request.project_id.clone(),
            root_id: request.root_id.clone(),
            request_identity: binding.request_identity.clone(),
            state_identity: binding.state_identity.clone(),
            job_generation: state.generation,
            attempt_number: binding.attempt_number,
            process_identity: process_identity.clone(),
            process_registration_identity: registration.registration_identity.clone(),
            registered_at_ms,
        };
        self.processes.insert(request.job_id.clone(), owned);
        verified_registration(registration)
    }

    pub fn rebind_active_state(
        &mut self,
        request: &BackgroundJobRequest,
        previous: &BackgroundJobState,
        next: &BackgroundJobState,
        rebound_at_ms: u64,
    ) -> Result<VerifiedBackgroundProcessRegistration, BackgroundProcessObservationError> {
        let previous_binding = process_binding(request, previous)?;
        let next_binding = process_binding(request, next)?;
        if !process_observation_required(previous.status)
            || !process_observation_required(next.status)
            || previous_binding.attempt_number != next_binding.attempt_number
            || rebound_at_ms < next.updated_at_ms
        {
            return Err(error(
                "background-process-rebind-state-invalid",
                "background process can only rebind the same active attempt",
            ));
        }
        let owned = self.processes.get_mut(&request.job_id).ok_or_else(|| {
            error(
                "background-process-rebind-missing",
                "background process ownership is missing",
            )
        })?;
        if rebound_at_ms < owned.registered_at_ms {
            return Err(error(
                "background-process-rebind-time-invalid",
                "background process rebind time is before registration",
            ));
        }
        if !owned.matches(request, previous, &previous_binding) {
            return Err(error(
                "background-process-rebind-mismatch",
                "background process ownership does not match the previous state",
            ));
        }
        let registration = registration(
            &self.owner_identity,
            request,
            next,
            &next_binding,
            owned.process_identity.clone(),
            owned.registered_at_ms,
        )?;
        owned.state_identity = next_binding.state_identity.clone();
        owned.job_generation = next.generation;
        owned.process_registration_identity = registration.registration_identity.clone();
        verified_registration(registration)
    }

    pub fn observe_for_scheduler(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        scheduler_owner_identity: &str,
        observed_at_ms: u64,
    ) -> Result<VerifiedBackgroundProcessObservation, BackgroundProcessObservationError> {
        let binding = process_binding(request, state)?;
        if !valid_owner_identity(scheduler_owner_identity)
            || observed_at_ms == 0
            || observed_at_ms < state.updated_at_ms
        {
            return Err(error(
                "background-process-observation-request-invalid",
                "background process observation request is invalid",
            ));
        }
        let required = process_observation_required(state.status);
        if scheduler_owner_identity != self.owner_identity {
            let owned = self.processes.get(&request.job_id);
            return verified_observation(build_observation(
                scheduler_owner_identity,
                request,
                &binding,
                BackgroundProcessObservationState::Mismatched,
                owned.map(|process| process.process_registration_identity.as_str()),
                owned.map(|process| process.process_identity.as_str()),
                owned.map(|process| process.registered_at_ms),
                observed_at_ms,
                None,
            )?);
        }
        let Some(owned) = self.processes.get_mut(&request.job_id) else {
            let state = if required {
                BackgroundProcessObservationState::Absent
            } else {
                BackgroundProcessObservationState::Unknown
            };
            return verified_observation(build_observation(
                scheduler_owner_identity,
                request,
                &binding,
                state,
                None,
                None,
                None,
                observed_at_ms,
                None,
            )?);
        };
        if !required || !owned.matches(request, state, &binding) {
            return verified_observation(build_observation(
                scheduler_owner_identity,
                request,
                &binding,
                if required {
                    BackgroundProcessObservationState::Mismatched
                } else {
                    BackgroundProcessObservationState::Unknown
                },
                Some(&owned.process_registration_identity),
                Some(&owned.process_identity),
                Some(owned.registered_at_ms),
                observed_at_ms,
                None,
            )?);
        }
        let outcome = match owned.child.try_wait() {
            Ok(None) => ProcessProbeOutcome::Running,
            Ok(Some(status)) => ProcessProbeOutcome::Exited(status),
            Err(_) => ProcessProbeOutcome::Inaccessible,
        };
        let (observation_state, exit_code) = match outcome {
            ProcessProbeOutcome::Running => (BackgroundProcessObservationState::OwnedRunning, None),
            ProcessProbeOutcome::Exited(status) => (
                BackgroundProcessObservationState::OwnedExited,
                status.code(),
            ),
            ProcessProbeOutcome::Inaccessible => {
                (BackgroundProcessObservationState::Inaccessible, None)
            }
        };
        verified_observation(build_observation(
            scheduler_owner_identity,
            request,
            &binding,
            observation_state,
            Some(&owned.process_registration_identity),
            Some(&owned.process_identity),
            Some(owned.registered_at_ms),
            observed_at_ms,
            exit_code,
        )?)
    }
}

impl Drop for BackgroundProcessRegistry {
    fn drop(&mut self) {
        for process in self.processes.values_mut() {
            if process.child.try_wait().ok().flatten().is_some() {
                continue;
            }
            if process.child.kill().is_err() {
                continue;
            }
            for _ in 0..10 {
                if process.child.try_wait().ok().flatten().is_some() {
                    break;
                }
                thread::sleep(Duration::from_millis(10));
            }
        }
    }
}

impl OwnedBackgroundProcess {
    fn matches(
        &self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        binding: &ProcessBinding,
    ) -> bool {
        self.job_id == request.job_id
            && self.session_id == request.session_id
            && self.project_id == request.project_id
            && self.root_id == request.root_id
            && self.request_identity == binding.request_identity
            && self.state_identity == binding.state_identity
            && self.job_generation == state.generation
            && self.attempt_number == binding.attempt_number
    }
}

#[derive(Debug)]
enum ProcessProbeOutcome {
    Running,
    Exited(ExitStatus),
    Inaccessible,
}

#[derive(Debug)]
struct ProcessBinding {
    request_identity: String,
    state_identity: String,
    job_generation: u64,
    attempt_number: u16,
}

#[derive(Serialize)]
struct ProcessIdentityBinding<'a> {
    schema_version: &'a str,
    owner_identity: &'a str,
    job_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    attempt_number: u16,
    runtime_process_id: u32,
    registered_at_ms: u64,
}

#[derive(Serialize)]
struct RegistrationIdentityBinding<'a> {
    schema_version: &'a str,
    owner_identity: &'a str,
    job_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    attempt_number: u16,
    process_identity: &'a str,
    registered_at_ms: u64,
}

#[derive(Serialize)]
struct ObservationIdentityBinding<'a> {
    schema_version: &'a str,
    owner_identity: &'a str,
    job_id: &'a str,
    session_id: &'a str,
    project_id: &'a str,
    root_id: &'a str,
    request_identity: &'a str,
    state_identity: &'a str,
    job_generation: u64,
    attempt_number: Option<u16>,
    process_registration_identity: Option<&'a str>,
    process_identity: Option<&'a str>,
    state: BackgroundProcessObservationState,
    registered_at_ms: Option<u64>,
    observed_at_ms: u64,
    process_exit_code: Option<i32>,
    manual_reconciliation_required: bool,
    terminal_job_event_required: bool,
    completion_inferred: bool,
    dispatch_authority: bool,
}

pub fn process_observation_required(status: BackgroundJobStatus) -> bool {
    matches!(
        status,
        BackgroundJobStatus::Running
            | BackgroundJobStatus::PauseRequested
            | BackgroundJobStatus::Cancelling
    )
}

fn process_binding(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
) -> Result<ProcessBinding, BackgroundProcessObservationError> {
    request.validate().map_err(|_| {
        error(
            "background-process-request-invalid",
            "background process request binding is invalid",
        )
    })?;
    state.validate(request).map_err(|_| {
        error(
            "background-process-state-invalid",
            "background process state binding is invalid",
        )
    })?;
    let request_identity = request.identity().map_err(|_| {
        error(
            "background-process-request-invalid",
            "background process request identity is invalid",
        )
    })?;
    let state_identity = state.identity(request).map_err(|_| {
        error(
            "background-process-state-invalid",
            "background process state identity is invalid",
        )
    })?;
    let attempt_number = state
        .attempts
        .last()
        .filter(|attempt| attempt.status == JobAttemptStatus::Running)
        .map(|attempt| attempt.number)
        .unwrap_or(0);
    Ok(ProcessBinding {
        request_identity,
        state_identity,
        job_generation: state.generation,
        attempt_number,
    })
}

fn process_identity(
    owner_identity: &str,
    request: &BackgroundJobRequest,
    binding: &ProcessBinding,
    runtime_process_id: u32,
    registered_at_ms: u64,
) -> Result<String, BackgroundProcessObservationError> {
    if runtime_process_id == 0 {
        return Err(error(
            "background-process-runtime-handle-invalid",
            "background process runtime handle is invalid",
        ));
    }
    let bytes = to_vec(&ProcessIdentityBinding {
        schema_version: SCHEMA_VERSION,
        owner_identity,
        job_id: &request.job_id,
        request_identity: &binding.request_identity,
        state_identity: &binding.state_identity,
        job_generation: binding.job_generation,
        attempt_number: binding.attempt_number,
        runtime_process_id,
        registered_at_ms,
    })
    .map_err(|_| {
        error(
            "background-process-identity-failed",
            "background process identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-process:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn registration(
    owner_identity: &str,
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
    binding: &ProcessBinding,
    process_identity: String,
    registered_at_ms: u64,
) -> Result<BackgroundProcessRegistration, BackgroundProcessObservationError> {
    let identity_bytes = to_vec(&RegistrationIdentityBinding {
        schema_version: SCHEMA_VERSION,
        owner_identity,
        job_id: &request.job_id,
        request_identity: &binding.request_identity,
        state_identity: &binding.state_identity,
        job_generation: state.generation,
        attempt_number: binding.attempt_number,
        process_identity: &process_identity,
        registered_at_ms,
    })
    .map_err(|_| {
        error(
            "background-process-registration-identity-failed",
            "background process registration identity could not be created",
        )
    })?;
    Ok(BackgroundProcessRegistration {
        schema_version: SCHEMA_VERSION.into(),
        owner_identity: owner_identity.into(),
        job_id: request.job_id.clone(),
        request_identity: binding.request_identity.clone(),
        state_identity: binding.state_identity.clone(),
        job_generation: state.generation,
        attempt_number: binding.attempt_number,
        process_identity,
        registered_at_ms,
        registration_identity: format!(
            "background-job-process-registration:sha256:{:x}",
            Sha256::digest(identity_bytes)
        ),
    })
}

#[allow(clippy::too_many_arguments)]
fn build_observation(
    owner_identity: &str,
    request: &BackgroundJobRequest,
    binding: &ProcessBinding,
    state: BackgroundProcessObservationState,
    process_registration_identity: Option<&str>,
    process_identity: Option<&str>,
    registered_at_ms: Option<u64>,
    observed_at_ms: u64,
    process_exit_code: Option<i32>,
) -> Result<BackgroundProcessObservation, BackgroundProcessObservationError> {
    let mut observation = BackgroundProcessObservation {
        schema_version: SCHEMA_VERSION.into(),
        owner_identity: owner_identity.into(),
        job_id: request.job_id.clone(),
        session_id: request.session_id.clone(),
        project_id: request.project_id.clone(),
        root_id: request.root_id.clone(),
        request_identity: binding.request_identity.clone(),
        state_identity: binding.state_identity.clone(),
        job_generation: binding.job_generation,
        attempt_number: (binding.attempt_number != 0).then_some(binding.attempt_number),
        process_registration_identity: process_registration_identity.map(str::to_owned),
        process_identity: process_identity.map(str::to_owned),
        state,
        registered_at_ms,
        observed_at_ms,
        process_exit_code,
        manual_reconciliation_required: state != BackgroundProcessObservationState::OwnedRunning,
        terminal_job_event_required: state == BackgroundProcessObservationState::OwnedExited,
        completion_inferred: false,
        dispatch_authority: false,
        observation_identity: String::new(),
    };
    observation.observation_identity = observation_identity(&observation)?;
    observation.validate()?;
    Ok(observation)
}

fn verified_observation(
    observation: BackgroundProcessObservation,
) -> Result<VerifiedBackgroundProcessObservation, BackgroundProcessObservationError> {
    observation.validate()?;
    Ok(VerifiedBackgroundProcessObservation { observation })
}

fn verified_registration(
    registration: BackgroundProcessRegistration,
) -> Result<VerifiedBackgroundProcessRegistration, BackgroundProcessObservationError> {
    if registration.schema_version != SCHEMA_VERSION
        || !valid_owner_identity(&registration.owner_identity)
        || !valid_identifier(&registration.job_id)
        || !valid_identity(&registration.request_identity, "background-job:sha256:")
        || !valid_identity(&registration.state_identity, "background-job-state:sha256:")
        || registration.job_generation == 0
        || registration.attempt_number == 0
        || !valid_identity(
            &registration.process_identity,
            "background-job-process:sha256:",
        )
        || registration.registered_at_ms == 0
        || !valid_identity(
            &registration.registration_identity,
            "background-job-process-registration:sha256:",
        )
    {
        return Err(error(
            "background-process-registration-invalid",
            "background process registration invariant is invalid",
        ));
    }
    Ok(VerifiedBackgroundProcessRegistration { registration })
}

fn observation_identity(
    observation: &BackgroundProcessObservation,
) -> Result<String, BackgroundProcessObservationError> {
    let bytes = to_vec(&ObservationIdentityBinding {
        schema_version: &observation.schema_version,
        owner_identity: &observation.owner_identity,
        job_id: &observation.job_id,
        session_id: &observation.session_id,
        project_id: &observation.project_id,
        root_id: &observation.root_id,
        request_identity: &observation.request_identity,
        state_identity: &observation.state_identity,
        job_generation: observation.job_generation,
        attempt_number: observation.attempt_number,
        process_registration_identity: observation.process_registration_identity.as_deref(),
        process_identity: observation.process_identity.as_deref(),
        state: observation.state,
        registered_at_ms: observation.registered_at_ms,
        observed_at_ms: observation.observed_at_ms,
        process_exit_code: observation.process_exit_code,
        manual_reconciliation_required: observation.manual_reconciliation_required,
        terminal_job_event_required: observation.terminal_job_event_required,
        completion_inferred: observation.completion_inferred,
        dispatch_authority: observation.dispatch_authority,
    })
    .map_err(|_| {
        error(
            "background-process-observation-identity-failed",
            "background process observation identity could not be created",
        )
    })?;
    Ok(format!(
        "background-job-process-observation:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn valid_owner_identity(value: &str) -> bool {
    valid_identity(value, OWNER_IDENTITY_PREFIX)
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

fn error(code: &'static str, message: &'static str) -> BackgroundProcessObservationError {
    BackgroundProcessObservationError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::background_job::{
        JobRetryPolicy, JobSchedule, JobScheduleKind, REQUEST_SCHEMA_VERSION,
    };
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    use std::io::Write;
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    use std::process::{Command, Stdio};

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn owner(byte: char) -> String {
        identity(OWNER_IDENTITY_PREFIX, byte)
    }

    fn request() -> BackgroundJobRequest {
        BackgroundJobRequest {
            schema_version: REQUEST_SCHEMA_VERSION.into(),
            job_id: "job-1".into(),
            session_id: "session-1".into(),
            project_id: "project-1".into(),
            root_id: "root-1".into(),
            execution_plan_identity: identity("unified-execution-plan:sha256:", 'a'),
            idempotency_identity: identity("idempotency:sha256:", 'b'),
            child_task_identity: None,
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 100,
                safe_retry_boundary_identity: Some(identity("retry-boundary:sha256:", 'c')),
            },
            created_at_ms: 100,
        }
    }

    #[test]
    fn missing_and_inactive_observations_fail_closed() {
        let request = request();
        let queued = BackgroundJobState::new(&request, 100).unwrap();
        let mut registry = BackgroundProcessRegistry::new(owner('d')).unwrap();
        let unknown = registry
            .observe_for_scheduler(&request, &queued, &owner('d'), 110)
            .unwrap();
        assert_eq!(
            unknown.observation().state,
            BackgroundProcessObservationState::Unknown
        );

        let mut running = queued;
        running.start(&request, 120).unwrap();
        let absent = registry
            .observe_for_scheduler(&request, &running, &owner('d'), 130)
            .unwrap();
        assert_eq!(
            absent.observation().state,
            BackgroundProcessObservationState::Absent
        );
        let wrong_owner_without_a_registered_process = registry
            .observe_for_scheduler(&request, &running, &owner('e'), 130)
            .unwrap();
        assert_eq!(
            wrong_owner_without_a_registered_process.observation().state,
            BackgroundProcessObservationState::Mismatched
        );
        assert!(absent.observation().manual_reconciliation_required);
        assert!(!absent.observation().completion_inferred);
        assert!(!absent.observation().dispatch_authority);
    }

    #[test]
    fn inaccessible_and_exited_contracts_never_infer_job_completion() {
        let request = request();
        let mut state = BackgroundJobState::new(&request, 100).unwrap();
        state.start(&request, 120).unwrap();
        let binding = process_binding(&request, &state).unwrap();
        let registration_identity = identity("background-job-process-registration:sha256:", 'e');
        let process_identity = identity("background-job-process:sha256:", 'f');
        let inaccessible = build_observation(
            &owner('d'),
            &request,
            &binding,
            BackgroundProcessObservationState::Inaccessible,
            Some(&registration_identity),
            Some(&process_identity),
            Some(120),
            130,
            None,
        )
        .unwrap();
        assert!(inaccessible.manual_reconciliation_required);
        assert!(!inaccessible.terminal_job_event_required);
        assert!(!inaccessible.completion_inferred);

        let mismatched = build_observation(
            &owner('d'),
            &request,
            &binding,
            BackgroundProcessObservationState::Mismatched,
            Some(&registration_identity),
            Some(&process_identity),
            Some(120),
            135,
            None,
        )
        .unwrap();
        assert!(mismatched.manual_reconciliation_required);

        let exited = build_observation(
            &owner('d'),
            &request,
            &binding,
            BackgroundProcessObservationState::OwnedExited,
            Some(&registration_identity),
            Some(&process_identity),
            Some(120),
            140,
            Some(0),
        )
        .unwrap();
        assert!(exited.manual_reconciliation_required);
        assert!(exited.terminal_job_event_required);
        assert!(!exited.completion_inferred);

        let mut partial_binding = exited.clone();
        partial_binding.process_identity = None;
        assert_eq!(
            partial_binding.validate().unwrap_err().code,
            "background-process-observation-state-invalid"
        );

        let mut tampered = exited;
        tampered.job_generation += 1;
        assert_eq!(
            tampered.validate().unwrap_err().code,
            "background-process-observation-identity-invalid"
        );
    }

    #[cfg(any(target_os = "macos", target_os = "windows"))]
    #[test]
    fn runtime_owned_child_is_observed_running_then_exited_without_pid_output() {
        let request = request();
        let mut state = BackgroundJobState::new(&request, 100).unwrap();
        state.start(&request, 120).unwrap();
        let mut command = blocking_command();
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut input = child.stdin.take().unwrap();
        input.flush().unwrap();
        let mut registry = BackgroundProcessRegistry::new(owner('d')).unwrap();
        let registration = registry
            .register_spawned_child(&request, &state, child, 120)
            .unwrap();
        assert_eq!(registration.registration().job_generation, state.generation);
        let running = registry
            .observe_for_scheduler(&request, &state, &owner('d'), 130)
            .unwrap();
        assert_eq!(
            running.observation().state,
            BackgroundProcessObservationState::OwnedRunning
        );
        assert!(!running.observation().manual_reconciliation_required);
        assert_eq!(
            running
                .observation()
                .process_registration_identity
                .as_deref(),
            Some(registration.registration().registration_identity.as_str())
        );
        let visible = serde_json::to_string(running.observation()).unwrap();
        assert!(!visible.contains("\"process_id\":"));
        assert!(!visible.contains("runtime_process_id"));

        let previous = state.clone();
        state.request_cancel(&request, 135).unwrap();
        let stale_binding = registry
            .observe_for_scheduler(&request, &state, &owner('d'), 136)
            .unwrap();
        assert_eq!(
            stale_binding.observation().state,
            BackgroundProcessObservationState::Mismatched
        );
        let rebound = registry
            .rebind_active_state(&request, &previous, &state, 136)
            .unwrap();
        assert_eq!(rebound.registration().job_generation, state.generation);
        assert_ne!(
            rebound.registration().registration_identity,
            registration.registration().registration_identity
        );
        assert_eq!(
            rebound.registration().process_identity,
            registration.registration().process_identity
        );
        let rebound_running = registry
            .observe_for_scheduler(&request, &state, &owner('d'), 137)
            .unwrap();
        assert_eq!(
            rebound_running.observation().state,
            BackgroundProcessObservationState::OwnedRunning
        );
        assert_eq!(
            rebound_running
                .observation()
                .process_registration_identity
                .as_deref(),
            Some(rebound.registration().registration_identity.as_str())
        );

        drop(input);
        let mut exited = None;
        for offset in 0..100 {
            let observation = registry
                .observe_for_scheduler(&request, &state, &owner('d'), 140 + offset)
                .unwrap();
            if observation.observation().state == BackgroundProcessObservationState::OwnedExited {
                exited = Some(observation);
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        let exited = exited.expect("owned child should exit after stdin closes");
        assert!(exited.observation().terminal_job_event_required);
        assert!(!exited.observation().completion_inferred);
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
